// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioResourceOwner.h"
#include <stdint.h>
#include <string.h>
static constexpr ULONG Tag=0x4f413754; // T7AO
T70AudioResourceOwner::T70AudioResourceOwner(ULONG I):Refs(1),Resource{},Users(0),Open(false),Closing(false),Closed(false),Quiesced(false){Resource.MemoryIndex=I;}
NTSTATUS T70AudioResourceOwner::Create(ULONG I,T70AudioResourceOwner **Out){
 if(!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 void *Memory=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(T70AudioResourceOwner),Tag);
 if(!Memory)return STATUS_INSUFFICIENT_RESOURCES;
 *Out=new(Memory) T70AudioResourceOwner(I);return STATUS_SUCCESS;
}
NTSTATUS T70AudioResourceOwner::QueryInterface(REFIID Id,void **Out){
 static const GUID Unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
 if(!Out)return STATUS_INVALID_PARAMETER;*Out=nullptr;
 if(memcmp(&Id,&Unknown,sizeof Id))return STATUS_NOINTERFACE;
 *Out=static_cast<IUnknown *>(this);AddRef();return STATUS_SUCCESS;
}
ULONG T70AudioResourceOwner::AddRef(){return (ULONG)InterlockedIncrement(&Refs);}
ULONG T70AudioResourceOwner::Release(){
 LONG N=InterlockedDecrement(&Refs);
 if(!N){this->~T70AudioResourceOwner();ExFreePoolWithTag(this,Tag);}return (ULONG)N;
}
NTSTATUS T70AudioResourceOwner::Initialize(void *C,PUNKNOWN A,PRESOURCELIST R,PPORTWAVERT P){
 auto O=static_cast<T70AudioResourceOwner *>(C);
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Open||O->Closing||O->Closed)return STATUS_INVALID_DEVICE_STATE;
 O->AddRef(); // Mapping lifetime does not depend on miniport's last reference.
 NTSTATUS S=T70AfeResourceMappingInitialize(&O->Resource,A,R,P);
 if(NT_SUCCESS(S))O->Open=true;else O->Release();return S;
}
NTSTATUS T70AudioResourceOwner::BeginUse(T70_AUDIO_RESOURCE_USE *Use,T70_AFE_MAPPING_OWNER **Out){
 if(!Use||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 uintptr_t A=(uintptr_t)Use,B=(uintptr_t)Out,C=(uintptr_t)this;
 if(sizeof(*Use)>UINTPTR_MAX-A||sizeof(*Out)>UINTPTR_MAX-B||sizeof(*this)>UINTPTR_MAX-C||
    (A<B+sizeof(*Out)&&B<A+sizeof(*Use))||(A<C+sizeof(*this)&&C<A+sizeof(*Use))||
    (B<C+sizeof(*this)&&C<B+sizeof(*Out)))return STATUS_INVALID_PARAMETER;
 if(Use->Owner||!Open||Closing||Users==UINT32_MAX)return STATUS_INVALID_DEVICE_STATE;
 ++Users;AddRef();Use->Owner=this;*Out=&Resource.Afe;return STATUS_SUCCESS;
}
NTSTATUS T70AudioResourceOwner::EndUse(T70_AUDIO_RESOURCE_USE *Use){
 if(!Use||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Use->Owner!=this||!Open||!Users)return STATUS_INVALID_DEVICE_STATE;
 Use->Owner=nullptr;--Users;Release();return STATUS_SUCCESS;
}
NTSTATUS T70AudioResourceOwner::Close(NTSTATUS (*Confirm)(void *),void *Context){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Closed)return STATUS_SUCCESS;
 Closing=true; // Admission remains closed across failure/recovery.
 if(!Open){Closed=true;return STATUS_SUCCESS;}
 if(Users||Resource.Afe.ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 if(!Quiesced){
  if(!Confirm)return STATUS_DEVICE_NOT_READY;
  NTSTATUS S=Confirm(Context);if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
  Quiesced=true;
 }
 NTSTATUS S=T70AfeResourceMappingRelease(&Resource);if(!NT_SUCCESS(S))return S;
 Open=false;Closed=true;Release(); // Drop self-pin only after successful unmap.
 return STATUS_SUCCESS;
}
