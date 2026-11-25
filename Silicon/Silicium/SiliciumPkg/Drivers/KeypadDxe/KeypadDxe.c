/**
  Keypad driver. Routines that interacts with callers, conforming to EFI driver model.

  Copyright (c) 2006 - 2016, Intel Corporation. All rights reserved.<BR>

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD
  License which accompanies this distribution.  The full text of the license may
  be found at http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "KeypadDxe.h"

#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>

#include <Configuration/BootDevices.h>

//
// Function prototypes
//

/**
  Test controller is a keypad Controller.

  @param This                 Pointer of EFI_DRIVER_BINDING_PROTOCOL
  @param Controller           driver's controller
  @param RemainingDevicePath  children device path

  @retval EFI_UNSUPPORTED     controller is not floppy disk
  @retval EFI_SUCCESS         controller is floppy disk
**/
EFI_STATUS
EFIAPI
KeypadControllerDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  );

/**
  Create KEYPAD_CONSOLE_IN_DEV instance on controller.

  @param This                Pointer of EFI_DRIVER_BINDING_PROTOCOL
  @param Controller          driver controller handle
  @param RemainingDevicePath Children's device path

  @retval                    whether success to create floppy control instance.
**/
EFI_STATUS
EFIAPI
KeypadControllerDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  );

/**
  Stop this driver on ControllerHandle. Support stopping any child handles
  created by this driver.

  @param  This              Protocol instance pointer.
  @param  ControllerHandle  Handle of device to stop driver on
  @param  NumberOfChildren  Number of Handles in ChildHandleBuffer. If number of
                            children is zero stop the entire bus driver.
  @param  ChildHandleBuffer List of Child Handles to Stop.

  @retval EFI_SUCCESS       This driver is removed ControllerHandle
  @retval other             This driver was not removed from this device
**/
EFI_STATUS
EFIAPI
KeypadControllerDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                  *ChildHandleBuffer
  );

/**
  Free the waiting key notify list.

  @param ListHead  Pointer to list head

  @retval EFI_INVALID_PARAMETER  ListHead is NULL
  @retval EFI_SUCCESS            Sucess to free NotifyList
**/
EFI_STATUS
KbdFreeNotifyList (IN OUT LIST_ENTRY *ListHead);

//
// DriverBinding Protocol Instance
//
EFI_DRIVER_BINDING_PROTOCOL gKeypadControllerDriver = {
  KeypadControllerDriverSupported,
  KeypadControllerDriverStart,
  KeypadControllerDriverStop,
  0xa,
  NULL,
  NULL
};

STATIC KEYPAD_CONSOLE_IN_DEV *mPendingConsoleIn;
STATIC EFI_HANDLE             mPendingController;
STATIC EFI_HANDLE             mPendingAgent;
STATIC BOOLEAN                mPendingSupportedClose;
STATIC EFI_HANDLE             mPendingSupportedController;
STATIC EFI_HANDLE             mPendingSupportedAgent;

STATIC
EFI_STATUS
KeypadClosePendingSupported (
  IN EFI_HANDLE Agent,
  IN EFI_HANDLE Controller
  )
{
  EFI_STATUS Status;

  if (!mPendingSupportedClose) {
    return EFI_SUCCESS;
  }
  if (mPendingSupportedController != Controller || mPendingSupportedAgent != Agent) {
    return EFI_NOT_READY;
  }

  Status = gBS->CloseProtocol (
                  Controller,
                  &gKeypadDeviceProtocolGuid,
                  Agent,
                  Controller
                  );
  if (!EFI_ERROR (Status)) {
    mPendingSupportedClose = FALSE;
    mPendingSupportedController = NULL;
    mPendingSupportedAgent = NULL;
  }
  return Status;
}

