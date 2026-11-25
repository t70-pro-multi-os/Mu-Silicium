// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeFormatUpdate.h"
T70_AFE_FIELDS_RESULT T70AfeDl1UpdateFormat(uint32_t Rate,uint32_t SampleFormat,
                                          uint32_t Channels,const T70_AFE_FORMAT_IO *Io) {
  T70_AFE_FIELDS_RESULT R={T70AfeFieldsInvalid,0,4,0,0,0};T70_AFE_FORMAT F;
  if(!Io || !Io->Read32 || !Io->Write32 || !T70AfeDl1Format(Rate,SampleFormat,Channels,&F))return R;
  const uint32_t Masks[]={0x100,0x0f000000,0x3,0x10};
  const uint32_t Values[]={F.Mono<<8,F.RateCode<<24,F.HdMode,F.AlignmentCode<<4};
  for(unsigned I=0;I<4;I++){
    uint32_t Old;R.ReadsAttempted++;
    R.TransportStatus=Io->Read32(Io->Context,0x4c,&Old);
    if(R.TransportStatus){R.Code=T70AfeFieldsReadFailed;R.FailedField=I;return R;}
    uint32_t Next=(Old&~Masks[I])|(Values[I]&Masks[I]);
    R.WritesAttempted++;R.TransportStatus=Io->Write32(Io->Context,0x4c,Next);
    if(R.TransportStatus){R.Code=T70AfeFieldsWriteFailed;R.FailedField=I;return R;}
    R.WritesCompleted++;
  }
  R.Code=T70AfeFieldsUpdated;return R;
}
