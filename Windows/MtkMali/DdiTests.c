/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real production DDIs; only kernel synchronization, command preparation,
   cache hooks, GPU MMIO and the Dxgkrnl callbacks are test boundaries. */
#include "Worker.h"
static unsigned FirstFailure;
unsigned MtkMaliDdiAssertionCount;
#define CHECK(X) do { ++MtkMaliDdiAssertionCount; if(!(X) && !FirstFailure) FirstFailure=__LINE__; } while(0)
#include "TestGpu.h"
static KIRQL Irql;
static MALI_DDI_WORKER Worker;
static MALI_DDI_WORKER *ActiveWorker;
static NTSTATUS (*EventWait)(PLARGE_INTEGER Timeout);
static unsigned GateHeld,Waits,WakeCount,Prepared,Released,Notified,DpcsQueued,DpcsNotified;
static unsigned SyncFailure,PrepareFailure,QueueDpcFalse,CacheClean,Barriers,SyncPreemptFence;
static NTSTATUS TestWait(PVOID Object,KWAIT_REASON Why,KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    CHECK(Object && Why==Executive && Mode==KernelMode && !Alert);
    CHECK(Irql==PASSIVE_LEVEL && !GateHeld);
    if(Object==&Worker.Signal){CHECK(EventWait!=0);return EventWait ? EventWait(Timeout) : STATUS_UNSUCCESSFUL;}
    CHECK(!Timeout);GateHeld=1;++Waits;return STATUS_SUCCESS;
}
static LONG TestRelease(PKMUTEX Mutex,BOOLEAN Wait) {CHECK(Mutex && !Wait && GateHeld);GateHeld=0;return 0;}
static void TestInitMutex(PKMUTEX Mutex,ULONG Level) {CHECK(Mutex && !Level);}
#define KeGetCurrentIrql() Irql
#define KeWaitForSingleObject TestWait
#define KeReleaseMutex TestRelease
#define KeInitializeMutex TestInitMutex
#include "TestPool.h"
#include "Allocation.c"
#include "Vm.c"
#include "Ddi.c"
static MALI_DDI_ADAPTER Adapter;
static GPU G;
static MALI_JM D;
static DXGKRNL_INTERFACE K;
static DXGKARGCB_NOTIFY_INTERRUPT_DATA LastNotify;
static HANDLE P,V,C;
static unsigned Lease[40],Reasons[5];
static MALI_EXEC_PACKET Private;
static unsigned ManagedMode,SyncIrqFailure,SyncInjectBank,SyncInjectBits,SyncInjectFire;
static KIRQL SyncLevel=5;
static uint32_t DropMaskOffset=UINT32_MAX,DropMaskValue;
static uint32_t InterruptRead(void *X,uint32_t O) {
    if(Irql>=DISPATCH_LEVEL) {
        CHECK(O==0x28 || O==0x1008 || O==0x2008 || O==0x2c || O==0x100c || O==0x200c);
        CHECK(Irql==Adapter.InterruptSyncIrql && TestIrqLockHeld && TestIrqOwnedLock==&Adapter.InterruptGate);
    }
    return Read(X,O);
}
static void InterruptWrite(void *X,uint32_t O,uint32_t Value) {
    if(Value && (O==0x28 || O==0x1008 || O==0x2008))CHECK(Irql>DISPATCH_LEVEL);
    if(Irql>=DISPATCH_LEVEL) {
        CHECK(O==0x28 || O==0x1008 || O==0x2008);
        CHECK(Irql==Adapter.InterruptSyncIrql && TestIrqLockHeld && TestIrqOwnedLock==&Adapter.InterruptGate);
    }
    if(O==DropMaskOffset && Value==DropMaskValue)return;
    Write(X,O,Value);
}
static int InterruptPowerReady(void *X) {CHECK(Irql==PASSIVE_LEVEL);return Ready(X);}

static NTSTATUS APIENTRY Sync(HANDLE H,PKSYNCHRONIZE_ROUTINE Routine,PVOID Data,ULONG Message,PBOOLEAN Return) {
    CHECK(H==(HANDLE)0x777 && Message==0 && Irql==PASSIVE_LEVEL && GateHeld);
    unsigned Failure=Routine==IrqAtInterrupt && SyncIrqFailure?SyncIrqFailure:SyncFailure;
    if(Failure==1){*Return=FALSE;return STATUS_UNSUCCESSFUL;}
    KIRQL Saved=Irql;Irql=SyncLevel;*Return=Routine(Data);
    CHECK(Irql==SyncLevel && !TestIrqLockHeld);
    if(Routine==IrqAtInterrupt && ((IRQ_CHANGE*)Data)->Action==IrqRearm && SyncInjectBank) {
        static const uint32_t Raw[3]={0x20,0x1000,0x2000};
        G.Reg[Raw[SyncInjectBank-1]/4]|=SyncInjectBits;SyncInjectBank=0;
        if(SyncInjectFire)CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0));
    }
    Irql=Saved;
    if(SyncPreemptFence) {
        DXGKARG_PREEMPTCOMMAND A={0};A.PreemptionFenceId=SyncPreemptFence;SyncPreemptFence=0;
        Irql=DISPATCH_LEVEL;CHECK(MtkMaliDdiPreemptCommand(&Adapter,&A)==STATUS_SUCCESS);Irql=Saved;
    }
    return Failure==2 ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
