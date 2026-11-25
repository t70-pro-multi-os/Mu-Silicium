// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeDl1Interrupt.h"
#include <vector>
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
static unsigned Irql;static std::vector<unsigned> Order;
static NTSTATUS CreateStatus,ProofStatus;static bool NullGroup;static unsigned Proofs;
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
LONG InterlockedCompareExchange(volatile LONG *P,LONG N,LONG V){__atomic_compare_exchange_n(P,&V,N,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return V;}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
struct Group:IServiceGroup {unsigned Refs=0;ULONG AddRef()override{return ++Refs;}ULONG Release()override{assert(Refs);return --Refs;}};static Group G;
NTSTATUS PcNewServiceGroup(PSERVICEGROUP *Out,PUNKNOWN O){assert(!O);if(CreateStatus)return CreateStatus;if(!NullGroup){G.Refs=1;*Out=&G;}return 0;}
static NTSTATUS Proof(void *){++Proofs;return ProofStatus;}
struct Port:IPortWaveCyclic {
 unsigned Refs=1;std::atomic<unsigned> Calls{0};std::atomic<bool> Hold{false},Entered{false};
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{assert(Refs>1);return --Refs;}
 NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)override{assert(false);return -1;}
 NTSTATUS NewMasterDmaChannel(IDmaChannel **,PUNKNOWN,PRESOURCELIST,ULONG,BOOLEAN,BOOLEAN,DMA_WIDTH,DMA_SPEED)override{assert(false);return -1;}
 void Notify(IServiceGroup *Group)override{assert(Group==&G&&Refs==2&&G.Refs==1);++Calls;Order.push_back(3);Entered=true;while(Hold)std::this_thread::yield();}
};

struct Io {ULONG Enabled,Status,Written;unsigned Fail,Reads,Writes;};
static NTSTATUS Read(void *V,ULONG Offset,ULONG *Out){
 auto I=(Io *)V;++I->Reads;Order.push_back(I->Reads);
 assert(Offset==(I->Reads==1?0x3b4u:0x3a4u));
 if(I->Fail==I->Reads)return STATUS_DEVICE_DATA_ERROR;
 *Out=I->Reads==1?I->Enabled:I->Status;return STATUS_SUCCESS;
}
static NTSTATUS Write(void *V,ULONG Offset,ULONG Value){
 auto I=(Io *)V;++I->Writes;Order.push_back(4);assert(Offset==0x3a8);I->Written=Value;
 return I->Fail==3?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS;
}
int main(){
 Port P;T70_WAVECYCLIC_SERVICE Service={};assert(T70WaveCyclicServiceAcquire(&Service,&P)==0);
 for(unsigned Bit=0;Bit<32;Bit++)for(unsigned Enable=0;Enable<2;Enable++){
  Io I={Enable?0xffffffffu:0u,1u<<Bit,0,0,0,0};
  T70_AFE_DL1_INTERRUPT C={&I,Read,Write,&Service,true,0,0,0};Order.clear();auto Calls=P.Calls.load();
  BOOLEAN Handled=T70AfeDl1Interrupt(nullptr,&C);ULONG Mask=I.Enabled&I.Status&0x87ffffffu;
  assert(Handled==(Mask!=0)&&!C.Faulted&&C.LastMask==Mask);
  assert(I.Reads==2&&I.Writes==(Mask!=0)&&P.Calls==Calls+((Mask&1)!=0));
  if(Mask)assert(I.Written==Mask&&Order.back()==4);
 }
 for(unsigned Fail=1;Fail<=3;Fail++){
  Io I={0xffffffffu,0x80000003u,0,Fail,0,0};T70_AFE_DL1_INTERRUPT C={&I,Read,Write,&Service,true,0,0,0};Order.clear();
  assert(T70AfeDl1Interrupt(nullptr,&C)==(Fail==3)&&C.Faulted&&C.LastError==STATUS_DEVICE_DATA_ERROR);
  assert(I.Reads==(Fail==1?1u:2u)&&I.Writes==(Fail==3));
  if(Fail==3)assert((Order==std::vector<unsigned>{1,2,3,4})&&I.Written==0x80000003u);
  auto Reads=I.Reads,Calls=P.Calls.load();I.Fail=0;
  assert(!T70AfeDl1Interrupt(nullptr,&C)&&I.Reads==Reads&&P.Calls==Calls);
 }
 // Suppressed software service during pause is still a handled device IRQ:
 // acknowledge in the same stock order, without turning pause into ISR fault.
 assert(T70WaveCyclicServicePause(&Service)==0);
 {Io X={1,1,0,0,0,0};T70_AFE_DL1_INTERRUPT H={&X,Read,Write,&Service,true,0,0,0};
  Order.clear();auto Before=P.Calls.load();
  assert(T70AfeDl1Interrupt(nullptr,&H)&&!H.Faulted&&X.Written==1&&P.Calls==Before);
  assert((Order==std::vector<unsigned>{1,2,4}));}
 assert(T70WaveCyclicServiceResume(&Service)==0);
 {Io X={1,1,0,0,0,0};T70_AFE_DL1_INTERRUPT H={&X,Read,Write,&Service,true,0,0,0};
  Order.clear();auto Before=P.Calls.load();
  assert(T70AfeDl1Interrupt(nullptr,&H)&&!H.Faulted&&P.Calls==Before+1);
  assert((Order==std::vector<unsigned>{1,2,3,4}));}
 Io I={1,1,0,0,0,0};T70_AFE_DL1_INTERRUPT C={&I,Read,Write,&Service,false,0,0,0};
 assert(!T70AfeDl1Interrupt(nullptr,&C)&&!I.Reads);C.DomainExclusive=true;
 assert(T70WaveCyclicServiceClose(&Service,Proof,nullptr)==0);
 assert(T70AfeDl1Interrupt(nullptr,&C)&&C.Faulted&&!I.Writes);
 puts("AFE fixed DL1 IRQ0 ISR: all 32 status bits, source order, failures and notification quarantine passed");
}