STATIC
EFI_STATUS
KeypadReconcileProtocols (
  IN EFI_HANDLE                  Controller,
  IN OUT KEYPAD_CONSOLE_IN_DEV *ConsoleIn
  )
{
  EFI_STATUS Status;
  EFI_STATUS FirstError;
  VOID       *Interface;

  FirstError = EFI_SUCCESS;
  Interface = NULL;
  Status = gBS->HandleProtocol (Controller, &gEfiSimpleTextInProtocolGuid, &Interface);
  if (Status == EFI_SUCCESS) {
    ConsoleIn->TextInInstalled = (BOOLEAN)(Interface == &ConsoleIn->ConIn);
  } else if (Status == EFI_NOT_FOUND) {
    ConsoleIn->TextInInstalled = FALSE;
  } else {
    ConsoleIn->TextInInstalled = TRUE;
    FirstError = Status;
  }

  Interface = NULL;
  Status = gBS->HandleProtocol (Controller, &gEfiSimpleTextInputExProtocolGuid, &Interface);
  if (Status == EFI_SUCCESS) {
    ConsoleIn->TextInExInstalled = (BOOLEAN)(Interface == &ConsoleIn->ConInEx);
  } else if (Status == EFI_NOT_FOUND) {
    ConsoleIn->TextInExInstalled = FALSE;
  } else {
    ConsoleIn->TextInExInstalled = TRUE;
    if (!EFI_ERROR (FirstError)) {
      FirstError = Status;
    }
  }

  return FirstError;
}

STATIC
EFI_STATUS
KeypadUninstallProtocols (
  IN EFI_HANDLE                  Controller,
  IN OUT KEYPAD_CONSOLE_IN_DEV *ConsoleIn
  )
{
  EFI_STATUS Status;
  BOOLEAN    OldTextIn;
  BOOLEAN    OldTextInEx;

  for (UINTN Attempt = 0; Attempt < 2; Attempt++) {
    if (!ConsoleIn->TextInInstalled && !ConsoleIn->TextInExInstalled) {
      return EFI_SUCCESS;
    }

    OldTextIn = ConsoleIn->TextInInstalled;
    OldTextInEx = ConsoleIn->TextInExInstalled;
    if (OldTextIn && OldTextInEx) {
      Status = gBS->UninstallMultipleProtocolInterfaces (
                      Controller,
                      &gEfiSimpleTextInProtocolGuid,
                      &ConsoleIn->ConIn,
                      &gEfiSimpleTextInputExProtocolGuid,
                      &ConsoleIn->ConInEx,
                      NULL
                      );
    } else if (OldTextIn) {
      Status = gBS->UninstallProtocolInterface (
                      Controller,
                      &gEfiSimpleTextInProtocolGuid,
                      &ConsoleIn->ConIn
                      );
    } else {
      Status = gBS->UninstallProtocolInterface (
                      Controller,
                      &gEfiSimpleTextInputExProtocolGuid,
                      &ConsoleIn->ConInEx
                      );
    }

    if (!EFI_ERROR (Status)) {
      ConsoleIn->TextInInstalled = FALSE;
      ConsoleIn->TextInExInstalled = FALSE;
      return EFI_SUCCESS;
    }

    if (EFI_ERROR (KeypadReconcileProtocols (Controller, ConsoleIn))) {
      return Status;
    }
    if (OldTextIn == ConsoleIn->TextInInstalled &&
        OldTextInEx == ConsoleIn->TextInExInstalled) {
      return Status;
    }
  }

  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
KeypadCloseEvent (
  IN OUT EFI_EVENT *Event
  )
{
  EFI_STATUS Status;

  if (*Event == NULL) {
    return EFI_SUCCESS;
  }

  Status = gBS->CloseEvent (*Event);
  if (!EFI_ERROR (Status)) {
    *Event = NULL;
  }
  return Status;
}

STATIC
EFI_STATUS
KeypadCleanupConsole (
  IN EFI_HANDLE                  Controller,
  IN EFI_HANDLE                  Agent,
  IN OUT KEYPAD_CONSOLE_IN_DEV *ConsoleIn
  )
{
  EFI_STATUS FirstError;
  EFI_STATUS Status;

  ConsoleIn->Stopping = TRUE;
  ConsoleIn->KeypadErr = TRUE;
  FirstError = EFI_SUCCESS;

  if (ConsoleIn->TimerEvent != NULL) {
    (VOID)gBS->SetTimer (ConsoleIn->TimerEvent, TimerCancel, 0);
    Status = KeypadCloseEvent (&ConsoleIn->TimerEvent);
    if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }
  }
  Status = KeypadCloseEvent (&ConsoleIn->KeyNotifyProcessEvent);
  if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
    FirstError = Status;
  }
  Status = KeypadCloseEvent (&ConsoleIn->ConIn.WaitForKey);
  if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
    FirstError = Status;
  }
  Status = KeypadCloseEvent (&ConsoleIn->ConInEx.WaitForKeyEx);
  if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
    FirstError = Status;
  }

  if (ConsoleIn->TimerEvent != NULL || ConsoleIn->KeyNotifyProcessEvent != NULL ||
      ConsoleIn->ConIn.WaitForKey != NULL || ConsoleIn->ConInEx.WaitForKeyEx != NULL) {
    return EFI_ERROR (FirstError) ? FirstError : EFI_NOT_READY;
  }

  Status = KeypadUninstallProtocols (Controller, ConsoleIn);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ConsoleIn->DeviceProtocolOpen) {
    Status = gBS->CloseProtocol (
                    Controller,
                    &gKeypadDeviceProtocolGuid,
                    Agent,
                    Controller
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    ConsoleIn->DeviceProtocolOpen = FALSE;
  }

  Status = KbdFreeNotifyList (&ConsoleIn->NotifyList);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (ConsoleIn->ControllerNameTable != NULL) {
    Status = FreeUnicodeStringTable (ConsoleIn->ControllerNameTable);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    ConsoleIn->ControllerNameTable = NULL;
  }
  return gBS->FreePool (ConsoleIn);
}

