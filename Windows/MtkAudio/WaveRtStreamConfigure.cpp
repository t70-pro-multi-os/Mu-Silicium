// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtStreamBase.h"
T70_STREAM_CONFIG_RESULT T70WaveRtStreamBase::ConfigureMapped(T70_WAVERT_DMA_OWNER *D,T70_AFE_MAPPING_OWNER *M){
 T70_STREAM_CONFIG_RESULT R={};
 if(!Valid||ConfigurationAttempted||!D||!M||D->ActiveUses)return R;
 /* Ordinary owner objects must not overlap each other or this stream. */
 uintptr_t A[]={(uintptr_t)this,(uintptr_t)D,(uintptr_t)M};
 size_t N[]={sizeof(*this),sizeof(*D),sizeof(*M)};
 for(unsigned I=0;I<3;I++){
  if(N[I]>UINTPTR_MAX-A[I])return R;
  for(unsigned J=0;J<I;J++)if(A[I]<A[J]+N[J]&&A[J]<A[I]+N[I])return R;
 }
 T70_AFE_PREPARED Next;
 if(!T70AfeDl1Prepare(Plan.Rate,Plan.SampleFormat,Plan.Channels,
    (uint64_t)D->Logical.QuadPart,D->View.Bytes,Plan.PeriodBytes,&Next))return R;
 R.BeginAttempted=true;R.BeginStatus=T70WaveRtDmaBeginUse(D);
 if(!NT_SUCCESS(R.BeginStatus))return R;
 R.DmaLeaseHeld=true;
 R.Mapped=T70AfeConfigureMapped(M,&Next);
 if(R.Mapped.ConfigureStarted){
  ConfigurationAttempted=true;Plan=Next;return R;
 }
 R.EndAttempted=true;R.EndStatus=T70WaveRtDmaEndUse(D);
 if(NT_SUCCESS(R.EndStatus))R.DmaLeaseHeld=false;
 else ConfigurationAttempted=true; // Outstanding ownership must not be retried.
 return R;
}
