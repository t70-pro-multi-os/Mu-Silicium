// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbTarget.h"
#include <reshub.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Irql,Phase,Failure,CloseCount,DeleteCount;
static uint64_t Id;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
NTSTATUS RESOURCE_HUB_CREATE_PATH_FROM_ID(UNICODE_STRING *P,ULONG L,ULONG H){
 C(Phase++==0);C((((uint64_t)H<<32)|L)==Id);C(P->Length==0&&P->MaximumLength==RESOURCE_HUB_PATH_SIZE&&P->Buffer);
 P->Buffer[0]=1;P->Length=2;return Failure==1?STATUS_INVALID_PARAMETER:0;
}
NTSTATUS WdfIoTargetCreate(WDFDEVICE D,WDF_OBJECT_ATTRIBUTES *A,WDFIOTARGET *T){
 C(Phase++==1);C(D==(void *)1&&!A);if(Failure==2)return STATUS_INSUFFICIENT_RESOURCES;*T=(void *)2;return 0;
}
NTSTATUS WdfIoTargetOpen(WDFIOTARGET T,WDF_IO_TARGET_OPEN_PARAMS *P){
 C(Phase++==2);C(T==(void *)2);C(P->Name->Length==2&&P->Name->Buffer[0]==1);
 C(P->DesiredAccess==(GENERIC_READ|GENERIC_WRITE)&&P->ShareAccess==0&&P->CreateDisposition==FILE_OPEN&&P->FileAttributes==FILE_ATTRIBUTE_NORMAL);
 return Failure==3?STATUS_IO_TIMEOUT:0;
}
void WdfIoTargetClose(WDFIOTARGET T){C(T==(void *)2);C(!CloseCount&&!DeleteCount);CloseCount++;}
void WdfObjectDelete(void *T){C(T==(void *)2);DeleteCount++;}
int main(void){
 const NTSTATUS Expected[]={STATUS_SUCCESS,STATUS_INVALID_PARAMETER,STATUS_INSUFFICIENT_RESOURCES,STATUS_IO_TIMEOUT};
 for(unsigned F=0;F<4;F++)for(unsigned I=0;I<3;I++){
  T70_AW_SPB_TARGET_OWNER O={0};Phase=CloseCount=DeleteCount=0;Failure=F;
  Id=I==0?0:I==1?UINT64_MAX:UINT64_C(0x1234567887654321);
  NTSTATUS S=T70AwSpbTargetAcquire(&O,(void *)1,Id,0x11e01000,0x34,0);
  C(S==Expected[F]);C(Phase==(F==0?3:F));C(DeleteCount==(F==3));C(CloseCount==0);
  if(F){C(!O.Target&&!O.ActiveUses);continue;}
  C(O.Target==(void *)2&&!O.ActiveUses);C(T70AwSpbTargetBegin(&O)==0);C(O.ActiveUses==1);
  C(T70AwSpbTargetRelease(&O)==STATUS_INVALID_DEVICE_STATE);C(!CloseCount&&!DeleteCount);
  C(T70AwSpbTargetAcquire(&O,(void *)1,Id,0x11e01000,0x34,0)==STATUS_INVALID_DEVICE_STATE);
  C(T70AwSpbTargetEnd(&O)==0);C(T70AwSpbTargetEnd(&O)==STATUS_INVALID_DEVICE_STATE);
  C(T70AwSpbTargetRelease(&O)==0);C(!O.Target&&!O.ActiveUses&&CloseCount==1&&DeleteCount==1);
  C(T70AwSpbTargetRelease(&O)==0);C(CloseCount==1&&DeleteCount==1);
 }
 T70_AW_SPB_TARGET_OWNER O={0};Phase=0;Failure=0;
 C(T70AwSpbTargetAcquire(&O,NULL,0,0x11e01000,0x34,0)==STATUS_INVALID_PARAMETER);
 C(T70AwSpbTargetAcquire(&O,(void *)1,0,0x11e01000,0x35,0)==STATUS_INVALID_PARAMETER);C(Phase==0);
 C(T70AwSpbTargetBegin(&O)==STATUS_INVALID_DEVICE_STATE);C(T70AwSpbTargetEnd(&O)==STATUS_INVALID_DEVICE_STATE);
 O.Target=(void *)2;O.ActiveUses=UINT32_MAX;C(T70AwSpbTargetBegin(&O)==STATUS_INVALID_DEVICE_STATE);C(O.ActiveUses==UINT32_MAX);
 Irql=1;T70_AW_SPB_TARGET_OWNER Old=O;
 C(T70AwSpbTargetAcquire(&O,(void *)1,0,0x11e01000,0x34,0)==STATUS_INVALID_DEVICE_STATE);
 C(T70AwSpbTargetBegin(&O)==STATUS_INVALID_DEVICE_STATE);C(T70AwSpbTargetEnd(&O)==STATUS_INVALID_DEVICE_STATE);C(T70AwSpbTargetRelease(&O)==STATUS_INVALID_DEVICE_STATE);C(!memcmp(&O,&Old,sizeof O));
 C(T70AwSpbTargetBegin(NULL)==STATUS_INVALID_PARAMETER);C(T70AwSpbTargetEnd(NULL)==STATUS_INVALID_PARAMETER);C(T70AwSpbTargetRelease(NULL)==STATUS_INVALID_PARAMETER);
 printf("SpbTarget: %u checks; reparse-open only, MtkI2c validates descriptor\n",Checks);return 0;
}
