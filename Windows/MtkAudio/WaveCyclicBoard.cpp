// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicBoard.h"
static NTSTATUS Exact(NTSTATUS S){return S==0?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);}
static bool Memory(PCM_PARTIAL_RESOURCE_DESCRIPTOR R){return R&&R->Type==CmResourceTypeMemory&&
 R->Flags==CM_RESOURCE_MEMORY_READ_WRITE&&R->u.Memory.Start.QuadPart==0x11210000&&R->u.Memory.Length==0x2000;}
static bool Line(PCM_PARTIAL_RESOURCE_DESCRIPTOR R){return R&&R->Type==CmResourceTypeInterrupt&&
 R->Flags==CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE&&R->ShareDisposition==CmResourceShareDeviceExclusive;}
static NTSTATUS Read(void *C,ULONG A,ULONG *V){
 if(!V)return STATUS_INVALID_PARAMETER;uint32_t Value=0;
 NTSTATUS S=T70AfeMappedRead32(C,A,&Value);if(S==0)*V=(ULONG)Value;return S;
}
static NTSTATUS Write(void *C,ULONG A,ULONG V){return T70AfeMappedWrite32(C,A,V);}
static NTSTATUS Ready(void *C){
 auto O=(T70_WAVECYCLIC_BOARD *)C;ULONG On=0,Enable=0,Status=0;
 NTSTATUS S=Read(&O->Mapping.Mapping,0x3a0,&On);if(S)return S;
 S=Read(&O->Mapping.Mapping,0x3b4,&Enable);if(S)return S;
 S=Read(&O->Mapping.Mapping,0x3a4,&Status);if(S)return S;
 return ((On|(Enable&Status))&0x87ffffffu)?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicBoardClose(T70_WAVECYCLIC_BOARD *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 O->Started=false;NTSTATUS S=0;auto &C=O->Contract;
 // Started=false rejects new streams before removing the public KS link.
 S=T70WaveCyclicPortWithdraw(&O->Port);if(S)goto Done;
 if(O->BeforeIrqClose){S=Exact(O->BeforeIrqClose(O->StreamOwner));if(S)goto Done;O->BeforeIrqClose=nullptr;O->StreamOwner=nullptr;}
 if(O->Irq.Initialized){S=T70AfeIrqRecoveryClose(&O->Irq,C.DrainService,C.Context);if(S)goto Done;}
 S=T70WaveCyclicPortClose(&O->Port,C.DetachStreams,C.Context);if(S)goto Done;
 if(O->Lease){S=T70AfeMappingEnd(&O->Mapping);if(S)goto Done;O->Lease=false;}
 S=T70AfeMappingRelease(&O->Mapping);if(S)goto Done;
 if(O->DomainHeld){S=Exact(C.ReleaseDomain(C.Context));if(S)goto Done;O->DomainHeld=false;}
 if(O->Resources){auto R=O->Resources;O->Resources=nullptr;R->Release();}
 if(O->Parent){auto P=O->Parent;O->Parent=nullptr;P->Release();}
 O->Started=false;
Done:O->CleanupStatus=S;return S;
}
NTSTATUS T70WaveCyclicBoardOpen(T70_WAVECYCLIC_BOARD *O,PDEVICE_OBJECT Device,PDEVICE_OBJECT Pdo,
 PIRP Irp,PUNKNOWN Parent,PUNKNOWN Adapter,PRESOURCELIST Resources,
 const T70_WAVECYCLIC_MINIPORT_PROVIDER &Provider,const T70_WAVECYCLIC_BOARD_CONTRACT &C){
 if(!O||!Device||!Pdo||!Irp||!Parent||!Resources||!C.ClaimDomain||!C.ReleaseDomain||
    !C.DrainService||!C.DetachStreams||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Attempted||O->Parent)return STATUS_INVALID_DEVICE_STATE;
 if(Resources->NumberOfEntriesOfType(CmResourceTypeMemory)!=1||
    Resources->NumberOfEntriesOfType(CmResourceTypeInterrupt)!=1)return STATUS_DEVICE_DATA_ERROR;
 auto M=Resources->FindTranslatedEntry(CmResourceTypeMemory,0);
 auto RM=Resources->FindUntranslatedEntry(CmResourceTypeMemory,0);
 auto I=Resources->FindTranslatedEntry(CmResourceTypeInterrupt,0);
 auto RI=Resources->FindUntranslatedEntry(CmResourceTypeInterrupt,0);
 if(!Memory(M)||!Memory(RM)||!Line(I)||!Line(RI)||RI->u.Interrupt.Vector!=266||
    I->u.Interrupt.Level<=DISPATCH_LEVEL||I->u.Interrupt.Level>HIGH_LEVEL||
    !I->u.Interrupt.Vector||!I->u.Interrupt.Affinity)return STATUS_DEVICE_DATA_ERROR;
 USHORT Group=0;
#if defined(NT_PROCESSOR_GROUPS)
 Group=I->u.Interrupt.Group;
 if(Group==0xffff)return STATUS_DEVICE_DATA_ERROR; // require a concrete OS group
#endif
 T70_AFE_INTERRUPT_RESOURCE R={Pdo,266,I->u.Interrupt.Vector,(unsigned char)I->u.Interrupt.Level,Group,I->u.Interrupt.Affinity};
 O->Attempted=true;O->Contract=C;Parent->AddRef();O->Parent=Parent;Resources->AddRef();O->Resources=Resources;
 NTSTATUS S=Exact(C.ClaimDomain(C.Context));
 IPortWaveCyclic *Port=nullptr;
 if(S)goto Failed;O->DomainHeld=true;
 if(C.AccessReady){S=Exact(C.AccessReady(C.Context));if(S)goto Failed;}
 S=T70AfeMappingAcquire(&O->Mapping,(uint64_t)M->u.Memory.Start.QuadPart,M->u.Memory.Length);if(S)goto Failed;
 S=T70AfeMappingBegin(&O->Mapping);if(S)goto Failed;O->Lease=true;
 S=T70WaveCyclicPortOpen(&O->Port,Device,Irp,Parent,Adapter,Resources,Provider,C.DetachStreams,C.Context);
 if(S&&O->Port.Miniport&&O->Port.CleanupStatus!=0){
  O->StartStatus=S;O->CleanupStatus=O->Port.CleanupStatus;return S;
 }
 if(S)goto Failed;
 {
  static const GUID Id={0xb4c90a26,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
  S=Exact(O->Port.Port->QueryInterface(Id,(void **)&Port));
 }
 if(S)goto Failed;if(!Port){S=STATUS_DEVICE_DATA_ERROR;goto Failed;}
 S=T70AfeIrqRecoveryInit(&O->Irq,Parent,Device);if(S)goto Failed;
 {
  T70_AFE_DL1_INTERRUPT H={};H.IoContext=&O->Mapping.Mapping;H.Read32=Read;H.Write32=Write;H.DomainExclusive=true;
  S=T70AfeIrqRecoveryConnect(&O->Irq,Port,R,H,Ready,O);
 }
 if(S)goto Failed;
 Port->Release();O->Started=true;O->StartStatus=STATUS_SUCCESS;return STATUS_SUCCESS;
Failed:
 if(Port)Port->Release();O->StartStatus=S;T70WaveCyclicBoardClose(O);return S;
}