STATIC
EFI_STATUS
KeypadRememberAndCleanup (
  IN EFI_HANDLE                  Controller,
  IN EFI_HANDLE                  Agent,
  IN OUT KEYPAD_CONSOLE_IN_DEV *ConsoleIn
  )
{
  EFI_STATUS Status;

  if (mPendingConsoleIn != NULL &&
      (mPendingConsoleIn != ConsoleIn || mPendingController != Controller || mPendingAgent != Agent)) {
    return EFI_NOT_READY;
  }
  mPendingConsoleIn = ConsoleIn;
  mPendingController = Controller;
  mPendingAgent = Agent;
  Status = KeypadCleanupConsole (Controller, Agent, ConsoleIn);
  if (!EFI_ERROR (Status)) {
    mPendingConsoleIn = NULL;
    mPendingController = NULL;
    mPendingAgent = NULL;
  }
  return Status;
}

STATIC
EFI_STATUS
KeypadCleanupPending (
  IN EFI_HANDLE ThisAgent,
  IN EFI_HANDLE Controller
  )
{
  if (mPendingConsoleIn == NULL) {
    return KeypadClosePendingSupported (ThisAgent, Controller);
  }
  if (mPendingController != Controller || mPendingAgent != ThisAgent) {
    return EFI_NOT_READY;
  }
  return KeypadRememberAndCleanup (Controller, ThisAgent, mPendingConsoleIn);
}

/**
  Test controller is a keypad Controller.

  @param This                 Pointer of EFI_DRIVER_BINDING_PROTOCOL
  @param Controller           driver's controller
  @param RemainingDevicePath  children device path

  @retval EFI_UNSUPPORTED     controller is not floppy disk
  @retval EFI_SUCCESS         controller is floppy disk
**/
EFI_STATUS
EFIAPI
KeypadControllerDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath)
{
  EFI_STATUS              Status;
  EFI_STATUS              CloseStatus;
  KEYPAD_DEVICE_PROTOCOL *KeypadDevice;

  Status = KeypadCleanupPending (This->DriverBindingHandle, Controller);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Open the IO Abstraction(s) needed to perform the supported test
  //
  Status = gBS->OpenProtocol (Controller, &gKeypadDeviceProtocolGuid, (VOID **)&KeypadDevice, This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    goto exit;
  }

  //
  // Close the I/O Abstraction(s) used to perform the supported test
  //
  CloseStatus = gBS->CloseProtocol (Controller, &gKeypadDeviceProtocolGuid, This->DriverBindingHandle, Controller);
  if (EFI_ERROR (CloseStatus)) {
    mPendingSupportedClose = TRUE;
    mPendingSupportedController = Controller;
    mPendingSupportedAgent = This->DriverBindingHandle;
    return CloseStatus;
  }

exit:
  return Status;
}

