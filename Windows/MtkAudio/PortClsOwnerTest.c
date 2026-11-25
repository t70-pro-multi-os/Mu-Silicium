// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "PortClsOwner.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks,Irql,Seq,CreateCalls,PcCalls,FreeCalls,PortCalls;
static unsigned NoPort;
static NTSTATUS CreateStatus,PcStatus;static T70_PORTCLS_OWNER *Reenter;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
static NTSTATUS Dispatch(PDEVICE_OBJECT D,struct _IRP *I){(void)D;(void)I;return 0;}
static NTSTATUS OldDispatch(PDEVICE_OBJECT D,struct _IRP *I){(void)D;(void)I;return 0;}
static NTSTATUS Add(PDRIVER_OBJECT D,PDEVICE_OBJECT P){(void)D;(void)P;return 0;}
static NTSTATUS OldAdd(PDRIVER_OBJECT D,PDEVICE_OBJECT P){(void)D;(void)P;return 0;}
static void OldUnload(PDRIVER_OBJECT D){(void)D;assert(0);}
static void ClientUnload(PDRIVER_OBJECT D){(void)D;assert(0);}
static void PortUnload(PDRIVER_OBJECT D){C(D!=NULL);C(Seq++==2);PortCalls++;if(Reenter)C(T70PortClsFinishUnload(Reenter)==0);}
NTSTATUS WdfDriverCreate(PDRIVER_OBJECT D,PUNICODE_STRING R,WDF_OBJECT_ATTRIBUTES *A,WDF_DRIVER_CONFIG *Config,WDFDRIVER *F){
 C(D&&R&&!A);C(!Config->EvtDriverDeviceAdd);C(Config->DriverInitFlags==WdfDriverInitNoDispatchOverride);
 C(Seq++==0);CreateCalls++;if(NT_SUCCESS(CreateStatus))*F=(void *)0x1234;return CreateStatus;
}
NTSTATUS PcInitializeAdapterDriver(PDRIVER_OBJECT D,PUNICODE_STRING R,PDRIVER_ADD_DEVICE A){
 C(R!=NULL);C(A==Add);C(Seq++==1);PcCalls++;D->DriverUnload=NoPort?NULL:PortUnload;D->DriverExtension->AddDevice=A;
 for(unsigned I=0;I<28;I++)D->MajorFunction[I]=Dispatch;
 return PcStatus;
}
void WdfDriverMiniportUnload(WDFDRIVER F){C(F==(void *)0x1234);C(Seq++==(NT_SUCCESS(PcStatus)?3u-NoPort:2u));FreeCalls++;}
int main(void){
 WCHAR Path[]={1,2,0};UNICODE_STRING R={4,6,Path};DRIVER_EXTENSION E={OldAdd};
 DRIVER_OBJECT D={0};D.DriverExtension=&E;D.DriverUnload=OldUnload;
 for(unsigned I=0;I<28;I++)D.MajorFunction[I]=OldDispatch;
 DRIVER_OBJECT Original=D;T70_PORTCLS_OWNER O={0};
 C(T70PortClsInitialize(NULL,&D,&R,Add,ClientUnload)==STATUS_INVALID_PARAMETER);
 C(T70PortClsInitialize(&O,NULL,&R,Add,ClientUnload)==STATUS_INVALID_PARAMETER);
 C(T70PortClsInitialize(&O,&D,&R,NULL,ClientUnload)==STATUS_INVALID_PARAMETER);
 C(T70PortClsInitialize(&O,&D,&R,Add,NULL)==STATUS_INVALID_PARAMETER);
 for(unsigned N=0;N<9;N++){UNICODE_STRING Bad=R;Bad.Length=(USHORT)N;
 if(N==2||N==4||N==6)continue;
 C(T70PortClsInitialize(&O,&D,&Bad,Add,ClientUnload)==STATUS_INVALID_PARAMETER);}
 Irql=2;C(T70PortClsInitialize(&O,&D,&R,Add,ClientUnload)==STATUS_INVALID_PARAMETER);Irql=0;
 C(T70PortClsInitialize(&O,&D,NULL,Add,ClientUnload)==STATUS_INVALID_PARAMETER);
 UNICODE_STRING NoBuffer=R;NoBuffer.Buffer=NULL;
 C(T70PortClsInitialize(&O,&D,&NoBuffer,Add,ClientUnload)==STATUS_INVALID_PARAMETER);
 D.DriverExtension=NULL;C(T70PortClsInitialize(&O,&D,&R,Add,ClientUnload)==STATUS_INVALID_PARAMETER);D.DriverExtension=&E;
 C(!CreateCalls&&!PcCalls&&!FreeCalls);
 CreateStatus=STATUS_INSUFFICIENT_RESOURCES;
 C(T70PortClsInitialize(&O,&D,&R,Add,ClientUnload)==CreateStatus);
 C(!O.Driver&&!O.Framework&&!O.PortUnload);C(!memcmp(&D,&Original,sizeof(D)));C(!PcCalls&&!FreeCalls);
 Seq=0;CreateStatus=0;PcStatus=STATUS_INVALID_DEVICE_STATE;
 C(T70PortClsInitialize(&O,&D,&R,Add,ClientUnload)==PcStatus);
 C(!O.Driver&&!O.Framework&&!O.PortUnload);C(!memcmp(&D,&Original,sizeof(D)));C(E.AddDevice==OldAdd);C(FreeCalls==1);
 Seq=0;PcStatus=0;C(T70PortClsInitialize(&O,&D,&R,Add,ClientUnload)==0);
 C(O.Driver==&D&&O.Framework==(void *)0x1234&&O.PortUnload==PortUnload);
 C(D.DriverUnload==ClientUnload&&E.AddDevice==Add);for(unsigned I=0;I<28;I++)C(D.MajorFunction[I]==Dispatch);
 C(T70PortClsInitialize(&O,&D,&R,Add,ClientUnload)==STATUS_INVALID_DEVICE_STATE);
 Irql=2;C(T70PortClsFinishUnload(&O)==STATUS_INVALID_PARAMETER);Irql=0;
 Reenter=&O;C(T70PortClsFinishUnload(&O)==0);C(PortCalls==1&&FreeCalls==2);C(!O.Driver&&!O.Framework&&!O.PortUnload);
 C(T70PortClsFinishUnload(&O)==0);C(PortCalls==1&&FreeCalls==2);
 Seq=0;NoPort=1;D=Original;E.AddDevice=OldAdd;
 C(T70PortClsInitialize(&O,&D,&R,Add,ClientUnload)==0);C(O.PortUnload==NULL);
 C(T70PortClsFinishUnload(&O)==0);C(PortCalls==1&&FreeCalls==3);
 O.Driver=&D;C(T70PortClsFinishUnload(&O)==STATUS_INVALID_DEVICE_STATE);C(O.Driver==&D);
 printf("PortCls owner: %u checks passed\n",Checks);return 0;
}
