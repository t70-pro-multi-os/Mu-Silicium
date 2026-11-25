// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CommonBufferConfigure.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Calls,Fail,Irql,Raise,Freed;static ULONG Ram[2048];static unsigned char Bytes[4096];
static T70_AFE_COMMON_BUFFER *Buffer;static T70_AFE_MAPPING_OWNER *Map;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG F){C(P.QuadPart==0x11210000&&N==8192&&F==(PAGE_READWRITE|PAGE_NOCACHE));return Ram;}
void MmUnmapIoSpace(void *V,size_t N){C(V==Ram&&N==8192);}
static void Free(PDMA_ADAPTER A,ULONG L,PHYSICAL_ADDRESS P,void *V,BOOLEAN Cache){(void)A;C(L==4096&&P.QuadPart==0x100000000&&V==Bytes&&!Cache);Freed++;}
static void *Allocate(PDMA_ADAPTER A,PHYSICAL_ADDRESS *Max,ULONG L,PHYSICAL_ADDRESS *P,BOOLEAN Cache,ULONG Node){(void)A;C(Max->QuadPart==0x3ffffffff&&L==4096&&!Cache&&!Node);P->QuadPart=0x100000000;return Bytes;}
static int32_t Step(void *Ctx,uint32_t O,int Write,uint32_t V){
 C(Ctx==&Map->Mapping&&Map->ActiveUses==1&&Buffer->ActiveUses==1);
 unsigned I=Calls++;const uint32_t Off[]={0x54,0x64,0x50,0x60},Val[]={0,4095,1,1};
 C(I<4?(Write&&O==Off[I]&&V==Val[I]):(O==0x4c&&Write==(I%2)));
 if(Raise&&(Calls==Fail||Calls==12))Irql=3;
 return Calls==Fail?-73:0;
}
int32_t T70AfeMappedRead32(void *Ctx,uint32_t O,uint32_t *V){*V=0;return Step(Ctx,O,0,0);}
int32_t T70AfeMappedWrite32(void *Ctx,uint32_t O,uint32_t V){return Step(Ctx,O,1,V);}
int main(void){DMA_OPERATIONS Ops={sizeof Ops,Free,Allocate,NULL,NULL};DMA_ADAPTER A={1,sizeof A,&Ops};T70_AFE_COMMON_BUFFER B={0};T70_AFE_MAPPING_OWNER M={0};Buffer=&B;Map=&M;
 memset(Bytes,0xa5,sizeof Bytes);
 C(!T70AfeCommonBufferAcquire(&B,&A,48000,1,2,4096,1024));C(!T70AfeMappingAcquire(&M,0x11210000,8192));
 for(unsigned X=0;X<2;X++)for(unsigned F=0;F<=12;F++){
  Raise=X;Fail=F;Calls=0;T70_BUFFER_CONFIG_RESULT R=T70AfeConfigureCommonBufferMapped(&B,&M);
  C(R.Code==T70BufferConfigLeaseRetained&&R.BufferLeaseHeld&&!R.BufferEndAttempted&&R.MappedCalled);
  C(B.ActiveUses==1&&Calls==(F?F:12));C(M.ActiveUses==X);
  C(R.Mapped.Code==(X?T70MappedConfigEndFailed:F?T70MappedConfigConfigureFailed:T70MappedConfigCallbacksCompleted));
  C(R.Mapped.EndStatus==(X?STATUS_INVALID_DEVICE_STATE:0)&&R.Mapped.LeaseEnded==!X);
  C(R.Mapped.Configure.Code==(F?(F<=4?T70AfeConfigAddressFailed:T70AfeConfigFormatFailed):T70AfeConfigCallbacksCompleted));
  Irql=0;C(T70AfeCommonBufferRelease(&B)==STATUS_INVALID_DEVICE_STATE&&Freed==0);
  unsigned Before=Calls;C(T70AfeConfigureCommonBufferMapped(&B,&M).Code==T70BufferConfigInvalid&&Calls==Before);
  if(X)C(!T70AfeMappingEnd(&M));C(!T70AfeCommonBufferEnd(&B)); /* model caller recovery, not hardware proof */
 }
 Raise=Fail=Calls=0;T70_AFE_PREPARED Saved=B.Prepared;
 for(unsigned Bad=0;Bad<4;Bad++){
  B.Prepared=Saved;if(Bad==0)B.Prepared.DmaBase++;if(Bad==1)B.Length++;if(Bad==2)B.LogicalAddress.QuadPart++;if(Bad==3)B.Prepared.Format.RateCode++;
  C(T70AfeConfigureCommonBufferMapped(&B,&M).Code==T70BufferConfigInvalid);C(!Calls&&!B.ActiveUses&&!M.ActiveUses);
  B.Length=4096;B.LogicalAddress.QuadPart=0x100000000;
 }B.Prepared=Saved;
 C(T70AfeDl1Prepare(48000,1,2,0,4096,1024,&B.Prepared));
 C(T70AfeConfigureCommonBufferMapped(&B,&M).Code==T70BufferConfigInvalid);C(!Calls&&!B.ActiveUses);
 C(T70AfeDl1Prepare(48000,1,2,0x100000000,2048,1024,&B.Prepared));
 C(T70AfeConfigureCommonBufferMapped(&B,&M).Code==T70BufferConfigInvalid);C(!Calls&&!B.ActiveUses);B.Prepared=Saved;
 for(unsigned I=0;I<sizeof Bytes;I++)C(Bytes[I]==0xa5);
 Irql=1;C(T70AfeConfigureCommonBufferMapped(&B,&M).Code==T70BufferConfigBeginFailed);C(!Calls&&!B.ActiveUses);Irql=0;
 C(!T70AfeMappingRelease(&M));T70_BUFFER_CONFIG_RESULT R=T70AfeConfigureCommonBufferMapped(&B,&M);
 C(R.Code==T70BufferConfigNotStarted&&R.BufferEndAttempted&&!R.BufferLeaseHeld&&R.Mapped.Code==T70MappedConfigBeginFailed);C(!B.ActiveUses&&!Calls);
 C(T70AfeConfigureCommonBufferMapped(NULL,&M).Code==T70BufferConfigInvalid);C(T70AfeConfigureCommonBufferMapped(&B,NULL).Code==T70BufferConfigInvalid);
 C(T70AfeConfigureCommonBufferMapped(&B,(void *)&B).Code==T70BufferConfigInvalid);
 C(!T70AfeCommonBufferRelease(&B)&&Freed==1);
 printf("CommonBufferConfigure: %u checks; fake register callbacks only\n",Checks);return 0;
}
