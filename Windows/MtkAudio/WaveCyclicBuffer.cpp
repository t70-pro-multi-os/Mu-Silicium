// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicBuffer.h"
static bool Live(const T70_WAVECYCLIC_BUFFER *O){return O&&O->Port&&O->Channel&&O->Cpu&&O->Bytes&&O->FrameBytes;}
NTSTATUS T70WaveCyclicBufferAcquire(T70_WAVECYCLIC_BUFFER *O,IPortWaveCyclic *P,ULONG N,ULONG F){
 if(!O||!P||KeGetCurrentIrql()!=PASSIVE_LEVEL||!N||N>196608||N%16||(F!=2&&F!=4)||N%F)return STATUS_INVALID_PARAMETER;
 if(O->Port||O->Channel||O->Cpu||O->Bytes||O->FrameBytes||O->Uses)return STATUS_INVALID_DEVICE_STATE;
 P->AddRef();IDmaChannel *C=nullptr;
 NTSTATUS S=P->NewMasterDmaChannel(&C,nullptr,nullptr,196608,TRUE,FALSE,(DMA_WIDTH)-1,(DMA_SPEED)-1);
 if(S!=STATUS_SUCCESS){P->Release();return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 if(!C){P->Release();return STATUS_DEVICE_DATA_ERROR;}
 S=C->AllocateBuffer(N,nullptr);
 if(S!=STATUS_SUCCESS){C->Release();P->Release();return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 void *Cpu=C->SystemAddress();uintptr_t A=(uintptr_t)Cpu,B=(uintptr_t)O;
 if(!Cpu||C->MaximumBufferSize()<N||C->AllocatedBufferSize()<N||N>UINTPTR_MAX-A||sizeof(*O)>UINTPTR_MAX-B||
    (A<B+sizeof(*O)&&B<A+N)){
  C->FreeBuffer();C->Release();P->Release();return STATUS_DEVICE_DATA_ERROR;
 }
 C->SetBufferSize(N);
 if(C->BufferSize()!=N){C->FreeBuffer();C->Release();P->Release();return STATUS_DEVICE_DATA_ERROR;}
 *O={P,C,Cpu,N,F,0};return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicBufferBegin(T70_WAVECYCLIC_BUFFER *O){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Live(O)||O->Uses==UINT32_MAX)return STATUS_INVALID_DEVICE_STATE;
 ++O->Uses;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicBufferEnd(T70_WAVECYCLIC_BUFFER *O){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Live(O)||!O->Uses)return STATUS_INVALID_DEVICE_STATE;
 --O->Uses;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicBufferCopy(T70_WAVECYCLIC_BUFFER *O,ULONG Offset,const void *S,ULONG N){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Live(O))return STATUS_INVALID_DEVICE_STATE;
 uintptr_t A=(uintptr_t)S,B=(uintptr_t)O->Cpu,C=(uintptr_t)O;
 if(Offset>O->Bytes||N>O->Bytes-Offset||Offset%O->FrameBytes||N%O->FrameBytes||
    (N&&(!S||N>UINTPTR_MAX-A||O->Bytes>UINTPTR_MAX-B||
     (A<B+O->Bytes&&B<A+N)||(A<C+sizeof(*O)&&C<A+N))))return STATUS_INVALID_PARAMETER;
 NTSTATUS R=T70WaveCyclicBufferBegin(O);if(R!=STATUS_SUCCESS)return R;
 if(N)O->Channel->CopyTo((unsigned char *)O->Cpu+Offset,const_cast<void *>(S),N);
 return T70WaveCyclicBufferEnd(O);
}
NTSTATUS T70WaveCyclicBufferRelease(T70_WAVECYCLIC_BUFFER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Port&&!O->Channel&&!O->Cpu&&!O->Bytes&&!O->FrameBytes&&!O->Uses)return STATUS_SUCCESS;
 if(!Live(O)||O->Uses)return STATUS_INVALID_DEVICE_STATE;
 auto P=O->Port;auto C=O->Channel;
 C->FreeBuffer();C->Release();*O={};P->Release();return STATUS_SUCCESS;
}
