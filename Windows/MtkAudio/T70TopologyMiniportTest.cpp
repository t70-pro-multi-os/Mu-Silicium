// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T70TopologyMiniport.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
static std::atomic<unsigned> Checks{0};static unsigned Irql,Alloc,Freed;static bool Fail;
#define C(X) do{++Checks;assert(X);}while(0)
unsigned char KeGetCurrentIrql(){return Irql;}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
static std::atomic<bool> CallEntered{false},CallContinue{false};
static void BlockCall(){CallEntered=true;while(!CallContinue.load())std::this_thread::yield();}
static void (*GateHook)();
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG C){__atomic_compare_exchange_n(P,&C,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);if(C==0&&V==1&&GateHook){auto H=GateHook;GateHook=nullptr;H();}return C;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(Fail)return nullptr;++Alloc;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Freed;free(P);}
struct Parent:IUnknown {volatile LONG Refs=1;ULONG AddRef()override{return InterlockedIncrement(&Refs);}ULONG Release()override{return InterlockedDecrement(&Refs);}};
struct Port:IPortTopology {
 ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)override{assert(false);return STATUS_NOT_SUPPORTED;}
};
static T70TopologyMiniport *GateObject;
static void CloseDuringCall(){
 C(GateObject->Close()==STATUS_DEVICE_NOT_READY);
 PPCFILTER_DESCRIPTOR D=(PPCFILTER_DESCRIPTOR)1;C(GateObject->GetDescription(&D)==STATUS_DEVICE_NOT_READY&&D==(void*)1);
}
int main(){
 Parent P;Port Port;T70TopologyMiniport *M=nullptr;
 C(T70TopologyMiniport::Create(nullptr,&M)==STATUS_INVALID_PARAMETER&&!M);
 Fail=true;C(T70TopologyMiniport::Create(&P,&M)==STATUS_INSUFFICIENT_RESOURCES&&!M&&P.Refs==1);Fail=false;
 C(T70TopologyMiniport::Create(&P,&M)==0&&P.Refs==2);
 PPCFILTER_DESCRIPTOR D=nullptr;C(M->GetDescription(&D)==0&&D);
 C(D->Version==0&&!D->AutomationTable&&D->PinCount==2&&D->PinSize==sizeof(PCPIN_DESCRIPTOR));
 C(D->NodeCount==0&&!D->Nodes&&D->NodeSize==sizeof(PCNODE_DESCRIPTOR)&&D->ConnectionCount==1);
 const auto Conn=(const PCCONNECTION_DESCRIPTOR *)D->Connections;
 C(Conn->FromNode==PCFILTER_NODE&&Conn->FromNodePin==0&&Conn->ToNode==PCFILTER_NODE&&Conn->ToNodePin==1);
 C(D->CategoryCount==0&&!D->Categories);
 for(unsigned I=0;I<2;I++){
  const auto &Pin=D->Pins[I];const auto &K=Pin.KsPinDescriptor;
  C(!Pin.MaxGlobalInstanceCount&&!Pin.MaxFilterInstanceCount&&!Pin.MinFilterInstanceCount&&!Pin.AutomationTable);
  C(K.DataFlow==(I?KSPIN_DATAFLOW_OUT:KSPIN_DATAFLOW_IN)&&K.Communication==KSPIN_COMMUNICATION_NONE);
  C(K.Category&&K.Category->Data1==(I?0xdff21ce1u:0x6994ad04u)&&!K.Name);
  C(K.DataRangesCount==1&&K.DataRanges&&K.DataRanges[0]);
  auto R=K.DataRanges[0];C(R->FormatSize==64&&!R->Flags&&!R->SampleSize&&!R->Reserved);
  C(R->MajorFormat.Data1==0x73647561&&R->SubFormat.Data1==0x6dba3190&&R->Specifier.Data1==0x0f6417d6);
  ULONG N=77;KSDATARANGE Result={};
  C(M->DataRangeIntersection(I,R,R,0,nullptr,&N)==STATUS_BUFFER_OVERFLOW&&N==64);
  for(ULONG Size=1;Size<64;Size++){
   memset(&Result,0xa5,sizeof Result);auto Old=Result;N=77;
   C(M->DataRangeIntersection(I,R,R,Size,&Result,&N)==STATUS_BUFFER_TOO_SMALL&&N==77&&!memcmp(&Result,&Old,sizeof Result));
  }
  C(M->DataRangeIntersection(I,R,R,sizeof Result,&Result,&N)==0&&N==64&&!memcmp(&Result,R,64));
  for(unsigned Bad=0;Bad<7;Bad++){
   KSDATARANGE Input=*R;memset(&Result,0xa5,sizeof Result);auto Old=Result;N=77;
   if(Bad==0)Input.FormatSize++;
   if(Bad==1)Input.Flags=1;
   if(Bad==2)Input.SampleSize=4;
   if(Bad==3)Input.Reserved=1;
   if(Bad==4)Input.MajorFormat.Data1++;
   if(Bad==5)Input.SubFormat.Data1=1; // PCM is not a topology streaming format.
   if(Bad==6)Input.Specifier.Data1++;
   C(M->DataRangeIntersection(I,&Input,R,64,&Result,&N)==STATUS_NO_MATCH&&N==77&&!memcmp(&Result,&Old,64));
   C(M->DataRangeIntersection(I,R,&Input,64,&Result,&N)==STATUS_NO_MATCH&&N==77);
  }
  C(M->DataRangeIntersection(2,R,R,64,&Result,&N)==STATUS_INVALID_PARAMETER);
  C(M->DataRangeIntersection(I,R,R,64,nullptr,&N)==STATUS_INVALID_PARAMETER);
  C(M->DataRangeIntersection(I,R,R,64,&Result,nullptr)==STATUS_INVALID_PARAMETER);
  C(M->DataRangeIntersection(I,R,R,64,R,&N)==STATUS_INVALID_PARAMETER);
  C(M->DataRangeIntersection(I,R,R,64,&Result,(PULONG)&Result)==STATUS_INVALID_PARAMETER);
 }
 const GUID Ids[]={
 {0,0,0,{0xc0,0,0,0,0,0,0,0x46}},
 {0xb4c90a24,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}},
 {0xb4c90a31,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}}};
 for(auto Id:Ids){void *Q=nullptr;C(M->QueryInterface(Id,&Q)==0&&Q==static_cast<IMiniportTopology *>(M));C(M->Release()==1);}
 GUID Bad={1,0,0,{}};void *Q=(void *)1;C(M->QueryInterface(Bad,&Q)==STATUS_NOINTERFACE&&!Q);
 C(M->Init(nullptr,nullptr,&Port)==STATUS_INVALID_PARAMETER);
 C(M->Init(&P,nullptr,nullptr)==STATUS_INVALID_PARAMETER);
 C(M->Init(&P,nullptr,&Port)==0&&Port.Refs==1&&P.Refs==2);
 C(M->Init(&P,nullptr,&Port)==STATUS_INVALID_DEVICE_STATE);
 std::thread T([&]{for(unsigned I=0;I<40000;I++){M->AddRef();M->Release();}});
 for(unsigned I=0;I<40000;I++){M->AddRef();M->Release();}T.join();
 auto OldD=D;Irql=DISPATCH_LEVEL;C(M->GetDescription(&D)==STATUS_INVALID_PARAMETER&&D==OldD);
 C(M->Close()==STATUS_INVALID_PARAMETER);Irql=0;
 C(M->Close()==0&&M->Close()==0&&Port.Refs==1);
 C(M->GetDescription(&D)==STATUS_DEVICE_NOT_READY&&D==OldD);
 auto R=D->Pins[0].KsPinDescriptor.DataRanges[0];ULONG N=77;
 C(M->DataRangeIntersection(0,R,R,0,nullptr,&N)==STATUS_DEVICE_NOT_READY&&N==77);
 C(M->Init(&P,nullptr,&Port)==STATUS_INVALID_DEVICE_STATE);
 C(M->Release()==0&&P.Refs==1&&Alloc==Freed);
 C(T70TopologyMiniport::Create(&P,&M)==0);C(M->Close()==0&&M->Init(&P,nullptr,&Port)==STATUS_INVALID_DEVICE_STATE);C(M->Release()==0&&P.Refs==1&&Alloc==Freed);
 C(T70TopologyMiniport::Create(&P,&M)==0);GateObject=M;GateHook=CloseDuringCall;D=nullptr;
 C(M->GetDescription(&D)==0&&D&&P.Refs==2);C(M->Close()==0);
 C(M->Release()==0&&P.Refs==1&&Alloc==Freed);
 C(T70TopologyMiniport::Create(&P,&M)==0);GateHook=BlockCall;PPCFILTER_DESCRIPTOR Racing=nullptr;
 std::thread Active([&]{C(M->GetDescription(&Racing)==0);});
 while(!CallEntered.load())std::this_thread::yield();
 C(M->Close()==STATUS_DEVICE_NOT_READY);PPCFILTER_DESCRIPTOR Rejected=(PPCFILTER_DESCRIPTOR)1;
 C(M->GetDescription(&Rejected)==STATUS_DEVICE_NOT_READY&&Rejected==(void*)1);
 CallContinue=true;Active.join();C(Racing&&P.Refs==2);C(M->Close()==0);
 C(M->Release()==0&&P.Refs==1&&Alloc==Freed);
 printf("T70TopologyMiniport: %u checks and 80000 reference pairs; metadata only, no controls or hardware\n",Checks.load());
}
