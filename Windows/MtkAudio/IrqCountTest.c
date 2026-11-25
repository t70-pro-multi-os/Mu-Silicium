// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeIrqCount.h"
#include <assert.h>
#include <stdio.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
int main(void){
  for(uint64_t F=0;F<=0x40010;F++){
    uint32_t Out=0xdeadbeef;int Valid=F>0&&F<=0x3ffff;
    C(T70AfeIrqCountField(F,&Out)==Valid);
    C(Out==(Valid?(uint32_t)F:0xdeadbeef));
  }
  uint32_t Out=123;C(!T70AfeIrqCountField(UINT64_MAX,&Out)&&Out==123);
  C(!T70AfeIrqCountField(96,NULL));
  for(unsigned F=1;F<=2;F++)for(unsigned Ch=1;Ch<=2;Ch++)for(uint64_t Period=0;Period<=98320;Period++){
    T70_AFE_PREPARED P;
    if(!T70AfeDl1Prepare(48000,F,Ch,0x100000000,Period*2,Period,&P))continue;
    Out=123;C(T70AfeDl1IrqCount(&P,&Out));C(Out==Period/((F==1?2U:4U)*Ch));
  }
  T70_AFE_PREPARED P,Copy;C(T70AfeDl1Prepare(48000,1,1,0,196608,98304,&P));
  C(T70AfeDl1IrqCount(&P,&Out)&&Out==49152);
#define BAD(G,F) do{Copy=P;Copy.G.F^=1;Out=123;C(!T70AfeDl1IrqCount(&Copy,&Out)&&Out==123);}while(0)
  BAD(Format,RateCode);BAD(Format,HdMode);BAD(Format,AlignmentCode);BAD(Format,Mono);
  BAD(Format,SampleBytes);BAD(Format,FrameBytes);BAD(Geometry,EndInclusive);
  BAD(Geometry,BaseLow);BAD(Geometry,BaseHigh);BAD(Geometry,EndLow);BAD(Geometry,EndHigh);
  BAD(Geometry,BufferFrames);BAD(Geometry,PeriodFrames);BAD(Geometry,Periods);
#undef BAD
  Copy=P;Copy.Geometry.PeriodFrames=0x40000;Out=123;C(!T70AfeDl1IrqCount(&Copy,&Out)&&Out==123);
  Copy=P;Copy.BufferBytes=UINT64_MAX;C(!T70AfeDl1IrqCount(&Copy,&Out)&&Out==123);
  Copy=P;Copy.SampleFormat=3;C(!T70AfeDl1IrqCount(&Copy,&Out)&&Out==123);
  C(!T70AfeDl1IrqCount(NULL,&Out)&&Out==123);C(!T70AfeDl1IrqCount(&P,NULL));
  printf("AFE IRQ count: %u checks; no IRQ programming\n",Checks);
}
