// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfePrepare.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
static const uint32_t Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
static const uint32_t Codes[]={0,1,4,5,8,9,10,11,12,13,14};
static void Test(uint32_t Rate,uint32_t F,uint32_t Ch,uint64_t Base,uint64_t B,uint64_t Period){
  unsigned I;for(I=0;I<11&&Rates[I]!=Rate;I++);
  unsigned Frame=(F==1?2U:4U)*Ch;
  int Valid=I<11&&(F==1||F==2)&&(Ch==1||Ch==2);
  if(Valid)Valid=B&&B<=196608&&B%16==0&&Period>=96&&Period<=196608;
  if(Valid)Valid=B%Period==0&&B%Frame==0&&Period%Frame==0&&B/Period>=2&&B/Period<=256&&
    (__uint128_t)Base+B<=(__uint128_t)T70_AFE_DMA_MAX+1;
  T70_AFE_PREPARED P;memset(&P,0x5a,sizeof P);unsigned char Old[sizeof P];memcpy(Old,&P,sizeof P);
  C(T70AfeDl1Prepare(Rate,F,Ch,Base,B,Period,&P)==Valid);
  if(!Valid){C(!memcmp(Old,&P,sizeof P));return;}
  C(P.Rate==Rate&&P.SampleFormat==F&&P.Channels==Ch&&P.DmaBase==Base&&P.BufferBytes==B&&P.PeriodBytes==Period);
  C(P.Format.RateCode==Codes[I]&&P.Format.FrameBytes==Frame&&P.Format.SampleBytes==Frame/Ch);
  C(P.Geometry.BufferFrames==B/Frame&&P.Geometry.PeriodFrames==Period/Frame&&P.Geometry.Periods==B/Period);
  C(P.Geometry.EndInclusive==Base+B-1);
}
int main(void){
  const uint64_t Bases[]={0,1,0xffffff80,0x100000000,T70_AFE_DMA_MAX-191,T70_AFE_DMA_MAX,T70_AFE_DMA_MAX+1,UINT64_MAX};
  const uint64_t Bytes[]={0,191,192,256,400,408,416,24576,24672,196608,196609,UINT64_MAX};
  const uint64_t Periods[]={0,95,96,100,102,104,128,98304,196608,UINT64_MAX};
  for(unsigned R=0;R<11;R++)for(unsigned F=0;F<=3;F++)for(unsigned Ch=0;Ch<=3;Ch++)
   for(unsigned A=0;A<sizeof Bases/sizeof Bases[0];A++)
    for(unsigned B=0;B<sizeof Bytes/sizeof Bytes[0];B++)
     for(unsigned P=0;P<sizeof Periods/sizeof Periods[0];P++)Test(Rates[R],F,Ch,Bases[A],Bytes[B],Periods[P]);
  Test(12000,1,2,0,192,96);Test(UINT32_MAX,1,2,0,192,96);
  Test(48000,UINT32_MAX,2,0,192,96);Test(48000,1,UINT32_MAX,0,192,96);
  C(!T70AfeDl1Prepare(48000,1,2,0,192,96,NULL));
  T70_AFE_PREPARED First,Second;
  C(T70AfeDl1Prepare(48000,1,2,0,192,96,&First));
  unsigned char Saved[sizeof First];memcpy(Saved,&First,sizeof First);
  C(T70AfeDl1Prepare(44100,2,1,0x100000000,400,100,&Second));
  C(!memcmp(Saved,&First,sizeof First)&&Second.Format.FrameBytes==4);
  C(!T70AfeDl1Prepare(48000,2,2,0,400,100,&First));
  C(!memcmp(Saved,&First,sizeof First));
  printf("AFE prepare snapshot: %u checks; no hardware operations\n",Checks);
}
