/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production parser + preparation + DDIs + scheduler. Only VM/OS/GPU boundaries
   are fixtures; these do not prove the missing parent VidMm mapping backend. */
#include "Prepare.h"
static unsigned FirstFailure;
unsigned MtkMaliPrepareAssertionCount;
#define CHECK(X) do {++MtkMaliPrepareAssertionCount;if(!(X) && !FirstFailure)FirstFailure=__LINE__;} while(0)
#include "TestGpu.h"
static KIRQL Irql;
static unsigned GateHeld;
static NTSTATUS Wait(PVOID O,KWAIT_REASON Why,KPROCESSOR_MODE Mode,BOOLEAN A,PLARGE_INTEGER T) {
    CHECK(O && Why==Executive && Mode==KernelMode && !A && !T && !GateHeld && Irql==PASSIVE_LEVEL);
    GateHeld=1;return STATUS_SUCCESS;
}
static LONG Unlock(PKMUTEX M,BOOLEAN W) {CHECK(M && !W && GateHeld);GateHeld=0;return 0;}
static void InitMutex(PKMUTEX M,ULONG L) {CHECK(M && !L);}
#define KeGetCurrentIrql() Irql
#define KeWaitForSingleObject Wait
#define KeReleaseMutex Unlock
#define KeInitializeMutex InitMutex
#include "TestPool.h"
#include "Allocation.c"
#include "Vm.c"
#include "Ddi.c"
#include "Prepare.c"
static GPU G;
static MALI_JM Core;
static MALI_DDI_ADAPTER Adapter;
static MALI_PREPARE_SERVICE Service;
static MALI_DDI_HOOKS Hooks;
static HANDLE ProcessHandle,DeviceHandle,ContextHandle;
static MALI_EXEC_PACKET Private;
static uint8_t Dma[4096],Shadow[MALI_DDI_RESIDENTS][MALI_COMMAND_SHADOW_BYTES];
static struct {unsigned Live,Pins,Sealed;} Tokens[MALI_DDI_RESIDENTS];
static unsigned Begins,Copies,Reserves,PinCalls,Seals,Ends,Live,Wakes,Notifies,Reasons[5];
static unsigned Fail,Mutate,MutatePrivate,SyncFail,WrongOwner,ReadOnly,PhysicalAlias,ReturnedPins;
static uint64_t LastRoot;
static void Put32(uint8_t *P,uint32_t V) {for(unsigned I=0;I<4;I++)P[I]=(uint8_t)(V>>(I*8));}
static void Put64(uint8_t *P,uint64_t V) {Put32(P,(uint32_t)V);Put32(P+4,(uint32_t)(V>>32));}
static uint64_t Get64(const uint8_t *P) {uint64_t V=0;for(unsigned I=0;I<8;I++)V|=(uint64_t)P[I]<<(I*8);return V;}
static unsigned TokenIndex(void *T) {
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(T==&Tokens[I]) {CHECK(Tokens[I].Live);return I;}
    CHECK(0);return 0;
}
static void Boundary(void *X) {CHECK(X==&Adapter && GateHeld && Irql==PASSIVE_LEVEL);}
static NTSTATUS VmBegin(void *X,HANDLE P,uint64_t Id,uint64_t Root,uint64_t Va,uint32_t N,void **T) {
    Boundary(X);CHECK(P==(HANDLE)0x1234 && Id && Va==0x80000000 && N==sizeof(Dma));
    CHECK(Root && !(Root&4095));LastRoot=Root;++Begins;
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(!Tokens[I].Live) {
        Tokens[I].Live=1;*T=&Tokens[I];++Live;
        return Fail==1?STATUS_NO_MEMORY:Fail==6?STATUS_TIMEOUT:STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}
static NTSTATUS VmCopy(void *X,void *T,void *Dest,uint32_t N) {
    Boundary(X);TokenIndex(T);CHECK(Dest==Service.Input && N==sizeof(Dma));++Copies;
    if(Fail==2)return STATUS_ACCESS_DENIED;
    RtlCopyMemory(Dest,Dma,N);
    if(MutatePrivate){Private.Flags=UINT32_MAX;Private.Steps[0].Slot=0;}
    return STATUS_SUCCESS;
}
static NTSTATUS VmReserve(void *X,void *T,uint32_t N,void **Cpu,uint64_t *Va,uint64_t *Root) {
    Boundary(X);unsigned I=TokenIndex(T);CHECK(N==sizeof(Shadow[I]));++Reserves;
    *Root=LastRoot;*Cpu=Shadow[I];*Va=0x90000000+(uint64_t)I*MALI_COMMAND_SHADOW_BYTES;
    if(Fail==9)*Va=0x80000000;
    if(Fail==10)*Cpu=Service.Input;
    if(Fail==11)*Va+=64;
    if(Fail==12)*Cpu=0;
    return Fail==3?STATUS_NO_MEMORY:STATUS_SUCCESS;
}
static NTSTATUS VmPin(void *X,void *T,uint64_t Va,uint32_t N) {
    Boundary(X);unsigned I=TokenIndex(T);++PinCalls;
    CHECK((Va==0xa0000000 || Va==0xa0000008) && N==8 && !Tokens[I].Sealed);
    if(Fail==4 || (Fail==8 && Tokens[I].Pins==1) || WrongOwner || ReadOnly || PhysicalAlias)return STATUS_ACCESS_DENIED;
    ++Tokens[I].Pins;
    if(Mutate) {RtlFillMemory(Dma,sizeof(Dma),0xff);RtlFillMemory(&Private,sizeof(Private),0xff);}
    return STATUS_SUCCESS;
}
static NTSTATUS VmSeal(void *X,void *T,const void *Cpu,uint32_t N) {
    Boundary(X);unsigned I=TokenIndex(T);++Seals;
    CHECK(Cpu==Shadow[I] && N==128 && Tokens[I].Pins==1);
    CHECK(Get64(Shadow[I]+48)==UINT64_C(0x0123456789abcdef));
    CHECK(!Get64(Shadow[I]+24));Tokens[I].Sealed=1;
    return Fail==5?STATUS_IO_DEVICE_ERROR:Fail==7?STATUS_PENDING:STATUS_SUCCESS;
}
static void VmEnd(void *X,void *T,MALI_DDI_RELEASE_REASON Reason) {
    Boundary(X);unsigned I=TokenIndex(T);CHECK(Reason>=1 && Reason<=4);
    for(unsigned J=0;J<3;J++)CHECK(Reason==MaliDdiRejected || !Core.Jobs[J].Active);
    ReturnedPins+=Tokens[I].Pins;
    RtlZeroMemory(Shadow[I],sizeof(Shadow[I]));RtlZeroMemory(&Tokens[I],sizeof(Tokens[I]));
    --Live;++Ends;++Reasons[Reason];
}
static void VmWake(void *X) {CHECK(X==&Adapter && Irql<=DISPATCH_LEVEL && (Irql==DISPATCH_LEVEL || !GateHeld));++Wakes;}
static BOOLEAN APIENTRY QDpc(HANDLE H) {CHECK(H==(HANDLE)7 && Irql==5);return TRUE;}
static VOID APIENTRY NDpc(HANDLE H) {CHECK(H==(HANDLE)7 && Irql==DISPATCH_LEVEL);}
static VOID APIENTRY NInterrupt(HANDLE H,DXGKARGCB_NOTIFY_INTERRUPT_DATA *A) {
    CHECK(H==(HANDLE)7 && A && Irql==5 && GateHeld);++Notifies;
}
static NTSTATUS APIENTRY Sync(HANDLE H,PKSYNCHRONIZE_ROUTINE Fn,PVOID Data,ULONG Message,PBOOLEAN Out) {
    CHECK(H==(HANDLE)7 && !Message && Irql==PASSIVE_LEVEL && GateHeld);
    if(SyncFail)return STATUS_UNSUCCESSFUL;
    Irql=5;*Out=Fn(Data);Irql=PASSIVE_LEVEL;return STATUS_SUCCESS;
}
static void Clean(void *X,const void *Cpu,size_t N) {Boundary(X);CHECK(Cpu && N);}
static void BarrierCpu(void *X) {Boundary(X);}
static void Input(void) {
    RtlZeroMemory(Dma,sizeof(Dma));Put32(Dma+16,(71u<<16)|4u);Put64(Dma+32,0xa0000000);
    Put32(Dma+40,7);Put64(Dma+48,UINT64_C(0x0123456789abcdef));
    RtlZeroMemory(&Private,sizeof(Private));Private.Version=1;Private.StepCount=1;
    Private.Bytes=MALI_EXEC_PACKET_BYTES(1);Private.Steps[0].Chain=0x80000000;Private.Steps[0].Slot=1;
}
static void Begin(void) {
    Irql=PASSIVE_LEVEL;GateHeld=0;Begins=Copies=Reserves=PinCalls=Seals=Ends=Live=Wakes=Notifies=0;
    Fail=Mutate=MutatePrivate=SyncFail=WrongOwner=ReadOnly=PhysicalAlias=ReturnedPins=0;LastRoot=0;
    RtlZeroMemory(Tokens,sizeof(Tokens));RtlZeroMemory(Reasons,sizeof(Reasons));
    Init(&G,&Core);G.Reg[0xc4/4]=12;CHECK(MaliJmStart(&Core)==MaliOk);
    MALI_PREPARE_OPS Ops={&Adapter,VmBegin,VmCopy,VmReserve,VmPin,VmSeal,VmEnd,VmWake,0};
    CHECK(MtkMaliPrepareInitialize(&Service,&Core,&Ops,&Hooks)==STATUS_SUCCESS);
    DXGKRNL_INTERFACE K={0};K.Size=sizeof(K);K.DeviceHandle=(HANDLE)7;
    K.DxgkCbSynchronizeExecution=Sync;K.DxgkCbNotifyInterrupt=NInterrupt;
    K.DxgkCbQueueDpc=QDpc;K.DxgkCbNotifyDpc=NDpc;
    MALI_WDDM_CACHE_OPS Cache={&Adapter,Clean,BarrierCpu};
    CHECK(MtkMaliDdiInitialize(&Adapter,&Core,100,&K,0,&Hooks,&Cache)==MaliOk);
    DXGKARG_CREATEPROCESS P={0};P.hDxgkProcess=(HANDLE)0x1234;
    CHECK(MtkMaliDdiCreateProcess(&Adapter,&P)==STATUS_SUCCESS);ProcessHandle=P.hKmdProcess;
    DXGKARG_CREATEDEVICE V={0};V.hKmdProcess=ProcessHandle;
    CHECK(MtkMaliDdiCreateDevice(&Adapter,&V)==STATUS_SUCCESS);DeviceHandle=V.hDevice;
    DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(MtkMaliDdiCreateContext(DeviceHandle,&C)==STATUS_SUCCESS);ContextHandle=C.hContext;
    DXGKARG_SETROOTPAGETABLE R={0};R.hContext=ContextHandle;R.NumEntries=512;R.Address.SegmentOffset=0x40001000;
    MtkMaliDdiSetRootPageTable(&Adapter,&R);CHECK(Adapter.LastRootStatus==STATUS_SUCCESS);Input();
}
static NTSTATUS Submit(unsigned Fence) {
    DXGKARG_SUBMITCOMMANDVIRTUAL A={0};A.hContext=ContextHandle;A.DmaBufferVirtualAddress=0x80000000;
    A.DmaBufferSize=sizeof(Dma);A.DmaBufferPrivateDataSize=Private.Bytes;A.pDmaBufferPrivateData=&Private;
    A.SubmissionFenceId=Fence;return MtkMaliDdiSubmitCommandVirtual(&Adapter,&A);
}
static int Cleared(void) {
    const uint8_t *W=(const uint8_t*)&Service.Work;unsigned V=0;
    for(unsigned I=0;I<sizeof(Service.Input);I++)V|=Service.Input[I];
    for(unsigned I=0;I<sizeof(Service.Private);I++)V|=Service.Private[I];
    for(unsigned I=0;I<sizeof(Service.Work);I++)V|=W[I];return !V && !Service.CurrentToken;
}
static void End(void) {
    CHECK(!Live && Cleared());CHECK(MtkMaliDdiDestroyContext(ContextHandle)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyDevice(DeviceHandle)==STATUS_SUCCESS);CHECK(MtkMaliDdiDestroyProcess(&Adapter,ProcessHandle)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliPrepareShutdown(&Service)==STATUS_SUCCESS);
}
static void Lifecycle(void) {
    Begin();Mutate=MutatePrivate=1;CHECK(Submit(1)==STATUS_SUCCESS && Live==1 && Cleared());
    CHECK(LastRoot==0x40001000 && Get64(Shadow[0]+48)==UINT64_C(0x0123456789abcdef));
    CHECK(MtkMaliPrepareShutdown(&Service)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Core.Jobs[1].Active && Core.Jobs[1].Chain==0x90000000);
    Completion(&G,1,1,0);SyncFail=1;
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_UNSUCCESSFUL && Live==1 && !Ends && !Core.Jobs[1].Active);
    SyncFail=0;CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && !Live && Ends==1 && Reasons[MaliDdiCompleted]==1);
    Input();Mutate=MutatePrivate=0;CHECK(Submit(2)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,4)==STATUS_SUCCESS);
    Completion(&G,1,0x42,1);CHECK(MtkMaliDdiPump(&Adapter,5)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && Live==1);
    DXGKARG_RESETENGINE R={0};G.ResetStuck=1;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_IO_TIMEOUT && Live==1 && Ends==1);
    G.ResetStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_SUCCESS && !Live && Reasons[MaliDdiReset]==1);
    End();
}
static void Failures(void) {
    for(unsigned I=1;I<=12;I++) {
        Begin();Fail=I;
        if(I==8) {Put64(Dma+24,0x80000080);RtlCopyMemory(Dma+128,Dma,64);
            Put32(Dma+128+16,(72u<<16)|4u);Put64(Dma+128+24,0);Put64(Dma+128+32,0xa0000008);}
        unsigned Writes=G.Writes;NTSTATUS S=Submit(1);
        CHECK(!NT_SUCCESS(S) && !Live && Ends==1 && Reasons[MaliDdiRejected]==1);
        CHECK(G.Writes==Writes && !Adapter.Queue.Count && Cleared());
        if(I==8)CHECK(PinCalls==2 && ReturnedPins==1);
        End();
    }
    for(unsigned I=0;I<7;I++) {
        Begin();
        if(I==0)Put32(Dma+16,(1u<<16)|8u); // compute not accepted
        if(I==1)Put64(Dma+24,0x80000000);
        if(I==2)Private.Flags=1;
        if(I==3)WrongOwner=1;
        if(I==4)ReadOnly=1;
        if(I==5)PhysicalAlias=1;
        if(I==6)Put64(Dma+32,0x90000000);
        unsigned Writes=G.Writes;CHECK(!NT_SUCCESS(Submit(1)) && !Live && Ends==1);
        CHECK(G.Writes==Writes && !Adapter.Queue.Count && Cleared());End();
    }
    Begin();CHECK(Submit(7)==STATUS_SUCCESS && Live==1);
    CHECK(!NT_SUCCESS(Submit(7)) && Live==1 && Ends==1 && Reasons[MaliDdiRejected]==1);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Live && Ends==2);End();
}
static void QueueAndPreempt(void) {
    Begin();for(unsigned I=1;I<=32;I++)CHECK(Submit(I)==STATUS_SUCCESS);
    CHECK(Live==32 && !Ends && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    CHECK(Submit(33)==STATUS_SUCCESS && Live==33 && Seals==33);
    unsigned B=Begins;CHECK(Submit(34)==STATUS_DEVICE_BUSY && Begins==B && Live==33);
    DXGKARG_SUBMITCOMMANDVIRTUAL A={0};A.DmaBufferVirtualAddress=0x80000000;A.DmaBufferSize=sizeof(Dma);
    A.pDmaBufferPrivateData=&Private;A.DmaBufferPrivateDataSize=Private.Bytes;MALI_DDI_PREPARED Out={0};
    GateHeld=1;CHECK(Hooks.Prepare(Hooks.Context,(HANDLE)0x1234,((MALI_DDI_CONTEXT*)ContextHandle)->Id,
        0x40001000,&A,&Out)==STATUS_INSUFFICIENT_RESOURCES && !Out.Cookie);GateHeld=0;
    CHECK(Begins==B && Live==33);
    DXGKARG_PREEMPTCOMMAND P={0};P.PreemptionFenceId=90;Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliDdiPreemptCommand(&Adapter,&P)==STATUS_SUCCESS);Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && Live==33);
    Completion(&G,1,1,0);CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && !Live);
    CHECK(Reasons[MaliDdiCompleted]==1 && Reasons[MaliDdiPreempted]==32 && Ends==33);End();
}
static void ResetThenRestartFailure(void) {
    Begin();CHECK(Submit(1)==STATUS_SUCCESS && Submit(2)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Live==2);
    G.PowerStuck=1;G.Reg[0x140/4]=G.Reg[0x150/4]=G.Reg[0x160/4]=0;
    DXGKARG_RESETENGINE R={0};CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_IO_TIMEOUT);
    CHECK(!Live && Reasons[MaliDdiReset]==2 && Adapter.Queue.ResetEpoch==1 && !Core.Jobs[1].Active);End();
}
static void Interface(void) {
    Begin();GateHeld=1;Hooks.Release(Hooks.Context,(void*)1,MaliDdiRejected);GateHeld=0;
    CHECK(Service.LastReleaseStatus==STATUS_INVALID_PARAMETER && !Ends);
    MALI_PREPARE_OPS Ops=Service.Ops;Ops.PinWrite=0;MALI_DDI_HOOKS H={0};
    CHECK(MtkMaliPrepareInitialize(&Service,&Core,&Ops,&H)==STATUS_INVALID_PARAMETER && !H.Prepare);
    Irql=DISPATCH_LEVEL;CHECK(MtkMaliPrepareShutdown(&Service)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    DXGKARG_SETROOTPAGETABLE R={0};R.hContext=ContextHandle;R.NumEntries=512;R.Address.SegmentOffset=0x40009000;
    MtkMaliDdiSetRootPageTable(&Adapter,&R);CHECK(Submit(1)==STATUS_SUCCESS && LastRoot==0x40009000);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Live);End();
}
unsigned TestMain(void) {Lifecycle();Failures();QueueAndPreempt();ResetThenRestartFailure();Interface();return FirstFailure;}
