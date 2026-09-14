/** @file
  Android boot image (boot.img) format definitions, v0 through v4.

  Ported for the giulia BootAndroid application. Structure layouts follow the
  AOSP bootimg.h definition; fields are little-endian on arm64.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef BOOT_IMAGE_H_
#define BOOT_IMAGE_H_

#include <Uefi.h>

#define BOOT_MAGIC             "ANDROID!"
#define BOOT_MAGIC_SIZE        8
#define BOOT_NAME_SIZE         16
#define BOOT_ARGS_SIZE         512
#define BOOT_EXTRA_ARGS_SIZE   1024

// ---------------------------------------------------------------------------
// v0 header. For v1/v2 the same base layout is used and the extra fields
// below are appended right after it.
// ---------------------------------------------------------------------------
typedef struct {
  UINT8  Magic[BOOT_MAGIC_SIZE];
  UINT32 KernelSize;
  UINT32 KernelAddr;
  UINT32 RamdiskSize;
  UINT32 RamdiskAddr;
  UINT32 SecondSize;
  UINT32 SecondAddr;
  UINT32 TagsAddr;
  UINT32 PageSize;
  UINT32 HeaderVersion;
  UINT32 OsVersion;
  UINT8  Name[BOOT_NAME_SIZE];
  UINT8  Cmdline[BOOT_ARGS_SIZE];
  UINT32 Id[8];
  UINT8  ExtraCmdline[BOOT_EXTRA_ARGS_SIZE];
} BOOT_IMG_HDR_V0;

// ---------------------------------------------------------------------------
// Fields appended after BOOT_IMG_HDR_V0 for header v1 and v2.
// ---------------------------------------------------------------------------
typedef struct {
  UINT32 RecoveryDtboSize;      // v1+
  UINT64 RecoveryDtboOffset;    // v1+
  UINT32 HeaderSize;            // v1+
} BOOT_IMG_HDR_V1_EXTRA;

typedef struct {
  UINT32 DtbSize;               // v2+
  UINT64 DtbAddr;               // v2+
} BOOT_IMG_HDR_V2_EXTRA;

// ---------------------------------------------------------------------------
// v3/v4 header: no second stage, smaller layout.
// ---------------------------------------------------------------------------
typedef struct {
  UINT8  Magic[BOOT_MAGIC_SIZE];
  UINT32 KernelSize;
  UINT32 RamdiskSize;
  UINT32 OsVersion;
  UINT32 HeaderSize;
  UINT32 Reserved[4];
  UINT32 HeaderVersion;
  UINT8  Cmdline[BOOT_ARGS_SIZE + BOOT_EXTRA_ARGS_SIZE];
} BOOT_IMG_HDR_V3;

// v4 appends a boot signature size right after BOOT_IMG_HDR_V3.
typedef struct {
  UINT32 SignatureSize;
} BOOT_IMG_HDR_V4_EXTRA;

#endif // BOOT_IMAGE_H_
