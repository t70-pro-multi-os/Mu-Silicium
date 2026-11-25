// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwKsVolume.h"
#include "AwVolume.h"
static uint16_t U16(const uint8_t *P){return (uint16_t)(P[0]|((uint16_t)P[1]<<8));}
static int Baseline(const void *Acf,size_t Bytes,uint16_t Profile,unsigned Channel,uint16_t *Initial){
 T70_AW_PROFILE_VIEW V;unsigned Found=0;uint16_t Raw=0;
 if(!T70AwProfileView(Acf,Bytes,0x11e01000,Channel?0x35:0x34,(uint16_t)Channel,Profile,&V))return 0;
 for(uint32_t I=0;I<V.RecordCount;I++){
  const uint8_t *P=V.Section.Data+64+(size_t)I*4;
  if(U16(P)==T70_AW88261_VOLUME_REGISTER){Raw=U16(P+2);if(++Found>1)return 0;}
 }
 if(Found!=1)return 0;
 uint16_t N=T70Aw88261DecodeInitialVolume(Raw);*Initial=N>720?720:N;return 1;
}
static int Inputs(T70_AW_SPB_RESOURCES *O,const void *A,size_t N,const void *Out,size_t Size,size_t Align){
 uintptr_t P=(uintptr_t)A,Q=(uintptr_t)Out,R=(uintptr_t)O;
 if(!O||!A||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL||!O->Open||Q%Align||
    N>UINTPTR_MAX-P||Size>UINTPTR_MAX-Q||sizeof(*O)>UINTPTR_MAX-R)return 0;
 return !(P<Q+Size&&Q<P+N)&&!(R<Q+Size&&Q<R+sizeof(*O))&&
        !(P<R+sizeof(*O)&&R<P+N);
}
static int LevelFromRaw(uint16_t Raw,uint16_t Initial,int32_t *Level){
 uint16_t F=Raw&0x3ff,N=T70Aw88261DecodeInitialVolume(Raw);
 if((F&63)>=48||N>720||N<Initial)return 0;
 *Level=-(int32_t)(N-Initial)*8192;return 1;
}
T70_AW_KS_VOLUME_RESULT T70AwKsVolumeSet(T70_AW_SPB_RESOURCES *O,const void *A,size_t N,
 uint16_t Profile,int32_t Channel,int32_t Level,T70_AW_KS_LEVELS *Applied){
 T70_AW_KS_VOLUME_RESULT R={0};R.Status=STATUS_INVALID_PARAMETER;R.FailedChannel=UINT32_MAX;
 if(Channel< -1||Channel>1||!Inputs(O,A,N,Applied,sizeof(*Applied),_Alignof(T70_AW_KS_LEVELS)))return R;
 uint32_t Mask=Channel==-1?3u:1u<<(unsigned)Channel;uint16_t Initial[2]={0};
 for(unsigned I=0;I<2;I++)if(Mask&(1u<<I)){
  if(!Baseline(A,N,Profile,I,&Initial[I])){R.Status=STATUS_DEVICE_DATA_ERROR;R.FailedChannel=I;return R;}
 }
 T70_AW_KS_LEVELS Values={0};Values.ValidMask=Mask;
 for(unsigned I=0;I<2;I++)if(Mask&(1u<<I)){
  uint64_t Att=Level>=0?0:((uint64_t)(-(int64_t)Level)+8191)/8192;
  if(Att>(uint64_t)(720-Initial[I]))Att=720-Initial[I];
  uint16_t Raw=0;R.AttemptedMask|=1u<<I;
  R.Channel[I]=T70AwSpbResourcesApplyVolume(O,A,N,Profile,(uint16_t)I,(uint32_t)Att,&Raw);
  if(R.Channel[I].Apply.Code==T70AwProfileVolumeApplied)R.CompletedMask|=1u<<I;
  if(R.Channel[I].Status!=STATUS_SUCCESS){R.Status=R.Channel[I].Status;R.FailedChannel=I;return R;}
  if(!LevelFromRaw(Raw,Initial[I],&Values.Level[I])){R.Status=STATUS_DEVICE_DATA_ERROR;R.FailedChannel=I;return R;}
 }
 *Applied=Values;R.Status=STATUS_SUCCESS;return R;
}
NTSTATUS T70AwKsVolumeGet(T70_AW_SPB_RESOURCES *O,const void *A,size_t N,
 uint16_t Profile,uint32_t Channel,int32_t *Level){
 if(Channel>1||!Inputs(O,A,N,Level,sizeof(*Level),_Alignof(int32_t)))return STATUS_INVALID_PARAMETER;
 uint16_t Initial,Raw;int32_t Value;
 if(!Baseline(A,N,Profile,Channel,&Initial))return STATUS_DEVICE_DATA_ERROR;
 NTSTATUS S=T70AwSpbResourcesReadVolume(O,(uint16_t)Channel,&Raw);if(S!=STATUS_SUCCESS)return S;
 if(!LevelFromRaw(Raw,Initial,&Value))return STATUS_DEVICE_DATA_ERROR;
 *Level=Value;return STATUS_SUCCESS;
}
