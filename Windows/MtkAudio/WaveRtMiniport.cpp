// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtMiniport.h"
#include <string.h>
static constexpr ULONG Tag=0x4d573754; // T7WM
// Official GUID values; no additional exported GUID-definition unit required.
static const GUID UnknownId={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID MiniportId={0xb4c90a24,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
static const GUID WaveRtId={0x0f9fc4d6,0x6061,0x4f3c,{0xb1,0xfc,7,0x5e,0x35,0xf7,0x96,0x0a}};
static const GUID Audio={0x73647561,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static const GUID Pcm={1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static const GUID Wave={0x05589f81,0xc356,0x11ce,{0xbf,1,0,0xaa,0,0x55,0x59,0x5a}};
T70WaveRtMiniport::T70WaveRtMiniport(PUNKNOWN O,const T70_WAVERT_MINIPORT_PROVIDER &P)
 :T70WaveRtMiniportBoundary(0,P),Refs(1),Owner(O),Ranges{},RangePointers{},PinDescriptor{},Filter{}{
 const ULONG Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 for(unsigned I=0;I<11;I++){
  auto &R=Ranges[I];R.DataRange.FormatSize=sizeof R;R.DataRange.MajorFormat=Audio;
  R.DataRange.SubFormat=Pcm;R.DataRange.Specifier=Wave;
  R.MaximumChannels=2;R.MinimumBitsPerSample=R.MaximumBitsPerSample=16;
  R.MinimumSampleFrequency=R.MaximumSampleFrequency=Rates[I];RangePointers[I]=&R.DataRange;
 }
 // No physical pin/codec/connection claimed. One software stream maximum.
 PinDescriptor.MaxGlobalInstanceCount=PinDescriptor.MaxFilterInstanceCount=1;
 auto &K=PinDescriptor.KsPinDescriptor;K.DataRangesCount=11;K.DataRanges=RangePointers;
 K.DataFlow=KSPIN_DATAFLOW_IN;K.Communication=KSPIN_COMMUNICATION_SINK;
 Filter.PinSize=sizeof PinDescriptor;Filter.PinCount=1;Filter.Pins=&PinDescriptor;
}
NTSTATUS T70WaveRtMiniport::Create(PUNKNOWN O,const T70_WAVERT_MINIPORT_PROVIDER &P,T70WaveRtMiniport **Out){
 if(!O||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!P.Initialize||!P.Create)return STATUS_DEVICE_NOT_READY;
 void *Memory=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(T70WaveRtMiniport),Tag);
 if(!Memory)return STATUS_INSUFFICIENT_RESOURCES;
 O->AddRef();auto Object=new(Memory) T70WaveRtMiniport(O,P);*Out=Object;return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtMiniport::QueryInterface(REFIID Id,void **Out){
 if(!Out)return STATUS_INVALID_PARAMETER;
 *Out=nullptr;
 if(memcmp(&Id,&UnknownId,sizeof Id)&&memcmp(&Id,&MiniportId,sizeof Id)&&memcmp(&Id,&WaveRtId,sizeof Id))return STATUS_NOINTERFACE;
 *Out=static_cast<IMiniportWaveRT *>(this);AddRef();return STATUS_SUCCESS;
}
ULONG T70WaveRtMiniport::AddRef(){return (ULONG)InterlockedIncrement(&Refs);}
ULONG T70WaveRtMiniport::Release(){
 LONG Remaining=InterlockedDecrement(&Refs);
 if(!Remaining){PUNKNOWN Keep=Owner;this->~T70WaveRtMiniport();ExFreePoolWithTag(this,Tag);Keep->Release();}
 return (ULONG)Remaining;
}
NTSTATUS T70WaveRtMiniport::GetDescription(PPCFILTER_DESCRIPTOR *Out){
 if(!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!ResourceInitializationSucceeded())return STATUS_DEVICE_NOT_READY;
 *Out=&Filter;return STATUS_SUCCESS;
}
NTSTATUS T70WaveRtMiniport::GetDeviceDescription(PDEVICE_DESCRIPTION Out){
 if(!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!ResourceInitializationSucceeded())return STATUS_DEVICE_NOT_READY;
 // DL1 uses one contiguous BASE/END range, not a scatter/gather descriptor list.
 // VERSION3 expresses the proven 34-bit limit without claiming 64-bit DMA.
 // Let the real PDO supply bus identity; never label the SoC as PCI.
 DEVICE_DESCRIPTION D={};
 D.Version=DEVICE_DESCRIPTION_VERSION3;D.Master=TRUE;
 D.ScatterGather=FALSE;D.InterfaceType=InterfaceTypeUndefined;
 D.MaximumLength=196608;D.DmaAddressWidth=34;
 *Out=D;return STATUS_SUCCESS;
}
