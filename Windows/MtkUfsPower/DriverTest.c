/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <ntddscsi.h>
#include <prminterface.h>
#include <Library/MtkUfsPower.h>

#define TRACE_MINI_PREPARE 1001U
#define TRACE_MINI_STOP 1002U
#define TRACE_MINI_RESTART 1003U
#define TRACE_PRM_PREPARE 2001U
#define TRACE_PRM_RELEASE 2002U
#define TRACE_PRM_RESTORE 2003U
#define TRACE_LOWER_POWER 3001U

__declspec(dllexport) volatile ULONG MtkUfsPowerAssertionCount;

static PIO_STACK_LOCATION TestGetStack(PIRP Irp);
static NTSTATUS TestIoCreateDevice(PDRIVER_OBJECT,ULONG,PUNICODE_STRING,DEVICE_TYPE,ULONG,BOOLEAN,PDEVICE_OBJECT *);
static NTSTATUS TestIoAttachDeviceToDeviceStackSafe(PDEVICE_OBJECT,PDEVICE_OBJECT,PDEVICE_OBJECT *);
static NTSTATUS TestIoGetDeviceProperty(PDEVICE_OBJECT,DEVICE_REGISTRY_PROPERTY,ULONG,PVOID,PULONG);
static VOID TestIoDeleteDevice(PDEVICE_OBJECT);
static VOID TestIoDetachDevice(PDEVICE_OBJECT);
static NTSTATUS TestIoAcquireRemoveLock(PIO_REMOVE_LOCK,PVOID);
static VOID TestIoInitializeRemoveLock(PIO_REMOVE_LOCK,ULONG,ULONG,ULONG);
static VOID TestIoReleaseRemoveLock(PIO_REMOVE_LOCK,PVOID);
static VOID TestIoReleaseRemoveLockAndWait(PIO_REMOVE_LOCK,PVOID);
static PVOID TestExAllocatePool2(POOL_FLAGS,SIZE_T,ULONG);
static VOID TestExFreePoolWithTag(PVOID,ULONG);
static PIO_WORKITEM TestIoAllocateWorkItem(PDEVICE_OBJECT);
static VOID TestIoFreeWorkItem(PIO_WORKITEM);
static VOID TestIoQueueWorkItem(PIO_WORKITEM,PIO_WORKITEM_ROUTINE,WORK_QUEUE_TYPE,PVOID);
static PIRP TestIoAllocateIrp(CCHAR,BOOLEAN);
static VOID TestIoFreeIrp(PIRP);
static VOID TestIoSkipCurrentIrpStackLocation(PIRP);
static NTSTATUS TestIoCallDriver(PDEVICE_OBJECT,PIRP);
static BOOLEAN TestIoCancelIrp(PIRP);
static NTSTATUS TestPoCallDriver(PDEVICE_OBJECT,PIRP);
static VOID TestIoCopyCurrentIrpStackLocationToNext(PIRP);
static VOID TestIoSetCompletionRoutine(PIRP,PIO_COMPLETION_ROUTINE,PVOID,BOOLEAN,BOOLEAN,BOOLEAN);
static NTSTATUS TestIoSetCompletionRoutineEx(PDEVICE_OBJECT,PIRP,PIO_COMPLETION_ROUTINE,PVOID,BOOLEAN,BOOLEAN,BOOLEAN);
static VOID TestObReference(PVOID);
static VOID TestObDereference(PVOID);
static VOID TestKeInitializeSpinLock(PKSPIN_LOCK);
static VOID TestKeAcquireSpinLock(PKSPIN_LOCK,PKIRQL);
static VOID TestKeReleaseSpinLock(PKSPIN_LOCK,KIRQL);
static VOID TestKeClearEvent(PKEVENT);
static LONG TestKeReadStateEvent(PKEVENT);
static ULONG TestKeGetCurrentIrql(VOID);
static VOID TestKeInitializeEvent(PKEVENT,EVENT_TYPE,BOOLEAN);
static VOID TestKeInitializeMutex(PRKMUTEX,ULONG);
static LONG TestKeReleaseMutex(PRKMUTEX,BOOLEAN);
static LONG TestKeSetEvent(PRKEVENT,KPRIORITY,BOOLEAN);
static NTSTATUS TestKeWaitForSingleObject(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
static NTSTATUS TestKeDelayExecutionThread(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
static ULONGLONG TestKeQueryInterruptTime(VOID);
static VOID TestIoCompleteRequest(PIRP,CCHAR);
static VOID TestIoMarkIrpPending(PIRP);
static POWER_STATE TestPoSetPowerState(PDEVICE_OBJECT,POWER_STATE_TYPE,POWER_STATE);
static NTSTATUS TestExGetPrmInterface(ULONG,PPRM_INTERFACE);
static NTSTATUS TestPrmTryUpdate(VOID);
static VOID CompleteMiniportIrp(VOID);
static BOOLEAN TestRtlEqualUnicodeString(PCUNICODE_STRING,PCUNICODE_STRING,BOOLEAN);

#undef IoCreateDevice
#undef IoAttachDeviceToDeviceStackSafe
#undef IoGetDeviceProperty
#undef IoDeleteDevice
#undef IoDetachDevice
#undef IoAcquireRemoveLock
#undef IoInitializeRemoveLock
#undef IoReleaseRemoveLock
#undef IoReleaseRemoveLockAndWait
#undef ExAllocatePool2
#undef ExFreePoolWithTag
#undef IoAllocateWorkItem
#undef IoFreeWorkItem
#undef IoQueueWorkItem
#undef IoAllocateIrp
#undef IoFreeIrp
#undef IoSkipCurrentIrpStackLocation
#undef IoCallDriver
#undef IoCancelIrp
#undef PoCallDriver
#undef IoCopyCurrentIrpStackLocationToNext
#undef IoSetCompletionRoutine
#undef IoSetCompletionRoutineEx
#undef ObReferenceObject
#undef ObDereferenceObject
#undef KeInitializeSpinLock
#undef KeAcquireSpinLock
#undef KeReleaseSpinLock
#undef KeClearEvent
#undef KeReadStateEvent
#undef IoGetCurrentIrpStackLocation
#undef KeGetCurrentIrql
#undef KeInitializeEvent
#undef KeInitializeMutex
#undef KeReleaseMutex
#undef KeSetEvent
#undef KeWaitForSingleObject
#undef KeDelayExecutionThread
#undef KeQueryInterruptTime
#undef IoCompleteRequest
#undef IoMarkIrpPending
#undef PoSetPowerState
#undef ExGetPrmInterface
#undef RtlEqualUnicodeString

#define IoCreateDevice TestIoCreateDevice
#define IoAttachDeviceToDeviceStackSafe TestIoAttachDeviceToDeviceStackSafe
#define IoGetDeviceProperty TestIoGetDeviceProperty
#define IoDeleteDevice TestIoDeleteDevice
#define IoDetachDevice TestIoDetachDevice
#define IoAcquireRemoveLock TestIoAcquireRemoveLock
#define IoInitializeRemoveLock TestIoInitializeRemoveLock
#define IoReleaseRemoveLock TestIoReleaseRemoveLock
#define IoReleaseRemoveLockAndWait TestIoReleaseRemoveLockAndWait
#define ExAllocatePool2 TestExAllocatePool2
#define ExFreePoolWithTag TestExFreePoolWithTag
#define IoAllocateWorkItem TestIoAllocateWorkItem
#define IoFreeWorkItem TestIoFreeWorkItem
#define IoQueueWorkItem TestIoQueueWorkItem
#define IoAllocateIrp TestIoAllocateIrp
#define IoFreeIrp TestIoFreeIrp
#define IoSkipCurrentIrpStackLocation TestIoSkipCurrentIrpStackLocation
#define IoCallDriver TestIoCallDriver
#define IoCancelIrp TestIoCancelIrp
#define PoCallDriver TestPoCallDriver
#define IoCopyCurrentIrpStackLocationToNext TestIoCopyCurrentIrpStackLocationToNext
#define IoSetCompletionRoutine TestIoSetCompletionRoutine
#define IoSetCompletionRoutineEx TestIoSetCompletionRoutineEx
#define ObReferenceObject TestObReference
#define ObDereferenceObject TestObDereference
#define KeInitializeSpinLock TestKeInitializeSpinLock
#define KeAcquireSpinLock TestKeAcquireSpinLock
#define KeReleaseSpinLock TestKeReleaseSpinLock
#define KeClearEvent TestKeClearEvent
#define KeReadStateEvent TestKeReadStateEvent
#define IoGetCurrentIrpStackLocation TestGetStack
#define KeGetCurrentIrql TestKeGetCurrentIrql
#define KeInitializeEvent TestKeInitializeEvent
#define KeInitializeMutex TestKeInitializeMutex
#define KeReleaseMutex TestKeReleaseMutex
#define KeSetEvent TestKeSetEvent
#define KeWaitForSingleObject TestKeWaitForSingleObject
#define KeDelayExecutionThread TestKeDelayExecutionThread
#define KeQueryInterruptTime TestKeQueryInterruptTime
#define IoCompleteRequest TestIoCompleteRequest
#define IoMarkIrpPending TestIoMarkIrpPending
#define PoSetPowerState TestPoSetPowerState
#define ExGetPrmInterface TestExGetPrmInterface
#define RtlEqualUnicodeString TestRtlEqualUnicodeString

#include "Driver.c"
#include "PrmClient.c"

static ULONG FailureLine,Checks,MiniCallCount,MiniPrepareCount,MiniStopCount,MiniRestartCount,MiniNopCount,MiniLiveResumeCount;
static ULONG PrmCallCount,PrmPrepareCount,PrmReleaseCount,PrmRestoreCount;
static ULONG PrmGetCount,PrmLockCount,PrmQueryHandlerCount,PrmUnlockCount,PrmUpdateCount;
static LONG PrmModuleLockDepth;
static ULONG WorkQueued,WorkFreed,CompleteCount,RemoveAcquires,RemoveReleases;
static ULONG TraceCount,Trace[64],MalformedMiniResponse;
static LONG CurrentIrql=PASSIVE_LEVEL;
static ULONGLONG FakeTime=1;
static NTSTATUS LowerPowerStatus=STATUS_SUCCESS,LowerPowerReturnStatus=STATUS_SUCCESS;
static NTSTATUS PrmApiFailure=STATUS_SUCCESS,PrmLockFailure=STATUS_SUCCESS;
static NTSTATUS PrmQueryFailure=STATUS_SUCCESS,PrmInvokeFailure=STATUS_SUCCESS,PrmUnlockFailure=STATUS_SUCCESS;
static NTSTATUS IoctlReturnStatus=STATUS_SUCCESS,IoctlCompletionStatus=STATUS_SUCCESS;
static ULONG PrmFailureOperation=MAXULONG,MiniFailureOperation=MAXULONG;
static BOOLEAN PrmFound=TRUE,PrmEfiFailure,PrmMissingInvoke,PrmReenter,WorkPending,Deleted,Detached;
static NTSTATUS PrmReenterStatus=STATUS_SUCCESS;
static BOOLEAN MiniPending,MiniOperationAccepted,MiniPendingLiveResume,MiniHceLive,PrmPending,MiniTimeoutStop;
static ULONG PrmState=MtkUfsPowerActive,PrmPendingOperation;
static ULONGLONG PrmGeneration=1;
static ULONG MiniState=MtkUfsMiniPowerActive,MiniFlags;
static ULONGLONG MiniGeneration=1;
static USHORT MiniPendingOperation;
static UCHAR HardwareIdValid=TRUE;
static PIO_WORKITEM_ROUTINE QueuedWorkRoutine;
static PVOID QueuedWorkContext;
static PDEVICE_OBJECT QueuedWorkDevice;
static PIO_COMPLETION_ROUTINE SavedCompletion;
static PVOID SavedCompletionContext;
static BOOLEAN IoctlOwned,IoctlDone,IoctlTimeout,IoctlCompletionDelayed,RemoveForwarded,RemovePending;
static NTSTATUS UsageCompletionStatus,UsageReturnStatus;
static BOOLEAN UsageModifyLowerFlags,UsageSawLastRemovalPageable;
static ULONG UsageForwardCount;
static ULONG IoctlFrees,IoctlCancels,FilterReferences,LowerReferences;
static BOOLEAN IoctlDispatched,MiniportLockHeld;
static NTSTATUS IoctlRegistrationStatus;
static PIRP WorkerIrp;
static BOOLEAN WorkerCompleted;
static ULONG MalformedMiniInformation;
static ULONG MockPoolState;
static BOOLEAN PoolAllocationFails,WorkItemAllocationFails;
static __declspec(align(16)) UCHAR MockPool[256];
static __declspec(align(16)) UCHAR DeviceExtensionBuffer[512];
static DEVICE_OBJECT FilterDevice,PhysicalDevice,LowerDevice;
static DRIVER_OBJECT DriverObject;
static DRIVER_EXTENSION DriverExtension;
static __declspec(align(16)) UCHAR DummyWorkItemBuffer[128];
static IRP IoctlIrp;
static IO_STACK_LOCATION IoctlStack;

#define CHECK(Expression) do { MtkUfsPowerAssertionCount++; Checks++; if(!(Expression) && !FailureLine) FailureLine=__LINE__; } while(0)

static VOID ResetModel(VOID) {
  /* Test-only isolation; production has no PRM session reset or unlock path. */
  RtlZeroMemory(&mInterface,sizeof(mInterface));
  mClientState=MtkUfsPrmClientUninitialized;
  InterlockedExchange(&mClientBusy,0);
  RtlZeroMemory(&FilterDevice,sizeof(FilterDevice));
  RtlZeroMemory(&PhysicalDevice,sizeof(PhysicalDevice));
  RtlZeroMemory(&LowerDevice,sizeof(LowerDevice));
  RtlZeroMemory(&DriverObject,sizeof(DriverObject));
  RtlZeroMemory(&DriverExtension,sizeof(DriverExtension));
  RtlZeroMemory(DeviceExtensionBuffer,sizeof(DeviceExtensionBuffer));
  RtlZeroMemory(DummyWorkItemBuffer,sizeof(DummyWorkItemBuffer));
  RtlZeroMemory(&IoctlIrp,sizeof(IoctlIrp));
  RtlZeroMemory(&IoctlStack,sizeof(IoctlStack));
  DriverObject.DriverExtension=&DriverExtension;
  PhysicalDevice.DeviceType=FILE_DEVICE_DISK;
  LowerDevice.DeviceType=FILE_DEVICE_DISK;
  LowerDevice.StackSize=1;
  CurrentIrql=PASSIVE_LEVEL; FakeTime=1; LowerPowerStatus=LowerPowerReturnStatus=STATUS_SUCCESS;
  PrmApiFailure=STATUS_SUCCESS; PrmFailureOperation=MAXULONG; MiniFailureOperation=MAXULONG;
  PrmLockFailure=PrmQueryFailure=PrmInvokeFailure=PrmUnlockFailure=STATUS_SUCCESS;
  IoctlReturnStatus=IoctlCompletionStatus=STATUS_SUCCESS; MiniTimeoutStop=FALSE;
  UsageCompletionStatus=UsageReturnStatus=STATUS_SUCCESS;UsageModifyLowerFlags=FALSE;
  UsageSawLastRemovalPageable=FALSE;UsageForwardCount=0;
  PrmFound=TRUE; PrmEfiFailure=FALSE; PrmMissingInvoke=FALSE; PrmReenter=FALSE;
  PrmReenterStatus=STATUS_SUCCESS; WorkPending=FALSE; Deleted=FALSE; Detached=FALSE;
  MiniPending=FALSE; MiniOperationAccepted=FALSE; MiniPendingLiveResume=FALSE; MiniHceLive=TRUE;
  MiniPendingOperation=0; PrmPending=FALSE; PrmPendingOperation=MAXULONG;
  PrmState=MtkUfsPowerActive; PrmGeneration=1;
  MiniState=MtkUfsMiniPowerActive; MiniFlags=0; MiniGeneration=1;
  MiniCallCount=MiniPrepareCount=MiniStopCount=MiniRestartCount=MiniNopCount=MiniLiveResumeCount=0;
  PrmCallCount=PrmPrepareCount=PrmReleaseCount=PrmRestoreCount=0;
  PrmGetCount=PrmLockCount=PrmQueryHandlerCount=PrmUnlockCount=PrmUpdateCount=0;
  PrmModuleLockDepth=0;
  WorkQueued=WorkFreed=CompleteCount=RemoveAcquires=RemoveReleases=0;
  TraceCount=0; RtlZeroMemory(Trace,sizeof(Trace)); MalformedMiniResponse=0;
  QueuedWorkRoutine=NULL; QueuedWorkContext=NULL; QueuedWorkDevice=NULL;
  SavedCompletion=NULL; SavedCompletionContext=NULL;
  IoctlOwned=IoctlDone=IoctlTimeout=IoctlCompletionDelayed=RemoveForwarded=RemovePending=FALSE;
  IoctlFrees=IoctlCancels=FilterReferences=LowerReferences=0;
  IoctlDispatched=MiniportLockHeld=FALSE;
  IoctlRegistrationStatus=STATUS_SUCCESS;
  WorkerIrp=NULL; WorkerCompleted=FALSE;
  MalformedMiniInformation=0;
  MockPoolState=0; PoolAllocationFails=WorkItemAllocationFails=FALSE; HardwareIdValid=TRUE;
}

static VOID InitIrp(PIRP Irp,PIO_STACK_LOCATION Stack,UCHAR Major,UCHAR Minor) {
  RtlZeroMemory(Irp,sizeof(*Irp)); RtlZeroMemory(Stack,sizeof(*Stack));
  Stack->MajorFunction=Major; Stack->MinorFunction=Minor;
  Irp->Tail.Overlay.CurrentStackLocation=Stack;
  Irp->IoStatus.Status=STATUS_SUCCESS;
}

static PIO_STACK_LOCATION TestGetStack(PIRP Irp) {
  return Irp?Irp->Tail.Overlay.CurrentStackLocation:NULL;
}

static VOID AddFilter(VOID) {
  NTSTATUS Status;
  CHECK(DriverEntry(&DriverObject,NULL)==STATUS_SUCCESS);
  CHECK(DriverExtension.AddDevice==MtkUfsPowerAddDevice);
  Status=DriverExtension.AddDevice(&DriverObject,&PhysicalDevice);
  CHECK(Status==STATUS_SUCCESS && !Deleted);
  CHECK(((MTK_UFS_POWER_DEVICE *)FilterDevice.DeviceExtension)->Lower==&LowerDevice);
  CHECK(((MTK_UFS_POWER_DEVICE *)FilterDevice.DeviceExtension)->PowerState==PowerDeviceD0);
}

static VOID RunQueuedWork(VOID) {
  PIO_WORKITEM_ROUTINE Routine=QueuedWorkRoutine;
  PVOID Context=QueuedWorkContext;
  PDEVICE_OBJECT Device=QueuedWorkDevice;
  CHECK(WorkPending && Routine && Device==&FilterDevice && CurrentIrql==PASSIVE_LEVEL);
  WorkPending=FALSE; QueuedWorkRoutine=NULL; QueuedWorkContext=NULL; QueuedWorkDevice=NULL;
  Routine(Device,Context);
}

static NTSTATUS PowerIrp(PDEVICE_OBJECT Device,DEVICE_POWER_STATE Target) {
  IRP Irp; IO_STACK_LOCATION Stack; NTSTATUS Status;
  InitIrp(&Irp,&Stack,IRP_MJ_POWER,IRP_MN_SET_POWER);
  Stack.Parameters.Power.Type=DevicePowerState;
  Stack.Parameters.Power.State.DeviceState=Target;
  Status=MtkUfsPowerDispatch(Device,&Irp);
  if(Status==STATUS_PENDING) RunQueuedWork();
  return Status==STATUS_PENDING?Irp.IoStatus.Status:Status;
}

static VOID TestPrmClient(VOID) {
  MTK_UFS_POWER_REQUEST Request;
  NTSTATUS Status;
  RtlZeroMemory(&Request,sizeof(Request));
  Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
  Request.Operation=MtkUfsPowerQuery;

  CurrentIrql=DISPATCH_LEVEL;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_INVALID_PARAMETER && PrmGetCount==0);
  CurrentIrql=PASSIVE_LEVEL;
  Request.Size--;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_INVALID_PARAMETER && PrmGetCount==0);
  Request.Size=sizeof(Request);

  PrmApiFailure=STATUS_PENDING;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_UNSUCCESSFUL && PrmGetCount==1 &&
        PrmLockCount==0 && PrmUnlockCount==0 && PrmModuleLockDepth==0);
  PrmApiFailure=STATUS_DEVICE_BUSY;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_DEVICE_BUSY && PrmGetCount==2 &&
        PrmLockCount==0 && PrmUnlockCount==0 && PrmModuleLockDepth==0);
  PrmApiFailure=STATUS_SUCCESS; PrmMissingInvoke=TRUE;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_NOT_SUPPORTED && PrmGetCount==3 &&
        PrmLockCount==0 && PrmUnlockCount==0);
  PrmMissingInvoke=FALSE;

  PrmLockFailure=STATUS_TIMEOUT;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_UNSUCCESSFUL && PrmLockCount==1 &&
        PrmQueryHandlerCount==0 && PrmUnlockCount==0 && PrmModuleLockDepth==0);
  PrmLockFailure=STATUS_PENDING;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_UNSUCCESSFUL && PrmLockCount==2 &&
        PrmQueryHandlerCount==0 && PrmUnlockCount==0 && PrmModuleLockDepth==0);
  PrmLockFailure=STATUS_DEVICE_BUSY;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_DEVICE_BUSY && PrmLockCount==3 &&
        PrmQueryHandlerCount==0 && PrmUnlockCount==0 && PrmModuleLockDepth==0);
  PrmLockFailure=STATUS_SUCCESS;

  PrmQueryFailure=STATUS_TIMEOUT;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_UNSUCCESSFUL && PrmLockCount==4 &&
        PrmQueryHandlerCount==1 && PrmUnlockCount==1 && PrmModuleLockDepth==0);
  CHECK(TestPrmTryUpdate()==STATUS_SUCCESS);
  PrmQueryFailure=STATUS_PENDING;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_UNSUCCESSFUL && PrmLockCount==5 &&
        PrmQueryHandlerCount==2 && PrmUnlockCount==2 && PrmModuleLockDepth==0);
  PrmQueryFailure=STATUS_DEVICE_BUSY;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_DEVICE_BUSY && PrmLockCount==6 &&
        PrmQueryHandlerCount==3 && PrmUnlockCount==3 && PrmModuleLockDepth==0);
  PrmQueryFailure=STATUS_SUCCESS;
  PrmFound=FALSE;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_NOT_SUPPORTED && PrmLockCount==7 &&
        PrmQueryHandlerCount==4 && PrmUnlockCount==4 && PrmModuleLockDepth==0);
  CHECK(TestPrmTryUpdate()==STATUS_SUCCESS);
  PrmFound=TRUE;

  Status=MtkUfsPrmInvoke(&Request);
  CHECK(Status==STATUS_SUCCESS && Request.Result==MtkUfsPowerSuccess && Request.State==PrmState);
  CHECK(PrmCallCount==1 && PrmGetCount==11 && PrmLockCount==8 &&
        PrmQueryHandlerCount==5 && PrmUnlockCount==4 && PrmModuleLockDepth==1);
  CHECK(TestPrmTryUpdate()==STATUS_DEVICE_BUSY && PrmUpdateCount==3);

  RtlZeroMemory(&Request,sizeof(Request));
  Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
  Request.Operation=MtkUfsPowerQuery;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_SUCCESS && PrmCallCount==2 &&
        PrmGetCount==11 && PrmLockCount==8 && PrmQueryHandlerCount==5 &&
        PrmUnlockCount==4 && PrmModuleLockDepth==1);
  RtlZeroMemory(&Request,sizeof(Request));
  Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
  Request.Operation=MtkUfsPowerQuery;
  PrmReenter=TRUE;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_SUCCESS && PrmReenterStatus==STATUS_DEVICE_BUSY &&
        PrmCallCount==3 && PrmLockCount==8 && PrmQueryHandlerCount==5 && PrmUnlockCount==4);
}

