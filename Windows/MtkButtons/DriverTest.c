/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Execute production KMDF callbacks as ARM64 PE code with OS/WDF/MMIO/PMIC
 * boundaries mocked. This is not a Windows kernel or physical-button test.
 */
#include <ntddk.h>
#include <wdf.h>
#include "../MtkMali/PmicPrm.h"

static PVOID TestMapIoSpace(PHYSICAL_ADDRESS Address, SIZE_T Length, ULONG Protect);
static VOID TestUnmapIoSpace(PVOID Base, SIZE_T Length);
static USHORT TestReadRegisterUshort(volatile USHORT *Address);
static NTSTATUS TestGetDeviceInterfaces(LPGUID InterfaceClassGuid,
                                        PDEVICE_OBJECT PhysicalDeviceObject,
                                        ULONG Flags, PWSTR *SymbolicLinkList);
static VOID TestFreePool(PVOID Buffer);
static LONG TestInterlockedCompareExchange(volatile LONG *Target,
                                            LONG Exchange, LONG Comparand);
static LONG TestInterlockedExchange(volatile LONG *Target, LONG Value);
static MALI_RESULT TestPmicRead16(uint32_t Address, uint16_t *Value);
static MALI_RESULT TestPmicQueryKeyIrq(void);
static MALI_RESULT TestPmicAcquireKeyIrqLease(uint64_t *LeaseToken);
static MALI_RESULT TestPmicReleaseKeyIrqLease(uint64_t LeaseToken);
static MALI_RESULT TestPmicReadKeyIrqSnapshot(
  uint64_t LeaseToken, MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot);
static MALI_RESULT TestPmicAckKeyIrqSnapshot(
  const MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot);
static VOID TestZeroMemory(PVOID Buffer, SIZE_T Length);
static VOID TestCopyMemory(PVOID Destination, const VOID *Source, SIZE_T Length);

#undef MmMapIoSpaceEx
#undef MmUnmapIoSpace
#undef READ_REGISTER_USHORT
#undef IoGetDeviceInterfaces
#undef ExFreePool
#undef InterlockedCompareExchange
#undef InterlockedExchange
#undef RtlZeroMemory
#undef RtlCopyMemory
#define MmMapIoSpaceEx TestMapIoSpace
#define MmUnmapIoSpace TestUnmapIoSpace
#define READ_REGISTER_USHORT TestReadRegisterUshort
#define IoGetDeviceInterfaces TestGetDeviceInterfaces
#define ExFreePool TestFreePool
#define InterlockedCompareExchange TestInterlockedCompareExchange
#define InterlockedExchange TestInterlockedExchange
#define RtlZeroMemory(Destination, Length) TestZeroMemory((Destination), (Length))
#define RtlCopyMemory(Destination, Source, Length) TestCopyMemory((Destination), (Source), (Length))
#define MtkMaliPmicPrmRead16 TestPmicRead16
#define MtkMaliPmicPrmQueryKeyIrq TestPmicQueryKeyIrq
#define MtkMaliPmicPrmAcquireKeyIrqLease TestPmicAcquireKeyIrqLease
#define MtkMaliPmicPrmReleaseKeyIrqLease TestPmicReleaseKeyIrqLease
#define MtkMaliPmicPrmReadKeyIrqSnapshot TestPmicReadKeyIrqSnapshot
#define MtkMaliPmicPrmAckKeyIrqSnapshot TestPmicAckKeyIrqSnapshot

#include "Driver.c"

__declspec(dllexport) uint32_t MtkButtonsAssertionCount;

#define TEST_DEVICE ((WDFDEVICE)(ULONG_PTR)0x1001)
#define TEST_DRIVER ((WDFDRIVER)(ULONG_PTR)0x1002)
#define TEST_TARGET ((WDFIOTARGET)(ULONG_PTR)0x1003)
#define TEST_WORK ((WDFWORKITEM)(ULONG_PTR)0x1004)
#define TEST_TIMER ((WDFTIMER)(ULONG_PTR)0x1005)
#define TEST_INTERRUPT ((WDFINTERRUPT)(ULONG_PTR)0x1006)
#define TEST_RAW_LIST ((WDFCMRESLIST)(ULONG_PTR)0x1101)
#define TEST_TRANSLATED_LIST ((WDFCMRESLIST)(ULONG_PTR)0x1102)
#define TEST_INIT ((PWDFDEVICE_INIT)(ULONG_PTR)0x1201)
#define TEST_REG_BYTES 0x1000U
#define TEST_EXPECTED_READS 7U

static uint32_t mFailureLine;
static WDFFUNC mWdfTable[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions = mWdfTable;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;

static BUTTONS_DEVICE mDevice;
static UCHAR mRegisterBytes[TEST_REG_BYTES];
static CM_PARTIAL_RESOURCE_DESCRIPTOR mRawDescriptors[2];
static CM_PARTIAL_RESOURCE_DESCRIPTOR mTranslatedDescriptors[2];
static ULONG mRawCount, mTranslatedCount;
static WDF_INTERRUPT_CONFIG mInterruptConfig;
static WDF_INTERRUPT_INFO mInterruptInfo;
static NTSTATUS mInterruptCreateStatus;
static ULONG mInterruptCreateCalls, mInterruptInfoCalls;
static ULONG mInterruptEnableCalls, mInterruptDisableCalls, mInterruptDpcQueues;
static WDF_PNPPOWER_EVENT_CALLBACKS mPnpCallbacks;
static EVT_WDF_DRIVER_DEVICE_ADD *mDeviceAdd;
static WCHAR mNotifyList[128];
static ULONG mNotifyChars;
static ULONG mReadOffsets[256], mReadCount;
static ULONG mReadCalls, mPmicCalls, mMapCalls, mUnmapCalls;
static ULONG mInterfaceQueries, mInterfaceFrees, mTargetCreates, mTargetOpens;
static ULONG mTargetCloses, mWriteCalls, mWorkEnqueues, mTimerStarts;
static ULONG mTimerStops, mTimerWaitStops, mTimerNoWaitStops, mWorkFlushes;
static ULONG mPmicQueryCalls, mPmicLeaseAcquires, mPmicLeaseReleases;
static ULONG mPmicSnapshotReads, mPmicSnapshotAcks;
static ULONG mWrittenButtons[32];
static ULONG mWrittenBytes[32];
static NTSTATUS mDriverCreateStatus, mTargetCreateStatus, mTargetOpenStatus;
static NTSTATUS mInterfaceQueryStatus;
static NTSTATUS mWriteStatus;
static ULONG_PTR mWriteBytes;
static MALI_RESULT mPmicStatus;
static MALI_RESULT mPmicQueryStatus, mPmicLeaseAcquireStatus;
static MALI_RESULT mPmicSnapshotReadStatus, mPmicSnapshotAckStatus;
static MALI_RESULT mPmicLeaseReleaseStatus, mNextPmicSnapshotAckStatus;
static uint16_t mPmicValue;
static uint64_t mMockKeyIrqLeaseToken;
static MTK_PMIC_KEY_IRQ_SNAPSHOT mPmicSnapshot, mLastPmicSnapshotRead;
static ULONG mIoSequence, mLastToggleSequence, mLastSnapshotAckSequence;
static BOOLEAN mInterfaceAvailable, mMultipleInterfaces, mMapFailure;
static BOOLEAN mInjectTimerOnPmicRead, mInjectInterruptOnPmicRead;
static BOOLEAN mInjectTimerOnSnapshotRead, mInjectInterruptOnSnapshotRead;
static BOOLEAN mInjectD0ExitOnSnapshotRead, mUseNextPmicSnapshotAckStatus;
static BOOLEAN mInterruptCreateNoHandle, mInterruptObjectAlive;
static BOOLEAN mInjectD0ExitOnTargetWrite;

#define CHECK(Condition) do { \
  ++MtkButtonsAssertionCount; \
  if (!(Condition) && mFailureLine == 0) mFailureLine = (uint32_t)__LINE__; \
} while (0)

static VOID TestZeroMemory(PVOID Buffer, SIZE_T Length) {
  volatile UCHAR *Bytes = (volatile UCHAR *)Buffer;
  SIZE_T Index;
  for (Index = 0; Index < Length; ++Index) Bytes[Index] = 0;
}

static VOID TestCopyMemory(PVOID Destination, const VOID *Source, SIZE_T Length) {
  volatile UCHAR *Out = (volatile UCHAR *)Destination;
  const volatile UCHAR *In = (const volatile UCHAR *)Source;
  SIZE_T Index;
  for (Index = 0; Index < Length; ++Index) Out[Index] = In[Index];
}

static VOID TestCopyWide(PWSTR Destination, PCWSTR Source, ULONG Capacity) {
  ULONG Index = 0;
  while (Index + 1U < Capacity && Source[Index] != L'\0') {
    Destination[Index] = Source[Index];
    ++Index;
  }
  Destination[Index] = L'\0';
  Destination[Index + 1U] = L'\0';
}

static NTSTATUS TestDriverCreate(
  PWDF_DRIVER_GLOBALS Globals,
  PDRIVER_OBJECT DriverObject,
  PCUNICODE_STRING RegistryPath,
  PWDF_OBJECT_ATTRIBUTES Attributes,
  PWDF_DRIVER_CONFIG Config,
  WDFDRIVER *Driver
  ) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(DriverObject == (PDRIVER_OBJECT)(ULONG_PTR)0x1301);
  CHECK(RegistryPath == (PCUNICODE_STRING)(ULONG_PTR)0x1302);
  CHECK(Attributes == WDF_NO_OBJECT_ATTRIBUTES);
  CHECK(Config != NULL && Config->EvtDriverDeviceAdd == ButtonsDeviceAdd);
  if (Driver != NULL) *Driver = TEST_DRIVER;
  mDeviceAdd = Config != NULL ? Config->EvtDriverDeviceAdd : NULL;
  return mDriverCreateStatus;
}

static VOID TestSetPnpCallbacks(
  PWDF_DRIVER_GLOBALS Globals,
  PWDFDEVICE_INIT Init,
  PWDF_PNPPOWER_EVENT_CALLBACKS Callbacks
  ) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Init == TEST_INIT && Callbacks != NULL);
  if (Callbacks != NULL) mPnpCallbacks = *Callbacks;
}

