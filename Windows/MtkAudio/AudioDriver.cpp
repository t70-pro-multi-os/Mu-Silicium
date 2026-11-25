// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PortClsOwner.h"
#include "AudioCyclicStart.h"

static T70_PORTCLS_OWNER DriverOwner;
static T70_AUDIO_CYCLIC_DEVICE *DeviceState(PDEVICE_OBJECT D){
 if(!D||!D->DeviceExtension)return nullptr;
 return (T70_AUDIO_CYCLIC_DEVICE *)((unsigned char *)D->DeviceExtension+PORT_CLASS_DEVICE_EXTENSION_SIZE);
}
static NTSTATUS DispatchPnp(PDEVICE_OBJECT D,PIRP I){
 if(!D||!I)return STATUS_INVALID_PARAMETER;
 void *Retired=nullptr;
 auto Slot=DeviceState(D);auto Stack=IoGetCurrentIrpStackLocation(I);
 if(Slot&&Stack){
  if(Stack->MinorFunction==IRP_MN_QUERY_REMOVE_DEVICE||Stack->MinorFunction==IRP_MN_QUERY_STOP_DEVICE){
   // Only failed/unpublished starts exist today. No active playback is torn
   // down on a query; an active implementation needs cancel-remove handling.
   NTSTATUS S=T70AudioCyclicQueryRelease(Slot);
   if(S!=STATUS_SUCCESS){
    // This request was not passed to PortCls or marked pending. Complete it
    // synchronously, not with PcCompleteIrp (which is for pending IRPs).
    I->IoStatus.Status=S;I->IoStatus.Information=0;
    IoCompleteRequest(I,IO_NO_INCREMENT);return S;
   }
  }else if(Stack->MinorFunction==IRP_MN_SURPRISE_REMOVAL){
   (void)T70AudioCyclicSurpriseRemove(Slot);
  }else if(Stack->MinorFunction==IRP_MN_REMOVE_DEVICE){
   (void)T70AudioCyclicRemove(Slot);
   if(Slot->Runtime)(void)T70AudioCyclicRetire(Slot,&Retired);
   // Final REMOVE is not vetoable. Transfer any retained runtime out of the
   // soon-to-be-deleted extension. A dedicated OS-pinned thread can finish
   // eligible CPU-only cleanup; failed hardware remains explicitly retained.
   // Neither path authorizes physical admission or fabricates a stopped DMA.
  }
 }
 // PortCls may delete D/extension here. Only the separately retained heap
 // token is touched afterwards; a returned PENDING status is not completion.
 NTSTATUS S=PcDispatchIrp(D,I);
 if(Retired)T70AudioCyclicForwardReturned(Retired);
 return S;
}
static NTSTATUS StartDevice(PDEVICE_OBJECT D,PIRP I,PRESOURCELIST R){
 if(!D||!I||!R||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 // The project SSDT declares exactly one AFE memory range; do not select a
 // coincidental range from another device's resources.
 if(R->NumberOfEntriesOfType(CmResourceTypeMemory)!=1)return STATUS_DEVICE_DATA_ERROR;
 // Require the board's line interrupt before any allocation or mapping. Raw
 // Vector describes our ACPI GSI; translated Vector/Level are OS assignments,
 // not values to compare with that GSI or a guessed processor IRQL.
 if(R->NumberOfEntriesOfType(CmResourceTypeInterrupt)!=1)return STATUS_DEVICE_DATA_ERROR;
 auto RawIrq=R->FindUntranslatedEntry(CmResourceTypeInterrupt,0);
 auto Irq=R->FindTranslatedEntry(CmResourceTypeInterrupt,0);
 if(!RawIrq||!Irq||RawIrq->Type!=CmResourceTypeInterrupt||Irq->Type!=CmResourceTypeInterrupt||
    RawIrq->Flags!=CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE||
    Irq->Flags!=CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE||
    RawIrq->ShareDisposition!=CmResourceShareDeviceExclusive||
    Irq->ShareDisposition!=CmResourceShareDeviceExclusive||RawIrq->u.Interrupt.Vector!=266)
  return STATUS_DEVICE_DATA_ERROR;
 return T70AudioCyclicStart(DeviceState(D),D,I,R);
}
static NTSTATUS AddDevice(PDRIVER_OBJECT D,PDEVICE_OBJECT Pdo){
 if(!D||!Pdo||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 return PcAddAdapterDevice(D,Pdo,StartDevice,2,PORT_CLASS_DEVICE_EXTENSION_SIZE+sizeof(T70_AUDIO_CYCLIC_DEVICE));
}
static void Unload(PDRIVER_OBJECT D){
 if(D==DriverOwner.Driver)(void)T70PortClsFinishUnload(&DriverOwner);
}
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT D,PUNICODE_STRING R){
 NTSTATUS S=T70PortClsInitialize(&DriverOwner,D,R,AddDevice,Unload);
 if(S==STATUS_SUCCESS)D->MajorFunction[IRP_MJ_PNP]=DispatchPnp;
 return S;
}
