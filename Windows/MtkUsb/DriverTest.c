/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Execute production UCX/KMDF callbacks from Driver.c as ARM64 PE code.
 * WDF, UCX, kernel objects/MMIO, and USB transfers are mocked. Root-port
 * suspend and resume scenarios also execute the actual MusbHostCore code.
 */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <usbiodef.h>
#include <ucxclass.h>
#include <acpiioct.h>
#include <spb.h>
#include <Library/MtkUsbPhyCore.h>

static UCHAR UsbTestRead8(volatile UCHAR *Address);
static USHORT UsbTestRead16(volatile USHORT *Address);
static ULONG UsbTestRead32(PULONG Address);
static VOID UsbTestWrite8(volatile UCHAR *Address,UCHAR Value);
static VOID UsbTestWrite16(volatile USHORT *Address,USHORT Value);
static VOID UsbTestWrite32(PULONG Address,ULONG Value);
static PVOID UsbTestMapIoSpace(PHYSICAL_ADDRESS Address,SIZE_T Length,ULONG Flags);
static VOID UsbTestUnmapIoSpace(PVOID Base,SIZE_T Length);
static PVOID UsbTestMdlMap(PMDL Mdl,ULONG Priority);
static PVOID UsbTestPoolAlloc(POOL_FLAGS Flags,SIZE_T Bytes,ULONG Tag);
static VOID UsbTestPoolFree(PVOID Buffer,ULONG Tag);
static ULONGLONG UsbTestQueryTime(PULONGLONG Time);
static VOID UsbTestStall(ULONG Microseconds);
static NTSTATUS UsbTestDelayThread(KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Interval);
static VOID UsbTestInitializeEvent(PRKEVENT Event,EVENT_TYPE Type,BOOLEAN State);
static LONG UsbTestSetEvent(PRKEVENT Event,KPRIORITY Priority,BOOLEAN Wait);
static VOID UsbTestClearEvent(PRKEVENT Event);
static NTSTATUS UsbTestWaitEvent(PVOID Object,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Timeout);
static KIRQL UsbTestGetIrql(VOID);
static VOID UsbTestRaiseIrql(KIRQL NewIrql,PKIRQL OldIrql);
static VOID UsbTestLowerIrql(KIRQL NewIrql);
static PETHREAD UsbTestGetCurrentThread(VOID);
static VOID UsbTestInitializeSpinLock(PKSPIN_LOCK Lock);
static VOID UsbTestInitializeDpc(PRKDPC Dpc,PKDEFERRED_ROUTINE Routine,PVOID Context);
static VOID UsbTestAcquireSpinLock(PKSPIN_LOCK Lock,PKIRQL OldIrql);
static VOID UsbTestReleaseSpinLock(PKSPIN_LOCK Lock,KIRQL OldIrql);
static VOID UsbTestAcquireSpinLockAtDpc(PKSPIN_LOCK Lock);
static VOID UsbTestReleaseSpinLockFromDpc(PKSPIN_LOCK Lock);
static BOOLEAN UsbTestInsertQueueDpc(PKDPC Dpc,PVOID Arg1,PVOID Arg2);
static VOID UsbTestFlushDpcs(VOID);
static LONG UsbTestCompareExchange(volatile LONG *Target,LONG Exchange,LONG Compare);
static LONG UsbTestExchange(volatile LONG *Target,LONG Value);
static LONG UsbTestIncrement(volatile LONG *Target);
static LONG UsbTestDecrement(volatile LONG *Target);
static NTSTATUS UsbTestCreateRegistryKey(ULONG Root,PWSTR Path);
static NTSTATUS UsbTestWriteRegistryValue(ULONG Root,PCWSTR Path,PCWSTR Name,ULONG Type,PVOID Data,ULONG Bytes);
static MTK_USB_PHY_STATUS UsbTestMtkUsbPhyCoreSetUsbMode(
    MTK_USB_PHY_CORE *Core,MTK_USB_PHY_USB_MODE Mode,
    const MTK_USB_PHY_HOST_TUNING *HostTuning);

#undef READ_REGISTER_UCHAR
#undef READ_REGISTER_USHORT
#undef READ_REGISTER_ULONG
#undef WRITE_REGISTER_UCHAR
#undef WRITE_REGISTER_USHORT
#undef WRITE_REGISTER_ULONG
#undef MmMapIoSpaceEx
#undef MmUnmapIoSpace
#undef MmGetSystemAddressForMdlSafe
#undef ExAllocatePool2
#undef ExFreePoolWithTag
#undef KeQueryInterruptTimePrecise
#undef KeStallExecutionProcessor
#undef KeDelayExecutionThread
#undef KeInitializeEvent
#undef KeSetEvent
#undef KeClearEvent
#undef KeWaitForSingleObject
#undef KeGetCurrentIrql
#undef KeRaiseIrql
#undef KeLowerIrql
#undef KeInitializeSpinLock
#undef KeInitializeDpc
#undef KeAcquireSpinLock
#undef KeReleaseSpinLock
#undef KeAcquireSpinLockAtDpcLevel
#undef KeReleaseSpinLockFromDpcLevel
#undef KeInsertQueueDpc
#undef KeFlushQueuedDpcs
#undef PsGetCurrentThread
#undef InterlockedCompareExchange
#undef InterlockedExchange
#undef InterlockedIncrement
#undef InterlockedDecrement
#undef RtlZeroMemory
#undef RtlCopyMemory
#define READ_REGISTER_UCHAR UsbTestRead8
#define READ_REGISTER_USHORT UsbTestRead16
#define READ_REGISTER_ULONG UsbTestRead32
#define WRITE_REGISTER_UCHAR UsbTestWrite8
#define WRITE_REGISTER_USHORT UsbTestWrite16
#define WRITE_REGISTER_ULONG UsbTestWrite32
#define MmMapIoSpaceEx UsbTestMapIoSpace
#define MmUnmapIoSpace UsbTestUnmapIoSpace
#define MmGetSystemAddressForMdlSafe(M,F) UsbTestMdlMap((M),(ULONG)(F))
#define ExAllocatePool2 UsbTestPoolAlloc
#define ExFreePoolWithTag UsbTestPoolFree
#define KeQueryInterruptTimePrecise UsbTestQueryTime
#define KeStallExecutionProcessor UsbTestStall
#define KeDelayExecutionThread UsbTestDelayThread
#define KeInitializeEvent UsbTestInitializeEvent
#define KeSetEvent UsbTestSetEvent
#define KeClearEvent UsbTestClearEvent
#define KeWaitForSingleObject UsbTestWaitEvent
#define KeGetCurrentIrql UsbTestGetIrql
#define RtlCreateRegistryKey UsbTestCreateRegistryKey
#define RtlWriteRegistryValue UsbTestWriteRegistryValue
#define KeRaiseIrql UsbTestRaiseIrql
#define KeLowerIrql UsbTestLowerIrql
#define KeInitializeSpinLock UsbTestInitializeSpinLock
#define KeInitializeDpc UsbTestInitializeDpc
#define KeAcquireSpinLock UsbTestAcquireSpinLock
#define KeReleaseSpinLock UsbTestReleaseSpinLock
#define KeAcquireSpinLockAtDpcLevel UsbTestAcquireSpinLockAtDpc
#define KeReleaseSpinLockFromDpcLevel UsbTestReleaseSpinLockFromDpc
#define KeInsertQueueDpc UsbTestInsertQueueDpc
#define KeFlushQueuedDpcs UsbTestFlushDpcs
#define PsGetCurrentThread UsbTestGetCurrentThread
#define InterlockedCompareExchange UsbTestCompareExchange
#define InterlockedExchange UsbTestExchange
#define InterlockedIncrement UsbTestIncrement
#define InterlockedDecrement UsbTestDecrement
#define RtlZeroMemory(D,N) memset((D),0,(N))
#define RtlCopyMemory(D,S,N) memcpy((D),(S),(N))

#define MtkMusbPlatformCheckQuiescent UsbTestPlatformIdle
#define MtkMusbPlatformVerifyHostFifo UsbTestPlatformFifo
#define MtkMusbPlatformRestoreRetainedHostFifo UsbTestRestoreFifo
#define MtkMusbPlatformTakeOverHost UsbTestTakeOver
#define MtkMusbPlatformAcknowledgeIdleInterrupts UsbTestPlatformAck
#define MusbHostInitialize UsbTestMusbHostInitialize
#define MusbHostAdoptPortState UsbTestMusbHostAdoptPortState
#define MusbHostSetState UsbTestMusbHostSetState
#define MusbHostGetPortStatus UsbTestMusbHostGetPortStatus
#define MusbHostSetPortFeature UsbTestMusbHostSetPortFeature
#define MusbHostClearPortFeature UsbTestMusbHostClearPortFeature
#define MusbHostControlTransferRouted UsbTestMusbHostControlTransferRouted
#define MusbHostDataTransferRouted UsbTestMusbHostDataTransferRouted
#define MtkUsbPhyCoreSetUsbMode UsbTestMtkUsbPhyCoreSetUsbMode

#include "Driver.c"

#undef MusbHostGetPortStatus
#undef MusbHostSetPortFeature
#undef MusbHostClearPortFeature
#undef MusbHostDataTransferRouted
#undef MusbHostInitialize
#undef MtkUsbPhyCoreSetUsbMode
MUSB_STATUS MusbHostInitialize(MUSB_HOST *,const MUSB_HOST_IO *,const MUSB_HOST_CONFIG *);
MUSB_STATUS MusbHostDataTransferRouted(MUSB_HOST *,uint8_t,uint8_t,uint8_t,uint8_t,uint16_t,uint8_t,void *,uint32_t,uint8_t *,uint32_t,const MUSB_HOST_ROUTE *,uint32_t *);
MUSB_STATUS MusbHostGetPortStatus(MUSB_HOST *,uint8_t,uint16_t *,uint16_t *);
MUSB_STATUS MusbHostSetPortFeature(MUSB_HOST *,uint8_t,uint8_t);
MUSB_STATUS MusbHostClearPortFeature(MUSB_HOST *,uint8_t,uint8_t);
MTK_USB_PHY_STATUS MtkUsbPhyCoreSetUsbMode(
    MTK_USB_PHY_CORE *,MTK_USB_PHY_USB_MODE,const MTK_USB_PHY_HOST_TUNING *);

static unsigned UsbTestFailure,UsbTestChecks;
static BOOLEAN UsbTestUseRealPort,UsbTestUseRealCoreData;
static MUSB_STATUS UsbTestRealCoreStatus;
static ULONG UsbTestRealCoreBytes;
static BOOLEAN UsbTestFailColdMode,UsbTestColdModeFailureObserved;
#define CHECK(X) do { UsbTestChecks++; if (!(X) && !UsbTestFailure) UsbTestFailure=__LINE__; } while (0)

static MTK_USB_PHY_STATUS UsbTestMtkUsbPhyCoreSetUsbMode(
    MTK_USB_PHY_CORE *Core,MTK_USB_PHY_USB_MODE Mode,
    const MTK_USB_PHY_HOST_TUNING *HostTuning) {
  if(UsbTestFailColdMode) {
    UsbTestColdModeFailureObserved=TRUE;
    CHECK(MtkUsbPhyCoreIsPowered(Core)&&Mode==MtkUsbPhyModeOtg&&HostTuning==NULL);
    return MtkUsbPhyIoError;
  }
  return MtkUsbPhyCoreSetUsbMode(Core,Mode,HostTuning);
}

#define TEST_DEVICE ((WDFDEVICE)(ULONG_PTR)0x101)
#define TEST_UCX ((UCXCONTROLLER)(ULONG_PTR)0x102)
#define TEST_ROOTHUB ((UCXROOTHUB)(ULONG_PTR)0x103)
#define TEST_WORKER ((WDFWORKITEM)(ULONG_PTR)0x104)
#define TEST_TIMER ((WDFTIMER)(ULONG_PTR)0x105)
#define TEST_TARGET ((WDFIOTARGET)(ULONG_PTR)0x106)
#define TEST_ENDPOINT ((UCXENDPOINT)(ULONG_PTR)0x107)
#define TEST_USBDEVICE ((UCXUSBDEVICE)(ULONG_PTR)0x108)
#define TEST_TT_HUB ((UCXUSBDEVICE)(ULONG_PTR)0x109)
#define TEST_MANAGEMENT ((WDFQUEUE)(ULONG_PTR)0x10a)
#define TEST_ROOT_INTERRUPT ((WDFQUEUE)(ULONG_PTR)0x10b)
#define TEST_RAW_RESOURCES ((WDFCMRESLIST)(ULONG_PTR)0x10c)
#define TEST_TRANSLATED_RESOURCES ((WDFCMRESLIST)(ULONG_PTR)0x10d)
#define TEST_TCPC_TARGET ((WDFIOTARGET)(ULONG_PTR)0x10e)
#define TEST_INTERRUPT ((WDFINTERRUPT)(ULONG_PTR)0x10f)
#define TEST_USB_DIRQL (DISPATCH_LEVEL+1)
#define TEST_REQUEST_BASE ((ULONG_PTR)0x2000)
#define TEST_CONTEXTS 64
#define TEST_QUEUES 16
#define TEST_REQUESTS 10
#define TEST_QUEUE_DEPTH 16
#define TEST_STOP_CALLBACKS 8

typedef struct { WDFOBJECT Handle; PVOID Context; } TEST_CONTEXT_MAP;
typedef struct {
  WDFREQUEST Handle;
  PVOID Argument;
  WDFQUEUE OriginQueue;
  USB_REQUEST Context;
  PFN_WDF_REQUEST_CANCEL Cancel;
  NTSTATUS CompletionStatus;
  ULONG_PTR CompletionInformation;
  ULONG CompletionCount;
  ULONG References;
  BOOLEAN Allocated,Cancelled;
} TEST_REQUEST_SLOT;
typedef struct {
  WDFQUEUE Handle;
  USB_QUEUE Context;
  WDFREQUEST Requests[TEST_QUEUE_DEPTH];
  ULONG Count;
  BOOLEAN Started;
  EVT_WDF_IO_QUEUE_STATE *StopCallback;
  WDFCONTEXT StopContext;
} TEST_QUEUE_SLOT;
typedef struct {
  WDFQUEUE Queue;
  EVT_WDF_IO_QUEUE_STATE *Callback;
  WDFCONTEXT Context;
} TEST_STOP_SLOT;

static USB_CONTROLLER UsbTestController;
static USB_DEVICE UsbTestDevice,UsbTestHub;
static USB_ENDPOINT UsbTestEndpoint;
static USB_OBJECT UsbTestUcxObject,UsbTestRootObject,UsbTestWorkObject,UsbTestTimerObject;
static USB_QUEUE UsbTestManagementContext,UsbTestRootInterruptContext;
static TEST_CONTEXT_MAP UsbTestContexts[TEST_CONTEXTS];
static TEST_QUEUE_SLOT UsbTestQueues[TEST_QUEUES];
static TEST_REQUEST_SLOT UsbTestRequests[TEST_REQUESTS];
static TEST_STOP_SLOT UsbTestStops[TEST_STOP_CALLBACKS];
static ULONG UsbTestContextCount,UsbTestQueueCount,UsbTestRequestCount,UsbTestStopCount;
static ULONG UsbTestQueueCreates,UsbTestQueueCreateFailAt;
static ULONG UsbTestAddActive,UsbTestInterfaceCalls,UsbTestUcxCreates,UsbTestRootCreates,UsbTestDefaultQueues;
static NTSTATUS UsbTestInterfaceStatus;
static ULONG UsbTestIoctlCalls;
static BOOLEAN UsbTestIoctlHandled;
static ULONG UsbTestDeviceCreates,UsbTestDeviceCallbacks;
static NTSTATUS UsbTestDeviceCreateStatus;
static WDF_PNPPOWER_EVENT_CALLBACKS UsbTestPowerCallbacks;
static NTSTATUS UsbTestQueueCreateStatus;
static WDFFUNC UsbTestWdfTable[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions=UsbTestWdfTable;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
PFN_UCXFUNC UcxFunctions[UcxFunctionTableNumEntries];
PUCX_DRIVER_GLOBALS UcxDriverGlobals;

static union {
  UCHAR Bytes[USB_BYTES];
  volatile ULONGLONG Words[USB_BYTES/sizeof(ULONGLONG)];
} UsbTestRegisterPage;
#define UsbTestRegisters (UsbTestRegisterPage.Bytes)
static ULONG UsbTestHandoff[16];
static ULONG UsbTestPhy[MTK_MUSB_PLATFORM_PHY_BYTES/sizeof(ULONG)];
static CM_PARTIAL_RESOURCE_DESCRIPTOR UsbTestRaw[5],UsbTestTranslated[5];
static struct {
  NTSTATUS OpenStatus,LockStatus,UnlockStatus,ReadStatus;
  ULONG Create,Open,Close,Delete,Locks,Unlocks,Reads,ReadStep,FailAt,ShortAt;
  ULONG FirstState,SecondState;
  BOOLEAN Created,Opened,Locked,WrongIdentity,Safe0V;
} UsbTestTcpc;
static struct {
  WDF_INTERRUPT_CONFIG Config;
  NTSTATUS CreateStatus;
  ULONG Create,Delete,Pending;
  BOOLEAN Created,Connected,DropMaskWrite;
} UsbTestInterrupt;
static ULONG UsbTestRawCount,UsbTestTranslatedCount,UsbTestMapCalls,UsbTestUnmapCalls,UsbTestMapFailAt;
static ULONG UsbTestMmioReads,UsbTestMmioWrites,UsbTestLastWriteOffset;
static BOOLEAN UsbTestDropPowerWrite,UsbTestDropSessionWrite;
static ULONG UsbTestDevctlWrites;
static ULONG UsbTestPhyWriteCount,UsbTestDropPhyWriteAt,UsbTestLoseHostAt;
static ULONG UsbTestPhyWriteOffsets[ARRAYSIZE(UsbPhyTuningFields)];
static UCHAR UsbTestPool[USB_MAX_TRANSFER];
static BOOLEAN UsbTestPoolInUse;
static PMDL UsbTestMdlMapFail;
static ULONG UsbTestMdlMapCalls,UsbTestExpectedProbeMaps;
static ULONG UsbTestClock,UsbTestWdfLockDepth,UsbTestIrql,UsbTestDpcQueued,UsbTestDpcFlushes;
static ULONG UsbTestStallCalls,UsbTestDelayCalls;
static LONGLONG UsbTestDelayInterval;
static BOOLEAN UsbTestEndpointLockHeld;
static PETHREAD UsbTestThread=(PETHREAD)(ULONG_PTR)0x707;
static ULONG UsbTestWorkEnqueues,UsbTestWorkFlushes,UsbTestTimerStarts,UsbTestTimerStops;
static ULONG UsbTestQueueStarts,UsbTestQueuePurges,UsbTestForwardCalls,UsbTestRequeueCalls;
static WDFREQUEST UsbTestQueueStartDispatchRequest,UsbTestReentrantDisableRequest;
static ULONG UsbTestQueueStartDispatchCode,UsbTestQueueStartDispatches;
static BOOLEAN UsbTestReenterD0ExitOnQueueStart,UsbTestReenterD0ExitOnQueueStop,UsbTestReenterControllerResetOnQueueStart;
static BOOLEAN UsbTestReenterDisableOnRequestComplete;
static NTSTATUS UsbTestReentrantD0ExitStatus;
static BOOLEAN UsbTestReenterWorker;
static ULONG UsbTestResetCompleteCalls,UsbTestResetSequence,UsbTestResetFailedOrder,UsbTestResetCompleteOrder;
typedef enum {
  ResetPogo, ResetTypecLostFifo, ResetNoClock, ResetNoRoute, ResetChangedCc, ResetBusyDma,
  ResetClockLost, ResetVbusLost, ResetStopping, ResetReentrantPower, ResetWaitingForVbus
} TEST_RESET_CASE;
static TEST_RESET_CASE UsbTestResetCase;
static ULONG UsbTestSwResetWrites,UsbTestResetWaits;
static BOOLEAN UsbTestResetCanDrain;
static ULONG UsbTestMarkCalls,UsbTestUnmarkCalls,UsbTestReferenceCalls,UsbTestDereferenceCalls;
static LONG UsbTestEndpointReferences,UsbTestDeviceReferences;
static ULONG UsbTestMdlMapsAtCore,UsbTestCoreInitCalls,UsbTestCoreAdoptCalls,UsbTestCoreStateCalls;
static ULONG UsbTestPortCalls,UsbTestPortFeatureCalls,UsbTestControlCalls,UsbTestDataCalls,UsbTestRealDataCalls;
static ULONG UsbTestLastTransferType;
static ULONG UsbTestUcxRootChanged,UsbTestUcxPurgeDoneCount,UsbTestUcxAbortDoneCount;
static ULONG UsbTestUcxEndpointCreateCalls,UsbTestUcxQueueBindCalls,UsbTestEndpointCallbacks;
static ULONG UsbTestPurgeCalls,UsbTestDirectCompletes,UsbTestDpcCompletes;
static ULONG UsbTestAcpiCalls,UsbTestAcpiVariant;
static ULONG UsbTestTypecCalls,UsbTestTypecSample;
static NTSTATUS UsbTestTypecStatus;
static ULONG UsbTestRouteCalls,UsbTestRouteArgument,UsbTestRouteReply;
static BOOLEAN UsbTestRoutePerPath;
static ULONG UsbTestRouteReplies[4];
static NTSTATUS UsbTestRouteStatus;
static BOOLEAN UsbTestTypecBadReply;
static ULONG UsbTestClockCalls,UsbTestClockIdentity,UsbTestAckCalls;
static NTSTATUS UsbTestClockStatus,UsbTestGateStatus;
static ULONG UsbTestGateCalls,UsbTestGateArgument,UsbTestGateReply;
static BOOLEAN UsbTestGateLatches,UsbTestGateBadReply;
static MTK_MUSB_PLATFORM_STATUS UsbTestTakeOverResult;
static ULONG UsbTestTakeOverCalls;
static ULONG UsbTestIdleCalls,UsbTestFifoCalls,UsbTestFifoRestores,UsbTestControllerFailed;
static MTK_MUSB_PLATFORM_STATUS UsbTestIdleResult,UsbTestFifoResult,UsbTestFifoRestoreResult;
static ULONG UsbTestExpectedCoreFlags;
static UCHAR UsbTestDevctlAfterFifo;
static BOOLEAN UsbTestDropVbusOnAck;
static ULONG UsbTestExpectedPortStatus,UsbTestExpectedPortChange;
static ULONG UsbTestExpectedRoute,UsbTestExpectedHubAddress,UsbTestExpectedHubPort,UsbTestExpectedMultiTt;
static ULONG UsbTestExpectedTransferLength,UsbTestExpectedTimeout,UsbTestExpectedEndpointAddress;
static UCHAR UsbTestExpectedOut[64],UsbTestLastSetup[8],UsbTestLastControlAddress;
static MUSB_STATUS UsbTestCoreResult,UsbTestCoreInitResult,UsbTestCoreStateResult;
static ULONG UsbTestCoreDone,UsbTestCancelInCore,UsbTestCancelAtUnmark,UsbTestExpectCoreData;
static ULONG UsbTestExpectedAdoptStatus,UsbTestExpectedAdoptChange;
static PFN_WDF_REQUEST_CANCEL UsbTestCurrentCancel;
static UCX_ENDPOINT_EVENT_CALLBACKS UsbTestEndpointEvents;
static BOOLEAN UsbTestEndpointEventsSeen;
static UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS UsbTestDefaultEndpointEvents;
static BOOLEAN UsbTestDefaultEndpointEventsSeen;
static USB_REQUEST *UsbTestLastAllocatedRequest;
static ULONG_PTR UsbTestLastRequestHandle;
static ULONG UsbTestLastEndpointQueueMode;

#pragma function(memcpy,memset)
void *memcpy(void *To,const void *From,size_t Length) {
  volatile UCHAR *D=To;const volatile UCHAR *S=From;
  while(Length--) *D++=*S++;
  return To;
}
void *memset(void *To,int Value,size_t Length) {
  volatile UCHAR *D=To;
  while(Length--) *D++=(UCHAR)Value;
  return To;
}

static VOID UsbTestAddContext(WDFOBJECT Handle,PVOID Context) {
  ULONG I;
  for(I=0;I<UsbTestContextCount;I++) if(UsbTestContexts[I].Handle==Handle) {
    UsbTestContexts[I].Context=Context;return;
  }
  CHECK(UsbTestContextCount<TEST_CONTEXTS);
  if(UsbTestContextCount<TEST_CONTEXTS) {
    UsbTestContexts[UsbTestContextCount].Handle=Handle;
    UsbTestContexts[UsbTestContextCount].Context=Context;
    UsbTestContextCount++;
  }
}
static TEST_REQUEST_SLOT *UsbTestFindRequest(WDFREQUEST Handle) {
  ULONG I;
  for(I=0;I<UsbTestRequestCount;I++) if(UsbTestRequests[I].Handle==Handle) return &UsbTestRequests[I];
  return NULL;
}
static TEST_QUEUE_SLOT *UsbTestFindQueue(WDFQUEUE Handle) {
  ULONG I;
  for(I=0;I<UsbTestQueueCount;I++) if(UsbTestQueues[I].Handle==Handle) return &UsbTestQueues[I];
  return NULL;
}
static BOOLEAN UsbTestRemoveQueuedRequest(WDFQUEUE Handle,WDFREQUEST Request) {
  TEST_QUEUE_SLOT *Q=UsbTestFindQueue(Handle);ULONG I,J;
  if(!Q)return FALSE;
  for(I=0;I<Q->Count;I++)if(Q->Requests[I]==Request) {
    for(J=I+1;J<Q->Count;J++)Q->Requests[J-1]=Q->Requests[J];
    Q->Count--;return TRUE;
  }
  return FALSE;
}
static PVOID UsbTestContext(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Handle,PCWDF_OBJECT_CONTEXT_TYPE_INFO TypeInfo) {
  ULONG I;UNREFERENCED_PARAMETER(Globals);UNREFERENCED_PARAMETER(TypeInfo);
  for(I=0;I<UsbTestContextCount;I++) if(UsbTestContexts[I].Handle==Handle) return UsbTestContexts[I].Context;
  {
    TEST_REQUEST_SLOT *R=UsbTestFindRequest((WDFREQUEST)Handle);
    if(R&&R->Allocated)return &R->Context;
  }
  CHECK(0);return NULL;
}
static TEST_QUEUE_SLOT *UsbTestMakeQueue(WDFQUEUE Handle,USB_QUEUE *Context) {
  TEST_QUEUE_SLOT *Q;
  CHECK(UsbTestQueueCount<TEST_QUEUES);if(UsbTestQueueCount>=TEST_QUEUES)return NULL;
  Q=&UsbTestQueues[UsbTestQueueCount++];memset(Q,0,sizeof(*Q));Q->Handle=Handle;
  if(Context)Q->Context=*Context;
  UsbTestAddContext((WDFOBJECT)Handle,&Q->Context);return Q;
}
static WDFREQUEST UsbTestNewRequest(ULONG Index,PVOID Argument) {
  TEST_REQUEST_SLOT *R;
  CHECK(Index<TEST_REQUESTS);if(Index>=TEST_REQUESTS)return NULL;
  R=&UsbTestRequests[Index];memset(R,0,sizeof(*R));
  R->Handle=(WDFREQUEST)(TEST_REQUEST_BASE+((ULONG_PTR)Index*0x10));R->Argument=Argument;
  if(UsbTestRequestCount<=Index)UsbTestRequestCount=Index+1;
  return R->Handle;
}
static VOID UsbTestInitController(VOID) {
  memset(&UsbTestController,0,sizeof(UsbTestController));
  memset(&UsbTestDevice,0,sizeof(UsbTestDevice));memset(&UsbTestHub,0,sizeof(UsbTestHub));
  memset(&UsbTestEndpoint,0,sizeof(UsbTestEndpoint));memset(&UsbTestUcxObject,0,sizeof(UsbTestUcxObject));
  memset(&UsbTestRootObject,0,sizeof(UsbTestRootObject));memset(&UsbTestWorkObject,0,sizeof(UsbTestWorkObject));
  memset(&UsbTestTimerObject,0,sizeof(UsbTestTimerObject));memset(&UsbTestManagementContext,0,sizeof(UsbTestManagementContext));
  memset(&UsbTestRootInterruptContext,0,sizeof(UsbTestRootInterruptContext));
  memset(UsbTestContexts,0,sizeof(UsbTestContexts));memset(UsbTestQueues,0,sizeof(UsbTestQueues));
  memset(UsbTestRequests,0,sizeof(UsbTestRequests));memset(UsbTestStops,0,sizeof(UsbTestStops));
  UsbTestContextCount=UsbTestQueueCount=UsbTestRequestCount=UsbTestStopCount=0;
  UsbTestQueueCreates=UsbTestQueueCreateFailAt=0;UsbTestQueueCreateStatus=STATUS_INSUFFICIENT_RESOURCES;
  UsbTestAddActive=UsbTestInterfaceCalls=UsbTestUcxCreates=UsbTestRootCreates=UsbTestDefaultQueues=0;
  UsbTestInterfaceStatus=STATUS_SUCCESS;UsbTestIoctlCalls=0;UsbTestIoctlHandled=FALSE;
  UsbTestDeviceCreates=UsbTestDeviceCallbacks=0;UsbTestDeviceCreateStatus=STATUS_SUCCESS;
  memset(&UsbTestPowerCallbacks,0,sizeof(UsbTestPowerCallbacks));
  // Reset the whole aperture without a 64 KiB byte loop for every test case.
  for(ULONG I=0;I<ARRAYSIZE(UsbTestRegisterPage.Words);++I) UsbTestRegisterPage.Words[I]=0;
  memset(UsbTestHandoff,0,sizeof(UsbTestHandoff));
  memset(UsbTestRaw,0,sizeof(UsbTestRaw));memset(UsbTestTranslated,0,sizeof(UsbTestTranslated));
  UsbTestRawCount=UsbTestTranslatedCount=UsbTestMapCalls=UsbTestUnmapCalls=UsbTestMapFailAt=0;
  UsbTestMmioReads=UsbTestMmioWrites=UsbTestLastWriteOffset=0;
  UsbTestDropPowerWrite=UsbTestDropSessionWrite=FALSE;UsbTestUseRealPort=UsbTestUseRealCoreData=FALSE;
  UsbTestFailColdMode=UsbTestColdModeFailureObserved=FALSE;
  UsbTestDevctlWrites=0;
  UsbTestPhyWriteCount=UsbTestDropPhyWriteAt=UsbTestLoseHostAt=0;
  UsbTestPoolInUse=FALSE;UsbTestMdlMapFail=NULL;UsbTestMdlMapCalls=UsbTestExpectedProbeMaps=0;
  UsbTestClock=100000;UsbTestWdfLockDepth=UsbTestIrql=UsbTestDpcQueued=UsbTestDpcFlushes=0;UsbTestEndpointLockHeld=FALSE;
  UsbTestWorkEnqueues=UsbTestWorkFlushes=UsbTestTimerStarts=UsbTestTimerStops=0;
  UsbTestQueueStarts=UsbTestQueuePurges=UsbTestForwardCalls=UsbTestRequeueCalls=0;
  UsbTestQueueStartDispatchRequest=UsbTestReentrantDisableRequest=NULL;
  UsbTestQueueStartDispatchCode=UsbTestQueueStartDispatches=0;
  UsbTestReenterD0ExitOnQueueStart=UsbTestReenterD0ExitOnQueueStop=UsbTestReenterControllerResetOnQueueStart=FALSE;
  UsbTestReenterDisableOnRequestComplete=FALSE;
  UsbTestReentrantD0ExitStatus=STATUS_UNSUCCESSFUL;
  UsbTestReenterWorker=FALSE;
  UsbTestResetCompleteCalls=UsbTestResetSequence=UsbTestResetFailedOrder=UsbTestResetCompleteOrder=0;
  UsbTestResetCase=ResetPogo;UsbTestSwResetWrites=UsbTestResetWaits=0;
  UsbTestResetCanDrain=FALSE;
  UsbTestMarkCalls=UsbTestUnmarkCalls=UsbTestReferenceCalls=UsbTestDereferenceCalls=0;
  UsbTestEndpointReferences=UsbTestDeviceReferences=0;UsbTestMdlMapsAtCore=UsbTestCoreInitCalls=UsbTestCoreAdoptCalls=UsbTestCoreStateCalls=0;
  UsbTestPortCalls=UsbTestPortFeatureCalls=UsbTestControlCalls=UsbTestDataCalls=UsbTestRealDataCalls=0;
  UsbTestRealCoreStatus=MusbOk;
  UsbTestRealCoreBytes=0;
  UsbTestLastTransferType=0U;
  UsbTestStallCalls=UsbTestDelayCalls=0;UsbTestDelayInterval=0;
  UsbTestUcxRootChanged=UsbTestUcxPurgeDoneCount=UsbTestUcxAbortDoneCount=0;
  UsbTestUcxEndpointCreateCalls=UsbTestUcxQueueBindCalls=UsbTestEndpointCallbacks=0;
  UsbTestPurgeCalls=UsbTestDirectCompletes=UsbTestDpcCompletes=0;
  UsbTestAcpiCalls=UsbTestAcpiVariant=0;
  UsbTestTypecCalls=UsbTestTypecSample=0;
  memset(&UsbTestTcpc,0,sizeof(UsbTestTcpc));
  memset(&UsbTestInterrupt,0,sizeof(UsbTestInterrupt));
  UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=0x05020c;
  UsbTestTypecStatus=STATUS_OBJECT_NAME_NOT_FOUND;
  UsbTestRouteCalls=UsbTestRouteArgument=UsbTestRouteReply=0;
  UsbTestRoutePerPath=FALSE;
  memset(UsbTestRouteReplies,0,sizeof(UsbTestRouteReplies));
  UsbTestRouteStatus=STATUS_OBJECT_NAME_NOT_FOUND;
  UsbTestTypecBadReply=FALSE;
  UsbTestClockCalls=UsbTestAckCalls=0;
  UsbTestClockIdentity=199682;
  UsbTestClockStatus=STATUS_SUCCESS;
  UsbTestGateStatus=STATUS_OBJECT_NAME_NOT_FOUND;
  UsbTestGateCalls=UsbTestGateArgument=UsbTestGateReply=0;
  UsbTestGateLatches=UsbTestGateBadReply=FALSE;
  UsbTestIdleCalls=UsbTestFifoCalls=UsbTestFifoRestores=UsbTestControllerFailed=0;
  UsbTestTakeOverCalls=0;UsbTestTakeOverResult=MtkMusbPlatformSuccess;
  UsbTestFifoRestoreResult=MtkMusbPlatformIoError;
  UsbTestIdleResult=UsbTestFifoResult=MtkMusbPlatformSuccess;UsbTestExpectedCoreFlags=MUSB_READY_REQUIRED;
  UsbTestDevctlAfterFifo=0;
  UsbTestDropVbusOnAck=FALSE;
  UsbTestExpectedPortStatus=UsbTestExpectedPortChange=0;
  UsbTestExpectedRoute=UsbTestExpectedHubAddress=UsbTestExpectedHubPort=UsbTestExpectedMultiTt=0;
  UsbTestExpectedTransferLength=UsbTestExpectedTimeout=UsbTestExpectedEndpointAddress=0;
  memset(UsbTestExpectedOut,0,sizeof(UsbTestExpectedOut));
  memset(UsbTestLastSetup,0,sizeof(UsbTestLastSetup));UsbTestLastControlAddress=0;
  UsbTestCoreResult=MusbOk;UsbTestCoreInitResult=MusbOk;UsbTestCoreStateResult=MusbOk;
  UsbTestCoreDone=UsbTestCancelInCore=UsbTestCancelAtUnmark=UsbTestExpectCoreData=0;
  UsbTestExpectedAdoptStatus=UsbTestExpectedAdoptChange=0;
  UsbTestCurrentCancel=NULL;UsbTestEndpointEventsSeen=UsbTestDefaultEndpointEventsSeen=FALSE;
  memset(&UsbTestEndpointEvents,0,sizeof(UsbTestEndpointEvents));
  memset(&UsbTestDefaultEndpointEvents,0,sizeof(UsbTestDefaultEndpointEvents));
  UsbTestLastAllocatedRequest=NULL;UsbTestLastRequestHandle=0;UsbTestLastEndpointQueueMode=0;
  UsbTestController.Device=TEST_DEVICE;UsbTestController.Ucx=TEST_UCX;UsbTestController.RootHub=TEST_ROOTHUB;
  UsbTestController.Worker=TEST_WORKER;UsbTestController.Timer=TEST_TIMER;
  UsbTestController.Management=TEST_MANAGEMENT;UsbTestController.RootInterrupt=TEST_ROOT_INTERRUPT;
  UsbTestController.EndpointLock=(WDFSPINLOCK)(ULONG_PTR)0x303;
  UsbTestController.Online=1;UsbTestController.Stopping=0;
  KeInitializeEvent(&UsbTestController.QueueStartsDrained,NotificationEvent,TRUE);
  KeInitializeEvent(&UsbTestController.ResetDrained,NotificationEvent,TRUE);
  InitializeListHead(&UsbTestController.Endpoints);InitializeListHead(&UsbTestController.QueueStartingEndpoints);
  InitializeListHead(&UsbTestController.Completions);
  UsbTestInitializeSpinLock(&UsbTestController.CompletionLock);
  UsbTestUcxObject.Controller=&UsbTestController;UsbTestRootObject.Controller=&UsbTestController;
  UsbTestWorkObject.Controller=&UsbTestController;UsbTestTimerObject.Controller=&UsbTestController;
  UsbTestManagementContext.Controller=&UsbTestController;UsbTestRootInterruptContext.Controller=&UsbTestController;
  UsbTestAddContext((WDFOBJECT)TEST_DEVICE,&UsbTestController);
  UsbTestAddContext((WDFOBJECT)TEST_UCX,&UsbTestUcxObject);
  UsbTestAddContext((WDFOBJECT)TEST_ROOTHUB,&UsbTestRootObject);
  UsbTestAddContext((WDFOBJECT)TEST_WORKER,&UsbTestWorkObject);
  UsbTestAddContext((WDFOBJECT)TEST_TIMER,&UsbTestTimerObject);
  UsbTestMakeQueue(TEST_MANAGEMENT,&UsbTestManagementContext)->Started=TRUE;
  UsbTestMakeQueue(TEST_ROOT_INTERRUPT,&UsbTestRootInterruptContext)->Started=TRUE;
}
static VOID UsbTestInitEndpoint(UCHAR Type,UCHAR Address,USHORT Packet,UCHAR Interval) {
  memset(&UsbTestDevice,0,sizeof(UsbTestDevice));memset(&UsbTestHub,0,sizeof(UsbTestHub));
  UsbTestDevice.Controller=&UsbTestController;UsbTestDevice.Handle=TEST_USBDEVICE;
  UsbTestDevice.Speed=UsbFullSpeed;UsbTestDevice.Enabled=1;
  UsbTestDevice.Path.PortPathDepth=2;UsbTestDevice.Path.TTHubDepth=1;
  UsbTestDevice.Path.PortPath[0]=1;UsbTestDevice.Path.PortPath[1]=3;
  UsbTestHub.Controller=&UsbTestController;UsbTestHub.Handle=TEST_TT_HUB;
  UsbTestHub.Address=5;UsbTestHub.Speed=UsbHighSpeed;UsbTestHub.MultiTt=TRUE;
  UsbTestAddContext((WDFOBJECT)TEST_USBDEVICE,&UsbTestDevice);
  UsbTestAddContext((WDFOBJECT)TEST_TT_HUB,&UsbTestHub);
  UsbTestController.Stopping=0;UsbTestController.Online=1;
  UsbTestEndpoint.Controller=&UsbTestController;UsbTestEndpoint.Device=&UsbTestDevice;
  UsbTestEndpoint.Handle=TEST_ENDPOINT;UsbTestEndpoint.Address=Address;UsbTestEndpoint.Type=Type;
  UsbTestEndpoint.MaximumPacket=Packet;UsbTestEndpoint.Interval=Interval;
  UsbTestEndpoint.PollUs=UsbTestDevice.Speed==UsbHighSpeed?(125ULL<<(Interval-1)):(ULONGLONG)Interval*1000;
  UsbTestEndpoint.Stopped=0;
  InitializeListHead(&UsbTestEndpoint.StartLink);
  UsbTestAddContext((WDFOBJECT)TEST_ENDPOINT,&UsbTestEndpoint);
}

static UCHAR UsbTestRead8(volatile UCHAR *Address) {
  ULONG_PTR A=(ULONG_PTR)Address,B=(ULONG_PTR)UsbTestRegisters;
  CHECK(A>=B&&A<B+sizeof(UsbTestRegisters));UsbTestMmioReads++;return *Address;
}
static USHORT UsbTestRead16(volatile USHORT *Address) {
  ULONG_PTR A=(ULONG_PTR)Address,B=(ULONG_PTR)UsbTestRegisters;
  if(A==B+0x0c) CHECK(UsbTestEndpointLockHeld);
  CHECK(A>=B&&A+sizeof(*Address)<=B+sizeof(UsbTestRegisters));UsbTestMmioReads++;return *Address;
}
static ULONG UsbTestRead32(PULONG Address) {
  ULONG_PTR A=(ULONG_PTR)Address,B=(ULONG_PTR)UsbTestHandoff;
  if(A>=B&&A+sizeof(*Address)<=B+sizeof(UsbTestHandoff)){UsbTestMmioReads++;return *Address;}
  B=(ULONG_PTR)UsbTestPhy;
  if(A>=B&&A+sizeof(*Address)<=B+sizeof(UsbTestPhy)){UsbTestMmioReads++;return *Address;}
  B=(ULONG_PTR)UsbTestRegisters;CHECK(A>=B&&A+sizeof(*Address)<=B+sizeof(UsbTestRegisters));
  UsbTestMmioReads++;return *Address;
}
static VOID UsbTestWrite8(volatile UCHAR *Address,UCHAR Value) {
  ULONG_PTR A=(ULONG_PTR)Address,B=(ULONG_PTR)UsbTestRegisters;
  CHECK(A>=B&&A<B+sizeof(UsbTestRegisters));UsbTestMmioWrites++;UsbTestLastWriteOffset=(ULONG)(A-B);
  if(UsbTestLastWriteOffset==USB_REG_POWER) {
    CHECK(UsbTestController.CoreLock);
    CHECK((UsbTestController.Stopping&&!UsbTestController.Online) ||
          (UsbTestUseRealPort&&UsbTestController.WorkerThread&&!UsbTestController.Stopping));
    if(UsbTestDropPowerWrite) return;
  }
  if(UsbTestLastWriteOffset==USB_DEVCTL_REGISTER) {
    USB_CONTROLLER *D=&UsbTestController;
    CHECK(D->CoreLock && D->Core.State==MUSB_HOST_STATE_HALTED);
    CHECK(!D->Online || (D->WorkerThread && D->WaitingForVbus &&
        !D->InterruptUnmasked &&
        (D->ColdStartPending?D->InterruptOwned:!D->InterruptOwned)) ||
        (D->ColdStartOwned&&D->Stopping&&!D->InterruptUnmasked));
    CHECK(((*Address^Value)==USB_DEVCTL_SESSION) ||
          ((D->ColdStartPending||D->ColdStartOwned)&&*Address==Value));
    UsbTestDevctlWrites++;
    if(UsbTestDropSessionWrite) return;
  }
  if(UsbTestLastWriteOffset==0x0a) {
    CHECK(UsbTestIrql==PASSIVE_LEVEL);
    *Address&=(UCHAR)~Value; // INTRUSB is write-one-to-clear on this controller.
    return;
  }
  if(UsbTestLastWriteOffset==0x0b && UsbTestIrql==TEST_USB_DIRQL &&
      !Value && UsbTestInterrupt.DropMaskWrite) return;
  *Address=Value;
}
static VOID UsbTestWrite16(volatile USHORT *Address,USHORT Value) {
  ULONG_PTR A=(ULONG_PTR)Address,B=(ULONG_PTR)UsbTestRegisters;
  CHECK(A>=B&&A+sizeof(*Address)<=B+sizeof(UsbTestRegisters));
  UsbTestMmioWrites++;UsbTestLastWriteOffset=(ULONG)(A-B);
  if(UsbTestUseRealCoreData&&A==B+0x12) {
    *Address=0; // Model a completed PIO packet and an empty FIFO.
    return;
  }
  *Address=Value;
  if(A==B+0x74) {
    BOOLEAN ColdWorker=UsbTestController.ColdStartPending&&
        UsbTestController.ColdStartOwned&&UsbTestController.WaitingForVbus&&
        UsbTestController.WorkerThread==UsbTestThread&&!UsbTestController.Stopping;
    UsbTestSwResetWrites++;
    CHECK((ColdWorker?Value==3U:Value==0x610b)&&UsbTestController.CoreLock&&
        UsbTestController.Core.State==MUSB_HOST_STATE_HALTED&&
        ((UsbTestController.ResetThread&&UsbTestController.Stopping&&
          !UsbTestController.Online)||ColdWorker));
    CHECK(!(*(PULONG)(UsbTestRegisters+USB_REG_L1INTM)&USB_L1_IRQ_GROUPS));
    if(UsbTestResetCase==ResetTypecLostFifo) {
      UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
      UsbTestFifoRestoreResult=MtkMusbPlatformSuccess;
      UsbTestRegisters[USB_REG_POWER]=0;
    }
  }
}
static VOID UsbTestWrite32(PULONG Address,ULONG Value) {
  ULONG_PTR A=(ULONG_PTR)Address,B=(ULONG_PTR)UsbTestPhy;
  if(A>=B&&A+sizeof(*Address)<=B+sizeof(UsbTestPhy)) {
    BOOLEAN ColdWorker=UsbTestController.ColdStartOwned&&
        UsbTestController.Core.State==MUSB_HOST_STATE_HALTED&&
        UsbTestIrql==PASSIVE_LEVEL&&
        ((UsbTestController.ColdStartPending&&UsbTestController.WaitingForVbus&&
          UsbTestController.WorkerThread==UsbTestThread&&!UsbTestController.Stopping)||
         UsbTestController.Stopping);
    if(ColdWorker) {
      *Address=Value;
    } else {
      CHECK(UsbTestController.CoreLock&&UsbTestController.Stopping&&!UsbTestController.Online);
      CHECK(UsbTestPhyWriteCount<ARRAYSIZE(UsbTestPhyWriteOffsets));
      UsbTestPhyWriteOffsets[UsbTestPhyWriteCount++]=(ULONG)(A-B);
      if(UsbTestPhyWriteCount!=UsbTestDropPhyWriteAt)*Address=Value;
      if(UsbTestPhyWriteCount==UsbTestLoseHostAt)UsbTestRegisters[0x60]=0x99;
    }
    return;
  }
  B=(ULONG_PTR)UsbTestRegisters;
  CHECK(A>=B&&A+sizeof(*Address)<=B+sizeof(UsbTestRegisters));
  UsbTestMmioWrites++;UsbTestLastWriteOffset=(ULONG)(A-B);*Address=Value;
}
static PVOID UsbTestMapIoSpace(PHYSICAL_ADDRESS Address,SIZE_T Length,ULONG Flags) {
  UsbTestMapCalls++;if(UsbTestMapCalls==UsbTestMapFailAt)return NULL;
  if(Address.QuadPart==USB_BASE){CHECK(Length==USB_BYTES&&Flags==(PAGE_READWRITE|PAGE_NOCACHE));return UsbTestRegisters;}
  if(Address.QuadPart==MTK_MUSB_PLATFORM_PHY_BASE) {
    CHECK((Length==MTK_MUSB_PLATFORM_PHY_BYTES&&Flags==(PAGE_READONLY|PAGE_NOCACHE)) ||
          (Length==MTK_MUSB_PLATFORM_PHY_REG_BYTES&&Flags==(PAGE_READWRITE|PAGE_NOCACHE)));
    return UsbTestPhy;
  }
  CHECK(Address.QuadPart==USB_HANDOFF&&Length==USB_HANDOFF_BYTES&&Flags==(PAGE_READONLY|PAGE_NOCACHE));
  return UsbTestHandoff;
}
static VOID UsbTestUnmapIoSpace(PVOID Base,SIZE_T Length) {
  CHECK((Base==UsbTestRegisters&&Length==USB_BYTES)||(Base==UsbTestHandoff&&Length==USB_HANDOFF_BYTES)||
        (Base==UsbTestPhy&&(Length==MTK_MUSB_PLATFORM_PHY_BYTES||Length==MTK_MUSB_PLATFORM_PHY_REG_BYTES)));
  UsbTestUnmapCalls++;
}
static PVOID UsbTestMdlMap(PMDL Mdl,ULONG Priority) {
  UNREFERENCED_PARAMETER(Priority);UsbTestMdlMapCalls++;
  if(Mdl==UsbTestMdlMapFail)return NULL;return Mdl?Mdl->MappedSystemVa:NULL;
}
static PVOID UsbTestPoolAlloc(POOL_FLAGS Flags,SIZE_T Bytes,ULONG Tag) {
  CHECK(Flags==POOL_FLAG_NON_PAGED&&Tag==USB_TAG&&Bytes<=sizeof(UsbTestPool)&&!UsbTestPoolInUse);
  if(Bytes>sizeof(UsbTestPool)||UsbTestPoolInUse)return NULL;
  UsbTestPoolInUse=TRUE;memset(UsbTestPool,0,Bytes);return UsbTestPool;
}
static VOID UsbTestPoolFree(PVOID Buffer,ULONG Tag) {
  CHECK(Buffer==UsbTestPool&&Tag==USB_TAG&&UsbTestPoolInUse);UsbTestPoolInUse=FALSE;
}
static ULONGLONG UsbTestQueryTime(PULONGLONG Time) {
  ULONGLONG T=(ULONGLONG)UsbTestClock*10U;if(Time)*Time=T;return T;
}
static VOID UsbTestStall(ULONG Microseconds) {
  CHECK(Microseconds<=50);++UsbTestStallCalls;
}
static NTSTATUS UsbTestDelayThread(KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Interval) {
  CHECK(Mode==KernelMode&&!Alertable&&Interval->QuadPart<0);
  CHECK(UsbTestIrql==PASSIVE_LEVEL&&!UsbTestEndpointLockHeld);
  ++UsbTestDelayCalls;
  UsbTestDelayInterval=Interval->QuadPart;
  if(UsbTestController.ResetThread) {
    CHECK(Interval->QuadPart==-30000 && !UsbTestController.CoreLock &&
          UsbTestController.ResetDrained.Header.SignalState==0);
    if(UsbTestResetCase==ResetClockLost) UsbTestClockIdentity=0;
    if(UsbTestResetCase==ResetVbusLost) UsbTestRegisters[USB_DEVCTL_REGISTER]&=(UCHAR)~0x18;
    if(UsbTestResetCase==ResetStopping) {
      WdfSpinLockAcquire(UsbTestController.EndpointLock);
      UsbTestController.LeavingD0=TRUE;
      WdfSpinLockRelease(UsbTestController.EndpointLock);
    }
    if(UsbTestResetCase==ResetReentrantPower)
      UsbTestReentrantD0ExitStatus=UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3);
  }
  return STATUS_SUCCESS;
}
static VOID UsbTestInitializeEvent(PRKEVENT Event,EVENT_TYPE Type,BOOLEAN State) {
  memset(Event,0,sizeof(*Event));Event->Header.Type=(UCHAR)Type;Event->Header.SignalState=State?1:0;
}
static LONG UsbTestSetEvent(PRKEVENT Event,KPRIORITY Priority,BOOLEAN Wait) {
  LONG Old=Event->Header.SignalState;UNREFERENCED_PARAMETER(Priority);UNREFERENCED_PARAMETER(Wait);
  Event->Header.SignalState=1;return Old;
}
static VOID UsbTestClearEvent(PRKEVENT Event) { Event->Header.SignalState=0; }
static NTSTATUS UsbTestWaitEvent(PVOID Object,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Timeout) {
  PRKEVENT Event=(PRKEVENT)Object;
  CHECK(Reason==Executive&&Mode==KernelMode&&!Alertable&&!Timeout);
  if(Event==&UsbTestController.ResetDrained && UsbTestResetCanDrain) {
    CHECK(!UsbTestEndpointLockHeld && UsbTestIrql==PASSIVE_LEVEL && !Event->Header.SignalState);
    CHECK(UsbTestController.ResetThread && UsbTestController.LeavingD0 &&
          UsbTestController.Stopping && UsbTestController.InterruptReady);
    // Let the simulated reset thread release its pin while PnP waits.
    UsbTestResetWaits++;
    UsbTestController.ResetThread=NULL;
    UsbTestSetEvent(Event,IO_NO_INCREMENT,FALSE);
  }
  CHECK(Event->Header.SignalState==1);
  return Event->Header.SignalState?STATUS_SUCCESS:STATUS_TIMEOUT;
}
static NTSTATUS UsbTestCreateRegistryKey(ULONG Root,PWSTR Path) {
  UNREFERENCED_PARAMETER(Root);UNREFERENCED_PARAMETER(Path);return STATUS_SUCCESS;
}
static NTSTATUS UsbTestWriteRegistryValue(ULONG Root,PCWSTR Path,PCWSTR Name,ULONG Type,PVOID Data,ULONG Bytes) {
  UNREFERENCED_PARAMETER(Root);UNREFERENCED_PARAMETER(Path);UNREFERENCED_PARAMETER(Name);
  UNREFERENCED_PARAMETER(Type);UNREFERENCED_PARAMETER(Data);UNREFERENCED_PARAMETER(Bytes);return STATUS_SUCCESS;
}
static KIRQL UsbTestGetIrql(VOID) { return (KIRQL)UsbTestIrql; }
static VOID UsbTestRaiseIrql(KIRQL NewIrql,PKIRQL OldIrql) { *OldIrql=(KIRQL)UsbTestIrql;UsbTestIrql=NewIrql; }
static VOID UsbTestLowerIrql(KIRQL NewIrql) { UsbTestIrql=NewIrql; }
static PETHREAD UsbTestGetCurrentThread(VOID) { return UsbTestThread; }
static VOID UsbTestInitializeSpinLock(PKSPIN_LOCK Lock) { *Lock=0; }
static VOID UsbTestInitializeDpc(PRKDPC Dpc,PKDEFERRED_ROUTINE Routine,PVOID Context) {
  CHECK(Routine==UsbCompletionDpc&&Context==&UsbTestController);memset(Dpc,0,sizeof(*Dpc));
}
static VOID UsbTestAcquireSpinLock(PKSPIN_LOCK Lock,PKIRQL OldIrql) {
  *OldIrql=(KIRQL)UsbTestIrql;CHECK(*Lock==0);*Lock=1;UsbTestIrql=DISPATCH_LEVEL;
}
static VOID UsbTestReleaseSpinLock(PKSPIN_LOCK Lock,KIRQL OldIrql) {
  CHECK(*Lock==1);*Lock=0;UsbTestIrql=OldIrql;
}
static VOID UsbTestAcquireSpinLockAtDpc(PKSPIN_LOCK Lock) { CHECK(*Lock==0);*Lock=1; }
static VOID UsbTestReleaseSpinLockFromDpc(PKSPIN_LOCK Lock) { CHECK(*Lock==1);*Lock=0; }
static BOOLEAN UsbTestInsertQueueDpc(PKDPC Dpc,PVOID Arg1,PVOID Arg2) {
  UNREFERENCED_PARAMETER(Dpc);UNREFERENCED_PARAMETER(Arg1);UNREFERENCED_PARAMETER(Arg2);
  UsbTestDpcQueued++;return TRUE;
}
static VOID UsbTestFlushDpcs(VOID) { UsbTestDpcFlushes++; }
static LONG UsbTestCompareExchange(volatile LONG *Target,LONG Exchange,LONG Compare) {
  LONG Old=*Target;if(Old==Compare)*Target=Exchange;return Old;
}
static LONG UsbTestExchange(volatile LONG *Target,LONG Value) { LONG Old=*Target;*Target=Value;return Old; }
static LONG UsbTestIncrement(volatile LONG *Target) { return ++(*Target); }
static LONG UsbTestDecrement(volatile LONG *Target) { return --(*Target); }

