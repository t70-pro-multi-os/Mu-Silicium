// SPDX-License-Identifier: BSD-2-Clause-Patent
// Executes the production SD DXE code with host-owned Boot Services and PassThru mocks.
#include <Uefi.h>
#include <IndustryStandard/Sd.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DiskInfo.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/EraseBlock.h>
#include <Protocol/SdMmcPassThru.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Sd/SdDxe/SdDxe.h"

#define CHECK(Expression)                                                     \
  do {                                                                        \
    mCheckCount++;                                                            \
    if (!(Expression)) {                                                      \
      fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #Expression);     \
      abort ();                                                               \
    }                                                                         \
  } while (0)

typedef enum {
  AllocationGeneral = 1,
  AllocationDevice,
  AllocationChildPath,
  AllocationBuildPath,
  AllocationRequest,
  AllocationEvent
} TEST_ALLOCATION_KIND;

typedef union {
  max_align_t  Alignment;
  struct {
    UINT32                 Magic;
    UINT32                 Kind;
  } Fields;
} TEST_ALLOCATION_HEADER;

#define TEST_ALLOCATION_MAGIC  SIGNATURE_32 ('S', 'D', 'A', 'L')

typedef struct {
  UINT32             Signature;
  EFI_EVENT_NOTIFY   Notify;
  VOID               *Context;
  BOOLEAN            Internal;
  EFI_TPL            Tpl;
  BOOLEAN            Timer;
  BOOLEAN            Armed;
  UINT32             Signals;
  UINT32             Closes;
} TEST_EVENT;

typedef struct {
  EFI_HANDLE  Handle;
  SD_DEVICE   *Device;
  BOOLEAN     Installed;
  BOOLEAN     ChildAssociation;
} TEST_CHILD;

#define TEST_EVENT_SIGNATURE  SIGNATURE_32 ('S', 'D', 'E', 'V')

typedef enum {
  PathValid = 0,
  PathWrongType,
  PathWrongSubtype,
  PathWrongLength,
  PathWrongSlot
} TEST_PATH_SHAPE;

static BOOLEAN mTraceWrites;
static UINT32 mWriteCommand, mWriteArgument, mWriteLength, mWriteCalls;
static UINT8 mWritePayload[1024];

// Keep the wire values independent of the production masks.
#define TEST_SD_TRAN   (4U << 9)
#define TEST_SD_PRG    (7U << 9)
#define TEST_SD_READY  (TEST_SD_TRAN | BIT8)

static UINT16  mReturnedRca;
static UINT32  mWriteResponse;
static UINT32  mDefaultStatusResponse;
static UINT32  mStatusResponses[4];
static UINTN   mStatusResponseCount;
static UINTN   mStatusCalls;
static VOID    (*mOnStatus) (VOID);
static UINTN   mStallCalls;
static UINTN   mArmedTimers;
static EFI_STATUS  mSetTimerStatus;
static BOOLEAN     mTraceCommands;
static UINTN       mCommandCount;
static struct {
  UINT16   Command;
  UINT32   Argument;
  BOOLEAN  Async;
  EFI_TPL  Tpl;
} mCommands[256];

static EFI_BOOT_SERVICES              mServices;
EFI_BOOT_SERVICES                     *gBS = &mServices;
static EFI_TPL                        mCurrentTpl = TPL_APPLICATION;
static BOOLEAN                        mDeferUserNotify;
static TEST_EVENT                     *mDeferredUserEvent;
static UINT32                         mCheckCount;
static UINT32                         mAllocations;
static UINT32                         mFrees;
static UINT32                         mBuildPathAllocations;
static UINT32                         mBuildPathFrees;
static UINT32                         mDeviceFrees;
static UINT32                         mPrivateFrees;
static UINT32                         mRequestFrees;
static UINT32                         mEventFrees;
static UINT32                         mTplRaises;
static UINT32                         mTplRestores;
static UINT32                         mTplViolations;

EFI_GUID  gEfiSdMmcPassThruProtocolGuid = EFI_SD_MMC_PASS_THRU_PROTOCOL_GUID;
EFI_GUID  gEfiBlockIoProtocolGuid       = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID  gEfiBlockIo2ProtocolGuid      = EFI_BLOCK_IO2_PROTOCOL_GUID;
EFI_GUID  gEfiEraseBlockProtocolGuid    = EFI_ERASE_BLOCK_PROTOCOL_GUID;
EFI_GUID  gEfiDiskInfoProtocolGuid      = EFI_DISK_INFO_PROTOCOL_GUID;
EFI_GUID  gEfiDevicePathProtocolGuid    = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID  gEfiCallerIdGuid              = { 0 };

EFI_COMPONENT_NAME_PROTOCOL   gSdDxeComponentName  = { NULL, NULL, "eng" };
EFI_COMPONENT_NAME2_PROTOCOL  gSdDxeComponentName2 = { NULL, NULL, "en" };

static SD_DEVICE                      *mDevice;
static SD_CSD                         mMockCsd;
static UINT8                          mCidResponse[16];
static UINT32                         mCidArgument;
static UINT32 mLastReadArgument, mLastReadLength;
static UINT16 mLastReadCommand;
static UINT32 mEraseStartArgument, mEraseEndArgument;
static struct {
  UINT16 Command;
  UINT32 Argument;
  UINT32 Length;
  VOID   *Buffer;
} mReadTrace[4];
static UINTN mReadTraceCount;
static BOOLEAN mCopyPrefixOnReadError;
static BOOLEAN mIgnorePassThruEvent;
static UINTN mAllocationAttempt, mFailAllocationAttempt;
static BOOLEAN mPoisonFailedEvent;
static EFI_STATUS mAsyncCompletionStatus;
static SD_DRIVER_PRIVATE_DATA         mPrivate;
static EFI_SD_MMC_PASS_THRU_PROTOCOL  mPassThru;
static EFI_DRIVER_BINDING_PROTOCOL    mBinding;
static EFI_HANDLE                     mController = (EFI_HANDLE)(UINTN)0x1110;
static EFI_HANDLE                     mChild      = (EFI_HANDLE)(UINTN)0x2220;
static EFI_HANDLE                     mDriver     = (EFI_HANDLE)(UINTN)0x3330;
static EFI_DEVICE_PATH_PROTOCOL       mParentDevicePath;
static TEST_CHILD                     mChildren[8];
static UINTN                          mChildEntryCount;
static UINTN                          mNextChildHandle;
static UINTN                          mNextSlotIndex;
static UINTN                          mSlotCount;
static UINT8                          mSlots[4];
static UINT8                          mCurrentPassSlot;
static SD_DRIVER_PRIVATE_DATA         *mInstalledPrivate;
static BOOLEAN                        mCallerIdInstalled;
static EFI_STATUS                     mOpenByDriverStatus;
static EFI_STATUS                     mChildOpenStatus;
static EFI_STATUS                     mInstallCallerStatus;
static EFI_STATUS                     mInstallChildStatus;
static EFI_STATUS                     mUninstallCallerStatus;
static EFI_STATUS                     mUninstallChildStatus;
static EFI_STATUS                     mParentCloseStatus;
static UINT32                         mChildOpenFailuresRemaining;
static UINT32                         mChildOpenAttempts;
static UINT32                         mParentCloseCalls;
static BOOLEAN                        mReenterStopOnChildUninstall;
static EFI_STATUS                     mNestedStopStatus;
static BOOLEAN                        mResetBlockIo2OnChildUninstall;
static UINT32                         mInstallCallerCalls;
static UINT32                         mInstallChildCalls;
static UINT32                         mUninstallCallerCalls;
static UINT32                         mUninstallChildCalls;
static UINT32                         mOpenProtocolInformationCalls;
static UINT16                         mFailCommandIndex;
static UINT32                         mFailCommandOccurrence;
static UINT32                         mMatchingCommandCount;
static EFI_STATUS                     mCommandFailureStatus;
static BOOLEAN                        mCompletePassThruSynchronously;
static UINT32                         mUserTokenFrees;

static EFI_STATUS                mBuildStatus;
static TEST_PATH_SHAPE           mPathShape;
static BOOLEAN                   mBuildReturnsPathOnError;
static BOOLEAN                   mBuildReentersStop;
static EFI_STATUS                mReentrantStopStatus;
static UINT32                    mBuildCalls;
static UINT32                    mPassThruCalls;
static EFI_STATUS                mPassThruStatus;
static UINT32                    mResetCalls;
static EFI_STATUS                mResetStatus;
static UINT32                    mCloseProtocolCalls;
static EFI_STATUS                mCloseProtocolStatus;
static UINT32                    mUninstallCalls;
static UINT32                    mReopenPassThruCalls;
static EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *mPendingPacket;
static TEST_EVENT                         *mPendingEvent;

static VOID  *mLastBuildPath;
static TEST_EVENT  *mUserEvent;

static VOID *
TestAllocate (
  IN UINTN                Size,
  IN TEST_ALLOCATION_KIND Kind,
  IN BOOLEAN              Zero
  )
{
  TEST_ALLOCATION_HEADER  *Header;
  UINTN                   PayloadSize;

  if (++mAllocationAttempt == mFailAllocationAttempt) { return NULL; }
  PayloadSize = (Size == 0) ? 1 : Size;
  Header      = (TEST_ALLOCATION_HEADER *)malloc (sizeof (*Header) + PayloadSize);
  if (Header == NULL) {
    return NULL;
  }

  Header->Fields.Magic = TEST_ALLOCATION_MAGIC;
  Header->Fields.Kind  = (UINT32)Kind;
  if (Zero) {
    memset (Header + 1, 0, PayloadSize);
  }

  mAllocations++;
  if (Kind == AllocationBuildPath) {
    mBuildPathAllocations++;
  }

  return (VOID *)(Header + 1);
}

VOID *
EFIAPI
AllocatePool (
  IN UINTN  AllocationSize
  )
{
  return TestAllocate (AllocationSize, AllocationGeneral, FALSE);
}

VOID *
EFIAPI
AllocateZeroPool (
  IN UINTN  AllocationSize
  )
{
  if (AllocationSize == 0) {
    return NULL;
  }

  return TestAllocate (AllocationSize, AllocationSize == sizeof (SD_REQUEST) ? AllocationRequest : AllocationGeneral, TRUE);
}

VOID *
EFIAPI
AllocateCopyPool (
  IN UINTN       AllocationSize,
  IN CONST VOID  *Buffer
  )
{
  VOID  *Copy;

  if ((AllocationSize == 0) || (Buffer == NULL)) {
    return NULL;
  }

  Copy = TestAllocate (AllocationSize, AllocationSize == sizeof (SD_DEVICE) ? AllocationDevice : AllocationGeneral, FALSE);
  if (Copy != NULL) {
    memcpy (Copy, Buffer, AllocationSize);
  }

  return Copy;
}

VOID
EFIAPI
FreePool (
  IN VOID  *Buffer
  )
{
  TEST_ALLOCATION_HEADER  *Header;

  if (Buffer == NULL) {
    return;
  }

  Header = ((TEST_ALLOCATION_HEADER *)Buffer) - 1;
  CHECK (Header->Fields.Magic == TEST_ALLOCATION_MAGIC);
  CHECK (mAllocations != 0);
  Header->Fields.Magic = 0;
  mAllocations--;
  mFrees++;

  switch ((TEST_ALLOCATION_KIND)Header->Fields.Kind) {
    case AllocationBuildPath:
      mBuildPathFrees++;
      break;
    case AllocationDevice:
      mDeviceFrees++;
      break;
    case AllocationRequest:
      mRequestFrees++;
      break;
    case AllocationEvent:
      mEventFrees++;
      break;
    case AllocationGeneral:
    case AllocationChildPath:
      break;
    default:
      CHECK (FALSE);
      break;
  }

  if (Buffer == mInstalledPrivate) {
    mPrivateFrees++;
    mInstalledPrivate = NULL;
  }

  free (Header);
}

VOID *
EFIAPI
CopyMem (
  OUT VOID        *Destination,
  IN CONST VOID   *Source,
  IN UINTN        Length
  )
{
  return memmove (Destination, Source, Length);
}

VOID *
EFIAPI
ZeroMem (
  OUT VOID  *Buffer,
  IN UINTN  Length
  )
{
  return memset (Buffer, 0, Length);
}

INTN
EFIAPI
CompareMem (
  IN CONST VOID  *DestinationBuffer,
  IN CONST VOID  *SourceBuffer,
  IN UINTN       Length
  )
{
  return memcmp (DestinationBuffer, SourceBuffer, Length);
}

BOOLEAN
EFIAPI
DebugCodeEnabled (
  VOID
  )
{
  return TRUE;
}

UINTN
EFIAPI
DevicePathNodeLength (
  IN CONST VOID  *Node
  )
{
  UINT16  Length;

  CHECK (Node != NULL);
  memcpy (&Length, (CONST UINT8 *)Node + OFFSET_OF (EFI_DEVICE_PATH_PROTOCOL, Length), sizeof (Length));
  return Length;
}

RETURN_STATUS
EFIAPI
AsciiStrToUnicodeStrS (
  IN  CONST CHAR8  *Source,
  OUT CHAR16       *Destination,
  IN  UINTN        DestMax
  )
{
  UINTN  Index;

  if ((Source == NULL) || (Destination == NULL) || (DestMax == 0)) {
    return RETURN_INVALID_PARAMETER;
  }

  for (Index = 0; (Index + 1 < DestMax) && (Source[Index] != '\0'); Index++) {
    Destination[Index] = (CHAR16)(UINT8)Source[Index];
  }

  Destination[Index] = 0;
  return RETURN_SUCCESS;
}

EFI_DEVICE_PATH_PROTOCOL *
EFIAPI
AppendDevicePathNode (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath OPTIONAL,
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePathNode OPTIONAL
  )
{
  UINTN                       Length;
  EFI_DEVICE_PATH_PROTOCOL    *Copy;

  (VOID)DevicePath;
  if (DevicePathNode == NULL) {
    return NULL;
  }

  Length = DevicePathNodeLength (DevicePathNode);
  if (Length < sizeof (EFI_DEVICE_PATH_PROTOCOL)) {
    return NULL;
  }

  Copy = (EFI_DEVICE_PATH_PROTOCOL *)TestAllocate (Length, AllocationChildPath, FALSE);
  if (Copy != NULL) {
    CopyMem (Copy, DevicePathNode, Length);
  }

  return Copy;
}

BOOLEAN
EFIAPI
IsDevicePathEnd (
  IN CONST VOID  *Node
  )
{
  CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePathNode;

  CHECK (Node != NULL);
  DevicePathNode = (CONST EFI_DEVICE_PATH_PROTOCOL *)Node;
  return (BOOLEAN)(
                    (DevicePathNode->Type == END_DEVICE_PATH_TYPE) &&
                    (DevicePathNode->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE)
                    );
}

static EFI_TPL
EFIAPI
MockRaiseTpl (
  IN EFI_TPL  NewTpl
  )
{
  EFI_TPL  OldTpl;

  OldTpl = mCurrentTpl;
  mTplRaises++;
  if (NewTpl <= mCurrentTpl) {
    mTplViolations++;
    CHECK (NewTpl > mCurrentTpl);
  }

  mCurrentTpl = NewTpl;
  return OldTpl;
}

static VOID
DispatchUserEvent (
  VOID
  )
{
  if ((mDeferredUserEvent != NULL) && (mCurrentTpl < TPL_CALLBACK)) {
    TEST_EVENT *Event = mDeferredUserEvent;
    EFI_TPL OldTpl = mCurrentTpl;
    mDeferredUserEvent = NULL;
    mCurrentTpl = TPL_CALLBACK;
    Event->Notify ((EFI_EVENT)Event, Event->Context);
    mCurrentTpl = OldTpl;
  }
}

static VOID
EFIAPI
MockRestoreTpl (
  IN EFI_TPL  OldTpl
  )
{
  mTplRestores++;
  CHECK (mCurrentTpl == TPL_NOTIFY);
  CHECK (OldTpl < mCurrentTpl);
  mCurrentTpl = OldTpl;
  DispatchUserEvent ();
}

