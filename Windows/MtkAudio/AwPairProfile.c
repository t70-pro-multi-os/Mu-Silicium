// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairProfile.h"
#include "AwBoard.h"
int T70AwPairProfileView(const void *Acf,size_t Bytes,uint64_t Base,
                         uint16_t Address0,uint16_t Address1,uint16_t Profile,
                         T70_AW_PAIR_PROFILE_VIEW *View) {
  T70_AW_PAIR_PROFILE_VIEW V={0};
  const uint16_t Addresses[]={Address0,Address1},Profiles[]={0,10};
  if(!View || (Profile!=0&&Profile!=10) ||
     !T70AwBoardTupleValid(Base,Address0,0) ||
     !T70AwBoardTupleValid(Base,Address1,1))return 0;
  for(unsigned C=0;C<2;C++)for(unsigned P=0;P<2;P++){
    T70_AW_PROFILE_VIEW One;
    if(!T70AwProfileView(Acf,Bytes,Base,Addresses[C],(uint16_t)C,Profiles[P],&One))return 0;
    if(Profiles[P]==Profile)V.Channel[C]=One;
  }
  V.RegisterProfileCount=2;*View=V;return 1;
}
