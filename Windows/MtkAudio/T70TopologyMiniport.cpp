// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T70TopologyMiniport.h"
#include <stdint.h>
#include <string.h>
namespace {
constexpr ULONG Tag=0x54413754; // T7AT
const GUID Unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
const GUID Miniport={0xb4c90a24,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
const GUID Topology={0xb4c90a31,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
const GUID AudioType={0x73647561,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
const GUID Analog={0x6dba3190,0x67bd,0x11cf,{0xa0,0xf7,0,0x20,0xaf,0xd1,0x56,0xe4}};
const GUID NoSpecifier={0x0f6417d6,0xc318,0x11d0,{0xa4,0x3f,0,0xa0,0xc9,0x22,0x31,0x96}};
const GUID AudioCategory={0x6994ad04,0x93ef,0x11d0,{0xa3,0xcc,0,0xa0,0xc9,0x22,0x31,0x96}};
const GUID Speaker={0xdff21ce1,0xf70f,0x11d0,{0xb9,0x17,0,0xa0,0xc9,0x22,0x31,0x96}};
}
T70TopologyMiniport::T70TopologyMiniport(PUNKNOWN P):Refs(1),Parent(P),Initialized(false),Closed(false),BridgeRange{},RangePointer(&BridgeRange),Pins{},Connection{},Filter{}{
 BridgeRange.FormatSize=sizeof BridgeRange;BridgeRange.MajorFormat=AudioType;
 BridgeRange.SubFormat=Analog;BridgeRange.Specifier=NoSpecifier;
 for(unsigned I=0;I<2;I++){
  auto &K=Pins[I].KsPinDescriptor;K.DataRangesCount=1;K.DataRanges=&RangePointer;
  K.DataFlow=I?KSPIN_DATAFLOW_OUT:KSPIN_DATAFLOW_IN;
  K.Communication=KSPIN_COMMUNICATION_NONE;K.Category=I?&Speaker:&AudioCategory;
 }
 Connection={PCFILTER_NODE,0,PCFILTER_NODE,1};
 Filter.PinSize=sizeof Pins[0];Filter.PinCount=2;Filter.Pins=Pins;
 Filter.NodeSize=sizeof(PCNODE_DESCRIPTOR);Filter.ConnectionCount=1;Filter.Connections=&Connection;
}
NTSTATUS T70TopologyMiniport::Create(PUNKNOWN P,T70TopologyMiniport **Out){
 if(!P||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 void *M=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(T70TopologyMiniport),Tag);
 if(!M)return STATUS_INSUFFICIENT_RESOURCES;
 P->AddRef();*Out=new(M) T70TopologyMiniport(P);return STATUS_SUCCESS;
}
NTSTATUS T70TopologyMiniport::QueryInterface(REFIID I,void **Out){
 if(!Out)return STATUS_INVALID_PARAMETER;*Out=nullptr;
 if(memcmp(&I,&Unknown,sizeof I)&&memcmp(&I,&Miniport,sizeof I)&&memcmp(&I,&Topology,sizeof I))return STATUS_NOINTERFACE;
 *Out=static_cast<IMiniportTopology *>(this);AddRef();return STATUS_SUCCESS;
}
ULONG T70TopologyMiniport::AddRef(){return (ULONG)InterlockedIncrement(&Refs);}
ULONG T70TopologyMiniport::Release(){
 LONG N=InterlockedDecrement(&Refs);if(!N){auto P=Parent;this->~T70TopologyMiniport();ExFreePoolWithTag(this,Tag);P->Release();}return (ULONG)N;
}
NTSTATUS T70TopologyMiniport::Init(PUNKNOWN Adapter,PRESOURCELIST Resources,PPORTTOPOLOGY Port){
 (void)Resources; // This control-free topology owns no hardware resources.
 if(!Adapter||!Port||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Call Guard(&Calls,Parent);if(!Guard.Entered)return STATUS_INVALID_DEVICE_STATE;
 if(Initialized||Closed)return STATUS_INVALID_DEVICE_STATE;
 Initialized=true;return STATUS_SUCCESS;
}
NTSTATUS T70TopologyMiniport::Close(){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!BeginClose())return STATUS_DEVICE_NOT_READY;
 Closed=true;EndClose();return STATUS_SUCCESS;
}
NTSTATUS T70TopologyMiniport::GetDescription(PPCFILTER_DESCRIPTOR *Out){
 if(!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Call Guard(&Calls,Parent);if(!Guard.Entered)return STATUS_DEVICE_NOT_READY;
 if(Closed)return STATUS_DEVICE_NOT_READY;
 *Out=&Filter;return STATUS_SUCCESS;
}
NTSTATUS T70TopologyMiniport::DataRangeIntersection(ULONG Pin,PKSDATARANGE Client,
 PKSDATARANGE Matching,ULONG Capacity,void *Output,PULONG Length){
 if(Pin>1||!Client||!Matching||!Length||(!Output&&Capacity)||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Call Guard(&Calls,Parent);if(!Guard.Entered)return STATUS_DEVICE_NOT_READY;
 if(Closed)return STATUS_DEVICE_NOT_READY;
 if(Client->FormatSize!=sizeof BridgeRange||Matching->FormatSize!=sizeof BridgeRange||
    memcmp(Client,&BridgeRange,sizeof BridgeRange)||memcmp(Matching,&BridgeRange,sizeof BridgeRange))return STATUS_NO_MATCH;
 uintptr_t A[]={(uintptr_t)Client,(uintptr_t)Matching,(uintptr_t)this,(uintptr_t)Length,(uintptr_t)Output};
 size_t N[]={sizeof BridgeRange,sizeof BridgeRange,sizeof(*this),sizeof(*Length),Capacity?sizeof BridgeRange:0};
 for(unsigned I=0;I<5;I++)if(N[I]>UINTPTR_MAX-A[I])return STATUS_INVALID_PARAMETER;
 for(unsigned I=3;I<5;I++)if(N[I])for(unsigned J=0;J<I;J++)
  if(N[J]&&A[I]<A[J]+N[J]&&A[J]<A[I]+N[I])return STATUS_INVALID_PARAMETER;
 if(!Capacity){*Length=sizeof BridgeRange;return STATUS_BUFFER_OVERFLOW;}
 if(Capacity<sizeof BridgeRange)return STATUS_BUFFER_TOO_SMALL;
 memcpy(Output,&BridgeRange,sizeof BridgeRange);*Length=sizeof BridgeRange;return STATUS_SUCCESS;
}
