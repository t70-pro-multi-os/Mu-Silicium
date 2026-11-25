// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicService.h"
static constexpr LONG Closing=0x40000000,Paused=0x20000000,CountMask=0x1fffffff;
NTSTATUS T70WaveCyclicServiceAcquire(T70_WAVECYCLIC_SERVICE *O,IPortWaveCyclic *P){
 if(!O||!P||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Attempted||O->Port||O->Group||O->Gate)return STATUS_INVALID_DEVICE_STATE;
 O->Attempted=true;P->AddRef();PSERVICEGROUP G=nullptr;
 NTSTATUS S=PcNewServiceGroup(&G,nullptr);
 if(S!=STATUS_SUCCESS){P->Release();return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 if(!G){P->Release();return STATUS_DEVICE_DATA_ERROR;}
 O->Port=P;O->Group=G;return STATUS_SUCCESS;
}
bool T70WaveCyclicServiceNotifyConfirmed(T70_WAVECYCLIC_SERVICE *O){
 if(!O)return false;
 LONG V=InterlockedCompareExchange(&O->Gate,0,0);
 for(;;){
  if(V&Closing)return false;
  // Pause can linearize immediately before Close. No pointer access on this
  // path: the paused notification is deliberately accepted without submission.
  if(V&Paused)return true;
  if((V&CountMask)==CountMask)return false;
  LONG Old=InterlockedCompareExchange(&O->Gate,V+1,V);
  if(Old==V)break;V=Old;
 }
 // Acquire completion precedes installing ISR; Close cannot free while counted.
 bool Live=O->Port&&O->Group;
 if(Live)O->Port->Notify(O->Group);
 InterlockedDecrement(&O->Gate);return Live;
}
NTSTATUS T70WaveCyclicServicePause(T70_WAVECYCLIC_SERVICE *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Port||!O->Group)return STATUS_DEVICE_NOT_READY;
 LONG V=InterlockedCompareExchange(&O->Gate,0,0);
 for(;;){
  if(V&Closing)return STATUS_DEVICE_NOT_READY;
  LONG Old=InterlockedCompareExchange(&O->Gate,V|Paused,V);
  if(Old==V)break;V=Old;
 }
 return (InterlockedCompareExchange(&O->Gate,0,0)&CountMask)?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicServiceResume(T70_WAVECYCLIC_SERVICE *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Port||!O->Group)return STATUS_DEVICE_NOT_READY;
 LONG V=InterlockedCompareExchange(&O->Gate,0,0);
 for(;;){
  if((V&Closing)||(V&CountMask))return STATUS_DEVICE_NOT_READY;
  LONG Old=InterlockedCompareExchange(&O->Gate,V&~Paused,V);
  if(Old==V)return STATUS_SUCCESS;V=Old;
 }
}
NTSTATUS T70WaveCyclicServiceClose(T70_WAVECYCLIC_SERVICE *O,NTSTATUS (*Confirm)(void *),void *C){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 LONG V=InterlockedCompareExchange(&O->Gate,0,0);
 for(;;){LONG Old=InterlockedCompareExchange(&O->Gate,V|Closing,V);if(Old==V)break;V=Old;}
 if(InterlockedCompareExchange(&O->Gate,0,0)&CountMask)return STATUS_DEVICE_NOT_READY;
 if(!O->Port&&!O->Group)return STATUS_SUCCESS;
 if(!O->Port||!O->Group||!Confirm)return STATUS_DEVICE_NOT_READY;
 NTSTATUS S=Confirm(C);if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 auto P=O->Port;auto G=O->Group;O->Port=nullptr;O->Group=nullptr;
 G->Release();P->Release();return STATUS_SUCCESS;
}
