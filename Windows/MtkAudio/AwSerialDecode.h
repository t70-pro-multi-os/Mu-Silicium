// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_SERIAL_DECODE_H
#define T70_AW_SERIAL_DECODE_H
#include <stdint.h>
typedef struct {
 uint16_t Reg06,Reg07,Reg08,Reg13;
 uint16_t InputChannel,ModeCode,DataBits,BckCode,RateCode,SlotModeCode;
 uint16_t TxSlot,RxRightSlot,RxLeftSlot,TxChannel;
} T70_AW_SERIAL_FIELDS;
/* Pure decoding of configuration words, not live hardware or compatibility.
 * InputChannel 1/2/3 means digital left/right/mono, NOT physical speaker location.
 * Mode 0/1/2 means standard/MSB/LSB justified. SlotMode 0 means legacy I2S.
 * Rate/BCK/slot codes remain codes; no timing or transmitter enable inferred.
 * Reg13 is deliberately opaque; all four raw words retained. Other unexamined
 * bits are NOT validated. Reserved or undocumented selected fields reject with Out intact.
 * Valid writable Out required. No pointers retained or I/O. Returns 1/0. */
int T70AwDecodeSerialFields(uint16_t Reg06,uint16_t Reg07,uint16_t Reg08,
                            uint16_t Reg13,T70_AW_SERIAL_FIELDS *Out);
#endif
