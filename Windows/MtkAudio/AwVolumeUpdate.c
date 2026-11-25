// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwVolumeUpdate.h"
T70_AW_VOLUME_UPDATE_RESULT T70Aw88261UpdateVolume(const T70_AW_VOLUME_IO *Io,
  uint16_t Initial,uint32_t Requested,uint16_t *AppliedRegister) {
  T70_AW_VOLUME_UPDATE_RESULT Result={T70AwVolumeInvalid,0};
  uint16_t Existing,Updated;
  if(!Io || !Io->Read || !Io->Write || !AppliedRegister || Initial>783 ||
     Requested>T70_AW88261_ATTENUATION_LIMIT)return Result;
  Result.TransportStatus=Io->Read(Io->Context,T70_AW88261_VOLUME_REGISTER,&Existing);
  if(Result.TransportStatus!=0){Result.Code=T70AwVolumeReadFailed;return Result;}
  Updated=T70Aw88261ComposeVolume(Existing,Initial,Requested);
  Result.TransportStatus=Io->Write(Io->Context,T70_AW88261_VOLUME_REGISTER,Updated);
  if(Result.TransportStatus!=0){Result.Code=T70AwVolumeWriteFailed;return Result;}
  *AppliedRegister=Updated;Result.Code=T70AwVolumeUpdated;return Result;
}