static VOID APIENTRY NotifyInterrupt(HANDLE H,DXGKARGCB_NOTIFY_INTERRUPT_DATA *A) {
    CHECK(H==(HANDLE)0x777 && A && Irql==5 && GateHeld);
    LastNotify=*A;++Notified;
}
static BOOLEAN APIENTRY QueueDpc(HANDLE H) {CHECK(H==(HANDLE)0x777 && Irql>DISPATCH_LEVEL && Irql<PROFILE_LEVEL && !TestIrqLockHeld);++DpcsQueued;return QueueDpcFalse?FALSE:TRUE;}
static VOID APIENTRY NotifyDpc(HANDLE H) {CHECK(H==(HANDLE)0x777 && Irql==DISPATCH_LEVEL);++DpcsNotified;}
static void Wake(void *X) {
    CHECK(X==&Adapter && Irql<=DISPATCH_LEVEL && (Irql==DISPATCH_LEVEL || !GateHeld));++WakeCount;
    if(ActiveWorker)MtkMaliWorkerWake(ActiveWorker);
}
static void Clean(void *X,const void *A,size_t N) {CHECK(X==&Adapter && A && N && GateHeld);++CacheClean;}
static void CacheBarrier(void *X) {CHECK(X==&Adapter && GateHeld);++Barriers;}
static NTSTATUS Prepare(void *X,HANDLE Process,uint64_t Id,uint64_t Root,const DXGKARG_SUBMITCOMMANDVIRTUAL *A,
                        MALI_DDI_PREPARED *Out) {
    CHECK(X==&Adapter && Process==(HANDLE)0x1234 && Id && (Root==0x40001000 || Root==0x40002000) && A && Out && !Out->Cookie);
    CHECK(Irql==PASSIVE_LEVEL && GateHeld);++Prepared;
    if(PrepareFailure)return STATUS_ACCESS_DENIED;
    /* Fixture only: production must implement the GPU command/residency
       validator, which these tests deliberately do not pretend to provide. */
    if(A->DmaBufferPrivateDataSize!=MALI_EXEC_PACKET_BYTES(1) || !A->pDmaBufferPrivateData)
        return STATUS_INVALID_PARAMETER;
    Out->Packet=Private;Out->ExecutionRoot=Root;Out->DmaVa=0x90000000;Out->DmaBytes=4096;
    for(unsigned I=0;I<Out->Packet.StepCount && I<MALI_EXEC_MAX_STEPS;I++)
        Out->Packet.Steps[I].Chain+=0x10000000;
    for(unsigned I=0;I<40;I++)if(!Lease[I]){Lease[I]=1;Out->Cookie=&Lease[I];return STATUS_SUCCESS;}
    return STATUS_INSUFFICIENT_RESOURCES;
}
static void Release(void *X,void *Cookie,MALI_DDI_RELEASE_REASON Why) {
    CHECK(X==&Adapter && Cookie && GateHeld && Irql==PASSIVE_LEVEL);
    unsigned *L=Cookie;
    CHECK(L>=Lease && L<Lease+40 && *L==1 && Why>=MaliDdiRejected && Why<=MaliDdiReset);
    for(unsigned I=0;I<3;I++)CHECK(!D.Jobs[I].Active || Why==MaliDdiRejected);
    if(L>=Lease && L<Lease+40)*L=0;
    ++Released;++Reasons[Why];
}
static void Begin(void) {
    Irql=PASSIVE_LEVEL;SyncLevel=5;SyncFailure=0;PrepareFailure=0;QueueDpcFalse=0;SyncPreemptFence=0;
    CHECK(!TestIrqLockHeld && TestIrqAcquires==TestIrqReleases && !TestIrqBeforeAcquire && !TestIrqAfterLower);
    GateHeld=0;RtlZeroMemory(Lease,sizeof(Lease));RtlZeroMemory(Reasons,sizeof(Reasons));
    WakeCount=Prepared=Released=Notified=DpcsQueued=DpcsNotified=CacheClean=Barriers=0;
    DropMaskOffset=UINT32_MAX;SyncIrqFailure=SyncInjectBank=SyncInjectBits=SyncInjectFire=0;
    Init(&G,&D);
    if(ManagedMode) {
        D.Io.Read32=InterruptRead;D.Io.Write32=InterruptWrite;D.Io.PowerReady=InterruptPowerReady;
        CHECK(MaliJmSetManagedInterrupts(&D,1)==MaliOk);
    }
    CHECK(MaliJmStart(&D)==MaliOk);
    RtlZeroMemory(&K,sizeof(K));K.Size=sizeof(K);K.DeviceHandle=(HANDLE)0x777;
    K.DxgkCbSynchronizeExecution=Sync;K.DxgkCbNotifyInterrupt=NotifyInterrupt;
    K.DxgkCbQueueDpc=QueueDpc;K.DxgkCbNotifyDpc=NotifyDpc;
    MALI_DDI_HOOKS Hooks={&Adapter,Prepare,Release,Wake};
    MALI_WDDM_CACHE_OPS Cache={&Adapter,Clean,CacheBarrier};
    CHECK(MtkMaliDdiInitialize(&Adapter,&D,100,&K,0,&Hooks,&Cache)==MaliOk);
    DXGKARG_CREATEPROCESS Process={0};Process.hDxgkProcess=(HANDLE)0x1234;
    CHECK(MtkMaliDdiCreateProcess(&Adapter,&Process)==STATUS_SUCCESS);P=Process.hKmdProcess;
    DXGKARG_CREATEDEVICE Device={0};Device.hKmdProcess=P;
    CHECK(MtkMaliDdiCreateDevice(&Adapter,&Device)==STATUS_SUCCESS);V=Device.hDevice;
    DXGKARG_CREATECONTEXT Context={0};Context.Flags.VirtualAddressing=1;Context.EngineAffinity=1;
    CHECK(MtkMaliDdiCreateContext(V,&Context)==STATUS_SUCCESS);C=Context.hContext;
    CHECK(Context.ContextInfo.Caps.NoPatchingRequired && !Context.ContextInfo.Caps.DriverManagesResidency);
    CHECK(Context.ContextInfo.DmaBufferSize==MALI_DDI_DMA_BYTES && !Context.ContextInfo.DmaBufferSegmentSet);
    DXGKARG_SETROOTPAGETABLE Root={0};Root.hContext=C;Root.Address.SegmentOffset=0x40001000;Root.NumEntries=512;
    MtkMaliDdiSetRootPageTable(&Adapter,&Root);CHECK(Adapter.LastRootStatus==STATUS_SUCCESS);
    RtlZeroMemory(&Private,sizeof(Private));Private.Version=1;Private.Bytes=MALI_EXEC_PACKET_BYTES(1);Private.StepCount=1;
    Private.Steps[0].Chain=0x80000000;
}
static NTSTATUS Submit(unsigned Fence) {
    DXGKARG_SUBMITCOMMANDVIRTUAL A={0};A.hContext=C;A.DmaBufferVirtualAddress=0x80000000;
    A.DmaBufferSize=4096;A.pDmaBufferPrivateData=&Private;A.DmaBufferPrivateDataSize=Private.Bytes;
    A.SubmissionFenceId=Fence;return MtkMaliDdiSubmitCommandVirtual(&Adapter,&A);
}
static void End(void) {
    CHECK(MtkMaliDdiDestroyContext(C)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyDevice(V)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyProcess(&Adapter,P)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_SUCCESS);
    CHECK(!CanSchedule(&Adapter) && !GateHeld);
    for(unsigned I=0;I<40;I++)CHECK(!Lease[I]);
    ManagedMode=0;
}
static void LifecycleAndCompletion(void) {
    Begin();CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliDdiDestroyProcess(&Adapter,P)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliDdiDestroyDevice(V)==STATUS_DEVICE_BUSY);
    CHECK(Submit(UINT32_MAX)==STATUS_SUCCESS && WakeCount==1 && Prepared==1 && !Released);
    CHECK(MtkMaliDdiDestroyContext(C)==STATUS_DEVICE_BUSY);
    Private.Steps[0].Chain+=64;
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && D.Jobs[0].Active);
    CHECK(D.Jobs[0].Chain==0x90000000);Private.Steps[0].Chain-=64;
    DXGKARG_QUERYCURRENTFENCE Fence={0};CHECK(MtkMaliDdiQueryCurrentFence(&Adapter,&Fence)==STATUS_SUCCESS && !Fence.CurrentFence);
    Completion(&G,0,1,0);SyncFailure=1;
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_UNSUCCESSFUL && !D.Jobs[0].Active);
    CHECK(!Notified && !Released && Adapter.Queue.EventPending);
    CHECK(MtkMaliDdiDestroyContext(C)==STATUS_DEVICE_BUSY);
    SyncFailure=0;QueueDpcFalse=1;
    CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && Notified==1 && Released==1 && DpcsQueued==1);
    CHECK(LastNotify.InterruptType==DXGK_INTERRUPT_DMA_COMPLETED && LastNotify.DmaCompleted.SubmissionFenceId==UINT32_MAX);
    CHECK(Reasons[MaliDdiCompleted]==1 && !Adapter.Queue.EventPending);
    CHECK(MtkMaliDdiPump(&Adapter,4)==STATUS_SUCCESS && Notified==1 && Released==1);
    CHECK(MtkMaliDdiQueryCurrentFence(&Adapter,&Fence)==STATUS_SUCCESS && Fence.CurrentFence==UINT32_MAX);
    Irql=DISPATCH_LEVEL;MtkMaliDdiDpcRoutine(&Adapter);Irql=PASSIVE_LEVEL;CHECK(DpcsNotified==1);
    CHECK(Submit(0)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,5)==STATUS_SUCCESS);
    Completion(&G,0,1,0);SyncFailure=2;CHECK(MtkMaliDdiPump(&Adapter,6)==STATUS_SUCCESS);
    CHECK(Adapter.LastNotifyStatus==STATUS_UNSUCCESSFUL);
    CHECK(Notified==2 && Released==2 && LastNotify.DmaCompleted.SubmissionFenceId==0);
    End();
}
static void PreemptionAndReset(void) {
    Begin();CHECK(Submit(10)==STATUS_SUCCESS && Submit(11)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);unsigned W=G.Writes;
    DXGKARG_PREEMPTCOMMAND A={0};A.PreemptionFenceId=100;
    Irql=DISPATCH_LEVEL;CHECK(MtkMaliDdiPreemptCommand(&Adapter,&A)==STATUS_SUCCESS && G.Writes==W);
    A.PreemptionFenceId=101;CHECK(MtkMaliDdiPreemptCommand(&Adapter,&A)==STATUS_DEVICE_BUSY);
    Irql=PASSIVE_LEVEL;CHECK(Submit(12)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Released && !Notified);
    Completion(&G,0,1,0);CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS);
    CHECK(Notified==2 && Released==2 && Reasons[MaliDdiCompleted]==1 && Reasons[MaliDdiPreempted]==1);
    CHECK(LastNotify.InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED);
    CHECK(LastNotify.DmaPreempted.PreemptionFenceId==100 && LastNotify.DmaPreempted.LastCompletedFenceId==10);
    CHECK(!Adapter.Queue.Count && !(State(&Adapter)&REQUEST_BIT) && !D.Jobs[0].Active);
    CHECK(Submit(20)==STATUS_SUCCESS && Submit(21)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,4)==STATUS_SUCCESS);Completion(&G,0,0x42,1);
    CHECK(MtkMaliDdiPump(&Adapter,5)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE);
    CHECK(Notified==3 && Released==2 && LastNotify.InterruptType==DXGK_INTERRUPT_DMA_FAULTED);
    CHECK(LastNotify.DmaFaulted.FaultedFenceId==20 && D.Jobs[0].Active);
    DXGKARG_RESETENGINE Reset={0};Reset.LastAbortedFenceId=99;G.ResetStuck=1;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_IO_TIMEOUT && Reset.LastAbortedFenceId==99);
    CHECK(Released==2 && D.Jobs[0].Active && !CanSchedule(&Adapter));
    CHECK(MtkMaliDdiDestroyContext(C)==STATUS_DEVICE_BUSY);
    G.ResetStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS);
    CHECK(Reset.LastAbortedFenceId==20 && Released==4 && Reasons[MaliDdiReset]==2 && CanSchedule(&Adapter));
    CHECK(!Adapter.Queue.Count && !D.Jobs[0].Active);
    DXGKARG_QUERYCURRENTFENCE Current={0};
    CHECK(MtkMaliDdiQueryCurrentFence(&Adapter,&Current)==STATUS_SUCCESS && Current.CurrentFence==20);
    A.PreemptionFenceId=200; Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliDdiPreemptCommand(&Adapter,&A)==STATUS_SUCCESS);
    Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliDdiPump(&Adapter,6)==STATUS_SUCCESS);
    CHECK(LastNotify.InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED &&
          LastNotify.DmaPreempted.PreemptionFenceId==200 && LastNotify.DmaPreempted.LastCompletedFenceId==20);
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS && Reset.LastAbortedFenceId==20);
    /* OS can now resubmit fence 21; it was not incorrectly included in 20. */
    CHECK(Submit(21)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,7)==STATUS_SUCCESS);
    Completion(&G,0,1,0);CHECK(MtkMaliDdiPump(&Adapter,8)==STATUS_SUCCESS);End();
}
static void RejectedAndRoot(void) {
    Begin();unsigned W=G.Writes;PrepareFailure=1;
    CHECK(Submit(1)==STATUS_ACCESS_DENIED && !Released && !Adapter.Queue.Count && G.Writes==W);
    PrepareFailure=0;Private.Steps[0].Chain++;
    CHECK(Submit(1)==STATUS_INVALID_PARAMETER && Reasons[MaliDdiRejected]==1 && !Adapter.Queue.Count);
    Private.Steps[0].Chain--;
    DXGKARG_SETROOTPAGETABLE Root={0};Root.hContext=C;Root.NumEntries=512;
    Root.Address.SegmentId=1;Root.Address.SegmentOffset=0x40001000;
    MtkMaliDdiSetRootPageTable(&Adapter,&Root);
    unsigned N=Prepared;CHECK(Submit(1)==STATUS_NOT_SUPPORTED && Prepared==N && G.Writes==W);
    Root.Address.SegmentId=0;Root.Address.SegmentOffset=0x40001001;MtkMaliDdiSetRootPageTable(&Adapter,&Root);
    CHECK(Submit(1)==STATUS_INVALID_PARAMETER && Prepared==N);
    Root.Address.SegmentOffset=0x40001000;MtkMaliDdiSetRootPageTable(&Adapter,&Root);
    CHECK(Submit(1)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    Root.Address.SegmentOffset=0x40002000;MtkMaliDdiSetRootPageTable(&Adapter,&Root);
    CHECK(Adapter.LastRootStatus==STATUS_DEVICE_BUSY && D.Roots[0]==0x40001000);
    Completion(&G,0,1,0);CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS);
    CHECK(Submit(2)==STATUS_DEVICE_BUSY);
    MtkMaliDdiSetRootPageTable(&Adapter,&Root);CHECK(Adapter.LastRootStatus==STATUS_SUCCESS);
    CHECK(Submit(2)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && D.Roots[0]==0x40002000);
    Completion(&G,0,1,0);CHECK(MtkMaliDdiPump(&Adapter,4)==STATUS_SUCCESS);End();
}
__declspec(align(4096)) static uint64_t Table[512];
static void PagingAndArguments(void) {
    Begin();DXGKARG_BUILDPAGINGBUFFER B={0};DXGK_PTE E={0};
    B.Operation=DXGK_OPERATION_UPDATE_PAGE_TABLE;B.UpdatePageTable.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    B.UpdatePageTable.PageTableAddress.CpuVirtual=Table;B.UpdatePageTable.pPageTableEntries=&E;
    B.UpdatePageTable.NumPageTableEntries=1;E.Valid=1;E.PageAddress=0x50001000;E.NoExecute=1;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&B)==STATUS_SUCCESS && CacheClean==1 && Barriers==1);
    CHECK(Table[0]==UINT64_C(0x00400000500014cb));
    CHECK(Submit(1)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&B)==STATUS_DEVICE_BUSY && CacheClean==1);
    DXGKARG_RESETENGINE Reset={0};CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS && !Reset.LastAbortedFenceId);
    RtlZeroMemory(&B,sizeof(B));B.Operation=DXGK_OPERATION_FLUSH_TLB;B.FlushTlb.hProcess=P;
    B.FlushTlb.RootPageTableAddress.SegmentOffset=0x40001000;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&B)==STATUS_SUCCESS);
    B.FlushTlb.StartVirtualAddress=4096;B.FlushTlb.EndVirtualAddress=2048;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&B)==STATUS_INVALID_PARAMETER);
    B.FlushTlb.StartVirtualAddress=0;B.FlushTlb.EndVirtualAddress=4096;B.FlushTlb.hProcess=(HANDLE)((char*)P+1);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&B)==STATUS_INVALID_PARAMETER);
    B.Operation=DXGK_OPERATION_DISCARD_CONTENT;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&B)==STATUS_NOT_SUPPORTED);
    DXGKARG_GETROOTPAGETABLESIZE Size={0};
    CHECK(MtkMaliDdiGetRootPageTableSize(&Adapter,&Size)==4096 && Size.NumberOfPte==512);
    Size.NumberOfPte=513;CHECK(MtkMaliDdiGetRootPageTableSize(&Adapter,&Size)==0);
    DXGKARG_CREATECONTEXT Context={0};Context.EngineAffinity=1;
    CHECK(MtkMaliDdiCreateContext(V,&Context)==STATUS_NOT_SUPPORTED);
    DXGKARG_CREATEPROCESS Process={0};Process.hDxgkProcess=(HANDLE)0x1234;Process.Flags.VirtualMachineProcess=1;
    CHECK(MtkMaliDdiCreateProcess(&Adapter,&Process)==STATUS_NOT_SUPPORTED);
    DXGKARG_SUBMITCOMMANDVIRTUAL A={0};A.hContext=C;A.DmaBufferVirtualAddress=0x80000000;A.DmaBufferSize=4096;
    A.pDmaBufferPrivateData=&Private;A.DmaBufferPrivateDataSize=MALI_COMMAND_MAX_PRIVATE+1;
    unsigned N=Prepared;CHECK(MtkMaliDdiSubmitCommandVirtual(&Adapter,&A)==STATUS_INVALID_PARAMETER && Prepared==N);
    A.DmaBufferPrivateDataSize=32;A.Flags.Paging=1;
    CHECK(MtkMaliDdiSubmitCommandVirtual(&Adapter,&A)==STATUS_NOT_SUPPORTED && Prepared==N);
    DXGKARG_PREEMPTCOMMAND Preempt={0};
    CHECK(MtkMaliDdiPreemptCommand(&Adapter,&Preempt)==STATUS_INVALID_PARAMETER);
    Irql=DISPATCH_LEVEL;unsigned WaitCount=Waits;
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_INVALID_PARAMETER && Waits==WaitCount);
    CHECK(MtkMaliDdiSubmitCommandVirtual(&Adapter,&A)==STATUS_INVALID_PARAMETER && Waits==WaitCount);
    Irql=PASSIVE_LEVEL;End();
}
static void ResetThenRestartFailure(void) {
    Begin();CHECK(Submit(30)==STATUS_SUCCESS && Submit(31)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    G.PowerStuck=1;G.Reg[0x140/4]=G.Reg[0x150/4]=G.Reg[0x160/4]=0;
    DXGKARG_RESETENGINE Reset={0};Reset.LastAbortedFenceId=99;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_IO_TIMEOUT);
    CHECK(!CanSchedule(&Adapter) && Adapter.Queue.ResetEpoch==1 && !Adapter.Queue.ActiveValid);
    CHECK(Released==2 && Reasons[MaliDdiReset]==2 && Reset.LastAbortedFenceId==99);
    CHECK(!D.Jobs[0].Active && !D.Started && D.Poisoned);
    CHECK(Submit(32)==STATUS_DEVICE_NOT_READY);End();
}
static MALI_DDI_WORKER *TestWorker=&Worker;
#include "TestWorker.h"
static NTSTATUS WorkerWait(PLARGE_INTEGER Timeout) {
    ++WaitStage;
    if(WaitStage==1) {
        CHECK(!Timeout);CHECK(Submit(40)==STATUS_SUCCESS);return STATUS_SUCCESS;
    }
    if(WaitStage==2) {
        CHECK(Timeout && Timeout->QuadPart==-500000 && D.Jobs[0].Active);
        Completion(&G,0,WorkerFault?0x42:1,WorkerFault!=0);return STATUS_TIMEOUT;
    }
    CHECK(WaitStage==3 && !Timeout);
    InterlockedExchange(&Worker.StopRequested,1);return STATUS_SUCCESS;
}
static void WorkerLifecycle(void) {
    for(WorkerFault=0;WorkerFault<2;WorkerFault++) {
        Begin();RtlZeroMemory(&Worker,sizeof(Worker));ActiveWorker=&Worker;
        ThreadCreateFailure=ThreadWaitFailure=Terminated=Closed=Signals=0;
        WaitStage=0;CounterTicks=0;EventWait=WorkerWait;
        CHECK(MtkMaliWorkerStart(&Worker,&Adapter)==STATUS_SUCCESS);
        CHECK(Worker.Frequency.QuadPart==1000 && Worker.Thread);
        CHECK(MtkMaliWorkerStart(&Worker,&Adapter)==STATUS_DEVICE_BUSY);
        Irql=5;MtkMaliWorkerWake(&Worker);CHECK(!Signals);
        Irql=DISPATCH_LEVEL;MtkMaliWorkerWake(&Worker);CHECK(Signals==1);
        Irql=PASSIVE_LEVEL;ThreadRoutine(ThreadContext);
        CHECK(Terminated==1 && WaitStage==3 && Notified==1);
        CHECK(Released==(WorkerFault?0u:1u));
        CHECK(Worker.LastStatus==(WorkerFault?STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE:STATUS_SUCCESS));
        CHECK(MtkMaliWorkerStop(&Worker)==STATUS_SUCCESS && Closed==1 && !Worker.Thread);
        CHECK(!Worker.Initialized && !CallRefs && Closing);
        unsigned N=Signals;MtkMaliWorkerWake(&Worker);CHECK(Signals==N);
        CHECK(MtkMaliWorkerStop(&Worker)==STATUS_SUCCESS && Closed==1);
        if(WorkerFault) {
            /* Joining CPU work must not pretend the GPU stopped on a fault. */
            CHECK(D.Jobs[0].Active && Lease[0]);DXGKARG_RESETENGINE Reset={0};
            CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS && Released==1);
        }
        ActiveWorker=0;End();
    }
}
static void WorkerFailures(void) {
    Begin();RtlZeroMemory(&Worker,sizeof(Worker));ActiveWorker=&Worker;
    ThreadCreateFailure=1;Terminated=Closed=Signals=0;CounterTicks=0;
    CHECK(MtkMaliWorkerStart(&Worker,&Adapter)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(!Worker.Thread && !Worker.Initialized && !CallRefs && Closing);
    ThreadCreateFailure=0;CounterFrequency=0;
    CHECK(MtkMaliWorkerStart(&Worker,&Adapter)==STATUS_INVALID_PARAMETER && !Worker.Thread);
    CounterFrequency=1000;CHECK(MtkMaliWorkerStart(&Worker,&Adapter)==STATUS_SUCCESS);
    Irql=DISPATCH_LEVEL;CHECK(MtkMaliWorkerStop(&Worker)==STATUS_INVALID_PARAMETER && Worker.Thread);
    Irql=PASSIVE_LEVEL;ThreadWaitFailure=1;
    CHECK(MtkMaliWorkerStop(&Worker)==STATUS_UNSUCCESSFUL && Worker.Thread && Worker.Initialized && !Closed);
    unsigned Before=Signals;MtkMaliWorkerWake(&Worker);CHECK(Signals==Before && !CallRefs);
    ThreadWaitFailure=2;
    CHECK(MtkMaliWorkerStop(&Worker)==STATUS_UNSUCCESSFUL && Worker.Thread && Worker.Initialized && !Closed);
    ThreadWaitFailure=0;CHECK(MtkMaliWorkerStop(&Worker)==STATUS_SUCCESS && Closed==1 && Terminated==1);
    ActiveWorker=0;End();
}
static void PreemptArrivesDuringCompletion(void) {
    Begin();CHECK(Submit(50)==STATUS_SUCCESS && Submit(51)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    SyncPreemptFence=200;Completion(&G,0,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS);
    CHECK(Notified==2 && Released==2 && !Adapter.Queue.Count && !D.Jobs[0].Active);
    CHECK(LastNotify.InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED);
    CHECK(LastNotify.DmaPreempted.PreemptionFenceId==200 && LastNotify.DmaPreempted.LastCompletedFenceId==50);
    End();
}
static void BeginManaged(void) {
    ManagedMode=1;Begin();CHECK(!G.Reg[0x28/4] && !G.Reg[0x1008/4] && !G.Reg[0x2008/4]);
    CM_PARTIAL_RESOURCE_DESCRIPTOR R[3];TestIrqResources(R);
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,3)==STATUS_SUCCESS);
    CHECK(Adapter.InterruptSyncIrql==7 && Adapter.InterruptLevelMask==((1u<<5)|(1u<<6)|(1u<<7)));
}
static void InterruptLifecycle(void) {
    Begin();unsigned W=G.Writes;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_NOT_SUPPORTED && G.Writes==W);
    End();BeginManaged();W=G.Writes;G.Powered=0;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_DEVICE_NOT_READY && G.Writes==W);
    G.Powered=1;Adapter.Queue.Faulted=1;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_DEVICE_NOT_READY && G.Writes==W);
    Adapter.Queue.Faulted=0;Adapter.InterruptMessage=1;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_NOT_SUPPORTED && G.Writes==W);
    Adapter.InterruptMessage=0;Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_INVALID_PARAMETER && G.Writes==W);
    Irql=PASSIVE_LEVEL;CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS && IrqConnected(&Adapter));
    CHECK(G.Reg[0x28/4]==0x81 && G.Reg[0x1008/4]==0x70007 && G.Reg[0x2008/4]==0xff00ff);
    W=G.Writes;CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_DEVICE_BUSY && G.Writes==W);
    unsigned R=G.Reads;
    CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0) && !MtkMaliDdiInterruptRoutine(0,0));
    Irql=DISPATCH_LEVEL;CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0) && G.Reads==R && G.Writes==W);
    Irql=5;CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,1) && G.Reads==R && G.Writes==W);
    CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0) && !Adapter.InterruptPending);
    G.Reg[0x20/4]=256;CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0) && G.Reg[0x20/4]==256);
    Irql=PASSIVE_LEVEL;G.Reg[0x20/4]=0;
    CHECK(Submit(1)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    unsigned N=Notified,L=Released,A=Waits;Completion(&G,0,1,0);QueueDpcFalse=1;
    Irql=5;CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0));
    CHECK(Adapter.InterruptPending==2 && !G.Reg[0x1008/4] && G.Reg[0x1000/4]==1);
    CHECK(Notified==N && Released==L && Waits==A && !GateHeld);
    CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0));
    Irql=DISPATCH_LEVEL;unsigned WakeBefore=WakeCount;MtkMaliDdiDpcRoutine(&Adapter);
    CHECK(WakeCount==WakeBefore+1 && DpcsNotified==1);Irql=PASSIVE_LEVEL;
    BOOLEAN Pending=FALSE;CHECK(MtkMaliDdiHasWork(&Adapter,&Pending)==STATUS_SUCCESS && Pending);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && Notified==N+1 && Released==L+1);
    CHECK(!Adapter.InterruptPending && !G.Reg[0x1000/4] && G.Reg[0x1008/4]==0x70007);
    CHECK(MtkMaliDdiHasWork(&Adapter,&Pending)==STATUS_SUCCESS && !Pending);
    Irql=DISPATCH_LEVEL;WakeBefore=WakeCount;MtkMaliDdiDpcRoutine(&Adapter);
    CHECK(WakeCount==WakeBefore);Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS && !IrqConnected(&Adapter));
    CHECK(!G.Reg[0x28/4] && !G.Reg[0x1008/4] && !G.Reg[0x2008/4]);
    R=G.Reads;W=G.Writes;G.Reg[0x1000/4]=1;Irql=5;
    CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0) && G.Reads==R && G.Writes==W);
    Irql=DISPATCH_LEVEL;MtkMaliDdiDpcRoutine(&Adapter);CHECK(WakeCount==WakeBefore);Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS);G.Reg[0x1000/4]=0;End();
}
static void InterruptFailures(void) {
    BeginManaged();unsigned W=G.Writes;SyncIrqFailure=1;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_UNSUCCESSFUL && !IrqConnected(&Adapter) && G.Writes==W);
    SyncIrqFailure=2;CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_UNSUCCESSFUL && IrqConnected(&Adapter));
    CHECK(Adapter.InterruptRetry && G.Reg[0x1008/4]==0x70007);
    SyncIrqFailure=0;CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS);
    DropMaskOffset=0x1008;DropMaskValue=0x70007;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_IO_DEVICE_ERROR && IrqConnected(&Adapter));
    CHECK(Adapter.InterruptRetry && Adapter.InterruptPaused && !G.Reg[0x28/4] && !G.Reg[0x1008/4]);
    DropMaskOffset=UINT32_MAX;CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
    DropMaskOffset=0x28;DropMaskValue=0;
    CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_IO_DEVICE_ERROR && IrqConnected(&Adapter));
    CHECK(Adapter.InterruptRetry && G.Reg[0x28/4]==0x81);
    DropMaskOffset=UINT32_MAX;CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
    CHECK(Submit(1)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    Completion(&G,0,1,0);Irql=5;CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0));Irql=PASSIVE_LEVEL;
    DropMaskOffset=0x1008;DropMaskValue=0x70007;
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_IO_DEVICE_ERROR && Released==1 && Notified==1);
    BOOLEAN Pending=FALSE;CHECK(MtkMaliDdiHasWork(&Adapter,&Pending)==STATUS_SUCCESS && Pending);
    CHECK(Adapter.InterruptRetry && Adapter.InterruptPending==2 && !G.Reg[0x1008/4]);
    DropMaskOffset=UINT32_MAX;CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && !Adapter.InterruptRetry);
    CHECK(!Adapter.InterruptPending && Released==1 && Notified==1 && G.Reg[0x1008/4]==0x70007);
    CHECK(MtkMaliDdiHasWork(&Adapter,&Pending)==STATUS_SUCCESS && !Pending);
    SyncIrqFailure=1;CHECK(MtkMaliDdiPump(&Adapter,4)==STATUS_UNSUCCESSFUL && Adapter.InterruptRetry);
    CHECK(MtkMaliDdiHasWork(&Adapter,&Pending)==STATUS_SUCCESS && Pending);
    SyncIrqFailure=0;CHECK(MtkMaliDdiPump(&Adapter,5)==STATUS_SUCCESS && !Adapter.InterruptRetry);End();
}
static void InterruptFaultAndReset(void) {
    BeginManaged();CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
    CHECK(Submit(10)==STATUS_SUCCESS && Submit(11)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    G.Reg[0x20/4]=1;G.Reg[0x3c/4]=0x42;G.Reg[0x2000/4]=1;G.Reg[0x241c/4]=0xc1;
    Completion(&G,0,0x42,1);Irql=5;CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0));
    CHECK(Adapter.InterruptPending==7 && !G.Reg[0x28/4] && !G.Reg[0x1008/4] && !G.Reg[0x2008/4]);
    Irql=PASSIVE_LEVEL;CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE);
    CHECK(Adapter.Queue.Faulted && D.Poisoned && D.Jobs[0].Active && !Released && Notified==1);
    CHECK(!Adapter.InterruptPending && Adapter.InterruptPaused);
    BOOLEAN Pending=TRUE;CHECK(MtkMaliDdiHasWork(&Adapter,&Pending)==STATUS_SUCCESS && !Pending);
    DXGKARG_RESETENGINE Reset={0};Reset.LastAbortedFenceId=99;G.ResetStuck=1;unsigned Start=G.Writes;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_IO_TIMEOUT && Reset.LastAbortedFenceId==99);
    CHECK(!Released && D.Jobs[0].Active && IrqConnected(&Adapter) && !CanSchedule(&Adapter));
    G.ResetStuck=0;SyncIrqFailure=1;unsigned W=G.Writes;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_UNSUCCESSFUL && G.Writes==W && !Released);
    SyncIrqFailure=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS);
    CHECK(Reset.LastAbortedFenceId==10 && Released==2 && Reasons[MaliDdiReset]==2 && CanSchedule(&Adapter));
    unsigned FirstUnmask=G.Writes;
    for(unsigned I=Start;I<G.Writes;I++)if((G.Offsets[I]==0x28 || G.Offsets[I]==0x1008 || G.Offsets[I]==0x2008) && G.Values[I]) {FirstUnmask=I;break;}
    CHECK(FirstUnmask<G.Writes && G.Offsets[FirstUnmask]==0x28 && !Adapter.InterruptPaused);
    CHECK(G.Reg[0x28/4]==0x81 && G.Reg[0x1008/4]==0x70007 && G.Reg[0x2008/4]==0xff00ff);End();
}
static void InterruptArrivalDuringRearm(void) {
    BeginManaged();CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
    SyncInjectBank=1;SyncInjectBits=1;SyncInjectFire=1;
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Adapter.InterruptPending==1 && !Adapter.Queue.Faulted);
    unsigned WakeBefore=WakeCount;Irql=DISPATCH_LEVEL;MtkMaliDdiDpcRoutine(&Adapter);Irql=PASSIVE_LEVEL;
    CHECK(WakeCount==WakeBefore+1);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE);
    CHECK(Adapter.Queue.Faulted && D.Poisoned && !Notified && !Released && !G.Reg[0x20/4]);
    CHECK(!Adapter.Queue.EventPending && !Adapter.InterruptPending && Adapter.InterruptPaused);
    BOOLEAN Pending=TRUE;CHECK(MtkMaliDdiHasWork(&Adapter,&Pending)==STATUS_SUCCESS && !Pending);
    DXGKARG_RESETENGINE Reset={0};CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS && !Reset.LastAbortedFenceId);
    CHECK(CanSchedule(&Adapter) && !Adapter.InterruptPaused);End();
}
static void InterruptDomainValidation(void) {
    CM_PARTIAL_RESOURCE_DESCRIPTOR R[3];TestIrqResources(R);Begin();unsigned W=G.Writes;
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,3)==STATUS_NOT_SUPPORTED && G.Writes==W);End();
    ManagedMode=1;Begin();W=G.Writes;
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_DEVICE_CONFIGURATION_ERROR && G.Writes==W);
    const UINT Counts[]={0,1,2,4,UINT32_MAX};
    for(UINT I=0;I<sizeof(Counts)/sizeof(Counts[0]);I++)
        CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,Counts[I])==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,0,3)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,(void*)((uintptr_t)R+1),3)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,(void*)(UINTPTR_MAX-7u),3)==STATUS_INVALID_PARAMETER);
    for(UINT I=0;I<16;I++) {
        TestIrqResources(R);
        switch(I) {
        case 0:R[2].Type=CmResourceTypeMemory;break;
        case 1:R[2].Flags=CM_RESOURCE_INTERRUPT_LATCHED;break;
        case 2:R[2].Flags=CM_RESOURCE_INTERRUPT_MESSAGE;break;
        case 3:R[2].Flags=CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT;break;
        case 4:R[2].Flags=0x8000;break;
        case 5:R[2].u.Interrupt.Level=0;break;
        case 6:R[2].u.Interrupt.Level=DISPATCH_LEVEL;break;
        case 7:R[2].u.Interrupt.Level=PROFILE_LEVEL;break;
        case 8:R[2].u.Interrupt.Level=255;break;
        case 9:
#if defined(NT_PROCESSOR_GROUPS)
            R[2].u.Interrupt.Group=1;
#else
            R[2].u.Interrupt.Level|=1u<<16;
#endif
            break;
        case 10:R[2].u.Interrupt.Vector=0;break;
        case 11:R[2].u.Interrupt.Vector=R[0].u.Interrupt.Vector;break;
        case 12:R[2].u.Interrupt.Affinity=0;break;
        case 13:R[2].ShareDisposition=CmResourceShareUndetermined;break;
        case 14:R[2].ShareDisposition=255;break;
        case 15:R[0].u.Interrupt.Level=DISPATCH_LEVEL;break;
        }
        CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,3)==STATUS_DEVICE_CONFIGURATION_ERROR);
        CHECK(!Adapter.InterruptLevelMask && !Adapter.InterruptSyncIrql && G.Writes==W);
    }
    TestIrqResources(R);Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,3)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    R[0].ShareDisposition=CmResourceShareShared;R[0].Flags=CM_RESOURCE_INTERRUPT_POLICY_INCLUDED;
    R[1].ShareDisposition=CmResourceShareDriverExclusive;R[1].Flags=CM_RESOURCE_INTERRUPT_WAKE_HINT;
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,3)==STATUS_SUCCESS && G.Writes==W);
    R[2].u.Interrupt.Level=8;
    CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,3)==STATUS_DEVICE_BUSY && Adapter.InterruptSyncIrql==7);
    SyncLevel=4;CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_DEVICE_CONFIGURATION_ERROR && !IrqConnected(&Adapter) && G.Writes==W);
    SyncLevel=5;CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);End();
}
static void InterruptPriorityBoundaries(void) {
    for(UINT Variant=0;Variant<3;Variant++) {
        ManagedMode=1;Begin();CM_PARTIAL_RESOURCE_DESCRIPTOR R[3];TestIrqResources(R);
        for(UINT I=0;I<3;I++)R[I].u.Interrupt.Level=Variant==0?DISPATCH_LEVEL+1:PROFILE_LEVEL-1;
        if(Variant==2){R[1].u.Interrupt.Level=DISPATCH_LEVEL+1;R[2].u.Interrupt.Level=7;}
        KIRQL Maximum=(KIRQL)R[0].u.Interrupt.Level;
        CHECK(MtkMaliDdiConfigureInterruptDomain(&Adapter,R,3)==STATUS_SUCCESS);
        CHECK(Adapter.InterruptSyncIrql==Maximum);
        SyncLevel=Maximum;CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
        /* Descriptors are consumed during configuration, never retained. */
        RtlZeroMemory(R,sizeof(R));G.Reg[0x1000/4]=1;
        KIRQL Source=Variant==2?DISPATCH_LEVEL+1:Maximum;Irql=Source;
        CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0) && Irql==Source && !TestIrqLockHeld);
        CHECK(Adapter.InterruptPending==2 && G.Reg[0x1000/4]==1);
        G.Reg[0x1000/4]=0;Irql=PASSIVE_LEVEL;
        CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS);End();
    }
}
static void InterruptDeferredSource(void) {
    /* A level-6 source deferred while the level-5 ISR held the lock at 7. */
    CHECK(Irql==5 && !TestIrqLockHeld);G.Reg[0x20/4]=1;Irql=6;
    CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0));CHECK(Irql==6 && !TestIrqLockHeld);Irql=5;
}
static void InterruptOtherCpuDisconnect(void) {
    /* Deterministic interleaving: another CPU wins the lock after our initial
       connected check, then disconnects before this CPU acquires it. */
    IRQ_CHANGE C={&Adapter,IrqDisconnect,0,STATUS_SUCCESS};
    CHECK(Irql==7 && !TestIrqLockHeld && IrqAtInterrupt(&C) && C.Called && C.Status==STATUS_SUCCESS);
}
static void InterruptDistinctLevels(void) {
    BeginManaged();CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
    unsigned R=G.Reads,W=G.Writes,A=TestIrqAcquires;
    const KIRQL Bad[]={DISPATCH_LEVEL,3,4,8,PROFILE_LEVEL,HIGH_LEVEL};
    for(UINT I=0;I<sizeof(Bad)/sizeof(Bad[0]);I++) {
        Irql=Bad[I];CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0));
        CHECK(G.Reads==R && G.Writes==W && TestIrqAcquires==A && Irql==Bad[I]);
    }
    static const uint32_t Raw[3]={0x20,0x1000,0x2000};
    for(UINT I=0;I<3;I++) {
        G.Reg[Raw[I]/4]=1;Irql=(KIRQL)(5+I);CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0));
        CHECK(Irql==5+I && !TestIrqLockHeld && Adapter.InterruptPending==((1u<<(I+1))-1));
        CHECK(G.Reg[Raw[I]/4]==1);
    }
    CHECK(TestIrqAcquires==A+3 && TestIrqAcquires==TestIrqReleases);
    for(UINT I=0;I<3;I++)G.Reg[Raw[I]/4]=0;
    Irql=PASSIVE_LEVEL;CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
    unsigned Q=DpcsQueued;G.Reg[0x1000/4]=1;Irql=5;TestIrqAfterLower=InterruptDeferredSource;
    CHECK(MtkMaliDdiInterruptRoutine(&Adapter,0));
    CHECK(Irql==5 && !TestIrqLockHeld && Adapter.InterruptPending==3 && DpcsQueued==Q+2);
    CHECK(G.Reg[0x20/4]==1 && G.Reg[0x1000/4]==1);
    G.Reg[0x20/4]=G.Reg[0x1000/4]=0;Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliDdiDisconnectInterrupts(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiConnectInterrupts(&Adapter)==STATUS_SUCCESS);
    R=G.Reads;W=G.Writes;Q=DpcsQueued;Irql=5;TestIrqBeforeAcquire=InterruptOtherCpuDisconnect;
    CHECK(!MtkMaliDdiInterruptRoutine(&Adapter,0));
    CHECK(Irql==5 && !IrqConnected(&Adapter) && !TestIrqLockHeld && DpcsQueued==Q);
    CHECK(G.Reads==R+3 && G.Writes==W+3 && !Adapter.InterruptPending);
    Irql=PASSIVE_LEVEL;End();
}
int TestMain(void) {
    InterruptDomainValidation();InterruptDistinctLevels();InterruptPriorityBoundaries();if(FirstFailure)return (int)FirstFailure;
    LifecycleAndCompletion();PreemptionAndReset();RejectedAndRoot();PagingAndArguments();ResetThenRestartFailure();WorkerLifecycle();WorkerFailures();PreemptArrivesDuringCompletion();InterruptLifecycle();InterruptFailures();InterruptFaultAndReset();InterruptArrivalDuringRearm();return (int)FirstFailure;
}
