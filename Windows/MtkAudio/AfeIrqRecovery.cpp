// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeIrqRecovery.h"
static LONG Load(volatile LONG *V){return InterlockedCompareExchange(V,0,0);}
static constexpr LONG ObservationClosing=0x40000000;
static NTSTATUS Quiesce(T70_AFE_IRQ_RECOVERY *R){
 LONG V=Load(&R->ObservationGate);
 for(;;){LONG Old=InterlockedCompareExchange(&R->ObservationGate,V|ObservationClosing,V);if(Old==V)break;V=Old;}
 if(Load(&R->RemovalRequested)){
  NTSTATUS S=T70AfeInterruptMarkRemoved(&R->Owner);if(S!=STATUS_SUCCESS)return S;
 }
 if(Load(&R->ObservationGate)!=ObservationClosing)return STATUS_DEVICE_NOT_READY;
 return T70AfeInterruptQuiesce(&R->Owner,T70AfeIrqSourceMask,&R->Source);
}
static void Worker(PDEVICE_OBJECT,void *Context){
 auto R=(T70_AFE_IRQ_RECOVERY *)Context;
 KeWaitForSingleObject(&R->Mutex,Executive,KernelMode,FALSE,nullptr);
 R->RecoveryStatus=Quiesce(R);
 KeReleaseMutex(&R->Mutex,FALSE);
 KeSetEvent(&R->Done,IO_NO_INCREMENT,FALSE); // last access to R
}
static void Dpc(KDPC *,void *Context,void *,void *){
 auto R=(T70_AFE_IRQ_RECOVERY *)Context;
 if(Load(&R->Published)&&Load(&R->Fault)==2&&
    InterlockedCompareExchange(&R->Queued,1,0)==0)
  IoQueueWorkItem(R->Work,Worker,DelayedWorkQueue,R);
}
static void Schedule(void *Context,NTSTATUS Error){
 auto R=(T70_AFE_IRQ_RECOVERY *)Context;
 if(InterlockedCompareExchange(&R->Fault,1,0)!=0)return;
 R->OriginalError=Error;
 InterlockedCompareExchange(&R->Fault,2,1);
 KeInsertQueueDpc(&R->Dpc,nullptr,nullptr);
}
NTSTATUS T70AfeIrqRecoveryInit(T70_AFE_IRQ_RECOVERY *R,PUNKNOWN Parent,PDEVICE_OBJECT Device){
 if(!R||!Parent||!Device||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(R->Initialized||R->Parent||R->Work)return STATUS_INVALID_DEVICE_STATE;
 PIO_WORKITEM W=IoAllocateWorkItem(Device);if(!W)return STATUS_INSUFFICIENT_RESOURCES;
 Parent->AddRef();R->Parent=Parent;R->Work=W;
 KeInitializeMutex(&R->Mutex,0);KeInitializeEvent(&R->Done,NotificationEvent,FALSE);
 KeInitializeDpc(&R->Dpc,Dpc,R);
 R->Source={&R->Owner,Schedule,R,STATUS_DEVICE_NOT_READY,0};
 R->RecoveryStatus=STATUS_DEVICE_NOT_READY;R->Initialized=true;return STATUS_SUCCESS;
}
NTSTATUS T70AfeIrqRecoveryConnect(T70_AFE_IRQ_RECOVERY *R,IPortWaveCyclic *P,
 const T70_AFE_INTERRUPT_RESOURCE &Resource,const T70_AFE_DL1_INTERRUPT &H,
 NTSTATUS (*Ready)(void *),void *Context){
 if(!R||!R->Initialized||R->Closed||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(R->Closing||Load(&R->Published))return STATUS_INVALID_DEVICE_STATE;
 KeWaitForSingleObject(&R->Mutex,Executive,KernelMode,FALSE,nullptr);
 auto Handler=H;Handler.OnFault=T70AfeIrqSourceFault;Handler.FaultContext=&R->Source;
 NTSTATUS S=T70AfeInterruptConnect(&R->Owner,R->Parent,P,Resource,Handler,Ready,Context);
 InterlockedCompareExchange(&R->Published,1,0);
 KeReleaseMutex(&R->Mutex,FALSE);
 // An early DPC may have run before Connect returned. Publish retries the queue,
 // never the hardware operation. Queued CAS makes the work item exactly once.
 if(Load(&R->Fault)==2)KeInsertQueueDpc(&R->Dpc,nullptr,nullptr);
 return S;
}
NTSTATUS T70AfeIrqRecoverySurpriseRemove(T70_AFE_IRQ_RECOVERY *R,NTSTATUS (*Drain)(void *),void *Context){
 if(!R||!R->Initialized||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(R->Closed)return STATUS_SUCCESS;
 // Publish before waiting behind queued recovery. New synchronized mask/ISR
 // transactions must see removal even when a worker already owns Mutex.
 // An already admitted transaction can finish; MarkRemoved below joins it.
 InterlockedCompareExchange(&R->RemovalRequested,1,0);
 InterlockedCompareExchange(&R->Owner.Removed,1,0);
 InterlockedCompareExchange(&R->Owner.Handler.Faulted,1,0);
 KeWaitForSingleObject(&R->Mutex,Executive,KernelMode,FALSE,nullptr);
 R->Closing=true;
 LONG V=Load(&R->ObservationGate);
 for(;;){LONG Old=InterlockedCompareExchange(&R->ObservationGate,V|ObservationClosing,V);if(Old==V)break;V=Old;}
 NTSTATUS S=T70AfeInterruptMarkRemoved(&R->Owner);
 KeReleaseMutex(&R->Mutex,FALSE);
 if(S!=STATUS_SUCCESS)return S;
 return T70AfeIrqRecoveryClose(R,Drain,Context);
}
NTSTATUS T70AfeIrqRecoveryClose(T70_AFE_IRQ_RECOVERY *R,NTSTATUS (*Drain)(void *),void *Context){
 if(!R||!R->Initialized||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(R->Closed)return STATUS_SUCCESS;
 KeWaitForSingleObject(&R->Mutex,Executive,KernelMode,FALSE,nullptr);
 R->Closing=true;
 NTSTATUS S=Quiesce(R);
 KeReleaseMutex(&R->Mutex,FALSE);if(S!=STATUS_SUCCESS)return S;
 // No new ISR after successful disconnect; failed Connect also owns no IRQ.
 // No producer can queue after this point under the wrapper lifetime contract.
 KeFlushQueuedDpcs();
 if(Load(&R->Queued)){
  S=KeWaitForSingleObject(&R->Done,Executive,KernelMode,FALSE,nullptr);
  if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 }
 // Worker has finished using owner. Do not hold Mutex while waiting or draining.
 S=T70AfeInterruptClose(&R->Owner,T70AfeIrqSourceMask,Drain,Context);
 if(S!=STATUS_SUCCESS)return S;
 IoFreeWorkItem(R->Work);R->Work=nullptr;R->Closed=true;
 auto Parent=R->Parent;R->Parent=nullptr;Parent->Release();return STATUS_SUCCESS;
}

struct ExecuteContext {T70_AFE_IRQ_RECOVERY *Recovery;NTSTATUS (*Function)(void *);void *Context;NTSTATUS Result;};
static BOOLEAN ExecuteSynchronized(void *C){
 auto X=(ExecuteContext *)C;
 // ISR may have faulted after the outer mutex-side check but before this
 // interrupt spin lock was acquired. Never restart an ISR-masked source.
 if(Load(&X->Recovery->RemovalRequested)||Load(&X->Recovery->Owner.Removed)||Load(&X->Recovery->Owner.Handler.Faulted))return TRUE;
 X->Result=X->Function(X->Context);return TRUE;
}
NTSTATUS T70AfeIrqRecoveryExecute(void *Context,NTSTATUS (*Function)(void *),void *Argument){
 auto R=(T70_AFE_IRQ_RECOVERY *)Context;
 if(!R||!Function||!R->Initialized||R->Closed||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 KeWaitForSingleObject(&R->Mutex,Executive,KernelMode,FALSE,nullptr);
 NTSTATUS S=STATUS_DEVICE_NOT_READY;
 if(!R->Closing&&!(Load(&R->ObservationGate)&ObservationClosing)&&R->Owner.Interrupt&&!R->Owner.ConnectionUnknown&&!Load(&R->Owner.Handler.Faulted)){
  ExecuteContext X={R,Function,Argument,STATUS_DEVICE_NOT_READY};
  if(KeSynchronizeExecution(R->Owner.Interrupt,ExecuteSynchronized,&X))S=X.Result;
 }
 KeReleaseMutex(&R->Mutex,FALSE);return S==0?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);
}

NTSTATUS T70AfeIrqRecoveryObserve(void *Context,NTSTATUS (*Function)(void *),void *Argument){
 auto R=(T70_AFE_IRQ_RECOVERY *)Context;
 if(!R||!Function||KeGetCurrentIrql()>DISPATCH_LEVEL)return STATUS_INVALID_PARAMETER;
 LONG V=Load(&R->ObservationGate);
 for(;;){
  if((V&ObservationClosing)||V==ObservationClosing-1)return STATUS_DEVICE_NOT_READY;
  LONG Old=InterlockedCompareExchange(&R->ObservationGate,V+1,V);if(Old==V)break;V=Old;
 }
 NTSTATUS S=STATUS_DEVICE_NOT_READY;
 if(Load(&R->Published)&&R->Owner.Interrupt&&!R->Owner.ConnectionUnknown){
  ExecuteContext X={R,Function,Argument,STATUS_DEVICE_NOT_READY};
  if(KeSynchronizeExecution(R->Owner.Interrupt,ExecuteSynchronized,&X))S=X.Result;
 }
 InterlockedDecrement(&R->ObservationGate);
 return S==0?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);
}
