// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtStreamBase.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static unsigned Checks,Irql,Calls,Fail,Raise,Freed;
static ULONG Ram[2048];static T70_WAVERT_DMA_OWNER *Dma;static T70_AFE_MAPPING_OWNER *Map;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS,size_t,ULONG){return Ram;}
void MmUnmapIoSpace(void *,size_t){}
static void Free(PDMA_ADAPTER,ULONG,PHYSICAL_ADDRESS,void *,BOOLEAN){Freed++;}
static int32_t Step(void *Ctx,uint32_t Off,bool Write,uint32_t V){
 C(Ctx==&Map->Mapping&&Map->ActiveUses==1&&Dma->ActiveUses==1&&Dma->Backing->Uses==1);
 unsigned I=Calls++;const unsigned O[]={0x54,0x64,0x50,0x60},Values[]={0,4095,1,1};
 C(I<4?(Write&&Off==O[I]&&V==Values[I]):(Off==0x4c&&Write==(I%2)));
 if(Raise&&(Calls==Fail||Calls==12))Irql=3;
 return Calls==Fail?-73:0;
}
extern "C" int32_t T70AfeMappedRead32(void *Ctx,uint32_t O,uint32_t *V){*V=0;return Step(Ctx,O,false,0);}
extern "C" int32_t T70AfeMappedWrite32(void *Ctx,uint32_t O,uint32_t V){return Step(Ctx,O,true,V);}
class Fake final:public T70WaveRtStreamBase{
public:explicit Fake(const T70_AFE_PREPARED &P):T70WaveRtStreamBase(P){}
 NTSTATUS QueryInterface(const GUID &,void **)override{return STATUS_NOT_SUPPORTED;}
 ULONG AddRef()override{return 1;}ULONG Release()override{return 1;}
 NTSTATUS AllocateAudioBuffer(ULONG,PMDL *,ULONG *,ULONG *,MEMORY_CACHING_TYPE *)override{return STATUS_DEVICE_NOT_READY;}
 void FreeAudioBuffer(PMDL,ULONG)override{}void GetHWLatency(KSRTAUDIO_HWLATENCY *)override{assert(false);}
};
class FakePort final:public IPortWaveRTStream{
public:ULONG Refs=1,Frees=0;MDL M={};alignas(PAGE_SIZE) unsigned char Bytes[4096];
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 PMDL AllocateContiguousPagesForMdl(PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,size_t N)override{C(N==4096);M.ByteCount=4096;return &M;}
 void *MapAllocatedPages(PMDL,MEMORY_CACHING_TYPE T)override{C(T==MmCached);return Bytes;}
 void UnmapAllocatedPages(void *V,PMDL P)override{C(V==Bytes&&P==&M);}
 void FreePagesFromMdl(PMDL P)override{C(P==&M);Frees++;}
 ULONG GetPhysicalPagesCount(PMDL)override{return 1;}
};
static NTSTATUS Create(PDMA_ADAPTER,PMDL,DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION *L,ULONG Count,PHYSICAL_ADDRESS *Out){
 C(Count==1&&L->LogicalAddressLimits.MaximumAddress.QuadPart==0x3ffffffffLL);Out->QuadPart=0x100000000LL;return STATUS_SUCCESS;
}
int main(){
 T70_AFE_PREPARED Plan;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,4096,1024,&Plan));
 T70_WAVERT_MDL_OWNER Back={};Back.Uses=1;
 DMA_ADAPTER Adapter={};T70_WAVERT_DMA_OWNER D={};D.Backing=&Back;D.Adapter=&Adapter;D.Free=Free;D.LeaseHeld=D.Mapped=true;D.View.Bytes=4096;D.Logical.QuadPart=0x100000000LL;
 T70_AFE_MAPPING_OWNER M={};Dma=&D;Map=&M;C(T70AfeMappingAcquire(&M,0x11210000,8192)==0);
 for(unsigned X=0;X<2;X++)for(unsigned F=0;F<=12;F++){
  Fake Stream(Plan);Raise=X;Fail=F;Calls=0;
  auto R=Stream.ConfigureMapped(&D,&M);
  C(R.BeginAttempted&&R.BeginStatus==0&&R.DmaLeaseHeld&&!R.EndAttempted&&R.Mapped.ConfigureStarted);
  C(Calls==(F?F:12)&&D.ActiveUses==1&&M.ActiveUses==X);
  C(R.Mapped.Code==(X?T70MappedConfigEndFailed:F?T70MappedConfigConfigureFailed:T70MappedConfigCallbacksCompleted));
  Irql=0;C(T70WaveRtDmaRelease(&D)==STATUS_INVALID_DEVICE_STATE&&Freed==0);
  unsigned Before=Calls;C(!Stream.ConfigureMapped(&D,&M).BeginAttempted&&Calls==Before);
  alignas(KSDATAFORMAT) unsigned char Format[104]={};C(Stream.SetFormat((PKSDATAFORMAT)Format)==STATUS_INVALID_DEVICE_STATE);
  C(Stream.SetState(KSSTATE_RUN)==STATUS_NOT_SUPPORTED);
  T70_AFE_PREPARED Snapshot;C(Stream.CopyPlan(&Snapshot)&&Snapshot.DmaBase==0x100000000ULL);
  if(X)C(T70AfeMappingEnd(&M)==0);
  C(T70WaveRtDmaEndUse(&D)==0); // simulated external recovery only
  C(!Stream.ConfigureMapped(&D,&M).BeginAttempted); // even after counter recovery no retry
 }
 Calls=Raise=Fail=0;C(T70AfeMappingRelease(&M)==0);Fake MissingMap(Plan);
 auto R=MissingMap.ConfigureMapped(&D,&M);C(R.BeginAttempted&&R.EndAttempted&&!R.DmaLeaseHeld&&!R.Mapped.ConfigureStarted);C(!D.ActiveUses&&!Calls);
 D.Logical.QuadPart=0x400000000LL;Fake Bad(Plan);C(!Bad.ConfigureMapped(&D,&M).BeginAttempted&&!Calls);D.Logical.QuadPart=0x100000000LL;
 Irql=1;R=Bad.ConfigureMapped(&D,&M);C(R.BeginAttempted&&R.BeginStatus==STATUS_INVALID_PARAMETER&&!D.ActiveUses);Irql=0;
 D.ActiveUses=UINT32_MAX;C(T70WaveRtDmaBeginUse(&D)==STATUS_INVALID_DEVICE_STATE);D.ActiveUses=0;
 C(T70WaveRtDmaEndUse(&D)==STATUS_INVALID_DEVICE_STATE);C(!Bad.ConfigureMapped(nullptr,&M).BeginAttempted);
 // A failure before callbacks is retryable once the missing AFE mapping exists.
 C(T70AfeMappingAcquire(&M,0x11210000,8192)==0);Calls=0;
 R=MissingMap.ConfigureMapped(&D,&M);C(R.Mapped.ConfigureStarted&&Calls==12&&D.ActiveUses==1);
 C(T70WaveRtDmaEndUse(&D)==0);C(T70AfeMappingRelease(&M)==0);
 // Full fake-provider allocation -> logical mapping -> configure -> release chain.
 static FakePort Port;T70_WAVERT_MDL_OWNER B2={};T70_WAVERT_DMA_OWNER D2={};T70_AFE_MAPPING_OWNER M2={};
 DMA_OPERATIONS Ops={};Ops.Size=sizeof Ops;Ops.FreeCommonBuffer=Free;Ops.CreateCommonBufferFromMdl=Create;
 DMA_ADAPTER A2={1,sizeof A2,&Ops};
 C(T70WaveRtMdlAcquire(&B2,&Port,4096)==0);C(T70WaveRtDmaAcquire(&D2,&B2,&A2)==0);
 C(T70AfeMappingAcquire(&M2,0x11210000,8192)==0);Dma=&D2;Map=&M2;Calls=0;
 Fake Integrated(Plan);R=Integrated.ConfigureMapped(&D2,&M2);C(R.Mapped.ConfigureStarted&&Calls==12&&R.DmaLeaseHeld);
 C(T70WaveRtDmaRelease(&D2)==STATUS_INVALID_DEVICE_STATE);C(T70WaveRtMdlRelease(&B2)==STATUS_INVALID_DEVICE_STATE);
 C(T70WaveRtDmaEndUse(&D2)==0);C(T70WaveRtDmaRelease(&D2)==0);C(T70WaveRtMdlRelease(&B2)==0);
 C(Port.Refs==1&&Port.Frees==1);C(T70AfeMappingRelease(&M2)==0);
 printf("WaveRT configure: %u checks; mock callbacks, retained DMA use\n",Checks);
}
