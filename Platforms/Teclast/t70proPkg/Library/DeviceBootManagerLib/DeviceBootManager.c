/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Pi/PiFirmwareFile.h>
#include <Guid/EventGroup.h>
#include <Library/BootLogoLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include "../PlatformSecLib/RamBootWatchdog.h"
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/UsbIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>

STATIC BOOLEAN mConnecting;
STATIC BOOLEAN mInputRescanPending, mOutputRescanPending;
STATIC EFI_EVENT mOutputNotifyEvents[2];
STATIC VOID *mOutputRegistrations[2];
STATIC BOOLEAN mOutputNotifyActive[2], mOutputNotifyStarted;
STATIC BOOLEAN mInputNotifyStarted, mInputNotifyActive, mInputServicesEnded;
STATIC EFI_EVENT mInputNotifyEvent, mInputExitEvent;
STATIC VOID *mInputRegistration;
STATIC BOOLEAN mRecoveryShellActive;
STATIC BOOLEAN mSetupActive, mSetupAttempted;
STATIC UINT32 mSetupLaunchPhase, mSetupLaunchStatus;
STATIC CONST EFI_GUID mSetupGuid = {
  0x462caa21, 0x7614, 0x4503, {0x83,0x6e,0x8a,0xb6,0xf4,0x66,0x23,0x31}
};
STATIC VOID LaunchRamTrialSetup (VOID);
STATIC CONST EFI_GUID mRecoveryShellGuid = {
  0x7c04a583, 0x9e3e, 0x4f1c, {0xad,0x65,0xe0,0x52,0x68,0xd0,0xb4,0xd1}
};
// Recovery must not execute a removable medium's startup.nsh automatically.
STATIC CONST CHAR16 mRecoveryShellArguments[] = L"-nostartup";


STATIC UINT32 mRegisteredInputCount, mRegisteredOutputCount, mTraceSequence;

// Own fixed RAM record: protocol counts and booleans only, never handles,
// key contents, filenames or framebuffer contents. Protocol presence alone
// does not establish that a physical key or touch has worked.
STATIC UINT32
ProtocolHandleCount (EFI_GUID *Protocol)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  EFI_STATUS Status = gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return 0;
  if (Handles != NULL) FreePool (Handles);
  return (UINT32)(Count > MAX_UINT16 ? MAX_UINT16 : Count);
}

// A photo-sized diagnostic, using only an already available text console.
// No files, variables, device writes, key data, addresses or serials are saved.
STATIC VOID
WriteDiagnosticHex (CHAR16 *Text, UINT32 Value, UINTN Digits)
{
  CONST CHAR16 Hex[] = L"0123456789ABCDEF";
  while (Digits != 0) {
    Text[--Digits] = Hex[Value & 15U];
    Value >>= 4;
  }
}

STATIC VOID
ShowBootDiagnostic (VOID)
{
  STATIC BOOLEAN Printing;
  CHAR16 Line[] = L"\r\nT70 D1 I=0000 O=0000 F=0000 P=00000000 E=00000000\r\n";
  if (Printing || mInputServicesEnded || gST == NULL || gST->ConOut == NULL ||
      gST->ConOut->OutputString == NULL) return;
  Printing = TRUE;
  WriteDiagnosticHex (&Line[11], mRegisteredInputCount, 4);
  WriteDiagnosticHex (&Line[18], mRegisteredOutputCount, 4);
  WriteDiagnosticHex (&Line[25], ProtocolHandleCount (&gEfiSimpleFileSystemProtocolGuid), 4);
  WriteDiagnosticHex (&Line[32], mSetupLaunchPhase, 8);
  WriteDiagnosticHex (&Line[43], mSetupLaunchStatus, 8);
  if (!mInputServicesEnded) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out = gST->ConOut;
    UINTN Columns = 20, Rows, ReportedColumns;
    CHAR16 Chunk[80];
    UINTN Position = 2, End = ARRAY_SIZE (Line) - 3;
    if (Out->Mode != NULL && Out->Mode->Mode >= 0 && Out->QueryMode != NULL &&
        !EFI_ERROR (Out->QueryMode (Out, (UINTN)Out->Mode->Mode, &ReportedColumns, &Rows))) {
      if (ReportedColumns < 2) { Printing = FALSE; return; }
      Columns = ReportedColumns;
    }
    // Leave the last column unused to avoid firmware auto-wrap plus CR/LF.
    UINTN Width = MIN (Columns - 1, ARRAY_SIZE (Chunk) - 3);
    if (!mInputServicesEnded && !EFI_ERROR (Out->OutputString (Out, L"\r\n"))) {
      while (Position < End && !mInputServicesEnded) {
        UINTN Count = MIN (Width, End - Position);
        if (Position + Count < End) {
          UINTN Boundary = Count;
          while (Boundary != 0 && Line[Position + Boundary] != L' ') Boundary--;
          if (Boundary != 0) Count = Boundary;
        }
        for (UINTN Index = 0; Index < Count; Index++) Chunk[Index] = Line[Position + Index];
        Chunk[Count] = L'\r'; Chunk[Count + 1] = L'\n'; Chunk[Count + 2] = 0;
        if (EFI_ERROR (Out->OutputString (Out, Chunk))) break;
        Position += Count;
        while (Line[Position] == L' ') Position++;
      }
    }
  }
  Printing = FALSE;
}

