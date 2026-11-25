// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioServiceSinkOwner.h"
#include <string.h>
namespace {
constexpr LONG Closed=0x40000000,Paused=0x20000000,CountMask=0x1fffffff;
constexpr ULONG Tag=0x53413754;
enum {New,Registered,Unregistered,Unknown,Finished,Failed};
struct Lock {
 T70_AUDIO_SERVICE_SINK_OWNER *O;bool Held;
 explicit Lock(T70_AUDIO_SERVICE_SINK_OWNER *P):O(P),Held(InterlockedCompareExchange(&P->Busy,1,0)==0){}
 ~Lock(){if(Held)InterlockedCompareExchange(&O->Busy,0,1);}
};
class Sink final:public IServiceSink {
 volatile LONG Refs=1,Gate=0;
 PUNKNOWN Parent;PSERVICESINK Target;T70_AUDIO_SERVICE_GATE *Shared;
 static void *operator new(size_t,void *P)noexcept{return P;}
 Sink(PUNKNOWN P,PSERVICESINK T,T70_AUDIO_SERVICE_GATE *S):Parent(P),Target(T),Shared(S){P->AddRef();T->AddRef();}
public:
 static Sink *Create(PUNKNOWN P,PSERVICESINK T,T70_AUDIO_SERVICE_GATE *S){
  void *M=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(Sink),Tag);
  return M?new(M) Sink(P,T,S):nullptr;
 }
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID I,void **Out)override{
  if(!Out)return STATUS_INVALID_PARAMETER;*Out=nullptr;
  static const GUID U={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
  static const GUID S={0x22c6ac64,0x851b,0x11d0,{0x9a,0x7f,0,0xaa,0,0x38,0xac,0xfe}};
  if(memcmp(&I,&U,sizeof I)&&memcmp(&I,&S,sizeof I))return STATUS_NOINTERFACE;
  *Out=static_cast<IServiceSink *>(this);AddRef();return STATUS_SUCCESS;
 }
 STDMETHODIMP_(ULONG) AddRef()override{return (ULONG)InterlockedIncrement(&Refs);}
 STDMETHODIMP_(ULONG) Release()override{
  LONG N=InterlockedDecrement(&Refs);if(!N){auto P=Parent;auto T=Target;
   this->~Sink();ExFreePoolWithTag(this,Tag);T->Release();P->Release();}return (ULONG)N;
 }
 STDMETHODIMP_(void) RequestService()override{
  // Own a reference even if callback reenters RemoveMember/Close.
  AddRef();LONG V=InterlockedCompareExchange(&Gate,0,0);bool Entered=false;
  for(;;){if(V<0||(V&Closed)||V==Closed-1)break;
   LONG Old=InterlockedCompareExchange(&Gate,V+1,V);if(Old==V){Entered=true;break;}V=Old;}
  if(Entered){
   bool SharedEntered=!Shared;
   if(Shared){
    LONG S=InterlockedCompareExchange(&Shared->State,0,0);
    for(;;){
     if(S<0||(S&(Closed|Paused))||(S&CountMask)==CountMask)break;
     LONG Old=InterlockedCompareExchange(&Shared->State,S+1,S);
     if(Old==S){SharedEntered=true;break;}S=Old;
    }
   }
   if(SharedEntered){Target->RequestService();if(Shared)InterlockedDecrement(&Shared->State);}
   InterlockedDecrement(&Gate);
  }
  Release();
 }
 bool Quiesce(){
  LONG V=InterlockedCompareExchange(&Gate,0,0);
  for(;;){LONG Old=InterlockedCompareExchange(&Gate,V|Closed,V);if(Old==V)break;V=Old;}
  return InterlockedCompareExchange(&Gate,0,0)==Closed;
 }
};
}
NTSTATUS T70AudioServiceSinkOpen(T70_AUDIO_SERVICE_SINK_OWNER *O,PUNKNOWN P,
 PSERVICEGROUP G,PSERVICESINK T,T70_AUDIO_SERVICE_GATE *Shared){
 if(!O||!P||!G||!T||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Shared&&(InterlockedCompareExchange(&Shared->State,0,0)&Closed))return STATUS_DEVICE_NOT_READY;
 Lock L(O);if(!L.Held)return STATUS_DEVICE_NOT_READY;
 if(O->State!=New||O->Group||O->Proxy)return STATUS_INVALID_DEVICE_STATE;
 O->State=Failed;auto S=Sink::Create(P,T,Shared);if(!S)return STATUS_INSUFFICIENT_RESOURCES;
 G->AddRef();O->Group=G;O->Proxy=S;
 NTSTATUS R=G->AddMember(S);
 if(R==STATUS_SUCCESS){O->State=Registered;return R;}
 S->Quiesce();
 if(NT_SUCCESS(R)){O->State=Unknown;return STATUS_DEVICE_NOT_READY;}
 O->State=Unregistered;
 if(!S->Quiesce())return R; // early callback can outlive failed registration
 O->Group=nullptr;O->Proxy=nullptr;G->Release();S->Release();return R;
}
NTSTATUS T70AudioServiceSinkClose(T70_AUDIO_SERVICE_SINK_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Lock L(O);if(!L.Held)return STATUS_DEVICE_NOT_READY;
 if(O->State==Finished)return STATUS_SUCCESS;
 if(!O->Proxy){O->State=Finished;return STATUS_SUCCESS;}
 auto S=static_cast<Sink *>(O->Proxy);S->Quiesce();
 if(O->State==Unknown)return STATUS_DEVICE_NOT_READY;
 if(O->State==Registered){
  // Busy prevents recursive/concurrent RemoveMember of the same registration.
  O->Group->RemoveMember(S);O->State=Unregistered;
 }
 if(!S->Quiesce())return STATUS_DEVICE_NOT_READY;
 auto G=O->Group;O->Group=nullptr;O->Proxy=nullptr;O->State=Finished;
 G->Release();S->Release();return STATUS_SUCCESS;
}

static NTSTATUS SetSharedFlag(T70_AUDIO_SERVICE_GATE *G,LONG Flag){
 if(!G||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 LONG S=InterlockedCompareExchange(&G->State,0,0);
 for(;;){
  if(S<0||((S&Closed)&&Flag!=Closed))return STATUS_DEVICE_NOT_READY;
  LONG Old=InterlockedCompareExchange(&G->State,S|Flag,S);if(Old==S)break;S=Old;
 }
 return (InterlockedCompareExchange(&G->State,0,0)&CountMask)?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
}
NTSTATUS T70AudioServiceGatePause(T70_AUDIO_SERVICE_GATE *G){return SetSharedFlag(G,Paused);}
NTSTATUS T70AudioServiceGateSeal(T70_AUDIO_SERVICE_GATE *G){return SetSharedFlag(G,Closed);}
NTSTATUS T70AudioServiceGateResume(T70_AUDIO_SERVICE_GATE *G){
 if(!G||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 LONG S=InterlockedCompareExchange(&G->State,0,0);
 for(;;){
  if(S<0||(S&Closed)||(S&CountMask))return STATUS_DEVICE_NOT_READY;
  LONG Old=InterlockedCompareExchange(&G->State,S&~Paused,S);
  if(Old==S)return STATUS_SUCCESS;S=Old;
 }
}
