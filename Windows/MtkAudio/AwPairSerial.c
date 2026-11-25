// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairSerial.h"
static uint16_t U16(const uint8_t *P){return (uint16_t)(P[0]|((uint16_t)P[1]<<8));}
int T70AwPairSerialObserve(const void *Acf,size_t Bytes,uint64_t Base,
 uint16_t Address0,uint16_t Address1,uint16_t Profile,T70_AW_PAIR_SERIAL *Out){
 T70_AW_PAIR_PROFILE_VIEW V;T70_AW_PAIR_SERIAL S={0};
 uintptr_t A=(uintptr_t)Acf,O=(uintptr_t)Out;
 if(!Acf||!Out||!Bytes||Bytes>UINTPTR_MAX-A||sizeof(*Out)>UINTPTR_MAX-O||
    O%_Alignof(T70_AW_PAIR_SERIAL)||(A<O+sizeof(*Out)&&O<A+Bytes))return 0;
 if(!T70AwPairProfileView(Acf,Bytes,Base,Address0,Address1,Profile,&V))return 0;
 for(unsigned C=0;C<2;C++){
  uint16_t Words[4]={0};unsigned Seen=0;
  for(uint32_t I=0;I<V.Channel[C].RecordCount;I++){
   const uint8_t *P=V.Channel[C].Section.Data+64+(size_t)I*4;
   uint16_t R=U16(P);unsigned K=R==6?0:R==7?1:R==8?2:R==0x13?3:4;
   if(K==4)continue;
   if(Seen&(1u<<K))return 0;
   Seen|=1u<<K;Words[K]=U16(P+2);
  }
  if(Seen!=15||!T70AwDecodeSerialFields(Words[0],Words[1],Words[2],Words[3],&S.Channel[C]))return 0;
 }
 S.Profile=Profile;*Out=S;return 1;
}