MUSB_STATUS UsbTestMusbHostInitialize(MUSB_HOST *Host,const MUSB_HOST_IO *Io,const MUSB_HOST_CONFIG *Config) {
  ULONG Enqueues=UsbTestWorkEnqueues;
  UsbTestCoreInitCalls++;CHECK(Host==&UsbTestController.Core&&Io&&Config);
  if(UsbTestController.Online)
    CHECK(UsbTestController.WaitingForVbus&&UsbTestController.WorkerThread&&!Io->Cancelled(Io->Context));
  else
    CHECK(Io->Cancelled(Io->Context));
  UsbTimer(TEST_TIMER);
  CHECK(UsbTestWorkEnqueues==Enqueues);
  CHECK(Config->Revision==1&&Config->Flags==UsbTestExpectedCoreFlags&&Config->ControllerLength==USB_BYTES);
  CHECK(Config->PhyLength==USB_BYTES&&Config->InterruptVector==USB_GSIV&&Config->EndpointCount==16);
  CHECK(Config->PortCount==1&&Config->FifoProfile==MUSB_HOST_T70_FIFO_PROFILE&&Config->InterruptMask==(MUSB_HOST_CONNECT_DISCONNECT_MASK|MUSB_HOST_RESUME_MASK));
  if(UsbTestCoreInitResult!=MusbOk)return UsbTestCoreInitResult;
  Host->Io=*Io;Host->Config=*Config;Host->Initialized=1;Host->State=MUSB_HOST_STATE_HALTED;return MusbOk;
}
MUSB_STATUS UsbTestMusbHostAdoptPortState(MUSB_HOST *Host,uint16_t Status,uint16_t Change) {
  UsbTestCoreAdoptCalls++;CHECK(Host==&UsbTestController.Core);
  CHECK(Status==UsbTestExpectedAdoptStatus&&Change==UsbTestExpectedAdoptChange);
  Host->PortStatus=Status;Host->PortChangeStatus=Change;return MusbOk;
}
MUSB_STATUS UsbTestMusbHostSetState(MUSB_HOST *Host,uint8_t State) {
  UsbTestCoreStateCalls++;CHECK(Host==&UsbTestController.Core);
  CHECK(State==MUSB_HOST_STATE_OPERATIONAL||State==MUSB_HOST_STATE_HALTED);
  if(State==MUSB_HOST_STATE_OPERATIONAL)
    CHECK(!UsbTestController.Online ||
          (UsbTestController.WaitingForVbus && UsbTestController.WorkerThread));
  if(UsbTestCoreStateResult!=MusbOk)return UsbTestCoreStateResult;
  Host->State=State;return MusbOk;
}
MUSB_STATUS UsbTestMusbHostGetPortStatus(MUSB_HOST *Host,uint8_t Port,uint16_t *Status,uint16_t *Change) {
  UsbTestPortCalls++;CHECK(Host==&UsbTestController.Core&&Port==0&&Status&&Change);
  if(UsbTestReenterWorker) {
    PETHREAD OriginalThread=UsbTestThread;
    UsbTestReenterWorker=FALSE;
    UsbTimer(TEST_TIMER);
    UsbTestThread=(PETHREAD)(ULONG_PTR)0x708;
    UsbWork(TEST_WORKER);
    UsbTestThread=OriginalThread;
    UsbTestReentrantD0ExitStatus=UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3);
  }
  if(UsbTestUseRealPort) return MusbHostGetPortStatus(Host,Port,Status,Change);
  *Status=(uint16_t)UsbTestExpectedPortStatus;*Change=(uint16_t)UsbTestExpectedPortChange;return UsbTestCoreResult;
}
MUSB_STATUS UsbTestMusbHostSetPortFeature(MUSB_HOST *Host,uint8_t Port,uint8_t Feature) {
  UsbTestPortFeatureCalls++;CHECK(Host==&UsbTestController.Core&&Port==0);
  if(UsbTestUseRealPort) return MusbHostSetPortFeature(Host,Port,Feature);
  CHECK(Feature==MUSB_PORT_FEATURE_RESET);
  if(UsbTestCoreResult==MusbOk&&Host->Io.DelayUs) Host->Io.DelayUs(Host->Io.Context,50000U);
  return UsbTestCoreResult;
}
MUSB_STATUS UsbTestMusbHostClearPortFeature(MUSB_HOST *Host,uint8_t Port,uint8_t Feature) {
  UsbTestPortFeatureCalls++;CHECK(Host==&UsbTestController.Core&&Port==0);
  if(UsbTestUseRealPort) return MusbHostClearPortFeature(Host,Port,Feature);
  return UsbTestCoreResult;
}
MUSB_STATUS UsbTestMusbHostControlTransferRouted(MUSB_HOST *Host,uint8_t Address,uint8_t Speed,uint8_t Packet,
    const uint8_t Setup[8],void *Data,uint32_t Capacity,uint32_t Timeout,const MUSB_HOST_ROUTE *Route,uint32_t *Done) {
  UsbTestControlCalls++;CHECK(Host==&UsbTestController.Core&&Setup&&Route&&Done);
  memcpy(UsbTestLastSetup,Setup,8);UsbTestLastControlAddress=Address;
  CHECK(Address<=127&&Speed<=MUSB_SPEED_HIGH&&Packet!=0&&Capacity<=USB_MAX_TRANSFER);
  CHECK(Timeout==UsbTestExpectedTimeout||UsbTestExpectedTimeout==0);
  if(UsbTestExpectedRoute) {
    CHECK(Route->HubAddress==UsbTestExpectedHubAddress&&Route->HubPort==UsbTestExpectedHubPort);
    CHECK(Route->MultiTt==UsbTestExpectedMultiTt&&Route->Reserved==0);
  }
  if(Data&&Capacity)memset(Data,0x5a,Capacity);
  *Done=UsbTestCoreDone;return UsbTestCoreResult;
}
MUSB_STATUS UsbTestMusbHostDataTransferRouted(MUSB_HOST *Host,uint8_t Type,uint8_t Address,uint8_t Endpoint,
    uint8_t Speed,uint16_t Packet,uint8_t Interval,void *Data,uint32_t Length,uint8_t *Toggle,
    uint32_t Timeout,const MUSB_HOST_ROUTE *Route,uint32_t *Done) {
  ULONG I;UsbTestDataCalls++;UsbTestMdlMapsAtCore=UsbTestMdlMapCalls;
  CHECK(Host==&UsbTestController.Core&&Route&&Done&&Toggle);
  if(UsbTestUseRealCoreData) {
    UsbTestRealDataCalls++;
    UsbTestRealCoreStatus=MusbHostDataTransferRouted(Host,Type,Address,Endpoint,Speed,Packet,
        Interval,Data,Length,Toggle,Timeout,Route,Done);
    if(Done)UsbTestRealCoreBytes=*Done;
    return UsbTestRealCoreStatus;
  }
  USB_ENDPOINT *Active=UsbTestController.ActiveEndpoint;
  CHECK(Active!=NULL);
  if(Active) {
    UCHAR ExpectedType=(Active->Type==USB_ENDPOINT_TYPE_INTERRUPT &&
        (Endpoint&USB_ENDPOINT_DIRECTION_MASK)!=0U) ?
        MUSB_TRANSFER_INTERRUPT_POLL : (UCHAR)Active->Type;
    CHECK(Type==ExpectedType);
  }
  UsbTestLastTransferType=Type;
  CHECK(Address<=127&&Endpoint==UsbTestExpectedEndpointAddress&&Speed<=MUSB_SPEED_HIGH);
  CHECK(Packet>0&&Length==UsbTestExpectedTransferLength&&Timeout==UsbTestExpectedTimeout);
  /* Interrupt IN polls are paced in software; the controller polls each frame. */
  CHECK(Type!=MUSB_TRANSFER_INTERRUPT_POLL||Interval==1);
  if(UsbTestExpectedRoute) {
    CHECK(Route->HubAddress==UsbTestExpectedHubAddress&&Route->HubPort==UsbTestExpectedHubPort);
    CHECK(Route->MultiTt==UsbTestExpectedMultiTt&&Route->Reserved==0);
  }
  if(UsbTestExpectCoreData) {
    CHECK(UsbTestMdlMapsAtCore>=UsbTestExpectedProbeMaps);CHECK(Data!=NULL);
    if(Data)for(I=0;I<Length&&I<sizeof(UsbTestExpectedOut);I++)CHECK(((UCHAR *)Data)[I]==UsbTestExpectedOut[I]);
  }
  if(UsbTestCoreResult==MusbOk&&Data&&Length&&(Endpoint&USB_ENDPOINT_DIRECTION_MASK))
    for(I=0;I<Length;I++)((UCHAR *)Data)[I]=(UCHAR)(0x40U+I);
  if(UsbTestCancelInCore&&UsbTestCurrentCancel) {
    UsbTestCancelInCore=0;UsbTestCurrentCancel((WDFREQUEST)UsbTestLastRequestHandle);
  }
  *Done=UsbTestCoreDone;return UsbTestCoreResult;
}

static VOID UsbTestUcxPortChanged(PUCX_DRIVER_GLOBALS Globals,UCXROOTHUB RootHub);
static VOID UsbTestUcxPurgeComplete(PUCX_DRIVER_GLOBALS Globals,UCXENDPOINT Endpoint);
static VOID UsbTestUcxAbortComplete(PUCX_DRIVER_GLOBALS Globals,UCXENDPOINT Endpoint);
static NTSTATUS UsbTestUcxEndpointCreate(PUCX_DRIVER_GLOBALS Globals,UCXUSBDEVICE Device,
    PUCXENDPOINT_INIT *Init,PWDF_OBJECT_ATTRIBUTES Attributes,UCXENDPOINT *Endpoint);
static VOID UsbTestUcxEndpointCallbacks(PUCX_DRIVER_GLOBALS Globals,PUCXENDPOINT_INIT Init,PUCX_ENDPOINT_EVENT_CALLBACKS Callbacks);
static VOID UsbTestUcxDefaultEndpointCallbacks(PUCX_DRIVER_GLOBALS Globals,PUCXENDPOINT_INIT Init,PUCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS Callbacks);
static VOID UsbTestUcxEndpointSetQueue(PUCX_DRIVER_GLOBALS Globals,UCXENDPOINT Endpoint,WDFQUEUE Queue);

