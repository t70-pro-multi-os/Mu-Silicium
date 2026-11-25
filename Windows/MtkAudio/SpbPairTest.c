// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwSpbPair.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Calls,Irql,FailAt,Kind;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
NTSTATUS T70AwSpbReadRegister(WDFIOTARGET Target,USHORT Register,USHORT *Value){
 C(Target==(void *)(uintptr_t)(Calls/2+1));C(Register==Calls%2);Calls++;
 if(Calls==FailAt&&Kind==1)return STATUS_IO_TIMEOUT;
 if(Calls==FailAt&&Kind==2)return STATUS_DEVICE_DATA_ERROR;
 *Value=Register?0x11:(Calls==FailAt&&Kind==3?0x1234:0x2113);return STATUS_SUCCESS;
}
int main(void){T70_AW_PAIR_SNAPSHOT S,Old;T70_AW_PAIR_RESULT R;
 for(unsigned K=0;K<4;K++)for(unsigned F=1;F<=4;F++){
  memset(&S,0xa5,sizeof S);Old=S;Calls=0;Kind=K;FailAt=F;
  R=T70AwSpbProbePair(0x11e01000,0x34,0x35,(void *)1,(void *)2,&S);
  int Failed=K==1||K==2||(K==3&&(F%2));
  C(Calls==(Failed?F:4));C(R.Code==(Failed?T70AwPairEndpointFailed:T70AwPairRead));
  if(Failed){C(!memcmp(&S,&Old,sizeof S));C(R.FailedChannel==(F-1)/2);
   C(R.Endpoint.Code==(K==3?T70AwProbeWrongId:(F%2?T70AwProbeIdReadFailed:T70AwProbeStatusReadFailed)));
   C(R.Endpoint.TransportStatus==(K==1?STATUS_IO_TIMEOUT:K==2?STATUS_DEVICE_DATA_ERROR:0));}
  else C(S.Channels[0].ChipId==0x2113&&S.Channels[1].ChipId==0x2113);
 }
 for(unsigned Bad=0;Bad<8;Bad++){
  memset(&S,0xa5,sizeof S);Old=S;Calls=0;Irql=Bad==6?1:0;
  R=T70AwSpbProbePair(Bad==0?0:0x11e01000,Bad==1?0x35:0x34,Bad==7?0x34:0x35,
    Bad==2?NULL:(void *)1,Bad==5?NULL:Bad==3?(void *)1:(void *)2,Bad==4?NULL:&S);
  C(R.Code==T70AwPairInvalid&&R.FailedChannel==2);C(Calls==0);C(!memcmp(&S,&Old,sizeof S));
 }
 printf("SpbPair: %u checks; borrowed fake targets\n",Checks);return 0;
}