static NTSTATUS TestDeviceCreate(
  PWDF_DRIVER_GLOBALS Globals,
  PWDFDEVICE_INIT *Init,
  PWDF_OBJECT_ATTRIBUTES Attributes,
  WDFDEVICE *Device
  ) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Init != NULL && *Init == TEST_INIT && Attributes != NULL);
  memset(&mDevice, 0, sizeof(mDevice));
  if (Init != NULL) *Init = NULL;
  if (Device != NULL) *Device = TEST_DEVICE;
  return STATUS_SUCCESS;
}

static PVOID TestGetContext(
  PWDF_DRIVER_GLOBALS Globals,
  WDFOBJECT Object,
  PCWDF_OBJECT_CONTEXT_TYPE_INFO TypeInfo
  ) {
  UNREFERENCED_PARAMETER(Globals);
  UNREFERENCED_PARAMETER(TypeInfo);
  CHECK(Object == TEST_DEVICE);
  return &mDevice;
}

static NTSTATUS TestTargetCreate(
  PWDF_DRIVER_GLOBALS Globals,
  WDFDEVICE Device,
  PWDF_OBJECT_ATTRIBUTES Attributes,
  WDFIOTARGET *Target
  ) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Device == TEST_DEVICE && Attributes != NULL &&
        Attributes->ParentObject == TEST_DEVICE);
  ++mTargetCreates;
  if (NT_SUCCESS(mTargetCreateStatus) && Target != NULL) *Target = TEST_TARGET;
  return mTargetCreateStatus;
}

static NTSTATUS TestWorkCreate(
  PWDF_DRIVER_GLOBALS Globals,
  PWDF_WORKITEM_CONFIG Config,
  PWDF_OBJECT_ATTRIBUTES Attributes,
  WDFWORKITEM *Work
  ) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Config != NULL && Config->EvtWorkItemFunc == ButtonsWork &&
        Config->AutomaticSerialization == FALSE);
  CHECK(Attributes != NULL && Attributes->ParentObject == TEST_DEVICE);
  if (Work != NULL) *Work = TEST_WORK;
  return STATUS_SUCCESS;
}

static NTSTATUS TestTimerCreate(
  PWDF_DRIVER_GLOBALS Globals,
  PWDF_TIMER_CONFIG Config,
  PWDF_OBJECT_ATTRIBUTES Attributes,
  WDFTIMER *Timer
  ) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Config != NULL && Config->EvtTimerFunc == ButtonsTimer &&
        Config->Period == BUTTONS_POLL_MS &&
        Config->AutomaticSerialization == FALSE);
  CHECK(Attributes != NULL && Attributes->ParentObject == TEST_DEVICE);
  if (Timer != NULL) *Timer = TEST_TIMER;
  return STATUS_SUCCESS;
}

static NTSTATUS TestInterruptCreate(
  PWDF_DRIVER_GLOBALS Globals,
  WDFDEVICE Device,
  PWDF_INTERRUPT_CONFIG Config,
  PWDF_OBJECT_ATTRIBUTES Attributes,
  WDFINTERRUPT *Interrupt
  ) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Device == TEST_DEVICE && Config != NULL && Attributes == NULL);
  CHECK(Config != NULL && !Config->PassiveHandling &&
        !Config->AutomaticSerialization && !Config->CanWakeDevice &&
        Config->EvtInterruptIsr == ButtonsInterruptIsr &&
        Config->EvtInterruptDpc == ButtonsInterruptDpc &&
        Config->EvtInterruptEnable == ButtonsInterruptEnable &&
        Config->EvtInterruptDisable == ButtonsInterruptDisable &&
        Config->EvtInterruptWorkItem == NULL);
  CHECK(Config != NULL && Config->InterruptRaw == &mRawDescriptors[1] &&
        Config->InterruptTranslated == &mTranslatedDescriptors[1]);
  CHECK(!mInterruptObjectAlive);
  ++mInterruptCreateCalls;
  if (NT_SUCCESS(mInterruptCreateStatus)) {
    if (Interrupt != NULL && !mInterruptCreateNoHandle) {
      *Interrupt = TEST_INTERRUPT;
      mInterruptObjectAlive = TRUE;
    }
    if (Config != NULL && Config->InterruptTranslated != NULL) {
      WDF_INTERRUPT_INFO_INIT(&mInterruptInfo);
      mInterruptInfo.Vector = Config->InterruptTranslated->u.Interrupt.Vector;
      mInterruptInfo.Mode = Latched;
      mInterruptInfo.Polarity = WdfInterruptActiveHigh;
      mInterruptInfo.ShareDisposition = CmResourceShareDeviceExclusive;
      mInterruptConfig = *Config;
    }
  }
  return mInterruptCreateStatus;
}

static VOID TestInterruptGetInfo(PWDF_DRIVER_GLOBALS Globals,
                                 WDFINTERRUPT Interrupt,
                                 PWDF_INTERRUPT_INFO Info) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(mInterruptObjectAlive && Interrupt == TEST_INTERRUPT && Info != NULL &&
        Info->Size == mInterruptInfo.Size);
  ++mInterruptInfoCalls;
  if (Info != NULL) {
    *Info = mInterruptInfo;
  }
}

static BOOLEAN TestQueueInterruptDpc(PWDF_DRIVER_GLOBALS Globals,
                                     WDFINTERRUPT Interrupt) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(mInterruptObjectAlive && Interrupt == TEST_INTERRUPT);
  ++mInterruptDpcQueues;
  return TRUE;
}

static WDFDEVICE TestInterruptGetDevice(PWDF_DRIVER_GLOBALS Globals,
                                        WDFINTERRUPT Interrupt) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(mInterruptObjectAlive && Interrupt == TEST_INTERRUPT);
  return TEST_DEVICE;
}

static VOID TestWdfInterruptEnable(PWDF_DRIVER_GLOBALS Globals,
                                  WDFINTERRUPT Interrupt) {
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Globals);
  CHECK(mInterruptObjectAlive && Interrupt == TEST_INTERRUPT &&
        mInterruptConfig.EvtInterruptEnable != NULL);
  ++mInterruptEnableCalls;
  Status = mInterruptConfig.EvtInterruptEnable(Interrupt, TEST_DEVICE);
  CHECK(Status == STATUS_SUCCESS);
}

static VOID TestWdfInterruptDisable(PWDF_DRIVER_GLOBALS Globals,
                                    WDFINTERRUPT Interrupt) {
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Globals);
  CHECK(mInterruptObjectAlive && Interrupt == TEST_INTERRUPT &&
        mInterruptConfig.EvtInterruptDisable != NULL);
  ++mInterruptDisableCalls;
  Status = mInterruptConfig.EvtInterruptDisable(Interrupt, TEST_DEVICE);
  CHECK(Status == STATUS_SUCCESS);
}

static WDFOBJECT TestWorkParent(PWDF_DRIVER_GLOBALS Globals, WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Work == TEST_WORK);
  return TEST_DEVICE;
}

static WDFOBJECT TestTimerParent(PWDF_DRIVER_GLOBALS Globals, WDFTIMER Timer) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Timer == TEST_TIMER);
  return TEST_DEVICE;
}

static ULONG TestResourceCount(PWDF_DRIVER_GLOBALS Globals, WDFCMRESLIST List) {
  UNREFERENCED_PARAMETER(Globals);
  if (List == TEST_RAW_LIST) return mRawCount;
  if (List == TEST_TRANSLATED_LIST) return mTranslatedCount;
  CHECK(FALSE);
  return 0;
}

static PCM_PARTIAL_RESOURCE_DESCRIPTOR TestResource(
  PWDF_DRIVER_GLOBALS Globals,
  WDFCMRESLIST List,
  ULONG Index
  ) {
  UNREFERENCED_PARAMETER(Globals);
  if (List == TEST_RAW_LIST && Index < mRawCount) return &mRawDescriptors[Index];
  if (List == TEST_TRANSLATED_LIST && Index < mTranslatedCount)
    return &mTranslatedDescriptors[Index];
  CHECK(FALSE);
  return NULL;
}

static VOID TestWorkEnqueue(PWDF_DRIVER_GLOBALS Globals, WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Work == TEST_WORK);
  ++mWorkEnqueues;
}

static VOID TestWorkFlush(PWDF_DRIVER_GLOBALS Globals, WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Work == TEST_WORK);
  ++mWorkFlushes;
}

static VOID TestTimerStart(PWDF_DRIVER_GLOBALS Globals, WDFTIMER Timer,
                           LARGE_INTEGER DueTime) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Timer == TEST_TIMER && DueTime.QuadPart < 0);
  ++mTimerStarts;
}

static BOOLEAN TestTimerStop(PWDF_DRIVER_GLOBALS Globals, WDFTIMER Timer,
                             BOOLEAN Wait) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Timer == TEST_TIMER);
  if (Wait != FALSE) ++mTimerWaitStops;
  else ++mTimerNoWaitStops;
  ++mTimerStops;
  return TRUE;
}

static NTSTATUS TestTargetOpen(PWDF_DRIVER_GLOBALS Globals, WDFIOTARGET Target,
                               PWDF_IO_TARGET_OPEN_PARAMS Open) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Target == TEST_TARGET && Open != NULL &&
        Open->Type == WdfIoTargetOpenByName &&
        Open->DesiredAccess == FILE_WRITE_DATA && Open->ShareAccess == 0 &&
        Open->FileAttributes == FILE_ATTRIBUTE_NORMAL);
  CHECK(Open->TargetDeviceName.Buffer != NULL &&
        Open->TargetDeviceName.Length != 0 &&
        Open->TargetDeviceName.Length == mNotifyChars * sizeof(WCHAR));
  ++mTargetOpens;
  return mTargetOpenStatus;
}

static VOID TestTargetClose(PWDF_DRIVER_GLOBALS Globals, WDFIOTARGET Target) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Target == TEST_TARGET);
  ++mTargetCloses;
}

