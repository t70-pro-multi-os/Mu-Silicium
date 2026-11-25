/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual WDK Memory.c with allocation/cache imports replaced at the OS boundary. */
#include "Memory.h"
static struct { MDL Mdl; PFN_NUMBER Pages[2]; } Backing;
__declspec(align(4096)) static unsigned char Data[8192];
static KIRQL Irql;
static unsigned Allocations, Maps, Unmaps, PageFrees, PoolFrees, Flushes, Reads;
static unsigned FailureMode, FirstFailure;
unsigned MtkMaliMemoryAssertionCount;
#define CHECK(C) do { ++MtkMaliMemoryAssertionCount; if(!(C) && !FirstFailure) FirstFailure=__LINE__; } while(0)
static PMDL TestAllocate(PHYSICAL_ADDRESS Low, PHYSICAL_ADDRESS High, PHYSICAL_ADDRESS Skip,
                        SIZE_T Bytes, MEMORY_CACHING_TYPE Cache, ULONG Flags) {
    ++Allocations;
    CHECK(Low.QuadPart==4096 && High.QuadPart==MALI_JM_PA_LIMIT-1 && !Skip.QuadPart);
    CHECK(Cache==MmNonCached && Flags==MM_ALLOCATE_FULLY_REQUIRED && Bytes<=sizeof(Data));
    if(FailureMode==1) return 0;
    Backing.Mdl.ByteOffset=FailureMode==2 ? 1 : 0;
    Backing.Mdl.ByteCount=(ULONG)(FailureMode==3 ? Bytes-4096 : Bytes);
    Backing.Pages[0]=FailureMode==4 ? 0 : FailureMode==5 ? (MALI_JM_PA_LIMIT>>12) : 0x12345;
    Backing.Pages[1]=0x67890;
    return &Backing.Mdl;
}
static void *TestMap(PMDL Mdl, KPROCESSOR_MODE Mode, MEMORY_CACHING_TYPE Cache,
                     PVOID Address, ULONG Bugcheck, ULONG Priority) {
    ++Maps;
    CHECK(Mdl==&Backing.Mdl && Mode==KernelMode && Cache==MmNonCached && !Address && !Bugcheck);
    CHECK(Priority==(NormalPagePriority|MdlMappingNoExecute));
    return FailureMode==6 ? 0 : FailureMode==7 ? Data+1 : Data;
}
static void TestUnmap(void *Cpu, PMDL Mdl) { ++Unmaps; CHECK((Cpu==Data || Cpu==Data+1) && Mdl==&Backing.Mdl); }
static void TestFreePages(PMDL Mdl) { ++PageFrees; CHECK(Mdl==&Backing.Mdl); }
static void TestFreePool(void *Mdl) { ++PoolFrees; CHECK(Mdl==&Backing.Mdl); }
static void TestFlush(PMDL Mdl, BOOLEAN Read, BOOLEAN Dma) {
    ++Flushes; Reads+=Read ? 1u : 0u; CHECK(Mdl!=0 && Dma);
}
#define KeGetCurrentIrql() Irql
#define MmAllocatePagesForMdlEx TestAllocate
#define MmMapLockedPagesSpecifyCache TestMap
#define MmUnmapLockedPages TestUnmap
#define MmFreePagesFromMdl TestFreePages
#define ExFreePool TestFreePool
#define KeFlushIoBuffers TestFlush
#include "Memory.c"
static MALI_WDDM_BRIDGE Bridge;
static DXGKARG_BUILDPAGINGBUFFER Build;
static DXGK_PTE Entry;

