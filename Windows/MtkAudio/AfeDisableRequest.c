// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeDisableRequest.h"
T70_AFE_DISABLE_RESULT T70AfeDl1RequestDisable(const T70_AFE_FORMAT_IO *Io){
 T70_AFE_DISABLE_RESULT R={0};uint32_t V=0;
 if(!Io||!Io->Context||!Io->Read32||!Io->Write32)return R;
 R.ReadAttempted=1;R.TransportStatus=Io->Read32(Io->Context,0x1b08,&V);
 if(R.TransportStatus){R.Code=T70DisableReadFailed;return R;}
 R.AliasValue=V;
 if(V&~UINT32_C(0x10)){R.Code=T70DisableAliasUnresolved;return R;}
 if(V==0x10){R.Code=T70DisableAlreadySetObserved;return R;}
 R.WriteAttempted=1;R.TransportStatus=Io->Write32(Io->Context,0x1b08,0x10);
 R.Code=R.TransportStatus?T70DisableWriteFailed:T70DisableWriteReturned;
 return R;
}
