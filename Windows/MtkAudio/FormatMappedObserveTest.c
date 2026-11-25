// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "FormatMappedObserve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Calls,Raise,Uses;static unsigned char Irql;
static int32_t Error;static uint32_t Raw;static ULONG Ram[2048];
static T70_AFE_MAPPING_OWNER *Current;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG F){(void)P;(void)N;(void)F;abort();}
void MmUnmapIoSpace(void *P,size_t N){(void)P;(void)N;abort();}
int32_t T70AfeMappedRead32(void *Ctx,uint32_t Offset,uint32_t *V){
 C(Ctx==&Current->Mapping && Offset==0x4c && Current->ActiveUses==Uses+1);
 Calls++;*V=Raw;if(Raise)Irql=3;return Error;
}
int main(void){
 T70_AFE_MAPPING_OWNER O={{0x11210000,Ram,sizeof Ram},0};Current=&O;
 T70_AFE_PREPARED P;T70_AFE_FORMAT_OBSERVATION Out,Old;T70_FORMAT_OBSERVE_RESULT R;
 const uint32_t Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 for(unsigned K=0;K<11;K++)for(unsigned F=1;F<=2;F++)for(unsigned Ch=1;Ch<=2;Ch++){
  C(T70AfeDl1Prepare(Rates[K],F,Ch,4096,192,96,&P));
  for(unsigned Bits=0;Bits<256;Bits++)for(unsigned Fill=0;Fill<2;Fill++){
   unsigned Mono=(Bits>>7)&1,Rate=(Bits>>3)&15,Hd=(Bits>>1)&3,Align=Bits&1;
   Raw=(Mono<<8)|(Rate<<24)|Hd|(Align<<4);
   if(Fill)Raw|=~(uint32_t)0x0f000113;
   Calls=0;R=T70AfeObserveMappedFormat(&O,&P,&Out);
   unsigned Match=(Mono==P.Format.Mono?1:0)|(Rate==P.Format.RateCode?2:0)|
     (Hd==P.Format.HdMode?4:0)|(Align==P.Format.AlignmentCode?8:0);
   C(R.Code==T70FormatObserved && Calls==1 && R.LeaseEnded && O.ActiveUses==0);
   C(Out.Raw==Raw && Out.Mono==Mono && Out.RateCode==Rate && Out.HdMode==Hd &&
     Out.AlignmentCode==Align && Out.MatchMask==Match);
  }
 }
 const int32_t Errors[]={0,-73,1,INT32_MIN,INT32_MAX};
 for(Uses=0;Uses<3;Uses++)for(unsigned E=0;E<5;E++)for(Raise=0;Raise<2;Raise++){
  O.ActiveUses=Uses;Error=Errors[E];Irql=0;Calls=0;
  memset(&Out,0xa5,sizeof Out);Old=Out;
  R=T70AfeObserveMappedFormat(&O,&P,&Out);
  C(Calls==1 && R.EndAttempted && R.ReadStatus==Error);
  C(R.Code==(Raise?T70FormatObserveEndFailed:Error?T70FormatObserveReadFailed:T70FormatObserved));
  C(O.ActiveUses==Uses+(Raise?1:0) && R.LeaseEnded==!Raise);
  if(Raise||Error)C(!memcmp(&Out,&Old,sizeof Out));
  if(Raise){Irql=0;C(!T70AfeMappingEnd(&O));}
 }
 Uses=0;O.ActiveUses=0;Raise=0;Error=0;Irql=0;Calls=0;
 memset(&Out,0xa5,sizeof Out);Old=Out;
#define BAD(A,B,D) do{R=T70AfeObserveMappedFormat(A,B,D);C(R.Code==T70FormatObserveInvalid && !Calls);C(!memcmp(&Out,&Old,sizeof Out));}while(0)
 BAD(NULL,&P,&Out);BAD(&O,NULL,&Out);BAD(&O,&P,NULL);
 BAD(&O,&P,(void *)&O);BAD(&O,(void *)Ram,&Out);BAD(&O,&P,(void *)Ram);
 BAD(&O,&P,(void *)&P);BAD(&O,(void *)(UINTPTR_MAX-7),&Out);
 P.Format.HdMode^=1;BAD(&O,&P,&Out);P.Format.HdMode^=1;
 Irql=3;R=T70AfeObserveMappedFormat(&O,&P,&Out);
 C(R.Code==T70FormatObserveBeginFailed && !Calls && !R.EndAttempted);
 C(!memcmp(&Out,&Old,sizeof Out));Irql=0;
 O.ActiveUses=UINT32_MAX;R=T70AfeObserveMappedFormat(&O,&P,&Out);
 C(R.Code==T70FormatObserveBeginFailed && !Calls && !R.EndAttempted);
 C(!memcmp(&Out,&Old,sizeof Out));
 printf("FormatMappedObserve: %u checks; fake mapped reader only\n",Checks);return 0;
}