STATIC VOID
RecordBdsProgress (UINT32 Stage)
{
  UINT32 Words[14];
  if (MmioRead64 (0x43F7F000) != 0x0000000148303754ULL ||
      MmioRead64 (0x43F7F060) != 0x0000000147503754ULL ||
      MmioRead32 (0x43F7F068) != 8 || MmioRead64 (0x43F7F070) != EFI_SUCCESS)
    return;
  if (mTraceSequence < MAX_UINT16) mTraceSequence++;
  // On-demand input may run inside SendForm; preserve the application's
  // actual entry/SendForm marker while refreshing the console counts.
  if (mSetupActive && Stage == 6) {
    UINT32 UiStage = MmioRead32 (0x43F7F810);
    if (UiStage >= 9 && UiStage <= 11) Stage = UiStage;
  }
  Words[0] = Stage; Words[1] = mTraceSequence;
  Words[2] = mRegisteredInputCount; Words[3] = mRegisteredOutputCount;
  Words[4] = ProtocolHandleCount (&gEfiAbsolutePointerProtocolGuid);
  Words[5] = ProtocolHandleCount (&gEfiSimplePointerProtocolGuid);
  Words[6] = ProtocolHandleCount (&gEfiUsbIoProtocolGuid);
  Words[7] = ProtocolHandleCount (&gEfiSimpleFileSystemProtocolGuid);
  Words[8] = ProtocolHandleCount (&gEfiGraphicsOutputProtocolGuid);
  Words[9] = gST != NULL && gST->ConsoleInHandle != NULL && gST->ConIn != NULL;
  Words[10] = gST != NULL && gST->ConsoleOutHandle != NULL && gST->ConOut != NULL;
  Words[11] = (mConnecting ? 1U : 0U) | (mRecoveryShellActive ? 2U : 0U) | (mSetupActive ? 4U : 0U);
  Words[12] = mSetupLaunchPhase; Words[13] = mSetupLaunchStatus;
  MmioWrite64 (0x43F7F800, 0);
  MmioWrite32 (0x43F7F808, 0x43F7F060U);
  MmioWrite32 (0x43F7F80C, 0);
  for (UINTN Index = 0; Index < ARRAY_SIZE (Words); Index++)
    MmioWrite32 (0x43F7F810 + Index * 4, Words[Index]);
  MemoryFence ();
  MmioWrite64 (0x43F7F800, 0x000000074C443754ULL);
  WriteBackDataCacheRange ((VOID *)(UINTN)0x43F7F800, 72);
}

