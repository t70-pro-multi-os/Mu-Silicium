// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtBufferedStreamBase.h"
T70WaveRtBufferedStreamBase::T70WaveRtBufferedStreamBase(const T70_AFE_PREPARED &P,
 T70_WAVERT_BUFFER_TRANSACTION *B,IPortWaveRTStream *S,PDMA_ADAPTER D,T70_AFE_MAPPING_OWNER *M)
 :T70WaveRtStreamBase(P),T(B),Port(S),Adapter(D),Afe(M){}
NTSTATUS T70WaveRtBufferedStreamBase::SetFormat(PKSDATAFORMAT F){
 if(!T||T->Terminal)return STATUS_INVALID_DEVICE_STATE;
 return T70WaveRtStreamBase::SetFormat(F);
}
NTSTATUS T70WaveRtBufferedStreamBase::FailBeforeConfigure(NTSTATUS S){
 NTSTATUS Cleanup=Recover();T->LastStatus=NT_SUCCESS(Cleanup)?S:Cleanup;return T->LastStatus;
}
NTSTATUS T70WaveRtBufferedStreamBase::AllocateAudioBuffer(ULONG N,PMDL *M,ULONG *Size,
 ULONG *Offset,MEMORY_CACHING_TYPE *Cache){
 if(!T||!Port||!Adapter||!Afe||!M||!Size||!Offset||!Cache||KeGetCurrentIrql()!=PASSIVE_LEVEL||
    !N||N>196608)return STATUS_INVALID_PARAMETER;
 uintptr_t A[]={(uintptr_t)M,(uintptr_t)Size,(uintptr_t)Offset,(uintptr_t)Cache,(uintptr_t)T,(uintptr_t)this};
 size_t L[]={sizeof(*M),sizeof(*Size),sizeof(*Offset),sizeof(*Cache),sizeof(*T),sizeof(*this)};
 for(unsigned I=0;I<6;I++){
  if(L[I]>UINTPTR_MAX-A[I])return STATUS_INVALID_PARAMETER;
  for(unsigned J=0;J<I;J++)if(A[I]<A[J]+L[J]&&A[J]<A[I]+L[I])return STATUS_INVALID_PARAMETER;
 }
 T70_AFE_PREPARED Plan,Check;
 if(!CopyPlan(&Plan))return STATUS_INVALID_PARAMETER;
 /* PortCls requests bytes, not pages. Round UP to the smallest page-backed
  * geometry compatible with the existing period; never publish less than N.
  * Bounded page search avoids LCM multiplication/rounding overflow. */
 ULONG ActualBytes=0;
 for(ULONG Candidate=PAGE_SIZE;Candidate<=196608;Candidate+=PAGE_SIZE){
  if(Candidate>=N&&T70AfeDl1Prepare(Plan.Rate,Plan.SampleFormat,Plan.Channels,
      0,Candidate,Plan.PeriodBytes,&Check)){ActualBytes=Candidate;break;}
 }
 if(!ActualBytes)return STATUS_INVALID_PARAMETER;
 if(T->Terminal||T->LifetimeHeld||T->Pages.Port||T->Dma.LeaseHeld||T->Published)return STATUS_INVALID_DEVICE_STATE;
 NTSTATUS S=PinRecoveryLifetime();if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 T->LifetimeHeld=true;T->Terminal=true;
 S=T70WaveRtMdlAcquire(&T->Pages,Port,ActualBytes);if(!NT_SUCCESS(S))return FailBeforeConfigure(S);
 S=T70WaveRtDmaAcquire(&T->Dma,&T->Pages,Adapter);if(!NT_SUCCESS(S))return FailBeforeConfigure(S);
 S=VerifyClientMapping(T->Dma);if(S!=STATUS_SUCCESS)return FailBeforeConfigure(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);
 T->Configure=ConfigureMapped(&T->Dma,Afe);
 if(T->Configure.Mapped.Code!=T70MappedConfigCallbacksCompleted){
  T->LastStatus=STATUS_DEVICE_NOT_READY;
  if(!T->Configure.Mapped.ConfigureStarted)return FailBeforeConfigure(T->LastStatus);
  return T->LastStatus; // Partial hardware application: explicit recovery only.
 }
 T->Published=T->Pages.Mdl;T->PublishedBytes=T->Pages.Bytes;
 *M=T->Published;*Size=T->PublishedBytes;*Offset=0;*Cache=MmCached;
 T->LastStatus=STATUS_SUCCESS;return STATUS_SUCCESS;
}
void T70WaveRtBufferedStreamBase::FreeAudioBuffer(PMDL M,ULONG N){
 if(!T)return;
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!T->Published||M!=T->Published||N!=T->PublishedBytes){
  T->LastStatus=STATUS_INVALID_PARAMETER;return;
 }
 T->FreeRequested=true;T->LastStatus=Recover();
}
NTSTATUS T70WaveRtBufferedStreamBase::Recover(){
 if(!T)return STATUS_INVALID_PARAMETER;
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL){T->LastStatus=STATUS_INVALID_PARAMETER;return T->LastStatus;}
 if(!T->LifetimeHeld){T->LastStatus=STATUS_SUCCESS;return STATUS_SUCCESS;}
 if(T->Published&&!T->FreeRequested){T->LastStatus=STATUS_INVALID_DEVICE_STATE;return T->LastStatus;}
 NTSTATUS S;
 if(T->Configure.Mapped.ConfigureStarted&&!T->HardwareReleased){
  S=RemoveHardwareReferences();if(S!=STATUS_SUCCESS){T->LastStatus=NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;return T->LastStatus;}
  T->HardwareReleased=true;
 }
 if(T->Configure.Mapped.ConfigureStarted&&!T->Configure.Mapped.LeaseEnded){
  S=T70AfeMappingEnd(Afe);if(!NT_SUCCESS(S)){T->LastStatus=S;return S;}
  T->Configure.Mapped.LeaseEnded=1;
 }
 if(T->Dma.ActiveUses){
  if(T->Dma.ActiveUses!=1){T->LastStatus=STATUS_INVALID_DEVICE_STATE;return T->LastStatus;}
  S=T70WaveRtDmaEndUse(&T->Dma);if(!NT_SUCCESS(S)){T->LastStatus=S;return S;}
 }
 S=T70WaveRtDmaRelease(&T->Dma);if(!NT_SUCCESS(S)){T->LastStatus=S;return S;}
 S=T70WaveRtMdlRelease(&T->Pages);if(!NT_SUCCESS(S)){T->LastStatus=S;return S;}
 T->Published=nullptr;T->PublishedBytes=0;T->LifetimeHeld=false;T->LastStatus=STATUS_SUCCESS;
 UnpinRecoveryLifetime();return STATUS_SUCCESS;
}
