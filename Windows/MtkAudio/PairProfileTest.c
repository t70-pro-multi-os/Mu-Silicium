// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairProfile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
static uint32_t U32(const uint8_t *P){return P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);}
static void Put32(uint8_t *P,uint32_t V){for(unsigned I=0;I<4;I++)P[I]=(uint8_t)(V>>(I*8));}
static uint8_t Crc(const uint8_t *P,size_t N){uint8_t V=0;for(size_t I=0;I<N;I++){V^=P[I];for(unsigned J=0;J<8;J++)V=(uint8_t)((V>>1)^((V&1)?0x8c:0));}return V;}
static void Fail(const void *B,size_t N,uint16_t A,uint16_t D,uint16_t P){
 T70_AW_PAIR_PROFILE_VIEW V;memset(&V,0x5a,sizeof V);unsigned char Old[sizeof V];memcpy(Old,&V,sizeof V);
 C(!T70AwPairProfileView(B,N,0x11e01000,A,D,P,&V));C(!memcmp(Old,&V,sizeof V));
}
int main(int argc,char **argv){
 C(argc==2);FILE *F=fopen(argv[1],"rb");C(F);uint8_t B[3857],Copy[3857];size_t N=fread(B,1,sizeof B,F);C(!ferror(F));fclose(F);C(N==3856);
 for(unsigned P=0;P<=10;P+=10){T70_AW_PAIR_PROFILE_VIEW V;
 C(T70AwPairProfileView(B,N,0x11e01000,0x34,0x35,(uint16_t)P,&V));C(V.RegisterProfileCount==2);
 for(unsigned Ch=0;Ch<2;Ch++){C(V.Channel[Ch].Section.DeviceIndex==Ch);C(V.Channel[Ch].Section.Profile==P);C(V.Channel[Ch].Section.SectionType==4);C(V.Channel[Ch].RecordCount==57);C(V.Channel[Ch].SpecialCount==4);}
 for(size_t I=0;I<N;I++)Fail(B,I,0x34,0x35,(uint16_t)P);
 }
 Fail(NULL,N,0x34,0x35,0);Fail(B,N,0x35,0x34,0);Fail(B,N,0x34,0x34,0);Fail(B,N,0x34,0x35,1);
 T70_AW_PAIR_PROFILE_VIEW V;C(!T70AwPairProfileView(B,N,0,0x34,0x35,0,&V));C(!T70AwPairProfileView(B,N,0x11e01000,0x34,0x35,0,NULL));
 unsigned Selected=0;
 for(unsigned I=0;I<12;I++){
 const uint8_t *D=B+80+64*I;
 if(D[20]>1||U32(D+28)!=4)continue;
 Selected++;
 /* Build a valid container with exactly this descriptor/payload removed. */
 memcpy(Copy,B,80);Put32(Copy+56,11);Put32(Copy+52,11*64);
 uint32_t Cursor=80+11*64,OutIndex=0;
 for(unsigned J=0;J<12;J++)if(J!=I){
   const uint8_t *Src=B+80+64*J;uint8_t *Dst=Copy+80+64*OutIndex++;
   uint32_t Length=U32(Src+32);memcpy(Dst,Src,64);Put32(Dst+36,Cursor);
   memcpy(Copy+Cursor,B+U32(Src+36),Length);Cursor+=Length;
 }
 uint32_t Count;C(T70AwAcfValidate(Copy,Cursor,&Count));C(Count==11);
 Fail(Copy,Cursor,0x34,0x35,0);Fail(Copy,Cursor,0x34,0x35,10);
 /* Move a required profile to the other profile: duplicate or missing identity. */
 memcpy(Copy,B,N);Copy[80+64*I+26]=(D[26]==0?10:0);Fail(Copy,N,0x34,0x35,0);Fail(Copy,N,0x34,0x35,10);
 /* Missing channel member/type mismatch, while retaining payload offsets. */
 memcpy(Copy,B,N);Copy[80+64*I+20]=2;Fail(Copy,N,0x34,0x35,0);
 memcpy(Copy,B,N);Copy[80+64*I+28]=10;Fail(Copy,N,0x34,0x35,0);
 /* Corrupt any required inner bin, even one not requested; repair outer CRC. */
 memcpy(Copy,B,N);uint32_t O=U32(D+36),L=U32(D+32);Copy[O+32]=3;Copy[80+64*I+40]=Crc(Copy+O,L);
 Fail(Copy,N,0x34,0x35,0);Fail(Copy,N,0x34,0x35,10);
 }
 C(Selected==4);printf("PairProfileTest: %u checks; no I/O\n",Checks);
}
