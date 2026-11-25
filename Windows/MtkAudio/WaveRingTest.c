// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRing.h"
#include "Wave24Ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks;
#define CHECK(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
static void Put(unsigned char *P,uint32_t V,unsigned N){while(N--){*P++=(unsigned char)V;V>>=8;}}
int main(void){
 unsigned char D[40]={0},S[192],Scratch[200],OtherScratch[200],R[200],Expected[200],Old[200];
 T70_AFE_PREPARED P;T70_PCM_RING_PROGRESS G,H,Before;
 unsigned F,C,I,O,N;
 Put(D,0xfffe,2);Put(D+4,48000,4);Put(D+16,22,2);
 const unsigned char Guid[16]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 memcpy(D+24,Guid,16);
 for(F=0;F<2;F++)for(C=1;C<=2;C++){
  unsigned Sample=F?4:2,Frame=Sample*C;
  Put(D+2,C,2);Put(D+8,48000*Frame,4);Put(D+12,Frame,2);
  Put(D+14,Sample*8,2);Put(D+18,F?24:16,2);Put(D+20,C==1?4:3,4);
  CHECK(T70AfeDl1Prepare(48000,F?T70AfeS24Le32:T70AfeS16Le,C,4096,192,96,&P));
  for(I=0;I<192;I++)S[I]=(unsigned char)(I*17);
  if(F)for(I=0;I<192;I+=4)S[I]=0;
  for(O=0;O<192;O+=Frame)for(N=0;N<=192;N+=Frame){
   memset(R,0xa5,sizeof R);memcpy(Expected,R,sizeof R);memset(Scratch,0x77,sizeof Scratch);
   CHECK(T70WaveRingCopy(D,40,&P,S,N,F?Scratch:NULL,F?200:0,R,200,O,192,&G));
   CHECK(F?T70Wave24RingCopy(D,40,&P,S,N,OtherScratch,200,Expected,200,O,192,&H):
     T70PcmRingCopy(&P,Expected,200,O,192,S,N,&H));
   CHECK(!memcmp(R,Expected,sizeof R) && G.NextWriteOffset==H.NextWriteOffset &&
         G.BytesCopied==H.BytesCopied && G.FramesCopied==H.FramesCopied);
   if(!F)CHECK(Scratch[0]==0x77);
  }
  memset(R,0xa5,sizeof R);memcpy(Old,R,sizeof R);memset(&G,0x55,sizeof G);Before=G;
#define FAIL(DP,DL,PP,SP,B,SC,CL,RP,RL,OFF,AV,GP) do{\
 CHECK(!T70WaveRingCopy(DP,DL,PP,SP,B,SC,CL,RP,RL,OFF,AV,GP));\
 CHECK(!memcmp(R,Old,sizeof R)&&!memcmp(&G,&Before,sizeof G));}while(0)
  FAIL(D,39,&P,S,Frame,Scratch,200,R,200,0,192,&G);
  FAIL(D,40,&P,S,Frame-1,Scratch,200,R,200,0,192,&G);
  FAIL(D,40,&P,S,Frame,Scratch,200,R,191,0,192,&G);
  FAIL(D,40,&P,S,Frame,R,200,R,200,0,192,&G);
  FAIL(D,40,&P,S,Frame,S,192,R,200,0,192,&G);
  FAIL(D,40,&P,S,Frame,Scratch,SIZE_MAX,R,200,0,192,&G);
  FAIL(D,40,&P,S,Frame,NULL,1,R,200,0,192,&G);
  FAIL(D,40,&P,S,Frame,Scratch,200,R,200,192,192,&G);
  FAIL(D,40,&P,S,Frame,Scratch,200,R,200,0,0,&G);
  FAIL(D,40,&P,(void *)(UINTPTR_MAX-1),Frame,Scratch,200,R,200,0,192,&G);
  D[24]=3;FAIL(D,40,&P,S,Frame,Scratch,200,R,200,0,192,&G);D[24]=1;
  Put(D+18,32,2);FAIL(D,40,&P,S,Frame,Scratch,200,R,200,0,192,&G);Put(D+18,F?24:16,2);
  if(F){
   S[188]=1;FAIL(D,40,&P,S,192,Scratch,200,R,200,0,192,&G);S[188]=0;
   FAIL(D,40,&P,S,Frame,Scratch,Frame-1,R,200,0,192,&G);
  }else{
   CHECK(T70WaveRingCopy(D,40,&P,S,Frame,Scratch,200,R,200,0,192,&G));
   CHECK(Scratch[0]==0x77);
  }
  CHECK(T70AfeDl1Prepare(48000,F?T70AfeS16Le:T70AfeS24Le32,C,4096,192,96,&P));
  memcpy(R,Old,sizeof R);G=Before;
  FAIL(D,40,&P,S,Frame,Scratch,200,R,200,0,192,&G);
 }
 printf("WaveRing: %u checks\n",Checks);return 0;
}