static NTSTATUS TestTargetWrite(
  PWDF_DRIVER_GLOBALS Globals,
  WDFIOTARGET Target,
  WDFREQUEST Request,
  PWDF_MEMORY_DESCRIPTOR Buffer,
  PLONGLONG DeviceOffset,
  PWDF_REQUEST_SEND_OPTIONS Options,
  PULONG_PTR BytesWritten
  ) {
  ULONG Button = 0xffffffffU;
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Target == TEST_TARGET && Request == NULL && DeviceOffset == NULL &&
        Buffer != NULL && Buffer->Type == WdfMemoryDescriptorTypeBuffer &&
        Buffer->u.BufferType.Buffer != NULL &&
        Buffer->u.BufferType.Length == sizeof(ULONG));
  CHECK(Options != NULL &&
        (Options->Flags & WDF_REQUEST_SEND_OPTION_TIMEOUT) != 0);
  if (Buffer != NULL && Buffer->Type == WdfMemoryDescriptorTypeBuffer &&
      Buffer->u.BufferType.Buffer != NULL &&
      Buffer->u.BufferType.Length == sizeof(ULONG)) {
    RtlCopyMemory(&Button, Buffer->u.BufferType.Buffer, sizeof(Button));
  }
  if (BytesWritten != NULL) *BytesWritten = mWriteBytes;
  if (mWriteCalls < RTL_NUMBER_OF(mWrittenButtons)) {
    mWrittenButtons[mWriteCalls] = Button;
    mWrittenBytes[mWriteCalls] = BytesWritten != NULL ? (ULONG)*BytesWritten : 0;
  }
  ++mWriteCalls;
  mLastToggleSequence = ++mIoSequence;
  if (mInjectD0ExitOnTargetWrite) {
    mInjectD0ExitOnTargetWrite = FALSE;
    CHECK(ButtonsD0ExitPreInterruptsDisabled(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  }
  return mWriteStatus;
}

static PVOID TestMapIoSpace(PHYSICAL_ADDRESS Address, SIZE_T Length, ULONG Protect) {
  ++mMapCalls;
  CHECK(Address.QuadPart == BUTTONS_KPD_PHYSICAL &&
        Length == BUTTONS_KPD_LENGTH &&
        Protect == (PAGE_READONLY | PAGE_NOCACHE));
  return mMapFailure ? NULL : mRegisterBytes;
}

static VOID TestUnmapIoSpace(PVOID Base, SIZE_T Length) {
  CHECK(Base == mRegisterBytes && Length == BUTTONS_KPD_LENGTH);
  ++mUnmapCalls;
}

static USHORT TestReadRegisterUshort(volatile USHORT *Address) {
  ULONG_PTR Base = (ULONG_PTR)mRegisterBytes;
  ULONG_PTR Current = (ULONG_PTR)Address;
  ULONG Offset;
  CHECK(Current >= Base && Current + sizeof(USHORT) <= Base + sizeof(mRegisterBytes));
  if (Current < Base || Current + sizeof(USHORT) > Base + sizeof(mRegisterBytes)) return 0;
  Offset = (ULONG)(Current - Base);
  CHECK((Offset & 1U) == 0 && mReadCount < RTL_NUMBER_OF(mReadOffsets));
  if (mReadCount < RTL_NUMBER_OF(mReadOffsets)) mReadOffsets[mReadCount++] = Offset;
  ++mReadCalls;
  return *Address;
}

static NTSTATUS TestGetDeviceInterfaces(LPGUID Guid, PDEVICE_OBJECT Physical,
                                        ULONG Flags, PWSTR *List) {
  static const WCHAR One[] = L"\\??\\GPIOBUTTONS\\BTN0";
  static const WCHAR Two[] = L"\\??\\GPIOBUTTONS\\BTN1";
  UNREFERENCED_PARAMETER(Physical);
  CHECK(Guid != NULL && Guid->Data1 == 0x30ebfbf8U &&
        Guid->Data2 == 0xdf5fU && Guid->Data3 == 0x4d4dU);
  if (Guid != NULL) {
    static const UCHAR ExpectedData4[8] = {
      0x9f, 0xc5, 0xa2, 0x6c, 0x7f, 0xd1, 0xdf, 0x4a
    };
    ULONG Index;
    for (Index = 0; Index < RTL_NUMBER_OF(ExpectedData4); ++Index) {
      CHECK(Guid->Data4[Index] == ExpectedData4[Index]);
    }
  }
  CHECK(Flags == 0 && List != NULL);
  ++mInterfaceQueries;
  if (List == NULL) return STATUS_INVALID_PARAMETER;
  *List = NULL;
  if (mInterfaceQueryStatus != STATUS_SUCCESS) return mInterfaceQueryStatus;
  if (!mInterfaceAvailable) return STATUS_NOT_FOUND;
  RtlZeroMemory(mNotifyList, sizeof(mNotifyList));
  TestCopyWide(mNotifyList, One, RTL_NUMBER_OF(mNotifyList));
  mNotifyChars = 0;
  while (mNotifyChars < RTL_NUMBER_OF(One) && One[mNotifyChars] != L'\0') ++mNotifyChars;
  if (mMultipleInterfaces) {
    ULONG Position = mNotifyChars + 1U;
    ULONG Index = 0;
    while (Two[Index] != L'\0' && Position + Index + 1U < RTL_NUMBER_OF(mNotifyList)) {
      mNotifyList[Position + Index] = Two[Index];
      ++Index;
    }
    mNotifyList[Position + Index] = L'\0';
    mNotifyList[Position + Index + 1U] = L'\0';
  }
  *List = mNotifyList;
  return STATUS_SUCCESS;
}

static VOID TestFreePool(PVOID Buffer) {
  CHECK(Buffer == mNotifyList);
  ++mInterfaceFrees;
}

static LONG TestInterlockedCompareExchange(volatile LONG *Target,
                                           LONG Exchange, LONG Comparand) {
  LONG Old = *Target;
  if (Old == Comparand) *Target = Exchange;
  return Old;
}

static LONG TestInterlockedExchange(volatile LONG *Target, LONG Value) {
  LONG Old = *Target;
  *Target = Value;
  return Old;
}

static MALI_RESULT TestPmicRead16(uint32_t Address, uint16_t *Value) {
  CHECK(Address == 0x28U && Value != NULL);
  ++mPmicCalls;
  if (mPmicStatus == MaliOk && Value != NULL) *Value = mPmicValue;
  if (mInjectTimerOnPmicRead || mInjectInterruptOnPmicRead) {
    BOOLEAN InjectTimer = mInjectTimerOnPmicRead;
    BOOLEAN InjectInterrupt = mInjectInterruptOnPmicRead;
    mInjectTimerOnPmicRead = FALSE;
    mInjectInterruptOnPmicRead = FALSE;
    ((PUSHORT)mRegisterBytes)[0x04U / sizeof(USHORT)] = 0xfffeU;
    if (InjectInterrupt) {
      CHECK(ButtonsInterruptIsr(TEST_INTERRUPT, 0U));
      ButtonsInterruptDpc(TEST_INTERRUPT, TEST_DEVICE);
    }
    if (InjectTimer) ButtonsTimer(TEST_TIMER);
  }
  return mPmicStatus;
}

static MALI_RESULT TestPmicQueryKeyIrq(void) {
  ++mPmicQueryCalls;
  return mPmicQueryStatus;
}

static MALI_RESULT TestPmicAcquireKeyIrqLease(uint64_t *LeaseToken) {
  CHECK(LeaseToken != NULL);
  ++mPmicLeaseAcquires;
  if (mPmicLeaseAcquireStatus == MaliOk && LeaseToken != NULL) {
    *LeaseToken = mMockKeyIrqLeaseToken;
  }
  return mPmicLeaseAcquireStatus;
}

static MALI_RESULT TestPmicReleaseKeyIrqLease(uint64_t LeaseToken) {
  CHECK(LeaseToken == mMockKeyIrqLeaseToken);
  ++mPmicLeaseReleases;
  return mPmicLeaseReleaseStatus;
}

static MALI_RESULT TestPmicReadKeyIrqSnapshot(
  uint64_t LeaseToken,
  MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot
  ) {
  CHECK(LeaseToken == mMockKeyIrqLeaseToken && Snapshot != NULL);
  ++mPmicSnapshotReads;
  if (mPmicSnapshotReadStatus == MaliOk && Snapshot != NULL) {
    *Snapshot = mPmicSnapshot;
    mLastPmicSnapshotRead = mPmicSnapshot;
    ++mPmicSnapshot.Generation;
  }

  if (mInjectTimerOnSnapshotRead || mInjectInterruptOnSnapshotRead ||
      mInjectD0ExitOnSnapshotRead) {
    BOOLEAN InjectTimer = mInjectTimerOnSnapshotRead;
    BOOLEAN InjectInterrupt = mInjectInterruptOnSnapshotRead;
    BOOLEAN InjectD0Exit = mInjectD0ExitOnSnapshotRead;
    mInjectTimerOnSnapshotRead = FALSE;
    mInjectInterruptOnSnapshotRead = FALSE;
    mInjectD0ExitOnSnapshotRead = FALSE;
    ((PUSHORT)mRegisterBytes)[0x04U / sizeof(USHORT)] = 0xfffeU;
    if (InjectInterrupt) {
      CHECK(ButtonsInterruptIsr(TEST_INTERRUPT, 0U));
      ButtonsInterruptDpc(TEST_INTERRUPT, TEST_DEVICE);
    }
    if (InjectTimer) ButtonsTimer(TEST_TIMER);
    if (InjectD0Exit) {
      CHECK(ButtonsD0ExitPreInterruptsDisabled(
        TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    }
  }
  return mPmicSnapshotReadStatus;
}

static MALI_RESULT TestPmicAckKeyIrqSnapshot(
  const MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot
  ) {
  MALI_RESULT Status;
  CHECK(Snapshot != NULL && Snapshot->Generation == mLastPmicSnapshotRead.Generation &&
        Snapshot->GroupStatus == mLastPmicSnapshotRead.GroupStatus &&
        Snapshot->TopStatus == mLastPmicSnapshotRead.TopStatus);
  CHECK(mDevice.Core.ActionCount == 0);
  ++mPmicSnapshotAcks;
  mLastSnapshotAckSequence = ++mIoSequence;
  if (mUseNextPmicSnapshotAckStatus) {
    Status = mNextPmicSnapshotAckStatus;
    mUseNextPmicSnapshotAckStatus = FALSE;
  } else Status = mPmicSnapshotAckStatus;
  if (Status == MaliOk) {
    /* Only the captured causes are consumed; the next read owns a new token. */
    mPmicSnapshot.GroupStatus &= (uint16_t)~Snapshot->GroupStatus;
  }
  return Status;
}

static void TestInstallWdfFunctions(void) {
  RtlZeroMemory(mWdfTable, sizeof(mWdfTable));
  mWdfTable[WdfDriverCreateTableIndex] = (WDFFUNC)TestDriverCreate;
  mWdfTable[WdfDeviceInitSetPnpPowerEventCallbacksTableIndex] = (WDFFUNC)TestSetPnpCallbacks;
  mWdfTable[WdfDeviceCreateTableIndex] = (WDFFUNC)TestDeviceCreate;
  mWdfTable[WdfObjectGetTypedContextWorkerTableIndex] = (WDFFUNC)TestGetContext;
  mWdfTable[WdfIoTargetCreateTableIndex] = (WDFFUNC)TestTargetCreate;
  mWdfTable[WdfIoTargetOpenTableIndex] = (WDFFUNC)TestTargetOpen;
  mWdfTable[WdfIoTargetCloseTableIndex] = (WDFFUNC)TestTargetClose;
  mWdfTable[WdfIoTargetSendWriteSynchronouslyTableIndex] = (WDFFUNC)TestTargetWrite;
  mWdfTable[WdfWorkItemCreateTableIndex] = (WDFFUNC)TestWorkCreate;
  mWdfTable[WdfWorkItemGetParentObjectTableIndex] = (WDFFUNC)TestWorkParent;
  mWdfTable[WdfWorkItemEnqueueTableIndex] = (WDFFUNC)TestWorkEnqueue;
  mWdfTable[WdfWorkItemFlushTableIndex] = (WDFFUNC)TestWorkFlush;
  mWdfTable[WdfTimerCreateTableIndex] = (WDFFUNC)TestTimerCreate;
  mWdfTable[WdfTimerGetParentObjectTableIndex] = (WDFFUNC)TestTimerParent;
  mWdfTable[WdfTimerStartTableIndex] = (WDFFUNC)TestTimerStart;
  mWdfTable[WdfTimerStopTableIndex] = (WDFFUNC)TestTimerStop;
  mWdfTable[WdfCmResourceListGetCountTableIndex] = (WDFFUNC)TestResourceCount;
  mWdfTable[WdfCmResourceListGetDescriptorTableIndex] = (WDFFUNC)TestResource;
  mWdfTable[WdfInterruptCreateTableIndex] = (WDFFUNC)TestInterruptCreate;
  mWdfTable[WdfInterruptGetInfoTableIndex] = (WDFFUNC)TestInterruptGetInfo;
  mWdfTable[WdfInterruptQueueDpcForIsrTableIndex] = (WDFFUNC)TestQueueInterruptDpc;
  mWdfTable[WdfInterruptGetDeviceTableIndex] = (WDFFUNC)TestInterruptGetDevice;
  mWdfTable[WdfInterruptEnableTableIndex] = (WDFFUNC)TestWdfInterruptEnable;
  mWdfTable[WdfInterruptDisableTableIndex] = (WDFFUNC)TestWdfInterruptDisable;
}

static void TestReset(void) {
  RtlZeroMemory(&mDevice, sizeof(mDevice));
  RtlZeroMemory(mRegisterBytes, sizeof(mRegisterBytes));
  RtlZeroMemory(mRawDescriptors, sizeof(mRawDescriptors));
  RtlZeroMemory(mTranslatedDescriptors, sizeof(mTranslatedDescriptors));
  RtlZeroMemory(&mInterruptConfig, sizeof(mInterruptConfig));
  RtlZeroMemory(&mInterruptInfo, sizeof(mInterruptInfo));
  RtlZeroMemory(&mPnpCallbacks, sizeof(mPnpCallbacks));
  RtlZeroMemory(mNotifyList, sizeof(mNotifyList));
  RtlZeroMemory(mReadOffsets, sizeof(mReadOffsets));
  RtlZeroMemory(mWrittenButtons, sizeof(mWrittenButtons));
  RtlZeroMemory(mWrittenBytes, sizeof(mWrittenBytes));
  mDeviceAdd = NULL;
  mRawCount = mTranslatedCount = 1;
  mReadCount = mReadCalls = mPmicCalls = mMapCalls = mUnmapCalls = 0;
  mInterfaceQueries = mInterfaceFrees = mTargetCreates = mTargetOpens = 0;
  mTargetCloses = mWriteCalls = mWorkEnqueues = mTimerStarts = 0;
  mTimerStops = mTimerWaitStops = mTimerNoWaitStops = mWorkFlushes = 0;
  mInterruptCreateCalls = mInterruptInfoCalls = 0;
  mInterruptEnableCalls = mInterruptDisableCalls = mInterruptDpcQueues = 0;
  mNotifyChars = 0;
  mDriverCreateStatus = mTargetCreateStatus = mTargetOpenStatus = STATUS_SUCCESS;
  mInterfaceQueryStatus = STATUS_SUCCESS;
  mWriteStatus = STATUS_SUCCESS;
  mInterruptCreateStatus = STATUS_SUCCESS;
  mWriteBytes = sizeof(ULONG);
  mPmicStatus = MaliOk;
  mPmicValue = 0xffffU;
  mPmicQueryCalls = mPmicLeaseAcquires = mPmicLeaseReleases = 0;
  mPmicSnapshotReads = mPmicSnapshotAcks = 0;
  mPmicQueryStatus = MaliUnsupported; /* Keep the original v1 cases explicit. */
  mPmicLeaseAcquireStatus = mPmicLeaseReleaseStatus = MaliOk;
  mPmicSnapshotReadStatus = mPmicSnapshotAckStatus = MaliOk;
  mNextPmicSnapshotAckStatus = MaliOk;
  mUseNextPmicSnapshotAckStatus = FALSE;
  mMockKeyIrqLeaseToken = 1;
  RtlZeroMemory(&mPmicSnapshot, sizeof(mPmicSnapshot));
  RtlZeroMemory(&mLastPmicSnapshotRead, sizeof(mLastPmicSnapshotRead));
  mPmicSnapshot.Generation = 2;
  mPmicSnapshot.TopStatus = 0xffffU;
  mIoSequence = mLastToggleSequence = mLastSnapshotAckSequence = 0;
  mInjectTimerOnSnapshotRead = mInjectInterruptOnSnapshotRead = FALSE;
  mInjectD0ExitOnSnapshotRead = FALSE;
  mInterfaceAvailable = FALSE;
  mMultipleInterfaces = FALSE;
  mInjectTimerOnPmicRead = FALSE;
  mInjectInterruptOnPmicRead = FALSE;
  mInterruptCreateNoHandle = FALSE;
  mInterruptObjectAlive = FALSE;
  mInjectD0ExitOnTargetWrite = FALSE;
  mMapFailure = FALSE;
  mRawDescriptors[0].Type = CmResourceTypeMemory;
  mRawDescriptors[0].Flags = CM_RESOURCE_MEMORY_READ_ONLY;
  mRawDescriptors[0].u.Memory.Start.QuadPart = BUTTONS_KPD_PHYSICAL;
  mRawDescriptors[0].u.Memory.Length = BUTTONS_KPD_LENGTH;
  mTranslatedDescriptors[0] = mRawDescriptors[0];
  mWdfTable[WdfDriverCreateTableIndex] = (WDFFUNC)TestDriverCreate;
}

static NTSTATUS TestFrameworkReleaseHardware(void) {
  WDFINTERRUPT OldInterrupt = mDevice.Interrupt;
  NTSTATUS Status = ButtonsReleaseHardware(TEST_DEVICE, TEST_RAW_LIST);
  if (OldInterrupt != NULL) {
    CHECK(mInterruptObjectAlive && OldInterrupt == TEST_INTERRUPT);
    mInterruptObjectAlive = FALSE;
  }
  return Status;
}

static NTSTATUS TestCreateDevice(void) {
  NTSTATUS Status;
  Status = DriverEntry((PDRIVER_OBJECT)(ULONG_PTR)0x1301,
                       (PUNICODE_STRING)(ULONG_PTR)0x1302);
  CHECK(Status == STATUS_SUCCESS && mDeviceAdd == ButtonsDeviceAdd);
  if (!NT_SUCCESS(Status) || mDeviceAdd == NULL) return STATUS_UNSUCCESSFUL;
  Status = mDeviceAdd(TEST_DRIVER, TEST_INIT);
  CHECK(Status == STATUS_SUCCESS && mDevice.Device == TEST_DEVICE &&
        mDevice.NotifyTarget == TEST_TARGET && mDevice.Work == TEST_WORK &&
        mDevice.Timer == TEST_TIMER && mTargetCreates == 1);
  CHECK(mPnpCallbacks.EvtDevicePrepareHardware == ButtonsPrepareHardware &&
        mPnpCallbacks.EvtDeviceReleaseHardware == ButtonsReleaseHardware &&
        mPnpCallbacks.EvtDeviceD0Entry == ButtonsD0Entry &&
        mPnpCallbacks.EvtDeviceD0ExitPreInterruptsDisabled ==
          ButtonsD0ExitPreInterruptsDisabled &&
        mPnpCallbacks.EvtDeviceD0Exit == ButtonsD0Exit);
  return Status;
}

static NTSTATUS TestPrepare(void) {
  NTSTATUS Status = ButtonsPrepareHardware(
    TEST_DEVICE, TEST_RAW_LIST, TEST_TRANSLATED_LIST);
  if (Status == STATUS_SUCCESS && mDevice.Interrupt != NULL && mRawCount > 1U) {
    mInterruptInfo.Vector = mTranslatedDescriptors[1].u.Interrupt.Vector;
  }
  return Status;
}

static void SetKpd(USHORT StatusWord, USHORT Debounce) {
  ((PUSHORT)mRegisterBytes)[0x24U / sizeof(USHORT)] = 1U;
  ((PUSHORT)mRegisterBytes)[0x18U / sizeof(USHORT)] = Debounce;
  ((PUSHORT)mRegisterBytes)[0x04U / sizeof(USHORT)] = StatusWord;
}

static void RunWorker(void) {
  if (mDevice.WorkQueued == 0) ButtonsTimer(TEST_TIMER);
  CHECK(mDevice.Active == 1 && mDevice.WorkQueued == 1);
  ButtonsWork(TEST_WORK);
  CHECK(mDevice.WorkQueued == 0);
}

static void CheckReadTriples(ULONG Start, ULONG Samples) {
  static const ULONG ExpectedOffsets[TEST_EXPECTED_READS] = {
    0x24U, 0x18U, 0x04U, 0x08U, 0x0cU, 0x10U, 0x14U
  };
  ULONG I;
  CHECK(mReadCount == Start + Samples * TEST_EXPECTED_READS);
  for (I = 0; I < Samples && Start + (I + 1U) * TEST_EXPECTED_READS <= mReadCount; ++I) {
    ULONG J, Offset = Start + I * TEST_EXPECTED_READS;
    for (J = 0; J < TEST_EXPECTED_READS; ++J)
      CHECK(mReadOffsets[Offset + J] == ExpectedOffsets[J]);
  }
}

static void TestResourcesAndPolling(void) {
  NTSTATUS Status;
  ULONG Reads, Enqueues;

  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS);

  mRawCount = mTranslatedCount = 2;
  CHECK(TestPrepare() == STATUS_DEVICE_CONFIGURATION_ERROR && mMapCalls == 0);
  mRawCount = mTranslatedCount = 1;
  mRawDescriptors[0].u.Memory.Start.QuadPart++;
  CHECK(TestPrepare() == STATUS_DEVICE_CONFIGURATION_ERROR && mMapCalls == 0);
  mRawDescriptors[0].u.Memory.Start.QuadPart = BUTTONS_KPD_PHYSICAL;
  mTranslatedDescriptors[0].Flags = 0;
  CHECK(TestPrepare() == STATUS_DEVICE_CONFIGURATION_ERROR && mMapCalls == 0);
  mTranslatedDescriptors[0] = mRawDescriptors[0];
  mRawDescriptors[0].u.Memory.Length--;
  CHECK(TestPrepare() == STATUS_DEVICE_CONFIGURATION_ERROR && mMapCalls == 0);
  mRawDescriptors[0].u.Memory.Length = BUTTONS_KPD_LENGTH;
  mMapFailure = TRUE;
  CHECK(TestPrepare() == STATUS_INSUFFICIENT_RESOURCES && mMapCalls == 1 &&
        mDevice.Registers == NULL && mUnmapCalls == 0);
  mMapFailure = FALSE;
  CHECK(TestPrepare() == STATUS_SUCCESS && mDevice.Prepared && mMapCalls == 2);

  SetKpd(0xffffU, 1024U);
  Status = ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3);
  CHECK(Status == STATUS_SUCCESS && mDevice.Active == 1 &&
        mWorkEnqueues == 1 && mTimerStarts == 1);
  RunWorker();
  CHECK(mDevice.Core.BaselineValid == 1 && mDevice.Core.ActionCount == 0 &&
        mPmicCalls == 1 && mWriteCalls == 0 && !mDevice.NotifyOpen);

  /* A transition can wait in the bounded core queue until the in-box array appears. */
  SetKpd(0xfffeU, 1024U);
  RunWorker();
  CHECK(mDevice.Core.ActionCount == 1 && mDevice.Core.PhysicalPressed ==
        MTK_BUTTON_STATE_VOLUME_UP && mWriteCalls == 0);

  mInterfaceAvailable = TRUE;
  mPmicStatus = MaliBusy;
  RunWorker();
  CHECK(mDevice.NotifyOpen && mTargetOpens == 1 && mDevice.Core.ActionCount == 1 &&
        mWriteCalls == 0);
  mPmicStatus = MaliOk;
  RunWorker();
  CHECK(mDevice.Core.ActionCount == 0 && mDevice.Core.DeliveredPressed ==
        MTK_BUTTON_STATE_VOLUME_UP && mWriteCalls == 1 &&
        mWrittenButtons[0] == (ULONG)MtkButtonVolumeUp &&
        mWrittenBytes[0] == sizeof(ULONG));

  /* All five status words are read, but bad debounce blocks PMIC and commits. */
  Reads = mReadCount;
  SetKpd(0xfffcU, 1023U);
  {
    ULONG PmicBefore = mPmicCalls;
    RunWorker();
    CHECK(mPmicCalls == PmicBefore && mWriteCalls == 1 &&
          mDevice.Core.PhysicalPressed == MTK_BUTTON_STATE_VOLUME_UP);
  }
  CheckReadTriples(Reads, 1);

  /* Simultaneous power/down press and release use the documented enum IDs. */
  SetKpd(0xfffcU, 1024U);
  mPmicValue = 0xfffdU;
  RunWorker();
  CHECK(mWriteCalls == 3 && mWrittenButtons[1] == (ULONG)MtkButtonPower &&
        mWrittenButtons[2] == (ULONG)MtkButtonVolumeDown);
  SetKpd(0xffffU, 1024U);
  mPmicValue = 0xffffU;
  RunWorker();
  CHECK(mWriteCalls == 6 && mWrittenButtons[3] == (ULONG)MtkButtonVolumeDown &&
        mWrittenButtons[4] == (ULONG)MtkButtonVolumeUp &&
        mWrittenButtons[5] == (ULONG)MtkButtonPower &&
        mDevice.Core.DeliveredPressed == 0);

  /* DPC timer only queues passive work, once; D0 exit sends an all-up toggle. */
  Enqueues = mWorkEnqueues;
  ButtonsTimer(TEST_TIMER);
  CHECK(mWorkEnqueues == Enqueues + 1U && mDevice.WorkQueued == 1);
  ButtonsTimer(TEST_TIMER);
  CHECK(mWorkEnqueues == Enqueues + 1U);
  RunWorker();
  SetKpd(0xfffeU, 1024U);
  RunWorker();
  CHECK(mWriteCalls == 7 && mDevice.Core.DeliveredPressed == MTK_BUTTON_STATE_VOLUME_UP);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  CHECK(mWriteCalls == 8 && mWrittenButtons[7] == (ULONG)MtkButtonVolumeUp &&
        mDevice.Core.DeliveredPressed == 0 && mDevice.Core.ActionCount == 0 &&
        mDevice.Core.BaselineValid == 0 && !mDevice.Active &&
        !mDevice.NotifyOpen && mTargetCloses == 1 &&
        mTimerStops == 1 && mTimerWaitStops == 1 && mWorkFlushes == 1);

  /* A different key held while D0 is off is seeded, never synthesized. */
  SetKpd(0xfffdU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  RunWorker();
  CHECK(mWriteCalls == 8 && mDevice.Core.BaselineValid == 1 &&
        mDevice.Core.SuppressUntilRelease == MTK_BUTTON_STATE_VOLUME_DOWN &&
        mDevice.Core.ActionCount == 0);
  SetKpd(0xffffU, 1024U);
  RunWorker();
  CHECK(mWriteCalls == 8 && mDevice.Core.SuppressUntilRelease == 0);
  SetKpd(0xfffdU, 1024U);
  RunWorker();
  CHECK(mWriteCalls == 9 && mWrittenButtons[8] == (ULONG)MtkButtonVolumeDown);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mWriteCalls == 10 && mWrittenButtons[9] == (ULONG)MtkButtonVolumeDown &&
        mDevice.Core.BaselineValid == 0 && mDevice.Core.DeliveredPressed == 0);

  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS &&
        mUnmapCalls == 1 && mDevice.Registers == NULL && !mDevice.Prepared);
  CHECK(mReadCalls != 0 && mWriteCalls == 10 && mMapCalls == 2);
}

