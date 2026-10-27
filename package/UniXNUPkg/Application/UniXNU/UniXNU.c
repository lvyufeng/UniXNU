/** @file
  UniXNU UEFI application entry point.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UniXNULib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>

//
// Optional data of the boot option UniXNUDxe registers. Keep in sync with
// UniXNUDxe.c.
//
STATIC CONST CHAR16  mFirmwareMarker[] = L"unixnu:firmware";

//
// Bring-up used to paint a band per step straight into the panel at a hardcoded address.
//
// That address is the splash region on this board and nothing else, and writing to it from the
// boot path is unsafe the moment anything else owns the memory - it took the panel away from
// the menu without repainting it, which is exactly how a load that was still making progress
// came to look like a hang. The progress it reported is in the log now, so it is gone.
//
STATIC
VOID
UniXNUProgress (
  IN UINTN  Step
  )
{
  //
  // One line per stage, on the channel that has been reaching the panel.
  //
  DEBUG ((DEBUG_ERROR, "UNIXNU: stage %d\n", Step));
}

EFI_STATUS
EFIAPI
UniXNUApplicationEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  UNIXNU_HOST             Host;
  EFI_STATUS                 Status;

  //
  // The entry point is otherwise silent, and a boot that hangs here used to leave the
  // dispatcher's "Loading driver ... UniXNU.efi" as the last thing on screen - which reads
  // exactly like a failure to start, when in fact the image was started and stopped inside.
  //
  //
  // Arm the watchdog before anything else.
  //
  // This board has no console, no USB until the stack is up, and a panel that may or may not
  // be repainted, so a stop anywhere inside this application used to leave the phone sitting
  // there until somebody held the power key. Arming it here - the first instruction that
  // touches the firmware at all - makes every later stop a reboot the build loop can drive on
  // its own. It is cancelled only when the menu is running and waiting for input.
  //
  if ((SystemTable != NULL) && (SystemTable->BootServices != NULL)) {
    SystemTable->BootServices->SetWatchdogTimer (180, 0, 0, NULL);
  }

  DEBUG ((DEBUG_ERROR, "UNIXNU: entry\n"));

  UniXNUProgress (0);
  DEBUG ((DEBUG_ERROR, "UNIXNU: band 0\n"));

  Host = UniXNUHostApplication;
  DEBUG ((DEBUG_ERROR, "UNIXNU: host set\n"));
  UniXNUProgress (1);

  //
  // SystemTable is the second argument the loader passed. If the loader handed over a table
  // that is not there, this is where the difference shows.
  //
  if (SystemTable == NULL) {
    //
    // Distinguish "the loader passed no table" from "something later hung": paint band 9
    // (a different place on the panel) and return cleanly, so the panel says which it was.
    //
    UniXNUProgress (9);
    return EFI_LOAD_ERROR;
  }

  UniXNUProgress (2);

  //
  // LoadedImage is the first protocol lookup this application makes; the run stopped with one
  // band on screen, which puts the stop in this call.
  //
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded))) {
    UniXNUProgress (3);

    if ((Loaded->LoadOptions != NULL) && (Loaded->LoadOptionsSize >= sizeof (mFirmwareMarker)) &&
        (CompareMem (Loaded->LoadOptions, mFirmwareMarker, sizeof (mFirmwareMarker)) == 0))
    {
      Host = UniXNUHostFirmware;
    }

    UniXNUProgress (4);
  }

  UniXNUProgress (5);

  Status = UniXNURun (ImageHandle, Host);

  UniXNUProgress (11);


  //
  // "Return to firmware" from a firmware boot option: report failure so BDS
  // moves on to its next option instead of restarting UniXNU.
  //
  if ((Host == UniXNUHostFirmware) && !EFI_ERROR (Status)) {
    return EFI_ABORTED;
  }

  return Status;
}
