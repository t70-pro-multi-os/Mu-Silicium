/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual miniport/Core/runtime/DDIs. Only OS, board lease and MMIO boundaries
   are fixtures; registration is not a Windows load or physical IRQ binding. */
#define MALI_VM_TEST_COUNTER MtkMaliMiniportAssertionCount
#define _VCRTIMP
#include "Miniport.h"
#include "TestResidency.h"
static MALI_MINIPORT Parent;
static MALI_DDI_WORKER *TestWorker=&Parent.Runtime.Worker;
#include "TestWorker.h"
#include "Runtime.c"
static DXGKRNL_INTERFACE Kernel;
static DXGK_START_INFO Start;
static DRIVER_INITIALIZATION_DATA Registered;
static __declspec(align(8)) uint8_t ResourceStorage[sizeof(CM_RESOURCE_LIST)+3u*sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR)];
static PDEVICE_OBJECT Pdo=(PDEVICE_OBJECT)0x12345000,Target=(PDEVICE_OBJECT)0x12346000;
static PIRP QueryIrp;
static IRP IrpStorage;
static IO_STACK_LOCATION QueryStack;
static PKEVENT QueryEvent;
static PIO_STATUS_BLOCK QueryResult;
static unsigned PoolLive,PoolFail,PoolFrees,BoardRefs,BoardLive,MapLive,TargetRefs;
static unsigned QueryCalls,MapCalls,UnmapCalls,AcquireCallsCount,ReleaseCallsCount,ReadyCalls,WaitCalls,DelayCalls,QueuedDpcs;
static unsigned MissingTarget,BuildIrpFail,MalformedInterface,MapNull,AcquireNoCookie,BadLease,ReenterAcquire;
static unsigned FeatureDisabled,KeepFeature,DrainAfter;
static NTSTATUS QueryStatus,QueryCompletion,InfoStatus,MapStatus,UnmapStatus,AcquireStatus,ReadyStatus,ReleaseStatus,DelayStatus,InitializeStatus;
static unsigned InitializeCalls;
static unsigned PowerRefs,PowerQueryCalls,PowerOnCalls,PowerOffCalls,MalformedPower,PowerPartialChange,PowerReenter,PowerDpcs;
static NTSTATUS PowerQueryStatus,PowerQueryCompletion,PowerOnStatus,PowerOffStatus;
static POWER_ACTION LastPowerAction;
static ULONG Views,Children;
static DXGKDDI_FEATURE_INTERFACE Borrowed;
static CM_PARTIAL_RESOURCE_LIST *Part(void) {
    return &((PCM_RESOURCE_LIST)ResourceStorage)->List[0].PartialResourceList;
}
static void ResetResources(void) {
    RtlZeroMemory(ResourceStorage,sizeof(ResourceStorage));
    PCM_RESOURCE_LIST R=(PCM_RESOURCE_LIST)ResourceStorage;R->Count=1;
    CM_PARTIAL_RESOURCE_LIST *P=Part();P->Version=P->Revision=1;P->Count=4;
    P->PartialDescriptors[0].Type=CmResourceTypeMemory;
    P->PartialDescriptors[0].ShareDisposition=CmResourceShareDeviceExclusive;
    P->PartialDescriptors[0].u.Memory.Start.QuadPart=0x13000000;
    P->PartialDescriptors[0].u.Memory.Length=0x4000;
    TestIrqResources(&P->PartialDescriptors[1]);
}
static void *MiniportPool(POOL_FLAGS Flags,SIZE_T Bytes,ULONG Tag) {
    CHECK(Irql==PASSIVE_LEVEL && !GateHeld && Flags==POOL_FLAG_NON_PAGED && Tag==MALI_MINIPORT_POOL_TAG && Bytes==sizeof(Parent) && !PoolLive);
    if(PoolFail)return 0;PoolLive=1;return &Parent;
}
static void MiniportFree(void *P,ULONG Tag) {
    CHECK(P==&Parent && Tag==MALI_MINIPORT_POOL_TAG && PoolLive && !BoardRefs && !PowerRefs && !BoardLive && !MapLive && !TargetRefs);
    CHECK(!Parent.Stages && !Parent.Runtime.Stages && !Parent.ActiveCallbacks);
    PoolLive=0;++PoolFrees;
}
static void BoardReference(void *X) {CHECK(X==&BoardLive && BoardRefs);++BoardRefs;}
static void BoardDereference(void *X) {
    CHECK(Irql==PASSIVE_LEVEL && X==&BoardLive && BoardRefs==1 && !BoardLive && !MapLive);--BoardRefs;
}
static NTSTATUS BoardAcquire(void *X,MALI_BOARD_LEASE *L) {
    CHECK(Irql==PASSIVE_LEVEL && !GateHeld && X==&BoardLive && BoardRefs==1 && MapLive && !BoardLive && L);
    ++AcquireCallsCount;
    if(ReenterAcquire) {
        CHECK(MtkMaliMiniportStartDevice(&Parent,&Start,&Kernel,&Views,&Children)==STATUS_DEVICE_BUSY);
        CHECK(MtkMaliMiniportStopDevice(&Parent)==STATUS_DEVICE_BUSY && MtkMaliMiniportRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    }
    RtlZeroMemory(L,sizeof(*L));if(AcquireNoCookie)return AcquireStatus;
    L->Cookie=&BoardLive;BoardLive=1;G.Powered=1;
    L->AddressModel=MaliVmDirectSystemPhysical;L->ApertureCommitLimit=64u*1024u*1024u;L->TimeoutTicks=100;
    L->Cache.Context=&Adapter;L->Cache.CleanForGpu=VmClean;L->Cache.StoreBarrier=VmBarrier;
    switch(BadLease) {
    case 1:L->AddressModel=MaliVmAddressingUnknown;break;
    case 2:L->ApertureCommitLimit=0;break;
    case 3:L->ApertureCommitLimit=4097;break;
    case 4:L->ApertureCommitLimit=MALI_ALLOCATION_MAX_BYTES+4096;break;
    case 5:L->TimeoutTicks=0;break;
    case 6:L->Cache.CleanForGpu=0;break;
    case 7:L->Cache.StoreBarrier=0;break;
    }
    return AcquireStatus;
}
static NTSTATUS BoardReady(void *X,void *Cookie) {
    CHECK(Irql==PASSIVE_LEVEL && X==&BoardLive && Cookie==&BoardLive && BoardLive && BoardRefs==1 && MapLive);
    ++ReadyCalls;return ReadyStatus;
}
static NTSTATUS BoardRelease(void *X,void *Cookie) {
    CHECK(Irql==PASSIVE_LEVEL && !GateHeld && X==&BoardLive && Cookie==&BoardLive && BoardLive && BoardRefs==1 && MapLive);
    CHECK(!Parent.Runtime.Stages && !Parent.Core.Started && !Parent.ActiveCallbacks);
    ++ReleaseCallsCount;if(ReleaseStatus!=STATUS_SUCCESS)return ReleaseStatus;
    BoardLive=0;G.Powered=0;return STATUS_SUCCESS;
}
static void PowerReference(void *X) {CHECK(X==&PowerRefs && PowerRefs);++PowerRefs;}
static void PowerDereference(void *X) {
    CHECK(Irql==PASSIVE_LEVEL && X==&PowerRefs && PowerRefs==1 && !BoardLive && !MapLive && BoardRefs==1);--PowerRefs;
}
static NTSTATUS BoardSetPower(void *X,void *Cookie,DEVICE_POWER_STATE State,POWER_ACTION Action) {
    CHECK(Irql==PASSIVE_LEVEL && !GateHeld && X==&PowerRefs && Cookie==&BoardLive && PowerRefs==1 && BoardRefs==1 && BoardLive && MapLive);
    CHECK(Parent.Started && Parent.Closing && !Parent.ActiveCallbacks && Parent.Runtime.Suspended && !Parent.Runtime.Worker.Thread);
    CHECK(!Parent.Core.Started && !Parent.Core.Poisoned && !IrqConnected(&Parent.Runtime.Adapter) && !CanSchedule(&Parent.Runtime.Adapter));
    CHECK(Parent.PowerState==PowerDeviceUnspecified && (State==PowerDeviceD0 || State==PowerDeviceD3));
    LastPowerAction=Action;
    if(PowerReenter) {
        CHECK(Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_BUSY);
        CHECK(Registered.DxgkDdiStartDevice(&Parent,&Start,&Kernel,&Views,&Children)==STATUS_DEVICE_BUSY);
        CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_DEVICE_BUSY && Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    }
    NTSTATUS S;
    if(State==PowerDeviceD0){++PowerOnCalls;S=PowerOnStatus;CHECK(Action==PowerActionNone);}
    else {++PowerOffCalls;S=PowerOffStatus;}
    if(S==STATUS_SUCCESS || PowerPartialChange)G.Powered=State==PowerDeviceD0;
    return S;
}
static VOID APIENTRY PowerNotifyDpc(HANDLE H) {VmNotifyDpc(H);++PowerDpcs;}
static PDEVICE_OBJECT Attached(PDEVICE_OBJECT P) {
    CHECK(Irql==PASSIVE_LEVEL && P==Pdo && !TargetRefs);
    if(MissingTarget)return 0;++TargetRefs;return Target;
}
static void Dereference(PVOID P) {CHECK(P==Target && TargetRefs==1);--TargetRefs;}
static PIRP BuildQuery(ULONG Major,PDEVICE_OBJECT P,PVOID Buffer,ULONG Bytes,PLARGE_INTEGER Offset,PKEVENT Event,PIO_STATUS_BLOCK Result) {
    CHECK(Irql==PASSIVE_LEVEL && Major==IRP_MJ_PNP && P==Target && !Buffer && !Bytes && !Offset && Event && Result && TargetRefs==1);
    if(BuildIrpFail)return 0;
    RtlZeroMemory(&IrpStorage,sizeof(IrpStorage));RtlZeroMemory(&QueryStack,sizeof(QueryStack));
    QueryEvent=Event;QueryResult=Result;QueryIrp=&IrpStorage;return QueryIrp;
}
static PIO_STACK_LOCATION NextStack(PIRP I) {CHECK(I==QueryIrp);return &QueryStack;}
static NTSTATUS CallQuery(PDEVICE_OBJECT P,PIRP I) {
    int Power=QueryStack.Parameters.QueryInterface.InterfaceType==&MtkMaliBoardPowerInterfaceGuid;
    CHECK(Irql==PASSIVE_LEVEL && P==Target && I==QueryIrp && TargetRefs==1 && !BoardLive && !MapLive && !PowerRefs);
    CHECK(BoardRefs==(Power?1u:0u));
    CHECK(QueryStack.MinorFunction==IRP_MN_QUERY_INTERFACE && I->IoStatus.Status==STATUS_NOT_SUPPORTED);
    CHECK(QueryStack.Parameters.QueryInterface.InterfaceType==(Power?&MtkMaliBoardPowerInterfaceGuid:&MtkMaliBoardInterfaceGuid));
    CHECK(QueryStack.Parameters.QueryInterface.Size==(Power?sizeof(MALI_BOARD_POWER_INTERFACE):sizeof(MALI_BOARD_INTERFACE)));
    CHECK(QueryStack.Parameters.QueryInterface.Version==1 && QueryStack.Parameters.QueryInterface.InterfaceSpecificData);
    const MALI_BOARD_QUERY *Proof=QueryStack.Parameters.QueryInterface.InterfaceSpecificData;
    CHECK(Proof->Size==sizeof(*Proof) && Proof->Version==1 && Proof->Pdo==Parent.Pdo);
    CHECK(RtlEqualMemory(Proof->Interrupts,Parent.Interrupts,sizeof(Proof->Interrupts)));
    NTSTATUS Immediate=Power?PowerQueryStatus:QueryStatus;
    NTSTATUS Result=Immediate==STATUS_PENDING?(Power?PowerQueryCompletion:QueryCompletion):Immediate;
    if(Power)++PowerQueryCalls;else ++QueryCalls;
    if(Result==STATUS_SUCCESS && Power) {
        MALI_BOARD_POWER_INTERFACE *B=(MALI_BOARD_POWER_INTERFACE*)QueryStack.Parameters.QueryInterface.Interface;
        CHECK(B==&Parent.Power);RtlZeroMemory(B,sizeof(*B));
        B->Header.Size=sizeof(*B);B->Header.Version=1;B->Header.Context=&PowerRefs;
        B->Header.InterfaceReference=PowerReference;B->Header.InterfaceDereference=PowerDereference;B->SetPower=BoardSetPower;
        switch(MalformedPower) {
        case 1:B->Header.Size=sizeof(INTERFACE);break;case 2:B->Header.Version=2;break;
        case 3:B->Header.InterfaceReference=0;break;case 4:B->Header.InterfaceDereference=0;break;
        case 5:B->SetPower=0;break;case 6:B->Header.Size=0xffff;break;
        }
        ++PowerRefs;
    } else if(Result==STATUS_SUCCESS) {
        MALI_BOARD_INTERFACE *B=(MALI_BOARD_INTERFACE*)QueryStack.Parameters.QueryInterface.Interface;
        CHECK(B==&Parent.Board);RtlZeroMemory(B,sizeof(*B));
        B->Header.Size=sizeof(*B);B->Header.Version=1;B->Header.Context=&BoardLive;
        B->Header.InterfaceReference=BoardReference;B->Header.InterfaceDereference=BoardDereference;
        B->Acquire=BoardAcquire;B->Ready=BoardReady;B->Release=BoardRelease;
        switch(MalformedInterface) {
        case 1:B->Header.Size=sizeof(INTERFACE);break;case 2:B->Header.Version=2;break;
        case 3:B->Acquire=0;break;case 4:B->Ready=0;break;case 5:B->Release=0;break;
        case 6:B->Header.InterfaceReference=0;break;case 7:B->Header.InterfaceDereference=0;break;
        }
        ++BoardRefs;
    }
    QueryResult->Status=Result;return Immediate;
}
static void QueryInitEvent(PRKEVENT Event,EVENT_TYPE Type,BOOLEAN Set) {
    CHECK(Irql==PASSIVE_LEVEL && Event && Event!=&TestWorker->Signal && Type==NotificationEvent && !Set);
}
static NTSTATUS QueryWait(PVOID P,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    CHECK(Irql==PASSIVE_LEVEL && P==QueryEvent && Reason==Executive && Mode==KernelMode && !Alert && !Timeout && TargetRefs==1);
    ++WaitCalls;return STATUS_SUCCESS;
}
static NTSTATUS CallbackDelay(KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    CHECK(Irql==PASSIVE_LEVEL && !GateHeld && Mode==KernelMode && !Alert && Timeout && Timeout->QuadPart==-10000);
    CHECK(Parent.Closing && (!Parent.Runtime.Stages || (Parent.Runtime.Suspended && !Parent.Core.Started)) && Parent.ActiveCallbacks && BoardLive && MapLive);
    ++DelayCalls;if(DelayCalls==DrainAfter)Parent.ActiveCallbacks=0;return DelayStatus;
}
static ULONG RegisterRead(volatile ULONG *P) {
    uintptr_t Offset=(uintptr_t)P-(uintptr_t)G.Reg;CHECK(MapLive && BoardLive && G.Powered && Offset<0x4000 && !(Offset&3u));
    return Read(&G,(uint32_t)Offset);
}
static void RegisterWrite(volatile ULONG *P,ULONG Value) {
    uintptr_t Offset=(uintptr_t)P-(uintptr_t)G.Reg;CHECK(MapLive && BoardLive && G.Powered && Offset<0x4000 && !(Offset&3u));
    Write(&G,(uint32_t)Offset,Value);
}
static void RegisterBarrier(void) {CHECK(MapLive && BoardLive && G.Powered);++G.Barriers;}
static void Stall(ULONG Us) {CHECK(Irql==PASSIVE_LEVEL && BoardLive && G.Powered);Delay(&G,Us);}
static NTSTATUS APIENTRY DeviceInfo(HANDLE H,DXGK_DEVICE_INFO *I) {
    CHECK(Irql==PASSIVE_LEVEL && H==(HANDLE)0x777 && I && !MapLive && !BoardLive);
    if(InfoStatus!=STATUS_SUCCESS)return InfoStatus;
    I->PhysicalDeviceObject=Pdo;I->TranslatedResourceList=(PCM_RESOURCE_LIST)ResourceStorage;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY MapGpu(HANDLE H,PHYSICAL_ADDRESS Address,ULONG Bytes,BOOLEAN Io,BOOLEAN User,MEMORY_CACHING_TYPE Cache,PVOID *P) {
    CHECK(Irql==PASSIVE_LEVEL && H==(HANDLE)0x777 && Address.QuadPart==0x13000000 && Bytes==0x4000 && !Io && !User && Cache==MmNonCached && P && !MapLive && !BoardLive && BoardRefs==1);
    ++MapCalls;if(MapStatus!=STATUS_SUCCESS)return MapStatus;
    *P=MapNull?0:G.Reg;MapLive=!MapNull;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY UnmapGpu(HANDLE H,PVOID P) {
    CHECK(Irql==PASSIVE_LEVEL && H==(HANDLE)0x777 && P==G.Reg && MapLive && !BoardLive && BoardRefs==1 && !Parent.Runtime.Stages && !Parent.ActiveCallbacks);
    ++UnmapCalls;if(UnmapStatus!=STATUS_SUCCESS)return UnmapStatus;MapLive=0;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Feature(DXGKARGCB_QUERYFEATURESUPPORT *A) {
    CHECK(!GateHeld && Irql==PASSIVE_LEVEL && A && A->DeviceHandle==(HANDLE)0x777 && A->FeatureId==DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD);
    QUERY_INTERFACE Q={0};Q.InterfaceType=&MaliFeatureGuid;Q.Version=1;Q.Size=sizeof(Borrowed);
    Q.Interface=(PINTERFACE)&Borrowed;Q.DeviceUid=DISPLAY_ADAPTER_HW_ID;
    CHECK(MtkMaliDdiQueryInterface(&Parent,&Q)==STATUS_SUCCESS);
    DXGKARG_QUERYFEATURESUPPORT F={0};F.FeatureId=A->FeatureId;
    CHECK(Borrowed.QueryFeatureSupport(Borrowed.Context,&F)==STATUS_SUCCESS && F.SupportedOnCurrentConfig);
    if(!KeepFeature)Borrowed.InterfaceDereference(Borrowed.Context);
    A->Enabled=(BOOLEAN)!FeatureDisabled;return STATUS_SUCCESS;
}
static BOOLEAN APIENTRY QueueDpc(HANDLE H) {
    CHECK(H==(HANDLE)0x777 && Irql>=5 && Irql<=7 && !TestIrqLockHeld);++QueuedDpcs;return TRUE;
}
static NTSTATUS Initialize(PDRIVER_OBJECT D,PUNICODE_STRING Path,PDRIVER_INITIALIZATION_DATA InitData) {
    CHECK(Irql==PASSIVE_LEVEL && D==(PDRIVER_OBJECT)0x8000 && Path && InitData && !BoardRefs && !PoolLive);
    Registered=*InitData;++InitializeCalls;return InitializeStatus;
}
#undef ExAllocatePool2
#undef ExFreePoolWithTag
#undef KeInitializeEvent
#undef KeWaitForSingleObject
#undef IoGetNextIrpStackLocation
#undef IoCallDriver
#undef ObDereferenceObject
#undef KeMemoryBarrier
#define ExAllocatePool2 MiniportPool
#define ExFreePoolWithTag MiniportFree
#define KeInitializeEvent QueryInitEvent
#define KeWaitForSingleObject QueryWait
#define IoGetAttachedDeviceReference Attached
#define IoBuildSynchronousFsdRequest BuildQuery
#define IoGetNextIrpStackLocation NextStack
#define IoCallDriver CallQuery
#define ObDereferenceObject Dereference
#define KeDelayExecutionThread CallbackDelay
#define READ_REGISTER_ULONG RegisterRead
#define WRITE_REGISTER_ULONG RegisterWrite
#define KeMemoryBarrier() RegisterBarrier()
#define KeStallExecutionProcessor Stall
#define DxgkInitialize Initialize
#include "Miniport.c"
static void ResetAttempt(void) {
    CHECK(!GateHeld && !BoardLive && !BoardRefs && !PowerRefs && !MapLive && !TargetRefs && !Parent.Stages && !Parent.Runtime.Stages);
    CHECK(!PrivateLive && !SharedLive && !TestPoolLive && !VmPoolLive && !Parent.ActiveCallbacks);
    Irql=PASSIVE_LEVEL;Init(&G,&Core);G.Powered=0;G.Reg[0xc4/4]=12;
    ThreadCreateFailure=ThreadWaitFailure=ThreadCloseFailure=Terminated=Closed=Signals=0;CounterTicks=0;CounterFrequency=1000;
    TestVmSyncFailure=TestVmSyncCalls=TestVmFailSyncAt=0;
    MissingTarget=BuildIrpFail=MalformedInterface=MapNull=AcquireNoCookie=BadLease=ReenterAcquire=0;
    FeatureDisabled=KeepFeature=DrainAfter=0;
    PowerRefs=PowerQueryCalls=PowerOnCalls=PowerOffCalls=MalformedPower=PowerPartialChange=PowerReenter=PowerDpcs=0;
    PowerQueryStatus=STATUS_NOT_SUPPORTED;PowerQueryCompletion=PowerOnStatus=PowerOffStatus=STATUS_SUCCESS;LastPowerAction=PowerActionNone;
    QueryStatus=QueryCompletion=InfoStatus=MapStatus=UnmapStatus=AcquireStatus=ReadyStatus=ReleaseStatus=DelayStatus=STATUS_SUCCESS;
    QueryCalls=MapCalls=UnmapCalls=AcquireCallsCount=ReleaseCallsCount=ReadyCalls=WaitCalls=DelayCalls=QueuedDpcs=0;
    RtlZeroMemory(&Kernel,sizeof(Kernel));Kernel.Size=sizeof(Kernel);Kernel.Version=DXGKDDI_INTERFACE_VERSION_WDDM3_1;
    Kernel.DeviceHandle=(HANDLE)0x777;Kernel.DxgkCbGetDeviceInformation=DeviceInfo;Kernel.DxgkCbMapMemory=MapGpu;Kernel.DxgkCbUnmapMemory=UnmapGpu;
    Kernel.DxgkCbSynchronizeExecution=VmSync;Kernel.DxgkCbNotifyInterrupt=VmNotify;Kernel.DxgkCbQueueDpc=QueueDpc;
    Kernel.DxgkCbNotifyDpc=VmNotifyDpc;Kernel.DxgkCbGetHandleData=VmHandleData;Kernel.DxgkCbQueryFeatureSupport=Feature;
    ResetResources();Views=42;Children=43;
}
static NTSTATUS StartParent(void) {return Registered.DxgkDdiStartDevice(&Parent,&Start,&Kernel,&Views,&Children);}
static void Empty(void) {
    CHECK(!BoardRefs && !PowerRefs && !BoardLive && !MapLive && !TargetRefs && !Parent.Stages && !Parent.Runtime.Stages && Parent.Closing && !Parent.Started);
    CHECK(!Parent.Core.Started && !Parent.ActiveCallbacks && !Parent.PnpActive && !G.Powered);
    CHECK(!PrivateLive && !SharedLive && !TestPoolLive && !VmPoolLive && !GateHeld);
}
static void Add(void) {PVOID Context=0;CHECK(Registered.DxgkDdiAddDevice(Pdo,&Context)==STATUS_SUCCESS && Context==&Parent && PoolLive);}
static void EntryAndInput(void) {
    UNICODE_STRING Path={0};Irql=PASSIVE_LEVEL;InitializeStatus=STATUS_UNSUCCESSFUL;
    CHECK(DriverEntry((PDRIVER_OBJECT)0x8000,&Path)==STATUS_UNSUCCESSFUL && InitializeCalls==1);
    InitializeStatus=STATUS_SUCCESS;CHECK(DriverEntry((PDRIVER_OBJECT)0x8000,&Path)==STATUS_SUCCESS && InitializeCalls==2);
    CHECK(Registered.Version==DXGKDDI_INTERFACE_VERSION_WDDM3_1);
    CHECK(Registered.DxgkDdiAddDevice==MtkMaliMiniportAddDevice && Registered.DxgkDdiStartDevice==MtkMaliMiniportStartDevice);
    CHECK(Registered.DxgkDdiStopDevice==MtkMaliMiniportStopDevice && Registered.DxgkDdiRemoveDevice==MtkMaliMiniportRemoveDevice);
    CHECK(Registered.DxgkDdiInterruptRoutine==MtkMaliMiniportInterrupt && Registered.DxgkDdiDpcRoutine==MtkMaliMiniportDpc);
    CHECK(Registered.DxgkDdiQueryAdapterInfo==MtkMaliDdiQueryAdapterInfo && Registered.DxgkDdiQueryInterface==MtkMaliDdiQueryInterface);
    CHECK(Registered.DxgkDdiCreateProcess==MtkMaliDdiCreateProcess && Registered.DxgkDdiCreateContext==MtkMaliDdiCreateContext);
    CHECK(Registered.DxgkDdiBuildPagingBuffer==MtkMaliDdiBuildPagingBuffer && Registered.DxgkDdiSubmitCommandVirtual==MtkMaliDdiSubmitCommandVirtual);
    CHECK(Registered.DxgkDdiCreateAllocation==MtkMaliDdiCreateAllocation && Registered.DxgkDdiSetAllocationBackingStore==MtkMaliDdiSetAllocationBackingStore);
    CHECK(DriverEntry(0,&Path)==STATUS_INVALID_PARAMETER && DriverEntry((PDRIVER_OBJECT)0x8000,0)==STATUS_INVALID_PARAMETER);
    Irql=DISPATCH_LEVEL;CHECK(DriverEntry((PDRIVER_OBJECT)0x8000,&Path)==STATUS_INVALID_PARAMETER && InitializeCalls==2);
    PVOID Context=(void*)0xbeef;CHECK(Registered.DxgkDdiAddDevice(Pdo,&Context)==STATUS_INVALID_PARAMETER && Context==(void*)0xbeef);
    Irql=PASSIVE_LEVEL;CHECK(Registered.DxgkDdiAddDevice(0,&Context)==STATUS_INVALID_PARAMETER && Registered.DxgkDdiAddDevice(Pdo,0)==STATUS_INVALID_PARAMETER);
    PoolFail=1;CHECK(Registered.DxgkDdiAddDevice(Pdo,&Context)==STATUS_INSUFFICIENT_RESOURCES && Context==(void*)0xbeef);PoolFail=0;
    Add();ResetAttempt();
    for(UINT F=0;F<27;F++) {
        ResetAttempt();NTSTATUS Expected=STATUS_DEVICE_CONFIGURATION_ERROR;
        CM_PARTIAL_RESOURCE_DESCRIPTOR *R=Part()->PartialDescriptors;
        switch(F) {
        case 0:((PCM_RESOURCE_LIST)ResourceStorage)->Count=2;break;case 1:Part()->Count=3;break;
        case 2:Part()->Version=0;break;case 3:Part()->Revision=0;break;
        case 4:R[0].u.Memory.Start.QuadPart+=4096;break;case 5:R[0].u.Memory.Length-=4096;break;
        case 6:R[0].ShareDisposition=CmResourceShareShared;break;case 7:R[0].Flags=CM_RESOURCE_MEMORY_READ_ONLY;break;
        case 8:R[1].Flags=CM_RESOURCE_INTERRUPT_LATCHED;break;case 9:R[1].Flags=CM_RESOURCE_INTERRUPT_MESSAGE;break;
        case 10:R[1].u.Interrupt.Level=DISPATCH_LEVEL;break;case 11:R[1].u.Interrupt.Level=PROFILE_LEVEL;break;
        case 12:R[1].u.Interrupt.Affinity=0;break;case 13:R[2].u.Interrupt.Vector=R[1].u.Interrupt.Vector;break;
        case 14:R[1].Type=CmResourceTypeDma;break;case 15:Parent.Pdo=Target;break;
        case 16:InfoStatus=STATUS_UNSUCCESSFUL;Expected=InfoStatus;break;
        case 17:Kernel.DxgkCbSynchronizeExecution=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 18:Kernel.DxgkCbNotifyInterrupt=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 19:Kernel.DxgkCbQueueDpc=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 20:Kernel.DxgkCbNotifyDpc=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 21:Kernel.DxgkCbGetHandleData=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 22:Kernel.DxgkCbQueryFeatureSupport=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 23:Kernel.Size=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 24:Kernel.Version=DXGKDDI_INTERFACE_VERSION_WDDM2_9;Expected=STATUS_INVALID_PARAMETER;break;
        case 25:Kernel.DxgkCbMapMemory=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 26:Kernel.DxgkCbUnmapMemory=0;Expected=STATUS_INVALID_PARAMETER;break;
        }
        CHECK(StartParent()==Expected && Views==42 && Children==43);
        CHECK(!QueryCalls && !MapCalls && !AcquireCallsCount && !G.Reads && !G.Writes);
        Parent.Pdo=Pdo;Empty();if(FirstFailure)return;
    }
    Irql=DISPATCH_LEVEL;CHECK(StartParent()==STATUS_INVALID_PARAMETER && Registered.DxgkDdiStopDevice(&Parent)==STATUS_INVALID_PARAMETER);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliMiniportStartDevice(0,&Start,&Kernel,&Views,&Children)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliMiniportStartDevice(&Parent,0,&Kernel,&Views,&Children)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliMiniportStartDevice(&Parent,&Start,0,&Views,&Children)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliMiniportStartDevice(&Parent,&Start,&Kernel,0,&Children)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliMiniportStartDevice(&Parent,&Start,&Kernel,&Views,0)==STATUS_INVALID_PARAMETER);
    Parent.PnpActive=1;CHECK(StartParent()==STATUS_DEVICE_BUSY && Registered.DxgkDdiStopDevice(&Parent)==STATUS_DEVICE_BUSY && Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);Parent.PnpActive=0;
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS && !PoolLive);
}
static void AcquisitionFailures(void) {
    Add();ResetAttempt();
    for(UINT F=0;F<27;F++) {
        ResetAttempt();NTSTATUS Expected=STATUS_DRIVER_INTERNAL_ERROR;
        switch(F) {
        case 0:MissingTarget=1;Expected=STATUS_NO_SUCH_DEVICE;break;
        case 1:BuildIrpFail=1;Expected=STATUS_INSUFFICIENT_RESOURCES;break;
        case 2:QueryStatus=STATUS_NOT_SUPPORTED;Expected=QueryStatus;break;
        case 3:QueryStatus=STATUS_PENDING;QueryCompletion=STATUS_NOT_SUPPORTED;Expected=QueryCompletion;break;
        case 4:case 5:case 6:case 7:case 8:case 9:MalformedInterface=F-3;break;
        case 10:MapStatus=STATUS_INSUFFICIENT_RESOURCES;Expected=MapStatus;break;
        case 11:MapNull=1;Expected=STATUS_DEVICE_CONFIGURATION_ERROR;break;
        case 12:AcquireNoCookie=1;Expected=STATUS_DEVICE_CONFIGURATION_ERROR;break;
        case 13:AcquireStatus=STATUS_UNSUCCESSFUL;Expected=AcquireStatus;break;
        case 14:AcquireStatus=STATUS_DEVICE_BUSY;AcquireNoCookie=1;Expected=AcquireStatus;break;
        case 15:case 16:case 17:case 18:case 19:case 20:case 21:BadLease=F-14;Expected=STATUS_DEVICE_CONFIGURATION_ERROR;break;
        case 22:ReadyStatus=STATUS_DEVICE_NOT_READY;Expected=ReadyStatus;break;
        case 23:ThreadCreateFailure=1;Expected=STATUS_INSUFFICIENT_RESOURCES;break;
        case 24:FeatureDisabled=1;Expected=STATUS_NOT_SUPPORTED;break;
        case 25:CounterFrequency=0;Expected=STATUS_INVALID_PARAMETER;break;
        case 26:G.PowerStuck=1;Expected=STATUS_IO_TIMEOUT;break;
        }
        CHECK(StartParent()==Expected && Parent.StartStatus==Expected && Parent.CleanupStatus==STATUS_SUCCESS && Views==42 && Children==43);
        if(F<23)CHECK(!G.Reads && !G.Writes);
        if(F==3)CHECK(WaitCalls==1);Empty();if(FirstFailure)return;
    }
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS && !PoolLive);
}
static void LifecycleAndInterrupts(void) {
    Add();ResetAttempt();QueryStatus=STATUS_PENDING;ReenterAcquire=1;
    CHECK(StartParent()==STATUS_SUCCESS && !Views && !Children && Parent.Started && WaitCalls==1);
    CHECK(Parent.Runtime.Stages && Parent.Core.Started && BoardLive && MapLive && BoardRefs==1 && !Parent.Closing);
    CHECK(Parent.Runtime.Adapter.InterruptSyncIrql==7 && Parent.Runtime.Adapter.InterruptLevelMask==0xe0);
    unsigned Writes=G.Writes,Reads=G.Reads;
    CHECK(StartParent()==STATUS_DEVICE_BUSY && G.Writes==Writes && G.Reads==Reads);
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,PowerDeviceD3,PowerActionSleep)==STATUS_NOT_SUPPORTED && Parent.Started && BoardLive && G.Writes==Writes);
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,0,PowerDeviceD0,PowerActionNone)==STATUS_INVALID_PARAMETER);
    ReadyStatus=STATUS_DEVICE_NOT_READY;
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_NOT_READY && G.Writes==Writes);
    ReadyStatus=STATUS_SUCCESS;
    DXGKARG_QUERYADAPTERINFO Q={0};DXGK_DRIVERCAPS Caps;Q.Type=DXGKQAITYPE_DRIVERCAPS;Q.pOutputData=&Caps;Q.OutputDataSize=sizeof(Caps);
    CHECK(Registered.DxgkDdiQueryAdapterInfo(&Parent,&Q)==STATUS_SUCCESS && Caps.GpuEngineTopology.NbAsymetricProcessingNodes==1 && Caps.SupportPerEngineTDR);
    DXGK_CHILD_DESCRIPTOR Child;RtlFillMemory(&Child,sizeof(Child),0xa5);
    CHECK(Registered.DxgkDdiQueryChildRelations(&Parent,&Child,sizeof(Child))==STATUS_SUCCESS && Child.ChildDeviceType==0 && !Child.ChildUid);
    DXGK_CHILD_STATUS ChildStatus={0};DXGK_DEVICE_DESCRIPTOR Descriptor={0};VIDEO_REQUEST_PACKET Request={0};
    CHECK(Registered.DxgkDdiQueryChildStatus(&Parent,&ChildStatus,FALSE)==STATUS_NO_SUCH_DEVICE);
    CHECK(Registered.DxgkDdiQueryDeviceDescriptor(&Parent,0,&Descriptor)==STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA);
    CHECK(Registered.DxgkDdiDispatchIoRequest(&Parent,0,&Request)==STATUS_NOT_SUPPORTED);
    CHECK(Registered.DxgkDdiQueryChildRelations(&Parent,&Child,sizeof(Child)-1)==STATUS_INVALID_PARAMETER);
    DXGKARG_CREATEPROCESS P={0};P.hDxgkProcess=(HANDLE)0x1234;
    CHECK(Registered.DxgkDdiCreateProcess(&Parent,&P)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_DEVICE_BUSY && Parent.Started && !Parent.Closing && BoardLive && CanSchedule(&Parent.Runtime.Adapter));
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY && PoolLive);
    CHECK(Registered.DxgkDdiDestroyProcess(&Parent,P.hKmdProcess)==STATUS_SUCCESS);
    Reads=G.Reads;CHECK(!Registered.DxgkDdiInterruptRoutine(&Parent,0) && G.Reads==Reads);
    Irql=DISPATCH_LEVEL;CHECK(!Registered.DxgkDdiInterruptRoutine(&Parent,0) && G.Reads==Reads);Irql=PASSIVE_LEVEL;
    for(UINT I=0;I<3;I++) {
        G.Reg[(I==0?0x20:I==1?0x1000:0x2000)/4]=1;
        Irql=(KIRQL)(5+I);CHECK(Registered.DxgkDdiInterruptRoutine(&Parent,0) && !Parent.ActiveCallbacks && Irql==5+I);
        Irql=DISPATCH_LEVEL;unsigned Before=Signals;Registered.DxgkDdiDpcRoutine(&Parent);
        CHECK(Signals==Before+1 && !Parent.ActiveCallbacks);Irql=PASSIVE_LEVEL;
    }
    CHECK(QueuedDpcs==3);
    CHECK(MtkMaliDdiPump(&Parent.Runtime.Adapter,1)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE);
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_NOT_READY);
    DXGKARG_RESETENGINE Reset={0};CHECK(Registered.DxgkDdiResetEngine(&Parent,&Reset)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    Reads=G.Reads;Irql=5;CHECK(!Registered.DxgkDdiInterruptRoutine(&Parent,0));
    Irql=DISPATCH_LEVEL;Registered.DxgkDdiDpcRoutine(&Parent);Irql=PASSIVE_LEVEL;CHECK(G.Reads==Reads);
    ResetAttempt();CHECK(StartParent()==STATUS_SUCCESS && Parent.Started);
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    Views=42;Children=43;
    CHECK(Registered.DxgkDdiStartDevice(&Parent,&Start,&Parent.Dxgk,&Views,&Children)==STATUS_SUCCESS && !Views && !Children);
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS && !PoolLive);
}
static void FailureOwnershipAndRetry(void) {
    Add();ResetAttempt();AcquireStatus=STATUS_UNSUCCESSFUL;ReleaseStatus=STATUS_IO_DEVICE_ERROR;
    CHECK(StartParent()==STATUS_UNSUCCESSFUL && Parent.StartStatus==STATUS_UNSUCCESSFUL && Parent.CleanupStatus==STATUS_IO_DEVICE_ERROR);
    CHECK(BoardLive && MapLive && BoardRefs==1 && !G.Writes && !G.Reads);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    ReleaseStatus=STATUS_SUCCESS;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();G.ResetStuck=1;
    CHECK(StartParent()==STATUS_IO_TIMEOUT && Parent.CleanupStatus==STATUS_IO_TIMEOUT && BoardLive && MapLive && BoardRefs==1 && Parent.CoreTouched);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    G.ResetStuck=0;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();CHECK(StartParent()==STATUS_SUCCESS);G.CacheStuck=1;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_IO_TIMEOUT && !Parent.Runtime.Stages && BoardLive && MapLive && Parent.Closing && !Parent.Started);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    G.CacheStuck=0;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();KeepFeature=FeatureDisabled=1;
    CHECK(StartParent()==STATUS_NOT_SUPPORTED && Parent.CleanupStatus==STATUS_DEVICE_BUSY && Parent.Runtime.Stages && BoardLive && MapLive && BoardRefs==1);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    Borrowed.InterfaceDereference(Borrowed.Context);CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();CHECK(StartParent()==STATUS_SUCCESS);ThreadWaitFailure=1;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_UNSUCCESSFUL && Parent.Runtime.Stages && BoardLive && MapLive && Parent.Runtime.Worker.Thread);
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_NOT_READY);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    ThreadWaitFailure=0;ThreadCloseFailure=1;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_UNSUCCESSFUL && Parent.Runtime.Stages && BoardLive && MapLive && Terminated==1);
    ThreadCloseFailure=0;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();CHECK(StartParent()==STATUS_SUCCESS);TestVmSyncFailure=1;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_UNSUCCESSFUL && Parent.Runtime.Stages && Parent.Started && BoardLive && MapLive);
    TestVmSyncFailure=0;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();CHECK(StartParent()==STATUS_SUCCESS);Parent.ActiveCallbacks=1;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_IO_TIMEOUT && DelayCalls==1000 && !Parent.Runtime.Stages && BoardLive && MapLive && Parent.Closing);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);Parent.ActiveCallbacks=0;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();CHECK(StartParent()==STATUS_SUCCESS);Parent.ActiveCallbacks=1;DelayStatus=STATUS_UNSUCCESSFUL;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_UNSUCCESSFUL && DelayCalls==1 && BoardLive && MapLive);
    DelayStatus=STATUS_SUCCESS;DrainAfter=4;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS && DelayCalls==4);Empty();
    ResetAttempt();CHECK(StartParent()==STATUS_SUCCESS);ReleaseStatus=STATUS_IO_DEVICE_ERROR;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_IO_DEVICE_ERROR && BoardLive && MapLive && BoardRefs==1 && !Parent.Runtime.Stages && !Parent.Core.Started);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    ReleaseStatus=STATUS_SUCCESS;UnmapStatus=STATUS_UNSUCCESSFUL;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_UNSUCCESSFUL && !BoardLive && MapLive && BoardRefs==1);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    UnmapStatus=STATUS_SUCCESS;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    ResetAttempt();MalformedInterface=7;
    CHECK(StartParent()==STATUS_DRIVER_INTERNAL_ERROR && Parent.CleanupStatus==STATUS_DRIVER_INTERNAL_ERROR && BoardRefs==1 && !MapLive && !BoardLive && !G.Writes);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    /* Supply the missing provider cleanup callback only to verify retention;
       a real malformed provider cannot be repaired by the miniport itself. */
    Parent.Board.Header.InterfaceDereference=BoardDereference;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS && !PoolLive && PoolFrees==4);
}
static NTSTATUS PowerRequest(DEVICE_POWER_STATE State,POWER_ACTION Action) {
    return Registered.DxgkDdiSetPowerState(&Parent,DISPLAY_ADAPTER_HW_ID,State,Action);
}
static void PowerRoundTrip(void) {
    Add();ResetAttempt();PowerQueryStatus=STATUS_PENDING;Kernel.DxgkCbNotifyDpc=PowerNotifyDpc;PowerReenter=1;
    CHECK(StartParent()==STATUS_SUCCESS && PowerQueryCalls==1 && PowerRefs==1 && WaitCalls==1 && Parent.PowerState==PowerDeviceD0);
    RtlZeroMemory(OsTables,4u*4096u);for(UINT I=0;I<256;I++)OsPhysical[I]=0x40001000+(uint64_t)I*4096;
    for(UINT I=0;I<OS_SHARED_PAGES;I++)SharedPhysical[I]=0x50000000+(uint64_t)I*3u*4096;
    MALI_DDI_ADAPTER *D=&Parent.Runtime.Adapter;
    DXGKARG_CREATEPROCESS P={0};P.hDxgkProcess=(HANDLE)0x1234;
    CHECK(Registered.DxgkDdiCreateProcess(&Parent,&P)==STATUS_SUCCESS);Proc[0]=P.hKmdProcess;
    DXGKARG_CREATEDEVICE V={0};V.hKmdProcess=Proc[0];CHECK(Registered.DxgkDdiCreateDevice(&Parent,&V)==STATUS_SUCCESS);Dev[0]=V.hDevice;
    MALI_ALLOCATION_DESC Desc={1,sizeof(Desc),1,3,8192,0,0,0,0};DXGK_ALLOCATIONINFO Info={0};
    Info.pPrivateDriverData=&Desc;Info.PrivateDriverDataSize=sizeof(Desc);
    DXGKARG_CREATEALLOCATION A={0};A.Flags.Resource=1;A.NumAllocations=1;A.pAllocationInfo=&Info;
    CHECK(Registered.DxgkDdiCreateAllocation(&Parent,&A)==STATUS_SUCCESS);AllocationHandle=Info.hAllocation;ResourceHandle=A.hResource;
    DXGK_OPENALLOCATIONINFO OI={0};OI.hAllocation=10;DXGKARG_OPENALLOCATION O={0};O.NumAllocations=1;O.pOpenAllocation=&OI;
    CHECK(Registered.DxgkDdiOpenAllocation(Dev[0],&O)==STATUS_SUCCESS);OpenHandle=OI.hDeviceSpecificAllocation;
    DXGKARG_SETALLOCATIONBACKINGSTORE B={AllocationHandle,SharedData};CHECK(Registered.DxgkDdiSetAllocationBackingStore(&Parent,&B)==STATUS_SUCCESS);
    DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(Registered.DxgkDdiCreateContext(Dev[0],&C)==STATUS_SUCCESS && C.ContextInfo.DmaBufferPrivateDataSize==MALI_COMMAND_MAX_PRIVATE);Ctx=C.hContext;
    DXGKARG_SETROOTPAGETABLE R={0};R.hContext=Ctx;R.NumEntries=512;R.Address.SegmentOffset=OsPhysical[0];
    Registered.DxgkDdiSetRootPageTable(&Parent,&R);CHECK(D->LastRootStatus==STATUS_SUCCESS);
    for(UINT L=0;L<3;L++) {
        Entry(0,OsPhysical[L+1],0,0);Update(L,3-L,0,(UINT)((0x80000000ULL>>(39-L*9))&511),1,1,0,0);
        CHECK(Registered.DxgkDdiBuildPagingBuffer(&Parent,&Build)==STATUS_SUCCESS);
    }
    Entry(0,SharedPhysical[0],1,0);Entry(1,SharedPhysical[1],1,0);Update(3,0,0,0,2,1,AllocationHandle,0x80000000);
    CHECK(Registered.DxgkDdiBuildPagingBuffer(&Parent,&Build)==STATUS_SUCCESS);
    RtlZeroMemory(SharedData,8192);TestPut32(SharedData+16,(71u<<16)|4u);TestPut64(SharedData+32,0x80001000);
    TestPut32(SharedData+40,7);TestPut64(SharedData+48,0x123456789abcdefULL);
    RtlZeroMemory(&Private,sizeof(Private));Private.Version=1;Private.Bytes=MALI_EXEC_PACKET_BYTES(1);
    Private.StepCount=1;Private.Steps[0].Slot=1;Private.Steps[0].Chain=0x80000000;
    DXGKARG_SUBMITCOMMANDVIRTUAL S={0};S.hContext=Ctx;S.DmaBufferVirtualAddress=0x80000000;S.DmaBufferSize=4096;
    S.pDmaBufferPrivateData=&Private;S.DmaBufferPrivateDataSize=Private.Bytes;S.SubmissionFenceId=1;
    CHECK(Registered.DxgkDdiSubmitCommandVirtual(&Parent,&S)==STATUS_SUCCESS && Parent.Runtime.Residency.Count==1);
    UINT Writes=G.Writes;CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==STATUS_DEVICE_BUSY && !PowerOffCalls && G.Writes==Writes);
    CHECK(Parent.Started && CanSchedule(D) && Parent.Core.Started && !Parent.Closing && Parent.PowerState==PowerDeviceD0);
    CHECK(MtkMaliDdiPump(D,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(D,2)==STATUS_SUCCESS && !Parent.Runtime.Residency.Count && LockedPages==UnlockedPages);
    uint64_t SavedId=((MALI_DDI_CONTEXT*)Ctx)->Id;UINT Tables=Parent.Runtime.Vm.TableCount;
    for(UINT Cycle=0;Cycle<3;Cycle++) {
        CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS && PowerOffCalls==Cycle+1 && LastPowerAction==PowerActionSleep);
        CHECK(Parent.Started && Parent.PowerState==PowerDeviceD3 && Parent.Closing && Parent.Runtime.Suspended && !Parent.CoreTouched);
        CHECK(!G.Powered && !Parent.Core.Started && !Parent.Runtime.Worker.Thread && !IrqConnected(D) && !CanSchedule(D));
        CHECK(BoardLive && MapLive && BoardRefs==1 && PowerRefs==1 && Parent.Runtime.Vm.TableCount==Tables);
        UINT Reads=G.Reads;Writes=G.Writes;
        DXGK_DRIVERCAPS Caps={0};DXGKARG_QUERYADAPTERINFO Q={0};Q.Type=DXGKQAITYPE_DRIVERCAPS;
        Q.pOutputData=&Caps;Q.OutputDataSize=sizeof(Caps);
        CHECK(Registered.DxgkDdiQueryAdapterInfo(&Parent,&Q)==STATUS_SUCCESS && Caps.MemoryManagementCaps.GpuMmuSupported);
        DXGKARG_GETNODEMETADATA Node={0};CHECK(Registered.DxgkDdiGetNodeMetadata(&Parent,0,&Node)==STATUS_SUCCESS && Node.GpuMmuSupported);
        DXGKARG_RESETENGINE Reset={0};CHECK(Registered.DxgkDdiResetEngine(&Parent,&Reset)==STATUS_DEVICE_NOT_READY);
        CHECK(Registered.DxgkDdiSubmitCommandVirtual(&Parent,&S)==STATUS_DEVICE_NOT_READY);
        Irql=5;CHECK(!Registered.DxgkDdiInterruptRoutine(&Parent,0));Irql=DISPATCH_LEVEL;
        UINT Before=PowerDpcs;Registered.DxgkDdiDpcRoutine(&Parent);Irql=PASSIVE_LEVEL;
        CHECK(PowerDpcs==Before+1 && !Parent.ActiveCallbacks);
        CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS && PowerOffCalls==Cycle+1);
        CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_DEVICE_BUSY && Parent.Started && Parent.Runtime.Suspended);
        CHECK(G.Reads==Reads && G.Writes==Writes);
        CHECK(PowerRequest(PowerDeviceD0,(POWER_ACTION)0x7fff)==STATUS_SUCCESS && PowerOnCalls==Cycle+1 && LastPowerAction==PowerActionNone);
        CHECK(Parent.PowerState==PowerDeviceD0 && !Parent.Closing && !Parent.Runtime.Suspended && Parent.Core.Started && G.Powered);
        CHECK(Parent.Runtime.Worker.Thread && CanSchedule(D) && IrqConnected(D) && Parent.Runtime.Vm.TableCount==Tables);
        CHECK(((MALI_DDI_CONTEXT*)Ctx)->Id==SavedId && ((MALI_DDI_CONTEXT*)Ctx)->Root==OsPhysical[0]);
        Reads=G.Reads;Writes=G.Writes;
        CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS && PowerOnCalls==Cycle+1 && G.Reads==Reads && G.Writes==Writes);
        S.SubmissionFenceId=2+Cycle;
        CHECK(Registered.DxgkDdiSubmitCommandVirtual(&Parent,&S)==STATUS_SUCCESS);
        CHECK(MtkMaliDdiPump(D,3+Cycle*2)==STATUS_SUCCESS);Completion(&G,1,1,0);
        CHECK(MtkMaliDdiPump(D,4+Cycle*2)==STATUS_SUCCESS && D->Queue.LastCompletedFence==2+Cycle);
        CHECK(!Parent.Runtime.Residency.Count && !Parent.Runtime.Vm.HoldCount && !D->Allocations.ReferenceCount);
    }
    CHECK(PowerRequest(PowerDeviceD3,PowerActionHibernate)==STATUS_SUCCESS && PowerOffCalls==4);
    UINT Reads=G.Reads;Writes=G.Writes;
    CHECK(Registered.DxgkDdiDestroyContext(Ctx)==STATUS_SUCCESS);
    DXGKARG_CLOSEALLOCATION Close={1,&OpenHandle};CHECK(Registered.DxgkDdiCloseAllocation(Dev[0],&Close)==STATUS_SUCCESS);
    DXGKARG_DESTROYALLOCATION Destroy={0};Destroy.NumAllocations=1;Destroy.pAllocationList=&AllocationHandle;
    Destroy.hResource=ResourceHandle;Destroy.Flags.DestroyResource=1;CHECK(Registered.DxgkDdiDestroyAllocation(&Parent,&Destroy)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiDestroyDevice(Dev[0])==STATUS_SUCCESS && Registered.DxgkDdiDestroyProcess(&Parent,Proc[0])==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS && G.Reads==Reads && G.Writes==Writes);Empty();
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS);
}
static void PowerInterfaceFailures(void) {
    Add();ResetAttempt();
    for(UINT Failure=0;Failure<9;Failure++) {
        ResetAttempt();PowerQueryStatus=STATUS_SUCCESS;NTSTATUS Expected=STATUS_DRIVER_INTERNAL_ERROR;
        if(Failure<6)MalformedPower=Failure+1;
        else if(Failure==6){PowerQueryStatus=STATUS_IO_DEVICE_ERROR;Expected=PowerQueryStatus;}
        else if(Failure==7){PowerQueryStatus=STATUS_PENDING;PowerQueryCompletion=STATUS_IO_DEVICE_ERROR;Expected=PowerQueryCompletion;}
        else {PowerQueryStatus=STATUS_TIMEOUT;Expected=STATUS_UNSUCCESSFUL;}
        CHECK(StartParent()==Expected && PowerQueryCalls==1 && !MapCalls && !AcquireCallsCount && !G.Reads && !G.Writes);
        if(Failure==3) {
            CHECK(PowerRefs==1 && BoardRefs==1 && Parent.CleanupStatus==STATUS_DRIVER_INTERNAL_ERROR);
            CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
            Parent.Power.Header.InterfaceDereference=PowerDereference;
            CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);
        }
        if(Failure==7)CHECK(WaitCalls==1);
        Empty();if(FirstFailure)return;
    }
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS);
}
static void PowerSuspendFailures(void) {
    Add();ResetAttempt();
    for(UINT Failure=0;Failure<9;Failure++) {
        ResetAttempt();PowerQueryStatus=STATUS_SUCCESS;CHECK(StartParent()==STATUS_SUCCESS);
        NTSTATUS Expected=STATUS_UNSUCCESSFUL;
        switch(Failure) {
        case 0:TestVmSyncFailure=1;break;case 1:ThreadWaitFailure=1;break;
        case 2:ThreadCloseFailure=1;break;case 3:G.CacheStuck=1;Expected=STATUS_IO_TIMEOUT;break;
        case 4:Parent.ActiveCallbacks=1;Expected=STATUS_IO_TIMEOUT;break;
        case 5:Parent.ActiveCallbacks=1;DelayStatus=STATUS_UNSUCCESSFUL;break;
        case 6:PowerOffStatus=STATUS_IO_DEVICE_ERROR;Expected=PowerOffStatus;break;
        case 7:PowerOffStatus=STATUS_IO_DEVICE_ERROR;PowerPartialChange=1;Expected=PowerOffStatus;break;
        case 8:PowerOffStatus=STATUS_TIMEOUT;PowerPartialChange=1;break;
        }
        CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==Expected && Parent.PowerStatus==Expected);
        CHECK(BoardLive && MapLive && BoardRefs==1 && PowerRefs==1 && Parent.Started && Parent.Runtime.Suspended && !CanSchedule(&Parent.Runtime.Adapter));
        CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
        if(Failure<6)CHECK(!PowerOffCalls && Parent.PowerState==PowerDeviceD0 && G.Powered);
        else {
            CHECK(PowerOffCalls==1 && Parent.PowerState==PowerDeviceUnspecified && Parent.Closing && !Parent.CoreTouched);
            UINT Reads=G.Reads,Writes=G.Writes;PowerOnStatus=STATUS_IO_DEVICE_ERROR;
            CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_IO_DEVICE_ERROR && Parent.PowerState==PowerDeviceUnspecified);
            CHECK(G.Reads==Reads && G.Writes==Writes && Parent.Runtime.Suspended && !Parent.Core.Started);
            PowerOnStatus=STATUS_SUCCESS;
        }
        TestVmSyncFailure=ThreadWaitFailure=ThreadCloseFailure=0;G.CacheStuck=0;DelayStatus=STATUS_SUCCESS;
        if(Failure==4) {CHECK(DelayCalls==1000);Parent.ActiveCallbacks=0;}
        if(Failure==5)DrainAfter=4;
        PowerOffStatus=STATUS_SUCCESS;
        if(!(Failure&1u))CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS && Parent.PowerState==PowerDeviceD3 && !G.Powered);
        CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS && Parent.PowerState==PowerDeviceD0 && CanSchedule(&Parent.Runtime.Adapter));
        if(Failure==5)CHECK(DelayCalls==4);
        CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();if(FirstFailure)return;
    }
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS);
}
static void PowerResumeFailures(void) {
    Add();ResetAttempt();
    for(UINT Failure=0;Failure<8;Failure++) {
        ResetAttempt();PowerQueryStatus=STATUS_SUCCESS;CHECK(StartParent()==STATUS_SUCCESS);
        CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS && !G.Powered);
        UINT Reads=G.Reads,Writes=G.Writes;NTSTATUS Expected=STATUS_DEVICE_CONFIGURATION_ERROR;
        switch(Failure) {
        case 0:PowerOnStatus=STATUS_IO_DEVICE_ERROR;Expected=PowerOnStatus;break;
        case 1:PowerOnStatus=STATUS_TIMEOUT;PowerPartialChange=1;Expected=STATUS_UNSUCCESSFUL;break;
        case 2:ReadyStatus=STATUS_DEVICE_NOT_READY;Expected=ReadyStatus;break;
        case 3:ThreadCreateFailure=1;Expected=STATUS_INSUFFICIENT_RESOURCES;break;
        case 4:TestVmSyncFailure=1;Expected=STATUS_UNSUCCESSFUL;break;
        case 5:G.ResetStuck=1;Expected=STATUS_IO_TIMEOUT;break;
        case 6:G.Reg[0x100/4]=7;break;case 7:G.Reg[0xc4/4]=28;break;
        }
        CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==Expected && Parent.PowerStatus==Expected && Parent.Runtime.Suspended);
        CHECK(Parent.Started && BoardLive && MapLive && PowerRefs==1 && BoardRefs==1 && !CanSchedule(&Parent.Runtime.Adapter));
        if(Failure<3)CHECK(G.Reads==Reads && G.Writes==Writes && Parent.Closing && !Parent.Core.Started && !Parent.CoreTouched);
        if(Failure<2)CHECK(Parent.PowerState==PowerDeviceUnspecified);else CHECK(Parent.PowerState==PowerDeviceD0);
        CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
        PowerOnStatus=ReadyStatus=STATUS_SUCCESS;PowerPartialChange=ThreadCreateFailure=TestVmSyncFailure=0;
        G.ResetStuck=0;G.Reg[0x100/4]=5;G.Reg[0xc4/4]=12;
        CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS && !Parent.Runtime.Suspended && Parent.Core.Started);
        CHECK(Parent.PowerState==PowerDeviceD0 && !Parent.Closing && CanSchedule(&Parent.Runtime.Adapter) && IrqConnected(&Parent.Runtime.Adapter));
        CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS);Empty();if(FirstFailure)return;
    }
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS);
}
static void PowerInputsAndOffCleanup(void) {
    Add();ResetAttempt();CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_NOT_READY);
    PowerQueryStatus=STATUS_SUCCESS;CHECK(StartParent()==STATUS_SUCCESS);
    UINT Reads=G.Reads,Writes=G.Writes;
    CHECK(PowerRequest(PowerDeviceUnspecified,PowerActionNone)==STATUS_INVALID_PARAMETER);
    CHECK(PowerRequest(PowerDeviceMaximum,PowerActionNone)==STATUS_INVALID_PARAMETER);
    CHECK(PowerRequest((DEVICE_POWER_STATE)-1,PowerActionNone)==STATUS_INVALID_PARAMETER);
    CHECK(Registered.DxgkDdiSetPowerState(&Parent,0,PowerDeviceD3,PowerActionSleep)==STATUS_INVALID_PARAMETER);
    Irql=DISPATCH_LEVEL;CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    CHECK(!PowerOffCalls && !PowerOnCalls && G.Reads==Reads && G.Writes==Writes && CanSchedule(&Parent.Runtime.Adapter));
    CHECK(PowerRequest(PowerDeviceD1,PowerActionSleep)==STATUS_SUCCESS && PowerOffCalls==1);
    CHECK(Parent.PowerState==PowerDeviceD3 && Parent.Runtime.Suspended && !G.Powered);
    CHECK(PowerRequest(PowerDeviceD2,PowerActionSleep)==STATUS_SUCCESS && PowerOffCalls==1);
    CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS && PowerOnCalls==1);
    CHECK(Parent.PowerState==PowerDeviceD0 && G.Powered && CanSchedule(&Parent.Runtime.Adapter));
    CHECK(PowerRequest(PowerDeviceD2,PowerActionSleep)==STATUS_SUCCESS && PowerOffCalls==2 && !G.Powered);
    CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS && PowerOnCalls==2 && G.Powered);
    CHECK(PowerRequest(PowerDeviceD3,PowerActionShutdown)==STATUS_SUCCESS && LastPowerAction==PowerActionShutdown);
    Reads=G.Reads;Writes=G.Writes;ReleaseStatus=STATUS_TIMEOUT;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_UNSUCCESSFUL && BoardLive && MapLive && PowerRefs==1 && BoardRefs==1);
    CHECK(!Parent.Runtime.Stages && !Parent.Core.Started && !Parent.CoreTouched && Parent.PowerState==PowerDeviceD3);
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_DEVICE_BUSY);
    ReleaseStatus=STATUS_SUCCESS;UnmapStatus=STATUS_UNSUCCESSFUL;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_UNSUCCESSFUL && !BoardLive && MapLive && PowerRefs==1 && BoardRefs==1);
    UnmapStatus=STATUS_SUCCESS;CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS && G.Reads==Reads && G.Writes==Writes);Empty();
    ResetAttempt();PowerQueryStatus=STATUS_SUCCESS;CHECK(StartParent()==STATUS_SUCCESS);
    PowerOffStatus=STATUS_IO_DEVICE_ERROR;PowerPartialChange=1;
    CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==STATUS_IO_DEVICE_ERROR && !G.Powered && Parent.PowerState==PowerDeviceUnspecified);
    Reads=G.Reads;Writes=G.Writes;
    CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS && G.Reads==Reads && G.Writes==Writes);Empty();
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS);
}
static void PowerOffMmioGuards(void) {
    Add();ResetAttempt();
    for(UINT Unknown=0;Unknown<2;Unknown++) {
        ResetAttempt();PowerQueryStatus=STATUS_SUCCESS;CHECK(StartParent()==STATUS_SUCCESS);
        if(Unknown){PowerOffStatus=STATUS_IO_DEVICE_ERROR;PowerPartialChange=1;}
        CHECK(PowerRequest(PowerDeviceD3,PowerActionSleep)==(Unknown?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS) && !G.Powered);
        CHECK(Parent.PowerState==(Unknown?PowerDeviceUnspecified:PowerDeviceD3));
        UINT Reads=G.Reads,Writes=G.Writes,Ready=ReadyCalls;
        CHECK(!MpReady(&Parent) && ReadyCalls==Ready);
        CHECK(MpRead(&Parent,0)==UINT32_MAX && Parent.IoFault);MpWrite(&Parent,0x30,1);
        CHECK(G.Reads==Reads && G.Writes==Writes);
        CHECK(PowerRequest(PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_NOT_READY && G.Powered && Parent.PowerState==PowerDeviceD0);
        CHECK(G.Reads==Reads && G.Writes==Writes && Parent.Runtime.Suspended && !Parent.Core.Started && !Parent.CoreTouched);
        CHECK(Registered.DxgkDdiStopDevice(&Parent)==STATUS_SUCCESS && G.Reads==Reads && G.Writes==Writes);Empty();
    }
    CHECK(Registered.DxgkDdiRemoveDevice(&Parent)==STATUS_SUCCESS);
}
int TestMain(void) {
    EntryAndInput();if(FirstFailure)return (int)FirstFailure;
    AcquisitionFailures();if(FirstFailure)return (int)FirstFailure;
    LifecycleAndInterrupts();if(FirstFailure)return (int)FirstFailure;
    FailureOwnershipAndRetry();if(FirstFailure)return (int)FirstFailure;
    PowerInterfaceFailures();if(FirstFailure)return (int)FirstFailure;
    PowerInputsAndOffCleanup();if(FirstFailure)return (int)FirstFailure;
    PowerRoundTrip();if(FirstFailure)return (int)FirstFailure;
    PowerSuspendFailures();if(FirstFailure)return (int)FirstFailure;
    PowerResumeFailures();if(FirstFailure)return (int)FirstFailure;
    PowerOffMmioGuards();return (int)FirstFailure;
}
