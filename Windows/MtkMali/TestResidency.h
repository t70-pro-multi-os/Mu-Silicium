#ifndef T70_MALI_TEST_RESIDENCY_H
#define T70_MALI_TEST_RESIDENCY_H
/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real MDL residency + private tables + preparation + scheduler/DDIs.
   Only OS allocation/MDL/cache services, callbacks and GPU MMIO are fixtures.
   Windows exception dispatch is not emulated; the real SEH path is compiled. */
#define _VCRTIMP
#include "Residency.h"
#include "TestVm.h"
#define OS_SHARED_PAGES 82u
#define OS_MEMORY_SLOTS 512u
#define OS_PRIVATE_PAGES (MALI_COMMAND_SHADOW_BYTES/4096u)
#define OS_MDL_SLOTS 128u
static __declspec(align(4096)) uint8_t SharedData[OS_SHARED_PAGES*4096];
static uint64_t SharedPhysical[OS_SHARED_PAGES];
static struct {MDL Mdl;PFN_NUMBER Pfns[OS_SHARED_PAGES];unsigned Live,Locked,Write,Index,Cleans,Invalidates;} SharedMdls[OS_MDL_SLOTS];
static struct {MDL Mdl;PFN_NUMBER Pfns[OS_PRIVATE_PAGES];unsigned Live;} OsMemory[OS_MEMORY_SLOTS];
static __declspec(align(4096)) uint8_t OsMemoryData[OS_MEMORY_SLOTS][MALI_COMMAND_SHADOW_BYTES];
static unsigned MdlCalls,MdlFrees,LockedPages,UnlockedPages,SharedLive,PrivateLive,MemoryCalls,MemoryFrees,FlushCalls;
static unsigned FailMdl,FailMap,FailPfn,FailPfnPage,FailMemory,MemoryAlias;
static unsigned SharedPages,SourcePages,Jobs,DmaLength;
static uint64_t DmaVa,TargetVa;
static MALI_RESIDENCY Residency;
static MALI_PREPARE_SERVICE TestPrepareService;
static MALI_PREPARE_OPS Ops;
static MALI_EXEC_PACKET Private;
static void *ResidencyPoolAllocate(POOL_FLAGS F,SIZE_T N,ULONG Tag) {
    CHECK(Tag==0x52303754u);return VmPoolAllocate(F,N,0x56303754u);
}
static void ResidencyPoolFree(void *P,ULONG Tag) {CHECK(Tag==0x52303754u);VmPoolFree(P,0x56303754u);}
static PMDL OsIoAllocate(PVOID P,ULONG N,BOOLEAN Secondary,BOOLEAN Quota,PIRP Irp) {
    CHECK(GateHeld && Irql==PASSIVE_LEVEL && N && !(N&4095u) && !Secondary && !Quota && !Irp);
    uintptr_t Offset=(uintptr_t)P-(uintptr_t)SharedData;CHECK(Offset<sizeof(SharedData) && N<=sizeof(SharedData)-Offset && !(Offset&4095u));
    ++MdlCalls;if(MdlCalls==FailMdl)return 0;
    for(UINT I=0;I<OS_MDL_SLOTS;I++)if(!SharedMdls[I].Live) {
        RtlZeroMemory(&SharedMdls[I],sizeof(SharedMdls[I]));SharedMdls[I].Live=1;
        SharedMdls[I].Index=(UINT)(Offset/4096);SharedMdls[I].Mdl.StartVa=P;SharedMdls[I].Mdl.ByteCount=N;
        ++SharedLive;return &SharedMdls[I].Mdl;
    }
    return 0;
}
static UINT SharedIndex(PMDL M) {
    for(UINT I=0;I<OS_MDL_SLOTS;I++)if(M==&SharedMdls[I].Mdl){CHECK(SharedMdls[I].Live);return I;}
    CHECK(0);return 0;
}
static void OsIoFree(PMDL M) {
    UINT I=SharedIndex(M);CHECK(GateHeld && !SharedMdls[I].Locked && SharedLive);
    SharedMdls[I].Live=0;--SharedLive;++MdlFrees;
}
static void OsProbe(PMDL M,KPROCESSOR_MODE Mode,LOCK_OPERATION Access) {
    UINT I=SharedIndex(M);CHECK(GateHeld && Mode==KernelMode && !SharedMdls[I].Locked &&
        (Access==IoReadAccess || Access==IoWriteAccess));
    SharedMdls[I].Locked=1;SharedMdls[I].Write=Access==IoWriteAccess;
    for(UINT J=0;J<M->ByteCount/4096u;J++)SharedMdls[I].Pfns[J]=(PFN_NUMBER)(SharedPhysical[SharedMdls[I].Index+J]>>12);
    ++LockedPages;if(LockedPages==FailPfn)++SharedMdls[I].Pfns[FailPfnPage];M->MdlFlags|=MDL_PAGES_LOCKED;
}
static void OsUnlock(PMDL M) {
    UINT I=SharedIndex(M);CHECK(GateHeld && SharedMdls[I].Locked);
    SharedMdls[I].Locked=0;M->MdlFlags=0;++UnlockedPages;
}
static UINT MemoryIndex(PMDL M) {
    for(UINT I=0;I<OS_MEMORY_SLOTS;I++)if(M==&OsMemory[I].Mdl){CHECK(OsMemory[I].Live);return I;}
    CHECK(0);return 0;
}
static PMDL OsAllocate(PHYSICAL_ADDRESS Low,PHYSICAL_ADDRESS High,PHYSICAL_ADDRESS Skip,SIZE_T N,MEMORY_CACHING_TYPE Cache,ULONG Flags) {
    CHECK(GateHeld && Irql==PASSIVE_LEVEL && Low.QuadPart==4096 && High.QuadPart==MALI_JM_PA_LIMIT-1 &&
        !Skip.QuadPart && (N==4096 || N==8192 || N==MALI_COMMAND_SHADOW_BYTES) && Cache==MmNonCached && Flags==MM_ALLOCATE_FULLY_REQUIRED);
    ++MemoryCalls;if(MemoryCalls==FailMemory)return 0;
    for(UINT I=0;I<OS_MEMORY_SLOTS;I++)if(!OsMemory[I].Live) {
        RtlZeroMemory(&OsMemory[I],sizeof(OsMemory[I]));OsMemory[I].Live=1;
        OsMemory[I].Mdl.ByteCount=(ULONG)N;
        for(UINT J=0;J<N/4096u;J++)OsMemory[I].Pfns[J]=0x70000+I*OS_PRIVATE_PAGES+J;
        if(MemoryAlias==1)OsMemory[I].Pfns[0]=(PFN_NUMBER)(OsPhysical[0]>>12);
        if(MemoryAlias==2)OsMemory[I].Pfns[0]=(PFN_NUMBER)(SharedPhysical[0]>>12);
        if(MemoryAlias==3 && N>4096)OsMemory[I].Pfns[1]=OsMemory[I].Pfns[0];
        if(MemoryAlias==4 && MemoryCalls>=6)OsMemory[I].Pfns[0]=(PFN_NUMBER)(SharedPhysical[1]>>12);
        ++PrivateLive;return &OsMemory[I].Mdl;
    }
    return 0;
}
static PVOID OsMap(PMDL M,KPROCESSOR_MODE Mode,MEMORY_CACHING_TYPE Cache,PVOID Address,ULONG Bugcheck,ULONG Priority) {
    CHECK(GateHeld && Mode==KernelMode && !Address && !Bugcheck && (Priority&MdlMappingNoExecute));
    if(Cache==MmNonCached) {
        UINT I=MemoryIndex(M);CHECK(Priority==(NormalPagePriority|MdlMappingNoExecute));return OsMemoryData[I];
    }
    UINT I=SharedIndex(M);CHECK(Cache==MmCached && SharedMdls[I].Locked &&
        Priority==(NormalPagePriority|MdlMappingNoExecute|(SharedMdls[I].Write?0:MdlMappingNoWrite)));
    if(LockedPages==FailMap)return 0;
    M->MdlFlags|=MDL_MAPPED_TO_SYSTEM_VA;M->MappedSystemVa=M->StartVa;return M->StartVa;
}
/* WDK's already-parsed inline MmGetSystemAddressForMdlSafe calls the real
   symbol, while Memory.c calls the macro below. Both reach the same OS fixture. */
