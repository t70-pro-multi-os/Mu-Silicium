// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfePrefetch.h"
int32_t T70AfeDl1PreparePrefetch(const T70_AFE_PREPARED *P,const T70_AFE_FORMAT_IO *I){
 if(!T70AfeDl1PreparedValid(P)||!I||!I->Context||!I->Read32||!I->Write32)return (int32_t)0xc000000d;
 uint32_t Code=((uint64_t)P->Geometry.PeriodFrames*1000/P->Rate)>10?3:0;
 const uint32_t Masks[]={0x3000,0xf00000},Shifts[]={12,20};
 for(unsigned N=0;N<2;N++){
  uint32_t Old;int32_t S=I->Read32(I->Context,0x4c,&Old);if(S)return S;
  uint32_t New=(Old&~Masks[N])|(Code<<Shifts[N]);
  if(New!=Old){S=I->Write32(I->Context,0x4c,New);if(S)return S;}
 }
 return 0;
}
