// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwProbe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks,Calls;static uint16_t Id,Raw;static int32_t Errors[2];
#define C(X) do {assert(X);Checks++;}while(0)
static int32_t Read(void *Context,uint16_t Reg,uint16_t *Value){
  C(Context==Errors&&Calls<2&&Reg==Calls);*Value=Calls?Raw:Id;return Errors[Calls++];
}
int main(void){
  T70_AW_STATUS_IO Io={Errors,Read};T70_AW_PROBE_SNAPSHOT S;const uint64_t Base=UINT64_C(0x11e01000);
  for(unsigned Ch=0;Ch<2;Ch++)for(unsigned R=0;R<65536;R++){
    Calls=0;Id=0x2113;Raw=(uint16_t)R;
    T70_AW_PROBE_RESULT V=T70Aw88261Probe(Base,(uint16_t)(0x34+Ch),(uint16_t)Ch,&Io,&S);
    C(V.Code==T70AwProbeRead&&V.TransportStatus==0&&Calls==2&&S.ChipId==Id);
    C(S.Clock.Raw==Raw&&S.Clock.UninterpretedBits==(Raw&0xffee)&&
      S.Clock.PllLocked==(Raw&1)&&S.Clock.ClockStable==((Raw>>4)&1));
  }
  for(unsigned I=0;I<65536;I++)if(I!=0x2113){
    Calls=0;Id=(uint16_t)I;memset(&S,0xa5,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
    C(T70Aw88261Probe(Base,0x34,0,&Io,&S).Code==T70AwProbeWrongId);
    C(Calls==1&&!memcmp(&S,Old,sizeof S));
  }
  const int32_t Failures[]={-1,1,INT32_MIN,INT32_MAX};
  for(unsigned Ch=0;Ch<2;Ch++)for(unsigned Phase=0;Phase<2;Phase++)for(unsigned F=0;F<4;F++){
    Calls=0;Id=0x2113;Errors[0]=Errors[1]=0;Errors[Phase]=Failures[F];
    memset(&S,0xa5,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
    T70_AW_PROBE_RESULT V=T70Aw88261Probe(Base,(uint16_t)(0x34+Ch),(uint16_t)Ch,&Io,&S);
    C(V.Code==(Phase?T70AwProbeStatusReadFailed:T70AwProbeIdReadFailed)&&V.TransportStatus==Failures[F]);
    C(Calls==Phase+1&&!memcmp(&S,Old,sizeof S));
  }
  Errors[0]=Errors[1]=0;
  memset(&S,0x5a,sizeof S);unsigned char InvalidOld[sizeof S];memcpy(InvalidOld,&S,sizeof S);
  for(unsigned I=0;I<65536;I++){
    if(I==0x34)continue;Calls=0;
    C(T70Aw88261Probe(Base,(uint16_t)I,0,&Io,&S).Code==T70AwProbeInvalid&&Calls==0);
    C(!memcmp(&S,InvalidOld,sizeof S));
  }
  Calls=0;
  C(T70Aw88261Probe(Base+1,0x34,0,&Io,&S).Code==T70AwProbeInvalid);
  C(T70Aw88261Probe(Base,0x35,0,&Io,&S).Code==T70AwProbeInvalid);
  C(T70Aw88261Probe(Base,0x34,1,&Io,&S).Code==T70AwProbeInvalid);
  C(T70Aw88261Probe(Base,0x36,2,&Io,&S).Code==T70AwProbeInvalid);
  C(T70Aw88261Probe(Base,0x34,0,NULL,&S).Code==T70AwProbeInvalid);
  C(T70Aw88261Probe(Base,0x34,0,&Io,NULL).Code==T70AwProbeInvalid);
  Io.Read=NULL;C(T70Aw88261Probe(Base,0x34,0,&Io,&S).Code==T70AwProbeInvalid&&Calls==0);
  C(!memcmp(&S,InvalidOld,sizeof S));
  printf("AW non-destructive probe: %u checks; no hardware I/O\n",Checks);
}