STATIC VOID
RegisterConsoleHandles (IN EFI_GUID *Protocol, IN CONSOLE_TYPE ConsoleType, IN BOOLEAN Recursive)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  UINTN Count, Index;

  if (mInputServicesEnded) return;
  if (ConsoleType == ConIn) mRegisteredInputCount = 0;
  if (ConsoleType == ConOut) mRegisteredOutputCount = 0;
  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) return;
  for (Index = 0; Index < Count && !mInputServicesEnded; Index++) {
    Path = DevicePathFromHandle (Handles[Index]);
    if (mInputServicesEnded) break;
    if (Path == NULL || !IsDevicePathValid (Path, 0)) continue;
    Status = EfiBootManagerUpdateConsoleVariable (ConsoleType, Path, NULL);
    if (mInputServicesEnded) break;
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "T70 console registration failed: %r\n", Status));
    } else {
      if (ConsoleType == ConIn) {
        if (mRegisteredInputCount < MAX_UINT16) mRegisteredInputCount++;
        // ConPlatform checks ConIn when it starts; register the device path
        // first, then bind it and ConSplitter to the live system console.
        Status = gBS->ConnectController (Handles[Index], NULL, NULL, Recursive);
        if (EFI_ERROR (Status) && Status != EFI_ALREADY_STARTED)
          DEBUG ((DEBUG_WARN, "T70 input console connection failed: %r\n", Status));
      }
      if (ConsoleType == ConOut && mRegisteredOutputCount < MAX_UINT16) mRegisteredOutputCount++;
    }
  }
  if (!mInputServicesEnded) FreePool (Handles);
}

// Connect only physical, path-bearing console handles. The virtual splitter
// GOP has no device path and must not be recursively connected to itself.
STATIC VOID
ConnectConsoleProtocol (EFI_GUID *Protocol, BOOLEAN Recursive)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0;
  if (mInputServicesEnded || EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Handles))) return;
  for (UINTN Index = 0; Index < Count && !mInputServicesEnded; Index++) {
    EFI_DEVICE_PATH_PROTOCOL *Path = DevicePathFromHandle (Handles[Index]);
    if (mInputServicesEnded) break;
    if (Path != NULL && IsDevicePathValid (Path, 0))
      (VOID)gBS->ConnectController (Handles[Index], NULL, NULL, Recursive);
  }
  if (!mInputServicesEnded) FreePool (Handles);
}

STATIC VOID
RegisterOutputConsoles (BOOLEAN Recursive)
{
  if (mInputServicesEnded) return;
  // ConPlatform reads both variables in Start, so populate both before bind.
  RegisterConsoleHandles (&gEfiSimpleTextOutProtocolGuid, ConOut, Recursive);
  RegisterConsoleHandles (&gEfiSimpleTextOutProtocolGuid, ErrOut, Recursive);
  ConnectConsoleProtocol (&gEfiSimpleTextOutProtocolGuid, Recursive);
}

STATIC VOID
DrainConsoleNotifications (BOOLEAN Recursive)
{
  UINTN Passes = 0;
  if (mConnecting || mInputServicesEnded) return;
  mConnecting = TRUE;
  do {
    BOOLEAN Input = mInputRescanPending, Output = mOutputRescanPending;
    mInputRescanPending = mOutputRescanPending = FALSE;
    if (Input) RegisterConsoleHandles (&gEfiSimpleTextInProtocolGuid, ConIn, Recursive);
    if (Output && !mInputServicesEnded) {
      ConnectConsoleProtocol (&gEfiGraphicsOutputProtocolGuid, Recursive);
      RegisterOutputConsoles (Recursive);
    }
    Passes++;
  } while ((mInputRescanPending || mOutputRescanPending) && !mInputServicesEnded && Passes < 8);
  // Keep pending work on exhaustion; do not create a self-signalling loop.
  mConnecting = FALSE;
}

STATIC VOID EFIAPI
OutputConsoleNotify (EFI_EVENT Event, VOID *Context)
{
  (VOID)Context;
  if (mInputServicesEnded) return;
  for (UINTN Index = 0; Index < 2; Index++) {
    if (Event == mOutputNotifyEvents[Index] && mOutputNotifyActive[Index]) {
      mOutputRescanPending = TRUE;
      DrainConsoleNotifications (FALSE);
      return;
    }
  }
}

