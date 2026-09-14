/** @file
  Minimal "Boot Android" UEFI application for giulia (OnePlus 13R / Ace 5).

  Reads the Android boot image (boot.img) from the "boot"/"boot_a" GPT
  partition, parses the header (v0-v4), gunzips the kernel/ramdisk when
  needed, loads the kernel + DTB + initramfs, and transfers control using
  the arm64 Linux boot protocol (x0 = DTB address).

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
#include <Library/DevicePathLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/Decompress.h>

#include "bootimg.h"

#define GZIP_MAGIC_0  0x1F
#define GZIP_MAGIC_1  0x8B

#define KERNEL_LOAD_ADDR  0x80000ULL   // typical arm64 Image load address
#define RAMDISK_LOAD_ADDR 0x90000000ULL
#define DTB_LOAD_ADDR     0x8f000000ULL

typedef VOID (*ARM64_KERNEL_ENTRY)(
  UINTN  FdtAddress,
  UINTN  X1,
  UINTN  X2,
  UINTN  X3
  );

// ---------------------------------------------------------------------------
// Returns TRUE if Buffer starts with a gzip stream.
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
// Decompress a gzip stream using the EFI decompression protocol.
// Returns NULL on failure; caller frees with FreePool.
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
    DEBUG ((DEBUG_ERROR, "BootAndroid: LocateProtocol(Decompress) failed: %r\n", Status));
    return NULL;
  }

  Status = Decompress->GetInfo (Decompress, Src, SrcSize, &UncompressedSize, &ScratchSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootAndroid: Decompress.GetInfo failed: %r\n", Status));
    return NULL;
  }

  Dst = AllocatePool (UncompressedSize);
  if (Dst == NULL) {
    return NULL;
  }

  Status = Decompress->Decompress (Decompress, Src, SrcSize, Dst, UncompressedSize, NULL, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootAndroid: Decompress failed: %r\n", Status));
    FreePool (Dst);
    return NULL;
  }

  *DstSize = UncompressedSize;
  return Dst;
}

// ---------------------------------------------------------------------------
// Locate the GPT partition whose name matches one of BootPartNames.
// ---------------------------------------------------------------------------
STATIC
EFI_BLOCK_IO_PROTOCOL *
LocateBootPartition (
  IN CHAR16 **BootPartNames,
  IN UINTN    NameCount
  )
{
  EFI_STATUS                 Status;
  UINTN                      HandleCount = 0;
  EFI_HANDLE                 *Handles    = NULL;
  EFI_BLOCK_IO_PROTOCOL      *BlockIo    = NULL;
  UINTN                      Index;
  UINTN                      NameIndex;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL,
                                    &HandleCount, &Handles);
  if (EFI_ERROR (Status) || (HandleCount == 0)) {
    return NULL;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_PARTITION_INFO_PROTOCOL *PartInfo = NULL;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&PartInfo);
    if (EFI_ERROR (Status) || (PartInfo == NULL) || (PartInfo->Type != PartTypeGpt)) {
      continue;
    }

    for (NameIndex = 0; NameIndex < NameCount; NameIndex++) {
      if (StrCmp (PartInfo->Info.Gpt.PartitionName, BootPartNames[NameIndex]) == 0) {
        Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
        if (!EFI_ERROR (Status)) {
          goto Done;
        }
      }
    }
  }

Done:
  if (Handles != NULL) {
    FreePool (Handles);
  }
  return BlockIo;
}

// ---------------------------------------------------------------------------
// Read the entire boot partition into memory (caller frees with FreePool).
// ---------------------------------------------------------------------------
STATIC
UINT8 *
ReadBootPartition (
  IN  EFI_BLOCK_IO_PROTOCOL *BlockIo,
  OUT UINTN                  *Size
  )
{
  EFI_STATUS  Status;
  UINTN       BufferSize;
  UINT8       *Buffer;

  BufferSize = (UINTN)(BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize;
  Buffer     = AllocatePool (BufferSize);
  if (Buffer == NULL) {
    return NULL;
  }

  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, BufferSize, Buffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BootAndroid: ReadBlocks failed: %r\n", Status));
    FreePool (Buffer);
    return NULL;
  }

  *Size = BufferSize;
  return Buffer;
}

// ---------------------------------------------------------------------------
// Compute the offset (page aligned) of kernel/ramdisk within the boot image.
// Returns the kernel offset via *KernelOffset and the ramdisk offset via
// *RamdiskOffset. PageSize is from the header.
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
  EFI_STATUS            Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINT8                 *Image = NULL;
  UINTN                 ImageSize;
  BOOT_IMG_HDR_V0       *Hdr;
  UINT32                PageSize;
  UINT32                Version;
  UINTN                 KernelOffset;
  UINTN                 RamdiskOffset;
  UINT8                 *Kernel;
  UINTN                 KernelSize;
  UINT8                 *Ramdisk = NULL;
  UINTN                 RamdiskSize = 0;
  UINT8                 *Dtb = NULL;
  UINTN                 DtbSize = 0;
  ARM64_KERNEL_ENTRY    Entry;
  UINTN                 FdtAddress;
  CHAR16                *BootNames[] = { L"boot", L"boot_a", L"boot_b" };

  DEBUG ((DEBUG_INFO, "BootAndroid: starting\n"));

  BlockIo = LocateBootPartition (BootNames, ARRAY_SIZE (BootNames));
  if (BlockIo == NULL) {
    Print (L"BootAndroid: no boot partition found\n");
    return EFI_NOT_FOUND;
  }

  Image = ReadBootPartition (BlockIo, &ImageSize);
  if (Image == NULL) {
    Print (L"BootAndroid: failed to read boot partition\n");
    return EFI_DEVICE_ERROR;
  }

  // Validate magic + header.
  Hdr = (BOOT_IMG_HDR_V0 *)Image;
  if (CompareMem (Hdr->Magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) != 0) {
    Print (L"BootAndroid: bad boot image magic\n");
    goto Error;
  }

  Version  = Hdr->HeaderVersion;
  PageSize = (Hdr->PageSize == 0) ? 2048 : Hdr->PageSize;

  // Locate kernel/ramdisk in the image (page aligned).
  KernelOffset  = AlignPage (sizeof (BOOT_IMG_HDR_V0), PageSize);
  RamdiskOffset = AlignPage (KernelOffset + Hdr->KernelSize, PageSize);

  if (Version >= 1) {
    // v1/v2 append recovery_dtbo + header_size; account for the larger header.
    KernelOffset = AlignPage (((BOOT_IMG_HDR_V1_EXTRA *)(Image + sizeof (BOOT_IMG_HDR_V0)))->HeaderSize,
                              PageSize);
    RamdiskOffset = AlignPage (KernelOffset + Hdr->KernelSize, PageSize);
  }

  if (Version >= 3) {
    BOOT_IMG_HDR_V3 *Hdr3 = (BOOT_IMG_HDR_V3 *)Image;
    KernelOffset  = Hdr3->HeaderSize;
    RamdiskOffset = AlignPage (KernelOffset + Hdr3->KernelSize, PageSize);
  }

  // Kernel
  Kernel     = Image + KernelOffset;
  KernelSize = Hdr->KernelSize;

  // Ramdisk (v0/v1/v2). v3/v4 have no second stage, ramdisk follows kernel.
  if (Hdr->RamdiskSize != 0) {
    Ramdisk     = Image + RamdiskOffset;
    RamdiskSize = Hdr->RamdiskSize;
  }

  // DTB: v2+ carries it in the header (after recovery_dtbo).
  if (Version >= 2) {
    BOOT_IMG_HDR_V2_EXTRA *Extra = (BOOT_IMG_HDR_V2_EXTRA *)
      (Image + sizeof (BOOT_IMG_HDR_V0) + sizeof (BOOT_IMG_HDR_V1_EXTRA));
    if (Extra->DtbSize != 0) {
      UINTN DtbOffset = (UINTN)((Version >= 3) ? (RamdiskOffset + Hdr->RamdiskSize)
                                               : (Extra->DtbAddr));
      // For v2 the DTB is page-aligned after recovery_dtbo; approximate by
      // scanning for the FDT magic as a fallback.
      if (Version >= 2 && Version < 3) {
        UINTN Scan;
        for (Scan = RamdiskOffset + AlignPage (RamdiskSize, PageSize);
             Scan + 8 <= ImageSize; Scan += PageSize) {
          if (*(UINT32 *)(Image + Scan) == SwapBytes32 (0xd00dfeed)) {
            Dtb = Image + Scan;
            DtbSize = Extra->DtbSize;
            break;
          }
        }
      } else {
        Dtb = Image + DtbOffset;
        DtbSize = Extra->DtbSize;
      }
    }
  }

  // --- Gunzip kernel/ramdisk if compressed ---
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

    // Load kernel to its run address.
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

  // We reuse the boot image buffer region for the DTB; if none was found,
  // pass 0 (kernel without appended DTB may still boot on some configs).
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
    UINTN                  Pages = 0;

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

  // Not reached.
  return EFI_SUCCESS;

Error:
  if (Image != NULL) {
    FreePool (Image);
  }
  return EFI_ABORTED;
}