static void TestD0ExitRetryAndAmbiguousWrite(void) {
  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS);
  CHECK(TestPrepare() == STATUS_SUCCESS);
  SetKpd(0xffffU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  RunWorker();
  SetKpd(0xfffeU, 1024U);
  RunWorker();
  CHECK(mDevice.Core.ActionCount == 1 && mDevice.Core.DeliveredPressed == 0 &&
        mWriteCalls == 0);

  /* D0 exit cannot release/unmap while a notification is waiting for its target. */
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_NOT_READY &&
        mDevice.Core.ActionCount == 1 && mUnmapCalls == 0 && mDevice.Registers != NULL);
  mInterfaceAvailable = TRUE;
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mDevice.Core.ActionCount == 0 && mDevice.Core.DeliveredPressed == 0 &&
        mWriteCalls == 2 && mWrittenButtons[0] == (ULONG)MtkButtonVolumeUp &&
        mWrittenButtons[1] == (ULONG)MtkButtonVolumeUp && mTargetCloses == 1);
  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS &&
        mUnmapCalls == 1);

  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS && TestPrepare() == STATUS_SUCCESS);
  mInterfaceAvailable = TRUE;
  SetKpd(0xffffU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  RunWorker();
  SetKpd(0xfffdU, 1024U);
  mWriteStatus = STATUS_TIMEOUT;
  mWriteBytes = sizeof(ULONG);
  RunWorker();
  CHECK(mWriteCalls == 1 && mDevice.Quarantined == 1 &&
        mDevice.Core.Quarantined == 1 && mDevice.Core.ActionCount == 1 &&
        mDevice.Core.DeliveredPressed == 0);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_HARDWARE_ERROR);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_HARDWARE_ERROR);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_HARDWARE_ERROR &&
        mWriteCalls == 1 && mUnmapCalls == 1 && mDevice.Registers == NULL &&
        !mDevice.Prepared && mTargetCloses == 1);

  /* A success status with a short byte count is also an uncertain toggle. */
  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS && TestPrepare() == STATUS_SUCCESS);
  mInterfaceAvailable = TRUE;
  SetKpd(0xffffU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  RunWorker();
  SetKpd(0xfffeU, 1024U);
  mWriteStatus = STATUS_SUCCESS;
  mWriteBytes = sizeof(ULONG) - 1U;
  RunWorker();
  CHECK(mWriteCalls == 1 && mDevice.Quarantined == 1 &&
        mDevice.Core.Quarantined == 1 && mDevice.Core.ActionCount == 1 &&
        mWrittenBytes[0] == sizeof(ULONG) - 1U);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_HARDWARE_ERROR &&
        mWriteCalls == 1 && mUnmapCalls == 0);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_HARDWARE_ERROR &&
        mWriteCalls == 1 && mUnmapCalls == 1 && mDevice.Registers == NULL &&
        !mDevice.Prepared && mTargetCloses == 1);
}

