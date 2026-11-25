// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioDomainOwner.h"
static volatile LONG Claimed;
NTSTATUS T70AudioDomainAcquire(T70_AUDIO_DOMAIN_OWNER *O,PDEVICE_OBJECT D,PDEVICE_OBJECT P){
 if(!O||!D||!P||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Held||O->Device||O->Pdo)return STATUS_INVALID_DEVICE_STATE;
 PDEVICE_OBJECT Actual=nullptr;NTSTATUS S=PcGetPhysicalDeviceObject(D,&Actual);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(Actual!=P)return STATUS_NO_SUCH_DEVICE;
 if(InterlockedCompareExchange(&Claimed,1,0))return STATUS_DEVICE_NOT_READY;
 ObReferenceObject(D);ObReferenceObject(P);O->Device=D;O->Pdo=P;O->Held=true;
 return STATUS_SUCCESS;
}
NTSTATUS T70AudioDomainRelease(T70_AUDIO_DOMAIN_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Held)return (!O->Device&&!O->Pdo)?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
 if(!O->Device||!O->Pdo)return STATUS_INVALID_DEVICE_STATE;
 auto D=O->Device;auto P=O->Pdo;O->Device=O->Pdo=nullptr;O->Held=false;
 ObDereferenceObject(P);ObDereferenceObject(D);
 (void)InterlockedCompareExchange(&Claimed,0,1);return STATUS_SUCCESS;
}