static EFI_STATUS
EFIAPI
MockCreateEvent (
  IN  UINT32           Type,
  IN  EFI_TPL          NotifyTpl,
  IN  EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
  IN  VOID             *NotifyContext OPTIONAL,
  OUT EFI_EVENT        *Event
  )
{
  TEST_EVENT  *NewEvent;

  CHECK ((Type & EVT_NOTIFY_SIGNAL) != 0);
  CHECK (NotifyTpl == (((Type & EVT_TIMER) != 0) ? TPL_CALLBACK : TPL_NOTIFY));
  CHECK (NotifyFunction != NULL);
  CHECK (Event != NULL);
  NewEvent = (TEST_EVENT *)TestAllocate (sizeof (*NewEvent), AllocationEvent, TRUE);
  if (NewEvent == NULL) {
    if (mPoisonFailedEvent) { *Event = (EFI_EVENT)(UINTN)1; }
    return EFI_OUT_OF_RESOURCES;
  }

  NewEvent->Signature = TEST_EVENT_SIGNATURE;
  NewEvent->Notify   = NotifyFunction;
  NewEvent->Context  = NotifyContext;
  NewEvent->Internal = TRUE;
  NewEvent->Tpl      = NotifyTpl;
  NewEvent->Timer    = (BOOLEAN)((Type & EVT_TIMER) != 0);
  *Event = (EFI_EVENT)NewEvent;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockSignalEvent (
  IN EFI_EVENT  Event
  )
{
  TEST_EVENT  *TestEvent;

  CHECK (Event != NULL);
  CHECK (Event != (EFI_EVENT)(UINTN)1);
  TestEvent = (TEST_EVENT *)Event;
  CHECK (TestEvent->Signature == TEST_EVENT_SIGNATURE);
  TestEvent->Signals++;
  if (mDeferUserNotify && !TestEvent->Internal &&
      (TestEvent->Notify != NULL) && (mCurrentTpl >= TPL_CALLBACK)) {
    CHECK (mDeferredUserEvent == NULL);
    mDeferredUserEvent = TestEvent;
  } else if (TestEvent->Notify != NULL) {
    EFI_TPL OldTpl = mCurrentTpl;
    if (TestEvent->Internal) {
      CHECK (mCurrentTpl <= TestEvent->Tpl);
      mCurrentTpl = TestEvent->Tpl;
    } else if (mDeferUserNotify) {
      mCurrentTpl = TPL_CALLBACK;
    }
    TestEvent->Notify (Event, TestEvent->Context);
    mCurrentTpl = OldTpl;
    DispatchUserEvent ();
  }

  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockCloseEvent (
  IN EFI_EVENT  Event
  )
{
  TEST_EVENT  *TestEvent;

  CHECK (Event != NULL);
  CHECK (Event != (EFI_EVENT)(UINTN)1);
  TestEvent = (TEST_EVENT *)Event;
  CHECK (TestEvent->Signature == TEST_EVENT_SIGNATURE);
  TestEvent->Closes++;
  if (TestEvent->Armed) {
    CHECK (mArmedTimers != 0);
    mArmedTimers--;
  }

  if (TestEvent->Internal) {
    FreePool (TestEvent);
  }

  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockSetTimer (
  IN EFI_EVENT        Event,
  IN EFI_TIMER_DELAY  Type,
  IN UINT64           TriggerTime
  )
{
  TEST_EVENT  *Timer = Event;

  CHECK (mCurrentTpl <= TPL_NOTIFY);
  CHECK (Timer->Timer && !Timer->Armed && Timer->Tpl == TPL_CALLBACK);
  CHECK (Type == TimerPeriodic && TriggerTime != 0);
  if (!EFI_ERROR (mSetTimerStatus)) {
    Timer->Armed = TRUE;
    mArmedTimers++;
  }

  return mSetTimerStatus;
}

static EFI_STATUS
EFIAPI
MockStall (
  IN UINTN  Microseconds
  )
{
  CHECK (mCurrentTpl <= TPL_CALLBACK);
  CHECK (Microseconds != 0 && Microseconds <= 10000);
  mStallCalls++;
  return EFI_SUCCESS;
}

static EFI_STATUS
FindChild (
  IN  EFI_HANDLE  Handle,
  OUT TEST_CHILD  **Entry OPTIONAL
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mChildren); Index++) {
    if (mChildren[Index].Handle == Handle) {
      if (Entry != NULL) {
        *Entry = &mChildren[Index];
      }

      return EFI_SUCCESS;
    }
  }

  if (Entry != NULL) {
    *Entry = NULL;
  }

  return EFI_NOT_FOUND;
}

static UINTN
CountChildAssociations (
  VOID
  )
{
  UINTN  Index;
  UINTN  Count;

  Count = 0;
  for (Index = 0; Index < ARRAY_SIZE (mChildren); Index++) {
    if (mChildren[Index].Installed && mChildren[Index].ChildAssociation) {
      Count++;
    }
  }

  return Count;
}

static EFI_STATUS
EFIAPI
MockOpenProtocol (
  IN  EFI_HANDLE  Handle,
  IN  EFI_GUID    *Protocol,
  OUT VOID        **Interface OPTIONAL,
  IN  EFI_HANDLE  AgentHandle,
  IN  EFI_HANDLE  ControllerHandle,
  IN  UINT32      Attributes
  )
{
  TEST_CHILD  *Child;

  (VOID)AgentHandle;
  if (Interface == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *Interface = NULL;
  if (Handle == mController) {
    if (Protocol == &gEfiSdMmcPassThruProtocolGuid) {
      *Interface = &mPassThru;
      if (Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER) {
        return mOpenByDriverStatus;
      }

      if (Attributes == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) {
        mReopenPassThruCalls++;
        mChildOpenAttempts++;
        if (mChildOpenFailuresRemaining != 0) {
          mChildOpenFailuresRemaining--;
          return mChildOpenStatus;
        }

        if (EFI_ERROR (FindChild (ControllerHandle, &Child))) {
          return EFI_NOT_FOUND;
        }

        Child->ChildAssociation = TRUE;
        return EFI_SUCCESS;
      }

      return EFI_SUCCESS;
    }

    if (Protocol == &gEfiDevicePathProtocolGuid) {
      *Interface = &mParentDevicePath;
      return EFI_SUCCESS;
    }

    if (Protocol == &gEfiCallerIdGuid) {
      if (!mCallerIdInstalled || (mInstalledPrivate == NULL)) {
        return EFI_NOT_FOUND;
      }

      *Interface = mInstalledPrivate;
      return EFI_SUCCESS;
    }
  }

  if ((Handle == mChild) && (mDevice != NULL)) {
    if (Protocol == &gEfiBlockIoProtocolGuid) {
      *Interface = &mDevice->BlockIo;
      return EFI_SUCCESS;
    }

    if (Protocol == &gEfiBlockIo2ProtocolGuid) {
      *Interface = &mDevice->BlockIo2;
      return EFI_SUCCESS;
    }

    if (Protocol == &gEfiSdMmcPassThruProtocolGuid) {
      *Interface = &mPassThru;
      mReopenPassThruCalls++;
      return EFI_SUCCESS;
    }
  }

  if (!EFI_ERROR (FindChild (Handle, &Child)) && Child->Installed) {
    if (Protocol == &gEfiBlockIoProtocolGuid) {
      *Interface = &Child->Device->BlockIo;
      return EFI_SUCCESS;
    }

    if (Protocol == &gEfiBlockIo2ProtocolGuid) {
      *Interface = &Child->Device->BlockIo2;
      return EFI_SUCCESS;
    }
  }

  return EFI_UNSUPPORTED;
}

static EFI_STATUS
EFIAPI
MockCloseProtocol (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN EFI_HANDLE  AgentHandle,
  IN EFI_HANDLE  ControllerHandle
  )
{
  TEST_CHILD  *Child;

  CHECK (Handle == mController);
  CHECK (Protocol == &gEfiSdMmcPassThruProtocolGuid);
  CHECK (AgentHandle == mBinding.DriverBindingHandle);
  mCloseProtocolCalls++;
  if (ControllerHandle == mController) {
    mParentCloseCalls++;
    return mParentCloseStatus;
  }

  if ((ControllerHandle == mChild) && (mDevice != NULL)) {
    return mCloseProtocolStatus;
  }

  if (!EFI_ERROR (FindChild (ControllerHandle, &Child)) && Child->Installed) {
    if (!Child->ChildAssociation) {
      return EFI_NOT_FOUND;
    }

    if (!EFI_ERROR (mCloseProtocolStatus)) {
      Child->ChildAssociation = FALSE;
    }

    return mCloseProtocolStatus;
  }

  return EFI_NOT_FOUND;
}

static EFI_STATUS
EFIAPI
MockInstallProtocolInterface (
  IN OUT EFI_HANDLE  *Handle,
  IN EFI_GUID        *Protocol,
  IN EFI_INTERFACE_TYPE InterfaceType,
  IN VOID            *Interface
  )
{
  CHECK (Handle != NULL);
  CHECK (*Handle == mController);
  CHECK (Protocol == &gEfiCallerIdGuid);
  CHECK (InterfaceType == EFI_NATIVE_INTERFACE);
  mInstallCallerCalls++;
  if (EFI_ERROR (mInstallCallerStatus)) {
    return mInstallCallerStatus;
  }

  mInstalledPrivate = (SD_DRIVER_PRIVATE_DATA *)Interface;
  mCallerIdInstalled = TRUE;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockUninstallProtocolInterface (
  IN EFI_HANDLE  Handle,
  IN EFI_GUID    *Protocol,
  IN VOID        *Interface
  )
{
  CHECK (Handle == mController);
  CHECK (Protocol == &gEfiCallerIdGuid);
  CHECK (Interface == mInstalledPrivate);
  mUninstallCallerCalls++;
  if (EFI_ERROR (mUninstallCallerStatus)) {
    return mUninstallCallerStatus;
  }

  mCallerIdInstalled = FALSE;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockInstallMultipleProtocolInterfaces (
  IN OUT EFI_HANDLE  *Handle,
  ...
  )
{
  va_list       Args;
  EFI_GUID      *Protocol;
  VOID          *Interface;
  SD_DEVICE     *Device;
  UINTN         Index;

  CHECK (Handle != NULL);
  CHECK (*Handle == NULL);
  Device = NULL;
  va_start (Args, Handle);
  while ((Protocol = va_arg (Args, EFI_GUID *)) != NULL) {
    Interface = va_arg (Args, VOID *);
    if (Protocol == &gEfiBlockIoProtocolGuid) {
      Device = SD_DEVICE_DATA_FROM_BLKIO ((EFI_BLOCK_IO_PROTOCOL *)Interface);
    }
  }

  va_end (Args);
  CHECK (Device != NULL);
  mInstallChildCalls++;
  if (EFI_ERROR (mInstallChildStatus)) {
    return mInstallChildStatus;
  }

  for (Index = 0; Index < ARRAY_SIZE (mChildren); Index++) {
    if (!mChildren[Index].Installed) {
      mChildren[Index].Handle = (EFI_HANDLE)(UINTN)(0x4000 + mNextChildHandle++);
      mChildren[Index].Device = Device;
      mChildren[Index].Installed = TRUE;
      mChildren[Index].ChildAssociation = FALSE;
      Device->Handle = mChildren[Index].Handle;
      *Handle = Device->Handle;
      mChildEntryCount++;
      return EFI_SUCCESS;
    }
  }

  return EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS
EFIAPI
MockUninstallMultipleProtocolInterfaces (
  IN EFI_HANDLE  Handle,
  ...
  )
{
  EFI_STATUS  Status;
  TEST_CHILD  *Child;
  UINT32      BuildCalls;
  UINT32      ResetCalls;

  mUninstallCalls++;
  if (Handle == mController) {
    Status = mUninstallCallerStatus;
    mUninstallCallerCalls++;
    if (!EFI_ERROR (Status)) {
      mCallerIdInstalled = FALSE;
    }

    return Status;
  }

  CHECK (!EFI_ERROR (FindChild (Handle, &Child)));
  CHECK (Child->Installed);
  mUninstallChildCalls++;
  if (mReenterStopOnChildUninstall) {
    EFI_HANDLE  ChildBuffer[1];

    mReenterStopOnChildUninstall = FALSE;
    ChildBuffer[0] = Handle;
    mNestedStopStatus = SdDxeDriverBindingStop (&mBinding, mController, 1, ChildBuffer);
  }

  // DiskIoDxe::Stop resets BlockIo2 before releasing the SD child's protocols.
  if (mResetBlockIo2OnChildUninstall) {
    CHECK (Child->Device->StopInProgress);
    CHECK (Child->Device->MediaInvalidated);
    CHECK (Child->Device->ActiveOperations == 0);
    CHECK (IsListEmpty (&Child->Device->Queue));
    BuildCalls = mBuildCalls;
    ResetCalls = mResetCalls;
    CHECK (Child->Device->BlockIo2.Reset (&Child->Device->BlockIo2, FALSE) == EFI_SUCCESS);
    CHECK (mBuildCalls == BuildCalls);
    CHECK (mResetCalls == ResetCalls);
    Child->Device->ActiveOperations = 1;
    CHECK (Child->Device->BlockIo2.Reset (&Child->Device->BlockIo2, FALSE) == EFI_NOT_READY);
    Child->Device->ActiveOperations = 0;
  }

  Status = mUninstallChildStatus;
  if (!EFI_ERROR (Status)) {
    Child->Installed = FALSE;
    Child->ChildAssociation = FALSE;
    Child->Device = NULL;
    mChildEntryCount--;
  }

  return Status;
}

static EFI_STATUS
EFIAPI
MockLocateDevicePath (
  IN EFI_GUID                       *Protocol,
  IN OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePath,
  OUT EFI_HANDLE                    *Device
  )
{
  CHECK (Protocol == &gEfiDevicePathProtocolGuid);
  CHECK (DevicePath != NULL);
  CHECK (*DevicePath != NULL);
  CHECK (Device != NULL);
  *Device = NULL;
  return EFI_NOT_FOUND;
}

static EFI_STATUS
EFIAPI
MockGetNextSlot (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN OUT UINT8                          *Slot
  )
{
  CHECK (This == &mPassThru);
  CHECK (Slot != NULL);
  if (mNextSlotIndex >= mSlotCount) {
    return EFI_NOT_FOUND;
  }

  *Slot = mSlots[mNextSlotIndex++];
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockGetSlotNumber (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN  EFI_DEVICE_PATH_PROTOCOL      *DevicePath,
  OUT UINT8                         *Slot
  )
{
  CHECK (This == &mPassThru);
  if ((DevicePath == NULL) || (Slot == NULL) || (DevicePath->SubType != MSG_SD_DP)) {
    return EFI_INVALID_PARAMETER;
  }

  *Slot = ((SD_DEVICE_PATH *)DevicePath)->SlotNumber;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockOpenProtocolInformation (
  IN  EFI_HANDLE                            Handle,
  IN  EFI_GUID                              *Protocol,
  OUT EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  **EntryBuffer,
  OUT UINTN                                 *EntryCount
  )
{
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY  *Entries;
  UINTN                                Index;
  UINTN                                Count;
  UINTN                                Current;

  CHECK (Handle == mController);
  CHECK (Protocol == &gEfiSdMmcPassThruProtocolGuid);
  CHECK (EntryBuffer != NULL && EntryCount != NULL);
  mOpenProtocolInformationCalls++;
  Count = CountChildAssociations ();
  *EntryBuffer = NULL;
  *EntryCount = 0;
  if (Count == 0) {
    return EFI_SUCCESS;
  }

  Entries = (EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *)AllocateZeroPool (
                                                       Count * sizeof (*Entries)
                                                       );
  if (Entries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Current = 0;
  for (Index = 0; Index < ARRAY_SIZE (mChildren); Index++) {
    if (mChildren[Index].Installed && mChildren[Index].ChildAssociation) {
      Entries[Current].AgentHandle = mDriver;
      Entries[Current].ControllerHandle = mChildren[Index].Handle;
      Entries[Current].Attributes = EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;
      Entries[Current].OpenCount = 1;
      Current++;
    }
  }

  *EntryBuffer = Entries;
  *EntryCount = Count;
  return EFI_SUCCESS;
}

static EFI_STATUS
EFIAPI
MockBuildDevicePath (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN  UINT8                          Slot,
  OUT EFI_DEVICE_PATH_PROTOCOL       **DevicePath
  )
{
  SD_DEVICE_PATH  *Path;

  CHECK (This == &mPassThru);
  CHECK (DevicePath != NULL);
  mBuildCalls++;
  mCurrentPassSlot = Slot;
  *DevicePath = NULL;

  if (!EFI_ERROR (mBuildStatus) || mBuildReturnsPathOnError) {
    Path = (SD_DEVICE_PATH *)TestAllocate (sizeof (*Path), AllocationBuildPath, TRUE);
    if (Path == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Path->Header.Type    = MESSAGING_DEVICE_PATH;
    Path->Header.SubType = MSG_SD_DP;
    Path->Header.Length[0] = (UINT8)(sizeof (*Path) & 0xff);
    Path->Header.Length[1] = (UINT8)(sizeof (*Path) >> 8);
    Path->SlotNumber = Slot;
    switch (mPathShape) {
      case PathWrongType:
        Path->Header.Type = ACPI_DEVICE_PATH;
        break;
      case PathWrongSubtype:
        Path->Header.SubType = MSG_UFS_DP;
        break;
      case PathWrongLength:
        Path->Header.Length[0] = (UINT8)(sizeof (*Path) - 1);
        Path->Header.Length[1] = 0;
        break;
      case PathWrongSlot:
        Path->SlotNumber = (UINT8)(Slot + 1);
        break;
      case PathValid:
      default:
        break;
    }

    *DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)Path;
    mLastBuildPath = Path;
  }

  if (mBuildReentersStop) {
    EFI_HANDLE  ChildBuffer[1];

    mBuildReentersStop = FALSE;
    ChildBuffer[0] = mChild;
    mReentrantStopStatus = SdDxeDriverBindingStop (&mBinding, mController, 1, ChildBuffer);
  }

  return mBuildStatus;
}

static EFI_STATUS
EFIAPI
MockPassThru (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL        *This,
  IN     UINT8                                Slot,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET  *Packet,
  IN     EFI_EVENT                            Event OPTIONAL
  )
{
  EFI_STATUS  Status;

  CHECK (This == &mPassThru);
  CHECK (Slot == ((mSlotCount == 0) ? 2 : mCurrentPassSlot));
  CHECK (Packet != NULL && Packet->SdMmcCmdBlk != NULL);
  CHECK (mCurrentTpl <= TPL_CALLBACK);
  mPassThruCalls++;
  if (mTraceCommands) {
    CHECK (mCommandCount < ARRAY_SIZE (mCommands));
    mCommands[mCommandCount].Command  = Packet->SdMmcCmdBlk->CommandIndex;
    mCommands[mCommandCount].Argument = Packet->SdMmcCmdBlk->CommandArgument;
    mCommands[mCommandCount].Async    = (BOOLEAN)(Event != NULL);
    mCommands[mCommandCount].Tpl      = mCurrentTpl;
    mCommandCount++;
  }
  if (mTraceWrites && (Packet->SdMmcCmdBlk != NULL) &&
      ((Packet->SdMmcCmdBlk->CommandIndex == SD_WRITE_SINGLE_BLOCK) ||
       (Packet->SdMmcCmdBlk->CommandIndex == SD_WRITE_MULTIPLE_BLOCK))) {
    mWriteCalls++;
    mWriteCommand = Packet->SdMmcCmdBlk->CommandIndex;
    mWriteArgument = Packet->SdMmcCmdBlk->CommandArgument;
    mWriteLength = Packet->OutTransferLength;
    CHECK (Packet->InTransferLength == 0);
    CHECK (Packet->OutDataBuffer != NULL);
    CHECK (mWriteLength <= sizeof (mWritePayload));
    CopyMem (mWritePayload, Packet->OutDataBuffer, mWriteLength);
  }
  if ((Packet->SdMmcCmdBlk != NULL) &&
      ((Packet->SdMmcCmdBlk->CommandIndex == SD_READ_SINGLE_BLOCK) ||
       (Packet->SdMmcCmdBlk->CommandIndex == SD_READ_MULTIPLE_BLOCK))) {
    mLastReadCommand = Packet->SdMmcCmdBlk->CommandIndex;
    mLastReadArgument = Packet->SdMmcCmdBlk->CommandArgument;
    mLastReadLength = Packet->InTransferLength;
    if (mReadTraceCount < ARRAY_SIZE (mReadTrace)) {
      mReadTrace[mReadTraceCount].Command = mLastReadCommand;
      mReadTrace[mReadTraceCount].Argument = mLastReadArgument;
      mReadTrace[mReadTraceCount].Length = mLastReadLength;
      mReadTrace[mReadTraceCount].Buffer = Packet->InDataBuffer;
      mReadTraceCount++;
    }
  }
  if (Packet->SdMmcCmdBlk != NULL) {
    if (Packet->SdMmcCmdBlk->CommandIndex == SD_ERASE_WR_BLK_START) {
      mEraseStartArgument = Packet->SdMmcCmdBlk->CommandArgument;
    } else if (Packet->SdMmcCmdBlk->CommandIndex == SD_ERASE_WR_BLK_END) {
      mEraseEndArgument = Packet->SdMmcCmdBlk->CommandArgument;
    }
  }
  Status = mPassThruStatus;
  if ((Packet->SdMmcCmdBlk != NULL) &&
      (Packet->SdMmcCmdBlk->CommandIndex == mFailCommandIndex) &&
      (mFailCommandIndex != 0)) {
    mMatchingCommandCount++;
    if (mMatchingCommandCount == mFailCommandOccurrence) {
      Status = mCommandFailureStatus;
    }
  }

  if (Packet->SdMmcCmdBlk->CommandIndex == SD_SET_RELATIVE_ADDR) {
    Packet->SdMmcStatusBlk->Resp0 = (UINT32)mReturnedRca << 16;
  } else if (Packet->SdMmcCmdBlk->CommandIndex == SD_SEND_STATUS) {
    CHECK (Event == NULL);
    CHECK (Packet->SdMmcCmdBlk->CommandType == SdMmcCommandTypeAc);
    CHECK (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR1);
    CHECK (Packet->InTransferLength == 0 && Packet->OutTransferLength == 0);
    CHECK (Packet->Timeout != 0);
    Packet->SdMmcStatusBlk->Resp0 = (mStatusCalls < mStatusResponseCount) ?
                                    mStatusResponses[mStatusCalls] : mDefaultStatusResponse;
    mStatusCalls++;
    if (mOnStatus != NULL) {
      mOnStatus ();
    }
  } else if ((Packet->SdMmcCmdBlk->CommandIndex == SD_WRITE_SINGLE_BLOCK) ||
             (Packet->SdMmcCmdBlk->CommandIndex == SD_WRITE_MULTIPLE_BLOCK)) {
    Packet->SdMmcStatusBlk->Resp0 = mWriteResponse;
  }

  if (!EFI_ERROR (Status) &&
      (Packet->SdMmcCmdBlk->CommandIndex == SD_SEND_CSD)) {
    CHECK (Packet->SdMmcStatusBlk != NULL);
    CopyMem (
      &Packet->SdMmcStatusBlk->Resp0,
      ((UINT8 *)&mMockCsd) + 1,
      sizeof (mMockCsd) - 1
      );
  }

  if ((Packet->SdMmcCmdBlk != NULL) &&
      (Packet->SdMmcCmdBlk->CommandIndex == SD_SEND_CID)) {
    CHECK (Event == NULL && Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR2);
    CHECK (Packet->InTransferLength == 0 && Packet->OutTransferLength == 0);
    mCidArgument = Packet->SdMmcCmdBlk->CommandArgument;
    // Even a failing provider can leave register bytes; the caller must ignore them.
    CopyMem (&Packet->SdMmcStatusBlk->Resp0, mCidResponse, sizeof (mCidResponse));
  }

  if (mCopyPrefixOnReadError && EFI_ERROR (Status) &&
      (Packet->InTransferLength >= 16) && (Packet->InDataBuffer != NULL)) {
    memset (Packet->InDataBuffer, 0x33, 16);
  }
  Packet->TransactionStatus = Status;
  if ((Event != NULL) && !mIgnorePassThruEvent && !EFI_ERROR (Status)) {
    Packet->TransactionStatus = mAsyncCompletionStatus;
    if (mCompletePassThruSynchronously && !EFI_ERROR (Status)) {
      // The callback may free both Packet's owning request and the caller token.
      // Do not inspect either object after signaling this internal event.
      MockSignalEvent (Event);
      return Status;
    }

    mPendingPacket = Packet;
    mPendingEvent  = (TEST_EVENT *)Event;
  }

  return Status;
}

static EFI_STATUS
EFIAPI
MockResetDevice (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL  *This,
  IN UINT8                          Slot
  )
{
  CHECK (This == &mPassThru);
  CHECK (Slot == 2);
  mResetCalls++;
  return mResetStatus;
}

static BOOLEAN mSdReadOnly;
static BOOLEAN mSynchronousPassThru;
#define _PCD_GET_MODE_BOOL_PcdSdDxeSynchronousPassThru mSynchronousPassThru
#define _PCD_GET_MODE_BOOL_PcdSdDxeReadOnly mSdReadOnly

#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Sd/SdDxe/SdDxe.c"
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Sd/SdDxe/SdBlockIo.c"
#include "../../../../../Mu_Basecore/MdeModulePkg/Bus/Sd/SdDxe/SdDiskInfo.c"

static VOID
SetMockCsdV1 (
  IN UINT32   CSizeValue,
  IN UINT8    CSizeMultiplier,
  IN UINT8    ReadBlockLength,
  IN UINT8    WriteBlockLength,
  IN BOOLEAN  EraseSingleBlock,
  IN UINT8    EraseSectorSize
  )
{
  ZeroMem (&mMockCsd, sizeof (mMockCsd));
  mMockCsd.CsdStructure = 0;
  mMockCsd.CSizeLow = CSizeValue & 0x3U;
  mMockCsd.CSizeHigh = (CSizeValue >> 2) & 0x3ffU;
  mMockCsd.CSizeMul = CSizeMultiplier;
  mMockCsd.ReadBlLen = ReadBlockLength;
  mMockCsd.WriteBlLen = WriteBlockLength;
  mMockCsd.EraseBlkEn = EraseSingleBlock;
  mMockCsd.SectorSize = EraseSectorSize;
}

static VOID
SetMockCsdV2 (
  IN UINT32   CSizeValue,
  IN BOOLEAN  EraseSingleBlock,
  IN UINT8    EraseSectorSize
  )
{
  SD_CSD2  *Csd2;

  ZeroMem (&mMockCsd, sizeof (mMockCsd));
  Csd2 = (SD_CSD2 *)(VOID *)&mMockCsd;
  Csd2->CsdStructure = 1;
  Csd2->CSizeLow = CSizeValue & 0xffffU;
  Csd2->CSizeHigh = (CSizeValue >> 16) & 0x3fU;
  Csd2->EraseBlkEn = EraseSingleBlock;
  Csd2->SectorSize = EraseSectorSize;
}

typedef struct {
  UINT8    Structure;
  UINT32   CSizeValue;
  UINT8    CSizeMultiplier;
  UINT8    ReadBlockLength;
  UINT8    WriteBlockLength;
  BOOLEAN  EraseSingleBlock;
  UINT8    EraseSectorSize;
} MOCK_CSD_FIXTURE;

static VOID
SetMockCsdFixture (
  IN CONST MOCK_CSD_FIXTURE  *Fixture
  )
{
  if (Fixture->Structure == 1) {
    SetMockCsdV2 (
      Fixture->CSizeValue,
      Fixture->EraseSingleBlock,
      Fixture->EraseSectorSize
      );
  } else {
    SetMockCsdV1 (
      Fixture->CSizeValue,
      Fixture->CSizeMultiplier,
      Fixture->ReadBlockLength,
      Fixture->WriteBlockLength,
      Fixture->EraseSingleBlock,
      Fixture->EraseSectorSize
      );
    mMockCsd.CsdStructure = Fixture->Structure;
  }
}

static VOID
ResetCounters (
  VOID
  )
{
  CHECK (mAllocations == 0);
  mBuildCalls           = 0;
  mPassThruCalls        = 0;
  mReadTraceCount       = 0;
  mTraceCommands        = FALSE;
  mCommandCount         = 0;
  mReturnedRca          = 0x1A2B;
  mWriteResponse        = 0;
  mDefaultStatusResponse = TEST_SD_READY;
  mStatusResponseCount  = 0;
  mStatusCalls          = 0;
  mOnStatus             = NULL;
  mStallCalls           = 0;
  CHECK (mArmedTimers == 0);
  mSetTimerStatus       = EFI_SUCCESS;
  mTraceWrites = FALSE;
  mWriteCalls = mWriteCommand = mWriteArgument = mWriteLength = 0;
  ZeroMem (mWritePayload, sizeof (mWritePayload));
  mCopyPrefixOnReadError = FALSE;
  mIgnorePassThruEvent = FALSE;
  mSynchronousPassThru = FALSE;
  mAllocationAttempt = mFailAllocationAttempt = 0;
  mPoisonFailedEvent = FALSE;
  mAsyncCompletionStatus = EFI_SUCCESS;
  mResetCalls           = 0;
  mCloseProtocolCalls   = 0;
  mUninstallCalls       = 0;
  mReopenPassThruCalls  = 0;
  mBuildPathAllocations = 0;
  mBuildPathFrees       = 0;
  mDeviceFrees          = 0;
  mPrivateFrees         = 0;
  mRequestFrees         = 0;
  mEventFrees           = 0;
  mTplRaises            = 0;
  mTplRestores          = 0;
  mTplViolations        = 0;
  mPendingPacket        = NULL;
  mPendingEvent         = NULL;
  mCompletePassThruSynchronously = FALSE;
  mUserTokenFrees       = 0;
  mLastBuildPath        = NULL;
  mCurrentTpl           = TPL_APPLICATION;
  ZeroMem (mCidResponse, sizeof (mCidResponse));
  mCidArgument = 0;
  mChildEntryCount      = 0;
  mNextChildHandle      = 1;
  mNextSlotIndex        = 0;
  mSlotCount            = 0;
  mInstalledPrivate     = NULL;
  mCallerIdInstalled    = FALSE;
  mOpenByDriverStatus   = EFI_SUCCESS;
  mChildOpenStatus      = EFI_ACCESS_DENIED;
  mInstallCallerStatus  = EFI_SUCCESS;
  mInstallChildStatus   = EFI_SUCCESS;
  mUninstallCallerStatus = EFI_SUCCESS;
  mUninstallChildStatus = EFI_SUCCESS;
  mParentCloseStatus    = EFI_SUCCESS;
  mChildOpenFailuresRemaining = 0;
  mChildOpenAttempts    = 0;
  mParentCloseCalls     = 0;
  mReenterStopOnChildUninstall = FALSE;
  mNestedStopStatus     = EFI_SUCCESS;
  mResetBlockIo2OnChildUninstall = FALSE;
  mInstallCallerCalls   = 0;
  mInstallChildCalls    = 0;
  mUninstallCallerCalls = 0;
  mUninstallChildCalls  = 0;
  mOpenProtocolInformationCalls = 0;
  SetMockCsdV1 (0, 0, 9, 9, TRUE, 0);
  mFailCommandIndex     = 0;
  mFailCommandOccurrence = 0;
  mMatchingCommandCount = 0;
  mCommandFailureStatus = EFI_DEVICE_ERROR;
  ZeroMem (mChildren, sizeof (mChildren));
  mSlotCount = 0;
  ZeroMem (mSlots, sizeof (mSlots));
}

static VOID
InitializeServices (
  VOID
  )
{
  ZeroMem (&mServices, sizeof (mServices));
  mServices.RaiseTPL = MockRaiseTpl;
  mServices.RestoreTPL = MockRestoreTpl;
  mServices.CreateEvent = MockCreateEvent;
  mServices.SignalEvent = MockSignalEvent;
  mServices.CloseEvent = MockCloseEvent;
  mServices.SetTimer = MockSetTimer;
  mServices.Stall = MockStall;
  mServices.OpenProtocol = MockOpenProtocol;
  mServices.CloseProtocol = MockCloseProtocol;
  mServices.OpenProtocolInformation = MockOpenProtocolInformation;
  mServices.InstallProtocolInterface = MockInstallProtocolInterface;
  mServices.UninstallProtocolInterface = MockUninstallProtocolInterface;
  mServices.LocateDevicePath = MockLocateDevicePath;
  mServices.InstallMultipleProtocolInterfaces = MockInstallMultipleProtocolInterfaces;
  mServices.UninstallMultipleProtocolInterfaces = MockUninstallMultipleProtocolInterfaces;
}

static VOID
SetupDevice (
  VOID
  )
{
  SD_DEVICE_PATH  *ChildPath;

  CHECK (mDevice == NULL);
  ResetCounters ();
  InitializeServices ();
  ZeroMem (&mPassThru, sizeof (mPassThru));
  mPassThru.PassThru = MockPassThru;
  mPassThru.GetNextSlot = MockGetNextSlot;
  mPassThru.BuildDevicePath = MockBuildDevicePath;
  mPassThru.GetSlotNumber = MockGetSlotNumber;
  mPassThru.ResetDevice = MockResetDevice;

  ZeroMem (&mPrivate, sizeof (mPrivate));
  mPrivate.PassThru = &mPassThru;
  mPrivate.Controller = mController;
  mPrivate.DriverBindingHandle = mDriver;
  InitializeListHead (&mPrivate.Children);
  mPrivate.ChildCount = 1;
  mInstalledPrivate = &mPrivate;
  mCallerIdInstalled = TRUE;

  ZeroMem (&mBinding, sizeof (mBinding));
  mBinding.DriverBindingHandle = mDriver;
  mParentDevicePath.Type = ACPI_DEVICE_PATH;
  mParentDevicePath.SubType = 1;

  mDevice = (SD_DEVICE *)TestAllocate (sizeof (*mDevice), AllocationDevice, TRUE);
  CHECK (mDevice != NULL);
  CopyMem (mDevice, &mSdDeviceTemplate, sizeof (*mDevice));
  InitializeListHead (&mDevice->Queue);
  InitializeListHead (&mDevice->OrderedIo);
  mDevice->Rca = mReturnedRca;
  mDevice->Signature = SD_DEVICE_SIGNATURE;
  mDevice->Handle = mChild;
  mDevice->Slot = 2;
  mDevice->SectorAddressing = TRUE;
  mDevice->BlockIo.Media = &mDevice->BlockMedia;
  mDevice->BlockIo2.Media = &mDevice->BlockMedia;
  mDevice->BlockMedia.MediaId = 7;
  mDevice->BlockMedia.MediaPresent = TRUE;
  mDevice->BlockMedia.RemovableMedia = TRUE;
  mDevice->BlockMedia.BlockSize = 512;
  mDevice->BlockMedia.LastBlock = 31;
  mDevice->EraseBlock.EraseLengthGranularity = 1;
  mDevice->Private = &mPrivate;
  InitializeListHead (&mDevice->ChildLink);
  InsertTailList (&mPrivate.Children, &mDevice->ChildLink);
  mDevice->ChildTracked = TRUE;
  mChildren[0].Handle = mChild;
  mChildren[0].Device = mDevice;
  mChildren[0].Installed = TRUE;
  mChildren[0].ChildAssociation = TRUE;
  mChildEntryCount = 1;

  ChildPath = (SD_DEVICE_PATH *)TestAllocate (sizeof (*ChildPath), AllocationChildPath, TRUE);
  CHECK (ChildPath != NULL);
  ChildPath->Header.Type = MESSAGING_DEVICE_PATH;
  ChildPath->Header.SubType = MSG_SD_DP;
  ChildPath->Header.Length[0] = (UINT8)sizeof (*ChildPath);
  ChildPath->SlotNumber = 2;
  mDevice->DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)ChildPath;

  mBuildStatus = EFI_SUCCESS;
  mPathShape = PathValid;
  mBuildReturnsPathOnError = FALSE;
  mBuildReentersStop = FALSE;
  mReentrantStopStatus = EFI_SUCCESS;
  mPassThruStatus = EFI_SUCCESS;
  mResetStatus = EFI_SUCCESS;
  mCloseProtocolStatus = EFI_SUCCESS;
  mUninstallChildStatus = EFI_SUCCESS;
}

static VOID
SetupStartEnvironment (
  IN UINTN  SlotCount
  )
{
  CHECK (mDevice == NULL);
  CHECK (SlotCount <= ARRAY_SIZE (mSlots));
  ResetCounters ();
  InitializeServices ();
  ZeroMem (&mPassThru, sizeof (mPassThru));
  mPassThru.PassThru = MockPassThru;
  mPassThru.GetNextSlot = MockGetNextSlot;
  mPassThru.BuildDevicePath = MockBuildDevicePath;
  mPassThru.GetSlotNumber = MockGetSlotNumber;
  mPassThru.ResetDevice = MockResetDevice;
  mBinding.DriverBindingHandle = mDriver;
  mParentDevicePath.Type = ACPI_DEVICE_PATH;
  mParentDevicePath.SubType = 1;
  mSlotCount = SlotCount;
  if (SlotCount > 0) {
    mSlots[0] = 1;
  }

  if (SlotCount > 1) {
    mSlots[1] = 2;
  }

  mBuildStatus = EFI_SUCCESS;
  mPathShape = PathValid;
  mPassThruStatus = EFI_SUCCESS;
  mInstallChildStatus = EFI_SUCCESS;
  mChildOpenStatus = EFI_ACCESS_DENIED;
  mUninstallCallerStatus = EFI_SUCCESS;
  mUninstallChildStatus = EFI_SUCCESS;
  mParentCloseStatus = EFI_SUCCESS;
}

static EFI_STATUS
StopCurrentChild (
  VOID
  )
{
  EFI_HANDLE  Children[1];

  Children[0] = mChild;
  return SdDxeDriverBindingStop (&mBinding, mController, 1, Children);
}

static EFI_STATUS
StopTrackedChild (
  IN UINTN  Index
  )
{
  EFI_HANDLE  ChildBuffer[1];

  CHECK (Index < ARRAY_SIZE (mChildren));
  CHECK (mChildren[Index].Installed);
  ChildBuffer[0] = mChildren[Index].Handle;
  return SdDxeDriverBindingStop (&mBinding, mController, 1, ChildBuffer);
}

static UINTN
CountInstalledChildren (
  VOID
  )
{
  UINTN  Index;
  UINTN  Count;

  Count = 0;
  for (Index = 0; Index < ARRAY_SIZE (mChildren); Index++) {
    if (mChildren[Index].Installed) {
      Count++;
    }
  }

  return Count;
}

static UINTN
FirstInstalledChild (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mChildren); Index++) {
    if (mChildren[Index].Installed) {
      return Index;
    }
  }

  CHECK (FALSE);
  return 0;
}

static VOID
CheckTombstone (
  IN UINT32  ExpectedMediaId
  )
{
  CHECK (mDevice != NULL);
  CHECK (mDevice->MediaInvalidated);
  CHECK (!mDevice->BlockMedia.MediaPresent);
  CHECK (mDevice->BlockMedia.MediaId == ExpectedMediaId);
  CHECK (mDevice->BlockMedia.LastBlock == 0);
  CHECK (mDevice->EraseBlock.EraseLengthGranularity == 0);
  CHECK (!mDevice->SectorAddressing);
}

static VOID
FinishAndCheckNoLeaks (
  VOID
  )
{
  CHECK (mDevice != NULL);
  CHECK (StopCurrentChild () == EFI_SUCCESS);
  CHECK (mDeviceFrees == 1);
  mDevice = NULL;
  CHECK (mAllocations == 0);
  CHECK (mBuildPathAllocations == mBuildPathFrees);
  CHECK (mTplRaises == mTplRestores);
  CHECK (mTplViolations == 0);
}

static VOID
TestBuildDevicePathEvidence (
  VOID
  )
{
  static CONST struct {
    EFI_STATUS       Status;
    TEST_PATH_SHAPE  Shape;
    BOOLEAN          ReturnPathOnError;
    EFI_STATUS       ExpectedStatus;
    BOOLEAN          Tombstone;
  } Cases[] = {
    { EFI_SUCCESS, PathValid, FALSE, EFI_SUCCESS, FALSE },
    { EFI_NOT_FOUND, PathValid, TRUE, EFI_NO_MEDIA, TRUE },
    { EFI_NO_MEDIA, PathValid, TRUE, EFI_NO_MEDIA, TRUE },
    { EFI_MEDIA_CHANGED, PathValid, TRUE, EFI_MEDIA_CHANGED, TRUE },
    { EFI_OUT_OF_RESOURCES, PathValid, TRUE, EFI_OUT_OF_RESOURCES, FALSE },
    { EFI_NOT_READY, PathValid, TRUE, EFI_NOT_READY, FALSE },
    { EFI_DEVICE_ERROR, PathValid, TRUE, EFI_DEVICE_ERROR, FALSE },
    { EFI_SUCCESS, PathWrongType, FALSE, EFI_DEVICE_ERROR, FALSE },
    { EFI_SUCCESS, PathWrongSubtype, FALSE, EFI_DEVICE_ERROR, FALSE },
    { EFI_SUCCESS, PathWrongLength, FALSE, EFI_DEVICE_ERROR, FALSE },
    { EFI_SUCCESS, PathWrongSlot, FALSE, EFI_DEVICE_ERROR, FALSE }
  };
  EFI_STATUS  Status;
  UINT8       Data[512];
  UINT32      Index;
  UINT32      OriginalMediaId;
  UINT32      CallsBefore;

  for (Index = 0; Index < ARRAY_SIZE (Cases); Index++) {
    SetupDevice ();
    OriginalMediaId = mDevice->BlockMedia.MediaId;
    mBuildStatus = Cases[Index].Status;
    mPathShape = Cases[Index].Shape;
    mBuildReturnsPathOnError = Cases[Index].ReturnPathOnError;

    Status = mDevice->BlockIo.ReadBlocks (
                                &mDevice->BlockIo,
                                OriginalMediaId,
                                0,
                                sizeof (Data),
                                Data
                                );
    CHECK (Status == Cases[Index].ExpectedStatus);
    CHECK (mBuildCalls == 1);
    CHECK (mBuildPathAllocations == mBuildPathFrees);
    CHECK (mDevice->ActiveOperations == 0);
    if (Cases[Index].Tombstone) {
      CheckTombstone (OriginalMediaId + 1);
      CallsBefore = mBuildCalls;
      mBuildStatus = EFI_SUCCESS;
      Status = mDevice->BlockIo.ReadBlocks (
                                  &mDevice->BlockIo,
                                  mDevice->BlockMedia.MediaId,
                                  0,
                                  sizeof (Data),
                                  Data
                                  );
      CHECK (Status == EFI_NO_MEDIA);
      CHECK (mBuildCalls == CallsBefore);
      CHECK (mPassThruCalls == 0);
    } else if (Cases[Index].Status == EFI_SUCCESS && Cases[Index].Shape == PathValid) {
      CHECK (mPassThruCalls == 1);
      CHECK (mDevice->BlockMedia.MediaId == OriginalMediaId);
      CHECK (mDevice->BlockMedia.LastBlock == 31);
    } else {
      CHECK (!mDevice->MediaInvalidated);
      CHECK (mDevice->BlockMedia.MediaId == OriginalMediaId);
      CHECK (mDevice->BlockMedia.LastBlock == 31);
      CHECK (mPassThruCalls == 0);
    }

    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestTransportStatusInvalidation (
  VOID
  )
{
  static CONST struct {
    EFI_STATUS  Status;
    EFI_STATUS  Expected;
    BOOLEAN     Tombstone;
  } Cases[] = {
    { EFI_NO_MEDIA, EFI_NO_MEDIA, TRUE },
    { EFI_NOT_FOUND, EFI_NO_MEDIA, TRUE },
    { EFI_MEDIA_CHANGED, EFI_MEDIA_CHANGED, TRUE },
    { EFI_NOT_READY, EFI_NOT_READY, FALSE },
    { EFI_DEVICE_ERROR, EFI_DEVICE_ERROR, FALSE }
  };
  UINT8       Data[512];
  UINT32      Index;
  EFI_STATUS  Status;

  for (Index = 0; Index < ARRAY_SIZE (Cases); Index++) {
    SetupDevice ();
    mPassThruStatus = Cases[Index].Status;
    Status = mDevice->BlockIo.ReadBlocks (
                                &mDevice->BlockIo,
                                mDevice->BlockMedia.MediaId,
                                0,
                                sizeof (Data),
                                Data
                                );
    CHECK (Status == Cases[Index].Expected);
    CHECK (mBuildCalls == 1);
    CHECK (mPassThruCalls == 1);
    if (Cases[Index].Tombstone) {
      CheckTombstone (8);
    } else {
      CHECK (!mDevice->MediaInvalidated);
      CHECK (mDevice->BlockMedia.MediaId == 7);
      CHECK (mDevice->BlockMedia.LastBlock == 31);
    }

    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestStopDuringPinnedProbe (
  VOID
  )
{
  UINT8       Data[512];
  UINT32      CloseBefore;
  UINT32      UninstallBefore;
  EFI_STATUS  Status;

  SetupDevice ();
  mBuildReentersStop = TRUE;
  CloseBefore = mCloseProtocolCalls;
  UninstallBefore = mUninstallCalls;
  Status = mDevice->BlockIo.ReadBlocks (
                              &mDevice->BlockIo,
                              mDevice->BlockMedia.MediaId,
                              0,
                              sizeof (Data),
                              Data
                              );
  CHECK (mReentrantStopStatus == EFI_NOT_READY);
  CHECK (Status == EFI_NO_MEDIA);
  CHECK (mDevice != NULL);
  CHECK (mDevice->ActiveOperations == 0);
  CheckTombstone (8);
  CHECK (mCloseProtocolCalls == CloseBefore);
  CHECK (mUninstallCalls == UninstallBefore);
  CHECK (mPassThruCalls == 0);
  CHECK (mBuildPathAllocations == 1 && mBuildPathFrees == 1);

  FinishAndCheckNoLeaks ();
}

static VOID
TestAllPublicIoMethodsRejectTombstone (
  VOID
  )
{
  UINT8                    Data[512];
  EFI_BLOCK_IO2_TOKEN      BlockToken;
  EFI_ERASE_BLOCK_TOKEN    EraseToken;
  UINT32                   BuildCalls;
  UINT32                   PassCalls;

  SetupDevice ();
  SdInvalidateMedia (mDevice);
  CheckTombstone (8);
  BuildCalls = mBuildCalls;
  PassCalls = mPassThruCalls;
  ZeroMem (&BlockToken, sizeof (BlockToken));
  ZeroMem (&EraseToken, sizeof (EraseToken));

  CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 8, 0, sizeof (Data), Data) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo.WriteBlocks (&mDevice->BlockIo, 8, 0, sizeof (Data), Data) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo.Reset (&mDevice->BlockIo, FALSE) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo.FlushBlocks (&mDevice->BlockIo) == EFI_NO_MEDIA);

  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 8, 0, NULL, sizeof (Data), Data) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 8, 0, NULL, sizeof (Data), Data) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo2.Reset (&mDevice->BlockIo2, FALSE) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo2.FlushBlocksEx (&mDevice->BlockIo2, NULL) == EFI_NO_MEDIA);
  CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 8, 0, &EraseToken, sizeof (Data)) == EFI_NO_MEDIA);

  CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, sizeof (Data), Data) == EFI_MEDIA_CHANGED);
  BlockToken.Event = NULL;
  CHECK (mDevice->BlockIo2.FlushBlocksEx (&mDevice->BlockIo2, &BlockToken) == EFI_NO_MEDIA);
  CHECK (BlockToken.TransactionStatus == EFI_NO_MEDIA);
  CHECK (mBuildCalls == BuildCalls);
  CHECK (mPassThruCalls == PassCalls);
  CHECK (mDevice->ActiveOperations == 0);
  CHECK (IsListEmpty (&mDevice->Queue));
  FinishAndCheckNoLeaks ();
}

