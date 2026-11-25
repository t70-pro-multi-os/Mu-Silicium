// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairInspect.h"
#include "AwBoard.h"
T70_AW_INSPECT_RESULT T70Aw88261InspectPair(const void *Acf,size_t Bytes,
 const T70_AW_PROBE_ENDPOINT *First,const T70_AW_PROBE_ENDPOINT *Second,
 uint16_t Profile,T70_AW_PAIR_INSPECTION *Inspection) {
 T70_AW_INSPECT_RESULT R={0};T70_AW_PAIR_INSPECTION S={0};
 if(!First||!Second||!Inspection||First->Channel!=0||Second->Channel!=1||
    !T70AwBoardTupleValid(First->Base,First->Address,0)||
    !T70AwBoardTupleValid(Second->Base,Second->Address,1)||
    !First->Io.Read||!Second->Io.Read||!First->Io.Context||!Second->Io.Context||
    First->Io.Context==Second->Io.Context)return R;
 if(!T70AwPairProfileView(Acf,Bytes,First->Base,First->Address,Second->Address,
                          Profile,&S.Profiles)){R.Code=T70AwInspectProfileFailed;return R;}
 R.ProbeStarted=1;R.Probe=T70Aw88261PairProbe(First,Second,&S.Observations);
 if(R.Probe.Code!=T70AwPairRead){R.Code=T70AwInspectProbeFailed;return R;}
 *Inspection=S;R.Code=T70AwInspectObserved;return R;
}