static PVOID UsbTestWdfContext(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Object,PCWDF_OBJECT_CONTEXT_TYPE_INFO Info) {
  return UsbTestContext(Globals,Object,Info);
}
static NTSTATUS UsbTestObjectAllocateContext(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Object,
    PWDF_OBJECT_ATTRIBUTES Attributes,PVOID *Context) {
  TEST_REQUEST_SLOT *R=UsbTestFindRequest((WDFREQUEST)Object);
  UNREFERENCED_PARAMETER(Globals);UNREFERENCED_PARAMETER(Attributes);
  if(!R||!Context)return STATUS_INVALID_PARAMETER;
  *Context=&R->Context;UsbTestLastAllocatedRequest=&R->Context;UsbTestLastRequestHandle=(ULONG_PTR)Object;
  if(R->Allocated)return STATUS_OBJECT_NAME_EXISTS;
  R->Allocated=TRUE;return STATUS_SUCCESS;
}
static VOID UsbTestRequestGetParameters(PWDF_DRIVER_GLOBALS Globals,WDFREQUEST Request,PWDF_REQUEST_PARAMETERS Parameters) {
  TEST_REQUEST_SLOT *R=UsbTestFindRequest(Request);UNREFERENCED_PARAMETER(Globals);
  CHECK(R&&Parameters);
  if(R&&Parameters){memset(Parameters,0,sizeof(*Parameters));Parameters->Parameters.Others.Arg1=R->Argument;}
}
static VOID UsbTestRequestComplete(PWDF_DRIVER_GLOBALS Globals,WDFREQUEST Request,NTSTATUS Status) {
  TEST_REQUEST_SLOT *R=UsbTestFindRequest(Request);UNREFERENCED_PARAMETER(Globals);
  UsbTestDirectCompletes++;CHECK(R&&R->CompletionCount==0);
  if(R){R->CompletionCount++;R->CompletionStatus=Status;}
  if(UsbTestReenterDisableOnRequestComplete) {
    UsbTestReenterDisableOnRequestComplete=FALSE;
    CHECK(!UsbTestEndpointLockHeld&&UsbTestReentrantDisableRequest!=NULL);
    if(UsbTestReentrantDisableRequest)UsbDisable(TEST_UCX,UsbTestReentrantDisableRequest);
  }
}
static VOID UsbTestRequestCompleteWithInformation(PWDF_DRIVER_GLOBALS Globals,WDFREQUEST Request,NTSTATUS Status,ULONG_PTR Information) {
  TEST_REQUEST_SLOT *R=UsbTestFindRequest(Request);UNREFERENCED_PARAMETER(Globals);
  UsbTestDpcCompletes++;CHECK(R&&R->CompletionCount==0&&UsbTestIrql==DISPATCH_LEVEL);
  if(R){R->CompletionCount++;R->CompletionStatus=Status;R->CompletionInformation=Information;}
}
static NTSTATUS UsbTestRequestForward(PWDF_DRIVER_GLOBALS Globals,WDFREQUEST Request,WDFQUEUE Queue) {
  TEST_QUEUE_SLOT *Q=UsbTestFindQueue(Queue);UNREFERENCED_PARAMETER(Globals);
  UsbTestForwardCalls++;CHECK(Q&&Q->Count<TEST_QUEUE_DEPTH);
  if(!Q||Q->Count>=TEST_QUEUE_DEPTH)return STATUS_INSUFFICIENT_RESOURCES;
  Q->Requests[Q->Count++]=Request;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestQueueRetrieve(PWDF_DRIVER_GLOBALS Globals,WDFQUEUE Queue,WDFREQUEST *Request) {
  TEST_QUEUE_SLOT *Q=UsbTestFindQueue(Queue);UNREFERENCED_PARAMETER(Globals);
  if(!Q||!Request||!Q->Started||!Q->Count)return STATUS_NO_MORE_ENTRIES;
  *Request=Q->Requests[0];for(ULONG I=1;I<Q->Count;I++)Q->Requests[I-1]=Q->Requests[I];Q->Count--;
  {TEST_REQUEST_SLOT *R=UsbTestFindRequest(*Request);if(R)R->OriginQueue=Queue;}
  return STATUS_SUCCESS;
}
static NTSTATUS UsbTestRequestMarkCancelable(PWDF_DRIVER_GLOBALS Globals,WDFREQUEST Request,PFN_WDF_REQUEST_CANCEL Cancel) {
  TEST_REQUEST_SLOT *R=UsbTestFindRequest(Request);UNREFERENCED_PARAMETER(Globals);
  UsbTestMarkCalls++;CHECK(R&&Cancel);if(!R)return STATUS_INVALID_PARAMETER;
  R->Cancel=Cancel;UsbTestCurrentCancel=Cancel;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestRequestUnmarkCancelable(PWDF_DRIVER_GLOBALS Globals,WDFREQUEST Request) {
  TEST_REQUEST_SLOT *R=UsbTestFindRequest(Request);UNREFERENCED_PARAMETER(Globals);
  UsbTestUnmarkCalls++;CHECK(R!=NULL);if(!R)return STATUS_INVALID_PARAMETER;
  if(UsbTestCancelAtUnmark&&R->Cancel) {UsbTestCancelAtUnmark=0;R->Cancel(Request);return STATUS_CANCELLED;}
  if(R->Cancelled||R->Context.Cancelled)return STATUS_CANCELLED;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestRequestRequeue(PWDF_DRIVER_GLOBALS Globals,WDFREQUEST Request) {
  TEST_REQUEST_SLOT *R=UsbTestFindRequest(Request);TEST_QUEUE_SLOT *Q;UNREFERENCED_PARAMETER(Globals);
  UsbTestRequeueCalls++;CHECK(R&&R->OriginQueue&&R->Allocated&&R->Context.Endpoint!=NULL);
  if(!R||!R->OriginQueue)return STATUS_INVALID_DEVICE_STATE;
  Q=UsbTestFindQueue(R->OriginQueue);CHECK(Q&&Q->Count<TEST_QUEUE_DEPTH);
  if(!Q||Q->Count>=TEST_QUEUE_DEPTH)return STATUS_INSUFFICIENT_RESOURCES;
  Q->Requests[Q->Count++]=Request;return STATUS_SUCCESS;
}
static VOID UsbTestObjectReference(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Object,PVOID Tag,LONG Line,PCCH File) {
  TEST_REQUEST_SLOT *R;
  UNREFERENCED_PARAMETER(Globals);UNREFERENCED_PARAMETER(Tag);UNREFERENCED_PARAMETER(Line);UNREFERENCED_PARAMETER(File);
  UsbTestReferenceCalls++;
  if(Object==(WDFOBJECT)TEST_ENDPOINT)UsbTestEndpointReferences++;
  else if(Object==(WDFOBJECT)TEST_DEVICE)UsbTestDeviceReferences++;
  else {R=UsbTestFindRequest((WDFREQUEST)Object);CHECK(R!=NULL);if(R)R->References++;}
}
static VOID UsbTestObjectDereference(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Object,PVOID Tag,LONG Line,PCCH File) {
  TEST_REQUEST_SLOT *R;
  UNREFERENCED_PARAMETER(Globals);UNREFERENCED_PARAMETER(Tag);UNREFERENCED_PARAMETER(Line);UNREFERENCED_PARAMETER(File);
  UsbTestDereferenceCalls++;
  if(Object==(WDFOBJECT)TEST_ENDPOINT){UsbTestEndpointReferences--;CHECK(UsbTestEndpointReferences>=0);}
  else if(Object==(WDFOBJECT)TEST_DEVICE){UsbTestDeviceReferences--;CHECK(UsbTestDeviceReferences>=0);}
  else {R=UsbTestFindRequest((WDFREQUEST)Object);CHECK(R&&R->References>0);if(R&&R->References)R->References--;}
}
static VOID UsbTestCheckStartReferencesDrained(VOID) {
  CHECK(UsbTestDeviceReferences==0);
  CHECK(UsbTestController.QueueStartsActive==0);
  CHECK(IsListEmpty(&UsbTestController.QueueStartingEndpoints));
}
static VOID UsbTestSpinAcquire(PWDF_DRIVER_GLOBALS Globals,WDFSPINLOCK Lock) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Lock==(WDFSPINLOCK)(ULONG_PTR)0x303&&UsbTestWdfLockDepth==0&&!UsbTestEndpointLockHeld);
  UsbTestEndpointLockHeld=TRUE;UsbTestWdfLockDepth++;
}
static VOID UsbTestSpinRelease(PWDF_DRIVER_GLOBALS Globals,WDFSPINLOCK Lock) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Lock==(WDFSPINLOCK)(ULONG_PTR)0x303&&UsbTestWdfLockDepth==1&&UsbTestEndpointLockHeld);
  UsbTestEndpointLockHeld=FALSE;UsbTestWdfLockDepth--;
}
static VOID UsbTestWorkEnqueue(PWDF_DRIVER_GLOBALS Globals,WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Work==TEST_WORKER);UsbTestWorkEnqueues++;
}
static VOID UsbTestWorkFlush(PWDF_DRIVER_GLOBALS Globals,WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Work==TEST_WORKER);UsbTestWorkFlushes++;
  if(UsbTestController.ResetThread && UsbTestController.WorkerScheduled) UsbWork(Work);
}
static BOOLEAN UsbTestTimerStart(PWDF_DRIVER_GLOBALS Globals,WDFTIMER Timer,LONGLONG DueTime) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Timer==TEST_TIMER&&DueTime<0);UsbTestTimerStarts++;return TRUE;
}
static BOOLEAN UsbTestTimerStop(PWDF_DRIVER_GLOBALS Globals,WDFTIMER Timer,BOOLEAN Wait) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Timer==TEST_TIMER&&Wait);UsbTestTimerStops++;return TRUE;
}
static VOID UsbTestQueueStart(PWDF_DRIVER_GLOBALS Globals,WDFQUEUE Queue) {
  TEST_QUEUE_SLOT *Q=UsbTestFindQueue(Queue);UNREFERENCED_PARAMETER(Globals);
  CHECK(Q!=NULL);if(Q){Q->Started=TRUE;UsbTestQueueStarts++;}
  if(Queue==UsbTestEndpoint.Incoming)CHECK(UsbTestDeviceReferences==1);
  if(Queue==UsbTestEndpoint.Incoming && UsbTestReenterD0ExitOnQueueStart) {
    UsbTestReenterD0ExitOnQueueStart=FALSE;
    CHECK(UsbTestDeviceReferences==1);
    UsbTestReentrantD0ExitStatus=UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3);
  }
  if(Queue==UsbTestEndpoint.Incoming && UsbTestReenterControllerResetOnQueueStart) {
    UsbTestReenterControllerResetOnQueueStart=FALSE;
    CHECK(UsbTestDeviceReferences==1);
    UsbControllerReset(TEST_UCX);
  }
  if(Queue==UsbTestEndpoint.Incoming && UsbTestQueueStartDispatchRequest) {
    WDFREQUEST Request=UsbTestQueueStartDispatchRequest;ULONG Code=UsbTestQueueStartDispatchCode;
    UsbTestQueueStartDispatchRequest=NULL;
    CHECK(UsbTestDeviceReferences==1);
    CHECK(!UsbTestEndpointLockHeld);
    if(!UsbTestEndpointLockHeld) {UsbTestQueueStartDispatches++;UsbSubmit(Queue,Request,0,0,Code);}
  }
}
static VOID UsbTestQueuePurgeSync(PWDF_DRIVER_GLOBALS Globals,WDFQUEUE Queue) {
  TEST_QUEUE_SLOT *Q=UsbTestFindQueue(Queue);UNREFERENCED_PARAMETER(Globals);
  CHECK(Q!=NULL);
  if(Q) {
    if(UsbTestController.ResetThread) {
      while(Q->Count) UsbQueuedCancel(Queue,Q->Requests[--Q->Count]);
    }
    Q->Count=0;Q->Started=FALSE;UsbTestQueuePurges++;
  }
}
static VOID UsbTestQueueStopAndPurge(PWDF_DRIVER_GLOBALS Globals,WDFQUEUE Queue,EVT_WDF_IO_QUEUE_STATE *Callback,WDFCONTEXT Context) {
  TEST_QUEUE_SLOT *Q=UsbTestFindQueue(Queue);UNREFERENCED_PARAMETER(Globals);
  CHECK(Q&&Callback&&Q->StopCallback==NULL&&UsbTestStopCount<TEST_STOP_CALLBACKS);
  if(!Q||!Callback||UsbTestStopCount>=TEST_STOP_CALLBACKS)return;
  Q->Started=FALSE;Q->StopCallback=Callback;Q->StopContext=Context;
  UsbTestStops[UsbTestStopCount].Queue=Queue;UsbTestStops[UsbTestStopCount].Callback=Callback;
  UsbTestStops[UsbTestStopCount].Context=Context;UsbTestStopCount++;
  if(Queue==UsbTestEndpoint.Incoming && UsbTestReenterD0ExitOnQueueStop) {
    UsbTestReenterD0ExitOnQueueStop=FALSE;
    if(UsbTestEndpoint.QueueStartActive)CHECK(UsbTestDeviceReferences==1);
    CHECK(!UsbTestEndpointLockHeld);
    UsbTestReentrantD0ExitStatus=UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3);
  }
}
static NTSTATUS UsbTestQueueCreate(PWDF_DRIVER_GLOBALS Globals,WDFDEVICE Device,
    PWDF_IO_QUEUE_CONFIG Config,PWDF_OBJECT_ATTRIBUTES Attributes,WDFQUEUE *Queue) {
  TEST_QUEUE_SLOT *Q;WDFQUEUE Handle;UNREFERENCED_PARAMETER(Globals);
  UsbTestQueueCreates++;CHECK(Device==TEST_DEVICE&&Config);
  if(UsbTestQueueCreateFailAt&&UsbTestQueueCreates==UsbTestQueueCreateFailAt)return UsbTestQueueCreateStatus;
  if(UsbTestAddActive) {
    if(Config->DefaultQueue) {
      CHECK(!Attributes&&!Queue&&Config->DispatchType==WdfIoQueueDispatchParallel);
      CHECK(Config->PowerManaged==WdfFalse&&Config->EvtIoDeviceControl==UsbIoControl);
      UsbTestDefaultQueues++;return STATUS_SUCCESS;
    }
    CHECK(Attributes&&Queue&&Attributes->ParentObject==TEST_DEVICE);
    CHECK(Config->DispatchType==WdfIoQueueDispatchManual&&Config->EvtIoCanceledOnQueue==UsbQueuedCancel);
    CHECK(Config->PowerManaged==WdfFalse);
    Handle=(WDFQUEUE)(ULONG_PTR)(0x600+UsbTestQueueCreates);Q=UsbTestMakeQueue(Handle,NULL);
    if(!Q)return STATUS_INSUFFICIENT_RESOURCES;*Queue=Handle;return STATUS_SUCCESS;
  }
  CHECK(Attributes&&Queue&&Attributes->ParentObject==(WDFOBJECT)TEST_ENDPOINT);
  if(Config->DispatchType==WdfIoQueueDispatchParallel) {
    CHECK(Config->PowerManaged==WdfFalse&&Config->EvtIoInternalDeviceControl==UsbSubmit);UsbTestLastEndpointQueueMode=1;
  } else {
    CHECK(Config->DispatchType==WdfIoQueueDispatchManual&&Config->PowerManaged==WdfFalse);
    CHECK(Config->EvtIoCanceledOnQueue==UsbQueuedCancel);UsbTestLastEndpointQueueMode=2;
  }
  CHECK(UsbTestQueueCount<TEST_QUEUES);if(UsbTestQueueCount>=TEST_QUEUES)return STATUS_INSUFFICIENT_RESOURCES;
  Handle=(WDFQUEUE)(ULONG_PTR)(0x500+UsbTestQueueCount);Q=UsbTestMakeQueue(Handle,NULL);
  if(!Q)return STATUS_INSUFFICIENT_RESOURCES;*Queue=Handle;return STATUS_SUCCESS;
}
static WDFDEVICE UsbTestQueueGetDevice(PWDF_DRIVER_GLOBALS Globals,WDFQUEUE Queue) {
  UNREFERENCED_PARAMETER(Globals);UNREFERENCED_PARAMETER(Queue);return TEST_DEVICE;
}
static ULONG UsbTestResourceCount(PWDF_DRIVER_GLOBALS Globals,WDFCMRESLIST List) {
  UNREFERENCED_PARAMETER(Globals);return List==TEST_RAW_RESOURCES?UsbTestRawCount:UsbTestTranslatedCount;
}
static PCM_PARTIAL_RESOURCE_DESCRIPTOR UsbTestResourceDescriptor(PWDF_DRIVER_GLOBALS Globals,WDFCMRESLIST List,ULONG Index) {
  UNREFERENCED_PARAMETER(Globals);
  if(List==TEST_RAW_RESOURCES)return Index<UsbTestRawCount?&UsbTestRaw[Index]:NULL;
  return Index<UsbTestTranslatedCount?&UsbTestTranslated[Index]:NULL;
}
static WDFIOTARGET UsbTestDeviceGetIoTarget(PWDF_DRIVER_GLOBALS Globals,WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Device==TEST_DEVICE);return TEST_TARGET;
}
static NTSTATUS UsbTestTcpcCreate(PWDF_DRIVER_GLOBALS Globals,WDFDEVICE Device,
    PWDF_OBJECT_ATTRIBUTES Attributes,WDFIOTARGET *Target) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Device==TEST_DEVICE && !Attributes && Target);
  CHECK(UsbTestIrql==PASSIVE_LEVEL && !UsbTestTcpc.Created && !UsbTestEndpointLockHeld);
  UsbTestTcpc.Created=TRUE;
  UsbTestTcpc.Create++;
  *Target=TEST_TCPC_TARGET;
  return STATUS_SUCCESS;
}
static NTSTATUS UsbTestTcpcOpen(PWDF_DRIVER_GLOBALS Globals,WDFIOTARGET Target,
    PWDF_IO_TARGET_OPEN_PARAMS Params) {
  static const WCHAR Expected[]=L"\\Device\\RESOURCE_HUB\\0123456789abcdef";
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Target==TEST_TCPC_TARGET && UsbTestTcpc.Created && !UsbTestTcpc.Opened);
  CHECK(UsbTestIrql==PASSIVE_LEVEL && !UsbTestEndpointLockHeld);
  CHECK(Params->Type==WdfIoTargetOpenByName && Params->CreateDisposition==FILE_OPEN);
  CHECK(Params->ShareAccess==(FILE_SHARE_READ|FILE_SHARE_WRITE));
  CHECK(Params->DesiredAccess==(GENERIC_READ|GENERIC_WRITE));
  CHECK(Params->TargetDeviceName.Length==sizeof(Expected)-sizeof(WCHAR));
  CHECK(Params->TargetDeviceName.MaximumLength==sizeof(Expected));
  CHECK(!memcmp(Params->TargetDeviceName.Buffer,Expected,sizeof(Expected)));
  UsbTestTcpc.Open++;
  if(UsbTestTcpc.OpenStatus==STATUS_SUCCESS)UsbTestTcpc.Opened=TRUE;
  return UsbTestTcpc.OpenStatus;
}
static VOID UsbTestTcpcClose(PWDF_DRIVER_GLOBALS Globals,WDFIOTARGET Target) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Target==TEST_TCPC_TARGET && UsbTestTcpc.Created && UsbTestTcpc.Opened);
  CHECK(UsbTestIrql==PASSIVE_LEVEL && !UsbTestEndpointLockHeld);
  UsbTestTcpc.Opened=UsbTestTcpc.Locked=FALSE;
  UsbTestTcpc.Close++;
}
static VOID UsbTestObjectDelete(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Object) {
  UNREFERENCED_PARAMETER(Globals);
  if(Object==(WDFOBJECT)TEST_INTERRUPT) {
    CHECK(FALSE); // KMDF owns the interrupt created in PrepareHardware.
    return;
  }
  CHECK(Object==(WDFOBJECT)TEST_TCPC_TARGET && UsbTestTcpc.Created && !UsbTestTcpc.Opened && !UsbTestTcpc.Locked);
  UsbTestTcpc.Created=FALSE;
  UsbTestTcpc.Delete++;
}
static NTSTATUS UsbTestTcpcIoctl(ULONG Code,PWDF_MEMORY_DESCRIPTOR Input,
    PWDF_MEMORY_DESCRIPTOR Output,PWDF_REQUEST_SEND_OPTIONS Options,ULONG_PTR *Returned) {
  static const UCHAR Registers[]={0,0x1a,0x97,0x1a,0x97};
  static const ULONG Lengths[]={4,5,1,5,1};
  CHECK(UsbTestIrql==PASSIVE_LEVEL && !UsbTestEndpointLockHeld && !UsbTestController.CoreLock);
  CHECK(UsbTestTcpc.Opened && !Output && Options &&
        Options->Flags==WDF_REQUEST_SEND_OPTION_TIMEOUT && Options->Timeout==WDF_REL_TIMEOUT_IN_MS(50));
  if(Code==IOCTL_SPB_LOCK_CONNECTION) {
    CHECK(!UsbTestTcpc.Locked && !Input && !Returned);
    UsbTestTcpc.Locks++;
    UsbTestTcpc.ReadStep=0;
    if(UsbTestTcpc.LockStatus==STATUS_SUCCESS)UsbTestTcpc.Locked=TRUE;
    return UsbTestTcpc.LockStatus;
  }
  if(Code==IOCTL_SPB_UNLOCK_CONNECTION) {
    CHECK(UsbTestTcpc.Locked && !Input && !Returned);
    UsbTestTcpc.Unlocks++;
    if(UsbTestTcpc.UnlockStatus==STATUS_SUCCESS)UsbTestTcpc.Locked=FALSE;
    return UsbTestTcpc.UnlockStatus;
  }
  CHECK(Code==IOCTL_SPB_EXECUTE_SEQUENCE && UsbTestTcpc.Locked && Input && Returned);
  if(!Input || !Returned)return STATUS_INVALID_PARAMETER;
  PSPB_TRANSFER_LIST List=Input->u.BufferType.Buffer;
  CHECK(Input->Type==WdfMemoryDescriptorTypeBuffer &&
        Input->u.BufferType.Length==sizeof(*List)+sizeof(List->Transfers[0]));
  CHECK(List->Size==sizeof(*List) && List->TransferCount==2);
  SPB_TRANSFER_LIST_ENTRY *Write=&List->Transfers[0],*Read=&List->Transfers[1];
  CHECK(Write->Direction==SpbTransferDirectionToDevice && Read->Direction==SpbTransferDirectionFromDevice);
  CHECK(Write->Buffer.Format==SpbTransferBufferFormatSimple && Read->Buffer.Format==SpbTransferBufferFormatSimple);
  CHECK(!Write->DelayInUs && !Read->DelayInUs && Write->Buffer.Simple.BufferCb==1);
  ULONG Step=UsbTestTcpc.ReadStep++;
  CHECK(Step<ARRAYSIZE(Registers));
  if(Step>=ARRAYSIZE(Registers))return STATUS_INVALID_PARAMETER;
  PUCHAR Data=Read->Buffer.Simple.Buffer;
  CHECK(*(PUCHAR)Write->Buffer.Simple.Buffer==Registers[Step] && Read->Buffer.Simple.BufferCb==Lengths[Step]);
  UsbTestTcpc.Reads++;
  if(UsbTestTcpc.FailAt==Step+1)return UsbTestTcpc.ReadStatus;
  if(Step==0) {
    const UCHAR Id[]={UsbTestTcpc.WrongIdentity?0x98:0x99,0x2e,0x11,0x03};
    memcpy(Data,Id,sizeof(Id));
  } else if(Step==1 || Step==3) {
    ULONG State=Step==1?UsbTestTcpc.FirstState:UsbTestTcpc.SecondState;
    memset(Data,0,5);
    Data[0]=(UCHAR)(State>>16);Data[3]=(UCHAR)(State>>8);Data[4]=(UCHAR)State;
  } else {
    Data[0]=UsbTestTcpc.Safe0V?0x82:0x80;
  }
  *Returned=Lengths[Step]+(UsbTestTcpc.ShortAt!=Step+1);
  return STATUS_SUCCESS;
}
static NTSTATUS UsbTestSendIoctl(PWDF_DRIVER_GLOBALS Globals,WDFIOTARGET Target,WDFREQUEST Request,
    ULONG Code,PWDF_MEMORY_DESCRIPTOR Input,PWDF_MEMORY_DESCRIPTOR Output,
    PWDF_REQUEST_SEND_OPTIONS Options,ULONG_PTR *Returned) {
  PACPI_EVAL_INPUT_BUFFER In;PACPI_EVAL_OUTPUT_BUFFER Out;
  BOOLEAN Clock,Gate,Typec,Route;
  UNREFERENCED_PARAMETER(Globals);UNREFERENCED_PARAMETER(Request);
  if(Target==TEST_TCPC_TARGET)return UsbTestTcpcIoctl(Code,Input,Output,Options,Returned);
  CHECK(Target==TEST_TARGET&&Code==IOCTL_ACPI_EVAL_METHOD&&Input&&Output&&Options&&Returned);
  CHECK(!UsbTestEndpointLockHeld);
  CHECK(Options->Timeout==WDF_REL_TIMEOUT_IN_SEC(1));
  In=Input->u.BufferType.Buffer;Out=Output->u.BufferType.Buffer;
  CHECK(Input->Type==WdfMemoryDescriptorTypeBuffer&&Output->Type==WdfMemoryDescriptorTypeBuffer);
  Clock=memcmp(In->MethodName,"MCLK",4)==0;
  Gate=memcmp(In->MethodName,"MGON",4)==0;
  Typec=memcmp(In->MethodName,"MTCV",4)==0;
  Route=memcmp(In->MethodName,"MROU",4)==0;
  if(Gate) {
    PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER Arg=Input->u.BufferType.Buffer;
    CHECK(Input->u.BufferType.Length==sizeof(*Arg));
    CHECK(Arg->Signature==ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE);
    UsbTestGateCalls++;
    UsbTestGateArgument=Arg->IntegerArgument;
    if(!NT_SUCCESS(UsbTestGateStatus))return UsbTestGateStatus;
    if(UsbTestGateLatches)UsbTestClockIdentity=UsbTestGateReply;
  } else if(Clock) {
    CHECK(In->Signature==ACPI_EVAL_INPUT_BUFFER_SIGNATURE);
    UsbTestClockCalls++;
    if(!NT_SUCCESS(UsbTestClockStatus))return UsbTestClockStatus;
  } else if(Typec) {
    CHECK(In->Signature==ACPI_EVAL_INPUT_BUFFER_SIGNATURE);
    CHECK(UsbTestIrql==PASSIVE_LEVEL);
    UsbTestTypecCalls++;
    if(!NT_SUCCESS(UsbTestTypecStatus))return UsbTestTypecStatus;
  } else if(Route) {
    PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER Arg=Input->u.BufferType.Buffer;
    CHECK(Input->u.BufferType.Length==sizeof(*Arg));
    CHECK(Arg->Signature==ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE);
    CHECK(UsbTestIrql==PASSIVE_LEVEL && !UsbTestController.CoreLock);
    UsbTestRouteCalls++;
    UsbTestRouteArgument=Arg->IntegerArgument;
    if(!NT_SUCCESS(UsbTestRouteStatus))return UsbTestRouteStatus;
  } else {
    CHECK(In->Signature==ACPI_EVAL_INPUT_BUFFER_SIGNATURE);
    CHECK(memcmp(In->MethodName,"MHRD",4)==0);
    UsbTestAcpiCalls++;
    if(UsbTestAcpiVariant==1)return STATUS_IO_TIMEOUT;
  }
  memset((PVOID)Out,0,Output->u.BufferType.Length);
  Out->Signature=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE;Out->Count=1;Out->Length=sizeof(*Out);
  Out->Argument[0].Type=ACPI_METHOD_ARGUMENT_INTEGER;Out->Argument[0].DataLength=sizeof(ULONG);
  Out->Argument[0].Argument=Clock?UsbTestClockIdentity:1;*Returned=sizeof(*Out);
  if(Clock)return STATUS_SUCCESS;
  if(Route) {
    Out->Argument[0].Argument=(UsbTestRoutePerPath && UsbTestRouteArgument<4U)?
        UsbTestRouteReplies[UsbTestRouteArgument]:UsbTestRouteReply;
    return STATUS_SUCCESS;
  }
  if(Typec) {
    Out->Argument[0].Argument=UsbTestTypecSample;
    if(UsbTestTypecBadReply)Out->Argument[0].DataLength=0;
    return STATUS_SUCCESS;
  }
  if(Gate) {
    Out->Argument[0].Argument=UsbTestGateReply;
    if(UsbTestGateBadReply)Out->Argument[0].DataLength=0;
    return STATUS_SUCCESS;
  }
  switch(UsbTestAcpiVariant) {
    case 2:Out->Signature=0;break;
    case 3:Out->Count=2;break;
    case 4:Out->Length=1;break;
    case 5:Out->Argument[0].Type=ACPI_METHOD_ARGUMENT_BUFFER;break;
    case 6:Out->Argument[0].DataLength=8;break;
    case 7:Out->Argument[0].Argument=0;break;
    case 8:*Returned=Output->u.BufferType.Length+1;break;
    case 9:UsbTestHandoff[0]=0;break;
  }
  return STATUS_SUCCESS;
}
static VOID UsbTestFireQueueStop(WDFQUEUE Queue) {
  TEST_QUEUE_SLOT *Q=UsbTestFindQueue(Queue);CHECK(Q&&Q->StopCallback&&Q->StopContext);
  if(Q&&Q->StopCallback) {
    EVT_WDF_IO_QUEUE_STATE *Callback=Q->StopCallback;WDFCONTEXT Context=Q->StopContext;
    Q->StopCallback=NULL;Q->StopContext=NULL;
    while(Q->Count) {
      WDFREQUEST Request=Q->Requests[0];
      for(ULONG I=1;I<Q->Count;I++)Q->Requests[I-1]=Q->Requests[I];
      Q->Count--;UsbQueuedCancel(Queue,Request);
    }
    Callback(Queue,Context);
  }
}
static VOID UsbTestRunDpc(VOID) {
  if(UsbTestDpcQueued){
    ULONG Old=UsbTestIrql;UsbTestDpcQueued=0;UsbTestIrql=DISPATCH_LEVEL;
    UsbCompletionDpc(&UsbTestController.CompletionDpc,&UsbTestController,NULL,NULL);UsbTestIrql=Old;
  }
}
MTK_MUSB_PLATFORM_STATUS UsbTestPlatformIdle(const MTK_MUSB_PLATFORM_IO *Io,uint32_t SessionClear) {
  UsbTestIdleCalls++;CHECK(Io&&Io->Context==&UsbTestController&&Io->ControllerLength==USB_BYTES);
  CHECK(!SessionClear&&Io->Read8&&Io->Read16&&Io->Read32&&Io->Write8&&Io->TryLock&&Io->Unlock);
  return UsbTestIdleResult;
}
MTK_MUSB_PLATFORM_STATUS UsbTestPlatformFifo(const MTK_MUSB_PLATFORM_IO *Io) {
  UsbTestFifoCalls++;CHECK(Io&&Io->Context==&UsbTestController);
  if(UsbTestDevctlAfterFifo) { UsbTestRegisters[0x60]=UsbTestDevctlAfterFifo;UsbTestDevctlAfterFifo=0; }
  return UsbTestFifoResult;
}
MTK_MUSB_PLATFORM_STATUS UsbTestPlatformAck(const MTK_MUSB_PLATFORM_IO *Io) {
  UsbTestAckCalls++;CHECK(Io&&Io->Context==&UsbTestController&&Io->ControllerLength==USB_BYTES);
  CHECK((!UsbTestController.Online && UsbTestController.Stopping) ||
        (UsbTestController.WaitingForVbus && UsbTestController.WorkerThread));
  if(UsbTestDropVbusOnAck) {
    UsbTestRegisters[USB_DEVCTL_REGISTER]&=(UCHAR)~0x18U; // DEVCTL VBUS[4:3]
    UsbTestDropVbusOnAck=FALSE;
  }
  return UsbTestIdleResult;
}
/* The portable core test covers the register sequence. Here the previous
 * owner's endpoints and DMA stop and the host POWER bits are installed. */
MTK_MUSB_PLATFORM_STATUS UsbTestTakeOver(const MTK_MUSB_PLATFORM_IO *Io) {
  UsbTestTakeOverCalls++;
  CHECK(Io&&Io->Context==&UsbTestController&&Io->ControllerLength==USB_BYTES);
  CHECK(UsbTestIrql==PASSIVE_LEVEL && !UsbTestController.CoreLock);
  CHECK(UsbTestController.Core.State==MUSB_HOST_STATE_HALTED);
  if(UsbTestTakeOverResult==MtkMusbPlatformSuccess) {
    UsbTestRegisters[USB_REG_INTRUSBE]=0;UsbTestRegisters[0x214]=0;
    UsbTestRegisters[USB_REG_POWER]=(UCHAR)((UsbTestRegisters[USB_REG_POWER]&~0x4eU)|0x21U);
    UsbTestIdleResult=MtkMusbPlatformSuccess;
  }
  return UsbTestTakeOverResult;
}
MTK_MUSB_PLATFORM_STATUS UsbTestRestoreFifo(const MTK_MUSB_PLATFORM_IO *Io) {
  UsbTestFifoRestores++;
  CHECK(Io&&Io->Context==&UsbTestController&&Io->ControllerLength==USB_BYTES);
  CHECK(UsbTestAckCalls && ((!UsbTestController.Online && UsbTestController.Stopping) ||
        (UsbTestController.WaitingForVbus && UsbTestController.WorkerThread)));
  CHECK(UsbTestController.Core.State==MUSB_HOST_STATE_HALTED);
  return UsbTestFifoRestoreResult;
}
static VOID UsbTestUcxFailed(PUCX_DRIVER_GLOBALS Globals,UCXCONTROLLER C) {
  UNREFERENCED_PARAMETER(Globals);CHECK(C==TEST_UCX);UsbTestControllerFailed++;
  UsbTestResetFailedOrder=++UsbTestResetSequence;
}
static VOID UsbTestUcxResetComplete(PUCX_DRIVER_GLOBALS Globals,UCXCONTROLLER C,PUCX_CONTROLLER_RESET_COMPLETE_INFO Info) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(C==TEST_UCX && Info && Info->Size==sizeof(*Info) &&
        Info->UcxControllerState==UcxControllerStateLost && Info->UcxCoordinated);
  CHECK(!UsbTestController.ResetThread && UsbTestController.ResetDrained.Header.SignalState==1);
  UsbTestResetCompleteCalls++;UsbTestResetCompleteOrder=++UsbTestResetSequence;
}
static NTSTATUS UsbTestUcxInit(PUCX_DRIVER_GLOBALS Globals,PWDFDEVICE_INIT Init) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Init==(PWDFDEVICE_INIT)(ULONG_PTR)0x400);return STATUS_SUCCESS;
}
static VOID UsbTestSetPowerCallbacks(PWDF_DRIVER_GLOBALS Globals,PWDFDEVICE_INIT Init,PWDF_PNPPOWER_EVENT_CALLBACKS Callbacks) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Init==(PWDFDEVICE_INIT)(ULONG_PTR)0x400&&Callbacks);
  UsbTestPowerCallbacks=*Callbacks;
}
static NTSTATUS UsbTestInterruptCreate(PWDF_DRIVER_GLOBALS Globals,WDFDEVICE Device,
    PWDF_INTERRUPT_CONFIG Config,PWDF_OBJECT_ATTRIBUTES Attributes,WDFINTERRUPT *Interrupt) {
  ULONG Index;
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Device==TEST_DEVICE && !Attributes && Config && Config->Size==sizeof(*Config));
  for(Index=0;Index<UsbTestRawCount;Index++) if(UsbTestRaw[Index].Type==CmResourceTypeInterrupt) break;
  CHECK(Index<UsbTestRawCount && Config->InterruptRaw==&UsbTestRaw[Index] &&
        Config->InterruptTranslated==&UsbTestTranslated[Index]);
  CHECK(Config->EvtInterruptIsr && Config->EvtInterruptDpc && Config->EvtInterruptDisable);
  CHECK(!Config->PassiveHandling && !Config->AutomaticSerialization);
  CHECK(!UsbTestInterrupt.Created);
  UsbTestInterrupt.Create++;
  if(!NT_SUCCESS(UsbTestInterrupt.CreateStatus)) return UsbTestInterrupt.CreateStatus;
  UsbTestInterrupt.Config=*Config;
  UsbTestInterrupt.Created=TRUE;
  *Interrupt=TEST_INTERRUPT;
  return STATUS_SUCCESS;
}
static WDFDEVICE UsbTestInterruptGetDevice(PWDF_DRIVER_GLOBALS Globals,WDFINTERRUPT Interrupt) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Interrupt==TEST_INTERRUPT && UsbTestIrql==TEST_USB_DIRQL && UsbTestInterrupt.Connected);
  return TEST_DEVICE;
}
static BOOLEAN UsbTestInterruptSynchronize(PWDF_DRIVER_GLOBALS Globals,WDFINTERRUPT Interrupt,
    PFN_WDF_INTERRUPT_SYNCHRONIZE Callback,WDFCONTEXT Context) {
  BOOLEAN Result;
  ULONG Previous=UsbTestIrql;
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Interrupt==TEST_INTERRUPT && UsbTestInterrupt.Connected && UsbTestEndpointLockHeld &&
        UsbTestIrql<=DISPATCH_LEVEL && Callback);
  UsbTestIrql=TEST_USB_DIRQL;
  Result=Callback(Interrupt,Context);
  UsbTestIrql=Previous;
  return Result;
}
static BOOLEAN UsbTestInterruptQueueDpc(PWDF_DRIVER_GLOBALS Globals,WDFINTERRUPT Interrupt) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Interrupt==TEST_INTERRUPT && UsbTestInterrupt.Connected && UsbTestIrql==TEST_USB_DIRQL);
  if(UsbTestInterrupt.Pending) return FALSE;
  UsbTestInterrupt.Pending=1;
  return TRUE;
}
static BOOLEAN UsbTestRunPortIsr(VOID) {
  BOOLEAN Result;
  CHECK(UsbTestInterrupt.Connected && UsbTestIrql==PASSIVE_LEVEL && !UsbTestEndpointLockHeld);
  if(!UsbTestInterrupt.Config.EvtInterruptIsr) return FALSE;
  UsbTestIrql=TEST_USB_DIRQL;
  Result=UsbTestInterrupt.Config.EvtInterruptIsr(TEST_INTERRUPT,0);
  UsbTestIrql=PASSIVE_LEVEL;
  return Result;
}
static VOID UsbTestRunPortDpc(VOID) {
  CHECK(UsbTestInterrupt.Pending && UsbTestIrql==PASSIVE_LEVEL);
  if(!UsbTestInterrupt.Pending) return;
  UsbTestInterrupt.Pending=0;
  UsbTestIrql=DISPATCH_LEVEL;
  UsbTestInterrupt.Config.EvtInterruptDpc(TEST_INTERRUPT,(WDFOBJECT)TEST_DEVICE);
  UsbTestIrql=PASSIVE_LEVEL;
}
static VOID UsbTestStartPortInterrupt(WDF_POWER_DEVICE_STATE Previous) {
  CHECK(UsbTestInterrupt.Created && !UsbTestInterrupt.Connected);
  UsbTestInterrupt.Connected=TRUE;
  CHECK(UsbD0PostInterruptsEnabled(TEST_DEVICE,Previous)==STATUS_SUCCESS);
}
static VOID UsbTestStopPortInterrupt(WDF_POWER_DEVICE_STATE Target) {
  CHECK(UsbTestInterrupt.Connected);
  CHECK(UsbD0PreInterruptsDisabled(TEST_DEVICE,Target)==STATUS_SUCCESS);
  if(UsbTestInterrupt.Pending) UsbTestRunPortDpc();
  if(UsbTestInterrupt.Config.EvtInterruptDisable) {
    UsbTestIrql=TEST_USB_DIRQL;
    CHECK(UsbTestInterrupt.Config.EvtInterruptDisable(TEST_INTERRUPT,TEST_DEVICE)==STATUS_SUCCESS);
    UsbTestIrql=PASSIVE_LEVEL;
  }
  UsbTestInterrupt.Connected=FALSE;
}
static VOID UsbTestPnpRelease(VOID) {
  CHECK(UsbTestInterrupt.Created && !UsbTestInterrupt.Connected && !UsbTestInterrupt.Pending);
  CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  CHECK(!UsbTestController.Registers && !UsbTestController.Interrupt);
  UsbTestInterrupt.Created=FALSE;
  UsbTestInterrupt.Delete++; // Framework cleanup follows the driver callback.
}
static NTSTATUS UsbTestCreateDevice(PWDF_DRIVER_GLOBALS Globals,PWDFDEVICE_INIT *Init,PWDF_OBJECT_ATTRIBUTES Attributes,WDFDEVICE *Device) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Init&&*Init==(PWDFDEVICE_INIT)(ULONG_PTR)0x400&&Attributes&&Device);
  CHECK(Attributes->SynchronizationScope==WdfSynchronizationScopeNone&&Attributes->EvtCleanupCallback==UsbCleanup);
  CHECK(Attributes->ExecutionLevel==WdfExecutionLevelPassive);
  *Init=NULL;*Device=TEST_DEVICE;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestCreateSpinLock(PWDF_DRIVER_GLOBALS Globals,PWDF_OBJECT_ATTRIBUTES Attributes,WDFSPINLOCK *Lock) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Attributes&&Attributes->ParentObject==TEST_DEVICE&&Lock);
  *Lock=(WDFSPINLOCK)(ULONG_PTR)0x303;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestCreateWorkItem(PWDF_DRIVER_GLOBALS Globals,PWDF_WORKITEM_CONFIG Config,PWDF_OBJECT_ATTRIBUTES Attributes,WDFWORKITEM *Work) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Config&&Attributes&&Work);
  CHECK(Config->EvtWorkItemFunc==UsbWork&&!Config->AutomaticSerialization&&Attributes->ParentObject==TEST_DEVICE);
  *Work=TEST_WORKER;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestCreateTimer(PWDF_DRIVER_GLOBALS Globals,PWDF_TIMER_CONFIG Config,PWDF_OBJECT_ATTRIBUTES Attributes,WDFTIMER *Timer) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Config&&Attributes&&Timer);
  CHECK(Config->EvtTimerFunc==UsbTimer&&Config->Period==1&&!Config->AutomaticSerialization&&Attributes->ParentObject==TEST_DEVICE);
  CHECK(Attributes->ExecutionLevel==WdfExecutionLevelDispatch);
  *Timer=TEST_TIMER;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestCreateController(PUCX_DRIVER_GLOBALS Globals,WDFDEVICE Device,PUCX_CONTROLLER_CONFIG Config,PWDF_OBJECT_ATTRIBUTES Attributes,UCXCONTROLLER *Controller) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Device==TEST_DEVICE&&Config&&Attributes&&Controller);
  CHECK(Config->EvtControllerUsbDeviceAdd==UsbDeviceAdd&&Config->EvtControllerReset==UsbControllerReset);
  CHECK(Config->EvtControllerGetCurrentFrameNumber==UsbFrame&&Config->EvtControllerQueryUsbCapability==UsbCapability);
  UsbTestUcxCreates++;*Controller=TEST_UCX;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestCreateRoot(PUCX_DRIVER_GLOBALS Globals,UCXCONTROLLER Controller,PUCX_ROOTHUB_CONFIG Config,PWDF_OBJECT_ATTRIBUTES Attributes,UCXROOTHUB *Root) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Controller==TEST_UCX&&Config&&Attributes&&Root);
  CHECK(Config->EvtRootHubClearHubFeature==UsbRootControl&&Config->EvtRootHubClearPortFeature==UsbRootControl);
  CHECK(Config->EvtRootHubGetHubStatus==UsbRootControl&&Config->EvtRootHubGetPortStatus==UsbRootControl);
  CHECK(Config->EvtRootHubSetHubFeature==UsbRootControl&&Config->EvtRootHubSetPortFeature==UsbRootControl);
  CHECK(Config->EvtRootHubGetPortErrorCount==UsbRootControl&&Config->EvtRootHubInterruptTx==UsbRootInterrupt);
  CHECK(Config->EvtRootHubControlUrb==NULL);
  UsbTestRootCreates++;*Root=TEST_ROOTHUB;return STATUS_SUCCESS;
}
static NTSTATUS UsbTestCreateInterface(PWDF_DRIVER_GLOBALS Globals,WDFDEVICE Device,CONST GUID *Guid,PCUNICODE_STRING Reference) {
  static const UCHAR Tail[8]={0x8a,0x92,0x1e,0x68,0x61,0xe6,0xaf,0x27};ULONG I;
  UNREFERENCED_PARAMETER(Globals);CHECK(Device==TEST_DEVICE&&Guid&&!Reference);
  CHECK(UsbTestDefaultQueues==1&&UsbTestUcxCreates==1&&UsbTestRootCreates==1);
  CHECK(Guid->Data1==0x3abf6f2d&&Guid->Data2==0x71c4&&Guid->Data3==0x462a);
  for(I=0;I<8;I++)CHECK(Guid->Data4[I]==Tail[I]);
  UsbTestInterfaceCalls++;return UsbTestInterfaceStatus;
}
static BOOLEAN UsbTestUcxIoctl(PUCX_DRIVER_GLOBALS Globals,WDFDEVICE Device,WDFREQUEST Request,size_t Out,size_t In,ULONG Code) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Device==TEST_DEVICE&&UsbTestFindRequest(Request));
  CHECK(Out==64&&In==8&&Code==IOCTL_USB_GET_ROOT_HUB_NAME);UsbTestIoctlCalls++;return UsbTestIoctlHandled;
}
static VOID UsbTestDeviceEventCallbacks(PUCX_DRIVER_GLOBALS Globals,PUCXUSBDEVICE_INIT Init,PUCX_USBDEVICE_EVENT_CALLBACKS Events) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Init==(PUCXUSBDEVICE_INIT)(ULONG_PTR)0x430&&Events);
  CHECK(Events->EvtUsbDeviceEndpointsConfigure==UsbConfigure&&Events->EvtUsbDeviceEnable==UsbEnable);
  CHECK(Events->EvtUsbDeviceDisable==UsbDisable&&Events->EvtUsbDeviceReset==UsbResetDevice);
  CHECK(Events->EvtUsbDeviceAddress==UsbAddressDevice&&Events->EvtUsbDeviceUpdate==UsbUpdateDevice);
  CHECK(Events->EvtUsbDeviceHubInfo==UsbHubInfo&&Events->EvtUsbDeviceDefaultEndpointAdd==UsbDefaultEndpointAdd);
  CHECK(Events->EvtUsbDeviceEndpointAdd==UsbEndpointAdd);UsbTestDeviceCallbacks++;
}
static NTSTATUS UsbTestCreateUsbDevice(PUCX_DRIVER_GLOBALS Globals,UCXCONTROLLER Controller,PUCXUSBDEVICE_INIT *Init,PWDF_OBJECT_ATTRIBUTES Attributes,UCXUSBDEVICE *Device) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Controller==TEST_UCX&&Init&&*Init==(PUCXUSBDEVICE_INIT)(ULONG_PTR)0x430&&Attributes&&Device);
  UsbTestDeviceCreates++;if(!NT_SUCCESS(UsbTestDeviceCreateStatus))return UsbTestDeviceCreateStatus;
  memset(&UsbTestDevice,0,sizeof(UsbTestDevice));UsbTestAddContext(TEST_USBDEVICE,&UsbTestDevice);
  *Device=TEST_USBDEVICE;*Init=NULL;return STATUS_SUCCESS;
}
static VOID UsbTestInstallMocks(VOID) {
  memset(UsbTestWdfTable,0,sizeof(UsbTestWdfTable));
  UsbTestWdfTable[WdfDeviceInitSetPnpPowerEventCallbacksTableIndex]=(WDFFUNC)UsbTestSetPowerCallbacks;
  UsbTestWdfTable[WdfDeviceCreateTableIndex]=(WDFFUNC)UsbTestCreateDevice;
  UsbTestWdfTable[WdfDeviceCreateDeviceInterfaceTableIndex]=(WDFFUNC)UsbTestCreateInterface;
  UsbTestWdfTable[WdfSpinLockCreateTableIndex]=(WDFFUNC)UsbTestCreateSpinLock;
  UsbTestWdfTable[WdfWorkItemCreateTableIndex]=(WDFFUNC)UsbTestCreateWorkItem;
  UsbTestWdfTable[WdfTimerCreateTableIndex]=(WDFFUNC)UsbTestCreateTimer;
  UsbTestWdfTable[WdfInterruptCreateTableIndex]=(WDFFUNC)UsbTestInterruptCreate;
  UsbTestWdfTable[WdfInterruptGetDeviceTableIndex]=(WDFFUNC)UsbTestInterruptGetDevice;
  UsbTestWdfTable[WdfInterruptSynchronizeTableIndex]=(WDFFUNC)UsbTestInterruptSynchronize;
  UsbTestWdfTable[WdfInterruptQueueDpcForIsrTableIndex]=(WDFFUNC)UsbTestInterruptQueueDpc;
  UsbTestWdfTable[WdfObjectGetTypedContextWorkerTableIndex]=(WDFFUNC)UsbTestWdfContext;
  UsbTestWdfTable[WdfObjectAllocateContextTableIndex]=(WDFFUNC)UsbTestObjectAllocateContext;
  UsbTestWdfTable[WdfRequestGetParametersTableIndex]=(WDFFUNC)UsbTestRequestGetParameters;
  UsbTestWdfTable[WdfRequestCompleteTableIndex]=(WDFFUNC)UsbTestRequestComplete;
  UsbTestWdfTable[WdfRequestCompleteWithInformationTableIndex]=(WDFFUNC)UsbTestRequestCompleteWithInformation;
  UsbTestWdfTable[WdfRequestForwardToIoQueueTableIndex]=(WDFFUNC)UsbTestRequestForward;
  UsbTestWdfTable[WdfIoQueueRetrieveNextRequestTableIndex]=(WDFFUNC)UsbTestQueueRetrieve;
  UsbTestWdfTable[WdfRequestMarkCancelableExTableIndex]=(WDFFUNC)UsbTestRequestMarkCancelable;
  UsbTestWdfTable[WdfRequestUnmarkCancelableTableIndex]=(WDFFUNC)UsbTestRequestUnmarkCancelable;
  UsbTestWdfTable[WdfRequestRequeueTableIndex]=(WDFFUNC)UsbTestRequestRequeue;
  UsbTestWdfTable[WdfObjectReferenceActualTableIndex]=(WDFFUNC)UsbTestObjectReference;
  UsbTestWdfTable[WdfObjectDereferenceActualTableIndex]=(WDFFUNC)UsbTestObjectDereference;
  UsbTestWdfTable[WdfSpinLockAcquireTableIndex]=(WDFFUNC)UsbTestSpinAcquire;
  UsbTestWdfTable[WdfSpinLockReleaseTableIndex]=(WDFFUNC)UsbTestSpinRelease;
  UsbTestWdfTable[WdfWorkItemEnqueueTableIndex]=(WDFFUNC)UsbTestWorkEnqueue;
  UsbTestWdfTable[WdfWorkItemFlushTableIndex]=(WDFFUNC)UsbTestWorkFlush;
  UsbTestWdfTable[WdfTimerStartTableIndex]=(WDFFUNC)UsbTestTimerStart;
  UsbTestWdfTable[WdfTimerStopTableIndex]=(WDFFUNC)UsbTestTimerStop;
  UsbTestWdfTable[WdfIoQueueStartTableIndex]=(WDFFUNC)UsbTestQueueStart;
  UsbTestWdfTable[WdfIoQueuePurgeSynchronouslyTableIndex]=(WDFFUNC)UsbTestQueuePurgeSync;
  UsbTestWdfTable[WdfIoQueueStopAndPurgeTableIndex]=(WDFFUNC)UsbTestQueueStopAndPurge;
  UsbTestWdfTable[WdfIoQueueCreateTableIndex]=(WDFFUNC)UsbTestQueueCreate;
  UsbTestWdfTable[WdfIoQueueGetDeviceTableIndex]=(WDFFUNC)UsbTestQueueGetDevice;
  UsbTestWdfTable[WdfCmResourceListGetCountTableIndex]=(WDFFUNC)UsbTestResourceCount;
  UsbTestWdfTable[WdfCmResourceListGetDescriptorTableIndex]=(WDFFUNC)UsbTestResourceDescriptor;
  UsbTestWdfTable[WdfDeviceGetIoTargetTableIndex]=(WDFFUNC)UsbTestDeviceGetIoTarget;
  UsbTestWdfTable[WdfIoTargetCreateTableIndex]=(WDFFUNC)UsbTestTcpcCreate;
  UsbTestWdfTable[WdfIoTargetOpenTableIndex]=(WDFFUNC)UsbTestTcpcOpen;
  UsbTestWdfTable[WdfIoTargetCloseTableIndex]=(WDFFUNC)UsbTestTcpcClose;
  UsbTestWdfTable[WdfObjectDeleteTableIndex]=(WDFFUNC)UsbTestObjectDelete;
  UsbTestWdfTable[WdfIoTargetSendIoctlSynchronouslyTableIndex]=(WDFFUNC)UsbTestSendIoctl;
  memset(UcxFunctions,0,sizeof(UcxFunctions));
  UcxFunctions[UcxInitializeDeviceInitTableIndex]=(PFN_UCXFUNC)UsbTestUcxInit;
  UcxFunctions[UcxIoDeviceControlTableIndex]=(PFN_UCXFUNC)UsbTestUcxIoctl;
  UcxFunctions[UcxControllerCreateTableIndex]=(PFN_UCXFUNC)UsbTestCreateController;
  UcxFunctions[UcxRootHubCreateTableIndex]=(PFN_UCXFUNC)UsbTestCreateRoot;
  UcxFunctions[UcxUsbDeviceInitSetEventCallbacksTableIndex]=(PFN_UCXFUNC)UsbTestDeviceEventCallbacks;
  UcxFunctions[UcxUsbDeviceCreateTableIndex]=(PFN_UCXFUNC)UsbTestCreateUsbDevice;
  UcxFunctions[UcxControllerSetFailedTableIndex]=(PFN_UCXFUNC)UsbTestUcxFailed;
  UcxFunctions[UcxControllerResetCompleteTableIndex]=(PFN_UCXFUNC)UsbTestUcxResetComplete;
  UcxFunctions[UcxRootHubPortChangedTableIndex]=(PFN_UCXFUNC)UsbTestUcxPortChanged;
  UcxFunctions[UcxEndpointPurgeCompleteTableIndex]=(PFN_UCXFUNC)UsbTestUcxPurgeComplete;
  UcxFunctions[UcxEndpointAbortCompleteTableIndex]=(PFN_UCXFUNC)UsbTestUcxAbortComplete;
  UcxFunctions[UcxEndpointCreateTableIndex]=(PFN_UCXFUNC)UsbTestUcxEndpointCreate;
  UcxFunctions[UcxEndpointInitSetEventCallbacksTableIndex]=(PFN_UCXFUNC)UsbTestUcxEndpointCallbacks;
  UcxFunctions[UcxDefaultEndpointInitSetEventCallbacksTableIndex]=(PFN_UCXFUNC)UsbTestUcxDefaultEndpointCallbacks;
  UcxFunctions[UcxEndpointSetWdfIoQueueTableIndex]=(PFN_UCXFUNC)UsbTestUcxEndpointSetQueue;
}
static VOID UsbTestUcxPortChanged(PUCX_DRIVER_GLOBALS Globals,UCXROOTHUB RootHub) {
  UNREFERENCED_PARAMETER(Globals);CHECK(RootHub==TEST_ROOTHUB);UsbTestUcxRootChanged++;
}
static VOID UsbTestUcxPurgeComplete(PUCX_DRIVER_GLOBALS Globals,UCXENDPOINT Endpoint) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Endpoint==TEST_ENDPOINT);UsbTestUcxPurgeDoneCount++;
}
static VOID UsbTestUcxAbortComplete(PUCX_DRIVER_GLOBALS Globals,UCXENDPOINT Endpoint) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Endpoint==TEST_ENDPOINT);UsbTestUcxAbortDoneCount++;
}
static NTSTATUS UsbTestUcxEndpointCreate(PUCX_DRIVER_GLOBALS Globals,UCXUSBDEVICE Device,
    PUCXENDPOINT_INIT *Init,PWDF_OBJECT_ATTRIBUTES Attributes,UCXENDPOINT *Endpoint) {
  UNREFERENCED_PARAMETER(Globals);UsbTestUcxEndpointCreateCalls++;
  CHECK(Device==TEST_USBDEVICE&&Init&&*Init&&Attributes&&Endpoint&&Attributes->EvtCleanupCallback==UsbEndpointCleanup);
  memset(&UsbTestEndpoint,0,sizeof(UsbTestEndpoint));UsbTestAddContext((WDFOBJECT)TEST_ENDPOINT,&UsbTestEndpoint);
  *Endpoint=TEST_ENDPOINT;return STATUS_SUCCESS;
}
static VOID UsbTestUcxEndpointCallbacks(PUCX_DRIVER_GLOBALS Globals,PUCXENDPOINT_INIT Init,PUCX_ENDPOINT_EVENT_CALLBACKS Callbacks) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Init&&Callbacks&&Callbacks->EvtEndpointPurge==UsbEndpointPurge);
  CHECK(Callbacks->EvtEndpointStart==UsbEndpointStart&&Callbacks->EvtEndpointAbort==UsbEndpointAbort);
  CHECK(Callbacks->EvtEndpointReset==UsbEndpointReset&&Callbacks->EvtEndpointOkToCancelTransfers==UsbOkToCancel);
  UsbTestEndpointEvents=*Callbacks;UsbTestEndpointEventsSeen=TRUE;UsbTestEndpointCallbacks++;
}
static VOID UsbTestUcxDefaultEndpointCallbacks(PUCX_DRIVER_GLOBALS Globals,PUCXENDPOINT_INIT Init,PUCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS Callbacks) {
  UNREFERENCED_PARAMETER(Globals);
  CHECK(Init&&Callbacks&&Callbacks->EvtEndpointPurge==UsbEndpointPurge);
  CHECK(Callbacks->EvtEndpointStart==UsbEndpointStart&&Callbacks->EvtEndpointAbort==UsbEndpointAbort);
  CHECK(Callbacks->EvtEndpointOkToCancelTransfers==UsbOkToCancel&&Callbacks->EvtDefaultEndpointUpdate==UsbDefaultUpdate);
  UsbTestDefaultEndpointEvents=*Callbacks;UsbTestDefaultEndpointEventsSeen=TRUE;
}
static VOID UsbTestUcxEndpointSetQueue(PUCX_DRIVER_GLOBALS Globals,UCXENDPOINT Endpoint,WDFQUEUE Queue) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Endpoint==TEST_ENDPOINT&&Queue==UsbTestEndpoint.Incoming);
  UsbTestUcxQueueBindCalls++;
}

