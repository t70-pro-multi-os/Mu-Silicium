// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwAcf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
  assert(argc==2);FILE *F=fopen(argv[1],"rb");assert(F);
  uint8_t B[3857];size_t N=fread(B,1,sizeof B,F);assert(!ferror(F));fclose(F);assert(N==3856);
  uint32_t Count=99;assert(T70AwAcfValidate(B,N,&Count)&&Count==12);
  unsigned Checks=1;
  for(unsigned Dev=0;Dev<4;Dev++) {
    T70_AW_SECTION S;
    assert(T70AwAcfFind(B,N,(uint16_t)Dev,0,10,&S)&&S.Length==168);
    assert(T70AwAcfFind(B,N,(uint16_t)Dev,0,4,&S)&&S.Length==292);
    assert(T70AwAcfFind(B,N,(uint16_t)Dev,10,4,&S)&&S.Length==292);Checks+=3;
  }
  T70_AW_SECTION S={99,99,99,NULL,99},Old=S;
  assert(!T70AwAcfFind(B,N,4,0,4,&S)&&!memcmp(&S,&Old,sizeof S));Checks++;
  for(size_t I=0;I<N;I++) {Count=99;assert(!T70AwAcfValidate(B,I,&Count)&&Count==99);Checks++;}
  for(size_t I=848;I<N;I++) {
    B[I]^=1;Count=99;assert(!T70AwAcfValidate(B,N,&Count)&&Count==99);B[I]^=1;Checks++;
  }
  const size_t Fields[]={0,4,52,56,60,64,80,100,104,106,108,112,116,120};
  for(size_t I=0;I<sizeof Fields/sizeof Fields[0];I++) {
    uint8_t Save[4];memcpy(Save,B+Fields[I],4);memset(B+Fields[I],0xff,4);
    Count=99;if(T70AwAcfValidate(B,N,&Count)||Count!=99){fprintf(stderr,"accepted mutation at %zu\n",Fields[I]);return 1;}memcpy(B+Fields[I],Save,4);Checks++;
  }
  // Duplicate keys must never select an arbitrary amplifier section.
  uint8_t SavedProfile=B[234];B[234]=0;
  assert(!T70AwAcfValidate(B,N,&Count));B[234]=SavedProfile;
  uint8_t Unaligned[3857];memcpy(Unaligned+1,B,N);
  assert(T70AwAcfValidate(Unaligned+1,N,&Count)&&Count==12);
  B[N]=0;assert(!T70AwAcfValidate(B,N+1,&Count));
  assert(!T70AwAcfValidate(NULL,N,&Count)&&!T70AwAcfValidate(B,N,NULL));
  printf("AW88261 stock ACF: %u checks + null/trailing-data checks passed; no device I/O\n",Checks);
  return 0;
}
