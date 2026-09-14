/** @file
  Minimal "Boot Android" UEFI application for giulia (OnePlus 13R / Ace 5).

  Reads the Android boot image from a FILE on a FAT filesystem (default
  "\android\boot.img"), parses the header (v0-v4), gunzips the kernel/ramdisk
  when needed, loads the kernel + DTB + initramfs, and transfers control using
  the arm64 Linux boot protocol (x0 = DTB address).

  The boot image is expected to be stored as a file (NOT on the boot
  partition, which carries BootShim.bin).

  NOTE: first revision. The DTB source (appended-to-kernel vs. header dtb
  field) and the kernel load address may need per-image adjustment after
  on-device testing.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Guid/FileInfo.h>
#include <Protocol/Decompress.h>
#include <Protocol/SimpleFileSystem.h>

#include "bootimg.h"

#define GZIP_MAGIC_0  0x1F
#define GZIP_MAGIC_1  0x8B

// Path of the Android boot image on a FAT volume (e.g. the ESP).
#define ANDROID_BOOT_IMAGE_PATH  L"\\android\\boot.img"

#define KERNEL_LOAD_ADDR  0x80000ULL   // typical arm64 Image load address

typedef VOID (*ARM64_KERNEL_ENTRY)(
  UINTN  FdtAddress,
  UINTN  X1,
  UINTN  X2,
  UINTN  X3
  );

// ---------------------------------------------------------------------------
STATIC
BOOLEAN
IsGzip (
  IN UINT8  *Buffer
  )
{
  return (Buffer != NULL) && (Buffer[0] == GZIP_MAGIC_0) && (Buffer[1] == GZIP_MAGIC_1);
}

// ---------------------------------------------------------------------------
STATIC
VOID *
Gunzip (
  IN UINT8   *Src,
  IN UINTN    SrcSize,
  OUT UINTN  *DstSize
  )
{
  EFI_STATUS              Status;
  EFI_DECOMPRESS_PROTOCOL *Decompress;
  UINTN                   UncompressedSize = 0;
  UINTN                   ScratchSize      = 0;
  VOID                    *Dst;

  *DstSize = 0;

  Status = gBS->LocateProtocol (&gEfiDecompressProtocolGuid, NULL, (VOID **)&Decompress);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Status = Decompress->GetInfo (Decompress, Src, SrcSize, &UncompressedSize, &ScratchSize);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Dst = AllocatePool (UncompressedSize);
  if (Dst == NULL) {
    return NULL;
  }

  Status = Decompress->Decompress (Decompress, Src, SrcSize, Dst, UncompressedSize, NULL, 0);
  if (EFI_ERROR (Status)) {
    FreePool (Dst);
    return NULL;
  }

  *DstSize = UncompressedSize;
  return Dst;
}

// ---------------------------------------------------------------------------
// Open and read ANDROID_BOOT_IMAGE_PATH from the first FAT volume that has it.
// Caller frees with FreePool.
// ---------------------------------------------------------------------------
STATIC
UINT8 *
ReadBootImageFile (
  OUT UINTN  *Size
  )
{
  EFI_STATUS                          Status;
  UINTN                               HandleCount = 0;
  EFI_HANDLE                          *Handles    = NULL;
  UINTN                               Index;
  UINT8                               *Buffer     = NULL;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL,
                                    &HandleCount, &Handles);
  if (EFI_ERROR (Status) || (HandleCount == 0)) {
    return NULL;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs  = NULL;
    EFI_FILE_PROTOCOL               *Root = NULL;
    EFI_FILE_PROTOCOL               *File = NULL;
    EFI_FILE_INFO                   *Info = NULL;
    UINTN                            InfoSize = 0;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Sfs);
    if (EFI_ERROR (Status) || (Sfs == NULL)) {
      continue;
    }

    Status = Sfs->OpenVolume (Sfs, &Root);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = Root->Open (Root, &File, ANDROID_BOOT_IMAGE_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (Status)) {
      continue;
    }

    // Get file size.
    InfoSize = 0;
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if ((Status != EFI_BUFFER_TOO_SMALL) || (InfoSize == 0)) {
      continue;
    }
    Info = AllocatePool (InfoSize);
    if (Info == NULL) {
      continue;
    }
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
    if (EFI_ERROR (Status)) {
      FreePool (Info);
      continue;
    }

    Buffer = AllocatePool ((UINTN)Info->FileSize);
    if (Buffer == NULL) {
      FreePool (Info);
      continue;
    }

    *Size = (UINTN)Info->FileSize;
    Status = File->Read (File, Size, Buffer);
    FreePool (Info);
    if (EFI_ERROR (Status)) {
      FreePool (Buffer);
      Buffer = NULL;
      continue;
    }

    break;
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }
  return Buffer;
}

// ---------------------------------------------------------------------------
STATIC
UINTN
AlignPage (
  IN UINTN Value,
  IN UINTN PageSize
  )
{
  return (Value + PageSize - 1) & ~(PageSize - 1);
}

// ---------------------------------------------------------------------------
EFI_STATUS
EFIAPI
BootAndroidEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  UINT8                  *Image = NULL;
  UINTN                  ImageSize;
  BOOT_IMG_HDR_V0        *Hdr;
  UINT32                 PageSize;
  UINT32                 Version;
  UINTN                  KernelOffset;
  UINTN                  RamdiskOffset;
  UINT8                  *Kernel;
  UINTN                  KernelSize;
  UINT8                  *Ramdisk = NULL;
  UINTN                  RamdiskSize = 0;
  UINT8                  *Dtb = NULL;
  UINTN                  DtbSize = 0;
  ARM64_KERNEL_ENTRY     Entry;
  UINTN                  FdtAddress;

  DEBUG ((DEBUG_INFO, "BootAndroid: starting\n"));

  Image = ReadBootImageFile (&ImageSize);
  if (Image == NULL) {
    Print (L"BootAndroid: cannot open %s\n", ANDROID_BOOT_IMAGE_PATH);
    return EFI_NOT_FOUND;
  }

  Hdr = (BOOT_IMG_HDR_V0 *)Image;
  if (CompareMem (Hdr->Magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) != 0) {
    Print (L"BootAndroid: bad boot image magic\n");
    goto Error;
  }

  Version  = Hdr->HeaderVersion;
  PageSize = (Hdr->PageSize == 0) ? 2048 : Hdr->PageSize;

  KernelOffset  = AlignPage (sizeof (BOOT_IMG_HDR_V0), PageSize);
  RamdiskOffset = AlignPage (KernelOffset + Hdr->KernelSize, PageSize);

  if (Version >= 1 && Version < 3) {
    KernelOffset  = AlignPage (((BOOT_IMG_HDR_V1_EXTRA *)(Image + sizeof (BOOT_IMG_HDR_V0)))->HeaderSize, PageSize);
    RamdiskOffset = AlignPage (KernelOffset + Hdr->KernelSize, PageSize);
  }

  if (Version >= 3) {
    BOOT_IMG_HDR_V3 *Hdr3 = (BOOT_IMG_HDR_V3 *)Image;
    KernelOffset  = Hdr3->HeaderSize;
    RamdiskOffset = AlignPage (KernelOffset + Hdr3->KernelSize, PageSize);
  }

  Kernel     = Image + KernelOffset;
  KernelSize = Hdr->KernelSize;

  if (Hdr->RamdiskSize != 0) {
    Ramdisk     = Image + RamdiskOffset;
    RamdiskSize = Hdr->RamdiskSize;
  }

  if (Version >= 2) {
    BOOT_IMG_HDR_V2_EXTRA *Extra = (BOOT_IMG_HDR_V2_EXTRA *)
      (Image + sizeof (BOOT_IMG_HDR_V0) + sizeof (BOOT_IMG_HDR_V1_EXTRA));
    if (Extra->DtbSize != 0) {
      UINTN Scan;
      DtbSize = Extra->DtbSize;
      for (Scan = RamdiskOffset + AlignPage (RamdiskSize, PageSize);
           Scan + 8 <= ImageSize; Scan += PageSize) {
        if (*(UINT32 *)(Image + Scan) == SwapBytes32 (0xd00dfeed)) {
          Dtb = Image + Scan;
          break;
        }
      }
    }
  }

  // --- Gunzip kernel/ramdisk if compressed, then load ---
  {
    UINT8 *K = Kernel;
    UINTN  Ks = KernelSize;
    if (IsGzip (Kernel)) {
      K = Gunzip (Kernel, KernelSize, &Ks);
      if (K == NULL) {
        Print (L"BootAndroid: gunzip kernel failed\n");
        goto Error;
      }
    }

    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderData,
                                 EFI_SIZE_TO_PAGES (Ks), (EFI_PHYSICAL_ADDRESS *)&Kernel);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BootAndroid: AllocatePages(kernel) failed: %r\n", Status));
      if (K != Kernel) FreePool (K);
      goto Error;
    }
    CopyMem ((VOID *)Kernel, K, Ks);
    if (K != Kernel) FreePool (K);
    KernelSize = Ks;

    if (Ramdisk != NULL) {
      UINT8 *R = Ramdisk;
      UINTN  Rs = RamdiskSize;
      if (IsGzip (Ramdisk)) {
        R = Gunzip (Ramdisk, RamdiskSize, &Rs);
      }
      if (R != NULL) {
        Ramdisk = AllocatePool (Rs);
        if (Ramdisk != NULL) {
          CopyMem (Ramdisk, R, Rs);
          RamdiskSize = Rs;
        }
        if (R != Ramdisk) FreePool (R);
      }
    }
  }

  FdtAddress = (Dtb != NULL) ? (UINTN)Dtb : 0;

  DEBUG ((DEBUG_INFO, "BootAndroid: kernel=0x%lx (%lx bytes), dtb=0x%lx, ramdisk=0x%lx (%lx)\n",
          (UINT64)(UINTN)Kernel, KernelSize, FdtAddress, (UINT64)(UINTN)Ramdisk, RamdiskSize));

  // --- Exit boot services and jump ---
  {
    EFI_MEMORY_DESCRIPTOR *MemMap = NULL;
    UINTN                  MemMapSize = 0;
    UINTN                  MapKey;
    UINTN                  DescriptorSize;
    UINT32                 DescriptorVersion;

    do {
      Status = gBS->GetMemoryMap (&MemMapSize, MemMap, &MapKey, &DescriptorSize, &DescriptorVersion);
      if (Status == EFI_BUFFER_TOO_SMALL) {
        if (MemMap != NULL) FreePool (MemMap);
        MemMap = AllocatePool (MemMapSize);
        continue;
      }
      break;
    } while (TRUE);

    if (EFI_ERROR (Status)) {
      Print (L"BootAndroid: GetMemoryMap failed: %r\n", Status);
      goto Error;
    }

    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (EFI_ERROR (Status)) {
      Print (L"BootAndroid: ExitBootServices failed: %r\n", Status);
      goto Error;
    }

    Entry = (ARM64_KERNEL_ENTRY)(UINTN)Kernel;
    Entry (FdtAddress, 0, 0, 0);
  }

  return EFI_SUCCESS;

Error:
  if (Image != NULL) {
    FreePool (Image);
  }
  return EFI_ABORTED;
}