/* Root hub callbacks verify the Windows port-1 to MUSB port-0 translation. */
static VOID UsbTestRootHubAndPortZero(VOID) {
  ROOTHUB_INFO Info={0};ROOTHUB_20PORTS_INFO Info20={0};ROOTHUB_20PORT_INFO Port={0};
  PROOTHUB_20PORT_INFO Ports[1]={&Port};ROOTHUB_30PORTS_INFO Info30={0};
  TEST_REQUEST_SLOT *Slot;WDFREQUEST Request;URB Urb;UCHAR Setup[8]={0xa3,0,0,0,1,0,4,0};UCHAR Data[4]={0};
  UsbTestInitController();
  Info.Size=sizeof(Info);Request=UsbTestNewRequest(0,&Info);UsbRootInfo(TEST_ROOTHUB,Request);
  Slot=UsbTestFindRequest(Request);CHECK(Slot&&Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(Info.NumberOf20Ports==1&&Info.NumberOf30Ports==0&&Info.ControllerType==ControllerTypeSoftXhci);
  Info20.Size=sizeof(Info20);Info20.NumberOfPorts=1;Info20.PortInfoSize=sizeof(Port);Info20.PortInfoArray=Ports;
  Request=UsbTestNewRequest(1,&Info20);UsbRoot20Info(TEST_ROOTHUB,Request);
  Slot=UsbTestFindRequest(Request);CHECK(Slot&&Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(Port.PortNumber==1&&Port.Removable==TriStateTrue&&Port.HubDepth==0);
  Info30.Size=sizeof(Info30);Info30.NumberOfPorts=0;Request=UsbTestNewRequest(2,&Info30);
  UsbRoot30Info(TEST_ROOTHUB,Request);Slot=UsbTestFindRequest(Request);
  CHECK(Slot&&Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  memset(&Info,0,sizeof(Info));Request=UsbTestNewRequest(3,&Info);UsbRootInfo(TEST_ROOTHUB,Request);
  Slot=UsbTestFindRequest(Request);CHECK(Slot&&Slot->CompletionStatus==STATUS_INVALID_PARAMETER);
  UsbTestExpectedPortStatus=0x0101;UsbTestExpectedPortChange=0x0005;
  memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER_EX;
  Urb.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER_EX);
  Urb.UrbControlTransferEx.TransferBuffer=Data;Urb.UrbControlTransferEx.TransferBufferLength=sizeof(Data);
  memcpy(Urb.UrbControlTransferEx.SetupPacket,Setup,8);Urb.UrbControlTransferEx.Timeout=100;
  Request=UsbTestNewRequest(4,&Urb);UsbRootControl(TEST_ROOTHUB,Request);UsbWork(TEST_WORKER);
  Slot=UsbTestFindRequest(Request);CHECK(Slot&&Slot->Context.Urb==&Urb&&Slot->CompletionCount==0);
  CHECK(UsbTestPortCalls==2&&Data[0]==1&&Data[1]==1&&Data[2]==5&&Data[3]==0);
  CHECK(UsbTestUcxRootChanged==1&&UsbTestDpcQueued==1);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(Urb.UrbHeader.Status==USBD_STATUS_SUCCESS&&Urb.UrbControlTransferEx.TransferBufferLength==4);
  {
    URB ResetPort;UCHAR ResetSetup[8]={0x23,3,MUSB_PORT_FEATURE_RESET,0,1,0,0,0};
    memset(&ResetPort,0,sizeof(ResetPort));ResetPort.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER_EX;
    ResetPort.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER_EX);
    memcpy(ResetPort.UrbControlTransferEx.SetupPacket,ResetSetup,8);ResetPort.UrbControlTransferEx.TransferBufferLength=0;
    ResetPort.UrbControlTransferEx.Timeout=100;
    UsbTestController.Core.Io.DelayUs=UsbDelay;
    UsbDelay(NULL,50);
    CHECK(UsbTestStallCalls==1&&!UsbTestDelayCalls);
    Request=UsbTestNewRequest(7,&ResetPort);UsbRootControl(TEST_ROOTHUB,Request);UsbWork(TEST_WORKER);
    Slot=UsbTestFindRequest(Request);CHECK(UsbTestPortFeatureCalls==1&&Slot->Context.Completed==1);
    CHECK(UsbTestStallCalls==1&&UsbTestDelayCalls==1&&UsbTestDelayInterval==-500000LL);
    CHECK(Slot->Context.Status==STATUS_SUCCESS);UsbTestRunDpc();
    CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  }
  {
    URB InterruptUrb;UCHAR Bitmap=0;memset(&InterruptUrb,0,sizeof(InterruptUrb));
    InterruptUrb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    InterruptUrb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    InterruptUrb.UrbBulkOrInterruptTransfer.TransferBuffer=&Bitmap;
    InterruptUrb.UrbBulkOrInterruptTransfer.TransferBufferLength=1;
    InterruptUrb.UrbBulkOrInterruptTransfer.TransferFlags=USBD_SHORT_TRANSFER_OK;
    UsbTestExpectedPortChange=1;Request=UsbTestNewRequest(5,&InterruptUrb);
    UsbRootInterrupt(TEST_ROOTHUB,Request);UsbWork(TEST_WORKER);
    Slot=UsbTestFindRequest(Request);CHECK(Slot&&Slot->Context.Job==JobRootInterrupt);
    /* Removing an already reported change bit does not create a port event. */
    CHECK(Bitmap==2&&UsbTestUcxRootChanged==1);
    UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  }
  {
    URB PendingInterrupt;UCHAR Bitmap=0;memset(&PendingInterrupt,0,sizeof(PendingInterrupt));
    PendingInterrupt.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    PendingInterrupt.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    PendingInterrupt.UrbBulkOrInterruptTransfer.TransferBuffer=&Bitmap;
    PendingInterrupt.UrbBulkOrInterruptTransfer.TransferBufferLength=1;
    Request=UsbTestNewRequest(6,&PendingInterrupt);UsbRootInterrupt(TEST_ROOTHUB,Request);
    Slot=UsbTestFindRequest(Request);
    CHECK(UsbTestFindQueue(TEST_ROOT_INTERRUPT)->Count==1&&Slot->Context.Job==JobRootInterrupt);
    CHECK(UsbTestRemoveQueuedRequest(TEST_ROOT_INTERRUPT,Request));
    UsbQueuedCancel(TEST_ROOT_INTERRUPT,Request);
    CHECK(Slot->Context.Completed==1&&UsbTestDpcQueued==1);
    UsbTestRunDpc();
    CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_CANCELLED);
  }
}

static VOID UsbTestRootRepeatedChange(VOID) {
  URB Urb={0}; WDFREQUEST Request; TEST_REQUEST_SLOT *Slot;
  const UCHAR Clear[8]={0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,0,1,0,0,0};
  UsbTestInitController();
  UsbTestExpectedPortChange=MUSB_PORT_CHANGE_CONNECTION|MUSB_PORT_CHANGE_ENABLE;
  UsbWork(TEST_WORKER);
  CHECK(UsbTestUcxRootChanged==1);

  /* Another bit disappearing does not make the pending connection bit new. */
  UsbTestExpectedPortChange=MUSB_PORT_CHANGE_CONNECTION;
  UsbWork(TEST_WORKER);
  CHECK(UsbTestUcxRootChanged==1);

  Urb.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER_EX;
  Urb.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER_EX);
  memcpy(Urb.UrbControlTransferEx.SetupPacket,Clear,sizeof(Clear));
  Request=UsbTestNewRequest(0,&Urb);
  UsbRootControl(TEST_ROOTHUB,Request);
  /* The core clears the old event and a new one latches before the same
   * worker reads the bitmap. Both reads of the bitmap are identical. */
  UsbWork(TEST_WORKER);
  UsbTestRunDpc();
  Slot=UsbTestFindRequest(Request);
  CHECK(UsbTestPortFeatureCalls==1&&Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(UsbTestUcxRootChanged==2);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestUcxRootChanged==2);
}

/* Endpoint descriptor and TT routing execute through production callbacks.
 * The two MDL segments must both map before the MUSB transfer mock is entered. */
static VOID UsbTestActualMusbHostCoreBulkOut(VOID) {
  URB Urb={0};UCHAR Data[4]={0x11,0x22,0x33,0x44};
  WDFREQUEST Request;USB_REQUEST *Transfer=NULL;TEST_REQUEST_SLOT *Slot;
  MUSB_HOST_IO Io={0};MUSB_HOST_CONFIG Config={0};USB_CONTROLLER *D=&UsbTestController;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
  D->Registers=UsbTestRegisters;D->Phy=(PUCHAR)UsbTestPhy;
  D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;D->Prepared=TRUE;D->Online=TRUE;
  UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;UsbTestRegisters[USB_REG_POWER]=0x21;
  Io.Context=D;Io.ControllerLength=USB_BYTES;Io.Read8=UsbRead8;Io.Read16=UsbRead16;
  Io.Read32=UsbRead32;Io.Write8=UsbWrite8;Io.Write16=UsbWrite16;Io.Write32=UsbWrite32;
  Io.NowUs=UsbNow;Io.DelayUs=UsbDelay;Io.TryLock=UsbLock;Io.Unlock=UsbUnlock;Io.Cancelled=UsbCancelled;
  Config.Revision=MUSB_HOST_CORE_REVISION;Config.Flags=MUSB_READY_REQUIRED;
  Config.ControllerLength=USB_BYTES;Config.PhyLength=MTK_MUSB_PLATFORM_PHY_BYTES;
  Config.InterruptVector=USB_GSIV;Config.EndpointCount=MUSB_HOST_MAX_ENDPOINTS;
  Config.PortCount=MUSB_HOST_PORT_COUNT;Config.FifoProfile=MUSB_HOST_T70_FIFO_PROFILE;
  Config.InterruptMask=(UCHAR)(MUSB_HOST_CONNECT_DISCONNECT_MASK|MUSB_HOST_RESUME_MASK);
  CHECK(MusbHostInitialize(&D->Core,&Io,&Config)==MusbOk);
  D->Core.PortStatus=MUSB_PORT_POWER|MUSB_PORT_CONNECTION|MUSB_PORT_ENABLE;
  D->Core.State=MUSB_HOST_STATE_OPERATIONAL;
  UsbTestDevice.Address=7;UsbTestDevice.Enabled=TRUE;UsbTestUseRealCoreData=TRUE;
  Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;
  Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=sizeof(Data);
  Request=UsbTestNewRequest(0,&Urb);Slot=UsbTestFindRequest(Request);
  CHECK(UsbNewRequest(D,&UsbTestEndpoint,Request,JobTransfer,&Urb,&Transfer)==STATUS_SUCCESS);
  CHECK(UsbTestUseRealCoreData);
  UsbRunTransfer(D,&UsbTestEndpoint,Transfer);
  CHECK(UsbTestDataCalls==1);
  CHECK(UsbTestUseRealCoreData);
  CHECK(UsbTestRealDataCalls==1);
  CHECK(UsbTestRealCoreStatus==MusbOk);
  CHECK(UsbTestMmioWrites>0);
  CHECK(UsbTestRealCoreStatus==MusbOk);
  CHECK(UsbTestRealCoreBytes==sizeof(Data));
  CHECK(D->CoreLock==0&&D->Core.State==MUSB_HOST_STATE_OPERATIONAL&&UsbTestEndpoint.Toggle==1U);
  CHECK(*(PULONG)(UsbTestRegisters+0x24)==0x44332211U);
  UsbTestRunDpc();
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS&&
      Slot->CompletionInformation==0);
  CHECK(Urb.UrbHeader.Status==USBD_STATUS_SUCCESS&&
      Urb.UrbBulkOrInterruptTransfer.TransferBufferLength==sizeof(Data));
}

static VOID UsbTestEndpointDescriptorRouteAndMdl(VOID) {
  USB_ENDPOINT_DESCRIPTOR Desc={0};PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x900;
  WDFREQUEST Request;TEST_REQUEST_SLOT *Slot;URB Urb;MDL M1,M2;UCHAR Part1[2]={0x11,0x22},Part2[2]={0x33,0x44};
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
  Desc.bLength=sizeof(Desc);Desc.bDescriptorType=USB_ENDPOINT_DESCRIPTOR_TYPE;
  Desc.bEndpointAddress=0x01;Desc.bmAttributes=USB_ENDPOINT_TYPE_BULK;Desc.wMaxPacketSize=64;
  CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Desc,sizeof(Desc),NULL,Init)==STATUS_SUCCESS);
  CHECK(UsbTestEndpoint.Stopped==1&&UsbTestEndpoint.Listed&&UsbTestController.EndpointCount==1);
  CHECK(UsbTestUcxEndpointCreateCalls==1&&UsbTestEndpointCallbacks==1&&UsbTestUcxQueueBindCalls==1);
  CHECK(UsbTestEndpoint.Incoming!=NULL&&UsbTestEndpoint.Pending!=NULL);
  UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
  UsbTestCheckStartReferencesDrained();
  CHECK(!UsbTestEndpoint.Stopped&&UsbTestQueueStarts==2);
  UsbTestDevice.Address=9;UsbTestDevice.TtHub=TEST_TT_HUB;
  memset(&M1,0,sizeof(M1));memset(&M2,0,sizeof(M2));
  M1.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA;M1.MappedSystemVa=Part1;M1.ByteCount=sizeof(Part1);M1.Next=&M2;
  M2.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA;M2.MappedSystemVa=Part2;M2.ByteCount=sizeof(Part2);
  memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBufferMDL=&M1;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=4;
  UsbTestExpectedRoute=1;UsbTestExpectedHubAddress=5;UsbTestExpectedHubPort=3;UsbTestExpectedMultiTt=1;
  UsbTestExpectedEndpointAddress=0x01;UsbTestExpectedTransferLength=4;UsbTestExpectedTimeout=1000;
  UsbTestExpectedProbeMaps=2;UsbTestExpectedOut[0]=0x11;UsbTestExpectedOut[1]=0x22;
  UsbTestExpectedOut[2]=0x33;UsbTestExpectedOut[3]=0x44;UsbTestExpectCoreData=1;
  Request=UsbTestNewRequest(0,&Urb);UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  Slot=UsbTestFindRequest(Request);CHECK(Slot&&Slot->Context.Endpoint==&UsbTestEndpoint&&UsbTestEndpoint.Inflight==1);
  CHECK(UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1&&UsbTestForwardCalls==1);
  UsbTestCoreDone=4;UsbTestClock+=1000;UsbWork(TEST_WORKER);
  CHECK(UsbTestDataCalls==1&&UsbTestMdlMapsAtCore>=2);
  CHECK(Slot->Context.Endpoint==&UsbTestEndpoint&&UsbTestEndpoint.Inflight==1);
  CHECK(UsbTestDpcQueued==1&&Slot->CompletionCount==0);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(Slot->Context.Endpoint==NULL&&UsbTestEndpoint.Inflight==0);
  CHECK(UsbTestDeviceReferences==0);
  CHECK(Urb.UrbHeader.Status==USBD_STATUS_SUCCESS&&Urb.UrbBulkOrInterruptTransfer.TransferBufferLength==4);
  CHECK(UsbTestEndpointReferences==0&&UsbTestPoolInUse==FALSE);
  {
    ULONG CoreBefore=UsbTestDataCalls;MDL Short={0};UCHAR ShortBytes[3]={0};
    Short.MappedSystemVa=ShortBytes;Short.ByteCount=sizeof(ShortBytes);Short.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA;
    memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    Urb.UrbBulkOrInterruptTransfer.TransferBufferMDL=&Short;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=4;
    UsbTestExpectedRoute=0;UsbTestExpectedProbeMaps=1;UsbTestExpectedTransferLength=4;
    Request=UsbTestNewRequest(1,&Urb);UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
    UsbTestClock+=1000;UsbWork(TEST_WORKER);Slot=UsbTestFindRequest(Request);
    CHECK(UsbTestDataCalls==CoreBefore&&Slot->Context.Status==STATUS_INVALID_BUFFER_SIZE);
    UsbTestRunDpc();CHECK(Slot->CompletionCount==1);
  }
  {
    ULONG CoreBefore=UsbTestDataCalls;MDL Bad={0};UCHAR BadBytes[4]={0};
    Bad.MappedSystemVa=BadBytes;Bad.ByteCount=sizeof(BadBytes);Bad.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA;
    UsbTestMdlMapFail=&Bad;UsbTestMdlMapCalls=0;
    memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    Urb.UrbBulkOrInterruptTransfer.TransferBufferMDL=&Bad;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=4;
    UsbTestExpectedRoute=0;UsbTestExpectedProbeMaps=1;UsbTestExpectedTransferLength=4;
    Request=UsbTestNewRequest(2,&Urb);UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
    UsbTestClock+=1000;UsbWork(TEST_WORKER);Slot=UsbTestFindRequest(Request);
    CHECK(UsbTestDataCalls==CoreBefore&&Slot->Context.Status==STATUS_INSUFFICIENT_RESOURCES);
    UsbTestRunDpc();CHECK(Slot->CompletionCount==1);UsbTestMdlMapFail=NULL;
  }
  {
    ULONG CreateBefore=UsbTestUcxEndpointCreateCalls;USB_ENDPOINT_DESCRIPTOR Invalid=Desc;
    Invalid.bEndpointAddress=0x11;
    CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Invalid,sizeof(Invalid),NULL,Init)==STATUS_INVALID_PARAMETER);
    Invalid=Desc;Invalid.bmAttributes=USB_ENDPOINT_TYPE_ISOCHRONOUS;
    CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Invalid,sizeof(Invalid),NULL,Init)==STATUS_NOT_SUPPORTED);
    CHECK(UsbTestUcxEndpointCreateCalls==CreateBefore);
  }
}

/* NAK retry retains the original endpoint/request references on requeue.
 * A cancel invoked during a mock transfer is retired once via the real DPC. */
static VOID UsbTestNakRetryAndCancel(VOID) {
  USB_ENDPOINT_DESCRIPTOR Desc={0};PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x901;
  WDFREQUEST Request;TEST_REQUEST_SLOT *Slot;URB Urb;UCHAR Data[16]={0};USB_REQUEST *RaceRequest=NULL;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_INTERRUPT,0x81,8,1);
  Desc.bLength=sizeof(Desc);Desc.bDescriptorType=USB_ENDPOINT_DESCRIPTOR_TYPE;
  Desc.bEndpointAddress=0x81;Desc.bmAttributes=USB_ENDPOINT_TYPE_INTERRUPT;Desc.wMaxPacketSize=8;Desc.bInterval=1;
  CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Desc,sizeof(Desc),NULL,Init)==STATUS_SUCCESS);
  UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
  UsbTestCheckStartReferencesDrained();
  UsbTestExpectedRoute=0;UsbTestExpectedEndpointAddress=0x81;
  UsbTestExpectedTransferLength=sizeof(Data);UsbTestExpectedTimeout=3;UsbTestExpectedProbeMaps=0;
  memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=sizeof(Data);
  Urb.UrbBulkOrInterruptTransfer.TransferFlags=USBD_TRANSFER_DIRECTION_IN;
  UsbTestCoreResult=MusbTimeout;UsbTestCoreDone=0;Request=UsbTestNewRequest(0,&Urb);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  UsbTestClock+=2000;UsbWork(TEST_WORKER);Slot=UsbTestFindRequest(Request);
  CHECK(UsbTestDataCalls==1&&UsbTestRequeueCalls==1&&Slot->CompletionCount==0);
  CHECK(Slot->Context.Endpoint==&UsbTestEndpoint&&UsbTestEndpoint.Inflight==1);
  CHECK(UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1);
  UsbTestCoreResult=MusbOk;UsbTestCoreDone=sizeof(Data);UsbTestClock+=5000;UsbWork(TEST_WORKER);
  CHECK(UsbTestDataCalls==2&&Slot->Context.Endpoint==&UsbTestEndpoint&&UsbTestEndpoint.Inflight==1);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(UsbTestEndpoint.Inflight==0&&UsbTestEndpointReferences==0);

  /* A multi-packet interrupt URB advances one full packet per scheduled poll. */
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_INTERRUPT,0x81,8,4);
  Desc.bLength=sizeof(Desc);Desc.bDescriptorType=USB_ENDPOINT_DESCRIPTOR_TYPE;
  Desc.bEndpointAddress=0x81;Desc.bmAttributes=USB_ENDPOINT_TYPE_INTERRUPT;Desc.wMaxPacketSize=8;Desc.bInterval=4;
  CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Desc,sizeof(Desc),NULL,Init)==STATUS_SUCCESS);
  UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);UsbTestCheckStartReferencesDrained();
  UsbTestDevice.Enabled=1;UsbTestDevice.Address=7;UsbTestExpectedRoute=0;
  UsbTestExpectedEndpointAddress=0x81;UsbTestExpectedTransferLength=sizeof(Data);UsbTestExpectedTimeout=3;
  UsbTestCoreResult=MusbOk;UsbTestCoreDone=8;
  memset(Data,0,sizeof(Data));memset(&Urb,0,sizeof(Urb));
  Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=sizeof(Data);
  Urb.UrbBulkOrInterruptTransfer.TransferFlags=USBD_TRANSFER_DIRECTION_IN|USBD_SHORT_TRANSFER_OK;
  Request=UsbTestNewRequest(0,&Urb);Slot=UsbTestFindRequest(Request);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  UsbTestClock+=UsbTestEndpoint.PollUs;UsbWork(TEST_WORKER);
  CHECK(UsbTestDataCalls==1&&UsbTestRequeueCalls==1&&Slot->CompletionCount==0&&Slot->Context.Bytes==8);
  CHECK(UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1&&Data[0]==0x40&&Data[7]==0x47);
  CHECK(UsbTestRemoveQueuedRequest(UsbTestEndpoint.Pending,Request));
  UsbTestExpectedTransferLength=8;UsbTestExpectedTimeout=3;UsbTestClock+=UsbTestEndpoint.PollUs;
  UsbRunTransfer(&UsbTestController,&UsbTestEndpoint,&Slot->Context);
  CHECK(UsbTestDataCalls==2&&UsbTestRequeueCalls==1&&Slot->CompletionCount==0&&Slot->Context.Bytes==16);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS&&
      Slot->CompletionInformation==0&&Urb.UrbBulkOrInterruptTransfer.TransferBufferLength==16);
  CHECK(Data[0]==0x40&&Data[7]==0x47&&Data[8]==0x40&&Data[15]==0x47);
  CHECK(UsbTestEndpoint.Inflight==0&&UsbTestEndpointReferences==0);

  UsbTestEndpoint.PollUs=5000000U;UsbTestExpectedRoute=0;
  UsbTestExpectedEndpointAddress=0x81;UsbTestExpectedTransferLength=8;UsbTestExpectedTimeout=3;
  UsbTestCoreResult=MusbTimeout;UsbTestCoreDone=0;
  memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=8;
  Urb.UrbBulkOrInterruptTransfer.TransferFlags=USBD_TRANSFER_DIRECTION_IN;
  Request=UsbTestNewRequest(1,&Urb);Slot=UsbTestFindRequest(Request);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  UsbTestClock+=UsbTestEndpoint.PollUs;UsbWork(TEST_WORKER);
  CHECK(UsbTestDataCalls==3&&UsbTestExpectedTimeout==3&&UsbTestRequeueCalls==2&&Slot->CompletionCount==0);
  CHECK(UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1);
  CHECK(UsbTestRemoveQueuedRequest(UsbTestEndpoint.Pending,Request));
  UsbQueuedCancel(UsbTestEndpoint.Pending,Request);UsbTestRunDpc();
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_CANCELLED);

  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_INTERRUPT,0x81,8,1);
  UsbTestEndpoint.Stopped=0;UsbTestDevice.Enabled=1;UsbTestDevice.Address=7;
  UsbTestExpectedEndpointAddress=0x81;UsbTestExpectedTransferLength=sizeof(Data);UsbTestExpectedTimeout=3;
  UsbTestCoreResult=MusbTimeout;UsbTestCoreDone=0;
  memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=sizeof(Data);
  Urb.UrbBulkOrInterruptTransfer.TransferFlags=USBD_TRANSFER_DIRECTION_IN;
  Request=UsbTestNewRequest(0,&Urb);Slot=UsbTestFindRequest(Request);
  CHECK(UsbNewRequest(&UsbTestController,&UsbTestEndpoint,Request,JobTransfer,&Urb,&RaceRequest)==STATUS_SUCCESS);
  UsbTestLastRequestHandle=(ULONG_PTR)Request;UsbTestCancelAtUnmark=1;
  UsbRunTransfer(&UsbTestController,&UsbTestEndpoint,RaceRequest);
  CHECK(UsbTestRequeueCalls==0&&RaceRequest->Cancelled&&RaceRequest->Completed==1);
  CHECK(RaceRequest->Status==STATUS_CANCELLED&&RaceRequest->CancelDone.Header.SignalState==1);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_CANCELLED);
  CHECK(UsbTestEndpoint.Inflight==0&&UsbTestEndpointReferences==0);

  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
  UsbTestEndpoint.Stopped=0;UsbTestDevice.Enabled=1;UsbTestDevice.Address=7;
  UsbTestExpectedEndpointAddress=0x01;UsbTestExpectedTransferLength=4;UsbTestExpectedTimeout=1000;
  UsbTestExpectedRoute=0;UsbTestExpectedProbeMaps=0;UsbTestExpectCoreData=0;
  UsbTestCoreResult=MusbOk;UsbTestCoreDone=4;UsbTestCancelInCore=1;
  memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=4;
  Request=UsbTestNewRequest(0,&Urb);Slot=UsbTestFindRequest(Request);
  USB_REQUEST *R=NULL;CHECK(UsbNewRequest(&UsbTestController,&UsbTestEndpoint,Request,JobTransfer,&Urb,&R)==STATUS_SUCCESS);
  UsbTestLastRequestHandle=(ULONG_PTR)Request;UsbRunTransfer(&UsbTestController,&UsbTestEndpoint,R);
  CHECK(Slot->Context.Cancelled&&Slot->Context.Completed==1&&Slot->CompletionCount==0);
  CHECK(Slot->Context.Status==STATUS_CANCELLED&&Slot->Context.CancelDone.Header.SignalState==1);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_CANCELLED);
  UsbQueuedCancel(UsbTestEndpoint.Pending,Request);UsbTestRunDpc();
  CHECK(Slot->CompletionCount==1&&UsbTestDpcCompletes==1&&UsbTestEndpoint.Inflight==0);

  {
    USB_ENDPOINT_DESCRIPTOR QueuedDesc={0};PUCXENDPOINT_INIT QueuedInit=(PUCXENDPOINT_INIT)(ULONG_PTR)0x902;
    UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
    QueuedDesc.bLength=sizeof(QueuedDesc);QueuedDesc.bDescriptorType=USB_ENDPOINT_DESCRIPTOR_TYPE;
    QueuedDesc.bEndpointAddress=0x01;QueuedDesc.bmAttributes=USB_ENDPOINT_TYPE_BULK;QueuedDesc.wMaxPacketSize=64;
    CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&QueuedDesc,sizeof(QueuedDesc),NULL,QueuedInit)==STATUS_SUCCESS);
    UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
    UsbTestCheckStartReferencesDrained();
    memset(&Urb,0,sizeof(Urb));Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
    Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
    Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=4;
    Request=UsbTestNewRequest(0,&Urb);UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
    Slot=UsbTestFindRequest(Request);
    CHECK(UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1&&UsbTestEndpoint.Inflight==1);
    CHECK(UsbTestRemoveQueuedRequest(UsbTestEndpoint.Pending,Request));
    UsbQueuedCancel(UsbTestEndpoint.Pending,Request);
    CHECK(Slot->Context.Completed==1&&UsbTestEndpoint.Inflight==1&&UsbTestDpcQueued==1);
    UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_CANCELLED);
    CHECK(UsbTestEndpoint.Inflight==0&&UsbTestEndpointReferences==0);
  }
}

static VOID UsbTestColdFailureRetainsCleanupClock(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  UsbTestInitController();
  D->ResumeClock=0x3456U;
  D->ColdStartPending=TRUE;
  D->ColdStartOwned=TRUE;
  D->ColdPhyBound=TRUE;
  D->ColdHost.Signature=0x4d555342U;
  CHECK(UsbD0Failed(D,STATUS_DEVICE_NOT_READY)==STATUS_DEVICE_NOT_READY);
  CHECK(D->ResumeClock==0x3456U && D->ColdStartPending &&
        D->ColdStartOwned && D->ColdPhyBound &&
        D->ColdHost.Signature==0x4d555342U);
  for(ULONG Kind=0;Kind<4U;++Kind) {
    UsbTestInitController();
    D->ResumeClock=0x3456U;
    if(Kind==0U)D->ColdStartPending=TRUE;
    if(Kind==1U)D->ColdStartOwned=TRUE;
    if(Kind==2U)D->ColdPhyBound=TRUE;
    if(Kind==3U)D->ColdHost.Signature=0x4d555342U;
    UsbWaitForVbus(D);
    UsbFinishVbusWait(D);
    CHECK(!D->WaitingForVbus && D->ResumeClock==0x3456U);
    D->ColdStartPending=D->ColdStartOwned=D->ColdPhyBound=FALSE;
    D->ColdHost.Signature=0U;D->ResumeClock=0x6789U;
    UsbFinishVbusWait(D);
    CHECK(D->ResumeClock==0U);
  }
}