static VOID TestPrmUnlockFailureQuarantine(NTSTATUS UnlockFailure,NTSTATUS ExpectedStatus) {
  MTK_UFS_POWER_REQUEST Request;
  ULONG Gets,Locks,Queries,Invokes,Unlocks;
  RtlZeroMemory(&Request,sizeof(Request));
  Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
  Request.Operation=MtkUfsPowerQuery;
  PrmQueryFailure=STATUS_TIMEOUT; PrmUnlockFailure=UnlockFailure;
  CHECK(MtkUfsPrmInvoke(&Request)==ExpectedStatus &&
        mClientState==MtkUfsPrmClientQuarantined && PrmModuleLockDepth==1 &&
        PrmGetCount==1 && PrmLockCount==1 && PrmQueryHandlerCount==1 &&
        PrmCallCount==0 && PrmUnlockCount==1);
  Gets=PrmGetCount; Locks=PrmLockCount; Queries=PrmQueryHandlerCount;
  Invokes=PrmCallCount; Unlocks=PrmUnlockCount;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_DEVICE_NOT_READY &&
        PrmGetCount==Gets && PrmLockCount==Locks && PrmQueryHandlerCount==Queries &&
        PrmCallCount==Invokes && PrmUnlockCount==Unlocks &&
        TestPrmTryUpdate()==STATUS_DEVICE_BUSY);
}

