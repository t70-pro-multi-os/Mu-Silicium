// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwTxField.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned Checks;
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();}}while(0)
int main(void){
 for(unsigned Old=0;Old<65536;Old++)for(unsigned Enabled=0;Enabled<2;Enabled++) {
  uint16_t Result=0xbeef,Again=0xbeef;
  CHECK(T70Aw88261ComposeTxField((uint16_t)Old,Enabled,&Result));
  CHECK(((Result>>6)&1)==Enabled);
  for(unsigned Bit=0;Bit<16;Bit++)if(Bit!=6)CHECK(((Result>>Bit)&1)==((Old>>Bit)&1));
  CHECK(T70Aw88261ComposeTxField(Result,Enabled,&Again) && Again==Result);
  CHECK(T70Aw88261ComposeTxField(Result,!Enabled,&Again));
  CHECK((Again^Result)==64);
 }
 uint16_t Output=0xbeef;
 for(unsigned E=2;E<256;E++)CHECK(!T70Aw88261ComposeTxField(0,E,&Output) && Output==0xbeef);
 CHECK(!T70Aw88261ComposeTxField(0,UINT_MAX,&Output) && Output==0xbeef);
 CHECK(!T70Aw88261ComposeTxField(0,0,NULL));
 CHECK(!T70Aw88261ComposeTxField(0,1,NULL));
 printf("AW TX field: %u checks passed\n",Checks);return 0;
}
