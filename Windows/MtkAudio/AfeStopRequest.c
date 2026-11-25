// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeStopRequest.h"
T70_AFE_STOP_RESULT T70AfeDl1RequestStop(const T70_AFE_FORMAT_IO *I){
 T70_AFE_STOP_RESULT R={0};uint32_t V;
 R.Disable=T70AfeDl1RequestDisable(I);R.Status=R.Disable.TransportStatus;
 if(R.Disable.Code!=T70DisableAlreadySetObserved&&R.Disable.Code!=T70DisableWriteReturned)return R;
 R.Stage=1;R.Status=I->Read32(I->Context,0x1b00,&V);if(R.Status)return R;
 R.Stage=2;R.Status=I->Read32(I->Context,0x3a0,&V);if(R.Status)return R;
 if(V&1){R.Stage=3;R.Status=I->Write32(I->Context,0x3a0,V&~1u);if(R.Status)return R;}
 R.Stage=4;R.Status=I->Write32(I->Context,0x3a8,1);if(R.Status)return R;
 R.Completed=1;return R;
}
