// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CommonBufferConfigure.h"
T70_BUFFER_CONFIG_RESULT T70AfeConfigureCommonBufferMapped(
 T70_AFE_COMMON_BUFFER *B,T70_AFE_MAPPING_OWNER *M){
 T70_BUFFER_CONFIG_RESULT R={0};uintptr_t X=(uintptr_t)B,Y=(uintptr_t)M;
 if(!B||!M||sizeof(*B)>UINTPTR_MAX-X||sizeof(*M)>UINTPTR_MAX-Y||
    (X<Y+sizeof(*M)&&Y<X+sizeof(*B)))return R;
 if(!B->Adapter||!B->Free||!B->VirtualAddress||!B->Length||B->ActiveUses||
    !T70AfeDl1PreparedValid(&B->Prepared)||
    B->Prepared.DmaBase!=(uint64_t)B->LogicalAddress.QuadPart||
    B->Prepared.BufferBytes!=B->Length)return R;
 R.BufferBeginAttempted=1;R.BufferBeginStatus=T70AfeCommonBufferBegin(B);
 if(!NT_SUCCESS(R.BufferBeginStatus)){R.Code=T70BufferConfigBeginFailed;return R;}
 R.BufferLeaseHeld=1;R.MappedCalled=1;
 R.Mapped=T70AfeConfigureMapped(M,&B->Prepared);
 if(R.Mapped.ConfigureStarted){R.Code=T70BufferConfigLeaseRetained;return R;}
 R.Code=T70BufferConfigNotStarted;R.BufferEndAttempted=1;
 R.BufferEndStatus=T70AfeCommonBufferEnd(B);
 if(NT_SUCCESS(R.BufferEndStatus))R.BufferLeaseHeld=0;
 return R;
}
