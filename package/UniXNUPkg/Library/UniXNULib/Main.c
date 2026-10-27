/** @file
  UniXNURun() and logging.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

#include <Protocol/SerialIo.h>

STATIC EFI_SERIAL_IO_PROTOCOL  *mSerial;

VOID
UniXNULogInit (
  VOID
  )
{
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiSerialIoProtocolGuid, NULL, (VOID **)&mSerial))) {
    mSerial = NULL;
  }
}

VOID
EFIAPI
UniXNULog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Args;
  CHAR8    Line[256];
  UINTN    Prefix;

  DEBUG ((DEBUG_ERROR, "UNIXNU: log enter\n"));
  Prefix = AsciiSPrint (Line, sizeof (Line), "UNIXNU: ");
  DEBUG ((DEBUG_ERROR, "UNIXNU: log prefix %d\n", Prefix));
  VA_START (Args, Format);
  AsciiVSPrint (Line + Prefix, sizeof (Line) - Prefix - 2, Format, Args);
  VA_END (Args);
  DEBUG ((DEBUG_ERROR, "UNIXNU: log formatted\n"));
  AsciiStrCatS (Line, sizeof (Line), "\r\n");

  //
  // DEBUG_ERROR, not the configured level.
  //
  // The configured PcdUniXNUDebugLevel is 0x40, and a Debug() call at that level from this
  // application never returns: the boot stops right here, after the line is formatted and
  // before a single byte of it reaches the panel. DEBUG_ERROR is the level the firmware's own
  // traces use - the dispatcher's [load] lines are visible on this board - so this prints at
  // that level instead.
  //
  DEBUG ((DEBUG_ERROR, "UNIXNU: %a", Line));

  //
  // No serial write.
  //
  // UniXNULogInit finds an EFI_SERIAL_IO_PROTOCOL on this board and hands it to this function,
  // and the write to it never comes back: the protocol exists because the platform publishes
  // one over the framebuffer, and the boot stops inside Write with no output at all. The DEBUG
  // line above is the channel that works, and on a board with no UART it is the only one that
  // should be used.
  //
}

EFI_STATUS
EFIAPI
UniXNURun (
  IN EFI_HANDLE      ImageHandle,
  IN UNIXNU_HOST  Host
  )
{
  EFI_STATUS       Status;
  UNIXNU_CONFIG        Config;
  UNIXNU_UI            Ui;
  UNIXNU_INJECT_STATS  Stats;
  UINTN            Index;
  BOOLEAN          Countdown;
  UNIXNU_ENTRY         *Entry;
  UNIXNU_ENTRY         BootCopy;

  //
  // A boot manager waits for people; the 5 minute BDS watchdog does not - but a boot that
  // hangs must not wait for a person either.
  //
  // This board has no serial console, no USB until the stack is up and a panel that may or may
  // not be repainted, so a stop inside this application used to leave the phone sitting there
  // until somebody held the power key. Arming the watchdog before anything else turns that
  // into a reboot the build loop can drive on its own: whatever hangs, the phone comes back to
  // a state adb can reach, and the next experiment can start. The countdown is cancelled only
  // once the menu is actually running and waiting for input, which is the one state that is
  // supposed to be idle.
  //
  //
  // The watchdog is armed by the application entry point, before this function is entered.
  // Calling it here as well was the last change before the boot started stopping at stage 5,
  // and on this board SetWatchdogTimer on an already-armed timer does not come back - so this
  // one call is gone and the entry point's stays.
  //
  DEBUG ((DEBUG_ERROR, "UNIXNU: run entered\n"));
  UniXNULogInit ();
  DEBUG ((DEBUG_ERROR, "UNIXNU: log init done\n"));
  UniXNULog (
    "UniXNU %a starting as %a, firmware %s rev 0x%x",
    UNIXNU_VERSION_STRING,
    (Host == UniXNUHostFirmware) ? "firmware boot option" : "application",
    (gST->FirmwareVendor != NULL) ? gST->FirmwareVendor : L"?",
    gST->FirmwareRevision
    );

  //
  // No ConnectAll here.
  //
  // The comment on UniXNUConnectAll says it walks every handle in the system and does not come
  // back on this board - and this call is the one that hung: with the log finally reaching the
  // panel, the boot stopped on "starting as firmware boot option", which is exactly the branch
  // that calls it. Whatever it was meant to connect, the payload is reached through the
  // block-device path in Config.c, and that runs its own narrower connect.
  //

  UniXNUConfigLoad (ImageHandle, &Config);
  UniXNULog (
    "config: %u entries, %u drivers, %u dt patches, %u acpi tables, %u slices, %u kits loaded, %u skipped",
    (UINT32)Config.EntryCount,
    (UINT32)Config.DriverCount,
    (UINT32)Config.PatchCount,
    (UINT32)Config.AcpiCount,
    (UINT32)Config.SliceCount,
    (UINT32)Config.KitsLoaded,
    (UINT32)Config.KitsSkipped
    );

  UniXNUPublishSlices (&Config);

  //
  // Drivers before the UI: an injected driver may provide the display or
  // the input device the menu needs.
  //
  ZeroMem (&Stats, sizeof (Stats));
  UniXNUInjectDrivers (ImageHandle, &Config, &Stats);
  UniXNUInjectAcpi (&Config, &Stats);

  UniXNUUiInit (&Config, &Ui);
  UnicodeSPrint (
    Ui.Info,
    sizeof (Ui.Info),
    L"drivers %u loaded / %u failed    kits %u    acpi %u",
    (UINT32)Stats.DriversLoaded,
    (UINT32)Stats.DriversFailed,
    (UINT32)Config.KitsLoaded,
    (UINT32)Stats.AcpiInstalled
    );

  Countdown = TRUE;
  for ( ; ;) {
    Index  = UniXNUMenuRun (&Ui, &Config, Countdown);
    Entry  = &Config.Entries[Index];
    BootCopy = *Entry;
    if (Entry->Confirm != NULL) {
      UINTN  Choice;

      if (!UniXNUMenuConfirm (&Ui, Entry, &Choice)) {
        UniXNULog ("menu: entry %u not confirmed", (UINT32)Index);
        UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice), L"'%s' cancelled", Entry->Title);
        Countdown = FALSE;
        continue;
      }

      if (Entry->ConfirmCount > 0) {
        //
        // The chosen option's arguments follow the entry's own, on a copy: the entry itself stays as configured.
        //
        UINTN  Length = (Entry->Args != NULL ? StrLen (Entry->Args) : 0) + 1 + StrLen (Entry->ConfirmArgs[Choice]) + 1;

        BootCopy.Args = AllocateZeroPool (Length * sizeof (CHAR16));
        if (BootCopy.Args != NULL) {
          UnicodeSPrint (BootCopy.Args, Length * sizeof (CHAR16), L"%s %s", Entry->Args != NULL ? Entry->Args : L"", Entry->ConfirmArgs[Choice]);
        } else {
          BootCopy.Args = Entry->Args;
        }
      }
    }

    Status = UniXNUBootEntry (ImageHandle, &Config, &BootCopy);
    if ((Entry->Type == UniXNUEntryAction) && (Entry->Action == UniXNUActionExit)) {
      return EFI_SUCCESS;
    }

    //
    // The payload returned or failed: back to the menu, without the
    // countdown so a broken default cannot loop on a machine with input.
    //
    UnicodeSPrint (Ui.Notice, sizeof (Ui.Notice), L"'%s' returned: %r", Entry->Title, Status);
    UniXNULog ("menu: entry %u returned %r", (UINT32)Index, Status);
    //
    // Back at the menu, which is idle by design: the countdown is cancelled here and nowhere
    // else, so everything before this point is still covered.
    //
    gBS->SetWatchdogTimer (0, 0, 0, NULL);
    Countdown = FALSE;
  }
}
