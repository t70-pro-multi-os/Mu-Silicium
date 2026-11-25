// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbResources.h"
#include <reshub.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Irql,Creates,Opens,Closes,Deletes,Applies,FailCreate,FailOpen,FailApply,QueryCalls;
static uint64_t PathId;
static unsigned Validations,FailValidate;
static T70_AW_SPB_RESOURCES *Owner;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
NTSTATUS RESOURCE_HUB_CREATE_PATH_FROM_ID(UNICODE_STRING *P,ULONG L,ULONG H){PathId=((uint64_t)H<<32)|L;C(P&&P->Buffer);P->Length=2;return 0;}
NTSTATUS WdfIoTargetCreate(WDFDEVICE D,WDF_OBJECT_ATTRIBUTES *A,WDFIOTARGET *T){
 C(D==(void *)(uintptr_t)(10+Creates));C(!A);Creates++;
 if(Creates==FailCreate)return STATUS_INSUFFICIENT_RESOURCES;
 *T=(void *)(uintptr_t)(100+Creates);return 0;
}
NTSTATUS WdfIoTargetOpen(WDFIOTARGET T,WDF_IO_TARGET_OPEN_PARAMS *P){
 Opens++;C(T==(void *)(uintptr_t)(100+Opens));C(PathId==UINT64_C(0x123400000000)+Opens);
 C(P->ShareAccess==0);return Opens==FailOpen?STATUS_IO_TIMEOUT:0;
}
NTSTATUS WdfIoTargetSendIoctlSynchronously(WDFIOTARGET T,void *Request,unsigned Code,
 WDF_MEMORY_DESCRIPTOR *Input,WDF_MEMORY_DESCRIPTOR *Output,WDF_REQUEST_SEND_OPTIONS *Options,ULONG_PTR *Bytes){
 (void)T;(void)Request;(void)Code;(void)Input;(void)Output;(void)Options;(void)Bytes;
 QueryCalls++;C(FALSE);return STATUS_DEVICE_NOT_READY;
}