static VOID TestPrmInvokeQuarantine(NTSTATUS InjectedStatus,BOOLEAN EfiFailure) {
  MTK_UFS_POWER_REQUEST Request;
  NTSTATUS ExpectedStatus;
  ULONG Gets,Locks,Queries,Invokes,Unlocks;
  RtlZeroMemory(&Request,sizeof(Request));
  Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
  Request.Operation=MtkUfsPowerPrepareStop;
  Request.Flags=MTK_UFS_POWER_DEVICE_DOWN_ACKED;
  Request.Generation=PrmGeneration;
  PrmInvokeFailure=InjectedStatus; PrmEfiFailure=EfiFailure;
  ExpectedStatus=NT_SUCCESS(InjectedStatus)?STATUS_UNSUCCESSFUL:InjectedStatus;
  CHECK(MtkUfsPrmInvoke(&Request)==ExpectedStatus &&
        PrmPending && PrmState==MtkUfsPowerStopPreparing && PrmGeneration>1 &&
        mClientState==MtkUfsPrmClientQuarantined && PrmModuleLockDepth==1 &&
        PrmLockCount==1 && PrmQueryHandlerCount==1 && PrmUnlockCount==0);
  Gets=PrmGetCount; Locks=PrmLockCount; Queries=PrmQueryHandlerCount;
  Invokes=PrmCallCount; Unlocks=PrmUnlockCount;
  PrmInvokeFailure=STATUS_SUCCESS; PrmEfiFailure=FALSE;
  CHECK(MtkUfsPrmInvoke(&Request)==STATUS_DEVICE_NOT_READY &&
        PrmGetCount==Gets && PrmLockCount==Locks && PrmQueryHandlerCount==Queries &&
        PrmCallCount==Invokes && PrmUnlockCount==Unlocks &&
        TestPrmTryUpdate()==STATUS_DEVICE_BUSY);
}

static VOID TestPowerTransitions(VOID) {
  MTK_UFS_POWER_DEVICE *Device;
  ULONG I,PrepareIndex=MAXULONG,StopIndex=MAXULONG,LowerIndex=MAXULONG,ReleaseIndex=MAXULONG;
  AddFilter(); Device=FilterDevice.DeviceExtension;
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD1)==STATUS_NOT_SUPPORTED && !WorkQueued && !MiniCallCount);
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD2)==STATUS_NOT_SUPPORTED && !WorkQueued && !MiniCallCount);
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_SUCCESS);
  CHECK(Device->PowerState==PowerDeviceD3 && !Device->PowerFaulted &&
        MiniState==MtkUfsMiniPowerHostStopped && MiniFlags==MTK_UFS_POWER_DEVICE_DOWN_ACKED &&
        PrmState==MtkUfsPowerReleased && !MiniHceLive);
  CHECK(PrmLockCount==1 && PrmQueryHandlerCount==1 && PrmUnlockCount==0 &&
        PrmModuleLockDepth==1 && TestPrmTryUpdate()==STATUS_DEVICE_BUSY);
  CHECK(WorkQueued==1 && WorkFreed==0 && !Device->PowerWorkActive &&
        RemoveAcquires==3 && RemoveReleases==3 && CompleteCount==3);
  for(I=0;I<TraceCount;I++) {
    if(Trace[I]==TRACE_MINI_PREPARE) PrepareIndex=I;
    if(Trace[I]==TRACE_MINI_STOP) StopIndex=I;
    if(Trace[I]==TRACE_LOWER_POWER) LowerIndex=I;
    if(Trace[I]==TRACE_PRM_RELEASE) ReleaseIndex=I;
  }
  for(I=0;I<TraceCount;I++) if(Trace[I]==TRACE_PRM_PREPARE) break;
  CHECK(PrepareIndex<I && I<StopIndex && StopIndex<LowerIndex && LowerIndex<ReleaseIndex);
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD0)==STATUS_SUCCESS);
  CHECK(Device->PowerState==PowerDeviceD0 && !Device->PowerFaulted &&
        MiniState==MtkUfsMiniPowerActive && MiniFlags==0 && PrmState==MtkUfsPowerActive && MiniHceLive);
  CHECK(PrmLockCount==1 && PrmQueryHandlerCount==1 && PrmUnlockCount==0 &&
        PrmModuleLockDepth==1 && TestPrmTryUpdate()==STATUS_DEVICE_BUSY);
  CHECK(WorkQueued==2 && WorkFreed==0 && !Device->PowerWorkActive &&
        RemoveAcquires==4 && RemoveReleases==4 && CompleteCount==4);
  CHECK(MiniPrepareCount==1 && MiniStopCount==1 && MiniRestartCount==1 && MiniNopCount==1 &&
        PrmPrepareCount==1 && PrmReleaseCount==1 && PrmRestoreCount==1);
}

