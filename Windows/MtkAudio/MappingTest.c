// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeMapping.h"
#include "ConfigureMapped.h"
#include "RouteMapped.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks,Maps,Unmaps;static unsigned char Irql;
static ULONG Ram[2048];static void *MapResult=Ram,*Last;
#define C(X) do{assert(X);++Checks;}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG Flags){
 C(P.QuadPart==0x11210000 && N==0x2000 && Flags==(PAGE_READWRITE|PAGE_NOCACHE));
 ++Maps;Last=MapResult;return MapResult;
}
void MmUnmapIoSpace(void *V,size_t N){C(V==Last && N==0x2000);++Unmaps;}
ULONG READ_REGISTER_ULONG(volatile ULONG *P){C(P>=Ram && P<Ram+2048);return *P;}
void WRITE_REGISTER_ULONG(volatile ULONG *P,ULONG V){C(P>=Ram && P<Ram+2048);*P=V;}
int main(void){
 T70_AFE_MAPPING_OWNER O={0},Before;C(!T70AfeMappingRelease(&O));C(!Unmaps);
 C(T70AfeMappingBegin(&O)!=0);C(T70AfeMappingEnd(&O)!=0);
 const uint64_t BadBases[]={0,0x11210001,0x11212000,UINT64_MAX};
 for(unsigned I=0;I<4;I++)C(T70AfeMappingAcquire(&O,BadBases[I],0x2000)!=0);
 const size_t BadSizes[]={0,1,0x1fff,0x2001,SIZE_MAX};
 for(unsigned I=0;I<5;I++)C(T70AfeMappingAcquire(&O,0x11210000,BadSizes[I])!=0);
 C(!Maps && !Unmaps);
 MapResult=NULL;Before=O;C(T70AfeMappingAcquire(&O,0x11210000,0x2000)==STATUS_INSUFFICIENT_RESOURCES);C(!memcmp(&O,&Before,sizeof O));C(Maps==1 && !Unmaps);
 void *BadMappings[]={(char *)Ram+1,(void *)(UINTPTR_MAX-3),&O};
 for(unsigned I=0;I<3;I++){
 MapResult=BadMappings[I];unsigned U=Unmaps;
 C(T70AfeMappingAcquire(&O,0x11210000,0x2000)!=0);C(Unmaps==U+1);C(!memcmp(&O,&Before,sizeof O));
 }
 MapResult=Ram;
 for(Irql=0;Irql<=DISPATCH_LEVEL;Irql++){
 C(!T70AfeMappingAcquire(&O,0x11210000,0x2000));C(O.Mapping.VirtualBase==Ram);
 T70_AFE_PREPARED Prepared;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x100000000ULL,512,256,&Prepared));
 T70_MAPPED_CONFIG_RESULT Config=T70AfeConfigureMapped(&O,&Prepared);
 C(Config.Code==T70MappedConfigCallbacksCompleted && Config.LeaseEnded && O.ActiveUses==0);
 Ram[0x20/4]=32;Ram[0x24/4]=64;T70_AFE_ROUTE_OBSERVATION Observation;
 T70_MAPPED_ROUTE_RESULT Observed=T70AfeObserveMappedRoute(&O,&Observation);
 C(Observed.Code==T70MappedRouteObserved && Observed.LeaseEnded && O.ActiveUses==0);
 C(Observation.Dl1Channel1Selected==1 && Observation.Dl1Channel2Selected==1);
 unsigned M=Maps,U=Unmaps;Before=O;
 C(T70AfeMappingAcquire(&O,0x11210000,0x2000)!=0);C(Maps==M && !memcmp(&O,&Before,sizeof O));
 C(!T70AfeMappingBegin(&O));C(!T70AfeMappingBegin(&O));Before=O;
 C(!T70AfeMappedWrite32(&O.Mapping,0x4c,0x12345678));uint32_t Value=0;
 C(!T70AfeMappedRead32(&O.Mapping,0x4c,&Value));C(Value==0x12345678);
 C(T70AfeMappingRelease(&O)!=0);C(Unmaps==U && !memcmp(&O,&Before,sizeof O));
 C(!T70AfeMappingEnd(&O));C(T70AfeMappingRelease(&O)!=0);C(!T70AfeMappingEnd(&O));
 C(T70AfeMappingEnd(&O)!=0);C(!T70AfeMappingRelease(&O));C(Unmaps==U+1);
 C(!T70AfeMappingRelease(&O));C(Unmaps==U+1 && O.Mapping.VirtualBase==NULL);
 C(T70AfeMappedRead32(&O.Mapping,0x4c,&Value)!=0);
 }
 Irql=0;C(!T70AfeMappingAcquire(&O,0x11210000,0x2000));
 O.ActiveUses=UINT32_MAX;Before=O;C(T70AfeMappingBegin(&O)!=0);C(!memcmp(&O,&Before,sizeof O));O.ActiveUses=0;
 Before=O;Irql=DISPATCH_LEVEL+1;unsigned M=Maps,U=Unmaps;
 C(T70AfeMappingAcquire(&O,0x11210000,0x2000)!=0);C(T70AfeMappingBegin(&O)!=0);C(T70AfeMappingEnd(&O)!=0);C(T70AfeMappingRelease(&O)!=0);
 C(Maps==M && Unmaps==U && !memcmp(&O,&Before,sizeof O));
 Irql=0;C(!T70AfeMappingRelease(&O));
 C(T70AfeMappingAcquire(NULL,0x11210000,0x2000)!=0);C(T70AfeMappingBegin(NULL)!=0);C(T70AfeMappingEnd(NULL)!=0);C(T70AfeMappingRelease(NULL)!=0);
 O.Mapping.Length=1;Before=O;M=Maps;U=Unmaps;C(T70AfeMappingAcquire(&O,0x11210000,0x2000)!=0);C(T70AfeMappingRelease(&O)!=0);C(Maps==M && Unmaps==U && !memcmp(&O,&Before,sizeof O));
 printf("MappingTest: %u checks; fake map/unmap only\n",Checks);
}
