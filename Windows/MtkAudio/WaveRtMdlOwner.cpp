// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtMdlOwner.h"
#include <string.h>
static bool Live(const T70_WAVERT_MDL_OWNER *O){return O&&O->Port&&O->Mdl&&O->Cpu&&O->Bytes;}
NTSTATUS T70WaveRtMdlAcquire(T70_WAVERT_MDL_OWNER *O,IPortWaveRTStream *P,ULONG N){
 if(!O||!P||KeGetCurrentIrql()!=PASSIVE_LEVEL||!N||N>196608||N%PAGE_SIZE)
  return STATUS_INVALID_PARAMETER;
 if(O->Port||O->Mdl||O->Cpu||O->Bytes||O->Uses)return STATUS_INVALID_DEVICE_STATE;
 P->AddRef();
 PHYSICAL_ADDRESS Low={},High;High.QuadPart=MAXLONGLONG;
 PMDL M=P->AllocateContiguousPagesForMdl(Low,High,N);
 if(!M){P->Release();return STATUS_INSUFFICIENT_RESOURCES;}
 /* Fail closed on a provider response inconsistent with requested ownership. */
 if(M->Next||MmGetMdlByteOffset(M)||MmGetMdlByteCount(M)!=N||P->GetPhysicalPagesCount(M)!=N/PAGE_SIZE){
  P->FreePagesFromMdl(M);P->Release();return STATUS_DEVICE_DATA_ERROR;
 }
 void *Cpu=P->MapAllocatedPages(M,MmCached);
 if(!Cpu){P->FreePagesFromMdl(M);P->Release();return STATUS_INSUFFICIENT_RESOURCES;}
 uintptr_t A=(uintptr_t)Cpu,B=(uintptr_t)O;
 if(A%PAGE_SIZE||N>UINTPTR_MAX-A||sizeof(*O)>UINTPTR_MAX-B||
    (A<B+sizeof(*O)&&B<A+N)){
  P->UnmapAllocatedPages(Cpu,M);P->FreePagesFromMdl(M);P->Release();return STATUS_DEVICE_DATA_ERROR;
 }
 /* Actual page allocation, cached ordinary RAM, not existing Device Memory. */
 memset(Cpu,0,N);
 O->Port=P;O->Mdl=M;O->Cpu=Cpu;O->Bytes=N;O->Uses=0;
 return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtMdlBegin(T70_WAVERT_MDL_OWNER *O,T70_WAVERT_MDL_VIEW *V){
 if(!O||!V||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 uintptr_t A=(uintptr_t)O,B=(uintptr_t)V;
 if(sizeof(*O)>UINTPTR_MAX-A||sizeof(*V)>UINTPTR_MAX-B||
    (A<B+sizeof(*V)&&B<A+sizeof(*O)))return STATUS_INVALID_PARAMETER;
 if(!Live(O)||O->Uses==UINT32_MAX)return STATUS_INVALID_DEVICE_STATE;
 uintptr_t Cpu=(uintptr_t)O->Cpu;
 if(O->Bytes>UINTPTR_MAX-Cpu||(B<Cpu+O->Bytes&&Cpu<B+sizeof(*V)))return STATUS_INVALID_PARAMETER;
 ++O->Uses;*V={O->Mdl,O->Cpu,O->Bytes,MmCached};return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtMdlEnd(T70_WAVERT_MDL_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!Live(O)||!O->Uses)return STATUS_INVALID_DEVICE_STATE;
 --O->Uses;return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtMdlRelease(T70_WAVERT_MDL_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Port&&!O->Mdl&&!O->Cpu&&!O->Bytes&&!O->Uses)return STATUS_SUCCESS;
 if(!Live(O)||O->Uses)return STATUS_INVALID_DEVICE_STATE;
 auto P=O->Port;auto M=O->Mdl;auto Cpu=O->Cpu;
 /* Calls are externally serialized and non-reentrant. Retain owner until the
  * provider has returned from each void teardown method; no rollback claim. */
 P->UnmapAllocatedPages(Cpu,M);P->FreePagesFromMdl(M);
 *O={};P->Release();return STATUS_SUCCESS;
}
