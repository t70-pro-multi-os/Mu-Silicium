// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwStatus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Reads;static uint16_t Raw;static int32_t Error;
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();}}while(0)
static int32_t Read(void *C,uint16_t R,uint16_t *V){CHECK(C==&Raw && R==1);Reads++;*V=Raw;return Error;}
int main(void){
 T70_AW_STATUS_IO Io={&Raw,Read};T70_AW_CLOCK_SNAPSHOT Out,Before;
 for(unsigned V=0;V<65536;V++){
  Raw=(uint16_t)V;Reads=0;memset(&Out,0xa5,sizeof(Out));
  T70_AW_STATUS_RESULT R=T70Aw88261ReadClockStatus(&Io,&Out);
  CHECK(R.Code==T70AwStatusRead && R.TransportStatus==0 && Reads==1);
  CHECK(Out.Raw==V && Out.PllLocked==((V&1)!=0) && Out.ClockStable==((V&16)!=0));
  CHECK(Out.ClockPairSet==((V&17)==17));
  CHECK((Out.UninterpretedBits|((unsigned)Out.ClockStable<<4)|Out.PllLocked)==V);
  CHECK((Out.UninterpretedBits&17)==0);
 }
 const int32_t Errors[]={-1,-123,19,INT32_MIN,INT32_MAX};
 for(unsigned E=0;E<5;E++){
  Error=Errors[E];Raw=0xffff;Reads=0;memset(&Out,0xa5,sizeof(Out));memcpy(&Before,&Out,sizeof(Out));
  T70_AW_STATUS_RESULT R=T70Aw88261ReadClockStatus(&Io,&Out);
  CHECK(R.Code==T70AwStatusReadFailed && R.TransportStatus==Error && Reads==1);
  CHECK(memcmp(&Before,&Out,sizeof(Out))==0);
 }
 Reads=0;
 CHECK(T70Aw88261ReadClockStatus(NULL,&Out).Code==T70AwStatusInvalid);
 CHECK(T70Aw88261ReadClockStatus(&Io,NULL).Code==T70AwStatusInvalid);
 Io.Read=NULL;CHECK(T70Aw88261ReadClockStatus(&Io,&Out).Code==T70AwStatusInvalid);
 CHECK(Reads==0 && memcmp(&Before,&Out,sizeof(Out))==0);
 printf("AW status snapshot: %u checks passed\n",Checks);return 0;
}
