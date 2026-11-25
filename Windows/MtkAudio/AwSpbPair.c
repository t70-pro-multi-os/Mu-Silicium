// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbPair.h"
static int32_t Read(void *Context,uint16_t Register,uint16_t *Value){
 return T70AwSpbReadRegister((WDFIOTARGET)Context,Register,Value);
}
T70_AW_PAIR_RESULT T70AwSpbProbePair(uint64_t Base,uint16_t Address0,
 uint16_t Address1,WDFIOTARGET Target0,WDFIOTARGET Target1,
 T70_AW_PAIR_SNAPSHOT *Snapshot){
 T70_AW_PAIR_RESULT Invalid={0};
 Invalid.Code=T70AwPairInvalid;Invalid.FailedChannel=2;
 if(!Target0||!Target1||Target0==Target1||!Snapshot||
    KeGetCurrentIrql()!=PASSIVE_LEVEL)return Invalid;
 T70_AW_PROBE_ENDPOINT First={Base,Address0,0,{Target0,Read}};
 T70_AW_PROBE_ENDPOINT Second={Base,Address1,1,{Target1,Read}};
 return T70Aw88261PairProbe(&First,&Second,Snapshot);
}
