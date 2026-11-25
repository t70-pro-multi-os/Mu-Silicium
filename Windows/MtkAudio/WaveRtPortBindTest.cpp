// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtPortBind.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static unsigned Checks,Irql,Creates,Inits,Releases;
static NTSTATUS NewStatus,InitStatus;static bool NullPort;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
static PDEVICE_OBJECT Dev;static PIRP Request;static PUNKNOWN Mini,Adapter;static PRESOURCELIST Res;
class Unknown:public IResourceList{public:ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}};
class Port:public IPort{
public:ULONG Refs=0;
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{C(Refs==1);Releases++;return --Refs;}
 NTSTATUS Init(void *D,void *I,PUNKNOWN M,PUNKNOWN A,PRESOURCELIST R)override{
  Inits++;C(Creates==1&&Refs==1&&D==Dev&&I==Request&&M==Mini&&A==Adapter&&R==Res);
  return InitStatus;
 }
};
static Port Fake;
NTSTATUS PcNewPort(PPORT *Out,const GUID &G){
 const GUID Expected={0xcc9be57a,0xeb9e,0x42b4,{0x94,0xfc,0x0c,0xad,0x3d,0xbc,0xe7,0xfa}};
 C(!memcmp(&G,&Expected,sizeof G));Creates++;
 if(!NT_SUCCESS(NewStatus))return NewStatus;
 if(!NullPort){Fake.Refs=1;*Out=&Fake;}return STATUS_SUCCESS;
}
int main(){
 Unknown M,A,R;unsigned char D=0,I=0;Dev=(PDEVICE_OBJECT)&D;Request=(PIRP)&I;Mini=&M;Res=&R;
 for(unsigned T=0;T<7;T++)for(unsigned Optional=0;Optional<2;Optional++){
  Creates=Inits=Releases=0;Adapter=Optional?&A:nullptr;Irql=0;NullPort=T==2;
  NewStatus=T==0?STATUS_INSUFFICIENT_RESOURCES:0;InitStatus=T==1?STATUS_NOT_SUPPORTED:0;
  PPORT Out=(PPORT)(uintptr_t)0x55;
  if(T==4)Irql=DISPATCH_LEVEL;
  NTSTATUS S=T70WaveRtPortBind(T==5?nullptr:Dev,T==6?nullptr:Request,Mini,Adapter,Res,T==3?nullptr:&Out);
  if(T==0||T==1||T==2||T>=3){
   C(!NT_SUCCESS(S)&&Out==(PPORT)(uintptr_t)0x55);
   C(Creates==(T<3?1u:0u)&&Inits==(T==1?1u:0u)&&Releases==(T==1?1u:0u));
   if(T==0)C(S==NewStatus);if(T==1)C(S==InitStatus);
  }
  C(M.Refs==1&&A.Refs==1&&R.Refs==1);
 }
 Irql=0;NullPort=false;NewStatus=InitStatus=0;
 for(unsigned Optional=0;Optional<2;Optional++){
  Creates=Inits=Releases=0;Adapter=Optional?&A:nullptr;PPORT Out=nullptr;
  C(T70WaveRtPortBind(Dev,Request,Mini,Adapter,Res,&Out)==0);
  C(Out==&Fake&&Fake.Refs==1&&Creates==1&&Inits==1&&!Releases);
  Out->Release();C(!Fake.Refs&&Releases==1);
 }
 PPORT Out=nullptr;Creates=0;
 C(T70WaveRtPortBind(Dev,Request,nullptr,Adapter,Res,&Out)==STATUS_INVALID_PARAMETER);
 C(T70WaveRtPortBind(Dev,Request,Mini,Adapter,nullptr,&Out)==STATUS_INVALID_PARAMETER);C(!Creates);
 printf("WaveRT port binding: %u checks; fake explicit startup objects only\n",Checks);
}
