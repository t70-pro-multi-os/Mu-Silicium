// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicService.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
static unsigned Irql;
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
 void Notify(IServiceGroup *Group)override{assert(Group==&G&&Refs==2&&G.Refs==1);++Calls;Entered=true;while(Hold)std::this_thread::yield();}
};
int main(){
 for(unsigned T=0;T<3;T++){
  Port P;T70_WAVECYCLIC_SERVICE O={};CreateStatus=T==0?STATUS_NOT_SUPPORTED:T==1?0x103:0;NullGroup=T==2;
  assert(T70WaveCyclicServiceAcquire(&O,&P)!=0&&P.Refs==1&&!G.Refs);
  assert(T70WaveCyclicServiceClose(&O,Proof,nullptr)==0);
 }
 CreateStatus=0;NullGroup=false;Port P;T70_WAVECYCLIC_SERVICE O={};
 assert(T70WaveCyclicServiceAcquire(&O,&P)==0&&P.Refs==2&&G.Refs==1);
 O.Gate=0x1fffffff;assert(!T70WaveCyclicServiceNotifyConfirmed(&O)&&!P.Calls);O.Gate=0;
 Irql=DISPATCH_LEVEL;assert(T70WaveCyclicServiceClose(&O,Proof,nullptr)==STATUS_INVALID_PARAMETER&&!O.Gate);Irql=0;
 assert(T70WaveCyclicServiceNotifyConfirmed(&O)&&P.Calls==1);
 P.Hold=true;P.Entered=false;std::thread Worker([&]{assert(T70WaveCyclicServiceNotifyConfirmed(&O));});
 while(!P.Entered)std::this_thread::yield();
 assert(T70WaveCyclicServiceClose(&O,Proof,nullptr)==STATUS_DEVICE_NOT_READY&&!Proofs&&P.Refs==2);
 assert(!T70WaveCyclicServiceNotifyConfirmed(&O));P.Hold=false;Worker.join();
 assert(T70WaveCyclicServiceClose(&O,nullptr,nullptr)==STATUS_DEVICE_NOT_READY&&P.Refs==2);
 ProofStatus=0x103;assert(T70WaveCyclicServiceClose(&O,Proof,nullptr)==STATUS_DEVICE_NOT_READY&&P.Refs==2);
 ProofStatus=STATUS_NOT_SUPPORTED;assert(T70WaveCyclicServiceClose(&O,Proof,nullptr)==ProofStatus&&G.Refs==1);
 ProofStatus=0;assert(T70WaveCyclicServiceClose(&O,Proof,nullptr)==0&&P.Refs==1&&!G.Refs);
 assert(!T70WaveCyclicServiceNotifyConfirmed(&O)&&P.Calls==2);
 auto Count=Proofs;assert(T70WaveCyclicServiceClose(&O,Proof,nullptr)==0&&Proofs==Count);
 assert(T70WaveCyclicServiceAcquire(&O,&P)!=0);
 // A temporary pause is not Close, including a pause that initially finds an
 // already-entered Notify. New submissions are suppressed, not reported failed.
 T70_WAVECYCLIC_SERVICE Empty={};
 assert(T70WaveCyclicServicePause(nullptr)==STATUS_INVALID_PARAMETER);
 assert(T70WaveCyclicServiceResume(nullptr)==STATUS_INVALID_PARAMETER);
 assert(T70WaveCyclicServicePause(&Empty)==STATUS_DEVICE_NOT_READY);
 assert(T70WaveCyclicServiceResume(&Empty)==STATUS_DEVICE_NOT_READY);
 for(unsigned Round=0;Round<100;Round++){
  Port Q;T70_WAVECYCLIC_SERVICE S={};assert(T70WaveCyclicServiceAcquire(&S,&Q)==0);
  Irql=DISPATCH_LEVEL;
  assert(T70WaveCyclicServicePause(&S)==STATUS_INVALID_PARAMETER&&!S.Gate);
  assert(T70WaveCyclicServiceResume(&S)==STATUS_INVALID_PARAMETER&&!S.Gate);Irql=0;
  assert(T70WaveCyclicServiceResume(&S)==0);
  Q.Hold=true;std::thread N([&]{assert(T70WaveCyclicServiceNotifyConfirmed(&S));});
  while(!Q.Entered)std::this_thread::yield();
  assert(T70WaveCyclicServicePause(&S)==STATUS_DEVICE_NOT_READY);
  assert(T70WaveCyclicServiceNotifyConfirmed(&S)&&Q.Calls==1);
  assert(T70WaveCyclicServiceResume(&S)==STATUS_DEVICE_NOT_READY);
  Q.Hold=false;N.join();
  assert(T70WaveCyclicServicePause(&S)==0);
  for(unsigned I=0;I<16;I++)assert(T70WaveCyclicServiceNotifyConfirmed(&S));
  assert(Q.Calls==1&&Q.Refs==2&&G.Refs==1);
  assert(T70WaveCyclicServiceResume(&S)==0&&S.Gate==0);
  assert(T70WaveCyclicServiceNotifyConfirmed(&S)&&Q.Calls==2);
  assert(T70WaveCyclicServicePause(&S)==0);
  // Close while paused still needs its independent downstream rundown proof.
  ProofStatus=STATUS_NOT_SUPPORTED;
  assert(T70WaveCyclicServiceClose(&S,Proof,nullptr)==ProofStatus&&Q.Refs==2&&G.Refs==1);
  assert(!T70WaveCyclicServiceNotifyConfirmed(&S));
  assert(T70WaveCyclicServicePause(&S)==STATUS_DEVICE_NOT_READY);
  assert(T70WaveCyclicServiceResume(&S)==STATUS_DEVICE_NOT_READY);
  ProofStatus=0;assert(T70WaveCyclicServiceClose(&S,Proof,nullptr)==0&&Q.Refs==1&&!G.Refs);
  assert(T70WaveCyclicServiceClose(&S,Proof,nullptr)==0);
 }
 // Paused suppressors may race final Close: they never dereference released
 // Port/Group pointers. Owner storage remains live until contenders are joined.
 for(unsigned Round=0;Round<100;Round++){
  Port Q;T70_WAVECYCLIC_SERVICE S={};assert(T70WaveCyclicServiceAcquire(&S,&Q)==0);
  assert(T70WaveCyclicServicePause(&S)==0);
  std::atomic<bool> Started{false},Stop{false};
  std::thread N([&]{Started=true;while(!Stop.load())(void)T70WaveCyclicServiceNotifyConfirmed(&S);});
  while(!Started.load())std::this_thread::yield();
  assert(T70WaveCyclicServiceClose(&S,Proof,nullptr)==0);
  Stop=true;N.join();assert(!Q.Calls&&Q.Refs==1&&!G.Refs);
 }
 puts("WaveCyclic service group and Notify gate: 100 concurrent pause/resume rounds, 100 paused close races, retained rundown passed");
}
