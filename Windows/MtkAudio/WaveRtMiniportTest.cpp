// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtMiniport.h"
extern "C" {
#include "WaveRtFormat.h"
}
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
static unsigned Checks,Irql,Allocated,Freed;static bool FailAlloc;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
void *ExAllocatePool2(uint64_t F,size_t N,ULONG T){C(F==POOL_FLAG_NON_PAGED&&T==0x4d573754);if(FailAlloc)return nullptr;Allocated++;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG T){C(T==0x4d573754);Freed++;free(P);}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
class Owner:public IUnknown{public:ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}};
static NTSTATUS Initialize(void *,PUNKNOWN,PRESOURCELIST,PPORTWAVERT){return 0;}
static NTSTATUS CreateStream(void *,PPORTWAVERTSTREAM,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *){return STATUS_DEVICE_NOT_READY;}
int main(){
 Owner O;T70_WAVERT_MINIPORT_PROVIDER P={nullptr,Initialize,CreateStream};T70WaveRtMiniport *M=(T70WaveRtMiniport *)(uintptr_t)0x55;
 FailAlloc=true;C(T70WaveRtMiniport::Create(&O,P,&M)==STATUS_INSUFFICIENT_RESOURCES&&M==(T70WaveRtMiniport *)(uintptr_t)0x55&&O.Refs==1);FailAlloc=false;
 auto Missing=P;Missing.Create=nullptr;C(T70WaveRtMiniport::Create(&O,Missing,&M)==STATUS_DEVICE_NOT_READY&&!Allocated);
 C(T70WaveRtMiniport::Create(nullptr,P,&M)==STATUS_INVALID_PARAMETER);
 C(T70WaveRtMiniport::Create(&O,P,nullptr)==STATUS_INVALID_PARAMETER);
 Irql=2;C(T70WaveRtMiniport::Create(&O,P,&M)==STATUS_INVALID_PARAMETER);Irql=0;
 C(T70WaveRtMiniport::Create(&O,P,&M)==0&&O.Refs==2&&Allocated==1);
 const GUID Ids[]={ {0,0,0,{0xc0,0,0,0,0,0,0,0x46}},
 {0xb4c90a24,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}},
 {0x0f9fc4d6,0x6061,0x4f3c,{0xb1,0xfc,7,0x5e,0x35,0xf7,0x96,0x0a}} };
 for(const auto &Id:Ids){void *V=nullptr;C(M->QueryInterface(Id,&V)==0&&V==static_cast<IMiniportWaveRT *>(M));C(static_cast<IMiniportWaveRT *>(V)->Release()==1);}
 GUID Bad={1,2,3,{}};void *V=(void *)(uintptr_t)0x55;C(M->QueryInterface(Bad,&V)==STATUS_NOINTERFACE&&!V);C(M->QueryInterface(Bad,nullptr)==STATUS_INVALID_PARAMETER);
 std::thread Threads[8];for(auto &T:Threads)T=std::thread([&]{for(unsigned I=0;I<10000;I++){M->AddRef();M->Release();}});for(auto &T:Threads)T.join();
 C(M->AddRef()==2&&M->Release()==1&&!Freed&&O.Refs==2);
 PPCFILTER_DESCRIPTOR D=(PPCFILTER_DESCRIPTOR)(uintptr_t)0x55;C(M->GetDescription(&D)==STATUS_DEVICE_NOT_READY&&D==(PPCFILTER_DESCRIPTOR)(uintptr_t)0x55);
 DEVICE_DESCRIPTION Early,EarlyBefore;memset(&Early,0xa5,sizeof Early);EarlyBefore=Early;
 C(M->GetDeviceDescription(&Early)==STATUS_DEVICE_NOT_READY&&!memcmp(&Early,&EarlyBefore,sizeof Early));
 unsigned char Fake[3]={};C(M->Init(nullptr,(PRESOURCELIST)&Fake[0],(PPORTWAVERT)&Fake[1])==0);
 C(M->GetDescription(&D)==0&&D&&D->Version==0&&D->PinCount==1&&D->PinSize==sizeof(PCPIN_DESCRIPTOR)&&D->NodeCount==0&&D->ConnectionCount==0);
 const auto &K=D->Pins[0].KsPinDescriptor;C(K.DataRangesCount==11&&K.DataFlow==KSPIN_DATAFLOW_IN&&K.Communication==KSPIN_COMMUNICATION_SINK);
 const ULONG Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 for(unsigned I=0;I<11;I++){auto R=(KSDATARANGE_AUDIO *)K.DataRanges[I];C(R->DataRange.FormatSize==sizeof(*R)&&R->MaximumChannels==2&&R->MinimumBitsPerSample==16&&R->MaximumBitsPerSample==16&&R->MinimumSampleFrequency==Rates[I]&&R->MaximumSampleFrequency==Rates[I]);}
 C(M->GetDescription(nullptr)==STATUS_INVALID_PARAMETER);Irql=2;C(M->GetDescription(&D)==STATUS_INVALID_PARAMETER);Irql=0;
 ULONG Length=55;C(M->DataRangeIntersection(0,nullptr,nullptr,0,nullptr,&Length)==STATUS_INVALID_PARAMETER&&Length==55);
 // Match every published rate and channel ceiling. Returned bytes are
 // checked against exact WAVEFORMATEXTENSIBLE fields, not mock ABI layout.
 for(unsigned I=0;I<11;I++)for(unsigned Ch=1;Ch<=3;Ch++){
  auto Matching=K.DataRanges[I];KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)Matching;
  Client.MaximumChannels=Ch;Client.MinimumSampleFrequency=8000;Client.MaximumSampleFrequency=192000;
  unsigned char Output[108];memset(Output,0xa5,sizeof Output);ULONG L=77;
  C(M->DataRangeIntersection(0,&Client.DataRange,Matching,0,nullptr,&L)==STATUS_BUFFER_OVERFLOW&&L==104);
  for(ULONG Cap=1;Cap<104;Cap++){
   L=77;C(M->DataRangeIntersection(0,&Client.DataRange,Matching,Cap,Output,&L)==STATUS_BUFFER_TOO_SMALL&&L==77);
   for(auto V:Output)C(V==0xa5);
  }
  C(M->DataRangeIntersection(0,&Client.DataRange,Matching,sizeof Output,Output,&L)==0&&L==104);
  unsigned Channels=Ch>2?2:Ch,Align=Channels*2;
  C(Output[0]==104&&Output[8]==Align&&Output[64]==0xfe&&Output[65]==0xff&&Output[66]==Channels);
  ULONG Rate=Output[68]|((ULONG)Output[69]<<8)|((ULONG)Output[70]<<16)|((ULONG)Output[71]<<24);
  ULONG Bytes=Output[72]|((ULONG)Output[73]<<8)|((ULONG)Output[74]<<16)|((ULONG)Output[75]<<24);
  C(Rate==Rates[I]&&Bytes==Rate*Align&&Output[76]==Align&&Output[78]==16&&Output[80]==22&&Output[82]==16&&Output[84]==(Channels==1?4:3));
  C(Output[104]==0xa5&&Output[107]==0xa5);
  T70_AFE_PREPARED Prepared;C(T70WaveRtPrepareBufferFormat(Output,104,0,4096,1024,&Prepared)==T70WavePrepared);
  C(Prepared.Rate==Rates[I]&&Prepared.Channels==Channels&&Prepared.SampleFormat==T70AfeS16Le);
  Client.MinimumBitsPerSample=24;L=77;unsigned char Before[108];memcpy(Before,Output,108);
  C(M->DataRangeIntersection(0,&Client.DataRange,Matching,108,Output,&L)==STATUS_NO_MATCH&&L==77&&!memcmp(Output,Before,108));
 }
 for(ULONG Ch=1;Ch<=3;Ch++)for(ULONG Sample=0;Sample<=8;Sample++){
  KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)K.DataRanges[6];
  Client.MaximumChannels=Ch;Client.DataRange.SampleSize=Sample;ULONG L=77;
  unsigned char Output[104];memset(Output,0xa5,104);
  bool Accepted=Sample==0||Sample==2||(Sample==4&&Ch>=2);
  NTSTATUS R=M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[6],104,Output,&L);
  C((R==0)==Accepted);
  if(Accepted){
   ULONG Channels=Sample?Sample/2:(Ch>=2?2:1);T70_AFE_PREPARED Prepared;
   C(L==104&&Output[66]==Channels&&Output[8]==Channels*2);
   C(T70WaveRtPrepareBufferFormat(Output,104,0,4096,1024,&Prepared)==T70WavePrepared&&Prepared.Channels==Channels);
  }else{C(R==STATUS_NO_MATCH&&L==77);for(auto V:Output)C(V==0xa5);}
 }
 for(unsigned Case=0;Case<12;Case++){
  KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)K.DataRanges[0],Match=Client;
  if(Case==0)Client.DataRange.FormatSize--;
  if(Case==1)Client.DataRange.Flags=1;
  if(Case==2)Client.DataRange.SubFormat.Data1=3;
  if(Case==3)Client.MaximumChannels=0;
  if(Case==4)Client.MinimumBitsPerSample=32;
  if(Case==5)Client.MaximumBitsPerSample=8;
  if(Case==6)Client.MinimumSampleFrequency=9000;
  if(Case==7)Client.MaximumSampleFrequency=7999;
  if(Case==8)Client.DataRange.SampleSize=1;
  if(Case==9)Match.MinimumSampleFrequency=9999;
  if(Case==10)Match.DataRange.FormatSize++;
  if(Case==11)Client.DataRange.Specifier.Data1=0;
  ULONG L=77;unsigned char Output[104];memset(Output,0xa5,104);
  C(M->DataRangeIntersection(0,&Client.DataRange,&Match.DataRange,104,Output,&L)==STATUS_NO_MATCH&&L==77);
  for(auto V:Output)C(V==0xa5);
 }
 KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)K.DataRanges[0];ULONG L=77;unsigned char Output[104]={};
 C(M->DataRangeIntersection(1,&Client.DataRange,K.DataRanges[0],104,Output,&L)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,nullptr,&L)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,Output,nullptr)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,&Client,&L)==STATUS_INVALID_PARAMETER);
 C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,Output,(PULONG)Output)==STATUS_INVALID_PARAMETER);
 Irql=2;C(M->DataRangeIntersection(0,&Client.DataRange,K.DataRanges[0],104,Output,&L)==STATUS_INVALID_PARAMETER);Irql=0;
 DEVICE_DESCRIPTION DD,Before;memset(&DD,0xa5,sizeof DD);Before=DD;
 Irql=2;C(M->GetDeviceDescription(&DD)==STATUS_INVALID_PARAMETER&&!memcmp(&DD,&Before,sizeof DD));Irql=0;
 C(M->GetDeviceDescription(nullptr)==STATUS_INVALID_PARAMETER);
 C(M->GetDeviceDescription(&DD)==STATUS_SUCCESS);
 DEVICE_DESCRIPTION Expected={};Expected.Version=DEVICE_DESCRIPTION_VERSION3;
 Expected.Master=TRUE;Expected.InterfaceType=InterfaceTypeUndefined;
 Expected.MaximumLength=196608;Expected.DmaAddressWidth=34;
 C(!memcmp(&DD,&Expected,sizeof DD));
 PMINIPORTWAVERTSTREAM S=(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55;
 C(M->NewStream(&S,(PPORTWAVERTSTREAM)&Fake[0],0,0,(PKSDATAFORMAT)&Fake[1])==STATUS_DEVICE_NOT_READY&&S==(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55);
 C(M->Release()==0&&Freed==1&&O.Refs==1);
 printf("WaveRT concrete COM: %u checks + 80000 concurrent reference pairs; no endpoint\n",Checks);
}
