// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Actual KS/resource/target lease/profile apply functions; synthetic profile-view and SPB
 * callbacks, not a real ACF validation/transport or Windows property test. */
#include "AwKsVolume.h"
#include "AwVolume.h"
#include <reshub.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static unsigned long Checks;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
typedef struct {uint8_t Bin[2][72];} FIXTURE;
static unsigned Irql,Calls,FailCall,EndFail,InvalidProfile,Records=1;
static uint16_t ReadRaw;static NTSTATUS ReadStatus;
static T70_AW_SPB_RESOURCES Owner;
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
NTSTATUS RESOURCE_HUB_CREATE_PATH_FROM_ID(UNICODE_STRING *P,ULONG L,ULONG H){(void)P;(void)L;(void)H;abort();}
NTSTATUS WdfIoTargetCreate(WDFDEVICE D,WDF_OBJECT_ATTRIBUTES *A,WDFIOTARGET *T){(void)D;(void)A;(void)T;abort();}
NTSTATUS WdfIoTargetOpen(WDFIOTARGET T,WDF_IO_TARGET_OPEN_PARAMS *P){(void)T;(void)P;abort();}
NTSTATUS WdfIoTargetSendIoctlSynchronously(WDFIOTARGET T,void *Request,unsigned Code,
 WDF_MEMORY_DESCRIPTOR *Input,WDF_MEMORY_DESCRIPTOR *Output,WDF_REQUEST_SEND_OPTIONS *Options,ULONG_PTR *Bytes){
 (void)T;(void)Request;(void)Code;(void)Input;(void)Output;(void)Options;(void)Bytes;abort();
}
void WdfIoTargetClose(WDFIOTARGET T){(void)T;abort();}void WdfObjectDelete(void *T){(void)T;abort();}
int T70AwSpbValidateProfileSerial(const void *A,size_t N,uint64_t B,uint16_t D,uint16_t Ch,uint16_t P){(void)A;(void)N;(void)B;(void)D;(void)Ch;(void)P;abort();}
T70_AW_SERIAL_APPLY_RESULT T70AwSpbApplyProfileSerial(const void *A,size_t N,uint64_t B,uint16_t D,uint16_t Ch,uint16_t P,WDFIOTARGET T){(void)A;(void)N;(void)B;(void)D;(void)Ch;(void)P;(void)T;abort();}
int T70AwProfileView(const void *A,size_t N,uint64_t B,uint16_t D,uint16_t Ch,uint16_t P,T70_AW_PROFILE_VIEW *V){
 C(A&&N==sizeof(FIXTURE)&&B==0x11e01000&&Ch<2&&D==0x34+Ch);
 if(InvalidProfile==Ch+1||P!=10)return 0;
 memset(V,0,sizeof(*V));V->Section.Data=((const FIXTURE *)A)->Bin[Ch];V->RecordCount=Records;return 1;
}
static unsigned GetMode;
NTSTATUS T70AwSpbWriteRegister(WDFIOTARGET T,USHORT Reg,USHORT Value){
 unsigned Ch=T==Owner.Target[0].Target?0:1;C(T==Owner.Target[Ch].Target&&Reg==5&&Owner.Target[Ch].ActiveUses==1);
 Calls++;if(EndFail==Calls)Irql=1;
 if(FailCall==Calls)return STATUS_IO_TIMEOUT;
 C((Value&0xfc00)==(ReadRaw&0xfc00));return 0;
}
NTSTATUS T70AwSpbReadRegister(WDFIOTARGET T,USHORT Reg,USHORT *Out){
 unsigned Ch=T==Owner.Target[0].Target?0:1;C(T==Owner.Target[Ch].Target&&(Reg==0||Reg==5)&&Owner.Target[Ch].ActiveUses==1);
 if(GetMode){C(Reg==5);Calls++;if(EndFail==Calls)Irql=1;}
 *Out=Reg==0?0x2113:ReadRaw;return ReadStatus;
}
static void Reset(void){memset(&Owner,0,sizeof Owner);Owner.Open=1;Owner.Target[0].Target=(void *)1;Owner.Target[1].Target=(void *)2;Irql=Calls=FailCall=EndFail=InvalidProfile=0;Records=1;ReadStatus=0;ReadRaw=0xac00;GetMode=0;}
static void Baselines(FIXTURE *F,unsigned A,unsigned B){
 memset(F,0,sizeof(*F));unsigned V[2]={A,B};for(unsigned I=0;I<2;I++){
  unsigned Field=V[I]<=720?(V[I]/48)*64+V[I]%48:15*64+(V[I]-720);
  F->Bin[I][64]=5;F->Bin[I][66]=(uint8_t)Field;F->Bin[I][67]=(uint8_t)(Field>>8);
 }
}
int main(void){
 FIXTURE F;T70_AW_KS_LEVELS Out,Saved;int32_t Got;
 const int32_t Levels[]={INT32_MIN,INT32_MIN+1,-5898241,-5898240,-8193,-8192,-8191,-1,0,1,INT32_MAX};
 for(unsigned Initial=0;Initial<=783;Initial++)for(unsigned K=0;K<sizeof Levels/sizeof Levels[0];K++)for(int Ch=-1;Ch<=1;Ch++){
  Reset();Baselines(&F,Initial,(Initial+17)%784);memset(&Out,0x55,sizeof Out);
  T70_AW_KS_VOLUME_RESULT R=T70AwKsVolumeSet(&Owner,&F,sizeof F,10,Ch,Levels[K],&Out);
  unsigned Mask=Ch<0?3u:1u<<Ch;C(R.Status==0&&R.CompletedMask==Mask&&R.AttemptedMask==Mask&&Out.ValidMask==Mask);
  for(unsigned I=0;I<2;I++)if(Mask&(1u<<I)){
   unsigned Base=I?(Initial+17)%784:Initial;if(Base>720)Base=720;
   int64_t Att=Levels[K]>=0?0: (-(int64_t)Levels[K]+8191)/8192;
   if(Att>720-Base)Att=720-Base;
   C(Out.Level[I]==-Att*8192&&!Owner.Target[I].ActiveUses);
  }
 }
 // Exhaustive canonical/noncanonical GET fields, upper bits ignored.
 Reset();GetMode=1;Baselines(&F,24,0);
 for(unsigned Raw=0;Raw<=65535;Raw++){
  ReadRaw=(uint16_t)Raw;Got=123;unsigned A=((Raw&1023)>>6)*48+(Raw&63);
  NTSTATUS S=T70AwKsVolumeGet(&Owner,&F,sizeof F,10,0,&Got);
  int Valid=(Raw&63)<48&&A<=720&&A>=24;
  C((S==0)==Valid);C(Got==(Valid?-(int32_t)(A-24)*8192:123));
 }
 for(unsigned Fail=1;Fail<=2;Fail++)for(unsigned End=0;End<2;End++){
  Reset();Baselines(&F,0,24);memset(&Out,0x55,sizeof Out);Saved=Out;
  if(End)EndFail=Fail;else FailCall=Fail;
  T70_AW_KS_VOLUME_RESULT R=T70AwKsVolumeSet(&Owner,&F,sizeof F,10,-1,-8192,&Out);
  C(R.Status!=0&&R.FailedChannel==Fail-1&&Calls==Fail&&!memcmp(&Out,&Saved,sizeof Out));
  C(R.AttemptedMask==(Fail==1?1u:3u));C(R.CompletedMask==(End?(Fail==1?1u:3u):(Fail==1?0u:1u)));
  C(Owner.Target[Fail-1].ActiveUses==(End?1u:0u));
 }
 for(unsigned Bad=0;Bad<9;Bad++){
  Reset();Baselines(&F,0,0);memset(&Out,0x55,sizeof Out);Saved=Out;
  int Ch=-1;const void *A=&F;size_t N=sizeof F;T70_AW_KS_LEVELS *Dest=&Out;
  switch(Bad){case 0:Ch=2;break;case 1:Ch=-2;break;case 2:InvalidProfile=2;break;case 3:Records=2;F.Bin[1][68]=5;break;case 4:F.Bin[1][64]=6;break;case 5:A=&Owner;N=sizeof Owner;break;case 6:Dest=(void *)&Owner;break;case 7:Irql=1;break;case 8:Owner.Open=0;break;}
  C(T70AwKsVolumeSet(&Owner,A,N,10,Ch,-8192,Dest).Status!=0);C(!Calls&&!memcmp(&Out,&Saved,sizeof Out));
 }
 for(unsigned Fail=0;Fail<3;Fail++){
  Reset();GetMode=1;Baselines(&F,0,0);Got=123;ReadRaw=0;
  if(Fail==0)ReadStatus=STATUS_IO_TIMEOUT;if(Fail==1)ReadStatus=(NTSTATUS)0x103;if(Fail==2)EndFail=1;
  C(T70AwKsVolumeGet(&Owner,&F,sizeof F,10,1,&Got)!=0&&Got==123&&Calls==1);
 }
 Reset();Baselines(&F,0,0);Got=123;C(T70AwKsVolumeGet(&Owner,&F,sizeof F,10,UINT32_MAX,&Got)==STATUS_INVALID_PARAMETER&&!Calls&&Got==123);
 Reset();Got=123;C(T70AwKsVolumeGet(&Owner,&Owner,sizeof Owner,10,0,&Got)==STATUS_INVALID_PARAMETER&&!Calls&&Got==123);
 printf("AW KS volume: %lu checks; synthetic profiles, actual resource/lease/profile-apply path, no hardware\n",Checks);return 0;
}
