// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtDmaOwner.h"
#include <stddef.h>
NTSTATUS T70WaveRtDmaRelease(T70_WAVERT_DMA_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 if(!O->LeaseHeld){
  if(O->Mapped||O->Backing||O->Adapter||O->Free)return STATUS_INVALID_DEVICE_STATE;
  return STATUS_SUCCESS;
 }
 if(!O->Backing||!O->Adapter||!O->Free)return STATUS_INVALID_DEVICE_STATE;
 if(O->Mapped){
  /* Cached ordinary WaveRT page mapping; borrowed backing is not freed here. */
  O->Free(O->Adapter,O->View.Bytes,O->Logical,O->View.Cpu,TRUE);
  O->Mapped=false;
 }
 NTSTATUS S=T70WaveRtMdlEnd(O->Backing);
 if(!NT_SUCCESS(S))return S; // Logical mapping removed; MDL lease still retained.
 *O={};return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtDmaAcquire(T70_WAVERT_DMA_OWNER *O,T70_WAVERT_MDL_OWNER *B,PDMA_ADAPTER A){
 if(!O||!B||!A||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Backing||O->Adapter||O->Free||O->LeaseHeld||O->Mapped||O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 if(B->Uses)return STATUS_INVALID_DEVICE_STATE; // Bind before any borrowed publication.
 if(A->Size<sizeof(DMA_ADAPTER)||!A->DmaOperations)return STATUS_NOT_SUPPORTED;
 auto Ops=A->DmaOperations;
 if(Ops->Size<offsetof(DMA_OPERATIONS,CreateCommonBufferFromMdl)+sizeof(Ops->CreateCommonBufferFromMdl)||
    !Ops->CreateCommonBufferFromMdl||!Ops->FreeCommonBuffer)return STATUS_NOT_SUPPORTED;
 T70_WAVERT_MDL_VIEW V={};NTSTATUS S=T70WaveRtMdlBegin(B,&V);
 if(!NT_SUCCESS(S))return S;
 O->Backing=B;O->Adapter=A;O->Free=Ops->FreeCommonBuffer;O->View=V;O->LeaseHeld=true;
 if(!V.Mdl||!V.Cpu||V.CpuCache!=MmCached||!V.Bytes||V.Bytes%PAGE_SIZE||
    ((uintptr_t)V.Cpu)%PAGE_SIZE||V.Mdl->Next||MmGetMdlByteOffset(V.Mdl)||MmGetMdlByteCount(V.Mdl)!=V.Bytes){
  S=STATUS_INVALID_PARAMETER;
 }else{
  DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION Limit={};
  Limit.ConfigType=CommonBufferConfigTypeLogicalAddressLimits;
  Limit.LogicalAddressLimits.MinimumAddress.QuadPart=0;
  Limit.LogicalAddressLimits.MaximumAddress.QuadPart=0x3ffffffffLL;
  PHYSICAL_ADDRESS Logical={};
  S=Ops->CreateCommonBufferFromMdl(A,V.Mdl,&Limit,1,&Logical);
  if(NT_SUCCESS(S)){
   O->Logical=Logical;O->Mapped=true;
   uint64_t L=(uint64_t)Logical.QuadPart;
   if(L<=0x3ffffffffULL&&!(L%16)&&V.Bytes-1<=0x3ffffffffULL-L)return STATUS_SUCCESS;
   S=STATUS_DEVICE_DATA_ERROR;
  }
 }
 NTSTATUS Cleanup=T70WaveRtDmaRelease(O);
 return NT_SUCCESS(Cleanup)?S:Cleanup;
}

NTSTATUS T70WaveRtDmaBeginUse(T70_WAVERT_DMA_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Mapped||!O->LeaseHeld||!O->Backing||!O->Backing->Uses||!O->Adapter||!O->Free||O->ActiveUses==UINT32_MAX)
  return STATUS_INVALID_DEVICE_STATE;
 ++O->ActiveUses;return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtDmaEndUse(T70_WAVERT_DMA_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Mapped||!O->LeaseHeld||!O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 --O->ActiveUses;return STATUS_SUCCESS;
}