static void TestDuplicateNotifyInterfaceFailsClosed(void) {
  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS && TestPrepare() == STATUS_SUCCESS);
  mInterfaceAvailable = TRUE;
  mMultipleInterfaces = TRUE;
  SetKpd(0xffffU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  RunWorker();
  CHECK(mDevice.Quarantined == 1 && mDevice.Active == 0 && mPmicCalls == 0 &&
        mTargetOpens == 0 && mWriteCalls == 0 && mInterfaceFrees == 1 &&
        mTimerNoWaitStops == 1);
}

static void TestInformationalQueryAndOpenAreNotAccepted(void) {
  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS && TestPrepare() == STATUS_SUCCESS);
  mInterfaceAvailable = TRUE;
  mInterfaceQueryStatus = STATUS_TIMEOUT;
  SetKpd(0xffffU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  RunWorker();
  SetKpd(0xfffeU, 1024U);
  RunWorker();
  CHECK(mDevice.Core.ActionCount == 1 && !mDevice.NotifyOpen &&
        mTargetOpens == 0 && mWriteCalls == 0);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_TIMEOUT &&
        mDevice.Core.ActionCount == 1 && mUnmapCalls == 0 && mWriteCalls == 0);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_BUSY &&
        mDevice.Core.ActionCount == 1 && mWriteCalls == 0 && mUnmapCalls == 1 &&
        mDevice.Registers == NULL && !mDevice.Prepared);

  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS && TestPrepare() == STATUS_SUCCESS);
  mInterfaceAvailable = TRUE;
  mTargetOpenStatus = STATUS_TIMEOUT;
  SetKpd(0xffffU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  RunWorker();
  SetKpd(0xfffeU, 1024U);
  RunWorker();
  CHECK(mDevice.Core.ActionCount == 1 && !mDevice.NotifyOpen &&
        mTargetOpens == 3 && mTargetCloses == 3 && mWriteCalls == 0);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_TIMEOUT &&
        mDevice.Core.ActionCount == 1 && mUnmapCalls == 0 && mWriteCalls == 0);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_BUSY &&
        mDevice.Core.ActionCount == 1 && mWriteCalls == 0 && mUnmapCalls == 1 &&
        mDevice.Registers == NULL && !mDevice.Prepared);
}

