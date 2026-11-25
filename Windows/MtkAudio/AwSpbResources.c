// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbResources.h"
#include "AwBoard.h"
NTSTATUS T70AwSpbResourcesAcquire(T70_AW_SPB_RESOURCES *O,const T70_AW_SPB_RESOURCE R[2]){
 if(!O||!R)return STATUS_INVALID_PARAMETER;
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
 if(O->Open||O->Attempted||O->Target[0].Target||O->Target[1].Target||
    O->Target[0].ActiveUses||O->Target[1].ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 uint64_t Id[2];
 for(unsigned I=0;I<2;I++){
  const CM_PARTIAL_RESOURCE_DESCRIPTOR *D=R[I].Connection;
  if(!R[I].Device||!D||R[I].Channel!=I||
     !T70AwBoardTupleValid(R[I].ControllerBase,R[I].Address,R[I].Channel)||
     D->Type!=CmResourceTypeConnection||
     D->u.Connection.Class!=CM_RESOURCE_CONNECTION_CLASS_SERIAL||
     D->u.Connection.Type!=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)return STATUS_INVALID_PARAMETER;
  Id[I]=((uint64_t)D->u.Connection.IdHighPart<<32)|D->u.Connection.IdLowPart;
 }
 if(Id[0]==Id[1])return STATUS_INVALID_PARAMETER;
 for(unsigned I=0;I<2;I++){
  NTSTATUS S=T70AwSpbTargetAcquire(&O->Target[I],R[I].Device,Id[I],
    R[I].ControllerBase,R[I].Address,R[I].Channel);
  if(S!=STATUS_SUCCESS){
   // No leases or requests exist yet; reverse cleanup uses the same thread.
   if(I)(void)T70AwSpbTargetRelease(&O->Target[0]);
   return S;
  }
 }
 O->Open=1;return STATUS_SUCCESS;
}
NTSTATUS T70AwSpbResourcesRelease(T70_AW_SPB_RESOURCES *O){
 if(!O)return STATUS_INVALID_PARAMETER;
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
 // Preflight both: never close half of a pair while the other is leased.
 if(O->Target[0].ActiveUses||O->Target[1].ActiveUses)return STATUS_INVALID_DEVICE_STATE;
 NTSTATUS S=T70AwSpbTargetRelease(&O->Target[1]);if(S!=STATUS_SUCCESS)return S;
 S=T70AwSpbTargetRelease(&O->Target[0]);if(S!=STATUS_SUCCESS)return S;
 O->Open=O->Attempted=0;return STATUS_SUCCESS;
}
T70_AW_SPB_RESOURCES_RESULT T70AwSpbResourcesApplySerial(T70_AW_SPB_RESOURCES *O,
 const void *Acf,size_t Bytes,uint16_t Profile){
 T70_AW_SPB_RESOURCES_RESULT R={0};R.Status=STATUS_INVALID_PARAMETER;R.FailedChannel=2;
 if(!O||!Acf||Bytes>UINTPTR_MAX-(uintptr_t)Acf)return R;
 R.Status=STATUS_INVALID_DEVICE_STATE;
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!O->Open||O->Attempted||
    !O->Target[0].Target||!O->Target[1].Target||
    O->Target[0].Target==O->Target[1].Target||
    O->Target[0].ActiveUses||O->Target[1].ActiveUses)return R;
 // Reject either malformed selected profile before touching either device.
 // Caller keeps ACF immutable through validation and both application stages.
 for(unsigned I=0;I<2;I++)if(!T70AwSpbValidateProfileSerial(Acf,Bytes,0x11e01000,
    (uint16_t)(0x34+I),(uint16_t)I,Profile)){
  R.Status=STATUS_INVALID_PARAMETER;R.FailedChannel=I;
  R.Channel[I].Code=T70AwSerialBadProfile;return R;
 }
 R.Status=T70AwSpbTargetBegin(&O->Target[0]);if(R.Status!=STATUS_SUCCESS)return R;
 R.Status=T70AwSpbTargetBegin(&O->Target[1]);
 if(R.Status!=STATUS_SUCCESS){(void)T70AwSpbTargetEnd(&O->Target[0]);return R;}
 O->Attempted=1;
 for(unsigned I=0;I<2;I++){
  R.Channel[I]=T70AwSpbApplyProfileSerial(Acf,Bytes,0x11e01000,(uint16_t)(0x34+I),
   (uint16_t)I,Profile,O->Target[I].Target);
  if(R.Channel[I].Code!=T70AwSerialFieldsWritten){
   R.FailedChannel=I;R.Status=STATUS_UNSUCCESSFUL;break;
  }
  R.ChannelsCompleted++;
 }
 NTSTATUS E1=T70AwSpbTargetEnd(&O->Target[1]),E0=T70AwSpbTargetEnd(&O->Target[0]);
 if(R.ChannelsCompleted==2)R.Status=STATUS_SUCCESS;
 if(E1!=STATUS_SUCCESS)R.Status=E1;else if(E0!=STATUS_SUCCESS)R.Status=E0;
 return R;
}