STATIC VOID
StartOutputConsoleNotify (VOID)
{
  EFI_GUID *Protocols[2] = { &gEfiGraphicsOutputProtocolGuid, &gEfiSimpleTextOutProtocolGuid };
  EFI_STATUS Status;
  if (mOutputNotifyStarted || mInputServicesEnded || mInputExitEvent == NULL) return;
  mOutputNotifyStarted = TRUE;
  for (UINTN Index = 0; Index < 2; Index++) {
    Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OutputConsoleNotify, NULL, &mOutputNotifyEvents[Index]);
    if (EFI_ERROR (Status)) { mOutputNotifyEvents[Index] = NULL; continue; }
    Status = gBS->RegisterProtocolNotify (Protocols[Index], mOutputNotifyEvents[Index], &mOutputRegistrations[Index]);
    if (EFI_ERROR (Status)) {
      if (!EFI_ERROR (gBS->CloseEvent (mOutputNotifyEvents[Index]))) mOutputNotifyEvents[Index] = NULL;
      continue;
    }
    mOutputNotifyActive[Index] = TRUE;
  }
}

STATIC VOID EFIAPI
InputConsoleExit (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  mInputServicesEnded = TRUE;
  mInputNotifyActive = FALSE;
}

STATIC VOID EFIAPI
InputConsoleNotify (EFI_EVENT Event, VOID *Context)
{
  (VOID)Context;
  if (Event != mInputNotifyEvent || !mInputNotifyActive || mInputServicesEnded) return;
  mInputRescanPending = TRUE;
  DrainConsoleNotifications (FALSE);
  if (!mConnecting && !mInputServicesEnded) RecordBdsProgress (6);
}

STATIC VOID
StartInputConsoleNotify (VOID)
{
  EFI_STATUS Status;
  if (mInputNotifyStarted || mInputServicesEnded) return;
  mInputNotifyStarted = TRUE;
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, InputConsoleExit,
                              NULL, &gEfiEventExitBootServicesGuid, &mInputExitEvent);
  if (EFI_ERROR (Status)) { mInputExitEvent = NULL; return; }
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, InputConsoleNotify,
                            NULL, &mInputNotifyEvent);
  if (EFI_ERROR (Status)) { mInputNotifyEvent = NULL; return; }
  Status = gBS->RegisterProtocolNotify (&gEfiSimpleTextInProtocolGuid,
                                        mInputNotifyEvent, &mInputRegistration);
  if (EFI_ERROR (Status)) {
    if (!EFI_ERROR (gBS->CloseEvent (mInputNotifyEvent))) mInputNotifyEvent = NULL;
    return;
  }
  mInputNotifyActive = TRUE;
}

// An interactive RAM trial has no automatic reset deadline.
STATIC BOOLEAN
ValidRamTrial (VOID)
{
  return MmioRead64 (T70_HANDOFF_BASE) == T70_HANDOFF_MAGIC &&
    MmioRead64 (T70_TRIAL_RECORD) == T70_TRIAL_MAGIC &&
    MmioRead64 (T70_TRIAL_RECORD + 8) == T70_TRIAL_INVERSE &&
    MmioRead64 (T70_TRIAL_RECORD + 24) == 2 &&
    (MmioRead32 (T70_WDT_BASE) & 1U) == 0U;
}

STATIC EFI_STATUS
PrepareRamTrialSetup (VOID)
{
  if (!ValidRamTrial ()) return EFI_NOT_READY;
  // Disable the UEFI software watchdog as well as the early hardware one.
  return gBS->SetWatchdogTimer (0, 0, 0, NULL);
}

STATIC VOID
RecordSetupLaunch (UINT32 Phase, EFI_STATUS Status)
{
  mSetupLaunchPhase = Phase;
  mSetupLaunchStatus = (UINT32)(Status & 0xFFFFU) | (EFI_ERROR (Status) ? 0x10000U : 0);
  RecordBdsProgress (5);
}

