// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtStream.h"
#include "WaveRtMiniportBoundary.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned Checks,Irql,Allocations,Frees,Pins,Unpins,Describes,PageAttempts;
static bool FailAllocation;static NTSTATUS DescribeStatus,PinStatus,VerifyStatus,RemoveStatus;
static unsigned Verifies,Removes,Writes,CommonFrees;
#define C(X) do{++Checks;assert(X);}while(0)
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
LONG InterlockedIncrement(volatile LONG *P){return ++*P;}
LONG InterlockedDecrement(volatile LONG *P){return --*P;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(FailAllocation)return nullptr;Allocations++;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){Frees++;free(P);}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS,size_t,ULONG){assert(false);return nullptr;}
void MmUnmapIoSpace(void *,size_t){assert(false);}
extern "C" int32_t T70AfeMappedRead32(void *,uint32_t,uint32_t *){assert(false);return -1;}
extern "C" int32_t T70AfeMappedWrite32(void *,uint32_t,uint32_t){Writes++;return -73;}
class Owner:public IUnknown{public:ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}};
class Port:public IPortWaveRTStream{
public:ULONG Refs=1;bool Allow=false;MDL M={};alignas(PAGE_SIZE) unsigned char Bytes[8192];ULONG PageFrees=0;
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 PMDL AllocateContiguousPagesForMdl(PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,size_t N)override{PageAttempts++;M.ByteCount=(ULONG)N;return Allow?&M:nullptr;}
 void *MapAllocatedPages(PMDL Mdl,MEMORY_CACHING_TYPE C)override{assert(Mdl==&M&&C==MmCached);return Bytes;}
 void UnmapAllocatedPages(void *V,PMDL Mdl)override{assert(V==Bytes&&Mdl==&M);}
 void FreePagesFromMdl(PMDL Mdl)override{assert(Mdl==&M);PageFrees++;}
 ULONG GetPhysicalPagesCount(PMDL)override{return M.ByteCount/PAGE_SIZE;}
};
static NTSTATUS Describe(void *C,const T70_AFE_PREPARED *P,KSRTAUDIO_HWLATENCY *L){
 assert(C&&P->Rate==48000&&P->Channels==2);Describes++;*L={32,100,200};return DescribeStatus;
}
static NTSTATUS Pin(void *){Pins++;return PinStatus;}
static void Unpin(void *){Unpins++;}
static NTSTATUS Verify(void *,const T70_WAVERT_DMA_OWNER *){Verifies++;return VerifyStatus;}
static NTSTATUS Remove(void *){Removes++;return RemoveStatus;}
static NTSTATUS MapDma(PDMA_ADAPTER,PMDL,DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION *,ULONG,PHYSICAL_ADDRESS *Out){Out->QuadPart=0x100000000LL;return 0;}
static void FreeDma(PDMA_ADAPTER,ULONG,PHYSICAL_ADDRESS,void *,BOOLEAN){CommonFrees++;}
static NTSTATUS Init(void *,PUNKNOWN,PRESOURCELIST,PPORTWAVERT){return STATUS_SUCCESS;}
class Boundary:public T70WaveRtMiniportBoundary{
public:Boundary(const T70_WAVERT_MINIPORT_PROVIDER &P):T70WaveRtMiniportBoundary(0,P){}
 NTSTATUS QueryInterface(const GUID &,void **)override{return STATUS_NOT_SUPPORTED;}
 ULONG AddRef()override{return 1;}ULONG Release()override{return 1;}
 NTSTATUS GetDescription(PPCFILTER_DESCRIPTOR *)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,ULONG *)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS GetDeviceDescription(PDEVICE_DESCRIPTION)override{return STATUS_NOT_SUPPORTED;}
};
#include <initializer_list>
static void W16(uint8_t *B,unsigned V){B[0]=(uint8_t)V;B[1]=(uint8_t)(V>>8);}
static void W32(uint8_t *B,uint32_t V){W16(B,V);W16(B+2,V>>16);}
static void Format(uint8_t *B,unsigned Rate,unsigned Valid){
 const uint8_t Audio[]={0x61,0x75,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 const uint8_t Pcm[]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 const uint8_t Wave[]={0x81,0x9f,0x58,5,0x56,0xc3,0xce,0x11,0xbf,1,0,0xaa,0,0x55,0x59,0x5a};
 unsigned A=Valid==16?4:8;memset(B,0,104);W32(B,104);W32(B+8,A);
 memcpy(B+16,Audio,16);memcpy(B+32,Pcm,16);memcpy(B+48,Wave,16);
 W16(B+64,0xfffe);W16(B+66,2);W32(B+68,Rate);W32(B+72,Rate*A);W16(B+76,A);W16(B+78,Valid==16?16:32);W16(B+80,22);W16(B+82,Valid);W32(B+84,3);memcpy(B+88,Pcm,16);
}
int main(){
 Owner O;Port P;DMA_ADAPTER D={};T70_AFE_MAPPING_OWNER A={};unsigned Context=1;
 T70_WAVERT_STREAM_PROVIDER B={&O,&Context,&D,&A,8192,4096,Describe,Pin,Unpin,Verify,Remove};
 alignas(KSDATAFORMAT) uint8_t F[104];Format(F,48000,16);
 PMINIPORTWAVERTSTREAM S=(PMINIPORTWAVERTSTREAM)(uintptr_t)1;
 C(T70WaveRtStream::Create(nullptr,&P,0,(PKSDATAFORMAT)F,&S)==STATUS_DEVICE_NOT_READY&&S==(void *)1);
 for(unsigned Missing=0;Missing<9;Missing++){
  auto Bad=B;switch(Missing){case 0:Bad.Owner=nullptr;break;case 1:Bad.Context=nullptr;break;
   case 2:Bad.Adapter=nullptr;break;case 3:Bad.Afe=nullptr;break;case 4:Bad.Describe=nullptr;break;
   case 5:Bad.Pin=nullptr;break;case 6:Bad.Unpin=nullptr;break;case 7:Bad.Verify=nullptr;break;case 8:Bad.Remove=nullptr;break;}
  C(T70WaveRtStream::Create(&Bad,&P,0,(PKSDATAFORMAT)F,&S)==STATUS_DEVICE_NOT_READY&&S==(void *)1);
 }
 C(!Describes&&!Allocations&&O.Refs==1&&P.Refs==1);
 unsigned BeforeDescribe=Describes;
 Format(F,48000,24);C(T70WaveRtStream::Create(&B,&P,0,(PKSDATAFORMAT)F,&S)==STATUS_NOT_SUPPORTED);
 Format(F,48000,16);auto BadGeometry=B;BadGeometry.PeriodBytes=0;
 C(T70WaveRtStream::Create(&BadGeometry,&P,0,(PKSDATAFORMAT)F,&S)==STATUS_NOT_SUPPORTED);
 C(T70WaveRtStream::Create(&B,&P,1,(PKSDATAFORMAT)F,&S)==STATUS_NOT_SUPPORTED);
 Irql=2;C(T70WaveRtStream::Create(&B,&P,0,(PKSDATAFORMAT)F,&S)==STATUS_INVALID_PARAMETER);Irql=0;
 C(S==(void *)1&&Describes==BeforeDescribe&&!Allocations);
 for(NTSTATUS Status:{STATUS_DEVICE_DATA_ERROR,(NTSTATUS)0x103,(NTSTATUS)1}){
  DescribeStatus=Status;C(T70WaveRtStream::Create(&B,&P,0,(PKSDATAFORMAT)F,&S)==(Status<0?Status:STATUS_DEVICE_NOT_READY));
  C(S==(void *)1&&!Allocations&&O.Refs==1&&P.Refs==1);
 }
 DescribeStatus=0;FailAllocation=true;
 C(T70WaveRtStream::Create(&B,&P,0,(PKSDATAFORMAT)F,&S)==STATUS_INSUFFICIENT_RESOURCES&&S==(void *)1);FailAllocation=false;
 T70_WAVERT_MINIPORT_PROVIDER MP={&B,Init,T70WaveRtStream::Create};Boundary M(MP);
 C(M.Init(nullptr,(PRESOURCELIST)&Context,(PPORTWAVERT)&Context)==0);
 C(M.NewStream(&S,&P,0,0,(PKSDATAFORMAT)F)==0&&S&&O.Refs==2&&P.Refs==2);
 KSRTAUDIO_HWLATENCY L={};S->GetHWLatency(&L);C(L.FifoSize==32&&L.ChipsetDelay==100&&L.CodecDelay==200);
 KSAUDIO_POSITION Pos={9,9};C(S->GetPosition(&Pos)==0&&!Pos.PlayOffset&&!Pos.WriteOffset);
 C(S->SetState(KSSTATE_RUN)==STATUS_NOT_SUPPORTED);
 Format(F,44100,16);C(S->SetFormat((PKSDATAFORMAT)F)==STATUS_NOT_SUPPORTED);
 Format(F,48000,16);C(S->SetFormat((PKSDATAFORMAT)F)==0);
 PMDL Mdl=(PMDL)1;ULONG Bytes=7,Offset=8;MEMORY_CACHING_TYPE Cache=MmNonCached;
 PinStatus=0x103;C(S->AllocateAudioBuffer(8192,&Mdl,&Bytes,&Offset,&Cache)==STATUS_DEVICE_NOT_READY);
 C(Mdl==(PMDL)1&&Bytes==7&&Offset==8&&Cache==MmNonCached&&!PageAttempts&&!Unpins);
 PinStatus=0;C(S->AllocateAudioBuffer(8192,&Mdl,&Bytes,&Offset,&Cache)==STATUS_INSUFFICIENT_RESOURCES);
 C(PageAttempts==1&&Unpins==1&&O.Refs==2&&P.Refs==2);
 Pos={7,8};C(S->GetPosition(&Pos)==STATUS_DEVICE_NOT_READY&&Pos.PlayOffset==7&&Pos.WriteOffset==8);
 C(S->Release()==0&&O.Refs==1&&P.Refs==1&&Allocations==Frees);
 // Exercise the concrete provider hooks, not just the abstract base model.
 P.Allow=true;DMA_OPERATIONS Ops={};Ops.Size=sizeof Ops;
 Ops.CreateCommonBufferFromMdl=MapDma;Ops.FreeCommonBuffer=FreeDma;D={1,sizeof D,&Ops};
 A.Mapping.VirtualBase=P.Bytes;A.Mapping.PhysicalBase=0x11210000;A.Mapping.Length=8192;
 Format(F,48000,16);VerifyStatus=STATUS_DEVICE_NOT_READY;
 C(M.NewStream(&S,&P,0,0,(PKSDATAFORMAT)F)==0);
 C(S->AllocateAudioBuffer(8192,&Mdl,&Bytes,&Offset,&Cache)==STATUS_DEVICE_NOT_READY);
 C(Verifies==1&&!Writes&&CommonFrees==1&&P.PageFrees==1&&Unpins==2);
 C(S->Release()==0&&O.Refs==1&&P.Refs==1);
 VerifyStatus=0;RemoveStatus=STATUS_DEVICE_NOT_READY;
 C(M.NewStream(&S,&P,0,0,(PKSDATAFORMAT)F)==0);
 C(S->AllocateAudioBuffer(8192,&Mdl,&Bytes,&Offset,&Cache)==STATUS_DEVICE_NOT_READY);
 C(Writes==1&&Verifies==2&&Unpins==2&&O.Refs==2&&P.Refs==3);
 auto Concrete=static_cast<T70WaveRtStream *>(S);
 C(Concrete->Recover()==STATUS_DEVICE_NOT_READY&&Removes==1&&Unpins==2&&CommonFrees==1);
 RemoveStatus=0;C(Concrete->Recover()==0);
 C(Removes==2&&Unpins==3&&CommonFrees==2&&P.PageFrees==2&&P.Refs==2);
 C(Concrete->Recover()==0&&Unpins==3&&CommonFrees==2);
 C(S->Release()==0&&O.Refs==1&&P.Refs==1&&Allocations==Frees);
 printf("Concrete WaveRT factory: %u checks; fake providers, RUN unsupported\n",Checks);
}
