// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeGeometry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do {assert(X);Checks++;}while(0)
static void Test(uint64_t Base,uint64_t Bytes,uint64_t Period,uint32_t Frame) {
  T70_AFE_GEOMETRY G;memset(&G,0x5a,sizeof G);unsigned char Old[sizeof G];memcpy(Old,&G,sizeof G);
  /* Independent wide-arithmetic oracle, including overflow inputs. */
  __uint128_t End=(__uint128_t)Base+Bytes;
  int Valid=Frame&&Bytes&&Bytes<=196608&&Bytes%16==0&&Period>=96&&Period<=196608;
  if(Valid)Valid=Bytes%Period==0&&Bytes%Frame==0&&Period%Frame==0&&
    Bytes/Period>=2&&Bytes/Period<=256&&End<=(__uint128_t)T70_AFE_DMA_MAX+1;
  C(T70AfeBufferGeometry(Base,Bytes,Period,Frame,&G)==Valid);
  if(!Valid){C(!memcmp(Old,&G,sizeof G));return;}
  C(G.EndInclusive==(uint64_t)(End-1));
  C((((uint64_t)G.BaseHigh<<32)|G.BaseLow)==Base);
  C((((uint64_t)G.EndHigh<<32)|G.EndLow)==G.EndInclusive);
  C((uint64_t)G.BufferFrames*Frame==Bytes&&(uint64_t)G.PeriodFrames*Frame==Period&&G.Periods==Bytes/Period);
}
int main(void){
  const uint64_t Bases[]={0,1,15,16,0xffffff80,0xffffffff,0x100000000,0x300000000,
    T70_AFE_DMA_MAX-191,T70_AFE_DMA_MAX,T70_AFE_DMA_MAX+1,UINT64_MAX};
  const uint64_t Sizes[]={0,1,15,16,95,96,97,191,192,193,256,512,1024,24576,24672,196608,196609,UINT64_MAX};
  const uint64_t Periods[]={0,1,95,96,97,128,256,768,98304,196608,196609,UINT64_MAX};
  const uint32_t Frames[]={0,1,2,3,4,6,8,16,UINT32_MAX};
  for(unsigned B=0;B<sizeof Bases/sizeof Bases[0];B++)
   for(unsigned S=0;S<sizeof Sizes/sizeof Sizes[0];S++)
    for(unsigned P=0;P<sizeof Periods/sizeof Periods[0];P++)
     for(unsigned F=0;F<sizeof Frames/sizeof Frames[0];F++)Test(Bases[B],Sizes[S],Periods[P],Frames[F]);
  for(uint64_t S=0;S<=196624;S++)Test(0x100000000,S,96,4);
  C(!T70AfeBufferGeometry(0,192,96,4,NULL));
  printf("AFE buffer geometry: %u arithmetic checks; no MMIO/DMA operation\n",Checks);
}
