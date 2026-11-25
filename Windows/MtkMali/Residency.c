/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Residency.h"
#define RESIDENCY_SIGNATURE 0x52533730u
#define RESIDENCY_TAG 0x52303754u
#define RESIDENCY_SHADOW_VA (MALI_JM_VA_LIMIT-MALI_COMMAND_SHADOW_BYTES)
#define RESIDENCY_PA_MASK (MALI_JM_PA_LIMIT-UINT64_C(4096))
typedef struct {
    PMDL Mdl;
    void *Cpu;
    uint64_t Va,Physical,Reference;
    uint32_t Locked,Write;
} RESIDENCY_PAGE;
typedef struct {MALI_WDK_MEMORY Memory;uint64_t Physical;} RESIDENCY_TABLE;
typedef struct {
    PMDL Mdl;void *Cpu;
    uint64_t Va,Reference;
    uint32_t Bytes,Access,Locked;
} RESIDENCY_BUFFER;
typedef struct MALI_RESIDENCY_LEASE {
    struct MALI_RESIDENCY_LEASE *Next;
    MALI_VM_HOLD Hold;
    MALI_DDI_DEVICE *Device;
    HANDLE Process;
    uint64_t DmaVa;
    uint32_t DmaBytes,DataCount,TableCount,Copied,Reserved,Sealed;
    MALI_WDK_MEMORY Shadow;
    RESIDENCY_BUFFER Buffers[MALI_COMMAND_MAX_RESOURCES];
    uint64_t *ResourceHash;
    uint32_t BufferCount,ResourcePages,HashCapacity;
    RESIDENCY_PAGE Data[MALI_RESIDENCY_DATA_LIMIT];
    RESIDENCY_TABLE Tables[MALI_RESIDENCY_TABLE_LIMIT];
} RESIDENCY_LEASE;
static int ResidencyValid(const MALI_RESIDENCY *R) {
    return KeGetCurrentIrql()==PASSIVE_LEVEL && R && R->Signature==RESIDENCY_SIGNATURE &&
        R->Adapter && R->Vm && R->Adapter->Vm==R->Vm &&
        MtkMaliVmUsesBook(R->Vm,&R->Adapter->Allocations);
}
static RESIDENCY_LEASE *ResidencyLease(MALI_RESIDENCY *R,void *Token) {
    if(ResidencyValid(R))for(RESIDENCY_LEASE *L=R->Leases;L;L=L->Next)if(L==Token)return L;
    return 0;
}
static NTSTATUS ResidencyResult(MALI_RESULT V) {
    return V==MaliOk?STATUS_SUCCESS:V==MaliOffline?STATUS_NO_MEMORY:
        V==MaliUnsupported?STATUS_NOT_SUPPORTED:STATUS_INVALID_ADDRESS;
}
static int OwnedAlias(const MALI_RESIDENCY *R,uint64_t Pa,const MALI_WDK_MEMORY *Ignore) {
    for(const RESIDENCY_LEASE *L=R->Leases;L;L=L->Next) {
        if(&L->Shadow!=Ignore)for(size_t I=0;I<L->Shadow.Bytes/4096u;I++) {
            uint64_t P=0;if(MtkMaliMemoryPhysical(&L->Shadow,I,&P)!=MaliOk || P==Pa)return 1;
        }
        for(UINT I=0;I<L->TableCount;I++)if(&L->Tables[I].Memory!=Ignore && L->Tables[I].Physical==Pa)return 1;
    }
    return 0;
}
static uint64_t *HashSlot(uint64_t *Hash,uint32_t Capacity,uint64_t Pa) {
    if(!Capacity)return 0;
    uint32_t I=(uint32_t)((Pa>>12)*UINT64_C(11400714819323198485))&(Capacity-1u);
    for(uint32_t N=0;N<Capacity;N++,I=(I+1u)&(Capacity-1u))
        if(!Hash[I] || (Hash[I]&RESIDENCY_PA_MASK)==Pa)return &Hash[I];
    return 0;
}
static uint32_t ResourceAccess(const RESIDENCY_LEASE *L,uint64_t Pa) {
    uint64_t *P=HashSlot(L->ResourceHash,L->HashCapacity,Pa);return P?(uint32_t)(*P&15u):0;
}
static NTSTATUS GrowResourceHash(RESIDENCY_LEASE *L,uint32_t Pages) {
    uint32_t Capacity=64;while(Capacity<Pages*2u)Capacity*=2u;
    if(Capacity<=L->HashCapacity)return STATUS_SUCCESS;
    uint64_t *H=ExAllocatePool2(POOL_FLAG_NON_PAGED,(SIZE_T)Capacity*sizeof(*H),RESIDENCY_TAG);
    if(!H)return STATUS_NO_MEMORY;
    for(uint32_t I=0;I<L->HashCapacity;I++)if(L->ResourceHash[I]) {
        uint64_t V=L->ResourceHash[I];*HashSlot(H,Capacity,V&RESIDENCY_PA_MASK)=V;
    }
    if(L->ResourceHash)ExFreePoolWithTag(L->ResourceHash,RESIDENCY_TAG);
    L->ResourceHash=H;L->HashCapacity=Capacity;return STATUS_SUCCESS;
}
static int DataAlias(const MALI_RESIDENCY *R,uint64_t Pa) {
    for(const RESIDENCY_LEASE *L=R->Leases;L;L=L->Next) {
        if(ResourceAccess(L,Pa))return 1;
        for(UINT I=0;I<L->DataCount;I++)if(L->Data[I].Physical==Pa)return 1;
    }
    return 0;
}
static NTSTATUS AllocateOwned(MALI_RESIDENCY *R,MALI_WDK_MEMORY *M,size_t Bytes) {
    NTSTATUS S=ResidencyResult(MtkMaliMemoryAllocate(M,Bytes));if(S!=STATUS_SUCCESS)return S;
    for(size_t I=0;I<M->Bytes/4096u;I++) {
        uint64_t Pa=0;S=ResidencyResult(MtkMaliMemoryPhysical(M,I,&Pa));
        if(S==STATUS_SUCCESS && (MtkMaliVmIsTablePage(R->Vm,Pa) || OwnedAlias(R,Pa,M) || DataAlias(R,Pa)))S=STATUS_ACCESS_DENIED;
        for(size_t J=0;J<I && S==STATUS_SUCCESS;J++) {
            uint64_t Other=0;if(MtkMaliMemoryPhysical(M,J,&Other)!=MaliOk || Other==Pa)S=STATUS_ACCESS_DENIED;
        }
        if(S!=STATUS_SUCCESS){MtkMaliMemoryRelease(M);return S;}
    }
    return STATUS_SUCCESS;
}
static NTSTATUS NewTable(MALI_RESIDENCY *R,RESIDENCY_LEASE *L,RESIDENCY_TABLE **Out) {
    if(L->TableCount==MALI_RESIDENCY_TABLE_LIMIT)return STATUS_INSUFFICIENT_RESOURCES;
    RESIDENCY_TABLE *T=&L->Tables[L->TableCount];
    NTSTATUS S=AllocateOwned(R,&T->Memory,4096);if(S!=STATUS_SUCCESS)return S;
    S=ResidencyResult(MtkMaliMemoryPhysical(&T->Memory,0,&T->Physical));
    if(S!=STATUS_SUCCESS){MtkMaliMemoryRelease(&T->Memory);return S;}
    ++L->TableCount;*Out=T;return STATUS_SUCCESS;
}
static NTSTATUS MapPage(MALI_RESIDENCY *R,RESIDENCY_LEASE *L,uint64_t Va,uint64_t Pa,uint32_t Flags) {
    RESIDENCY_TABLE *T=&L->Tables[0];
    for(UINT Level=0;Level<4;Level++) {
        uint64_t *E=(uint64_t*)T->Memory.Cpu+((Va>>(39-Level*9))&511u);
        if(Level==3) {
            MALI_PTE P={Pa,MaliPteValid|(Flags&(MaliPteRead|MaliPteWrite|MaliPteExecute|MaliPteCached))};
            uint64_t Encoded=0;NTSTATUS S=ResidencyResult(MaliJmEncodePte(3,&P,&Encoded));
            if(S!=STATUS_SUCCESS)return S;
            if(*E && *E!=Encoded)return STATUS_ACCESS_DENIED;
            *E=Encoded;return STATUS_SUCCESS;
        }
        if(!*E) {
            RESIDENCY_TABLE *Next=0;NTSTATUS S=NewTable(R,L,&Next);if(S!=STATUS_SUCCESS)return S;
            MALI_PTE P={Next->Physical,MaliPteValid|MaliPteTable};
            S=ResidencyResult(MaliJmEncodePte(Level,&P,E));if(S!=STATUS_SUCCESS)return S;T=Next;
        } else {
            uint64_t Next=*E&RESIDENCY_PA_MASK;T=0;
            for(UINT I=0;I<L->TableCount;I++)if(L->Tables[I].Physical==Next){T=&L->Tables[I];break;}
            if(!T)return STATUS_INVALID_ADDRESS;
        }
    }
    return STATUS_INVALID_ADDRESS;
}
static NTSTATUS LockPage(RESIDENCY_PAGE *P,void *Backing) {
    P->Mdl=IoAllocateMdl(Backing,4096,FALSE,FALSE,0);if(!P->Mdl)return STATUS_NO_MEMORY;
    NTSTATUS S=STATUS_SUCCESS;
    __try {
        MmProbeAndLockPages(P->Mdl,KernelMode,P->Write?IoWriteAccess:IoReadAccess);
        P->Locked=1;
        P->Cpu=MmGetSystemAddressForMdlSafe(P->Mdl,NormalPagePriority|MdlMappingNoExecute|
            (P->Write?0:MdlMappingNoWrite));
    } __except(EXCEPTION_EXECUTE_HANDLER) {S=GetExceptionCode();}
    if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_UNSUCCESSFUL:S;
    if(!P->Cpu)return STATUS_NO_MEMORY;
    if(((uintptr_t)P->Cpu&4095u) || MmGetMdlByteOffset(P->Mdl) || MmGetMdlByteCount(P->Mdl)!=4096)
        return STATUS_INVALID_ADDRESS;
    PFN_NUMBER Pfn=MmGetMdlPfnArray(P->Mdl)[0];
    if(!Pfn || Pfn>=(MALI_JM_PA_LIMIT>>12) || ((uint64_t)Pfn<<12)!=P->Physical)return STATUS_INVALID_ADDRESS;
    return STATUS_SUCCESS;
}
static NTSTATUS PinPage(MALI_RESIDENCY *R,RESIDENCY_LEASE *L,uint64_t Va,uint32_t Bytes,int Write,RESIDENCY_PAGE **Out) {
    if(!Bytes || Va>=MALI_JM_VA_LIMIT || Bytes>MALI_JM_VA_LIMIT-Va || Bytes>4096u-(Va&4095u))return STATUS_INVALID_PARAMETER;
    MALI_VM_PAGE P;NTSTATUS S=MtkMaliVmLookup(R->Vm,&L->Hold,Va,&P);if(S!=STATUS_SUCCESS)return S;
    if(!(P.Flags&MaliPteRead) || (Write && !(P.Flags&MaliPteWrite)))return STATUS_ACCESS_DENIED;
    MALI_ALLOCATION_IDENTITY Identity;
    S=MtkMaliAllocationIdentity(&R->Adapter->Allocations,P.Allocation,&Identity);if(S!=STATUS_SUCCESS)return S;
    if(Identity.Key!=P.AllocationKey || P.AllocationOffset>=Identity.Bytes || Bytes>Identity.Bytes-P.AllocationOffset)
        return STATUS_INVALID_PARAMETER;
    uint64_t PageVa=Va&~UINT64_C(4095),Pa=P.Physical&~UINT64_C(4095);
    for(UINT I=0;I<L->DataCount;I++)if(L->Data[I].Va==PageVa) {
        RESIDENCY_PAGE *D=&L->Data[I];
        if(D->Physical!=Pa || D->Write!=(UINT)Write)return STATUS_ACCESS_DENIED;
        *Out=D;return STATUS_SUCCESS;
    }
    if(MtkMaliVmIsTablePage(R->Vm,Pa) || OwnedAlias(R,Pa,0))return STATUS_ACCESS_DENIED;
    if(Write)for(const RESIDENCY_LEASE *Other=R->Leases;Other;Other=Other->Next)
        if(ResourceAccess(Other,Pa)&MaliPteExecute)return STATUS_ACCESS_DENIED;
    for(UINT I=0;I<L->DataCount;I++)if(L->Data[I].Physical==Pa)return STATUS_ACCESS_DENIED;
    if(L->DataCount==MALI_RESIDENCY_DATA_LIMIT)return STATUS_INSUFFICIENT_RESOURCES;
    RESIDENCY_PAGE *D=&L->Data[L->DataCount++];D->Va=PageVa;D->Physical=Pa;D->Write=(UINT)Write;
    uint64_t Offset=P.AllocationOffset-(Va&4095u),N=Identity.Bytes-Offset;if(N>4096)N=4096;
    S=MtkMaliAllocationAcquireObject(&R->Adapter->Allocations,L->Device,L->Process,P.Allocation,
        P.AllocationKey,Offset,N,(BOOLEAN)Write,&D->Reference);if(S!=STATUS_SUCCESS)return S;
    MALI_ALLOCATION_VIEW V;S=MtkMaliAllocationReferenceView(&R->Adapter->Allocations,D->Reference,&V);if(S!=STATUS_SUCCESS)return S;
    if(!V.Backing || V.Key!=P.AllocationKey || Offset>V.BackingBytes || 4096u>V.BackingBytes-Offset ||
       (uintptr_t)V.Backing>UINTPTR_MAX-Offset-4096u)return STATUS_NOT_SUPPORTED;
    S=LockPage(D,(uint8_t*)V.Backing+(size_t)Offset);if(S!=STATUS_SUCCESS)return S;
    *Out=D;return STATUS_SUCCESS;
}
static NTSTATUS ResidencyBegin(void *X,HANDLE Process,uint64_t Id,uint64_t Root,uint64_t Va,uint32_t Bytes,void **Token) {
    MALI_RESIDENCY *R=X;if(!ResidencyValid(R) || !Token)return STATUS_INVALID_PARAMETER;
    *Token=0;
    if(R->Count==MALI_DDI_RESIDENTS)return STATUS_INSUFFICIENT_RESOURCES;
    if(!Bytes || Bytes>MALI_COMMAND_MAX_DMA || Va>=MALI_JM_VA_LIMIT || Bytes>MALI_JM_VA_LIMIT-Va)return STATUS_INVALID_PARAMETER;
    MALI_DDI_CONTEXT *C=0;
    for(UINT I=0;I<MALI_DDI_OBJECTS;I++)if(R->Adapter->Contexts[I].Live && R->Adapter->Contexts[I].Id==Id) {
        C=&R->Adapter->Contexts[I];break;
    }
    if(!C || C->Adapter!=R->Adapter || !C->Device || !C->Device->Live || !C->Device->Process ||
       !C->Device->Process->Live || C->Device->Process->DxgkProcess!=Process || C->Root!=Root ||
       C->RootStatus!=STATUS_SUCCESS)return STATUS_INVALID_HANDLE;
    RESIDENCY_LEASE *L=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*L),RESIDENCY_TAG);if(!L)return STATUS_NO_MEMORY;
    NTSTATUS S=MtkMaliVmHold(R->Vm,&L->Hold,C->Device->Process->Owner,Root);
    if(S!=STATUS_SUCCESS){ExFreePoolWithTag(L,RESIDENCY_TAG);return S;}
    L->Device=C->Device;L->Process=Process;L->DmaVa=Va;L->DmaBytes=Bytes;
    L->Next=R->Leases;R->Leases=L;++R->Count;*Token=L;return STATUS_SUCCESS;
}
static NTSTATUS ResidencyCopy(void *X,void *Token,void *Destination,uint32_t Bytes) {
    MALI_RESIDENCY *R=X;RESIDENCY_LEASE *L=ResidencyLease(R,Token);
    if(!L || !Destination || Bytes!=L->DmaBytes || L->Copied || L->Reserved)return STATUS_INVALID_PARAMETER;
    uint8_t *D=Destination;uint64_t Va=L->DmaVa;uint32_t Left=Bytes;
    while(Left) {
        uint32_t N=4096u-(uint32_t)(Va&4095u);if(N>Left)N=Left;
        RESIDENCY_PAGE *P=0;NTSTATUS S=PinPage(R,L,Va,N,0,&P);if(S!=STATUS_SUCCESS)return S;
        RtlCopyMemory(D,(uint8_t*)P->Cpu+(Va&4095u),N);D+=N;Va+=N;Left-=N;
    }
    L->Copied=1;return STATUS_SUCCESS;
}
static NTSTATUS ResidencyReserve(void *X,void *Token,uint32_t Bytes,void **Cpu,uint64_t *Va,uint64_t *Root) {
    MALI_RESIDENCY *R=X;RESIDENCY_LEASE *L=ResidencyLease(R,Token);
    if(!L || !Cpu || !Va || !Root || !L->Copied || L->Reserved || Bytes!=MALI_COMMAND_SHADOW_BYTES)return STATUS_INVALID_PARAMETER;
    NTSTATUS S=AllocateOwned(R,&L->Shadow,Bytes);if(S!=STATUS_SUCCESS)return S;
    RESIDENCY_TABLE *T=0;S=NewTable(R,L,&T);if(S!=STATUS_SUCCESS)return S;
    for(UINT I=0;I<Bytes/4096u;I++) {
        uint64_t Pa=0;S=ResidencyResult(MtkMaliMemoryPhysical(&L->Shadow,I,&Pa));if(S!=STATUS_SUCCESS)return S;
        S=MapPage(R,L,RESIDENCY_SHADOW_VA+(uint64_t)I*4096u,Pa,MaliPteRead|MaliPteWrite);if(S!=STATUS_SUCCESS)return S;
    }
    L->Reserved=1;*Cpu=L->Shadow.Cpu;*Va=RESIDENCY_SHADOW_VA;*Root=T->Physical;return STATUS_SUCCESS;
}
static NTSTATUS ResidencyResource(void *X,void *Token,uint64_t Va,uint32_t Bytes,uint32_t Access) {
    MALI_RESIDENCY *R=X;RESIDENCY_LEASE *L=ResidencyLease(R,Token);
    if(!L || !L->Reserved || L->Sealed || !Va || (Va&4095u) || !Bytes || (Bytes&4095u) ||
       Va>=RESIDENCY_SHADOW_VA || Bytes>RESIDENCY_SHADOW_VA-Va ||
       !(Access&MaliPteRead) || (Access&~(MaliPteRead|MaliPteWrite|MaliPteExecute)) ||
       ((Access&(MaliPteWrite|MaliPteExecute))==(MaliPteWrite|MaliPteExecute)))return STATUS_INVALID_PARAMETER;
    if(Va<L->DmaVa+L->DmaBytes && L->DmaVa<Va+Bytes)return STATUS_ACCESS_DENIED;
    for(UINT I=0;I<L->BufferCount;I++)if(Va<L->Buffers[I].Va+L->Buffers[I].Bytes && L->Buffers[I].Va<Va+Bytes)
        return STATUS_ACCESS_DENIED;
    if(L->BufferCount==MALI_COMMAND_MAX_RESOURCES || Bytes/4096u>MALI_COMMAND_MAX_RESOURCE_PAGES-L->ResourcePages)
        return STATUS_INSUFFICIENT_RESOURCES;
    MALI_VM_PAGE First;NTSTATUS S=MtkMaliVmLookup(R->Vm,&L->Hold,Va,&First);if(S!=STATUS_SUCCESS)return S;
    if((First.Flags&Access)!=Access || (First.AllocationOffset&4095u))return STATUS_ACCESS_DENIED;
    MALI_ALLOCATION_IDENTITY Identity;
    S=MtkMaliAllocationIdentity(&R->Adapter->Allocations,First.Allocation,&Identity);if(S!=STATUS_SUCCESS)return S;
    if(Identity.Key!=First.AllocationKey || First.AllocationOffset>=Identity.Bytes ||
       Bytes>Identity.Bytes-First.AllocationOffset)return STATUS_INVALID_PARAMETER;
    RESIDENCY_BUFFER *B=&L->Buffers[L->BufferCount++];B->Va=Va;B->Bytes=Bytes;B->Access=Access;
    S=MtkMaliAllocationAcquireObject(&R->Adapter->Allocations,L->Device,L->Process,First.Allocation,
        First.AllocationKey,First.AllocationOffset,Bytes,(BOOLEAN)!!(Access&MaliPteWrite),&B->Reference);
    if(S!=STATUS_SUCCESS)return S;
    MALI_ALLOCATION_VIEW V;S=MtkMaliAllocationReferenceView(&R->Adapter->Allocations,B->Reference,&V);
    if(S!=STATUS_SUCCESS)return S;
    uint64_t Offset=First.AllocationOffset;
    if(!V.Backing || V.Key!=First.AllocationKey || Offset>V.BackingBytes || Bytes>V.BackingBytes-Offset ||
       (uintptr_t)V.Backing>UINTPTR_MAX-Offset-Bytes)return STATUS_NOT_SUPPORTED;
    B->Mdl=IoAllocateMdl((uint8_t*)V.Backing+(size_t)Offset,Bytes,FALSE,FALSE,0);
    if(!B->Mdl)return STATUS_NO_MEMORY;
    __try {
        MmProbeAndLockPages(B->Mdl,KernelMode,Access&MaliPteWrite?IoWriteAccess:IoReadAccess);B->Locked=1;
        B->Cpu=MmGetSystemAddressForMdlSafe(B->Mdl,NormalPagePriority|MdlMappingNoExecute|
            (Access&MaliPteWrite?0:MdlMappingNoWrite));
    } __except(EXCEPTION_EXECUTE_HANDLER) {S=GetExceptionCode();}
    if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_UNSUCCESSFUL:S;
    if(!B->Cpu)return STATUS_NO_MEMORY;
    if(((uintptr_t)B->Cpu&4095u) || MmGetMdlByteOffset(B->Mdl) || MmGetMdlByteCount(B->Mdl)!=Bytes)
        return STATUS_INVALID_ADDRESS;
    S=GrowResourceHash(L,L->ResourcePages+Bytes/4096u);if(S!=STATUS_SUCCESS)return S;
    L->ResourcePages+=Bytes/4096u;
    const PFN_NUMBER *Pfns=MmGetMdlPfnArray(B->Mdl);
    for(uint32_t I=0;I<Bytes/4096u;I++) {
        uint64_t Pa=(uint64_t)Pfns[I]<<12;MALI_VM_PAGE P;
        if(!Pfns[I] || Pfns[I]>=(MALI_JM_PA_LIMIT>>12))return STATUS_INVALID_ADDRESS;
        S=MtkMaliVmLookup(R->Vm,&L->Hold,Va+(uint64_t)I*4096u,&P);if(S!=STATUS_SUCCESS)return S;
        if(P.Allocation!=First.Allocation || P.AllocationKey!=First.AllocationKey ||
           P.AllocationOffset!=Offset+(uint64_t)I*4096u || P.Physical!=Pa || (P.Flags&Access)!=Access)
            return STATUS_ACCESS_DENIED;
        if(MtkMaliVmIsTablePage(R->Vm,Pa) || OwnedAlias(R,Pa,0))return STATUS_ACCESS_DENIED;
        for(const RESIDENCY_LEASE *Other=R->Leases;Other;Other=Other->Next) {
            uint32_t Union=ResourceAccess(Other,Pa)|Access;
            if((Union&(MaliPteWrite|MaliPteExecute))==(MaliPteWrite|MaliPteExecute))return STATUS_ACCESS_DENIED;
            for(UINT J=0;J<Other->DataCount;J++)if(Other->Data[J].Physical==Pa &&
                (Other==L || ((Access&MaliPteExecute) && Other->Data[J].Write)))return STATUS_ACCESS_DENIED;
        }
        uint64_t *H=HashSlot(L->ResourceHash,L->HashCapacity,Pa);
        if(!H)return STATUS_INSUFFICIENT_RESOURCES;
        *H=Pa|Access|(*H&15u);
        S=MapPage(R,L,Va+(uint64_t)I*4096u,Pa,Access|(P.Flags&MaliPteCached));if(S!=STATUS_SUCCESS)return S;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS ResidencyPin(void *X,void *Token,uint64_t Va,uint32_t Bytes) {
    MALI_RESIDENCY *R=X;RESIDENCY_LEASE *L=ResidencyLease(R,Token);
    if(!L || !L->Reserved || L->Sealed || !Bytes || Bytes>8)return STATUS_INVALID_PARAMETER;
    if(Va>=RESIDENCY_SHADOW_VA || (Va<RESIDENCY_SHADOW_VA && Bytes>RESIDENCY_SHADOW_VA-Va))return STATUS_ACCESS_DENIED;
    for(UINT I=0;I<L->BufferCount;I++) {
        const RESIDENCY_BUFFER *B=&L->Buffers[I];
        if(Va>=B->Va && Va-B->Va<B->Bytes && Bytes<=B->Bytes-(Va-B->Va))
            return B->Access&MaliPteWrite?STATUS_SUCCESS:STATUS_ACCESS_DENIED;
    }
    RESIDENCY_PAGE *P=0;NTSTATUS S=PinPage(R,L,Va,Bytes,1,&P);if(S!=STATUS_SUCCESS)return S;
    MALI_VM_PAGE Mapping;S=MtkMaliVmLookup(R->Vm,&L->Hold,Va,&Mapping);if(S!=STATUS_SUCCESS)return S;
    return MapPage(R,L,Va&~UINT64_C(4095),P->Physical,Mapping.Flags&~MaliPteExecute);
}
static NTSTATUS ResidencySeal(void *X,void *Token,const void *Cpu,uint32_t Bytes) {
    MALI_RESIDENCY *R=X;RESIDENCY_LEASE *L=ResidencyLease(R,Token);
    if(!L || !L->Reserved || L->Sealed || Cpu!=L->Shadow.Cpu || !Bytes || Bytes>MALI_COMMAND_SHADOW_BYTES)
        return STATUS_INVALID_PARAMETER;
    for(UINT I=0;I<L->DataCount;I++)if(L->Data[I].Write) {
        KeFlushIoBuffers(L->Data[I].Mdl,FALSE,TRUE);KeFlushIoBuffers(L->Data[I].Mdl,TRUE,TRUE);
    }
    for(UINT I=0;I<L->BufferCount;I++) {
        KeFlushIoBuffers(L->Buffers[I].Mdl,FALSE,TRUE);
        if(L->Buffers[I].Access&MaliPteWrite)KeFlushIoBuffers(L->Buffers[I].Mdl,TRUE,TRUE);
    }
    KeFlushIoBuffers(L->Shadow.Mdl,FALSE,TRUE);
    for(UINT I=0;I<L->TableCount;I++)KeFlushIoBuffers(L->Tables[I].Memory.Mdl,FALSE,TRUE);
    __dsb(15);L->Sealed=1;return STATUS_SUCCESS;
}
static int StillBound(const MALI_RESIDENCY *R,const RESIDENCY_LEASE *L) {
    MALI_JM *D=R->Adapter->Queue.Core;
    for(UINT A=0;A<MALI_JM_MAX_AS;A++)if(D->BoundSpaces&(1u<<A))
        for(UINT I=0;I<L->TableCount;I++)if(D->Roots[A]==L->Tables[I].Physical)return 1;
    return 0;
}
static void ReleaseOwned(MALI_WDK_MEMORY *M) {
    if(M->Cpu){RtlZeroMemory(M->Cpu,M->Bytes);KeFlushIoBuffers(M->Mdl,FALSE,TRUE);__dsb(15);}
    MtkMaliMemoryRelease(M);
}
static void ResidencyEnd(void *X,void *Token,MALI_DDI_RELEASE_REASON Reason) {
    MALI_RESIDENCY *R=X;RESIDENCY_LEASE *L=ResidencyLease(R,Token);
    if(!L || Reason<MaliDdiRejected || Reason>MaliDdiReset) {
        if(R && R->Signature==RESIDENCY_SIGNATURE)R->LastReleaseStatus=STATUS_INVALID_PARAMETER;return;
    }
    /* A broken parent cannot turn early release into freeing a live GPU root. */
    if(StillBound(R,L)){R->LastReleaseStatus=STATUS_DEVICE_BUSY;return;}
    for(UINT I=0;I<L->DataCount;I++) {
        RESIDENCY_PAGE *P=&L->Data[I];
        if(P->Locked) {
            if(P->Write && L->Sealed){KeFlushIoBuffers(P->Mdl,TRUE,TRUE);__dsb(15);}
            MmUnlockPages(P->Mdl);
        }
        if(P->Mdl)IoFreeMdl(P->Mdl);
        if(P->Reference)MtkMaliAllocationRelease(&R->Adapter->Allocations,P->Reference);
    }
    for(UINT I=0;I<L->BufferCount;I++) {
        RESIDENCY_BUFFER *B=&L->Buffers[I];
        if(B->Locked) {
            if((B->Access&MaliPteWrite) && L->Sealed){KeFlushIoBuffers(B->Mdl,TRUE,TRUE);__dsb(15);}
            MmUnlockPages(B->Mdl);
        }
        if(B->Mdl)IoFreeMdl(B->Mdl);
        if(B->Reference)MtkMaliAllocationRelease(&R->Adapter->Allocations,B->Reference);
    }
    if(L->ResourceHash)ExFreePoolWithTag(L->ResourceHash,RESIDENCY_TAG);
    for(UINT I=0;I<L->TableCount;I++)ReleaseOwned(&L->Tables[I].Memory);
    ReleaseOwned(&L->Shadow);MtkMaliVmDrop(R->Vm,&L->Hold);
    RESIDENCY_LEASE **P=&R->Leases;while(*P!=L)P=&(*P)->Next;*P=L->Next;--R->Count;
    RtlZeroMemory(L,sizeof(*L));ExFreePoolWithTag(L,RESIDENCY_TAG);R->LastReleaseStatus=STATUS_SUCCESS;
}
static void ResidencyWake(void *X) {
    MALI_RESIDENCY *R=X;if(R && R->Signature==RESIDENCY_SIGNATURE && KeGetCurrentIrql()<=DISPATCH_LEVEL)
        R->Wake(R->WakeContext);
}
void MtkMaliResidencyGetOps(MALI_RESIDENCY *R,MALI_PREPARE_OPS *Ops) {
    if(!Ops)return;
    MALI_PREPARE_OPS Value={R,ResidencyBegin,ResidencyCopy,ResidencyReserve,ResidencyPin,ResidencySeal,ResidencyEnd,ResidencyWake,ResidencyResource};*Ops=Value;
}
NTSTATUS MtkMaliResidencyInitialize(MALI_RESIDENCY *R,MALI_DDI_ADAPTER *D,MALI_VM *Vm,void *WakeContext,void (*Wake)(void *)) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !R || !D || !Vm || !Wake || D->Vm!=Vm ||
       !MtkMaliVmUsesBook(Vm,&D->Allocations) || D->Queue.Count || D->Queue.ActiveValid || Vm->HoldCount)
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(R,sizeof(*R));R->Adapter=D;R->Vm=Vm;R->WakeContext=WakeContext;R->Wake=Wake;
    R->Signature=RESIDENCY_SIGNATURE;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliResidencyShutdown(MALI_RESIDENCY *R) {
    if(!ResidencyValid(R))return STATUS_INVALID_PARAMETER;
    if(R->Count)return STATUS_DEVICE_BUSY;
    R->Signature=0;return STATUS_SUCCESS;
}
