// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the production Keypad DXE callbacks with mocked UEFI services.
#include <Uefi.h>

#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include <Protocol/EfiKeypadDevice.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(Expression) do { \
  Checks++; \
  if (!(Expression)) { \
    fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #Expression); \
    abort (); \
  } \
} while (0)

#define TEST_EVENT_MAX 16U
#define TEST_ALLOC_MAX 128U

static unsigned Checks;
static EFI_BOOT_SERVICES mServices;
EFI_BOOT_SERVICES *gBS = &mServices;

EFI_GUID gKeypadDeviceProtocolGuid = KEYPAD_DEVICE_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInProtocolGuid = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInputExProtocolGuid = EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;

EFI_COMPONENT_NAME_PROTOCOL gKeypadComponentName = { NULL, NULL, "eng" };
EFI_COMPONENT_NAME2_PROTOCOL gKeypadComponentName2 = { NULL, NULL, "en" };

typedef struct {
  EFI_EVENT_NOTIFY Notify;
  VOID             *Context;
  UINT32            Type;
  BOOLEAN           Active;
  BOOLEAN           TimerArmed;
  UINTN             Signals;
} TEST_EVENT;

typedef struct {
  VOID    *Pointer;
  BOOLEAN  Live;
} TEST_ALLOCATION;

static TEST_EVENT                mEvents[TEST_EVENT_MAX];
static TEST_ALLOCATION           mAllocations[TEST_ALLOC_MAX];
static EFI_HANDLE                mController;
static EFI_HANDLE                mAgent;
static KEYPAD_DEVICE_PROTOCOL    mKeypadDevice;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL    *mInstalledTextIn;
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *mInstalledTextInEx;
static BOOLEAN                   mDeviceOpen;
static BOOLEAN                   mTextInInstalled;
static BOOLEAN                   mTextInExInstalled;
static EFI_STATUS                mResetStatus;
static EFI_STATUS                mGetKeysStatus;
static EFI_STATUS                mInstallStatus;
static EFI_STATUS                mCloseEventStatus;
static EFI_STATUS                mCloseProtocolStatus;
static EFI_STATUS                mUninstallStatus;
static EFI_EVENT                 mFailCloseEvent;
static VOID                     *mFailFreePointer;
static UINTN                     mFailCreateAt;
static UINTN                     mFailCloseEventCount;
static UINTN                     mFailCloseProtocolCount;
static UINTN                     mFailUninstallCount;
static UINTN                     mPartialInstallMask;
static BOOLEAN                   mNotifyDuringClose;
static UINTN                     mCreateCalls;
static UINTN                     mCloseEventCalls;
static UINTN                     mCloseProtocolCalls;
static UINTN                     mOpenCalls;
static UINTN                     mInstallCalls;
static UINTN                     mUninstallCalls;
static UINTN                     mResetCalls;
static UINTN                     mGetKeysCalls;
static UINTN                     mSetTimerCancelCalls;
static UINTN                     mSetTimerArmCalls;
static UINT64                    mCounter;
static UINT64                    mDeltas[8];
static UINTN                     mDeltaCount;
static EFI_TPL                   mCurrentTpl;

#include "../../../../Silicon/Silicium/SiliciumPkg/Drivers/KeypadDxe/KeypadController.c"
#include "../../../../Silicon/Silicium/SiliciumPkg/Drivers/KeypadDxe/KeypadTextIn.c"
#include "../../../../Silicon/Silicium/SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.c"

static
INTN
FindAllocation (
  IN VOID *Pointer
  )
{
  UINTN Index;

  for (Index = 0; Index < TEST_ALLOC_MAX; Index++) {
    if (mAllocations[Index].Live && mAllocations[Index].Pointer == Pointer) {
      return (INTN)Index;
    }
  }

  return -1;
}

static
VOID *
TrackAllocation (
  IN VOID *Pointer
  )
{
  UINTN Index;

  if (Pointer == NULL) {
    return NULL;
  }
  for (Index = 0; Index < TEST_ALLOC_MAX; Index++) {
    if (!mAllocations[Index].Live) {
      mAllocations[Index].Pointer = Pointer;
      mAllocations[Index].Live = TRUE;
      return Pointer;
    }
  }

  free (Pointer);
  return NULL;
}

static
UINTN
LiveAllocationCount (
  VOID
  )
{
  UINTN Index;
  UINTN Count;

  Count = 0;
  for (Index = 0; Index < TEST_ALLOC_MAX; Index++) {
    Count += mAllocations[Index].Live ? 1U : 0U;
  }
  return Count;
}

VOID *
EFIAPI
AllocatePool (
  IN UINTN AllocationSize
  )
{
  return TrackAllocation (malloc (AllocationSize == 0 ? 1 : AllocationSize));
}

VOID *
EFIAPI
AllocateZeroPool (
  IN UINTN AllocationSize
  )
{
  VOID *Buffer;

  Buffer = AllocatePool (AllocationSize);
  if (Buffer != NULL) {
    memset (Buffer, 0, AllocationSize);
  }
  return Buffer;
}

VOID *
EFIAPI
AllocateCopyPool (
  IN UINTN       AllocationSize,
  IN CONST VOID *Buffer
  )
{
  VOID *Copy;

  if (Buffer == NULL) {
    return NULL;
  }
  Copy = AllocatePool (AllocationSize);
  if (Copy != NULL) {
    memcpy (Copy, Buffer, AllocationSize);
  }
  return Copy;
}

