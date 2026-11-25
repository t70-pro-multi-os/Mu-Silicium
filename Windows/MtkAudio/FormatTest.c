// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeFormat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
static const uint32_t Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
static const uint32_t Codes[]={0,1,4,5,8,9,10,11,12,13,14};
static void Test(uint32_t Rate,uint32_t Format,uint32_t Ch){
  unsigned I;for(I=0;I<11&&Rates[I]!=Rate;I++);
  int Valid=I<11&&(Format==1||Format==2)&&(Ch==1||Ch==2);
  T70_AFE_FORMAT R;memset(&R,0xa5,sizeof R);unsigned char Old[sizeof R];memcpy(Old,&R,sizeof R);
  C(T70AfeDl1Format(Rate,Format,Ch,&R)==Valid);
  if(!Valid){C(!memcmp(Old,&R,sizeof R));return;}
  C(R.RateCode==Codes[I]&&R.HdMode==Format-1&&R.AlignmentCode==0&&R.Mono==(Ch==1));
  C(R.SampleBytes==(Format==1?2U:4U)&&R.FrameBytes==R.SampleBytes*Ch);
}
int main(void){
  for(uint32_t R=0;R<=384001;R++)for(uint32_t F=1;F<=2;F++)for(uint32_t Ch=1;Ch<=2;Ch++)Test(R,F,Ch);
  for(uint32_t F=0;F<256;F++)for(uint32_t Ch=0;Ch<16;Ch++)Test(48000,F,Ch);
  Test(UINT32_MAX,1,2);Test(48000,UINT32_MAX,2);Test(48000,1,UINT32_MAX);
  C(!T70AfeDl1Format(48000,1,2,NULL));
  printf("AFE DL1 format: %u checks; no hardware operations\n",Checks);
}
