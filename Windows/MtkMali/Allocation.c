/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Allocation.h"
#define ALLOCATION_SIGNATURE 0x414c3730u
#define ALLOCATION_TAG 0x41303754u
C_ASSERT(sizeof(MALI_ALLOCATION_DESC)==40);
typedef struct MALI_ALLOCATION_RESOURCE {
    struct MALI_ALLOCATION_RESOURCE *Next;
    uint32_t Allocations;
} ALLOCATION_RESOURCE;
typedef struct MALI_ALLOCATION {
    struct MALI_ALLOCATION *Next;
    ALLOCATION_RESOURCE *Resource;
    MALI_ALLOCATION_DESC Desc;
    SIZE_T RoundedBytes;
    uint64_t Key;
    uint32_t Opens, References;
    void *Backing;
} ALLOCATION;
typedef struct MALI_ALLOCATION_OPEN {
    struct MALI_ALLOCATION_OPEN *Next;
    ALLOCATION *Allocation;
    HANDLE Device, Process;
    uint32_t ReadOnly, References;
} ALLOCATION_OPEN;
typedef struct MALI_ALLOCATION_REFERENCE {
    struct MALI_ALLOCATION_REFERENCE *Next;
    ALLOCATION_OPEN *Open;
    uint64_t Id, Offset, Bytes;
    uint32_t Write;
} ALLOCATION_REFERENCE;
static int AllocationBookValid(const MALI_ALLOCATION_BOOK *B) {
    return KeGetCurrentIrql()==PASSIVE_LEVEL && B && B->Signature==ALLOCATION_SIGNATURE;
}
static ALLOCATION *AllocationFind(const MALI_ALLOCATION_BOOK *B,HANDLE H) {
    for(ALLOCATION *A=B->Allocations;A;A=A->Next)if(A==H)return A;
    return 0;
}
static ALLOCATION_RESOURCE *AllocationResourceFind(const MALI_ALLOCATION_BOOK *B,HANDLE H) {
    for(ALLOCATION_RESOURCE *R=B->Resources;R;R=R->Next)if(R==H)return R;
    return 0;
}
static ALLOCATION_OPEN *AllocationOpenFind(const MALI_ALLOCATION_BOOK *B,HANDLE H) {
    for(ALLOCATION_OPEN *O=B->Opens;O;O=O->Next)if(O==H)return O;
    return 0;
}
static ALLOCATION_REFERENCE *AllocationReferenceFind(const MALI_ALLOCATION_BOOK *B,uint64_t Id) {
    for(ALLOCATION_REFERENCE *R=B->References;R;R=R->Next)if(R->Id==Id)return R;
    return 0;
}
static void *AllocationNew(size_t Bytes) {
    /* ExAllocatePool2 is zero-initializing, nonpaged and NX by default. */
    return ExAllocatePool2(POOL_FLAG_NON_PAGED,Bytes,ALLOCATION_TAG);
}
static void AllocationFree(void *P) {ExFreePoolWithTag(P,ALLOCATION_TAG);}
#define ALLOCATION_K_HAS(K,Member) ((K)->Size>=FIELD_OFFSET(DXGKRNL_INTERFACE,Member)+sizeof((K)->Member))
void MtkMaliAllocationInitialize(MALI_ALLOCATION_BOOK *B,const DXGKRNL_INTERFACE *K) {
    RtlZeroMemory(B,sizeof(*B));B->Signature=ALLOCATION_SIGNATURE;
    B->BackingFeatureStatus=STATUS_NOT_SUPPORTED;
    if(!K || !ALLOCATION_K_HAS(K,DxgkCbGetHandleData))return;
    B->GetHandleData=K->DxgkCbGetHandleData;
    if(!B->GetHandleData || K->Version<DXGKDDI_INTERFACE_VERSION_WDDM3_1)return;
    if(K->Version>=DXGKDDI_INTERFACE_VERSION_WDDM3_2) {
        if(!ALLOCATION_K_HAS(K,DxgkCbQueryServices) || !K->DxgkCbQueryServices)return;
        DXGK_FEATURE_INTERFACE F={0};F.Size=sizeof(F);F.Version=DXGK_FEATURE_INTERFACE_VERSION_1;
        NTSTATUS S=K->DxgkCbQueryServices(K->DeviceHandle,DxgkServicesFeature,(PINTERFACE)&F);
        B->BackingFeatureStatus=S;
        if(S!=STATUS_SUCCESS)return;
        if(F.Size>=sizeof(F) && F.Version==DXGK_FEATURE_INTERFACE_VERSION_1 &&
           F.IsFeatureEnabled && F.InterfaceDereference) {
            DXGKARGCB_ISFEATUREENABLED2 A={0};A.FeatureId=DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD;
            S=F.IsFeatureEnabled(K->DeviceHandle,&A);B->BackingFeatureStatus=S;
            B->ShareBackingEnabled=S==STATUS_SUCCESS && A.Result.Version==1 && A.Result.Enabled &&
                A.Result.KnownFeature && A.Result.SupportedByDriver && A.Result.SupportedOnCurrentConfig;
        } else B->BackingFeatureStatus=STATUS_NOT_SUPPORTED;
        /* QueryServices returns a referenced interface. Retain no callback. */
        if(F.Size>=sizeof(INTERFACE) && F.InterfaceDereference)F.InterfaceDereference(F.Context);
    } else if(ALLOCATION_K_HAS(K,DxgkCbQueryFeatureSupport) && K->DxgkCbQueryFeatureSupport) {
        DXGKARGCB_QUERYFEATURESUPPORT A={0};A.DeviceHandle=K->DeviceHandle;
        A.FeatureId=DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD;A.DriverSupportState=DXGK_FEATURE_SUPPORT_ALWAYS_ON;
        NTSTATUS S=K->DxgkCbQueryFeatureSupport(&A);B->BackingFeatureStatus=S;
        B->ShareBackingEnabled=S==STATUS_SUCCESS && A.Enabled;
    } else if(ALLOCATION_K_HAS(K,DxgkCbIsFeatureEnabled) && K->DxgkCbIsFeatureEnabled) {
        DXGKARGCB_ISFEATUREENABLED A={0};A.DeviceHandle=K->DeviceHandle;
        A.FeatureId=DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD;
        NTSTATUS S=K->DxgkCbIsFeatureEnabled(&A);B->BackingFeatureStatus=S;
        B->ShareBackingEnabled=S==STATUS_SUCCESS && A.Enabled;
    }
}
static NTSTATUS AllocationDescCheck(const MALI_ALLOCATION_BOOK *B,const MALI_ALLOCATION_DESC *D) {
    if(D->Version!=MALI_ALLOCATION_VERSION || D->Bytes!=sizeof(*D) || !D->Size ||
       D->Size>MALI_ALLOCATION_MAX_BYTES)return STATUS_INVALID_PARAMETER;
    if(D->Flags&~(MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING))return STATUS_NOT_SUPPORTED;
    if(D->Flags&MALI_ALLOCATION_SHARE_BACKING) {
        if(!(D->Flags&MALI_ALLOCATION_CPU_VISIBLE))return STATUS_INVALID_PARAMETER;
        if(!B->ShareBackingEnabled)return STATUS_NOT_SUPPORTED;
    }
    if(D->Kind==MALI_ALLOCATION_BUFFER)
        return D->Width || D->Height || D->Pitch || D->Format ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;
    if(D->Kind!=MALI_ALLOCATION_LINEAR_2D)return STATUS_NOT_SUPPORTED;
    if(!D->Width || D->Width>16384 || !D->Height || D->Height>16384 ||
       D->Pitch<D->Width*4u || (D->Pitch&63u) ||
       (D->Size!=(uint64_t)D->Pitch*D->Height &&
        D->Size!=(((uint64_t)D->Pitch*D->Height+4095u)&~UINT64_C(4095))))
        return STATUS_INVALID_PARAMETER;
    switch(D->Format) {
        case D3DDDIFMT_A8R8G8B8:case D3DDDIFMT_X8R8G8B8:
        case D3DDDIFMT_A8B8G8R8:case D3DDDIFMT_X8B8G8R8:return STATUS_SUCCESS;
        default:return STATUS_NOT_SUPPORTED;
    }
}
NTSTATUS MtkMaliAllocationCreate(MALI_ALLOCATION_BOOK *B,DXGKARG_CREATEALLOCATION *A) {
    if(!AllocationBookValid(B) || !A || !A->NumAllocations || A->NumAllocations>MALI_ALLOCATION_BATCH ||
       !A->pAllocationInfo || (A->Flags.Value&~1u) || (!A->Flags.Resource && A->hResource))return STATUS_INVALID_PARAMETER;
    if(!B->GetHandleData || A->PrivateDriverDataSize)return STATUS_NOT_SUPPORTED;
    if(A->NumAllocations>MALI_ALLOCATION_LIMIT-B->AllocationCount ||
       A->NumAllocations>UINT64_MAX-B->NextAllocation)return STATUS_INSUFFICIENT_RESOURCES;
    ALLOCATION_RESOURCE *Resource=0;int NewResource=0;
    if(A->Flags.Resource) {
        if(A->hResource) {
            Resource=AllocationResourceFind(B,A->hResource);if(!Resource)return STATUS_INVALID_HANDLE;
        } else {
            if(B->ResourceCount==MALI_ALLOCATION_LIMIT)return STATUS_INSUFFICIENT_RESOURCES;
            Resource=AllocationNew(sizeof(*Resource));if(!Resource)return STATUS_NO_MEMORY;
            NewResource=1;
        }
    }
    ALLOCATION *Pending=0,**Tail=&Pending;NTSTATUS S=STATUS_SUCCESS;
    for(UINT I=0;I<A->NumAllocations;I++) {
        DXGK_ALLOCATIONINFO *Info=&A->pAllocationInfo[I];MALI_ALLOCATION_DESC D;
        if(!Info->pPrivateDriverData || Info->PrivateDriverDataSize!=sizeof(D)) {S=STATUS_INVALID_PARAMETER;break;}
        if(Info->FlagsWddm2.CreateInVpr) {S=STATUS_NOT_SUPPORTED;break;}
        RtlCopyMemory(&D,Info->pPrivateDriverData,sizeof(D));S=AllocationDescCheck(B,&D);
        if(S!=STATUS_SUCCESS)break;
        if((D.Flags&MALI_ALLOCATION_SHARE_BACKING) && !Resource){S=STATUS_INVALID_PARAMETER;break;}
        ALLOCATION *N=AllocationNew(sizeof(*N));if(!N){S=STATUS_NO_MEMORY;break;}
        N->Key=B->NextAllocation+I+1u;N->Desc=D;N->Resource=Resource;N->RoundedBytes=(SIZE_T)((D.Size+4095u)&~UINT64_C(4095));
        *Tail=N;Tail=&N->Next;
    }
    if(S!=STATUS_SUCCESS) {
        while(Pending){ALLOCATION *N=Pending->Next;AllocationFree(Pending);Pending=N;}
        if(NewResource)AllocationFree(Resource);
        return S;
    }
    ALLOCATION *N=Pending;
    for(UINT I=0;I<A->NumAllocations;I++,N=N->Next) {
        DXGK_ALLOCATIONINFO *Info=&A->pAllocationInfo[I];
        void *Private=Info->pPrivateDriverData;UINT Bytes=Info->PrivateDriverDataSize;
        RtlZeroMemory(Info,sizeof(*Info));Info->pPrivateDriverData=Private;Info->PrivateDriverDataSize=Bytes;
        Info->Alignment=4096;Info->Size=N->RoundedBytes;Info->hAllocation=N;
        /* Single GPU, system-memory aperture segment 1. GpuMmu upgrades virtual
           allocations to implicit system segment 0 (AccessedPhysically=FALSE).
           Parent QuerySegment must enumerate that aperture; no local VRAM. */
        Info->MmuSet=1;Info->PreferredSegment.SegmentId0=1;Info->SupportedWriteSegmentSet=1;
        Info->FlagsWddm2.CpuVisible=!!(N->Desc.Flags&MALI_ALLOCATION_CPU_VISIBLE);
        Info->FlagsWddm2.Cached=Info->FlagsWddm2.CpuVisible;
        Info->Flags2.ShareBackingStoreWithKmd=!!(N->Desc.Flags&MALI_ALLOCATION_SHARE_BACKING);
        Info->AllocationPriority=D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    }
    *Tail=B->Allocations;B->Allocations=Pending;B->AllocationCount+=A->NumAllocations;B->NextAllocation+=A->NumAllocations;
    if(Resource) {
        Resource->Allocations+=A->NumAllocations;
        if(NewResource){Resource->Next=B->Resources;B->Resources=Resource;++B->ResourceCount;}
        A->hResource=Resource;
    }
    return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationDestroy(MALI_ALLOCATION_BOOK *B,const DXGKARG_DESTROYALLOCATION *A) {
    if(!AllocationBookValid(B) || !A || A->NumAllocations>MALI_ALLOCATION_BATCH ||
       (A->NumAllocations && !A->pAllocationList) || (A->Flags.Value&~1u) ||
       (!A->NumAllocations && !A->Flags.DestroyResource))return STATUS_INVALID_PARAMETER;
    ALLOCATION_RESOURCE *Resource=AllocationResourceFind(B,A->hResource);
    if((A->hResource || A->Flags.DestroyResource) && !Resource)return STATUS_INVALID_HANDLE;
    if(A->Flags.DestroyResource && Resource->Allocations!=A->NumAllocations)return STATUS_INVALID_PARAMETER;
    for(UINT I=0;I<A->NumAllocations;I++) {
        ALLOCATION *N=AllocationFind(B,A->pAllocationList[I]);if(!N)return STATUS_INVALID_HANDLE;
        if(Resource && N->Resource!=Resource)return STATUS_INVALID_PARAMETER;
        if(N->Opens || N->References)return STATUS_DEVICE_BUSY;
        for(UINT J=0;J<I;J++)if(A->pAllocationList[I]==A->pAllocationList[J])return STATUS_INVALID_PARAMETER;
    }
    for(UINT I=0;I<A->NumAllocations;I++) {
        ALLOCATION **P=&B->Allocations;while(*P!=(ALLOCATION*)A->pAllocationList[I])P=&(*P)->Next;
        ALLOCATION *N=*P;*P=N->Next;--B->AllocationCount;
        if(N->Resource)--N->Resource->Allocations;
        /* Backing belongs to VidMm. Never unmap/free its CPU VA or PFNs. */
        AllocationFree(N);
    }
    if(A->Flags.DestroyResource) {
        ALLOCATION_RESOURCE **P=&B->Resources;while(*P!=Resource)P=&(*P)->Next;
        *P=Resource->Next;--B->ResourceCount;AllocationFree(Resource);
    }
    return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationOpen(MALI_ALLOCATION_BOOK *B,HANDLE Device,HANDLE Process,const DXGKARG_OPENALLOCATION *A) {
    if(!AllocationBookValid(B) || !Device || !Process || !A || !A->NumAllocations ||
       A->NumAllocations>MALI_ALLOCATION_BATCH || !A->pOpenAllocation || (A->Flags.Value&~3u))return STATUS_INVALID_PARAMETER;
    if(!B->GetHandleData || A->SubresourceIndex || A->PrivateDriverSize)return STATUS_NOT_SUPPORTED;
    if(A->NumAllocations>MALI_ALLOCATION_LIMIT-B->OpenCount)return STATUS_INSUFFICIENT_RESOURCES;
    ALLOCATION_OPEN *Pending=0,**Tail=&Pending;NTSTATUS S=STATUS_SUCCESS;
    for(UINT I=0;I<A->NumAllocations;I++) {
        DXGKARGCB_GETHANDLEDATA H={0};H.hObject=A->pOpenAllocation[I].hAllocation;H.Type=DXGK_HANDLE_ALLOCATION;
        ALLOCATION *N=AllocationFind(B,B->GetHandleData(&H));
        if(!N){S=STATUS_INVALID_HANDLE;break;}
        ALLOCATION_OPEN *O=AllocationNew(sizeof(*O));if(!O){S=STATUS_NO_MEMORY;break;}
        O->Allocation=N;O->Device=Device;O->Process=Process;O->ReadOnly=A->Flags.ReadOnly;
        *Tail=O;Tail=&O->Next;
    }
    if(S!=STATUS_SUCCESS) {
        while(Pending){ALLOCATION_OPEN *N=Pending->Next;AllocationFree(Pending);Pending=N;}
        return S;
    }
    ALLOCATION_OPEN *O=Pending;
    for(UINT I=0;I<A->NumAllocations;I++,O=O->Next) {
        A->pOpenAllocation[I].hDeviceSpecificAllocation=O;++O->Allocation->Opens;
    }
    *Tail=B->Opens;B->Opens=Pending;B->OpenCount+=A->NumAllocations;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationClose(MALI_ALLOCATION_BOOK *B,HANDLE Device,const DXGKARG_CLOSEALLOCATION *A) {
    if(!AllocationBookValid(B) || !Device || !A || !A->NumAllocations ||
       A->NumAllocations>MALI_ALLOCATION_BATCH || !A->pOpenHandleList)return STATUS_INVALID_PARAMETER;
    for(UINT I=0;I<A->NumAllocations;I++) {
        ALLOCATION_OPEN *O=AllocationOpenFind(B,A->pOpenHandleList[I]);if(!O)return STATUS_INVALID_HANDLE;
        if(O->Device!=Device)return STATUS_ACCESS_DENIED;
        if(O->References)return STATUS_DEVICE_BUSY;
        for(UINT J=0;J<I;J++)if(A->pOpenHandleList[I]==A->pOpenHandleList[J])return STATUS_INVALID_PARAMETER;
    }
    for(UINT I=0;I<A->NumAllocations;I++) {
        ALLOCATION_OPEN **P=&B->Opens;while(*P!=(ALLOCATION_OPEN*)A->pOpenHandleList[I])P=&(*P)->Next;
        ALLOCATION_OPEN *O=*P;*P=O->Next;--O->Allocation->Opens;--B->OpenCount;AllocationFree(O);
    }
    return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationDescribe(MALI_ALLOCATION_BOOK *B,DXGKARG_DESCRIBEALLOCATION *A) {
    if(!AllocationBookValid(B) || !A)return STATUS_INVALID_PARAMETER;
    ALLOCATION *N=AllocationFind(B,A->hAllocation);if(!N)return STATUS_INVALID_HANDLE;
    if(A->Flags.Value || N->Desc.Kind!=MALI_ALLOCATION_LINEAR_2D)return STATUS_NOT_SUPPORTED;
    A->Width=N->Desc.Width;A->Height=N->Desc.Height;A->Format=(D3DDDIFORMAT)N->Desc.Format;
    A->MultisampleMethod.NumSamples=1;A->MultisampleMethod.NumQualityLevels=0;
    A->RefreshRate.Numerator=A->RefreshRate.Denominator=0;A->PrivateDriverFormatAttribute=0;
    A->Rotation=D3DDDI_ROTATION_IDENTITY;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationSetBacking(MALI_ALLOCATION_BOOK *B,const DXGKARG_SETALLOCATIONBACKINGSTORE *A) {
    if(!AllocationBookValid(B) || !A)return STATUS_INVALID_PARAMETER;
    ALLOCATION *N=AllocationFind(B,A->hDriverAllocation);if(!N)return STATUS_INVALID_HANDLE;
    if(!B->ShareBackingEnabled || !(N->Desc.Flags&MALI_ALLOCATION_SHARE_BACKING))return STATUS_NOT_SUPPORTED;
    if(!A->pBackingStore || ((uintptr_t)A->pBackingStore&4095u) ||
       N->RoundedBytes>UINTPTR_MAX-(uintptr_t)A->pBackingStore)return STATUS_INVALID_PARAMETER;
    if(N->Backing && N->Backing!=A->pBackingStore)return STATUS_DEVICE_BUSY;
    N->Backing=A->pBackingStore;return STATUS_SUCCESS;
}
int MtkMaliAllocationHasDevice(const MALI_ALLOCATION_BOOK *B,HANDLE Device) {
    for(ALLOCATION_OPEN *O=B->Opens;O;O=O->Next)if(O->Device==Device)return 1;
    return 0;
}
int MtkMaliAllocationHasObjects(const MALI_ALLOCATION_BOOK *B) {
    return B->AllocationCount || B->ResourceCount || B->OpenCount || B->ReferenceCount;
}
NTSTATUS MtkMaliAllocationIdentity(MALI_ALLOCATION_BOOK *B,HANDLE Handle,MALI_ALLOCATION_IDENTITY *Out) {
    if(!AllocationBookValid(B) || !Out)return STATUS_INVALID_PARAMETER;
    ALLOCATION *A=AllocationFind(B,Handle);if(!A)return STATUS_INVALID_HANDLE;
    MALI_ALLOCATION_IDENTITY I={A->Key,A->Desc.Size,A->RoundedBytes,A->Backing};*Out=I;return STATUS_SUCCESS;
}
static NTSTATUS AllocationReferenceNew(MALI_ALLOCATION_BOOK *B,ALLOCATION_OPEN *O,
    uint64_t Offset,uint64_t Bytes,BOOLEAN Write,uint64_t *Token) {
    if(Write && O->ReadOnly)return STATUS_ACCESS_DENIED;
    if(Offset>=O->Allocation->Desc.Size || Bytes>O->Allocation->Desc.Size-Offset)return STATUS_INVALID_PARAMETER;
    if(B->ReferenceCount==MALI_ALLOCATION_LIMIT || B->NextReference==UINT64_MAX)return STATUS_INSUFFICIENT_RESOURCES;
    ALLOCATION_REFERENCE *R=AllocationNew(sizeof(*R));if(!R)return STATUS_NO_MEMORY;
    R->Open=O;R->Id=++B->NextReference;R->Offset=Offset;R->Bytes=Bytes;R->Write=Write;
    R->Next=B->References;B->References=R;++B->ReferenceCount;++O->References;++O->Allocation->References;
    *Token=R->Id;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationAcquire(MALI_ALLOCATION_BOOK *B,HANDLE Device,HANDLE Process,
    D3DKMT_HANDLE Handle,uint64_t Offset,uint64_t Bytes,BOOLEAN Write,uint64_t *Token) {
    if(!AllocationBookValid(B) || !Device || !Process || !Handle || !Token || Write>TRUE || !Bytes)return STATUS_INVALID_PARAMETER;
    if(!B->GetHandleData)return STATUS_NOT_SUPPORTED;
    DXGKARGCB_GETHANDLEDATA H={0};H.hObject=Handle;H.Type=DXGK_HANDLE_ALLOCATION;H.Flags.DeviceSpecific=1;
    ALLOCATION_OPEN *O=AllocationOpenFind(B,B->GetHandleData(&H));if(!O)return STATUS_INVALID_HANDLE;
    if(O->Device!=Device || O->Process!=Process)return STATUS_ACCESS_DENIED;
    return AllocationReferenceNew(B,O,Offset,Bytes,Write,Token);
}
NTSTATUS MtkMaliAllocationAcquireObject(MALI_ALLOCATION_BOOK *B,HANDLE Device,HANDLE Process,
    HANDLE Handle,uint64_t Key,uint64_t Offset,uint64_t Bytes,BOOLEAN Write,uint64_t *Token) {
    if(!AllocationBookValid(B) || !Device || !Process || !Key || !Token || Write>TRUE || !Bytes)return STATUS_INVALID_PARAMETER;
    ALLOCATION *A=AllocationFind(B,Handle);if(!A || A->Key!=Key)return STATUS_INVALID_HANDLE;
    for(ALLOCATION_OPEN *O=B->Opens;O;O=O->Next)
        if(O->Allocation==A && O->Device==Device && O->Process==Process && (!Write || !O->ReadOnly))
            return AllocationReferenceNew(B,O,Offset,Bytes,Write,Token);
    return STATUS_ACCESS_DENIED;
}
NTSTATUS MtkMaliAllocationReferenceView(MALI_ALLOCATION_BOOK *B,uint64_t Token,MALI_ALLOCATION_VIEW *V) {
    if(!AllocationBookValid(B) || !V || !Token)return STATUS_INVALID_PARAMETER;
    ALLOCATION_REFERENCE *R=AllocationReferenceFind(B,Token);if(!R)return STATUS_INVALID_HANDLE;
    MALI_ALLOCATION_VIEW Out={0};Out.Allocation=R->Open->Allocation;Out.Device=R->Open->Device;Out.Process=R->Open->Process;
    Out.AllocationBytes=R->Open->Allocation->Desc.Size;Out.Offset=R->Offset;Out.Bytes=R->Bytes;
    Out.CpuVisible=!!(R->Open->Allocation->Desc.Flags&MALI_ALLOCATION_CPU_VISIBLE);Out.ReadOnly=R->Open->ReadOnly;
    Out.Write=R->Write;Out.Key=R->Open->Allocation->Key;Out.Backing=R->Open->Allocation->Backing;
    Out.BackingBytes=R->Open->Allocation->RoundedBytes;*V=Out;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationCopyBacking(MALI_ALLOCATION_BOOK *B,uint64_t Token,uint64_t Offset,void *Dest,size_t Bytes) {
    if(!AllocationBookValid(B) || !Token || !Dest || !Bytes)return STATUS_INVALID_PARAMETER;
    ALLOCATION_REFERENCE *R=AllocationReferenceFind(B,Token);if(!R)return STATUS_INVALID_HANDLE;
    ALLOCATION *A=R->Open->Allocation;if(!A->Backing)return STATUS_DEVICE_NOT_READY;
    if(Offset>=R->Bytes || Bytes>R->Bytes-Offset || Bytes>UINTPTR_MAX-(uintptr_t)Dest)return STATUS_INVALID_PARAMETER;
    uintptr_t D=(uintptr_t)Dest,S=(uintptr_t)A->Backing;
    /* No memcpy overlap and no write back into any byte of this shared BO. */
    if(D<S+A->RoundedBytes && S<D+Bytes)return STATUS_ACCESS_DENIED;
    RtlCopyMemory(Dest,(uint8_t*)A->Backing+R->Offset+Offset,Bytes);return STATUS_SUCCESS;
}
NTSTATUS MtkMaliAllocationRelease(MALI_ALLOCATION_BOOK *B,uint64_t Token) {
    if(!AllocationBookValid(B) || !Token)return STATUS_INVALID_PARAMETER;
    ALLOCATION_REFERENCE **P=&B->References;while(*P && (*P)->Id!=Token)P=&(*P)->Next;
    if(!*P)return STATUS_INVALID_HANDLE;
    ALLOCATION_REFERENCE *R=*P;*P=R->Next;--B->ReferenceCount;--R->Open->References;--R->Open->Allocation->References;
    AllocationFree(R);return STATUS_SUCCESS;
}
