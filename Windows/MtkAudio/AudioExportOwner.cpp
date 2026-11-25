// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioExportOwner.h"
#include "AudioServiceSinkOwner.h"
#include <stdint.h>
#include <string.h>
namespace {
constexpr LONG Closing=0x40000000;
constexpr ULONG Tag=0x58413754; // T7AX
const GUID Unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
const GUID StreamId={0xb4c90a28,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
const GUID ChannelId={0x22c6ac61,0x851b,0x11d0,{0x9a,0x7f,0,0xaa,0,0x38,0xac,0xfe}};
const GUID SinkId={0x22c6ac64,0x851b,0x11d0,{0x9a,0x7f,0,0xaa,0,0x38,0xac,0xfe}};
const GUID GroupId={0x22c6ac65,0x851b,0x11d0,{0x9a,0x7f,0,0xaa,0,0x38,0xac,0xfe}};
bool Same(REFIID A,const GUID &B){return !memcmp(&A,&B,sizeof A);}
// Lock-free admission works at the forwarded interface's original IRQL.
struct Work {
 T70_AUDIO_EXPORT_OWNER *Owner;
 bool Entered=false;
 explicit Work(T70_AUDIO_EXPORT_OWNER *O):Owner(O){
  LONG V=InterlockedCompareExchange(&O->WorkGate,0,0);
  for(;;){
   if(V<0||(V&Closing)||V==Closing-1)return;
   LONG Old=InterlockedCompareExchange(&O->WorkGate,V+1,V);
   if(Old==V){Entered=true;return;}V=Old;
  }
 }
 ~Work(){if(Entered)InterlockedDecrement(&Owner->WorkGate);}
};
struct Life {
 volatile LONG Refs=1;T70_AUDIO_EXPORT_OWNER *Owner;PUNKNOWN Parent;
 Life(T70_AUDIO_EXPORT_OWNER *O,PUNKNOWN P):Owner(O),Parent(P){P->AddRef();}
};
#define COMMON(Type,Interface,Id,Extra) \
 static void *operator new(size_t,void *P)noexcept{return P;} \
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID I,void **Out)override{ \
  if(!Out)return STATUS_INVALID_PARAMETER;*Out=nullptr; \
  if(!Same(I,Unknown)&&!Same(I,Id)&&!(Extra))return STATUS_NOINTERFACE; \
  *Out=static_cast<Interface *>(this);AddRef();return STATUS_SUCCESS;} \
 STDMETHODIMP_(ULONG) AddRef()override{return (ULONG)InterlockedIncrement(&L.Refs);} \
 STDMETHODIMP_(ULONG) Release()override{ \
  LONG N=InterlockedDecrement(&L.Refs);if(!N){ \
   auto O=L.Owner;auto P=L.Parent;Target->Release();this->~Type(); \
   ExFreePoolWithTag(this,Tag);InterlockedDecrement(&O->Gate);P->Release();} \
  return (ULONG)N;}
class Stream final:public IMiniportWaveCyclicStream {
 Life L;PMINIPORTWAVECYCLICSTREAM Target;
public:
 Stream(T70_AUDIO_EXPORT_OWNER *O,PUNKNOWN P,PMINIPORTWAVECYCLICSTREAM T):L(O,P),Target(T){T->AddRef();}
 COMMON(Stream,IMiniportWaveCyclicStream,StreamId,false)
 STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT F)override{Work W(L.Owner);if(!W.Entered){return STATUS_DEVICE_NOT_READY;}return Target->SetFormat(F);}
 STDMETHODIMP_(ULONG) SetNotificationFreq(ULONG I,PULONG N)override{Work W(L.Owner);if(!W.Entered){return 0;}return Target->SetNotificationFreq(I,N);}
 STDMETHODIMP_(NTSTATUS) SetState(KSSTATE S)override{Work W(L.Owner);if(!W.Entered){return STATUS_DEVICE_NOT_READY;}return Target->SetState(S);}
 STDMETHODIMP_(NTSTATUS) GetPosition(PULONG P)override{Work W(L.Owner);if(!W.Entered){return STATUS_DEVICE_NOT_READY;}return Target->GetPosition(P);}
 STDMETHODIMP_(NTSTATUS) NormalizePhysicalPosition(PLONGLONG P)override{Work W(L.Owner);if(!W.Entered){return STATUS_DEVICE_NOT_READY;}return Target->NormalizePhysicalPosition(P);}
 STDMETHODIMP_(void) Silence(void *P,ULONG N)override{Work W(L.Owner);if(!W.Entered){return;}Target->Silence(P,N);}
};
class Channel final:public IDmaChannel {
 Life L;PDMACHANNEL Target;
public:
 Channel(T70_AUDIO_EXPORT_OWNER *O,PUNKNOWN P,PDMACHANNEL T):L(O,P),Target(T){T->AddRef();}
 COMMON(Channel,IDmaChannel,ChannelId,false)
 STDMETHODIMP_(NTSTATUS) AllocateBuffer(ULONG N,PPHYSICAL_ADDRESS A)override{Work W(L.Owner);if(!W.Entered){return STATUS_DEVICE_NOT_READY;}return Target->AllocateBuffer(N,A);}
 STDMETHODIMP_(void) FreeBuffer()override{Work W(L.Owner);if(!W.Entered){return;}Target->FreeBuffer();}
 STDMETHODIMP_(ULONG) TransferCount()override{Work W(L.Owner);if(!W.Entered){return 0;}return Target->TransferCount();}
 STDMETHODIMP_(ULONG) MaximumBufferSize()override{Work W(L.Owner);if(!W.Entered){return 0;}return Target->MaximumBufferSize();}
 STDMETHODIMP_(ULONG) AllocatedBufferSize()override{Work W(L.Owner);if(!W.Entered){return 0;}return Target->AllocatedBufferSize();}
 STDMETHODIMP_(ULONG) BufferSize()override{Work W(L.Owner);if(!W.Entered){return 0;}return Target->BufferSize();}
 STDMETHODIMP_(void) SetBufferSize(ULONG N)override{Work W(L.Owner);if(!W.Entered){return;}Target->SetBufferSize(N);}
 STDMETHODIMP_(void *) SystemAddress()override{Work W(L.Owner);if(!W.Entered){return nullptr;}return Target->SystemAddress();}
 STDMETHODIMP_(PHYSICAL_ADDRESS) PhysicalAddress()override{Work W(L.Owner);if(!W.Entered){return PHYSICAL_ADDRESS{};}return Target->PhysicalAddress();}
 STDMETHODIMP_(PADAPTER_OBJECT) GetAdapterObject()override{Work W(L.Owner);if(!W.Entered){return nullptr;}return Target->GetAdapterObject();}
 STDMETHODIMP_(void) CopyTo(void *D,void *S,ULONG N)override{Work W(L.Owner);if(!W.Entered){return;}Target->CopyTo(D,S,N);}
 STDMETHODIMP_(void) CopyFrom(void *D,void *S,ULONG N)override{Work W(L.Owner);if(!W.Entered){return;}Target->CopyFrom(D,S,N);}
};
struct Collection {
 T70_AUDIO_EXPORT_OWNER *Owner;bool Entered=false;
 explicit Collection(T70_AUDIO_EXPORT_OWNER *O):Owner(O){
  LONG V=InterlockedCompareExchange(&O->CollectionGate,0,0);
  for(;;){if(V<0||(V&Closing)||V==Closing-1)return;
   LONG Old=InterlockedCompareExchange(&O->CollectionGate,V+1,V);if(Old==V){Entered=true;return;}V=Old;}
 }
 ~Collection(){if(Entered)InterlockedDecrement(&Owner->CollectionGate);}
};
struct Member {
 Member *Next;
 PUNKNOWN Parent;PSERVICEGROUP Group;PSERVICESINK Original;
 T70_AUDIO_SERVICE_SINK_OWNER Registration;
 volatile LONG RemoveRequested,AddDone,Closed;
};
static Member *Head(T70_AUDIO_EXPORT_OWNER *O){return static_cast<Member *>(InterlockedCompareExchangePointer(&O->Members,nullptr,nullptr));}
static NTSTATUS Remove(Member *M){
 LONG ClosedState=InterlockedCompareExchange(&M->Closed,0,0);
 if(ClosedState)return ClosedState==2?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
 InterlockedCompareExchange(&M->RemoveRequested,1,0);
 if(!InterlockedCompareExchange(&M->AddDone,0,0))return STATUS_DEVICE_NOT_READY;
 NTSTATUS S=T70AudioServiceSinkClose(&M->Registration);
 if(S==STATUS_SUCCESS){
  if(InterlockedCompareExchange(&M->Closed,1,0)==0){
   // Callback admission/rundown is complete. Do not retain a PortCls sink
   // until exported proxy destruction: it may itself own those proxies.
   M->Original->Release();M->Group->Release();
   InterlockedCompareExchange(&M->Closed,2,1);
  }else if(InterlockedCompareExchange(&M->Closed,0,0)!=2)return STATUS_DEVICE_NOT_READY;
 }
 return S;
}
static void FreeMember(Member *M){
 auto P=M->Parent;auto G=M->Group;auto S=M->Original;
 bool Released=InterlockedCompareExchange(&M->Closed,0,0)==2;
 ExFreePoolWithTag(M,Tag);if(!Released){S->Release();G->Release();}P->Release();
}
static NTSTATUS Add(T70_AUDIO_EXPORT_OWNER *O,PUNKNOWN P,PSERVICEGROUP G,PSERVICESINK S){
 if(!S||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Collection Guard(O);if(!Guard.Entered)return STATUS_DEVICE_NOT_READY;
 auto M=static_cast<Member *>(ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(Member),Tag));
 if(!M)return STATUS_INSUFFICIENT_RESOURCES;
 memset(M,0,sizeof(*M));M->Parent=P;M->Group=G;M->Original=S;
 P->AddRef();G->AddRef();S->AddRef();
 Member *H=Head(O);
 for(;;){
  for(auto N=H;N;N=N->Next)if(!InterlockedCompareExchange(&N->Closed,0,0)&&N->Group==G&&N->Original==S){
   FreeMember(M);return STATUS_INVALID_DEVICE_STATE;
  }
  M->Next=H;
  auto Old=static_cast<Member *>(InterlockedCompareExchangePointer(&O->Members,M,H));
  if(Old==H)break;H=Old;
 }
 NTSTATUS Status=T70AudioServiceSinkOpen(&M->Registration,P,G,S,&O->ServiceGate);
 InterlockedCompareExchange(&M->AddDone,1,0);
 if(Status!=STATUS_SUCCESS||InterlockedCompareExchange(&M->RemoveRequested,0,0)){
  (void)Remove(M);
 }
 return Status;
}
static void RemoveMatching(T70_AUDIO_EXPORT_OWNER *O,PSERVICEGROUP G,PSERVICESINK S){
 if(!S||KeGetCurrentIrql()!=PASSIVE_LEVEL)return;
 Collection Guard(O);if(!Guard.Entered)return;
 for(auto M=Head(O);M;M=M->Next)if(!InterlockedCompareExchange(&M->Closed,0,0)&&M->Group==G&&M->Original==S)
  (void)Remove(M);
}
class Group final:public IServiceGroup {
 Life L;PSERVICEGROUP Target;
public:
 Group(T70_AUDIO_EXPORT_OWNER *O,PUNKNOWN P,PSERVICEGROUP T):L(O,P),Target(T){T->AddRef();}
 COMMON(Group,IServiceGroup,GroupId,Same(I,SinkId))
 STDMETHODIMP_(void) RequestService()override{Work W(L.Owner);if(!W.Entered){return;}Target->RequestService();}
 STDMETHODIMP_(NTSTATUS) AddMember(PSERVICESINK S)override{Work W(L.Owner);if(!W.Entered){return STATUS_DEVICE_NOT_READY;}return Add(L.Owner,L.Parent,Target,S);}
 STDMETHODIMP_(void) RemoveMember(PSERVICESINK S)override{RemoveMatching(L.Owner,Target,S);}
 STDMETHODIMP_(void) SupportDelayedService()override{Work W(L.Owner);if(!W.Entered){return;}Target->SupportDelayedService();}
 STDMETHODIMP_(void) RequestDelayedService(ULONGLONG N)override{Work W(L.Owner);if(!W.Entered){return;}Target->RequestDelayedService(N);}
 STDMETHODIMP_(void) CancelDelayedService()override{Target->CancelDelayedService();}
};
#undef COMMON
}
NTSTATUS T70AudioExportWrap(T70_AUDIO_EXPORT_OWNER *O,PUNKNOWN P,
 PMINIPORTWAVECYCLICSTREAM S,PDMACHANNEL D,PSERVICEGROUP G,
 PMINIPORTWAVECYCLICSTREAM *OutS,PDMACHANNEL *OutD,PSERVICEGROUP *OutG){
 if(!O||!P||!S||!D||!G||!OutS||!OutD||!OutG||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 uintptr_t A[]={(uintptr_t)O,(uintptr_t)OutS,(uintptr_t)OutD,(uintptr_t)OutG};
 size_t N[]={sizeof(*O),sizeof(*OutS),sizeof(*OutD),sizeof(*OutG)};
 for(unsigned I=0;I<4;I++){
  if(N[I]>UINTPTR_MAX-A[I])return STATUS_INVALID_PARAMETER;
  for(unsigned J=0;J<I;J++)if(A[I]<A[J]+N[J]&&A[J]<A[I]+N[I])return STATUS_INVALID_PARAMETER;
 }
 LONG V=InterlockedCompareExchange(&O->Gate,0,0);
 for(;;){if(V<0||(V&Closing)||V>Closing-4)return STATUS_DEVICE_NOT_READY;
  LONG Old=InterlockedCompareExchange(&O->Gate,V+3,V);if(Old==V)break;V=Old;}
 void *MS=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(Stream),Tag);
 void *MD=MS?ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(Channel),Tag):nullptr;
 void *MG=MD?ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(Group),Tag):nullptr;
 if(!MG){if(MD)ExFreePoolWithTag(MD,Tag);if(MS)ExFreePoolWithTag(MS,Tag);
  for(unsigned I=0;I<3;I++)InterlockedDecrement(&O->Gate);return STATUS_INSUFFICIENT_RESOURCES;}
 auto WS=new(MS) Stream(O,P,S);auto WD=new(MD) Channel(O,P,D);auto WG=new(MG) Group(O,P,G);
 *OutS=WS;*OutD=WD;*OutG=WG;return STATUS_SUCCESS;
}
NTSTATUS T70AudioExportPauseServices(T70_AUDIO_EXPORT_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 return T70AudioServiceGatePause(&O->ServiceGate);
}
NTSTATUS T70AudioExportResumeServices(T70_AUDIO_EXPORT_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if((InterlockedCompareExchange(&O->Gate,0,0)&Closing)||
    (InterlockedCompareExchange(&O->WorkGate,0,0)&Closing)||
    (InterlockedCompareExchange(&O->CollectionGate,0,0)&Closing))return STATUS_DEVICE_NOT_READY;
 return T70AudioServiceGateResume(&O->ServiceGate);
}
NTSTATUS T70AudioExportQuiesce(T70_AUDIO_EXPORT_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 (void)T70AudioServiceGateSeal(&O->ServiceGate); // Seal now; callback drain remains separate.
 // Close Wrap admission too; existing proxies remain independently referenced.
 LONG V=InterlockedCompareExchange(&O->Gate,0,0);
 for(;;){if(V<0)return STATUS_INVALID_DEVICE_STATE;
  LONG Old=InterlockedCompareExchange(&O->Gate,V|Closing,V);if(Old==V)break;V=Old;}
 V=InterlockedCompareExchange(&O->WorkGate,0,0);
 for(;;){if(V<0)return STATUS_INVALID_DEVICE_STATE;
  LONG Old=InterlockedCompareExchange(&O->WorkGate,V|Closing,V);if(Old==V)break;V=Old;}
 return InterlockedCompareExchange(&O->WorkGate,0,0)==Closing?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}