STATIC
EFIAPI
VOID
KeypadReturnApiPushEfikeyBufTail (
  KEYPAD_RETURN_API *This,
  EFI_KEY_DATA      *KeyData)
{
  KEYPAD_CONSOLE_IN_DEV       *ConsoleIn;
  LIST_ENTRY                  *Link;
  KEYPAD_CONSOLE_IN_EX_NOTIFY *CurrentNotify;
  EFI_TPL OldTpl;

  ConsoleIn = KEYPAD_CONSOLE_IN_DEV_FROM_KEYPAD_RETURN_API(This);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (ConsoleIn->Stopping || ConsoleIn->KeypadErr) {
    goto Exit;
  }

  //
  // If the key can not be converted then just return.
  //
  if (KeyData->Key.ScanCode == SCAN_NULL && KeyData->Key.UnicodeChar == CHAR_NULL) {
    if (!ConsoleIn->IsSupportPartialKey) {
      DEBUG ((EFI_D_WARN, "Cannot convert this Keycode.\n"));
      goto Exit;
    }
  }

  //
  // Signal KeyNotify process event if this key pressed matches any key
  // registered.
  //
  for (Link = GetFirstNode (&ConsoleIn->NotifyList); !IsNull (&ConsoleIn->NotifyList, Link); Link = GetNextNode (&ConsoleIn->NotifyList, Link)) {
    CurrentNotify = CR (Link, KEYPAD_CONSOLE_IN_EX_NOTIFY, NotifyEntry, KEYPAD_CONSOLE_IN_EX_NOTIFY_SIGNATURE);
    if (IsKeyRegistered (&CurrentNotify->KeyData, KeyData)) {
      //
      // The key notification function needs to run at TPL_CALLBACK
      // while current TPL is TPL_NOTIFY. It will be invoked in
      // KeyNotifyProcessHandler() which runs at TPL_CALLBACK.
      //
      PushEfikeyBufTail (&ConsoleIn->EfiKeyQueueForNotify, KeyData);
      gBS->SignalEvent (ConsoleIn->KeyNotifyProcessEvent);
      break;
    }
  }

  PushEfikeyBufTail (&ConsoleIn->EfiKeyQueue, KeyData);
Exit:
  gBS->RestoreTPL (OldTpl);
}