static void TestInterruptResourceAndD0Rundown(void) {
  ULONG Reads, Enqueues, PmicBefore;

  TestReset();
  TestInstallWdfFunctions();
  CHECK(TestCreateDevice() == STATUS_SUCCESS);
  mRawCount = mTranslatedCount = 2;
  mRawDescriptors[1].Type = CmResourceTypeInterrupt;
  mRawDescriptors[1].Flags = CM_RESOURCE_INTERRUPT_LATCHED;
  mRawDescriptors[1].ShareDisposition = CmResourceShareDeviceExclusive;
  mRawDescriptors[1].u.Interrupt.Vector = 137U;
  mRawDescriptors[1].u.Interrupt.Level = 0x9876U;
  mTranslatedDescriptors[1].Type = CmResourceTypeInterrupt;
  mTranslatedDescriptors[1].Flags = CM_RESOURCE_INTERRUPT_LATCHED;
  mTranslatedDescriptors[1].ShareDisposition = CmResourceShareDeviceExclusive;
  mTranslatedDescriptors[1].u.Interrupt.Vector = 0x56U;
  mTranslatedDescriptors[1].u.Interrupt.Level = 0x78U;
  CHECK(TestPrepare() == STATUS_DEVICE_CONFIGURATION_ERROR &&
        mMapCalls == 0 && mInterruptCreateCalls == 0);

  /* Resource setup failures release the mapping and can be retried cleanly. */
  mRawDescriptors[1].u.Interrupt.Vector = 138U;
  mInterruptCreateStatus = STATUS_INSUFFICIENT_RESOURCES;
  CHECK(TestPrepare() == STATUS_INSUFFICIENT_RESOURCES && mMapCalls == 1 &&
        mUnmapCalls == 1 && mDevice.Registers == NULL &&
        mDevice.Interrupt == NULL && !mDevice.Prepared);
  mInterruptCreateStatus = STATUS_SUCCESS;
  mInterruptCreateNoHandle = TRUE;
  CHECK(TestPrepare() == STATUS_DEVICE_CONFIGURATION_ERROR && mMapCalls == 2 &&
        mUnmapCalls == 2 && mDevice.Registers == NULL &&
        mDevice.Interrupt == NULL && !mDevice.Prepared);
  mInterruptCreateNoHandle = FALSE;

  /* Raw GSI is 138; translated vector and level are passed through as assigned. */
  CHECK(TestPrepare() == STATUS_SUCCESS && mDevice.Interrupt == TEST_INTERRUPT &&
        mDevice.Prepared && mMapCalls == 3 && mInterruptCreateCalls == 3 &&
        mInterruptInfoCalls == 0 &&
        mDevice.TranslatedInterruptVector == 0x56U &&
        mDevice.InterruptResourcePresent);
  CHECK(mInterruptConfig.InterruptRaw == &mRawDescriptors[1] &&
        mInterruptConfig.InterruptTranslated == &mTranslatedDescriptors[1] &&
        mInterruptConfig.InterruptTranslated->u.Interrupt.Vector == 0x56U &&
        mInterruptConfig.InterruptTranslated->u.Interrupt.Level == 0x78U &&
        !mInterruptConfig.CanWakeDevice);

  mInterfaceAvailable = TRUE;
  SetKpd(0xffffU, 1024U);
  Reads = mReadCalls;
  Enqueues = mWorkEnqueues;
  {
    ULONG PmicBefore = mPmicCalls;
    ULONG TimerBefore = mTimerStarts;
    ULONG WriteBefore = mWriteCalls;
    mInterruptInfo.Vector = 0x57U;
    CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) ==
          STATUS_DEVICE_CONFIGURATION_ERROR &&
          mInterruptInfoCalls == 1 && mReadCalls == Reads &&
          mPmicCalls == PmicBefore && mWriteCalls == WriteBefore &&
          mWorkEnqueues == Enqueues && mTimerStarts == TimerBefore &&
          !mDevice.Active && !mDevice.WorkQueued);
    mInterruptInfo.Vector = 0x56U;
    mInterruptInfo.Mode = LevelSensitive;
    CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) ==
          STATUS_DEVICE_CONFIGURATION_ERROR &&
          mInterruptInfoCalls == 2 && mReadCalls == Reads &&
          mPmicCalls == PmicBefore && mWriteCalls == WriteBefore &&
          mWorkEnqueues == Enqueues && mTimerStarts == TimerBefore &&
          !mDevice.Active && !mDevice.WorkQueued);
    mInterruptInfo.Mode = Latched;
    mInterruptInfo.Polarity = WdfInterruptActiveLow;
    CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) ==
          STATUS_DEVICE_CONFIGURATION_ERROR &&
          mInterruptInfoCalls == 3 && mReadCalls == Reads &&
          mPmicCalls == PmicBefore && mWriteCalls == WriteBefore &&
          mWorkEnqueues == Enqueues && mTimerStarts == TimerBefore &&
          !mDevice.Active && !mDevice.WorkQueued);
    mInterruptInfo.Polarity = WdfInterruptActiveHigh;
    mInterruptInfo.ShareDisposition = CmResourceShareShared;
    CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) ==
          STATUS_DEVICE_CONFIGURATION_ERROR &&
          mInterruptInfoCalls == 4 && mReadCalls == Reads &&
          mPmicCalls == PmicBefore && mWriteCalls == WriteBefore &&
          mWorkEnqueues == Enqueues && mTimerStarts == TimerBefore &&
          !mDevice.Active && !mDevice.WorkQueued);
    mInterruptInfo.ShareDisposition = CmResourceShareDeviceExclusive;
  }
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mInterruptInfoCalls == 5);
  /* WDF enables the interrupt after EvtDeviceD0Entry returns. */
  TestWdfInterruptEnable(NULL, TEST_INTERRUPT);
  CHECK(mInterruptEnableCalls == 1 && mInterruptDisableCalls == 0);
  RunWorker();
  CHECK(mDevice.Core.BaselineValid == 1 && mPmicCalls == 1 &&
        mReadCalls == TEST_EXPECTED_READS);

  /* ISR only queues a DPC; MMIO and PMIC sampling happen in PASSIVE work. */
  Reads = mReadCalls;
  Enqueues = mWorkEnqueues;
  SetKpd(0xfffeU, 1024U);
  CHECK(ButtonsInterruptIsr(TEST_INTERRUPT, 0U) &&
        mReadCalls == Reads && mPmicCalls == 1 && mInterruptDpcQueues == 1);
  ButtonsInterruptDpc(TEST_INTERRUPT, TEST_DEVICE);
  CHECK(mDevice.WorkQueued == 1 && mWorkEnqueues == Enqueues + 1U);
  ButtonsTimer(TEST_TIMER);
  CHECK(mWorkEnqueues == Enqueues + 1U);
  ButtonsWork(TEST_WORK);
  CheckReadTriples(Reads, 1);
  CHECK(mDevice.Core.DeliveredPressed == MTK_BUTTON_STATE_VOLUME_UP &&
        mWriteCalls == 1 && mWrittenButtons[0] == (ULONG)MtkButtonVolumeUp &&
        mInterruptEnableCalls == 1 && mInterruptDisableCalls == 0);

  /* IRQ and timer during PMIC I/O coalesce into one more complete sample. */
  Reads = mReadCount;
  Enqueues = mWorkEnqueues;
  {
    ULONG PmicBefore = mPmicCalls;
    ULONG DpcBefore = mInterruptDpcQueues;
    SetKpd(0xffffU, 1024U);
    mInjectTimerOnPmicRead = TRUE;
    mInjectInterruptOnPmicRead = TRUE;
    ButtonsTimer(TEST_TIMER);
    ButtonsWork(TEST_WORK);
    CheckReadTriples(Reads, 2);
    CHECK(mPmicCalls == PmicBefore + 2U &&
          mInterruptDpcQueues == DpcBefore + 1U &&
          mWorkEnqueues == Enqueues + 1U &&
          mDevice.Core.DeliveredPressed == MTK_BUTTON_STATE_VOLUME_UP &&
          mWriteCalls == 3 &&
          mWrittenButtons[1] == (ULONG)MtkButtonVolumeUp &&
          mWrittenButtons[2] == (ULONG)MtkButtonVolumeUp &&
          mInterruptEnableCalls == 1 && mInterruptDisableCalls == 0);
  }

  /* A DPC already queued at D0 exit cannot enqueue MMIO work after rundown. */
  Reads = mReadCalls;
  Enqueues = mWorkEnqueues;
  CHECK(ButtonsInterruptIsr(TEST_INTERRUPT, 0U) && mReadCalls == Reads);
  CHECK(ButtonsD0ExitPreInterruptsDisabled(TEST_DEVICE, WdfPowerDeviceD3) ==
        STATUS_SUCCESS);
  TestWdfInterruptDisable(NULL, TEST_INTERRUPT);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mDevice.Core.DeliveredPressed == 0 && mDevice.Active == 0 &&
        mInterruptEnableCalls == 1 && mInterruptDisableCalls == 1);
  Reads = mReadCalls;
  ButtonsInterruptDpc(TEST_INTERRUPT, TEST_DEVICE);
  CHECK(mReadCalls == Reads && mWorkEnqueues == Enqueues);
  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS &&
        mUnmapCalls == 3 && mDevice.Registers == NULL && !mDevice.Prepared &&
        !mDevice.InterruptResourcePresent &&
        mDevice.TranslatedInterruptVector == 0U);
  CHECK(mDevice.Interrupt == NULL && !mInterruptObjectAlive);
  ButtonsTimer(TEST_TIMER);
  ButtonsWork(TEST_WORK);
  CHECK(mReadCalls == Reads && mWorkEnqueues == Enqueues);

  /* WDF deleted the first object; reprepare must create a fresh interrupt. */
  mTranslatedDescriptors[1].u.Interrupt.Vector = 0x57U;
  CHECK(TestPrepare() == STATUS_SUCCESS && mDevice.Interrupt == TEST_INTERRUPT &&
        mDevice.TranslatedInterruptVector == 0x57U &&
        mInterruptCreateCalls == 4 && mInterruptInfoCalls == 5 &&
        mInterruptObjectAlive);
  mInterruptInfo.Vector = 0x56U;
  Reads = mReadCalls;
  Enqueues = mWorkEnqueues;
  PmicBefore = mPmicCalls;
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) ==
        STATUS_DEVICE_CONFIGURATION_ERROR && mInterruptInfoCalls == 6 &&
        mReadCalls == Reads && mPmicCalls == PmicBefore &&
        mWorkEnqueues == Enqueues && !mDevice.Active && !mDevice.WorkQueued);
  mInterruptInfo.Vector = 0x57U;
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mInterruptInfoCalls == 7 && mDevice.Active &&
        mWorkEnqueues == Enqueues + 1U);
  TestWdfInterruptEnable(NULL, TEST_INTERRUPT);
  CHECK(mInterruptEnableCalls == 2 && mInterruptDisableCalls == 1);
  CHECK(ButtonsD0ExitPreInterruptsDisabled(TEST_DEVICE, WdfPowerDeviceD3) ==
        STATUS_SUCCESS);
  TestWdfInterruptDisable(NULL, TEST_INTERRUPT);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mInterruptEnableCalls == 2 && mInterruptDisableCalls == 2);
  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS &&
        mUnmapCalls == 4 && mDevice.Registers == NULL && !mDevice.Prepared);
}