static VOID TestFailureBoundaries(VOID) {
  MTK_UFS_POWER_DEVICE *Device;
  AddFilter(); Device=FilterDevice.DeviceExtension;
  LowerPowerStatus=STATUS_UNSUCCESSFUL;
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_UNSUCCESSFUL);
  CHECK(!Device->PowerFaulted && Device->PowerState==PowerDeviceD0 &&
        MiniState==MtkUfsMiniPowerActive && PrmState==MtkUfsPowerActive &&
        MiniRestartCount==1 && PrmRestoreCount==1);

  ResetModel(); AddFilter(); Device=FilterDevice.DeviceExtension;
  LowerPowerStatus=STATUS_TIMEOUT;
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_UNSUCCESSFUL);
  CHECK(!Device->PowerFaulted && Device->PowerState==PowerDeviceD0 &&
        MiniState==MtkUfsMiniPowerActive && PrmState==MtkUfsPowerActive && PrmReleaseCount==0);

  ResetModel(); AddFilter(); Device=FilterDevice.DeviceExtension;
  PrmFailureOperation=MtkUfsPowerPrepareStop;
  CHECK(!NT_SUCCESS(PowerIrp(&FilterDevice,PowerDeviceD3)));
  CHECK(!Device->PowerFaulted && MiniState==MtkUfsMiniPowerActive && PrmState==MtkUfsPowerActive &&
        MiniStopCount==0 && MiniRestartCount==1 && MiniLiveResumeCount==1 &&
        MiniNopCount==0 && MiniHceLive && MiniOperationAccepted);
  CHECK(mClientState==MtkUfsPrmClientPinned && PrmModuleLockDepth==1 &&
        PrmUnlockCount==0 && TestPrmTryUpdate()==STATUS_DEVICE_BUSY);

  ResetModel(); AddFilter(); Device=FilterDevice.DeviceExtension;
  MiniFailureOperation=MtkUfsMiniPowerStopHost;
  CHECK(!NT_SUCCESS(PowerIrp(&FilterDevice,PowerDeviceD3)));
  CHECK(Device->PowerFaulted && MiniState==MtkUfsMiniPowerFault && PrmState==MtkUfsPowerStopPrepared);
  CHECK(mClientState==MtkUfsPrmClientPinned && PrmModuleLockDepth==1 && PrmUnlockCount==0);

  ResetModel(); AddFilter(); Device=FilterDevice.DeviceExtension;
  MiniTimeoutStop=TRUE;
  CHECK(!NT_SUCCESS(PowerIrp(&FilterDevice,PowerDeviceD3)));
  CHECK(Device->PowerFaulted && MiniState==MtkUfsMiniPowerHostStopped &&
        PrmState==MtkUfsPowerStopPrepared && PrmReleaseCount==0 && MiniStopCount==1);

  ResetModel(); AddFilter(); Device=FilterDevice.DeviceExtension;
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_SUCCESS);
  LowerPowerReturnStatus=STATUS_PENDING; LowerPowerStatus=STATUS_TIMEOUT;
  {
    ULONG Calls=MiniCallCount;
    ULONGLONG Generation=MiniGeneration;
    CHECK(PowerIrp(&FilterDevice,PowerDeviceD0)==STATUS_UNSUCCESSFUL);
    CHECK(Device->PowerFaulted && Device->PowerState==PowerDeviceD3 &&
          PrmState==MtkUfsPowerActive && MiniState==MtkUfsMiniPowerHostStopped &&
          MiniFlags==MTK_UFS_POWER_DEVICE_DOWN_ACKED && !MiniRestartCount && MiniHceLive);
    CHECK(PowerIrp(&FilterDevice,PowerDeviceD0)==STATUS_DEVICE_NOT_READY && !MiniRestartCount &&
          MiniCallCount==Calls && MiniGeneration==Generation);
  }
}

static VOID TestPowerWorkAdmission(VOID) {
  IRP First,Second; IO_STACK_LOCATION FirstStack,SecondStack; NTSTATUS Status;
  MTK_UFS_POWER_DEVICE *Device;
  AddFilter(); Device=FilterDevice.DeviceExtension;
  InitIrp(&First,&FirstStack,IRP_MJ_POWER,IRP_MN_SET_POWER);
  FirstStack.Parameters.Power.Type=DevicePowerState;
  FirstStack.Parameters.Power.State.DeviceState=PowerDeviceD3;
  Status=MtkUfsPowerDispatch(&FilterDevice,&First);
  CHECK(Status==STATUS_PENDING && First.PendingReturned && WorkPending && WorkQueued==1 && Device->PowerWorkActive==1);
  InitIrp(&Second,&SecondStack,IRP_MJ_POWER,IRP_MN_SET_POWER);
  SecondStack.Parameters.Power.Type=DevicePowerState;
  SecondStack.Parameters.Power.State.DeviceState=PowerDeviceD0;
  Status=MtkUfsPowerDispatch(&FilterDevice,&Second);
  CHECK(Status==STATUS_DEVICE_BUSY && Second.IoStatus.Status==STATUS_DEVICE_BUSY &&
        WorkPending && WorkQueued==1 && WorkFreed==0 && Device->PowerWorkActive==1 &&
        RemoveAcquires==2 && RemoveReleases==1);
  RunQueuedWork();
  CHECK(First.IoStatus.Status==STATUS_SUCCESS && Device->PowerState==PowerDeviceD3 &&
        !Device->PowerWorkActive && WorkQueued==1 && WorkFreed==0 &&
        RemoveAcquires==2 && RemoveReleases==2 && CompleteCount==2);
}

static VOID TestPowerWorkAllocationFailures(VOID) {
  ResetModel();
  WorkItemAllocationFails=TRUE;
  CHECK(DriverEntry(&DriverObject,NULL)==STATUS_SUCCESS);
  CHECK(DriverExtension.AddDevice(&DriverObject,&PhysicalDevice)==STATUS_INSUFFICIENT_RESOURCES);
  CHECK(Deleted && !Detached && WorkQueued==0U && WorkFreed==0U &&
        MockPoolState==0U);

  ResetModel(); AddFilter();
  MTK_UFS_POWER_DEVICE *Device=FilterDevice.DeviceExtension;
  PoolAllocationFails=TRUE;
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_SUCCESS);
  CHECK(!Device->PowerWorkActive && WorkQueued==1U && WorkFreed==0U &&
        MockPoolState==0U);
}

static VOID TestMalformedEndpointReplies(VOID) {
  MTK_UFS_POWER_DEVICE *Device;
  MTK_UFS_POWER_REQUEST Request;
  ULONG Mode;
  AddFilter(); Device=FilterDevice.DeviceExtension;
  for(Mode=1;Mode<=4;Mode++) {
    RtlZeroMemory(&Request,sizeof(Request));
    Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
    Request.Operation=MtkUfsMiniPowerQuery;
    MalformedMiniResponse=Mode;
    CHECK(MtkUfsMiniportCall(Device,&Request)==STATUS_DATA_ERROR && MalformedMiniResponse==0);
  }
  RtlZeroMemory(&Request,sizeof(Request));
  Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
  Request.Operation=MtkUfsMiniPowerQuery;
  MalformedMiniResponse=5; /* out-of-range state */
  CHECK(MtkUfsMiniQuery(Device,&Request)==STATUS_DATA_ERROR && MalformedMiniResponse==0);
  RtlZeroMemory(&Request,sizeof(Request));
  Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
  Request.Operation=MtkUfsMiniPowerQuery;
  MalformedMiniInformation=1;
  CHECK(MtkUfsMiniportCall(Device,&Request)==STATUS_DATA_ERROR && !MalformedMiniInformation);
  IoctlReturnStatus=STATUS_TIMEOUT;
  CHECK(MtkUfsMiniQuery(Device,&Request)==STATUS_UNSUCCESSFUL && IoctlReturnStatus==STATUS_SUCCESS);
  IoctlCompletionStatus=STATUS_TIMEOUT;
  CHECK(MtkUfsMiniQuery(Device,&Request)==STATUS_UNSUCCESSFUL && IoctlCompletionStatus==STATUS_SUCCESS);
  IoctlReturnStatus=STATUS_PENDING; IoctlCompletionStatus=STATUS_TIMEOUT;
  CHECK(MtkUfsMiniQuery(Device,&Request)==STATUS_UNSUCCESSFUL &&
        IoctlReturnStatus==STATUS_SUCCESS && IoctlCompletionStatus==STATUS_SUCCESS);
  ULONG PreviousFrees=IoctlFrees;
  IoctlReturnStatus=STATUS_PENDING; IoctlTimeout=TRUE;
  CHECK(MtkUfsMiniQuery(Device,&Request)==STATUS_IO_TIMEOUT &&
        IoctlCancels==1 && IoctlFrees==PreviousFrees+1 && !IoctlOwned);
  PreviousFrees=IoctlFrees;
  ULONG PreviousCalls=MiniCallCount;
  IoctlRegistrationStatus=STATUS_INSUFFICIENT_RESOURCES;
  CHECK(MtkUfsMiniQuery(Device,&Request)==STATUS_INSUFFICIENT_RESOURCES &&
        IoctlFrees==PreviousFrees+1 && MiniCallCount==PreviousCalls &&
        !IoctlOwned && !FilterReferences && !LowerReferences);
  IoctlRegistrationStatus=STATUS_SUCCESS;
  CHECK(MtkUfsMiniQuery(Device,&Request)==STATUS_SUCCESS && !IoctlOwned);
}

static VOID TestLateMiniportTimeout(VOID) {
  for(ULONG Remove=0;Remove<2;++Remove) {
    ULONG Calls;
    ResetModel(); AddFilter();
    IoctlReturnStatus=STATUS_PENDING;
    IoctlTimeout=IoctlCompletionDelayed=TRUE;
    CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_IO_TIMEOUT);
    CHECK(IoctlOwned && !IoctlDone && !IoctlFrees && IoctlCancels==1);
    CHECK(FilterReferences==1 && LowerReferences==1);
    CHECK(RemoveAcquires==RemoveReleases && !WorkPending);
    if(!IoctlOwned) return;

    Calls=MiniCallCount;
    CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    CHECK(MiniCallCount==Calls && IoctlOwned && !IoctlDone && IoctlCancels==1);
    if(Remove) {
      IRP Irp;
      IO_STACK_LOCATION Stack;
      InitIrp(&Irp,&Stack,IRP_MJ_PNP,IRP_MN_REMOVE_DEVICE);
      CHECK(MtkUfsPowerDispatch(&FilterDevice,&Irp)==STATUS_SUCCESS &&
            Detached && Deleted && IoctlOwned && FilterReferences==1 && LowerReferences==1);
    }
    IoctlIrp.IoStatus.Status=STATUS_CANCELLED;
    CurrentIrql=DISPATCH_LEVEL;
    CompleteMiniportIrp();
    CurrentIrql=PASSIVE_LEVEL;
    CHECK(!IoctlOwned && IoctlFrees==1 && !FilterReferences && !LowerReferences);
    if(!Remove) {
      CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_SUCCESS);
      CHECK(!IoctlOwned && MiniCallCount>Calls && IoctlCancels==1);
    }
  }
}

static NTSTATUS SendUsageNotification(DEVICE_USAGE_NOTIFICATION_TYPE Type,BOOLEAN InPath) {
  IRP Irp; IO_STACK_LOCATION Stack;
  InitIrp(&Irp,&Stack,IRP_MJ_PNP,IRP_MN_DEVICE_USAGE_NOTIFICATION);
  Stack.Parameters.UsageNotification.Type=Type;
  Stack.Parameters.UsageNotification.InPath=InPath;
  return MtkUfsPowerDispatch(&FilterDevice,&Irp);
}

