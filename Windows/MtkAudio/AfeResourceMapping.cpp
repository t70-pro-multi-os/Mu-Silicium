// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeResourceMapping.h"
static bool MappingEmpty(const T70_AFE_MAPPING_OWNER &M){
 return !M.Mapping.VirtualBase&&!M.Mapping.PhysicalBase&&!M.Mapping.Length&&!M.ActiveUses;
}
static bool BoardMemory(PCM_PARTIAL_RESOURCE_DESCRIPTOR R){
 return R&&R->Type==CmResourceTypeMemory&&R->Flags==CM_RESOURCE_MEMORY_READ_WRITE&&
  R->u.Memory.Start.QuadPart==0x11210000LL&&R->u.Memory.Length==0x2000;
}
NTSTATUS T70AfeResourceMappingInitialize(void *Context,PUNKNOWN Adapter,
 PRESOURCELIST Resources,PPORTWAVERT Port){
 (void)Adapter;
 auto O=static_cast<T70_AFE_RESOURCE_MAPPING *>(Context);
 if(!O||!Resources||!Port||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Resources||!MappingEmpty(O->Afe))return STATUS_INVALID_DEVICE_STATE;
 // Reference the list before retaining any entry/view; raw and translated
 // addresses are independently checked, never derived from each other.
 Resources->AddRef();
 if(O->MemoryIndex>=Resources->NumberOfEntriesOfType(CmResourceTypeMemory)){
  Resources->Release();return STATUS_DEVICE_DATA_ERROR;
 }
 auto Raw=Resources->FindUntranslatedEntry(CmResourceTypeMemory,O->MemoryIndex);
 auto Translated=Resources->FindTranslatedEntry(CmResourceTypeMemory,O->MemoryIndex);
 if(!BoardMemory(Raw)||!BoardMemory(Translated)){
  Resources->Release();return STATUS_DEVICE_DATA_ERROR;
 }
 NTSTATUS S=T70AfeMappingAcquire(&O->Afe,(uint64_t)Translated->u.Memory.Start.QuadPart,
                                Translated->u.Memory.Length);
 if(!NT_SUCCESS(S)){Resources->Release();return S;}
 O->Resources=Resources;return STATUS_SUCCESS;
}
NTSTATUS T70AfeResourceMappingRelease(T70_AFE_RESOURCE_MAPPING *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Resources){
  return !MappingEmpty(O->Afe)?STATUS_INVALID_DEVICE_STATE:STATUS_SUCCESS;
 }
 NTSTATUS S=T70AfeMappingRelease(&O->Afe);if(!NT_SUCCESS(S))return S;
 auto Resources=O->Resources;O->Resources=nullptr;Resources->Release();return STATUS_SUCCESS;
}