static NTSTATUS DrainMembers(T70_AUDIO_EXPORT_OWNER *O){
 NTSTATUS S;NTSTATUS Result=STATUS_SUCCESS;
 for(auto M=Head(O);M;M=M->Next){S=Remove(M);if(S!=STATUS_SUCCESS)Result=S;}
 return Result;
}
NTSTATUS T70AudioExportDrainServices(T70_AUDIO_EXPORT_OWNER *O){
 NTSTATUS S=T70AudioExportQuiesce(O);if(S!=STATUS_SUCCESS)return S;
 Collection Guard(O);if(!Guard.Entered)return Head(O)?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
 return DrainMembers(O);
}
NTSTATUS T70AudioExportDrain(T70_AUDIO_EXPORT_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 NTSTATUS Service=T70AudioServiceGateSeal(&O->ServiceGate);
 LONG V=InterlockedCompareExchange(&O->Gate,0,0);
 for(;;){if(V<0)return STATUS_INVALID_DEVICE_STATE;
  LONG Old=InterlockedCompareExchange(&O->Gate,V|Closing,V);if(Old==V)break;V=Old;}
 if(Service!=STATUS_SUCCESS)return Service;
 if(InterlockedCompareExchange(&O->Gate,0,0)!=Closing)return STATUS_DEVICE_NOT_READY;
 if(InterlockedCompareExchange(&O->ReclaimBusy,1,0)!=0)return STATUS_DEVICE_NOT_READY;
 V=InterlockedCompareExchange(&O->CollectionGate,0,0);
 for(;;){LONG Old=InterlockedCompareExchange(&O->CollectionGate,V|Closing,V);if(Old==V)break;V=Old;}
 NTSTATUS S=InterlockedCompareExchange(&O->CollectionGate,0,0)==Closing?DrainMembers(O):STATUS_DEVICE_NOT_READY;
 if(S==STATUS_SUCCESS){
  auto M=Head(O);InterlockedCompareExchangePointer(&O->Members,nullptr,M);
  while(M){auto Next=M->Next;FreeMember(M);M=Next;}
 }
 InterlockedCompareExchange(&O->ReclaimBusy,0,1);return S;
}
