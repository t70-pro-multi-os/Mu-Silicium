// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PortClsOwner.h"
#include <portcls.h>
#include <wdfminiport.h>
NTSTATUS T70PortClsInitialize(T70_PORTCLS_OWNER *O,PDRIVER_OBJECT D,
 PUNICODE_STRING R,PDRIVER_ADD_DEVICE Add,PDRIVER_UNLOAD Unload){
 if(!O||!D||!D->DriverExtension||!R||!R->Buffer||!R->Length||
    R->Length>R->MaximumLength||R->Length%sizeof(WCHAR)||!Add||!Unload||
    KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(O->Driver||O->Framework||O->PortUnload)return STATUS_INVALID_DEVICE_STATE;
 PDRIVER_UNLOAD PreviousUnload=D->DriverUnload;
 PDRIVER_ADD_DEVICE PreviousAdd=D->DriverExtension->AddDevice;
 PDRIVER_DISPATCH Previous[IRP_MJ_MAXIMUM_FUNCTION+1];
 for(unsigned I=0;I<=IRP_MJ_MAXIMUM_FUNCTION;I++)Previous[I]=D->MajorFunction[I];
 WDF_DRIVER_CONFIG C;WDF_DRIVER_CONFIG_INIT(&C,WDF_NO_EVENT_CALLBACK);
 C.DriverInitFlags|=WdfDriverInitNoDispatchOverride;
 WDFDRIVER F=NULL;
 NTSTATUS S=WdfDriverCreate(D,R,WDF_NO_OBJECT_ATTRIBUTES,&C,&F);
 if(!NT_SUCCESS(S))return S;
 S=PcInitializeAdapterDriver(D,R,Add);
 if(!NT_SUCCESS(S)){
  WdfDriverMiniportUnload(F);
  D->DriverUnload=PreviousUnload;D->DriverExtension->AddDevice=PreviousAdd;
  for(unsigned I=0;I<=IRP_MJ_MAXIMUM_FUNCTION;I++)D->MajorFunction[I]=Previous[I];
  return S;
 }
 O->Driver=D;O->Framework=F;O->PortUnload=D->DriverUnload;
 D->DriverUnload=Unload;
 return S;
}
NTSTATUS T70PortClsFinishUnload(T70_PORTCLS_OWNER *O){
 if(!O||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(!O->Driver&&!O->Framework&&!O->PortUnload)return STATUS_SUCCESS;
 if(!O->Driver||!O->Framework)return STATUS_INVALID_DEVICE_STATE;
 /* Detach before callbacks: reentry cannot release the same owner twice. */
 T70_PORTCLS_OWNER Saved=*O;
 O->Driver=NULL;O->Framework=NULL;O->PortUnload=NULL;
 if(Saved.PortUnload)Saved.PortUnload(Saved.Driver);
 WdfDriverMiniportUnload(Saved.Framework);
 return STATUS_SUCCESS;
}
