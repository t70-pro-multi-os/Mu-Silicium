// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AddressMappedObserve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Calls,Fail,Raise,Uses;static unsigned char Irql;
static int32_t Error;static uint32_t Values[4];static ULONG Ram[2048];
static T70_AFE_MAPPING_OWNER *Current;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG F){(void)P;(void)N;(void)F;abort();}
void MmUnmapIoSpace(void *P,size_t N){(void)P;(void)N;abort();}
int32_t T70AfeMappedRead32(void *Ctx,uint32_t Offset,uint32_t *V){
 static const unsigned Off[]={0x54,0x64,0x50,0x60};
 C(Ctx==&Current->Mapping && Calls<4 && Offset==Off[Calls] && Current->ActiveUses==Uses+1);
 *V=Values[Calls++];if(Raise)Irql=3;return Calls==Fail?Error:0;
}
int main(void){
 T70_AFE_MAPPING_OWNER O={{0x11210000,Ram,sizeof Ram},0};Current=&O;
 T70_AFE_PREPARED P;T70_AFE_ADDRESS_OBSERVATION Out,Old;T70_ADDRESS_OBSERVE_RESULT R;
 const uint64_t Bases[]={0,0xffffff80,0x100000000,0x3ffffff40};
 const unsigned Off[]={0x54,0x64,0x50,0x60};
 for(unsigned K=0;K<4;K++){
  C(T70AfeDl1Prepare(48000,T70AfeS24Le32,2,Bases[K],192,96,&P));
  const uint32_t E[]={P.Geometry.BaseLow,P.Geometry.EndLow,P.Geometry.BaseHigh,P.Geometry.EndHigh};
  for(unsigned Match=0;Match<16;Match++)for(unsigned Bit=0;Bit<32;Bit++){
   for(unsigned I=0;I<4;I++)Values[I]=E[I]^((Match&(1u<<I))?0:(1u<<Bit));
   Calls=0;R=T70AfeObserveMappedAddress(&O,&P,&Out);
   C(R.Code==T70AddressObserved && R.ReadsAttempted==4 && R.ReadsCompleted==4 &&
     !R.FailedOffset && R.LeaseEnded && !O.ActiveUses);
   C(Out.BaseLow==Values[0] && Out.EndLow==Values[1] && Out.BaseHigh==Values[2] &&
     Out.EndHigh==Values[3] && Out.MatchMask==Match);
  }
 }
 const int32_t Errors[]={-73,1,INT32_MIN,INT32_MAX};
 for(Uses=0;Uses<3;Uses++)for(Fail=1;Fail<=4;Fail++)for(unsigned E=0;E<4;E++)for(Raise=0;Raise<2;Raise++){
  O.ActiveUses=Uses;Error=Errors[E];Irql=0;Calls=0;
  memset(&Out,0xa5,sizeof Out);Old=Out;
  R=T70AfeObserveMappedAddress(&O,&P,&Out);
  C(Calls==Fail && R.EndAttempted && R.ReadStatus==Error);
  C(R.ReadsAttempted==Fail && R.ReadsCompleted==Fail-1 && R.FailedOffset==Off[Fail-1]);
  C(R.Code==(Raise?T70AddressObserveEndFailed:T70AddressObserveReadFailed));
  C(O.ActiveUses==Uses+(Raise?1:0) && R.LeaseEnded==!Raise);
  C(!memcmp(&Out,&Old,sizeof Out));
  if(Raise){Irql=0;C(!T70AfeMappingEnd(&O));}
 }
 Uses=0;O.ActiveUses=0;Raise=1;Fail=0;Error=0;Irql=0;Calls=0;
 R=T70AfeObserveMappedAddress(&O,&P,&Out);
 C(R.Code==T70AddressObserveEndFailed && R.ReadsCompleted==4 && !R.FailedOffset && O.ActiveUses==1);
 C(!memcmp(&Out,&Old,sizeof Out));Irql=0;C(!T70AfeMappingEnd(&O));Raise=0;Calls=0;
#define BAD(A,B,D) do{R=T70AfeObserveMappedAddress(A,B,D);C(R.Code==T70AddressObserveInvalid && !Calls);C(!memcmp(&Out,&Old,sizeof Out));}while(0)
 BAD(NULL,&P,&Out);BAD(&O,NULL,&Out);BAD(&O,&P,NULL);
 BAD(&O,&P,(void *)&O);BAD(&O,(void *)Ram,&Out);BAD(&O,&P,(void *)Ram);
 BAD(&O,&P,(void *)&P);BAD(&O,(void *)(UINTPTR_MAX-7),&Out);
 P.Geometry.EndLow^=1;BAD(&O,&P,&Out);P.Geometry.EndLow^=1;
 Irql=3;R=T70AfeObserveMappedAddress(&O,&P,&Out);
 C(R.Code==T70AddressObserveBeginFailed && !Calls && !R.EndAttempted);
 C(!memcmp(&Out,&Old,sizeof Out));Irql=0;
 O.ActiveUses=UINT32_MAX;R=T70AfeObserveMappedAddress(&O,&P,&Out);
 C(R.Code==T70AddressObserveBeginFailed && !Calls && !R.EndAttempted);
 C(!memcmp(&Out,&Old,sizeof Out));
 printf("AddressMappedObserve: %u checks; fake mapped reader only\n",Checks);return 0;
}