/**
  Create KEYPAD_CONSOLE_IN_DEV instance on controller.

  @param This                Pointer of EFI_DRIVER_BINDING_PROTOCOL
  @param Controller          driver controller handle
  @param RemainingDevicePath Children's device path

  @retval                    whether success to create floppy control instance.
**/
EFI_STATUS
EFIAPI
KeypadControllerDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath)
{
  EFI_STATUS              Status;
  EFI_STATUS              CleanupStatus;
  KEYPAD_DEVICE_PROTOCOL *KeypadDevice;
  KEYPAD_CONSOLE_IN_DEV  *ConsoleIn = NULL;

  Status = KeypadCleanupPending (This->DriverBindingHandle, Controller);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ConsoleIn = AllocateZeroPool (sizeof (KEYPAD_CONSOLE_IN_DEV));
  if (ConsoleIn == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Get the Keypad Protocol on Controller's handle
  //
  Status = gBS->OpenProtocol (Controller, &gKeypadDeviceProtocolGuid, (VOID **)&KeypadDevice, This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Open Keypad Protocol! Status = %r\n", Status));
    FreePool (ConsoleIn);
    return Status;
  }

  //
  // Setup the device instance
  //
  ConsoleIn->Signature                         = KEYPAD_CONSOLE_IN_DEV_SIGNATURE;
  ConsoleIn->Handle                            = Controller;
  ConsoleIn->DeviceProtocolOpen                = TRUE;
  (ConsoleIn->ConIn).Reset                     = KeypadEfiReset;
  (ConsoleIn->ConIn).ReadKeyStroke             = KeypadReadKeyStroke;
  ConsoleIn->KeypadDevice                      = KeypadDevice;
  ConsoleIn->KeypadReturnApi.PushEfikeyBufTail = KeypadReturnApiPushEfikeyBufTail;
  ConsoleIn->Last                              = 0;
  ConsoleIn->LastValid                         = FALSE;
  ConsoleIn->ConInEx.Reset                     = KeypadEfiResetEx;
  ConsoleIn->ConInEx.ReadKeyStrokeEx           = KeypadReadKeyStrokeEx;
  ConsoleIn->ConInEx.SetState                  = KeypadSetState;
  ConsoleIn->ConInEx.RegisterKeyNotify         = KeypadRegisterKeyNotify;
  ConsoleIn->ConInEx.UnregisterKeyNotify       = KeypadUnregisterKeyNotify;

  InitializeListHead(&ConsoleIn->NotifyList);

  //
  // Fix for random hangs in System waiting for the Key if no KBC is present in
  // BIOS. When KBC decode (IO port 0x60/0x64 decode) is not enabled, Keypad Read
  // will read back as 0xFF and return status is EFI_SUCCESS. So instead we read
  // status register to detect after read if KBC decode is enabled.
  //

  //
  // Setup the WaitForKey event
  //
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKey, ConsoleIn, &((ConsoleIn->ConIn).WaitForKey));
  if (EFI_ERROR (Status)) {
    ConsoleIn->ConIn.WaitForKey = NULL; // Failed creation does not transfer event ownership.
    DEBUG ((EFI_D_ERROR, "Failed to Create WaitForKey Event! Status = %r\n", Status));
    goto CleanUp;
  }

  //
  // Setup the WaitForKeyEx event
  //
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeypadWaitForKeyEx, ConsoleIn, &(ConsoleIn->ConInEx.WaitForKeyEx));
  if (EFI_ERROR (Status)) {
    ConsoleIn->ConInEx.WaitForKeyEx = NULL; // Failed creation does not transfer event ownership.
    DEBUG ((EFI_D_ERROR, "Failed to Create WaitForKeyEx Event! Status = %r\n", Status));
    goto CleanUp;
  }

  //
  // Setup a periodic timer, used for reading keystrokes at a fixed interval
  //
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, KeypadTimerHandler, ConsoleIn, &ConsoleIn->TimerEvent);
  if (EFI_ERROR (Status)) {
    ConsoleIn->TimerEvent = NULL; // Failed creation does not transfer event ownership.
    DEBUG ((EFI_D_ERROR, "Failed to Create Periodic Timer Event! Status = %r\n", Status));
    goto CleanUp;
  }

  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, KeyNotifyProcessHandler, ConsoleIn, &ConsoleIn->KeyNotifyProcessEvent);
  if (EFI_ERROR (Status)) {
    ConsoleIn->KeyNotifyProcessEvent = NULL; // Failed creation does not transfer event ownership.
    DEBUG ((EFI_D_ERROR, "Failed to Create KeyNotify Event! Status = %r\n", Status));
    goto CleanUp;
  }

  //
  // Reset the keypad device
  //
  Status = ConsoleIn->ConInEx.Reset (&ConsoleIn->ConInEx, FALSE);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Reset Keypad Device! Status = %r\n", Status));
    goto CleanUp;
  }

  ConsoleIn->ControllerNameTable = NULL;

  Status = AddUnicodeString2 ("eng", gKeypadComponentName.SupportedLanguages, &ConsoleIn->ControllerNameTable, L"Keypad Device", TRUE);
  if (EFI_ERROR (Status)) {
    goto CleanUp;
  }
  Status = AddUnicodeString2 ("en", gKeypadComponentName2.SupportedLanguages, &ConsoleIn->ControllerNameTable, L"Keypad Device", FALSE);
  if (EFI_ERROR (Status)) {
    goto CleanUp;
  }

  //
  // Install protocol interfaces for the keypad device.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (&Controller, &gEfiSimpleTextInProtocolGuid, &ConsoleIn->ConIn, &gEfiSimpleTextInputExProtocolGuid, &ConsoleIn->ConInEx, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Install Keypad Protocol! Status = %r\n", Status));
    (VOID)KeypadReconcileProtocols (Controller, ConsoleIn);
    goto CleanUp;
  }

  ConsoleIn->TextInInstalled = TRUE;
  ConsoleIn->TextInExInstalled = TRUE;

  Status = gBS->SetTimer (ConsoleIn->TimerEvent, TimerPeriodic, KEYPAD_TIMER_INTERVAL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Set Timer State! Status = %r\n", Status));
    goto CleanUp;
  }
  goto exit;

