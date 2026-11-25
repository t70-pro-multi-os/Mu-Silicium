/**
  Routines that talk to the Keypad Device protocol.

  Copyright (c) 2006 - 2016, Intel Corporation. All rights reserved.<BR>

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD
  License which accompanies this distribution.  The full text of the license may
  be found at http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "KeypadDxe.h"

#include <Library/DebugLib.h>
#include <Library/TimerLib.h>

/**
  Display error message.

  @param ConsoleIn Pointer to instance of KEYPAD_CONSOLE_IN_DEV
  @param ErrMsg    Unicode string of error message
**/
VOID
KeypadError (
  IN KEYPAD_CONSOLE_IN_DEV *ConsoleIn,
  IN CHAR16                *ErrMsg)
{
  ConsoleIn->KeypadErr = TRUE;
}

/**
  Timer event handler: read a series of scancodes from 8042
  and put them into memory scancode buffer.
  it read as much scancodes to either fill
  the memory buffer or empty the keypad buffer.
  It is registered as running under TPL_CALLBACK.

  @param Event       The timer event
  @param Context     A KEYPAD_CONSOLE_IN_DEV pointer
**/
VOID
EFIAPI
KeypadTimerHandler (
  IN EFI_EVENT Event,
  IN VOID     *Context)
{
  EFI_STATUS             Status;
  KEYPAD_CONSOLE_IN_DEV *ConsoleIn;

  ConsoleIn = (KEYPAD_CONSOLE_IN_DEV *)Context;

  if (ConsoleIn == NULL || ConsoleIn->Stopping || ConsoleIn->KeypadErr ||
      ConsoleIn->KeypadDevice == NULL || ConsoleIn->KeypadDevice->GetKeys == NULL) {
    return;
  }

  UINT64 CurrentCounterValue = GetPerformanceCounter ();
  UINT64 DeltaCounter        = ConsoleIn->LastValid ? CurrentCounterValue - ConsoleIn->Last : 0;
  ConsoleIn->Last            = CurrentCounterValue;
  ConsoleIn->LastValid       = TRUE;

  Status = ConsoleIn->KeypadDevice->GetKeys (
             ConsoleIn->KeypadDevice,
             &ConsoleIn->KeypadReturnApi,
             GetTimeInNanoSecond (DeltaCounter)
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN, "Keypad GetKeys returned %r; polling will retry.\n", Status));
  }

}

/**
  Perform 8042 controller and keypad Initialization.
  If ExtendedVerification is TRUE, do additional test for
  the keypad interface

  @param ConsoleIn            - KEYPAD_CONSOLE_IN_DEV instance pointer
  @param ExtendedVerification - indicates a thorough initialization

  @retval EFI_DEVICE_ERROR      Fail to init keypad
  @retval EFI_SUCCESS           Success to init keypad
**/
EFI_STATUS
InitKeypad (
  IN OUT KEYPAD_CONSOLE_IN_DEV *ConsoleIn,
  IN BOOLEAN                    ExtendedVerification)
{
  EFI_STATUS Status;

  if (ConsoleIn == NULL || ConsoleIn->KeypadDevice == NULL || ConsoleIn->KeypadDevice->Reset == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = ConsoleIn->KeypadDevice->Reset (ConsoleIn->KeypadDevice);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ConsoleIn->Last      = 0;
  ConsoleIn->LastValid = FALSE;

  //
  // Clear Memory Scancode Buffer
  //
  ConsoleIn->EfiKeyQueue.Head          = 0;
  ConsoleIn->EfiKeyQueue.Tail          = 0;
  ConsoleIn->EfiKeyQueueForNotify.Head = 0;
  ConsoleIn->EfiKeyQueueForNotify.Tail = 0;

  //
  // Reset the status indicators
  //
  ConsoleIn->CapsLock   = FALSE;
  ConsoleIn->NumLock    = FALSE;
  ConsoleIn->ScrollLock = FALSE;
  ConsoleIn->LeftCtrl   = FALSE;
  ConsoleIn->RightCtrl  = FALSE;
  ConsoleIn->LeftAlt    = FALSE;
  ConsoleIn->RightAlt   = FALSE;
  ConsoleIn->LeftShift  = FALSE;
  ConsoleIn->RightShift = FALSE;
  ConsoleIn->LeftLogo   = FALSE;
  ConsoleIn->RightLogo  = FALSE;
  ConsoleIn->Menu       = FALSE;
  ConsoleIn->SysReq     = FALSE;

  ConsoleIn->IsSupportPartialKey = FALSE;

  return EFI_SUCCESS;
}
