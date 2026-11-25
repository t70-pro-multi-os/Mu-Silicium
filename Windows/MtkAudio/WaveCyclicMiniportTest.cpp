// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicMiniport.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <cstring>
static std::atomic<unsigned> Checks{0};static unsigned Irql,Allocations,Frees,InitCalls,CreateCalls;static bool Fail;
#define C(X) do{++Checks;assert(X);}while(0)
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
static std::atomic<bool> CallEntered{false},CallContinue{false};
static void BlockCall(){CallEntered=true;while(!CallContinue.load())std::this_thread::yield();}
static void (*GateHook)();
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG C){__atomic_compare_exchange_n(P,&C,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);if(C==0&&V==1&&GateHook){auto H=GateHook;GateHook=nullptr;H();}return C;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(Fail)return nullptr;++Allocations;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Frees;free(P);}
#define REFS ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{C(Refs>0);return --Refs;}
struct Owner: IUnknown {REFS};
struct Group:IServiceGroup {REFS};
struct Channel:IDmaChannel {REFS
 NTSTATUS AllocateBuffer(ULONG,PHYSICAL_ADDRESS *)override{assert(false);return -1;}
 void FreeBuffer()override{assert(false);}void *SystemAddress()override{assert(false);return nullptr;}
 ULONG AllocatedBufferSize()override{assert(false);return 0;}void SetBufferSize(ULONG)override{assert(false);}
 void CopyTo(void *,void *,ULONG)override{assert(false);}
};
struct Stream:IMiniportWaveCyclicStream {REFS
 NTSTATUS SetFormat(PKSDATAFORMAT)override{assert(false);return -1;}
 ULONG SetNotificationFreq(ULONG,PULONG)override{assert(false);return 0;}
 NTSTATUS SetState(KSSTATE)override{assert(false);return -1;}
 NTSTATUS GetPosition(PULONG)override{assert(false);return -1;}
 NTSTATUS NormalizePhysicalPosition(PLONGLONG)override{assert(false);return -1;}
 void Silence(void *,ULONG)override{assert(false);}
};
struct Port:IPortWaveCyclic {REFS
 NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)override{assert(false);return -1;}
 NTSTATUS NewMasterDmaChannel(IDmaChannel **,PUNKNOWN,PRESOURCELIST,ULONG,BOOLEAN,BOOLEAN,DMA_WIDTH,DMA_SPEED)override{assert(false);return -1;}
};
struct Context {NTSTATUS InitStatus=0,CreateStatus=0;unsigned Mask=7;Port P;Stream S;Channel D;Group G;};
static NTSTATUS Initialize(void *V,PUNKNOWN,PRESOURCELIST R,IPortWaveCyclic *P){auto X=(Context *)V;C(R&&P==&X->P);++InitCalls;return X->InitStatus;}
static bool BlockFactory;
static NTSTATUS Factory(void *V,IPortWaveCyclic *P,PKSDATAFORMAT F,PMINIPORTWAVECYCLICSTREAM *S,PDMACHANNEL *D,PSERVICEGROUP *G){
 auto X=(Context *)V;C(P==&X->P&&F);++CreateCalls;if(BlockFactory)BlockCall();
 if(X->Mask&1){X->S.AddRef();*S=&X->S;}if(X->Mask&2){X->D.AddRef();*D=&X->D;}if(X->Mask&4){X->G.AddRef();*G=&X->G;}return X->CreateStatus;
}
static NTSTATUS Confirm(void *P){return *(NTSTATUS *)P;}
static NTSTATUS BlockConfirm(void *){BlockCall();return STATUS_SUCCESS;}
static T70WaveCyclicMiniport *GateObject;
static void CloseDuringCall(){
 NTSTATUS S=0;C(GateObject->Close(Confirm,&S)==STATUS_DEVICE_NOT_READY);
 PPCFILTER_DESCRIPTOR D=(PPCFILTER_DESCRIPTOR)1;C(GateObject->GetDescription(&D)==STATUS_DEVICE_NOT_READY&&D==(void*)1);
}
int main(){
 Owner O;Context X;T70_WAVECYCLIC_MINIPORT_PROVIDER B={&X,Initialize,Factory};T70WaveCyclicMiniport *M=nullptr;
 Fail=true;C(T70WaveCyclicMiniport::Create(&O,B,&M)==STATUS_INSUFFICIENT_RESOURCES&&!M&&O.Refs==1);Fail=false;
 auto Bad=B;Bad.Create=nullptr;C(T70WaveCyclicMiniport::Create(&O,Bad,&M)==STATUS_DEVICE_NOT_READY&&!M);
 C(T70WaveCyclicMiniport::Create(&O,B,&M)==0&&O.Refs==2);
 PPCFILTER_DESCRIPTOR Desc=(PPCFILTER_DESCRIPTOR)1;C(M->GetDescription(&Desc)==STATUS_DEVICE_NOT_READY&&Desc==(void *)1);
 const GUID Id={0xb4c90a27,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};void *Q=nullptr;
 C(M->QueryInterface(Id,&Q)==0&&Q==static_cast<IMiniportWaveCyclic *>(M));C(M->Release()==1);
 unsigned Dummy=0;C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==0&&X.P.Refs==2);
 C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==STATUS_INVALID_DEVICE_STATE&&InitCalls==1);
 C(M->GetDescription(&Desc)==0&&Desc->PinCount==2&&Desc->Pins->KsPinDescriptor.DataRangesCount==11);
 C(Desc->Pins[1].KsPinDescriptor.DataFlow==KSPIN_DATAFLOW_OUT&&
   Desc->Pins[1].KsPinDescriptor.Communication==KSPIN_COMMUNICATION_NONE&&
   Desc->NodeCount==1&&Desc->ConnectionCount==2);
 const auto *Edges=(const PCCONNECTION_DESCRIPTOR *)Desc->Connections;
 C(Edges[0].FromNode==PCFILTER_NODE&&Edges[0].FromNodePin==0&&Edges[0].ToNode==0&&Edges[0].ToNodePin==1);
 C(Edges[1].FromNode==0&&Edges[1].FromNodePin==0&&Edges[1].ToNode==PCFILTER_NODE&&Edges[1].ToNodePin==1);
 const auto &K=Desc->Pins[0].KsPinDescriptor;
 const ULONG Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 ULONG Length=55;C(M->DataRangeIntersection(0,nullptr,nullptr,0,nullptr,&Length)==STATUS_INVALID_PARAMETER&&Length==55);
 // Match every published rate and channel ceiling. Returned bytes are
 // checked against exact WAVEFORMATEXTENSIBLE fields, not mock ABI layout.
 for(unsigned I=0;I<11;I++)for(unsigned Ch=1;Ch<=3;Ch++){
  auto Matching=K.DataRanges[I];KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)Matching;
  Client.MaximumChannels=Ch;Client.MinimumSampleFrequency=8000;Client.MaximumSampleFrequency=192000;
  unsigned char Output[108];memset(Output,0xa5,sizeof Output);ULONG L=77;
  C(M->DataRangeIntersection(0,&Client.DataRange,Matching,0,nullptr,&L)==STATUS_BUFFER_OVERFLOW&&L==104);
  for(ULONG Cap=1;Cap<104;Cap++){
   L=77;C(M->DataRangeIntersection(0,&Client.DataRange,Matching,Cap,Output,&L)==STATUS_BUFFER_TOO_SMALL&&L==77);
   for(auto V:Output)C(V==0xa5);
  }
  C(M->DataRangeIntersection(0,&Client.DataRange,Matching,sizeof Output,Output,&L)==0&&L==104);
  unsigned Channels=Ch>2?2:Ch,Align=Channels*2;
  C(Output[0]==104&&Output[8]==Align&&Output[64]==0xfe&&Output[65]==0xff&&Output[66]==Channels);
  ULONG Rate=Output[68]|((ULONG)Output[69]<<8)|((ULONG)Output[70]<<16)|((ULONG)Output[71]<<24);
  ULONG Bytes=Output[72]|((ULONG)Output[73]<<8)|((ULONG)Output[74]<<16)|((ULONG)Output[75]<<24);
  C(Rate==Rates[I]&&Bytes==Rate*Align&&Output[76]==Align&&Output[78]==16&&Output[80]==22&&Output[82]==16&&Output[84]==(Channels==1?4:3));
  C(Output[104]==0xa5&&Output[107]==0xa5);

  Client.MinimumBitsPerSample=24;L=77;unsigned char Before[108];memcpy(Before,Output,108);
  C(M->DataRangeIntersection(0,&Client.DataRange,Matching,108,Output,&L)==STATUS_NO_MATCH&&L==77&&!memcmp(Output,Before,108));
 }
 for(ULONG Ch=1;Ch<=3;Ch++)for(ULONG Sample=0;Sample<=8;Sample++){
  KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)K.DataRanges[6];
  Client.MaximumChannels=Ch;Client.DataRange.SampleSize=Sample;ULONG L=77;
  unsigned char Output[104];memset(Output,0xa5,104);
  bool Accepted=Sample==0||Sample==2||(Sample==4&&Ch>=2);
  NTSTATUS R=M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[6],104,Output,&L);
  C((R==0)==Accepted);
  if(Accepted){
   ULONG Channels=Sample?Sample/2:(Ch>=2?2:1);
   C(L==104&&Output[66]==Channels&&Output[8]==Channels*2);

  }else{C(R==STATUS_NO_MATCH&&L==77);for(auto V:Output)C(V==0xa5);}
 }
 for(unsigned Case=0;Case<12;Case++){
  KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)K.DataRanges[0],Match=Client;
  if(Case==0)Client.DataRange.FormatSize--;
  if(Case==1)Client.DataRange.Flags=1;
  if(Case==2)Client.DataRange.SubFormat.Data1=3;
  if(Case==3)Client.MaximumChannels=0;
  if(Case==4)Client.MinimumBitsPerSample=32;
  if(Case==5)Client.MaximumBitsPerSample=8;
  if(Case==6)Client.MinimumSampleFrequency=9000;
  if(Case==7)Client.MaximumSampleFrequency=7999;
  if(Case==8)Client.DataRange.SampleSize=1;
  if(Case==9)Match.MinimumSampleFrequency=9999;
  if(Case==10)Match.DataRange.FormatSize++;
  if(Case==11)Client.DataRange.Specifier.Data1=0;
  ULONG L=77;unsigned char Output[104];memset(Output,0xa5,104);
  C(M->DataRangeIntersection(0,&Client.DataRange,&Match.DataRange,104,Output,&L)==STATUS_NO_MATCH&&L==77);
  for(auto V:Output)C(V==0xa5);
 }
 KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)K.DataRanges[0];ULONG L=77;unsigned char Output[104]={};
 C(M->DataRangeIntersection(1,&Client.DataRange,K.DataRanges[0],104,Output,&L)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,nullptr,&L)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,Output,nullptr)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,&Client,&L)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,Output,(PULONG)Output)==STATUS_INVALID_PARAMETER);
 Irql=2;C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,Output,&L)==STATUS_INVALID_PARAMETER);Irql=0;
 KSDATAFORMAT F={};
 for(NTSTATUS Status:{STATUS_SUCCESS,STATUS_DEVICE_DATA_ERROR,(NTSTATUS)0x103,(NTSTATUS)1})for(unsigned Mask=0;Mask<8;Mask++){
  X.CreateStatus=Status;X.Mask=Mask;PMINIPORTWAVECYCLICSTREAM S=(PMINIPORTWAVECYCLICSTREAM)1;PDMACHANNEL D=(PDMACHANNEL)2;PSERVICEGROUP G=(PSERVICEGROUP)3;
  NTSTATUS R=M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&F,&D,&G);
  if(Status==0&&Mask==7){C(R==0&&S==&X.S&&D==&X.D&&G==&X.G);S->Release();D->Release();G->Release();}
  else C(R==(Status<0?Status:STATUS_DEVICE_NOT_READY)&&S==(void *)1&&D==(void *)2&&G==(void *)3);
  C(X.S.Refs==1&&X.D.Refs==1&&X.G.Refs==1);
 }
 PMINIPORTWAVECYCLICSTREAM S=nullptr;PDMACHANNEL D=nullptr;PSERVICEGROUP G=nullptr;unsigned Calls=CreateCalls;
 C(M->NewStream(&S,&O,(POOL_TYPE)0,0,FALSE,&F,&D,&G)==STATUS_NOT_SUPPORTED);
 C(M->NewStream(&S,nullptr,(POOL_TYPE)0,1,FALSE,&F,&D,&G)==STATUS_NOT_SUPPORTED);
 C(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,TRUE,&F,&D,&G)==STATUS_NOT_SUPPORTED);
 C(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&F,(PDMACHANNEL *)&S,&G)==STATUS_INVALID_PARAMETER);
 Irql=2;C(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&F,&D,&G)==STATUS_INVALID_PARAMETER);Irql=0;
 C(CreateCalls==Calls);
 NTSTATUS Proof=0x103;C(M->Close(Confirm,&Proof)==STATUS_DEVICE_NOT_READY&&X.P.Refs==2);
 C(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&F,&D,&G)==STATUS_DEVICE_NOT_READY&&CreateCalls==Calls);
 Proof=STATUS_DEVICE_DATA_ERROR;C(M->Close(Confirm,&Proof)==Proof&&X.P.Refs==2);
 Proof=0;C(M->Close(Confirm,&Proof)==0&&X.P.Refs==1);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,Output,&L)==STATUS_DEVICE_NOT_READY);
 C(M->Close(nullptr,nullptr)==0);C(M->Release()==0&&O.Refs==1&&X.P.Refs==1);
 for(NTSTATUS Status:{STATUS_DEVICE_DATA_ERROR,(NTSTATUS)0x103,(NTSTATUS)1}){
  X.InitStatus=Status;C(T70WaveCyclicMiniport::Create(&O,B,&M)==0);
  C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==(Status<0?Status:STATUS_DEVICE_NOT_READY)&&X.P.Refs==1);
  C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==STATUS_INVALID_DEVICE_STATE);
  C(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&F,&D,&G)==STATUS_DEVICE_NOT_READY);
  C(M->Release()==0&&O.Refs==1);
 }
 X.InitStatus=0;C(T70WaveCyclicMiniport::Create(&O,B,&M)==0);
 C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==0);
 GateObject=M;GateHook=CloseDuringCall;Desc=nullptr;
 C(M->GetDescription(&Desc)==0&&Desc&&X.P.Refs==2&&O.Refs==2);
 Proof=0;C(M->Close(Confirm,&Proof)==0&&X.P.Refs==1);
 C(M->Release()==0&&O.Refs==1);
 C(T70WaveCyclicMiniport::Create(&O,B,&M)==0);C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==0);
 GateHook=BlockCall;PPCFILTER_DESCRIPTOR Racing=nullptr;
 std::thread Active([&]{C(M->GetDescription(&Racing)==0);});
 while(!CallEntered.load())std::this_thread::yield();
 C(M->Close(Confirm,&Proof)==STATUS_DEVICE_NOT_READY&&X.P.Refs==2);
 PPCFILTER_DESCRIPTOR Rejected=(PPCFILTER_DESCRIPTOR)1;
 C(M->GetDescription(&Rejected)==STATUS_DEVICE_NOT_READY&&Rejected==(void*)1);
 CallContinue=true;Active.join();C(Racing&&O.Refs==2);
 C(M->Close(Confirm,&Proof)==0&&X.P.Refs==1);C(M->Release()==0&&O.Refs==1);
 C(T70WaveCyclicMiniport::Create(&O,B,&M)==0);C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==0);
 CallEntered=false;CallContinue=false;
 std::thread Closing([&]{C(M->Close(BlockConfirm,nullptr)==0);});
 while(!CallEntered.load())std::this_thread::yield();
 C(O.Refs==3&&M->Close(Confirm,&Proof)==STATUS_DEVICE_NOT_READY&&X.P.Refs==2);
 CallContinue=true;Closing.join();C(O.Refs==2&&X.P.Refs==1);C(M->Release()==0&&O.Refs==1);
 C(T70WaveCyclicMiniport::Create(&O,B,&M)==0);C(M->Init(nullptr,(PRESOURCELIST)&Dummy,&X.P)==0);
 CallEntered=false;CallContinue=false;BlockFactory=true;X.CreateStatus=STATUS_DEVICE_DATA_ERROR;X.Mask=0;
 std::thread Creating([&]{C(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&F,&D,&G)==STATUS_DEVICE_DATA_ERROR);});
 while(!CallEntered.load())std::this_thread::yield();
 C(M->QuiesceCalls()==STATUS_DEVICE_NOT_READY&&X.P.Refs==2);
 CallContinue=true;Creating.join();BlockFactory=false;
 C(M->QuiesceCalls()==0&&M->QuiesceCalls()==0&&X.P.Refs==2);
 C(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&F,&D,&G)==STATUS_DEVICE_NOT_READY);
 C(M->Close(Confirm,&Proof)==0&&X.P.Refs==1);C(M->Release()==0&&O.Refs==1);
 C(Allocations==Frees);printf("WaveCyclic miniport: %u checks; fake providers only\n",Checks.load());
}
