// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CommonBufferFill.h"
T70_BUFFER_FILL_RESULT T70AfeCommonBufferFill(T70_AFE_COMMON_BUFFER *B,
 const void *D,size_t DN,const void *S,size_t N,void *Scratch,size_t SN,
 void *Staging,size_t TN,size_t Offset,size_t Available){
 T70_BUFFER_FILL_RESULT R={0};
 if(!B)return R;
 const uintptr_t A[]={(uintptr_t)B,(uintptr_t)D,(uintptr_t)S,
  (uintptr_t)Scratch,(uintptr_t)Staging,(uintptr_t)B->VirtualAddress};
 const size_t L[]={sizeof(*B),DN,N,SN,TN,B->Length};
 for(unsigned I=0;I<6;I++){
  if(L[I]&&(!A[I]||L[I]>UINTPTR_MAX-A[I]))return R;
  for(unsigned J=0;J<I;J++)if(L[I]&&L[J]&&A[I]<A[J]+L[J]&&A[J]<A[I]+L[I])return R;
 }
 if(!B->Adapter||!B->Free||!B->VirtualAddress||!B->Length||TN<B->Length||
    !T70AfeDl1PreparedValid(&B->Prepared)||B->Prepared.BufferBytes!=B->Length||
    B->Prepared.DmaBase!=(uint64_t)B->LogicalAddress.QuadPart)return R;
 R.BeginAttempted=1;R.BeginStatus=T70AfeCommonBufferBegin(B);
 if(!NT_SUCCESS(R.BeginStatus)){R.Code=T70BufferFillBeginFailed;return R;}
 R.LeaseHeld=1;T70_PCM_RING_PROGRESS P;
 if(!T70WaveRingCopy(D,DN,&B->Prepared,S,N,Scratch,SN,Staging,TN,Offset,Available,&P)){
  R.Code=T70BufferFillConversionFailed;
 }else{
  size_t First=N;
  if(First>B->Length-Offset)First=B->Length-Offset;
  if(First){RtlCopyDeviceMemory((volatile unsigned char *)B->VirtualAddress+Offset,
     (unsigned char *)Staging+Offset,First);++R.CopyCalls;}
  if(N>First){RtlCopyDeviceMemory(B->VirtualAddress,Staging,N-First);++R.CopyCalls;}
  R.Progress=P;R.ProgressValid=1;R.Code=T70BufferFillCopied;
 }
 R.EndAttempted=1;R.EndStatus=T70AfeCommonBufferEnd(B);
 if(!NT_SUCCESS(R.EndStatus)){R.Code=T70BufferFillEndFailed;return R;}
 R.LeaseHeld=0;return R;
}
