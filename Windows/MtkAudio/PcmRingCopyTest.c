// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PcmRingCopy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{assert(X);++Checks;}while(0)
int main(void) {
 T70_AFE_PREPARED P;unsigned char Ring[528],Old[528],Want[528],Src[512];
 T70_PCM_RING_PROGRESS Out,Before;
 C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x100000000ULL,512,256,&P));
 for(size_t I=0;I<sizeof Src;I++)Src[I]=(unsigned char)(I*7+3);
 for(unsigned Fmt=0;Fmt<2;Fmt++)for(unsigned Ch=1;Ch<=2;Ch++){
 C(T70AfeDl1Prepare(48000,Fmt?T70AfeS24Le32:T70AfeS16Le,Ch,0x100000000ULL,512,256,&P));
 size_t Frame=P.Format.FrameBytes;
 for(size_t Offset=0;Offset<512;Offset+=Frame)for(size_t Bytes=0;Bytes<=512;Bytes+=Frame){
 memset(Ring,0xa5,sizeof Ring);memcpy(Want,Ring,sizeof Ring);
 for(size_t I=0;I<Bytes;I++)Want[(Offset+I)%512]=Src[I];
 C(T70PcmRingCopy(&P,Ring,sizeof Ring,Offset,Bytes,Src,Bytes,&Out));
 C(!memcmp(Ring,Want,sizeof Ring));C(Out.NextWriteOffset==(Offset+Bytes)%512);
 C(Out.BytesCopied==Bytes && Out.FramesCopied==Bytes/Frame);
 }
 }
 C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x100000000ULL,512,256,&P));
#define RESET() do{memset(Ring,0xa5,sizeof Ring);memcpy(Old,Ring,sizeof Ring);memset(&Out,0x5a,sizeof Out);Before=Out;}while(0)
#define BAD(CALL) do{RESET();C(!(CALL));C(!memcmp(Ring,Old,sizeof Ring));C(!memcmp(&Out,&Before,sizeof Out));}while(0)
 BAD(T70PcmRingCopy(&P,Ring,511,0,512,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,512,512,Src,0,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,1,512,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,513,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,511,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,0,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Src,3,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Src,SIZE_MAX,&Out));
 BAD(T70PcmRingCopy(&P,Ring,SIZE_MAX,0,512,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Ring+508,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,(void *)(UINTPTR_MAX-1),4,&Out));
 BAD(T70PcmRingCopy(&P,(void *)(UINTPTR_MAX-1),512,0,512,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,0,4,&Out));
 BAD(T70PcmRingCopy(0,Ring,sizeof Ring,0,512,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,0,sizeof Ring,0,512,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Src,4,0));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Src,4,(T70_PCM_RING_PROGRESS *)(void *)Ring));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,&Out,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Src,4,(T70_PCM_RING_PROGRESS *)(void *)&P));
 P.Geometry.EndLow^=1;BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Src,4,&Out));P.Geometry.EndLow^=1;
 RESET();C(T70PcmRingCopy(&P,Ring,sizeof Ring,508,0,0,0,&Out));C(Out.NextWriteOffset==508 && Out.BytesCopied==0);C(!memcmp(Ring,Old,sizeof Ring));
 /* Unaligned ordinary CPU storage is legal; PCM alignment is relative to ring. */
 RESET();C(T70PcmRingCopy(&P,Ring+1,512,508,8,Src,8,&Out));C(Out.NextWriteOffset==4);
 /* Geometry extrema and full-ring wrap using real ordinary storage. */
 static unsigned char Big[196624],BigSource[196608];
 for(size_t I=0;I<sizeof BigSource;I++)BigSource[I]=(unsigned char)(I*3+1);
 for(size_t Size=192;Size<=196608;Size=196608){
 C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,Size,Size/2,&P));
 memset(Big,0xa5,sizeof Big);
 C(T70PcmRingCopy(&P,Big,sizeof Big,Size-4,Size,BigSource,Size,&Out));
 C(Out.NextWriteOffset==Size-4 && Out.FramesCopied==Size/4);
 C(!memcmp(Big+Size-4,BigSource,4));C(!memcmp(Big,BigSource+4,Size-4));
 C(Big[Size]==0xa5);
 if(Size==196608)break;
 }
 C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,512,256,&P));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,SIZE_MAX,Src,4,&Out));
 BAD(T70PcmRingCopy(&P,Ring,sizeof Ring,0,512,Src,4,(T70_PCM_RING_PROGRESS *)(UINTPTR_MAX-1)));
 union {T70_AFE_PREPARED Align;unsigned char Data[528];} Shared;
 memset(&Shared,0,sizeof Shared);memcpy(Shared.Data,&P,sizeof P);
 unsigned char Saved[sizeof Shared];memcpy(Saved,&Shared,sizeof Shared);
 RESET();C(!T70PcmRingCopy(&Shared.Align,Shared.Data,sizeof Shared,0,512,Src,4,&Out));
 C(!memcmp(Saved,&Shared,sizeof Shared));C(!memcmp(&Out,&Before,sizeof Out));
 /* Metadata cannot change the allocation's trailing storage either. */
 union {T70_PCM_RING_PROGRESS Align;unsigned char Data[544];} Tail;
 memset(&Tail,0xa5,sizeof Tail);unsigned char TailBefore[sizeof Tail];
 memcpy(TailBefore,&Tail,sizeof Tail);
 C(!T70PcmRingCopy(&P,Tail.Data,sizeof Tail,0,512,Src,4,
                  (T70_PCM_RING_PROGRESS *)(void *)(Tail.Data+512)));
 C(!memcmp(TailBefore,&Tail,sizeof Tail));
 printf("PcmRingCopyTest: %u checks; CPU memory only\n",Checks);
}