static VOID
TestDiskInfoInquiryTombstone (
  VOID
  )
{
  UINT8   Output[sizeof (SD_CID) + 4];
  UINT8   Expected[sizeof (SD_CID)];
  UINT32  Size;

  SetupDevice ();
  for (UINT32 Index = 0; Index < sizeof (mDevice->Cid); Index++) {
    ((UINT8 *)&mDevice->Cid)[Index] = (UINT8)(Index ^ 0xA5);
  }

  CopyMem (Expected, &mDevice->Cid, sizeof (Expected));
  ZeroMem (Output, sizeof (Output));
  Size = sizeof (Output);
  CHECK (mDevice->DiskInfo.Inquiry (&mDevice->DiskInfo, Output, &Size) == EFI_SUCCESS);
  CHECK (Size == sizeof (SD_CID));
  CHECK (memcmp (Output, Expected, sizeof (Expected)) == 0);

  memset (Output, 0x6D, sizeof (Output));
  Size = 1;
  CHECK (mDevice->DiskInfo.Inquiry (&mDevice->DiskInfo, Output, &Size) == EFI_BUFFER_TOO_SMALL);
  CHECK (Size == sizeof (SD_CID));
  for (UINT32 Index = 0; Index < sizeof (Output); Index++) {
    CHECK (Output[Index] == 0x6D);
  }

  Size = sizeof (SD_CID);
  CHECK (mDevice->DiskInfo.Inquiry (&mDevice->DiskInfo, NULL, &Size) == EFI_INVALID_PARAMETER);
  CHECK (mDevice->DiskInfo.Inquiry (&mDevice->DiskInfo, Output, NULL) == EFI_INVALID_PARAMETER);

  SdInvalidateMedia (mDevice);
  memset (Output, 0x3C, sizeof (Output));
  Size = sizeof (Output);
  CHECK (mDevice->DiskInfo.Inquiry (&mDevice->DiskInfo, Output, &Size) == EFI_NO_MEDIA);
  CHECK (Size == sizeof (SD_CID));
  for (UINT32 Index = 0; Index < sizeof (Output); Index++) {
    CHECK (Output[Index] == 0x3C);
  }

  FinishAndCheckNoLeaks ();
}

