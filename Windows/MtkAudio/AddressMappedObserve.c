// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AddressMappedObserve.h"
T70_ADDRESS_OBSERVE_RESULT T70AfeObserveMappedAddress(T70_AFE_MAPPING_OWNER *Owner,
 const T70_AFE_PREPARED *Expected,T70_AFE_ADDRESS_OBSERVATION *Out)
{
 T70_ADDRESS_OBSERVE_RESULT R={0};T70_AFE_ADDRESS_OBSERVATION S={0};
 const void *P[4]={Owner,Expected,Out,NULL};
 size_t N[4]={sizeof(*Owner),sizeof(*Expected),sizeof(*Out),0};
 uintptr_t A[4],E[4];unsigned I,J;
 if(!Owner || !Expected || !Out ||
    (uintptr_t)Owner%_Alignof(T70_AFE_MAPPING_OWNER) ||
    (uintptr_t)Expected%_Alignof(T70_AFE_PREPARED) ||
    (uintptr_t)Out%_Alignof(T70_AFE_ADDRESS_OBSERVATION))return R;
 /* Owner must be valid resident memory before its mapping can be inspected. */
 if((uintptr_t)Owner>UINTPTR_MAX-sizeof(*Owner))return R;
 P[3]=(const void *)Owner->Mapping.VirtualBase;N[3]=Owner->Mapping.Length;
 for(I=0;I<4;I++){
   A[I]=(uintptr_t)P[I];if(N[I]>UINTPTR_MAX-A[I])return R;E[I]=A[I]+N[I];
   for(J=0;J<I;J++)
     if(N[I] && N[J] && A[I]<E[J] && A[J]<E[I])return R;
 }
 if(!T70AfeDl1PreparedValid(Expected))return R;
 R.BeginAttempted=1;R.BeginStatus=T70AfeMappingBegin(Owner);
 if(!NT_SUCCESS(R.BeginStatus)){R.Code=T70AddressObserveBeginFailed;return R;}
 const uint32_t Offsets[]={0x54,0x64,0x50,0x60};
 uint32_t *Fields[]={&S.BaseLow,&S.EndLow,&S.BaseHigh,&S.EndHigh};
 for(I=0;I<4;I++){
   R.ReadsAttempted++;
   R.ReadStatus=T70AfeMappedRead32(&Owner->Mapping,Offsets[I],Fields[I]);
   if(R.ReadStatus){R.FailedOffset=Offsets[I];break;}
   R.ReadsCompleted++;
 }
 R.EndAttempted=1;R.EndStatus=T70AfeMappingEnd(Owner);
 if(!NT_SUCCESS(R.EndStatus)){R.Code=T70AddressObserveEndFailed;return R;}
 R.LeaseEnded=1;
 if(R.ReadStatus){R.Code=T70AddressObserveReadFailed;return R;}
 if(S.BaseLow==Expected->Geometry.BaseLow)S.MatchMask|=T70AddressMatchBaseLow;
 if(S.EndLow==Expected->Geometry.EndLow)S.MatchMask|=T70AddressMatchEndLow;
 if(S.BaseHigh==Expected->Geometry.BaseHigh)S.MatchMask|=T70AddressMatchBaseHigh;
 if(S.EndHigh==Expected->Geometry.EndHigh)S.MatchMask|=T70AddressMatchEndHigh;
 *Out=S;R.Code=T70AddressObserved;return R;
}
