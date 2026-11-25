// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "RouteMapped.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static unsigned Checks,Calls,Fail,ExpectedUses,Raise,Maps,Unmaps;
static unsigned char Irql;static int32_t Error;
static ULONG Ram[2048];static uint32_t Values[2];static T70_AFE_MAPPING_OWNER *Current;
#define C(X) do{assert(X);++Checks;}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG Flags){C(P.QuadPart==0x11210000 && N==8192 && Flags==(PAGE_READWRITE|PAGE_NOCACHE));++Maps;return Ram;}
void MmUnmapIoSpace(void *P,size_t N){C(P==Ram && N==8192);++Unmaps;}
/* Inject adapter statuses, not exceptions or real kernel primitive failures. */
int32_t T70AfeMappedRead32(void *Ctx,uint32_t Off,uint32_t *V){
 C(Ctx==&Current->Mapping && Current->ActiveUses==ExpectedUses);
 C(Calls<2 && Off==0x20+Calls*4);*V=Values[Calls++];
 if(Raise && (Calls==Fail || Calls==2))Irql=3;
 return Calls==Fail?Error:0;
}
int main(void){
 T70_AFE_MAPPING_OWNER O={0};Current=&O;
 T70_AFE_ROUTE_OBSERVATION Out;unsigned char Before[sizeof Out];T70_MAPPED_ROUTE_RESULT R;
#define RESET() do{memset(&Out,0xa5,sizeof Out);memcpy(Before,&Out,sizeof Out);Calls=0;Fail=0;Raise=0;Irql=0;}while(0)
#define KEEP() C(!memcmp(Before,&Out,sizeof Out))
 RESET();R=T70AfeObserveMappedRoute(&O,&Out);C(R.Code==T70MappedRouteBeginFailed && !R.EndAttempted && !Calls);KEEP();
 C(!T70AfeMappingAcquire(&O,0x11210000,8192));
 for(unsigned Uses=0;Uses<=2;Uses++){
 O.ActiveUses=Uses;ExpectedUses=Uses+1;
 for(unsigned Bits=0;Bits<4;Bits++){
 RESET();Values[0]=Bits&1?32:0;Values[1]=Bits&2?64:0;
 R=T70AfeObserveMappedRoute(&O,&Out);
 C(R.Code==T70MappedRouteObserved && R.LeaseEnded && R.EndAttempted);
 C(Calls==2 && O.ActiveUses==Uses && Maps==1 && !Unmaps);
 C(Out.Conn0==Values[0] && Out.Conn1==Values[1]);
 C(Out.Dl1Channel1Selected==(Bits&1) && Out.Dl1Channel2Selected==((Bits>>1)&1));
 }
 const int32_t Errors[]={-73,1,INT32_MIN,INT32_MAX};
 for(unsigned F=1;F<=2;F++)for(unsigned E=0;E<4;E++){
 RESET();Fail=F;Error=Errors[E];R=T70AfeObserveMappedRoute(&O,&Out);
 C(R.Code==T70MappedRouteReadFailed && R.EndAttempted && R.LeaseEnded);
 C(R.Route.TransportStatus==Error && R.Route.ReadsAttempted==F && R.Route.FailedRead==F-1);
 C(Calls==F && O.ActiveUses==Uses);KEEP();
 }
 }
 O.ActiveUses=0;ExpectedUses=1;
 for(unsigned F=0;F<=2;F++){
 RESET();Raise=1;Fail=F;Error=-73;R=T70AfeObserveMappedRoute(&O,&Out);
 C(R.Code==T70MappedRouteEndFailed && R.EndAttempted && !R.LeaseEnded);
 C(O.ActiveUses==1 && R.EndStatus==STATUS_INVALID_DEVICE_STATE);
 C(Calls==(F?F:2));C(R.Route.Code==(F?T70AfeRouteReadFailed:T70AfeRouteObserved));KEEP();
 Irql=0;C(!T70AfeMappingEnd(&O));
 }
 RESET();R=T70AfeObserveMappedRoute(NULL,&Out);C(R.Code==T70MappedRouteInvalid && !Calls);KEEP();
 R=T70AfeObserveMappedRoute(&O,NULL);C(R.Code==T70MappedRouteInvalid && !Calls);
 R=T70AfeObserveMappedRoute(&O,(T70_AFE_ROUTE_OBSERVATION *)(void *)&O);C(R.Code==T70MappedRouteInvalid && !Calls);
 R=T70AfeObserveMappedRoute(&O,(T70_AFE_ROUTE_OBSERVATION *)(void *)Ram);C(R.Code==T70MappedRouteInvalid && !Calls);
 R=T70AfeObserveMappedRoute(&O,(T70_AFE_ROUTE_OBSERVATION *)(UINTPTR_MAX-3));C(R.Code==T70MappedRouteInvalid && !Calls);
 R=T70AfeObserveMappedRoute(&O,(T70_AFE_ROUTE_OBSERVATION *)(void *)((char *)&Out+1));C(R.Code==T70MappedRouteInvalid && !Calls);
 Irql=3;R=T70AfeObserveMappedRoute(&O,&Out);C(R.Code==T70MappedRouteBeginFailed && !R.ReadStarted && !Calls);KEEP();Irql=0;
 O.ActiveUses=UINT32_MAX;R=T70AfeObserveMappedRoute(&O,&Out);C(R.Code==T70MappedRouteBeginFailed && !Calls);KEEP();O.ActiveUses=0;
 C(!T70AfeMappingRelease(&O));C(Unmaps==1);R=T70AfeObserveMappedRoute(&O,&Out);C(R.Code==T70MappedRouteBeginFailed && !Calls);KEEP();
 printf("RouteMappedTest: %u checks; fake mapping/reads only\n",Checks);
}