static VOID
TestCidResponseContract (
  VOID
  )
{
  CONST EFI_STATUS Statuses[] = { EFI_SUCCESS, EFI_CRC_ERROR, EFI_TIMEOUT,
                                 EFI_DEVICE_ERROR, EFI_NO_MEDIA };
  SetupDevice ();
  for (UINTN Pattern = 0; Pattern < 256; Pattern++) {
    for (UINTN Status = 0; Status < ARRAY_SIZE (Statuses); Status++) {
      struct { UINT8 Before[8]; SD_CID Cid; UINT8 After[8]; } Output;
      memset (&Output, 0xa5, sizeof (Output));
      for (UINTN Byte = 0; Byte < sizeof (mCidResponse); Byte++)
        mCidResponse[Byte] = (UINT8)(Pattern + Byte);
      mPassThruStatus = Statuses[Status];
      CHECK (SdGetCid (mDevice, (UINT16)(0x1200 + Pattern), &Output.Cid) == Statuses[Status]);
      CHECK (mCidArgument == (UINT32)(0x1200 + Pattern) << 16);
      for (UINTN Byte = 0; Byte < 8; Byte++)
        CHECK (Output.Before[Byte] == 0xa5 && Output.After[Byte] == 0xa5);
      // R2 exposes exactly 120 payload bits; the omitted low byte is zeroed.
      CHECK (((UINT8 *)&Output.Cid)[0] == 0);
      for (UINTN Byte = 1; Byte < sizeof (SD_CID); Byte++)
        CHECK (((UINT8 *)&Output.Cid)[Byte] ==
               (EFI_ERROR (Statuses[Status]) ? 0 : mCidResponse[Byte - 1]));
    }
  }
  // A second successful fetch replaces all prior identity bytes, not just a prefix.
  mPassThruStatus = EFI_SUCCESS;
  memset (mCidResponse, 0x19, sizeof (mCidResponse));
  CHECK (SdGetCid (mDevice, 1, &mDevice->Cid) == EFI_SUCCESS);
  memset (mCidResponse, 0xe7, sizeof (mCidResponse));
  CHECK (SdGetCid (mDevice, 2, &mDevice->Cid) == EFI_SUCCESS);
  for (UINTN Byte = 1; Byte < sizeof (SD_CID); Byte++) CHECK (((UINT8 *)&mDevice->Cid)[Byte] == 0xe7);
  SdInvalidateMedia (mDevice);
  UINT8 Inquiry[sizeof (SD_CID)]; UINT32 Size = sizeof (Inquiry);
  memset (Inquiry, 0x5a, sizeof (Inquiry));
  CHECK (mDevice->DiskInfo.Inquiry (&mDevice->DiskInfo, Inquiry, &Size) == EFI_NO_MEDIA);
  for (UINTN Byte = 0; Byte < sizeof (Inquiry); Byte++) CHECK (Inquiry[Byte] == 0x5a);
  FinishAndCheckNoLeaks ();
}

static TEST_EVENT *
CreateUserEvent (
  VOID
  )
{
  TEST_EVENT  *Event;

  Event = (TEST_EVENT *)TestAllocate (sizeof (*Event), AllocationEvent, TRUE);
  CHECK (Event != NULL);
  Event->Signature = TEST_EVENT_SIGNATURE;
  Event->Internal = FALSE;
  return Event;
}

static VOID
EFIAPI
FreeTokenOnSignal (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  CHECK (Event == (EFI_EVENT)mUserEvent);
  CHECK (Context != NULL);
  mUserTokenFrees++;
  FreePool (Context);
}

static VOID
BindFreeingUserEvent (
  IN TEST_EVENT  *Event,
  IN VOID        *Token
  )
{
  CHECK (Event != NULL);
  CHECK (Token != NULL);
  Event->Notify  = FreeTokenOnSignal;
  Event->Context = Token;
  mUserEvent     = Event;
}

static VOID
TestImmediateAsyncCompletionMayFreeCallerToken (
  VOID
  )
{
  EFI_BLOCK_IO2_TOKEN  *Token;
  EFI_ERASE_BLOCK_TOKEN  *EraseToken;
  TEST_EVENT           *UserEvent;

  // The final read command may complete and signal its caller before PassThru
  // returns. The callback frees Token, so production code must use only its
  // cached async/event values after that point.
  SetupDevice ();
  mCompletePassThruSynchronously = TRUE;
  Token = (EFI_BLOCK_IO2_TOKEN *)TestAllocate (sizeof (*Token), AllocationGeneral, TRUE);
  CHECK (Token != NULL);
  UserEvent = CreateUserEvent ();
  BindFreeingUserEvent (UserEvent, Token);
  Token->Event = (EFI_EVENT)UserEvent;
  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, Token, 512, (VOID *)mDevice) == EFI_SUCCESS);
  CHECK (mUserTokenFrees == 1);
  CHECK (UserEvent->Signals == 1);
  CHECK (mPassThruCalls == 1);
  CHECK (mRequestFrees == 1 && mEventFrees == 1);
  CHECK (IsListEmpty (&mDevice->Queue));
  CHECK (mDevice->ActiveOperations == 0);
  FreePool (UserEvent);
  mUserEvent = NULL;
  FinishAndCheckNoLeaks ();

  // Multi-block requests share the same immediate-completion lifetime edge.
  SetupDevice ();
  mCompletePassThruSynchronously = TRUE;
  Token = (EFI_BLOCK_IO2_TOKEN *)TestAllocate (sizeof (*Token), AllocationGeneral, TRUE);
  CHECK (Token != NULL);
  UserEvent = CreateUserEvent ();
  BindFreeingUserEvent (UserEvent, Token);
  Token->Event = (EFI_EVENT)UserEvent;
  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, Token, 1024, (VOID *)mDevice) == EFI_SUCCESS);
  CHECK (mUserTokenFrees == 1);
  CHECK (UserEvent->Signals == 1);
  CHECK (mPassThruCalls == 1);
  CHECK (mRequestFrees == 1 && mEventFrees == 1);
  CHECK (IsListEmpty (&mDevice->Queue));
  CHECK (mDevice->ActiveOperations == 0);
  FreePool (UserEvent);
  mUserEvent = NULL;
  FinishAndCheckNoLeaks ();

  // The final erase command signals the token from inside SdEraseBlock. The
  // outer erase helper must log the cached event and never inspect Token again.
  SetupDevice ();
  mCompletePassThruSynchronously = TRUE;
  EraseToken = (EFI_ERASE_BLOCK_TOKEN *)TestAllocate (sizeof (*EraseToken), AllocationGeneral, TRUE);
  CHECK (EraseToken != NULL);
  UserEvent = CreateUserEvent ();
  BindFreeingUserEvent (UserEvent, EraseToken);
  EraseToken->Event = (EFI_EVENT)UserEvent;
  CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7, 0, EraseToken, 512) == EFI_SUCCESS);
  CHECK (mUserTokenFrees == 1);
  CHECK (UserEvent->Signals == 1);
  CHECK (mPassThruCalls == 3);
  CHECK (mRequestFrees == 3 && mEventFrees == 3);
  CHECK (IsListEmpty (&mDevice->Queue));
  CHECK (mDevice->ActiveOperations == 0);
  FreePool (UserEvent);
  mUserEvent = NULL;
  FinishAndCheckNoLeaks ();
}

