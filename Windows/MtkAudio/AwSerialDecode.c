// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSerialDecode.h"
int T70AwDecodeSerialFields(uint16_t A,uint16_t B,uint16_t C,uint16_t D,
                            T70_AW_SERIAL_FIELDS *Out)
{
 T70_AW_SERIAL_FIELDS S={0};
 if(!Out)return 0;
 S.Reg06=A;S.Reg07=B;S.Reg08=C;S.Reg13=D;
 S.InputChannel=(A>>10)&3;S.ModeCode=(A>>8)&3;
 S.DataBits=(uint16_t[]){16,20,24,32}[(A>>6)&3];
 S.BckCode=(A>>4)&3;S.RateCode=A&15;S.SlotModeCode=(B>>12)&7;
 S.TxSlot=(B>>8)&15;S.RxRightSlot=(B>>4)&15;S.RxLeftSlot=B&15;
 S.TxChannel=C&1;
 if(!S.InputChannel || S.ModeCode==3 || S.BckCode==3 || S.RateCode>9 ||
    S.SlotModeCode>5 || S.TxSlot>7 || S.RxRightSlot>7 || S.RxLeftSlot>7)return 0;
 *Out=S;return 1;
}
