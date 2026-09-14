/**
  Copyright (C) Microsoft Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/MsBootPolicyLib.h>
#include <Library/DevicePathLib.h>

/*
 * giulia (OnePlus 13R / Ace 5, SM8650) default boot sequence:
 *   1. UFS internal storage (classified as "HDD", i.e. any non-USB device path)
 *   2. USB mass storage
 *
 * This is used by the "default" (MS / no-parameter) boot path. The dedicated
 * "Internal Storage" (SSD) and "USB Storage" (USB) boot options use their own
 * static sequences inside MsBootPolicy, so they are unaffected.
 */
STATIC BOOT_SEQUENCE mGiuliaBootSequence[] = {
  MsBootHDD,
  MsBootUSB,
  MsBootDone
};

BOOLEAN
EFIAPI
MsBootPolicyLibIsSettingsBoot ()
{
  return FALSE;
}

BOOLEAN
EFIAPI
MsBootPolicyLibIsAltBoot ()
{
  return FALSE;
}

EFI_STATUS
EFIAPI
MsBootPolicyLibClearBootRequests ()
{
  return EFI_SUCCESS;
}

BOOLEAN
EFIAPI
MsBootPolicyLibIsDevicePathBootable (IN EFI_DEVICE_PATH_PROTOCOL *DevicePath)
{
  // Check Device Path (NULL-check first, before dereferencing)
  if ((DevicePath == NULL) || !IsDevicePathValid (DevicePath, 0)) {
    return FALSE;
  }

  return TRUE;
}

BOOLEAN
EFIAPI
MsBootPolicyLibIsDeviceBootable (IN EFI_HANDLE ControllerHandle)
{
  return MsBootPolicyLibIsDevicePathBootable (DevicePathFromHandle (ControllerHandle));
}

BOOLEAN
EFIAPI
MsBootPolicyLibIsDevicePathUsb (IN EFI_DEVICE_PATH_PROTOCOL *DevicePath)
{
  // Go thru each Device Path
  for (EFI_DEVICE_PATH_PROTOCOL *Node = DevicePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
    // Compare Device Path Types
    if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) &&
        ((DevicePathSubType (Node) == MSG_USB_CLASS_DP) ||
         (DevicePathSubType (Node) == MSG_USB_WWID_DP) ||
         (DevicePathSubType (Node) == MSG_USB_DP))) {
      return TRUE;
    }
  }

  return FALSE;
}

EFI_STATUS
EFIAPI
MsBootPolicyLibGetBootSequence (
  OUT BOOT_SEQUENCE **BootSequence,
  IN  BOOLEAN         AltBootRequest)
{
  if (BootSequence == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // AltBoot is not used on giulia; return the same default sequence.
  *BootSequence = mGiuliaBootSequence;
  return EFI_SUCCESS;
}