static VOID UsbTestColdHaltFailureStillClosesTcpc(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  UsbTestInitController();
  D->ColdStartPending=TRUE;D->ColdStartOwned=TRUE;
  D->Core.Initialized=TRUE;D->Core.State=MUSB_HOST_STATE_OPERATIONAL;
  D->TcpcTarget=TEST_TCPC_TARGET;
  UsbTestTcpc.Created=TRUE;UsbTestTcpc.Opened=TRUE;
  UsbTestCoreStateResult=MusbIoError;
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_IO_DEVICE_ERROR);
  CHECK(UsbTestTcpc.Close==1U && UsbTestTcpc.Delete==1U &&
        !UsbTestTcpc.Opened && !UsbTestTcpc.Created);
}

static VOID UsbTestColdPhyModeFailureAfterPowerOn(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  UsbTestInitController();
  D->ColdStartPending=TRUE;D->ColdStartOwned=TRUE;D->WaitingForVbus=TRUE;
  D->WorkerThread=UsbTestThread;
  D->Registers=UsbTestRegisters;D->Phy=(PUCHAR)UsbTestPhy;
  D->PhyBytes=MTK_MUSB_PLATFORM_PHY_REG_BYTES;
  D->ResumeClock=UsbTestClockIdentity;D->ResumeTuningValid=TRUE;
  D->ResumeTuning.Values[0]=0x00007000U;
  D->ResumeTuning.Values[1]=0x00984400U;
  D->ResumeTuning.Values[2]=0x50100075U;
  UsbTestFailColdMode=TRUE;
  NTSTATUS Status=UsbColdPhyPrepare(D);
  if(Status!=STATUS_IO_DEVICE_ERROR) {
    switch(Status) {
      case STATUS_DEVICE_NOT_READY: CHECK(0); break;
      case STATUS_NOT_SUPPORTED: CHECK(0); break;
      case STATUS_INVALID_PARAMETER: CHECK(0); break;
      case STATUS_DEVICE_CONFIGURATION_ERROR: CHECK(0); break;
      case STATUS_INSUFFICIENT_RESOURCES: CHECK(0); break;
      default: CHECK(0); break;
    }
  }
  CHECK(Status==STATUS_IO_DEVICE_ERROR);
  CHECK(UsbTestColdModeFailureObserved && D->ColdPhyBound &&
        MtkUsbPhyCoreIsPowered(&D->ColdPhy) &&
        D->ColdPhyMode==MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN);
  UsbTestFailColdMode=FALSE;
  CHECK(UsbColdPhyRelease(D)==STATUS_SUCCESS && !D->ColdPhyBound &&
        !MtkUsbPhyCoreIsPowered(&D->ColdPhy));
}

static VOID UsbTestInterruptOutTimeoutAndBulkZlp(VOID) {
  URB Urb={0};
  UCHAR Data[8]={0};
  WDFREQUEST Request;
  TEST_REQUEST_SLOT *Slot;
  USB_REQUEST *Transfer=NULL;

  UsbTestInitController();
  UsbTestInitEndpoint(USB_ENDPOINT_TYPE_INTERRUPT,0x01,8,4);
  UsbTestEndpoint.Stopped=0;UsbTestDevice.Enabled=1;UsbTestDevice.Address=7;
  UsbTestExpectedEndpointAddress=0x01;
  UsbTestExpectedTransferLength=sizeof(Data);
  UsbTestExpectedTimeout=1000;
  UsbTestCoreResult=MusbTimeout;UsbTestCoreDone=0;
  Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;
  Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=sizeof(Data);
  Request=UsbTestNewRequest(0,&Urb);Slot=UsbTestFindRequest(Request);
  CHECK(UsbNewRequest(&UsbTestController,&UsbTestEndpoint,Request,
      JobTransfer,&Urb,&Transfer)==STATUS_SUCCESS);
  UsbRunTransfer(&UsbTestController,&UsbTestEndpoint,Transfer);
  CHECK(UsbTestDataCalls==1U && UsbTestLastTransferType==MUSB_TRANSFER_INTERRUPT);
  CHECK(UsbTestRequeueCalls==0U && Slot->CompletionCount==0U);
  UsbTestRunDpc();
  CHECK(Slot->CompletionCount==1U && Slot->CompletionStatus==STATUS_IO_TIMEOUT);

  UsbTestInitController();
  UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
  UsbTestEndpoint.Stopped=0;UsbTestDevice.Enabled=1;UsbTestDevice.Address=7;
  UsbTestExpectedEndpointAddress=0x01;
  UsbTestExpectedTransferLength=0U;UsbTestExpectedTimeout=1000U;
  UsbTestCoreResult=MusbOk;UsbTestCoreDone=0U;
  memset(&Urb,0,sizeof(Urb));
  Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=NULL;
  Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=0U;
  Request=UsbTestNewRequest(1,&Urb);Slot=UsbTestFindRequest(Request);
  Transfer=NULL;
  CHECK(UsbNewRequest(&UsbTestController,&UsbTestEndpoint,Request,
      JobTransfer,&Urb,&Transfer)==STATUS_SUCCESS);
  UsbRunTransfer(&UsbTestController,&UsbTestEndpoint,Transfer);
  CHECK(UsbTestDataCalls==1U && UsbTestLastTransferType==MUSB_TRANSFER_BULK);
  UsbTestRunDpc();
  CHECK(Slot->CompletionCount==1U && Slot->CompletionStatus==STATUS_SUCCESS &&
        Slot->CompletionInformation==0U);
}

/* Two queue purge callbacks and the active request must drain before both UCX
 * completion callbacks run and the endpoint reference is released. */
static VOID UsbTestPurgeAbortFence(VOID) {
  WDFREQUEST Request;USB_REQUEST *R=NULL;TEST_REQUEST_SLOT *Slot;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
  UsbTestEndpoint.Incoming=TEST_MANAGEMENT;UsbTestEndpoint.Pending=TEST_ROOT_INTERRUPT;
  Request=UsbTestNewRequest(0,NULL);
  CHECK(UsbNewRequest(&UsbTestController,&UsbTestEndpoint,Request,JobTransfer,NULL,&R)==STATUS_SUCCESS);
  Slot=UsbTestFindRequest(Request);CHECK(UsbTestEndpoint.Inflight==1&&UsbTestEndpointReferences==1);
  UsbEndpointPurge(TEST_UCX,TEST_ENDPOINT);UsbEndpointAbort(TEST_UCX,TEST_ENDPOINT);
  CHECK(UsbTestEndpoint.Stopped&&UsbTestEndpoint.StopKind==3&&UsbTestEndpoint.Purges==2);
  CHECK(UsbTestStopCount==2&&UsbTestUcxPurgeDoneCount==0&&UsbTestUcxAbortDoneCount==0);
  UsbTestFireQueueStop(UsbTestEndpoint.Incoming);
  CHECK(UsbTestEndpoint.Purges==1&&UsbTestUcxPurgeDoneCount==0&&UsbTestUcxAbortDoneCount==0);
  UsbTestFireQueueStop(UsbTestEndpoint.Pending);
  CHECK(UsbTestEndpoint.Purges==0&&UsbTestEndpoint.Inflight==1);
  CHECK(UsbTestUcxPurgeDoneCount==0&&UsbTestUcxAbortDoneCount==0);
  UsbFinish(R,STATUS_CANCELLED,USBD_STATUS_CANCELED,0);
  CHECK(Slot->Context.Completed==1&&UsbTestUcxPurgeDoneCount==0);
  UsbTestRunDpc();
  CHECK(Slot->CompletionCount==1&&UsbTestEndpoint.Inflight==0);
  CHECK(UsbTestUcxPurgeDoneCount==1&&UsbTestUcxAbortDoneCount==1&&UsbTestEndpoint.StopKind==0);
  CHECK(UsbTestEndpointReferences==0);
  UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);UsbTestCheckStartReferencesDrained();
  CHECK(!UsbTestEndpoint.Stopped&&UsbTestQueueStarts==2);
}

/* Exact resource identity, read-only handoff, fresh MHRD response and live
 * DEVCTL gate must pass before D0 starts. The snapshot is adopted pre-operational. */
static VOID UsbTestFillResources(VOID) {
  UsbTestController.Online=0;UsbTestController.Stopping=1;
  UsbTestRawCount=UsbTestTranslatedCount=3;
  UsbTestRaw[0].Type=UsbTestTranslated[0].Type=CmResourceTypeMemory;
  UsbTestRaw[0].u.Memory.Start.QuadPart=UsbTestTranslated[0].u.Memory.Start.QuadPart=USB_BASE;
  UsbTestRaw[0].u.Memory.Length=UsbTestTranslated[0].u.Memory.Length=USB_BYTES;
  UsbTestRaw[1].Type=UsbTestTranslated[1].Type=CmResourceTypeMemory;
  UsbTestRaw[1].u.Memory.Start.QuadPart=UsbTestTranslated[1].u.Memory.Start.QuadPart=USB_HANDOFF;
  UsbTestRaw[1].u.Memory.Length=UsbTestTranslated[1].u.Memory.Length=USB_HANDOFF_BYTES;
  UsbTestTranslated[1].Flags=CM_RESOURCE_MEMORY_READ_ONLY;
  UsbTestRaw[2].Type=UsbTestTranslated[2].Type=CmResourceTypeInterrupt;
  UsbTestRaw[2].u.Interrupt.Vector=UsbTestTranslated[2].u.Interrupt.Vector=USB_GSIV;
}
static VOID UsbTestFillHandoff(VOID) {
  UsbTestHandoff[0]=1;UsbTestHandoff[1]=1;UsbTestHandoff[2]=MUSB_READY_REQUIRED;
  UsbTestHandoff[3]=(ULONG)USB_BASE;UsbTestHandoff[4]=USB_BYTES;
  UsbTestHandoff[5]=0x11f40000;UsbTestHandoff[6]=USB_BYTES;UsbTestHandoff[7]=USB_GSIV;
  UsbTestHandoff[8]=16;UsbTestHandoff[9]=1;UsbTestHandoff[10]=MUSB_HOST_T70_FIFO_PROFILE;
  UsbTestHandoff[11]=MUSB_HOST_CONNECT_DISCONNECT_MASK;
  UsbTestHandoff[12]=MUSB_PORT_POWER;UsbTestHandoff[13]=0;UsbTestHandoff[14]=1;UsbTestHandoff[15]=0;
  UsbTestExpectedAdoptStatus=UsbTestHandoff[12];UsbTestExpectedAdoptChange=UsbTestHandoff[13];
}
static VOID UsbTestHandoffPowerPathValidation(VOID) {
  static CONST struct { ULONG Revision,Flags,Path; BOOLEAN Valid; } Cases[]={
    {USB_HANDOFF_REVISION_LEGACY,USB_HANDOFF_FLAGS_TYPEC_SOURCE,USB_HANDOFF_PATH_LEGACY,TRUE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_TYPEC_SOURCE,USB_HANDOFF_PATH_TYPEC_SOURCE,TRUE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_TYPEC_SOURCE,USB_HANDOFF_PATH_TYPEC_ONLY,TRUE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER,USB_HANDOFF_PATH_ID_LOW_HUB_POWER,TRUE},
    {USB_HANDOFF_REVISION_LEGACY,USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER,USB_HANDOFF_PATH_ID_LOW_HUB_POWER,FALSE},
    {USB_HANDOFF_REVISION_LEGACY,USB_HANDOFF_FLAGS_TYPEC_SOURCE,USB_HANDOFF_PATH_TYPEC_SOURCE,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_COMMON,USB_HANDOFF_PATH_ID_LOW_HUB_POWER,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,0xffU,USB_HANDOFF_PATH_ID_LOW_HUB_POWER,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_TYPEC_SOURCE,USB_HANDOFF_PATH_LEGACY,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_TYPEC_SOURCE,USB_HANDOFF_PATH_ID_LOW_HUB_POWER,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_COMMON,USB_HANDOFF_PATH_TYPEC_ONLY,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER,USB_HANDOFF_PATH_TYPEC_SOURCE,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER,3U,FALSE},
    {USB_HANDOFF_REVISION_POWER_PATH,USB_HANDOFF_FLAGS_TYPEC_SOURCE|0x100U,USB_HANDOFF_PATH_TYPEC_SOURCE,FALSE},
    {3U,USB_HANDOFF_FLAGS_TYPEC_SOURCE,USB_HANDOFF_PATH_TYPEC_SOURCE,FALSE}
  };
  for(ULONG I=0;I<ARRAYSIZE(Cases);I++) {
    USB_HANDOFF_STATE State={0xaaaaaaaaU,0xffffU,0xffffU};
    UsbTestInitController();UsbTestFillHandoff();UsbTestController.Handoff=UsbTestHandoff;
    UsbTestHandoff[1]=Cases[I].Revision;UsbTestHandoff[2]=Cases[I].Flags;UsbTestHandoff[15]=Cases[I].Path;
    CHECK(UsbValidHandoff(&UsbTestController,&State)==Cases[I].Valid);
    CHECK(State.Flags==(Cases[I].Valid?Cases[I].Flags:0U));
    CHECK(State.PortStatus==(Cases[I].Valid?MUSB_PORT_POWER:0U)&&State.PortChange==0U);
    CHECK(UsbTestMmioWrites==0);
  }
}
static VOID UsbTestResourcesAndD0Gate(VOID) {
  NTSTATUS S;UsbTestInitController();UsbTestFillResources();
  UsbTestRaw[0].u.Memory.Start.QuadPart++;
  S=UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES);
  CHECK(S==STATUS_DEVICE_CONFIGURATION_ERROR&&UsbTestMapCalls==0&&UsbTestMmioWrites==0);
  UsbTestInitController();UsbTestFillResources();UsbTestTranslated[1].Flags=0;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR&&UsbTestMapCalls==0);
  UsbTestInitController();UsbTestFillResources();UsbTestRaw[2].u.Interrupt.Vector++;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR&&UsbTestMapCalls==0);
  UsbTestInitController();UsbTestFillResources();UsbTestRawCount--;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR&&UsbTestMapCalls==0);

  UsbTestInitController();UsbTestFillResources();UsbTestMapFailAt=2;
  S=UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES);
  CHECK(S==STATUS_INSUFFICIENT_RESOURCES&&UsbTestMapCalls==2&&UsbTestUnmapCalls==1);
  CHECK(UsbTestController.Registers==NULL&&UsbTestController.Handoff==NULL&&!UsbTestController.Prepared);

  UsbTestInitController();UsbTestFillResources();
  UsbTestTranslated[2].u.Interrupt.Vector=0x90;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  CHECK(UsbTestController.Prepared&&UsbTestController.Registers==UsbTestRegisters&&UsbTestController.Handoff==UsbTestHandoff);
  // No record and no live clock: the controller may be unpowered. No MMIO.
  UsbTestClockIdentity=0;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
  CHECK(UsbTestTakeOverCalls==0&&UsbTestCoreInitCalls==0&&UsbTestMmioWrites==0);
  UsbTestFillHandoff();
  UsbTestTakeOverResult=MtkMusbPlatformIoError;UsbTestRegisters[0x60]=0x1d;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
  CHECK(UsbTestTakeOverCalls==1&&UsbTestCoreInitCalls==0);
  UsbTestTakeOverResult=MtkMusbPlatformSuccess;
  UsbTestExpectedCoreFlags=USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
  UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
  UsbTestHandoff[2]=UsbTestExpectedCoreFlags;
  UsbTestHandoff[15]=USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
  /* Firmware can leave the endpoints busy and the FIFO layout unknown, and the
   * sideband clock, route and Type-C readers can all fail. */
  UsbTestIdleResult=MtkMusbPlatformQuiescenceFailed;
  UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
  UsbTestClockStatus=STATUS_IO_TIMEOUT;UsbTestRouteStatus=STATUS_IO_TIMEOUT;
  UsbTestTypecStatus=STATUS_IO_TIMEOUT;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbTestTakeOverCalls==2&&UsbTestCoreInitCalls==1&&UsbTestCoreAdoptCalls==1&&UsbTestCoreStateCalls==1);
  CHECK(UsbTestController.Core.PortStatus==MUSB_PORT_POWER&&UsbTestController.Core.PortChangeStatus==0);
  CHECK(UsbTestController.Online&&!UsbTestController.Stopping&&UsbTestController.SeenD0);
  CHECK(UsbTestQueueStarts==2&&UsbTestTimerStarts==1);
  UsbTestCheckStartReferencesDrained();
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  UsbTestCheckStartReferencesDrained();
  CHECK(!UsbTestController.Online&&UsbTestController.Stopping);
  CHECK(UsbTestCoreStateCalls==2&&UsbTestController.Core.State==MUSB_HOST_STATE_HALTED);
  CHECK(UsbTestWorkFlushes==1&&UsbTestTimerStops==1&&UsbTestDpcFlushes==1);
}
static VOID UsbTestWorkSerialization(VOID) {
  UsbTestInitController();
  UsbTestReenterWorker=TRUE;

  UsbTimer(TEST_TIMER);
  UsbTimer(TEST_TIMER);
  CHECK(UsbTestWorkEnqueues==1);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestPortCalls==1&&UsbTestWorkEnqueues==1);
  CHECK(UsbTestReentrantD0ExitStatus==STATUS_DEVICE_BUSY);
  CHECK(UsbTestController.Online&&!UsbTestController.Stopping&&UsbTestWorkFlushes==0);

  UsbTimer(TEST_TIMER);
  CHECK(UsbTestWorkEnqueues==2);
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  UsbWork(TEST_WORKER);
  UsbTimer(TEST_TIMER);
  CHECK(UsbTestPortCalls==1&&UsbTestWorkEnqueues==2&&UsbTestWorkFlushes==1);
}

static VOID UsbTestFrameAdmission(VOID) {
  ULONG Frame=0;
  UsbTestInitController();
  UsbTestController.Registers=UsbTestRegisters;
  UsbTestRegisters[0x0c]=0x23;
  UsbTestRegisters[0x0d]=0xf9;
  CHECK(UsbFrame(TEST_UCX,&Frame)==STATUS_SUCCESS&&Frame==0x123);
  CHECK(UsbTestMmioReads==1&&!UsbTestEndpointLockHeld);

  Frame=0;
  UsbTestController.Stopping=1;
  CHECK(UsbFrame(TEST_UCX,&Frame)==STATUS_DEVICE_NOT_READY&&Frame==0);
  UsbTestController.Stopping=0;
  UsbTestController.Online=0;
  CHECK(UsbFrame(TEST_UCX,&Frame)==STATUS_DEVICE_NOT_READY&&Frame==0);
  CHECK(UsbTestMmioReads==1&&!UsbTestEndpointLockHeld);
}

static VOID UsbTestPhyResourcesAndState(VOID) {
  static const ULONG Values[]={0x009c4400,0x50100475,0x02000000,0x00003f2d};
  static const ULONG Offsets[]={
    MTK_MUSB_PLATFORM_PHY_ACR1,MTK_MUSB_PLATFORM_PHY_ACR6,
    MTK_MUSB_PLATFORM_PHY_DTM0,MTK_MUSB_PLATFORM_PHY_DTM1
  };

  UsbTestInitController();UsbTestFillResources();
  UsbTestRawCount=UsbTestTranslatedCount=4;
  UsbTestRaw[3]=UsbTestTranslated[3]=UsbTestRaw[2];
  UsbTestRaw[2].Type=UsbTestTranslated[2].Type=CmResourceTypeMemory;
  UsbTestRaw[2].u.Memory.Start.QuadPart=UsbTestTranslated[2].u.Memory.Start.QuadPart=MTK_MUSB_PLATFORM_PHY_BASE;
  UsbTestRaw[2].u.Memory.Length=UsbTestTranslated[2].u.Memory.Length=MTK_MUSB_PLATFORM_PHY_BYTES;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR);
  UsbTestTranslated[2].Flags=CM_RESOURCE_MEMORY_READ_ONLY;
  ++UsbTestTranslated[2].u.Memory.Start.QuadPart;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR);
  --UsbTestTranslated[2].u.Memory.Start.QuadPart;
  CHECK(UsbTestMapCalls==0);

  UsbTestMapFailAt=3;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_INSUFFICIENT_RESOURCES);
  CHECK(UsbTestUnmapCalls==2&&!UsbTestController.Registers&&!UsbTestController.Handoff&&!UsbTestController.Phy);
  UsbTestMapFailAt=0;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  CHECK(UsbTestController.Phy==(PUCHAR)UsbTestPhy);

  UsbTestFillHandoff();UsbTestRegisters[0x60]=0x1d;
  for(ULONG I=0;I<ARRAYSIZE(Values);++I) UsbTestPhy[Offsets[I]/sizeof(ULONG)]=Values[I];
  {
    ULONG Reads=UsbTestMmioReads;
    UsbTestHandoff[0]=0;UsbTestClockIdentity=0;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    CHECK(UsbTestTakeOverCalls==0&&UsbTestMmioWrites==0&&UsbTestMmioReads>Reads);
    UsbTestHandoff[0]=1;
    CHECK(!UsbTestController.Online&&UsbTestIdleCalls==0&&UsbTestCoreInitCalls==0);
    UsbTestClockStatus=STATUS_SUCCESS;UsbTestClockIdentity=199682;
  }
  UsbTestFillHandoff();
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbTestCoreInitCalls==1&&UsbTestCoreAdoptCalls==1&&UsbTestTakeOverCalls==1);
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  CHECK(UsbTestUnmapCalls==5&&!UsbTestController.Phy&&!UsbTestController.Prepared);
}

static VOID UsbTestRetainedPower(VOID) {
  static const struct {
    WDF_POWER_DEVICE_STATE Target, Previous;
    NTSTATUS Result;
  } Cases[]={
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_SUCCESS},
    {WdfPowerDeviceD1,WdfPowerDeviceD1,STATUS_SUCCESS},
    {WdfPowerDeviceD2,WdfPowerDeviceD2,STATUS_SUCCESS},
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // clock gone
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // changed parent
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // clock timeout
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // role became peripheral
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // PHY lost
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // controller held in reset
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // reset FIFO geometry
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // owned DMA/FIFO not idle
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // port poll failure
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // core not operational
    {WdfPowerDeviceD3,WdfPowerDeviceD2,STATUS_NOT_SUPPORTED},
    {WdfPowerDeviceD3Final,WdfPowerDeviceD3Final,STATUS_NOT_SUPPORTED},
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_NOT_SUPPORTED}, // older DSDT
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_NOT_SUPPORTED}, // UCX reported controller failed
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // unmapped
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_NOT_SUPPORTED}, // no PHY resource
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_SUCCESS}, // rebuild a lost FIFO layout
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // FIFO loss with active DMA
    {WdfPowerDeviceD3,WdfPowerDeviceD3,STATUS_DEVICE_NOT_READY}, // role lost during FIFO check
  };
  for(ULONG I=0;I<ARRAYSIZE(Cases);++I) {
    ULONG Reads,ClockCalls,Starts;
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    if(I!=18) {
      UsbTestController.Phy=(PUCHAR)UsbTestPhy;
      UsbTestController.PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    }
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();UsbTestRegisters[0x60]=0x3d;
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestClockIdentity=199682;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    if(I==15)UsbTestClockIdentity=0;
    CHECK(UsbD0Exit(TEST_DEVICE,Cases[I].Target)==STATUS_SUCCESS);
    CHECK(!UsbTestController.Online&&UsbTestController.Stopping);
    CHECK(UsbTestQueuePurges==2&&UsbTestTimerStops==1);
    UsbTestHandoff[0]=0; // Runtime resume must not re-read the boot-only record.
    switch(I) {
      case 3: UsbTestClockIdentity=0;break;
      case 4: UsbTestClockIdentity++;break;
      case 5: UsbTestClockStatus=STATUS_IO_TIMEOUT;break;
      case 6: UsbTestRegisters[0x60]=0x99;break;
      case 7: UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]^=0x200;break;
      case 8: UsbTestRegisters[1]=0x08;break;
      case 9: UsbTestFifoResult=MtkMusbPlatformProfileMismatch;break;
      case 10: UsbTestIdleResult=MtkMusbPlatformQuiescenceFailed;break;
      case 11: UsbTestCoreResult=MusbTimeout;break;
      case 12: UsbTestCoreStateResult=MusbTimeout;break;
      case 16: UsbControllerReset(TEST_UCX);break;
      case 17: CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);break;
      case 19: UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
        UsbTestFifoRestoreResult=MtkMusbPlatformSuccess;break;
      case 20: UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
        UsbTestIdleResult=MtkMusbPlatformQuiescenceFailed;break;
      case 21: UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
        UsbTestDevctlAfterFifo=0x99;break;
    }
    Reads=UsbTestMmioReads;ClockCalls=UsbTestClockCalls;Starts=UsbTestQueueStarts;
    CHECK(UsbD0Entry(TEST_DEVICE,Cases[I].Previous)==Cases[I].Result);
    CHECK(UsbTestCoreInitCalls==1&&UsbTestCoreAdoptCalls==1&&UsbTestAcpiCalls==0);
    if(I>=3&&I<=5)CHECK(UsbTestMmioReads==Reads&&UsbTestClockCalls==ClockCalls+1);
    if(I>=13&&I<=18)CHECK(UsbTestMmioReads==Reads&&UsbTestClockCalls==ClockCalls);
    CHECK(UsbTestGateCalls==(I==3?1U:0U));
    CHECK(UsbTestFifoRestores==((I==9||I==19)?1U:0U));
    if(NT_SUCCESS(Cases[I].Result)) {
      CHECK(UsbTestController.Online&&!UsbTestController.Stopping);
      CHECK(UsbTestQueueStarts==Starts+2&&UsbTestTimerStarts==2);
      CHECK(UsbTestPortCalls==1&&UsbTestAckCalls==1&&UsbTestControllerFailed==0);
      CHECK(UsbTestController.Core.PortStatus==UsbTestExpectedAdoptStatus);
      UsbTestFifoResult=MtkMusbPlatformSuccess;
      CHECK(UsbD0Exit(TEST_DEVICE,Cases[I].Target)==STATUS_SUCCESS);
      CHECK(UsbD0Entry(TEST_DEVICE,Cases[I].Previous)==STATUS_SUCCESS);
      CHECK(UsbTestCoreInitCalls==1&&UsbTestCoreAdoptCalls==1&&UsbTestAckCalls==2);
    } else {
      CHECK(!UsbTestController.Online&&UsbTestController.Stopping);
      CHECK(UsbTestQueueStarts==Starts&&UsbTestTimerStarts==1);
      CHECK(UsbTestControllerFailed==(I==16?2:1));
      CHECK(UsbTestAckCalls==((I>=9&&I<=12)||I==20?1U:0U));
    }
    UsbTestCheckStartReferencesDrained();
  }
}

