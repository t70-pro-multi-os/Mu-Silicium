// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtComStreamBase.h"
#include <string.h>
static constexpr ULONG Tag=0x53573754; // T7WS
static const GUID UnknownId={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
// IID_IMiniportWaveRTStream from the official WDK portcls.h.
static const GUID StreamId={0x000ac9ab,0xfaab,0x4f3d,{0x94,0x55,0x6f,0xf8,0x30,0x6a,0x74,0xa0}};
void *T70WaveRtComStreamBase::operator new(size_t Bytes) noexcept {
 return ExAllocatePool2(POOL_FLAG_NON_PAGED,Bytes,Tag);
}
void T70WaveRtComStreamBase::operator delete(void *Memory) noexcept {
 ExFreePoolWithTag(Memory,Tag);
}
T70WaveRtComStreamBase::T70WaveRtComStreamBase(PUNKNOWN O,const T70_AFE_PREPARED &P,
 T70_WAVERT_BUFFER_TRANSACTION *T,IPortWaveRTStream *S,PDMA_ADAPTER D,T70_AFE_MAPPING_OWNER *A)
 :T70WaveRtBufferedStreamBase(P,T,S,D,A),Refs(1),Parent(O){Parent->AddRef();}
NTSTATUS T70WaveRtComStreamBase::QueryInterface(REFIID Id,void **Out){
 if(!Out)return STATUS_INVALID_PARAMETER;
 *Out=nullptr;
 if(memcmp(&Id,&UnknownId,sizeof Id)&&memcmp(&Id,&StreamId,sizeof Id))return STATUS_NOINTERFACE;
 *Out=static_cast<IMiniportWaveRTStream *>(this);AddRef();return STATUS_SUCCESS;
}
ULONG T70WaveRtComStreamBase::AddRef(){return (ULONG)InterlockedIncrement(&Refs);}
ULONG T70WaveRtComStreamBase::Release(){
 LONG Remaining=InterlockedDecrement(&Refs);
 if(!Remaining){
  PUNKNOWN Keep=Parent;
  // Virtual destruction runs the concrete provider's destructor, then frees
  // this allocation. Keep the provider owner alive throughout both operations.
  delete this;
  Keep->Release();
 }
 return (ULONG)Remaining;
}