static void AllocationFailures(void) {
    for(FailureMode=1; FailureMode<=7; FailureMode++) {
        MALI_WDK_MEMORY M={0};
        unsigned F=PageFrees, P=PoolFrees, U=Unmaps, C=Flushes;
        CHECK(MtkMaliMemoryAllocate(&M, 4096)!=MaliOk);
        CHECK(!M.Mdl && !M.Cpu && !M.Bytes && !M.Pins);
        CHECK(PageFrees-F==(FailureMode!=1) && PoolFrees-P==(FailureMode!=1));
        CHECK(Unmaps-U==(FailureMode==7) && Flushes==C);
    }
    FailureMode=0;
}
static void Lifetime(void) {
    MALI_WDK_MEMORY M={0}; uint64_t Pa=0;
    CHECK(MtkMaliMemoryAllocate(&M, 4097)==MaliOk);
    CHECK(M.Bytes==8192 && M.Cpu==Data && M.Mdl==&Backing.Mdl);
    for(size_t I=0; I<M.Bytes; I++) CHECK(!Data[I]);
    CHECK(MtkMaliMemoryAllocate(&M, 4096)==MaliBusy);
    CHECK(MtkMaliMemoryPhysical(&M, 0, &Pa)==MaliOk && Pa==UINT64_C(0x12345000));
    CHECK(MtkMaliMemoryPhysical(&M, 1, &Pa)==MaliOk && Pa==UINT64_C(0x67890000));
    CHECK(MtkMaliMemoryPhysical(&M, 2, &Pa)==MaliInvalid);
    CHECK(MtkMaliMemoryPhysical(&M, 0, 0)==MaliInvalid);
    CHECK(MtkMaliMemoryPin(&M, 3, 10, 0)==MaliInvalid);
    CHECK(MtkMaliMemoryPin(&M, 0, 0, 0)==MaliInvalid);
    CHECK(MtkMaliMemoryPin(&M, 0, 10, 2)==MaliInvalid);
    unsigned C=Flushes, R=Reads;
    CHECK(MtkMaliMemoryPin(&M, 0, 10, 1)==MaliOk);
    CHECK(Flushes==C+2 && Reads==R+1 && M.GpuDirty && M.Pins==1);
    CHECK(MtkMaliMemoryPin(&M, 0, 11, 0)==MaliBusy);
    CHECK(MtkMaliMemoryPin(&M, 1, 10, 0)==MaliInvalid);
    CHECK(MtkMaliMemoryPin(&M, 1, 11, 0)==MaliOk && Flushes==C+2);
    CHECK(MtkMaliMemoryRelease(&M)==MaliBusy);
    MALI_EVENT E={MaliJobDone, 0, 0, 1, 99, 0};
    CHECK(MtkMaliMemoryRetire(&M, &E)==MaliInvalid && M.Pins==3);
    E.Fence=10; E.Status=0;
    CHECK(MtkMaliMemoryRetire(&M, &E)==MaliInvalid && M.Pins==3);
    E.Status=1;
    CHECK(MtkMaliMemoryRetire(&M, &E)==MaliOk && M.Pins==2 && Flushes==C+2);
    E.Slot=1; E.Fence=11; E.Type=MaliJobFailed;
    CHECK(MtkMaliMemoryRetire(&M, &E)==MaliFault && M.Quarantined && M.Pins==2);
    CHECK(MtkMaliMemoryRelease(&M)==MaliBusy);
    CHECK(MtkMaliMemoryPin(&M, 2, 12, 0)==MaliFault);
    E.Type=MaliJobDone;
    CHECK(MtkMaliMemoryRetire(&M, &E)==MaliFault && M.Pins==2);
    E.Type=MaliJobAborted;
    CHECK(MtkMaliMemoryRetire(&M, &E)==MaliOk && !M.Pins && !M.GpuDirty && !M.Quarantined);
    CHECK(Flushes==C+3 && Reads==R+2);
    CHECK(MtkMaliMemoryRetire(&M, &E)==MaliInvalid);
    unsigned U=Unmaps, F=PageFrees, P=PoolFrees;
    CHECK(MtkMaliMemoryRelease(&M)==MaliOk && !M.Mdl && !M.Cpu && !M.Bytes);
    CHECK(Unmaps==U+1 && PageFrees==F+1 && PoolFrees==P+1);
    CHECK(MtkMaliMemoryRelease(&M)==MaliOk && Unmaps==U+1);
}
static void TableUpdate(void) {
    MALI_WDK_MEMORY M={0};
    CHECK(MtkMaliMemoryAllocate(&M, 4096)==MaliOk);
    RtlZeroMemory(&Build,sizeof(Build)); RtlZeroMemory(&Entry,sizeof(Entry));
    Build.Operation=DXGK_OPERATION_UPDATE_PAGE_TABLE;
    Build.UpdatePageTable.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    Build.UpdatePageTable.PageTableAddress.CpuVirtual=M.Cpu;
    Build.UpdatePageTable.NumPageTableEntries=1;
    Build.UpdatePageTable.pPageTableEntries=&Entry;
    Entry.Valid=1; Entry.PageAddress=UINT64_C(0x22345000); Entry.NoExecute=1;
    unsigned C=Flushes;
    CHECK(MtkMaliMemoryUpdateTable(&M,&Bridge,&Build)==MaliOk);
    CHECK(((uint64_t*)Data)[0]==UINT64_C(0x00400000223454cb) && Flushes==C+1);
    CHECK(MtkMaliMemoryPin(&M,0,44,0)==MaliOk);
    CHECK(MtkMaliMemoryUpdateTable(&M,&Bridge,&Build)==MaliBusy);
    MALI_EVENT E={MaliJobDone,0,0,1,44,0};
    CHECK(MtkMaliMemoryRetire(&M,&E)==MaliOk);
    Build.UpdatePageTable.PageTableAddress.CpuVirtual=Data+4096;
    CHECK(MtkMaliMemoryUpdateTable(&M,&Bridge,&Build)==MaliInvalid);
    CHECK(MtkMaliMemoryRelease(&M)==MaliOk);
}
static void IrqlAndArguments(void) {
    MALI_WDK_MEMORY M={0}; unsigned A=Allocations;
    CHECK(MtkMaliMemoryAllocate(0,1)==MaliInvalid);
    CHECK(MtkMaliMemoryAllocate(&M,0)==MaliInvalid);
    CHECK(MtkMaliMemoryAllocate(&M,(size_t)MALI_WDK_MAX_ALLOCATION+1)==MaliInvalid);
    Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliMemoryAllocate(&M,1)==MaliInvalid && Allocations==A);
    CHECK(MtkMaliMemoryRelease(&M)==MaliInvalid);
    Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliMemoryAllocate(&M,4096)==MaliOk);
    Irql=DISPATCH_LEVEL+1;
    CHECK(MtkMaliMemoryPin(&M,0,1,0)==MaliInvalid);
    MALI_EVENT E={MaliJobDone,0,0,1,1,0};
    CHECK(MtkMaliMemoryRetire(&M,&E)==MaliInvalid);
    Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliMemoryRelease(&M)==MaliOk);
}
typedef struct { unsigned Starts, Complete; } SUBMIT_IO;
static uint32_t SubmitRead(void *Context, uint32_t Offset) {
    SUBMIT_IO *Io=Context;
    if(Offset==0x140) return 1;
    if(Offset==0x1000) return Io->Complete;
    if(Offset==0x1824) return 1;
    return 0;
}
static void SubmitWrite(void *Context, uint32_t Offset, uint32_t Value) {
    SUBMIT_IO *Io=Context;
    if(Offset==0x1860 && Value==1) ++Io->Starts;
    if(Offset==0x1004) Io->Complete &= ~Value;
}
static int SubmitPower(void *Context) { UNREFERENCED_PARAMETER(Context); return 1; }
static void SubmitBarrier(void *Context) { UNREFERENCED_PARAMETER(Context); }
static void SubmitDelay(void *Context, uint32_t Us) { UNREFERENCED_PARAMETER(Context); UNREFERENCED_PARAMETER(Us); }
static void SubmitUnwind(void) {
    MALI_WDK_MEMORY A={0}, B={0};
    CHECK(MtkMaliMemoryAllocate(&A,4096)==MaliOk);
    /* A second independent owned MDL fixture; no two objects own one MDL. */
    struct { MDL Mdl; PFN_NUMBER Pages[1]; } Second;
    __declspec(align(4096)) static unsigned char SecondData[4096];
    RtlZeroMemory(&Second,sizeof(Second));
    Second.Mdl.ByteCount=4096; Second.Pages[0]=0x9988;
    B.Mdl=&Second.Mdl; B.Cpu=SecondData; B.Bytes=4096;
    MALI_WDK_RESIDENT Refs[2]={{&A,1},{&B,0}};
    MALI_JM D={0};
    CHECK(MtkMaliMemorySubmit(&D,0,0,0x1000,1,77,Refs,2)==MaliOffline);
    CHECK(!A.Pins && !A.Fences[0] && !A.GpuDirty && !B.Pins);
    Refs[1].Memory=&A;
    CHECK(MtkMaliMemorySubmit(&D,0,0,0x1000,1,77,Refs,2)==MaliInvalid && !A.Pins);
    Refs[1].Memory=&B; B.Quarantined=1;
    CHECK(MtkMaliMemorySubmit(&D,0,0,0x1000,1,77,Refs,2)==MaliFault);
    CHECK(!A.Pins && !A.Fences[0] && !A.GpuDirty && !B.Pins);
    B.Quarantined=0; B.Pins=1; B.Fences[0]=88;
    CHECK(MtkMaliMemorySubmit(&D,0,0,0x1000,1,77,Refs,2)==MaliBusy);
    CHECK(!A.Pins && B.Pins==1 && B.Fences[0]==88);
    CHECK(MtkMaliMemorySubmit(&D,0,0,0x1000,1,77,Refs,0)==MaliInvalid);
    CHECK(MtkMaliMemorySubmit(&D,3,0,0x1000,1,77,Refs,1)==MaliInvalid);
    B.Pins=0; B.Fences[0]=0;
    SUBMIT_IO Io={0};
    MALI_IO Callbacks={&Io,SubmitRead,SubmitWrite,SubmitBarrier,SubmitDelay,SubmitPower};
    CHECK(MaliJmInitialize(&D,&Callbacks)==MaliOk);
    /* Fake inherited ready AS; CoreTests separately exercises reset/power/bind. */
    D.Started=1; D.Caps.JobSlots=1; D.Caps.AddressSpaces=1; D.Caps.Shaders=1;
    D.BoundSpaces=1; D.Roots[0]=UINT64_C(0x12345000);
    CHECK(MtkMaliMemorySubmit(&D,0,0,0x1000,1,99,Refs,2)==MaliOk);
    CHECK(Io.Starts==1 && A.Pins==1 && B.Pins==1 && D.Jobs[0].Active);
    CHECK(MtkMaliMemoryRelease(&A)==MaliBusy);
    Io.Complete=1;
    MALI_EVENT Events[MALI_JM_MAX_EVENTS]; uint32_t Count=0;
    CHECK(MaliJmPoll(&D,Events,MALI_JM_MAX_EVENTS,&Count)==MaliOk && Count==1);
    CHECK(Events[0].Type==MaliJobDone && Events[0].Fence==99 && !D.Jobs[0].Active);
    CHECK(MtkMaliMemoryRetire(&A,&Events[0])==MaliOk);
    CHECK(MtkMaliMemoryRetire(&B,&Events[0])==MaliOk && !A.Pins && !B.Pins);
    CHECK(MtkMaliMemoryRelease(&A)==MaliOk);
}
int TestMain(void) {
    for(size_t I=0; I<sizeof(Data); I++) Data[I]=0xaa;
    IrqlAndArguments(); AllocationFailures(); Lifetime(); TableUpdate(); SubmitUnwind();
    return (int)FirstFailure;
}
