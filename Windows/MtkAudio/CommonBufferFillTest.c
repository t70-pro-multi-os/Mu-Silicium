// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CommonBufferFill.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Calls,Irql,Raise,ExpectedUses=1;static unsigned char Device[192],Stage[200];static T70_AFE_COMMON_BUFFER *Owner;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
volatile void *RtlCopyDeviceMemory(volatile void *D,volatile const void *S,size_t N){
 uintptr_t A=(uintptr_t)D,B=(uintptr_t)S;C(A>=(uintptr_t)Device&&A+N<=(uintptr_t)Device+192);C(B>=(uintptr_t)Stage&&B+N<=(uintptr_t)Stage+192);C(Owner->ActiveUses==ExpectedUses);
 memcpy((void *)D,(const void *)S,N);Calls++;if(Raise)Irql=1;return D;
}
static void Put(unsigned char *P,uint32_t V,unsigned N){while(N--){*P++=(unsigned char)V;V>>=8;}}
static void Free(PDMA_ADAPTER A,ULONG N,PHYSICAL_ADDRESS L,void *V,BOOLEAN Cache){(void)A;C(N==192&&L.QuadPart==4096&&V==Device&&!Cache);}
static void *Alloc(PDMA_ADAPTER A,PHYSICAL_ADDRESS *Max,ULONG N,PHYSICAL_ADDRESS *L,BOOLEAN Cache,ULONG Node){(void)A;C(N==192&&Max->QuadPart==0x3ffffffff&&!Cache&&!Node);L->QuadPart=4096;return Device;}
int main(void){DMA_OPERATIONS Ops={sizeof Ops,Free,Alloc,NULL,NULL};DMA_ADAPTER A={1,sizeof A,&Ops};T70_AFE_COMMON_BUFFER B={0};Owner=&B;
 unsigned char D[40]={0},S[192],Scratch[200],OtherScratch[200],Expected[200],Old[192];
 Put(D,0xfffe,2);Put(D+4,48000,4);Put(D+16,22,2);const unsigned char Guid[16]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};memcpy(D+24,Guid,16);
 for(unsigned F=0;F<2;F++)for(unsigned Ch=1;Ch<=2;Ch++){
  unsigned Sample=F?4:2,Frame=Sample*Ch;Put(D+2,Ch,2);Put(D+8,48000*Frame,4);Put(D+12,Frame,2);Put(D+14,Sample*8,2);Put(D+18,F?24:16,2);Put(D+20,Ch==1?4:3,4);
  C(!T70AfeCommonBufferAcquire(&B,&A,48000,F?2:1,Ch,192,96));
  for(unsigned I=0;I<192;I++)S[I]=(unsigned char)(I*17);if(F)for(unsigned I=0;I<192;I+=4)S[I]=0;
  for(unsigned O=0;O<192;O+=Frame)for(unsigned N=0;N<=192;N+=Frame){
   memset(Device,0xa5,sizeof Device);memset(Stage,0x77,sizeof Stage);memset(Expected,0xa5,sizeof Expected);Calls=0;
   T70_PCM_RING_PROGRESS P;C(T70WaveRingCopy(D,40,&B.Prepared,S,N,F?OtherScratch:NULL,F?200:0,Expected,200,O,192,&P));
   T70_BUFFER_FILL_RESULT R=T70AfeCommonBufferFill(&B,D,40,S,N,F?Scratch:NULL,F?200:0,Stage,200,O,192);
   C(R.Code==T70BufferFillCopied&&R.ProgressValid&&!R.LeaseHeld&&!B.ActiveUses);C(R.CopyCalls==Calls&&Calls==(N?(N>192-O?2u:1u):0u));
   C(R.Progress.NextWriteOffset==P.NextWriteOffset&&R.Progress.BytesCopied==N&&R.Progress.FramesCopied==N/Frame);C(!memcmp(Device,Expected,192));
  }
  memset(Device,0x5a,sizeof Device);memcpy(Old,Device,192);Calls=0;
#define FAIL(DP,DN,SP,N,SC,SN,ST,TN,O,AV) do{T70_BUFFER_FILL_RESULT R=T70AfeCommonBufferFill(&B,DP,DN,SP,N,SC,SN,ST,TN,O,AV);C(!R.ProgressValid&&!R.CopyCalls&&!R.LeaseHeld);C(!Calls&&!B.ActiveUses&&!memcmp(Device,Old,192));}while(0)
  FAIL(D,39,S,Frame,Scratch,200,Stage,200,0,192);
  FAIL(D,40,Device,Frame,Scratch,200,Stage,200,0,192);
  FAIL(D,40,S,Frame,Scratch,200,Device,192,0,192);
  FAIL(D,40,S,Frame,Device,192,Stage,200,0,192);
  FAIL(D,40,&B,Frame,Scratch,200,Stage,200,0,192);
  FAIL(D,40,S,Frame,Scratch,SIZE_MAX,Stage,200,0,192);
  FAIL(D,40,S,Frame,Scratch,200,Stage,191,0,192);
  FAIL(D,40,S,Frame-1,Scratch,200,Stage,200,0,192);
  FAIL(D,40,S,Frame,Scratch,200,Stage,200,192,192);
  FAIL(D,40,S,Frame,Scratch,200,Stage,200,0,0);
  if(F){S[188]=1;FAIL(D,40,S,192,Scratch,200,Stage,200,0,192);S[188]=0;FAIL(D,40,S,Frame,Scratch,Frame-1,Stage,200,0,192);}
  Irql=1;FAIL(D,40,S,Frame,Scratch,200,Stage,200,0,192);Irql=0;
  Raise=1;Calls=0;T70_BUFFER_FILL_RESULT R=T70AfeCommonBufferFill(&B,D,40,S,Frame,Scratch,200,Stage,200,0,192);
  C(R.Code==T70BufferFillEndFailed&&R.ProgressValid&&R.LeaseHeld&&R.CopyCalls==1&&B.ActiveUses==1);
  C(R.EndStatus==STATUS_INVALID_DEVICE_STATE);Irql=Raise=0;C(!T70AfeCommonBufferEnd(&B));
  C(!T70AfeCommonBufferBegin(&B));ExpectedUses=2;Calls=0;
  R=T70AfeCommonBufferFill(&B,D,40,S,Frame,Scratch,200,Stage,200,0,192);
  C(R.Code==T70BufferFillCopied&&!R.LeaseHeld&&B.ActiveUses==1&&Calls==1);
  C(!T70AfeCommonBufferEnd(&B));ExpectedUses=1;Raise=1;Calls=0;
  R=T70AfeCommonBufferFill(&B,D,40,S,Frame*2,Scratch,200,Stage,200,192-Frame,192);
  C(R.Code==T70BufferFillEndFailed&&R.ProgressValid&&R.Progress.BytesCopied==Frame*2&&R.CopyCalls==2&&Calls==2&&B.ActiveUses==1);
  Irql=Raise=0;C(!T70AfeCommonBufferEnd(&B));C(!T70AfeCommonBufferRelease(&B));
 }
 C(T70AfeCommonBufferFill(NULL,D,40,S,0,NULL,0,Stage,200,0,0).Code==T70BufferFillInvalid);
 printf("CommonBufferFill: %u checks; fake Device Memory accessor\n",Checks);return 0;
}
