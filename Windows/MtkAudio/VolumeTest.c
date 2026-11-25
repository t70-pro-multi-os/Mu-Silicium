// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwVolume.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned Checks;
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();}}while(0)
int main(void){
 for(unsigned R=0;R<65536;R++) {
  unsigned F=R%1024;CHECK(T70Aw88261DecodeInitialVolume((uint16_t)R)==F-16*(F/64));
  uint16_t Out=0xbeef;
  CHECK(T70Aw88261EncodeAttenuation((uint16_t)R,&Out)==(R<=720));
  if(R<=720){CHECK(Out==R+16*(R/48));CHECK(T70Aw88261DecodeInitialVolume(Out)==R);}
  else CHECK(Out==0xbeef);
 }
 const uint16_t Initial[]={0,47,48,49,719,720,721,783,65535};
 const uint32_t Requested[]={0,1,47,48,49,719,720,721,UINT32_MAX};
 for(unsigned Old=0;Old<65536;Old++)for(unsigned P=0;P<9;P++) {
  uint64_t Sum=(uint64_t)Initial[P]+Requested[P];unsigned A=(unsigned)(Sum>720?720:Sum);
  uint16_t Result=T70Aw88261ComposeVolume((uint16_t)Old,Initial[P],Requested[P]);
  CHECK((Result&0xfc00)==(Old&0xfc00));CHECK((Result&0x03ff)==A+16*(A/48));
 }
 for(unsigned I=0;I<=783;I++)for(unsigned V=0;V<=721;V++) {
  unsigned A=I+V;if(A>720)A=720;
  CHECK(T70Aw88261ComposeVolume(0xa800,(uint16_t)I,V)==(0xa800|(A+16*(A/48))));
 }
 CHECK(!T70Aw88261EncodeAttenuation(0,NULL));
 uint16_t Field=0;CHECK(T70Aw88261EncodeAttenuation(720,&Field)&&Field==0x3c0);
 printf("AW volume arithmetic: %u checks passed\n",Checks);return 0;
}