static VOID
TestAsyncStopDrainAndRetry (
  VOID
  )
{
  EFI_BLOCK_IO2_TOKEN  Token;
  TEST_EVENT           *UserEvent;
  EFI_TPL              OldTpl;
  UINT32               CallsBefore;
  UINT32               MediaId;

  SetupDevice ();
  UserEvent = CreateUserEvent ();
  mUserEvent = UserEvent;
  Token.Event = (EFI_EVENT)UserEvent;
  Token.TransactionStatus = EFI_SUCCESS;

  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &Token, 512, (VOID *)mDevice) == EFI_SUCCESS);
  CHECK (mBuildCalls == 0);
  CHECK (mPassThruCalls == 1);
  CHECK (mPendingPacket != NULL && mPendingEvent != NULL);
  CHECK (mDevice->ActiveOperations == 0);
  CHECK (!IsListEmpty (&mDevice->Queue));

  CHECK (StopCurrentChild () == EFI_NOT_READY);
  CheckTombstone (8);
  CHECK (mDevice != NULL);
  CHECK (!IsListEmpty (&mDevice->Queue));
  CHECK (mCloseProtocolCalls == 0);
  CHECK (mUninstallCalls == 0);
  CHECK (UserEvent->Signals == 0);

  CallsBefore = mBuildCalls;
  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 8, 0, NULL, 512, (VOID *)mDevice) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 8, 0, NULL, 512, (VOID *)mDevice) == EFI_NO_MEDIA);
  CHECK (mBuildCalls == CallsBefore);
  CHECK (mPassThruCalls == 1);

  mPendingPacket->TransactionStatus = EFI_NOT_FOUND;
  OldTpl = mCurrentTpl;
  mCurrentTpl = TPL_NOTIFY;
  mPendingEvent->Notify (mPendingEvent, mPendingEvent->Context);
  mCurrentTpl = OldTpl;
  mPendingPacket = NULL;
  mPendingEvent = NULL;
  CHECK (Token.TransactionStatus == EFI_NO_MEDIA);
  CHECK (UserEvent->Signals == 1);
  CHECK (IsListEmpty (&mDevice->Queue));
  CHECK (mDevice->ActiveOperations == 0);
  CheckTombstone (8);
  CHECK (mEventFrees == 1);
  CHECK (mRequestFrees == 1);

  MediaId = mDevice->BlockMedia.MediaId;
  CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, MediaId, 0, 512, (VOID *)mDevice) == EFI_NO_MEDIA);
  CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, 512, (VOID *)mDevice) == EFI_MEDIA_CHANGED);
  CHECK (mPassThruCalls == 1);
  CHECK (mBuildCalls == CallsBefore);

  mCloseProtocolStatus = EFI_ACCESS_DENIED;
  CHECK (StopCurrentChild () == EFI_DEVICE_ERROR);
  CHECK (mDevice != NULL && mDevice->MediaInvalidated);
  CHECK (mCloseProtocolCalls == 1);
  CHECK (mUninstallCalls == 0);

  mCloseProtocolStatus = EFI_SUCCESS;
  mUninstallChildStatus = EFI_ACCESS_DENIED;
  CHECK (StopCurrentChild () == EFI_NOT_READY);
  CHECK (mDevice != NULL && mDevice->MediaInvalidated);
  CHECK (mCloseProtocolCalls == 2);
  CHECK (mUninstallCalls == 1);
  CHECK (mReopenPassThruCalls == 1);

  mUninstallChildStatus = EFI_SUCCESS;
  CHECK (StopCurrentChild () == EFI_SUCCESS);
  CHECK (mCloseProtocolCalls == 3);
  CHECK (mUninstallCalls == 2);
  CHECK (mDeviceFrees == 1);
  mDevice = NULL;
  FreePool (UserEvent);
  mUserEvent = NULL;
  CHECK (mAllocations == 0);
  CHECK (mBuildPathAllocations == mBuildPathFrees);
  CHECK (mTplRaises == mTplRestores);
  CHECK (mTplViolations == 0);
}

static VOID
CheckNoStartLeaks (
  VOID
  )
{
  CHECK (mAllocations == 0);
  CHECK (mBuildPathAllocations == mBuildPathFrees);
  CHECK (mChildEntryCount == 0);
  CHECK (mTplRaises == mTplRestores);
  CHECK (mTplViolations == 0);
}

static VOID
StopAllStartedResources (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mChildren); Index++) {
    if (mChildren[Index].Installed) {
      CHECK (StopTrackedChild (Index) == EFI_SUCCESS);
    }
  }

  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_SUCCESS);
}

static VOID
TestLaterSlotDiscoveryFailureKeepsEarlierChildPrivate (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       ChildIndex;
  EFI_HANDLE  ChildBuffer[1];

  SetupStartEnvironment (2);
  mFailCommandIndex = SD_SEND_CSD;
  mFailCommandOccurrence = 2;
  mCommandFailureStatus = EFI_DEVICE_ERROR;
  Status = SdDxeDriverBindingStart (&mBinding, mController, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (CountInstalledChildren () == 1);
  CHECK (mChildEntryCount == 1);
  CHECK (mChildren[FirstInstalledChild ()].ChildAssociation);
  CHECK (mInstalledPrivate != NULL);
  CHECK (mCallerIdInstalled);
  CHECK (mInstalledPrivate->ChildCount == 1);
  CHECK (mChildren[FirstInstalledChild ()].Device->Private == mInstalledPrivate);
  CHECK (mPrivateFrees == 0);
  CHECK (mParentCloseCalls == 0);
  CHECK (mUninstallCallerCalls == 0);

  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_NOT_READY);
  CHECK (mCallerIdInstalled && mPrivateFrees == 0);

  ChildIndex = FirstInstalledChild ();
  ChildBuffer[0] = mChildren[ChildIndex].Handle;
  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 1, ChildBuffer) == EFI_SUCCESS);
  CHECK (mChildren[ChildIndex].Device == NULL);
  CHECK (mInstalledPrivate->ChildCount == 0);
  CHECK (mPrivateFrees == 0);
  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_SUCCESS);
  CHECK (mPrivateFrees == 1);
  CHECK (!mCallerIdInstalled);
  CheckNoStartLeaks ();
}

static VOID
TestChildInstallThenOpenFailureRollback (
  VOID
  )
{
  EFI_STATUS  Status;

  SetupStartEnvironment (1);
  mChildOpenFailuresRemaining = 1;
  mChildOpenStatus = EFI_ACCESS_DENIED;
  mUninstallChildStatus = EFI_SUCCESS;
  Status = SdDxeDriverBindingStart (&mBinding, mController, NULL);
  CHECK (Status == EFI_ACCESS_DENIED);
  CHECK (mChildOpenAttempts == 1);
  CHECK (mUninstallChildCalls == 1);
  CHECK (CountInstalledChildren () == 0);
  CHECK (mChildEntryCount == 0);
  CHECK (!mCallerIdInstalled);
  CHECK (mInstalledPrivate == NULL);
  CHECK (mPrivateFrees == 1);
  CHECK (mDeviceFrees == 1);
  CHECK (mParentCloseCalls == 1);
  CheckNoStartLeaks ();
}

static VOID
TestChildOpenAndUndoFailuresRetainOrphanForStop (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       ChildIndex;

  SetupStartEnvironment (1);
  mChildOpenFailuresRemaining = 2;
  mChildOpenStatus = EFI_ACCESS_DENIED;
  mUninstallChildStatus = EFI_ACCESS_DENIED;
  Status = SdDxeDriverBindingStart (&mBinding, mController, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (mChildOpenAttempts == 2);
  CHECK (mUninstallChildCalls == 1);
  CHECK (CountInstalledChildren () == 1);
  ChildIndex = FirstInstalledChild ();
  CHECK (mChildEntryCount == 1);
  CHECK (!mChildren[ChildIndex].ChildAssociation);
  CHECK (mInstalledPrivate != NULL);
  CHECK (mCallerIdInstalled);
  CHECK (mInstalledPrivate->ChildCount == 1);
  CHECK (mChildren[ChildIndex].Device->MediaInvalidated);
  CHECK (mChildren[ChildIndex].Device->Private == mInstalledPrivate);
  CHECK (mPrivateFrees == 0);
  CHECK (mParentCloseCalls == 0);

  mUninstallChildStatus = EFI_SUCCESS;
  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_SUCCESS);
  CHECK (CountInstalledChildren () == 0);
  CHECK (mChildEntryCount == 0);
  CHECK (mInstalledPrivate == NULL);
  CHECK (mPrivateFrees == 1);
  CHECK (!mCallerIdInstalled);
  CHECK (mParentCloseCalls == 1);
  CHECK (mChildren[ChildIndex].Device == NULL);
  CheckNoStartLeaks ();
}

static VOID
TestChildUndoFailureThenRelationshipRetry (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       ChildIndex;

  SetupStartEnvironment (1);
  mChildOpenFailuresRemaining = 1;
  mChildOpenStatus = EFI_ACCESS_DENIED;
  mUninstallChildStatus = EFI_ACCESS_DENIED;
  Status = SdDxeDriverBindingStart (&mBinding, mController, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (mChildOpenAttempts == 2);
  CHECK (mUninstallChildCalls == 1);
  CHECK (CountInstalledChildren () == 1);
  CHECK (mChildren[FirstInstalledChild ()].ChildAssociation);
  CHECK (mInstalledPrivate != NULL && mInstalledPrivate->ChildCount == 1);

  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_NOT_READY);
  CHECK (mCallerIdInstalled && mPrivateFrees == 0);
  ChildIndex = FirstInstalledChild ();
  mUninstallChildStatus = EFI_SUCCESS;
  CHECK (StopTrackedChild (ChildIndex) == EFI_SUCCESS);
  CHECK (mInstalledPrivate->ChildCount == 0);
  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_SUCCESS);
  CHECK (mPrivateFrees == 1);
  CheckNoStartLeaks ();
}

static VOID
TestNoChildStopFailureRetryPreservesPrivate (
  VOID
  )
{
  EFI_STATUS  Status;

  SetupStartEnvironment (0);
  CHECK (SdDxeDriverBindingStart (&mBinding, mController, NULL) == EFI_SUCCESS);
  CHECK (mInstalledPrivate != NULL);
  CHECK (mCallerIdInstalled);
  CHECK (mInstalledPrivate->ChildCount == 0);

  mUninstallCallerStatus = EFI_ACCESS_DENIED;
  Status = SdDxeDriverBindingStop (&mBinding, mController, 0, NULL);
  CHECK (Status == EFI_ACCESS_DENIED);
  CHECK (mCallerIdInstalled);
  CHECK (mInstalledPrivate != NULL);
  CHECK (mPrivateFrees == 0);
  CHECK (mParentCloseCalls == 1);

  mUninstallCallerStatus = EFI_SUCCESS;
  mParentCloseStatus = EFI_ACCESS_DENIED;
  Status = SdDxeDriverBindingStop (&mBinding, mController, 0, NULL);
  CHECK (Status == EFI_ACCESS_DENIED);
  CHECK (mCallerIdInstalled);
  CHECK (mInstalledPrivate != NULL);
  CHECK (mPrivateFrees == 0);
  CHECK (mParentCloseCalls == 2);

  mParentCloseStatus = EFI_SUCCESS;
  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_SUCCESS);
  CHECK (!mCallerIdInstalled);
  CHECK (mInstalledPrivate == NULL);
  CHECK (mPrivateFrees == 1);
  CHECK (mParentCloseCalls == 3);
  CheckNoStartLeaks ();
}

static VOID
TestStopReentryDuringChildUninstall (
  VOID
  )
{
  UINTN  ChildIndex;

  SetupStartEnvironment (1);
  CHECK (SdDxeDriverBindingStart (&mBinding, mController, NULL) == EFI_SUCCESS);
  ChildIndex = FirstInstalledChild ();
  CHECK (mInstalledPrivate != NULL && mInstalledPrivate->ChildCount == 1);

  mReenterStopOnChildUninstall = TRUE;
  mResetBlockIo2OnChildUninstall = TRUE;
  CHECK (StopTrackedChild (ChildIndex) == EFI_SUCCESS);
  CHECK (mNestedStopStatus == EFI_NOT_READY);
  CHECK (!mChildren[ChildIndex].Installed);
  CHECK (mChildren[ChildIndex].Device == NULL);
  CHECK (mInstalledPrivate != NULL && mInstalledPrivate->ChildCount == 0);
  CHECK (mDeviceFrees == 1 && mPrivateFrees == 0);
  CHECK (SdDxeDriverBindingStop (&mBinding, mController, 0, NULL) == EFI_SUCCESS);
  CHECK (mPrivateFrees == 1);
  CheckNoStartLeaks ();
}

static VOID
CheckValidCsdGeometry (
  IN CONST MOCK_CSD_FIXTURE  *Fixture,
  IN UINT32                  ExpectedLastBlock,
  IN BOOLEAN                 ExpectedSectorAddressing,
  IN UINT32                  ExpectedEraseGranularity
  )
{
  EFI_STATUS  Status;
  UINTN       ChildIndex;
  SD_DEVICE   *Device;

  SetupStartEnvironment (1);
  SetMockCsdFixture (Fixture);
  Status = SdDxeDriverBindingStart (&mBinding, mController, NULL);
  CHECK (Status == EFI_SUCCESS);
  CHECK (CountInstalledChildren () == 1);
  ChildIndex = FirstInstalledChild ();
  Device = mChildren[ChildIndex].Device;
  CHECK (Device != NULL);
  CHECK (Device->BlockMedia.MediaPresent);
  CHECK (Device->BlockMedia.BlockSize == 0x200);
  CHECK (Device->BlockMedia.LastBlock == ExpectedLastBlock);
  CHECK (Device->SectorAddressing == ExpectedSectorAddressing);
  CHECK (Device->EraseBlock.EraseLengthGranularity == ExpectedEraseGranularity);

  StopAllStartedResources ();
  CheckNoStartLeaks ();
}

static VOID
TestValidCsdGeometryBoundaries (
  VOID
  )
{
  static CONST MOCK_CSD_FIXTURE  Cases[] = {
    { 0, 0, 0, 9, 9, FALSE, 0 },
    { 0, 0, 0, 9, 11, FALSE, 1 },
    { 0, 4095, 7, 11, 0, TRUE, 127 },
    { 1, 0x3fffffU, 0, 0, 0, TRUE, 0 }
  };
  static CONST UINT32  ExpectedLastBlock[] = {
    3,
    3,
    8388607U,
    MAX_UINT32
  };
  static CONST BOOLEAN  ExpectedSectorAddressing[] = {
    FALSE,
    FALSE,
    FALSE,
    TRUE
  };
  static CONST UINT32  ExpectedEraseGranularity[] = {
    1,
    8,
    1,
    1
  };
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (Cases); Index++) {
    CheckValidCsdGeometry (
      &Cases[Index],
      ExpectedLastBlock[Index],
      ExpectedSectorAddressing[Index],
      ExpectedEraseGranularity[Index]
      );
  }
}

static VOID
TestInvalidCsdRejectedBeforePublication (
  VOID
  )
{
  static CONST MOCK_CSD_FIXTURE  Cases[] = {
    { 2, 0, 0, 9, 9, TRUE, 0 },
    { 3, 0, 0, 9, 9, TRUE, 0 },
    { 0, 0, 0, 8, 9, TRUE, 0 },
    { 0, 0, 0, 12, 9, TRUE, 0 },
    { 0, 0, 0, 9, 8, FALSE, 0 },
    { 0, 0, 0, 9, 12, FALSE, 0 },
    // Four bytes of computed capacity would make quotient - 1 underflow.
    { 0, 0, 0, 0, 9, TRUE, 0 }
  };
  UINTN       Index;
  EFI_STATUS  Status;

  for (Index = 0; Index < ARRAY_SIZE (Cases); Index++) {
    SetupStartEnvironment (1);
    SetMockCsdFixture (&Cases[Index]);
    Status = SdDxeDriverBindingStart (&mBinding, mController, NULL);
    CHECK (Status == EFI_UNSUPPORTED);
    CHECK (mInstallChildCalls == 0);
    CHECK (mUninstallChildCalls == 0);
    CHECK (CountInstalledChildren () == 0);
    CHECK (mChildEntryCount == 0);
    CHECK (!mCallerIdInstalled);
    CHECK (mPrivateFrees == 1);
    CHECK (mDeviceFrees == 1);
    CheckNoStartLeaks ();
  }
}

static UINTN
CountPendingReads (VOID)
{
  UINTN Count = 0;
  LIST_ENTRY *Link;
  for (Link = GetFirstNode (&mDevice->Queue); !IsNull (&mDevice->Queue, Link);
       Link = GetNextNode (&mDevice->Queue, Link)) { Count++; }
  return Count;
}

static VOID
CompletePendingRead (UINTN Index, EFI_STATUS Status)
{
  LIST_ENTRY *Link = GetFirstNode (&mDevice->Queue);
  while (Index-- != 0) { Link = GetNextNode (&mDevice->Queue, Link); }
  SD_REQUEST *Request = SD_REQUEST_FROM_LINK (Link);
  EFI_TPL OldTpl = mCurrentTpl;
  Request->Packet.TransactionStatus = Status;
  mCurrentTpl = TPL_NOTIFY;
  MockSignalEvent (Request->Event);
  mCurrentTpl = OldTpl;
  mPendingPacket = NULL;
  mPendingEvent = NULL;
}

