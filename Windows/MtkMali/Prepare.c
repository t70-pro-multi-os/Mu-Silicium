/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Prepare.h"
#define PREPARE_SIGNATURE 0x4d435039u
static int IsService(const MALI_PREPARE_SERVICE *S) {return S && S->Signature==PREPARE_SIGNATURE;}
static void ClearInputs(MALI_PREPARE_SERVICE *S) {
    RtlZeroMemory(S->Input,sizeof(S->Input));RtlZeroMemory(S->Private,sizeof(S->Private));
    RtlZeroMemory(&S->Work,sizeof(S->Work));S->CurrentToken=0;
}
static MALI_RESULT PinTarget(void *X,uint64_t Va,uint32_t Bytes) {
    MALI_PREPARE_SERVICE *S=X;
    S->PinStatus=S->Ops.PinWrite(S->Ops.Context,S->CurrentToken,Va,Bytes);
    return S->PinStatus==STATUS_SUCCESS?MaliOk:MaliInvalid;
}
static MALI_RESULT PinResource(void *X,uint64_t Va,uint32_t Bytes,uint32_t Access) {
    MALI_PREPARE_SERVICE *S=X;
    S->PinStatus=S->Ops.PinResource?S->Ops.PinResource(S->Ops.Context,S->CurrentToken,Va,Bytes,Access):STATUS_NOT_SUPPORTED;
    return S->PinStatus==STATUS_SUCCESS?MaliOk:MaliInvalid;
}
static NTSTATUS Prepare(void *X,HANDLE Process,uint64_t Id,uint64_t Root,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *A,MALI_DDI_PREPARED *Out) {
    MALI_PREPARE_SERVICE *S=X;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !IsService(S) || !A || !Out ||
       !Process || !Id || !Root || Root>=MALI_JM_PA_LIMIT || (Root&4095u) ||
       !A->pDmaBufferPrivateData || A->DmaBufferPrivateDataSize<16 ||
       A->DmaBufferPrivateDataSize>sizeof(S->Private) ||
       A->DmaBufferUmdPrivateDataSize>A->DmaBufferPrivateDataSize ||
       !A->DmaBufferVirtualAddress || A->DmaBufferVirtualAddress>=MALI_JM_VA_LIMIT ||
       (A->DmaBufferVirtualAddress&127u) || A->DmaBufferSize<64 ||
       A->DmaBufferSize>MALI_COMMAND_MAX_DMA ||
       A->DmaBufferSize>MALI_JM_VA_LIMIT-A->DmaBufferVirtualAddress)return STATUS_INVALID_PARAMETER;
    if(!S->Core->Started || S->Core->Poisoned)return STATUS_DEVICE_NOT_READY;
    MALI_PREPARE_LEASE *L=0;
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(!S->Leases[I].Live){L=&S->Leases[I];break;}
    if(!L)return STATUS_INSUFFICIENT_RESOURCES;
    uint32_t PN=A->DmaBufferPrivateDataSize,N=A->DmaBufferSize;
    RtlCopyMemory(S->Private,A->pDmaBufferPrivateData,PN);
    void *Token=0,*Shadow=0;uint64_t ShadowVa=0,ExecutionRoot=0;
    NTSTATUS R=S->Ops.Begin(S->Ops.Context,Process,Id,Root,A->DmaBufferVirtualAddress,N,&Token);
    if(R!=STATUS_SUCCESS)goto Failure;
    if(!Token){R=STATUS_INVALID_PARAMETER;goto Failure;}
    R=S->Ops.CopyDma(S->Ops.Context,Token,S->Input,N);if(R!=STATUS_SUCCESS)goto Failure;
    R=S->Ops.ReserveShadow(S->Ops.Context,Token,MALI_COMMAND_SHADOW_BYTES,&Shadow,&ShadowVa,&ExecutionRoot);
    if(R!=STATUS_SUCCESS)goto Failure;
    if(!ExecutionRoot || ExecutionRoot>=MALI_JM_PA_LIMIT || (ExecutionRoot&4095u)) {
        R=STATUS_INVALID_ADDRESS;goto Failure;
    }
    S->CurrentToken=Token;S->PinStatus=STATUS_SUCCESS;
    MALI_RESULT V=MaliCommandsPrepareResources(&S->Core->Caps,S->Input,A->DmaBufferVirtualAddress,N,
        S->Private,PN,Shadow,ShadowVa,MALI_COMMAND_SHADOW_BYTES,&S->Work,PinTarget,S->Ops.PinResource?PinResource:0,S);
    if(V) {R=S->PinStatus!=STATUS_SUCCESS?S->PinStatus:V==MaliUnsupported?STATUS_NOT_SUPPORTED:STATUS_INVALID_PARAMETER;goto Failure;}
    R=S->Ops.Seal(S->Ops.Context,Token,Shadow,S->Work.ShadowUsed);
    if(R!=STATUS_SUCCESS)goto Failure;
    L->Token=Token;L->Live=1;Out->Packet=S->Work.Packet;
    Out->ExecutionRoot=ExecutionRoot;Out->DmaVa=ShadowVa;Out->DmaBytes=S->Work.ShadowUsed;Out->Cookie=L;
    ClearInputs(S);return STATUS_SUCCESS;