static void UsbTestResumePortRequest(ULONG Index,UCHAR Type,UCHAR Code,UCHAR Feature,
                                    UCHAR *Data,ULONG Bytes,NTSTATUS Result) {
  URB Urb={0};
  WDFREQUEST Request;
  TEST_REQUEST_SLOT *Slot;
  Urb.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER_EX;
  Urb.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER_EX);
  Urb.UrbControlTransferEx.TransferBuffer=Data;
  Urb.UrbControlTransferEx.TransferBufferLength=Bytes;
  Urb.UrbControlTransferEx.SetupPacket[0]=Type;
  Urb.UrbControlTransferEx.SetupPacket[1]=Code;
  Urb.UrbControlTransferEx.SetupPacket[2]=Feature;
  Urb.UrbControlTransferEx.SetupPacket[4]=1;
  Urb.UrbControlTransferEx.SetupPacket[6]=(UCHAR)Bytes;
  Request=UsbTestNewRequest(Index,&Urb);
  UsbRootControl(TEST_ROOTHUB,Request);
  UsbWork(TEST_WORKER);UsbTestRunDpc();
  Slot=UsbTestFindRequest(Request);
  CHECK(Slot->CompletionCount==1 && Slot->CompletionStatus==Result);
}
static VOID UsbTestTypecVbusGate(VOID) {
  static const struct {
    ULONG Path, Sample;
    NTSTATUS AcpiStatus;
    BOOLEAN BadReply,Ready;
  } Cases[]={
    {1,0x0105020c,STATUS_SUCCESS,FALSE,TRUE},  // Rd on CC1
    {3,0x0105080c,STATUS_SUCCESS,FALSE,TRUE},  // Rd on CC2
    {1,0x014a020c,STATUS_SUCCESS,FALSE,TRUE},  // DRP settled as a source
    {1,0,STATUS_OBJECT_NAME_NOT_FOUND,FALSE,TRUE}, // earlier AML
    {2,0,STATUS_IO_TIMEOUT,FALSE,TRUE},       // dedicated ID-low hub
    {0,0x0105020c,STATUS_SUCCESS,FALSE,TRUE},  // legacy Type-C handoff
    {1,0,STATUS_SUCCESS,FALSE,FALSE},       // SPB not ready or no VBUS
    {1,0x0205020c,STATUS_SUCCESS,FALSE,FALSE}, // unknown reply version
    {1,0x01050208,STATUS_SUCCESS,FALSE,FALSE}, // VBUS disappeared
    {1,0x01050204,STATUS_SUCCESS,FALSE,FALSE}, // detector disabled
    {1,0x0105024c,STATUS_SUCCESS,FALSE,FALSE}, // TCPC initializing
    {1,0x01050a0c,STATUS_SUCCESS,FALSE,FALSE}, // two Rd terminations
    {1,0x0105220c,STATUS_SUCCESS,FALSE,FALSE}, // still looking for a connection
    {1,0x014a120c,STATUS_SUCCESS,FALSE,FALSE}, // DRP settled as a sink
    {1,0x010a020c,STATUS_SUCCESS,FALSE,FALSE}, // fixed sink
    {1,0x0105020c,STATUS_SUCCESS,TRUE,FALSE},  // malformed AML reply
    {1,0x0105020c,STATUS_IO_TIMEOUT,FALSE,FALSE}
  };
  USB_CONTROLLER *D=&UsbTestController;
  for(ULONG I=0;I<ARRAYSIZE(Cases);++I) {
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    UsbTestFillHandoff();
    if(Cases[I].Path)UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[15]=Cases[I].Path;
    if(Cases[I].Path==USB_HANDOFF_PATH_ID_LOW_HUB_POWER)
      UsbTestExpectedCoreFlags=UsbTestHandoff[2]=USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
    UsbTestTypecSample=Cases[I].Sample;UsbTestTypecStatus=Cases[I].AcpiStatus;
    UsbTestTypecBadReply=Cases[I].BadReply;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(D->Online && D->WaitingForVbus!=Cases[I].Ready);
    CHECK(UsbTestTypecCalls==(Cases[I].Path!=USB_HANDOFF_PATH_ID_LOW_HUB_POWER));
    CHECK(D->Core.Initialized==Cases[I].Ready && UsbTestControllerFailed==0);
    CHECK(Cases[I].Ready || UsbTestMmioWrites==0);
    if(!Cases[I].Ready) {
      UCHAR Data[4]={0xff,0xff,0xff,0xff};
      UsbTestResumePortRequest(0,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
      UsbTestResumePortRequest(1,0x23,3,MUSB_PORT_FEATURE_POWER,NULL,0,STATUS_SUCCESS);
      CHECK(Data[0]==0 && Data[1]==0 && Data[2]==0 && Data[3]==0);
      CHECK(UsbTestTypecCalls==1 && UsbTestMmioWrites==0);
    }
  }
}

static VOID UsbTestTypecVbusRecovery(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  UCHAR Data[4]={0};
  ULONG Writes;
  UsbTestInitController();UsbTestFillResources();
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
  UsbTestFillHandoff();
  UsbTestHandoff[12]=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
  UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
  UsbTestRegisters[USB_REG_POWER]=0x21;
  UsbTestUseRealPort=TRUE;
  UsbTestTypecStatus=STATUS_SUCCESS;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(D->WaitingForVbus && UsbTestCoreInitCalls==0 && UsbTestMmioWrites==0);
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  UsbTestTypecStatus=STATUS_OBJECT_NAME_NOT_FOUND;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(D->WaitingForVbus && UsbTestTypecCalls==2 && UsbTestMmioWrites==0);
  UsbTestTypecStatus=STATUS_SUCCESS;
  UsbTestTypecSample=0x0105020c;
  UsbWork(TEST_WORKER);
  CHECK(UsbTestTypecCalls==2 && UsbTestMmioWrites==0);
  UsbTestExpectedAdoptStatus=MUSB_PORT_POWER;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(UsbTestTypecCalls==3 && D->Core.Initialized && !D->WaitingForVbus);
  CHECK(D->Core.PortStatus&MUSB_PORT_CONNECTION);
  CHECK(!(D->Core.PortStatus&MUSB_PORT_ENABLE) && UsbTestUcxRootChanged==1);

  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  UsbTestHandoff[0]=0;
  UsbTestTypecSample=0x0105000c; // Both CC pins open while source VBUS decays.
  Writes=UsbTestMmioWrites;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(D->WaitingForVbus && UsbTestTypecCalls==4 && UsbTestMmioWrites==Writes);
  UsbTestResumePortRequest(0,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
  CHECK(Data[0]==0 && Data[1]==0 && (Data[2]&MUSB_PORT_CHANGE_CONNECTION));
  UsbTestTypecSample=0;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(D->WaitingForVbus && UsbTestTypecCalls==5 && UsbTestMmioWrites==Writes);
  UsbTestTypecSample=0x0105080c;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(!D->WaitingForVbus && UsbTestTypecCalls==6 && UsbTestAckCalls==1);
  CHECK(UsbTestCoreInitCalls==1 && UsbTestCoreAdoptCalls==1 && UsbTestControllerFailed==0);
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
}

static void UsbTestVbusBulkRequest(ULONG Index,NTSTATUS Result) {
  URB Urb={0};
  UCHAR Data[4]={0};
  WDFREQUEST Request;
  TEST_REQUEST_SLOT *Slot;
  ULONG Calls=UsbTestDataCalls;
  Urb.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Urb.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Urb.UrbBulkOrInterruptTransfer.TransferBuffer=Data;
  Urb.UrbBulkOrInterruptTransfer.TransferBufferLength=sizeof(Data);
  UsbTestExpectedEndpointAddress=0x01;UsbTestExpectedTransferLength=sizeof(Data);UsbTestExpectedTimeout=1000;
  UsbTestCoreDone=sizeof(Data);
  Request=UsbTestNewRequest(Index,&Urb);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  UsbWork(TEST_WORKER);UsbTestRunDpc();
  Slot=UsbTestFindRequest(Request);
  CHECK(Slot->CompletionCount==1 && Slot->CompletionStatus==Result);
  if(!UsbTestUseRealCoreData) CHECK(UsbTestDataCalls==Calls+(Result==STATUS_SUCCESS));
}
static VOID UsbTestLiveTypecLoss(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  for(ULONG Case=0;Case<6;++Case) {
    ULONG Writes;
    UCHAR Data[4]={0};
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestUseRealPort=TRUE;
    UsbTestTypecStatus=STATUS_SUCCESS;
    UsbTestTypecSample=Case==5?0x0105080c:0x0105020c;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(D->TypecProbe && D->Online && UsbTestTypecCalls==1);
    if(Case==0) UsbTestResumePortRequest(0,0x23,3,MUSB_PORT_FEATURE_SUSPEND,NULL,0,STATUS_SUCCESS);
    Writes=UsbTestMmioWrites;

    // Changes on the unused CC pin leave the same sink attached.
    UsbTestTypecSample=Case==5?0x0105090c:0x0105060c;
    UsbTestClock+=100000;
    UsbWork(TEST_WORKER);
    CHECK(UsbTestTypecCalls==2 && !D->WaitingForVbus && UsbTestMmioWrites==Writes);

    // A confirmed role change, CC detach or different active CC pin ends
    // the previous connection even while DEVCTL still reports VBUS.
    UsbTestTypecSample=Case==4?0x0105080c:Case==5?0x0105020c:
        Case==2?0x0105000c:0x014a120c;
    if(Case==3) UsbTestClockIdentity=0;
    if(Case==5) {
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      UsbWork(TEST_WORKER);
    } else {
      UsbWork(TEST_WORKER);
      CHECK(UsbTestTypecCalls==2 && !D->WaitingForVbus);
      UsbTestClock+=100000;
      UsbWork(TEST_WORKER);
    }
    CHECK(UsbTestTypecCalls==3 && UsbTestMmioWrites==Writes);
    if(Case==3) {
      CHECK(UsbTestControllerFailed==1 && !D->Online && !D->WaitingForVbus);
      CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
      continue;
    }
    CHECK(D->Online && D->WaitingForVbus && D->Core.State==MUSB_HOST_STATE_HALTED);
    CHECK(D->Core.PortPowerLost && UsbTestUcxRootChanged==1);
    UsbTestResumePortRequest(1,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
    CHECK(Data[0]==0 && Data[1]==0 && (Data[2]&MUSB_PORT_CHANGE_CONNECTION));
    UsbTestResumePortRequest(2,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_DEVICE_NOT_READY);
    CHECK(UsbTestMmioWrites==Writes && UsbTestAckCalls==0);

    if(Case==2) {
      UsbTestPnpRelease();
      UsbWork(TEST_WORKER);
      CHECK(UsbTestTypecCalls==3 && UsbTestMmioWrites==Writes && !D->TypecProbe && !D->SeenD0);
      CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
      UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
      UsbTestHandoff[2]=UsbTestExpectedCoreFlags=USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
      UsbTestHandoff[15]=USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
      UsbTestClock+=100000;
      UsbWork(TEST_WORKER);
      CHECK(D->Online && !D->WaitingForVbus && UsbTestTypecCalls==3);
      CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
      continue;
    }
    if(Case==1) {
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD2)==STATUS_SUCCESS);
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD2)==STATUS_SUCCESS);
      CHECK(D->WaitingForVbus && UsbTestTypecCalls==4 && UsbTestMmioWrites==Writes);
    }
    UsbTestTypecStatus=STATUS_SUCCESS;
    UsbTestTypecSample=Case==5?0x0105020c:0x0105080c;
    UsbTestClock+=100000;
    UsbWork(TEST_WORKER);
    CHECK(D->Online && !D->WaitingForVbus && D->Core.State==MUSB_HOST_STATE_OPERATIONAL);
    CHECK(UsbTestControllerFailed==0 && UsbTestAckCalls==1 && UsbTestRegisters[USB_REG_POWER]==0x21);
    UsbTestResumePortRequest(3,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
    CHECK(Data[0]==0 && Data[1]==1 && (Data[2]&MUSB_PORT_CHANGE_CONNECTION));
    UsbTestResumePortRequest(4,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
    UsbTestResumePortRequest(5,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
    CHECK(Data[0]==MUSB_PORT_CONNECTION && Data[1]==1 && D->Core.PortPowerLost);
    UsbTestResumePortRequest(6,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
    CHECK((D->Core.PortStatus&MUSB_PORT_ENABLE) && !D->Core.PortPowerLost);
    if(Case>=4) {
      ULONG Changes=UsbTestUcxRootChanged;
      UsbTestClock+=100000;
      UsbWork(TEST_WORKER);
      CHECK(!D->WaitingForVbus && UsbTestUcxRootChanged==Changes);
    }
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  }
}

static void UsbTestBoardRouteLoss(void) {
  enum { PogoLost, PogoLostAsleep, PogoBadAtStart, TypecRouteLost };
  USB_CONTROLLER *D=&UsbTestController;
  for(ULONG Case=PogoLost;Case<=TypecRouteLost;++Case) {
    ULONG Writes;
    UCHAR Data[4]={0};
    BOOLEAN Typec=Case==TypecRouteLost;
    ULONG Path=Typec?USB_HANDOFF_PATH_TYPEC_ONLY:USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[2]=UsbTestExpectedCoreFlags=Typec?USB_HANDOFF_FLAGS_TYPEC_SOURCE:USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestHandoff[15]=Path;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestUseRealPort=TRUE;
    UsbTestRouteStatus=UsbTestTypecStatus=STATUS_SUCCESS;
    UsbTestTypecSample=0x0105020c;
    UsbTestRouteReply=Case!=PogoBadAtStart;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    if(Case==PogoBadAtStart) {
      CHECK(UsbTestRouteCalls==2 && UsbTestRouteArgument==USB_HANDOFF_PATH_TYPEC_SOURCE &&
            UsbTestTypecCalls==0);
      CHECK(D->WaitingForVbus && !D->Core.Initialized && UsbTestMmioWrites==0);
      UsbTestRouteReply=1;
      UsbTestExpectedAdoptStatus=MUSB_PORT_POWER;
      UsbTestClock+=100000;
      UsbWork(TEST_WORKER);
      CHECK(UsbTestRouteCalls==3 && D->Core.Initialized && !D->WaitingForVbus);
      CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
      continue;
    }
    CHECK(UsbTestRouteCalls==1 && UsbTestRouteArgument==Path && UsbTestTypecCalls==Typec);
    CHECK(D->Core.Initialized && !D->WaitingForVbus);
    Writes=UsbTestMmioWrites;
    UsbTestRouteReply=0;
    if(Case==PogoLostAsleep) {
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      UsbTestHandoff[0]=0;
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    } else {
      UsbTestClock+=100000;
    }
    UsbWork(TEST_WORKER);
    if(Case==PogoLost || Case==PogoLostAsleep) {
      CHECK(UsbTestRouteCalls==3 && UsbTestRouteArgument==USB_HANDOFF_PATH_TYPEC_SOURCE &&
            UsbTestTypecCalls==0);
    } else {
      CHECK(UsbTestRouteCalls==2 && UsbTestRouteArgument==Path && UsbTestTypecCalls==2U*Typec);
    }
    CHECK(D->Online && D->WaitingForVbus && UsbTestMmioWrites==Writes);
    CHECK(D->Core.PortPowerLost && UsbTestUcxRootChanged==1);
    UsbTestResumePortRequest(0,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
    CHECK(!Data[0] && !Data[1] && (Data[2]&MUSB_PORT_CHANGE_CONNECTION));
    UsbTestRouteReply=1;UsbTestRouteStatus=STATUS_SUCCESS;
    UsbTestClock+=100000;
    UsbWork(TEST_WORKER);
    CHECK(D->Online && !D->WaitingForVbus && UsbTestAckCalls==1);
    UsbTestResumePortRequest(1,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
    UsbTestResumePortRequest(2,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
    CHECK((D->Core.PortStatus&MUSB_PORT_ENABLE) && !D->Core.PortPowerLost);
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  }
}

static void UsbTestLostRetainedSession(void) {
  enum { Pogo, NewTypecCable, OldAml, NoRoute, NoTypec, WrongRole, PowerBusy,
         BadPhy, BusyDma, WriteDropped, AfterFifoRoleChanged, FifoMissing };
  USB_CONTROLLER *D=&UsbTestController;
  for(ULONG Case=Pogo;Case<=FifoMissing;++Case) {
    BOOLEAN Typec=Case==NewTypecCable || Case==NoTypec;
    BOOLEAN ColdRecovery=Case==BadPhy;
    BOOLEAN Restored=Case<=NewTypecCable || Case==FifoMissing || ColdRecovery;
    ULONG Starts;
    UCHAR Port[4]={0};
    UCHAR Devctl=0x5c;
    NTSTATUS ResumeStatus;
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_REG_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[2]=UsbTestExpectedCoreFlags=Typec?USB_HANDOFF_FLAGS_TYPEC_SOURCE:USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestHandoff[15]=Typec?USB_HANDOFF_PATH_TYPEC_ONLY:USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestUseRealPort=TRUE;
    if(Case!=OldAml) UsbTestRouteStatus=STATUS_SUCCESS;
    UsbTestRouteReply=1;
    UsbTestTypecStatus=STATUS_SUCCESS;
    UsbTestTypecSample=0x0105020c;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    UsbTestHandoff[0]=0;
    switch(Case) {
      case NewTypecCable: UsbTestTypecSample=0x0105080c;break;
      case NoRoute: UsbTestRouteReply=0;break;
      case NoTypec: UsbTestTypecStatus=STATUS_IO_TIMEOUT;break;
      case WrongRole: Devctl=0xd8;break;
      case PowerBusy: UsbTestRegisters[USB_REG_POWER]|=USB_POWER_TRANSIENT_MASK;break;
      case BadPhy: UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]^=0x200;break;
      case BusyDma: UsbTestIdleResult=MtkMusbPlatformQuiescenceFailed;break;
      case WriteDropped: UsbTestDropSessionWrite=TRUE;break;
      case AfterFifoRoleChanged: UsbTestDevctlAfterFifo=0xdd;break;
      case FifoMissing:
        UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
        UsbTestFifoRestoreResult=MtkMusbPlatformSuccess;
        break;
    }
    UsbTestRegisters[USB_DEVCTL_REGISTER]=Devctl;
    Starts=UsbTestQueueStarts;
    ResumeStatus=UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3);
    if(ColdRecovery&&ResumeStatus==STATUS_SUCCESS)
      UsbTestStartPortInterrupt(WdfPowerDeviceD3);
    if(ColdRecovery) {
      CHECK(ResumeStatus==STATUS_SUCCESS);
    } else {
      CHECK(ResumeStatus==(Restored?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY));
    }
    if(ColdRecovery) {
      CHECK(D->Online&&D->WaitingForVbus&&D->ColdStartPending);
      for(ULONG Poll=0U;Poll<600U&&D->ColdStartPending;++Poll) {
        UsbTestClock+=MTK_MUSB_PLATFORM_COLD_POLL_US;
        UsbTimer(TEST_TIMER);
        UsbWork(TEST_WORKER);
      }
      CHECK(!D->ColdStartPending);
      CHECK(D->ColdStartOwned);
      CHECK(D->Core.State==MUSB_HOST_STATE_OPERATIONAL);
      CHECK(!D->WaitingForVbus);
      CHECK(D->InterruptReady&&D->InterruptOwned&&D->InterruptUnmasked);
      CHECK(!UsbTestControllerFailed);
      CHECK(D->ColdPhyBound&&D->ColdPhy.State==MTK_USB_TPHY_STATE_POWERED);
      CHECK(D->ColdPhyMode==MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST&&
          D->ColdPhySession.Owned==2U&&
          (UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]&0x00000200U));
      CHECK(D->Core.PortPowerLost&&
          (D->Core.PortChangeStatus&MUSB_PORT_CHANGE_CONNECTION));
      UsbTestStopPortInterrupt(WdfPowerDeviceD3);
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
      continue;
    }
    if(Restored) {
      CHECK(UsbTestDevctlWrites==1 && UsbTestRegisters[USB_DEVCTL_REGISTER]==(Devctl|0x01U));
      CHECK(UsbTestAckCalls==2 && UsbTestFifoRestores==(Case==FifoMissing) && UsbTestControllerFailed==0);
      CHECK(D->Online && D->Core.PortPowerLost && !D->WaitingForVbus);
      CHECK(Case!=NewTypecCable || D->TypecCc==2);
      UsbTestResumePortRequest(0,0xa3,0,0,Port,sizeof(Port),STATUS_SUCCESS);
      CHECK(Port[0]==0 && Port[1]==1 && (Port[2]&MUSB_PORT_CHANGE_CONNECTION));
      UsbTestResumePortRequest(1,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
      UsbTestResumePortRequest(2,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
      CHECK((D->Core.PortStatus&MUSB_PORT_ENABLE) && !D->Core.PortPowerLost);
    } else {
      CHECK(UsbTestDevctlWrites==(Case==AfterFifoRoleChanged?2U:Case==WriteDropped?1U:0U));
      CHECK(UsbTestRegisters[USB_DEVCTL_REGISTER]==(Case==AfterFifoRoleChanged?0xdc:Devctl));
      CHECK(!D->Online && UsbTestQueueStarts==Starts && UsbTestControllerFailed==1);
    }
    CHECK(!D->CoreLock);
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  }
}

static void UsbTestLostSessionWithDelayedVbus(void) {
  enum { Pogo, Typec, Suspended, SleepAgain, VbusBounced, FifoMissing,
         ClockChanged, RoleChanged, WriteDropped };
  for(ULONG Case=Pogo;Case<=WriteDropped;++Case) {
    USB_CONTROLLER *D=&UsbTestController;
    const BOOLEAN IsTypec=Case==Typec;
    UCHAR Port[4]={0};
    ULONG Writes,Acks,Changes;
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_REG_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[2]=UsbTestExpectedCoreFlags=IsTypec?USB_HANDOFF_FLAGS_TYPEC_SOURCE:USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestHandoff[15]=IsTypec?USB_HANDOFF_PATH_TYPEC_ONLY:USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestUseRealPort=TRUE;
    UsbTestRouteStatus=STATUS_SUCCESS;
    UsbTestRouteReply=1;
    UsbTestTypecStatus=STATUS_SUCCESS;
    UsbTestTypecSample=0x0105020c;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    if(Case==Suspended)
      UsbTestResumePortRequest(0,0x23,3,MUSB_PORT_FEATURE_SUSPEND,NULL,0,STATUS_SUCCESS);
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    UsbTestHandoff[0]=0;
    // The controller retains A-host mode, but SESSION and VBUS have both gone.
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x44;
    UsbTestRouteReply=0;
    Writes=UsbTestMmioWrites;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    CHECK(D->Online && D->WaitingForVbus && D->Core.PortPowerLost &&
          D->Core.State==MUSB_HOST_STATE_HALTED && !UsbTestControllerFailed);
    if(Case==SleepAgain) {
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS && D->WaitingForVbus);
    }
    UsbTestClock+=USB_TYPEC_POLL_US;
    UsbWork(TEST_WORKER);
    CHECK(D->WaitingForVbus && UsbTestMmioWrites==Writes && !UsbTestDevctlWrites);
    UsbTestResumePortRequest(1,0xa3,0,0,Port,sizeof(Port),STATUS_SUCCESS);
    CHECK(!Port[0] && !Port[1] && (Port[2]&MUSB_PORT_CHANGE_CONNECTION));
    UsbTestResumePortRequest(2,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
    UsbTestResumePortRequest(3,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_DEVICE_NOT_READY);
    Changes=UsbTestUcxRootChanged;
    Acks=UsbTestAckCalls;

    // Sampling VBUS alone cannot arm SESSION while the board route is off.
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5c;
    UsbWork(TEST_WORKER);
    CHECK(D->WaitingForVbus && !UsbTestDevctlWrites && UsbTestMmioWrites==Writes);
    UsbTestRouteReply=1;
    if(IsTypec) UsbTestTypecSample=0x0105080c;
    if(Case==ClockChanged) UsbTestClockIdentity^=0x100;
    if(Case==RoleChanged) UsbTestRegisters[USB_DEVCTL_REGISTER]=0xd8;
    if(Case==WriteDropped) UsbTestDropSessionWrite=TRUE;
    if(Case==FifoMissing) {
      UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
      UsbTestFifoRestoreResult=MtkMusbPlatformSuccess;
    }
    if(Case==VbusBounced) {
      UsbTestDropVbusOnAck=TRUE;
      UsbTestClock+=USB_TYPEC_POLL_US;
      UsbWork(TEST_WORKER);
      CHECK(D->Online && D->WaitingForVbus && !D->CoreLock && !UsbTestControllerFailed);
      CHECK(UsbTestRegisters[USB_DEVCTL_REGISTER]==0x44 && !UsbTestDevctlWrites);
      CHECK(UsbTestAckCalls==++Acks);
      UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5c;
    }
    UsbTestClock+=USB_TYPEC_POLL_US;
    UsbWork(TEST_WORKER);
    CHECK(!D->WaitingForVbus && !D->CoreLock);
    if(Case<ClockChanged) {
      CHECK(D->Online && !UsbTestControllerFailed && D->Core.State==MUSB_HOST_STATE_OPERATIONAL);
      CHECK(UsbTestDevctlWrites==1 && UsbTestRegisters[USB_DEVCTL_REGISTER]==0x5d &&
            UsbTestAckCalls==Acks+2 && UsbTestRegisters[USB_REG_POWER]==0x21);
      CHECK(UsbTestFifoRestores==(Case==FifoMissing?1U:0U));
      CHECK(D->TypecCc==(IsTypec?2:0));
      UsbTestResumePortRequest(4,0xa3,0,0,Port,sizeof(Port),STATUS_SUCCESS);
      CHECK(Port[0]==1 && Port[1]==1 && (Port[2]&MUSB_PORT_CHANGE_CONNECTION));
      CHECK(D->Core.PortPowerLost && UsbTestUcxRootChanged==Changes+1);
      UsbTestResumePortRequest(5,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
      UsbTestResumePortRequest(6,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
      CHECK((D->Core.PortStatus&MUSB_PORT_ENABLE) && !D->Core.PortPowerLost);
    } else {
      CHECK(!D->Online && UsbTestControllerFailed==1 && D->Core.State==MUSB_HOST_STATE_HALTED);
      CHECK(UsbTestDevctlWrites==(Case==WriteDropped?1U:0U));
      CHECK(UsbTestAckCalls==Acks+(Case==WriteDropped?1U:0U));
    }
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  }
}

static void UsbTestAddTcpcResource(void) {
  CM_PARTIAL_RESOURCE_DESCRIPTOR Connection={0};
  Connection.Type=CmResourceTypeConnection;
  Connection.u.Connection.Class=CM_RESOURCE_CONNECTION_CLASS_SERIAL;
  Connection.u.Connection.Type=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C;
  Connection.u.Connection.IdHighPart=0x01234567;
  Connection.u.Connection.IdLowPart=0x89abcdef;
  UsbTestRaw[UsbTestRawCount++]=Connection;
  UsbTestTranslated[UsbTestTranslatedCount++]=Connection;
}

static void UsbTestNativeTcpcAdmission(void) {
  enum { Typec,Pogo,OpenFailed,LockTimedOut,BadId,ShortRead,ReadFailed,
         UnlockTimedOut,CcChanged,Safe0V,NoVbus };
  USB_CONTROLLER *D=&UsbTestController;
  for(ULONG Case=Typec;Case<=NoVbus;Case++) {
    UsbTestInitController();UsbTestFillResources();UsbTestAddTcpcResource();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    CHECK(!UsbTestTcpc.Create && !UsbTestTcpc.Reads);
    UsbTestFillHandoff();
    UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[2]=UsbTestExpectedCoreFlags=Case==Pogo?
      USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER:USB_HANDOFF_FLAGS_TYPEC_SOURCE;
    UsbTestHandoff[15]=Case==Pogo?USB_HANDOFF_PATH_ID_LOW_HUB_POWER:USB_HANDOFF_PATH_TYPEC_ONLY;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRouteStatus=STATUS_SUCCESS;
    UsbTestRouteReply=1;
    switch(Case) {
      case OpenFailed: UsbTestTcpc.OpenStatus=STATUS_DEVICE_NOT_READY;break;
      case LockTimedOut: UsbTestTcpc.LockStatus=STATUS_IO_TIMEOUT;break;
      case BadId: UsbTestTcpc.WrongIdentity=TRUE;break;
      case ShortRead: UsbTestTcpc.ShortAt=3;break;
      case ReadFailed: UsbTestTcpc.FailAt=4;UsbTestTcpc.ReadStatus=STATUS_IO_DEVICE_ERROR;break;
      case UnlockTimedOut: UsbTestTcpc.UnlockStatus=STATUS_IO_TIMEOUT;break;
      case CcChanged: UsbTestTcpc.SecondState=0x05080c;break;
      case Safe0V: UsbTestTcpc.Safe0V=TRUE;break;
      case NoVbus: UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=0x050208;break;
    }
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(D->Online && D->WaitingForVbus==(Case>=OpenFailed) && !UsbTestTypecCalls);
    CHECK(D->Core.Initialized==(Case<OpenFailed) && D->TypecCc==(Case==Typec));
    CHECK(UsbTestTcpc.Create==(Case!=Pogo) && UsbTestTcpc.Open==UsbTestTcpc.Create);
    CHECK(UsbTestTcpc.Locks==(Case==Typec || Case>=LockTimedOut));
    CHECK(UsbTestTcpc.Unlocks==(Case==Typec || Case>=BadId));
    CHECK(UsbTestTcpc.Reads==(Case==BadId?1U:Case==ShortRead?3U:Case==ReadFailed?4U:
          Case==Typec || Case>=UnlockTimedOut?5U:0U));
    CHECK(UsbTestTcpc.Delete==(Case>=OpenFailed && Case<=UnlockTimedOut));
    CHECK(UsbTestTcpc.Close==(Case>=LockTimedOut && Case<=UnlockTimedOut));
    if(Case>=OpenFailed) {
      CHECK(!UsbTestMmioWrites);
      UsbTestTcpc.OpenStatus=UsbTestTcpc.LockStatus=UsbTestTcpc.UnlockStatus=STATUS_SUCCESS;
      UsbTestTcpc.FailAt=UsbTestTcpc.ShortAt=0;
      UsbTestTcpc.WrongIdentity=UsbTestTcpc.Safe0V=FALSE;
      UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=0x05080c;
      UsbWork(TEST_WORKER);
      CHECK(!D->Core.Initialized);
      UsbTestClock+=100000;
      UsbWork(TEST_WORKER);
      CHECK(D->Online && D->Core.Initialized && !D->WaitingForVbus &&
            D->TypecCc==2 && !UsbTestTcpc.Locked && UsbTestTcpc.Opened && !UsbTestTypecCalls);
    }
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    CHECK(!UsbTestTcpc.Opened && !UsbTestTcpc.Locked && UsbTestTcpc.Create==UsbTestTcpc.Delete);
    ULONG Reads=UsbTestTcpc.Reads;
    UsbTestClock+=100000;
    UsbWork(TEST_WORKER);
    CHECK(UsbTestTcpc.Reads==Reads);
  }
  for(ULONG Case=0;Case<4;Case++) {
    UsbTestInitController();UsbTestFillResources();UsbTestAddTcpcResource();
    if(Case==0)UsbTestRaw[3].u.Connection.IdLowPart++;
    if(Case==1)UsbTestTranslated[3].u.Connection.Type++;
    if(Case==2)UsbTestRaw[3].u.Connection.IdHighPart=UsbTestRaw[3].u.Connection.IdLowPart=
        UsbTestTranslated[3].u.Connection.IdHighPart=UsbTestTranslated[3].u.Connection.IdLowPart=0;
    if(Case==3)UsbTestAddTcpcResource();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!UsbTestMapCalls && !UsbTestTcpc.Create);
  }
}

static void UsbTestFirstD0VbusSettles(void) {
  enum { Typec, Pogo, AfterFifo, ReleaseWaiting, LostClock, LostHandoff, LostHost,
         LegacyHandoff, MissingClock, MissingRoute, MissingTcpc, WrongRole, PhyFault, PhyBusy, PortBusy };
  USB_CONTROLLER *D=&UsbTestController;

  for(ULONG Case=Typec;Case<=PortBusy;++Case) {
    BOOLEAN TypecPath=Case!=Pogo && Case!=LegacyHandoff;
    BOOLEAN Wait=Case<=LostHost;
    ULONG Reads,TypecReads;
    UCHAR Port[4]={0};
    UsbTestInitController();
    UsbTestFillResources();
    if(TypecPath && Case!=MissingTcpc) UsbTestAddTcpcResource();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_REG_BYTES;
    UsbTestPhy[USB_PHY_ACR5/4]=0;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    if(Case!=LegacyHandoff) {
      UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
      UsbTestHandoff[2]=UsbTestExpectedCoreFlags=TypecPath?
          USB_HANDOFF_FLAGS_TYPEC_SOURCE:USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
      UsbTestHandoff[15]=TypecPath?USB_HANDOFF_PATH_TYPEC_ONLY:USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
    }
    UsbTestHandoff[12]=MUSB_PORT_POWER|MUSB_PORT_CONNECTION|MUSB_PORT_ENABLE;
    UsbTestHandoff[13]=MUSB_PORT_CHANGE_CONNECTION;
    UsbTestRouteStatus=STATUS_SUCCESS;
    UsbTestRouteReply=1;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x0d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestUseRealPort=TRUE;
    switch(Case) {
      case AfterFifo:
        UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
        UsbTestDevctlAfterFifo=0x15;
        break;
      case MissingClock: UsbTestClockStatus=STATUS_OBJECT_NAME_NOT_FOUND;break;
      case MissingRoute: UsbTestRouteStatus=STATUS_OBJECT_NAME_NOT_FOUND;break;
      case WrongRole: UsbTestRegisters[USB_DEVCTL_REGISTER]=0x8d;break;
      case PhyFault: UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]^=0x200;break;
      case PhyBusy: UsbTestPhy[USB_PHY_ACR5/4]=USB_PHY_CALIBRATING;break;
      case PortBusy: UsbTestRegisters[USB_REG_POWER]|=USB_POWER_TRANSIENT_MASK;break;
    }
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==
          (Wait?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY));
    CHECK(D->WaitingForVbus==Wait && D->Online==Wait && !D->Core.Initialized);
    CHECK(!UsbTestCoreInitCalls && !UsbTestMmioWrites && UsbTestFifoCalls==(Case==AfterFifo));
    CHECK(UsbTestControllerFailed==!Wait);
    if(!Wait) {
      UsbTestPnpRelease();
      continue;
    }
    UsbTestStartPortInterrupt(WdfPowerDeviceD3Final);
    Reads=UsbTestMmioReads;
    TypecReads=UsbTestTcpc.Reads;
    UsbWork(TEST_WORKER);
    CHECK(UsbTestMmioReads==Reads && UsbTestTcpc.Reads==TypecReads && !D->InterruptUnmasked);
    UsbTestResumePortRequest(0,0xa3,0,0,Port,sizeof(Port),STATUS_SUCCESS);
    CHECK(!Port[0] && !Port[1] && !Port[2] && !Port[3]);
    if(Case==Typec) {
      const UCHAR First=MUSB_PORT_FEATURE_CHANGE_CONNECTION;
      for(UCHAR Feature=First;Feature<=MUSB_PORT_FEATURE_CHANGE_RESET;Feature++)
        UsbTestResumePortRequest(1+Feature-First,0x23,1,Feature,NULL,0,STATUS_SUCCESS);
      UsbTestResumePortRequest(6,0x23,1,MUSB_PORT_FEATURE_ENABLE,NULL,0,STATUS_SUCCESS);
      UsbTestResumePortRequest(7,0x23,1,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
      UsbTestResumePortRequest(8,0x23,1,MUSB_PORT_FEATURE_SUSPEND,NULL,0,STATUS_DEVICE_NOT_READY);
      UsbTestResumePortRequest(9,0x23,1,MUSB_PORT_FEATURE_POWER,NULL,0,STATUS_NOT_SUPPORTED);
      CHECK(!UsbTestPortFeatureCalls && UsbTestMmioReads==Reads && !UsbTestMmioWrites);
      CHECK(!D->Core.PortChangeStatus && !D->LastPortChange && !UsbTestUcxRootChanged);
    }

    if(Case==ReleaseWaiting) {
      UsbTestStopPortInterrupt(WdfPowerDeviceD3Final);
      UsbTestPnpRelease();
      Reads=UsbTestMmioReads;
      UsbTestClock+=USB_TYPEC_POLL_US;
      UsbWork(TEST_WORKER);
      CHECK(UsbTestMmioReads==Reads && UsbTestTcpc.Reads==TypecReads && !UsbTestMmioWrites);
      continue;
    }
    if(Case==Pogo) {
      UsbTestResumePortRequest(1,0x23,3,MUSB_PORT_FEATURE_POWER,NULL,0,STATUS_SUCCESS);
      UsbTestResumePortRequest(2,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_DEVICE_NOT_READY);
      CHECK(!UsbTestPortFeatureCalls && UsbTestMmioReads==Reads && !UsbTestMmioWrites);
      UsbTestStopPortInterrupt(WdfPowerDeviceD3);
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS && D->WaitingForVbus);
      UsbTestStartPortInterrupt(WdfPowerDeviceD3);
      CHECK(!UsbTestTcpc.Open);
    }
    UsbTestClock+=USB_TYPEC_POLL_US;
    if(Case>=LostClock) {
      if(Case==LostClock) UsbTestClockIdentity=0;
      if(Case==LostHandoff) UsbTestHandoff[0]=0;
      if(Case==LostHost) UsbTestRegisters[USB_DEVCTL_REGISTER]=0x0c;
      UsbWork(TEST_WORKER);
      CHECK(!D->Online && !D->WaitingForVbus && !D->InterruptUnmasked);
      CHECK(UsbTestControllerFailed==1 && !UsbTestMmioWrites && !UsbTestCoreInitCalls);
    } else {
      UsbWork(TEST_WORKER);
      CHECK(D->WaitingForVbus && !UsbTestMmioWrites && !D->InterruptUnmasked);
      UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
      UsbTestClock+=USB_TYPEC_POLL_US;
      UsbWork(TEST_WORKER);
      CHECK(D->Online && !D->WaitingForVbus && D->Core.Initialized && D->InterruptUnmasked);
      CHECK(UsbTestCoreInitCalls==1 && UsbTestCoreAdoptCalls==1 && !UsbTestControllerFailed);
      CHECK(D->Core.PortStatus==(MUSB_PORT_POWER|MUSB_PORT_CONNECTION));
      CHECK(D->Core.PortChangeStatus==MUSB_PORT_CHANGE_CONNECTION && UsbTestUcxRootChanged==1);
    }
    UsbTestStopPortInterrupt(WdfPowerDeviceD3Final);
    UsbTestPnpRelease();
  }
}

static void UsbTestNativeTcpcDetachAndResume(void) {
  USB_CONTROLLER *D=&UsbTestController;
  UsbTestInitController();UsbTestFillResources();UsbTestAddTcpcResource();
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
  UsbTestFillHandoff();
  UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
  UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
  UsbTestRegisters[USB_REG_POWER]=0x21;
  UsbTestUseRealPort=TRUE;
  UsbTestRouteStatus=STATUS_SUCCESS;
  UsbTestRouteReply=1;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(D->TypecCc==1 && UsbTestTcpc.Open==1 && UsbTestTcpc.Reads==5);
  UsbTestStartPortInterrupt(WdfPowerDeviceD3Final);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestRegisters[0x0b]==0x32);
  UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=0x050208;
  UsbTestTcpc.Safe0V=TRUE;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(D->Online && D->WaitingForVbus && D->Core.State==MUSB_HOST_STATE_HALTED);
  CHECK(D->Core.PortPowerLost && UsbTestUcxRootChanged==1 && UsbTestTcpc.Reads==10);
  CHECK(!UsbTestRegisters[0x0b] && !D->InterruptOwned && !UsbTestRunPortIsr());
  UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=0x05080c;
  UsbTestTcpc.Safe0V=FALSE;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(!D->WaitingForVbus && D->TypecCc==2 && UsbTestTcpc.Reads==15);
  CHECK(UsbTestRegisters[0x0b]==0x32);
  UsbTestResumePortRequest(0,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
  UsbTestResumePortRequest(1,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
  CHECK((D->Core.PortStatus&MUSB_PORT_ENABLE) && !D->Core.PortPowerLost);
  ULONG Flushes=UsbTestWorkFlushes;
  UsbTestStopPortInterrupt(WdfPowerDeviceD3);
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbTestWorkFlushes==Flushes+1 && !UsbTestTcpc.Opened &&
        UsbTestTcpc.Close==1 && UsbTestTcpc.Delete==1);
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(D->Online && !D->WaitingForVbus && UsbTestTcpc.Open==2 &&
        UsbTestTcpc.Opened && UsbTestTcpc.Reads==20);
  CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  CHECK(UsbTestTcpc.Close==2 && UsbTestTcpc.Delete==2 && !UsbTestTypecCalls);
}


static VOID UsbTestAttachedRouteReadFailure(VOID) {
  for(ULONG Native=0;Native<2;++Native) {
    USB_CONTROLLER *D=&UsbTestController;
    USB_ENDPOINT_DESCRIPTOR Desc={0};
    UsbTestInitController();UsbTestFillResources();
    if(Native) UsbTestAddTcpcResource();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[15]=USB_HANDOFF_PATH_TYPEC_ONLY;
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestUseRealPort=UsbTestUseRealCoreData=TRUE;
    UsbTestRouteStatus=UsbTestTypecStatus=STATUS_SUCCESS;
    UsbTestRouteReply=1;
    UsbTestTypecSample=0x0105020c;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    UsbTestResumePortRequest(0,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
    UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
    Desc.bLength=sizeof(Desc);Desc.bDescriptorType=USB_ENDPOINT_DESCRIPTOR_TYPE;
    Desc.bEndpointAddress=0x01;Desc.bmAttributes=USB_ENDPOINT_TYPE_BULK;Desc.wMaxPacketSize=64;
    CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Desc,sizeof(Desc),NULL,
          (PUCXENDPOINT_INIT)(ULONG_PTR)0x900)==STATUS_SUCCESS);
    UsbTestDevice.Address=9;
    UsbTestDevice.Path.PortPathDepth=1;
    UsbTestDevice.Path.TTHubDepth=0;
    UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
    UsbTestVbusBulkRequest(1,STATUS_SUCCESS);
    ULONG Changes=UsbTestUcxRootChanged;
    CHECK(UsbTestRealDataCalls==1 && UsbTestRealCoreBytes==4);

    // The device keeps transferring while the control bus is busy or a
    // status sample is inconclusive.
    for(ULONG Poll=0;Poll<4;++Poll) {
      UsbTestTcpc.LockStatus=Poll<2?STATUS_IO_TIMEOUT:STATUS_SUCCESS;
      UsbTestTcpc.SecondState=Poll==2?0x05000c:0x05020c;
      UsbTestTypecStatus=Poll<2?STATUS_IO_TIMEOUT:STATUS_SUCCESS;
      UsbTestTypecSample=Poll==2?0:0x0105020c;
      UsbTestRouteReply=Poll==3?2:1;
      UsbTestClock+=100000;
      UsbTestVbusBulkRequest(2+Poll,STATUS_SUCCESS);
      CHECK(D->TypecCc==1 && !D->WaitingForVbus && !D->Core.PortPowerLost &&
            (D->Core.PortStatus&MUSB_PORT_ENABLE) && UsbTestUcxRootChanged==Changes);
    }

    // Resume the same powered session with its route temporarily unreadable.
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    UsbTestTcpc.OpenStatus=STATUS_DEVICE_NOT_READY;
    UsbTestTypecStatus=STATUS_OBJECT_NAME_NOT_FOUND;
    UsbTestRouteReply=1;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    UsbTestVbusBulkRequest(6,STATUS_SUCCESS);
    CHECK(D->TypecCc==1 && !D->WaitingForVbus && !D->Core.PortPowerLost &&
          UsbTestDevice.Address==9 && UsbTestUcxRootChanged==Changes);
    CHECK(UsbTestRealDataCalls==6 && UsbTestRealCoreBytes==4);

    UsbTestClock+=100000;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=Native?0x45:0x1d;
    if(!Native) UsbTestRegisters[0x0a]=0x20; // A physical disconnect with VBUS still valid.
    UsbTestVbusBulkRequest(7,STATUS_DEVICE_NOT_CONNECTED);
    CHECK(!(D->Core.PortStatus&MUSB_PORT_CONNECTION) && UsbTestUcxRootChanged==Changes+1);
    CHECK(D->WaitingForVbus==Native && !UsbTestControllerFailed);
    CHECK(UsbTestRealDataCalls==(Native?6U:7U));
    if(!Native) CHECK(UsbTestRealCoreStatus==MusbDisconnected);
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  }
}

static void UsbTestRetainedVbusLoss(void) {
  for(ULONG Case=0;Case<7;++Case) {
    USB_CONTROLLER *D=&UsbTestController;
    UCHAR Data[4]={0};
    ULONG Frame=0x11223344, Writes, Changes;
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestUseRealPort=TRUE;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    if(Case==1 || Case==6) {
      UsbTestResumePortRequest(0,0x23,3,MUSB_PORT_FEATURE_SUSPEND,NULL,0,STATUS_SUCCESS);
      UsbTestRegisters[USB_DEVCTL_REGISTER]=0x45;
    }
    if(Case==6) {
      USHORT Status,Change;
      // Poll during D0 before power returns, when SUSPENDM is still asserted.
      CHECK(MusbHostGetPortStatus(&D->Core,0,&Status,&Change)==MusbOk);
      CHECK(!Status && (Change&MUSB_PORT_CHANGE_CONNECTION) && D->Core.PortPowerLost);
      CHECK(UsbTestRegisters[USB_REG_POWER]==0x23);
      UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    }
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    if(Case==6) {
      CHECK(D->WaitingForVbus && D->ResumeClock);
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      CHECK(D->Online && !D->WaitingForVbus && D->Core.PortPowerLost);
      CHECK(UsbTestRegisters[USB_REG_POWER]==0x21);
      UsbTestResumePortRequest(1,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
      CHECK(Data[0]==0 && Data[1]==1 && (Data[2]&MUSB_PORT_CHANGE_CONNECTION));
      UsbTestResumePortRequest(2,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
      UsbTestResumePortRequest(3,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
      CHECK((D->Core.PortStatus&MUSB_PORT_ENABLE) && !D->Core.PortPowerLost);
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      continue;
    }
    UsbTestHandoff[0]=0;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x45;
    Writes=UsbTestMmioWrites;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    CHECK(D->WaitingForVbus && D->Online && D->Core.State==MUSB_HOST_STATE_HALTED);
    CHECK(UsbTestControllerFailed==0 && UsbTestAckCalls==0 && UsbTestMmioWrites==Writes);
    CHECK(UsbFrame(TEST_UCX,&Frame)==STATUS_DEVICE_NOT_READY && Frame==0x11223344);
    UsbTestResumePortRequest(1,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
    CHECK(Data[0]==0 && Data[1]==0 &&
          Data[2]==(MUSB_PORT_CHANGE_CONNECTION|MUSB_PORT_CHANGE_ENABLE) && Data[3]==0);
    CHECK(UsbTestUcxRootChanged==1);
    UsbTestResumePortRequest(2,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_DEVICE_NOT_READY);
    CHECK(UsbTestMmioWrites==Writes && UsbTestPortFeatureCalls==(Case==1?1:0));
    if(Case==0) {
      USB_ENDPOINT_DESCRIPTOR Desc={0};
      ULONG Reads=UsbTestMmioReads, PollReads;
      UsbWork(TEST_WORKER);UsbTestRunDpc();
      PollReads=UsbTestMmioReads-Reads;
      Reads=UsbTestMmioReads;
      UsbTestResumePortRequest(8,0x23,1,MUSB_PORT_FEATURE_CHANGE_ENABLE,NULL,0,STATUS_SUCCESS);
      CHECK(D->Core.PortChangeStatus==MUSB_PORT_CHANGE_CONNECTION &&
            D->LastPortChange==MUSB_PORT_CHANGE_CONNECTION && UsbTestUcxRootChanged==1);
      CHECK(!UsbTestPortFeatureCalls && UsbTestMmioReads==Reads+PollReads && UsbTestMmioWrites==Writes);
      UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
      Desc.bLength=sizeof(Desc);Desc.bDescriptorType=USB_ENDPOINT_DESCRIPTOR_TYPE;
      Desc.bEndpointAddress=0x01;Desc.bmAttributes=USB_ENDPOINT_TYPE_BULK;Desc.wMaxPacketSize=64;
      CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Desc,sizeof(Desc),NULL,
            (PUCXENDPOINT_INIT)(ULONG_PTR)0x900)==STATUS_SUCCESS);
      UsbTestDevice.Address=9;
      UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
      UsbTestVbusBulkRequest(7,STATUS_DEVICE_NOT_CONNECTED);
      CHECK(UsbTestMmioWrites==Writes);
    }

    if(Case==2) {
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD2)==STATUS_SUCCESS);
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD2)==STATUS_SUCCESS);
      CHECK(D->WaitingForVbus && UsbTestMmioWrites==Writes);
      UsbWork(TEST_WORKER);
      CHECK(UsbTestUcxRootChanged==2);
    }
    if(Case==3) {
      CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
      UsbWork(TEST_WORKER);
      CHECK(!D->WaitingForVbus && !D->Online && !D->Prepared && !D->Core.Initialized);
      CHECK(UsbTestMmioWrites==Writes && UsbTestControllerFailed==0);
      continue;
    }
    if(Case>=4) {
      if(Case==4) UsbTestRegisters[USB_DEVCTL_REGISTER]=0x99;
      else {
        UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
        ++UsbTestClockIdentity;
      }
      UsbWork(TEST_WORKER);
      CHECK(UsbTestControllerFailed==1 && !D->Online && !D->WaitingForVbus);
      CHECK(UsbTestMmioWrites==Writes && UsbTestAckCalls==0);
      CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
      continue;
    }

    Changes=UsbTestUcxRootChanged;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbWork(TEST_WORKER);
    CHECK(D->Online && !D->WaitingForVbus && D->Core.State==MUSB_HOST_STATE_OPERATIONAL);
    CHECK(UsbTestAckCalls==1 && UsbTestUcxRootChanged==Changes && UsbTestControllerFailed==0);
    CHECK(UsbTestRegisters[USB_REG_POWER]==0x21 && UsbTestMmioWrites==Writes+(Case==1));
    UsbTestResumePortRequest(3,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
    CHECK(Data[0]==0 && Data[1]==1 && (Data[2]&MUSB_PORT_CHANGE_CONNECTION));
    UsbTestResumePortRequest(4,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
    CHECK(UsbTestUcxRootChanged==Changes+1);
    UsbTestResumePortRequest(5,0xa3,0,0,Data,sizeof(Data),STATUS_SUCCESS);
    CHECK(Data[0]==MUSB_PORT_CONNECTION && Data[1]==1 && D->Core.PortPowerLost);
    if(Case==0) UsbTestVbusBulkRequest(8,STATUS_DEVICE_NOT_CONNECTED);
    UsbTestResumePortRequest(6,0x23,3,MUSB_PORT_FEATURE_RESET,NULL,0,STATUS_SUCCESS);
    CHECK((D->Core.PortStatus&MUSB_PORT_ENABLE) && !D->Core.PortPowerLost);
    if(Case==0) UsbTestVbusBulkRequest(9,STATUS_SUCCESS);
    CHECK(UsbTestRegisters[USB_DEVCTL_REGISTER]==0x5d);
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  }
}

static VOID UsbTestPortInterruptLifecycle(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  PULONG L1Status=(PULONG)(UsbTestRegisters+0xa0);
  PULONG L1Mask=(PULONG)(UsbTestRegisters+0xa4);
  ULONG Enqueues,Reads;

  UsbTestInitController();UsbTestFillResources();
  UsbTestInterrupt.CreateStatus=STATUS_INSUFFICIENT_RESOURCES;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_INSUFFICIENT_RESOURCES);
  CHECK(UsbTestInterrupt.Create==1 && !UsbTestInterrupt.Delete && !D->Interrupt &&
        !D->Prepared && !D->Registers && !D->Handoff && UsbTestUnmapCalls==2);

  UsbTestInitController();UsbTestFillResources();
  UsbTestTranslated[2].Flags=CM_RESOURCE_INTERRUPT_LATCHED;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR);
  CHECK(!UsbTestInterrupt.Create && !UsbTestMapCalls);
  UsbTestTranslated[2].Flags=CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
  UsbTestTranslated[2].u.Interrupt.Vector=0x90;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  CHECK(UsbTestInterrupt.Create==1 && D->Interrupt==TEST_INTERRUPT && !UsbTestMmioWrites);
  D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
  UsbTestFillHandoff();
  UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
  UsbTestRegisters[0x60]=0x5d;
  UsbTestRegisters[USB_REG_POWER]=0x21;
  *L1Mask=0xa00f;
  UsbTestUseRealPort=TRUE;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestRegisters[0x0b]==0 && *L1Mask==0xa00f);
  UsbTestStartPortInterrupt(WdfPowerDeviceD3Final);
  CHECK(UsbTestWorkEnqueues==1);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestRegisters[0x0b]==0x32 && *L1Mask==0xa004 && D->InterruptUnmasked);

  *L1Status=0x0b; // TX, RX and DMA groups.
  UsbTestRegisters[0x0a]=0x10;
  CHECK(!UsbTestRunPortIsr() && !UsbTestInterrupt.Pending);
  *L1Status=4;
  UsbTestRegisters[0x0a]=0x08; // SOF alone is not owned.
  CHECK(!UsbTestRunPortIsr() && !UsbTestInterrupt.Pending);

  UsbTestResumePortRequest(0,0x23,3,MUSB_PORT_FEATURE_SUSPEND,NULL,0,STATUS_SUCCESS);
  UsbTestRegisters[USB_REG_POWER]=0x25; // Interrupt plus hardware resume signaling.
  UsbTestRegisters[0x0a]=0x0a;
  Enqueues=UsbTestWorkEnqueues;
  CHECK(UsbTestRunPortIsr() && !UsbTestRunPortIsr());
  CHECK(UsbTestRegisters[0x0a]==0x0a && UsbTestRegisters[0x0b]==0 &&
        UsbTestUcxRootChanged==0 && UsbTestWorkEnqueues==Enqueues);
  UsbTestRunPortDpc();
  CHECK(UsbTestWorkEnqueues==Enqueues+1);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestRegisters[0x0a]==0x08 && UsbTestRegisters[0x0b]==0x32);
  CHECK(!(D->Core.PortStatus&MUSB_PORT_SUSPEND) && UsbTestUcxRootChanged==1);

  UsbTestRegisters[0x0a]=0x20;
  UsbTestRegisters[0x60]=0x1d; // Power stays available while the device disconnects.
  UsbTestInterrupt.DropMaskWrite=TRUE;
  CHECK(UsbTestRunPortIsr() && UsbTestRegisters[0x0a]==0x20 && *L1Mask==0xa000);
  UsbTestInterrupt.DropMaskWrite=FALSE;
  UsbTestRunPortDpc();
  UsbWork(TEST_WORKER);
  CHECK(UsbTestRegisters[0x0a]==0 && UsbTestRegisters[0x0b]==0x32 && *L1Mask==0xa004);
  CHECK(D->Core.PortStatus==MUSB_PORT_POWER && UsbTestUcxRootChanged==2);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestUcxRootChanged==2);

  UsbTestResumePortRequest(1,0x23,1,MUSB_PORT_FEATURE_CHANGE_CONNECTION,NULL,0,STATUS_SUCCESS);
  UsbTestRegisters[0x0a]=0x10;
  UsbTestRegisters[0x60]=0x5d;
  Enqueues=UsbTestWorkEnqueues;
  CHECK(UsbTestRunPortIsr());
  UsbTestRunPortDpc();
  CHECK(UsbTestWorkEnqueues==Enqueues+1);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestUcxRootChanged==3 && (D->Core.PortStatus&MUSB_PORT_CONNECTION));

  UsbTestRegisters[0x0a]=0x10;
  CHECK(UsbTestRunPortIsr());
  *L1Mask|=0x10000; // Unrelated bits can change while the IRQ is owned.
  Enqueues=UsbTestWorkEnqueues;
  UsbTestStopPortInterrupt(WdfPowerDeviceD3);
  CHECK(UsbTestWorkEnqueues==Enqueues && UsbTestRegisters[0x0b]==0 &&
        *L1Mask==0x1a00f && !D->InterruptUnmasked);
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbTestRegisters[0x0b]==0 && *L1Mask==0x1a00f);
  UsbTestStartPortInterrupt(WdfPowerDeviceD3);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestRegisters[0x0b]==0x32 && *L1Mask==0x1a004 && (D->Core.PortStatus&MUSB_PORT_CONNECTION));
  UsbTestStopPortInterrupt(WdfPowerDeviceD3Final);
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  UsbTestPnpRelease();
  CHECK(UsbTestInterrupt.Delete==1 && UsbTestUnmapCalls==3 && !D->Registers && !D->Interrupt);
  Reads=UsbTestMmioReads;
  UsbWork(TEST_WORKER);
  CHECK(UsbTestMmioReads==Reads);

  // A new resource assignment gets a new framework interrupt object.
  UsbTestTranslated[2].u.Interrupt.Vector=0x91;
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  CHECK(UsbTestInterrupt.Create==2 && UsbTestInterrupt.Created && !D->InterruptReady);
  UsbTestPnpRelease();
  CHECK(UsbTestInterrupt.Delete==2 && UsbTestUnmapCalls==5);
}

