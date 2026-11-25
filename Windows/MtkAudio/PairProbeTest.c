// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairProbe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks,Calls;
#define C(X) do {assert(X);Checks++;}while(0)
typedef struct {unsigned Channel,Seen;uint16_t Id,Raw;int32_t Errors[2];} MOCK;
static int32_t Read(void *Context,uint16_t Reg,uint16_t *Value){
  MOCK *M=Context;C(M->Seen<2&&Reg==M->Seen&&Calls==2*M->Channel+M->Seen);
  *Value=M->Seen?M->Raw:M->Id;Calls++;return M->Errors[M->Seen++];
}
static void Invalid(T70_AW_PROBE_ENDPOINT *L,T70_AW_PROBE_ENDPOINT *R){
  Calls=0;T70_AW_PAIR_SNAPSHOT S;memset(&S,0xa5,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
  T70_AW_PAIR_RESULT V=T70Aw88261PairProbe(L,R,&S);
  C(V.Code==T70AwPairInvalid&&V.FailedChannel==2&&Calls==0&&!memcmp(Old,&S,sizeof S));
}
int main(void){
  MOCK M[2]={{0,0,0x2113,0x1,{0,0}},{1,0,0x2113,0x10,{0,0}}};
  T70_AW_PROBE_ENDPOINT E[2]={{0x11e01000,0x34,0,{&M[0],Read}},{0x11e01000,0x35,1,{&M[1],Read}}};
  for(unsigned Raw=0;Raw<65536;Raw++){
    Calls=M[0].Seen=M[1].Seen=0;M[0].Raw=(uint16_t)Raw;M[1].Raw=(uint16_t)~Raw;
    T70_AW_PAIR_SNAPSHOT S;T70_AW_PAIR_RESULT V=T70Aw88261PairProbe(E,E+1,&S);
    C(V.Code==T70AwPairRead&&V.FailedChannel==2&&Calls==4);
    C(S.Channels[0].ChipId==0x2113&&S.Channels[1].ChipId==0x2113&&
      S.Channels[0].Clock.Raw==M[0].Raw&&S.Channels[1].Clock.Raw==M[1].Raw);
  }
  const int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
  for(unsigned Ch=0;Ch<2;Ch++)for(unsigned Phase=0;Phase<2;Phase++)for(unsigned F=0;F<4;F++){
    M[Ch].Errors[Phase]=Errors[F];Calls=M[0].Seen=M[1].Seen=0;
    T70_AW_PAIR_SNAPSHOT S;memset(&S,0x5a,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
    T70_AW_PAIR_RESULT V=T70Aw88261PairProbe(E,E+1,&S);
    C(V.Code==T70AwPairEndpointFailed&&V.FailedChannel==Ch&&V.Endpoint.TransportStatus==Errors[F]);
    C(V.Endpoint.Code==(Phase?T70AwProbeStatusReadFailed:T70AwProbeIdReadFailed));
    C(Calls==Ch*2+Phase+1&&!memcmp(Old,&S,sizeof S));
    M[Ch].Errors[Phase]=0;
  }
  for(unsigned Ch=0;Ch<2;Ch++){
    M[Ch].Id=0;Calls=M[0].Seen=M[1].Seen=0;
    T70_AW_PAIR_SNAPSHOT S;memset(&S,0x33,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
    T70_AW_PAIR_RESULT V=T70Aw88261PairProbe(E,E+1,&S);
    C(V.Code==T70AwPairEndpointFailed&&V.FailedChannel==Ch&&V.Endpoint.Code==T70AwProbeWrongId);
    C(Calls==Ch*2+1&&!memcmp(Old,&S,sizeof S));M[Ch].Id=0x2113;
  }
  Invalid(NULL,E+1);Invalid(E,NULL);Invalid(E+1,E);Invalid(E,E);
  for(unsigned Ch=0;Ch<2;Ch++){
    T70_AW_PROBE_ENDPOINT Saved=E[Ch];
    E[Ch].Base++;Invalid(E,E+1);E[Ch]=Saved;
    E[Ch].Address^=1;Invalid(E,E+1);E[Ch]=Saved;
    E[Ch].Channel^=1;Invalid(E,E+1);E[Ch]=Saved;
    E[Ch].Io.Read=NULL;Invalid(E,E+1);E[Ch]=Saved;
    E[Ch].Io.Context=NULL;Invalid(E,E+1);E[Ch]=Saved;
  }
  E[1].Io.Context=E[0].Io.Context;Invalid(E,E+1);E[1].Io.Context=&M[1];
  Calls=0;C(T70Aw88261PairProbe(E,E+1,NULL).Code==T70AwPairInvalid&&Calls==0);
  printf("AW pair probe: %u checks; explicit sequential targets, no hardware I/O\n",Checks);
}