static void PrepareV2(USHORT TopStatus, BOOLEAN Start) {
  TestReset();
  TestInstallWdfFunctions();
  mPmicQueryStatus = MaliOk;
  mInterfaceAvailable = TRUE;
  mPmicSnapshot.TopStatus = TopStatus;
  mRawCount = mTranslatedCount = 2;
  mRawDescriptors[1].Type = CmResourceTypeInterrupt;
  mRawDescriptors[1].Flags = CM_RESOURCE_INTERRUPT_LATCHED;
  mRawDescriptors[1].ShareDisposition = CmResourceShareDeviceExclusive;
  mRawDescriptors[1].u.Interrupt.Vector = 138U;
  mTranslatedDescriptors[1] = mRawDescriptors[1];
  mTranslatedDescriptors[1].u.Interrupt.Vector = 0x56U;
  CHECK(TestCreateDevice() == STATUS_SUCCESS);
  CHECK(TestPrepare() == STATUS_SUCCESS);
  SetKpd(0xffffU, 1024U);
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  if (Start) {
    RunWorker();
    CHECK(mDevice.PmicMode == ButtonsPmicModeKeyIrqV2 &&
          mDevice.KeyIrqLeaseHeld && mDevice.KeyIrqLeaseToken == 1 &&
          !mDevice.KeyIrqSnapshotPending && mDevice.Core.BaselineValid &&
          mPmicQueryCalls == 1 && mPmicLeaseAcquires == 1 &&
          mPmicSnapshotReads == 1 && mPmicSnapshotAcks == 1 &&
          mWriteCalls == 0 && mPmicCalls == 0);
  }
}
static void TestV2CapturedShortTapsAndHeldCycles(void) {
  ULONG Writes, Reads, Acks;
  PrepareV2(0xffffU, TRUE);
  /* The level is up at both polls, but the retained press proves a short tap. */
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  RunWorker();
  CHECK(mWriteCalls == 2 && mWrittenButtons[0] == MtkButtonPower &&
        mWrittenButtons[1] == MtkButtonPower &&
        mDevice.Core.DeliveredPressed == 0 && !mDevice.KeyIrqSnapshotPending &&
        mLastSnapshotAckSequence > mLastToggleSequence &&
        mPmicSnapshotReads == 2 && mPmicSnapshotAcks == 2);
  mPmicSnapshot.TopStatus = 0xfffdU;
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_MASK;
  RunWorker();
  CHECK(mWriteCalls == 3 && mDevice.Core.DeliveredPressed == MTK_BUTTON_STATE_POWER);
  /* Down/down with a retained release is one up/down cycle. */
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_RELEASED;
  RunWorker();
  CHECK(mWriteCalls == 5 && mWrittenButtons[3] == MtkButtonPower &&
        mWrittenButtons[4] == MtkButtonPower &&
        mDevice.Core.DeliveredPressed == MTK_BUTTON_STATE_POWER);
  mPmicSnapshot.TopStatus = 0xffffU;
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_RELEASED;
  RunWorker();
  CHECK(mWriteCalls == 6 && mDevice.Core.DeliveredPressed == 0);
  Reads = mReadCalls; Acks = mPmicSnapshotAcks; Writes = mWriteCalls;
  CHECK(ButtonsD0ExitPreInterruptsDisabled(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mPmicLeaseReleases == 1 && !mDevice.KeyIrqLeaseHeld &&
        mDevice.KeyIrqLeaseToken == 0 && !mDevice.Core.BaselineValid &&
        mPmicSnapshotAcks == Acks && mWriteCalls == Writes && mReadCalls == Reads);
  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS &&
        mUnmapCalls == 1 && mDevice.Registers == NULL);

  PrepareV2(0xfffdU, TRUE);
  CHECK(mDevice.Core.SuppressUntilRelease == MTK_BUTTON_STATE_POWER);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_MASK; RunWorker();
  CHECK(mWriteCalls == 0 && mDevice.Core.SuppressUntilRelease == MTK_BUTTON_STATE_POWER);
  mPmicSnapshot.TopStatus = 0xffffU;
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_RELEASED; RunWorker();
  CHECK(mWriteCalls == 0 && mDevice.Core.SuppressUntilRelease == 0);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED; RunWorker();
  CHECK(mWriteCalls == 2 && mDevice.Core.DeliveredPressed == 0);
}
static void TestV2RetainedAckAndLateInbox(void) {
  ULONG Reads, Acks, Writes;
  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mPmicSnapshotAckStatus = MaliBusy;
  RunWorker();
  CHECK(mDevice.KeyIrqSnapshotPending && mDevice.KeyIrqSnapshotSampled &&
        mDevice.Core.ActionCount == 0 && mWriteCalls == 2 &&
        !mDevice.Quarantined && !mDevice.PmicClientFaulted);
  Reads = mPmicSnapshotReads; Acks = mPmicSnapshotAcks; Writes = mWriteCalls;
  RunWorker();
  CHECK(mPmicSnapshotReads == Reads && mWriteCalls == Writes &&
        mPmicSnapshotAcks == Acks + 1 && mDevice.KeyIrqSnapshotPending);
  mPmicSnapshotAckStatus = MaliOk; RunWorker();
  CHECK(mPmicSnapshotReads == Reads && mWriteCalls == Writes &&
        !mDevice.KeyIrqSnapshotPending && !mDevice.Quarantined);

  PrepareV2(0xffffU, TRUE);
  mInterfaceAvailable = FALSE;
  if(mDevice.NotifyOpen) {
    WdfIoTargetClose(mDevice.NotifyTarget); mDevice.NotifyOpen = FALSE;
  }
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  RunWorker();
  CHECK(mDevice.KeyIrqSnapshotPending && mDevice.KeyIrqSnapshotSampled &&
        mDevice.Core.ActionCount == 2 && mWriteCalls == 0 && mPmicSnapshotAcks == 1);
  Reads = mPmicSnapshotReads;
  mInterfaceAvailable = TRUE; RunWorker();
  CHECK(mPmicSnapshotReads == Reads && mWriteCalls == 2 &&
        mPmicSnapshotAcks == 2 && mDevice.Core.ActionCount == 0 &&
        !mDevice.KeyIrqSnapshotPending && !mDevice.Quarantined &&
        mLastSnapshotAckSequence > mLastToggleSequence);
}
static void TestV2BusyNeverFallsBack(void) {
  PrepareV2(0xffffU, FALSE);
  mPmicQueryStatus = MaliBusy; RunWorker();
  CHECK(mDevice.PmicMode == ButtonsPmicModeUnknown && mPmicQueryCalls == 1 &&
        mPmicLeaseAcquires == 0 && mPmicSnapshotReads == 0 &&
        !mDevice.Core.BaselineValid && mPmicCalls == 0 && !mDevice.Quarantined);
  mPmicQueryStatus = MaliOk; mPmicLeaseAcquireStatus = MaliBusy; RunWorker();
  CHECK(mDevice.PmicMode == ButtonsPmicModeKeyIrqV2 && mPmicQueryCalls == 2 &&
        mPmicLeaseAcquires == 1 && !mDevice.KeyIrqLeaseHeld &&
        mPmicSnapshotReads == 0 && mPmicCalls == 0 && !mDevice.Quarantined);
  mPmicLeaseAcquireStatus = MaliOk; mPmicSnapshotReadStatus = MaliBusy; RunWorker();
  CHECK(mPmicQueryCalls == 2 && mPmicLeaseAcquires == 2 &&
        mDevice.KeyIrqLeaseHeld && !mDevice.Core.BaselineValid &&
        mPmicSnapshotReads == 1 && mPmicCalls == 0 && !mDevice.Quarantined);
  mPmicSnapshotReadStatus = MaliOk; RunWorker();
  CHECK(mPmicSnapshotReads == 2 && mPmicSnapshotAcks == 1 &&
        mDevice.Core.BaselineValid && mPmicCalls == 0);
}
static void TestV2D0RacesAndReleaseRetry(void) {
  ULONG Reads, Writes, Acks;
  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mInjectTimerOnSnapshotRead = TRUE;
  mInjectInterruptOnSnapshotRead = TRUE;
  RunWorker();
  CHECK(mPmicSnapshotReads == 3 && mPmicSnapshotAcks == 3 &&
        mWriteCalls == 3 && mWrittenButtons[0] == MtkButtonPower &&
        mWrittenButtons[1] == MtkButtonPower && mWrittenButtons[2] == MtkButtonVolumeUp &&
        mDevice.Core.DeliveredPressed == MTK_BUTTON_STATE_VOLUME_UP &&
        !mDevice.KeyIrqSnapshotPending && !mDevice.Quarantined &&
        mInterruptDpcQueues == 1);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mWriteCalls == 4 && mWrittenButtons[3] == MtkButtonVolumeUp &&
        !mDevice.Core.DeliveredPressed && mPmicLeaseReleases == 1);

  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mInjectD0ExitOnSnapshotRead = TRUE; RunWorker();
  CHECK(!mDevice.Active && mDevice.KeyIrqSnapshotPending &&
        mDevice.KeyIrqSnapshotSampled && mDevice.Core.ActionCount == 2 &&
        mPmicSnapshotAcks == 1 && mWriteCalls == 0);
  Reads = mPmicSnapshotReads;
  CHECK(ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_BUSY);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mPmicSnapshotReads == Reads && mPmicSnapshotAcks == 2 &&
        mWriteCalls == 2 && !mDevice.KeyIrqSnapshotPending &&
        !mDevice.KeyIrqLeaseHeld && mPmicLeaseReleases == 1);
  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS &&
        mUnmapCalls == 1);

  PrepareV2(0xffffU, TRUE);
  mPmicLeaseReleaseStatus = MaliBusy; Writes = mWriteCalls; Acks = mPmicSnapshotAcks;
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_BUSY &&
        mDevice.KeyIrqLeaseHeld && mDevice.KeyIrqLeaseToken == 1 &&
        !mDevice.Active && !mDevice.Quarantined && mPmicLeaseReleases == 1);
  mPmicLeaseReleaseStatus = MaliOk;
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mPmicLeaseReleases == 2 && !mDevice.KeyIrqLeaseHeld &&
        mWriteCalls == Writes && mPmicSnapshotAcks == Acks);
  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS &&
        mUnmapCalls == 1);
}
static void TestV2FaultsRetainOwnershipWithoutReplay(void) {
  ULONG Reads, Writes, Acks;
  PrepareV2(0xffffU, FALSE);
  mPmicQueryStatus = MaliFault; RunWorker();
  CHECK(mDevice.Quarantined && mDevice.PmicClientFaulted && !mDevice.Active &&
        mPmicLeaseAcquires == 0 && mPmicCalls == 0);
  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.Generation = 0; RunWorker();
  CHECK(mDevice.Quarantined && mDevice.PmicClientFaulted &&
        mDevice.KeyIrqSnapshotPending && !mDevice.KeyIrqSnapshotSampled &&
        mDevice.KeyIrqLeaseHeld && mPmicSnapshotAcks == 1 && mWriteCalls == 0);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_HARDWARE_ERROR &&
        mDevice.Registers == NULL && !mDevice.Prepared && mUnmapCalls == 1 &&
        mDevice.KeyIrqLeaseHeld);
  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.Generation = mDevice.Core.LastSnapshotGeneration; RunWorker();
  CHECK(mDevice.Quarantined && mDevice.PmicClientFaulted &&
        mDevice.KeyIrqSnapshotPending && mPmicSnapshotAcks == 1);
  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mPmicSnapshotAckStatus = MaliFault; RunWorker();
  CHECK(mDevice.Quarantined && mDevice.PmicClientFaulted &&
        mDevice.KeyIrqSnapshotPending && mDevice.KeyIrqLeaseHeld &&
        mWriteCalls == 2 && mPmicSnapshotAcks == 2);
  Reads = mPmicSnapshotReads; Writes = mWriteCalls; Acks = mPmicSnapshotAcks;
  ButtonsTimer(TEST_TIMER); ButtonsWork(TEST_WORK);
  CHECK(mPmicSnapshotReads == Reads && mWriteCalls == Writes &&
        mPmicSnapshotAcks == Acks &&
        ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_HARDWARE_ERROR &&
        mPmicLeaseReleases == 0);
  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mWriteBytes = 2; RunWorker();
  CHECK(mDevice.Quarantined && mDevice.Core.Quarantined &&
        mDevice.KeyIrqSnapshotPending && mDevice.KeyIrqLeaseHeld &&
        mPmicSnapshotAcks == 1 && mWriteCalls == 1);
  Reads = mPmicSnapshotReads; Writes = mWriteCalls;
  ButtonsTimer(TEST_TIMER); ButtonsWork(TEST_WORK);
  CHECK(mPmicSnapshotReads == Reads && mWriteCalls == Writes &&
        mPmicSnapshotAcks == 1 && mPmicLeaseReleases == 0);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_HARDWARE_ERROR &&
        mDevice.Registers == NULL && !mDevice.Prepared && mUnmapCalls == 1 &&
        mDevice.KeyIrqLeaseHeld);
  PrepareV2(0xffffU, TRUE);
  mPmicLeaseReleaseStatus = MaliFault;
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_HARDWARE_ERROR &&
        mDevice.PmicClientFaulted && mDevice.Quarantined && mDevice.KeyIrqLeaseHeld);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_HARDWARE_ERROR &&
        mDevice.Registers == NULL && !mDevice.Prepared && mUnmapCalls == 1 &&
        mDevice.KeyIrqLeaseHeld);
}

