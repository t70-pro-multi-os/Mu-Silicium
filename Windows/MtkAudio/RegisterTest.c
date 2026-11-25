// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwRegisters.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void FixSum(uint8_t *B,size_t N) {
  uint32_t Sum=0;for(size_t I=4;I<N;I++)Sum+=B[I];
  for(unsigned I=0;I<4;I++)B[I]=(uint8_t)(Sum>>(8*I));
}
int main(int argc,char **argv) {
  assert(argc==2);FILE *F=fopen(argv[1],"rb");assert(F);
  uint8_t B[3857];size_t N=fread(B,1,sizeof B,F);assert(!ferror(F));fclose(F);assert(N==3856);
  unsigned Checks=0;
  for(uint16_t D=0;D<4;D++)for(uint16_t P=0;P<=10;P+=10) {
    T70_AW_SECTION S;uint32_t Count=99;
    assert(T70AwAcfFind(B,N,D,P,4,&S));
    assert(T70AwRegisterBinValidate(S.Data,S.Length,&Count)&&Count==57);
    for(uint32_t I=0;I<Count;I++) {
      T70_AW_REGISTER R;assert(T70AwAcfRegister(B,N,D,P,I,&R));
      const uint8_t *Q=S.Data+64+4*I;
      assert(R.Address==(uint16_t)(Q[0]|(Q[1]<<8))&&R.Value==(uint16_t)(Q[2]|(Q[3]<<8)));Checks++;
    }
    T70_AW_REGISTER R={999,999};assert(!T70AwAcfRegister(B,N,D,P,57,&R)&&R.Address==999&&R.Value==999);
    for(size_t I=0;I<S.Length;I++) {
      Count=99;assert(!T70AwRegisterBinValidate(S.Data,I,&Count)&&Count==99);Checks++;
      uint8_t Copy[292];memcpy(Copy,S.Data,sizeof Copy);Copy[I]^=1;
      Count=99;assert(!T70AwRegisterBinValidate(Copy,sizeof Copy,&Count)&&Count==99);Checks++;
    }
    const size_t Fields[]={4,8,12,16,24,32,36,60};
    for(size_t I=0;I<sizeof Fields/sizeof Fields[0];I++) {
      uint8_t Copy[292];memcpy(Copy,S.Data,sizeof Copy);Copy[Fields[I]]^=2;FixSum(Copy,sizeof Copy);
      assert(!T70AwRegisterBinValidate(Copy,sizeof Copy,&Count));Checks++;
    }
  }
  T70_AW_REGISTER R;assert(T70AwAcfRegister(B,N,0,0,0,&R)&&R.Address==3&&R.Value==0xffff);
  assert(!T70AwAcfRegister(B,N,0,0,0,NULL));
  printf("AW88261 register data: %u checks; 8 stock sections / 456 records, no I/O\n",Checks);
  return 0;
}
