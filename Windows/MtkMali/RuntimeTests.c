/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define MALI_VM_TEST_COUNTER MtkMaliRuntimeAssertionCount
#define _VCRTIMP
#include "Runtime.h"
#include "TestResidency.h"
static MALI_RUNTIME Runtime;
static MALI_DDI_WORKER *TestWorker=&Runtime.Worker;
#include "TestWorker.h"
#include "Runtime.c"
static DXGKRNL_INTERFACE Kernel;
static MALI_WDDM_CACHE_OPS RuntimeCache;
static MALI_RUNTIME_CONFIG Config;
static CM_PARTIAL_RESOURCE_DESCRIPTOR RuntimeIrqs[3];
static DXGKDDI_FEATURE_INTERFACE Borrowed;
static unsigned FeatureDisabled,KeepInterface;
static NTSTATUS APIENTRY RuntimeFeature(DXGKARGCB_QUERYFEATURESUPPORT *A) {
    CHECK(!GateHeld && A && A->DeviceHandle==(HANDLE)0x777 && A->FeatureId==DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD);
    QUERY_INTERFACE Q={0};Q.InterfaceType=&MaliFeatureGuid;Q.Version=1;Q.Size=sizeof(Borrowed);
    Q.Interface=(PINTERFACE)&Borrowed;Q.DeviceUid=DISPLAY_ADAPTER_HW_ID;
    CHECK(MtkMaliDdiQueryInterface(&Runtime.Adapter,&Q)==STATUS_SUCCESS);
    DXGKARG_QUERYFEATURESUPPORT F={0};F.FeatureId=A->FeatureId;
    CHECK(Borrowed.QueryFeatureSupport(Borrowed.Context,&F)==STATUS_SUCCESS && F.SupportedOnCurrentConfig);
    if(!KeepInterface)Borrowed.InterfaceDereference(Borrowed.Context);
    A->Enabled=(BOOLEAN)!FeatureDisabled;return STATUS_SUCCESS;
}
static void InitRuntime(void) {
    CHECK(!Runtime.Stages && !PrivateLive && !SharedLive && !TestPoolLive && !VmPoolLive && !GateHeld);
    RtlZeroMemory(&Runtime,sizeof(Runtime));Irql=PASSIVE_LEVEL;
    ThreadCreateFailure=ThreadWaitFailure=ThreadCloseFailure=Terminated=Closed=Signals=0;
    CounterTicks=0;CounterFrequency=1000;FeatureDisabled=KeepInterface=0;
    TestVmSyncFailure=TestVmSyncCalls=TestVmFailSyncAt=0;
    TestPoolCalls=TestPoolFrees=TestPoolFail=TestPoolCursor=0;VmPoolCalls=VmPoolFrees=VmPoolFail=0;
    MdlCalls=MdlFrees=LockedPages=UnlockedPages=MemoryCalls=MemoryFrees=FlushCalls=0;
    FailMdl=FailMap=FailPfn=FailMemory=MemoryAlias=0;
    RtlZeroMemory(OsTables,4u*4096u);for(UINT I=0;I<256;I++)OsPhysical[I]=0x40001000+(uint64_t)I*4096;
    for(UINT I=0;I<OS_SHARED_PAGES;I++)SharedPhysical[I]=0x50000000+(uint64_t)I*3u*4096;
    Init(&G,&Core);G.Reg[0xc4/4]=12;CHECK(MaliJmStart(&Core)==MaliOk);
    RtlZeroMemory(&Kernel,sizeof(Kernel));Kernel.Size=sizeof(Kernel);Kernel.DeviceHandle=(HANDLE)0x777;
    Kernel.Version=DXGKDDI_INTERFACE_VERSION_WDDM3_1;Kernel.DxgkCbQueryFeatureSupport=RuntimeFeature;
    Kernel.DxgkCbSynchronizeExecution=VmSync;Kernel.DxgkCbNotifyInterrupt=VmNotify;
    Kernel.DxgkCbQueueDpc=VmDpc;Kernel.DxgkCbNotifyDpc=VmNotifyDpc;Kernel.DxgkCbGetHandleData=VmHandleData;
    RuntimeCache.Context=&Adapter;RuntimeCache.CleanForGpu=VmClean;RuntimeCache.StoreBarrier=VmBarrier;
    RtlZeroMemory(&Config,sizeof(Config));
    Config.AddressModel=MaliVmDirectSystemPhysical;Config.InterruptMessage=0;Config.TimeoutTicks=100;Config.ApertureCommitLimit=64u*1024u*1024u;
}
static NTSTATUS StartRuntime(void) {return MtkMaliRuntimeStart(&Runtime,&Core,&Kernel,&RuntimeCache,&Config);}
static void BasicAndFailureCleanup(void) {
    InitRuntime();CHECK(StartRuntime()==STATUS_SUCCESS && Runtime.Stages && Runtime.Worker.Thread && Runtime.Adapter.Vm==&Runtime.Vm);
    CHECK(Runtime.Adapter.Hooks.Context==&Runtime.Prepare && Runtime.Prepare.Ops.Context==&Runtime.Residency);
    CHECK(Runtime.Residency.WakeContext==&Runtime.Worker && Runtime.Vm.Book==&Runtime.Adapter.Allocations);
    CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_SUCCESS && Runtime.PowerStatus==STATUS_SUCCESS);
    CHECK(StartRuntime()==STATUS_DEVICE_BUSY);Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_INVALID_PARAMETER && Runtime.Worker.Thread);
    CHECK(StartRuntime()==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS && !Runtime.Stages && !Runtime.Worker.Thread && !Runtime.Adapter.Vm);
    CHECK(Core.Started);CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS);
    CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_DEVICE_NOT_READY);
    CHECK(StartRuntime()==STATUS_SUCCESS && !Terminated);CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS && Terminated==1);
    for(UINT Failure=0;Failure<7;Failure++) {
        InitRuntime();NTSTATUS Expected=STATUS_INVALID_PARAMETER;
        switch(Failure) {
        case 0:Config.TimeoutTicks=0;break;
        case 1:Config.ApertureCommitLimit=4097;break;
        case 2:Config.AddressModel=MaliVmAddressingUnknown;Expected=STATUS_NOT_SUPPORTED;break;
        case 3:Kernel.DxgkCbQueueDpc=0;break;
        case 4:FeatureDisabled=1;Expected=STATUS_NOT_SUPPORTED;break;
        case 5:CounterFrequency=0;break;
        case 6:ThreadCreateFailure=1;Expected=STATUS_INSUFFICIENT_RESOURCES;break;
        }
        CHECK(StartRuntime()==Expected && Runtime.StartStatus==Expected && Runtime.CleanupStatus==STATUS_SUCCESS);
        CHECK(!Runtime.Stages && !Runtime.Worker.Thread && !Runtime.Prepare.Signature && !Runtime.Residency.Signature);
        CHECK(!PrivateLive && !SharedLive && !TestPoolLive && !VmPoolLive && !GateHeld);
    }
    InitRuntime();KeepInterface=FeatureDisabled=1;
    CHECK(StartRuntime()==STATUS_NOT_SUPPORTED && Runtime.Stages && Runtime.CleanupStatus==STATUS_DEVICE_BUSY);
    CHECK(Runtime.Adapter.InterfaceReferences==1 && !Runtime.Worker.Thread);
    CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_DEVICE_NOT_READY && Runtime.PowerStatus==STATUS_DEVICE_NOT_READY);
    Borrowed.InterfaceDereference(Borrowed.Context);CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS && !Runtime.Stages);
    InitRuntime();CHECK(StartRuntime()==STATUS_SUCCESS);ThreadWaitFailure=1;
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_UNSUCCESSFUL && Runtime.Stages && Runtime.Worker.Thread && !CanSchedule(&Runtime.Adapter));
    CHECK(Runtime.Residency.Signature && Runtime.Vm.Signature && Runtime.Prepare.Signature && !Closed);
    ThreadWaitFailure=0;ThreadCloseFailure=1;
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_UNSUCCESSFUL && Runtime.Stages && Runtime.Worker.Thread && Terminated==1);
    ThreadCloseFailure=0;CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS && !Runtime.Stages && Closed==1);
}
static void EndToEndSubmission(void) {
    InitRuntime();CHECK(StartRuntime()==STATUS_SUCCESS);MALI_DDI_ADAPTER *D=&Runtime.Adapter;
    DXGKARG_CREATEPROCESS P={0};P.hDxgkProcess=(HANDLE)0x1234;
    CHECK(MtkMaliDdiCreateProcess(D,&P)==STATUS_SUCCESS);Proc[0]=P.hKmdProcess;
    DXGKARG_CREATEDEVICE V={0};V.hKmdProcess=Proc[0];CHECK(MtkMaliDdiCreateDevice(D,&V)==STATUS_SUCCESS);Dev[0]=V.hDevice;
    MALI_ALLOCATION_DESC Desc={1,sizeof(Desc),1,3,8192,0,0,0,0};DXGK_ALLOCATIONINFO Info={0};
    Info.pPrivateDriverData=&Desc;Info.PrivateDriverDataSize=sizeof(Desc);
    DXGKARG_CREATEALLOCATION A={0};A.Flags.Resource=1;A.NumAllocations=1;A.pAllocationInfo=&Info;
    CHECK(MtkMaliDdiCreateAllocation(D,&A)==STATUS_SUCCESS);AllocationHandle=Info.hAllocation;ResourceHandle=A.hResource;
    DXGK_OPENALLOCATIONINFO OI={0};OI.hAllocation=10;DXGKARG_OPENALLOCATION O={0};O.NumAllocations=1;O.pOpenAllocation=&OI;
    CHECK(MtkMaliDdiOpenAllocation(Dev[0],&O)==STATUS_SUCCESS);OpenHandle=OI.hDeviceSpecificAllocation;
    DXGKARG_SETALLOCATIONBACKINGSTORE B={AllocationHandle,SharedData};CHECK(MtkMaliDdiSetAllocationBackingStore(D,&B)==STATUS_SUCCESS);
    DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(MtkMaliDdiCreateContext(Dev[0],&C)==STATUS_SUCCESS);Ctx=C.hContext;
    DXGKARG_SETROOTPAGETABLE R={0};R.hContext=Ctx;R.NumEntries=512;R.Address.SegmentOffset=OsPhysical[0];
    MtkMaliDdiSetRootPageTable(D,&R);CHECK(D->LastRootStatus==STATUS_SUCCESS);
    for(UINT L=0;L<3;L++) {
        Entry(0,OsPhysical[L+1],0,0);Update(L,3-L,0,(UINT)((0x80000000ULL>>(39-L*9))&511),1,1,0,0);
        CHECK(MtkMaliDdiBuildPagingBuffer(D,&Build)==STATUS_SUCCESS);
    }
    Entry(0,SharedPhysical[0],1,0);Entry(1,SharedPhysical[1],1,0);Update(3,0,0,0,2,1,AllocationHandle,0x80000000);
    CHECK(MtkMaliDdiBuildPagingBuffer(D,&Build)==STATUS_SUCCESS);
    RtlZeroMemory(SharedData,8192);TestPut32(SharedData+16,(71u<<16)|4u);TestPut64(SharedData+32,0x80001000);
    TestPut32(SharedData+40,7);TestPut64(SharedData+48,0x123456789abcdefULL);
    RtlZeroMemory(&Private,sizeof(Private));Private.Version=1;Private.Bytes=MALI_EXEC_PACKET_BYTES(1);
    Private.StepCount=1;Private.Steps[0].Slot=1;Private.Steps[0].Chain=0x80000000;
    DXGKARG_SUBMITCOMMANDVIRTUAL S={0};S.hContext=Ctx;S.DmaBufferVirtualAddress=0x80000000;S.DmaBufferSize=4096;
    S.pDmaBufferPrivateData=&Private;S.DmaBufferPrivateDataSize=Private.Bytes;S.SubmissionFenceId=1;
    CHECK(MtkMaliDdiSubmitCommandVirtual(D,&S)==STATUS_SUCCESS && Runtime.Residency.Count==1 && Signals==1);
    CHECK(Runtime.Vm.HoldCount==1 && LockedPages==2 && D->Allocations.ReferenceCount==2);
    CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_DEVICE_BUSY && !Runtime.Suspended && CanSchedule(D));
    CHECK(MtkMaliDdiPump(D,1)==STATUS_SUCCESS);Completion(&G,1,1,0);TestVmSyncFailure=1;
    CHECK(MtkMaliDdiPump(D,2)==STATUS_UNSUCCESSFUL && Runtime.Residency.Count==1 && PrivateLive);
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_DEVICE_BUSY && Runtime.Worker.Thread && CanSchedule(D));
    TestVmSyncFailure=0;CHECK(MtkMaliDdiPump(D,3)==STATUS_SUCCESS && !Runtime.Residency.Count && !PrivateLive && !SharedLive);
    CHECK(LockedPages==UnlockedPages && !Runtime.Vm.HoldCount && !D->Allocations.ReferenceCount);
    uint64_t SavedId=((MALI_DDI_CONTEXT*)Ctx)->Id;UINT Tables=Runtime.Vm.TableCount;
    for(UINT Cycle=0;Cycle<3;Cycle++) {
        CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_SUCCESS && Runtime.Suspended && D->PowerPaused);
        CHECK(!Runtime.Worker.Thread && !Core.Started && !Core.BoundSpaces && !CanSchedule(D));
        CHECK(Runtime.Vm.TableCount==Tables && Runtime.Adapter.Vm==&Runtime.Vm && Runtime.Residency.Signature);
        CHECK(((MALI_DDI_CONTEXT*)Ctx)->Id==SavedId && ((MALI_DDI_CONTEXT*)Ctx)->Root==OsPhysical[0]);
        CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_DEVICE_BUSY && Runtime.Suspended);
        G.Powered=0;UINT Reads=G.Reads,Writes=G.Writes;
        DXGK_DRIVERCAPS Caps={0};DXGKARG_QUERYADAPTERINFO Query={0};Query.Type=DXGKQAITYPE_DRIVERCAPS;
        Query.pOutputData=&Caps;Query.OutputDataSize=sizeof(Caps);
        CHECK(MtkMaliDdiQueryAdapterInfo(D,&Query)==STATUS_SUCCESS && Caps.MemoryManagementCaps.GpuMmuSupported);
        DXGKARG_GETNODEMETADATA Node={0};CHECK(MtkMaliDdiGetNodeMetadata(D,0,&Node)==STATUS_SUCCESS && Node.GpuMmuSupported);
        DXGKARG_RESETENGINE Reset={0};CHECK(MtkMaliDdiResetEngine(D,&Reset)==STATUS_DEVICE_NOT_READY);
        CHECK(MtkMaliDdiSubmitCommandVirtual(D,&S)==STATUS_DEVICE_NOT_READY);
        DXGKARG_PREEMPTCOMMAND Preempt={0};Irql=DISPATCH_LEVEL;
        CHECK(MtkMaliDdiPreemptCommand(D,&Preempt)==STATUS_DEVICE_NOT_READY);Irql=PASSIVE_LEVEL;
        CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_SUCCESS);
        CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_DEVICE_NOT_READY && Runtime.Suspended);
        CHECK(G.Reads==Reads && G.Writes==Writes && !Runtime.Worker.Thread && !Core.Started);
        G.Powered=1;
        CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_SUCCESS && !Runtime.Suspended && !D->PowerPaused && Core.Started);
        CHECK(Runtime.Worker.Thread && CanSchedule(D) && Runtime.Vm.TableCount==Tables);
        CHECK(((MALI_DDI_CONTEXT*)Ctx)->Id==SavedId && ((MALI_DDI_CONTEXT*)Ctx)->Root==OsPhysical[0]);
        S.SubmissionFenceId=2+Cycle;
        CHECK(MtkMaliDdiSubmitCommandVirtual(D,&S)==STATUS_SUCCESS);
        CHECK(MtkMaliDdiPump(D,4+Cycle*2)==STATUS_SUCCESS);Completion(&G,1,1,0);
        CHECK(MtkMaliDdiPump(D,5+Cycle*2)==STATUS_SUCCESS && D->Queue.LastCompletedFence==2+Cycle);
        CHECK(!Runtime.Residency.Count && !Runtime.Vm.HoldCount && !D->Allocations.ReferenceCount);
    }
    CHECK(MtkMaliDdiDestroyContext(Ctx)==STATUS_SUCCESS);
    DXGKARG_CLOSEALLOCATION Close={1,&OpenHandle};CHECK(MtkMaliDdiCloseAllocation(Dev[0],&Close)==STATUS_SUCCESS);
    DXGKARG_DESTROYALLOCATION Destroy={0};Destroy.NumAllocations=1;Destroy.pAllocationList=&AllocationHandle;
    Destroy.hResource=ResourceHandle;Destroy.Flags.DestroyResource=1;CHECK(MtkMaliDdiDestroyAllocation(D,&Destroy)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyDevice(Dev[0])==STATUS_SUCCESS && MtkMaliDdiDestroyProcess(D,Proc[0])==STATUS_SUCCESS);
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS && !Runtime.Stages && !TestPoolLive && !VmPoolLive);
    CHECK(MaliJmStop(&Core)==MaliOk && !Core.Started);
}
static unsigned InterruptSyncFailure;
static NTSTATUS APIENTRY RuntimeInterruptSync(HANDLE H,PKSYNCHRONIZE_ROUTINE F,PVOID X,ULONG M,PBOOLEAN Result) {
    if(F==IrqAtInterrupt && InterruptSyncFailure==1)return STATUS_UNSUCCESSFUL;
    NTSTATUS S=VmSync(H,F,X,M,Result);
    return F==IrqAtInterrupt && InterruptSyncFailure==2?STATUS_UNSUCCESSFUL:S;
}
static void InitManagedRuntime(void) {
    InitRuntime();CHECK(MaliJmStop(&Core)==MaliOk && MaliJmSetManagedInterrupts(&Core,1)==MaliOk);
    CHECK(MaliJmStart(&Core)==MaliOk && !G.Reg[0x28/4] && !G.Reg[0x1008/4] && !G.Reg[0x2008/4]);
    InterruptSyncFailure=0;Kernel.DxgkCbSynchronizeExecution=RuntimeInterruptSync;
    TestIrqResources(RuntimeIrqs);Config.Interrupts=RuntimeIrqs;Config.InterruptCount=3;
}
static void ManagedInterruptLifetime(void) {
    InitManagedRuntime();Config.InterruptCount=2;
    CHECK(StartRuntime()==STATUS_INVALID_PARAMETER && !Runtime.Stages && !Runtime.Worker.Thread);
    CHECK(!PrivateLive && !SharedLive && !TestPoolLive && !VmPoolLive);
    InitManagedRuntime();RuntimeIrqs[2].u.Interrupt.Level=PROFILE_LEVEL;
    CHECK(StartRuntime()==STATUS_DEVICE_CONFIGURATION_ERROR && !Runtime.Stages && !Runtime.Worker.Thread);
    InitManagedRuntime();CHECK(StartRuntime()==STATUS_SUCCESS && Runtime.Adapter.InterruptSyncIrql==7);
    CHECK((Runtime.Stages&RUNTIME_INTERRUPTS) && IrqConnected(&Runtime.Adapter) && Runtime.Worker.Thread);
    CHECK(G.Reg[0x28/4]==0x81 && G.Reg[0x1008/4]==0x70007 && G.Reg[0x2008/4]==0xff00ff);
    G.Reg[0x20/4]=1;Irql=5;CHECK(MtkMaliDdiInterruptRoutine(&Runtime.Adapter,0));
    Irql=DISPATCH_LEVEL;unsigned Before=Signals;MtkMaliDdiDpcRoutine(&Runtime.Adapter);Irql=PASSIVE_LEVEL;
    CHECK(Signals==Before+1 && Runtime.Adapter.InterruptPending==1);
    CHECK(MtkMaliDdiPump(&Runtime.Adapter,1)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && !Runtime.Adapter.InterruptPending);
    InterruptSyncFailure=1;CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_UNSUCCESSFUL);
    CHECK(Runtime.Stages && Runtime.Worker.Thread && IrqConnected(&Runtime.Adapter) && !Closed);
    InterruptSyncFailure=0;CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS && !Runtime.Stages);
    CHECK(!IrqConnected(&Runtime.Adapter) && !G.Reg[0x28/4] && !G.Reg[0x1008/4] && !G.Reg[0x2008/4]);
    Before=Signals;Irql=5;CHECK(!MtkMaliDdiInterruptRoutine(&Runtime.Adapter,0));
    Irql=DISPATCH_LEVEL;MtkMaliDdiDpcRoutine(&Runtime.Adapter);Irql=PASSIVE_LEVEL;CHECK(Signals==Before);
    InitManagedRuntime();InterruptSyncFailure=1;
    CHECK(StartRuntime()==STATUS_UNSUCCESSFUL && Runtime.StartStatus==STATUS_UNSUCCESSFUL);
    CHECK(!Runtime.Stages && !Runtime.Worker.Thread && !IrqConnected(&Runtime.Adapter));
    InitManagedRuntime();InterruptSyncFailure=2;
    CHECK(StartRuntime()==STATUS_UNSUCCESSFUL && Runtime.StartStatus==STATUS_UNSUCCESSFUL);
    CHECK(Runtime.Stages && Runtime.Worker.Thread && IrqConnected(&Runtime.Adapter));
    CHECK(Runtime.CleanupStatus==STATUS_UNSUCCESSFUL && !Closed);
    InterruptSyncFailure=0;CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS && !Runtime.Stages && Closed==1);
}
static unsigned InjectPowerPreempt;
static uint32_t PowerRaceRead(void *X,uint32_t Offset) {
    uint32_t Value=Read(X,Offset);
    if(Offset==0x20 && InjectPowerPreempt) {
        InjectPowerPreempt=0;KIRQL Before=Irql;Irql=DISPATCH_LEVEL;
        DXGKARG_PREEMPTCOMMAND A={0};A.PreemptionFenceId=42;
        CHECK(MtkMaliDdiPreemptCommand(&Runtime.Adapter,&A)==STATUS_SUCCESS);Irql=Before;
    }
    return Value;
}
static void PowerAdmissionAndFailureRetention(void) {
    InitManagedRuntime();CHECK(StartRuntime()==STATUS_SUCCESS);
    MALI_DDI_ADAPTER *D=&Runtime.Adapter;
    for(UINT Failure=0;Failure<9;Failure++) {
        switch(Failure) {
        case 0:D->Queue.Count=1;break;case 1:D->Queue.EventPending=1;break;
        case 2:D->Queue.PreemptionPending=1;break;case 3:Runtime.Vm.HoldCount=1;break;
        case 4:D->Allocations.ReferenceCount=1;break;case 5:D->Resident[0].Live=1;break;
        case 6:D->Queue.Contexts[0].References=1;break;case 7:Core.Jobs[0].Active=1;break;
        case 8:InterlockedOr64(&D->SchedulerState,REQUEST_BIT|42);break;
        }
        UINT Writes=G.Writes;
        CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_DEVICE_BUSY && !Runtime.Suspended && CanSchedule(D));
        CHECK(G.Writes==Writes && Runtime.Worker.Thread && IrqConnected(D));
        D->Queue.Count=D->Queue.EventPending=D->Queue.PreemptionPending=0;
        Runtime.Vm.HoldCount=D->Allocations.ReferenceCount=D->Resident[0].Live=D->Queue.Contexts[0].References=Core.Jobs[0].Active=0;
        InterlockedExchange64(&D->SchedulerState,READY_BIT);
    }
    Core.Io.Read32=PowerRaceRead;InjectPowerPreempt=1;
    CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_DEVICE_BUSY && !Runtime.Suspended && CanSchedule(D));
    CHECK(!InjectPowerPreempt && (State(D)&REQUEST_BIT) && (uint32_t)State(D)==42 && Runtime.Worker.Thread);
    Core.Io.Read32=Read;CHECK(MtkMaliDdiPump(D,1)==STATUS_SUCCESS && State(D)==READY_BIT);
    Irql=DISPATCH_LEVEL;CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    Borrowed.Context=D;DXGKARG_CREATEPROCESS P={0};P.hDxgkProcess=(HANDLE)0x1234;
    CHECK(MtkMaliDdiCreateProcess(D,&P)==STATUS_SUCCESS);D->InterfaceReferences=1;
    CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_SUCCESS && D->InterfaceReferences==1 && Process(D,P.hKmdProcess));
    CHECK(Runtime.Suspended && !Core.Started && !Runtime.Worker.Thread && !IrqConnected(D));
    CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_SUCCESS && CanSchedule(D) && IrqConnected(D) && Runtime.Worker.Thread);
    CHECK(D->InterfaceReferences==1 && Process(D,P.hKmdProcess));D->InterfaceReferences=0;
    CHECK(MtkMaliDdiDestroyProcess(D,P.hKmdProcess)==STATUS_SUCCESS);
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS);
    for(UINT Failure=0;Failure<5;Failure++) {
        InitManagedRuntime();CHECK(StartRuntime()==STATUS_SUCCESS);D=&Runtime.Adapter;
        switch(Failure) {
        case 0:InterruptSyncFailure=1;break;case 1:InterruptSyncFailure=2;break;
        case 2:ThreadWaitFailure=1;break;case 3:ThreadCloseFailure=1;break;case 4:G.CacheStuck=1;break;
        }
        NTSTATUS Expected=Failure==4?STATUS_IO_TIMEOUT:STATUS_UNSUCCESSFUL;
        CHECK(MtkMaliRuntimeSuspend(&Runtime)==Expected && Runtime.PowerStatus==Expected);
        CHECK(Runtime.Suspended && D->PowerPaused && !CanSchedule(D) && Runtime.Vm.Signature && Runtime.Residency.Signature);
        CHECK(Core.Started && G.Powered);
        InterruptSyncFailure=ThreadWaitFailure=ThreadCloseFailure=0;G.CacheStuck=0;
        CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_SUCCESS && !Core.Started && !Core.Poisoned);
        CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_SUCCESS && CanSchedule(D) && IrqConnected(D));
        CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS);
    }
    for(UINT Failure=0;Failure<7;Failure++) {
        InitManagedRuntime();CHECK(StartRuntime()==STATUS_SUCCESS);D=&Runtime.Adapter;
        CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_SUCCESS);
        switch(Failure) {
        case 0:ThreadCreateFailure=1;break;case 1:InterruptSyncFailure=1;break;
        case 2:InterruptSyncFailure=2;break;case 3:G.ResetStuck=1;break;
        case 4:G.Reg[0x100/4]=7;break;case 5:G.Powered=0;break;case 6:G.Reg[0xc4/4]=28;break;
        }
        NTSTATUS Expected=Failure==0?STATUS_INSUFFICIENT_RESOURCES:Failure<=2?STATUS_UNSUCCESSFUL:
            Failure==3?STATUS_IO_TIMEOUT:Failure==5?STATUS_DEVICE_NOT_READY:STATUS_DEVICE_CONFIGURATION_ERROR;
        CHECK(MtkMaliRuntimeResume(&Runtime)==Expected && Runtime.PowerStatus==Expected);
        CHECK(Runtime.Suspended && D->PowerPaused && !CanSchedule(D) && Runtime.Vm.Signature && Runtime.Residency.Signature);
        CHECK(D->PowerCaps.Shaders==5 && D->PowerCaps.JobFeatures[1]==12);
        MALI_ADAPTER_QUERY In={1,sizeof(In),0,0};MALI_ADAPTER_CAPS Out={0};DXGKARG_QUERYADAPTERINFO Q={0};
        Q.Type=DXGKQAITYPE_UMDRIVERPRIVATE;Q.pInputData=&In;Q.InputDataSize=sizeof(In);Q.pOutputData=&Out;Q.OutputDataSize=sizeof(Out);
        UINT Reads=G.Reads,Writes=G.Writes;
        CHECK(MtkMaliDdiQueryAdapterInfo(D,&Q)==STATUS_SUCCESS && Out.Features==7 && Out.GpuId==0x90930002);
        CHECK(G.Reads==Reads && G.Writes==Writes);
        ThreadCreateFailure=InterruptSyncFailure=0;G.ResetStuck=0;G.Reg[0x100/4]=5;G.Reg[0xc4/4]=12;G.Powered=1;
        CHECK(MtkMaliRuntimeResume(&Runtime)==STATUS_SUCCESS && Runtime.Worker.Thread && Core.Started && !Runtime.Suspended);
        CHECK(CanSchedule(D) && IrqConnected(D) && !Core.Poisoned && Runtime.PowerCleanupStatus==STATUS_SUCCESS);
        CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS);
    }
    InitManagedRuntime();CHECK(StartRuntime()==STATUS_SUCCESS);G.Reg[0x20/4]=1;
    CHECK(MtkMaliRuntimeSuspend(&Runtime)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE);
    CHECK(!Runtime.Suspended && Runtime.Adapter.Queue.Faulted && Core.Poisoned && Runtime.Worker.Thread);
    CHECK(MtkMaliRuntimeStop(&Runtime)==STATUS_SUCCESS);
}
int TestMain(void) {
    BasicAndFailureCleanup();if(FirstFailure)return (int)FirstFailure;
    EndToEndSubmission();if(FirstFailure)return (int)FirstFailure;
    ManagedInterruptLifetime();if(FirstFailure)return (int)FirstFailure;
    PowerAdmissionAndFailureRetention();return (int)FirstFailure;
}
