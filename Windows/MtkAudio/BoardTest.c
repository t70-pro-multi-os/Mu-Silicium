// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwBoard.h"
#include <assert.h>
#include <stdio.h>
int main(int argc,char **argv) {
  assert(argc==2);FILE *F=fopen(argv[1],"rb");assert(F);
  uint8_t B[3857];size_t N=fread(B,1,sizeof B,F);assert(!ferror(F));fclose(F);assert(N==3856);
  unsigned Checks=0;
  for(uint16_t Channel=0;Channel<4;Channel++)for(uint16_t Address=0;Address<128;Address++) {
    T70_AW_REGISTER R={999,999};
    int Expected=Channel<2 && Address==0x34+Channel;
    assert(T70AwBoardRegister(B,N,0x11e01000,Address,Channel,0,0,&R)==Expected);
    if(!Expected)assert(R.Address==999&&R.Value==999);
    Checks++;
  }
  for(uint16_t Channel=0;Channel<2;Channel++)for(uint16_t Profile=0;Profile<=10;Profile+=10)
    for(uint32_t Index=0;Index<57;Index++) {
      T70_AW_REGISTER A,R;
      assert(T70AwBoardRegister(B,N,0x11e01000,(uint16_t)(0x34+Channel),Channel,Profile,Index,&A));
      assert(T70AwAcfRegister(B,N,Channel,Profile,Index,&R));
      assert(A.Address==R.Address&&A.Value==R.Value);Checks++;
    }
  T70_AW_REGISTER R={999,999};
  assert(!T70AwBoardRegister(B,N,0x11e00000,0x34,0,0,0,&R));
  assert(!T70AwBoardRegister(B,N,0x11e01000,0x34,0,1,0,&R));
  assert(!T70AwBoardRegister(B,N,0x11e01000,0x34,0,0,57,&R));
  assert(R.Address==999&&R.Value==999);
  printf("T70 AW board mapping: %u checks plus invalid bus/profile/index; no I/O\n",Checks);
  return 0;
}
