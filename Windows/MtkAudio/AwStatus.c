// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwStatus.h"
T70_AW_STATUS_RESULT T70Aw88261ReadClockStatus(const T70_AW_STATUS_IO *Io,
                                             T70_AW_CLOCK_SNAPSHOT *Snapshot) {
  T70_AW_STATUS_RESULT Result={T70AwStatusInvalid,0};
  T70_AW_CLOCK_SNAPSHOT Local={0};uint16_t Raw;
  if(!Io || !Io->Read || !Snapshot)return Result;
  Result.TransportStatus=Io->Read(Io->Context,T70_AW88261_STATUS_REGISTER,&Raw);
  if(Result.TransportStatus!=0){Result.Code=T70AwStatusReadFailed;return Result;}
  Local.Raw=Raw;Local.UninterpretedBits=(uint16_t)(Raw&0xffee);
  Local.PllLocked=(uint8_t)((Raw>>0)&1);Local.ClockStable=(uint8_t)((Raw>>4)&1);
  Local.ClockPairSet=(uint8_t)(Local.PllLocked && Local.ClockStable);
  *Snapshot=Local;Result.Code=T70AwStatusRead;return Result;
}
