/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Memory.h"

static void CompleteStores(void) { __dsb(15); }
static int Owned(const MALI_WDK_MEMORY *M) {
    return M && M->Mdl && M->Cpu && M->Bytes && !(M->Bytes & 4095u) &&
        M->Bytes <= MALI_WDK_MAX_ALLOCATION && !MmGetMdlByteOffset(M->Mdl) &&
        MmGetMdlByteCount(M->Mdl) == M->Bytes;
}
static void FreeBacking(PMDL Mdl, void *Cpu) {
    if(Cpu) MmUnmapLockedPages(Cpu, Mdl);
    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
}
MALI_RESULT MtkMaliMemoryAllocate(MALI_WDK_MEMORY *M, size_t Bytes) {
    if(!M || !Bytes || Bytes > MALI_WDK_MAX_ALLOCATION || KeGetCurrentIrql() > APC_LEVEL)
        return MaliInvalid;
    if(M->Mdl || M->Cpu || M->Bytes || M->Pins || M->GpuDirty || M->Quarantined)
        return MaliBusy;
    for(uint32_t I=0; I<MALI_JM_MAX_SLOTS; I++) if(M->Fences[I]) return MaliBusy;
    Bytes = (Bytes + 4095u) & ~(size_t)4095u;
    PHYSICAL_ADDRESS Low, High, Skip;
    Low.QuadPart=4096; High.QuadPart=MALI_JM_PA_LIMIT-1; Skip.QuadPart=0;
    PMDL Mdl=MmAllocatePagesForMdlEx(Low, High, Skip, Bytes, MmNonCached,
                                    MM_ALLOCATE_FULLY_REQUIRED);
    if(!Mdl) return MaliOffline;
    if(MmGetMdlByteCount(Mdl)!=Bytes || MmGetMdlByteOffset(Mdl)) {
        FreeBacking(Mdl, 0); return MaliFault;
    }
    PPFN_NUMBER Pfns=MmGetMdlPfnArray(Mdl);
    for(size_t I=0; I<Bytes/4096u; I++) if(!Pfns[I] || Pfns[I]>=(MALI_JM_PA_LIMIT>>12)) {
        FreeBacking(Mdl, 0); return MaliUnsupported;
    }
    void *Cpu=MmMapLockedPagesSpecifyCache(Mdl, KernelMode, MmNonCached, 0, FALSE,
                                          NormalPagePriority | MdlMappingNoExecute);
    if(!Cpu) { FreeBacking(Mdl, 0); return MaliOffline; }
    if((uintptr_t)Cpu & 4095u) { FreeBacking(Mdl, Cpu); return MaliFault; }
    RtlZeroMemory(Cpu, Bytes);
    KeFlushIoBuffers(Mdl, FALSE, TRUE);
    CompleteStores();
    M->Mdl=Mdl; M->Cpu=Cpu; M->Bytes=Bytes;
    return MaliOk;
}
MALI_RESULT MtkMaliMemoryPhysical(const MALI_WDK_MEMORY *M, size_t Page, uint64_t *Pa) {
    if(!Pa || !Owned(M) || Page>=M->Bytes/4096u) return MaliInvalid;
    PFN_NUMBER Pfn=MmGetMdlPfnArray(M->Mdl)[Page];
    if(!Pfn || Pfn>=(MALI_JM_PA_LIMIT>>12)) return MaliUnsupported;
    *Pa=(uint64_t)Pfn<<12; return MaliOk;
}
MALI_RESULT MtkMaliMemoryPin(MALI_WDK_MEMORY *M, uint32_t Slot, uint64_t Fence, int GpuWrites) {
    if(!Owned(M) || Slot>=MALI_JM_MAX_SLOTS || !Fence ||
       (GpuWrites!=0 && GpuWrites!=1) || KeGetCurrentIrql()>DISPATCH_LEVEL) return MaliInvalid;
    if(M->Quarantined) return MaliFault;
    if(M->Pins & (1u<<Slot)) return MaliBusy;
    for(uint32_t I=0; I<MALI_JM_MAX_SLOTS; I++) if(M->Fences[I]==Fence) return MaliInvalid;
    if(!M->Pins) KeFlushIoBuffers(M->Mdl, FALSE, TRUE);
    if(GpuWrites && !M->GpuDirty) KeFlushIoBuffers(M->Mdl, TRUE, TRUE);
    CompleteStores();
    M->Fences[Slot]=Fence; M->Pins|=1u<<Slot;
    if(GpuWrites) M->GpuDirty=1;
    return MaliOk;
}
static void UndoUnsubmittedPin(MALI_WDK_MEMORY *M, uint32_t Slot) {
    /* Private: MaliJmSubmit guarantees every failure occurs before START. */
    M->Fences[Slot]=0; M->Pins &= ~(1u<<Slot);
    if(!M->Pins) {
        if(M->GpuDirty) KeFlushIoBuffers(M->Mdl, TRUE, TRUE);
        CompleteStores(); M->GpuDirty=0;
    }
}
MALI_RESULT MtkMaliMemorySubmit(MALI_JM *D, uint32_t Slot, uint32_t As,
                               uint64_t Chain, uint64_t Affinity, uint64_t Fence,
                               const MALI_WDK_RESIDENT *Buffers, uint32_t Count) {
    MALI_WDK_RESIDENT OwnedBuffers[MALI_WDK_MAX_JOB_BUFFERS];
    if(!D || Slot>=MALI_JM_MAX_SLOTS || As>=MALI_JM_MAX_AS || !Fence || !Buffers ||
       !Count || Count>MALI_WDK_MAX_JOB_BUFFERS || KeGetCurrentIrql()>DISPATCH_LEVEL)
        return MaliInvalid;
    /* Snapshot the caller's list before recording the first in-flight reference. */
    for(uint32_t I=0; I<Count; I++) {
        OwnedBuffers[I]=Buffers[I];
        if(!Owned(OwnedBuffers[I].Memory) ||
           (OwnedBuffers[I].GpuWrites!=0 && OwnedBuffers[I].GpuWrites!=1)) return MaliInvalid;
        for(uint32_t J=0; J<I; J++)
            if(OwnedBuffers[J].Memory==OwnedBuffers[I].Memory) return MaliInvalid;
    }
    uint32_t Pinned=0; MALI_RESULT R=MaliOk;
    for(; Pinned<Count; Pinned++) {
        R=MtkMaliMemoryPin(OwnedBuffers[Pinned].Memory, Slot, Fence, OwnedBuffers[Pinned].GpuWrites);
        if(R) break;
    }
    if(!R) R=MaliJmSubmit(D, Slot, As, Chain, Affinity, Fence);
    if(R) while(Pinned) UndoUnsubmittedPin(OwnedBuffers[--Pinned].Memory, Slot);
    return R;
}
MALI_RESULT MtkMaliMemoryRetire(MALI_WDK_MEMORY *M, const MALI_EVENT *E) {
    if(!Owned(M) || !E || E->Slot>=MALI_JM_MAX_SLOTS || !E->Fence ||
       KeGetCurrentIrql()>DISPATCH_LEVEL || !(M->Pins & (1u<<E->Slot)) ||
       M->Fences[E->Slot]!=E->Fence) return MaliInvalid;
    if(E->Type==MaliJobFailed) { M->Quarantined=1; return MaliFault; }
    if(E->Type!=MaliJobDone && E->Type!=MaliJobAborted) return MaliInvalid;
    if(E->Type==MaliJobDone && (E->Status & 255u)!=1u) return MaliInvalid;
    if(M->Quarantined && E->Type!=MaliJobAborted) return MaliFault;
    M->Fences[E->Slot]=0; M->Pins &= ~(1u<<E->Slot);
    if(!M->Pins) {
        if(M->GpuDirty) KeFlushIoBuffers(M->Mdl, TRUE, TRUE);
        CompleteStores(); M->GpuDirty=0; M->Quarantined=0;
    }
    return MaliOk;
}
MALI_RESULT MtkMaliMemoryRelease(MALI_WDK_MEMORY *M) {
    if(!M || KeGetCurrentIrql()>APC_LEVEL) return MaliInvalid;
    if(M->Pins || M->Quarantined) return MaliBusy;
    if(!M->Mdl && !M->Cpu && !M->Bytes) return MaliOk;
    if(!Owned(M)) return MaliInvalid;
    FreeBacking(M->Mdl, M->Cpu);
    MALI_WDK_MEMORY Empty={0}; *M=Empty; return MaliOk;
}
static void CleanTable(void *Context, const void *Address, size_t Bytes) {
    MALI_WDK_MEMORY *M=Context;
    UNREFERENCED_PARAMETER(Address); UNREFERENCED_PARAMETER(Bytes);
    KeFlushIoBuffers(M->Mdl, FALSE, TRUE);
}
static void TableBarrier(void *Context) { UNREFERENCED_PARAMETER(Context); CompleteStores(); }
MALI_RESULT MtkMaliMemoryUpdateTable(MALI_WDK_MEMORY *M, MALI_WDDM_BRIDGE *Bridge,
                                   const DXGKARG_BUILDPAGINGBUFFER *Build) {
    if(!Owned(M) || !Bridge || !Build || M->Bytes!=4096u || KeGetCurrentIrql()>APC_LEVEL)
        return MaliInvalid;
    if(M->Pins || M->Quarantined) return MaliBusy;
    if(Build->Operation!=DXGK_OPERATION_UPDATE_PAGE_TABLE ||
       Build->UpdatePageTable.UpdateMode!=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL)
        return MaliUnsupported;
    if(Build->UpdatePageTable.PageTableAddress.CpuVirtual!=M->Cpu) return MaliInvalid;
    MALI_WDDM_CACHE_OPS Ops={M, CleanTable, TableBarrier};
    MALI_RESULT R=MtkMaliWddmInitialize(Bridge, &Ops);
    if(R) return R;
    return MtkMaliWddmUpdatePageTable(Bridge, Build);
}
