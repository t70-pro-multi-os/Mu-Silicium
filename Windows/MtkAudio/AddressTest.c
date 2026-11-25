// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeAddress.h"
#include <assert.h>
#include <stdio.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
typedef struct {unsigned Calls,Fail;int32_t Error;uint32_t Offset[4],Value[4];} MOCK;
static int32_t Write(void *Context,uint32_t Offset,uint32_t Value){
  MOCK *M=Context;C(M&&M->Calls<4);unsigned I=M->Calls++;
  M->Offset[I]=Offset;M->Value[I]=Value;return I==M->Fail?M->Error:0;
}
int main(void){
  const uint32_t Offsets[]={0x54,0x64,0x50,0x60};
  const uint64_t Bases[]={0,1,0xffffff80,0xffffffff,0x100000000,0x200000000,T70_AFE_DMA_MAX-191};
  const int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
  for(unsigned B=0;B<7;B++)for(unsigned F=0;F<5;F++)for(unsigned Err=0;Err<4;Err++){
    T70_AFE_PREPARED P;C(T70AfeDl1Prepare(48000,1,2,Bases[B],192,96,&P));
    MOCK M={0};M.Fail=F;M.Error=Errors[Err];T70_AFE_ADDRESS_IO Io={&M,Write};
    T70_AFE_ADDRESS_RESULT R=T70AfeDl1SetAddress(&P,&Io);
    C(R.Code==(F==4?T70AfeAddressConfigured:T70AfeAddressWriteFailed));
    C(R.WritesAttempted==M.Calls);
    C(M.Calls==(F==4?4:F+1)&&R.WritesCompleted==(F==4?4:F));
    C(R.TransportStatus==(F==4?0:Errors[Err])&&R.FailedOffset==(F==4?0:Offsets[F]));
    const uint64_t End=Bases[B]+191;
    const uint32_t Values[]={(uint32_t)Bases[B],(uint32_t)End,(uint32_t)(Bases[B]>>32),(uint32_t)(End>>32)};
    for(unsigned I=0;I<M.Calls;I++)C(M.Offset[I]==Offsets[I]&&M.Value[I]==Values[I]);
  }
  T70_AFE_PREPARED P,Copy;C(T70AfeDl1Prepare(48000,1,2,0,192,96,&P));
  MOCK M={0};T70_AFE_ADDRESS_IO Io={&M,Write};
#define BAD(G,F) do{Copy=P;Copy.G.F^=1;C(T70AfeDl1SetAddress(&Copy,&Io).Code==T70AfeAddressInvalid&&M.Calls==0);}while(0)
  BAD(Format,RateCode);BAD(Format,HdMode);BAD(Format,AlignmentCode);BAD(Format,Mono);
  BAD(Format,SampleBytes);BAD(Format,FrameBytes);BAD(Geometry,EndInclusive);
  BAD(Geometry,BaseLow);BAD(Geometry,BaseHigh);BAD(Geometry,EndLow);BAD(Geometry,EndHigh);
  BAD(Geometry,BufferFrames);BAD(Geometry,PeriodFrames);BAD(Geometry,Periods);
#undef BAD
  Copy=P;Copy.DmaBase=UINT64_MAX;C(T70AfeDl1SetAddress(&Copy,&Io).Code==T70AfeAddressInvalid);
  Copy=P;Copy.BufferBytes=UINT64_MAX;C(T70AfeDl1SetAddress(&Copy,&Io).Code==T70AfeAddressInvalid);
  Copy=P;Copy.SampleFormat=3;C(T70AfeDl1SetAddress(&Copy,&Io).Code==T70AfeAddressInvalid);
  C(T70AfeDl1SetAddress(NULL,&Io).Code==T70AfeAddressInvalid);
  C(T70AfeDl1SetAddress(&P,NULL).Code==T70AfeAddressInvalid);
  Io.Write32=NULL;C(T70AfeDl1SetAddress(&P,&Io).Code==T70AfeAddressInvalid&&M.Calls==0);
  printf("AFE DL1 address callbacks: %u checks; mocks only, no MMIO\n",Checks);
}
