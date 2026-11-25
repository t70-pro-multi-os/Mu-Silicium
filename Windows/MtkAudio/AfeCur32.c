// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeCur32.h"
#include <ntddk.h>
int32_t T70AfeDl1ReadCur32(const T70_AFE_PREPARED *P,const T70_AFE_FORMAT_IO *I,T70_AFE_POSITION *Out){
 uint32_t Low,High;T70_AFE_POSITION Position;
 if(!Out||!I||!I->Context||!I->Read32||!T70AfeDl1PreparedValid(P)||
    P->Geometry.EndInclusive>UINT32_MAX)return STATUS_INVALID_PARAMETER;
 int32_t S=I->Read32(I->Context,0x5c,&Low);if(S)return S<0?S:STATUS_DEVICE_NOT_READY;
 S=I->Read32(I->Context,0x58,&High);if(S)return S<0?S:STATUS_DEVICE_NOT_READY;
 if(High||!T70AfeDl1NormalizedPosition(P,Low,&Position))return STATUS_DEVICE_DATA_ERROR;
 *Out=Position;return STATUS_SUCCESS;
}