static VOID
TestAsyncBatchOwnership (VOID)
{
  UINTN Failure, Reverse;
  for (Failure = 0; Failure < 11; Failure++) for (Reverse = 0; Reverse < 2; Reverse++) {
    EFI_BLOCK_IO2_TOKEN Token;
    TEST_EVENT *Event;
    VOID *Buffer;
    EFI_STATUS Expected, Returned;
    UINTN Pending;
    SetupDevice ();
    mDevice->BlockMedia.LastBlock = 131070;
    Buffer = AllocatePool (131071U * 512U);
    CHECK (Buffer != NULL);
    Event = CreateUserEvent ();
    Token.Event = (EFI_EVENT)Event;
    Token.TransactionStatus = EFI_NOT_READY;
    Expected = EFI_SUCCESS;
    if ((Failure >= 1) && (Failure <= 3)) {
      mFailCommandIndex = (Failure == 3) ? SD_READ_SINGLE_BLOCK : SD_READ_MULTIPLE_BLOCK;
      mFailCommandOccurrence = (Failure == 2) ? 2 : 1;
      mCommandFailureStatus = Expected = EFI_DEVICE_ERROR;
    } else if (Failure >= 4) {
      mFailAllocationAttempt = mAllocationAttempt + Failure - 3;
      Expected = EFI_OUT_OF_RESOURCES;
    }
    Returned = mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &Token,
                                             131071U * 512U, Buffer);
    mFailAllocationAttempt = 0;
    Pending = CountPendingReads ();
    CHECK (Returned == ((Pending != 0) ? EFI_SUCCESS : Expected));
    CHECK (Event->Signals == 0 && mDevice->ActiveOperations == 0);
    if (Failure == 0) CHECK (Pending == 3);
    if (Failure == 1 || (Failure >= 4 && Failure <= 6)) CHECK (Pending == 0);
    if (Failure == 2 || Failure == 7 || Failure == 8) CHECK (Pending == 1);
    if (Failure == 3 || Failure >= 9) CHECK (Pending == 2);
    while (CountPendingReads () != 0) {
      UINTN Count = CountPendingReads ();
      CompletePendingRead (Reverse ? Count - 1 : 0, EFI_SUCCESS);
      CHECK (Event->Signals == ((Count == 1) ? 1U : 0U));
    }
    if (Pending != 0) CHECK (Token.TransactionStatus == Expected);
    CHECK (mRequestFrees >= mPassThruCalls);
    FreePool (Buffer);
    FreePool (Event);
    FinishAndCheckNoLeaks ();
  }

  // The last-submitted command may complete first, including an error.
  for (UINTN Removal = 0; Removal < 2; Removal++) {
    SetupDevice ();
    mDevice->BlockMedia.LastBlock = 131070;
    VOID *Buffer = AllocatePool (131071U * 512U);
    TEST_EVENT *Event = CreateUserEvent ();
    EFI_BLOCK_IO2_TOKEN Token = { (EFI_EVENT)Event, EFI_NOT_READY };
    EFI_STATUS Error = Removal ? EFI_NO_MEDIA : EFI_CRC_ERROR;
    CHECK (Buffer != NULL);
    CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &Token,
                                         131071U * 512U, Buffer) == EFI_SUCCESS);
    CHECK (CountPendingReads () == 3);
    CompletePendingRead (2, Error);
    CHECK (Event->Signals == 0 && CountPendingReads () == 2);
    CompletePendingRead (0, EFI_SUCCESS);
    CHECK (Event->Signals == 0 && CountPendingReads () == 1);
    CHECK (StopCurrentChild () == EFI_NOT_READY);
    CompletePendingRead (0, EFI_SUCCESS);
    CHECK (Event->Signals == 1 && Token.TransactionStatus == Error);
    CHECK (IsListEmpty (&mDevice->Queue));
    FreePool (Buffer);
    FreePool (Event);
    FinishAndCheckNoLeaks ();
  }

  // Inline completion can free the caller token, but only after submission ends.
  for (UINTN Error = 0; Error < 2; Error++) {
    SetupDevice ();
    mDevice->BlockMedia.LastBlock = 131070;
    mCompletePassThruSynchronously = TRUE;
    mAsyncCompletionStatus = Error ? EFI_CRC_ERROR : EFI_SUCCESS;
    VOID *Buffer = AllocatePool (131071U * 512U);
    EFI_BLOCK_IO2_TOKEN *Token = TestAllocate (sizeof (*Token), AllocationGeneral, TRUE);
    TEST_EVENT *Event = CreateUserEvent ();
    CHECK (Buffer != NULL && Token != NULL);
    BindFreeingUserEvent (Event, Token);
    Token->Event = (EFI_EVENT)Event;
    CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, Token,
                                         131071U * 512U, Buffer) == EFI_SUCCESS);
    CHECK (Event->Signals == 1 && mUserTokenFrees == 1);
    CHECK (mPassThruCalls == (Error ? 1U : 3U));
    CHECK (IsListEmpty (&mDevice->Queue) && mDevice->ActiveOperations == 0);
    FreePool (Buffer);
    FreePool (Event);
    mUserEvent = NULL;
    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestSynchronousProviderPolicy (
  VOID
  )
{
  CONST UINTN Sizes[] = { 1, 65536, 131071 };
  UINTN Provider, Size, Failure;
  for (Provider = 0; Provider < 2; Provider++) {
    for (Size = 0; Size < ARRAY_SIZE (Sizes); Size++) {
      for (Failure = 0; Failure < ((Size == 0) ? 2U : 3U); Failure++) {
        EFI_BLOCK_IO2_TOKEN Token;
        TEST_EVENT *Event;
        VOID *Buffer;
        SetupDevice ();
        mSynchronousPassThru = TRUE;
        mIgnorePassThruEvent = (BOOLEAN)Provider;
        mDevice->BlockMedia.LastBlock = Sizes[Size] - 1;
        Buffer = AllocatePool (Sizes[Size] * 512);
        CHECK (Buffer != NULL);
        Event = CreateUserEvent ();
        Token.Event = (EFI_EVENT)Event;
        Token.TransactionStatus = EFI_NOT_READY;
        if (Failure != 0) {
          mFailCommandIndex = ((Size == 0) || ((Size == 1) && (Failure == 2))) ?
                              SD_READ_SINGLE_BLOCK : SD_READ_MULTIPLE_BLOCK;
          mFailCommandOccurrence = ((Size == 2) && (Failure == 2)) ? 2 : 1;
          mCommandFailureStatus = EFI_DEVICE_ERROR;
        }
        CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &Token,
                Sizes[Size] * 512, Buffer) == ((Failure == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR));
        CHECK (Event->Signals == ((Failure == 0) ? 1U : 0U));
        CHECK (Token.Event == (EFI_EVENT)Event);
        if (Failure == 0) CHECK (Token.TransactionStatus == EFI_SUCCESS);
        CHECK (mPassThruCalls == ((Failure == 0) ? (Sizes[Size] + 65534) / 65535 : Failure));
        CHECK (mPendingPacket == NULL && mPendingEvent == NULL);
        CHECK (IsListEmpty (&mDevice->Queue) && mDevice->ActiveOperations == 0);
        CHECK (mEventFrees == 0 && mRequestFrees == mPassThruCalls);
        FreePool (Buffer);
        FreePool (Event);
        FinishAndCheckNoLeaks ();
      }
    }
  }

  SetupDevice ();
  mSynchronousPassThru = TRUE;
  EFI_BLOCK_IO2_TOKEN *Token = TestAllocate (sizeof (*Token), AllocationGeneral, TRUE);
  TEST_EVENT *Event = CreateUserEvent ();
  UINT8 Data[512];
  BindFreeingUserEvent (Event, Token);
  Token->Event = (EFI_EVENT)Event;
  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, Token, sizeof (Data), Data) == EFI_SUCCESS);
  CHECK (mUserTokenFrees == 1 && Event->Signals == 1);
  FreePool (Event);
  mUserEvent = NULL;
  FinishAndCheckNoLeaks ();

  // The same policy covers EraseBlock's shared request/event machinery.
  SetupDevice ();
  mSynchronousPassThru = TRUE;
  Event = CreateUserEvent ();
  EFI_ERASE_BLOCK_TOKEN EraseToken = { (EFI_EVENT)Event, EFI_NOT_READY };
  CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7, 0, &EraseToken, 512) == EFI_SUCCESS);
  CHECK (Event->Signals == 1 && EraseToken.TransactionStatus == EFI_SUCCESS);
  CHECK (mPassThruCalls == 3 && IsListEmpty (&mDevice->Queue));
  FreePool (Event);
  FinishAndCheckNoLeaks ();
  mSynchronousPassThru = FALSE;
}

// Model caller notifications at TPL_CALLBACK, deferring signals raised while
// TPL_NOTIFY is held until RestoreTPL. This is still a focused event mock, not
// a complete UEFI dispatcher. Completion may reenter Stop and free the token.
static VOID
EFIAPI
StopAndFreeTokenOnSignal (
  IN EFI_EVENT Event,
  IN VOID *Context
  )
{
  EFI_BLOCK_IO2_TOKEN *Token = Context;
  CHECK (mCurrentTpl == TPL_CALLBACK);
  CHECK (Token->Event == Event && Token->TransactionStatus == EFI_SUCCESS);
  CHECK (IsListEmpty (&mDevice->Queue));
  CHECK (mDevice->ActiveOperations == (mSynchronousPassThru ? 0U : 1U));
  mReentrantStopStatus = StopCurrentChild ();
  CHECK (mReentrantStopStatus == (mSynchronousPassThru ? EFI_SUCCESS : EFI_NOT_READY));
  CHECK (mDeviceFrees == (mSynchronousPassThru ? 1U : 0U));
  mUserTokenFrees++;
  FreePool (Token);
}

static VOID
TestCompletionStopReentry (
  VOID
  )
{
  for (UINTN Synchronous = 0; Synchronous < 2; Synchronous++) {
    for (UINTN Split = 0; Split < 2; Split++) {
      UINTN Blocks = Split ? 131071 : 1;
      SetupDevice ();
      mDeferUserNotify = TRUE;
      mSynchronousPassThru = (BOOLEAN)Synchronous;
      mIgnorePassThruEvent = (BOOLEAN)Synchronous;
      mCompletePassThruSynchronously = TRUE;
      mDevice->BlockMedia.LastBlock = Blocks - 1;
      VOID *Buffer = AllocatePool (Blocks * 512);
      EFI_BLOCK_IO2_TOKEN *Token = TestAllocate (sizeof (*Token), AllocationGeneral, TRUE);
      TEST_EVENT *Event = CreateUserEvent ();
      CHECK (Buffer != NULL && Token != NULL);
      Token->Event = (EFI_EVENT)Event;
      Token->TransactionStatus = EFI_NOT_READY;
      Event->Notify = StopAndFreeTokenOnSignal;
      Event->Context = Token;
      CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, Token,
                                           Blocks * 512, Buffer) == EFI_SUCCESS);
      CHECK (Event->Signals == 1 && mUserTokenFrees == 1);
      CHECK (mPassThruCalls == (Split ? 3U : 1U));
      CHECK (mRequestFrees == mPassThruCalls);
      if (!Synchronous) {
        CHECK (mDevice->ActiveOperations == 0 && IsListEmpty (&mDevice->Queue));
        CHECK (StopCurrentChild () == EFI_SUCCESS);
      }
      // No second event after Stop retry; all internal requests and the child retired.
      CHECK (Event->Signals == 1 && mDeviceFrees == 1);
      CHECK (mDeferredUserEvent == NULL);
      mDeferUserNotify = FALSE;
      mDevice = NULL;
      FreePool (Buffer);
      FreePool (Event);
      CHECK (mAllocations == 0);
      CHECK (mBuildPathAllocations == mBuildPathFrees);
      CHECK (mTplRaises == mTplRestores && mTplViolations == 0);
    }
  }
}

static VOID
TestSynchronousTokenOutcomes (
  VOID
  )
{
  CONST EFI_STATUS Errors[] = {
    EFI_SUCCESS, EFI_CRC_ERROR, EFI_TIMEOUT, EFI_DEVICE_ERROR,
    EFI_NO_MEDIA, EFI_NOT_FOUND, EFI_MEDIA_CHANGED
  };
  for (UINTN Probe = 0; Probe < 2; Probe++) {
    for (UINTN Zero = 0; Zero < 2; Zero++) {
      for (UINTN Index = 0; Index < ARRAY_SIZE (Errors); Index++) {
        SetupDevice ();
        mSynchronousPassThru = TRUE;
        mIgnorePassThruEvent = TRUE;
        UINT8 Data[512];
        TEST_EVENT *Event = CreateUserEvent ();
        EFI_BLOCK_IO2_TOKEN Token = { (EFI_EVENT)Event, EFI_ABORTED };
        if (Probe) mBuildStatus = Errors[Index];
        else mPassThruStatus = Errors[Index];
        EFI_STATUS Expected = (!Probe && Zero) ? EFI_SUCCESS : Errors[Index];
        if (Expected == EFI_NOT_FOUND) Expected = EFI_NO_MEDIA;
        BOOLEAN Removed = (BOOLEAN)((Expected == EFI_NO_MEDIA) || (Expected == EFI_MEDIA_CHANGED));
        CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &Token,
                Zero ? 0 : sizeof (Data), Data) == Expected);
        CHECK (Token.Event == (EFI_EVENT)Event);
        CHECK (Token.TransactionStatus == (EFI_ERROR (Expected) ? EFI_ABORTED : EFI_SUCCESS));
        CHECK (Event->Signals == (EFI_ERROR (Expected) ? 0U : 1U));
        CHECK (mPassThruCalls == ((Zero || (Probe && EFI_ERROR (Errors[Index]))) ? 0U : 1U));
        CHECK (mPendingPacket == NULL && mPendingEvent == NULL);
        CHECK (IsListEmpty (&mDevice->Queue) && mDevice->ActiveOperations == 0);
        if (Removed) {
          CheckTombstone (8);
          // Failure is returned immediately, never a second delayed completion.
          CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &Token,
                  Zero ? 0 : sizeof (Data), Data) == EFI_MEDIA_CHANGED);
          CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 8, 0, &Token,
                  Zero ? 0 : sizeof (Data), Data) == EFI_NO_MEDIA);
          CHECK (Event->Signals == 0 && Token.TransactionStatus == EFI_ABORTED);
        } else {
          CHECK (mDevice->BlockMedia.MediaPresent && mDevice->BlockMedia.MediaId == 7);
        }
        FreePool (Event);
        FinishAndCheckNoLeaks ();
      }
    }
  }
}

static VOID
TestReadBufferAlignmentContract (
  VOID
  )
{
  UINT32 Storage[129];
  UINTN Offset;
  for (Offset = 1; Offset < 4; Offset++) {
    SetupDevice ();
    mDevice->BlockMedia.IoAlign = 4;
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, 512,
                                      (UINT8 *)Storage + Offset) == EFI_INVALID_PARAMETER);
    CHECK (mPassThruCalls == 0 && mDevice->ActiveOperations == 0);
    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestPartialReadErrorIdentity (
  VOID
  )
{
  CONST EFI_STATUS Errors[] = { EFI_CRC_ERROR, EFI_TIMEOUT };
  UINT8 Data[1024];
  UINTN Index, Byte;
  for (Index = 0; Index < ARRAY_SIZE (Errors); Index++) {
    SetupDevice ();
    memset (Data, 0xa5, sizeof (Data));
    mPassThruStatus = Errors[Index];
    mCopyPrefixOnReadError = TRUE;
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, sizeof (Data), Data) == Errors[Index]);
    CHECK (mPassThruCalls == 1 && mRequestFrees == 1);
    CHECK (mDevice->BlockMedia.MediaId == 7 && mDevice->BlockMedia.MediaPresent);
    CHECK (!mDevice->MediaInvalidated && mDevice->BlockMedia.LastBlock == 31);
    CHECK (mDevice->ActiveOperations == 0 && IsListEmpty (&mDevice->Queue));
    for (Byte = 0; Byte < sizeof (Data); Byte++) CHECK (Data[Byte] == ((Byte < 16) ? 0x33 : 0xa5));
    mPassThruStatus = EFI_SUCCESS;
    mCopyPrefixOnReadError = FALSE;
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, sizeof (Data), Data) == EFI_SUCCESS);
    CHECK (mPassThruCalls == 2 && mDevice->BlockMedia.MediaId == 7);
    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestSynchronousReadChunkBoundaries (
  VOID
  )
{
  static CONST UINTN Totals[] = { 65535, 65536, 65537, 131071 };
  UINTN Mode, Case, Failure, Index, ExpectedCalls, Remaining, Chunk, Advanced;
  EFI_LBA LastBlock, Start;
  VOID *Buffer;
  EFI_STATUS Status;

  for (Mode = 0; Mode < 2; Mode++) {
    LastBlock = (Mode == 0) ? 8388607 : MAX_UINT32;
    for (Case = 0; Case < ARRAY_SIZE (Totals); Case++) {
      for (Failure = 0; Failure <= ((Totals[Case] > 131070) ? 2U : 1U); Failure++) {
        SetupDevice ();
        mDevice->SectorAddressing = (BOOLEAN)Mode;
        mDevice->BlockMedia.LastBlock = LastBlock;
        Start = LastBlock - Totals[Case] + 1;
        Buffer = AllocatePool (Totals[Case] * 512);
        CHECK (Buffer != NULL);
        if (Failure != 0) {
          mFailCommandIndex = SD_READ_MULTIPLE_BLOCK;
          mFailCommandOccurrence = (UINT32)Failure;
          mCommandFailureStatus = EFI_TIMEOUT;
        }
        Status = mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7,
                                             Start, Totals[Case] * 512, Buffer);
        CHECK (Status == ((Failure == 0) ? EFI_SUCCESS : EFI_TIMEOUT));
        ExpectedCalls = (Failure == 0) ? ((Totals[Case] + 65534) / 65535) : Failure;
        CHECK (mPassThruCalls == ExpectedCalls);
        CHECK (mReadTraceCount == ExpectedCalls);
        Remaining = Totals[Case];
        Advanced = 0;
        for (Index = 0; Index < ExpectedCalls; Index++) {
          Chunk = (Remaining > 65535) ? 65535 : Remaining;
          CHECK (mReadTrace[Index].Command == ((Chunk == 1) ? SD_READ_SINGLE_BLOCK : SD_READ_MULTIPLE_BLOCK));
          CHECK (mReadTrace[Index].Argument == (UINT32)((Start + Advanced) * ((Mode == 0) ? 512 : 1)));
          CHECK (mReadTrace[Index].Length == Chunk * 512);
          CHECK (mReadTrace[Index].Buffer == (UINT8 *)Buffer + Advanced * 512);
          Remaining -= Chunk;
          Advanced += Chunk;
        }
        CHECK (mDevice->ActiveOperations == 0);
        CHECK (IsListEmpty (&mDevice->Queue));
        CHECK (mRequestFrees == ExpectedCalls);
        CHECK (mEventFrees == 0);
        FreePool (Buffer);
        FinishAndCheckNoLeaks ();
      }
    }
  }
}

