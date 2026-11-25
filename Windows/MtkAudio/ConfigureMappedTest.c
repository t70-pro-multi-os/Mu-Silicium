// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "ConfigureMapped.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks,Calls,Fail,Maps,Unmaps,ExpectedUses=1,RaiseIrql;
static unsigned char Irql;static int32_t Error;static ULONG Ram[2048];
static T70_AFE_MAPPING_OWNER *Current;static int MapFails;
#define C(X) do{assert(X);++Checks;}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG F){
 C(P.QuadPart==0x11210000 && N==8192 && F==(PAGE_NOCACHE|PAGE_READWRITE));++Maps;return MapFails?NULL:Ram;
}
void MmUnmapIoSpace(void *V,size_t N){C(V==Ram && N==8192);++Unmaps;}
/* Substitute adapter calls to inject status failures. Native register primitives
 * do not return these statuses; this does NOT model catching hardware faults. */
static int32_t Step(void *Ctx,uint32_t Offset,int Write){
 C(Ctx==&Current->Mapping && Current->ActiveUses==ExpectedUses);
 unsigned N=++Calls;C(N<=12);
 const uint32_t Address[]={0x54,0x64,0x50,0x60};
 C(N<=4?(Write && Offset==Address[N-1]):(Offset==0x4c && Write==(N%2==0)));
 if(RaiseIrql && (N==Fail || N==12))Irql=DISPATCH_LEVEL+1;
 return N==Fail?Error:0;
}
int32_t T70AfeMappedRead32(void *Ctx,uint32_t Off,uint32_t *V){*V=0xa5a5a5a5;return Step(Ctx,Off,0);}
int32_t T70AfeMappedWrite32(void *Ctx,uint32_t Off,uint32_t V){(void)V;return Step(Ctx,Off,1);}
int main(void){
 T70_AFE_MAPPING_OWNER O={0};Current=&O;T70_AFE_PREPARED P,Before;
 C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x100000000ULL,512,256,&P));Before=P;
 T70_MAPPED_CONFIG_RESULT R;
 MapFails=1;C(T70AfeMappingAcquire(&O,0x11210000,8192)!=0);
 R=T70AfeConfigureMapped(&O,&P);C(R.Code==T70MappedConfigBeginFailed && !Calls && !R.EndAttempted);
 MapFails=0;C(!T70AfeMappingAcquire(&O,0x11210000,8192));unsigned M=Maps,U=Unmaps;
 const int32_t Errors[]={-73,1,INT32_MIN,INT32_MAX};
 for(unsigned Uses=0;Uses<=2;Uses++){
 O.ActiveUses=Uses;ExpectedUses=Uses+1;
 for(unsigned F=0;F<=12;F++)for(unsigned E=0;E<4;E++){
 Calls=0;Fail=F;Error=Errors[E];R=T70AfeConfigureMapped(&O,&P);
 C(R.BeginAttempted && R.ConfigureStarted && R.EndAttempted && R.LeaseEnded);
 C(!R.BeginStatus && !R.EndStatus && O.ActiveUses==Uses);
 C(Calls==(F?F:12));C(Maps==M && Unmaps==U);C(!memcmp(&Before,&P,sizeof P));
 C(R.Code==(F?T70MappedConfigConfigureFailed:T70MappedConfigCallbacksCompleted));
 if(F)C(F<=4?R.Configure.Address.TransportStatus==Error:R.Configure.Format.TransportStatus==Error);
 }
 }
 O.ActiveUses=0;ExpectedUses=1;Calls=0;Fail=0;
 P.Format.RateCode^=1;R=T70AfeConfigureMapped(&O,&P);C(R.Code==T70MappedConfigInvalid && !R.BeginAttempted && !Calls);P=Before;
 R=T70AfeConfigureMapped(NULL,&P);C(R.Code==T70MappedConfigInvalid);
 R=T70AfeConfigureMapped(&O,NULL);C(R.Code==T70MappedConfigInvalid);
 O.ActiveUses=UINT32_MAX;R=T70AfeConfigureMapped(&O,&P);C(R.Code==T70MappedConfigBeginFailed && !R.EndAttempted && !Calls);O.ActiveUses=0;
 Irql=DISPATCH_LEVEL+1;R=T70AfeConfigureMapped(&O,&P);C(R.Code==T70MappedConfigBeginFailed && !R.ConfigureStarted && !Calls);Irql=0;
 /* Deliberate provider contract violation: End cannot run at raised IRQL.
  * Report unresolved lease, preserve both outcomes, never pretend cleanup. */
 RaiseIrql=1;
 for(unsigned F=0;F<=12;F++){
 Calls=0;Fail=F;Error=-73;R=T70AfeConfigureMapped(&O,&P);
 C(R.Code==T70MappedConfigEndFailed && R.EndAttempted && !R.LeaseEnded);
 C(R.EndStatus==STATUS_INVALID_DEVICE_STATE && O.ActiveUses==1);
 C(Calls==(F?F:12));C(R.Configure.Code==(F?(F<=4?T70AfeConfigAddressFailed:T70AfeConfigFormatFailed):T70AfeConfigCallbacksCompleted));
 Irql=0;C(!T70AfeMappingEnd(&O));
 }
 RaiseIrql=0;C(!T70AfeMappingRelease(&O));C(Maps==M && Unmaps==U+1);
 printf("ConfigureMappedTest: %u checks; fake adapter/map calls only\n",Checks);
}
