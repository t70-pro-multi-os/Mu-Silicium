// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairProbe.h"
#include "AwBoard.h"
T70_AW_PAIR_RESULT T70Aw88261PairProbe(const T70_AW_PROBE_ENDPOINT *First,
                                     const T70_AW_PROBE_ENDPOINT *Second,
                                     T70_AW_PAIR_SNAPSHOT *Snapshot) {
  T70_AW_PAIR_RESULT R={T70AwPairInvalid,2,{T70AwProbeInvalid,0}};
  T70_AW_PAIR_SNAPSHOT S={0};
  if(!First || !Second || !Snapshot || First->Channel!=0 || Second->Channel!=1 ||
     !T70AwBoardTupleValid(First->Base,First->Address,First->Channel) ||
     !T70AwBoardTupleValid(Second->Base,Second->Address,Second->Channel) ||
     !First->Io.Read || !Second->Io.Read || !First->Io.Context || !Second->Io.Context ||
     First->Io.Context==Second->Io.Context)return R;
  const T70_AW_PROBE_ENDPOINT *Endpoints[2]={First,Second};
  for(unsigned I=0;I<2;I++) {
    const T70_AW_PROBE_ENDPOINT *E=Endpoints[I];
    R.Endpoint=T70Aw88261Probe(E->Base,E->Address,E->Channel,&E->Io,&S.Channels[I]);
    if(R.Endpoint.Code!=T70AwProbeRead){R.Code=T70AwPairEndpointFailed;R.FailedChannel=I;return R;}
  }
  *Snapshot=S;R.Code=T70AwPairRead;return R;
}
