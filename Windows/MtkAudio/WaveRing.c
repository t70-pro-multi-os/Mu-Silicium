// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRing.h"
#include "WavePrepare.h"
#include "Wave24Ring.h"
int T70WaveRingCopy(const void *Descriptor,size_t DescriptorBytes,
 const T70_AFE_PREPARED *Prepared,const void *Source,size_t Bytes,
 void *Scratch,size_t ScratchCapacity,void *Ring,size_t RingCapacity,
 size_t WriteOffset,size_t AvailableBytes,T70_PCM_RING_PROGRESS *Progress)
{
 const void *P[6]={Descriptor,Prepared,Source,Scratch,Ring,Progress};
 size_t N[6]={DescriptorBytes,sizeof(*Prepared),Bytes,ScratchCapacity,
              RingCapacity,sizeof(*Progress)};
 uintptr_t A[6],E[6];size_t I,J;T70_AFE_PREPARED Parsed;
 if(!Descriptor || !Prepared || !Progress || !Ring || (!Source && Bytes) ||
    (!Scratch && ScratchCapacity))return 0;
 for(I=0;I<6;I++){
   A[I]=(uintptr_t)P[I];
   if(N[I]>UINTPTR_MAX-A[I])return 0;
   E[I]=A[I]+N[I];
   for(J=0;J<I;J++)
     if(N[I] && N[J] && A[I]<E[J] && A[J]<E[I])return 0;
 }
 if((uintptr_t)Prepared%_Alignof(T70_AFE_PREPARED) ||
    (uintptr_t)Progress%_Alignof(T70_PCM_RING_PROGRESS) ||
    !T70AfeDl1PreparedValid(Prepared))return 0;
 T70_WAVE_PREPARE_RESULT Kind=T70WaveExtensiblePrepare(Descriptor,
    DescriptorBytes,Prepared->DmaBase,Prepared->BufferBytes,
    Prepared->PeriodBytes,&Parsed);
 if(Kind==T70WaveNeeds24BitNormalization)
   return T70Wave24RingCopy(Descriptor,DescriptorBytes,Prepared,Source,Bytes,
     Scratch,ScratchCapacity,Ring,RingCapacity,WriteOffset,AvailableBytes,Progress);
 if(Kind!=T70WavePrepared || Parsed.SampleFormat!=T70AfeS16Le ||
    Prepared->SampleFormat!=Parsed.SampleFormat ||
    Prepared->Rate!=Parsed.Rate || Prepared->Channels!=Parsed.Channels)return 0;
 return T70PcmRingCopy(Prepared,Ring,RingCapacity,WriteOffset,AvailableBytes,
                       Source,Bytes,Progress);
}
