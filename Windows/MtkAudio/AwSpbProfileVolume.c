// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbProfileVolume.h"
static uint16_t U16(const uint8_t *P){return (uint16_t)(P[0]|((uint16_t)P[1]<<8));}
static int32_t Read(void *C,uint16_t A,uint16_t *V){USHORT X;NTSTATUS S=T70AwSpbReadRegister(C,A,&X);if(!S)*V=X;return S;}
static int32_t Write(void *C,uint16_t A,uint16_t V){return T70AwSpbWriteRegister(C,A,V);}
T70_AW_PROFILE_VOLUME_RESULT T70AwSpbApplyProfileVolume(const void *Acf,size_t Bytes,
 uint64_t Base,uint16_t Address,uint16_t Channel,uint16_t Profile,
 WDFIOTARGET Target,uint32_t Attenuation,uint16_t *Applied){
 T70_AW_PROFILE_VOLUME_RESULT R={T70AwProfileVolumeInvalid,0,0};T70_AW_PROFILE_VIEW V;
 uintptr_t A=(uintptr_t)Acf,O=(uintptr_t)Applied;
 if(!Acf||!Target||!Applied||KeGetCurrentIrql()!=PASSIVE_LEVEL||Attenuation>720||
    Bytes>UINTPTR_MAX-A||O%_Alignof(uint16_t)||sizeof(*Applied)>UINTPTR_MAX-O||
    (A<O+sizeof(*Applied)&&O<A+Bytes))return R;
 R.Code=T70AwProfileVolumeBadProfile;
 if(!T70AwProfileView(Acf,Bytes,Base,Address,Channel,Profile,&V))return R;
 uint16_t Raw=0;unsigned Found=0;
 for(uint32_t I=0;I<V.RecordCount;I++){
  const uint8_t *P=V.Section.Data+64+(size_t)I*4;
  if(U16(P)==T70_AW88261_VOLUME_REGISTER){Raw=U16(P+2);if(++Found>1)return R;}
 }
 if(Found!=1)return R;
 USHORT Id=0;R.TransportStatus=T70AwSpbReadRegister(Target,0,&Id);
 if(R.TransportStatus){R.Code=T70AwProfileVolumeIdReadFailed;return R;}
 if(Id!=0x2113){R.Code=T70AwProfileVolumeWrongId;return R;}
 T70_AW_VOLUME_IO Io={Target,Read,Write};uint16_t Updated;
 T70_AW_VOLUME_UPDATE_RESULT U=T70Aw88261UpdateVolume(&Io,T70Aw88261DecodeInitialVolume(Raw),Attenuation,&Updated);
 R.TransportStatus=U.TransportStatus;
 if(U.Code==T70AwVolumeReadFailed){R.Code=T70AwProfileVolumeReadFailed;return R;}
 R.WriteAttempted=1;
 if(U.Code!=T70AwVolumeUpdated){R.Code=T70AwProfileVolumeWriteFailed;return R;}
 *Applied=Updated;R.Code=T70AwProfileVolumeApplied;return R;
}