static VOID
TestEraseRangeBoundaries (
  VOID
  )
{
  UINTN  Mode, Blocks;
  EFI_LBA LastBlock;

  // PassThru is a host mock: these calls never access or erase real media.
  for (Mode = 0; Mode < 2; Mode++) {
    LastBlock = (Mode == 0) ? 8388607 : MAX_UINT32;
    for (Blocks = 1; Blocks <= 2; Blocks++) {
      SetupDevice ();
      mDevice->SectorAddressing = (BOOLEAN)Mode;
      mDevice->BlockMedia.LastBlock = LastBlock;
      CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7,
              LastBlock - Blocks + 1, NULL, Blocks * 512) == EFI_SUCCESS);
      CHECK (mPassThruCalls == 3);
      CHECK (mEraseStartArgument == (UINT32)((LastBlock - Blocks + 1) * ((Mode == 0) ? 512 : 1)));
      CHECK (mEraseEndArgument == (UINT32)(LastBlock * ((Mode == 0) ? 512 : 1)));
      CHECK (mRequestFrees == 3);
      CHECK (IsListEmpty (&mDevice->Queue));
      CHECK (mDevice->ActiveOperations == 0);
      FinishAndCheckNoLeaks ();
    }

    SetupDevice ();
    mDevice->SectorAddressing = (BOOLEAN)Mode;
    mDevice->BlockMedia.LastBlock = LastBlock;
    CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7, MAX_UINT64, NULL, 1024) == EFI_INVALID_PARAMETER);
    CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7, LastBlock, NULL, 1024) == EFI_INVALID_PARAMETER);
    CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7, LastBlock + 1, NULL, 512) == EFI_INVALID_PARAMETER);
    CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7, 0, NULL, 513) == EFI_INVALID_PARAMETER);
    CHECK (mPassThruCalls == 0);
    CHECK (mDevice->ActiveOperations == 0);
    CHECK (IsListEmpty (&mDevice->Queue));
    mDevice->BlockMedia.ReadOnly = TRUE;
    CHECK (mDevice->EraseBlock.EraseBlocks (&mDevice->EraseBlock, 7, 0, NULL, 512) == EFI_WRITE_PROTECTED);
    CHECK (mPassThruCalls == 0);
    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestReadAddressBoundaries (
  VOID
  )
{
  UINT8       Data[1024];
  UINTN       Mode, Blocks;
  EFI_LBA     LastBlock;
  EFI_STATUS  Status;

  for (Mode = 0; Mode < 2; Mode++) {
    LastBlock = (Mode == 0) ? 8388607 : MAX_UINT32;
    for (Blocks = 1; Blocks <= 2; Blocks++) {
      SetupDevice ();
      mDevice->SectorAddressing = (BOOLEAN)Mode;
      mDevice->BlockMedia.LastBlock = LastBlock;
      Status = mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7,
                                          LastBlock - Blocks + 1, Blocks * 512, Data);
      CHECK (Status == EFI_SUCCESS);
      CHECK (mLastReadCommand == ((Blocks == 1) ? SD_READ_SINGLE_BLOCK : SD_READ_MULTIPLE_BLOCK));
      CHECK (mLastReadArgument == (UINT32)((LastBlock - Blocks + 1) * ((Mode == 0) ? 512 : 1)));
      CHECK (mLastReadLength == Blocks * 512);
      CHECK (mPassThruCalls == 1);
      CHECK (IsListEmpty (&mDevice->Queue));
      CHECK (mDevice->ActiveOperations == 0);
      FinishAndCheckNoLeaks ();
    }

    SetupDevice ();
    mDevice->SectorAddressing = (BOOLEAN)Mode;
    mDevice->BlockMedia.LastBlock = LastBlock;
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, MAX_UINT64, sizeof (Data), Data) == EFI_INVALID_PARAMETER);
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, LastBlock, sizeof (Data), Data) == EFI_INVALID_PARAMETER);
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, LastBlock + 1, 512, Data) == EFI_INVALID_PARAMETER);
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, 513, Data) == EFI_BAD_BUFFER_SIZE);
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, 1024,
                                      (VOID *)(MAX_UINTN - 511)) == EFI_INVALID_PARAMETER);
    CHECK (mPassThruCalls == 0);
    CHECK (IsListEmpty (&mDevice->Queue));
    CHECK (mDevice->ActiveOperations == 0);
    FinishAndCheckNoLeaks ();

    SetupDevice ();
    mDevice->SectorAddressing = (BOOLEAN)Mode;
    mPassThruStatus = EFI_TIMEOUT;
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 1, sizeof (Data), Data) == EFI_TIMEOUT);
    CHECK (mPassThruCalls == 1);
    CHECK (IsListEmpty (&mDevice->Queue));
    CHECK (mDevice->ActiveOperations == 0);
    CHECK (mRequestFrees == 1);
    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestCsdCapacityFieldBoundaries (
  VOID
  )
{
  static CONST UINT32  V1Sizes[] = { 0, 3, 4, 4095 };
  static CONST UINT32  V2Sizes[] = { 0, 1, 0xfffe, 0xffff, 0x10000, 0x10001, 0x3ffffe, 0x3fffff };
  UINTN       Index;
  UINT8       ReadLength;
  UINT8       Multiplier;
  UINT64      ExpectedBlocks;
  EFI_STATUS  Status;

  // Exercise the encoded fields through the real CMD9 response and parser.
  for (ReadLength = 0; ReadLength < 16; ReadLength++) {
    for (Multiplier = 0; Multiplier < 8; Multiplier++) {
      for (Index = 0; Index < ARRAY_SIZE (V1Sizes); Index++) {
        SetupDevice ();
        SetMockCsdV1 (V1Sizes[Index], Multiplier, ReadLength, 9, TRUE, 0);
        mDevice->BlockMedia.MediaPresent = FALSE;
        mDevice->BlockMedia.LastBlock = 0;
        Status = DiscoverUserArea (mDevice);
        if ((ReadLength >= 9) && (ReadLength <= 11)) {
          ExpectedBlocks = ((UINT64)V1Sizes[Index] + 1) << (Multiplier + 2 + ReadLength - 9);
          CHECK (Status == EFI_SUCCESS);
          CHECK (mDevice->BlockMedia.MediaPresent);
          CHECK (mDevice->BlockMedia.BlockSize == 512);
          CHECK (mDevice->BlockMedia.LastBlock == ExpectedBlocks - 1);
          CHECK (!mDevice->SectorAddressing);
        } else {
          CHECK (Status == EFI_UNSUPPORTED);
          CHECK (!mDevice->BlockMedia.MediaPresent);
          CHECK (mDevice->BlockMedia.LastBlock == 0);
        }
        FinishAndCheckNoLeaks ();
      }
    }
  }

  for (Index = 0; Index < ARRAY_SIZE (V2Sizes); Index++) {
    SetupDevice ();
    SetMockCsdV2 (V2Sizes[Index], TRUE, 0);
    mDevice->BlockMedia.MediaPresent = FALSE;
    ExpectedBlocks = ((UINT64)V2Sizes[Index] + 1) * 1024;
    CHECK (DiscoverUserArea (mDevice) == EFI_SUCCESS);
    CHECK (mDevice->BlockMedia.MediaPresent);
    CHECK (mDevice->BlockMedia.BlockSize == 512);
    CHECK (mDevice->BlockMedia.LastBlock == ExpectedBlocks - 1);
    CHECK (mDevice->SectorAddressing);
    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestTinyCsdDoesNotMarkMediaPresent (
  VOID
  )
{
  EFI_STATUS  Status;

  SetupDevice ();
  SetMockCsdV1 (0, 0, 0, 9, TRUE, 0);
  mDevice->BlockMedia.MediaPresent = FALSE;
  mDevice->BlockMedia.LastBlock = 0;
  Status = DiscoverUserArea (mDevice);
  CHECK (Status == EFI_UNSUPPORTED);
  CHECK (!mDevice->BlockMedia.MediaPresent);
  CHECK (mDevice->BlockMedia.LastBlock == 0);
  CHECK (mInstallChildCalls == 0);
  FinishAndCheckNoLeaks ();
}

static VOID
TestReadOnlyPolicy (
  VOID
  )
{
  UINTN       Policy;
  UINTN       Version;
  UINTN       Protection;
  UINT32      CallsBefore;
  BOOLEAN     ReadOnly;
  EFI_STATUS  Status;
  UINT8       Data[512];

  for (Policy = 0; Policy < 2; Policy++) {
    for (Version = 0; Version < 2; Version++) {
      for (Protection = 0; Protection < 4; Protection++) {
        SetupDevice ();
        mSdReadOnly = (BOOLEAN)Policy;
        if (Version == 0) {
          SetMockCsdV1 (3, 0, 9, 9, TRUE, 0);
        } else {
          SetMockCsdV2 (3, TRUE, 0);
        }

        mMockCsd.TmpWriteProtect = (Protection & 1) != 0;
        mMockCsd.PermWriteProtect = (Protection & 2) != 0;
        CHECK (DiscoverUserArea (mDevice) == EFI_SUCCESS);
        ReadOnly = (BOOLEAN)(Policy || Protection);
        CHECK (mDevice->BlockMedia.ReadOnly == ReadOnly);
        CHECK (mDevice->BlockIo2.Media->ReadOnly == ReadOnly);
        CallsBefore = mPassThruCalls;
        Status = mDevice->BlockIo.WriteBlocks (
                                   &mDevice->BlockIo,
                                   mDevice->BlockMedia.MediaId,
                                   0,
                                   sizeof (Data),
                                   Data
                                   );
        CHECK (Status == (ReadOnly ? EFI_WRITE_PROTECTED : EFI_SUCCESS));
        CHECK (mPassThruCalls == CallsBefore + (ReadOnly ? 0 : 2));
        if (ReadOnly) {
          TEST_EVENT *Event = CreateUserEvent ();
          EFI_BLOCK_IO2_TOKEN Token = { (EFI_EVENT)Event, EFI_ABORTED };
          CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, &Token,
                                                 sizeof (Data), Data) == EFI_WRITE_PROTECTED);
          CHECK (Event->Signals == 0 && mArmedTimers == 0 && mPassThruCalls == CallsBefore);
          FreePool (Event);
        }

        FinishAndCheckNoLeaks ();
      }
    }
  }

  mSdReadOnly = FALSE;
}

static VOID
TestWritePacketContract (
  VOID
  )
{
  CONST EFI_STATUS Failures[] = { EFI_SUCCESS, EFI_DEVICE_ERROR, EFI_CRC_ERROR };
  UINTN Addressing, Blocks, Failure, Index;
  UINT8 Data[1024];

  for (Index = 0; Index < sizeof (Data); Index++) {
    Data[Index] = (UINT8)(Index * 37 + 11);
  }
  for (Addressing = 0; Addressing < 2; Addressing++) {
    for (Blocks = 1; Blocks <= 2; Blocks++) {
      for (Failure = 0; Failure < ARRAY_SIZE (Failures); Failure++) {
        EFI_STATUS Status;
        UINT32 Calls;
        SetupDevice ();
        mDevice->SectorAddressing = (BOOLEAN)Addressing;
        mTraceWrites = TRUE;
        mFailCommandIndex = Blocks == 1 ? SD_WRITE_SINGLE_BLOCK : SD_WRITE_MULTIPLE_BLOCK;
        mFailCommandOccurrence = 1;
        mCommandFailureStatus = Failures[Failure];
        Status = mDevice->BlockIo.WriteBlocks (&mDevice->BlockIo, 7, 3, Blocks * 512, Data);
        CHECK (Status == Failures[Failure]);
        CHECK (mWriteCalls == 1);
        CHECK (mWriteCommand == (Blocks == 1 ? SD_WRITE_SINGLE_BLOCK : SD_WRITE_MULTIPLE_BLOCK));
        CHECK (mWriteArgument == (Addressing ? 3U : 3U * 512));
        CHECK (mWriteLength == Blocks * 512);
        CHECK (memcmp (mWritePayload, Data, Blocks * 512) == 0);
        CHECK (IsListEmpty (&mDevice->Queue));
        CHECK (mDevice->ActiveOperations == 0);
        // A previous write result must never make a removed medium flush succeed.
        SdInvalidateMedia (mDevice);
        Calls = mPassThruCalls;
        CHECK (mDevice->BlockIo.FlushBlocks (&mDevice->BlockIo) == EFI_NO_MEDIA);
        CHECK (mDevice->BlockIo2.FlushBlocksEx (&mDevice->BlockIo2, NULL) == EFI_NO_MEDIA);
        CHECK (mPassThruCalls == Calls);
        FinishAndCheckNoLeaks ();
      }
    }
  }
}


static VOID
TickOrderedIo (
  VOID
  )
{
  TEST_EVENT  *Timer = mDevice->OrderedIoTimer;

  CHECK (Timer != NULL && Timer->Armed);
  CHECK (mCurrentTpl == TPL_APPLICATION);
  MockSignalEvent (Timer);
}

static VOID
CompleteWriteTransfer (
  IN EFI_STATUS  Status
  )
{
  EFI_EVENT  Completion;

  CHECK (mPendingEvent != NULL && mPendingPacket != NULL);
  CHECK ((mPendingPacket->SdMmcCmdBlk->CommandIndex == SD_WRITE_SINGLE_BLOCK) ||
         (mPendingPacket->SdMmcCmdBlk->CommandIndex == SD_WRITE_MULTIPLE_BLOCK));
  Completion = mPendingEvent;
  mPendingPacket->TransactionStatus = Status;
  mPendingPacket = NULL;
  mPendingEvent = NULL;
  MockSignalEvent (Completion);
}

static VOID
CheckCommand (
  IN UINTN    Index,
  IN UINT16   Command,
  IN UINT32   Argument,
  IN BOOLEAN  Async
  )
{
  CHECK (Index < mCommandCount);
  CHECK (mCommands[Index].Command == Command);
  CHECK (mCommands[Index].Argument == Argument);
  CHECK (mCommands[Index].Async == Async);
  CHECK (mCommands[Index].Tpl <= TPL_CALLBACK);
}

static VOID
TestWriteStatus (
  VOID
  )
{
  enum { Ready, RecordingError, Protected, Exhausted, Removed, Replaced, NoResponse };
  STATIC CONST EFI_STATUS Expected[] = {
    EFI_SUCCESS, EFI_DEVICE_ERROR, EFI_WRITE_PROTECTED, EFI_TIMEOUT,
    EFI_NO_MEDIA, EFI_MEDIA_CHANGED, EFI_TIMEOUT
  };
  STATIC CONST UINTN Polls[] = { 3, 2, 2, 100, 2, 2, 1 };
  STATIC CONST UINTN Stalls[] = { 2, 1, 1, 99, 1, 1, 0 };
  UINT8 Data[1024];

  ZeroMem (Data, sizeof (Data));
  // Run the same command/status cases with Block I/O, a queued provider and
  // Block I/O 2 using the board's synchronous-provider policy.
  for (UINTN Mode = 0; Mode < 3; Mode++) {
    for (UINTN Case = 0; Case < ARRAY_SIZE (Expected); Case++) {
      SetupDevice ();
      BOOLEAN Queued = (BOOLEAN)(Mode == 1);
      mSynchronousPassThru = (BOOLEAN)(Mode == 2);
      mIgnorePassThruEvent = mSynchronousPassThru;
      mReturnedRca += (UINT16)(Mode * ARRAY_SIZE (Expected) + Case + 1);
      CHECK (DiscoverUserArea (mDevice) == EFI_SUCCESS);
      CHECK (mDevice->Rca == mReturnedRca);
      mStatusCalls = 0;
      mTraceCommands = TRUE;
      mDefaultStatusResponse = TEST_SD_PRG;
      mStatusResponses[0] = TEST_SD_PRG | BIT8;
      mStatusResponses[1] = TEST_SD_READY;
      mStatusResponseCount = 2;
      if (Case == Ready) {
        mStatusResponses[1] = TEST_SD_TRAN;
        mStatusResponses[2] = TEST_SD_READY;
        mStatusResponseCount = 3;
      } else if (Case == RecordingError) {
        mStatusResponses[0] = TEST_SD_PRG | BIT20;
      } else if (Case == Protected) {
        mWriteResponse = BIT26;
      } else if (Case == Exhausted) {
        mStatusResponseCount = 0;
      } else {
        mFailCommandIndex = SD_SEND_STATUS;
        mFailCommandOccurrence = (Case == NoResponse) ? 1 : 2;
        mCommandFailureStatus = (Case == Removed) ? EFI_NOT_FOUND : Expected[Case];
      }

      TEST_EVENT *Event = CreateUserEvent ();
      EFI_BLOCK_IO2_TOKEN Token = { (EFI_EVENT)Event, EFI_ABORTED };
      EFI_STATUS Status;
      if (Mode == 0) {
        Status = mDevice->BlockIo.WriteBlocks (&mDevice->BlockIo, 7, 0, sizeof (Data), Data);
      } else {
        Status = mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, &Token, sizeof (Data), Data);
      }

      if (Queued) {
        CHECK (Status == EFI_SUCCESS && Event->Signals == 0 && mCommandCount == 0);
        TickOrderedIo ();
        CHECK (mCommandCount == 1 && mStatusCalls == 0 && Event->Signals == 0);
        CompleteWriteTransfer (EFI_SUCCESS);
        CHECK (mCommandCount == 1 && Token.TransactionStatus == EFI_ABORTED && Event->Signals == 0);
        TickOrderedIo ();
        CHECK (Event->Signals == 1 && Token.TransactionStatus == Expected[Case]);
        CHECK (mArmedTimers == 0 && IsListEmpty (&mDevice->OrderedIo));
      } else {
        CHECK (Status == Expected[Case]);
        CHECK (Event->Signals == (((Mode == 2) && !EFI_ERROR (Status)) ? 1U : 0U));
        CHECK (mArmedTimers == 0 && mPendingPacket == NULL);
      }

      CHECK (mStatusCalls == Polls[Case] && mStallCalls == Stalls[Case]);
      CHECK (mCommandCount == 1 + Polls[Case]);
      CheckCommand (0, SD_WRITE_MULTIPLE_BLOCK, 0, Queued);
      for (UINTN Index = 1; Index < mCommandCount; Index++) {
        CheckCommand (Index, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
        if (Queued) {
          CHECK (mCommands[Index].Tpl == TPL_CALLBACK);
        }
      }
      if ((Case == Removed) || (Case == Replaced)) {
        CheckTombstone (8);
      }

      FreePool (Event);
      FinishAndCheckNoLeaks ();
    }
  }
}

