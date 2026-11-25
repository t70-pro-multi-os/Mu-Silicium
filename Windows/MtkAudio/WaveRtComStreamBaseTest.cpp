// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtComStreamBase.h"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <type_traits>
#include <vector>
static unsigned Checks,Allocated,Freed,Destroyed;static bool Fail;
#define C(X) do{++Checks;assert(X);}while(0)
unsigned char KeGetCurrentIrql(){return PASSIVE_LEVEL;}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
class Owner:public IUnknown{
public:std::atomic<ULONG> Refs{1};
 ULONG AddRef()override{return ++Refs;}
 ULONG Release()override{return --Refs;}
};
static Owner Parent;
void *ExAllocatePool2(uint64_t Flags,size_t Bytes,ULONG Tag){
 C(Flags==POOL_FLAG_NON_PAGED&&Tag==0x53573754);if(Fail)return nullptr;
 Allocated++;return calloc(1,Bytes);
}
void ExFreePoolWithTag(void *P,ULONG Tag){C(Tag==0x53573754&&Parent.Refs==2&&Destroyed==1);Freed++;free(P);}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS,size_t,ULONG){assert(false);return nullptr;}
void MmUnmapIoSpace(void *,size_t){assert(false);}
extern "C" int32_t T70AfeMappedRead32(void *,uint32_t,uint32_t *){assert(false);return -1;}
extern "C" int32_t T70AfeMappedWrite32(void *,uint32_t,uint32_t){assert(false);return -1;}
static_assert(std::is_abstract<T70WaveRtComStreamBase>::value,"hardware providers must remain mandatory");
class Fake final:public T70WaveRtComStreamBase{
public:Fake(PUNKNOWN O,const T70_AFE_PREPARED &P):T70WaveRtComStreamBase(O,P,nullptr,nullptr,nullptr,nullptr){}
 ~Fake()override{C(::Parent.Refs==2&&Freed==0);Destroyed++;}
 void GetHWLatency(KSRTAUDIO_HWLATENCY *)override{assert(false);}
 NTSTATUS PinRecoveryLifetime()override{return STATUS_DEVICE_NOT_READY;}
 void UnpinRecoveryLifetime()override{assert(false);}
 NTSTATUS VerifyClientMapping(const T70_WAVERT_DMA_OWNER &)override{return STATUS_DEVICE_NOT_READY;}
 NTSTATUS RemoveHardwareReferences()override{return STATUS_DEVICE_NOT_READY;}
};
int main(){
 T70_AFE_PREPARED P;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,512,256,&P));
 Fail=true;C(new Fake(&Parent,P)==nullptr);C(Parent.Refs==1&&!Allocated&&!Destroyed);Fail=false;
 Fake *S=new Fake(&Parent,P);C(S&&Parent.Refs==2&&Allocated==1);
 const GUID Unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
 const GUID Stream={0x000ac9ab,0xfaab,0x4f3d,{0x94,0x55,0x6f,0xf8,0x30,0x6a,0x74,0xa0}};
 GUID Other=Stream;Other.Data1++;
 void *A=nullptr,*B=nullptr;
 C(S->QueryInterface(Unknown,&A)==0&&A==static_cast<IMiniportWaveRTStream *>(S));
 C(S->QueryInterface(Stream,&B)==0&&A==B);
 C(static_cast<IMiniportWaveRTStream *>(A)->Release()==2);
 C(static_cast<IMiniportWaveRTStream *>(B)->Release()==1);
 A=(void *)1;C(S->QueryInterface(Other,&A)==STATUS_NOINTERFACE&&!A);
 C(S->QueryInterface(Stream,nullptr)==STATUS_INVALID_PARAMETER);
 // Each worker owns a reference before it is scheduled. The initial reference
 // remains held until joins, so a legitimate caller never races freed memory.
 std::vector<std::thread> Workers;
 for(unsigned I=0;I<8;I++){
  S->AddRef();Workers.emplace_back([S]{for(unsigned N=0;N<10000;N++){S->AddRef();S->Release();}S->Release();});
 }
 for(auto &W:Workers)W.join();
 C(Parent.Refs==2&&!Freed&&!Destroyed);
 C(S->SetState(KSSTATE_RUN)==STATUS_NOT_SUPPORTED);
 C(S->Release()==0);C(Parent.Refs==1&&Freed==1&&Destroyed==1);
 printf("WaveRT COM stream: %u checks + 80000 concurrent reference pairs; no hardware provider\n",Checks);
}
