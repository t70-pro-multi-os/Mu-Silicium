// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual topology dispatch and KS conversion, synthetic profile/resource I/O.
#include "T70TopologyVolume.h"
#include "AudioEndpoint.h"
extern "C" {
#include "AwVolume.h"
}
#include <atomic>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned Checks,Irql,Alloc,Freed,Calls,LastChannel,FailCall;
static bool FailAlloc,Reenter;
static T70TopologyMiniport *Active;
#define C(X) do{++Checks;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);abort();}}while(0)
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG X){__atomic_compare_exchange_n(P,&X,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return X;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(FailAlloc)return nullptr;Alloc++;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){Freed++;free(P);}
struct Parent:IUnknown {volatile LONG Refs=1;ULONG AddRef()override{return InterlockedIncrement(&Refs);}ULONG Release()override{return InterlockedDecrement(&Refs);}};
struct Port:IPortTopology {ULONG AddRef()override{return 2;}ULONG Release()override{return 1;}NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)override{abort();}};
struct Fixture {uint8_t Bin[2][68];};
static uint16_t Raw[2];
extern "C" int T70AwProfileView(const void *A,size_t N,uint64_t B,uint16_t D,uint16_t Ch,uint16_t P,T70_AW_PROFILE_VIEW *V){
 if(!A||N!=sizeof(Fixture)||B!=0x11e01000||Ch>1||D!=0x34+Ch||P!=10)return 0;
 memset(V,0,sizeof(*V));V->Section.Data=((const Fixture *)A)->Bin[Ch];V->RecordCount=1;return 1;
}
extern "C" T70_AW_SPB_VOLUME_RESULT T70AwSpbResourcesApplyVolume(T70_AW_SPB_RESOURCES *O,const void *A,size_t N,uint16_t P,uint16_t Ch,uint32_t Att,uint16_t *Out){
 C(O&&O->Open&&Ch<2);Calls++;LastChannel=Ch;
 if(Reenter){Reenter=false;C(Active->Close()==STATUS_DEVICE_NOT_READY);}
 T70_AW_SPB_VOLUME_RESULT R={0,0,{T70AwProfileVolumeApplied,0,1}};
 if(Calls==FailCall){R.Status=STATUS_IO_TIMEOUT;R.Apply.Code=T70AwProfileVolumeWriteFailed;R.Apply.TransportStatus=STATUS_IO_TIMEOUT;return R;}
 T70_AW_PROFILE_VIEW V;C(T70AwProfileView(A,N,0x11e01000,(uint16_t)(0x34+Ch),Ch,P,&V));
 const uint8_t *F=V.Section.Data+66;uint16_t B=(uint16_t)(F[0]|((uint16_t)F[1]<<8));
 Raw[Ch]=T70Aw88261ComposeVolume(Raw[Ch],T70Aw88261DecodeInitialVolume(B),Att);*Out=Raw[Ch];return R;
}
extern "C" NTSTATUS T70AwSpbResourcesReadVolume(T70_AW_SPB_RESOURCES *O,uint16_t Ch,uint16_t *Out){
 C(O&&O->Open&&Ch<2);Calls++;LastChannel=Ch;if(Calls==FailCall)return STATUS_IO_TIMEOUT;*Out=Raw[Ch];return 0;
}
static unsigned EndpointCalls;
NTSTATUS T70AudioEndpointOpenUsing(T70_AUDIO_ENDPOINT *O,PDEVICE_OBJECT D,PIRP I,PUNKNOWN P,
 PRESOURCELIST R,T70_WAVECYCLIC_PORT_OWNER *W,T70_TOPOLOGY_CREATOR Create,const void *B){
 C(O&&D==(void *)1&&I==(void *)2&&R==(void *)3&&W==(void *)4&&Create&&B);
 EndpointCalls++;return Create(P,B,&O->Miniport); // Creator forwarding only, no registration simulation.
}
static ULONG Word(const uint8_t *P){ULONG V;memcpy(&V,P,4);return V;}
int main(){
 Parent P;Port Q;Fixture F={};
 // Different ranges ensure Windows channel permutation is observable.
 F.Bin[0][64]=F.Bin[1][64]=5;F.Bin[0][66]=24;F.Bin[1][66]=64; // initial24/48
 T70_AW_SPB_RESOURCES O={};O.Open=1;O.Target[0].Target=(void *)1;O.Target[1].Target=(void *)2;
 T70_TOPOLOGY_VOLUME_BINDING B={&O,&F,sizeof F,10,{1,0}};
 T70TopologyMiniport *M=nullptr;C(T70TopologyMiniport::CreateWithVolume(&P,&B,&M)==0&&M&&P.Refs==2);Active=M;
 C(M->Init(&P,nullptr,&Q)==0);
 PPCFILTER_DESCRIPTOR D=nullptr;C(M->GetDescription(&D)==0&&D->NodeCount==1&&D->ConnectionCount==2);
 auto Nodes=(const PCNODE_DESCRIPTOR *)D->Nodes;
 auto Automation=(const PCAUTOMATION_TABLE *)Nodes[0].AutomationTable;
 C(Automation&&Automation->PropertyCount==1);
 const PCPROPERTY_ITEM *Item=Automation->Properties;C(Item->Id==4&&Item->Handler==T70TopologyMiniport::VolumeProperty);
 LONG Instance[2]={0,0x1234};alignas(8) uint8_t Value[128];
 PCPROPERTY_REQUEST R={};R.MajorTarget=static_cast<IMiniportTopology *>(M);R.Node=0;R.PropertyItem=Item;
 R.Verb=0x200;R.InstanceSize=sizeof Instance;R.Instance=Instance;
 for(unsigned Size=1;Size<88;Size++){
  memset(Value,0xa5,sizeof Value);R.Value=Value;R.ValueSize=Size;
  NTSTATUS S=Item->Handler(&R);
  if(Size==4){C(S==0&&Word(Value)==0x203);}
  else if(Size==40){C(S==0&&Word(Value+4)==88);}
  else{C(S==STATUS_BUFFER_TOO_SMALL);C(Value[0]==0xa5);}
  C(!Calls);
 }
 R.ValueSize=88;C(Item->Handler(&R)==0&&R.ValueSize==88);
 C(Word(Value)==0x203&&Word(Value+4)==88&&Word(Value+32)==1);
 C(Word(Value+40)==2&&Word(Value+44)==16&&Word(Value+48)==2&&Word(Value+52)==2);
 C(Word(Value+56)==8192&&Word(Value+60)==0&&(LONG)Word(Value+64)==-(720-48)*8192&&Word(Value+68)==0);
 C(Word(Value+72)==8192&&Word(Value+76)==0&&(LONG)Word(Value+80)==-(720-24)*8192&&Word(Value+84)==0);
 LONG Level=-8192;R.Verb=2;R.Value=&Level;R.ValueSize=4;Instance[0]=0;
 C(Item->Handler(&R)==0&&Calls==1&&LastChannel==1);
 R.Verb=1;Level=123;R.ValueSize=4;C(Item->Handler(&R)==0&&LastChannel==1&&Level==-8192);
 R.Verb=2;Instance[0]=-1;Level=-16384;R.ValueSize=4;unsigned Before=Calls;
 C(Item->Handler(&R)==0&&Calls==Before+2);
 // GET does not invent an aggregate for potentially unequal channels.
 R.Verb=1;Level=123;R.ValueSize=4;Before=Calls;
 C(Item->Handler(&R)!=0&&Calls==Before&&Level==123);
 R.Verb=2;R.ValueSize=4;FailCall=Calls+2;Before=Calls;C(Item->Handler(&R)==STATUS_IO_TIMEOUT&&Calls==Before+2);FailCall=0;
 for(unsigned Bad=0;Bad<5;Bad++){
  auto T=R;T.Verb=1;T.ValueSize=4;Instance[0]=0;Before=Calls;
  if(Bad==0)T.Node=1;if(Bad==1)T.InstanceSize=3;if(Bad==2)T.Instance=nullptr;if(Bad==3)T.Verb=3;if(Bad==4)Irql=DISPATCH_LEVEL;
  C(Item->Handler(&T)!=0&&Calls==Before);Irql=0;
 }
 // Standard GET size query does not access hardware or require output storage.
 {auto T=R;T.Verb=0x10000001;T.Value=nullptr;T.ValueSize=0;Before=Calls;
  C(Item->Handler(&T)==STATUS_BUFFER_OVERFLOW&&T.ValueSize==4&&Calls==Before);}
 // Input bytes are copied, so unaligned LONG storage remains supported.
 alignas(8) uint8_t Unaligned[9]={};LONG One=-8192;memcpy(Unaligned+1,&One,4);
 Instance[0]=0;R.Verb=2;R.Value=Unaligned+1;R.ValueSize=4;C(Item->Handler(&R)==0);
 R.Verb=1;C(Item->Handler(&R)==0);memcpy(&One,Unaligned+1,4);C(One==-8192);
 R.Value=&Level;R.ValueSize=4;
 for(unsigned Bad=0;Bad<3;Bad++){
  auto T=R;Before=Calls;if(Bad==0)T.Value=&O;if(Bad==1)T.Value=&F;if(Bad==2)T.Value=&T;
  C(Item->Handler(&T)==STATUS_INVALID_PARAMETER&&Calls==Before);
 }
 // PortCls preserves the TOPOLOGY routing modifier in Verb.
 Instance[0]=0;R.Value=&Level;R.ValueSize=4;R.Verb=0x10000002;Level=-8192;
 C(Item->Handler(&R)==0);R.Verb=0x10000001;R.ValueSize=4;Level=123;
 C(Item->Handler(&R)==0&&Level==-8192);
 R.Verb=0x10000200;R.Value=Value;R.ValueSize=88;Before=Calls;
 C(Item->Handler(&R)==0&&R.ValueSize==88&&Calls==Before);
 for(ULONG Verb:{0x10000003u,0x10000201u,0x40000001u,0x10000000u}){
  R.Verb=Verb;R.ValueSize=88;Before=Calls;C(Item->Handler(&R)!=0&&Calls==Before);
 }
 R.Verb=0x200;R.Value=nullptr;R.ValueSize=0;Before=Calls;
 C(Item->Handler(&R)==STATUS_BUFFER_TOO_SMALL&&Calls==Before);
 R.Value=&Level;
 // An admitted hardware callback pins the parent and prevents concurrent Close.
 Instance[0]=0;R.Verb=2;R.ValueSize=4;Reenter=true;C(Item->Handler(&R)==0);
 C(M->Close()==0);Before=Calls;R.Verb=1;R.ValueSize=4;C(Item->Handler(&R)!=0&&Calls==Before);
 C(M->Release()==0&&P.Refs==1&&Alloc==Freed);
 M=nullptr;B.Map[1]=1;C(T70TopologyMiniport::CreateWithVolume(&P,&B,&M)!=0&&!M&&P.Refs==1);
 B.Map[1]=0;B.Profile=99;C(T70TopologyMiniport::CreateWithVolume(&P,&B,&M)!=0&&!M);
 B.Profile=10;FailAlloc=true;C(T70TopologyMiniport::CreateWithVolume(&P,&B,&M)==STATUS_INSUFFICIENT_RESOURCES&&!M);FailAlloc=false;
 C(T70TopologyMiniport::Create(&P,&M)==0);C(M->GetDescription(&D)==0&&D->NodeCount==0);C(M->Release()==0&&P.Refs==1&&Alloc==Freed);
 T70_AUDIO_ENDPOINT Endpoint={};B.Profile=10;
 C(T70AudioEndpointOpenWithVolume(&Endpoint,(PDEVICE_OBJECT)1,(PIRP)2,&P,
   (PRESOURCELIST)3,(T70_WAVECYCLIC_PORT_OWNER *)4,&B)==0&&EndpointCalls==1);
 C(Endpoint.Miniport->GetDescription(&D)==0&&D->NodeCount==1);
 C(Endpoint.Miniport->Release()==0&&P.Refs==1&&Alloc==Freed);
 C(T70AudioEndpointOpenWithVolume(&Endpoint,(PDEVICE_OBJECT)1,(PIRP)2,&P,
   (PRESOURCELIST)3,(T70_WAVECYCLIC_PORT_OWNER *)4,nullptr)==STATUS_INVALID_PARAMETER&&EndpointCalls==1);
 // Provider validation happens before allocation/publication or register I/O.
 for(unsigned Bad=0;Bad<6;Bad++){
  auto SavedOwner=O;auto Binding=B;auto Targets=(T70TopologyMiniport **)nullptr;
  T70TopologyMiniport *Unchanged=(T70TopologyMiniport *)0x1234;Targets=&Unchanged;
  if(Bad==0)O.Target[0].Target=nullptr;
  if(Bad==1)O.Target[1].Target=O.Target[0].Target;
  if(Bad==2)O.Target[1].ActiveUses=1;
  if(Bad==3)Targets=(T70TopologyMiniport **)&F;
  if(Bad==4)Targets=(T70TopologyMiniport **)&O;
  if(Bad==5)F.Bin[1][64]=6;
  unsigned OldAlloc=Alloc;Before=Calls;
  C(T70TopologyMiniport::CreateWithVolume(&P,&Binding,Targets)!=0&&Alloc==OldAlloc&&Calls==Before);
  C(Unchanged==(T70TopologyMiniport *)0x1234&&P.Refs==1);O=SavedOwner;F.Bin[1][64]=5;
 }
 C(T70TopologyMiniport::CreateWithVolume(&P,&B,&M)==0);
 C(M->GetDescription(&D)==0);Nodes=(const PCNODE_DESCRIPTOR *)D->Nodes;
 Item=((const PCAUTOMATION_TABLE *)Nodes[0].AutomationTable)->Properties;
 R={};R.MajorTarget=static_cast<IMiniportTopology *>(M);R.PropertyItem=Item;R.Node=0;
 R.Verb=1;R.Instance=Instance;R.InstanceSize=8;R.Value=&Level;R.ValueSize=4;Before=Calls;
 C(Item->Handler(&R)==STATUS_DEVICE_NOT_READY&&Calls==Before);
 C(M->Init(&P,nullptr,&Q)==0);R.Verb=0x200;R.Value=nullptr;R.ValueSize=88;
 C(Item->Handler(&R)==STATUS_INVALID_PARAMETER&&Calls==Before);
 C(M->Close()==0&&M->Release()==0&&P.Refs==1&&Alloc==Freed);
 printf("Topology volume: %u checks; actual property/KS conversion, mock resource I/O\n",Checks);
}
