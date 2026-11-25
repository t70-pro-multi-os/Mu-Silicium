// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwVolumeUpdate.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned Checks,Reads,Writes,Sequence;
static uint16_t Existing,Expected,*CallerOutput;
static int32_t ReadStatus,WriteStatus;
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();}}while(0)
static int32_t Read(void *C,uint16_t Register,uint16_t *Value){
 CHECK(C==&Existing && Register==5 && Sequence++==0);Reads++;
 CHECK(*CallerOutput==0xbeef);*Value=Existing;return ReadStatus;
}
static int32_t Write(void *C,uint16_t Register,uint16_t Value){
 CHECK(C==&Existing && Register==5 && Sequence++==1);Writes++;
 CHECK(Value==Expected && *CallerOutput==0xbeef);return WriteStatus;
}
static void Reset(uint16_t *Out){Reads=Writes=Sequence=0;ReadStatus=WriteStatus=0;*Out=0xbeef;CallerOutput=Out;}
int main(void){
 T70_AW_VOLUME_IO Io={&Existing,Read,Write};uint16_t Output;
 const uint16_t Initial[]={0,47,48,719,720,783};const uint32_t Request[]={0,1,48,2,720,0};
 for(unsigned Old=0;Old<65536;Old++)for(unsigned P=0;P<6;P++){
  Reset(&Output);Existing=(uint16_t)Old;
  unsigned A=Initial[P]+Request[P];if(A>720)A=720;
  Expected=(uint16_t)((Old&0xfc00)|(A+16*(A/48)));
  T70_AW_VOLUME_UPDATE_RESULT R=T70Aw88261UpdateVolume(&Io,Initial[P],Request[P],&Output);
  CHECK(R.Code==T70AwVolumeUpdated && R.TransportStatus==0);
  CHECK(Reads==1 && Writes==1 && Output==Expected);
 }
 const int32_t Errors[]={-1,-123,17,INT32_MIN,INT32_MAX};
 for(unsigned E=0;E<5;E++)for(unsigned Phase=0;Phase<2;Phase++){
  Reset(&Output);Existing=0xac00;Expected=0xac00;
  if(Phase)WriteStatus=Errors[E];else ReadStatus=Errors[E];
  T70_AW_VOLUME_UPDATE_RESULT R=T70Aw88261UpdateVolume(&Io,0,0,&Output);
  CHECK(R.Code==(Phase?T70AwVolumeWriteFailed:T70AwVolumeReadFailed));
  CHECK(R.TransportStatus==Errors[E] && Output==0xbeef);
  CHECK(Reads==1 && Writes==Phase);
 }
 Reset(&Output);
 for(unsigned V=721;V<65536;V++)CHECK(T70Aw88261UpdateVolume(&Io,0,V,&Output).Code==T70AwVolumeInvalid);
 CHECK(T70Aw88261UpdateVolume(&Io,0,UINT32_MAX,&Output).Code==T70AwVolumeInvalid);
 for(unsigned I=784;I<65536;I++)CHECK(T70Aw88261UpdateVolume(&Io,(uint16_t)I,0,&Output).Code==T70AwVolumeInvalid);
 CHECK(T70Aw88261UpdateVolume(NULL,0,0,&Output).Code==T70AwVolumeInvalid);
 CHECK(T70Aw88261UpdateVolume(&Io,0,0,NULL).Code==T70AwVolumeInvalid);
 Io.Read=NULL;CHECK(T70Aw88261UpdateVolume(&Io,0,0,&Output).Code==T70AwVolumeInvalid);
 Io.Read=Read;Io.Write=NULL;CHECK(T70Aw88261UpdateVolume(&Io,0,0,&Output).Code==T70AwVolumeInvalid);
 CHECK(Reads==0 && Writes==0 && Output==0xbeef);
 printf("AW volume update: %u checks passed\n",Checks);return 0;
}