EFI_STATUS
EFIAPI
MockBootFreePool (
  IN VOID *Buffer
  )
{
  INTN Index;

  if (Buffer == mFailFreePointer) {
    mFailFreePointer = NULL;
    return EFI_DEVICE_ERROR;
  }
  Index = FindAllocation (Buffer);
  if (Index < 0) {
    return EFI_INVALID_PARAMETER;
  }
  free (Buffer);
  mAllocations[Index].Pointer = NULL;
  mAllocations[Index].Live = FALSE;
  return EFI_SUCCESS;
}

VOID
EFIAPI
FreePool (
  IN VOID *Buffer
  )
{
  (VOID)MockBootFreePool (Buffer);
}

VOID *
EFIAPI
CopyMem (
  OUT VOID       *Destination,
  IN  CONST VOID *Source,
  IN  UINTN       Length
  )
{
  return memcpy (Destination, Source, Length);
}

VOID *
EFIAPI
SetMem (
  OUT VOID *Buffer,
  IN  UINTN Length,
  IN  UINT8 Value
  )
{
  return memset (Buffer, Value, Length);
}

VOID *
EFIAPI
ZeroMem (
  OUT VOID *Buffer,
  IN  UINTN Length
  )
{
  return memset (Buffer, 0, Length);
}

UINT64
EFIAPI
GetPerformanceCounter (
  VOID
  )
{
  return mCounter;
}

UINT64
EFIAPI
GetTimeInNanoSecond (
  IN UINT64 Ticks
  )
{
  return Ticks;
}

EFI_TPL
EFIAPI
MockRaiseTpl (
  IN EFI_TPL NewTpl
  )
{
  EFI_TPL OldTpl;

  OldTpl = mCurrentTpl;
  mCurrentTpl = NewTpl;
  return OldTpl;
}

VOID
EFIAPI
MockRestoreTpl (
  IN EFI_TPL OldTpl
  )
{
  mCurrentTpl = OldTpl;
}

static
EFI_STATUS
EFIAPI
MockOpenProtocol (
  IN  EFI_HANDLE Handle,
  IN  EFI_GUID  *Protocol,
  OUT VOID     **Interface,
  IN  EFI_HANDLE AgentHandle,
  IN  EFI_HANDLE ControllerHandle,
  IN  UINT32     Attributes
  )
{
  CHECK (Handle == mController && AgentHandle == mAgent && ControllerHandle == mController);
  mOpenCalls++;
  if (Protocol == &gKeypadDeviceProtocolGuid && Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER) {
    if (mDeviceOpen) {
      return EFI_ALREADY_STARTED;
    }
    mDeviceOpen = TRUE;
    if (Interface != NULL) {
      *Interface = &mKeypadDevice;
    }
    return EFI_SUCCESS;
  }
  if (Protocol == &gEfiSimpleTextInProtocolGuid && Attributes == EFI_OPEN_PROTOCOL_GET_PROTOCOL) {
    if (!mTextInInstalled) {
      return EFI_NOT_FOUND;
    }
    if (Interface != NULL) {
      *Interface = mInstalledTextIn;
    }
    return EFI_SUCCESS;
  }
  return EFI_UNSUPPORTED;
}

