// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicPortOwner.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned Alloc,Freed,Irql,Calls,Confirms;static bool FailAlloc,NullPort;
static NTSTATUS NewStatus,InitStatus,ProviderStatus,ConfirmStatus;
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG E){__atomic_compare_exchange_n(P,&E,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return E;}
LONG InterlockedIncrement(volatile LONG *P){return ++*P;}
LONG InterlockedDecrement(volatile LONG *P){return --*P;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(FailAlloc)return nullptr;++Alloc;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Freed;free(P);}
struct Owner:IResourceList {ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{assert(Refs);return --Refs;}};
static unsigned Registrations,Withdrawals;static NTSTATUS RegisterStatus,WithdrawStatus,QiStatus;static bool NullQi;
static PDEVICE_OBJECT Device;static PIRP Request;static Owner Parent,Resources,Adapter;
struct Unregister:IUnregisterSubdevice {
 ULONG Refs=0;
 ULONG AddRef()override{return ++Refs;}
 ULONG Release()override{assert(Refs);return --Refs;}
 NTSTATUS UnregisterSubdevice(PDEVICE_OBJECT D,PUNKNOWN P)override;
};static Unregister Removal;
struct Port:IPortWaveCyclic {
 ULONG Refs=0;T70WaveCyclicMiniport *Mini=nullptr;
 NTSTATUS QueryInterface(const GUID &G,void **Out)override{
  assert(G.Data1==0x16738177);if(QiStatus)return QiStatus;
  if(!NullQi){Removal.AddRef();*Out=&Removal;}return 0;
 }
 ULONG AddRef()override{return ++Refs;}
 ULONG Release()override{assert(Refs);if(--Refs==0&&Mini){auto M=Mini;Mini=nullptr;M->Release();}return Refs;}
 NTSTATUS Init(void *D,void *I,PUNKNOWN M,PUNKNOWN A,PRESOURCELIST R)override{
  ++Calls;assert(D==Device&&I==Request&&A==&Adapter&&R==&Resources);
  Mini=static_cast<T70WaveCyclicMiniport *>(M);Mini->AddRef();
  NTSTATUS S=Mini->Init(A,R,this);return S==0?InitStatus:S;
 }
 NTSTATUS NewMasterDmaChannel(IDmaChannel **,PUNKNOWN,PRESOURCELIST,ULONG,BOOLEAN,BOOLEAN,DMA_WIDTH,DMA_SPEED)override{assert(false);return -1;}
};static Port Fake;
NTSTATUS Unregister::UnregisterSubdevice(PDEVICE_OBJECT D,PUNKNOWN P){
 assert(D==Device&&P==&Fake&&Refs==1);++Withdrawals;return WithdrawStatus;
}
NTSTATUS PcRegisterSubdevice(PDEVICE_OBJECT D,WCHAR *Name,PUNKNOWN P){
 assert(D==Device&&P==&Fake&&Removal.Refs==1);
 const WCHAR Expected[]={'T','7','0','W','a','v','e',0};
 assert(!memcmp(Name,Expected,sizeof Expected));++Registrations;return RegisterStatus;
}
NTSTATUS PcNewPort(PPORT *Out,const GUID &Id){
 const GUID G={0xb4c90a2a,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};assert(!memcmp(&G,&Id,sizeof G));
 if(NewStatus)return NewStatus;if(!NullPort){Fake.Refs=1;*Out=&Fake;}return 0;
}
static NTSTATUS Initialize(void *,PUNKNOWN A,PRESOURCELIST R,IPortWaveCyclic *P){assert(A==&Adapter&&R==&Resources&&P==&Fake);return ProviderStatus;}
static NTSTATUS Factory(void *,IPortWaveCyclic *,PKSDATAFORMAT,PMINIPORTWAVECYCLICSTREAM *,PDMACHANNEL *,PSERVICEGROUP *){assert(false);return -1;}
static NTSTATUS Confirm(void *){++Confirms;return ConfirmStatus;}
int main(){
 unsigned char D,I;Device=(PDEVICE_OBJECT)&D;Request=(PIRP)&I;
 T70_WAVECYCLIC_MINIPORT_PROVIDER P={nullptr,Initialize,Factory};
 for(unsigned T=0;T<9;++T){
  T70_WAVECYCLIC_PORT_OWNER O={};NewStatus=InitStatus=ProviderStatus=ConfirmStatus=0;Calls=Confirms=0;NullPort=false;FailAlloc=false;
  if(T==0)FailAlloc=true;if(T==1)NewStatus=STATUS_NOT_SUPPORTED;if(T==2)NewStatus=0x103;
  if(T==3)NullPort=true;if(T==4)ProviderStatus=STATUS_NOT_SUPPORTED;
  if(T==5)InitStatus=0x103;if(T==6)InitStatus=STATUS_NOT_SUPPORTED;
  if(T==7){InitStatus=STATUS_NOT_SUPPORTED;ConfirmStatus=0x103;}
  NTSTATUS S=T70WaveCyclicPortOpen(&O,Device,Request,&Parent,&Adapter,&Resources,P,Confirm,nullptr);
  if(T<8)assert(S!=0);else assert(S==0&&O.Initialized&&O.Port&&O.Miniport);
  assert(O.InitStatus==S);
  if(T==8){ConfirmStatus=STATUS_DEVICE_NOT_READY;
   assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)!=0&&O.Port&&O.Miniport&&O.InitStatus==0);
   ConfirmStatus=0;}
  if(T==7){assert(O.Port&&O.Miniport&&O.CleanupStatus==STATUS_DEVICE_NOT_READY&&O.InitStatus==InitStatus);ConfirmStatus=STATUS_NOT_SUPPORTED;
   assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)!=0&&O.Port);ConfirmStatus=0;}
  assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)==0&&!O.Port&&!O.Miniport);
  assert(!Fake.Refs&&!Fake.Mini&&Parent.Refs==1&&Resources.Refs==1&&Adapter.Refs==1&&Alloc==Freed);
  auto C=Confirms;assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)==0&&Confirms==C);
  assert(T70WaveCyclicPortOpen(&O,Device,Request,&Parent,&Adapter,&Resources,P,Confirm,nullptr)==STATUS_INVALID_DEVICE_STATE);
 }
 // Publication failures cannot become a registered owner or leak QI refs.
 for(unsigned Case=0;Case<7;Case++){
  T70_WAVECYCLIC_PORT_OWNER O={};
  NewStatus=InitStatus=ProviderStatus=ConfirmStatus=RegisterStatus=WithdrawStatus=QiStatus=0;
  NullQi=false;Registrations=Withdrawals=0;
  assert(T70WaveCyclicPortOpen(&O,Device,Request,&Parent,&Adapter,&Resources,P,Confirm,nullptr)==0);
  assert(T70WaveCyclicPortPublish(&O,(PDEVICE_OBJECT)(uintptr_t)1)==STATUS_INVALID_DEVICE_STATE&&!Registrations&&!Removal.Refs);
  if(Case==0)QiStatus=STATUS_NOT_SUPPORTED;if(Case==1)QiStatus=0x103;
  if(Case==2)NullQi=true;if(Case==3)RegisterStatus=STATUS_NOT_SUPPORTED;
  if(Case==4)RegisterStatus=0x103;
  auto S=T70WaveCyclicPortPublish(&O,Device);
  if(Case<5){assert(S!=0&&!O.Registered&&!O.Unregister&&!Removal.Refs);assert(Registrations==(Case>=3?1u:0u));}
  else{
   assert(S==0&&O.Registered&&O.Unregister&&Removal.Refs==1);
   assert(T70WaveCyclicPortPublish(&O,Device)==STATUS_INVALID_DEVICE_STATE&&Registrations==1);
   auto Count=Confirms;
   WithdrawStatus=Case==5?STATUS_NOT_SUPPORTED:0x103;
   assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)!=0&&O.Registered&&O.Port&&O.Miniport&&Removal.Refs==1&&Confirms==Count);
   WithdrawStatus=0;ConfirmStatus=STATUS_DEVICE_NOT_READY;
   assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)!=0&&!O.Registered&&O.Port&&Removal.Refs==1);
   assert(Withdrawals==2); // link removal did not prove stream/IRQ rundown
   ConfirmStatus=0;
  }
  assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)==0&&!Removal.Refs&&!Fake.Refs&&Alloc==Freed);
  assert(T70WaveCyclicPortPublish(&O,Device)==STATUS_INVALID_DEVICE_STATE);
  assert(T70WaveCyclicPortClose(&O,Confirm,nullptr)==0);
 }
 T70_WAVECYCLIC_PORT_OWNER O={};Irql=DISPATCH_LEVEL;
 assert(T70WaveCyclicPortOpen(&O,Device,Request,&Parent,&Adapter,&Resources,P,Confirm,nullptr)==STATUS_INVALID_PARAMETER&&!O.Attempted);
 Irql=0;assert(T70WaveCyclicPortOpen(&O,nullptr,Request,&Parent,&Adapter,&Resources,P,Confirm,nullptr)==STATUS_INVALID_PARAMETER);
 puts("WaveCyclic real miniport/port binding: startup failure, retained cleanup, reference-cycle tests passed");
}
