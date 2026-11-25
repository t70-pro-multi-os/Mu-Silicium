// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfePosition.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
static void Fail(const T70_AFE_PREPARED *P,uint64_t Cur){
  T70_AFE_POSITION R={123,456};C(!T70AfeDl1Position(P,Cur,&R));C(R.ByteOffset==123&&R.FrameOffset==456);
}
int main(void){
  const uint64_t Bases[]={0,1,0xffffff80,0x100000000,T70_AFE_DMA_MAX-196607};
  for(unsigned B=0;B<5;B++)for(unsigned F=1;F<=2;F++)for(unsigned Ch=1;Ch<=2;Ch++){
    T70_AFE_PREPARED P;C(T70AfeDl1Prepare(48000,F,Ch,Bases[B],196608,98304,&P));
    for(uint64_t O=0;O<=196608;O++){
      uint64_t Cur=Bases[B]+O;
      T70_AFE_POSITION Normal={123,456};
      if(O==196608){
        C(!T70AfeDl1NormalizedPosition(&P,Cur,&Normal));
        C(Normal.ByteOffset==123&&Normal.FrameOffset==456);
      }else{
        C(T70AfeDl1NormalizedPosition(&P,Cur,&Normal));
        /* Literal stock bit expression, safe here within validated bounds. */
        uint64_t StockBits=(O<<3)&UINT64_C(0x7ffffffff80);
        C(Normal.ByteOffset==StockBits/8&&Normal.FrameOffset==StockBits/(P.Format.FrameBytes*8));
      }
      if(O==196608||O%16){Fail(&P,Cur);continue;}
      T70_AFE_POSITION R;C(T70AfeDl1Position(&P,Cur,&R));
      C(R.ByteOffset==O&&R.FrameOffset==O/P.Format.FrameBytes);
    }
    if(Bases[B])Fail(&P,Bases[B]-1);
    Fail(&P,UINT64_MAX);Fail(&P,T70_AFE_DMA_MAX+1);
    C(!T70AfeDl1Position(&P,Bases[B],NULL));
  }
  T70_AFE_PREPARED P,Copy;C(T70AfeDl1Prepare(48000,1,2,0x100000000,192,96,&P));
#define BAD(G,F) do{Copy=P;Copy.G.F^=1;Fail(&Copy,P.DmaBase);}while(0)
  BAD(Format,RateCode);BAD(Format,HdMode);BAD(Format,AlignmentCode);BAD(Format,Mono);
  BAD(Format,SampleBytes);BAD(Format,FrameBytes);BAD(Geometry,EndInclusive);
  BAD(Geometry,BaseLow);BAD(Geometry,BaseHigh);BAD(Geometry,EndLow);BAD(Geometry,EndHigh);
  BAD(Geometry,BufferFrames);BAD(Geometry,PeriodFrames);BAD(Geometry,Periods);
#undef BAD
  Copy=P;Copy.SampleFormat=3;Fail(&Copy,P.DmaBase);
  Copy=P;Copy.BufferBytes=UINT64_MAX;Fail(&Copy,P.DmaBase);
  Copy=P;Copy.DmaBase=UINT64_MAX;Fail(&Copy,UINT64_MAX);
  Copy=P;Copy.PeriodBytes=0;Fail(&Copy,P.DmaBase);
  Fail(NULL,0);
  T70_AFE_POSITION N={123,456};
  C(!T70AfeDl1NormalizedPosition(NULL,0,&N));
  C(!T70AfeDl1NormalizedPosition(&P,P.DmaBase,NULL));
  C(!T70AfeDl1NormalizedPosition(&P,P.DmaBase-1,&N));
  C(!T70AfeDl1NormalizedPosition(&P,UINT64_MAX,&N));
  C(!T70AfeDl1NormalizedPosition(&P,T70_AFE_DMA_MAX+1,&N));
  Copy=P;Copy.Format.FrameBytes=0;C(!T70AfeDl1NormalizedPosition(&Copy,P.DmaBase+1,&N));
  Copy=P;Copy.BufferBytes=UINT64_MAX;C(!T70AfeDl1NormalizedPosition(&Copy,P.DmaBase,&N));
  C(N.ByteOffset==123&&N.FrameOffset==456);
  /* A later observation at base remains zero, not an inferred negative delta
     or monotonic lap count. The API deliberately has no previous-position state. */
  T70_AFE_POSITION R;C(T70AfeDl1Position(&P,P.DmaBase+176,&R)&&R.ByteOffset==176);
  C(T70AfeDl1Position(&P,P.DmaBase,&R)&&R.ByteOffset==0);
  printf("AFE CUR position: %u checks; no MMIO reads\n",Checks);
}