static
EFI_STATUS
EFIAPI
MockHandleProtocol (
  IN  EFI_HANDLE Handle,
  IN  EFI_GUID  *Protocol,
  OUT VOID     **Interface
  )
{
  CHECK (Handle == mController && Interface != NULL);
  if (Protocol == &gEfiSimpleTextInProtocolGuid && mTextInInstalled) {
    *Interface = mInstalledTextIn;
    return EFI_SUCCESS;
  }
  if (Protocol == &gEfiSimpleTextInputExProtocolGuid && mTextInExInstalled) {
    *Interface = mInstalledTextInEx;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static
EFI_STATUS
EFIAPI
MockCloseProtocol (
  IN EFI_HANDLE Handle,
  IN EFI_GUID  *Protocol,
  IN EFI_HANDLE AgentHandle,
  IN EFI_HANDLE ControllerHandle
  )
{
  CHECK (Handle == mController && Protocol == &gKeypadDeviceProtocolGuid);
  CHECK (AgentHandle == mAgent && ControllerHandle == mController && mDeviceOpen);
  mCloseProtocolCalls++;
  if (mFailCloseProtocolCount != 0) {
    mFailCloseProtocolCount--;
    return mCloseProtocolStatus;
  }
  mDeviceOpen = FALSE;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockCreateEvent (
  IN  UINT32           Type,
  IN  EFI_TPL          NotifyTpl,
  IN  EFI_EVENT_NOTIFY NotifyFunction,
  IN  VOID            *NotifyContext,
  OUT EFI_EVENT       *Event
  )
{
  UINTN Index;

  CHECK (Event != NULL && NotifyTpl <= TPL_NOTIFY);
  if (NotifyFunction == KeypadTimerHandler) {
    CHECK (NotifyTpl == TPL_CALLBACK);
  }
  mCreateCalls++;
  *Event = NULL;
  if (mFailCreateAt != 0 && mCreateCalls == mFailCreateAt) {
    *Event = (EFI_EVENT)(UINTN)0xdead;
    return EFI_OUT_OF_RESOURCES;
  }
  for (Index = 0; Index < TEST_EVENT_MAX; Index++) {
    if (!mEvents[Index].Active) {
      mEvents[Index].Notify = NotifyFunction;
      mEvents[Index].Context = NotifyContext;
      mEvents[Index].Type = Type;
      mEvents[Index].Active = TRUE;
      mEvents[Index].TimerArmed = FALSE;
      mEvents[Index].Signals = 0;
      *Event = (EFI_EVENT)&mEvents[Index];
      return EFI_SUCCESS;
    }
  }
  return EFI_OUT_OF_RESOURCES;
}

static
EFI_STATUS
EFIAPI
MockSetTimer (
  IN EFI_EVENT      Event,
  IN EFI_TIMER_DELAY Type,
  IN UINT64          TriggerTime
  )
{
  TEST_EVENT *Record;

  CHECK (Event != NULL);
  Record = (TEST_EVENT *)Event;
  CHECK (Record >= mEvents && Record < mEvents + TEST_EVENT_MAX && Record->Active);
  if (Type == TimerCancel) {
    CHECK (TriggerTime == 0);
    mSetTimerCancelCalls++;
    Record->TimerArmed = FALSE;
  } else {
    KEYPAD_CONSOLE_IN_DEV *ConsoleIn = Record->Context;
    CHECK (Type == TimerPeriodic && TriggerTime == KEYPAD_TIMER_INTERVAL);
    CHECK (ConsoleIn->KeyNotifyProcessEvent != NULL && mTextInInstalled && mTextInExInstalled);
    mSetTimerArmCalls++;
    Record->TimerArmed = TRUE;
  }
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockCloseEvent (
  IN EFI_EVENT Event
  )
{
  TEST_EVENT *Record;

  CHECK (Event != NULL);
  Record = (TEST_EVENT *)Event;
  CHECK (Record >= mEvents && Record < mEvents + TEST_EVENT_MAX && Record->Active);
  mCloseEventCalls++;
  if (mNotifyDuringClose && Record->Notify != NULL) {
    mNotifyDuringClose = FALSE;
    Record->Notify (Event, Record->Context);
  }
  if (Event == mFailCloseEvent && mFailCloseEventCount != 0) {
    mFailCloseEventCount--;
    return mCloseEventStatus;
  }
  Record->Active = FALSE;
  Record->TimerArmed = FALSE;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockSignalEvent (
  IN EFI_EVENT Event
  )
{
  TEST_EVENT *Record;

  CHECK (Event != NULL);
  Record = (TEST_EVENT *)Event;
  CHECK (Record >= mEvents && Record < mEvents + TEST_EVENT_MAX && Record->Active);
  Record->Signals++;
  if ((Record->Type & EVT_NOTIFY_SIGNAL) != 0 && Record->Notify != NULL) {
    Record->Notify (Event, Record->Context);
  }
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockInstallMultipleProtocolInterfaces (
  IN OUT EFI_HANDLE *Handle,
  ...
  )
{
  va_list  Args;
  EFI_GUID *Protocol;
  VOID     *Interface;
  VOID     *TextIn;
  VOID     *TextInEx;

  CHECK (Handle != NULL && *Handle == mController);
  mInstallCalls++;
  TextIn = NULL;
  TextInEx = NULL;
  va_start (Args, Handle);
  while ((Protocol = va_arg (Args, EFI_GUID *)) != NULL) {
    Interface = va_arg (Args, VOID *);
    if (Protocol == &gEfiSimpleTextInProtocolGuid) {
      TextIn = Interface;
    } else if (Protocol == &gEfiSimpleTextInputExProtocolGuid) {
      TextInEx = Interface;
    } else {
      CHECK (FALSE);
    }
  }
  va_end (Args);
  CHECK (TextIn != NULL && TextInEx != NULL);

  if (mInstallStatus != EFI_SUCCESS) {
    if ((mPartialInstallMask & BIT0) != 0) {
      mInstalledTextIn = (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)TextIn;
      mTextInInstalled = TRUE;
    }
    if ((mPartialInstallMask & BIT1) != 0) {
      mInstalledTextInEx = (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *)TextInEx;
      mTextInExInstalled = TRUE;
    }
    return mInstallStatus;
  }

  mInstalledTextIn = (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *)TextIn;
  mInstalledTextInEx = (EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *)TextInEx;
  mTextInInstalled = TRUE;
  mTextInExInstalled = TRUE;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockUninstallProtocolInterface (
  IN EFI_HANDLE Handle,
  IN EFI_GUID  *Protocol,
  IN VOID      *Interface
  )
{
  CHECK (Handle == mController);
  mUninstallCalls++;
  if (mFailUninstallCount != 0) {
    mFailUninstallCount--;
    return mUninstallStatus;
  }
  if (Protocol == &gEfiSimpleTextInProtocolGuid && Interface == mInstalledTextIn) {
    mTextInInstalled = FALSE;
    mInstalledTextIn = NULL;
    return EFI_SUCCESS;
  }
  if (Protocol == &gEfiSimpleTextInputExProtocolGuid && Interface == mInstalledTextInEx) {
    mTextInExInstalled = FALSE;
    mInstalledTextInEx = NULL;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

static
EFI_STATUS
EFIAPI
MockUninstallMultipleProtocolInterfaces (
  IN EFI_HANDLE Handle,
  ...
  )
{
  va_list  Args;
  EFI_GUID *Protocol;
  VOID     *Interface;
  BOOLEAN   FoundTextIn;
  BOOLEAN   FoundTextInEx;

  CHECK (Handle == mController);
  FoundTextIn = FALSE;
  FoundTextInEx = FALSE;
  va_start (Args, Handle);
  while ((Protocol = va_arg (Args, EFI_GUID *)) != NULL) {
    Interface = va_arg (Args, VOID *);
    if (Protocol == &gEfiSimpleTextInProtocolGuid && Interface == mInstalledTextIn) {
      FoundTextIn = TRUE;
    } else if (Protocol == &gEfiSimpleTextInputExProtocolGuid && Interface == mInstalledTextInEx) {
      FoundTextInEx = TRUE;
    } else {
      CHECK (FALSE);
    }
  }
  va_end (Args);
  CHECK (FoundTextIn == mTextInInstalled && FoundTextInEx == mTextInExInstalled);
  mUninstallCalls++;

  if (mFailUninstallCount != 0) {
    mFailUninstallCount--;
    if ((mPartialInstallMask & BIT0) != 0 && FoundTextIn) {
      mTextInInstalled = FALSE;
      mInstalledTextIn = NULL;
      mPartialInstallMask &= ~BIT0;
    }
    return mUninstallStatus;
  }

  mTextInInstalled = FALSE;
  mTextInExInstalled = FALSE;
  mInstalledTextIn = NULL;
  mInstalledTextInEx = NULL;
  return EFI_SUCCESS;
}

static
EFI_STATUS
EFIAPI
MockDeviceReset (
  IN KEYPAD_DEVICE_PROTOCOL *This
  )
{
  CHECK (This == &mKeypadDevice);
  mResetCalls++;
  return mResetStatus;
}

static
EFI_STATUS
EFIAPI
MockDeviceGetKeys (
  IN KEYPAD_DEVICE_PROTOCOL *This,
  IN KEYPAD_RETURN_API      *ReturnApi,
  IN UINT64                  Delta
  )
{
  CHECK (This == &mKeypadDevice && ReturnApi != NULL && ReturnApi->PushEfikeyBufTail != NULL);
  if (mDeltaCount < ARRAY_SIZE (mDeltas)) {
    mDeltas[mDeltaCount++] = Delta;
  }
  mGetKeysCalls++;
  return mGetKeysStatus;
}

static
EFI_STATUS
EFIAPI
MockKeyNotification (
  IN EFI_KEY_DATA *KeyData
  )
{
  CHECK (KeyData != NULL);
  return EFI_SUCCESS;
}

static
UINTN
CountActiveEvents (
  VOID
  )
{
  UINTN Index;
  UINTN Count;

  Count = 0;
  for (Index = 0; Index < TEST_EVENT_MAX; Index++) {
    Count += mEvents[Index].Active ? 1U : 0U;
  }
  return Count;
}

static
VOID
InitializeServices (
  VOID
  )
{
  memset (&mServices, 0, sizeof (mServices));
  mServices.RaiseTPL = MockRaiseTpl;
  mServices.RestoreTPL = MockRestoreTpl;
  mServices.OpenProtocol = MockOpenProtocol;
  mServices.HandleProtocol = MockHandleProtocol;
  mServices.CloseProtocol = MockCloseProtocol;
  mServices.CreateEvent = MockCreateEvent;
  mServices.SetTimer = MockSetTimer;
  mServices.CloseEvent = MockCloseEvent;
  mServices.SignalEvent = MockSignalEvent;
  mServices.FreePool = MockBootFreePool;
  mServices.InstallMultipleProtocolInterfaces = MockInstallMultipleProtocolInterfaces;
  mServices.UninstallProtocolInterface = MockUninstallProtocolInterface;
  mServices.UninstallMultipleProtocolInterfaces = MockUninstallMultipleProtocolInterfaces;
  gBS = &mServices;
}

static
VOID
ResetFixture (
  VOID
  )
{
  CHECK (mPendingConsoleIn == NULL);
  CHECK (!mPendingSupportedClose);
  CHECK (LiveAllocationCount () == 0);
  memset (mEvents, 0, sizeof (mEvents));
  memset (mAllocations, 0, sizeof (mAllocations));
  memset (&mKeypadDevice, 0, sizeof (mKeypadDevice));
  InitializeServices ();

  mController = (EFI_HANDLE)(UINTN)0x1000;
  mAgent = (EFI_HANDLE)(UINTN)0x2000;
  gKeypadControllerDriver.DriverBindingHandle = mAgent;
  mKeypadDevice.Reset = MockDeviceReset;
  mKeypadDevice.GetKeys = MockDeviceGetKeys;
  mInstalledTextIn = NULL;
  mInstalledTextInEx = NULL;
  mDeviceOpen = FALSE;
  mTextInInstalled = FALSE;
  mTextInExInstalled = FALSE;
  mResetStatus = EFI_SUCCESS;
  mGetKeysStatus = EFI_SUCCESS;
  mInstallStatus = EFI_SUCCESS;
  mCloseEventStatus = EFI_DEVICE_ERROR;
  mCloseProtocolStatus = EFI_DEVICE_ERROR;
  mUninstallStatus = EFI_ACCESS_DENIED;
  mFailCloseEvent = NULL;
  mFailFreePointer = NULL;
  mFailCreateAt = 0;
  mFailCloseEventCount = 0;
  mFailCloseProtocolCount = 0;
  mFailUninstallCount = 0;
  mPartialInstallMask = 0;
  mNotifyDuringClose = FALSE;
  mCreateCalls = 0;
  mCloseEventCalls = 0;
  mCloseProtocolCalls = 0;
  mOpenCalls = 0;
  mInstallCalls = 0;
  mUninstallCalls = 0;
  mResetCalls = 0;
  mGetKeysCalls = 0;
  mSetTimerCancelCalls = 0;
  mSetTimerArmCalls = 0;
  mCounter = 0;
  memset (mDeltas, 0, sizeof (mDeltas));
  mDeltaCount = 0;
  mCurrentTpl = TPL_APPLICATION;
}

static
EFI_STATUS
EFIAPI
MockAddUnicodeString2 (
  IN     CONST CHAR8              *Language,
  IN     CONST CHAR8              *SupportedLanguages,
  IN OUT EFI_UNICODE_STRING_TABLE **UnicodeStringTable,
  IN     CONST CHAR16             *UnicodeString,
  IN     BOOLEAN                    Iso639Language
  )
{
  EFI_UNICODE_STRING_TABLE *OldTable;
  EFI_UNICODE_STRING_TABLE *NewTable;
  UINTN                     Count;
  UINTN                     Length;
  UINTN                     Index;

  (VOID)SupportedLanguages;
  (VOID)Iso639Language;
  CHECK (Language != NULL && UnicodeString != NULL && UnicodeStringTable != NULL);
  OldTable = *UnicodeStringTable;
  Count = 0;
  if (OldTable != NULL) {
    while (OldTable[Count].Language != NULL) {
      Count++;
    }
  }
  NewTable = AllocateZeroPool ((Count + 2) * sizeof (*NewTable));
  if (NewTable == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (Index = 0; Index < Count; Index++) {
    NewTable[Index] = OldTable[Index];
  }
  NewTable[Count].Language = AllocateCopyPool (strlen ((CONST CHAR8 *)Language) + 1, Language);
  if (NewTable[Count].Language == NULL) {
    FreePool (NewTable);
    return EFI_OUT_OF_RESOURCES;
  }
  for (Length = 0; UnicodeString[Length] != 0; Length++) {
  }
  NewTable[Count].UnicodeString = AllocateCopyPool ((Length + 1) * sizeof (CHAR16), UnicodeString);
  if (NewTable[Count].UnicodeString == NULL) {
    FreePool (NewTable[Count].Language);
    FreePool (NewTable);
    return EFI_OUT_OF_RESOURCES;
  }
  if (OldTable != NULL) {
    FreePool (OldTable);
  }
  *UnicodeStringTable = NewTable;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
AddUnicodeString2 (
  IN     CONST CHAR8              *Language,
  IN     CONST CHAR8              *SupportedLanguages,
  IN OUT EFI_UNICODE_STRING_TABLE **UnicodeStringTable,
  IN     CONST CHAR16             *UnicodeString,
  IN     BOOLEAN                    Iso639Language
  )
{
  return MockAddUnicodeString2 (Language, SupportedLanguages, UnicodeStringTable, UnicodeString, Iso639Language);
}

EFI_STATUS
EFIAPI
FreeUnicodeStringTable (
  IN EFI_UNICODE_STRING_TABLE *UnicodeStringTable
  )
{
  UINTN Index;

  if (UnicodeStringTable == NULL) {
    return EFI_SUCCESS;
  }
  for (Index = 0; UnicodeStringTable[Index].Language != NULL; Index++) {
    FreePool (UnicodeStringTable[Index].Language);
    FreePool (UnicodeStringTable[Index].UnicodeString);
  }
  FreePool (UnicodeStringTable);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
EfiLibInstallDriverBindingComponentName2 (
  IN CONST EFI_HANDLE                   ImageHandle,
  IN CONST EFI_SYSTEM_TABLE             *SystemTable,
  IN EFI_DRIVER_BINDING_PROTOCOL        *DriverBinding,
  IN EFI_HANDLE                         DriverBindingHandle,
  IN CONST EFI_COMPONENT_NAME_PROTOCOL  *ComponentName OPTIONAL,
  IN CONST EFI_COMPONENT_NAME2_PROTOCOL *ComponentName2 OPTIONAL
  )
{
  (VOID)ImageHandle;
  (VOID)SystemTable;
  (VOID)DriverBinding;
  (VOID)DriverBindingHandle;
  (VOID)ComponentName;
  (VOID)ComponentName2;
  return EFI_SUCCESS;
}

static
EFI_STATUS
StartKeypad (
  VOID
  )
{
  return KeypadControllerDriverStart (&gKeypadControllerDriver, mController, NULL);
}

static
EFI_STATUS
StopKeypad (
  VOID
  )
{
  return KeypadControllerDriverStop (&gKeypadControllerDriver, mController, 0, NULL);
}

static
VOID
TestResetFailureAndPollingBaseline (
  VOID
  )
{
  KEYPAD_CONSOLE_IN_DEV ConsoleIn;

  ResetFixture ();
  memset (&ConsoleIn, 0, sizeof (ConsoleIn));
  ConsoleIn.KeypadDevice = &mKeypadDevice;
  ConsoleIn.KeypadReturnApi.PushEfikeyBufTail = KeypadReturnApiPushEfikeyBufTail;
  ConsoleIn.Last = 99;
  ConsoleIn.LastValid = TRUE;
  ConsoleIn.EfiKeyQueue.Head = 3;
  ConsoleIn.EfiKeyQueue.Tail = 7;
  mResetStatus = EFI_DEVICE_ERROR;
  CHECK (InitKeypad (&ConsoleIn, FALSE) == EFI_DEVICE_ERROR);
  CHECK (mResetCalls == 1 && ConsoleIn.Last == 99 && ConsoleIn.LastValid);
  CHECK (ConsoleIn.EfiKeyQueue.Head == 3 && ConsoleIn.EfiKeyQueue.Tail == 7);

  mResetStatus = EFI_SUCCESS;
  CHECK (InitKeypad (&ConsoleIn, FALSE) == EFI_SUCCESS);
  CHECK (mResetCalls == 2 && ConsoleIn.Last == 0 && !ConsoleIn.LastValid);
  CHECK (ConsoleIn.EfiKeyQueue.Head == 0 && ConsoleIn.EfiKeyQueue.Tail == 0);

  mGetKeysStatus = EFI_DEVICE_ERROR;
  mCounter = 1000;
  KeypadTimerHandler (NULL, &ConsoleIn);
  CHECK (mGetKeysCalls == 1 && mDeltaCount == 1 && mDeltas[0] == 0);
  CHECK (!ConsoleIn.KeypadErr && ConsoleIn.LastValid && ConsoleIn.Last == 1000);

  mGetKeysStatus = EFI_SUCCESS;
  mCounter = 1600;
  KeypadTimerHandler (NULL, &ConsoleIn);
  CHECK (mGetKeysCalls == 2 && mDeltaCount == 2 && mDeltas[1] == 600);
  CHECK (!ConsoleIn.KeypadErr && ConsoleIn.Last == 1600);

  ConsoleIn.KeypadErr = TRUE;
  mCounter = 2000;
  KeypadTimerHandler (NULL, &ConsoleIn);
  ConsoleIn.KeypadErr = FALSE;
  ConsoleIn.Stopping = TRUE;
  KeypadTimerHandler (NULL, &ConsoleIn);
  CHECK (mGetKeysCalls == 2 && mCurrentTpl == TPL_APPLICATION);
}

static
VOID
TestSupportedCloseCanRetry (
  VOID
  )
{
  EFI_STATUS Status;

  ResetFixture ();
  mFailCloseProtocolCount = 1;
  Status = KeypadControllerDriverSupported (&gKeypadControllerDriver, mController, NULL);
  CHECK (Status == EFI_DEVICE_ERROR);
  CHECK (mPendingSupportedClose && mDeviceOpen);

  Status = KeypadControllerDriverSupported (&gKeypadControllerDriver, mController, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (!mPendingSupportedClose && !mDeviceOpen);
  CHECK (mOpenCalls == 2 && mCloseProtocolCalls == 3);
  CHECK (LiveAllocationCount () == 0);
}

static
VOID
TestStartResetFailureCleansEveryResource (
  VOID
  )
{
  EFI_STATUS Status;

  ResetFixture ();
  mResetStatus = EFI_DEVICE_ERROR;
  Status = StartKeypad ();
  CHECK (Status == EFI_DEVICE_ERROR && mResetCalls == 1);
  CHECK (!mDeviceOpen && !mTextInInstalled && !mTextInExInstalled);
  CHECK (CountActiveEvents () == 0 && mSetTimerArmCalls == 0 && mSetTimerCancelCalls == 1);
  CHECK (mPendingConsoleIn == NULL && LiveAllocationCount () == 0);
}

static
VOID
TestFailedStartCloseRetryBlocksCallbackUse (
  VOID
  )
{
  EFI_STATUS                Status;
  KEYPAD_CONSOLE_IN_DEV    *ConsoleIn;
  UINTN                     PollsBefore;

  ResetFixture ();
  mFailCreateAt = 4;
  mFailCloseEvent = (EFI_EVENT)&mEvents[2]; // Third creation is the timer.
  mFailCloseEventCount = 1;
  Status = StartKeypad ();
  CHECK (Status == EFI_DEVICE_ERROR);
  ConsoleIn = mPendingConsoleIn;
  CHECK (ConsoleIn != NULL && ConsoleIn->Stopping && ConsoleIn->DeviceProtocolOpen);
  CHECK (CountActiveEvents () == 1 && !mTextInInstalled && mDeviceOpen);

  PollsBefore = mGetKeysCalls;
  mFailCloseEvent = ConsoleIn->TimerEvent;
  mFailCloseEventCount = 1;
  mNotifyDuringClose = TRUE;
  Status = StartKeypad ();
  CHECK (Status == EFI_DEVICE_ERROR);
  CHECK (mGetKeysCalls == PollsBefore && mPendingConsoleIn == ConsoleIn);
  CHECK (CountActiveEvents () == 1 && mDeviceOpen && !mTextInInstalled);
  Status = StartKeypad ();
  CHECK (Status == EFI_SUCCESS);
  CHECK (mGetKeysCalls == PollsBefore);
  CHECK (mPendingConsoleIn == NULL && mDeviceOpen && mTextInInstalled && mTextInExInstalled);
  CHECK (CountActiveEvents () == 4);
  CHECK (StopKeypad () == EFI_SUCCESS);
  CHECK (!mDeviceOpen && CountActiveEvents () == 0 && LiveAllocationCount () == 0);
}

static
VOID
TestPartialProtocolInstallAndRetry (
  VOID
  )
{
  EFI_STATUS             Status;
  KEYPAD_CONSOLE_IN_DEV *ConsoleIn;

  ResetFixture ();
  mInstallStatus = EFI_DEVICE_ERROR;
  mPartialInstallMask = BIT0;
  mFailUninstallCount = 1;
  mUninstallStatus = EFI_ACCESS_DENIED;
  Status = StartKeypad ();
  CHECK (Status == EFI_ACCESS_DENIED);
  ConsoleIn = mPendingConsoleIn;
  CHECK (ConsoleIn != NULL && ConsoleIn->TextInInstalled && !ConsoleIn->TextInExInstalled);
  CHECK (mTextInInstalled && !mTextInExInstalled && mDeviceOpen);

  mInstallStatus = EFI_SUCCESS;
  Status = StartKeypad ();
  CHECK (Status == EFI_SUCCESS);
  CHECK (mPendingConsoleIn == NULL && mTextInInstalled && mTextInExInstalled && mDeviceOpen);
  CHECK (mUninstallCalls >= 2);
  CHECK (StopKeypad () == EFI_SUCCESS);
  CHECK (!mTextInInstalled && !mTextInExInstalled && !mDeviceOpen);
  CHECK (LiveAllocationCount () == 0 && CountActiveEvents () == 0);
}

static
VOID
TestStopRetainsContextAfterNotifyFreeFailure (
  VOID
  )
{
  EFI_STATUS             Status;
  KEYPAD_CONSOLE_IN_DEV *ConsoleIn;
  EFI_KEY_DATA           KeyData;
  VOID                  *NotifyHandle;

  ResetFixture ();
  CHECK (StartKeypad () == EFI_SUCCESS);
  ConsoleIn = TEXT_INPUT_EX_KEYPAD_CONSOLE_IN_DEV_FROM_THIS (mInstalledTextInEx);
  memset (&KeyData, 0, sizeof (KeyData));
  KeyData.Key.UnicodeChar = L'K';
  NotifyHandle = NULL;
  CHECK (mInstalledTextInEx->RegisterKeyNotify (mInstalledTextInEx, &KeyData, MockKeyNotification, &NotifyHandle) == EFI_SUCCESS);
  CHECK (NotifyHandle != NULL && !IsListEmpty (&ConsoleIn->NotifyList));

  mFailFreePointer = NotifyHandle;
  Status = StopKeypad ();
  CHECK (Status == EFI_DEVICE_ERROR);
  CHECK (mPendingConsoleIn == ConsoleIn && IsListEmpty (&ConsoleIn->NotifyList) == FALSE);
  CHECK (!mDeviceOpen && !mTextInInstalled && !mTextInExInstalled);

  CHECK (StartKeypad () == EFI_SUCCESS);
  CHECK (mPendingConsoleIn == NULL && mTextInInstalled && mTextInExInstalled);
  CHECK (StopKeypad () == EFI_SUCCESS);
  CHECK (LiveAllocationCount () == 0 && CountActiveEvents () == 0);
}

static
VOID
TestStopRetriesDeviceProtocolClose (
  VOID
  )
{
  EFI_STATUS             Status;
  KEYPAD_CONSOLE_IN_DEV *ConsoleIn;

  ResetFixture ();
  CHECK (StartKeypad () == EFI_SUCCESS);
  ConsoleIn = KEYPAD_CONSOLE_IN_DEV_FROM_THIS (mInstalledTextIn);
  mFailCloseProtocolCount = 1;
  Status = StopKeypad ();
  CHECK (Status == EFI_DEVICE_ERROR && mPendingConsoleIn == ConsoleIn);
  CHECK (!mTextInInstalled && !mTextInExInstalled && mDeviceOpen);
  CHECK (CountActiveEvents () == 0 && LiveAllocationCount () != 0);

  CHECK (StopKeypad () == EFI_SUCCESS);
  CHECK (mPendingConsoleIn == NULL && !mDeviceOpen);
  CHECK (CountActiveEvents () == 0 && LiveAllocationCount () == 0);
}

static
VOID
TestStopRetainsContextAfterFinalFreeFailure (
  VOID
  )
{
  EFI_STATUS             Status;
  KEYPAD_CONSOLE_IN_DEV *ConsoleIn;

  ResetFixture ();
  CHECK (StartKeypad () == EFI_SUCCESS);
  ConsoleIn = KEYPAD_CONSOLE_IN_DEV_FROM_THIS (mInstalledTextIn);
  mFailFreePointer = ConsoleIn;
  Status = StopKeypad ();
  CHECK (Status == EFI_DEVICE_ERROR && mPendingConsoleIn == ConsoleIn);
  CHECK (!mDeviceOpen && !mTextInInstalled && !mTextInExInstalled && CountActiveEvents () == 0);
  CHECK (StartKeypad () == EFI_SUCCESS);
  CHECK (mPendingConsoleIn == NULL && mTextInInstalled && mTextInExInstalled);
  CHECK (StopKeypad () == EFI_SUCCESS);
  CHECK (LiveAllocationCount () == 0 && CountActiveEvents () == 0);
}

static VOID *mNotifySelf, *mNotifyNext;
static unsigned mNotifyCalls, mNotifyNextCalls;
static BOOLEAN mNotifyRemoveNext;
static EFI_STATUS EFIAPI TestNotifyNext(EFI_KEY_DATA *Key) {
  (VOID)Key; mNotifyNextCalls++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestNotifySelf(EFI_KEY_DATA *Key) {
  (VOID)Key; mNotifyCalls++;
  CHECK(KeypadUnregisterKeyNotify(mInstalledTextInEx,mNotifySelf)==EFI_SUCCESS);
  if (mNotifyRemoveNext)
    CHECK(KeypadUnregisterKeyNotify(mInstalledTextInEx,mNotifyNext)==EFI_SUCCESS);
  return EFI_SUCCESS;
}
static VOID TestNotifyUnregisterDuringDispatch(VOID) {
  EFI_KEY_DATA Key={.Key={.ScanCode=SCAN_UP}};
  for(unsigned RemoveNext=0;RemoveNext<2;RemoveNext++) {
    ResetFixture(); CHECK(StartKeypad()==EFI_SUCCESS);
    mNotifyCalls=mNotifyNextCalls=0; mNotifyRemoveNext=(BOOLEAN)RemoveNext;
    CHECK(KeypadRegisterKeyNotify(mInstalledTextInEx,&Key,TestNotifySelf,&mNotifySelf)==EFI_SUCCESS);
    CHECK(KeypadRegisterKeyNotify(mInstalledTextInEx,&Key,TestNotifyNext,&mNotifyNext)==EFI_SUCCESS);
    KEYPAD_CONSOLE_IN_DEV *ConsoleIn=TEXT_INPUT_EX_KEYPAD_CONSOLE_IN_DEV_FROM_THIS(mInstalledTextInEx);
    KeypadReturnApiPushEfikeyBufTail(&ConsoleIn->KeypadReturnApi,&Key);
    CHECK(((TEST_EVENT *)ConsoleIn->KeyNotifyProcessEvent)->Signals==1);
    CHECK(mNotifyCalls==1 && mNotifyNextCalls==(RemoveNext?0:1));
    CHECK(StopKeypad()==EFI_SUCCESS && LiveAllocationCount()==0);
  }
}

static VOID TestPartialKeyWait(VOID) {
  ResetFixture ();
  CHECK (StartKeypad () == EFI_SUCCESS);
  KEYPAD_CONSOLE_IN_DEV *ConsoleIn = TEXT_INPUT_EX_KEYPAD_CONSOLE_IN_DEV_FROM_THIS (mInstalledTextInEx);
  EFI_KEY_TOGGLE_STATE Toggle = EFI_TOGGLE_STATE_VALID | EFI_SCROLL_LOCK_ACTIVE |
                                EFI_NUM_LOCK_ACTIVE | EFI_CAPS_LOCK_ACTIVE | EFI_KEY_STATE_EXPOSED;
  CHECK (KeypadSetState (mInstalledTextInEx, &Toggle) == EFI_SUCCESS);
  CHECK (ConsoleIn->ScrollLock && ConsoleIn->NumLock && ConsoleIn->CapsLock && ConsoleIn->IsSupportPartialKey);
  EFI_KEY_DATA Partial = {.KeyState = {.KeyShiftState = EFI_SHIFT_STATE_VALID | EFI_LEFT_SHIFT_PRESSED}};
  KeypadReturnApiPushEfikeyBufTail (&ConsoleIn->KeypadReturnApi, &Partial);
  TEST_EVENT *Wait = mInstalledTextIn->WaitForKey;
  TEST_EVENT *WaitEx = mInstalledTextInEx->WaitForKeyEx;
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Wait->Notify (Wait, Wait->Context);
  WaitEx->Notify (WaitEx, WaitEx->Context);
  gBS->RestoreTPL (OldTpl);
  CHECK (Wait->Signals == 0 && WaitEx->Signals == 1);
  EFI_KEY_DATA Key;
  CHECK (KeypadReadKeyStroke (mInstalledTextIn, NULL) == EFI_INVALID_PARAMETER);
  CHECK (KeypadReadKeyStrokeEx (mInstalledTextInEx, &Key) == EFI_SUCCESS);
  CHECK (Key.Key.ScanCode == SCAN_NULL && Key.Key.UnicodeChar == CHAR_NULL &&
         Key.KeyState.KeyShiftState == Partial.KeyState.KeyShiftState);
  CHECK (StopKeypad () == EFI_SUCCESS && LiveAllocationCount () == 0);
}

int
main (
  VOID
  )
{
  TestNotifyUnregisterDuringDispatch ();
  TestPartialKeyWait ();
  TestResetFailureAndPollingBaseline ();
  TestSupportedCloseCanRetry ();
  TestStartResetFailureCleansEveryResource ();
  TestFailedStartCloseRetryBlocksCallbackUse ();
  TestPartialProtocolInstallAndRetry ();
  TestStopRetainsContextAfterNotifyFreeFailure ();
  TestStopRetriesDeviceProtocolClose ();
  TestStopRetainsContextAfterFinalFreeFailure ();
  printf ("KeypadDxeTest: %u checks passed\n", Checks);
  return 0;
}
