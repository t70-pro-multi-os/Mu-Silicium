// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioResourceOwner.h"
#include "WaveRtMiniport.h"
#include "WaveRtDmaOwner.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
static std::atomic<unsigned> Checks,Allocated,Freed;
static unsigned Irql,Maps,Unmaps,Confirms;static bool FailMap,FailAlloc;
static NTSTATUS ConfirmStatus;alignas(8) static unsigned char Ram[8192];
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
void *ExAllocatePool2(uint64_t F,size_t N,ULONG T){C(F==POOL_FLAG_NON_PAGED&&(T==0x4f413754||T==0x4d573754));if(FailAlloc)return nullptr;Allocated++;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG T){C(T==0x4f413754||T==0x4d573754);Freed++;free(P);}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG F){C(P.QuadPart==0x11210000&&N==8192&&F==(PAGE_READWRITE|PAGE_NOCACHE));Maps++;return FailMap?nullptr:Ram;}
void MmUnmapIoSpace(void *P,size_t N){C(P==Ram&&N==8192);Unmaps++;}
class Resources:public IResourceList{
public:ULONG Refs=1;CM_PARTIAL_RESOURCE_DESCRIPTOR R={};Resources(){R.Type=CmResourceTypeMemory;R.u.Memory.Start.QuadPart=0x11210000;R.u.Memory.Length=8192;}
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 ULONG NumberOfEntriesOfType(CM_RESOURCE_TYPE T)override{C(T==CmResourceTypeMemory);return 1;}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindTranslatedEntry(CM_RESOURCE_TYPE T,ULONG I)override{C(T==CmResourceTypeMemory&&!I);return &R;}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindUntranslatedEntry(CM_RESOURCE_TYPE T,ULONG I)override{C(T==CmResourceTypeMemory&&!I);return &R;}
};
static NTSTATUS NoStream(void *,PPORTWAVERTSTREAM,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *){return STATUS_DEVICE_NOT_READY;}
static NTSTATUS Confirm(void *Ctxt){C(Ctxt==Ram);Confirms++;return ConfirmStatus;}
// Failure-only integration fixture, NOT a production stream factory. It
// composes real ownership APIs, then refuses publication at the cache barrier.
static unsigned AdmissionCase,DmaLive,DmaFrees,AdmissionCalls;
static NTSTATUS MapMdl(PDMA_ADAPTER,PMDL,DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION *,ULONG,PHYSICAL_ADDRESS *P){
 if(AdmissionCase==3)return STATUS_INSUFFICIENT_RESOURCES;
 DmaLive++;P->QuadPart=0x100000000LL;return STATUS_SUCCESS;
}
static void FreeDma(PDMA_ADAPTER,ULONG,PHYSICAL_ADDRESS,void *,BOOLEAN){C(DmaLive==1);DmaLive--;DmaFrees++;}
class PortStream:public IPortWaveRTStream{
public:ULONG Refs=1,Frees=0;bool Mapped=false;MDL M={};alignas(PAGE_SIZE) unsigned char Bytes[4096];
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 PMDL AllocateContiguousPagesForMdl(PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,size_t N)override{C(N==4096);if(AdmissionCase==1)return nullptr;M.ByteCount=4096;return &M;}
 void *MapAllocatedPages(PMDL,MEMORY_CACHING_TYPE Caching)override{C(Caching==MmCached);if(AdmissionCase==2)return nullptr;Mapped=true;return Bytes;}
 void UnmapAllocatedPages(void *,PMDL)override{C(!DmaLive&&Mapped);Mapped=false;}
 void FreePagesFromMdl(PMDL)override{C(!DmaLive&&!Mapped);Frees++;}
 ULONG GetPhysicalPagesCount(PMDL)override{return 1;}
};
static NTSTATUS RejectAfterOwnership(void *Context,PPORTWAVERTSTREAM Port,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *){
 AdmissionCalls++;
 auto Owner=static_cast<T70AudioResourceOwner *>(Context);T70_AUDIO_RESOURCE_USE Use={};T70_AFE_MAPPING_OWNER *Afe=nullptr;
 NTSTATUS Status=Owner->BeginUse(&Use,&Afe);if(!NT_SUCCESS(Status))return Status;
 C(Afe&&Afe->Mapping.VirtualBase==Ram);
 T70_WAVERT_MDL_OWNER Pages={};T70_WAVERT_DMA_OWNER Dma={};
 DMA_OPERATIONS Ops={};Ops.Size=sizeof Ops;Ops.FreeCommonBuffer=FreeDma;Ops.CreateCommonBufferFromMdl=MapMdl;
 DMA_ADAPTER Adapter={1,sizeof Adapter,&Ops};
 Status=T70WaveRtMdlAcquire(&Pages,Port,4096);
 if(NT_SUCCESS(Status))Status=T70WaveRtDmaAcquire(&Dma,&Pages,&Adapter);
 // No cache proof, no hardware configuration or DMA use has occurred. Thus
 // unmap/free below is safe in this fixture, unlike partial DL1 configuration.
 if(NT_SUCCESS(Status))Status=STATUS_DEVICE_NOT_READY;
 C(T70WaveRtDmaRelease(&Dma)==0);C(T70WaveRtMdlRelease(&Pages)==0);C(Owner->EndUse(&Use)==0);
 return Status;
}
int main(){
 T70AudioResourceOwner *O=(T70AudioResourceOwner *)(uintptr_t)0x55;FailAlloc=true;
 C(T70AudioResourceOwner::Create(0,&O)==STATUS_INSUFFICIENT_RESOURCES&&O==(T70AudioResourceOwner *)(uintptr_t)0x55);FailAlloc=false;
 C(T70AudioResourceOwner::Create(0,&O)==0);
 GUID Id={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};void *Unknown=nullptr;
 C(O->QueryInterface(Id,&Unknown)==0&&Unknown==static_cast<IUnknown *>(O));C(static_cast<IUnknown *>(Unknown)->Release()==1);
 T70_AUDIO_RESOURCE_USE Use={};T70_AFE_MAPPING_OWNER *Afe=(T70_AFE_MAPPING_OWNER *)(uintptr_t)0x55;
 C(O->BeginUse(&Use,&Afe)==STATUS_INVALID_DEVICE_STATE&&!Use.Owner&&Afe==(T70_AFE_MAPPING_OWNER *)(uintptr_t)0x55);
 Resources R;unsigned char Port=0;
 T70_WAVERT_MINIPORT_PROVIDER P={O,T70AudioResourceOwner::Initialize,NoStream};T70WaveRtMiniport *M=nullptr;
 C(T70WaveRtMiniport::Create(O,P,&M)==0);C(M->Init(nullptr,&R,(PPORTWAVERT)&Port)==0&&R.Refs==2&&Maps==1);
 C(O->BeginUse(&Use,&Afe)==0&&Use.Owner==O&&Afe->Mapping.VirtualBase==Ram);
 C(O->BeginUse(&Use,&Afe)==STATUS_INVALID_DEVICE_STATE);
 C(M->Release()==0&&Freed==1&&R.Refs==2&&!Unmaps); // Miniport gone, lease/self-pin remain.
 C(O->Close(Confirm,Ram)==STATUS_INVALID_DEVICE_STATE&&!Confirms&&!Unmaps);
 T70_AUDIO_RESOURCE_USE Other={};T70_AFE_MAPPING_OWNER *Out=nullptr;
 C(O->BeginUse(&Other,&Out)==STATUS_INVALID_DEVICE_STATE&&!Other.Owner&&!Out);
 C(O->EndUse(&Use)==0&&!Use.Owner);C(O->EndUse(&Use)==STATUS_INVALID_DEVICE_STATE);
 C(O->Close(nullptr,nullptr)==STATUS_DEVICE_NOT_READY&&!Unmaps&&R.Refs==2);
 ConfirmStatus=STATUS_NOT_SUPPORTED;C(O->Close(Confirm,Ram)==STATUS_NOT_SUPPORTED&&Confirms==1&&!Unmaps&&R.Refs==2);
 Irql=2;C(O->Close(Confirm,Ram)==STATUS_INVALID_PARAMETER&&Confirms==1);Irql=0;
 ConfirmStatus=0x103;C(O->Close(Confirm,Ram)==STATUS_DEVICE_NOT_READY&&Confirms==2&&!Unmaps&&R.Refs==2);
 ConfirmStatus=0;C(O->Close(Confirm,Ram)==0&&Confirms==3&&Unmaps==1&&R.Refs==1);
 C(O->Close(Confirm,Ram)==0&&Confirms==3&&Unmaps==1);
 C(T70AudioResourceOwner::Initialize(O,nullptr,&R,(PPORTWAVERT)&Port)==STATUS_INVALID_DEVICE_STATE);
 C(O->Release()==0&&Allocated==Freed);
 // Failed initialization removes the provisional self-pin; empty close/free.
 C(T70AudioResourceOwner::Create(0,&O)==0);FailMap=true;
 C(T70AudioResourceOwner::Initialize(O,nullptr,&R,(PPORTWAVERT)&Port)==STATUS_INSUFFICIENT_RESOURCES&&R.Refs==1);
 C(O->AddRef()==2&&O->Release()==1);C(O->Close(nullptr,nullptr)==0);C(O->Release()==0&&Allocated==Freed);FailMap=false;
 // A raw mapping lease also blocks confirmation/unmap even after a client ends.
 C(T70AudioResourceOwner::Create(0,&O)==0);C(T70AudioResourceOwner::Initialize(O,nullptr,&R,(PPORTWAVERT)&Port)==0);
 C(O->BeginUse(&Use,&Afe)==0);C(T70AfeMappingBegin(Afe)==0);
 C(O->EndUse(&Use)==0);unsigned Before=Confirms;
 C(O->Close(Confirm,Ram)==STATUS_INVALID_DEVICE_STATE&&Confirms==Before);
 // Test keeps Afe only to simulate the independently retained mapping lease.
 C(T70AfeMappingEnd(Afe)==0);C(O->Close(Confirm,Ram)==0);C(O->Release()==0&&Allocated==Freed&&R.Refs==1);
 // Actual miniport -> explicit resource owner -> real MDL/DMA ownership,
 // failing at each allocation stage and at missing-cache-provider admission.
 for(AdmissionCase=1;AdmissionCase<=4;AdmissionCase++){
  static PortStream Ps;Ps.Frees=0;DmaFrees=AdmissionCalls=0;
  C(T70AudioResourceOwner::Create(0,&O)==0);
  T70_WAVERT_MINIPORT_PROVIDER Policy={O,T70AudioResourceOwner::Initialize,RejectAfterOwnership};
  C(T70WaveRtMiniport::Create(O,Policy,&M)==0);C(M->Init(nullptr,&R,(PPORTWAVERT)&Port)==0);
  KSDATAFORMAT F={};PMINIPORTWAVERTSTREAM Stream=(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55;
  NTSTATUS Status=M->NewStream(&Stream,&Ps,0,0,&F);
  C(Status==(AdmissionCase==4?STATUS_DEVICE_NOT_READY:STATUS_INSUFFICIENT_RESOURCES));
  C(Stream==(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55&&AdmissionCalls==1&&Ps.Refs==1&&!Ps.Mapped&&!DmaLive);
  C(Ps.Frees==(AdmissionCase==1?0u:1u)&&DmaFrees==(AdmissionCase==4?1u:0u));
  C(M->Release()==0);C(O->Close(nullptr,nullptr)==STATUS_DEVICE_NOT_READY);
  ConfirmStatus=0;C(O->Close(Confirm,Ram)==0);C(O->Release()==0&&Allocated==Freed&&R.Refs==1);
 }
 // Permitted concurrency: valid COM references overlap serialized Close.
 // Each worker owns references BEFORE launching. No raw-pointer resurrection
 // and no parallel Begin/End/Close (those remain externally serialized).
 for(unsigned Round=0;Round<32;Round++){
  C(T70AudioResourceOwner::Create(0,&O)==0);
  T70_WAVERT_MINIPORT_PROVIDER Policy={O,T70AudioResourceOwner::Initialize,NoStream};
  C(T70WaveRtMiniport::Create(O,Policy,&M)==0);C(M->Init(nullptr,&R,(PPORTWAVERT)&Port)==0);
  std::atomic<unsigned> Ready{0};std::atomic<bool> Go{false},Drop{false};
  std::thread Workers[8];auto Owner=O;auto Miniport=M;
  for(auto &Worker:Workers){
   Owner->AddRef();Miniport->AddRef();
   Worker=std::thread([&,Owner,Miniport]{
    Ready.fetch_add(1);while(!Go.load())std::this_thread::yield();
    for(unsigned N=0;N<1000;N++){
     Owner->AddRef();Miniport->AddRef();Miniport->Release();Owner->Release();
    }
    while(!Drop.load())std::this_thread::yield();
    Miniport->Release();Owner->Release();
   });
  }
  while(Ready.load()!=8)std::this_thread::yield();Go=true;
  unsigned BeforeFree=Freed.load(),BeforeUnmap=Unmaps;
  ConfirmStatus=0;C(O->Close(Confirm,Ram)==0&&Unmaps==BeforeUnmap+1&&R.Refs==1);
  // Final caller references go away while workers retain both objects.
  M->Release();O->Release();C(Freed.load()==BeforeFree);
  Drop=true;for(auto &Worker:Workers)Worker.join();
  C(Freed.load()==BeforeFree+2&&Allocated.load()==Freed.load());
 }
 printf("Audio resource lifetime: %u checks; fake resources, no DMA-stop proof\n",Checks.load());
}
