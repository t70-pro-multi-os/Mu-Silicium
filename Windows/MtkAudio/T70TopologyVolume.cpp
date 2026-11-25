// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "T70TopologyVolume.h"
extern "C" {
#include "AwVolume.h"
}
#include <string.h>
namespace {
const GUID Audio={0x45ffaaa0,0x6e1b,0x11d0,{0xbc,0xf2,0x44,0x45,0x53,0x54,0,0}};
const GUID VolumeType={0x3a5acc00,0xc557,0x11d0,{0x8a,0x2b,0,0xa0,0xc9,0x25,0x5a,0xc1}};
const GUID General={0x97e99ba0,0xbdea,0x11cf,{0xa5,0xd6,0x28,0xdb,4,0xc1,0,0}};
constexpr ULONG Get=1,Set=2,Basic=0x200,Flags=Get|Set|Basic;
const PCPROPERTY_ITEM Property={&Audio,4,Flags,T70TopologyMiniport::VolumeProperty};
const PCAUTOMATION_TABLE Automation={sizeof Property,1,&Property,0,0,nullptr,0,0,nullptr,0};
// Fixed KS wire layout: DESCRIPTION, MEMBERSHEADER, two STEPPING_LONG entries.
struct Support {
 ULONG Access,Size;GUID Type;ULONG TypeId,TypeFlags,Lists,Reserved;
 ULONG MembersFlags,MemberSize,MemberCount,MemberFlags;
 struct {ULONG Step,Reserved;LONG Minimum,Maximum;} Range[2];
};
static_assert(sizeof(Support)==88,"KS support wire layout");
#ifdef _NTDDK_
static_assert(sizeof(KSPROPERTY_DESCRIPTION)==40,"KS description layout");
static_assert(sizeof(KSPROPERTY_MEMBERSHEADER)==16,"KS members layout");
static_assert(sizeof(KSPROPERTY_STEPPING_LONG)==16,"KS stepping layout");
static_assert(KSPROPERTY_TYPE_TOPOLOGY==0x10000000,"KS topology modifier");
static_assert(KSPROPERTY_AUDIO_VOLUMELEVEL==4 && KSPROPERTY_TYPE_BASICSUPPORT==Basic,"KS identifiers");
static_assert(KSPROPERTY_MEMBER_STEPPEDRANGES==2 && KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL==2,"KS range flags");
#endif
static uint16_t U16(const uint8_t *P){return uint16_t(P[0]|uint16_t(P[1])<<8);}
static bool Overlap(const void *A,size_t N,const void *B,size_t M){
 uintptr_t X=(uintptr_t)A,Y=(uintptr_t)B;
 return N>UINTPTR_MAX-X||M>UINTPTR_MAX-Y||(N&&M&&X<Y+M&&Y<X+N);
}
}
NTSTATUS T70TopologyMiniport::CreateWithVolume(PUNKNOWN P,const T70_TOPOLOGY_VOLUME_BINDING *B,T70TopologyMiniport **Out){
 if(!P||!B||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL||!B->Owner||!B->Owner->Open||!B->Acf||
    !B->Owner->Target[0].Target||!B->Owner->Target[1].Target||
    B->Owner->Target[0].Target==B->Owner->Target[1].Target||
    B->Owner->Target[0].ActiveUses||B->Owner->Target[1].ActiveUses||
    B->Map[0]>1||B->Map[1]>1||B->Map[0]==B->Map[1]||
    Overlap(B->Acf,B->Bytes,B->Owner,sizeof(*B->Owner))||
    Overlap(Out,sizeof(*Out),B,sizeof(*B))||Overlap(Out,sizeof(*Out),B->Acf,B->Bytes)||
    Overlap(Out,sizeof(*Out),B->Owner,sizeof(*B->Owner)))return STATUS_INVALID_PARAMETER;
 LONG Minimum[2];
 for(unsigned C=0;C<2;C++){
  unsigned I=B->Map[C];T70_AW_PROFILE_VIEW V{};
  if(!T70AwProfileView(B->Acf,B->Bytes,0x11e01000,I?0x35:0x34,(uint16_t)I,B->Profile,&V))return STATUS_DEVICE_DATA_ERROR;
  unsigned Found=0;uint16_t Raw=0;
  for(uint32_t J=0;J<V.RecordCount;J++){const uint8_t *R=V.Section.Data+64+size_t(J)*4;if(U16(R)==5){Raw=U16(R+2);++Found;}}
  if(Found!=1)return STATUS_DEVICE_DATA_ERROR;
  unsigned Initial=T70Aw88261DecodeInitialVolume(Raw);if(Initial>720)Initial=720;
  Minimum[C]=-LONG(720-Initial)*8192;
 }
 T70TopologyMiniport *M=nullptr;NTSTATUS S=Create(P,&M);if(S)return S;
 M->Volume.Owner=B->Owner;M->Volume.Acf=B->Acf;M->Volume.Bytes=B->Bytes;M->Volume.Profile=B->Profile;
 for(unsigned C=0;C<2;C++){M->Volume.Map[C]=B->Map[C];M->Volume.Minimum[C]=Minimum[C];}
 M->VolumeNode={0,&Automation,&VolumeType,nullptr};
 M->VolumeConnections[0]={PCFILTER_NODE,0,0,1};M->VolumeConnections[1]={0,0,PCFILTER_NODE,1};
 M->Filter.NodeCount=1;M->Filter.Nodes=&M->VolumeNode;
 M->Filter.ConnectionCount=2;M->Filter.Connections=M->VolumeConnections;
 *Out=M;return STATUS_SUCCESS;
}
NTSTATUS T70TopologyMiniport::VolumeProperty(PPCPROPERTY_REQUEST R){
 if(!R||!R->MajorTarget||R->MinorTarget||R->Node!=0||!R->PropertyItem||
    !R->PropertyItem->Set||memcmp(R->PropertyItem->Set,&Audio,sizeof Audio)||R->PropertyItem->Id!=4||
    KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 const ULONG Verb=R->Verb&~ULONG(0x10000000); // KSPROPERTY_TYPE_TOPOLOGY routing modifier.
 if(Verb!=Get&&Verb!=Set&&Verb!=Basic)return STATUS_INVALID_PARAMETER;
 // PortCls MajorTarget is this registered miniport, not an arbitrary object.
 auto M=static_cast<T70TopologyMiniport *>(R->MajorTarget);
 Call Guard(&M->Calls,M->Parent);if(!Guard.Entered)return STATUS_DEVICE_NOT_READY;
 if(M->Closed||!M->Initialized||!M->Volume.Owner)return STATUS_DEVICE_NOT_READY;
 // PortCls GET buffer-size query performs no register access.
 if(Verb==Get&&!R->ValueSize){R->ValueSize=sizeof(LONG);return STATUS_BUFFER_OVERFLOW;}
 if(Verb==Basic&&R->ValueSize!=4&&R->ValueSize!=40&&R->ValueSize<sizeof(Support))return STATUS_BUFFER_TOO_SMALL;
 if(!R->Value||Overlap(R->Value,R->ValueSize,M,sizeof(*M))||
    Overlap(R->Value,R->ValueSize,R,sizeof(*R))||
    Overlap(R->Value,R->ValueSize,M->Volume.Acf,M->Volume.Bytes)||
    Overlap(R->Value,R->ValueSize,M->Volume.Owner,sizeof(T70_AW_SPB_RESOURCES)))return STATUS_INVALID_PARAMETER;
 if(Verb==Basic){
  ULONG N=R->ValueSize;
  Support S{};S.Access=Flags;S.Size=sizeof S;S.Type=General;S.TypeId=3;S.Lists=1;
  S.MembersFlags=2;S.MemberSize=16;S.MemberCount=2;S.MemberFlags=2;
  for(unsigned I=0;I<2;I++){S.Range[I].Step=8192;S.Range[I].Minimum=M->Volume.Minimum[I];}
  ULONG Written=N==4?4:N==40?40:sizeof S;memcpy(R->Value,&S,Written);R->ValueSize=Written;return STATUS_SUCCESS;
 }
 if(Verb!=Get&&Verb!=Set)return STATUS_INVALID_PARAMETER;
 if(!R->Instance||R->InstanceSize<8||
    R->InstanceSize>UINTPTR_MAX-(uintptr_t)R->Instance||
    Overlap(R->Instance,R->InstanceSize,R->Value,R->ValueSize))return STATUS_INVALID_PARAMETER;
 if(R->ValueSize<sizeof(LONG))return STATUS_BUFFER_TOO_SMALL;
 LONG C;memcpy(&C,R->Instance,sizeof C);
 if(C< -1||C>1)return STATUS_INVALID_PARAMETER;
 if(Verb==Get&&C==-1)return STATUS_NOT_SUPPORTED; // No defined aggregate for unequal levels.
 auto O=static_cast<T70_AW_SPB_RESOURCES *>(M->Volume.Owner);
 if(Verb==Get){
  int32_t L;NTSTATUS S=T70AwKsVolumeGet(O,M->Volume.Acf,M->Volume.Bytes,M->Volume.Profile,M->Volume.Map[C],&L);
  if(S)return S;memcpy(R->Value,&L,sizeof L);
 }else{
  int32_t L;memcpy(&L,R->Value,sizeof L);T70_AW_KS_LEVELS Applied{};
  auto Result=T70AwKsVolumeSet(O,M->Volume.Acf,M->Volume.Bytes,M->Volume.Profile,C==-1?-1:M->Volume.Map[C],L,&Applied);
  if(Result.Status)return Result.Status; // Pair failure can follow one completed write; no rollback/retry.
 }
 R->ValueSize=sizeof(LONG);return STATUS_SUCCESS;
}