Failure:
    if(Token)S->Ops.End(S->Ops.Context,Token,MaliDdiRejected);
    ClearInputs(S);
    /* Success-like statuses (e.g. TIMEOUT/PENDING) cannot publish a command. */
    return NT_SUCCESS(R)?STATUS_UNSUCCESSFUL:R;
}
static void Release(void *X,void *Cookie,MALI_DDI_RELEASE_REASON Reason) {
    MALI_PREPARE_SERVICE *S=X;if(!IsService(S))return;
    uintptr_t V=(uintptr_t)Cookie,B=(uintptr_t)S->Leases;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || V<B || V-B>=sizeof(S->Leases) ||
       (V-B)%sizeof(MALI_PREPARE_LEASE) || Reason<MaliDdiRejected || Reason>MaliDdiReset) {
        S->LastReleaseStatus=STATUS_INVALID_PARAMETER;return;
    }
    MALI_PREPARE_LEASE *L=Cookie;
    if(!L->Live || !L->Token){S->LastReleaseStatus=STATUS_INVALID_PARAMETER;return;}
    S->Ops.End(S->Ops.Context,L->Token,Reason);RtlZeroMemory(L,sizeof(*L));
    S->LastReleaseStatus=STATUS_SUCCESS;
}
static void Wake(void *X) {
    MALI_PREPARE_SERVICE *S=X;
    if(IsService(S) && KeGetCurrentIrql()<=DISPATCH_LEVEL)S->Ops.Wake(S->Ops.Context);
}
NTSTATUS MtkMaliPrepareInitialize(MALI_PREPARE_SERVICE *S,MALI_JM *Core,
    const MALI_PREPARE_OPS *Ops,MALI_DDI_HOOKS *Hooks) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !S || !Core || !Core->Started || Core->Poisoned ||
       !Ops || !Hooks || !Ops->Begin || !Ops->CopyDma || !Ops->ReserveShadow ||
       !Ops->PinWrite || !Ops->Seal || !Ops->End || !Ops->Wake)return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(S,sizeof(*S));S->Core=Core;S->Ops=*Ops;S->Signature=PREPARE_SIGNATURE;
    Hooks->Context=S;Hooks->Prepare=Prepare;Hooks->Release=Release;Hooks->Wake=Wake;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliPrepareShutdown(MALI_PREPARE_SERVICE *S) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !IsService(S))return STATUS_INVALID_PARAMETER;
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(S->Leases[I].Live)return STATUS_DEVICE_BUSY;
    ClearInputs(S);S->Signature=0;return STATUS_SUCCESS;
}
