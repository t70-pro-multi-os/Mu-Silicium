// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Wave24Convert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long Checks;
#define CHECK(X) do { ++Checks; if (!(X)) {fprintf(stderr,"line %d\n",__LINE__);exit(1);} } while(0)
int main(void)
{
  unsigned char S[18],D[18],Old[18];
  uint32_t V, C; size_t N; unsigned I;
  /* Exhaustive signed 24-bit oracle, unaligned storage, two distinct samples
   * per stereo frame. Decode output arithmetically without signed shifts. */
  for (V=0;V<0x1000000;V++) {
    uint32_t W=0xffffff-V;
    int64_t Expected=(int64_t)V-(V&0x800000 ? 0x1000000 : 0);
    int64_t Actual;
    S[1]=0;S[2]=(unsigned char)V;S[3]=(unsigned char)(V>>8);S[4]=(unsigned char)(V>>16);
    S[5]=0;S[6]=(unsigned char)W;S[7]=(unsigned char)(W>>8);S[8]=(unsigned char)(W>>16);
    memset(D,0xa5,sizeof D);
    CHECK(T70Wave24ToS24Le(S+1,8,2,D+1,16));
    Actual=(int64_t)((uint32_t)D[1]|(uint32_t)D[2]<<8|(uint32_t)D[3]<<16);
    if (D[3]&0x80) Actual-=0x1000000;
    CHECK(Actual==Expected && D[4]==(Expected<0?255:0));
    CHECK(D[5]==S[6] && D[6]==S[7] && D[7]==S[8] &&
          D[8]==((W&0x800000)?255:0) && D[0]==0xa5 && D[9]==0xa5 && D[17]==0xa5);
  }
  memset(S,0,sizeof S);memset(D,0xa5,sizeof D);memcpy(Old,D,sizeof D);
  for (C=0;C<5;C++) for (N=0;N<=16;N++) {
    int Valid=(C==1||C==2) && N%(4*C)==0;
    CHECK(T70Wave24ToS24Le(S,N,C,D,16)==Valid);
    if (!Valid) CHECK(!memcmp(D,Old,sizeof D));
    memset(D,0xa5,sizeof D);
  }
  for (I=1;I<256;I++) {
    S[12]=(unsigned char)I;
    CHECK(!T70Wave24ToS24Le(S,16,2,D,18));
    CHECK(!memcmp(D,Old,sizeof D));
  }
  S[12]=0;
  CHECK(!T70Wave24ToS24Le(S,16,2,D,15));
  CHECK(!T70Wave24ToS24Le(NULL,4,1,D,18));
  CHECK(!T70Wave24ToS24Le(S,4,1,NULL,18));
  CHECK(!T70Wave24ToS24Le((void *)(UINTPTR_MAX-2),4,1,D,18));
  CHECK(!T70Wave24ToS24Le(S,4,1,(void *)(UINTPTR_MAX-2),4));
  CHECK(!T70Wave24ToS24Le(S,4,1,D,SIZE_MAX));
  CHECK(!memcmp(D,Old,sizeof D));
  for (I=0;I<8;I++) CHECK(!T70Wave24ToS24Le(S,8,2,S+I,8));
  for (I=0;I<8;I++) CHECK(!T70Wave24ToS24Le(S+I,8,2,S,8));
  CHECK(T70Wave24ToS24Le(NULL,0,1,NULL,0));
  CHECK(!T70Wave24ToS24Le(NULL,0,0,NULL,0));
  printf("Wave24Convert: %lu checks\n",Checks);return 0;
}
