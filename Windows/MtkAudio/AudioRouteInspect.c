// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioRouteInspect.h"
T70_AUDIO_ROUTE_RESULT T70AudioInspectWithRoute(const T70_AFE_PREPARED *Dl1,
 const void *Acf,size_t Bytes,const T70_AW_PROBE_ENDPOINT *First,
 const T70_AW_PROBE_ENDPOINT *Second,uint16_t Profile,
 const T70_AFE_ROUTE_READER *Route,T70_AUDIO_ROUTE_INSPECTION *Out) {
 T70_AUDIO_ROUTE_RESULT R={0};T70_AUDIO_ROUTE_INSPECTION S={0};
 if(!Out || !Route || !Route->Read32 || !Route->Context || !First || !Second ||
    !First->Io.Context || !Second->Io.Context ||
    Route->Context==First->Io.Context || Route->Context==Second->Io.Context)
   return R;
 R.AudioStarted=1;
 R.Audio=T70AudioInspect(Dl1,Acf,Bytes,First,Second,Profile,&S.Audio);
 if(R.Audio.Code!=T70AudioInspectCollected){R.Code=T70AudioRouteAudioFailed;return R;}
 R.RouteStarted=1;R.Route=T70AfeObserveDl1I2s3(Route,&S.Route);
 if(R.Route.Code!=T70AfeRouteObserved){R.Code=T70AudioRouteReadFailed;return R;}
 *Out=S;R.Code=T70AudioRouteCollected;return R;
}