static VOID TestUsageNotificationPowerFlags(VOID) {
  MTK_UFS_POWER_DEVICE *Device;
  ResetModel();
  LowerDevice.Flags=DO_POWER_PAGABLE;
  AddFilter(); Device=FilterDevice.DeviceExtension;
  CHECK(Device->LowerPowerPagable && (FilterDevice.Flags&DO_POWER_PAGABLE));
  UsageReturnStatus=STATUS_PENDING;UsageModifyLowerFlags=TRUE;
  CHECK(SendUsageNotification(DeviceUsageTypePaging,TRUE)==STATUS_SUCCESS);
  CHECK(Device->UsageCount[0]==1U && UsageForwardCount==1U &&
        !(FilterDevice.Flags&DO_POWER_PAGABLE) && !(LowerDevice.Flags&DO_POWER_PAGABLE));
  CHECK(SendUsageNotification(DeviceUsageTypeHibernation,TRUE)==STATUS_SUCCESS);
  CHECK(Device->UsageCount[1]==1U && UsageForwardCount==2U && !(FilterDevice.Flags&DO_POWER_PAGABLE));
  CHECK(SendUsageNotification(DeviceUsageTypePaging,FALSE)==STATUS_SUCCESS);
  CHECK(Device->UsageCount[0]==0U && Device->UsageCount[1]==1U && !(FilterDevice.Flags&DO_POWER_PAGABLE));
  CHECK(SendUsageNotification(DeviceUsageTypeHibernation,FALSE)==STATUS_SUCCESS);
  CHECK(Device->UsageCount[1]==0U && Device->UsageCount[2]==0U &&
        (FilterDevice.Flags&DO_POWER_PAGABLE) && (LowerDevice.Flags&DO_POWER_PAGABLE) &&
        UsageSawLastRemovalPageable);

  ResetModel();LowerDevice.Flags=DO_POWER_PAGABLE;AddFilter();Device=FilterDevice.DeviceExtension;
  UsageCompletionStatus=STATUS_UNSUCCESSFUL;UsageModifyLowerFlags=TRUE;
  CHECK(SendUsageNotification(DeviceUsageTypeDumpFile,TRUE)==STATUS_UNSUCCESSFUL);
  CHECK(Device->UsageCount[2]==0U && (FilterDevice.Flags&DO_POWER_PAGABLE) &&
        (LowerDevice.Flags&DO_POWER_PAGABLE));

  UsageCompletionStatus=STATUS_SUCCESS;UsageReturnStatus=STATUS_SUCCESS;UsageModifyLowerFlags=TRUE;
  CHECK(SendUsageNotification(DeviceUsageTypePaging,TRUE)==STATUS_SUCCESS);
  CHECK(!(FilterDevice.Flags&DO_POWER_PAGABLE) && Device->UsageCount[0]==1U);
  UsageCompletionStatus=STATUS_UNSUCCESSFUL;UsageModifyLowerFlags=FALSE;
  UsageSawLastRemovalPageable=FALSE;
  CHECK(SendUsageNotification(DeviceUsageTypePaging,FALSE)==STATUS_UNSUCCESSFUL);
  CHECK(UsageSawLastRemovalPageable && Device->UsageCount[0]==1U &&
        !(FilterDevice.Flags&DO_POWER_PAGABLE));

  ResetModel();LowerDevice.Flags=DO_POWER_PAGABLE;AddFilter();Device=FilterDevice.DeviceExtension;
  UsageModifyLowerFlags=FALSE;
  CHECK(SendUsageNotification(DeviceUsageTypePaging,TRUE)==STATUS_SUCCESS);
  LowerDevice.Flags&=~DO_POWER_PAGABLE;
  CHECK(SendUsageNotification(DeviceUsageTypePaging,FALSE)==STATUS_SUCCESS);
  CHECK(!Device->LowerPowerPagable && !(FilterDevice.Flags&DO_POWER_PAGABLE));

  ULONG ForwardedBefore=UsageForwardCount;
  CHECK(SendUsageNotification(DeviceUsageTypeBoot,TRUE)==STATUS_SUCCESS);
  CHECK(UsageForwardCount==ForwardedBefore+1U && Device->UsageCount[0]==0U &&
        Device->UsageCount[1]==0U && Device->UsageCount[2]==0U);
  ResetModel();
}

static VOID TestPnpLifecycle(VOID) {
  IRP PowerRequest,Irp; IO_STACK_LOCATION PowerStack,Stack; NTSTATUS Status;
  TestUsageNotificationPowerFlags();
  HardwareIdValid=FALSE;
  CHECK(DriverEntry(&DriverObject,NULL)==STATUS_SUCCESS);
  CHECK(DriverExtension.AddDevice(&DriverObject,&PhysicalDevice)==STATUS_NO_SUCH_DEVICE && !FilterDevice.DeviceExtension);
  HardwareIdValid=TRUE; LowerDevice.Flags=DO_POWER_INRUSH; AddFilter();
  CHECK((FilterDevice.Flags&(DO_POWER_INRUSH|DO_POWER_PAGABLE))==DO_POWER_INRUSH);
  InitIrp(&PowerRequest,&PowerStack,IRP_MJ_POWER,IRP_MN_SET_POWER);
  PowerStack.Parameters.Power.Type=DevicePowerState;
  PowerStack.Parameters.Power.State.DeviceState=PowerDeviceD3;
  CHECK(MtkUfsPowerDispatch(&FilterDevice,&PowerRequest)==STATUS_PENDING && WorkPending);
  InitIrp(&Irp,&Stack,IRP_MJ_PNP,IRP_MN_SURPRISE_REMOVAL);
  Status=MtkUfsPowerDispatch(&FilterDevice,&Irp);
  CHECK(Status==STATUS_SUCCESS && ((MTK_UFS_POWER_DEVICE *)FilterDevice.DeviceExtension)->Removing &&
        RemoveAcquires==2 && RemoveReleases==1);
  WorkerIrp=&PowerRequest;
  RunQueuedWork();
  CHECK(PowerRequest.IoStatus.Status==STATUS_SUCCESS && WorkerCompleted && !MiniCallCount && !WorkPending &&
        WorkQueued==1 && WorkFreed==0 && RemoveAcquires==2 && RemoveReleases==2 && CompleteCount==1);
  WorkerIrp=NULL;
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_SUCCESS &&
        WorkQueued==1 && WorkFreed==0 && !WorkPending && !MiniCallCount && CompleteCount==1 &&
        RemoveAcquires==3 && RemoveReleases==3 && TraceCount==2);
  InitIrp(&Irp,&Stack,IRP_MJ_PNP,IRP_MN_REMOVE_DEVICE);
  RemovePending=TRUE;
  Status=MtkUfsPowerDispatch(&FilterDevice,&Irp);
  CHECK(Status==STATUS_PENDING && Detached && Deleted && WorkFreed==1U &&
        RemoveAcquires==RemoveReleases);

  ResetModel(); AddFilter();
  CHECK(PowerIrp(&FilterDevice,PowerDeviceD3)==STATUS_SUCCESS &&
        mClientState==MtkUfsPrmClientPinned && PrmModuleLockDepth==1 && PrmUnlockCount==0);
  CHECK(TestPrmTryUpdate()==STATUS_DEVICE_BUSY);
  InitIrp(&Irp,&Stack,IRP_MJ_PNP,IRP_MN_SURPRISE_REMOVAL);
  Status=MtkUfsPowerDispatch(&FilterDevice,&Irp);
  CHECK(Status==STATUS_SUCCESS && ((MTK_UFS_POWER_DEVICE *)FilterDevice.DeviceExtension)->Removing &&
        mClientState==MtkUfsPrmClientPinned && PrmModuleLockDepth==1 && PrmUnlockCount==0);
  InitIrp(&Irp,&Stack,IRP_MJ_PNP,IRP_MN_REMOVE_DEVICE);
  Status=MtkUfsPowerDispatch(&FilterDevice,&Irp);
  CHECK(Status==STATUS_SUCCESS && Detached && Deleted && mClientState==MtkUfsPrmClientPinned &&
        PrmModuleLockDepth==1 && PrmUnlockCount==0 && TestPrmTryUpdate()==STATUS_DEVICE_BUSY);
}

ULONG TestMain(VOID) {
  MtkUfsPowerAssertionCount=0; Checks=0; FailureLine=0;
  ResetModel(); TestPrmClient();
  ResetModel(); TestPowerTransitions();
  ResetModel(); TestFailureBoundaries();
  ResetModel(); TestPowerWorkAdmission();
  ResetModel(); TestPowerWorkAllocationFailures();
  ResetModel(); TestMalformedEndpointReplies();
  TestLateMiniportTimeout();
  ResetModel(); TestPnpLifecycle();
  ResetModel(); TestPrmUnlockFailureQuarantine(STATUS_PENDING,STATUS_UNSUCCESSFUL);
  ResetModel(); TestPrmUnlockFailureQuarantine(STATUS_DEVICE_BUSY,STATUS_DEVICE_BUSY);
  ResetModel(); TestPrmInvokeQuarantine(STATUS_DEVICE_BUSY,FALSE);
  ResetModel(); TestPrmInvokeQuarantine(STATUS_RETRY,FALSE);
  ResetModel(); TestPrmInvokeQuarantine(STATUS_TIMEOUT,FALSE);
  ResetModel(); TestPrmInvokeQuarantine(STATUS_PENDING,FALSE);
  ResetModel(); TestPrmInvokeQuarantine(STATUS_SUCCESS,TRUE);
  return FailureLine;
}

static NTSTATUS TestIoCreateDevice(PDRIVER_OBJECT Driver,ULONG ExtensionSize,PUNICODE_STRING Name,
                                   DEVICE_TYPE Type,ULONG Characteristics,BOOLEAN Exclusive,
                                   PDEVICE_OBJECT *DeviceObject) {
  CHECK(Driver==&DriverObject && ExtensionSize<=sizeof(DeviceExtensionBuffer) &&
        Name==NULL && Type==PhysicalDevice.DeviceType && (Characteristics&FILE_DEVICE_SECURE_OPEN) && !Exclusive);
  RtlZeroMemory(&FilterDevice,sizeof(FilterDevice));
  RtlZeroMemory(DeviceExtensionBuffer,sizeof(DeviceExtensionBuffer));
  FilterDevice.DeviceType=Type; FilterDevice.DeviceExtension=DeviceExtensionBuffer;
  *DeviceObject=&FilterDevice; return STATUS_SUCCESS;
}