void WdfIoTargetClose(WDFIOTARGET T){C(T);Closes++;}
void WdfObjectDelete(void *T){C(T);Deletes++;}
int T70AwSpbValidateProfileSerial(const void *A,size_t N,uint64_t B,
 uint16_t Addr,uint16_t Ch,uint16_t P){
 C(A==(void *)0x1000&&N==100&&B==0x11e01000&&P==10);
 C(Ch==Validations&&Addr==0x34+Ch);Validations++;return Validations!=FailValidate;
}
T70_AW_SERIAL_APPLY_RESULT T70AwSpbApplyProfileSerial(const void *A,size_t N,
 uint64_t B,uint16_t Addr,uint16_t Ch,uint16_t P,WDFIOTARGET T){
 C(A==(void *)0x1000&&N==100&&P==10&&B==0x11e01000);
 C(Ch==Applies&&Addr==0x34+Ch&&T==Owner->Target[Ch].Target);
 C(Owner->Target[0].ActiveUses==1&&Owner->Target[1].ActiveUses==1);
 C(T70AwSpbResourcesRelease(Owner)==STATUS_INVALID_DEVICE_STATE);C(!Closes);
 Applies++;T70_AW_SERIAL_APPLY_RESULT R={T70AwSerialFieldsWritten,0,0,2,2};
 if(Applies==FailApply){R.Code=T70AwSerialWriteFailed;R.TransportStatus=STATUS_IO_TIMEOUT;R.FailedRegister=7;R.WritesCompleted=1;}
 return R;
}
static unsigned VolumeCalls,ReadCalls,VolumeFailure,EndFailure,ExpectedChannel;
static NTSTATUS ReadFailure;
T70_AW_PROFILE_VOLUME_RESULT T70AwSpbApplyProfileVolume(const void *A,size_t N,
 uint64_t B,uint16_t Addr,uint16_t Ch,uint16_t P,WDFIOTARGET T,uint32_t Att,uint16_t *Out){
 C(A==(void *)0x1000&&N==100&&B==0x11e01000&&Addr==0x34+Ch&&P==10&&Att==96);
 C(Ch==ExpectedChannel&&T==Owner->Target[Ch].Target&&Owner->Target[Ch].ActiveUses==1);
 C(!Owner->Target[1-Ch].ActiveUses);VolumeCalls++;
 C(T70AwSpbResourcesRelease(Owner)==STATUS_INVALID_DEVICE_STATE);
 T70_AW_PROFILE_VOLUME_RESULT R={T70AwProfileVolumeApplied,0,1};
 if(VolumeFailure==1)R=(T70_AW_PROFILE_VOLUME_RESULT){T70AwProfileVolumeBadProfile,0,0};
 if(VolumeFailure==2)R=(T70_AW_PROFILE_VOLUME_RESULT){T70AwProfileVolumeReadFailed,STATUS_IO_TIMEOUT,0};
 if(VolumeFailure==3)R=(T70_AW_PROFILE_VOLUME_RESULT){T70AwProfileVolumeWriteFailed,STATUS_IO_TIMEOUT,1};
 if(VolumeFailure==4)R=(T70_AW_PROFILE_VOLUME_RESULT){T70AwProfileVolumeWrongId,0,0};
 if(VolumeFailure==5)R=(T70_AW_PROFILE_VOLUME_RESULT){T70AwProfileVolumeReadFailed,((NTSTATUS)0x103),0};
 if(R.Code==T70AwProfileVolumeApplied)*Out=0xa123;
 if(EndFailure)Irql=1; // Fault injection: End refuses without dropping the lease.
 return R;
}
NTSTATUS T70AwSpbReadRegister(WDFIOTARGET T,USHORT Reg,USHORT *Out){
 C(T==Owner->Target[ExpectedChannel].Target&&Reg==5);
 C(Owner->Target[ExpectedChannel].ActiveUses==1&&!Owner->Target[1-ExpectedChannel].ActiveUses);
 ReadCalls++;*Out=0xb456;if(EndFailure)Irql=1;return ReadFailure;
}
static void VolumeTests(const T70_AW_SPB_RESOURCE R[2]){
 for(unsigned Ch=0;Ch<2;Ch++)for(unsigned F=0;F<6;F++)for(unsigned E=0;E<2;E++){
  Creates=Opens=Closes=Deletes=FailCreate=FailOpen=Irql=0;
  T70_AW_SPB_RESOURCES O={0};Owner=&O;ExpectedChannel=Ch;VolumeFailure=F;EndFailure=E;VolumeCalls=0;
  C(T70AwSpbResourcesAcquire(&O,R)==0);uint16_t Out=0xdead;
  T70_AW_SPB_VOLUME_RESULT V=T70AwSpbResourcesApplyVolume(&O,(void *)0x1000,100,10,(uint16_t)Ch,96,&Out);
  C(VolumeCalls==1);C((V.Status==0)==(!F&&!E));C(Out==(!F&&!E?0xa123:0xdead));
  C((V.EndStatus!=0)==(E!=0));C(V.Apply.WriteAttempted==(F==0||F==3));
  if(F==2||F==3)C(V.Status==STATUS_IO_TIMEOUT&&V.Apply.TransportStatus==STATUS_IO_TIMEOUT);
  if(F==5)C(V.Status==STATUS_DEVICE_NOT_READY&&V.Apply.TransportStatus==((NTSTATUS)0x103));
  C(O.Target[Ch].ActiveUses==(E?1u:0u));Irql=0;if(E)C(T70AwSpbTargetEnd(&O.Target[Ch])==0);
  C(T70AwSpbResourcesRelease(&O)==0);
 }
 VolumeFailure=0;EndFailure=0;
 for(unsigned Ch=0;Ch<2;Ch++)for(unsigned F=0;F<3;F++)for(unsigned E=0;E<2;E++){
  Creates=Opens=Closes=Deletes=Irql=0;T70_AW_SPB_RESOURCES O={0};Owner=&O;ExpectedChannel=Ch;
  EndFailure=E;ReadFailure=F==1?STATUS_IO_TIMEOUT:F==2?((NTSTATUS)0x103):0;ReadCalls=0;
  C(T70AwSpbResourcesAcquire(&O,R)==0);uint16_t Out=0xdead;
  NTSTATUS S=T70AwSpbResourcesReadVolume(&O,(uint16_t)Ch,&Out);
  C(ReadCalls==1);C((S==0)==(!F&&!E));C(Out==(!F&&!E?0xb456:0xdead));
  Irql=0;if(E)C(T70AwSpbTargetEnd(&O.Target[Ch])==0);C(T70AwSpbResourcesRelease(&O)==0);
 }
 EndFailure=0;ReadFailure=0;
 for(unsigned F=0;F<10;F++){
  Creates=Opens=Closes=Deletes=Irql=VolumeCalls=ReadCalls=0;T70_AW_SPB_RESOURCES O={0};Owner=&O;
  C(T70AwSpbResourcesAcquire(&O,R)==0);uint16_t Out=0xdead;void *A=(void *)0x1000;size_t N=100;
  uint16_t Ch=0;uint32_t Att=96;uint16_t *Dest=&Out;
  switch(F){case 0:Ch=2;break;case 1:A=NULL;break;case 2:N=0;break;
   case 3:A=(void *)UINTPTR_MAX;break;case 4:Att=721;break;case 5:Dest=NULL;break;
   case 6:Dest=(uint16_t *)&O;break;case 7:O.Open=0;break;case 8:O.Target[0].ActiveUses=1;break;case 9:Irql=1;break;}
  C(T70AwSpbResourcesApplyVolume(&O,A,N,10,Ch,Att,Dest).Status!=0);
  C(!VolumeCalls&&!ReadCalls&&Out==0xdead);
  if(F==0||F>=5)C(T70AwSpbResourcesReadVolume(&O,Ch,Dest)!=0);
  C(!ReadCalls);Irql=0;O.Target[0].ActiveUses=0;C(T70AwSpbResourcesRelease(&O)==0);
 }
}
static void Reset(void){Creates=Opens=Closes=Deletes=Applies=FailCreate=FailOpen=FailApply=Irql=Validations=FailValidate=QueryCalls=0;}
int main(void){
 CM_PARTIAL_RESOURCE_DESCRIPTOR D[2]={0};T70_AW_SPB_RESOURCE R[2];
 for(unsigned I=0;I<2;I++){
  D[I].Type=CmResourceTypeConnection;D[I].u.Connection.Class=CM_RESOURCE_CONNECTION_CLASS_SERIAL;
  D[I].u.Connection.Type=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C;
  D[I].u.Connection.IdHighPart=0x1234;D[I].u.Connection.IdLowPart=I+1;
  R[I]=(T70_AW_SPB_RESOURCE){(void *)(uintptr_t)(10+I),&D[I],0x11e01000,(uint16_t)(0x34+I),(uint16_t)I};
 }
 for(unsigned F=0;F<5;F++){
  Reset();T70_AW_SPB_RESOURCES O={0};Owner=&O;
  if(F==1||F==2)FailCreate=F;else if(F)FailOpen=F-2;
  NTSTATUS S=T70AwSpbResourcesAcquire(&O,R);
  if(F){C(S<0);C(!O.Open&&!O.Target[0].Target&&!O.Target[1].Target);
   C(Closes==((F==2||F==4)?1u:0u));C(Deletes==(F==1?0u:F==4?2u:1u));}
  else{C(S==0&&O.Open);C(T70AwSpbResourcesAcquire(&O,R)==STATUS_INVALID_DEVICE_STATE);
   C(T70AwSpbResourcesRelease(&O)==0);C(Closes==2&&Deletes==2);C(T70AwSpbResourcesRelease(&O)==0);C(Closes==2);}
 }
 for(unsigned F=0;F<3;F++){
  Reset();T70_AW_SPB_RESOURCES O={0};Owner=&O;FailApply=F;
  C(T70AwSpbResourcesAcquire(&O,R)==0);
  T70_AW_SPB_RESOURCES_RESULT A=T70AwSpbResourcesApplySerial(&O,(void *)0x1000,100,10);
  C(A.Status==(F?STATUS_UNSUCCESSFUL:0));C(A.ChannelsCompleted==(F?F-1:2));
  C(A.FailedChannel==(F?F-1:2));C(Applies==(F?F:2));
  if(F){C(A.Channel[F-1].WritesAttempted==2&&A.Channel[F-1].WritesCompleted==1);C(A.Channel[F-1].TransportStatus==STATUS_IO_TIMEOUT);}
  C(!O.Target[0].ActiveUses&&!O.Target[1].ActiveUses&&O.Attempted);
  C(T70AwSpbResourcesApplySerial(&O,(void *)0x1000,100,10).Status==STATUS_INVALID_DEVICE_STATE);
  C(Applies==(F?F:2));C(T70AwSpbResourcesRelease(&O)==0);C(Closes==2);
 }
 for(unsigned F=0;F<10;F++){
  Reset();T70_AW_SPB_RESOURCES O={0};T70_AW_SPB_RESOURCE Bad[2]={R[0],R[1]};
  CM_PARTIAL_RESOURCE_DESCRIPTOR BadD=D[1];Bad[1].Connection=&BadD;
  switch(F){case 0:Bad[1].Device=NULL;break;case 1:Bad[1].Connection=NULL;break;
   case 2:Bad[1].ControllerBase++;break;case 3:Bad[1].Address=0x34;break;
   case 4:Bad[1].Channel=0;break;case 5:BadD.Type=0;break;
   case 6:BadD.u.Connection.Class=0;break;case 7:BadD.u.Connection.Type=0;break;
   case 8:BadD.u.Connection.IdLowPart=1;break;case 9:Irql=1;break;}
  C(T70AwSpbResourcesAcquire(&O,Bad)<0);C(!Creates&&!Opens&&!O.Open);
 }
 for(unsigned F=1;F<=2;F++){
  Reset();T70_AW_SPB_RESOURCES O={0};Owner=&O;FailValidate=F;
  C(T70AwSpbResourcesAcquire(&O,R)==0);
  T70_AW_SPB_RESOURCES_RESULT A=T70AwSpbResourcesApplySerial(&O,(void *)0x1000,100,10);
  C(A.Status==STATUS_INVALID_PARAMETER&&A.FailedChannel==F-1);
  C(!Applies&&!O.Attempted&&!O.Target[0].ActiveUses&&!O.Target[1].ActiveUses);
  C(T70AwSpbResourcesRelease(&O)==0);
 }
 Reset();T70_AW_SPB_RESOURCES O={0};Owner=&O;
 C(T70AwSpbResourcesApplySerial(&O,(void *)0x1000,100,10).Status<0);C(!Applies);
 C(T70AwSpbResourcesAcquire(&O,R)==0);O.Target[1].ActiveUses=1;
 C(T70AwSpbResourcesRelease(&O)==STATUS_INVALID_DEVICE_STATE);C(!Closes);
 C(T70AwSpbResourcesApplySerial(&O,(void *)0x1000,100,10).Status<0);C(!Applies);
 O.Target[1].ActiveUses=0;
 C(T70AwSpbResourcesApplySerial(&O,NULL,100,10).Status<0);
 C(T70AwSpbResourcesApplySerial(&O,(void *)UINTPTR_MAX,2,10).Status<0);C(!Applies&&!O.Attempted);
 C(T70AwSpbResourcesRelease(&O)==0);
 VolumeTests(R);
 printf("SpbResources: %u checks; fake resource/WDF/applicator boundaries\n",Checks);return 0;
}
