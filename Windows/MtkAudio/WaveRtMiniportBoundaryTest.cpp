// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtMiniportBoundary.h"
#include "WaveRtPortBind.h"
#include <cassert>
#include <cstdio>
#include <type_traits>
#include <initializer_list>
static unsigned Checks,Irql,InitCalls,Creates,Stops,Positions,Releases;
static NTSTATUS InitStatus,CreateStatus,StopStatus,PositionStatus;
static bool NullResult,Nonzero,Untouched,FailedOwned;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
class Stream:public IMiniportWaveRTStream{
public:ULONG Refs=0;
 NTSTATUS QueryInterface(const GUID &,void **)override{return STATUS_NOT_SUPPORTED;}
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{C(Refs==1);Releases++;return --Refs;}
 NTSTATUS SetFormat(PKSDATAFORMAT)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS SetState(KSSTATE S)override{C(S==KSSTATE_STOP);Stops++;return StopStatus;}
 NTSTATUS GetPosition(PKSAUDIO_POSITION P)override{C(Stops==1);Positions++;if(!Untouched)*P={Nonzero?16u:0u,0};return PositionStatus;}
 NTSTATUS AllocateAudioBuffer(ULONG,PMDL *,ULONG *,ULONG *,MEMORY_CACHING_TYPE *)override{return STATUS_NOT_SUPPORTED;}
 void FreeAudioBuffer(PMDL,ULONG)override{assert(false);}void GetHWLatency(KSRTAUDIO_HWLATENCY *)override{assert(false);}
 NTSTATUS GetPositionRegister(KSRTAUDIO_HWREGISTER *)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS GetClockRegister(KSRTAUDIO_HWREGISTER *)override{return STATUS_NOT_SUPPORTED;}
};
static Stream S;
static unsigned Token;static PUNKNOWN Adapter;static PRESOURCELIST Resources;static PPORTWAVERT Port;
static PPORTWAVERTSTREAM PortStream;static PKSDATAFORMAT Format;
static NTSTATUS Initialize(void *Ctx,PUNKNOWN A,PRESOURCELIST R,PPORTWAVERT P){
 C(Ctx==&Token&&A==Adapter&&R==Resources&&P==Port);InitCalls++;return InitStatus;
}
static NTSTATUS Create(void *Ctx,PPORTWAVERTSTREAM P,ULONG Pin,PKSDATAFORMAT F,PMINIPORTWAVERTSTREAM *O){
 C(Ctx==&Token&&P==PortStream&&Pin==7&&F==Format&&InitCalls==1);Creates++;
 if(!NullResult&&(NT_SUCCESS(CreateStatus)||FailedOwned)){S.Refs=1;*O=&S;}return CreateStatus;
}
class Mini:public T70WaveRtMiniportBoundary{
public:Mini(const T70_WAVERT_MINIPORT_PROVIDER &P):T70WaveRtMiniportBoundary(7,P){}
 NTSTATUS QueryInterface(const GUID &,void **)override{return STATUS_NOT_SUPPORTED;}
 ULONG AddRef()override{return 1;}ULONG Release()override{return 1;}
 NTSTATUS GetDescription(PPCFILTER_DESCRIPTOR *)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,ULONG *)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS GetDeviceDescription(PDEVICE_DESCRIPTION)override{return STATUS_NOT_SUPPORTED;}
};
class SystemPort:public IPortWaveRT{
public:ULONG Refs=0;
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{C(Refs==1);return --Refs;}
 NTSTATUS Init(void *,void *,PUNKNOWN M,PUNKNOWN A,PRESOURCELIST R)override{
  return static_cast<IMiniportWaveRT *>(M)->Init(A,R,this);
 }
};
static SystemPort BoundPort;
NTSTATUS PcNewPort(PPORT *O,const GUID &){BoundPort.Refs=1;*O=&BoundPort;return 0;}
static_assert(std::is_abstract<T70WaveRtMiniportBoundary>::value,"real metadata and COM owner missing");
int main(){
 unsigned char Objects[5]={};Adapter=(PUNKNOWN)&Objects[0];Resources=(PRESOURCELIST)&Objects[1];Port=(PPORTWAVERT)&Objects[2];PortStream=(PPORTWAVERTSTREAM)&Objects[3];Format=(PKSDATAFORMAT)&Objects[4];
 for(unsigned T=0;T<10;T++){
  InitCalls=Creates=Stops=Positions=Releases=0;Irql=0;
  InitStatus=T==1?STATUS_NOT_SUPPORTED:0;CreateStatus=(T==2||T==3)?STATUS_INSUFFICIENT_RESOURCES:0;
  StopStatus=T==5?STATUS_DEVICE_NOT_READY:0;PositionStatus=T==6?STATUS_DEVICE_NOT_READY:0;
  FailedOwned=T==3;NullResult=T==4;Nonzero=T==7;Untouched=T==8;
  T70_WAVERT_MINIPORT_PROVIDER P={&Token,T==0?nullptr:Initialize,Create};Mini M(P);IMiniportWaveRT *I=&M;
  PMINIPORTWAVERTSTREAM Out=(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55;
  C(I->NewStream(&Out,PortStream,7,0,Format)==STATUS_DEVICE_NOT_READY);C(!Creates);
  NTSTATUS R=I->Init(Adapter,Resources,Port);
  if(T<2){if(T==1){C(I->Init(Adapter,Resources,Port)==STATUS_INVALID_DEVICE_STATE);C(InitCalls==1);}C(!NT_SUCCESS(R));C(I->NewStream(&Out,PortStream,7,0,Format)==STATUS_DEVICE_NOT_READY);C(!Creates);}
  else{
   C(R==0);C(I->Init(Adapter,Resources,Port)==STATUS_INVALID_DEVICE_STATE);
   C(I->NewStream(&Out,PortStream,6,0,Format)==STATUS_NOT_SUPPORTED);
   C(I->NewStream(&Out,PortStream,7,1,Format)==STATUS_NOT_SUPPORTED);C(!Creates);
   R=I->NewStream(&Out,PortStream,7,0,Format);
   if(T==9){C(R==0&&Out==&S&&S.Refs==1&&!Releases);Out->Release();}
   else C(!NT_SUCCESS(R)&&Out==(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55);
   C(Creates==1&&Releases==((T==2||T==4)?0u:1u));
   C(Stops==(T>=5?1u:0u)&&Positions==(T>=6?1u:0u));
  }
  C(!S.Refs);
  Irql=DISPATCH_LEVEL;C(I->Init(Adapter,Resources,Port)==STATUS_INVALID_PARAMETER);
  C(I->NewStream(&Out,PortStream,7,0,Format)==STATUS_INVALID_PARAMETER);Irql=0;
  C(I->NewStream(nullptr,PortStream,7,0,Format)==STATUS_INVALID_PARAMETER);
  C(I->NewStream(&Out,nullptr,7,0,Format)==STATUS_INVALID_PARAMETER);
  C(I->NewStream(&Out,PortStream,7,0,nullptr)==STATUS_INVALID_PARAMETER);
 }
 for(NTSTATUS Pending:{(NTSTATUS)0x103,(NTSTATUS)1,(NTSTATUS)0x40000000})for(unsigned Stage=0;Stage<4;Stage++){
  InitCalls=Creates=Stops=Positions=Releases=0;InitStatus=CreateStatus=StopStatus=PositionStatus=0;
  NullResult=Nonzero=Untouched=FailedOwned=false;
  if(Stage==0)InitStatus=Pending;if(Stage==1)CreateStatus=Pending;
  if(Stage==2)StopStatus=Pending;if(Stage==3)PositionStatus=Pending;
  T70_WAVERT_MINIPORT_PROVIDER P={&Token,Initialize,Create};Mini M(P);
  PMINIPORTWAVERTSTREAM Out=(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55;
  NTSTATUS R=M.Init(Adapter,Resources,Port);
  if(Stage==0){C(R==STATUS_DEVICE_NOT_READY);C(M.NewStream(&Out,PortStream,7,0,Format)==STATUS_DEVICE_NOT_READY&&!Creates);}
  else{
   C(R==0);C(M.NewStream(&Out,PortStream,7,0,Format)==STATUS_DEVICE_NOT_READY);
   C(Releases==1&&Stops==(Stage>=2?1u:0u)&&Positions==(Stage==3?1u:0u));
  }
  C(Out==(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55&&!S.Refs);
  C(M.Init(Adapter,Resources,Port)==STATUS_INVALID_DEVICE_STATE);
 }
 T70_WAVERT_MINIPORT_PROVIDER Missing={&Token,Initialize,nullptr};Mini NoFactory(Missing);
 InitCalls=0;C(NoFactory.Init(Adapter,Resources,Port)==STATUS_DEVICE_NOT_READY);C(!InitCalls);
 static_assert(!std::is_copy_constructible<Mini>::value,"no initialized-state copying");
 static_assert(!std::is_copy_assignable<Mini>::value,"no initialized-state assignment");
 // Exercise the actual port-binding function into the actual miniport Init
 // and NewStream methods. The system port/factory remain explicit host fakes.
 InitCalls=Creates=Stops=Positions=Releases=0;InitStatus=CreateStatus=StopStatus=PositionStatus=0;
 NullResult=Nonzero=Untouched=FailedOwned=false;Port=&BoundPort;
 T70_WAVERT_MINIPORT_PROVIDER P={&Token,Initialize,Create};Mini M(P);PPORT Bound=nullptr;
 C(T70WaveRtPortBind((PDEVICE_OBJECT)&Objects[0],(PIRP)&Objects[1],&M,Adapter,Resources,&Bound)==0);
 C(Bound==&BoundPort&&InitCalls==1&&BoundPort.Refs==1);
 PMINIPORTWAVERTSTREAM Created=nullptr;
 C(M.NewStream(&Created,PortStream,7,0,Format)==0&&Created==&S);
 Created->Release();Bound->Release();C(!S.Refs&&!BoundPort.Refs);
 printf("WaveRT miniport boundary: %u checks; fake factory only\n",Checks);
}