static NTSTATUS TestIoAttachDeviceToDeviceStackSafe(PDEVICE_OBJECT Source,PDEVICE_OBJECT Target,
                                                    PDEVICE_OBJECT *AttachedTo) {
  CHECK(Source==&FilterDevice && Target==&PhysicalDevice);
  *AttachedTo=&LowerDevice; return STATUS_SUCCESS;
}

static NTSTATUS TestIoGetDeviceProperty(PDEVICE_OBJECT Pdo,DEVICE_REGISTRY_PROPERTY Property,
                                        ULONG Length,PVOID Buffer,PULONG Bytes) {
  static const WCHAR ValidId[]=L"ACPI\\HANA678B\0";
  static const WCHAR OtherId[]=L"ACPI\\OTHER0000\0";
  const WCHAR *Id=HardwareIdValid?ValidId:OtherId;
  ULONG Size=(ULONG)(HardwareIdValid?sizeof(ValidId):sizeof(OtherId));
  CHECK(Pdo==&PhysicalDevice && Property==DevicePropertyHardwareID && Length>=Size);
  RtlCopyMemory(Buffer,Id,Size); *Bytes=Size; return STATUS_SUCCESS;
}

static VOID TestIoDeleteDevice(PDEVICE_OBJECT Device) {
  MTK_UFS_POWER_DEVICE *Extension=Device?Device->DeviceExtension:NULL;
  CHECK(Device==&FilterDevice && CurrentIrql==PASSIVE_LEVEL &&
        (Detached || (Extension && Extension->Lower==NULL)));
  Deleted=TRUE;
}
static VOID TestIoDetachDevice(PDEVICE_OBJECT Lower) {
  CHECK(Lower==&LowerDevice && CurrentIrql==PASSIVE_LEVEL && RemoveForwarded);
  CHECK(!SavedCompletion || (IoctlOwned && !IoctlDone));
  Detached=TRUE;
}
static NTSTATUS TestIoAcquireRemoveLock(PIO_REMOVE_LOCK Lock,PVOID Tag) {
  UNREFERENCED_PARAMETER(Lock); UNREFERENCED_PARAMETER(Tag); RemoveAcquires++; return STATUS_SUCCESS;
}
static VOID TestIoInitializeRemoveLock(PIO_REMOVE_LOCK Lock,ULONG Tag,ULONG MaxLockedMinutes,ULONG HighWatermark) {
  CHECK(Lock!=NULL && Tag==UFS_POWER_TAG && !MaxLockedMinutes && !HighWatermark);
}
static VOID TestIoReleaseRemoveLock(PIO_REMOVE_LOCK Lock,PVOID Tag) {
  UNREFERENCED_PARAMETER(Lock);
  if(Tag==WorkerIrp) CHECK(WorkerCompleted);
  RemoveReleases++;
}
static VOID TestIoReleaseRemoveLockAndWait(PIO_REMOVE_LOCK Lock,PVOID Tag) {
  UNREFERENCED_PARAMETER(Lock); UNREFERENCED_PARAMETER(Tag); RemoveReleases++; RemoveAcquires=RemoveReleases;
}
static PVOID TestExAllocatePool2(POOL_FLAGS Flags,SIZE_T Bytes,ULONG Tag) {
  CHECK(Flags==POOL_FLAG_NON_PAGED && Bytes<=sizeof(MockPool) && Tag==UFS_POWER_TAG);
  if(PoolAllocationFails) return NULL;
  if(MockPoolState) return NULL; MockPoolState=1; RtlZeroMemory(MockPool,sizeof(MockPool)); return MockPool;
}
static VOID TestExFreePoolWithTag(PVOID Pool,ULONG Tag) {
  CHECK(Pool==MockPool && Tag==UFS_POWER_TAG); MockPoolState=0;
}
static PIO_WORKITEM TestIoAllocateWorkItem(PDEVICE_OBJECT Device) {
  CHECK(Device==&FilterDevice);
  if(WorkItemAllocationFails) return NULL;
  return (PIO_WORKITEM)DummyWorkItemBuffer;
}
static VOID TestIoFreeWorkItem(PIO_WORKITEM WorkItem) { CHECK(WorkItem==(PIO_WORKITEM)DummyWorkItemBuffer); WorkFreed++; }
static VOID TestIoQueueWorkItem(PIO_WORKITEM WorkItem,PIO_WORKITEM_ROUTINE Routine,
                               WORK_QUEUE_TYPE QueueType,PVOID Context) {
  CHECK(WorkItem==(PIO_WORKITEM)DummyWorkItemBuffer && Routine==MtkUfsPowerWorker && QueueType==CriticalWorkQueue && Context==FilterDevice.DeviceExtension && !WorkPending);
  WorkPending=TRUE; WorkQueued++; QueuedWorkRoutine=Routine; QueuedWorkContext=Context;
  QueuedWorkDevice=&FilterDevice;
}
static PIRP TestIoAllocateIrp(CCHAR StackSize,BOOLEAN ChargeQuota) {
  CHECK(StackSize==LowerDevice.StackSize && StackSize==1 && !ChargeQuota && !IoctlOwned);
  RtlZeroMemory(&IoctlIrp,sizeof(IoctlIrp)); RtlZeroMemory(&IoctlStack,sizeof(IoctlStack));
  IoctlIrp.Tail.Overlay.CurrentStackLocation=&IoctlStack+1;
  IoctlOwned=TRUE; IoctlDone=IoctlDispatched=FALSE;
  return &IoctlIrp;
}

static VOID TestIoFreeIrp(PIRP Irp) {
  CHECK(Irp==&IoctlIrp && IoctlOwned && (IoctlDone || !IoctlDispatched) && !SavedCompletion);
  IoctlOwned=FALSE;
  IoctlFrees++;
}

static VOID TestIoSkipCurrentIrpStackLocation(PIRP Irp) {
  CHECK(TestGetStack(Irp)->MajorFunction==IRP_MJ_PNP &&
        TestGetStack(Irp)->MinorFunction==IRP_MN_REMOVE_DEVICE);
  CHECK(!SavedCompletion || (IoctlOwned && !IoctlDone));
}

static VOID SetMiniResponse(UFS_POWER_WIRE_BUFFER *Wire,MTK_UFS_POWER_REQUEST *Request,ULONG Result) {
  Request->Result=Result; Request->State=MiniState; Request->Generation=MiniGeneration;
  Request->Flags=MiniFlags; Request->Capabilities=MTK_UFS_POWER_CAP_PROGRESS;
  Wire->Header.ReturnCode=Result;
}

static VOID ProcessMiniRequest(UFS_POWER_WIRE_BUFFER *Wire) {
  MTK_UFS_POWER_REQUEST Request;
  ULONG Result=MtkUfsPowerSuccess;
  RtlCopyMemory(&Request,Wire->RequestBytes,sizeof(Request));
  MiniCallCount++; Trace[TraceCount++]=10+Request.Operation;
  if(Request.Operation==MtkUfsMiniPowerQuery) {
    if(Request.Generation) Result=MtkUfsPowerInvalid;
    else if(MiniFailureOperation==MtkUfsMiniPowerQuery) Result=MtkUfsPowerIoError;
    SetMiniResponse(Wire,&Request,Result);
  } else if(Request.Operation==MtkUfsMiniPowerPrepareDevice ||
            Request.Operation==MtkUfsMiniPowerStopHost ||
            Request.Operation==MtkUfsMiniPowerRestartHost) {
    if(Request.Operation==MiniFailureOperation) {
      MiniPending=FALSE; MiniState=MtkUfsMiniPowerFault;
      Request.Result=MtkUfsPowerIoError; Request.State=MtkUfsMiniPowerFault;
      Request.Generation=MiniGeneration; Request.Flags=MiniFlags;
      Request.Capabilities=MTK_UFS_POWER_CAP_PROGRESS; Wire->Header.ReturnCode=Request.Result;
    } else if(MiniPending && Request.Operation==MiniPendingOperation &&
              Request.Generation==MiniGeneration) {
      MiniPending=FALSE;
      if(Request.Operation==MtkUfsMiniPowerPrepareDevice) {
        MiniState=MtkUfsMiniPowerDevicePrepared; MiniFlags=MTK_UFS_POWER_DEVICE_DOWN_ACKED;
        MiniPrepareCount++; Trace[TraceCount++]=TRACE_MINI_PREPARE;
      } else if(Request.Operation==MtkUfsMiniPowerStopHost) {
        MiniState=MtkUfsMiniPowerHostStopped; MiniHceLive=FALSE;
        MiniStopCount++; Trace[TraceCount++]=TRACE_MINI_STOP;
        if(MiniTimeoutStop) { MiniTimeoutStop=FALSE; IoctlCompletionStatus=STATUS_TIMEOUT; }
      } else {
        MiniState=MtkUfsMiniPowerActive; MiniFlags=0; MiniHceLive=TRUE;
        MiniRestartCount++; Trace[TraceCount++]=TRACE_MINI_RESTART;
      }
      SetMiniResponse(Wire,&Request,MtkUfsPowerSuccess);
    } else if(!MiniPending && Request.Generation==MiniGeneration) {
      if((Request.Operation==MtkUfsMiniPowerPrepareDevice && MiniState!=MtkUfsMiniPowerActive) ||
         (Request.Operation==MtkUfsMiniPowerStopHost &&
          (MiniState!=MtkUfsMiniPowerDevicePrepared || MiniFlags!=MTK_UFS_POWER_DEVICE_DOWN_ACKED)) ||
         (Request.Operation==MtkUfsMiniPowerRestartHost &&
          !((MiniState==MtkUfsMiniPowerHostStopped && MiniHceLive &&
             PrmState==MtkUfsPowerActive && MiniFlags==MTK_UFS_POWER_DEVICE_DOWN_ACKED) ||
            (MiniState==MtkUfsMiniPowerDevicePrepared && MiniHceLive &&
             PrmState==MtkUfsPowerActive && MiniFlags==MTK_UFS_POWER_DEVICE_DOWN_ACKED)))) {
        SetMiniResponse(Wire,&Request,MtkUfsPowerWrongState);
      } else {
        MiniPending=TRUE; MiniPendingOperation=Request.Operation; MiniOperationAccepted=TRUE;
        MiniPendingLiveResume=(BOOLEAN)(Request.Operation==MtkUfsMiniPowerRestartHost &&
          MiniState==MtkUfsMiniPowerDevicePrepared && MiniHceLive);
        if(Request.Operation==MtkUfsMiniPowerRestartHost) {
          if(MiniPendingLiveResume) MiniLiveResumeCount++; else MiniNopCount++;
        }
        MiniGeneration++;
        MiniState=Request.Operation==MtkUfsMiniPowerPrepareDevice?MtkUfsMiniPowerDevicePreparing:
          (Request.Operation==MtkUfsMiniPowerStopHost?MtkUfsMiniPowerHostStopping:MtkUfsMiniPowerHostRestarting);
        Request.Generation=MiniGeneration;
        Request.State=MiniState; Request.Flags=MiniFlags;
        Request.Capabilities=MTK_UFS_POWER_CAP_PROGRESS; Request.Result=MtkUfsPowerInProgress;
        Wire->Header.ReturnCode=Request.Result;
      }
    } else {
      SetMiniResponse(Wire,&Request,Request.Generation!=MiniGeneration?
        MtkUfsPowerStaleGeneration:MtkUfsPowerBusy);
    }
  } else {
    SetMiniResponse(Wire,&Request,MtkUfsPowerUnsupported);
  }
  if(MalformedMiniResponse) {
    switch(MalformedMiniResponse) {
    case 1: Wire->Header.ReturnCode^=1; break;
    case 2: Request.Operation++; RtlCopyMemory(Wire->RequestBytes,&Request,sizeof(Request)); break;
    case 3: Request.Reserved[0]=1; RtlCopyMemory(Wire->RequestBytes,&Request,sizeof(Request)); break;
    case 4: Request.Flags|=0x80000000U; RtlCopyMemory(Wire->RequestBytes,&Request,sizeof(Request)); break;
    case 5: Request.State=0; RtlCopyMemory(Wire->RequestBytes,&Request,sizeof(Request)); break;
    default: break;
    }
    MalformedMiniResponse=0;
  } else RtlCopyMemory(Wire->RequestBytes,&Request,sizeof(Request));
}