#undef MmMapLockedPagesSpecifyCache
PVOID MmMapLockedPagesSpecifyCache(PMDL M,KPROCESSOR_MODE Mode,MEMORY_CACHING_TYPE Cache,
    PVOID Address,ULONG Bugcheck,ULONG Priority) {return OsMap(M,Mode,Cache,Address,Bugcheck,Priority);}
static void OsUnmap(PVOID P,PMDL M) {UINT I=MemoryIndex(M);CHECK(GateHeld && P==OsMemoryData[I]);}
static void OsFreePages(PMDL M) {MemoryIndex(M);CHECK(GateHeld);}
static void OsFreePool(PVOID P) {
    UINT I=MemoryIndex(P);CHECK(GateHeld && PrivateLive);OsMemory[I].Live=0;--PrivateLive;++MemoryFrees;
}
static void OsFlush(PMDL M,BOOLEAN ReadOperation,BOOLEAN Dma) {
    CHECK(GateHeld && M && Dma && (ReadOperation==TRUE || ReadOperation==FALSE));++FlushCalls;
    for(UINT I=0;I<OS_MDL_SLOTS;I++)if(&SharedMdls[I].Mdl==M && SharedMdls[I].Live) {
        if(ReadOperation)++SharedMdls[I].Invalidates;else ++SharedMdls[I].Cleans;break;
    }
}
#define IoAllocateMdl OsIoAllocate
#define IoFreeMdl OsIoFree
#define MmProbeAndLockPages OsProbe
#define MmUnlockPages OsUnlock
#define MmAllocatePagesForMdlEx OsAllocate
#define MmMapLockedPagesSpecifyCache OsMap
#define MmUnmapLockedPages OsUnmap
#define MmFreePagesFromMdl OsFreePages
#undef ExFreePool
#define ExFreePool OsFreePool
#define KeFlushIoBuffers OsFlush
#include "Memory.c"
#undef ExAllocatePool2
#undef ExFreePoolWithTag
#define ExAllocatePool2 ResidencyPoolAllocate
#define ExFreePoolWithTag ResidencyPoolFree
#include "Residency.c"
#include "Prepare.c"
/* Unexpected exception dispatch fails this fixture rather than simulating a
   Windows exception unwinder. Successful probe and all returned-error paths run. */
