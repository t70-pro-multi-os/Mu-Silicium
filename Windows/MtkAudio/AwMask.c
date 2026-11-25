// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwMask.h"
int T70AwUpdatePreservedBits(const T70_AW_REGISTER_IO *Io,uint16_t Register,
                            uint16_t PreserveMask,uint16_t Value) {
  uint16_t Old,Updated;
  if(!Io || !Io->Read || !Io->Write || Register>0xff)return 0;
  if(!Io->Read(Io->Context,Register,&Old))return 0;
  Updated=(uint16_t)((Old&PreserveMask)|(Value&(uint16_t)~PreserveMask));
  return Io->Write(Io->Context,Register,Updated)!=0;
}