STATIC VOID
LaunchRamTrialSetup (VOID)
{
  EFI_STATUS Status;
  EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
  EFI_HANDLE Image = NULL;
  if (mSetupAttempted || mSetupActive || !ValidRamTrial ()) return;
  mSetupAttempted = TRUE;
  RecordSetupLaunch (1, EFI_SUCCESS);
  Status = GetFileDevicePathFromAnyFv (&mSetupGuid, EFI_SECTION_PE32, 0, &Path);
  if (EFI_ERROR (Status)) {
    RecordSetupLaunch (2, Status);
    return;
  }
  RecordSetupLaunch (3, EFI_SUCCESS);
  // Load the FV application directly. EfiBootManagerBoot first tries to create
  // Boot#### even for a transient option; setup must not depend on that write.
  Status = gBS->LoadImage (FALSE, gImageHandle, Path, NULL, 0, &Image);
  FreePool (Path);
  if (EFI_ERROR (Status)) {
    // Only SECURITY_VIOLATION may return an owned image on failure.
    // Other error outputs are not valid handles and must not be unloaded.
    if ((Status == EFI_SECURITY_VIOLATION) && (Image != NULL))
      (VOID)gBS->UnloadImage (Image);
    RecordSetupLaunch (4, Status);
    return;
  }
  RecordSetupLaunch (5, EFI_SUCCESS);
  Status = PrepareRamTrialSetup ();
  if (EFI_ERROR (Status)) {
    (VOID)gBS->UnloadImage (Image);
    RecordSetupLaunch (6, Status);
    return;
  }
  mSetupActive = TRUE;
  RecordSetupLaunch (7, EFI_SUCCESS);
  // StartImage unloads an EFI application when its entry point returns.
  Status = gBS->StartImage (Image, NULL, NULL);
  mSetupActive = FALSE;
  RecordSetupLaunch (8, Status);
}

STATIC VOID
ConnectBoardDevices (VOID)
{
  if (mConnecting || mInputServicesEnded) return;
  if (EfiGetCurrentTpl () != TPL_APPLICATION) {
    // A console wait callback must not start recursive bus enumeration.
    mInputRescanPending = TRUE;
    mOutputRescanPending = TRUE;
    return;
  }
  StartInputConsoleNotify ();
  StartOutputConsoleNotify ();
  mConnecting = TRUE;
  // EndOfDxe has published the verified GOP and UFS host by this point.
  // Recursive connection is required for USB HID/mass storage and for
  // UFS -> SCSI bus -> read-only LU2 BlockIo -> partition -> FAT children.
  // ScsiDiskDxe is deliberately absent: only our LU2 read-only adapter binds.
  EfiBootManagerConnectAll ();
  RegisterConsoleHandles (&gEfiSimpleTextInProtocolGuid, ConIn, TRUE);
  RegisterOutputConsoles (TRUE);
  mConnecting = FALSE;
  if (mInputRescanPending || mOutputRescanPending) DrainConsoleNotifications (TRUE);
}

EFI_HANDLE EFIAPI
DeviceBootManagerBeforeConsole (
  OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath,
  OUT BDS_CONSOLE_CONNECT_ENTRY **PlatformConsoles
  )
{
  if (DevicePath != NULL) *DevicePath = NULL;
  if (PlatformConsoles != NULL) *PlatformConsoles = NULL;
  RecordBdsProgress (2);
  ConnectBoardDevices ();
  RecordBdsProgress (3);
  return NULL;
}

STATIC BOOLEAN
BootPathIsUsb (IN EFI_DEVICE_PATH_PROTOCOL *Path)
{
  for (; Path != NULL && !IsDevicePathEnd (Path); Path = NextDevicePathNode (Path)) {
    if (DevicePathType (Path) == MESSAGING_DEVICE_PATH &&
        (DevicePathSubType (Path) == MSG_USB_DP || DevicePathSubType (Path) == MSG_USB_CLASS_DP ||
         DevicePathSubType (Path) == MSG_USB_WWID_DP)) return TRUE;
  }
  return FALSE;
}

STATIC INTN EFIAPI
CompareBootPriority (IN CONST VOID *Left, IN CONST VOID *Right)
{
  CONST EFI_BOOT_MANAGER_LOAD_OPTION *L = Left, *R = Right;
  BOOLEAN LeftUsb = BootPathIsUsb (L->FilePath), RightUsb = BootPathIsUsb (R->FilePath);
  if (LeftUsb != RightUsb) return LeftUsb ? 1 : -1;
  return (L->OptionNumber > R->OptionNumber) - (L->OptionNumber < R->OptionNumber);
}

