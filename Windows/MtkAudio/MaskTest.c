// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwMask.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned Checks,Reads,Writes,FailRead,FailWrite;
static uint16_t Old,Written,Address;
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();}}while(0)
static int Read(void *C,uint16_t R,uint16_t *V){CHECK(C==&Old && R==Address);Reads++;*V=Old;return !FailRead;}
static int Write(void *C,uint16_t R,uint16_t V){CHECK(C==&Old && R==Address);Writes++;Written=V;return !FailWrite;}
int main(void){
 T70_AW_REGISTER_IO Io={&Old,Read,Write};
 const uint16_t Before[]={0,0xffff,0xaaaa,0x1234},After[]={0xffff,0,0x5555,0x9876};
 for(unsigned M=0;M<65536;M++)for(unsigned P=0;P<4;P++){
  Old=Before[P];Address=(uint16_t)(M&255);Reads=Writes=0;
  CHECK(T70AwUpdatePreservedBits(&Io,Address,(uint16_t)M,After[P]));
  CHECK(Reads==1 && Writes==1);
  for(unsigned B=0;B<16;B++) CHECK(((Written>>B)&1)==(((M&(1U<<B))?Before[P]:After[P])>>B&1));
 }
 for(unsigned F=0;F<3;F++){
  FailRead=F==1;FailWrite=F==2;Reads=Writes=0;
  CHECK(T70AwUpdatePreservedBits(&Io,Address,0xffff,Old)==(F==0));
  CHECK(Reads==1 && Writes==(F==1?0U:1U));
 }
 unsigned R=Reads,W=Writes;FailRead=FailWrite=0;
 CHECK(!T70AwUpdatePreservedBits(NULL,0,0,0));
 CHECK(!T70AwUpdatePreservedBits(&Io,256,0,0));
 Io.Read=NULL;CHECK(!T70AwUpdatePreservedBits(&Io,0,0,0));
 Io.Read=Read;Io.Write=NULL;CHECK(!T70AwUpdatePreservedBits(&Io,0,0,0));
 CHECK(Reads==R && Writes==W);
 printf("AW preserve-mask: %u checks passed\n",Checks);return 0;
}
