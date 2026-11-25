// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeMapping.h"
static int Empty(const T70_AFE_MAPPING_OWNER *O){
 return !O->Mapping.VirtualBase && !O->Mapping.PhysicalBase &&
        !O->Mapping.Length && !O->ActiveUses;
}
static int Valid(const T70_AFE_MAPPING_OWNER *O){
 uintptr_t V=(uintptr_t)O->Mapping.VirtualBase;
 return V && !(V&3) && V<=UINTPTR_MAX-0x2000 &&
        O->Mapping.PhysicalBase==UINT64_C(0x11210000) && O->Mapping.Length==0x2000;
}
static NTSTATUS Check(const T70_AFE_MAPPING_OWNER *O){
 if(!O)return STATUS_INVALID_PARAMETER;
 return KeGetCurrentIrql()<=DISPATCH_LEVEL?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
}
NTSTATUS T70AfeMappingAcquire(T70_AFE_MAPPING_OWNER *O,uint64_t Base,size_t Length){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(Base!=UINT64_C(0x11210000) || Length!=0x2000)return STATUS_INVALID_PARAMETER;
 if(!Empty(O))return STATUS_INVALID_DEVICE_STATE;
 PHYSICAL_ADDRESS P;P.QuadPart=(LONGLONG)Base;
 void *V=MmMapIoSpaceEx(P,Length,PAGE_READWRITE|PAGE_NOCACHE);
 if(!V)return STATUS_INSUFFICIENT_RESOURCES;
 T70_AFE_MAPPING_OWNER Next={{Base,V,Length},0};
 uintptr_t A=(uintptr_t)O,B=(uintptr_t)V;
 if(!Valid(&Next) || A>UINTPTR_MAX-sizeof(*O) ||
    (A<B+Length && B<A+sizeof(*O))){
   MmUnmapIoSpace(V,Length);return STATUS_INVALID_DEVICE_STATE;
 }
 *O=Next;return STATUS_SUCCESS;
}
NTSTATUS T70AfeMappingBegin(T70_AFE_MAPPING_OWNER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(!Valid(O) || O->ActiveUses==UINT32_MAX)return STATUS_INVALID_DEVICE_STATE;
 ++O->ActiveUses;return STATUS_SUCCESS;
}
NTSTATUS T70AfeMappingEnd(T70_AFE_MAPPING_OWNER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(!Valid(O) || !O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 --O->ActiveUses;return STATUS_SUCCESS;
}
NTSTATUS T70AfeMappingRelease(T70_AFE_MAPPING_OWNER *O){
 NTSTATUS S=Check(O);if(!NT_SUCCESS(S))return S;
 if(Empty(O))return STATUS_SUCCESS;
 if(!Valid(O) || O->ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 MmUnmapIoSpace((void *)O->Mapping.VirtualBase,O->Mapping.Length);
 T70_AFE_MAPPING_OWNER EmptyOwner={0};*O=EmptyOwner;return STATUS_SUCCESS;
}
