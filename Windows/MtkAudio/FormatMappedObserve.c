// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "FormatMappedObserve.h"
T70_FORMAT_OBSERVE_RESULT T70AfeObserveMappedFormat(T70_AFE_MAPPING_OWNER *Owner,
 const T70_AFE_PREPARED *Expected,T70_AFE_FORMAT_OBSERVATION *Out)
{
 T70_FORMAT_OBSERVE_RESULT R={0};T70_AFE_FORMAT_OBSERVATION S={0};
 const void *P[4]={Owner,Expected,Out,NULL};
 size_t N[4]={sizeof(*Owner),sizeof(*Expected),sizeof(*Out),0};
 uintptr_t A[4],E[4];unsigned I,J;
 if(!Owner || !Expected || !Out ||
    (uintptr_t)Owner%_Alignof(T70_AFE_MAPPING_OWNER) ||
    (uintptr_t)Expected%_Alignof(T70_AFE_PREPARED) ||
    (uintptr_t)Out%_Alignof(T70_AFE_FORMAT_OBSERVATION))return R;
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
 if(!NT_SUCCESS(R.BeginStatus)){R.Code=T70FormatObserveBeginFailed;return R;}
 R.ReadAttempted=1;R.ReadStatus=T70AfeMappedRead32(&Owner->Mapping,0x4c,&S.Raw);
 R.EndAttempted=1;R.EndStatus=T70AfeMappingEnd(Owner);
 if(!NT_SUCCESS(R.EndStatus)){R.Code=T70FormatObserveEndFailed;return R;}
 R.LeaseEnded=1;
 if(R.ReadStatus){R.Code=T70FormatObserveReadFailed;return R;}
 S.Mono=(S.Raw>>8)&1;S.RateCode=(S.Raw>>24)&15;
 S.HdMode=S.Raw&3;S.AlignmentCode=(S.Raw>>4)&1;
 if(S.Mono==Expected->Format.Mono)S.MatchMask|=T70FormatMatchMono;
 if(S.RateCode==Expected->Format.RateCode)S.MatchMask|=T70FormatMatchRate;
 if(S.HdMode==Expected->Format.HdMode)S.MatchMask|=T70FormatMatchHd;
 if(S.AlignmentCode==Expected->Format.AlignmentCode)S.MatchMask|=T70FormatMatchAlignment;
 *Out=S;R.Code=T70FormatObserved;return R;
}