static VOID CompleteMiniportIrp(VOID) {
  PIO_COMPLETION_ROUTINE Routine=SavedCompletion;
  PVOID Context=SavedCompletionContext;
  CHECK(IoctlOwned && !IoctlDone && IoctlDispatched && Routine==MtkUfsMiniportComplete);
  SavedCompletion=NULL; SavedCompletionContext=NULL;
  IoctlDone=TRUE;
  if(Routine) CHECK(Routine(&LowerDevice,&IoctlIrp,Context)==STATUS_MORE_PROCESSING_REQUIRED);
}

static NTSTATUS TestIoCallDriver(PDEVICE_OBJECT Device,PIRP Irp) {
  CHECK(Device==&LowerDevice);
  if(Irp==&IoctlIrp) {
    UFS_POWER_WIRE_BUFFER *Wire=Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS ReturnStatus=IoctlReturnStatus;
    MTK_UFS_POWER_DEVICE *Extension=FilterDevice.DeviceExtension;
    CHECK(IoctlOwned && !IoctlDispatched && Wire==&Extension->MiniportBuffer &&
          Extension->MiniportIrp==Irp && FilterReferences>=1 && LowerReferences>=1 &&
          !Irp->Tail.Overlay.Thread && Irp->Flags==IRP_BUFFERED_IO);
    IoctlDispatched=TRUE;
    CHECK(IoctlStack.MajorFunction==IRP_MJ_DEVICE_CONTROL &&
          IoctlStack.Parameters.DeviceIoControl.IoControlCode==IOCTL_SCSI_MINIPORT &&
          IoctlStack.Parameters.DeviceIoControl.InputBufferLength==sizeof(*Wire) &&
          IoctlStack.Parameters.DeviceIoControl.OutputBufferLength==sizeof(*Wire));
    ProcessMiniRequest(Wire);
    Irp->IoStatus.Status=IoctlCompletionStatus; Irp->IoStatus.Information=sizeof(*Wire);
    if(MalformedMiniInformation) {
      Irp->IoStatus.Information--; MalformedMiniInformation=0;
    }
    IoctlReturnStatus=IoctlCompletionStatus=STATUS_SUCCESS;
    if(ReturnStatus!=STATUS_PENDING) CompleteMiniportIrp();
    return ReturnStatus;
  }
  if(TestGetStack(Irp)->MinorFunction==IRP_MN_REMOVE_DEVICE) {
    CHECK(CurrentIrql==PASSIVE_LEVEL && !Detached && !Deleted);
    CHECK(!SavedCompletion || (IoctlOwned && !IoctlDone));
    RemoveForwarded=TRUE;
    return RemovePending?STATUS_PENDING:STATUS_SUCCESS;
  }
  if(TestGetStack(Irp)->MajorFunction==IRP_MJ_PNP &&
     TestGetStack(Irp)->MinorFunction==IRP_MN_DEVICE_USAGE_NOTIFICATION) {
    PIO_STACK_LOCATION Stack=TestGetStack(Irp);
    CHECK(CurrentIrql==PASSIVE_LEVEL && SavedCompletion!=NULL && !Deleted);
    CHECK((Stack->Parameters.UsageNotification.Type>=DeviceUsageTypePaging &&
           Stack->Parameters.UsageNotification.Type<=DeviceUsageTypeDumpFile) ||
          Stack->Parameters.UsageNotification.Type==DeviceUsageTypeBoot);
    if(!Stack->Parameters.UsageNotification.InPath &&
       MtkUfsUsageTotal((MTK_UFS_POWER_DEVICE *)FilterDevice.DeviceExtension)==1U &&
       ((MTK_UFS_POWER_DEVICE *)FilterDevice.DeviceExtension)->LowerPowerPagable) {
      CHECK((FilterDevice.Flags&DO_POWER_PAGABLE)!=0);
      UsageSawLastRemovalPageable=TRUE;
    }
    if(NT_SUCCESS(UsageCompletionStatus) && UsageModifyLowerFlags) {
      if(Stack->Parameters.UsageNotification.InPath) LowerDevice.Flags&=~DO_POWER_PAGABLE;
      else LowerDevice.Flags|=DO_POWER_PAGABLE;
    }
    Irp->IoStatus.Status=UsageCompletionStatus;
    if(UsageReturnStatus==STATUS_PENDING) Irp->PendingReturned=TRUE;
    PIO_COMPLETION_ROUTINE Routine=SavedCompletion;PVOID Context=SavedCompletionContext;
    SavedCompletion=NULL;SavedCompletionContext=NULL;
    NTSTATUS CompletionStatus=Routine(Device,Irp,Context);
    CHECK(CompletionStatus==(Stack->Parameters.UsageNotification.Type==DeviceUsageTypeBoot?
          STATUS_CONTINUE_COMPLETION:STATUS_MORE_PROCESSING_REQUIRED));
    UsageForwardCount++;
    return UsageReturnStatus;
  }
  Irp->IoStatus.Status=STATUS_SUCCESS;
  if(SavedCompletion) {
    PIO_COMPLETION_ROUTINE Routine=SavedCompletion; PVOID Context=SavedCompletionContext;
    SavedCompletion=NULL; SavedCompletionContext=NULL;
    (void)Routine(Device,Irp,Context);
  }
  return STATUS_SUCCESS;
}
static BOOLEAN TestIoCancelIrp(PIRP Irp) {
  CHECK(Irp==&IoctlIrp && IoctlOwned);
  IoctlCancels++;
  return FALSE;
}

static NTSTATUS TestPoCallDriver(PDEVICE_OBJECT Device,PIRP Irp) {
  CHECK(Device==&LowerDevice);
  Trace[TraceCount++]=TRACE_LOWER_POWER;
  Irp->IoStatus.Status=LowerPowerStatus;
  if(SavedCompletion) {
    PIO_COMPLETION_ROUTINE Routine=SavedCompletion; PVOID Context=SavedCompletionContext;
    SavedCompletion=NULL; SavedCompletionContext=NULL;
    (void)Routine(Device,Irp,Context);
  }
  return LowerPowerReturnStatus;
}