static VOID
CheckSyncIoWaitsForWrite (
  VOID
  )
{
  UINT8 Data[512];
  UINT32 Calls = mPassThruCalls;

  CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, sizeof (Data), Data) == EFI_NOT_READY);
  CHECK (mDevice->BlockIo.WriteBlocks (&mDevice->BlockIo, 7, 0, sizeof (Data), Data) == EFI_NOT_READY);
  CHECK (mDevice->BlockIo.FlushBlocks (&mDevice->BlockIo) == EFI_NOT_READY);
  CHECK (mDevice->BlockIo2.Reset (&mDevice->BlockIo2, FALSE) == EFI_NOT_READY);
  CHECK (Calls == mPassThruCalls);
}

static VOID
TestSplitWriteOrdering (
  VOID
  )
{
  UINTN Bytes = 65536U * 512U;

  for (UINTN Queued = 0; Queued < 2; Queued++) {
    SetupDevice ();
    mDevice->BlockMedia.LastBlock = 65535;
    mTraceCommands = TRUE;
    mStatusResponses[0] = TEST_SD_PRG;
    mStatusResponses[1] = TEST_SD_READY;
    mStatusResponseCount = 2;
    VOID *Data = AllocatePool (Bytes);
    TEST_EVENT *Event = CreateUserEvent ();
    EFI_BLOCK_IO2_TOKEN Token = { (EFI_EVENT)Event, EFI_ABORTED };
    CHECK (Data != NULL);

    if (Queued) {
      CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, &Token, Bytes, Data) == EFI_SUCCESS);
      TickOrderedIo ();
      CHECK (mCommandCount == 1 && mPendingPacket->OutTransferLength == 65535U * 512U);
      CheckSyncIoWaitsForWrite ();
      CompleteWriteTransfer (EFI_SUCCESS);
      TickOrderedIo ();
      CHECK (mCommandCount == 4 && mPendingPacket->OutTransferLength == 512 && Event->Signals == 0);
      CompleteWriteTransfer (EFI_SUCCESS);
      CHECK (Event->Signals == 0);
      TickOrderedIo ();
      CHECK (Event->Signals == 1 && Token.TransactionStatus == EFI_SUCCESS);
    } else {
      mOnStatus = CheckSyncIoWaitsForWrite;
      CHECK (mDevice->BlockIo.WriteBlocks (&mDevice->BlockIo, 7, 0, Bytes, Data) == EFI_SUCCESS);
      mOnStatus = NULL;
    }

    CHECK (mCommandCount == 5);
    CheckCommand (0, SD_WRITE_MULTIPLE_BLOCK, 0, (BOOLEAN)Queued);
    CheckCommand (1, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
    CheckCommand (2, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
    CheckCommand (3, SD_WRITE_SINGLE_BLOCK, 65535, (BOOLEAN)Queued);
    CheckCommand (4, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, 512, Data) == EFI_SUCCESS);
    CheckCommand (5, SD_READ_SINGLE_BLOCK, 0, FALSE);
    FreePool (Event);
    FreePool (Data);
    FinishAndCheckNoLeaks ();
  }
}

static VOID
TestQueuedWriteFollowers (
  VOID
  )
{
  UINT8 Data[4][512];
  TEST_EVENT *Events[5];
  EFI_BLOCK_IO2_TOKEN Tokens[ARRAY_SIZE (Events)];

  SetupDevice ();
  mTraceCommands = TRUE;
  mStatusResponses[0] = TEST_SD_PRG;
  mStatusResponses[1] = TEST_SD_READY;
  mStatusResponseCount = 2;
  for (UINTN Index = 0; Index < ARRAY_SIZE (Events); Index++) {
    Events[Index] = CreateUserEvent ();
    Tokens[Index].Event = (EFI_EVENT)Events[Index];
    Tokens[Index].TransactionStatus = EFI_ABORTED;
  }

  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &Tokens[0], 512, Data[0]) == EFI_SUCCESS);
  CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 1, &Tokens[1], 512, Data[1]) == EFI_SUCCESS);
  CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 2, &Tokens[2], 512, Data[2]) == EFI_SUCCESS);
  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 3, &Tokens[3], 512, Data[3]) == EFI_SUCCESS);
  CHECK (mDevice->BlockIo2.FlushBlocksEx (&mDevice->BlockIo2, &Tokens[4]) == EFI_SUCCESS);
  TickOrderedIo ();
  CHECK (mCommandCount == 1);
  CompletePendingRead (0, EFI_SUCCESS);
  CHECK (Events[0]->Signals == 1);

  for (UINTN Index = 1; Index < 3; Index++) {
    TickOrderedIo ();
    CHECK (mPendingPacket->SdMmcCmdBlk->CommandArgument == Index);
    CompleteWriteTransfer (EFI_SUCCESS);
    for (UINTN Follower = Index; Follower < ARRAY_SIZE (Events); Follower++) {
      CHECK (Events[Follower]->Signals == 0);
    }
    TickOrderedIo ();
    CHECK (Events[Index]->Signals == 1);
  }

  TickOrderedIo ();
  CHECK (Events[3]->Signals == 1 && Events[4]->Signals == 0);
  TickOrderedIo ();
  CHECK (mArmedTimers == 0 && mCommandCount == 7);
  CheckCommand (0, SD_READ_SINGLE_BLOCK, 0, TRUE);
  CheckCommand (1, SD_WRITE_SINGLE_BLOCK, 1, TRUE);
  CheckCommand (2, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
  CheckCommand (3, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
  CheckCommand (4, SD_WRITE_SINGLE_BLOCK, 2, TRUE);
  CheckCommand (5, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
  CheckCommand (6, SD_READ_SINGLE_BLOCK, 3, FALSE);

  for (UINTN Index = 0; Index < ARRAY_SIZE (Events); Index++) {
    CHECK (Events[Index]->Signals == 1 && Tokens[Index].TransactionStatus == EFI_SUCCESS);
    FreePool (Events[Index]);
  }
  FinishAndCheckNoLeaks ();
}

static VOID
TestWriteTimeoutBlocksFollowingData (
  VOID
  )
{
  UINT8 Data[512];

  for (UINTN Queued = 0; Queued < 2; Queued++) {
    SetupDevice ();
    mDefaultStatusResponse = TEST_SD_PRG;
    mTraceCommands = TRUE;
    TEST_EVENT *WriteEvent = CreateUserEvent ();
    TEST_EVENT *ReadEvent = CreateUserEvent ();
    EFI_BLOCK_IO2_TOKEN WriteToken = { (EFI_EVENT)WriteEvent, EFI_ABORTED };
    EFI_BLOCK_IO2_TOKEN ReadToken = { (EFI_EVENT)ReadEvent, EFI_ABORTED };

    if (Queued) {
      CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, &WriteToken, 512, Data) == EFI_SUCCESS);
      CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 0, &ReadToken, 512, Data) == EFI_SUCCESS);
      TickOrderedIo ();
      CompleteWriteTransfer (EFI_SUCCESS);
      TickOrderedIo ();
      CHECK (WriteEvent->Signals == 1 && WriteToken.TransactionStatus == EFI_TIMEOUT);
      CHECK (ReadEvent->Signals == 0);
      TickOrderedIo ();
      CHECK (ReadEvent->Signals == 1 && ReadToken.TransactionStatus == EFI_TIMEOUT);
    } else {
      CHECK (mDevice->BlockIo.WriteBlocks (&mDevice->BlockIo, 7, 0, 512, Data) == EFI_TIMEOUT);
      CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, 512, Data) == EFI_TIMEOUT);
    }

    CHECK (mCommandCount == 201 && mReadTraceCount == 0);
    mDefaultStatusResponse = TEST_SD_READY;
    CHECK (mDevice->BlockIo.ReadBlocks (&mDevice->BlockIo, 7, 0, 512, Data) == EFI_SUCCESS);
    CHECK (mCommandCount == 203);
    CheckCommand (201, SD_SEND_STATUS, (UINT32)mReturnedRca << 16, FALSE);
    CheckCommand (202, SD_READ_SINGLE_BLOCK, 0, FALSE);
    FreePool (ReadEvent);
    FreePool (WriteEvent);
    FinishAndCheckNoLeaks ();
  }
}

static VOID
StopDuringWriteStatus (
  VOID
  )
{
  CHECK (StopCurrentChild () == EFI_NOT_READY);
  CHECK (mDeviceFrees == 0);
}

static VOID
TestWriteStopAndCancellation (
  VOID
  )
{
  enum { BeforeTransfer, DuringTransfer, DuringStatus, ProviderAbort, ProviderReject };
  UINT8 Data[512];

  for (UINTN Case = BeforeTransfer; Case <= ProviderReject; Case++) {
    SetupDevice ();
    TEST_EVENT *Event = CreateUserEvent ();
    EFI_BLOCK_IO2_TOKEN Token = { (EFI_EVENT)Event, EFI_ABORTED };
    BOOLEAN Submit = (BOOLEAN)(Case != BeforeTransfer);
    BOOLEAN Abort = (BOOLEAN)(Case == ProviderAbort);
    BOOLEAN Reject = (BOOLEAN)(Case == ProviderReject);
    if (Reject) {
      mFailCommandIndex = SD_WRITE_SINGLE_BLOCK;
      mFailCommandOccurrence = 1;
      mCommandFailureStatus = EFI_DEVICE_ERROR;
    }

    CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, &Token, 512, Data) == EFI_SUCCESS);
    if (Submit) {
      TickOrderedIo ();
    }

    if (Case <= DuringTransfer) {
      CHECK (StopCurrentChild () == EFI_NOT_READY && mDeviceFrees == 0);
      CHECK (Event->Signals == 0);
    }

    if (Case == DuringTransfer) {
      TickOrderedIo ();
      CHECK (mPendingPacket != NULL && Event->Signals == 0);
    }

    if (Case == DuringStatus) {
      mOnStatus = StopDuringWriteStatus;
    }

    if (Submit && !Reject) {
      CompleteWriteTransfer (Abort ? EFI_ABORTED : EFI_SUCCESS);
      CHECK (Event->Signals == 0);
    }

    if (!Reject) {
      TickOrderedIo ();
    }

    CHECK (mStatusCalls == ((Case == DuringStatus) ? 1U : 0U));
    CHECK (Event->Signals == 1);
    CHECK (Token.TransactionStatus == (Abort ? EFI_ABORTED : Reject ? EFI_DEVICE_ERROR : EFI_NO_MEDIA));
    CHECK (mArmedTimers == 0);
    FreePool (Event);
    FinishAndCheckNoLeaks ();
  }

  SetupDevice ();
  mOnStatus = StopDuringWriteStatus;
  CHECK (mDevice->BlockIo.WriteBlocks (&mDevice->BlockIo, 7, 0, 512, Data) == EFI_NO_MEDIA);
  CHECK (mStatusCalls == 1);
  FinishAndCheckNoLeaks ();
}

static VOID
TestWriteRemovalDrainsFollowers (
  VOID
  )
{
  UINT8 Data[2][512];

  SetupDevice ();
  TEST_EVENT *WriteEvent = CreateUserEvent ();
  TEST_EVENT *ReadEvent = CreateUserEvent ();
  EFI_BLOCK_IO2_TOKEN WriteToken = { (EFI_EVENT)WriteEvent, EFI_ABORTED };
  EFI_BLOCK_IO2_TOKEN ReadToken = { (EFI_EVENT)ReadEvent, EFI_ABORTED };

  CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, &WriteToken, 512, Data[0]) == EFI_SUCCESS);
  CHECK (mDevice->BlockIo2.ReadBlocksEx (&mDevice->BlockIo2, 7, 1, &ReadToken, 512, Data[1]) == EFI_SUCCESS);
  TickOrderedIo ();
  CompleteWriteTransfer (EFI_MEDIA_CHANGED);
  CheckTombstone (8);
  CHECK (WriteEvent->Signals == 0 && ReadEvent->Signals == 0);
  CHECK (StopCurrentChild () == EFI_NOT_READY);
  TickOrderedIo ();
  CHECK (WriteEvent->Signals == 1 && WriteToken.TransactionStatus == EFI_MEDIA_CHANGED);
  CHECK (ReadEvent->Signals == 0);
  TickOrderedIo ();
  CHECK (WriteEvent->Signals == 1 && ReadEvent->Signals == 1 && ReadToken.TransactionStatus == EFI_NO_MEDIA);
  CHECK (mPassThruCalls == 1 && mArmedTimers == 0);
  FreePool (ReadEvent);
  FreePool (WriteEvent);
  FinishAndCheckNoLeaks ();
}

static VOID
EFIAPI
StopOnOrderedWriteCompletion (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_BLOCK_IO2_TOKEN *Token = Context;

  CHECK (mCurrentTpl == TPL_CALLBACK);
  CHECK (Token->Event == Event && Token->TransactionStatus == EFI_SUCCESS);
  CHECK (mArmedTimers == 0 && IsListEmpty (&mDevice->OrderedIo));
  CHECK (StopCurrentChild () == EFI_SUCCESS);
  mDevice = NULL;
  FreePool (Token);
}

static VOID
TestQueuedWriteResources (
  VOID
  )
{
  UINT8 Data[512];

  for (UINTN Fail = 1; Fail <= 4; Fail++) {
    SetupDevice ();
    mPoisonFailedEvent = TRUE;
    TEST_EVENT *Event = CreateUserEvent ();
    EFI_BLOCK_IO2_TOKEN Token = { (EFI_EVENT)Event, EFI_ABORTED };
    EFI_STATUS Expected = EFI_OUT_OF_RESOURCES;
    if (Fail < 4) {
      mFailAllocationAttempt = mAllocationAttempt + Fail;
    } else {
      mSetTimerStatus = Expected = EFI_DEVICE_ERROR;
    }

    CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, &Token, 512, Data) == Expected);
    CHECK (Event->Signals == 0 && Token.TransactionStatus == EFI_ABORTED);
    CHECK (mArmedTimers == 0 && mPassThruCalls == 0);
    FreePool (Event);
    FinishAndCheckNoLeaks ();
  }

  SetupDevice ();
  mCompletePassThruSynchronously = TRUE;
  mDeferUserNotify = TRUE;
  TEST_EVENT *Event = CreateUserEvent ();
  EFI_BLOCK_IO2_TOKEN *Token = AllocateZeroPool (sizeof (*Token));
  CHECK (Token != NULL);
  Token->Event = (EFI_EVENT)Event;
  Event->Notify = StopOnOrderedWriteCompletion;
  Event->Context = Token;
  CHECK (mDevice->BlockIo2.WriteBlocksEx (&mDevice->BlockIo2, 7, 0, Token, 512, Data) == EFI_SUCCESS);
  TickOrderedIo ();
  CHECK (Event->Signals == 0 && mStatusCalls == 0);
  TickOrderedIo ();
  CHECK (Event->Signals == 1 && mStatusCalls == 1 && mDeviceFrees == 1);
  CHECK (mDeferredUserEvent == NULL && mDevice == NULL);
  mDeferUserNotify = FALSE;
  FreePool (Event);
  CHECK (mAllocations == 0);
}

int
main (
  void
  )
{
  TestBuildDevicePathEvidence ();
  TestTransportStatusInvalidation ();
  TestStopDuringPinnedProbe ();
  TestAllPublicIoMethodsRejectTombstone ();
  TestDiskInfoInquiryTombstone ();
  TestCidResponseContract ();
  TestAsyncStopDrainAndRetry ();
  TestImmediateAsyncCompletionMayFreeCallerToken ();
  TestLaterSlotDiscoveryFailureKeepsEarlierChildPrivate ();
  TestChildInstallThenOpenFailureRollback ();
  TestChildOpenAndUndoFailuresRetainOrphanForStop ();
  TestChildUndoFailureThenRelationshipRetry ();
  TestNoChildStopFailureRetryPreservesPrivate ();
  TestStopReentryDuringChildUninstall ();
  TestValidCsdGeometryBoundaries ();
  TestInvalidCsdRejectedBeforePublication ();
  TestAsyncBatchOwnership ();
  TestSynchronousProviderPolicy ();
  TestCompletionStopReentry ();
  TestSynchronousTokenOutcomes ();
  TestReadBufferAlignmentContract ();
  TestPartialReadErrorIdentity ();
  TestSynchronousReadChunkBoundaries ();
  TestEraseRangeBoundaries ();
  TestReadAddressBoundaries ();
  TestCsdCapacityFieldBoundaries ();
  TestTinyCsdDoesNotMarkMediaPresent ();
  TestReadOnlyPolicy ();
  TestWritePacketContract ();
  TestWriteStatus ();
  TestSplitWriteOrdering ();
  TestQueuedWriteFollowers ();
  TestWriteTimeoutBlocksFollowingData ();
  TestWriteStopAndCancellation ();
  TestWriteRemovalDrainsFollowers ();
  TestQueuedWriteResources ();
  printf ("SdDxeMediaTest: %u checks passed\n", mCheckCount);
  return 0;
}
