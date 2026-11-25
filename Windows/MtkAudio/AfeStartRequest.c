// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeStartRequest.h"
T70_AFE_START_RESULT T70AfeDl1RequestStart(const T70_AFE_PREPARED *P,const T70_AFE_FORMAT_IO *I){
 T70_AFE_START_RESULT R={0};uint32_t Count,V;
 if(!I||!I->Context||!I->Read32||!I->Write32||!T70AfeDl1IrqCount(P,&Count))return R;
 R.Valid=1;
 const uint32_t Reg[]={0x1b04,0x300,0x2e4,0x3a0};
 const uint32_t Mask[]={0x10,0x3ffff,15,1};
 const uint32_t Val[]={0x10,Count,P->Format.RateCode,1};
 for(unsigned N=0;N<4;N++){
  R.Stage=N;R.Reads++;R.Status=I->Read32(I->Context,Reg[N],&V);if(R.Status)return R;
  if(N==0&&(V&~UINT32_C(0x10)))return R;
  uint32_t Next=(V&~Mask[N])|Val[N];
  if(Next!=V){R.Writes++;R.Status=I->Write32(I->Context,Reg[N],Next);if(R.Status)return R;}
  if(N==0){R.Reads++;R.Status=I->Read32(I->Context,0x1b00,&V);if(R.Status)return R;}
 }
 R.Completed=1;return R;
}
