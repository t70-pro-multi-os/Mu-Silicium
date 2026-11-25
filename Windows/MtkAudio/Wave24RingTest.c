// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Wave24Ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks;
#define CHECK(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
static void Put(unsigned char *P,uint32_t V,unsigned N){while(N--){*P++=(unsigned char)V;V>>=8;}}
int main(void){
 unsigned char D[40]={0},S[192],Scratch[200],Ring[200],Old[200],Expected[200];
 T70_AFE_PREPARED P;T70_PCM_RING_PROGRESS G,Before;
 unsigned C,I,O,N;
 Put(D,0xfffe,2);Put(D+4,48000,4);Put(D+14,32,2);Put(D+16,22,2);
 Put(D+18,24,2);
 const unsigned char Guid[16]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 memcpy(D+24,Guid,16);
 for(I=0;I<192;I+=4){S[I]=0;S[I+1]=(unsigned char)I;S[I+2]=0x55;S[I+3]=(I&8)?0x80:0x7f;}
 for(C=1;C<=2;C++){
  Put(D+2,C,2);Put(D+8,48000*C*4,4);Put(D+12,C*4,2);Put(D+20,C==1?4:3,4);
  CHECK(T70AfeDl1Prepare(48000,T70AfeS24Le32,C,0x1000,192,96,&P));
  for(O=0;O<192;O+=C*4)for(N=0;N<=192;N+=C*4){
   memset(Ring,0xa5,sizeof Ring);memcpy(Expected,Ring,sizeof Ring);
   for(I=0;I<N;I+=4){
    Expected[(O+I)%192]=S[I+1];Expected[(O+I+1)%192]=S[I+2];
    Expected[(O+I+2)%192]=S[I+3];Expected[(O+I+3)%192]=(S[I+3]&128)?255:0;
   }
   CHECK(T70Wave24RingCopy(D,40,&P,S,N,Scratch,200,Ring,200,O,192,&G));
   CHECK(!memcmp(Ring,Expected,sizeof Ring));
   CHECK(G.NextWriteOffset==(O+N)%192 && G.BytesCopied==N && G.FramesCopied==N/(C*4));
  }
 }
 memset(Ring,0xa5,sizeof Ring);memcpy(Old,Ring,sizeof Ring);
 memset(&G,0x55,sizeof G);Before=G;
#define FAIL(DP,DL,PP,SP,B,SC,CL,RP,RL,OFF,AV,GP) do{\
 CHECK(!T70Wave24RingCopy(DP,DL,PP,SP,B,SC,CL,RP,RL,OFF,AV,GP));\
 CHECK(!memcmp(Ring,Old,sizeof Ring)&&!memcmp(&G,&Before,sizeof G));}while(0)
 S[188]=1;
 FAIL(D,40,&P,S,192,Scratch,200,Ring,200,184,192,&G);S[188]=0;
 FAIL(D,39,&P,S,8,Scratch,200,Ring,200,0,192,&G);
 FAIL(D,40,&P,S,7,Scratch,200,Ring,200,0,192,&G);
 FAIL(D,40,&P,S,8,Scratch,7,Ring,200,0,192,&G);
 FAIL(D,40,&P,S,8,Scratch,200,Ring,191,0,192,&G);
 FAIL(D,40,&P,S,8,Scratch,200,Ring,200,192,192,&G);
 FAIL(D,40,&P,S,8,Scratch,200,Ring,200,0,0,&G);
 FAIL(D,40,&P,S,8,Ring,200,Ring,200,0,192,&G);
 FAIL(D,40,&P,S,8,S,192,Ring,200,0,192,&G);
 FAIL(D,40,&P,S,8,Scratch,SIZE_MAX,Ring,200,0,192,&G);
 FAIL(D,40,&P,(void *)(UINTPTR_MAX-1),8,Scratch,200,Ring,200,0,192,&G);
 FAIL(D,40,&P,S,8,Scratch,200,Ring,200,0,192,(void *)Scratch);
 Put(D+18,32,2);FAIL(D,40,&P,S,8,Scratch,200,Ring,200,0,192,&G);Put(D+18,24,2);
 Put(D+4,44100,4);Put(D+8,44100*8,4);
 FAIL(D,40,&P,S,8,Scratch,200,Ring,200,0,192,&G);
 Put(D+4,48000,4);Put(D+8,48000*8,4);
 CHECK(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x1000,192,96,&P));
 FAIL(D,40,&P,S,8,Scratch,200,Ring,200,0,192,&G);
 CHECK(T70AfeDl1Prepare(48000,T70AfeS24Le32,2,0x1000,192,96,&P));
 CHECK(T70Wave24RingCopy(D,40,&P,NULL,0,NULL,0,Ring,200,0,192,&G));
 CHECK(!memcmp(Ring,Old,sizeof Ring)&&G.BytesCopied==0);
 printf("Wave24Ring: %u checks\n",Checks);return 0;
}
