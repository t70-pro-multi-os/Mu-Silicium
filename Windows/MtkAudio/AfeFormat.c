// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeFormat.h"
int T70AfeDl1Format(uint32_t Rate,uint32_t Format,uint32_t Channels,
                    T70_AFE_FORMAT *Result) {
  T70_AFE_FORMAT R={0};
  if(!Result || Channels<1 || Channels>2)return 0;
  switch(Format){
  case T70AfeS16Le:R.SampleBytes=2;R.HdMode=0;break;
  case T70AfeS24Le32:R.SampleBytes=4;R.HdMode=1;break;
  default:return 0;
  }
  switch(Rate){
  case 8000:R.RateCode=0;break;case 11025:R.RateCode=1;break;
  case 16000:R.RateCode=4;break;case 22050:R.RateCode=5;break;
  case 32000:R.RateCode=8;break;case 44100:R.RateCode=9;break;
  case 48000:R.RateCode=10;break;case 88200:R.RateCode=11;break;
  case 96000:R.RateCode=12;break;case 176400:R.RateCode=13;break;
  case 192000:R.RateCode=14;break;default:return 0;
  }
  R.Mono=Channels==1;R.FrameBytes=R.SampleBytes*Channels;
  *Result=R;return 1;
}
