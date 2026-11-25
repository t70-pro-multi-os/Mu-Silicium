// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwIdentity.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned Checks,Reads,Fail;static uint16_t Response;
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();}}while(0)
static int Read(void *C,uint16_t Register,uint16_t *Value) {
 CHECK(C==&Response && Register==0x00); Reads++;
 *Value=Response;return !Fail;
}
int main(void){
 T70_AW_REGISTER_IO Io={&Response,Read,NULL};
 for(unsigned V=0;V<65536;V++)for(unsigned F=0;F<2;F++) {
  Response=(uint16_t)V;Fail=F;Reads=0;uint16_t Id=0xbeef;
  int Expected=!F && V==0x2113;
  CHECK(T70Aw88261Identify(&Io,&Id)==Expected && Reads==1);
  CHECK(Id==(Expected?T70_AW88261_CHIP_ID:0xbeef));
 }
 unsigned Before=Reads;uint16_t Id=0xbeef;
 CHECK(!T70Aw88261Identify(NULL,&Id));CHECK(!T70Aw88261Identify(&Io,NULL));
 Io.Read=NULL;CHECK(!T70Aw88261Identify(&Io,&Id));CHECK(Reads==Before && Id==0xbeef);
 printf("AW identity: %u checks passed\n",Checks);return 0;
}
