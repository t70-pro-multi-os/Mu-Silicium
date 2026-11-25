// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioEndpoint.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static unsigned Checks,Irql,Alloc,Freed,FailAt,Step;static NTSTATUS Failure;
static bool NullResult;static std::vector<unsigned> Events;
#define C(X) do{++Checks;assert(X);}while(0)
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG E){__atomic_compare_exchange_n(P,&E,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return E;}
LONG InterlockedIncrement(volatile LONG *P){return ++*P;}
LONG InterlockedDecrement(volatile LONG *P){return --*P;}
static NTSTATUS Next(){return ++Step==FailAt?Failure:STATUS_SUCCESS;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(Next())return nullptr;++Alloc;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Freed;free(P);}
struct Ref:IResourceList {ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{C(Refs>0);return --Refs;}};
static Ref Parent,Resources;static _DEVICE_OBJECT Device{},Other{};static _IRP Irp{};
static NTSTATUS UnlinkStatus,WaveStatus,UnregisterStatus;
struct Port:IPortTopology {
 ULONG Refs=0;PUNKNOWN Mini=nullptr;
 ULONG AddRef()override{return ++Refs;}
 ULONG Release()override{C(Refs>0);if(!--Refs&&Mini){auto M=Mini;Mini=nullptr;Events.push_back(5);M->Release();}return Refs;}
 NTSTATUS Init(void *D,void *I,PUNKNOWN M,PUNKNOWN A,PRESOURCELIST R)override{
  C(D==&Device&&I==&Irp&&A==&Parent&&R==&Resources);M->AddRef();Mini=M;
  auto S=static_cast<T70TopologyMiniport *>(M)->Init(A,R,this);C(S==0);return Next();
 }
 NTSTATUS QueryInterface(const GUID &,void **)override;
};static Port Topo,WavePort;
struct Unregister:IUnregisterSubdevice {
 ULONG Refs=0;ULONG AddRef()override{Topo.AddRef();return ++Refs;}
 ULONG Release()override{C(Refs>0);--Refs;Topo.Release();return Refs;}
 NTSTATUS UnregisterSubdevice(PDEVICE_OBJECT D,PUNKNOWN P)override{C(D==&Device&&P==&Topo);Events.push_back(3);return UnregisterStatus;}
};static Unregister U;
struct Unlink:IUnregisterPhysicalConnection {
 ULONG Refs=0;ULONG AddRef()override{Topo.AddRef();return ++Refs;}
 ULONG Release()override{C(Refs>0);--Refs;Topo.Release();return Refs;}
 NTSTATUS UnregisterPhysicalConnection(PDEVICE_OBJECT D,PUNKNOWN F,ULONG FP,PUNKNOWN T,ULONG TP)override{
  C(D==&Device&&F==&WavePort&&FP==1&&T==&Topo&&TP==0);Events.push_back(1);return UnlinkStatus;
 }
};static Unlink L;
NTSTATUS Port::QueryInterface(const GUID &G,void **Out){
 auto S=Next();if(S)return S;if(NullResult&&Step==FailAt)return 0;
 if(G.Data1==0x16738177){U.AddRef();*Out=&U;}else{C(G.Data1==0x6c38e231);L.AddRef();*Out=&L;}return 0;
}
NTSTATUS PcNewPort(PPORT *Out,const GUID &G){C(G.Data1==0xb4c90a32);auto S=Next();if(S)return S;if(NullResult&&Step==FailAt)return 0;Topo.AddRef();*Out=&Topo;return 0;}
NTSTATUS PcRegisterSubdevice(PDEVICE_OBJECT D,WCHAR *N,PUNKNOWN P){
 C(D==&Device&&P==&Topo&&U.Refs&&L.Refs);const WCHAR Name[]={'T','7','0','T','o','p','o','l','o','g','y',0};C(!memcmp(N,Name,sizeof Name));return Next();
}
NTSTATUS T70WaveCyclicPortPublish(T70_WAVECYCLIC_PORT_OWNER *W,PDEVICE_OBJECT D){C(D==&Device&&W->Port==&WavePort);auto S=Next();if(!S)W->Registered=true;return S;}
NTSTATUS T70WaveCyclicPortWithdraw(T70_WAVECYCLIC_PORT_OWNER *W){Events.push_back(2);W->PublicationClosed=true;if(WaveStatus)return NT_SUCCESS(WaveStatus)?STATUS_DEVICE_NOT_READY:WaveStatus;W->Registered=false;return 0;}
NTSTATUS PcRegisterPhysicalConnection(PDEVICE_OBJECT D,PUNKNOWN F,ULONG FP,PUNKNOWN T,ULONG TP){C(D==&Device&&F==&WavePort&&FP==1&&T==&Topo&&TP==0);return Next();}
static void Reset(){C(!Topo.Refs&&!Topo.Mini&&!U.Refs&&!L.Refs&&Parent.Refs==1&&Alloc==Freed);Step=FailAt=0;Failure=0;NullResult=false;UnlinkStatus=WaveStatus=UnregisterStatus=0;Events.clear();}
static T70_WAVECYCLIC_PORT_OWNER Wave(){T70_WAVECYCLIC_PORT_OWNER W={};W.Port=&WavePort;W.Initialized=true;W.OpenDevice=&Device;return W;}
static NTSTATUS Open(T70_AUDIO_ENDPOINT &O,T70_WAVECYCLIC_PORT_OWNER &W){return T70AudioEndpointOpen(&O,&Device,&Irp,&Parent,&Resources,&W);}
int main(){
 // Eight creation stages: allocation, Port, Init, two QIs, topology registration,
 // wave publication and physical connection. Mock failures do not grant refs.
 for(unsigned Stage=1;Stage<=8;++Stage)for(NTSTATUS S:{STATUS_NOT_SUPPORTED,(NTSTATUS)0x103}){
  Reset();auto W=Wave();T70_AUDIO_ENDPOINT O={};FailAt=Stage;Failure=S;
  C(Open(O,W)!=0&&Step==Stage);C(!O.Connected);
  FailAt=0;Events.clear();C(T70AudioEndpointClose(&O)==0);C(!O.Port&&!O.Miniport&&!O.Unregister&&!O.Unlink&&!W.Registered);
  C(T70AudioEndpointClose(&O)==0);C(Open(O,W)==STATUS_INVALID_DEVICE_STATE);
 }
 for(unsigned Stage:{2u,4u,5u}){Reset();auto W=Wave();T70_AUDIO_ENDPOINT O={};FailAt=Stage;NullResult=true;C(Open(O,W)==STATUS_DEVICE_DATA_ERROR);C(T70AudioEndpointClose(&O)==0);}
 for(unsigned Stage=1;Stage<=3;++Stage)for(NTSTATUS S:{STATUS_NOT_SUPPORTED,(NTSTATUS)0x103}){
  Reset();auto W=Wave();T70_AUDIO_ENDPOINT O={};C(Open(O,W)==0&&O.Connected&&O.Registered&&W.Registered);
  auto P=O.Port;auto M=O.Miniport;auto Refs=Topo.Refs;auto Count=Freed;
  if(Stage==1)UnlinkStatus=S;if(Stage==2)WaveStatus=S;if(Stage==3)UnregisterStatus=S;
  C(T70AudioEndpointClose(&O)==(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S));
  C(O.Port==P&&O.Miniport==M&&Topo.Refs==Refs&&Freed==Count&&U.Refs&&L.Refs);
  C(O.Connected==(Stage==1)&&O.Registered&&W.Registered==(Stage<=2));
  C(Events.size()==Stage);for(unsigned I=0;I<Stage;++I)C(Events[I]==I+1);
  UnlinkStatus=WaveStatus=UnregisterStatus=0;Events.clear();
  C(T70AudioEndpointClose(&O)==0&&!W.Registered);C(!Events.empty()&&Events.back()==5);
  C(!O.Port&&!O.Miniport&&!O.Unregister&&!O.Unlink);C(T70AudioEndpointClose(&O)==0);
 }
 Reset();auto W=Wave();T70_AUDIO_ENDPOINT O={};
 W.OpenDevice=&Other;C(Open(O,W)==STATUS_INVALID_DEVICE_STATE&&!O.Attempted&&!Step);W.OpenDevice=&Device;
 Irql=DISPATCH_LEVEL;C(Open(O,W)==STATUS_INVALID_PARAMETER&&!Step);C(T70AudioEndpointClose(&O)==STATUS_INVALID_PARAMETER);Irql=0;
 C(T70AudioEndpointOpen(nullptr,&Device,&Irp,&Parent,&Resources,&W)==STATUS_INVALID_PARAMETER);
 C(Open(O,W)==0);Events.clear();C(T70AudioEndpointClose(&O)==0);C((Events==std::vector<unsigned>{1,2,3,5}));Reset();
 // Unregister/owner Close does not revoke existing client COM references.
 {auto W2=Wave();T70_AUDIO_ENDPOINT O2={};C(Open(O2,W2)==0);Topo.AddRef();auto Count=Freed;
  C(T70AudioEndpointClose(&O2)==0);C(Topo.Refs==1&&Topo.Mini&&Parent.Refs==2&&Freed==Count);
  Topo.Release();C(Parent.Refs==1&&Freed==Count+1);Reset();}
 printf("Audio endpoint: %u checks; real topology miniport, fake PortCls publication/withdrawal\n",Checks);
}
