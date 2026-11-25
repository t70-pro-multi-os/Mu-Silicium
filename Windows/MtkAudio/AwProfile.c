// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwProfile.h"
#include "AwBoard.h"
int T70AwProfileView(const void *Acf,size_t Bytes,uint64_t ControllerBase,
                     uint16_t Address,uint16_t SoundChannel,uint16_t Profile,
                     T70_AW_PROFILE_VIEW *View) {
  T70_AW_REGISTER First;
  T70_AW_PROFILE_VIEW V={0};
  if(!View || !T70AwBoardRegister(Acf,Bytes,ControllerBase,Address,SoundChannel,
                                 Profile,0,&First) ||
     !T70AwAcfFind(Acf,Bytes,SoundChannel,Profile,4,&V.Section) ||
     !T70AwRegisterBinValidate(V.Section.Data,V.Section.Length,&V.RecordCount) ||
     !T70AwPreflightCount(V.Section.Data,V.Section.Length,&V.SpecialCount))return 0;
  *View=V;return 1;
}
