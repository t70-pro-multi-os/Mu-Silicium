// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeInterruptOwner.h"
static NTSTATUS Exact(NTSTATUS S){return S==STATUS_SUCCESS?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);}
NTSTATUS T70AfeInterruptConnect(T70_AFE_INTERRUPT_OWNER *O,PUNKNOWN Parent,IPortWaveCyclic *Port,
 const T70_AFE_INTERRUPT_RESOURCE &R,const T70_AFE_DL1_INTERRUPT &H,
 NTSTATUS (*Ready)(void *),void *Context){
 if(!O||!Parent||!Port||!R.Pdo||!Ready||KeGetCurrentIrql()!=PASSIVE_LEVEL||
    R.RawGsi!=266||!R.Vector||!R.Affinity||R.Irql<=DISPATCH_LEVEL||R.Irql>HIGH_LEVEL||
    !H.Read32||!H.Write32||!H.OnFault||!H.DomainExclusive||H.Faulted)return STATUS_INVALID_PARAMETER;
 if(O->Removed||O->Attempted||O->Parent||O->Interrupt||O->Service.Attempted)return STATUS_INVALID_DEVICE_STATE;
 O->Attempted=true;NTSTATUS S=Exact(Ready(Context));if(S!=STATUS_SUCCESS)return S;
 Parent->AddRef();O->Parent=Parent;O->Handler=H;O->Handler.Service=&O->Service;
 S=T70WaveCyclicServiceAcquire(&O->Service,Port);
 if(S!=STATUS_SUCCESS){O->Parent=nullptr;Parent->Release();return S;}
 IO_CONNECT_INTERRUPT_PARAMETERS P={};P.Version=CONNECT_FULLY_SPECIFIED_GROUP;
 P.FullySpecified.PhysicalDeviceObject=R.Pdo;P.FullySpecified.InterruptObject=&O->Interrupt;
 P.FullySpecified.ServiceRoutine=T70AfeDl1Interrupt;P.FullySpecified.ServiceContext=&O->Handler;
 P.FullySpecified.SynchronizeIrql=R.Irql;P.FullySpecified.Irql=R.Irql;
 P.FullySpecified.Vector=R.Vector;P.FullySpecified.InterruptMode=LevelSensitive;
 P.FullySpecified.ProcessorEnableMask=R.Affinity;P.FullySpecified.Group=R.Group;
 P.FullySpecified.ShareVector=FALSE;P.FullySpecified.FloatingSave=FALSE;
 S=Exact(IoConnectInterruptEx(&P));
 if(S!=STATUS_SUCCESS){
  // Failed connect transfers no IRQ object, but a pre-return ISR could have
  // queued recovery/service work. Retain parent/group until explicit Drain.
  O->Interrupt=nullptr;return S;
 }
 O->Version=P.Version;
 if(!O->Interrupt||P.Version!=CONNECT_FULLY_SPECIFIED_GROUP){O->ConnectionUnknown=true;return STATUS_DEVICE_DATA_ERROR;}
 if(InterlockedCompareExchange(&O->Handler.Faulted,0,0))return STATUS_DEVICE_NOT_READY;
 return STATUS_SUCCESS;
}
static BOOLEAN MarkRemoved(void *Context){
 auto O=(T70_AFE_INTERRUPT_OWNER *)Context;
 InterlockedCompareExchange(&O->Removed,1,0);
 // Existing ISR checks this before any I/O. Do not invoke OnFault or invent an
 // OriginalError: physical removal is distinct from a register access failure.
 InterlockedCompareExchange(&O->Handler.Faulted,1,0);
 return TRUE;
}
NTSTATUS T70AfeInterruptMarkRemoved(T70_AFE_INTERRUPT_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->ConnectionUnknown)return STATUS_DEVICE_NOT_READY;
 if(O->Interrupt){
  if(!KeSynchronizeExecution(O->Interrupt,MarkRemoved,O))return STATUS_DEVICE_NOT_READY;
 }else MarkRemoved(O);
 return STATUS_SUCCESS;
}
NTSTATUS T70AfeInterruptQuiesce(T70_AFE_INTERRUPT_OWNER *O,NTSTATUS (*Mask)(void *),void *Context){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->ConnectionUnknown)return STATUS_DEVICE_NOT_READY;
 if(!O->Parent)return STATUS_SUCCESS;
 if(O->Interrupt){
  if(!InterlockedCompareExchange(&O->Removed,0,0)){
   if(!Mask)return STATUS_DEVICE_NOT_READY;
   NTSTATUS S=Exact(Mask(Context));if(S!=STATUS_SUCCESS)return S;
   O->SourceMasked=true;
  } // Removed device: no register access; source masking is not claimed.
  IO_DISCONNECT_INTERRUPT_PARAMETERS P={};P.Version=O->Version;P.ConnectionContext.InterruptObject=O->Interrupt;
  IoDisconnectInterruptEx(&P);O->Interrupt=nullptr;
 }
 return STATUS_SUCCESS;
}
NTSTATUS T70AfeInterruptClose(T70_AFE_INTERRUPT_OWNER *O,NTSTATUS (*Mask)(void *),NTSTATUS (*Drain)(void *),void *Context){
 NTSTATUS Q=T70AfeInterruptQuiesce(O,Mask,Context);if(Q!=STATUS_SUCCESS)return Q;
 if(!O->Parent)return STATUS_SUCCESS;
 NTSTATUS S=T70WaveCyclicServiceClose(&O->Service,Drain,Context);if(S!=STATUS_SUCCESS)return S;
 auto Parent=O->Parent;O->Parent=nullptr;Parent->Release();return STATUS_SUCCESS;
}
