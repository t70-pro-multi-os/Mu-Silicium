// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioExportOwner.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
static unsigned Irql,Alloc,Freed,Fail;
unsigned char KeGetCurrentIrql(){return Irql;}
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG E){__atomic_compare_exchange_n(P,&E,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return E;}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){++Alloc;if(Alloc==Fail)return nullptr;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Freed;free(P);}
#define REFS volatile LONG Refs=1;ULONG AddRef()override{return InterlockedIncrement(&Refs);}ULONG Release()override{return InterlockedDecrement(&Refs);}
struct Parent:IUnknown {REFS};
struct Stream:IMiniportWaveCyclicStream {REFS
 std::atomic<bool> Hold{false},Entered{false};
 unsigned Calls=0;
 NTSTATUS SetFormat(PKSDATAFORMAT F)override{assert(F);Calls++;return STATUS_NOT_SUPPORTED;}
 ULONG SetNotificationFreq(ULONG I,PULONG N)override{*N=I*4;Calls++;return I;}
 NTSTATUS SetState(KSSTATE S)override{assert(S==KSSTATE_PAUSE);Calls++;return 0;}
 NTSTATUS GetPosition(PULONG P)override{Entered=true;while(Hold)std::this_thread::yield();*P=16;Calls++;return 0;}
 NTSTATUS NormalizePhysicalPosition(PLONGLONG P)override{*P=123;Calls++;return 0;}
 void Silence(void *P,ULONG N)override{memset(P,0,N);Calls++;}
};
struct Channel:IDmaChannel {REFS
 unsigned Size=32;unsigned char Bytes[32]={};
 NTSTATUS AllocateBuffer(ULONG N,PPHYSICAL_ADDRESS A)override{assert(N==32&&!A);return STATUS_NOT_SUPPORTED;}
 void FreeBuffer()override{Size=0;}
 ULONG TransferCount()override{return 3;}
 ULONG MaximumBufferSize()override{return 32;}
 ULONG AllocatedBufferSize()override{return 32;}
 ULONG BufferSize()override{return Size;}
 void SetBufferSize(ULONG N)override{Size=N;}
 void *SystemAddress()override{return Bytes;}
 PHYSICAL_ADDRESS PhysicalAddress()override{PHYSICAL_ADDRESS P={};P.QuadPart=0x123400;return P;}
 PADAPTER_OBJECT GetAdapterObject()override{return (void *)0x1234;}
 void CopyTo(void *D,void *S,ULONG N)override{memcpy(D,S,N);}
 void CopyFrom(void *D,void *S,ULONG N)override{memcpy(D,S,N);}
};
struct Group:IServiceGroup {REFS
 unsigned Calls=0;PSERVICESINK Member=nullptr;
 void RequestService()override{Calls++;if(Member)Member->RequestService();}
 NTSTATUS AddMember(PSERVICESINK S)override{assert(!Member);Member=S;S->AddRef();return 0;}
 void RemoveMember(PSERVICESINK S)override{assert(S==Member);Member=nullptr;S->Release();}
 void SupportDelayedService()override{Calls++;}
 void RequestDelayedService(ULONGLONG N)override{assert(N==42);Calls++;}
 void CancelDelayedService()override{Calls++;}
};
struct Sink:IServiceSink {REFS unsigned Calls=0;void RequestService()override{Calls++;}};
struct MultiGroup:IServiceGroup {REFS
 std::mutex Mutex;std::vector<PSERVICESINK> Members;
 unsigned Adds=0,Removes=0;NTSTATUS Status=0;
 bool Early=false;std::atomic<bool> HoldAdd{false},InAdd{false};
 NTSTATUS AddMember(PSERVICESINK S)override{
  Adds++;InAdd=true;while(HoldAdd)std::this_thread::yield();
  if(Early)S->RequestService();if(Status)return Status;
  S->AddRef();std::lock_guard<std::mutex>L(Mutex);Members.push_back(S);return 0;
 }
 void RemoveMember(PSERVICESINK S)override{
  {std::lock_guard<std::mutex>L(Mutex);auto I=Members.begin();while(I!=Members.end()&&*I!=S)++I;assert(I!=Members.end());Members.erase(I);Removes++;}S->Release();
 }
 PSERVICESINK Retain(unsigned I){std::lock_guard<std::mutex>L(Mutex);auto S=Members.at(I);S->AddRef();return S;}
};
struct BlockingSink:IServiceSink {REFS
 std::atomic<bool> In{false},Hold{false};std::atomic<unsigned> Calls{0};
 PSERVICEGROUP Reenter=nullptr;
 void RequestService()override{Calls++;In=true;if(Reenter)Reenter->RemoveMember(this);while(Hold)std::this_thread::yield();}
};
struct CyclicSink final:IServiceSink {
 volatile LONG Refs=1;PSERVICEGROUP Held;bool *Destroyed;
 CyclicSink(PSERVICEGROUP G,bool *D):Held(G),Destroyed(D){G->AddRef();}
 ULONG AddRef()override{return InterlockedIncrement(&Refs);}
 ULONG Release()override{LONG N=InterlockedDecrement(&Refs);if(!N){auto G=Held;*Destroyed=true;delete this;G->Release();}return (ULONG)N;}
 void RequestService()override{}
};
static void PauseServicesTests(){
 for(unsigned Final=0;Final<3;Final++){
  Parent P;Stream S;Channel D;MultiGroup G;BlockingSink A,B;
  T70_AUDIO_EXPORT_OWNER O={};PMINIPORTWAVECYCLICSTREAM OS=nullptr;PDMACHANNEL OD=nullptr;PSERVICEGROUP OG=nullptr;
  assert(T70AudioExportWrap(&O,&P,&S,&D,&G,&OS,&OD,&OG)==0);
  assert(OG->AddMember(&A)==0);auto LateA=G.Retain(0);
  A.Hold=true;std::thread T([&]{LateA->RequestService();});while(!A.In)std::this_thread::yield();
  assert(T70AudioExportPauseServices(&O)==STATUS_DEVICE_NOT_READY);
  assert(T70AudioExportResumeServices(&O)==STATUS_DEVICE_NOT_READY);
  // New registration during pause inherits the same gate, including synchronous delivery.
  G.Early=true;assert(OG->AddMember(&B)==0);auto LateB=G.Retain(1);
  LateA->RequestService();LateB->RequestService();assert(A.Calls==1&&B.Calls==0);
  A.Hold=false;T.join();assert(T70AudioExportPauseServices(&O)==0);
  LateA->RequestService();LateB->RequestService();assert(A.Calls==1&&B.Calls==0);
  assert(T70AudioExportResumeServices(&O)==0);LateA->RequestService();LateB->RequestService();
  assert(A.Calls==2&&B.Calls==1); // queued pre-pause refs become CURRENT callbacks after resume.
  assert(T70AudioExportPauseServices(&O)==0);
  if(Final==0)assert(T70AudioExportQuiesce(&O)==0);
  if(Final==1)assert(T70AudioExportDrainServices(&O)==0);
  if(Final==2)assert(T70AudioExportDrain(&O)==STATUS_DEVICE_NOT_READY);
  assert(T70AudioExportResumeServices(&O)==STATUS_DEVICE_NOT_READY);
  LateA->RequestService();LateB->RequestService();assert(A.Calls==2&&B.Calls==1);
  assert(T70AudioExportDrainServices(&O)==0);OS->Release();OD->Release();OG->Release();
  assert(T70AudioExportDrain(&O)==0);LateA->Release();LateB->Release();
  assert(P.Refs==1&&A.Refs==1&&B.Refs==1&&G.Refs==1);
 }
}
int main(){
 PauseServicesTests();
 Parent P;Stream S;Channel D;Group G;
 for(unsigned Failure=1;Failure<=3;Failure++){
  T70_AUDIO_EXPORT_OWNER O={};Alloc=Freed=0;Fail=Failure;
  PMINIPORTWAVECYCLICSTREAM OS=(PMINIPORTWAVECYCLICSTREAM)1;PDMACHANNEL OD=(PDMACHANNEL)2;PSERVICEGROUP OG=(PSERVICEGROUP)3;
  assert(T70AudioExportWrap(&O,&P,&S,&D,&G,&OS,&OD,&OG)==STATUS_INSUFFICIENT_RESOURCES);
  assert(OS==(void *)1&&OD==(void *)2&&OG==(void *)3&&!O.Gate&&Freed==Failure-1);
  assert(P.Refs==1&&S.Refs==1&&D.Refs==1&&G.Refs==1);
 }
 T70_AUDIO_EXPORT_OWNER O={};Alloc=Freed=Fail=0;
 PMINIPORTWAVECYCLICSTREAM OS=nullptr;PDMACHANNEL OD=nullptr;PSERVICEGROUP OG=nullptr;
 assert(T70AudioExportWrap(&O,&P,&S,&D,&G,&OS,&OD,&OG)==0);
 assert(OS!=&S&&OD!=&D&&OG!=&G&&O.Gate==3&&P.Refs==4&&S.Refs==2&&D.Refs==2&&G.Refs==2);
 ULONG N=0;LONGLONG Position=0;KSDATAFORMAT F={};char Bytes[4]={1,2,3,4};
 assert(OS->SetFormat(&F)==STATUS_NOT_SUPPORTED&&OS->SetNotificationFreq(10,&N)==10&&N==40);
 assert(OS->SetState(KSSTATE_PAUSE)==0&&OS->GetPosition(&N)==0&&N==16);
 assert(OS->NormalizePhysicalPosition(&Position)==0&&Position==123);OS->Silence(Bytes,4);assert(!Bytes[0]&&S.Calls==6);
 assert(OD->AllocateBuffer(32,nullptr)==STATUS_NOT_SUPPORTED&&OD->TransferCount()==3&&OD->MaximumBufferSize()==32&&OD->AllocatedBufferSize()==32);
 OD->SetBufferSize(16);assert(OD->BufferSize()==16&&OD->SystemAddress()==D.Bytes&&OD->PhysicalAddress().QuadPart==0x123400&&OD->GetAdapterObject()==(void *)0x1234);
 const char Source[4]={1,2,3,4};OD->CopyTo(D.Bytes,(void *)Source,4);OD->CopyFrom(Bytes,D.Bytes,4);assert(!memcmp(Bytes,Source,4));
 OD->FreeBuffer();assert(!D.Size);
 Sink K;assert(OG->AddMember(&K)==0&&K.Refs==3);G.RequestService();assert(K.Calls==1);OG->RequestService();assert(K.Calls==2);
 OG->SupportDelayedService();OG->RequestDelayedService(42);OG->CancelDelayedService();OG->RemoveMember(&K);assert(K.Refs==1&&G.Calls==5);
 const GUID Unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
 const GUID SinkId={0x22c6ac64,0x851b,0x11d0,{0x9a,0x7f,0,0xaa,0,0x38,0xac,0xfe}};
 void *Q=nullptr;assert(OG->QueryInterface(SinkId,&Q)==0&&Q==OG);((IServiceSink *)Q)->Release();
 IUnknown *A[]={OS,OD,OG};for(auto X:A){assert(X->QueryInterface(Unknown,&Q)==0&&Q==X);((IUnknown *)Q)->Release();}
 GUID Bad={1,0,0,{}};Q=(void *)1;assert(OS->QueryInterface(Bad,&Q)==STATUS_NOINTERFACE&&!Q);
 std::thread T([&]{for(unsigned I=0;I<40000;I++){OS->AddRef();OS->Release();}});
 for(unsigned I=0;I<40000;I++){OD->AddRef();OD->Release();}
 T.join();
 assert(T70AudioExportDrain(&O)==STATUS_DEVICE_NOT_READY);
 auto Before=Alloc;PMINIPORTWAVECYCLICSTREAM B1=nullptr;PDMACHANNEL B2=nullptr;PSERVICEGROUP B3=nullptr;
 assert(T70AudioExportWrap(&O,&P,&S,&D,&G,&B1,&B2,&B3)==STATUS_DEVICE_NOT_READY&&Alloc==Before);
 assert(OS->Release()==0);assert(T70AudioExportDrain(&O)==STATUS_DEVICE_NOT_READY);
 assert(OD->Release()==0);assert(T70AudioExportDrain(&O)==STATUS_DEVICE_NOT_READY);
 assert(OG->Release()==0);assert(T70AudioExportDrain(&O)==0&&Freed==Alloc&&P.Refs==1&&S.Refs==1&&D.Refs==1&&G.Refs==1);
 assert(T70AudioExportDrain(&O)==0);
 T70_AUDIO_EXPORT_OWNER Empty={};assert(T70AudioExportDrain(&Empty)==0);
 assert(T70AudioExportWrap(&Empty,&P,&S,&D,&G,&B1,&B2,&B3)==STATUS_DEVICE_NOT_READY);
 assert(T70AudioExportWrap(nullptr,&P,&S,&D,&G,&B1,&B2,&B3)==STATUS_INVALID_PARAMETER);
 T70_AUDIO_EXPORT_OWNER Invalid={};
 assert(T70AudioExportWrap(&Invalid,&P,&S,&D,&G,&B1,(PDMACHANNEL *)&B1,&B3)==STATUS_INVALID_PARAMETER);
 Irql=DISPATCH_LEVEL;assert(T70AudioExportDrain(&Invalid)==STATUS_INVALID_PARAMETER);Irql=0;
 for(unsigned Race=0;Race<200;Race++){
  T70_AUDIO_EXPORT_OWNER Racing={};PMINIPORTWAVECYCLICSTREAM RS=nullptr;PDMACHANNEL RD=nullptr;PSERVICEGROUP RG=nullptr;
  assert(T70AudioExportWrap(&Racing,&P,&S,&D,&G,&RS,&RD,&RG)==0);
  std::atomic<bool> Go{false};
  std::thread Releaser([&]{while(!Go.load(std::memory_order_acquire)){}RS->Release();RD->Release();RG->Release();});
  Go.store(true,std::memory_order_release);
  auto Status=T70AudioExportDrain(&Racing);assert(Status==0||Status==STATUS_DEVICE_NOT_READY);
  Releaser.join();assert(T70AudioExportDrain(&Racing)==0&&P.Refs==1&&S.Refs==1&&D.Refs==1&&G.Refs==1);
 }
 // Quiesce closes work admission, not COM lifetime or original sink delivery.
 {
  T70_AUDIO_EXPORT_OWNER Work={};PMINIPORTWAVECYCLICSTREAM WS=nullptr;PDMACHANNEL WD=nullptr;PSERVICEGROUP WG=nullptr;
  assert(T70AudioExportWrap(&Work,&P,&S,&D,&G,&WS,&WD,&WG)==0);
  Work.WorkGate=0x3fffffff;ULONG Sentinel=42;
  assert(WS->GetPosition(&Sentinel)==STATUS_DEVICE_NOT_READY&&Sentinel==42);
  Work.WorkGate=0;
  Sink Member;assert(WG->AddMember(&Member)==0&&Member.Refs==3);
  S.Hold=true;S.Entered=false;ULONG At=0;
  std::thread Call([&]{assert(WS->GetPosition(&At)==0&&At==16);});
  while(!S.Entered)std::this_thread::yield();
  assert(T70AudioExportQuiesce(&Work)==STATUS_DEVICE_NOT_READY);
  ULONG Output=0x1234;LONGLONG Wide=0x1234;
  assert(WS->GetPosition(&Output)==STATUS_DEVICE_NOT_READY&&Output==0x1234);
  assert(WS->SetFormat(&F)==STATUS_DEVICE_NOT_READY&&WS->SetState(KSSTATE_PAUSE)==STATUS_DEVICE_NOT_READY);
  assert(WS->SetNotificationFreq(10,&Output)==0&&Output==0x1234);
  assert(WS->NormalizePhysicalPosition(&Wide)==STATUS_DEVICE_NOT_READY&&Wide==0x1234);
  char Data[]={1,2,3,4};WS->Silence(Data,4);assert(Data[0]==1);
  PHYSICAL_ADDRESS Address={};Address.QuadPart=1234;
  assert(WD->AllocateBuffer(32,&Address)==STATUS_DEVICE_NOT_READY&&Address.QuadPart==1234);
  D.Size=19;WD->FreeBuffer();WD->SetBufferSize(1);assert(D.Size==19);
  assert(!WD->TransferCount()&&!WD->MaximumBufferSize()&&!WD->AllocatedBufferSize()&&!WD->BufferSize());
  assert(!WD->SystemAddress()&&!WD->GetAdapterObject()&&!WD->PhysicalAddress().QuadPart);
  char Copy[]={9,9,9,9};WD->CopyTo(Copy,Data,4);WD->CopyFrom(Copy,Data,4);assert(Copy[0]==9);
  auto GroupCalls=G.Calls;WG->RequestService();WG->SupportDelayedService();WG->RequestDelayedService(42);
  assert(G.Calls==GroupCalls&&WG->AddMember(&Member)==STATUS_DEVICE_NOT_READY);
  // Permanent quiesce seals callbacks already registered with the original group.
  G.RequestService();assert(Member.Calls==0);
  WG->CancelDelayedService();assert(G.Calls==GroupCalls+2);
  WG->RemoveMember(&Member);assert(Member.Refs==1&&!G.Member);
  assert(T70AudioExportWrap(&Work,&P,&S,&D,&G,&B1,&B2,&B3)==STATUS_DEVICE_NOT_READY);
  S.Hold=false;Call.join();assert(T70AudioExportQuiesce(&Work)==0);
  assert(T70AudioExportDrain(&Work)==STATUS_DEVICE_NOT_READY);
  assert(WS->QueryInterface(Unknown,&Q)==0);((IUnknown*)Q)->Release();
  WS->Release();WD->Release();WG->Release();assert(T70AudioExportDrain(&Work)==0&&P.Refs==1);
 }
 {
  T70_AUDIO_EXPORT_OWNER Work={};assert(T70AudioExportQuiesce(&Work)==0);
  assert(T70AudioExportQuiesce(&Work)==0&&T70AudioExportDrain(&Work)==0);
  assert(T70AudioExportQuiesce(nullptr)==STATUS_INVALID_PARAMETER);
  Irql=DISPATCH_LEVEL;assert(T70AudioExportQuiesce(&Work)==STATUS_INVALID_PARAMETER);Irql=0;
 }
 {
  T70_AUDIO_EXPORT_OWNER Owner={};MultiGroup Actual;BlockingSink A,B;
  PMINIPORTWAVECYCLICSTREAM Sx=nullptr;PDMACHANNEL Dx=nullptr;PSERVICEGROUP Gx=nullptr;
  assert(T70AudioExportWrap(&Owner,&P,&S,&D,&Actual,&Sx,&Dx,&Gx)==0);
  assert(Gx->AddMember(&A)==0&&Gx->AddMember(&B)==0);
  assert(Gx->AddMember(&A)==STATUS_INVALID_DEVICE_STATE&&Actual.Adds==2);
  auto Late=Actual.Retain(0);A.Hold=true;
  std::thread Callback([&]{Late->RequestService();});while(!A.In)std::this_thread::yield();
  assert(T70AudioExportDrainServices(&Owner)==STATUS_DEVICE_NOT_READY&&Actual.Removes==2);
  assert(Actual.Members.empty());Late->RequestService(); // rejected, not blocked
  assert(A.Calls==1&&B.Calls==0);A.Hold=false;Callback.join();
  assert(T70AudioExportDrainServices(&Owner)==0&&Actual.Removes==2);
  assert(T70AudioExportDrainServices(&Owner)==0);
  Sx->Release();Dx->Release();Gx->Release();assert(T70AudioExportDrain(&Owner)==0);
  assert(A.Refs==2&&B.Refs==1);Late->RequestService();assert(A.Calls==1);Late->Release();assert(A.Refs==1&&P.Refs==1);
  assert(T70AudioExportDrainServices(&Owner)==0);
 }
 {
  T70_AUDIO_EXPORT_OWNER Owner={};MultiGroup Actual;BlockingSink A;
  PMINIPORTWAVECYCLICSTREAM Sx=nullptr;PDMACHANNEL Dx=nullptr;PSERVICEGROUP Gx=nullptr;
  assert(T70AudioExportWrap(&Owner,&P,&S,&D,&Actual,&Sx,&Dx,&Gx)==0);
  Actual.Early=true;A.Reenter=Gx;
  assert(Gx->AddMember(&A)==0&&Actual.Removes==1&&Actual.Members.empty());
  A.Reenter=nullptr;Actual.Early=false;
  // Fully removed identity may be explicitly registered again.
  assert(Gx->AddMember(&A)==0&&Actual.Adds==2);
  Gx->RemoveMember(&A);Gx->RemoveMember(&A);assert(Actual.Removes==2);
  // Allocation failure before publication and sink allocation failure afterward.
  Fail=Alloc+1;assert(Gx->AddMember(&A)==STATUS_INSUFFICIENT_RESOURCES);Fail=0;
  Fail=Alloc+2;assert(Gx->AddMember(&A)==STATUS_INSUFFICIENT_RESOURCES);Fail=0;
  Actual.Status=STATUS_NOT_SUPPORTED;assert(Gx->AddMember(&A)==STATUS_NOT_SUPPORTED);Actual.Status=0;
  assert(T70AudioExportDrainServices(&Owner)==0);Sx->Release();Dx->Release();Gx->Release();
  assert(T70AudioExportDrain(&Owner)==0&&P.Refs==1&&A.Refs==1&&Actual.Refs==1);
 }
 {
  T70_AUDIO_EXPORT_OWNER Owner={};MultiGroup Actual;BlockingSink A;
  PMINIPORTWAVECYCLICSTREAM Sx=nullptr;PDMACHANNEL Dx=nullptr;PSERVICEGROUP Gx=nullptr;
  assert(T70AudioExportWrap(&Owner,&P,&S,&D,&Actual,&Sx,&Dx,&Gx)==0);
  Actual.HoldAdd=true;
  std::thread Adding([&]{assert(Gx->AddMember(&A)==0);});
  while(!Actual.InAdd)std::this_thread::yield();
  assert(Gx->AddMember(&A)==STATUS_INVALID_DEVICE_STATE);
  Gx->RemoveMember(&A);assert(T70AudioExportDrainServices(&Owner)==STATUS_DEVICE_NOT_READY);
  Actual.HoldAdd=false;Adding.join();assert(Actual.Removes==1&&Actual.Members.empty());
  assert(T70AudioExportDrainServices(&Owner)==0);
  Sx->Release();Dx->Release();Gx->Release();assert(T70AudioExportDrain(&Owner)==0&&A.Refs==1&&P.Refs==1);
 }
 {
  T70_AUDIO_EXPORT_OWNER Owner={};MultiGroup Actual;
  PMINIPORTWAVECYCLICSTREAM Sx=nullptr;PDMACHANNEL Dx=nullptr;PSERVICEGROUP Gx=nullptr;
  assert(T70AudioExportWrap(&Owner,&P,&S,&D,&Actual,&Sx,&Dx,&Gx)==0);
  bool Destroyed=false;auto Cyclic=new CyclicSink(Gx,&Destroyed);
  assert(Gx->AddMember(Cyclic)==0);Cyclic->Release();
  assert(T70AudioExportDrainServices(&Owner)==0&&Destroyed);
  Sx->Release();Dx->Release();Gx->Release();assert(T70AudioExportDrain(&Owner)==0&&P.Refs==1);
 }
 {
  // Unknown informational registration ownership is intentionally quarantined.
  // Static roots retain the real lifetimes; no fictional RemoveMember/free.
  static T70_AUDIO_EXPORT_OWNER Owner={};static Parent QP;static Stream QS;
  static Channel QD;static MultiGroup Actual;static BlockingSink A;
  PMINIPORTWAVECYCLICSTREAM Sx=nullptr;PDMACHANNEL Dx=nullptr;PSERVICEGROUP Gx=nullptr;
  assert(T70AudioExportWrap(&Owner,&QP,&QS,&QD,&Actual,&Sx,&Dx,&Gx)==0);
  Actual.Status=1;
  assert(Gx->AddMember(&A)==STATUS_DEVICE_NOT_READY);
  assert(T70AudioExportDrainServices(&Owner)==STATUS_DEVICE_NOT_READY&&!Actual.Removes);
  Sx->Release();Dx->Release();Gx->Release();
  assert(T70AudioExportDrain(&Owner)==STATUS_DEVICE_NOT_READY&&Owner.Members&&QP.Refs>1&&A.Refs>1);
 }
 puts("AudioExportOwner: COM forwarding, partial-allocation cleanup, tracked group members/callback rundown/ownership cycles, work quiesce, admission/drain 80000 concurrent reference pairs and 200 drain/final-release races passed");
}