EXCEPTION_DISPOSITION __cdecl __C_specific_handler(struct _EXCEPTION_RECORD *A,void *B,
    struct _CONTEXT *C,struct _DISPATCHER_CONTEXT *D) {
    UNREFERENCED_PARAMETER(A);UNREFERENCED_PARAMETER(B);UNREFERENCED_PARAMETER(C);UNREFERENCED_PARAMETER(D);
    CHECK(0);return ExceptionContinueSearch;
}
static void TestPut32(uint8_t *P,uint32_t V) {for(UINT I=0;I<4;I++)P[I]=(uint8_t)(V>>(I*8));}
static void TestPut64(uint8_t *P,uint64_t V) {TestPut32(P,(uint32_t)V);TestPut32(P+4,(uint32_t)(V>>32));}
static uint64_t TestGet64(const uint8_t *P) {uint64_t V=0;for(UINT I=0;I<8;I++)V|=(uint64_t)P[I]<<(I*8);return V;}
static void InitializeHooks(MALI_DDI_HOOKS *H) {
    MtkMaliResidencyGetOps(&Residency,&Ops);CHECK(MtkMaliPrepareInitialize(&TestPrepareService,&Core,&Ops,H)==STATUS_SUCCESS);
}
static void SetMappings(UINT ReadOnly) {
    uint64_t Base=DmaVa&~UINT64_C(4095);
    for(UINT L=0;L<3;L++) {
        Entry(0,OsPhysical[L+1],0,0);Update(L,3-L,0,(UINT)((Base>>(39-L*9))&511),1,1,0,0);
        CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
    }
    for(UINT I=0;I<SharedPages;I++)Entry(I,SharedPhysical[I],1,I>=SourcePages?ReadOnly:0);
    Update(3,0,0,(UINT)((Base>>12)&511u),SharedPages,1,AllocationHandle,Base);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
}
static void BeginResidency(UINT Sources,UINT JobCount,UINT Offset,UINT Length) {
    CHECK(!PrivateLive && !SharedLive && LockedPages==UnlockedPages);
    MdlCalls=MdlFrees=LockedPages=UnlockedPages=MemoryCalls=MemoryFrees=FlushCalls=0;
    FailMdl=FailMap=FailPfn=FailPfnPage=FailMemory=MemoryAlias=0;
    SourcePages=Sources;Jobs=JobCount;SharedPages=Sources+JobCount;DmaVa=0x80000000+Offset;DmaLength=Length;
    TargetVa=0x80000000+(uint64_t)Sources*4096;
    for(UINT I=0;I<OS_SHARED_PAGES;I++)SharedPhysical[I]=0x50000000+(uint64_t)I*3u*4096;
    TestVmInitializeHooks=InitializeHooks;BeginWith(3,(uint64_t)SharedPages*4096);TestVmInitializeHooks=0;
    DXGKARG_SETALLOCATIONBACKINGSTORE B={AllocationHandle,SharedData};CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&B)==STATUS_SUCCESS);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliResidencyInitialize(&Residency,&Adapter,&Vm,&Adapter,VmWake)==STATUS_SUCCESS);Leave(&Adapter);
    DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(MtkMaliDdiCreateContext(Dev[0],&C)==STATUS_SUCCESS);Ctx=C.hContext;
    DXGKARG_SETROOTPAGETABLE R={0};R.hContext=Ctx;R.NumEntries=512;R.Address.SegmentOffset=OsPhysical[0];
    MtkMaliDdiSetRootPageTable(&Adapter,&R);CHECK(Adapter.LastRootStatus==STATUS_SUCCESS);
    SetMappings(0);RtlZeroMemory(SharedData,(size_t)SharedPages*4096u);
    for(UINT I=0;I<Jobs;I++) {
        uint8_t *P=SharedData+Offset+I*128u;TestPut32(P+16,((71u+I)<<16)|4u);
        TestPut64(P+24,I+1<Jobs?DmaVa+(uint64_t)(I+1)*128:0);
        TestPut64(P+32,TargetVa+(uint64_t)I*4096);TestPut32(P+40,7);TestPut64(P+48,UINT64_C(0x0123456789abcdef)+I);
    }
    RtlZeroMemory(&Private,sizeof(Private));Private.Version=1;Private.Bytes=MALI_EXEC_PACKET_BYTES(1);
    Private.StepCount=1;Private.Steps[0].Chain=DmaVa;Private.Steps[0].Slot=1;
}
static NTSTATUS SubmitResidency(UINT Fence) {
    DXGKARG_SUBMITCOMMANDVIRTUAL A={0};A.hContext=Ctx;A.DmaBufferVirtualAddress=DmaVa;A.DmaBufferSize=DmaLength;
    A.pDmaBufferPrivateData=&Private;A.DmaBufferPrivateDataSize=Private.Bytes;A.SubmissionFenceId=Fence;
    return MtkMaliDdiSubmitCommandVirtual(&Adapter,&A);
}
static void EndResidency(void) {
    CHECK(!Residency.Count && !PrivateLive && !SharedLive && LockedPages==UnlockedPages && !Vm.HoldCount);
    CHECK(!Adapter.Allocations.ReferenceCount && VmPoolLive==4);
    CHECK(MtkMaliDdiDestroyContext(Ctx)==STATUS_SUCCESS);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliResidencyShutdown(&Residency)==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(MtkMaliPrepareShutdown(&TestPrepareService)==STATUS_SUCCESS);End();
}
static const uint64_t *ResolvePrivate(void *X,uint64_t Pa) {
    RESIDENCY_LEASE *L=X;
    for(UINT I=0;I<L->TableCount;I++)if(L->Tables[I].Physical==Pa)return L->Tables[I].Memory.Cpu;
    return 0;
}

#endif
