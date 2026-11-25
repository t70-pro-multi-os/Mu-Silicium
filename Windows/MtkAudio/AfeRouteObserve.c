// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeRouteObserve.h"
T70_AFE_ROUTE_RESULT T70AfeObserveDl1I2s3(const T70_AFE_ROUTE_READER *Io,
                                        T70_AFE_ROUTE_OBSERVATION *Observation) {
  T70_AFE_ROUTE_RESULT R={T70AfeRouteInvalid,0,0,2};
  T70_AFE_ROUTE_OBSERVATION S={0};
  if(!Io || !Io->Read32 || !Observation)return R;
  R.ReadsAttempted=1;
  R.TransportStatus=Io->Read32(Io->Context,0x20,&S.Conn0);
  if(R.TransportStatus!=0){R.Code=T70AfeRouteReadFailed;R.FailedRead=0;return R;}
  R.ReadsAttempted=2;
  R.TransportStatus=Io->Read32(Io->Context,0x24,&S.Conn1);
  if(R.TransportStatus!=0){R.Code=T70AfeRouteReadFailed;R.FailedRead=1;return R;}
  S.Dl1Channel1Selected=(S.Conn0>>5)&1u;
  S.Dl1Channel2Selected=(S.Conn1>>6)&1u;
  *Observation=S;R.Code=T70AfeRouteObserved;return R;
}