static int VolumeOutputValid(const T70_AW_SPB_RESOURCES *O,const uint16_t *Out){
 uintptr_t A=(uintptr_t)O,B=(uintptr_t)Out;
 return O&&Out&&B%_Alignof(uint16_t)==0&&sizeof(*Out)<=UINTPTR_MAX-B&&
  sizeof(*O)<=UINTPTR_MAX-A&&!(A<B+sizeof(*Out)&&B<A+sizeof(*O));
}
static int VolumeOwnerReady(const T70_AW_SPB_RESOURCES *O){
 return KeGetCurrentIrql()==PASSIVE_LEVEL&&O->Open&&O->Target[0].Target&&
  O->Target[1].Target&&O->Target[0].Target!=O->Target[1].Target&&
  !O->Target[0].ActiveUses&&!O->Target[1].ActiveUses;
}
T70_AW_SPB_VOLUME_RESULT T70AwSpbResourcesApplyVolume(T70_AW_SPB_RESOURCES *O,
 const void *Acf,size_t Bytes,uint16_t Profile,uint16_t Channel,uint32_t Attenuation,uint16_t *Applied){
 T70_AW_SPB_VOLUME_RESULT R={STATUS_INVALID_PARAMETER,STATUS_SUCCESS,{T70AwProfileVolumeInvalid,0,0}};
 uintptr_t A=(uintptr_t)Acf,B=(uintptr_t)Applied,P=(uintptr_t)O;
 if(!VolumeOutputValid(O,Applied)||Channel>1||!Acf||!Bytes||Attenuation>720||
    Bytes>UINTPTR_MAX-A||(A<B+sizeof(*Applied)&&B<A+Bytes)||
    (A<P+sizeof(*O)&&P<A+Bytes))return R;
 R.Status=STATUS_INVALID_DEVICE_STATE;if(!VolumeOwnerReady(O))return R;
 R.Status=T70AwSpbTargetBegin(&O->Target[Channel]);if(R.Status!=STATUS_SUCCESS)return R;
 uint16_t Updated=0;
 R.Apply=T70AwSpbApplyProfileVolume(Acf,Bytes,0x11e01000,(uint16_t)(0x34+Channel),
   Channel,Profile,O->Target[Channel].Target,Attenuation,&Updated);
 R.EndStatus=T70AwSpbTargetEnd(&O->Target[Channel]);
 if(R.Apply.Code==T70AwProfileVolumeApplied)R.Status=STATUS_SUCCESS;
 else if(R.Apply.TransportStatus)R.Status=NT_SUCCESS(R.Apply.TransportStatus)?STATUS_DEVICE_NOT_READY:R.Apply.TransportStatus;
 else R.Status=(R.Apply.Code==T70AwProfileVolumeInvalid||R.Apply.Code==T70AwProfileVolumeBadProfile)?
    STATUS_INVALID_PARAMETER:STATUS_DEVICE_DATA_ERROR;
 if(R.Status==STATUS_SUCCESS)R.Status=R.EndStatus;
 if(R.Status==STATUS_SUCCESS)*Applied=Updated;
 return R;
}
NTSTATUS T70AwSpbResourcesReadVolume(T70_AW_SPB_RESOURCES *O,uint16_t Channel,uint16_t *Raw){
 if(!VolumeOutputValid(O,Raw)||Channel>1)return STATUS_INVALID_PARAMETER;
 if(!VolumeOwnerReady(O))return STATUS_INVALID_DEVICE_STATE;
 NTSTATUS S=T70AwSpbTargetBegin(&O->Target[Channel]);if(S!=STATUS_SUCCESS)return S;
 USHORT Value=0;S=T70AwSpbReadRegister(O->Target[Channel].Target,T70_AW88261_VOLUME_REGISTER,&Value);
 NTSTATUS E=T70AwSpbTargetEnd(&O->Target[Channel]);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(E!=STATUS_SUCCESS)return E;
 *Raw=Value;return STATUS_SUCCESS;
}
