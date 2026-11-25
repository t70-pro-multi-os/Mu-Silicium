// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeGeometry.h"
int T70AfeBufferGeometry(uint64_t Base,uint64_t BufferBytes,uint64_t PeriodBytes,
                         uint32_t FrameBytes,T70_AFE_GEOMETRY *Geometry) {
  T70_AFE_GEOMETRY G={0};uint64_t Periods;
  if(!Geometry || !FrameBytes || !BufferBytes || BufferBytes>0x30000 ||
     BufferBytes%16 || PeriodBytes<96 || PeriodBytes>0x30000 ||
     BufferBytes%PeriodBytes || BufferBytes%FrameBytes || PeriodBytes%FrameBytes ||
     Base>T70_AFE_DMA_MAX || BufferBytes-1>T70_AFE_DMA_MAX-Base)return 0;
  Periods=BufferBytes/PeriodBytes;
  if(Periods<2 || Periods>256)return 0;
  G.EndInclusive=Base+BufferBytes-1;
  G.BaseLow=(uint32_t)Base;G.BaseHigh=(uint32_t)(Base>>32);
  G.EndLow=(uint32_t)G.EndInclusive;G.EndHigh=(uint32_t)(G.EndInclusive>>32);
  G.BufferFrames=(uint32_t)(BufferBytes/FrameBytes);
  G.PeriodFrames=(uint32_t)(PeriodBytes/FrameBytes);G.Periods=(uint32_t)Periods;
  *Geometry=G;return 1;
}