static VOID UsbTestRootPortSleep(BOOLEAN RemoteWake) {
  URB Root={0},Bulk={0}; UCHAR Data[4]={0};
  WDFREQUEST Request; TEST_REQUEST_SLOT *Transfer;
  USB_CONTROLLER *D=&UsbTestController;
  const UCHAR Suspend[8]={0x23,3,MUSB_PORT_FEATURE_SUSPEND,0,1,0,0,0};
  ULONG Writes;

  UsbTestInitController();UsbTestFillResources();
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
  UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
  UsbTestFillHandoff();
  UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
  UsbTestRegisters[0x60]=0x5d;
  UsbTestRegisters[USB_REG_POWER]=0x21;
  UsbTestUseRealPort=TRUE;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);

  Root.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER_EX;
  Root.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER_EX);
  memcpy(Root.UrbControlTransferEx.SetupPacket,Suspend,sizeof(Suspend));
  Request=UsbTestNewRequest(0,&Root);UsbRootControl(TEST_ROOTHUB,Request);
  UsbWork(TEST_WORKER);UsbTestRunDpc();
  CHECK(UsbTestFindRequest(Request)->CompletionStatus==STATUS_SUCCESS);
  CHECK(UsbTestDelayCalls==1&&UsbTestDelayInterval==-30000);
  CHECK(UsbTestRegisters[USB_REG_POWER]==0x23&&D->Core.PortStatus&MUSB_PORT_SUSPEND);

  Writes=UsbTestMmioWrites;
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbTestMmioWrites==Writes&&UsbTestRegisters[USB_REG_POWER]==0x23);
  CHECK(D->Core.PortStatus&MUSB_PORT_SUSPEND);

  UsbTestInitEndpoint(USB_ENDPOINT_TYPE_BULK,0x01,64,0);
  {
    USB_ENDPOINT_DESCRIPTOR Desc={0};
    Desc.bLength=sizeof(Desc);Desc.bDescriptorType=USB_ENDPOINT_DESCRIPTOR_TYPE;
    Desc.bEndpointAddress=0x01;Desc.bmAttributes=USB_ENDPOINT_TYPE_BULK;Desc.wMaxPacketSize=64;
    CHECK(UsbEndpointAdd(TEST_UCX,TEST_USBDEVICE,&Desc,sizeof(Desc),NULL,
        (PUCXENDPOINT_INIT)(ULONG_PTR)0x900)==STATUS_SUCCESS);
  }
  UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
  UsbTestDevice.Address=9;
  UsbTestExpectedEndpointAddress=0x01;
  UsbTestExpectedTransferLength=sizeof(Data);UsbTestExpectedTimeout=1000;
  Bulk.UrbHeader.Function=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
  Bulk.UrbHeader.Length=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER);
  Bulk.UrbBulkOrInterruptTransfer.TransferBuffer=Data;
  Bulk.UrbBulkOrInterruptTransfer.TransferBufferLength=sizeof(Data);
  Request=UsbTestNewRequest(1,&Bulk);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  Transfer=UsbTestFindRequest(Request);UsbTestCoreDone=sizeof(Data);
  UsbWork(TEST_WORKER);
  CHECK(UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1);
  CHECK(UsbTestDataCalls==0&&Transfer->CompletionCount==0);

  if(RemoteWake) {
    ULONG Delays=UsbTestDelayCalls;
    // Stale interrupt without live RESUME must leave the port suspended.
    UsbTestRegisters[0x0a]=0x0a; // RESUME plus unrelated SOF
    UsbWork(TEST_WORKER);
    CHECK(UsbTestDelayCalls==Delays&&D->Core.PortStatus&MUSB_PORT_SUSPEND);
    CHECK(!UsbTestDataCalls&&!UsbTestUcxRootChanged);

    // The hardware takes over signaling and raises RESUME when the device wakes.
    UsbTestRegisters[0x0a]|=0x02;
    UsbTestRegisters[USB_REG_POWER]=0x25;
    UsbTestDropPowerWrite=TRUE;
    UsbWork(TEST_WORKER);
    CHECK(UsbTestDelayCalls==Delays+1&&UsbTestDelayInterval==-200000);
    CHECK(UsbTestRegisters[USB_REG_POWER]==0x25&&D->Core.PortStatus&MUSB_PORT_SUSPEND);
    CHECK(!UsbTestDataCalls&&!UsbTestUcxRootChanged&&UsbTestRegisters[0x0a]==0x08);

    // A later poll must finish an active signal even after W1C retired the event.
    Writes=UsbTestMmioWrites;
    UsbTestDropPowerWrite=FALSE;
    UsbWork(TEST_WORKER);UsbTestRunDpc();
    CHECK(UsbTestMmioWrites==Writes+1&&UsbTestLastWriteOffset==USB_REG_POWER);
    CHECK(UsbTestDelayCalls==Delays+3&&UsbTestDelayInterval==-100000);
    UsbWork(TEST_WORKER);UsbTestRunDpc();
    CHECK(UsbTestDelayCalls==Delays+3);
    Root.UrbControlTransferEx.SetupPacket[1]=1;
  } else {
    Root.UrbControlTransferEx.SetupPacket[1]=1;
    Request=UsbTestNewRequest(2,&Root);UsbRootControl(TEST_ROOTHUB,Request);
    UsbWork(TEST_WORKER);UsbTestRunDpc();
    CHECK(UsbTestFindRequest(Request)->CompletionStatus==STATUS_SUCCESS);
    CHECK(UsbTestDelayCalls==3&&UsbTestDelayInterval==-100000);
  }
  CHECK(UsbTestRegisters[USB_REG_POWER]==0x21&&!(D->Core.PortStatus&MUSB_PORT_SUSPEND));
  CHECK(D->Core.PortChangeStatus==MUSB_PORT_CHANGE_SUSPEND&&UsbTestUcxRootChanged==1);
  CHECK(UsbTestDataCalls==1&&Transfer->CompletionCount==1&&Transfer->CompletionStatus==STATUS_SUCCESS);

  Root.UrbControlTransferEx.SetupPacket[2]=MUSB_PORT_FEATURE_CHANGE_SUSPEND;
  Request=UsbTestNewRequest(3,&Root);UsbRootControl(TEST_ROOTHUB,Request);
  UsbWork(TEST_WORKER);UsbTestRunDpc();
  CHECK(UsbTestFindRequest(Request)->CompletionStatus==STATUS_SUCCESS);
  CHECK(D->Core.PortChangeStatus==0&&UsbTestUcxRootChanged==1);

  if(RemoteWake) {
    ULONG Delays=UsbTestDelayCalls;
    UsbTestResumePortRequest(4,0x23,3,MUSB_PORT_FEATURE_SUSPEND,NULL,0,STATUS_SUCCESS);
    CHECK(UsbTestDelayCalls==Delays+1);
    UsbTestRegisters[0x60]=0x45; // FSDEV, host session, VBUS lost.
    UsbTestRegisters[0x0a]=0x22; // DISCONNECT plus RESUME.
    UsbTestRegisters[USB_REG_POWER]=0x25;
    UsbWork(TEST_WORKER);UsbTestRunDpc();
    CHECK(UsbTestDelayCalls==Delays+1&&UsbTestRegisters[USB_REG_POWER]==0x25);
    CHECK(!D->Core.PortStatus&&UsbTestRegisters[0x0a]==0);
    CHECK(D->Core.PortChangeStatus==(MUSB_PORT_CHANGE_CONNECTION|MUSB_PORT_CHANGE_ENABLE));
    CHECK(UsbTestUcxRootChanged==2&&Transfer->CompletionCount==1);
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    return;
  }

  UsbTestRegisters[USB_REG_POWER]=0x23; // SUSPENDM without software ownership.
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_NOT_SUPPORTED);
  CHECK(!D->Online&&D->Stopping);
}

static VOID UsbTestRetainedGateRecovery(VOID) {
  const ULONG Clock=0xF000007EU;
  static const struct {
    NTSTATUS MethodStatus;
    ULONG Reply;
    BOOLEAN Latches,BadReply,RoleLost,Resumes;
  } Cases[]={
    {STATUS_SUCCESS,0xF000007EU,TRUE,FALSE,FALSE,TRUE},
    {STATUS_OBJECT_NAME_NOT_FOUND,0,FALSE,FALSE,FALSE,FALSE},
    {STATUS_IO_TIMEOUT,0,FALSE,FALSE,FALSE,FALSE},
    {STATUS_SUCCESS,0,FALSE,FALSE,FALSE,FALSE}, // Gate or clock validation failed in AML.
    {STATUS_SUCCESS,0xF000007FU,TRUE,FALSE,FALSE,FALSE}, // Unexpected clock identity.
    {STATUS_SUCCESS,0xF000007EU,TRUE,TRUE,FALSE,FALSE}, // Malformed response.
    {STATUS_SUCCESS,0xF000007EU,FALSE,FALSE,FALSE,FALSE}, // Read-back of MCLK still fails.
    {STATUS_SUCCESS,0xF000007EU,TRUE,FALSE,TRUE,FALSE}, // Host lost after clock came back.
  };

  for(ULONG I=0;I<ARRAYSIZE(Cases);++I) {
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    UsbTestController.Phy=(PUCHAR)UsbTestPhy;
    UsbTestController.PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();UsbTestRegisters[0x60]=0x3d;UsbTestClockIdentity=Clock;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    UsbTestHandoff[0]=0;
    UsbTestClockIdentity=0;
    UsbTestGateStatus=Cases[I].MethodStatus;
    UsbTestGateReply=Cases[I].Reply;
    UsbTestGateLatches=Cases[I].Latches;
    UsbTestGateBadReply=Cases[I].BadReply;
    if(Cases[I].RoleLost)UsbTestRegisters[0x60]=0x99;

    ULONG Reads=UsbTestMmioReads,Writes=UsbTestMmioWrites,Starts=UsbTestQueueStarts;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==(Cases[I].Resumes?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY));
    CHECK(UsbTestGateCalls==1&&UsbTestGateArgument==Clock);
    CHECK(UsbTestController.Online==Cases[I].Resumes);
    CHECK(UsbTestCoreInitCalls==1&&UsbTestCoreAdoptCalls==1&&UsbTestAcpiCalls==0);
    if(Cases[I].Resumes) {
      CHECK(UsbTestQueueStarts==Starts+2&&UsbTestAckCalls==1&&UsbTestControllerFailed==0);
    } else {
      CHECK(UsbTestQueueStarts==Starts&&UsbTestAckCalls==0&&UsbTestControllerFailed==1);
      CHECK(UsbTestMmioWrites==Writes);
      if(!Cases[I].RoleLost)CHECK(UsbTestMmioReads==Reads);
    }
    UsbTestCheckStartReferencesDrained();
  }
}

static VOID UsbTestRetainedHostMode(VOID) {
  static const struct {
    UCHAR Saved,Live,Expected;
    BOOLEAN DropWrite,Write,Resume;
  } Cases[]={
    {0x31,0xb1,0xb1,FALSE,FALSE,TRUE},
    {0x31,0x90,0xb1,FALSE,TRUE,TRUE},
    {0x20,0xa1,0xa1,FALSE,FALSE,TRUE}, // Takeover installs ENSUSPEND with HSENAB.
    {0x31,0x90,0x90,TRUE,TRUE,FALSE},
    {0x31,0x98,0x98,FALSE,FALSE,FALSE},
  };
  for(ULONG I=0;I<ARRAYSIZE(Cases);++I) {
    ULONG Writes,Starts;
    UsbTestInitController();UsbTestFillResources();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    UsbTestController.Phy=(PUCHAR)UsbTestPhy;
    UsbTestController.PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();UsbTestRegisters[0x60]=0x3d;UsbTestClockIdentity=0x1000007e;
    UsbTestRegisters[USB_REG_POWER]=Cases[I].Saved;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    UsbTestRegisters[USB_REG_POWER]=Cases[I].Live;
    UsbTestDropPowerWrite=Cases[I].DropWrite;
    Writes=UsbTestMmioWrites;Starts=UsbTestQueueStarts;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==
          (Cases[I].Resume?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY));
    CHECK(UsbTestRegisters[USB_REG_POWER]==Cases[I].Expected&&UsbTestRegisters[0x60]==0x3d);
    CHECK(UsbTestMmioWrites==Writes+(Cases[I].Write?1U:0U));
    if(Cases[I].Write)CHECK(UsbTestLastWriteOffset==USB_REG_POWER);
    CHECK(UsbTestController.Online==Cases[I].Resume&&!UsbTestController.CoreLock);
    CHECK(UsbTestQueueStarts==Starts+(Cases[I].Resume?2U:0U));
    UsbTestCheckStartReferencesDrained();
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  }
}

static VOID UsbTestRetainedPhyTuning(VOID) {
  enum {
    Retained,AnalogLost,AnalogAndFifoLost,RoleLost,PhySignalingLost,
    CalibrationBusy,DmaBusy,WriteDropped,RoleLostDuringWrite,UnsafeAtExit
  };
  static const ULONG Original[]={0x02007006,0x009c4400,0x50100475};
  static const ULONG Changed[]={0x00000004,0x01040000,0x00100800};
  for(ULONG Test=Retained;Test<=UnsafeAtExit;++Test) {
    ULONG Starts;
    BOOLEAN Expected=Test<=AnalogAndFifoLost;
    UsbTestInitController();UsbTestFillResources();
    UsbTestRawCount=UsbTestTranslatedCount=4;
    UsbTestRaw[3]=UsbTestTranslated[3]=UsbTestRaw[2];
    UsbTestRaw[2].Type=UsbTestTranslated[2].Type=CmResourceTypeMemory;
    UsbTestRaw[2].u.Memory.Start.QuadPart=UsbTestTranslated[2].u.Memory.Start.QuadPart=MTK_MUSB_PLATFORM_PHY_BASE;
    UsbTestRaw[2].u.Memory.Length=UsbTestTranslated[2].u.Memory.Length=MTK_MUSB_PLATFORM_PHY_REG_BYTES;
    UsbTestTranslated[2].Flags=CM_RESOURCE_MEMORY_READ_WRITE;
    if(Test==Retained) {
      UsbTestRaw[2].Flags=CM_RESOURCE_MEMORY_READ_ONLY;
      CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_DEVICE_CONFIGURATION_ERROR);
      CHECK(UsbTestMapCalls==0);
    }
    UsbTestRaw[2].Flags=CM_RESOURCE_MEMORY_READ_WRITE;
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    CHECK(UsbTestController.PhyBytes==MTK_MUSB_PLATFORM_PHY_REG_BYTES);
    for(ULONG I=0;I<ARRAYSIZE(Original);++I)
      UsbTestPhy[UsbPhyTuningFields[I].Offset/4]=Original[I];
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();UsbTestRegisters[0x60]=0x3d;UsbTestClockIdentity=0x1000007e;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    if(Test==UnsafeAtExit)UsbTestPhy[USB_PHY_ACR5/4]|=USB_PHY_CALIBRATING;
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
    if(Test!=Retained) {
      for(ULONG I=0;I<ARRAYSIZE(Original);++I)
        UsbTestPhy[UsbPhyTuningFields[I].Offset/4]=Changed[I];
    }
    switch(Test) {
      case AnalogAndFifoLost:
        UsbTestFifoResult=MtkMusbPlatformProfileMismatch;
        UsbTestFifoRestoreResult=MtkMusbPlatformSuccess;
        break;
      case RoleLost: UsbTestRegisters[0x60]=0x99;break;
      case PhySignalingLost: UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]^=0x200;break;
      case CalibrationBusy: UsbTestPhy[USB_PHY_ACR5/4]|=USB_PHY_CALIBRATING;break;
      case DmaBusy: UsbTestIdleResult=MtkMusbPlatformQuiescenceFailed;break;
      case WriteDropped: UsbTestDropPhyWriteAt=2;break;
      case RoleLostDuringWrite: UsbTestLoseHostAt=1;break;
    }
    Starts=UsbTestQueueStarts;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==
          (Expected?STATUS_SUCCESS:Test==UnsafeAtExit?STATUS_NOT_SUPPORTED:STATUS_DEVICE_NOT_READY));
    CHECK(UsbTestController.Online==Expected);
    CHECK(UsbTestQueueStarts==Starts+(Expected?2U:0U));
    CHECK(UsbTestCoreInitCalls==1&&UsbTestControllerFailed==(Expected?0U:1U));
    CHECK(UsbTestPhyWriteCount==(Test==WriteDropped?2U:Test==RoleLostDuringWrite?1U:
          Test==AnalogLost||Test==AnalogAndFifoLost?3U:0U));
    CHECK(UsbTestFifoRestores==(Test==AnalogAndFifoLost?1U:0U));
    for(ULONG I=0;I<UsbTestPhyWriteCount;++I)
      CHECK(UsbTestPhyWriteOffsets[I]==UsbPhyTuningFields[I].Offset);
    if(Test==AnalogLost||Test==AnalogAndFifoLost) {
      for(ULONG I=0;I<ARRAYSIZE(Original);++I) {
        ULONG Mask=UsbPhyTuningFields[I].Mask;
        CHECK(UsbTestPhy[UsbPhyTuningFields[I].Offset/4]==((Original[I]&Mask)|(Changed[I]&~Mask)));
      }
      CHECK(UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]==0x02000000&&
            UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]==0x00003f2d&&UsbTestRegisters[0x60]==0x3d);
      CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
      UsbTestFifoResult=MtkMusbPlatformSuccess;
      CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS&&UsbTestPhyWriteCount==3);
    }
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    CHECK(!UsbTestController.PhyBytes&&UsbTestUnmapCalls==3);
  }
}

static VOID UsbTestD0CoreFailureClosesAdmission(VOID) {
  UsbTestInitController();UsbTestFillResources();
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  UsbTestFillHandoff();UsbTestRegisters[0x60]=0x1d;UsbTestCoreStateResult=MusbTimeout;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
  CHECK(UsbTestController.Stopping&&!UsbTestController.Online&&UsbTestControllerFailed==1);
  UsbTestCheckStartReferencesDrained();
  CHECK(!UsbTestEndpointLockHeld&&UsbTestWdfLockDepth==0);
}

/* SET_ADDRESS must reach device address zero before updating UCX state.
 * Configure/disable and timer paths exercise the same production management queue. */
static VOID UsbTestAddressAndConfigure(VOID) {
  USBDEVICE_ENABLE Enable={0},EarlyEnable={0};USBDEVICE_ADDRESS Address={0};USBDEVICE_DISABLE Disable={0},DuplicateDisable={0};
  ENDPOINTS_CONFIGURE Configure={0};UCXENDPOINT Endpoints[1]={TEST_ENDPOINT};
  PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x903;
  WDFREQUEST Request;TEST_REQUEST_SLOT *Slot;ULONG Before,ControlCalls;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  UsbTestDevice.Enabled=0;
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
  CHECK(UsbTestDefaultEndpointEventsSeen&&UsbTestEndpoint.Stopped);
  Enable.Size=sizeof(Enable);Enable.UsbDevice=TEST_USBDEVICE;Enable.DefaultEndpoint=TEST_ENDPOINT;
  Request=UsbTestNewRequest(0,&Enable);UsbEnable(TEST_UCX,Request);
  Slot=UsbTestFindRequest(Request);CHECK(Slot->CompletionStatus==STATUS_SUCCESS&&UsbTestDevice.Enabled&&!UsbTestEndpoint.Stopped);
  UsbTestCheckStartReferencesDrained();
  Address.Size=sizeof(Address);Address.UsbDevice=TEST_USBDEVICE;
  UsbTestExpectedTimeout=1000;Request=UsbTestNewRequest(1,&Address);
  UsbAddressDevice(TEST_UCX,Request);UsbTimer(TEST_TIMER);UsbWork(TEST_WORKER);
  Slot=UsbTestFindRequest(Request);
  CHECK(UsbTestControlCalls==1&&UsbTestLastControlAddress==0);
  CHECK(UsbTestLastSetup[0]==0&&UsbTestLastSetup[1]==5&&UsbTestLastSetup[2]==1);
  CHECK(!UsbTestLastSetup[3]&&!UsbTestLastSetup[4]&&!UsbTestLastSetup[5]&&!UsbTestLastSetup[6]&&!UsbTestLastSetup[7]);
  CHECK(Address.Address==1&&UsbTestDevice.Address==1&&Slot->Context.Status==STATUS_SUCCESS&&Slot->CompletionCount==0);
  CHECK(!UsbTestStallCalls&&UsbTestDelayCalls==1&&UsbTestDelayInterval==-20000LL);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  UsbTestCoreResult=MusbStall;
  Request=UsbTestNewRequest(2,&Address);UsbAddressDevice(TEST_UCX,Request);UsbWork(TEST_WORKER);
  Slot=UsbTestFindRequest(Request);CHECK(UsbTestControlCalls==2&&!Address.Address&&UsbTestDevice.Address==1);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&!NT_SUCCESS(Slot->CompletionStatus));
  UsbTestCoreResult=MusbOk;Configure.Size=sizeof(Configure);Configure.UsbDevice=TEST_USBDEVICE;
  Configure.EndpointsToDisableCount=1;Configure.EndpointsToDisable=Endpoints;
  Before=UsbTestQueuePurges;Request=UsbTestNewRequest(3,&Configure);UsbConfigure(TEST_UCX,Request);UsbWork(TEST_WORKER);
  Slot=UsbTestFindRequest(Request);CHECK(UsbTestEndpoint.Stopped&&UsbTestQueuePurges==Before+2);
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  Configure.EndpointsToDisableCount=0;Configure.EndpointsToEnableCount=1;Configure.EndpointsToEnable=Endpoints;
  UsbTestEndpoint.Toggle=1;Request=UsbTestNewRequest(4,&Configure);UsbConfigure(TEST_UCX,Request);UsbWork(TEST_WORKER);
  Slot=UsbTestFindRequest(Request);CHECK(!UsbTestEndpoint.Stopped&&UsbTestEndpoint.Toggle==0);
  UsbTestCheckStartReferencesDrained();
  UsbTestRunDpc();CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  {
    URB ControlUrb={0};
    ControlUrb.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER;
    ControlUrb.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER);
    ControlUrb.UrbControlTransfer.SetupPacket[0]=0x80;
    ControlUrb.UrbControlTransfer.SetupPacket[1]=6;
    Request=UsbTestNewRequest(6,&ControlUrb);ControlCalls=UsbTestControlCalls;
    UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
    UsbWork(TEST_WORKER);UsbTestRunDpc();Slot=UsbTestFindRequest(Request);
    CHECK(UsbTestControlCalls==ControlCalls+1&&UsbTestLastSetup[0]==0x80);
    CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS&&
          ControlUrb.UrbHeader.Status==USBD_STATUS_SUCCESS);
  }
  {
    URB ControlUrb={0};UCHAR Setup[8]={0x80,6,0,1,0,0,0,0};
    ControlUrb.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER;
    ControlUrb.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER);
    ControlUrb.UrbControlTransfer.TransferBufferLength=0;
    RtlCopyMemory(ControlUrb.UrbControlTransfer.SetupPacket,Setup,sizeof(Setup));
    Request=UsbTestNewRequest(6,&ControlUrb);ControlCalls=UsbTestControlCalls;
    UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
    CHECK(UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1&&UsbTestEndpoint.Inflight==1);
    Disable.Size=sizeof(Disable);Disable.UsbDevice=TEST_USBDEVICE;Disable.DefaultEndpoint=TEST_ENDPOINT;
    Request=UsbTestNewRequest(5,&Disable);Slot=UsbTestFindRequest(Request);UsbDisable(TEST_UCX,Request);
    CHECK(!UsbTestDevice.Enabled&&!UsbTestDevice.Address&&UsbTestEndpoint.Stopped);
    CHECK(UsbTestDevice.DisableRequest==Request&&Slot->CompletionCount==0&&Slot->References==1);
    CHECK(UsbTestControlCalls==ControlCalls&&UsbTestStopCount==2);
    DuplicateDisable=Disable;Request=UsbTestNewRequest(7,&DuplicateDisable);Slot=UsbTestFindRequest(Request);
    UsbDisable(TEST_UCX,Request);
    CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_DEVICE_BUSY&&Slot->References==0);
    EarlyEnable.Size=sizeof(EarlyEnable);EarlyEnable.UsbDevice=TEST_USBDEVICE;EarlyEnable.DefaultEndpoint=TEST_ENDPOINT;
    Request=UsbTestNewRequest(7,&EarlyEnable);Slot=UsbTestFindRequest(Request);UsbEnable(TEST_UCX,Request);
    CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_DEVICE_BUSY&&!UsbTestDevice.Enabled);
    CHECK(UsbTestDeviceReferences==0);
    UsbTestFireQueueStop(UsbTestEndpoint.Incoming);
    CHECK(UsbTestEndpoint.Inflight==1&&!UsbTestRequests[6].Context.Completed&&
        UsbTestFindQueue(UsbTestEndpoint.Pending)->Count==1&&UsbTestEndpointReferences==2);
    CHECK(UsbTestRequests[5].CompletionCount==0);
    UsbTestFireQueueStop(UsbTestEndpoint.Pending);
    CHECK(UsbTestEndpoint.Inflight==1&&UsbTestRequests[6].Context.Completed==1&&
        UsbTestRequests[6].CompletionCount==0&&UsbTestRequests[5].CompletionCount==0&&UsbTestEndpointReferences==2);
    UsbTestRunDpc();
    CHECK(UsbTestEndpoint.Inflight==0&&UsbTestDevice.DisableRequest==NULL);
    CHECK(UsbTestRequests[6].CompletionCount==1&&UsbTestRequests[6].CompletionStatus==STATUS_CANCELLED);
    CHECK(UsbTestRequests[5].CompletionCount==1&&UsbTestRequests[5].CompletionStatus==STATUS_SUCCESS);
    CHECK(UsbTestRequests[5].References==0&&UsbTestEndpoint.StopKind==0&&UsbTestEndpointReferences==0);
  }
  Enable.Size=sizeof(Enable);Enable.UsbDevice=TEST_USBDEVICE;Enable.DefaultEndpoint=TEST_ENDPOINT;
  Request=UsbTestNewRequest(5,&Enable);UsbEnable(TEST_UCX,Request);Slot=UsbTestFindRequest(Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS&&UsbTestDevice.Enabled&&!UsbTestEndpoint.Stopped);
  UsbTestCheckStartReferencesDrained();
  CHECK(UsbReserveAddress(&UsbTestHub)==2);
  UsbDeviceCleanup((WDFOBJECT)TEST_USBDEVICE);
  UsbDeviceCleanup((WDFOBJECT)TEST_TT_HUB);
  CHECK(UsbReserveAddress(&UsbTestHub)==1);
  UsbDeviceCleanup((WDFOBJECT)TEST_TT_HUB);
}

static VOID UsbTestEnableRejectsUnavailableEndpoint(VOID) {
  USBDEVICE_ENABLE Enable={0};WDFREQUEST Request;TEST_REQUEST_SLOT *Slot;
  PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x905;ULONG Starts;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
  UsbTestDevice.Enabled=0;Starts=UsbTestQueueStarts;
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbTestController.Stopping&&!UsbTestController.Online);
  Enable.Size=sizeof(Enable);Enable.UsbDevice=TEST_USBDEVICE;Enable.DefaultEndpoint=TEST_ENDPOINT;
  Request=UsbTestNewRequest(0,&Enable);Slot=UsbTestFindRequest(Request);UsbEnable(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_DEVICE_NOT_READY);
  CHECK(!UsbTestDevice.Enabled&&UsbTestEndpoint.Stopped&&UsbTestQueueStarts==Starts);

  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
  UsbTestDevice.Enabled=0;UsbTestEndpoint.Dead=1;Starts=UsbTestQueueStarts;
  Request=UsbTestNewRequest(0,&Enable);Slot=UsbTestFindRequest(Request);UsbEnable(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_DEVICE_NOT_READY);
  CHECK(!UsbTestDevice.Enabled&&UsbTestEndpoint.Stopped&&UsbTestQueueStarts==Starts);
  UsbEndpointCleanup((WDFOBJECT)TEST_ENDPOINT);
  UsbTestCheckStartReferencesDrained();
  Request=UsbTestNewRequest(1,&Enable);Slot=UsbTestFindRequest(Request);UsbEnable(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_INVALID_PARAMETER);
  CHECK(!UsbTestDevice.Enabled&&UsbTestEndpoint.Stopped&&UsbTestQueueStarts==Starts);
  UsbTestCheckStartReferencesDrained();
}

/* QueueStart dispatch is permitted to run UsbSubmit inline. A synchronous
 * completion may reenter Disable, and its purge callbacks may reenter D0Exit.
 * The start owner must remain identifiable through both external calls. */
static VOID UsbTestInlineQueueStartDisableAndD0Exit(VOID) {
  USBDEVICE_ENABLE Enable={0};USBDEVICE_DISABLE Disable={0};
  WDFREQUEST EnableRequest,InlineRequest,DisableRequest;TEST_REQUEST_SLOT *EnableSlot,*InlineSlot,*DisableSlot;
  PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x906;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
  UsbTestDevice.Enabled=0;
  Enable.Size=sizeof(Enable);Enable.UsbDevice=TEST_USBDEVICE;Enable.DefaultEndpoint=TEST_ENDPOINT;
  Disable.Size=sizeof(Disable);Disable.UsbDevice=TEST_USBDEVICE;Disable.DefaultEndpoint=TEST_ENDPOINT;
  EnableRequest=UsbTestNewRequest(0,&Enable);EnableSlot=UsbTestFindRequest(EnableRequest);
  InlineRequest=UsbTestNewRequest(1,NULL);InlineSlot=UsbTestFindRequest(InlineRequest);
  DisableRequest=UsbTestNewRequest(2,&Disable);DisableSlot=UsbTestFindRequest(DisableRequest);
  UsbTestQueueStartDispatchRequest=InlineRequest;
  UsbTestQueueStartDispatchCode=IOCTL_INTERNAL_USB_SUBMIT_URB;
  UsbTestReentrantDisableRequest=DisableRequest;
  UsbTestReenterDisableOnRequestComplete=TRUE;
  UsbTestReenterD0ExitOnQueueStop=TRUE;
  UsbEnable(TEST_UCX,EnableRequest);
  UsbTestCheckStartReferencesDrained();

  CHECK(UsbTestQueueStartDispatches==1&&!UsbTestEndpointLockHeld);
  CHECK(InlineSlot->CompletionCount==1&&InlineSlot->CompletionStatus==STATUS_NOT_SUPPORTED);
  CHECK(!UsbTestDevice.Enabled&&UsbTestEndpoint.Stopped&&UsbTestEndpoint.StopKind==USB_STOP_DEVICE_DISABLE);
  CHECK(UsbTestEndpoint.QueueStopIssued&&UsbTestEndpoint.Purges==2&&UsbTestEndpoint.Inflight==0);
  CHECK(EnableSlot->CompletionCount==1&&EnableSlot->CompletionStatus==STATUS_DEVICE_BUSY);
  CHECK(DisableSlot->CompletionCount==0&&DisableSlot->References==1);
  CHECK(UsbTestReentrantD0ExitStatus==STATUS_DEVICE_BUSY);
  CHECK(UsbTestController.Online&&!UsbTestController.Stopping);
  CHECK(UsbTestTimerStops==0&&UsbTestWorkFlushes==0&&UsbTestQueuePurges==0&&UsbTestDpcFlushes==0);
  CHECK(UsbTestController.QueueStartsActive==0&&UsbTestController.QueueStartsDrained.Header.SignalState==1);

  UsbTestFireQueueStop(UsbTestEndpoint.Incoming);
  CHECK(DisableSlot->CompletionCount==0&&UsbTestEndpoint.Purges==1);
  UsbTestFireQueueStop(UsbTestEndpoint.Pending);
  CHECK(DisableSlot->CompletionCount==1&&DisableSlot->CompletionStatus==STATUS_SUCCESS);
  CHECK(DisableSlot->References==0&&UsbTestEndpoint.StopKind==0&&!UsbTestEndpoint.QueueStopIssued);
  CHECK(UsbTestEndpoint.Stopped&&UsbTestEndpointReferences==0);
  UsbTestCheckStartReferencesDrained();
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  UsbTestCheckStartReferencesDrained();
  CHECK(!UsbTestController.Online&&UsbTestController.Stopping);
  CHECK(UsbTestTimerStops==1&&UsbTestWorkFlushes==1&&UsbTestDpcFlushes==1);
}

