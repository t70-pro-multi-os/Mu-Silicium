// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtBufferedStreamBase.h"
#include <type_traits>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
static unsigned Checks,Irql,Calls,Fail,Raise,Freed;
static ULONG ExpectedBytes=4096;
static ULONG Ram[2048];static T70_WAVERT_DMA_OWNER *Dma;static T70_AFE_MAPPING_OWNER *Map;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS,size_t,ULONG){return Ram;}
void MmUnmapIoSpace(void *,size_t){}
static void Free(PDMA_ADAPTER,ULONG,PHYSICAL_ADDRESS,void *,BOOLEAN){Freed++;}
static int32_t Step(void *Ctx,uint32_t Off,bool Write,uint32_t V){
 C(Ctx==&Map->Mapping&&Map->ActiveUses==1&&Dma->ActiveUses==1&&Dma->Backing->Uses==1);
 unsigned I=Calls++;const unsigned O[]={0x54,0x64,0x50,0x60},Values[]={0,ExpectedBytes-1,1,1};
 C(I<4?(Write&&Off==O[I]&&V==Values[I]):(Off==0x4c&&Write==(I%2)));
 if(Raise&&(Calls==Fail||Calls==12))Irql=3;
 return Calls==Fail?-73:0;
}
extern "C" int32_t T70AfeMappedRead32(void *Ctx,uint32_t O,uint32_t *V){*V=0;return Step(Ctx,O,false,0);}
extern "C" int32_t T70AfeMappedWrite32(void *Ctx,uint32_t O,uint32_t V){return Step(Ctx,O,true,V);}
class FakePort final:public IPortWaveRTStream{
public:ULONG Refs=1,Frees=0;bool AllocFail=false,MapFail=false;MDL M={};alignas(PAGE_SIZE) unsigned char Bytes[196608];
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 PMDL AllocateContiguousPagesForMdl(PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,size_t N)override{C(N==ExpectedBytes);if(AllocFail)return nullptr;M.ByteCount=(ULONG)N;return &M;}
 void *MapAllocatedPages(PMDL,MEMORY_CACHING_TYPE T)override{C(T==MmCached);return MapFail?nullptr:Bytes;}
 void UnmapAllocatedPages(void *V,PMDL P)override{C(V==Bytes&&P==&M);}
 void FreePagesFromMdl(PMDL P)override{C(P==&M);Frees++;}
 ULONG GetPhysicalPagesCount(PMDL)override{return M.ByteCount/PAGE_SIZE;}
};
static NTSTATUS CreateStatus;
static NTSTATUS Create(PDMA_ADAPTER,PMDL,DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION *L,ULONG Count,PHYSICAL_ADDRESS *Out){
 C(Count==1&&L->LogicalAddressLimits.MaximumAddress.QuadPart==0x3ffffffffLL);Out->QuadPart=0x100000000LL;return CreateStatus;
}
static_assert(std::is_abstract<T70WaveRtBufferedStreamBase>::value,"real providers required");
class Stream final:public T70WaveRtBufferedStreamBase{
public:
 unsigned Pins=0,PolicyCalls=0,RecoveryCalls=0;NTSTATUS PinStatus=0,PolicyStatus=0,RecoveryStatus=STATUS_NOT_SUPPORTED;
 Stream(const T70_AFE_PREPARED &P,T70_WAVERT_BUFFER_TRANSACTION *T,IPortWaveRTStream *Port,PDMA_ADAPTER A,T70_AFE_MAPPING_OWNER *M):T70WaveRtBufferedStreamBase(P,T,Port,A,M){}
 NTSTATUS QueryInterface(const GUID &,void **)override{return STATUS_NOT_SUPPORTED;}
 ULONG AddRef()override{return 1;}ULONG Release()override{return 1;}
 void GetHWLatency(KSRTAUDIO_HWLATENCY *)override{assert(false);}
 NTSTATUS PinRecoveryLifetime()override{if(PinStatus==STATUS_SUCCESS)Pins++;return PinStatus;}
 void UnpinRecoveryLifetime()override{C(Pins==1);Pins--;}
 NTSTATUS VerifyClientMapping(const T70_WAVERT_DMA_OWNER &D)override{C(D.Mapped);PolicyCalls++;return PolicyStatus;}
 NTSTATUS RemoveHardwareReferences()override{RecoveryCalls++;return RecoveryStatus;}
};
int main(){
 T70_AFE_PREPARED Plan;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,4096,1024,&Plan));
 DMA_OPERATIONS Ops={};Ops.Size=sizeof Ops;Ops.FreeCommonBuffer=Free;Ops.CreateCommonBufferFromMdl=Create;DMA_ADAPTER A={1,sizeof A,&Ops};
 for(unsigned Test=0;Test<20;Test++){
  static FakePort Port;Port.AllocFail=Test==1;Port.MapFail=Test==2;Port.Frees=0;
  T70_WAVERT_BUFFER_TRANSACTION T={};T70_AFE_MAPPING_OWNER M={};C(T70AfeMappingAcquire(&M,0x11210000,8192)==0);Map=&M;Dma=&T.Dma;
  Stream S(Plan,&T,&Port,&A,&M);IMiniportWaveRTStream *I=&S;
  S.PinStatus=Test==0?STATUS_INSUFFICIENT_RESOURCES:0;S.PolicyStatus=Test==4?STATUS_NOT_SUPPORTED:0;
  CreateStatus=Test==3?STATUS_INSUFFICIENT_RESOURCES:0;Calls=0;Raise=Test==18;Fail=(Test>=5&&Test<=16)?Test-4:0;
  PMDL Out=(PMDL)(uintptr_t)0x55;ULONG Size=55,Offset=66;MEMORY_CACHING_TYPE Cache=MmNonCached;
  NTSTATUS Status=I->AllocateAudioBuffer(4096,&Out,&Size,&Offset,&Cache);Irql=0;
  if(Test<=4){
   C(!NT_SUCCESS(Status)&&Calls==0&&!T.LifetimeHeld&&!S.Pins&&Port.Refs==1);
   C(Out==(PMDL)(uintptr_t)0x55&&Size==55&&Offset==66&&Cache==MmNonCached);
  }else if(Test<=16||Test==18){
   C(!NT_SUCCESS(Status)&&T.LifetimeHeld&&S.Pins==1&&T.Dma.ActiveUses==1);
   C(Out==(PMDL)(uintptr_t)0x55&&Size==55&&Offset==66&&Cache==MmNonCached);
   C(S.Recover()==STATUS_NOT_SUPPORTED&&T.LifetimeHeld);
   S.RecoveryStatus=0;C(S.Recover()==0&&!T.LifetimeHeld&&Port.Refs==1&&!S.Pins);
  }else{
   C(Status==0&&Out==T.Pages.Mdl&&Size==4096&&Offset==0&&Cache==MmCached&&Calls==12);
   C(S.Recover()==STATUS_INVALID_DEVICE_STATE&&S.RecoveryCalls==0&&T.LastStatus==STATUS_INVALID_DEVICE_STATE); // PortCls has not freed it.
   I->FreeAudioBuffer(Out,Size-1);C(T.LastStatus==STATUS_INVALID_PARAMETER&&T.LifetimeHeld&&!T.FreeRequested);
   I->FreeAudioBuffer(Out,Size);C(T.LastStatus==STATUS_NOT_SUPPORTED&&T.LifetimeHeld&&T.FreeRequested&&S.Pins==1);
   C(T70WaveRtDmaRelease(&T.Dma)==STATUS_INVALID_DEVICE_STATE);
   S.RecoveryStatus=0;C(S.Recover()==0&&!T.LifetimeHeld&&!S.Pins&&Port.Refs==1);
   I->FreeAudioBuffer(Out,Size);C(T.LastStatus==STATUS_INVALID_PARAMETER);
  }
  C(M.ActiveUses==0);C(T70AfeMappingRelease(&M)==0);C(I->SetState(KSSTATE_RUN)==STATUS_NOT_SUPPORTED);
  if(Test!=0)C(I->SetFormat(nullptr)==STATUS_INVALID_DEVICE_STATE);
  if(Test!=0)C(I->AllocateAudioBuffer(4096,&Out,&Size,&Offset,&Cache)==STATUS_INVALID_DEVICE_STATE);
 }
 // Pending/informational provider results are NOT completed safety proofs.
 for(NTSTATUS Pending:{(NTSTATUS)0x103,(NTSTATUS)1,(NTSTATUS)0x40000000}){
  for(unsigned Stage=0;Stage<3;Stage++){
   static FakePort P;T70_WAVERT_BUFFER_TRANSACTION Tx={};T70_AFE_MAPPING_OWNER Mapping={};
   C(T70AfeMappingAcquire(&Mapping,0x11210000,8192)==0);Map=&Mapping;Dma=&Tx.Dma;
   Stream St(Plan,&Tx,&P,&A,&Mapping);Calls=Fail=Raise=0;CreateStatus=0;
   if(Stage==0)St.PinStatus=Pending;if(Stage==1)St.PolicyStatus=Pending;
   PMDL Result=(PMDL)(uintptr_t)0x55;ULONG Bytes=55,Off=66;MEMORY_CACHING_TYPE CacheType=MmNonCached;
   NTSTATUS Status=St.AllocateAudioBuffer(4096,&Result,&Bytes,&Off,&CacheType);
   if(Stage<2){
    C(Status==STATUS_DEVICE_NOT_READY&&Calls==0&&!Tx.LifetimeHeld&&!St.Pins&&P.Refs==1);
    C(Result==(PMDL)(uintptr_t)0x55&&Bytes==55&&Off==66&&CacheType==MmNonCached);
   }else{
    C(Status==0);St.RecoveryStatus=Pending;unsigned Before=Freed;
    St.FreeAudioBuffer(Result,Bytes);
    C(Tx.LastStatus==STATUS_DEVICE_NOT_READY&&Tx.LifetimeHeld&&!Tx.HardwareReleased&&Tx.Dma.ActiveUses==1&&Freed==Before&&St.Pins==1);
    St.RecoveryStatus=0;C(St.Recover()==0&&!Tx.LifetimeHeld&&Freed==Before+1&&!St.Pins);
   }
   C(T70AfeMappingRelease(&Mapping)==0);
  }
 }
 static FakePort Port;T70_WAVERT_BUFFER_TRANSACTION T={};T70_AFE_MAPPING_OWNER M={};Stream S(Plan,&T,&Port,&A,&M);
 PMDL Out=nullptr;ULONG N=0;MEMORY_CACHING_TYPE Cache=MmCached;
 ULONG Offset=0;
 C(S.AllocateAudioBuffer(4096,nullptr,&N,&Offset,&Cache)==STATUS_INVALID_PARAMETER);
 C(S.AllocateAudioBuffer(4096,&Out,nullptr,&Offset,&Cache)==STATUS_INVALID_PARAMETER);
 C(S.AllocateAudioBuffer(4096,&Out,&N,nullptr,&Cache)==STATUS_INVALID_PARAMETER);
 C(S.AllocateAudioBuffer(4096,&Out,&N,&Offset,nullptr)==STATUS_INVALID_PARAMETER);C(!S.Pins);
 C(S.AllocateAudioBuffer(4096,&Out,&N,&N,&Cache)==STATUS_INVALID_PARAMETER);C(!S.Pins);
 C(S.AllocateAudioBuffer(4095,&Out,&N,&N,&Cache)==STATUS_INVALID_PARAMETER);C(!S.Pins);
 // Every sub-page request is rounded without truncation. Successful fake
 // providers prove orchestration only, not real platform cache suitability.
 for(ULONG Request=1;Request<=4096;Request++){
  T70_WAVERT_BUFFER_TRANSACTION Tx={};T70_AFE_MAPPING_OWNER Mapping={};
  C(T70AfeMappingAcquire(&Mapping,0x11210000,8192)==0);Map=&Mapping;Dma=&Tx.Dma;
  Stream St(Plan,&Tx,&Port,&A,&Mapping);St.RecoveryStatus=0;
  Calls=Fail=Raise=0;CreateStatus=0;ExpectedBytes=4096;
  ULONG Actual=0,Off=99;PMDL Result=nullptr;
  C(St.AllocateAudioBuffer(Request,&Result,&Actual,&Off,&Cache)==0);
  C(Actual==4096&&Actual>=Request&&Off==0&&Calls==12);
  St.FreeAudioBuffer(Result,Actual);C(!Tx.LifetimeHeld&&Port.Refs==1);
  C(T70AfeMappingRelease(&Mapping)==0);
 }
 // Non-power-of-two period requires the first common page/period boundary,
 // not merely the next page. At the cap, no qualifying size must fail pre-I/O.
 for(ULONG Request: {4097u,196608u,196609u,UINT32_MAX}){
  T70_AFE_PREPARED P;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,12288,1536,&P));
  T70_WAVERT_BUFFER_TRANSACTION Tx={};T70_AFE_MAPPING_OWNER Mapping={};
  C(T70AfeMappingAcquire(&Mapping,0x11210000,8192)==0);Map=&Mapping;Dma=&Tx.Dma;
  Stream St(P,&Tx,&Port,&A,&Mapping);St.RecoveryStatus=0;
  Calls=Fail=Raise=0;ExpectedBytes=Request==4097?12288:196608;
  ULONG Actual=55,Off=66;PMDL Result=(PMDL)(uintptr_t)0x55;Cache=MmNonCached;
  NTSTATUS Status=St.AllocateAudioBuffer(Request,&Result,&Actual,&Off,&Cache);
  if(Request<=196608){
   C(Status==0&&Actual==ExpectedBytes&&Actual>=Request&&Calls==12);
   St.FreeAudioBuffer(Result,Actual);C(!Tx.LifetimeHeld);
  }else C(!NT_SUCCESS(Status)&&!St.Pins&&!Calls&&Actual==55&&Off==66&&Cache==MmNonCached&&Result==(PMDL)(uintptr_t)0x55);
  C(T70AfeMappingRelease(&Mapping)==0);
 }
 // A valid retained period may have no page-backed geometry within the cap.
 T70_AFE_PREPARED P;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,196576,98288,&P));
 T70_WAVERT_BUFFER_TRANSACTION Tx={};T70_AFE_MAPPING_OWNER Mapping={};
 Stream St(P,&Tx,&Port,&A,&Mapping);Calls=0;
 ULONG Actual=55,Off=66;PMDL Result=(PMDL)(uintptr_t)0x55;Cache=MmNonCached;
 C(St.AllocateAudioBuffer(1,&Result,&Actual,&Off,&Cache)==STATUS_INVALID_PARAMETER);
 C(!St.Pins&&!Calls&&Actual==55&&Off==66&&Result==(PMDL)(uintptr_t)0x55&&Cache==MmNonCached);
 printf("WaveRT Allocate/Free: %u checks; abstract provider contract only\n",Checks);
}
