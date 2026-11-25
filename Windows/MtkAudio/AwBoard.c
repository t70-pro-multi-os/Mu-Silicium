// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwBoard.h"
int T70AwBoardRegister(const void *Acf,size_t Bytes,uint64_t ControllerBase,
                      uint16_t Address,uint16_t SoundChannel,uint16_t Profile,
                      uint32_t Index,T70_AW_REGISTER *Register) {
  if(!T70AwBoardTupleValid(ControllerBase,Address,SoundChannel))return 0;
  return T70AwAcfRegister(Acf,Bytes,SoundChannel,Profile,Index,Register);
}