static VOID UsbTestControllerResetKeepsActiveStartForLaterDrain(VOID) {
  PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x908;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
  UsbTestReenterControllerResetOnQueueStart=TRUE;
  UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
  UsbTestCheckStartReferencesDrained();
  CHECK(UsbTestControllerFailed==1&&UsbTestResetCompleteCalls==1);
  CHECK(UsbTestResetFailedOrder!=0&&UsbTestResetFailedOrder<UsbTestResetCompleteOrder);
  CHECK(UsbTestController.Stopping&&!UsbTestController.Online);
  CHECK(UsbTestController.QueueStartsActive==0&&UsbTestController.QueueStartsDrained.Header.SignalState==1);
  CHECK(!UsbTestEndpoint.StartListed&&!UsbTestEndpoint.QueueStartActive&&UsbTestEndpointReferences==0);
  CHECK(UsbTestTimerStops==0&&UsbTestWorkFlushes==0&&UsbTestQueuePurges==0&&UsbTestDpcFlushes==0);

  /* The reentrant callback left admission closed and ownership intact; normal
   * PnP teardown can drain queues after the active start unwinds. */
  CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  UsbTestCheckStartReferencesDrained();
  CHECK(UsbTestTimerStops==1&&UsbTestWorkFlushes==1&&UsbTestQueuePurges==4&&UsbTestDpcFlushes==1);
}

static VOID UsbTestRetainedControllerReset(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  PULONG L1Mask=(PULONG)(UsbTestRegisters+USB_REG_L1INTM);
  PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x90a;

  for(TEST_RESET_CASE Case=ResetPogo;Case<=ResetWaitingForVbus;++Case) {
    BOOLEAN Typec=Case==ResetTypecLostFifo || Case==ResetChangedCc;
    /* Only the live controller decides. A sideband route, clock or CC sample
     * that is unavailable or different does not prevent the recovery. */
    BOOLEAN Recovered=Case!=ResetVbusLost && Case!=ResetStopping && Case!=ResetWaitingForVbus;
    BOOLEAN ResetWritten=Case!=ResetWaitingForVbus;
    WDFREQUEST OldTransfer,OldRoot;

    UsbTestInitController();UsbTestFillResources();
    UsbTestResetCase=Case;
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    D->Phy=(PUCHAR)UsbTestPhy;D->PhyBytes=MTK_MUSB_PLATFORM_PHY_BYTES;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[2]=Typec?USB_HANDOFF_FLAGS_TYPEC_SOURCE:USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
    UsbTestExpectedCoreFlags=UsbTestHandoff[2];
    UsbTestHandoff[12]=UsbTestExpectedAdoptStatus=MUSB_PORT_POWER|MUSB_PORT_ENABLE|MUSB_PORT_CONNECTION;
    UsbTestHandoff[15]=Typec?USB_HANDOFF_PATH_TYPEC_SOURCE:USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
    UsbTestRouteStatus=STATUS_SUCCESS;UsbTestRouteReply=1;
    UsbTestTypecStatus=STATUS_SUCCESS;UsbTestTypecSample=0x0105020c;
    UsbTestUseRealPort=TRUE;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestRegisters[0x74]=0x08;UsbTestRegisters[0x75]=0x61;
    *L1Mask=0xa00f;
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    UsbTestStartPortInterrupt(WdfPowerDeviceD3Final);
    UsbWork(TEST_WORKER);
    CHECK(D->InterruptUnmasked && *L1Mask==0xa004);

    UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
    CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
    UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
    UsbTestDevice.Address=11;UsbTestEndpoint.Toggle=1;
    OldTransfer=UsbTestNewRequest(0,NULL);
    UsbEnqueue(D,&UsbTestEndpoint,OldTransfer,JobTransfer,NULL,UsbTestEndpoint.Pending);
    OldRoot=UsbTestNewRequest(1,NULL);UsbRootInterrupt(TEST_ROOTHUB,OldRoot);
    CHECK(!UsbTestFindRequest(OldTransfer)->CompletionCount && !UsbTestFindRequest(OldRoot)->CompletionCount);

    if(Case==ResetNoClock) UsbTestClockIdentity=0;
    if(Case==ResetNoRoute) UsbTestRouteReply=0;
    if(Case==ResetChangedCc) UsbTestTypecSample=0x0105080c;
    if(Case==ResetBusyDma) UsbTestRegisters[0x214]=1;
    if(Case==ResetWaitingForVbus) D->WaitingForVbus=TRUE;
    UsbControllerReset(TEST_UCX);
    UsbTestRunDpc();
    CHECK(UsbTestFindRequest(OldTransfer)->CompletionCount==1 &&
          UsbTestFindRequest(OldTransfer)->CompletionStatus==STATUS_CANCELLED &&
          UsbTestFindRequest(OldRoot)->CompletionCount==1 &&
          UsbTestFindRequest(OldRoot)->CompletionStatus==STATUS_CANCELLED);
    CHECK(UsbTestSwResetWrites==ResetWritten && UsbTestDelayCalls==ResetWritten &&
          UsbTestDevctlWrites==0);
    CHECK(UsbTestResetCompleteCalls==1 && UsbTestControllerFailed==!Recovered);
    CHECK(D->Online==Recovered && D->Stopping==!Recovered && !D->WaitingForVbus);
    CHECK(!UsbTestFindQueue(UsbTestEndpoint.Pending)->Started &&
          !UsbTestFindQueue(UsbTestEndpoint.Incoming)->Started);
    CHECK(UsbTestFindQueue(D->RootInterrupt)->Started==Recovered &&
          UsbTestFindQueue(D->Management)->Started==Recovered);
    if(Case==ResetReentrantPower) CHECK(UsbTestReentrantD0ExitStatus==STATUS_DEVICE_BUSY);
    if(Recovered) {
      CHECK(!UsbTestDevice.Address && !UsbTestDevice.Enabled && UsbTestEndpoint.Stopped && !UsbTestEndpoint.Toggle);
      CHECK(UsbTestRegisters[USB_REG_POWER]==0x21 && D->Core.State==MUSB_HOST_STATE_OPERATIONAL);
      CHECK(D->Core.PortPowerLost && D->Core.PortChangeStatus&MUSB_PORT_CHANGE_CONNECTION);
      CHECK(*L1Mask==0xa000 && !D->InterruptUnmasked);
      UsbTimer(TEST_TIMER);UsbWork(TEST_WORKER);
      CHECK(D->InterruptUnmasked && *L1Mask==0xa004);
      CHECK(D->Core.PortStatus==MUSB_PORT_POWER && UsbTestUcxRootChanged==1);
      UsbWork(TEST_WORKER);
      CHECK(UsbTestUcxRootChanged==1);
    } else {
      CHECK(UsbTestResetFailedOrder<UsbTestResetCompleteOrder);
    }
    UsbTestStopPortInterrupt(WdfPowerDeviceD3Final);
    CHECK(UsbD0Exit(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    UsbTestPnpRelease();
    CHECK(!D->Interrupt && !D->Registers);
  }
}

static VOID UsbTestPowerDownWaitsForReset(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  UsbTestInitController();
  D->ResetThread=UsbTestThread;KeClearEvent(&D->ResetDrained);D->InterruptReady=TRUE;
  CHECK(UsbD0PreInterruptsDisabled(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_DEVICE_BUSY);
  CHECK(D->InterruptReady && !D->LeavingD0 && !D->Stopping);
  D->ResetThread=(PETHREAD)(ULONG_PTR)0x708;
  UsbTestResetCanDrain=TRUE;
  CHECK(UsbD0PreInterruptsDisabled(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(UsbTestResetWaits==1 && !D->InterruptReady && D->LeavingD0 && D->Stopping);
}

/* UCX reports that a device reset already occurred. The callback only mirrors
 * address/toggle state; it must not enable a disabled device or claim MMIO. */
static VOID UsbTestResetDeviceAdmissionAndState(VOID) {
  USBDEVICE_RESET Reset={0};WDFREQUEST Request;TEST_REQUEST_SLOT *Slot;ULONG Writes,CoreStates;
  PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x907;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
  Reset.Size=sizeof(Reset);Reset.UsbDevice=TEST_USBDEVICE;
  Writes=UsbTestMmioWrites;CoreStates=UsbTestCoreStateCalls;

  UsbTestDevice.Enabled=0;UsbTestDevice.Address=0;UsbTestEndpoint.Stopped=1;UsbTestEndpoint.Toggle=1;
  Request=UsbTestNewRequest(0,&Reset);Slot=UsbTestFindRequest(Request);UsbResetDevice(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(!UsbTestDevice.Enabled&&UsbTestEndpoint.Stopped&&!UsbTestEndpoint.Toggle);
  CHECK(UsbTestMmioWrites==Writes&&UsbTestCoreStateCalls==CoreStates);

  UsbTestDevice.Enabled=1;UsbTestDevice.Address=23;UsbTestEndpoint.Stopped=0;UsbTestEndpoint.Toggle=1;
  Request=UsbTestNewRequest(1,&Reset);Slot=UsbTestFindRequest(Request);UsbResetDevice(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_SUCCESS);
  CHECK(UsbTestDevice.Enabled&&UsbTestDevice.Address==0&&!UsbTestEndpoint.Stopped&&!UsbTestEndpoint.Toggle);
  CHECK(UsbTestMmioWrites==Writes&&UsbTestCoreStateCalls==CoreStates);

  UsbTestDevice.Address=31;UsbTestEndpoint.Toggle=1;UsbTestEndpoint.Inflight=1;
  Request=UsbTestNewRequest(2,&Reset);Slot=UsbTestFindRequest(Request);UsbResetDevice(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_DEVICE_BUSY);
  CHECK(UsbTestDevice.Address==31&&UsbTestDevice.Enabled&&UsbTestEndpoint.Toggle==1);
  UsbTestEndpoint.Inflight=0;UsbTestDevice.DisableRequest=(WDFREQUEST)(ULONG_PTR)0xdead;
  Request=UsbTestNewRequest(3,&Reset);Slot=UsbTestFindRequest(Request);UsbResetDevice(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_DEVICE_BUSY);
  CHECK(UsbTestDevice.Address==31&&UsbTestDevice.Enabled&&UsbTestEndpoint.Toggle==1);
  UsbTestDevice.DisableRequest=NULL;UsbTestController.Stopping=1;UsbTestController.Online=0;
  Request=UsbTestNewRequest(4,&Reset);Slot=UsbTestFindRequest(Request);UsbResetDevice(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_DEVICE_NOT_READY);
  CHECK(UsbTestDevice.Address==31&&UsbTestDevice.Enabled&&UsbTestEndpoint.Toggle==1);
  Request=UsbTestNewRequest(5,&Reset);Slot=UsbTestFindRequest(Request);UsbResetDevice((UCXCONTROLLER)(ULONG_PTR)0x999,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_INVALID_PARAMETER);
  CHECK(UsbTestDevice.Address==31&&UsbTestDevice.Enabled&&UsbTestEndpoint.Toggle==1);
  CHECK(UsbTestMmioWrites==Writes&&UsbTestCoreStateCalls==CoreStates);
}

static VOID UsbTestCleanupBeforeLockCreate(VOID) {
  UsbTestInitController();UsbTestController.EndpointLock=NULL;
  UsbCleanup((WDFOBJECT)TEST_DEVICE);
  CHECK(UsbTestController.Stopping&&!UsbTestController.Online&&UsbTestDpcFlushes==1);
  CHECK(UsbTestDeviceReferences==0);
}

static VOID UsbTestDisableRejectsWrongEndpoint(VOID) {
  USBDEVICE_DISABLE Disable={0};WDFREQUEST Request;TEST_REQUEST_SLOT *Slot;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  Disable.Size=sizeof(Disable);Disable.UsbDevice=TEST_USBDEVICE;Disable.DefaultEndpoint=TEST_ENDPOINT;
  UsbTestEndpoint.Incoming=TEST_MANAGEMENT;UsbTestEndpoint.Pending=TEST_ROOT_INTERRUPT;
  UsbTestDevice.DefaultEndpoint=NULL;
  Request=UsbTestNewRequest(0,&Disable);Slot=UsbTestFindRequest(Request);UsbDisable(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_INVALID_PARAMETER);
  CHECK(UsbTestDevice.Enabled&&UsbTestEndpoint.Inflight==0&&UsbTestStopCount==0);
  Disable.UsbDevice=TEST_TT_HUB;UsbTestHub.Controller=&UsbTestController;UsbTestHub.Handle=TEST_TT_HUB;
  Request=UsbTestNewRequest(1,&Disable);Slot=UsbTestFindRequest(Request);UsbDisable(TEST_UCX,Request);
  CHECK(Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_INVALID_PARAMETER);
  CHECK(UsbTestDevice.Enabled&&UsbTestStopCount==0);
}

static VOID UsbTestDisableWaitsForInflight(VOID) {
  USBDEVICE_DISABLE Disable={0};URB Urb={0};UCHAR Setup[8]={0x80,6,0,1,0,0,0,0};
  WDFREQUEST Transfer,DisableRequest;USB_REQUEST *Active=NULL;TEST_REQUEST_SLOT *DisableSlot;
  PUCXENDPOINT_INIT Init=(PUCXENDPOINT_INIT)(ULONG_PTR)0x904;
  UsbTestInitController();UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,Init)==STATUS_SUCCESS);
  UsbEndpointStart(TEST_UCX,TEST_ENDPOINT);
  UsbTestCheckStartReferencesDrained();
  Urb.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER;Urb.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER);
  RtlCopyMemory(Urb.UrbControlTransfer.SetupPacket,Setup,sizeof(Setup));
  Transfer=UsbTestNewRequest(0,&Urb);
  CHECK(UsbNewRequest(&UsbTestController,&UsbTestEndpoint,Transfer,JobTransfer,&Urb,&Active)==STATUS_SUCCESS);
  UsbTestController.Active=Active;UsbTestController.ActiveEndpoint=&UsbTestEndpoint;
  Disable.Size=sizeof(Disable);Disable.UsbDevice=TEST_USBDEVICE;Disable.DefaultEndpoint=TEST_ENDPOINT;
  DisableRequest=UsbTestNewRequest(1,&Disable);DisableSlot=UsbTestFindRequest(DisableRequest);
  UsbDisable(TEST_UCX,DisableRequest);
  CHECK(UsbCancelled(&UsbTestController)&&DisableSlot->CompletionCount==0);
  UsbTestFireQueueStop(UsbTestEndpoint.Incoming);UsbTestFireQueueStop(UsbTestEndpoint.Pending);
  CHECK(DisableSlot->CompletionCount==0&&UsbTestEndpoint.Inflight==1);
  UsbTestController.Active=NULL;UsbTestController.ActiveEndpoint=NULL;
  UsbFinish(Active,STATUS_CANCELLED,USBD_STATUS_CANCELED,0);
  CHECK(DisableSlot->CompletionCount==0);
  UsbTestRunDpc();
  CHECK(DisableSlot->CompletionCount==1&&DisableSlot->CompletionStatus==STATUS_SUCCESS);
  CHECK(UsbTestEndpoint.Inflight==0&&UsbTestEndpointReferences==0&&DisableSlot->References==0);
}

static VOID UsbTestDeviceEnumeration(VOID) {
  UCXUSBDEVICE_INFO Info;ULONG Speed;PUCXUSBDEVICE_INIT Init=(PUCXUSBDEVICE_INIT)(ULONG_PTR)0x430;
  UsbTestInitController();memset(&Info,0,sizeof(Info));Info.Size=sizeof(Info);
  Info.PortPath.Size=sizeof(Info.PortPath);Info.PortPath.PortPathDepth=2;
  Info.PortPath.TTHubDepth=1;Info.PortPath.PortPath[0]=1;Info.PortPath.PortPath[1]=3;Info.TtHub=TEST_TT_HUB;
  for(Speed=UsbLowSpeed;Speed<=UsbHighSpeed;Speed++) {
    Info.DeviceSpeed=(USB_DEVICE_SPEED)Speed;
    CHECK(UsbDeviceAdd(TEST_UCX,&Info,Init)==STATUS_SUCCESS);
    CHECK(UsbTestDevice.Controller==&UsbTestController&&UsbTestDevice.Handle==TEST_USBDEVICE);
    CHECK(UsbTestDevice.Speed==Info.DeviceSpeed&&UsbTestDevice.TtHub==TEST_TT_HUB&&!UsbTestDevice.Enabled);
    CHECK(UsbTestDevice.Path.PortPathDepth==2&&UsbTestDevice.Path.TTHubDepth==1&&UsbTestDevice.Path.PortPath[1]==3);
  }
  CHECK(UsbTestDeviceCreates==3&&UsbTestDeviceCallbacks==3);
  Info.DeviceSpeed=UsbSuperSpeed;CHECK(UsbDeviceAdd(TEST_UCX,&Info,Init)==STATUS_NOT_SUPPORTED);
  Info.DeviceSpeed=UsbFullSpeed;Info.PortPath.PortPath[0]=2;
  CHECK(UsbDeviceAdd(TEST_UCX,&Info,Init)==STATUS_NOT_SUPPORTED);
  Info.PortPath.PortPath[0]=1;Info.PortPath.PortPathDepth=0;
  CHECK(UsbDeviceAdd(TEST_UCX,&Info,Init)==STATUS_NOT_SUPPORTED);
  Info.PortPath.PortPathDepth=MAX_USB_DEVICE_DEPTH+1;
  CHECK(UsbDeviceAdd(TEST_UCX,&Info,Init)==STATUS_NOT_SUPPORTED);
  Info.PortPath.PortPathDepth=2;CHECK(UsbDeviceAdd(TEST_UCX,&Info,NULL)==STATUS_NOT_SUPPORTED);
  CHECK(UsbTestDeviceCreates==3&&UsbTestDeviceCallbacks==3);
  UsbTestDeviceCreateStatus=STATUS_INSUFFICIENT_RESOURCES;
  CHECK(UsbDeviceAdd(TEST_UCX,&Info,Init)==STATUS_INSUFFICIENT_RESOURCES);
  CHECK(UsbTestDeviceCreates==4&&UsbTestDeviceCallbacks==4);
}
static VOID UsbTestHostIoctls(VOID) {
  WDFREQUEST Request;TEST_REQUEST_SLOT *Slot;
  UsbTestInitController();Request=UsbTestNewRequest(0,NULL);Slot=UsbTestFindRequest(Request);
  UsbTestIoctlHandled=TRUE;UsbIoControl(TEST_MANAGEMENT,Request,64,8,IOCTL_USB_GET_ROOT_HUB_NAME);
  CHECK(UsbTestIoctlCalls==1&&Slot->CompletionCount==0);
  Request=UsbTestNewRequest(1,NULL);Slot=UsbTestFindRequest(Request);UsbTestIoctlHandled=FALSE;
  UsbIoControl(TEST_MANAGEMENT,Request,64,8,IOCTL_USB_GET_ROOT_HUB_NAME);
  CHECK(UsbTestIoctlCalls==2&&Slot->CompletionCount==1&&Slot->CompletionStatus==STATUS_NOT_SUPPORTED);
}
static VOID UsbTestAddAndInterface(VOID) {
  NTSTATUS Status;ULONG Fail;
  UsbTestInitController();memset(&UsbTestController,0,sizeof(UsbTestController));UsbTestAddActive=1;
  Status=UsbAdd(NULL,(PWDFDEVICE_INIT)(ULONG_PTR)0x400);
  CHECK(Status==STATUS_SUCCESS&&UsbTestInterfaceCalls==1&&UsbTestQueueCreates==3);
  CHECK(UsbTestPowerCallbacks.EvtDevicePrepareHardware==UsbPrepare&&UsbTestPowerCallbacks.EvtDeviceReleaseHardware==UsbRelease);
  CHECK(UsbTestPowerCallbacks.EvtDeviceD0Entry==UsbD0Entry&&UsbTestPowerCallbacks.EvtDeviceD0Exit==UsbD0Exit);
  CHECK(UsbTestPowerCallbacks.EvtDeviceD0EntryPostInterruptsEnabled==UsbD0PostInterruptsEnabled &&
        UsbTestPowerCallbacks.EvtDeviceD0ExitPreInterruptsDisabled==UsbD0PreInterruptsDisabled);
  CHECK(UsbTestController.Stopping&&!UsbTestController.Online);
  UsbTestInitController();memset(&UsbTestController,0,sizeof(UsbTestController));UsbTestAddActive=1;UsbTestInterfaceStatus=STATUS_ACCESS_DENIED;
  CHECK(UsbAdd(NULL,(PWDFDEVICE_INIT)(ULONG_PTR)0x400)==STATUS_ACCESS_DENIED&&UsbTestInterfaceCalls==1);
  for(Fail=1;Fail<=3;Fail++) {
    UsbTestInitController();memset(&UsbTestController,0,sizeof(UsbTestController));UsbTestAddActive=1;UsbTestQueueCreateFailAt=Fail;
    CHECK(UsbAdd(NULL,(PWDFDEVICE_INIT)(ULONG_PTR)0x400)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(UsbTestQueueCreates==Fail&&UsbTestInterfaceCalls==0);
  }
}
static VOID UsbTestControlFlags(VOID) {
  URB Urb={0};
  UCHAR Buffer[32]={0};
  WDFREQUEST Request;
  TEST_REQUEST_SLOT *Slot;

  // Control IN without USBD_TRANSFER_DIRECTION_IN and with an oversized transfer buffer.
  UsbTestInitController();
  UsbTestInitEndpoint(USB_ENDPOINT_TYPE_CONTROL,0,8,0);
  CHECK(UsbDefaultEndpointAdd(TEST_UCX,TEST_USBDEVICE,8,(PUCXENDPOINT_INIT)(ULONG_PTR)0x905)==STATUS_SUCCESS);
  {
    USBDEVICE_ENABLE Enable={0};
    Enable.Size=sizeof(Enable);Enable.UsbDevice=TEST_USBDEVICE;Enable.DefaultEndpoint=TEST_ENDPOINT;
    Request=UsbTestNewRequest(4,&Enable);UsbEnable(TEST_UCX,Request);
    CHECK(UsbTestFindRequest(Request)->CompletionStatus==STATUS_SUCCESS);
  }
  UsbTestDevice.Address=3;UsbTestDevice.Enabled=1;
  UsbTestCoreResult=MusbOk;UsbTestCoreDone=8;
  UsbTestExpectedTransferLength=8;
  Urb.UrbHeader.Function=URB_FUNCTION_CONTROL_TRANSFER_EX;
  Urb.UrbHeader.Length=sizeof(struct _URB_CONTROL_TRANSFER_EX);
  Urb.UrbControlTransferEx.TransferFlags=USBD_SHORT_TRANSFER_OK; // no USBD_TRANSFER_DIRECTION_IN
  Urb.UrbControlTransferEx.TransferBuffer=Buffer;
  Urb.UrbControlTransferEx.TransferBufferLength=sizeof(Buffer); // 32 > wLength(8)
  Urb.UrbControlTransferEx.SetupPacket[0]=0x81; // HID class/interface IN
  Urb.UrbControlTransferEx.SetupPacket[1]=0x06; // GET_DESCRIPTOR
  Urb.UrbControlTransferEx.SetupPacket[6]=8;
  Request=UsbTestNewRequest(0,&Urb);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  UsbWork(TEST_WORKER);UsbTestRunDpc();
  Slot=UsbTestFindRequest(Request);
  CHECK(Slot && Slot->CompletionCount==1 && Slot->CompletionStatus==STATUS_SUCCESS &&
        Urb.UrbHeader.Status==USBD_STATUS_SUCCESS &&
        Urb.UrbControlTransferEx.TransferBufferLength==8);

  // Undersized TransferBufferLength (< wLength) and contradictory IN flag on OUT must fail.
  Urb.UrbControlTransferEx.TransferBufferLength=4;
  Request=UsbTestNewRequest(1,&Urb);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  UsbWork(TEST_WORKER);UsbTestRunDpc();
  Slot=UsbTestFindRequest(Request);
  CHECK(Slot && Slot->CompletionStatus==STATUS_INVALID_PARAMETER);

  Urb.UrbControlTransferEx.TransferBufferLength=8;
  Urb.UrbControlTransferEx.SetupPacket[0]=0x21; // Host-to-device OUT
  Urb.UrbControlTransferEx.TransferFlags=USBD_TRANSFER_DIRECTION_IN;
  Request=UsbTestNewRequest(2,&Urb);
  UsbSubmit(UsbTestEndpoint.Incoming,Request,0,0,IOCTL_INTERNAL_USB_SUBMIT_URB);
  UsbWork(TEST_WORKER);UsbTestRunDpc();
  Slot=UsbTestFindRequest(Request);
  CHECK(Slot && Slot->CompletionStatus==STATUS_INVALID_PARAMETER);

}

static VOID UsbTestOtgFirstPogoHotplug(VOID) {
  USB_CONTROLLER *D=&UsbTestController;

  // Boot on Type-C OTG, then dock pogo behind the same hub.
  UsbTestInitController();UsbTestFillResources();
  UsbTestAddTcpcResource();
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  UsbTestFillHandoff();
  UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
  UsbTestHandoff[2]=USB_HANDOFF_FLAGS_TYPEC_SOURCE;
  UsbTestHandoff[15]=USB_HANDOFF_PATH_TYPEC_SOURCE;
  UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
  UsbTestRegisters[USB_REG_POWER]=0x21;
  UsbTestRouteStatus=STATUS_SUCCESS;
  UsbTestRoutePerPath=TRUE;
  UsbTestRouteReplies[1]=1;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(D->Online && !D->WaitingForVbus && D->Core.Initialized &&
        D->PowerPath==USB_HANDOFF_PATH_TYPEC_SOURCE && D->TypecCc==1);
  ULONG Changes=UsbTestUcxRootChanged;
  UsbTestRouteReplies[2]=1;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(D->Online && !D->WaitingForVbus && !D->Core.PortPowerLost &&
        UsbTestUcxRootChanged==Changes);

  // Removing OTG leaves the dock and its host online.
  UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=0x050008;
  UsbTestRouteReplies[1]=0;UsbTestRouteReplies[2]=1;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(D->Online && !D->WaitingForVbus && !D->Core.PortPowerLost &&
        D->Core.State==MUSB_HOST_STATE_OPERATIONAL &&
        D->PowerPath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER && D->TypecCc==0);

  UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=0x05020c;
  UsbTestRouteReplies[1]=1;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(D->Online && !D->WaitingForVbus && !D->Core.PortPowerLost);

  // Now undock while OTG is already attached.
  UsbTestRouteReplies[2]=0;
  UsbTestClock+=100000;
  UsbWork(TEST_WORKER);
  CHECK(D->Online && !D->WaitingForVbus && !D->Core.PortPowerLost &&
        D->Core.State==MUSB_HOST_STATE_OPERATIONAL &&
        D->PowerPath==USB_HANDOFF_PATH_TYPEC_SOURCE && D->TypecCc==1);

  CHECK(UsbTestUcxRootChanged==Changes && UsbTestCoreInitCalls==1);
  CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
}

static VOID UsbTestTypecHandoffWithI2cStarting(VOID) {
  USB_CONTROLLER *D=&UsbTestController;

  // WinPE boot with TYPEC_SOURCE (1) when I2C6/TCPC is not yet started,
  //    while Pogo IS docked (MROU(2)==1): UsbFirstD0 succeeds immediately on Pogo.
  UsbTestInitController();UsbTestFillResources();
  UsbTestAddTcpcResource();
  CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  UsbTestFillHandoff();
  UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
  UsbTestHandoff[2]=USB_HANDOFF_FLAGS_TYPEC_SOURCE;
  UsbTestHandoff[15]=USB_HANDOFF_PATH_TYPEC_SOURCE;
  UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
  UsbTestRegisters[USB_REG_POWER]=0x21;
  UsbTestTcpc.OpenStatus=STATUS_NO_SUCH_DEVICE;
  UsbTestRouteStatus=STATUS_SUCCESS;
  UsbTestRoutePerPath=TRUE;
  UsbTestRouteReplies[1]=1;UsbTestRouteReplies[2]=1;
  CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(D->Online && !D->WaitingForVbus && D->Core.Initialized &&
        D->PowerPath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER);
  CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);

}

static VOID UsbTestSetPogoOtgState(BOOLEAN Pogo,BOOLEAN Otg) {
  ULONG Sample=Otg?0x05020c:0x050008;
  UsbTestRouteStatus=UsbTestTypecStatus=STATUS_SUCCESS;
  UsbTestRoutePerPath=TRUE;
  UsbTestRouteReplies[USB_HANDOFF_PATH_TYPEC_SOURCE]=Otg;
  UsbTestRouteReplies[USB_HANDOFF_PATH_ID_LOW_HUB_POWER]=Pogo;
  UsbTestTcpc.FirstState=UsbTestTcpc.SecondState=Sample;
  UsbTestTypecSample=USB_TYPEC_POWER_SAMPLE|Sample;
}

static VOID UsbTestPogoFirstOtgHotplug(VOID) {
  USB_CONTROLLER *D=&UsbTestController;
  for(ULONG WithConnection=0;WithConnection<=1;++WithConnection) {
    UsbTestInitController();UsbTestFillResources();
    // The normal _CRS supplies the writable PHY aperture even when I2C6 is still starting.
    CM_PARTIAL_RESOURCE_DESCRIPTOR Phy={0};
    Phy.Type=CmResourceTypeMemory;
    Phy.Flags=CM_RESOURCE_MEMORY_READ_WRITE;
    Phy.u.Memory.Start.QuadPart=MTK_MUSB_PLATFORM_PHY_BASE;
    Phy.u.Memory.Length=MTK_MUSB_PLATFORM_PHY_REG_BYTES;
    UsbTestRaw[UsbTestRawCount++]=Phy;
    UsbTestTranslated[UsbTestTranslatedCount++]=Phy;
    if(WithConnection) UsbTestAddTcpcResource();
    CHECK(UsbPrepare(TEST_DEVICE,TEST_RAW_RESOURCES,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
    CHECK(D->Phy==(PUCHAR)UsbTestPhy && D->PhyBytes==MTK_MUSB_PLATFORM_PHY_REG_BYTES);
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR1/4]=0x009c4400;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_ACR6/4]=0x50100475;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM0/4]=0x02000000;
    UsbTestPhy[MTK_MUSB_PLATFORM_PHY_DTM1/4]=0x00003f2d;
    UsbTestFillHandoff();
    UsbTestHandoff[1]=USB_HANDOFF_REVISION_POWER_PATH;
    UsbTestHandoff[2]=UsbTestExpectedCoreFlags=USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER;
    UsbTestHandoff[15]=USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
    UsbTestRegisters[USB_DEVCTL_REGISTER]=0x5d;
    UsbTestRegisters[USB_REG_POWER]=0x21;
    UsbTestSetPogoOtgState(TRUE,FALSE);
    CHECK(UsbD0Entry(TEST_DEVICE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(D->Online && D->Core.Initialized && !D->WaitingForVbus &&
          D->PowerPath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER);
    ULONG Changes=UsbTestUcxRootChanged;

    // Add, remove and re-add OTG while pogo stays docked.
    for(ULONG Step=0;Step<3;++Step) {
      UsbTestSetPogoOtgState(TRUE,Step!=1);
      UsbTestClock+=100000;
      UsbWork(TEST_WORKER);
      CHECK(D->Online && !D->WaitingForVbus && !D->Core.PortPowerLost &&
            UsbTestUcxRootChanged==Changes);
    }

    UsbTestRouteStatus=STATUS_OBJECT_NAME_NOT_FOUND;
    UsbTestClock+=100000;
    UsbWork(TEST_WORKER);
    CHECK(D->PowerPath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER && !D->WaitingForVbus &&
          !D->Core.PortPowerLost && UsbTestUcxRootChanged==Changes);

    // Only the dock goes away. A busy TCPC bus delays selecting Type-C
    // as the power source for the already-powered hub.
    UsbTestSetPogoOtgState(FALSE,TRUE);
    UsbTestTcpc.LockStatus=UsbTestTypecStatus=STATUS_IO_TIMEOUT;
    UsbTestClock+=100000;
    UsbWork(TEST_WORKER);
    CHECK(D->PowerPath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER && !D->WaitingForVbus &&
          !D->Core.PortPowerLost && UsbTestUcxRootChanged==Changes);
    UsbTestTcpc.LockStatus=UsbTestTypecStatus=STATUS_SUCCESS;
    UsbTestClock+=100000;
    UsbWork(TEST_WORKER);
    CHECK(D->Online && !D->WaitingForVbus && !D->Core.PortPowerLost &&
          D->Core.State==MUSB_HOST_STATE_OPERATIONAL &&
          D->PowerPath==USB_HANDOFF_PATH_TYPEC_SOURCE && D->TypecCc==1);
    CHECK(UsbTestUcxRootChanged==Changes && UsbTestCoreInitCalls==1);
    if(WithConnection) {
      CHECK(UsbTestTcpc.Reads>0 && UsbTestTypecCalls==0);
    } else {
      CHECK(UsbTestTcpc.Reads==0 && UsbTestTypecCalls>0);
    }
    CHECK(UsbRelease(TEST_DEVICE,TEST_TRANSLATED_RESOURCES)==STATUS_SUCCESS);
  }
}
int TestMain(void) {
  UsbTestFailure=UsbTestChecks=0;UsbTestInstallMocks();
  {
    UCHAR Storage[sizeof(ACPI_METHOD_ARGUMENT)+sizeof(ULONGLONG)]={0};
    PACPI_METHOD_ARGUMENT A=(PACPI_METHOD_ARGUMENT)Storage;
    ULONG Small=0x89abcdef,Parsed;ULONGLONG Wide=MAXULONG;
    A->Type=ACPI_METHOD_ARGUMENT_INTEGER;A->DataLength=sizeof(Small);for(ULONG J=0;J<sizeof(Small);++J)A->Data[J]=((const UCHAR *)&Small)[J];
    CHECK(UsbFirmwareInteger32(A,&Parsed)&&Parsed==Small);
    A->DataLength=sizeof(Wide);for(ULONG J=0;J<sizeof(Wide);++J)A->Data[J]=((const UCHAR *)&Wide)[J];
    CHECK(UsbFirmwareInteger32(A,&Parsed)&&Parsed==MAXULONG);
    Wide=(ULONGLONG)MAXULONG+1;for(ULONG J=0;J<sizeof(Wide);++J)A->Data[J]=((const UCHAR *)&Wide)[J];
    CHECK(!UsbFirmwareInteger32(A,&Parsed));A->DataLength=6;CHECK(!UsbFirmwareInteger32(A,&Parsed));
    A->Type=ACPI_METHOD_ARGUMENT_BUFFER;A->DataLength=sizeof(Small);CHECK(!UsbFirmwareInteger32(A,&Parsed));
  }
  UsbTestAddAndInterface();
  UsbTestHostIoctls();
  UsbTestDeviceEnumeration();
  UsbTestAddressAndConfigure();
  UsbTestEnableRejectsUnavailableEndpoint();
  UsbTestInlineQueueStartDisableAndD0Exit();
  UsbTestControllerResetKeepsActiveStartForLaterDrain();
  UsbTestRetainedControllerReset();
  UsbTestPowerDownWaitsForReset();
  UsbTestResetDeviceAdmissionAndState();
  UsbTestCleanupBeforeLockCreate();
  UsbTestDisableRejectsWrongEndpoint();
  UsbTestDisableWaitsForInflight();
  UsbTestRootHubAndPortZero();
  UsbTestRootRepeatedChange();
  UsbTestActualMusbHostCoreBulkOut();
  UsbTestEndpointDescriptorRouteAndMdl();
  UsbTestNakRetryAndCancel();
  UsbTestInterruptOutTimeoutAndBulkZlp();
  UsbTestColdFailureRetainsCleanupClock();
  UsbTestColdPhyModeFailureAfterPowerOn();
  UsbTestColdHaltFailureStillClosesTcpc();
  UsbTestPurgeAbortFence();
  UsbTestHandoffPowerPathValidation();
  UsbTestD0CoreFailureClosesAdmission();
  UsbTestResourcesAndD0Gate();
  UsbTestFrameAdmission();
  UsbTestWorkSerialization();
  UsbTestPhyResourcesAndState();
  UsbTestRetainedPower();
  UsbTestRetainedVbusLoss();
  UsbTestTypecVbusGate();
  UsbTestTypecVbusRecovery();
  UsbTestLiveTypecLoss();
  UsbTestBoardRouteLoss();
  UsbTestLostRetainedSession();
  UsbTestLostSessionWithDelayedVbus();
  UsbTestNativeTcpcAdmission();
  UsbTestFirstD0VbusSettles();
  UsbTestNativeTcpcDetachAndResume();
  UsbTestAttachedRouteReadFailure();
  UsbTestPortInterruptLifecycle();
  UsbTestRootPortSleep(TRUE);
  UsbTestRootPortSleep(FALSE);
  UsbTestRetainedGateRecovery();
  UsbTestRetainedHostMode();
  UsbTestRetainedPhyTuning();
  UsbTestControlFlags();
  UsbTestOtgFirstPogoHotplug();
  UsbTestTypecHandoffWithI2cStarting();
  UsbTestPogoFirstOtgHotplug();
  CHECK(UsbTestChecks>100);
  return (int)UsbTestFailure;
}
