/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Vm.h"
#define VM_SIGNATURE 0x564d3730u
#define VM_TAG 0x56303754u
#define VM_PA_MASK (MALI_JM_PA_LIMIT-UINT64_C(4096))
typedef struct {HANDLE Allocation;uint64_t Key,Offset,Va;} VM_BINDING;
typedef struct MALI_VM_TABLE {
    struct MALI_VM_TABLE *Next;
    uint64_t Physical,Owner,Known[8],Entries[512];
    uint32_t Level;
    /* Leaf-only binding metadata follows this allocation. */
} VM_TABLE;
static VM_BINDING *VmBindings(VM_TABLE *T) {return (VM_BINDING*)(T+1);}
static int VmValid(const MALI_VM *V) {
    return KeGetCurrentIrql()==PASSIVE_LEVEL && V && V->Signature==VM_SIGNATURE;
}
static int VmPhysical(uint64_t P) {return P && P<MALI_JM_PA_LIMIT && !(P&4095u);}
static NTSTATUS VmResult(MALI_RESULT R) {
    return R==MaliOk?STATUS_SUCCESS:R==MaliUnsupported?STATUS_NOT_SUPPORTED:R==MaliBusy?STATUS_DEVICE_BUSY:STATUS_INVALID_PARAMETER;
}
static VM_TABLE *VmFind(const MALI_VM *V,uint64_t Physical) {
    for(VM_TABLE *T=V->Tables;T;T=T->Next)if(T->Physical==Physical)return T;
    return 0;
}
static int VmHolds(const MALI_VM *V,const MALI_VM_HOLD *H) {
    for(MALI_VM_HOLD *P=V->Holds;P;P=P->Next)if(P==H)return 1;
    return 0;
}
static void VmRemove(MALI_VM *V,VM_TABLE *T) {
    VM_TABLE **P=&V->Tables;while(*P!=T)P=&(*P)->Next;
    *P=T->Next;--V->TableCount;ExFreePoolWithTag(T,VM_TAG);
}
NTSTATUS MtkMaliVmInitialize(MALI_VM *V,MALI_ALLOCATION_BOOK *Book,MALI_VM_ADDRESS_MODEL Model) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !V || !Book)return STATUS_INVALID_PARAMETER;
    if(Model!=MaliVmDirectSystemPhysical)return STATUS_NOT_SUPPORTED;
    RtlZeroMemory(V,sizeof(*V));V->Book=Book;V->Signature=VM_SIGNATURE;return STATUS_SUCCESS;
}
int MtkMaliVmUsesBook(const MALI_VM *V,const MALI_ALLOCATION_BOOK *B) {return VmValid(V) && V->Book==B;}
NTSTATUS MtkMaliVmShutdown(MALI_VM *V) {
    if(!VmValid(V))return STATUS_INVALID_PARAMETER;
    if(V->TableCount || V->HoldCount)return STATUS_DEVICE_BUSY;
    V->Signature=0;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliVmUpdate(MALI_VM *V,MALI_WDDM_BRIDGE *Bridge,uint64_t Owner,DXGKARG_BUILDPAGINGBUFFER *A) {
    if(!VmValid(V) || !Bridge || !Owner || !A || A->Operation!=DXGK_OPERATION_UPDATE_PAGE_TABLE)return STATUS_INVALID_PARAMETER;
    if(V->HoldCount)return STATUS_DEVICE_BUSY;
    DXGKARG_BUILDPAGINGBUFFER Snapshot=*A;
    DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *U=&Snapshot.UpdatePageTable;
    if(A->pDmaBuffer || U->UpdateMode!=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL || U->PageTableLevel>=4 ||
       U->DriverProtection || U->Reserved0 || U->Flags.Reserved || U->Flags.Use64KBPages || U->Flags.NativeFence ||
       U->pPageTableEntries64KB)return STATUS_NOT_SUPPORTED;
    uintptr_t Cpu=(uintptr_t)U->PageTableAddress.CpuVirtual;
    if(!Cpu || (Cpu&4095u) || Cpu>UINTPTR_MAX-4096)return STATUS_INVALID_PARAMETER;
    PHYSICAL_ADDRESS Address=MmGetPhysicalAddress(U->PageTableAddress.CpuVirtual);
    uint64_t Pa=(uint64_t)Address.QuadPart;if(!VmPhysical(Pa))return STATUS_NOT_SUPPORTED;
    VM_TABLE *Old=VmFind(V,Pa);
    if(Old && (Old->Owner!=Owner || Old->Level!=U->PageTableLevel) && !U->Flags.InitialUpdate)
        return STATUS_INVALID_PARAMETER;
    if(U->Flags.NotifyEviction) {
        if(U->Flags.InitialUpdate)return STATUS_INVALID_PARAMETER;
        if(Old)VmRemove(V,Old);
        return STATUS_SUCCESS;
    }
    if(!U->NumPageTableEntries || U->StartIndex>512 || U->NumPageTableEntries>512-U->StartIndex ||
       !U->pPageTableEntries || ((uintptr_t)U->pPageTableEntries&(__alignof(DXGK_PTE)-1u)))
        return STATUS_INVALID_PARAMETER;
    uint64_t Key=0,Bytes=0;
    if(!U->PageTableLevel && U->hAllocation) {
        if((U->FirstPteVirtualAddress&4095u) || U->FirstPteVirtualAddress>=MALI_JM_VA_LIMIT ||
           ((U->FirstPteVirtualAddress>>12)&511u)!=U->StartIndex ||
           (uint64_t)U->NumPageTableEntries*4096u>MALI_JM_VA_LIMIT-U->FirstPteVirtualAddress)
            return STATUS_INVALID_PARAMETER;
        if(U->hAllocation) {
            MALI_ALLOCATION_IDENTITY I;
            NTSTATUS S=MtkMaliAllocationIdentity(V->Book,U->hAllocation,&I);if(S!=STATUS_SUCCESS)return S;
            Key=I.Key;Bytes=I.RoundedBytes;
            if((U->AllocationOffsetInBytes&4095u) || U->AllocationOffsetInBytes>=Bytes ||
               (uint64_t)U->NumPageTableEntries*4096u>Bytes-U->AllocationOffsetInBytes)
                return STATUS_INVALID_PARAMETER;
        }
    } else if(U->PageTableLevel && U->hAllocation)return STATUS_NOT_SUPPORTED;
    /* Allocate before the first hardware-table write. Publication cannot fail. */
    int New=!Old || U->Flags.InitialUpdate;
    VM_TABLE *T=Old;
    if(New) {
        if(!Old && V->TableCount==MALI_VM_TABLE_LIMIT)return STATUS_INSUFFICIENT_RESOURCES;
        size_t N=sizeof(*T)+(U->PageTableLevel?0:512u*sizeof(VM_BINDING));
        T=ExAllocatePool2(POOL_FLAG_NON_PAGED,N,VM_TAG);if(!T)return STATUS_NO_MEMORY;
        T->Physical=Pa;T->Owner=Owner;T->Level=U->PageTableLevel;
    }
    NTSTATUS S=VmResult(MtkMaliWddmUpdatePageTable(Bridge,&Snapshot));
    if(S!=STATUS_SUCCESS){if(New)ExFreePoolWithTag(T,VM_TAG);return S;}
    if(New){if(Old)VmRemove(V,Old);T->Next=V->Tables;V->Tables=T;++V->TableCount;}
    for(UINT I=0;I<U->NumPageTableEntries;I++) {
        UINT Index=U->StartIndex+I;T->Entries[Index]=Bridge->Encoded[I];T->Known[Index>>6]|=UINT64_C(1)<<(Index&63u);
        if(!T->Level) {
            VM_BINDING B={0};
            if(T->Entries[Index] && U->hAllocation) {
                B.Allocation=U->hAllocation;B.Key=Key;B.Offset=U->AllocationOffsetInBytes+(uint64_t)I*4096u;
                B.Va=U->FirstPteVirtualAddress+(uint64_t)I*4096u;
            }
            VmBindings(T)[Index]=B;
        }
    }
    /* Explicit invalidation allows complete deletion with no stale PFN record.
       NotifyEviction also ends this table's resident identity. */
    int Empty=1,AllKnown=1;
    for(UINT I=0;I<512;I++)if(T->Entries[I])Empty=0;
    for(UINT I=0;I<8;I++)if(T->Known[I]!=UINT64_MAX)AllKnown=0;
    if(U->Flags.NotifyEviction || (Empty && AllKnown))VmRemove(V,T);
    return STATUS_SUCCESS;
}
NTSTATUS MtkMaliVmForgetProcess(MALI_VM *V,uint64_t Owner) {
    if(!VmValid(V) || !Owner)return STATUS_INVALID_PARAMETER;
    for(MALI_VM_HOLD *H=V->Holds;H;H=H->Next)if(H->Owner==Owner)return STATUS_DEVICE_BUSY;
    VM_TABLE *T=V->Tables;
    while(T){VM_TABLE *N=T->Next;if(T->Owner==Owner)VmRemove(V,T);T=N;}
    return STATUS_SUCCESS;
}
NTSTATUS MtkMaliVmHold(MALI_VM *V,MALI_VM_HOLD *H,uint64_t Owner,uint64_t Root) {
    if(!VmValid(V) || !H || !Owner || !VmPhysical(Root) || VmHolds(V,H))return STATUS_INVALID_PARAMETER;
    VM_TABLE *T=VmFind(V,Root);if(!T || T->Owner!=Owner || T->Level!=3)return STATUS_INVALID_ADDRESS;
    if(V->HoldCount==MALI_VM_HOLD_LIMIT)return STATUS_INSUFFICIENT_RESOURCES;
    H->Owner=Owner;H->Root=Root;H->Live=1;H->Next=V->Holds;V->Holds=H;++V->HoldCount;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliVmDrop(MALI_VM *V,MALI_VM_HOLD *H) {
    if(!VmValid(V) || !H)return STATUS_INVALID_PARAMETER;
    MALI_VM_HOLD **P=&V->Holds;while(*P && *P!=H)P=&(*P)->Next;
    if(!*P)return STATUS_INVALID_HANDLE;
    *P=H->Next;--V->HoldCount;RtlZeroMemory(H,sizeof(*H));return STATUS_SUCCESS;
}
NTSTATUS MtkMaliVmLookup(MALI_VM *V,const MALI_VM_HOLD *H,uint64_t Va,MALI_VM_PAGE *Out) {
    if(!VmValid(V) || !H || !Out || Va>=MALI_JM_VA_LIMIT || !VmHolds(V,H))return STATUS_INVALID_PARAMETER;
    uint64_t Pa=H->Root,Seen[4];
    for(UINT Level=0;Level<4;Level++) {
        for(UINT J=0;J<Level;J++)if(Seen[J]==Pa)return STATUS_INVALID_ADDRESS;
        Seen[Level]=Pa;VM_TABLE *T=VmFind(V,Pa);
        if(!T || T->Owner!=H->Owner || T->Level!=3-Level)return STATUS_INVALID_ADDRESS;
        UINT Index=(UINT)((Va>>(39-Level*9))&511u);
        if(!(T->Known[Index>>6]&(UINT64_C(1)<<(Index&63u))))return STATUS_INVALID_ADDRESS;
        uint64_t E=T->Entries[Index];if(!E)return STATUS_GRAPHICS_ALLOCATION_INVALID;
        Pa=E&VM_PA_MASK;if(!VmPhysical(Pa))return STATUS_INVALID_ADDRESS;
        if(Level<3){if((E&~VM_PA_MASK)!=0x403)return STATUS_INVALID_ADDRESS;continue;}
        VM_BINDING *B=&VmBindings(T)[Index];
        if(!B->Allocation || !B->Key || B->Va!=(Va&~UINT64_C(4095)))return STATUS_ACCESS_DENIED;
        MALI_ALLOCATION_IDENTITY Identity;
        NTSTATUS S=MtkMaliAllocationIdentity(V->Book,B->Allocation,&Identity);
        if(S!=STATUS_SUCCESS || Identity.Key!=B->Key)return STATUS_INVALID_HANDLE;
        MALI_VM_PAGE P={0};P.Allocation=B->Allocation;P.AllocationKey=B->Key;
        P.Physical=Pa|(Va&4095u);P.AllocationOffset=B->Offset+(Va&4095u);
        P.Flags=MaliPteValid|MaliPteRead|((E&128u)?MaliPteWrite:0)|((E&(UINT64_C(1)<<54))?0:MaliPteExecute)|
            ((E&8u)?MaliPteCached:0);
        *Out=P;return STATUS_SUCCESS;
    }
    return STATUS_INVALID_ADDRESS;
}
int MtkMaliVmIsTablePage(const MALI_VM *V,uint64_t Physical) {
    return VmValid(V) && VmFind(V,Physical&~UINT64_C(4095))!=0;
}
