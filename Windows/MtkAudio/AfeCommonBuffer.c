// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeCommonBuffer.h"
#include <stddef.h>
static NTSTATUS Check(const T70_AFE_COMMON_BUFFER *O){
 if(!O)return STATUS_INVALID_PARAMETER;
 return KeGetCurrentIrql()==PASSIVE_LEVEL?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
}
static int Live(const T70_AFE_COMMON_BUFFER *O){return O->Adapter&&O->Free&&O->VirtualAddress&&O->Length;}
NTSTATUS T70AfeCommonBufferAcquire(T70_AFE_COMMON_BUFFER *O,PDMA_ADAPTER A,
 uint32_t Rate,uint32_t Format,uint32_t Channels,uint64_t Bytes,uint64_t Period){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(O->Adapter||O->Free||O->VirtualAddress||O->Length||O->ActiveUses||O->LogicalAddress.QuadPart)
  return STATUS_INVALID_DEVICE_STATE;
 T70_AFE_PREPARED P;
 if(!A||A->Size<sizeof(*A)||!A->DmaOperations||Bytes>UINT32_MAX||
    !T70AfeDl1Prepare(Rate,Format,Channels,0,Bytes,Period,&P))return STATUS_INVALID_PARAMETER;
 PDMA_OPERATIONS Ops=A->DmaOperations;
 if(Ops->Size<offsetof(DMA_OPERATIONS,AllocateCommonBufferEx)+sizeof(Ops->AllocateCommonBufferEx)||
    !Ops->AllocateCommonBufferEx||!Ops->FreeCommonBuffer)return STATUS_INVALID_PARAMETER;
 PFREE_COMMON_BUFFER Free=Ops->FreeCommonBuffer;
 PHYSICAL_ADDRESS Maximum,Logical;Maximum.QuadPart=UINT64_C(0x3ffffffff);Logical.QuadPart=0;
 void *V=Ops->AllocateCommonBufferEx(A,&Maximum,(ULONG)Bytes,&Logical,FALSE,0);
 if(!V)return STATUS_INSUFFICIENT_RESOURCES;
 uintptr_t B=(uintptr_t)V,Q=(uintptr_t)O;
 if(Bytes>UINTPTR_MAX-B||sizeof(*O)>UINTPTR_MAX-Q||
    (B<Q+sizeof(*O)&&Q<B+(uintptr_t)Bytes)||
    !T70AfeDl1Prepare(Rate,Format,Channels,(uint64_t)Logical.QuadPart,Bytes,Period,&P)){
  Free(A,(ULONG)Bytes,Logical,V,FALSE);return STATUS_INVALID_DEVICE_STATE;
 }
 T70_AFE_COMMON_BUFFER Next={0};Next.Adapter=A;Next.Free=Free;Next.VirtualAddress=V;
 Next.LogicalAddress=Logical;Next.Length=(ULONG)Bytes;Next.Prepared=P;*O=Next;
 return STATUS_SUCCESS;
}
NTSTATUS T70AfeCommonBufferBegin(T70_AFE_COMMON_BUFFER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(!Live(O)||O->ActiveUses==UINT32_MAX)return STATUS_INVALID_DEVICE_STATE;
 ++O->ActiveUses;return STATUS_SUCCESS;
}
NTSTATUS T70AfeCommonBufferEnd(T70_AFE_COMMON_BUFFER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(!Live(O)||!O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 --O->ActiveUses;return STATUS_SUCCESS;
}
NTSTATUS T70AfeCommonBufferRelease(T70_AFE_COMMON_BUFFER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 if(!O->Adapter&&!O->Free&&!O->VirtualAddress&&!O->Length&&!O->LogicalAddress.QuadPart)return STATUS_SUCCESS;
 if(!Live(O))return STATUS_INVALID_DEVICE_STATE;
 O->Free(O->Adapter,O->Length,O->LogicalAddress,O->VirtualAddress,FALSE);
 T70_AFE_COMMON_BUFFER Empty={0};*O=Empty;return STATUS_SUCCESS;
}
