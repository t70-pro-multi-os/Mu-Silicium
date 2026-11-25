// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioInspect.h"
T70_AUDIO_INSPECT_RESULT T70AudioInspect(const T70_AFE_PREPARED *Dl1,
 const void *Acf,size_t Bytes,const T70_AW_PROBE_ENDPOINT *First,
 const T70_AW_PROBE_ENDPOINT *Second,uint16_t Profile,T70_AUDIO_INSPECTION *Out){
 T70_AUDIO_INSPECT_RESULT R={0};T70_AUDIO_INSPECTION S={0};
 if(!Out||!T70AfeDl1PreparedValid(Dl1))return R;
 S.Dl1Values=*Dl1;R.AmplifiersStarted=1;
 R.Amplifiers=T70Aw88261InspectPair(Acf,Bytes,First,Second,Profile,&S.Amplifiers);
 if(R.Amplifiers.Code!=T70AwInspectObserved){R.Code=T70AudioInspectAmplifiersFailed;return R;}
 *Out=S;R.Code=T70AudioInspectCollected;return R;
}