CleanUp:
  CleanupStatus = KeypadRememberAndCleanup (Controller, This->DriverBindingHandle, ConsoleIn);
  if (EFI_ERROR (CleanupStatus)) {
    return CleanupStatus;
  }

exit:
  return Status;
}

/**
  Stop this driver on ControllerHandle. Support stopping any child handles
  created by this driver.

  @param  This              Protocol instance pointer.
  @param  ControllerHandle  Handle of device to stop driver on
  @param  NumberOfChildren  Number of Handles in ChildHandleBuffer. If number of
                            children is zero stop the entire bus driver.
  @param  ChildHandleBuffer List of Child Handles to Stop.

  @retval EFI_SUCCESS       This driver is removed ControllerHandle
  @retval other             This driver was not removed from this device
**/
EFI_STATUS
EFIAPI
KeypadControllerDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                   Controller,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                  *ChildHandleBuffer)
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
  KEYPAD_CONSOLE_IN_DEV          *ConsoleIn;

  if (NumberOfChildren != 0) {
    return EFI_UNSUPPORTED;
  }

  if (mPendingConsoleIn != NULL) {
    return KeypadCleanupPending (This->DriverBindingHandle, Controller);
  }
  if (mPendingSupportedClose) {
    Status = KeypadClosePendingSupported (This->DriverBindingHandle, Controller);
    return EFI_ERROR (Status) ? Status : EFI_NOT_STARTED;
  }

  Status = gBS->OpenProtocol (Controller, &gEfiSimpleTextInProtocolGuid, (VOID **)&ConIn, This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Open STI Protocol! Status = %r\n", Status));
    return Status;
  }

  ConsoleIn = KEYPAD_CONSOLE_IN_DEV_FROM_THIS(ConIn);
  if (ConsoleIn->Signature != KEYPAD_CONSOLE_IN_DEV_SIGNATURE || ConsoleIn->Handle != Controller) {
    return EFI_COMPROMISED_DATA;
  }
  if (ConsoleIn->NotifyDispatchActive) {
    return EFI_NOT_READY;
  }
  Status = KeypadReconcileProtocols (Controller, ConsoleIn);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  ConsoleIn->Stopping = TRUE;
  ConsoleIn->KeypadErr = TRUE;
  return KeypadRememberAndCleanup (Controller, This->DriverBindingHandle, ConsoleIn);
}

/**
  Free the waiting key notify list.

  @param ListHead                Pointer to list head

  @retval EFI_INVALID_PARAMETER  ListHead is NULL
  @retval EFI_SUCCESS            Sucess to free NotifyList
**/
EFI_STATUS
KbdFreeNotifyList (IN OUT LIST_ENTRY *ListHead)
{
  KEYPAD_CONSOLE_IN_EX_NOTIFY *NotifyNode;
  EFI_STATUS                   Status;

  if (ListHead == NULL) {
    DEBUG ((EFI_D_ERROR, "ListHead is NULL!\n"));
    return EFI_INVALID_PARAMETER;
  }

  while (!IsListEmpty (ListHead)) {
    NotifyNode = CR (ListHead->ForwardLink, KEYPAD_CONSOLE_IN_EX_NOTIFY, NotifyEntry, KEYPAD_CONSOLE_IN_EX_NOTIFY_SIGNATURE);
    RemoveEntryList (ListHead->ForwardLink);

    Status = gBS->FreePool (NotifyNode);
    if (EFI_ERROR (Status)) {
      InsertTailList (ListHead, &NotifyNode->NotifyEntry);
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  The module Entry Point for module Keypad.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.
**/
EFI_STATUS
EFIAPI
InitializeKeypad (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  //
  // Install driver model protocol(s).
  //
  Status = EfiLibInstallDriverBindingComponentName2 (ImageHandle, SystemTable, &gKeypadControllerDriver, ImageHandle, &gKeypadComponentName, &gKeypadComponentName2);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Install Driver Model Protocols!\n"));
    ASSERT_EFI_ERROR (Status);
  }

  return Status;
}