static void TestV2ReleaseRevokesResourcesButRetainsOwnership(void) {
  ULONG Maps, Reads, Writes, Acks, Releases;
  PrepareV2(0xffffU, TRUE);
  mPmicLeaseReleaseStatus = MaliBusy;
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_BUSY);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_BUSY &&
        !mDevice.Prepared && mDevice.Registers == NULL &&
        mDevice.Interrupt == NULL && mUnmapCalls == 1 &&
        mDevice.KeyIrqLeaseHeld && mDevice.KeyIrqLeaseToken == 1);
  Maps=mMapCalls; Reads=mReadCalls; Writes=mWriteCalls;
  Acks=mPmicSnapshotAcks; Releases=mPmicLeaseReleases;
  CHECK(TestPrepare() == STATUS_DEVICE_BUSY &&
        ButtonsD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_NOT_READY);
  ButtonsTimer(TEST_TIMER); ButtonsWork(TEST_WORK);
  CHECK(mMapCalls==Maps && mReadCalls==Reads && mWriteCalls==Writes &&
        mPmicSnapshotAcks==Acks && mPmicLeaseReleases==Releases &&
        mDevice.KeyIrqLeaseHeld);

  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mPmicSnapshotAckStatus = MaliBusy;
  RunWorker();
  CHECK(mDevice.KeyIrqSnapshotPending && mWriteCalls == 2);
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_BUSY);
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_BUSY &&
        !mDevice.Prepared && mDevice.Registers == NULL &&
        mDevice.KeyIrqSnapshotPending && mDevice.KeyIrqLeaseHeld &&
        mUnmapCalls == 1 && mPmicLeaseReleases == 0);
  Maps=mMapCalls; Writes=mWriteCalls; Acks=mPmicSnapshotAcks;
  CHECK(TestPrepare() == STATUS_DEVICE_BUSY && mMapCalls==Maps &&
        mWriteCalls==Writes && mPmicSnapshotAcks==Acks);

  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mWriteBytes=2;
  RunWorker();
  CHECK(TestFrameworkReleaseHardware() == STATUS_DEVICE_HARDWARE_ERROR &&
        !mDevice.Prepared && mDevice.Registers == NULL && mUnmapCalls==1 &&
        mDevice.KeyIrqSnapshotPending && mDevice.KeyIrqLeaseHeld &&
        mDevice.Core.Quarantined && mWriteCalls==1);
  Maps=mMapCalls; Writes=mWriteCalls; Acks=mPmicSnapshotAcks;
  CHECK(TestPrepare() == STATUS_DEVICE_HARDWARE_ERROR && mMapCalls==Maps &&
        mWriteCalls==Writes && mPmicSnapshotAcks==Acks);
}

static void TestV2D0ExitDuringInboxToggle(void) {
  ULONG Reads;
  PrepareV2(0xffffU, TRUE);
  mPmicSnapshot.GroupStatus = MTK_BUTTON_POWER_EVENT_PRESSED;
  mInjectD0ExitOnTargetWrite=TRUE;
  RunWorker();
  CHECK(!mDevice.Active && !mDevice.KeyIrqSnapshotPending &&
        mWriteCalls==2 && mPmicSnapshotAcks==2 &&
        mLastSnapshotAckSequence>mLastToggleSequence &&
        !mDevice.Core.ActionCount && !mDevice.Core.DeliveredPressed);
  Reads=mPmicSnapshotReads;
  CHECK(ButtonsD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS &&
        mPmicSnapshotReads==Reads && mWriteCalls==2 &&
        mPmicLeaseReleases==1 && !mDevice.KeyIrqLeaseHeld);
  CHECK(TestFrameworkReleaseHardware() == STATUS_SUCCESS && mUnmapCalls==1);
}

int TestMain(void) {
  TestInstallWdfFunctions();
  TestResourcesAndPolling();
  TestD0ExitRetryAndAmbiguousWrite();
  TestDuplicateNotifyInterfaceFailsClosed();
  TestInformationalQueryAndOpenAreNotAccepted();
  TestInterruptResourceAndD0Rundown();
  if (mFailureLine) return (int)mFailureLine;
  TestV2CapturedShortTapsAndHeldCycles();
  TestV2RetainedAckAndLateInbox();
  TestV2BusyNeverFallsBack();
  TestV2D0RacesAndReleaseRetry();
  TestV2FaultsRetainOwnershipWithoutReplay();
  TestV2ReleaseRevokesResourcesButRetainsOwnership();
  TestV2D0ExitDuringInboxToggle();
  return (int)mFailureLine;
}
