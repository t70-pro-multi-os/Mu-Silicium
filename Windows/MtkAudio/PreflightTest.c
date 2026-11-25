// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPreflight.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define CHECK(X) do { assert(X);Checks++; } while(0)
static void Sum(uint8_t *B,size_t N) {
  uint32_t S=0;for(size_t I=4;I<N;I++)S+=B[I];
  for(unsigned I=0;I<4;I++)B[I]=(uint8_t)(S>>(8*I));
}
static void Rejected(const void *B,size_t N) {
  uint32_t Count=123;
  T70_AW_PREFLIGHT_ENTRY E={123,456,{789,321}};
  CHECK(!T70AwPreflightCount(B,N,&Count)&&Count==123);
  CHECK(!T70AwPreflightEntry(B,N,0,&E)&&E.RecordIndex==123&&
        E.BinOffset==456&&E.Register.Address==789&&E.Register.Value==321);
}
int main(int argc,char **argv) {
  CHECK(argc==2);FILE *F=fopen(argv[1],"rb");CHECK(F);
  uint8_t B[3857];size_t N=fread(B,1,sizeof B,F);CHECK(!ferror(F));fclose(F);CHECK(N==3856);
  for(uint16_t D=0;D<4;D++)for(uint16_t P=0;P<=10;P+=10) {
    T70_AW_SECTION S;CHECK(T70AwAcfFind(B,N,D,P,4,&S));
    uint32_t Count=99;CHECK(T70AwPreflightCount(S.Data,S.Length,&Count));
    uint32_t Found=0;
    for(uint32_t I=0;I<57;I++) {
      T70_AW_REGISTER R;CHECK(T70AwAcfRegister(B,N,D,P,I,&R));
      switch(R.Address) {
      case 4:case 5:case 8:case 0x14:case 0x50: {
        T70_AW_PREFLIGHT_ENTRY E;
        CHECK(T70AwPreflightEntry(S.Data,S.Length,Found++,&E));
        CHECK(E.RecordIndex==I&&E.BinOffset==64+(size_t)I*4&&
              E.Register.Address==R.Address&&E.Register.Value==R.Value);break;
      } default:break;
      }
    }
    CHECK(Found==Count&&Count==4);
    printf("section %u/%u: %u special records\n",D,P,Count);
    T70_AW_PREFLIGHT_ENTRY E={123,456,{789,321}};
    CHECK(!T70AwPreflightEntry(S.Data,S.Length,Count,&E)&&E.RecordIndex==123);
    CHECK(!T70AwPreflightEntry(S.Data,S.Length,UINT32_MAX,&E)&&E.RecordIndex==123);
    for(size_t I=0;I<S.Length;I++) {
      Rejected(S.Data,I);
      uint8_t C[292];memcpy(C,S.Data,sizeof C);C[I]^=1;Rejected(C,sizeof C);
    }
    uint8_t C[293];memcpy(C,S.Data,S.Length);C[292]=0;Rejected(C,sizeof C);
    /* Repeated special addresses at every position preserve order, no dedup. */
    for(unsigned I=0;I<57;I++){C[64+I*4]=4;C[65+I*4]=0;}
    Sum(C,292);CHECK(T70AwPreflightCount(C,292,&Count)&&Count==57);
    for(uint32_t I=0;I<57;I++) {
      CHECK(T70AwPreflightEntry(C,292,I,&E)&&E.RecordIndex==I&&E.BinOffset==64+4*I);
    }
    const uint16_t Known[]={4,5,8,0x14,0x50};
    for(size_t K=0;K<sizeof Known/sizeof Known[0];K++) {
      for(unsigned I=0;I<57;I++)C[64+I*4]=(uint8_t)Known[K];
      Sum(C,292);CHECK(T70AwPreflightCount(C,292,&Count)&&Count==57);
      CHECK(T70AwPreflightEntry(C,292,56,&E)&&E.Register.Address==Known[K]);
    }
    /* Upper address byte must not be truncated to a known address. */
    for(unsigned I=0;I<57;I++)C[65+I*4]=1;
    Sum(C,292);CHECK(T70AwPreflightCount(C,292,&Count)&&Count==0);
    CHECK(!T70AwPreflightEntry(C,292,0,&E));
    const size_t Fields[]={4,8,12,16,24,32,36,60};
    for(size_t I=0;I<sizeof Fields/sizeof Fields[0];I++) {
      memcpy(C,S.Data,S.Length);C[Fields[I]]^=2;Sum(C,292);Rejected(C,292);
    }
    CHECK(!T70AwPreflightCount(S.Data,S.Length,NULL));
    CHECK(!T70AwPreflightEntry(S.Data,S.Length,0,NULL));
  }
  Rejected(NULL,292);
  if(SIZE_MAX>UINT32_MAX)Rejected(B,(size_t)UINT32_MAX+1);
  printf("AW preflight inventory: %u checks; 8 stock sections, no apply policy or I/O\n",Checks);
}