EFI_DEVICE_PATH_PROTOCOL ** EFIAPI
DeviceBootManagerAfterConsole (VOID)
{
  RecordBdsProgress (4);
  if (gST != NULL && gST->ConOut != NULL && gST->ConOut->ClearScreen != NULL)
    (VOID)gST->ConOut->ClearScreen (gST->ConOut);
  (VOID)BootLogoEnableLogo ();
  ConnectBoardDevices ();
  // The generic policy registered its defaults before the board devices were
  // connected. Enumerate the actual filesystems now, including removable EFI.
  EfiBootManagerRefreshAllBootOption ();
  // Variables live only in RAM, so Windows Setup's BootOrder is gone after a
  // reset and the generic order puts removable USB first. Try internal disks
  // first: one without a loader fails and falls through to USB, and an
  // installed Windows wins over a still-connected installer. Volume Up still
  // opens the one-time boot menu.
  EfiBootManagerSortLoadOptionVariable (LoadOptionTypeBoot, CompareBootPriority);
  RecordBdsProgress (5);
  LaunchRamTrialSetup ();
  return NULL;
}

EFI_DEVICE_PATH_PROTOCOL ** EFIAPI
DeviceBootManagerOnDemandConInConnect (VOID)
{
  ConnectBoardDevices ();
  RecordBdsProgress (6);
  return NULL;
}

VOID EFIAPI DeviceBootManagerBdsEntry (VOID)
{
  // A prior boot's RAM snapshot must not remain valid if this boot cannot
  // publish a fresh record. The HII reader rejects the cleared signature.
  if (MmioRead64 (0x43F7F000) == 0x0000000148303754ULL) {
    MmioWrite64 (0x43F7F800, 0);
    MemoryFence ();
    WriteBackDataCacheRange ((VOID *)(UINTN)0x43F7F800, sizeof (UINT64));
  }
  RecordBdsProgress (1);
}
VOID EFIAPI DeviceBootManagerProcessBootCompletion (IN EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{ (VOID)BootOption; }
EFI_STATUS EFIAPI DeviceBootManagerPriorityBoot (IN EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{ (VOID)BootOption; return EFI_NOT_FOUND; }
VOID EFIAPI
DeviceBootManagerUnableToBoot (VOID)
{
  EFI_STATUS Status;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_BOOT_MANAGER_LOAD_OPTION Option;
  EFI_HANDLE Image = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;

  if (mRecoveryShellActive) return;
  ShowBootDiagnostic ();
  Path = NULL;
  Status = GetFileDevicePathFromAnyFv (&mRecoveryShellGuid, EFI_SECTION_PE32, 0, &Path);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "T70 recovery shell not found: %r\n", Status));
    return;
  }
  Status = EfiBootManagerInitializeLoadOption (
             &Option, LoadOptionNumberUnassigned, LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP, L"UEFI Shell", Path,
             (UINT8 *)mRecoveryShellArguments, sizeof (mRecoveryShellArguments));
  FreePool (Path);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "T70 recovery shell option failed: %r\n", Status));
    return;
  }
  mRecoveryShellActive = TRUE;
  // EfiBootManagerBoot persists even temporary Boot#### options and refuses
  // to launch if that write fails. Recovery must work without variable writes.
  RecordBdsProgress (7);
  Status = gBS->LoadImage (FALSE, gImageHandle, Option.FilePath, NULL, 0, &Image);
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
    if (!EFI_ERROR (Status)) {
      LoadedImage->LoadOptions = Option.OptionalData;
      LoadedImage->LoadOptionsSize = Option.OptionalDataSize;
      // StartImage unloads an EFI application when its entry point returns.
      Status = gBS->StartImage (Image, NULL, NULL);
    } else {
      (VOID)gBS->UnloadImage (Image);
    }
  } else if ((Status == EFI_SECURITY_VIOLATION) && (Image != NULL)) {
    // LoadImage may return an owned but prohibited image for this status only.
    (VOID)gBS->UnloadImage (Image);
  }
  if (EFI_ERROR (Status)) DEBUG ((DEBUG_ERROR, "T70 recovery shell failed: %r\n", Status));
  mRecoveryShellActive = FALSE;
  RecordBdsProgress (8);
  EfiBootManagerFreeLoadOption (&Option);
}

