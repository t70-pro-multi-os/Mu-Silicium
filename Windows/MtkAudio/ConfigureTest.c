// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeConfigure.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
static unsigned Checks;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d: %s\n",__LINE__,#X);exit(1);}}while(0)
typedef struct {unsigned Calls,Fail;int32_t Error;uint32_t Register,Values[4];} MOCK;
static int32_t Write(void *Ctx,uint32_t Offset,uint32_t Value){
  MOCK *M=Ctx;unsigned N=++M->Calls;
  const uint32_t Offsets[]={0x54,0x64,0x50,0x60};
  C(N<=12);
  if(N<=4){C(Offset==Offsets[N-1]);C(Value==M->Values[N-1]);}
  else {C(!(N%2));C(Offset==0x4c);M->Register=Value;}
  return N==M->Fail?M->Error:0;
}
static int32_t Read(void *Ctx,uint32_t Offset,uint32_t *Value){
  MOCK *M=Ctx;unsigned N=++M->Calls;C(N>=5&&N<=11&&(N%2));C(Offset==0x4c);
  *Value=M->Register;return N==M->Fail?M->Error:0;
}
static void Run(T70_AFE_PREPARED P,unsigned Fail,int32_t Error){
  MOCK M={0,Fail,Error,0xa5a5a5a5,{P.Geometry.BaseLow,P.Geometry.EndLow,P.Geometry.BaseHigh,P.Geometry.EndHigh}};
  T70_AFE_FORMAT_IO Io={&M,Read,Write};
  T70_AFE_CONFIG_RESULT R=T70AfeDl1Configure(&P,&Io);
  C(M.Calls==(Fail?Fail:12));C(R.AddressStarted==1);
  C(R.FormatStarted==(Fail==0||Fail>4));
  C(R.Address.WritesAttempted==(Fail&&Fail<=4?Fail:4));
  C(R.Address.WritesCompleted==(Fail&&Fail<=4?Fail-1:4));
  if(Fail&&Fail<=4){C(R.Code==T70AfeConfigAddressFailed);C(R.Address.TransportStatus==Error);C((R.Address.FailedOffset==(uint32_t[]){0x54,0x64,0x50,0x60}[Fail-1]));}
  else {
    unsigned Steps=Fail?Fail-4:8;
    C(R.Format.ReadsAttempted==(Steps+1)/2);C(R.Format.WritesAttempted==Steps/2);
    C(R.Format.WritesCompleted==Steps/2-((Fail&&!(Fail%2))?1:0));
    if(Fail){C(R.Code==T70AfeConfigFormatFailed);C(R.Format.TransportStatus==Error);C(R.Format.FailedField==(Fail-5)/2);C(R.Format.Code==(Fail%2?T70AfeFieldsReadFailed:T70AfeFieldsWriteFailed));}
    else {C(R.Code==T70AfeConfigCallbacksCompleted);C(R.Format.Code==T70AfeFieldsUpdated);}
    uint32_t Expected=0xa5a5a5a5;
    const uint32_t Masks[]={0x100,0x0f000000,3,0x10};
    const uint32_t Values[]={P.Format.Mono<<8,P.Format.RateCode<<24,P.Format.HdMode,P.Format.AlignmentCode<<4};
    for(unsigned I=0;I<Steps/2;I++)Expected=(Expected&~Masks[I])|(Values[I]&Masks[I]);
    C(M.Register==Expected); /* failing writes intentionally may have applied */
  }
}
static void Invalid(const T70_AFE_PREPARED *P,T70_AFE_FORMAT_IO *Io,MOCK *M){
  T70_AFE_CONFIG_RESULT R=T70AfeDl1Configure(P,Io);C(R.Code==T70AfeConfigInvalid);C(!R.AddressStarted&&!R.FormatStarted);C(!M->Calls);
}
int main(void){
  const uint64_t Bases[]={0,0xfffffff0,0x300000000,0x3fffffe00};
  const uint32_t Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
  const int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
  T70_AFE_PREPARED P;
  for(unsigned B=0;B<4;B++)for(unsigned R=0;R<11;R++)for(unsigned F=1;F<=2;F++)for(unsigned Ch=1;Ch<=2;Ch++){
    C(T70AfeDl1Prepare(Rates[R],F,Ch,Bases[B],512,256,&P));Run(P,0,0);
    for(unsigned Fail=1;Fail<=12;Fail++)for(unsigned E=0;E<4;E++)Run(P,Fail,Errors[E]);
  }
  MOCK M={0};T70_AFE_FORMAT_IO Io={&M,Read,Write};
  Invalid(NULL,&Io,&M);Invalid(&P,NULL,&M);Io.Read32=NULL;Invalid(&P,&Io,&M);Io.Read32=Read;Io.Write32=NULL;Invalid(&P,&Io,&M);Io.Write32=Write;
#define BAD(FIELD) do{T70_AFE_PREPARED Bad=P;Bad.FIELD^=1;Invalid(&Bad,&Io,&M);}while(0)
  BAD(Format.RateCode);BAD(Format.HdMode);BAD(Format.AlignmentCode);BAD(Format.Mono);BAD(Format.SampleBytes);BAD(Format.FrameBytes);
  BAD(Geometry.EndInclusive);BAD(Geometry.BaseLow);BAD(Geometry.BaseHigh);BAD(Geometry.EndLow);BAD(Geometry.EndHigh);BAD(Geometry.BufferFrames);BAD(Geometry.PeriodFrames);BAD(Geometry.Periods);
  P.SampleFormat=T70AfeS32Le;Invalid(&P,&Io,&M);
  printf("ConfigureTest: %u checks passed\n",Checks);return 0;
}
