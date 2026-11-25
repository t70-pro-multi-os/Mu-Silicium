// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioServiceSinkOwner.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <initializer_list>
static thread_local unsigned Irql;
static bool FailAllocate;
static unsigned Allocated,Freed;
unsigned char KeGetCurrentIrql(){return Irql;}
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG E){__atomic_compare_exchange_n(P,&E,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return E;}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(FailAllocate)return nullptr;Allocated++;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){Freed++;free(P);}
#define REFS volatile LONG Refs=1;ULONG AddRef()override{return InterlockedIncrement(&Refs);}ULONG Release()override{return InterlockedDecrement(&Refs);}
struct Parent:IUnknown{REFS};
struct Target:IServiceSink{REFS
 std::atomic<unsigned> Calls{0};std::atomic<bool> Hold{false},Entered{false};
 T70_AUDIO_SERVICE_SINK_OWNER *Reenter=nullptr;NTSTATUS ReentrantStatus=0;
 void RequestService()override{Calls++;Entered=true;if(Reenter)ReentrantStatus=T70AudioServiceSinkClose(Reenter);while(Hold)std::this_thread::yield();}
};
struct Group:IServiceGroup{REFS
 PSERVICESINK Member=nullptr;unsigned Adds=0,Removes=0;NTSTATUS Status=0;
 bool Early=false,DuringRemove=false,AsyncEarly=false;std::thread Async;std::atomic<bool> *Entered=nullptr;T70_AUDIO_SERVICE_SINK_OWNER *Reenter=nullptr;NTSTATUS ReentrantStatus=0;
 NTSTATUS AddMember(PSERVICESINK S)override{Adds++;if(Early)S->RequestService();if(AsyncEarly){S->AddRef();Async=std::thread([=]{Irql=DISPATCH_LEVEL;S->RequestService();S->Release();});while(!*Entered)std::this_thread::yield();}if(Status)return Status;assert(!Member);S->AddRef();Member=S;return 0;}
 void RemoveMember(PSERVICESINK S)override{assert(Member==S);Removes++;if(DuringRemove)S->RequestService();if(Reenter)ReentrantStatus=T70AudioServiceSinkClose(Reenter);Member=nullptr;S->Release();}
};
int main(){
 Parent P;Target T;Group G;
 T70_AUDIO_SERVICE_SINK_OWNER Invalid={};
 assert(T70AudioServiceSinkOpen(nullptr,&P,&G,&T)==STATUS_INVALID_PARAMETER);
 Irql=DISPATCH_LEVEL;assert(T70AudioServiceSinkOpen(&Invalid,&P,&G,&T)==STATUS_INVALID_PARAMETER&&T70AudioServiceSinkClose(&Invalid)==STATUS_INVALID_PARAMETER);Irql=0;
 FailAllocate=true;assert(T70AudioServiceSinkOpen(&Invalid,&P,&G,&T)==STATUS_INSUFFICIENT_RESOURCES);FailAllocate=false;
 assert(P.Refs==1&&T.Refs==1&&G.Refs==1&&T70AudioServiceSinkClose(&Invalid)==0);
 {
  T70_AUDIO_SERVICE_SINK_OWNER O={};G.Status=STATUS_NOT_SUPPORTED;
  assert(T70AudioServiceSinkOpen(&O,&P,&G,&T)==STATUS_NOT_SUPPORTED&&T70AudioServiceSinkClose(&O)==0);
  assert(P.Refs==1&&T.Refs==1&&G.Refs==1);G.Status=0;
 }
 {
  T70_AUDIO_SERVICE_SINK_OWNER O={};assert(T70AudioServiceSinkOpen(&O,&P,&G,&T)==0);
  assert(P.Refs==2&&T.Refs==2&&G.Refs==2);
  auto Proxy=G.Member;assert(Proxy!=&T);
  const GUID U={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
  const GUID SI={0x22c6ac64,0x851b,0x11d0,{0x9a,0x7f,0,0xaa,0,0x38,0xac,0xfe}};
  void *Q=nullptr;assert(Proxy->QueryInterface(U,&Q)==0&&Q==Proxy);((IUnknown*)Q)->Release();
  assert(Proxy->QueryInterface(SI,&Q)==0&&Q==Proxy);((IUnknown*)Q)->Release();
  GUID Bad={1,0,0,{}};Q=(void*)1;assert(Proxy->QueryInterface(Bad,&Q)==STATUS_NOINTERFACE&&!Q);
  assert(Proxy->QueryInterface(U,nullptr)==STATUS_INVALID_PARAMETER);
  Proxy->AddRef(); // legitimate late retained callback
  assert(T70AudioServiceSinkOpen(&O,&P,&G,&T)==STATUS_INVALID_DEVICE_STATE);
  T.Hold=true;T.Entered=false;
  std::thread Call([&]{Irql=DISPATCH_LEVEL;Proxy->RequestService();});
  while(!T.Entered)std::this_thread::yield();auto Calls=T.Calls.load(),Removes=G.Removes;
  G.DuringRemove=true;G.Reenter=&O;
  assert(T70AudioServiceSinkClose(&O)==STATUS_DEVICE_NOT_READY);
  assert(G.Removes==Removes+1&&G.ReentrantStatus==STATUS_DEVICE_NOT_READY&&!G.Member);
  Proxy->RequestService();assert(T.Calls==Calls);
  assert(T70AudioServiceSinkClose(&O)==STATUS_DEVICE_NOT_READY&&G.Removes==Removes+1);
  T.Hold=false;Call.join();assert(T70AudioServiceSinkClose(&O)==0);
  assert(G.Refs==1&&P.Refs==2&&T.Refs==2); // late proxy still pins target/parent
  Proxy->RequestService();assert(T.Calls==Calls);Proxy->Release();
  assert(P.Refs==1&&T.Refs==1&&T70AudioServiceSinkClose(&O)==0);
  G.Reenter=nullptr;G.DuringRemove=false;
 }
 {
  T70_AUDIO_SERVICE_SINK_OWNER O={};T.Reenter=&O;G.Early=true;
  assert(T70AudioServiceSinkOpen(&O,&P,&G,&T)==0&&T.ReentrantStatus==STATUS_DEVICE_NOT_READY);
  G.Early=false;G.Member->RequestService();
  assert(T.ReentrantStatus==STATUS_DEVICE_NOT_READY&&!G.Member);
  assert(T70AudioServiceSinkClose(&O)==0&&P.Refs==1&&T.Refs==1);T.Reenter=nullptr;
 }
 // Informational AddMember cannot establish whether the real group retained
 // membership. Production deliberately retains both references and refuses Close.
 for(NTSTATUS Status:{(NTSTATUS)1,(NTSTATUS)0x103}){
  T70_AUDIO_SERVICE_SINK_OWNER O={};G.Status=Status;auto Removed=G.Removes;
  assert(T70AudioServiceSinkOpen(&O,&P,&G,&T)==STATUS_DEVICE_NOT_READY);
  assert(O.Proxy&&O.Group&&!G.Member&&P.Refs==2&&T.Refs==2&&G.Refs==2);
  auto Calls=T.Calls.load();O.Proxy->RequestService();assert(T.Calls==Calls);
  assert(T70AudioServiceSinkClose(&O)==STATUS_DEVICE_NOT_READY&&G.Removes==Removed);
  // Fixture-only destruction: this fake AddMember explicitly took no reference.
  // This is NOT a production recovery API or proof for an unknown real group.
  O.Proxy->Release();O.Group->Release();O={};G.Status=0;
 }
 {
  T70_AUDIO_SERVICE_SINK_OWNER O={};T.Hold=true;T.Entered=false;
  G.AsyncEarly=true;G.Entered=&T.Entered;G.Status=STATUS_NOT_SUPPORTED;auto Removed=G.Removes;
  assert(T70AudioServiceSinkOpen(&O,&P,&G,&T)==STATUS_NOT_SUPPORTED&&O.Proxy&&!G.Member);
  assert(T70AudioServiceSinkClose(&O)==STATUS_DEVICE_NOT_READY&&G.Removes==Removed);
  T.Hold=false;G.Async.join();G.AsyncEarly=false;G.Status=0;
  assert(T70AudioServiceSinkClose(&O)==0&&P.Refs==1&&T.Refs==1&&G.Refs==1);
 }
 for(unsigned I=0;I<200;I++){
  T70_AUDIO_SERVICE_SINK_OWNER O={};assert(T70AudioServiceSinkOpen(&O,&P,&G,&T)==0);
  auto Proxy=G.Member;Proxy->AddRef();std::atomic<bool> Go{false};
  std::thread Call([&]{while(!Go){}for(unsigned N=0;N<100;N++)Proxy->RequestService();Proxy->Release();});
  Go=true;auto S=T70AudioServiceSinkClose(&O);assert(S==0||S==STATUS_DEVICE_NOT_READY);
  Call.join();assert(T70AudioServiceSinkClose(&O)==0&&P.Refs==1&&T.Refs==1&&G.Refs==1);
 }
 // All registered proxies share one callback gate, including registrations
 // added while paused. Pause success drains Target calls, not opaque DPC queues.
 assert(T70AudioServiceGatePause(nullptr)==STATUS_INVALID_PARAMETER);
 assert(T70AudioServiceGateResume(nullptr)==STATUS_INVALID_PARAMETER);
 assert(T70AudioServiceGateSeal(nullptr)==STATUS_INVALID_PARAMETER);
 for(unsigned Round=0;Round<100;Round++){
  T70_AUDIO_SERVICE_GATE Shared={};Target A,B;Group GA,GB;
  T70_AUDIO_SERVICE_SINK_OWNER OA={},OB={};
  assert(T70AudioServiceSinkOpen(&OA,&P,&GA,&A,&Shared)==0);
  Irql=DISPATCH_LEVEL;assert(T70AudioServiceGatePause(&Shared)==STATUS_INVALID_PARAMETER);
  assert(T70AudioServiceGateResume(&Shared)==STATUS_INVALID_PARAMETER);
  assert(T70AudioServiceGateSeal(&Shared)==STATUS_INVALID_PARAMETER);Irql=0;
  A.Hold=true;std::thread First([&]{Irql=DISPATCH_LEVEL;GA.Member->RequestService();});
  while(!A.Entered)std::this_thread::yield();
  assert(T70AudioServiceGatePause(&Shared)==STATUS_DEVICE_NOT_READY);
  assert(T70AudioServiceGateResume(&Shared)==STATUS_DEVICE_NOT_READY);
  GA.Member->RequestService();assert(A.Calls==1);
  GB.Early=true;assert(T70AudioServiceSinkOpen(&OB,&P,&GB,&B,&Shared)==0&&!B.Calls);
  GB.Member->RequestService();assert(!B.Calls);
  A.Hold=false;First.join();assert(T70AudioServiceGatePause(&Shared)==0);
  auto Late=GB.Member;Late->AddRef();
  assert(T70AudioServiceGateResume(&Shared)==0);
  GA.Member->RequestService();Late->RequestService();assert(A.Calls==2&&B.Calls==1);
  B.Hold=true;B.Entered=false;std::thread Second([&]{Irql=DISPATCH_LEVEL;Late->RequestService();});
  while(!B.Entered)std::this_thread::yield();
  assert(T70AudioServiceGateSeal(&Shared)==STATUS_DEVICE_NOT_READY);
  GA.Member->RequestService();assert(A.Calls==2);
  assert(T70AudioServiceGatePause(&Shared)==STATUS_DEVICE_NOT_READY);
  assert(T70AudioServiceGateResume(&Shared)==STATUS_DEVICE_NOT_READY);
  B.Hold=false;Second.join();assert(T70AudioServiceGateSeal(&Shared)==0);
  T70_AUDIO_SERVICE_SINK_OWNER Rejected={};Group GC;
  assert(T70AudioServiceSinkOpen(&Rejected,&P,&GC,&B,&Shared)==STATUS_DEVICE_NOT_READY&&!GC.Adds);
  assert(T70AudioServiceSinkClose(&OA)==0&&T70AudioServiceSinkClose(&OB)==0);
  auto Calls=B.Calls.load();Late->RequestService();assert(B.Calls==Calls);Late->Release();
  assert(P.Refs==1&&A.Refs==1&&B.Refs==1&&GA.Refs==1&&GB.Refs==1);
 }
 {
  T70_AUDIO_SERVICE_GATE Shared={};Target A,B;Group GA,GB;T70_AUDIO_SERVICE_SINK_OWNER OA={},OB={};
  assert(T70AudioServiceSinkOpen(&OA,&P,&GA,&A,&Shared)==0);
  assert(T70AudioServiceSinkOpen(&OB,&P,&GB,&B,&Shared)==0);
  A.Hold=B.Hold=true;
  std::thread First([&]{GA.Member->RequestService();}),Second([&]{GB.Member->RequestService();});
  while(!A.Entered||!B.Entered)std::this_thread::yield();
  assert((InterlockedCompareExchange(&Shared.State,0,0)&0x1fffffff)==2);
  assert(T70AudioServiceGatePause(&Shared)==STATUS_DEVICE_NOT_READY);
  A.Hold=false;First.join();assert(T70AudioServiceGatePause(&Shared)==STATUS_DEVICE_NOT_READY);
  B.Hold=false;Second.join();assert(T70AudioServiceGatePause(&Shared)==0);
  assert(T70AudioServiceSinkClose(&OA)==0&&T70AudioServiceSinkClose(&OB)==0&&P.Refs==1);
 }
 {
  T70_AUDIO_SERVICE_GATE Shared={};Target A;Group GA;T70_AUDIO_SERVICE_SINK_OWNER OA={};
  assert(T70AudioServiceSinkOpen(&OA,&P,&GA,&A,&Shared)==0);
  Shared.State=0x1fffffff;GA.Member->RequestService();assert(!A.Calls&&Shared.State==0x1fffffff);
  Shared.State=0;assert(T70AudioServiceSinkClose(&OA)==0);
 }
 assert(Allocated==Freed);
 puts("AudioServiceSinkOwner: actual registration, reentrancy, callback rundown, retained late refs 200 close/callback races and 100 shared multi-sink pause/seal rounds passed");
}
