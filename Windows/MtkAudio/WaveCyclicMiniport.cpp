// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicMiniport.h"
#include "PcmIntersection.h"
#include <stdint.h>
#include <string.h>
static constexpr ULONG Tag=0x4d433754;
static const GUID Unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID Miniport={0xb4c90a24,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
static const GUID Cyclic={0xb4c90a27,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
static NTSTATUS Exact(NTSTATUS S){return S==STATUS_SUCCESS?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);}
T70WaveCyclicMiniport::T70WaveCyclicMiniport(PUNKNOWN O,const T70_WAVECYCLIC_MINIPORT_PROVIDER &P)
 :Refs(1),Owner(O),Port(nullptr),Provider(P),Attempted(false),Initialized(false),Closing(false),Ranges{},Pointers{},Pins{},Bridge{},BridgePointer{},Dac{},Connections{},Filter{}{
 const ULONG Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 const GUID Audio={0x73647561,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
 const GUID Pcm={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
 const GUID Wave={0x05589f81,0xc356,0x11ce,{0xbf,1,0,0xaa,0,0x55,0x59,0x5a}};
 for(unsigned I=0;I<11;I++){
  auto &R=Ranges[I];R.DataRange.FormatSize=sizeof R;R.DataRange.MajorFormat=Audio;
  R.DataRange.SubFormat=Pcm;R.DataRange.Specifier=Wave;R.MaximumChannels=2;
  R.MinimumBitsPerSample=R.MaximumBitsPerSample=16;
  R.MinimumSampleFrequency=R.MaximumSampleFrequency=Rates[I];Pointers[I]=&R.DataRange;
 }
 auto &Pin=Pins[0];
 Pin.MaxGlobalInstanceCount=Pin.MaxFilterInstanceCount=1;
 Pin.KsPinDescriptor.DataRangesCount=11;Pin.KsPinDescriptor.DataRanges=Pointers;
 Pin.KsPinDescriptor.DataFlow=KSPIN_DATAFLOW_IN;Pin.KsPinDescriptor.Communication=KSPIN_COMMUNICATION_SINK;
 static const GUID Category={0x6994ad04,0x93ef,0x11d0,{0xa3,0xcc,0,0xa0,0xc9,0x22,0x31,0x96}};
 static const GUID DacType={0x507ae360,0xc554,0x11d0,{0x8a,0x2b,0,0xa0,0xc9,0x25,0x5a,0xc1}};
 const GUID Analog={0x6dba3190,0x67bd,0x11cf,{0xa0,0xf7,0,0x20,0xaf,0xd1,0x56,0xe4}};
 const GUID None={0x0f6417d6,0xc318,0x11d0,{0xa4,0x3f,0,0xa0,0xc9,0x22,0x31,0x96}};
 Pin.KsPinDescriptor.Category=&Category;
 Bridge.FormatSize=sizeof Bridge;Bridge.MajorFormat=Audio;Bridge.SubFormat=Analog;Bridge.Specifier=None;
 BridgePointer=&Bridge;auto &B=Pins[1].KsPinDescriptor;B.DataRangesCount=1;B.DataRanges=&BridgePointer;
 B.DataFlow=KSPIN_DATAFLOW_OUT;B.Communication=KSPIN_COMMUNICATION_NONE;B.Category=&Category;
 Dac.Type=&DacType;Connections[0]={PCFILTER_NODE,0,0,1};Connections[1]={0,0,PCFILTER_NODE,1};
 Filter.PinSize=sizeof Pin;Filter.PinCount=2;Filter.Pins=Pins;
 Filter.NodeSize=sizeof Dac;Filter.NodeCount=1;Filter.Nodes=&Dac;
 Filter.ConnectionCount=2;Filter.Connections=Connections;
}
NTSTATUS T70WaveCyclicMiniport::Create(PUNKNOWN O,const T70_WAVECYCLIC_MINIPORT_PROVIDER &P,T70WaveCyclicMiniport **Out){
 if(!O||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!P.Initialize||!P.Create)return STATUS_DEVICE_NOT_READY;
 void *M=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(T70WaveCyclicMiniport),Tag);
 if(!M)return STATUS_INSUFFICIENT_RESOURCES;
 O->AddRef();*Out=new(M) T70WaveCyclicMiniport(O,P);return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicMiniport::QueryInterface(REFIID Id,void **Out){
 if(!Out)return STATUS_INVALID_PARAMETER;*Out=nullptr;
 if(memcmp(&Id,&Unknown,sizeof Id)&&memcmp(&Id,&Miniport,sizeof Id)&&memcmp(&Id,&Cyclic,sizeof Id))return STATUS_NOINTERFACE;
 *Out=static_cast<IMiniportWaveCyclic *>(this);AddRef();return STATUS_SUCCESS;
}
ULONG T70WaveCyclicMiniport::AddRef(){return (ULONG)InterlockedIncrement(&Refs);}
ULONG T70WaveCyclicMiniport::Release(){
 LONG N=InterlockedDecrement(&Refs);
 if(!N){auto O=Owner;auto P=Port;this->~T70WaveCyclicMiniport();ExFreePoolWithTag(this,Tag);if(P)P->Release();O->Release();}
 return (ULONG)N;
}
NTSTATUS T70WaveCyclicMiniport::Init(PUNKNOWN A,PRESOURCELIST R,PPORTWAVECYCLIC P){
 if(!R||!P||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Call Guard(&Calls,Owner);if(!Guard.Entered)return STATUS_INVALID_DEVICE_STATE;
 if(Attempted||Closing)return STATUS_INVALID_DEVICE_STATE;
 Attempted=true;P->AddRef();NTSTATUS S=Exact(Provider.Initialize(Provider.Context,A,R,P));
 if(S!=STATUS_SUCCESS){P->Release();return S;}Port=P;Initialized=true;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicMiniport::GetDescription(PPCFILTER_DESCRIPTOR *Out){
 if(!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Call Guard(&Calls,Owner);if(!Guard.Entered)return STATUS_DEVICE_NOT_READY;
 if(!Initialized||Closing)return STATUS_DEVICE_NOT_READY;*Out=&Filter;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicMiniport::DataRangeIntersection(ULONG PinId,PKSDATARANGE Client,
 PKSDATARANGE Matching,ULONG Capacity,void *Output,PULONG Length){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Call Guard(&Calls,Owner);if(!Guard.Entered)return STATUS_DEVICE_NOT_READY;
 return T70PcmIntersection(Ranges,this,sizeof(*this),Initialized&&!Closing,
  PinId,Client,Matching,Capacity,Output,Length);
}
NTSTATUS T70WaveCyclicMiniport::NewStream(PMINIPORTWAVECYCLICSTREAM *Out,PUNKNOWN Outer,POOL_TYPE Pool,ULONG PinId,BOOLEAN Capture,PKSDATAFORMAT F,PDMACHANNEL *D,PSERVICEGROUP *G){
 (void)Pool; // Allocation policy belongs to the explicit nonpaged provider.
 if(!Out||!D||!G||!F||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 uintptr_t A[]={(uintptr_t)Out,(uintptr_t)D,(uintptr_t)G,(uintptr_t)this};
 size_t N[]={sizeof(*Out),sizeof(*D),sizeof(*G),sizeof(*this)};
 for(unsigned I=0;I<4;I++){if(N[I]>UINTPTR_MAX-A[I])return STATUS_INVALID_PARAMETER;
  for(unsigned J=0;J<I;J++)if(A[I]<A[J]+N[J]&&A[J]<A[I]+N[I])return STATUS_INVALID_PARAMETER;}
 if(Outer||PinId||Capture)return STATUS_NOT_SUPPORTED;
 Call Guard(&Calls,Owner);if(!Guard.Entered)return STATUS_DEVICE_NOT_READY;
 if(!Initialized||Closing)return STATUS_DEVICE_NOT_READY;
 PMINIPORTWAVECYCLICSTREAM S=nullptr;PDMACHANNEL C=nullptr;PSERVICEGROUP V=nullptr;
 NTSTATUS R=Exact(Provider.Create(Provider.Context,Port,F,&S,&C,&V));
 if(R==STATUS_SUCCESS&&(!S||!C||!V))R=STATUS_DEVICE_NOT_READY;
 if(R!=STATUS_SUCCESS){if(S)S->Release();if(C)C->Release();if(V)V->Release();return R;}
 *Out=S;*D=C;*G=V;return STATUS_SUCCESS;
}

NTSTATUS T70WaveCyclicMiniport::QuiesceCalls(){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 LONG S=InterlockedCompareExchange(&Calls,0,0);
 for(;;){LONG N=S|2;LONG P=InterlockedCompareExchange(&Calls,N,S);
  if(P==S)return N==2?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;S=P;}
}
NTSTATUS T70WaveCyclicMiniport::Close(NTSTATUS (*Confirm)(void *),void *Context){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!BeginClose())return STATUS_DEVICE_NOT_READY;
 Closing=true;
 NTSTATUS S=STATUS_SUCCESS;
 if(Port){
  S=Confirm?Exact(Confirm(Context)):STATUS_DEVICE_NOT_READY;
  if(S==STATUS_SUCCESS){auto P=Port;Port=nullptr;Initialized=false;P->Release();}
 }
 EndClose();return S;
}
