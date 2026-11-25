// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwProfile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do {assert(X);Checks++;}while(0)
static uint32_t U32(const uint8_t *P){return P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);}
static uint8_t Crc(const uint8_t *P,size_t N){uint8_t V=0;for(size_t I=0;I<N;I++){V^=P[I];for(unsigned J=0;J<8;J++)V=(uint8_t)((V>>1)^((V&1)?0x8c:0));}return V;}
static void Fail(const void *B,size_t N,uint64_t Base,uint16_t A,uint16_t Ch,uint16_t P) {
  T70_AW_PROFILE_VIEW V;memset(&V,0x5a,sizeof V);
  unsigned char Old[sizeof V];memcpy(Old,&V,sizeof V);
  C(!T70AwProfileView(B,N,Base,A,Ch,P,&V));C(!memcmp(Old,&V,sizeof V));
}
int main(int argc,char **argv){
  C(argc==2);FILE *F=fopen(argv[1],"rb");C(F);uint8_t B[3857],Copy[3857];
  size_t N=fread(B,1,sizeof B,F);C(!ferror(F));fclose(F);C(N==3856);
  const uint64_t Base=UINT64_C(0x11e01000);
  for(uint16_t Ch=0;Ch<4;Ch++)for(uint16_t P=0;P<=10;P+=10){
    T70_AW_SECTION S;C(T70AwAcfFind(B,N,Ch,P,4,&S));
    if(Ch>=2){Fail(B,N,Base,(uint16_t)(0x34+Ch),Ch,P);continue;}
    T70_AW_PROFILE_VIEW V;C(T70AwProfileView(B,N,Base,(uint16_t)(0x34+Ch),Ch,P,&V));
    C(V.RecordCount==57&&V.SpecialCount==4&&V.Section.Data==S.Data&&V.Section.Length==S.Length);
    C(V.Section.DeviceIndex==Ch&&V.Section.Profile==P&&V.Section.SectionType==4);
    for(uint32_t I=0;I<V.SpecialCount;I++){T70_AW_PREFLIGHT_ENTRY E;C(T70AwPreflightEntry(V.Section.Data,V.Section.Length,I,&E));}
    /* A separate backing allocation yields its own borrowed pointers; selecting
       another profile does not modify a prior view or retain global state. */
    memcpy(Copy,B,N);T70_AW_PROFILE_VIEW W;C(T70AwProfileView(Copy,N,Base,(uint16_t)(0x34+Ch),Ch,P,&W));
    C(W.Section.Data-Copy==V.Section.Data-B&&W.Section.Data!=V.Section.Data);
    C(V.Section.Data==S.Data);
    for(size_t I=0;I<N;I++)Fail(B,I,Base,(uint16_t)(0x34+Ch),Ch,P);
    for(size_t I=0;I<N;I++){memcpy(Copy,B,N);Copy[I]^=1;
      /* Only mutations rejected by the ACF/bin contracts are asserted invalid:
         unused container bytes are not authenticated or semantic fields. */
      uint32_t Count;T70_AW_SECTION T;
      if(!T70AwAcfFind(Copy,N,Ch,P,4,&T)||!T70AwRegisterBinValidate(T.Data,T.Length,&Count))
        Fail(Copy,N,Base,(uint16_t)(0x34+Ch),Ch,P);
    }
  }
  Fail(NULL,N,Base,0x34,0,0);Fail(B,N,Base+1,0x34,0,0);
  Fail(B,N,Base,0x35,0,0);Fail(B,N,Base,0x34,1,0);
  Fail(B,N,Base,0x34,0,1);Fail(B,N,Base,0x34,UINT16_MAX,0);
  C(!T70AwProfileView(B,N,Base,0x34,0,0,NULL));
  memcpy(Copy,B,N);Copy[N]=0;Fail(Copy,N+1,Base,0x34,0,0);
  /* Duplicate a descriptor's identity without changing its payload CRC. */
  memcpy(Copy,B,N);memcpy(Copy+80+64+20,Copy+80+20,10);Fail(Copy,N,Base,0x34,0,0);
  /* Corrupt selected bin structure, repair outer CRC: inner validation remains
     mandatory even when container corruption checks pass. */
  memcpy(Copy,B,N);
  unsigned Found=0;
  for(unsigned I=0;I<12;I++){uint8_t *D=Copy+80+64*I;
    if(D[20]==0&&D[26]==0&&U32(D+28)==4){
      uint32_t O=U32(D+36),L=U32(D+32);Copy[O+32]=3;
      D[40]=Crc(Copy+O,L);Fail(Copy,N,Base,0x34,0,0);Found=1;break;
    }
  }
  C(Found==1);
  printf("AW profile view: %u checks; two board channels, eight ACF selections, no I/O\n",Checks);
}
