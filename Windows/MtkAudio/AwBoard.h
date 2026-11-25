// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_BOARD_H
#define T70_AW_BOARD_H
#include "AwRegisters.h"
/* Identity must come from a trusted board resource provider, not ACF content.
 * Matches only the two enabled DT amplifiers. Does not discover or power them. */
static inline int T70AwBoardTupleValid(uint64_t Base,uint16_t Address,uint16_t Channel) {
  return Base==UINT64_C(0x11e01000) && Channel<=1 &&
         Address==(uint16_t)(0x34+Channel);
}
int T70AwBoardRegister(const void *Acf,size_t Bytes,uint64_t ControllerBase,
                      uint16_t Address,uint16_t SoundChannel,uint16_t Profile,
                      uint32_t Index,T70_AW_REGISTER *Register);
#endif