static VOID TestIoCopyCurrentIrpStackLocationToNext(PIRP Irp) { CHECK(Irp!=NULL); }
static VOID TestIoSetCompletionRoutine(PIRP Irp,PIO_COMPLETION_ROUTINE Routine,PVOID Context,
                                       BOOLEAN Success,BOOLEAN Error,BOOLEAN Cancel) {
  CHECK(Irp && Routine && Success && Error && Cancel && !SavedCompletion);
  SavedCompletion=Routine; SavedCompletionContext=Context;
}
static NTSTATUS TestIoSetCompletionRoutineEx(PDEVICE_OBJECT Owner,PIRP Irp,PIO_COMPLETION_ROUTINE Routine,
                                             PVOID Context,BOOLEAN Success,BOOLEAN Error,BOOLEAN Cancel) {
  CHECK(Owner==&FilterDevice && Irp==&IoctlIrp && Routine==MtkUfsMiniportComplete &&
        Context==FilterDevice.DeviceExtension);
  if(IoctlRegistrationStatus==STATUS_SUCCESS)
    TestIoSetCompletionRoutine(Irp,Routine,Context,Success,Error,Cancel);
  return IoctlRegistrationStatus;
}
static VOID TestObReference(PVOID Object) {
  CHECK(Object==&FilterDevice || Object==&LowerDevice);
  if(Object==&FilterDevice) FilterReferences++;
  if(Object==&LowerDevice) LowerReferences++;
}
static VOID TestObDereference(PVOID Object) {
  CHECK(Object==&FilterDevice || Object==&LowerDevice);
  if(Object==&FilterDevice) { CHECK(FilterReferences!=0); FilterReferences--; }
  if(Object==&LowerDevice) { CHECK(LowerReferences!=0); LowerReferences--; }
}
static ULONG TestKeGetCurrentIrql(VOID) { return (ULONG)CurrentIrql; }
static VOID TestKeInitializeSpinLock(PKSPIN_LOCK Lock) { CHECK(Lock!=NULL && !MiniportLockHeld); }
static VOID TestKeAcquireSpinLock(PKSPIN_LOCK Lock,PKIRQL Irql) {
  CHECK(Lock!=NULL && Irql!=NULL && !MiniportLockHeld && CurrentIrql<=DISPATCH_LEVEL);
  *Irql=(KIRQL)CurrentIrql; CurrentIrql=DISPATCH_LEVEL; MiniportLockHeld=TRUE;
}
static VOID TestKeReleaseSpinLock(PKSPIN_LOCK Lock,KIRQL Irql) {
  CHECK(Lock!=NULL && MiniportLockHeld && CurrentIrql==DISPATCH_LEVEL);
  MiniportLockHeld=FALSE; CurrentIrql=Irql;
}
static VOID TestKeInitializeEvent(PKEVENT Event,EVENT_TYPE Type,BOOLEAN State) {
  UNREFERENCED_PARAMETER(Type); Event->Header.SignalState=State;
}
static VOID TestKeClearEvent(PKEVENT Event) { Event->Header.SignalState=0; }
static LONG TestKeReadStateEvent(PKEVENT Event) { return Event->Header.SignalState; }
static VOID TestKeInitializeMutex(PRKMUTEX Mutex,ULONG Level) {
  CHECK(Mutex!=NULL && Level==0);
}
static LONG TestKeReleaseMutex(PRKMUTEX Mutex,BOOLEAN Wait) {
  CHECK(Mutex!=NULL && !Wait);return 0;
}
static LONG TestKeSetEvent(PRKEVENT Event,KPRIORITY Increment,BOOLEAN Wait) {
  UNREFERENCED_PARAMETER(Increment); CHECK(!Wait);
  Event->Header.SignalState=1;
  return 1;
}
static NTSTATUS TestKeWaitForSingleObject(PVOID Object,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,
                                          BOOLEAN Alertable,PLARGE_INTEGER Timeout) {
  CHECK(Object && Reason==Executive && Mode==KernelMode && !Alertable && CurrentIrql==PASSIVE_LEVEL);
  if(IoctlOwned && !IoctlDone &&
     Object==&((MTK_UFS_POWER_DEVICE *)FilterDevice.DeviceExtension)->MiniportDone) {
    CHECK(Timeout && Timeout->QuadPart<0);
    if(IoctlTimeout) {
      IoctlTimeout=FALSE;
      if(!IoctlCompletionDelayed) CompleteMiniportIrp();
      return STATUS_TIMEOUT;
    }
    CompleteMiniportIrp();
  }
  return STATUS_SUCCESS;
}
static NTSTATUS TestKeDelayExecutionThread(KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Interval) {
  UNREFERENCED_PARAMETER(Mode); UNREFERENCED_PARAMETER(Alertable);
  if(Interval && Interval->QuadPart<0) FakeTime+=(ULONGLONG)(-Interval->QuadPart);
  return STATUS_SUCCESS;
}
static ULONGLONG TestKeQueryInterruptTime(VOID) { return FakeTime; }
static VOID TestIoCompleteRequest(PIRP Irp,CCHAR PriorityBoost) {
  UNREFERENCED_PARAMETER(PriorityBoost); CHECK(Irp!=NULL); CompleteCount++;
  if(Irp==WorkerIrp) WorkerCompleted=TRUE;
}
static VOID TestIoMarkIrpPending(PIRP Irp) { CHECK(Irp!=NULL); Irp->PendingReturned=TRUE; }
static POWER_STATE TestPoSetPowerState(PDEVICE_OBJECT Device,POWER_STATE_TYPE Type,POWER_STATE State) {
  POWER_STATE Previous; UNREFERENCED_PARAMETER(Device); UNREFERENCED_PARAMETER(Type); Previous.DeviceState=PowerDeviceD0;
  UNREFERENCED_PARAMETER(State); return Previous;
}

static NTSTATUS TestPrmLock(LPGUID Guid) {
  UNREFERENCED_PARAMETER(Guid); Trace[TraceCount++]=1; PrmLockCount++;
  if(PrmLockFailure!=STATUS_SUCCESS) return PrmLockFailure;
  if(PrmModuleLockDepth!=0) return STATUS_DEVICE_BUSY;
  PrmModuleLockDepth=1; return STATUS_SUCCESS;
}
static NTSTATUS TestPrmUnlock(LPGUID Guid) {
  UNREFERENCED_PARAMETER(Guid); Trace[TraceCount++]=4; PrmUnlockCount++;
  if(PrmUnlockFailure!=STATUS_SUCCESS) return PrmUnlockFailure;
  if(PrmModuleLockDepth!=1) return STATUS_INVALID_DEVICE_STATE;
  PrmModuleLockDepth=0; return STATUS_SUCCESS;
}
static NTSTATUS TestPrmQueryHandler(LPGUID Guid,PBOOLEAN Found) {
  UNREFERENCED_PARAMETER(Guid); Trace[TraceCount++]=2; PrmQueryHandlerCount++;
  *Found=PrmFound; return PrmQueryFailure;
}
static NTSTATUS TestPrmInvokeHandler(LPGUID Guid,PVOID Buffer,ULONG Reserved,PULONG64 EfiStatus) {
  MTK_UFS_POWER_REQUEST *Request=Buffer;
  UNREFERENCED_PARAMETER(Guid); CHECK(Reserved==0 && Request!=NULL); Trace[TraceCount++]=3; PrmCallCount++;
  if(PrmReenter) {
    MTK_UFS_POWER_REQUEST Nested;
    PrmReenter=FALSE;
    RtlZeroMemory(&Nested,sizeof(Nested));
    Nested.Size=sizeof(Nested); Nested.Version=MTK_UFS_POWER_VERSION;
    Nested.Operation=MtkUfsPowerQuery;
    PrmReenterStatus=MtkUfsPrmInvoke(&Nested);
  }
  if(Request->Operation==MtkUfsPowerQuery) {
    Request->Result=(PrmState==MtkUfsPowerActive && !MiniHceLive)?
      MtkUfsPowerWrongState:MtkUfsPowerSuccess;
    Request->State=PrmState; Request->Generation=PrmGeneration;
  } else if(Request->Operation==PrmFailureOperation) {
    Request->Result=MtkUfsPowerWrongState; Request->State=PrmState; Request->Generation=PrmGeneration;
  }
  else if(PrmPending) {
    if(Request->Operation!=PrmPendingOperation || Request->Generation!=PrmGeneration) {
      Request->Result=Request->Generation!=PrmGeneration?MtkUfsPowerStaleGeneration:MtkUfsPowerBusy;
    } else {
      PrmPending=FALSE;
      if(Request->Operation==MtkUfsPowerPrepareStop) {
        PrmState=MtkUfsPowerStopPrepared; Request->State=MtkUfsPowerStopPrepared;
        PrmPrepareCount++; Trace[TraceCount++]=TRACE_PRM_PREPARE;
      } else if(Request->Operation==MtkUfsPowerRelease) {
        PrmState=MtkUfsPowerReleased; Request->State=MtkUfsPowerReleased;
        PrmReleaseCount++; Trace[TraceCount++]=TRACE_PRM_RELEASE;
      } else {
        PrmState=MtkUfsPowerActive; MiniHceLive=TRUE; Request->State=MtkUfsPowerActive;
        PrmRestoreCount++; Trace[TraceCount++]=TRACE_PRM_RESTORE;
      }
      Request->Generation=PrmGeneration; Request->Result=MtkUfsPowerSuccess;
    }
  } else if(Request->Generation!=PrmGeneration) {
    Request->Result=MtkUfsPowerStaleGeneration;
  } else if(Request->Operation==MtkUfsPowerPrepareStop && PrmState==MtkUfsPowerActive &&
            Request->Flags==MTK_UFS_POWER_DEVICE_DOWN_ACKED) {
    PrmPending=TRUE; PrmPendingOperation=Request->Operation; PrmGeneration++;
    PrmState=MtkUfsPowerStopPreparing; Request->Generation=PrmGeneration;
    Request->State=MtkUfsPowerStopPreparing; Request->Result=MtkUfsPowerInProgress;
  } else if(Request->Operation==MtkUfsPowerRelease && PrmState==MtkUfsPowerStopPrepared && !Request->Flags) {
    PrmPending=TRUE; PrmPendingOperation=Request->Operation; PrmGeneration++;
    Request->Generation=PrmGeneration; Request->State=PrmState; Request->Result=MtkUfsPowerInProgress;
  } else if(Request->Operation==MtkUfsPowerRestore &&
            (PrmState==MtkUfsPowerReleased || PrmState==MtkUfsPowerStopPrepared) && !Request->Flags) {
    PrmPending=TRUE; PrmPendingOperation=Request->Operation; PrmGeneration++;
    PrmState=MtkUfsPowerRestorePreparing; Request->Generation=PrmGeneration;
    Request->State=MtkUfsPowerRestorePreparing; Request->Result=MtkUfsPowerInProgress;
  } else {
    Request->Result=MtkUfsPowerWrongState; Request->State=PrmState; Request->Generation=PrmGeneration;
  }
  Request->Capabilities=MTK_UFS_POWER_CAP_RETAINED_VEMC|MTK_UFS_POWER_CAP_LEAF_GATES|MTK_UFS_POWER_CAP_PROGRESS;
  *EfiStatus=PrmEfiFailure?1:0; return PrmInvokeFailure;
}

static NTSTATUS TestExGetPrmInterface(ULONG Version,PPRM_INTERFACE Interface) {
  PrmGetCount++;
  CHECK(Version==PRM_INTERFACE_V1 && Interface);
  if(PrmApiFailure!=STATUS_SUCCESS) return PrmApiFailure;
  Interface->Version=PRM_INTERFACE_V1; Interface->LockModule=TestPrmLock;
  Interface->UnlockModule=TestPrmUnlock; Interface->QueryHandler=TestPrmQueryHandler;
  Interface->InvokeHandler=PrmMissingInvoke?NULL:TestPrmInvokeHandler; return STATUS_SUCCESS;
}

static NTSTATUS TestPrmTryUpdate(VOID) {
  PrmUpdateCount++;
  return PrmModuleLockDepth?STATUS_DEVICE_BUSY:STATUS_SUCCESS;
}

static BOOLEAN TestRtlEqualUnicodeString(PCUNICODE_STRING A,PCUNICODE_STRING B,BOOLEAN CaseInsensitive) {
  USHORT I;
  if(!A || !B || A->Length!=B->Length) return FALSE;
  for(I=0;I<A->Length/sizeof(WCHAR);I++) {
    WCHAR X=A->Buffer[I],Y=B->Buffer[I];
    if(CaseInsensitive) {
      if(X>='a' && X<='z') X=(WCHAR)(X-('a'-'A'));
      if(Y>='a' && Y<='z') Y=(WCHAR)(Y-('a'-'A'));
    }
    if(X!=Y) return FALSE;
  }
  return TRUE;
}
