/* Shared OS/VM/DDI fixture; define MALI_VM_TEST_COUNTER before inclusion. */
#ifndef T70_MALI_TEST_VM_H
#define T70_MALI_TEST_VM_H
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual OS CPU page-table writes, registry and allocation/DDI lifetimes.
   Only OS memory allocation/physical identity, callbacks and GPU MMIO are fixtures. */
#include "Ddi.h"
static unsigned FirstFailure;
unsigned MALI_VM_TEST_COUNTER;
#define CHECK(X) do {++MALI_VM_TEST_COUNTER;if(!(X) && !FirstFailure)FirstFailure=__LINE__;} while(0)
#include "TestGpu.h"
static KIRQL Irql;
static unsigned GateHeld;
static NTSTATUS VmTestWait(PVOID P,KWAIT_REASON W,KPROCESSOR_MODE M,BOOLEAN A,PLARGE_INTEGER T) {
    CHECK(P && W==Executive && M==KernelMode && !A && !T && !GateHeld && Irql==PASSIVE_LEVEL);GateHeld=1;return STATUS_SUCCESS;
}
static LONG VmTestUnlock(PKMUTEX M,BOOLEAN W) {CHECK(M && !W && GateHeld);GateHeld=0;return 0;}
static void VmTestMutex(PKMUTEX M,ULONG L) {CHECK(M && !L);}
#define KeGetCurrentIrql() Irql
#define KeWaitForSingleObject VmTestWait
#define KeReleaseMutex VmTestUnlock
#define KeInitializeMutex VmTestMutex
static __declspec(align(4096)) uint64_t OsTables[256][512];
static uint64_t OsPhysical[256];
static PHYSICAL_ADDRESS VmTestPhysical(PVOID P) {
    CHECK(GateHeld && Irql==PASSIVE_LEVEL);PHYSICAL_ADDRESS A;A.QuadPart=0;
    for(unsigned I=0;I<256;I++)if(P==OsTables[I]){A.QuadPart=(LONGLONG)OsPhysical[I];return A;}
    CHECK(0);return A;
}
#define MmGetPhysicalAddress VmTestPhysical
#include "TestPool.h"
static __declspec(align(16)) uint8_t VmPool[256][131072];
static uint8_t VmPoolUsed[256];
static __declspec(align(16)) uint8_t VmLargePool[4][2097152];
static uint8_t VmLargeUsed[4];
static unsigned VmPoolLive,VmPoolCalls,VmPoolFrees,VmPoolFail;
static void *VmPoolAllocate(POOL_FLAGS F,SIZE_T N,ULONG Tag) {
    if(Tag==0x41303754u)return TestPoolAllocate(F,N,Tag);
    CHECK(GateHeld && Irql==PASSIVE_LEVEL && F==POOL_FLAG_NON_PAGED && Tag==0x56303754u && N<=2097152);
    ++VmPoolCalls;if(VmPoolCalls==VmPoolFail)return 0;
    if(N>131072) {
        for(unsigned I=0;I<4;I++)if(!VmLargeUsed[I]) {
            VmLargeUsed[I]=1;++VmPoolLive;RtlZeroMemory(VmLargePool[I],N);return VmLargePool[I];
        }
        return 0;
    }
    for(unsigned I=0;I<256;I++)if(!VmPoolUsed[I]) {
        VmPoolUsed[I]=1;++VmPoolLive;RtlZeroMemory(VmPool[I],N);return VmPool[I];
    }
    return 0;
}
static void VmPoolFree(void *P,ULONG Tag) {
    if(Tag==0x41303754u){TestPoolFree(P,Tag);return;}
    CHECK(GateHeld && Irql==PASSIVE_LEVEL && Tag==0x56303754u);
    uintptr_t A=(uintptr_t)P,Large=(uintptr_t)VmLargePool;
    if(A>=Large && A-Large<sizeof(VmLargePool) && !((A-Large)%2097152)) {
        unsigned I=(unsigned)((A-Large)/2097152);CHECK(VmLargeUsed[I] && VmPoolLive);
        if(VmLargeUsed[I]){VmLargeUsed[I]=0;--VmPoolLive;++VmPoolFrees;}return;
    }
    uintptr_t B=(uintptr_t)VmPool;CHECK(A>=B && A-B<sizeof(VmPool) && !((A-B)%131072));
    if(A<B || A-B>=sizeof(VmPool) || (A-B)%131072)return;
    unsigned I=(unsigned)((A-B)/131072);CHECK(VmPoolUsed[I] && VmPoolLive);
    if(VmPoolUsed[I]){VmPoolUsed[I]=0;--VmPoolLive;++VmPoolFrees;}
}
#undef ExAllocatePool2
#undef ExFreePoolWithTag
#define ExAllocatePool2 VmPoolAllocate
#define ExFreePoolWithTag VmPoolFree
#include "Allocation.c"
#include "Vm.c"
#include "Ddi.c"
static GPU G;
static MALI_JM Core;
static MALI_DDI_ADAPTER Adapter;
static MALI_VM Vm;
static HANDLE Proc[2],Dev[2],Ctx;
static uint64_t Owner[2];
static HANDLE AllocationHandle,ResourceHandle,OpenHandle;
static unsigned Cleans,Barriers,MutatePtes,Released,TestVmSyncFailure,TestVmSyncCalls,TestVmFailSyncAt;
static void (*TestVmInitializeHooks)(MALI_DDI_HOOKS *);
static DXGKARG_BUILDPAGINGBUFFER Build;
static DXGK_PTE Entries[512];
static uint64_t Before[512];
static NTSTATUS APIENTRY VmSync(HANDLE H,PKSYNCHRONIZE_ROUTINE R,PVOID X,ULONG N,PBOOLEAN Ret) {
    CHECK(H==(HANDLE)0x777 && GateHeld && !N);if(TestVmSyncFailure || ++TestVmSyncCalls==TestVmFailSyncAt)return STATUS_UNSUCCESSFUL;Irql=5;*Ret=R(X);Irql=PASSIVE_LEVEL;return STATUS_SUCCESS;
}
static VOID APIENTRY VmNotify(HANDLE H,DXGKARGCB_NOTIFY_INTERRUPT_DATA *A) {CHECK(H==(HANDLE)0x777 && A && Irql==5);}
static BOOLEAN APIENTRY VmDpc(HANDLE H) {CHECK(H==(HANDLE)0x777 && Irql==5);return TRUE;}
static VOID APIENTRY VmNotifyDpc(HANDLE H) {CHECK(H==(HANDLE)0x777 && Irql==DISPATCH_LEVEL);}
static void VmWake(void *X) {CHECK(X==&Adapter);}
static void VmClean(void *X,const void *P,size_t N) {
    CHECK(X==&Adapter && GateHeld && P && N);++Cleans;
    if(MutatePtes)Entries[0].PageAddress=0x666000;
}
static void VmBarrier(void *X) {CHECK(X==&Adapter && GateHeld);++Barriers;}
static NTSTATUS VmFakePrepare(void *X,HANDLE P,uint64_t Id,uint64_t Root,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *A,MALI_DDI_PREPARED *Out) {
    CHECK(X==&Adapter && P==(HANDLE)0x1234 && Id && Root==OsPhysical[0] && A && GateHeld);
    Out->Packet.Version=1;Out->Packet.Bytes=MALI_EXEC_PACKET_BYTES(1);Out->Packet.StepCount=1;
    Out->Packet.Steps[0].Chain=0x90000000;Out->ExecutionRoot=Root;Out->DmaVa=0x90000000;Out->DmaBytes=4096;Out->Cookie=&Released;return STATUS_SUCCESS;
}
static void VmFakeRelease(void *X,void *C,MALI_DDI_RELEASE_REASON R) {
    CHECK(X==&Adapter && C==&Released && GateHeld && R>=MaliDdiRejected && R<=MaliDdiReset);++Released;
}
static HANDLE TestVmAllocation2,TestVmOpen2;
static VOID *APIENTRY VmHandleData(const DXGKARGCB_GETHANDLEDATA *H) {
    CHECK(GateHeld && H && H->Type==DXGK_HANDLE_ALLOCATION);
    return H->hObject==10?(H->Flags.DeviceSpecific?OpenHandle:AllocationHandle):
        H->hObject==11?(H->Flags.DeviceSpecific?TestVmOpen2:TestVmAllocation2):0;
}
static NTSTATUS APIENTRY VmFeature(DXGKARGCB_QUERYFEATURESUPPORT *A) {
    CHECK(!GateHeld && Irql==PASSIVE_LEVEL && A && A->DeviceHandle==(HANDLE)0x777 &&
        A->FeatureId==DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD);A->Enabled=TRUE;return STATUS_SUCCESS;
}
static void BeginWith(UINT AllocationFlags,uint64_t AllocationBytes) {
    CHECK(!GateHeld && !TestPoolLive && !VmPoolLive);Irql=PASSIVE_LEVEL;
    TestVmAllocation2=TestVmOpen2=0;
    TestPoolCalls=TestPoolFrees=TestPoolFail=TestPoolCursor=0;VmPoolCalls=VmPoolFrees=VmPoolFail=0;
    Cleans=Barriers=MutatePtes=Released=TestVmSyncFailure=TestVmSyncCalls=TestVmFailSyncAt=0;RtlZeroMemory(OsTables,16u*4096u);
    for(unsigned I=0;I<256;I++)OsPhysical[I]=0x40001000+(uint64_t)I*4096;
    Init(&G,&Core);G.Reg[0xc4/4]=12;CHECK(MaliJmStart(&Core)==MaliOk);
    DXGKRNL_INTERFACE K={0};K.Size=sizeof(K);K.DeviceHandle=(HANDLE)0x777;
    K.Version=DXGKDDI_INTERFACE_VERSION_WDDM3_1;K.DxgkCbQueryFeatureSupport=VmFeature;
    K.DxgkCbSynchronizeExecution=VmSync;K.DxgkCbNotifyInterrupt=VmNotify;K.DxgkCbQueueDpc=VmDpc;
    K.DxgkCbNotifyDpc=VmNotifyDpc;K.DxgkCbGetHandleData=VmHandleData;
    MALI_DDI_HOOKS H={&Adapter,VmFakePrepare,VmFakeRelease,VmWake};
    if(TestVmInitializeHooks)TestVmInitializeHooks(&H);
    MALI_WDDM_CACHE_OPS C={&Adapter,VmClean,VmBarrier};
    CHECK(MtkMaliDdiInitialize(&Adapter,&Core,100,&K,0,&H,&C)==MaliOk);
    CHECK(MtkMaliVmInitialize(&Vm,&Adapter.Allocations,MaliVmDirectSystemPhysical)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiAttachVm(&Adapter,&Vm)==STATUS_SUCCESS);
    for(unsigned I=0;I<2;I++) {
        DXGKARG_CREATEPROCESS P={0};P.hDxgkProcess=(HANDLE)(uintptr_t)(0x1234+I);
        CHECK(MtkMaliDdiCreateProcess(&Adapter,&P)==STATUS_SUCCESS);Proc[I]=P.hKmdProcess;Owner[I]=((MALI_DDI_PROCESS*)Proc[I])->Owner;
        DXGKARG_CREATEDEVICE V={0};V.hKmdProcess=Proc[I];CHECK(MtkMaliDdiCreateDevice(&Adapter,&V)==STATUS_SUCCESS);Dev[I]=V.hDevice;
    }
    MALI_ALLOCATION_DESC Desc={1,sizeof(Desc),1,AllocationFlags,AllocationBytes,0,0,0,0};DXGK_ALLOCATIONINFO Info={0};
    Info.pPrivateDriverData=&Desc;Info.PrivateDriverDataSize=sizeof(Desc);
    DXGKARG_CREATEALLOCATION A={0};A.Flags.Resource=1;A.NumAllocations=1;A.pAllocationInfo=&Info;
    CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS);AllocationHandle=Info.hAllocation;ResourceHandle=A.hResource;
    DXGK_OPENALLOCATIONINFO OI={0};OI.hAllocation=10;DXGKARG_OPENALLOCATION O={0};O.NumAllocations=1;O.pOpenAllocation=&OI;
    CHECK(MtkMaliDdiOpenAllocation(Dev[0],&O)==STATUS_SUCCESS);OpenHandle=OI.hDeviceSpecificAllocation;
}
static void Begin(void) {BeginWith(0,8192);}
static void End(void) {
    DXGKARG_CLOSEALLOCATION C={1,&OpenHandle};CHECK(MtkMaliDdiCloseAllocation(Dev[0],&C)==STATUS_SUCCESS);
    DXGKARG_DESTROYALLOCATION A={0};A.NumAllocations=1;A.pAllocationList=&AllocationHandle;A.hResource=ResourceHandle;A.Flags.DestroyResource=1;
    CHECK(MtkMaliDdiDestroyAllocation(&Adapter,&A)==STATUS_SUCCESS);
    for(unsigned I=0;I<2;I++) {
        CHECK(MtkMaliDdiDestroyDevice(Dev[I])==STATUS_SUCCESS);
        CHECK(MtkMaliDdiDestroyProcess(&Adapter,Proc[I])==STATUS_SUCCESS);
    }
    CHECK(!Vm.TableCount && !Vm.HoldCount && !VmPoolLive && !TestPoolLive);
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_SUCCESS && !Adapter.Vm);
    CHECK(MtkMaliVmShutdown(&Vm)==STATUS_SUCCESS && !GateHeld);
}
static void Entry(UINT I,uint64_t Pa,UINT Leaf,UINT ReadOnly) {
    RtlZeroMemory(&Entries[I],sizeof(Entries[I]));Entries[I].Valid=1;
    if(Leaf){Entries[I].PageAddress=Pa;Entries[I].ReadOnly=ReadOnly;Entries[I].NoExecute=1;}
    else Entries[I].PageTableAddress=Pa;
}
static void Update(unsigned Table,unsigned Level,unsigned Process,UINT Start,UINT Count,UINT Initial,HANDLE Allocation,uint64_t Va) {
    RtlZeroMemory(&Build,sizeof(Build));Build.Operation=DXGK_OPERATION_UPDATE_PAGE_TABLE;
    Build.UpdatePageTable.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;Build.UpdatePageTable.PageTableLevel=Level;
    Build.UpdatePageTable.PageTableAddress.CpuVirtual=OsTables[Table];Build.UpdatePageTable.hProcess=Proc[Process];
    Build.UpdatePageTable.pPageTableEntries=Entries;Build.UpdatePageTable.StartIndex=Start;Build.UpdatePageTable.NumPageTableEntries=Count;
    Build.UpdatePageTable.Flags.InitialUpdate=Initial;Build.UpdatePageTable.hAllocation=Allocation;Build.UpdatePageTable.FirstPteVirtualAddress=Va;
}
static void Tree(unsigned First,unsigned Process,uint64_t Va,uint64_t Target) {
    for(unsigned L=0;L<3;L++) {
        Entry(0,OsPhysical[First+L+1],0,0);Update(First+L,3-L,Process,(UINT)((Va>>(39-L*9))&511),1,1,0,0);
        CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
    }
    Entry(0,Target,1,0);Entry(1,Target+0x3000,1,1);
    Update(First+3,0,Process,(UINT)((Va>>12)&511),2,1,AllocationHandle,Va);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
}
static NTSTATUS Lookup(MALI_VM_HOLD *H,uint64_t Va,MALI_VM_PAGE *Out) {
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);NTSTATUS S=MtkMaliVmLookup(&Vm,H,Va,Out);Leave(&Adapter);return S;
}

#endif
