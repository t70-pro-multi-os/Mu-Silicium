// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Wave24Ring.h"
#include "WavePrepare.h"
#include "Wave24Convert.h"
#include <stdint.h>
int T70Wave24RingCopy(const void *Descriptor,size_t DescriptorBytes,
 const T70_AFE_PREPARED *Prepared,const void *Source,size_t Bytes,
 void *Scratch,size_t ScratchCapacity,void *Ring,size_t RingCapacity,
 size_t WriteOffset,size_t AvailableBytes,T70_PCM_RING_PROGRESS *Progress)
{
 const void *P[6]={Descriptor,Prepared,Source,Scratch,Ring,Progress};
 size_t N[6]={DescriptorBytes,sizeof(*Prepared),Bytes,ScratchCapacity,
              RingCapacity,sizeof(*Progress)};
 uintptr_t A[6],E[6];size_t I,J;
 T70_AFE_PREPARED Unused;T70_PCM_RING_PROGRESS Check;
 const unsigned char *D=Descriptor;
 if(!Descriptor || !Prepared || !Progress || !Ring || (!Source && Bytes) ||
    (!Scratch && ScratchCapacity) || Bytes>ScratchCapacity)return 0;
 for(I=0;I<6;I++){
   A[I]=(uintptr_t)P[I];
   if(N[I]>UINTPTR_MAX-A[I])return 0;
   E[I]=A[I]+N[I];
   for(J=0;J<I;J++)
     if(N[I] && N[J] && A[I]<E[J] && A[J]<E[I])return 0;
 }
 if((uintptr_t)Prepared%_Alignof(T70_AFE_PREPARED) ||
    (uintptr_t)Progress%_Alignof(T70_PCM_RING_PROGRESS) ||
    !T70AfeDl1PreparedValid(Prepared) ||
    Prepared->SampleFormat!=T70AfeS24Le32)return 0;
 if(T70WaveExtensiblePrepare(Descriptor,DescriptorBytes,Prepared->DmaBase,
    Prepared->BufferBytes,Prepared->PeriodBytes,&Unused)!=
    T70WaveNeeds24BitNormalization)return 0;
 /* The parser already proved layout, sizes, rate and geometry. Require the
  * caller's immutable numeric snapshot to describe exactly the same stream. */
 uint32_t Rate=(uint32_t)D[4]|(uint32_t)D[5]<<8|
               (uint32_t)D[6]<<16|(uint32_t)D[7]<<24;
 uint32_t Channels=(uint32_t)D[2]|(uint32_t)D[3]<<8;
 if(Rate!=Prepared->Rate || Channels!=Prepared->Channels ||
    Bytes>AvailableBytes || Bytes%Prepared->Format.FrameBytes)return 0;
 /* Zero-copy preflight uses local metadata and performs no ring writes. */
 if(!T70PcmRingCopy(Prepared,Ring,RingCapacity,WriteOffset,AvailableBytes,
                   NULL,0,&Check))return 0;
 if(!T70Wave24ToS24Le(Source,Bytes,Channels,Scratch,ScratchCapacity))return 0;
 return T70PcmRingCopy(Prepared,Ring,RingCapacity,WriteOffset,AvailableBytes,
                       Scratch,Bytes,Progress);
}
