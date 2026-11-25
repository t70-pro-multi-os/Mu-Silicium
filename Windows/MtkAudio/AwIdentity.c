// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwIdentity.h"
int T70Aw88261Identify(const T70_AW_REGISTER_IO *Io,uint16_t *ChipId) {
  uint16_t Id;
  if(!Io || !Io->Read || !ChipId)return 0;
  if(!Io->Read(Io->Context,T70_AW88261_ID_REGISTER,&Id) || Id!=T70_AW88261_CHIP_ID)return 0;
  *ChipId=Id;return 1;
}
