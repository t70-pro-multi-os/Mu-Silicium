// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfePrepare.h"
int T70AfeDl1Prepare(uint32_t Rate,uint32_t SampleFormat,uint32_t Channels,
                     uint64_t DmaBase,uint64_t BufferBytes,uint64_t PeriodBytes,
                     T70_AFE_PREPARED *Prepared) {
  T70_AFE_PREPARED P={0};
  if(!Prepared || !T70AfeDl1Format(Rate,SampleFormat,Channels,&P.Format) ||
     !T70AfeBufferGeometry(DmaBase,BufferBytes,PeriodBytes,P.Format.FrameBytes,
                           &P.Geometry))return 0;
  P.Rate=Rate;P.SampleFormat=SampleFormat;P.Channels=Channels;
  P.DmaBase=DmaBase;P.BufferBytes=BufferBytes;P.PeriodBytes=PeriodBytes;
  *Prepared=P;return 1;
}

int T70AfeDl1PreparedValid(const T70_AFE_PREPARED *Prepared) {
  T70_AFE_PREPARED P;
  if(!Prepared ||
     !T70AfeDl1Prepare(Prepared->Rate,Prepared->SampleFormat,Prepared->Channels,
                       Prepared->DmaBase,Prepared->BufferBytes,Prepared->PeriodBytes,&P))return 0;
#define SAME(G,F) (Prepared->G.F==P.G.F)
  if(!SAME(Format,RateCode)||!SAME(Format,HdMode)||!SAME(Format,AlignmentCode)||
     !SAME(Format,Mono)||!SAME(Format,SampleBytes)||!SAME(Format,FrameBytes)||
     !SAME(Geometry,EndInclusive)||!SAME(Geometry,BaseLow)||!SAME(Geometry,BaseHigh)||
     !SAME(Geometry,EndLow)||!SAME(Geometry,EndHigh)||!SAME(Geometry,BufferFrames)||
     !SAME(Geometry,PeriodFrames)||!SAME(Geometry,Periods))return 0;
#undef SAME
  return 1;
}
