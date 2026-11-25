// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PcmRingCopy.h"
#include <stdint.h>
#include <string.h>
static int Range(const void *P,size_t N,uintptr_t *Start,uintptr_t *End) {
 uintptr_t A=(uintptr_t)P;
 if(N>UINTPTR_MAX-A)return 0;
 *Start=A;*End=A+N;return 1;
}
static int Overlap(uintptr_t A,uintptr_t B,uintptr_t C,uintptr_t D) {
 return A<B && C<D && A<D && C<B;
}
int T70PcmRingCopy(const T70_AFE_PREPARED *Prepared,void *Ring,size_t RingCapacity,
 size_t WriteOffset,size_t AvailableBytes,const void *Source,size_t Bytes,
 T70_PCM_RING_PROGRESS *Progress) {
 uintptr_t R,RE,S,SE,P,PE,G,GE,CapacityEnd;
 if(!Ring || !Progress || !Prepared || (!Source && Bytes) ||
    !T70AfeDl1PreparedValid(Prepared) || Prepared->BufferBytes>SIZE_MAX)return 0;
 size_t Size=(size_t)Prepared->BufferBytes,Frame=Prepared->Format.FrameBytes;
 if(RingCapacity<Size || WriteOffset>=Size || WriteOffset%Frame ||
    AvailableBytes>Size || AvailableBytes%Frame || Bytes>AvailableBytes ||
    Bytes%Frame)return 0;
 if(!Range(Ring,RingCapacity,&R,&CapacityEnd) || !Range(Ring,Size,&R,&RE) ||
    !Range(Source,Bytes,&S,&SE) || !Range(Progress,sizeof(*Progress),&P,&PE) ||
    !Range(Prepared,sizeof(*Prepared),&G,&GE))return 0;
 if(Overlap(R,RE,S,SE) || Overlap(P,PE,R,CapacityEnd) || Overlap(P,PE,S,SE) ||
    Overlap(P,PE,G,GE) || Overlap(G,GE,R,RE))return 0;
 size_t First=Bytes<Size-WriteOffset?Bytes:Size-WriteOffset;
 if(First)memcpy((unsigned char *)Ring+WriteOffset,Source,First);
 if(Bytes>First)memcpy(Ring,(const unsigned char *)Source+First,Bytes-First);
 T70_PCM_RING_PROGRESS Next={Bytes>=Size-WriteOffset?Bytes-(Size-WriteOffset):
                             WriteOffset+Bytes,Bytes,Bytes/Frame};
 *Progress=Next;return 1;
}
