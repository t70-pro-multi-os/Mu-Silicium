// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicStream.h"
#include "WaveCyclicFactory.h"
#include "AudioExportOwner.h"
#include <ksmedia.h>
#include <cstdlib>
#include <cassert>
#include <initializer_list>

#include <cstring>
#include <cstdio>
#include <thread>
#include <atomic>
static unsigned char Irql;
unsigned char KeGetCurrentIrql(){return Irql;}
struct Channel:IDmaChannel{
 ULONG Refs=1,Free=0,Copies=0,Size=0;NTSTATUS Status=0;bool Bad=false;LONGLONG Dma=0x123400;ULONG Actual=512;unsigned char Ram[32768]={};
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 NTSTATUS AllocateBuffer(ULONG N,PHYSICAL_ADDRESS *P)override{assert(!P);Size=N;return Status;}
 void FreeBuffer()override{++Free;}void *SystemAddress()override{return Bad?nullptr:Ram;}
 ULONG AllocatedBufferSize()override{return Actual;}void SetBufferSize(ULONG N)override{Size=N;}
 ULONG BufferSize()override{return Size;}
 PHYSICAL_ADDRESS PhysicalAddress()override{PHYSICAL_ADDRESS P={};P.QuadPart=Dma;return P;}
 void CopyTo(void *D,void *S,ULONG N)override{++Copies;memcpy(D,S,N);}
};
struct Port:IPortWaveCyclic{
 ULONG Refs=1,Notifications=0;NTSTATUS Status=0;bool Null=false;Channel C;
 void Notify(PSERVICEGROUP G)override{assert(G);++Notifications;}
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS NewMasterDmaChannel(IDmaChannel **Out,PUNKNOWN U,PRESOURCELIST R,ULONG N,BOOLEAN A,BOOLEAN B,DMA_WIDTH,DMA_SPEED)override{
 assert(!U&&!R&&N==196608&&A&&!B);if(Status==0){if(!Null){assert(C.Refs<=1);C.Refs=1;}*Out=Null?nullptr:&C;}return Status;
 }
};
volatile void *RtlCopyDeviceMemory(volatile void *D,const volatile void *S,size_t N){memcpy((void *)D,(const void *)S,N);return D;}
static bool FailAllocation;static unsigned Allocations,Frees;
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG E){return __sync_val_compare_and_swap(P,E,V);}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){if(FailAllocation)return nullptr;++Allocations;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Frees;free(P);}
static unsigned Calls,Fail;static NTSTATUS ConfirmStatus,RemoveStatus,ExecuteStatus;static bool SkipExecute;static uint32_t Registers[0x800],Trace[256];
static int32_t ReadReg(void *,uint32_t A,uint32_t *V){Trace[Calls]=A;++Calls;if(Calls==Fail)return STATUS_DEVICE_DATA_ERROR;*V=Registers[A/4];return 0;}
static int32_t WriteReg(void *,uint32_t A,uint32_t V){Trace[Calls]=A|0x80000000u;++Calls;if(Calls==Fail)return STATUS_DEVICE_DATA_ERROR;Registers[A/4]=V;return 0;}
static NTSTATUS Execute(void *,NTSTATUS (*F)(void *),void *C){if(ExecuteStatus)return ExecuteStatus;if(SkipExecute)return 0;Irql=7;auto S=F(C);Irql=0;return S;}
static NTSTATUS PositionExecute(void *,NTSTATUS (*F)(void *),void *C){auto Saved=Irql;Irql=7;auto S=F(C);Irql=Saved;return S;}
static NTSTATUS PrepareStatus,StoppedStatus;static unsigned Prepared,Stopped;
static NTSTATUS PrepareState(void *){++Prepared;if(PrepareStatus)return PrepareStatus;Registers[0x5c/4]=0x123400;Registers[0x58/4]=0;return 0;}
static NTSTATUS StoppedState(void *){++Stopped;return StoppedStatus;}
static NTSTATUS Confirm(void *){return ConfirmStatus;}
static NTSTATUS Remove(void *){return RemoveStatus;}
// Factory tests replace MMIO and IRQ locking only; real stream/buffer/miniport
// functions execute below. Full owner concurrency is covered separately.
extern "C" int32_t T70AfeMappedRead32(void *C,uint32_t A,uint32_t *V){return ReadReg(C,A,V);}
extern "C" int32_t T70AfeMappedWrite32(void *C,uint32_t A,uint32_t V){return WriteReg(C,A,V);}
NTSTATUS T70AfeIrqRecoveryExecute(void *C,NTSTATUS (*F)(void *),void *X){return Execute(C,F,X);}
NTSTATUS T70AfeIrqRecoveryObserve(void *C,NTSTATUS (*F)(void *),void *X){return PositionExecute(C,F,X);}
struct Group:IServiceGroup {ULONG Refs=1;PSERVICESINK Member=nullptr;
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 NTSTATUS AddMember(PSERVICESINK S)override{assert(!Member);Member=S;S->AddRef();return 0;}
 void RemoveMember(PSERVICESINK S)override{assert(Member==S);Member=nullptr;S->Release();}
};
NTSTATUS PcNewServiceGroup(PSERVICEGROUP *,PUNKNOWN){abort();}
static NTSTATUS DrainStatus;static unsigned Drains;
static T70_AUDIO_EXPORT_OWNER *Exports;
static NTSTATUS Publish(void *P,PMINIPORTWAVECYCLICSTREAM S,PDMACHANNEL D,PSERVICEGROUP G,
 PMINIPORTWAVECYCLICSTREAM *OS,PDMACHANNEL *OD,PSERVICEGROUP *OG){
 return T70AudioExportWrap(Exports,(Port *)P,S,D,G,OS,OD,OG);
}
static NTSTATUS Drain(void *){++Drains;return DrainStatus?DrainStatus:Exports?T70AudioExportDrain(Exports):0;}
static void FactoryTests(){
 const ULONG Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 for(unsigned Error=0;Error<115;Error++){
  ULONG Rate=Error>=18&&Error<40?Rates[(Error-18)/2]:48000,Channels=Error>=18&&Error<40?1+(Error-18)%2:2,Frame=Channels*2;
  if(Error>=49){Rate=Rates[((Error-49)%22)/2];Channels=1+(Error-49)%2;Frame=Channels*2;}
  Port P;P.C.Actual=32768;Group Group;T70_AUDIO_EXPORT_OWNER ExportOwner={};Exports=Error==47?&ExportOwner:nullptr;
  Calls=Fail=0;ConfirmStatus=ExecuteStatus=RemoveStatus=PrepareStatus=StoppedStatus=DrainStatus=0;SkipExecute=false;memset(Registers,0,sizeof Registers);
  T70_WAVECYCLIC_BOARD B={};B.Parent=&P;B.Resources=(PRESOURCELIST)&B;B.Lease=true;B.Started=true;
  B.Irq.Owner.Interrupt=(PKINTERRUPT)&B;B.Irq.Owner.Service.Port=&P;B.Irq.Owner.Service.Group=&Group;
  T70_WAVECYCLIC_FACTORY F={};F.Board=&B;F.Contract={&P,Confirm,Remove,PrepareState,Confirm,StoppedState,Drain};
  if(Exports)F.Contract.Publish=Publish;
  auto Provider=T70WaveCyclicFactoryProvider(&F);T70WaveCyclicMiniport *Mini=nullptr;
  assert(T70WaveCyclicMiniport::Create(&P,Provider,&Mini)==0&&Mini->Init(nullptr,B.Resources,&P)==0);
  assert(B.BeforeIrqClose&&B.StreamOwner==&F);
  KSDATAFORMAT_WAVEFORMATEXTENSIBLE W={};auto &K=W.DataFormat;auto V=(unsigned char *)&W.WaveFormatExt;
  auto U16=[&](unsigned A,unsigned X){V[A]=(unsigned char)X;V[A+1]=(unsigned char)(X>>8);};
  auto U32=[&](unsigned A,unsigned X){for(unsigned J=0;J<4;J++)V[A+J]=(unsigned char)(X>>(J*8));};
  K.FormatSize=104;K.SampleSize=Frame;
  K.MajorFormat=GUID{STATIC_KSDATAFORMAT_TYPE_AUDIO};K.SubFormat=GUID{STATIC_KSDATAFORMAT_SUBTYPE_PCM};K.Specifier=GUID{STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX};
  U16(0,0xfffe);U16(2,Channels);U32(4,Rate);U32(8,Rate*Frame);U16(12,Frame);U16(14,16);U16(16,22);U16(18,16);U32(20,Channels==2?3:4);
  GUID PCM={STATIC_KSDATAFORMAT_SUBTYPE_PCM};memcpy(V+24,&PCM,16);
  // Model PortCls negotiation before the actual NewStream/factory/stream flow.
  PPCFILTER_DESCRIPTOR Desc=nullptr;assert(Mini->GetDescription(&Desc)==0);
  PKSDATARANGE Matching=nullptr;
  for(unsigned J=0;J<Desc->Pins[0].KsPinDescriptor.DataRangesCount;J++){
   auto A=(KSDATARANGE_AUDIO *)Desc->Pins[0].KsPinDescriptor.DataRanges[J];
   if(A->MinimumSampleFrequency==Rate)Matching=&A->DataRange;
  }
  assert(Matching);KSDATARANGE_AUDIO Client=*(KSDATARANGE_AUDIO *)Matching;
  Client.MaximumChannels=Channels;ULONG FormatBytes=0;
  KSDATAFORMAT_WAVEFORMATEXTENSIBLE Negotiated={};
  assert(Mini->DataRangeIntersection(0,&Client.DataRange,Matching,0,nullptr,&FormatBytes)==STATUS_BUFFER_OVERFLOW&&FormatBytes==104);
  assert(Mini->DataRangeIntersection(0,&Client.DataRange,Matching,104,&Negotiated,&FormatBytes)==0);
  assert(!memcmp(&W,&Negotiated,104));W=Negotiated;
  // Equivalent official PCM representations must reach the same factory and
  // stream path, rather than failing before any device configuration.
  if(Error>=49&&Error<93){K.FormatSize=82;U16(0,1);U16(16,Error<71?0:65535);K.SampleSize=Error<71?0:Frame;}
  if(Error>=93)K.SampleSize=0;
  if(Error>=1&&Error<=12){Fail=Error;RemoveStatus=STATUS_DEVICE_NOT_READY;}
  if(Error==13)B.Started=false;
  if(Error==14)B.Irq.Owner.Service.Port=nullptr;
  if(Error==15){U16(14,32);U16(18,24);U16(12,8);U32(8,384000);K.SampleSize=8;}
  if(Error==16)P.Status=STATUS_INSUFFICIENT_RESOURCES;
  if(Error==17)FailAllocation=true;
  if(Error==46)Fail=3; // partial configure error, then successful normal STOP cleanup
  PMINIPORTWAVECYCLICSTREAM S=nullptr;PDMACHANNEL D=nullptr;PSERVICEGROUP G=nullptr;
  auto R=Mini->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&K,&D,&G);FailAllocation=false;
  if(Error==17||Error==46){
   assert(R==(Error==17?STATUS_INSUFFICIENT_RESOURCES:STATUS_DEVICE_DATA_ERROR)&&!S&&!D&&!G);
   if(Error==17)assert(!Calls);else assert(Calls>3);
   assert(!F.Closing&&F.Port==&P&&F.Initialized&&!F.Stream&&!F.Buffer.Channel);
   assert(B.BeforeIrqClose&&B.StreamOwner==&F);
   // A subsequent real request succeeds without device re-init or fake restart.
   Calls=Fail=0;R=Mini->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,&K,&D,&G);
   assert(R==0&&S&&D&&G&&P.C.Free==1);
   assert(S->SetFormat(&K)==0);
   S->Release();D->Release();G->Release();
  }
  if(Error&&Error<17){assert(R!=0&&!S&&!D&&!G);if(Error<=12)assert(F.Stream&&F.Buffer.Uses==1&&F.CleanupStatus!=0);else assert(!Calls);}
  else if(Error!=17&&Error!=46){
   assert(R==0&&Group.Refs==2);
   assert(S->SetFormat(&K)==STATUS_SUCCESS);
   if(Error>=49){
    // PortCls may spell the same accepted format differently in SetFormat.
    assert(S->SetFormat(&Negotiated.DataFormat)==STATUS_SUCCESS);
    auto Different=Negotiated;((unsigned char *)&Different.WaveFormatExt)[4]^=1;
    assert(S->SetFormat(&Different.DataFormat)==STATUS_NOT_SUPPORTED);
   }
   if(Exports)assert(S!=F.Stream&&D!=F.Buffer.Channel&&G!=&Group&&Exports->Gate==3);
   else assert(S==F.Stream&&D==F.Buffer.Channel&&G==&Group);
   ULONG Size=0;auto Interval=S->SetNotificationFreq(10,&Size);
   ULONG Expected=(ULONG)((((uint64_t)Rate*10+999)/1000*Frame+15)&~15ull);
   assert(Interval>=10&&Interval<=11&&Size==Expected&&F.Buffer.Bytes==Expected*2);
   if(Error==0){assert(S->SetNotificationFreq(5,&Size)==5&&Size==960);Expected=960;}
   assert(S->SetNotificationFreq(0,&Size)==0&&!Size);assert(S->SetNotificationFreq(UINT32_MAX,&Size)==0&&!Size);
   auto RepeatCadence=[&]{
    unsigned char BeforeStream[sizeof(T70WaveCyclicStream)];uint32_t BeforeRegs[0x800];
    memcpy(BeforeStream,static_cast<const void *>(F.Stream),sizeof BeforeStream);memcpy(BeforeRegs,Registers,sizeof BeforeRegs);
    auto BeforeCalls=Calls;ULONG CurrentRequest=Error==0?5:10;
    ULONG ExpectedInterval=(ULONG)(((uint64_t)Expected*1000+Rate*Frame-1)/(Rate*Frame));
    Size=77;assert(S->SetNotificationFreq(CurrentRequest,&Size)==ExpectedInterval&&Size==Expected);
    assert(Calls==BeforeCalls&&!memcmp(BeforeRegs,Registers,sizeof BeforeRegs)&&!memcmp(BeforeStream,static_cast<const void *>(F.Stream),sizeof BeforeStream));
    // Error 0 selected 5ms while stopped, so 10ms is genuinely different.
    // 48kHz stereo also has a valid 5ms alternative when 10ms is selected.
    ULONG Other=Error==0?10:(Rate==48000&&Channels==2?5:20);
    Size=77;assert(S->SetNotificationFreq(Other,&Size)==0&&!Size);
    assert(Calls==BeforeCalls&&!memcmp(BeforeRegs,Registers,sizeof BeforeRegs)&&!memcmp(BeforeStream,static_cast<const void *>(F.Stream),sizeof BeforeStream));
   };
   auto CheckPublicCopy=[&](ULONG Ring){
    if(Error!=47)return;
    assert(D->SystemAddress()==F.Buffer.Cpu&&F.Buffer.Cpu==P.C.Ram);
    assert((uintptr_t)F.Buffer.Cpu!=(uintptr_t)P.C.Dma);
    assert(Registers[0x54/4]==(ULONG)P.C.Dma); // configured device address, not CPU pointer
    unsigned char Source[9]={1,2,3,4,5,6,7,8,9},Saved[9];memcpy(Saved,Source,9);
    for(unsigned char Level:{(unsigned char)0,(unsigned char)DISPATCH_LEVEL,(unsigned char)7}){
     for(ULONG Offset:{0u,13u,Ring-9}){
      memset(P.C.Ram,0xa5,sizeof P.C.Ram);auto Copies=P.C.Copies;
      Irql=Level;D->CopyTo((unsigned char *)D->SystemAddress()+Offset,Source,9);Irql=0;
      assert(P.C.Copies==Copies+1&&!memcmp(Source,Saved,9));
      for(ULONG J=0;J<sizeof P.C.Ram;J++)assert(P.C.Ram[J]==(J>=Offset&&J<Offset+9?Source[J-Offset]:0xa5));
     }
     // Model a wrap with two contract-valid contiguous destination spans,
     // not an out-of-range CopyTo. Include non-frame-aligned byte counts.
     memset(P.C.Ram,0xa5,sizeof P.C.Ram);auto Copies=P.C.Copies;
     Irql=Level;D->CopyTo((unsigned char *)D->SystemAddress()+Ring-3,Source,3);
     D->CopyTo(D->SystemAddress(),Source+3,6);Irql=0;
     assert(P.C.Copies==Copies+2&&!memcmp(Source,Saved,9));
     for(ULONG J=0;J<sizeof P.C.Ram;J++){
      unsigned char ExpectedByte=J<6?Source[J+3]:(J>=Ring-3&&J<Ring?Source[J-(Ring-3)]:0xa5);
      assert(P.C.Ram[J]==ExpectedByte);
     }
    }
   };
   assert(S->SetState(KSSTATE_ACQUIRE)==0);RepeatCadence();
   assert(S->SetState(KSSTATE_PAUSE)==0);RepeatCadence();CheckPublicCopy(D->BufferSize());
   assert(S->SetState(KSSTATE_RUN)==0);RepeatCadence();CheckPublicCopy(D->BufferSize());
   assert((Registers[0x300/4]&0x3ffff)==Expected/Frame); // IRQ counts frames, not bytes
   Registers[0x5c/4]=0x123400+Expected;Registers[0x58/4]=0;
   assert(S->SetState(KSSTATE_PAUSE)==0);RepeatCadence();
   assert(S->SetState(KSSTATE_ACQUIRE)==0);RepeatCadence();
   assert(S->SetState(KSSTATE_STOP)==0);
   ULONG Position=99;assert(S->GetPosition(&Position)==0&&Position==0);
   if(Error==47){
    D->SetBufferSize(1920);CheckPublicCopy(1920); // same retained allocation while STOP
   }
   assert(S->SetState(KSSTATE_ACQUIRE)==0&&S->SetState(KSSTATE_PAUSE)==0&&S->SetState(KSSTATE_RUN)==0);
   if(Error==47){
    assert(Registers[0x64/4]-Registers[0x54/4]+1==1920&&F.Buffer.Bytes==3840);
    CheckPublicCopy(1920);Expected=960; // new hardware period, same common-buffer CPU allocation
   }
   PMINIPORTWAVECYCLICSTREAM Other=nullptr;PDMACHANNEL DC=nullptr;PSERVICEGROUP GC=nullptr;
   assert(Mini->NewStream(&Other,nullptr,(POOL_TYPE)0,0,FALSE,&K,&DC,&GC)!=0&&!Other&&!DC&&!GC);
   Registers[0x5c/4]=0x123400+Expected;Registers[0x58/4]=0;
   if(Error>=40&&Error<46){Registers[0x1b08/4]=0;Registers[0x3a0/4]=1;Fail=Calls+Error-39;}
   if(Error==48){StoppedStatus=STATUS_DEVICE_NOT_READY;Calls=0;}
   auto StoppedBefore=Stopped;
   DrainStatus=STATUS_DEVICE_NOT_READY;auto DrainsBefore=Drains;
   auto Quiesced=B.BeforeIrqClose(B.StreamOwner);
   assert((Quiesced!=0)==(Error>=40&&Error<46));
   assert(F.Closing&&F.Stream&&F.Buffer.Channel&&F.Port==&P&&!P.C.Free&&Drains==DrainsBefore);
   if(Quiesced==0){
    assert(F.Buffer.Uses==0&&F.Published);
    auto IoBefore=Calls;
    // Model Board's IRQ teardown between the real pre-hook and final Close.
    B.Irq.Owner.Interrupt=nullptr;
    assert(B.BeforeIrqClose(B.StreamOwner)==0&&Calls==IoBefore&&Drains==DrainsBefore);
    assert(T70WaveCyclicFactoryClose(&F)==STATUS_DEVICE_NOT_READY&&Calls==IoBefore);
    assert(F.Stream&&F.Buffer.Channel&&F.Port==&P&&!P.C.Free);
   }
   if(Error==48){
    assert(F.Buffer.Uses==0&&Stopped==StoppedBefore); // stock STOP succeeded, exports still retained
    bool Readback=false,Cleared=false;
    for(unsigned J=0;J<Calls;J++){
     assert(Trace[J]!=0x5c&&Trace[J]!=0x58); // destruction does not need CUR/PAUSE
     Readback|=Trace[J]==0x1b00;Cleared|=Trace[J]==(0x80000000u|0x3a8);
    }
    assert(Readback&&Cleared);StoppedStatus=0;
   }
   S->Release();D->Release();G->Release();DrainStatus=0;
  }
  RemoveStatus=0;Fail=0;assert(B.BeforeIrqClose(B.StreamOwner)==0);
  auto BeforeFinalIo=Calls;B.Irq.Owner.Interrupt=nullptr;
  assert(T70WaveCyclicFactoryClose(&F)==0&&!F.Stream&&!F.Buffer.Channel&&!F.Port&&Calls==BeforeFinalIo);
  assert(T70WaveCyclicFactoryClose(&F)==0);
  assert(Mini->Close(Confirm,&P)==0&&Mini->Release()==0&&P.Refs==1&&Group.Refs==1);
 }
}
static void PrefetchTests(){
 T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_PREPARED P={};
 for(unsigned Frames:{480u,524u,528u})for(unsigned Pattern=0;Pattern<256;Pattern++){
  assert(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x123400,Frames*8,Frames*4,&P));
  uint32_t Old=0x01010101u*Pattern;Registers[0x4c/4]=Old;Calls=Fail=0;
  assert(T70AfeDl1PreparePrefetch(&P,&Io)==0);
  uint32_t Code=Frames>=528?3:0;
  assert(Registers[0x4c/4]==((Old&~0xf03000u)|(Code<<12)|(Code<<20)));
  for(unsigned N=0;N<Calls;N++)assert((Trace[N]&0x7fffffff)==0x4c);
 }
 for(unsigned Failure=1;Failure<=4;Failure++){
  Registers[0x4c/4]=0;Calls=0;Fail=Failure;
  assert(T70AfeDl1PreparePrefetch(&P,&Io)==STATUS_DEVICE_DATA_ERROR&&Calls==Failure);
 }
 Calls=Fail=0;P.Rate=0;assert(T70AfeDl1PreparePrefetch(&P,&Io)!=0&&!Calls);
}
static T70_WAVECYCLIC_SERVICE *CheckedService;
static Port *CheckedPort;
static unsigned ReadyCalls,PrepareCalls,StopCalls;
static T70_AUDIO_EXPORT_OWNER *CheckedExports;
static NTSTATUS PauseCallbacks(void *){return T70AudioExportPauseServices(CheckedExports);}
static NTSTATUS ResumeCallbacks(void *){return T70AudioExportResumeServices(CheckedExports);}
struct CallbackSink:IServiceSink {
 ULONG Refs=1,Calls=0;std::atomic<bool> Hold{false},Entered{false};
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 void RequestService()override{++Calls;Entered=true;while(Hold.load())std::this_thread::yield();}
};
static NTSTATUS CheckPaused(){
 unsigned N=CheckedPort->Notifications;assert(T70WaveCyclicServiceNotifyConfirmed(CheckedService));
 assert(CheckedPort->Notifications==N);return STATUS_SUCCESS;
}
static NTSTATUS CheckedPrepare(void *C){++PrepareCalls;CheckPaused();return PrepareState(C);}
static NTSTATUS CheckedReady(void *){++ReadyCalls;CheckPaused();return ConfirmStatus;}
static NTSTATUS CheckedStop(void *C){++StopCalls;CheckPaused();return StoppedState(C);}
static void ServiceFactoryTests(){
 for(unsigned Case=0;Case<12;Case++){
  Port P;P.C.Actual=32768;Group G;T70_AUDIO_EXPORT_OWNER E={};Exports=&E;CheckedExports=&E;CallbackSink Sink;
  Calls=Fail=Prepared=Stopped=ReadyCalls=PrepareCalls=StopCalls=0;
  ConfirmStatus=ExecuteStatus=RemoveStatus=PrepareStatus=StoppedStatus=DrainStatus=0;SkipExecute=false;memset(Registers,0,sizeof Registers);
  T70_WAVECYCLIC_BOARD B={};B.Parent=&P;B.Resources=(PRESOURCELIST)&B;B.Lease=B.Started=true;
  B.Irq.Owner.Interrupt=(PKINTERRUPT)&B;B.Irq.Owner.Service.Port=&P;B.Irq.Owner.Service.Group=&G;
  CheckedService=&B.Irq.Owner.Service;CheckedPort=&P;
  T70_WAVECYCLIC_FACTORY F={};F.Board=&B;F.Contract={&P,Confirm,Remove,CheckedPrepare,CheckedReady,CheckedStop,Drain,Publish,PauseCallbacks,ResumeCallbacks};
  T70WaveCyclicMiniport *M=nullptr;auto Provider=T70WaveCyclicFactoryProvider(&F);
  assert(T70WaveCyclicMiniport::Create(&P,Provider,&M)==0&&M->Init(nullptr,B.Resources,&P)==0);
  alignas(8) unsigned char W[104]={};auto Put16=[&](unsigned A,unsigned V){W[A]=(unsigned char)V;W[A+1]=(unsigned char)(V>>8);};
  auto Put32=[&](unsigned A,uint32_t V){for(unsigned I=0;I<4;I++)W[A+I]=(unsigned char)(V>>(I*8));};
  GUID Audio={STATIC_KSDATAFORMAT_TYPE_AUDIO},Pcm={STATIC_KSDATAFORMAT_SUBTYPE_PCM},Wave={STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX};
  Put32(0,82);memcpy(W+16,&Audio,16);memcpy(W+32,&Pcm,16);memcpy(W+48,&Wave,16);
  Put16(64,1);Put16(66,2);Put32(68,48000);Put32(72,192000);Put16(76,4);Put16(78,16);
  PMINIPORTWAVECYCLICSTREAM S=nullptr;PDMACHANNEL D=nullptr;PSERVICEGROUP GroupOut=nullptr;
  assert(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,(PKSDATAFORMAT)W,&D,&GroupOut)==0);
  assert(GroupOut->AddMember(&Sink)==0&&G.Member);auto Late=G.Member;Late->AddRef();
  if(Case==1)PrepareStatus=STATUS_IO_TIMEOUT;
  if(Case==2)B.Irq.Owner.Service.Gate=1; // model an admitted ISR Notify not yet returned
  NTSTATUS A=S->SetState(KSSTATE_ACQUIRE);
  if(Case==1||Case==2){assert(A==(Case==1?STATUS_IO_TIMEOUT:STATUS_DEVICE_NOT_READY));assert(PrepareCalls==(Case==1?1u:0u));}
  else{
   assert(A==0&&PrepareCalls==1);CheckPaused();assert(S->SetState(KSSTATE_PAUSE)==0);
   if(Case==3)ConfirmStatus=STATUS_IO_TIMEOUT;
   if(Case==4)ConfirmStatus=(NTSTATUS)0x103;
   if(Case==5)Fail=Calls+1;
   if(Case==6)ExecuteStatus=STATUS_DEVICE_DATA_ERROR;
   NTSTATUS Run=S->SetState(KSSTATE_RUN);assert(ReadyCalls==1);
   if(Case>=3&&Case<=6){
    assert(Run==(Case==3?STATUS_IO_TIMEOUT:Case==4?STATUS_DEVICE_NOT_READY:STATUS_DEVICE_DATA_ERROR));
    CheckPaused();auto Count=Calls;assert(S->SetState(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY&&Calls==Count);
    assert(F.Stream&&F.Buffer.Channel&&!P.C.Free); // no rollback/free disguised as success
   }else{
    assert(Run==0);unsigned N=P.Notifications;assert(T70WaveCyclicServiceNotifyConfirmed(CheckedService)&&P.Notifications==N+1);
    Registers[0x5c/4]=0x123400+192;Registers[0x58/4]=0;
    if(Case==7)StoppedStatus=STATUS_IO_TIMEOUT;
    if(Case==8)B.Irq.Owner.Service.Gate=1;
    std::thread Active;
    if(Case>=9){Sink.Entered=false;Sink.Hold=true;Active=std::thread([&]{Late->RequestService();});while(!Sink.Entered.load())std::this_thread::yield();}
    NTSTATUS Pause=S->SetState(KSSTATE_PAUSE);
    if(Case>=9){
     assert(Pause==STATUS_DEVICE_NOT_READY&&StopCalls==0);auto AfterStop=Calls;
     ULONG Position=0xfeed;assert(S->GetPosition(&Position)==STATUS_DEVICE_NOT_READY&&Position==0xfeed);
     assert(S->SetState(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY&&S->SetState(KSSTATE_ACQUIRE)==STATUS_DEVICE_NOT_READY);
     assert(S->SetState(KSSTATE_PAUSE)==STATUS_DEVICE_NOT_READY&&Calls==AfterStop);
     Sink.Hold=false;Active.join();
     if(Case==10){
      D->SetBufferSize(1920);
      assert(S->SetState(KSSTATE_PAUSE)==STATUS_DEVICE_DATA_ERROR&&Calls==AfterStop&&StopCalls==0);
      D->SetBufferSize(3840);assert(S->SetState(KSSTATE_PAUSE)==STATUS_DEVICE_NOT_READY&&Calls==AfterStop);
     }else{
      if(Case==11){
       ExecuteStatus=STATUS_DEVICE_NOT_READY;
       assert(S->SetState(KSSTATE_PAUSE)==STATUS_DEVICE_NOT_READY&&StopCalls==1&&Calls==AfterStop);
       assert(S->GetPosition(&Position)==STATUS_DEVICE_NOT_READY&&Position==0xfeed);
       ExecuteStatus=0;
      }
      assert(S->SetState(KSSTATE_PAUSE)==0&&StopCalls==1);
      for(unsigned I=AfterStop;I<Calls;I++)assert(!(Trace[I]&0x80000000u)); // CUR reads only; STOP is not repeated.
      assert(S->GetPosition(&Position)==0&&Position==192);
      assert(S->SetState(KSSTATE_RUN)==0&&S->SetState(KSSTATE_PAUSE)==0&&StopCalls==2);
     }
    }else
    if(Case==7||Case==8){assert(Pause==(Case==7?STATUS_IO_TIMEOUT:STATUS_DEVICE_NOT_READY));assert(StopCalls==(Case==7?1u:0u));}
    else{assert(Pause==0&&StopCalls==1);CheckPaused();assert(S->SetState(KSSTATE_RUN)==0&&ReadyCalls==2);}
   }
  }
  // Actual export member proxy suppresses originals during settled pause and
  // failed RUN. Early service-Pause failures never claimed callback rundown.
  ULONG BeforeSink=Sink.Calls;Late->RequestService();
  if(Case==0||Case==2||Case==8)assert(Sink.Calls==BeforeSink+1);
  else assert(Sink.Calls==BeforeSink);
  // Complete injected active Notify; production Pause itself does not fake it.
  if(Case==2||Case==8)InterlockedDecrement(&B.Irq.Owner.Service.Gate);
  if(Case!=0)CheckPaused();
  Fail=0;ExecuteStatus=ConfirmStatus=PrepareStatus=StoppedStatus=0;
  assert(T70AudioExportDrainServices(&E)==0);Late->RequestService();Late->Release();assert(Sink.Refs==1);
  S->Release();D->Release();GroupOut->Release();
  assert(B.BeforeIrqClose(B.StreamOwner)==0);B.Irq.Owner.Interrupt=nullptr;
  assert(T70WaveCyclicFactoryClose(&F)==0&&M->Close(Confirm,&P)==0&&M->Release()==0);
  assert(P.Refs==1&&G.Refs==1&&P.C.Free==1);
 }
 puts("Factory service pause/resume: 12 actual service/stream failure and ordering cases");
}
static void ResizeTests(){
 for(unsigned Case=0;Case<15;Case++){
  Port P;P.C.Actual=32768;Group G;T70_AUDIO_EXPORT_OWNER E={};Exports=&E;
  Calls=Fail=0;ConfirmStatus=ExecuteStatus=RemoveStatus=PrepareStatus=StoppedStatus=DrainStatus=0;SkipExecute=false;memset(Registers,0,sizeof Registers);
  T70_WAVECYCLIC_BOARD B={};B.Parent=&P;B.Resources=(PRESOURCELIST)&B;B.Lease=B.Started=true;
  B.Irq.Owner.Interrupt=(PKINTERRUPT)&B;B.Irq.Owner.Service.Port=&P;B.Irq.Owner.Service.Group=&G;
  T70_WAVECYCLIC_FACTORY F={};F.Board=&B;F.Contract={&P,Confirm,Remove,PrepareState,Confirm,StoppedState,Drain,Publish};
  T70WaveCyclicMiniport *M=nullptr;auto Provider=T70WaveCyclicFactoryProvider(&F);
  assert(T70WaveCyclicMiniport::Create(&P,Provider,&M)==0&&M->Init(nullptr,B.Resources,&P)==0);
  unsigned char W[104]={};auto Put16=[&](unsigned A,unsigned V){W[A]=(unsigned char)V;W[A+1]=(unsigned char)(V>>8);};
  auto Put32=[&](unsigned A,uint32_t V){for(unsigned I=0;I<4;I++)W[A+I]=(unsigned char)(V>>(I*8));};
  GUID Audio={STATIC_KSDATAFORMAT_TYPE_AUDIO},Pcm={STATIC_KSDATAFORMAT_SUBTYPE_PCM},Wave={STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX};
  Put32(0,82);memcpy(W+16,&Audio,16);memcpy(W+32,&Pcm,16);memcpy(W+48,&Wave,16);
  Put16(64,1);Put16(66,2);Put32(68,48000);Put32(72,192000);Put16(76,4);Put16(78,16);
  PMINIPORTWAVECYCLICSTREAM S=nullptr;PDMACHANNEL D=nullptr;PSERVICEGROUP GroupOut=nullptr;
  assert(M->NewStream(&S,nullptr,(POOL_TYPE)0,0,FALSE,(PKSDATAFORMAT)W,&D,&GroupOut)==0);
  assert(F.Buffer.Bytes==3840&&D!=F.Buffer.Channel);
  if(Case>=10){assert(S->SetState(KSSTATE_ACQUIRE)==0&&S->SetState(KSSTATE_PAUSE)==0);if(Case>=11)assert(S->SetState(KSSTATE_RUN)==0);}
  const ULONG Sizes[]={1920,3072,192,48,0,16,3841,3856,1936,1920,1920,1920,1920,1920,1920};
  D->SetBufferSize(Sizes[Case]);assert(D->BufferSize()==Sizes[Case]);
  if(Case==9)P.C.Dma=0x100000000LL; // a new device address is never silently adopted
  if(Case==12)Fail=Calls+1;
  if(Case==13)StoppedStatus=STATUS_DEVICE_NOT_READY;
  if(Case==14)P.C.Actual=1024;
  if(Case==0||Case==1){
   ULONG Budget=77;ULONG Interval=S->SetNotificationFreq(10,&Budget);
   assert(Interval>0&&Budget==1920); // request budget fits allocation, not necessarily half the current ring
  }
  auto Before=Calls;ULONG Position=77;
  if(Case<10)assert(S->GetPosition(&Position)==0&&Position==0);
  else assert(S->GetPosition(&Position)==STATUS_DEVICE_DATA_ERROR&&Position==77);
  auto Result=S->SetState(Case>=10?KSSTATE_RUN:KSSTATE_ACQUIRE);
  bool Good=Case<3||Case==8;
  if(Good){
   assert(Result==0&&F.Buffer.Bytes==3840);
   ULONG Span=Registers[0x64/4]-Registers[0x54/4]+1;assert(Span==Sizes[Case]);
   assert(S->SetState(KSSTATE_PAUSE)==0&&S->SetState(KSSTATE_RUN)==0);
   ULONG Period=(Registers[0x300/4]&0x3ffff)*4;
   assert(Period<=1920&&Period>=16&&Span%Period==0&&Span/Period>=2);
   if(Case==2){
    unsigned char BeforeStream[sizeof(T70WaveCyclicStream)];
    memcpy(BeforeStream,static_cast<const void *>(F.Stream),sizeof BeforeStream);
    auto BeforeQuery=Calls;ULONG Budget=77;
    assert(S->SetNotificationFreq(1,&Budget)==1&&Budget==1920);
    assert(Calls==BeforeQuery&&!memcmp(BeforeStream,static_cast<const void *>(F.Stream),sizeof BeforeStream));
   }
   // A later STOP can enlarge the ring within the retained allocation.
   Registers[0x5c/4]=0x123400;assert(S->SetState(KSSTATE_PAUSE)==0&&S->SetState(KSSTATE_ACQUIRE)==0&&S->SetState(KSSTATE_STOP)==0);
   // 192->1936 cannot use period96; the accepted 1920-byte budget must
   // survive the temporary tiny ring and allow a new valid divisor.
   ULONG Regrown=Case==2?1936:3840;
   D->SetBufferSize(Regrown);assert(S->SetState(KSSTATE_ACQUIRE)==0);
   assert(Registers[0x64/4]-Registers[0x54/4]+1==Regrown);
   assert(S->SetState(KSSTATE_PAUSE)==0&&S->SetState(KSSTATE_RUN)==0);
   ULONG RegrownPeriod=(Registers[0x300/4]&0x3ffff)*4;
   assert(RegrownPeriod<=1920&&Regrown%RegrownPeriod==0);
   if(Case==2)assert(RegrownPeriod==968);

  }else{
   assert(Result!=0&&F.Buffer.Uses==1&&!P.C.Free);
   if(Case<11)assert(Calls==Before); // no programming for invalid geometry or pre-RUN mismatch
   else{assert(Calls>Before);assert(S->SetState(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY);}
  }
  Fail=0;StoppedStatus=0;P.C.Actual=32768;P.C.Dma=0x123400;
  S->Release();D->Release();GroupOut->Release();
  assert(B.BeforeIrqClose(B.StreamOwner)==0);B.Irq.Owner.Interrupt=nullptr;
  assert(T70WaveCyclicFactoryClose(&F)==0&&P.C.Free==1);
  assert(M->Close(Confirm,&P)==0&&M->Release()==0&&P.Refs==1&&G.Refs==1);
 }
 Exports=nullptr;puts("WaveCyclic channel resize: 15 actual proxy/factory geometry and failure cases");
}
static void NormalizeTests(){
 const unsigned Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 unsigned Cases=0;
 for(unsigned Rate:Rates)for(unsigned Ch=1;Ch<=2;Ch++){
  Port P;T70_WAVECYCLIC_BUFFER B={};T70WaveCyclicStream *S=nullptr;
  assert(T70WaveCyclicBufferAcquire(&B,&P,192,Ch*2)==0);
  assert(T70WaveCyclicStream::Create(&P,&B,Rate,&S)==0);
  uint64_t Den=(uint64_t)Rate*Ch*2;
  auto Check=[&](uint64_t Bytes){
   assert(Bytes<=(uint64_t)INT64_MAX);LONGLONG Value=(LONGLONG)Bytes,Before=Value;
   unsigned __int128 Expected=(unsigned __int128)Bytes*10000000/Den;
   NTSTATUS Status=S->NormalizePhysicalPosition(&Value);Cases++;
   if(Expected<=(unsigned __int128)INT64_MAX)assert(Status==0&&Value==(LONGLONG)Expected);
   else assert(Status==STATUS_INVALID_PARAMETER&&Value==Before);
  };
  for(uint64_t Bytes=0;Bytes<33;Bytes++)Check(Bytes);
  for(uint64_t Bytes:{Den-1,Den,Den+1,(uint64_t)INT64_MAX-3,(uint64_t)INT64_MAX})Check(Bytes);
  uint64_t Last=(uint64_t)((((unsigned __int128)INT64_MAX+1)*Den-1)/10000000);
  for(uint64_t Bytes:{Last-1,Last,Last+1,Last+2})Check(Bytes);
  uint64_t Seed=Rate*Ch;
  for(unsigned I=0;I<16;I++){Seed=Seed*6364136223846793005ULL+1;Check(Seed&INT64_MAX);}
  Irql=7;Check(3);Irql=0; // documented any-IRQL arithmetic; no pageable or hardware calls
  assert(S->NormalizePhysicalPosition(nullptr)==STATUS_INVALID_PARAMETER);
  LONGLONG Negative=-1;assert(S->NormalizePhysicalPosition(&Negative)==STATUS_INVALID_PARAMETER&&Negative==-1);
  assert(S->Close()==0);LONGLONG Closed=3;
  assert(S->NormalizePhysicalPosition(&Closed)==STATUS_INVALID_DEVICE_STATE&&Closed==3);
  assert(S->Release()==0&&T70WaveCyclicBufferRelease(&B)==0&&P.Refs==1);
 }
 printf("NormalizePhysicalPosition: %u byte/overflow oracle cases\n",Cases);
}
int main(){

 Port P;T70_WAVECYCLIC_BUFFER B={};assert(T70WaveCyclicBufferAcquire(&B,&P,192,4)==0);
 T70WaveCyclicStream *S=nullptr;
 FailAllocation=true;assert(T70WaveCyclicStream::Create(&P,&B,48000,&S)!=0&&!S&&!B.Uses);
 FailAllocation=false;assert(T70WaveCyclicStream::Create(&P,&B,48000,&S)==0&&B.Uses==1&&P.Refs==3);
 assert(T70WaveCyclicBufferRelease(&B)!=0);
 assert(S->SetState(KSSTATE_STOP)==0&&S->SetState(KSSTATE_RUN)!=0);
 ULONG Position=77;assert(S->GetPosition(&Position)==0&&Position==0);
 Irql=DISPATCH_LEVEL;Position=77;assert(S->GetPosition(&Position)==0&&Position==0);
 Irql=7;Position=77;assert(S->GetPosition(&Position)==STATUS_INVALID_PARAMETER&&Position==77);Irql=0;
 ULONG Budget=77;assert(S->SetNotificationFreq(10,&Budget)==0&&Budget==0);
 LONGLONG Time=192000;assert(S->NormalizePhysicalPosition(&Time)==0&&Time==10000000);
 Time=-1;assert(S->NormalizePhysicalPosition(&Time)!=0&&Time==-1);
 Time=3;assert(S->NormalizePhysicalPosition(&Time)==0&&Time==156);
 Time=INT64_MAX-3;auto Saved=Time;assert(S->NormalizePhysicalPosition(&Time)!=0&&Time==Saved);
 unsigned char Silence[7];memset(Silence,0xaa,7);S->Silence(Silence,7);for(auto V:Silence)assert(V==0);
 GUID Id={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};void *Q=nullptr;
 assert(S->QueryInterface(Id,&Q)==0&&Q==S);((IUnknown *)Q)->Release();
 assert(S->Close()==0&&!B.Uses);assert(S->Close()==0);assert(S->SetState(KSSTATE_STOP)!=0);
 assert(S->Release()==0&&Allocations==Frees&&P.Refs==2);
 assert(T70WaveCyclicStream::Create(&P,&B,48000,&S)==0);
 assert(S->Release()==1&&B.Uses==1); // only the recovery/self reference remains
 assert(S->Close()==0&&B.Uses==0&&Allocations==Frees);
 assert(T70WaveCyclicBufferRelease(&B)==0&&P.Refs==1);
 for(unsigned Error=0;Error<=12;Error++){
  Port Q;T70_WAVECYCLIC_BUFFER V={};T70WaveCyclicStream *Stream=nullptr;
  assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==0);
  assert(T70WaveCyclicStream::Create(&Q,&V,48000,&Stream)==0);
  Calls=0;Fail=Error;ConfirmStatus=0;RemoveStatus=STATUS_DEVICE_NOT_READY;memset(Registers,0,sizeof Registers);
  T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_CONFIG_RESULT C={};
  auto Result=Stream->ConfigureDl1(Io,Confirm,Remove,nullptr,Execute,&Q,&C);
  assert((Result==0)==(Error==0)&&Calls==(Error?Error:12));
  assert(Stream->SetState(KSSTATE_STOP)!=0&&Stream->Close()!=0&&V.Uses==1);
  assert(T70WaveCyclicBufferRelease(&V)!=0);RemoveStatus=0;
  assert(Stream->Close()==0&&Stream->Release()==0&&T70WaveCyclicBufferRelease(&V)==0);
 }
 for(unsigned Error=0;Error<=10;Error++){
  Port Q;T70_WAVECYCLIC_BUFFER V={};T70WaveCyclicStream *Stream=nullptr;
  assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==0&&T70WaveCyclicStream::Create(&Q,&V,48000,&Stream)==0);
  Calls=Fail=0;ConfirmStatus=RemoveStatus=0;memset(Registers,0,sizeof Registers);
  T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_CONFIG_RESULT C={};
  assert(Stream->ConfigureDl1(Io,Confirm,Remove,nullptr,Execute,&Q,&C)==0);
  Calls=0;Fail=Error==10?0:Error;if(Error==10)Registers[0x1b04/4]=2;T70_AFE_START_RESULT R={};
  auto Result=Stream->RequestStartDl1(Confirm,nullptr,&R);
  assert((Result==0)==(Error==0)&&Calls==(Error==10?1:Error?Error:9));
  if(!Error){const uint32_t Expected[]={0x1b04,0x80001b04,0x1b00,0x300,0x80000300,0x2e4,0x800002e4,0x3a0,0x800003a0};assert(!memcmp(Trace,Expected,sizeof Expected));}
  auto N=Calls;assert(Stream->RequestStartDl1(Confirm,nullptr,&R)!=0&&Calls==N);
  assert(Stream->SetState(KSSTATE_RUN)!=0); // callbacks do not establish playback
  assert(Stream->Close()==0&&Stream->Release()==0&&T70WaveCyclicBufferRelease(&V)==0);
 }
 for(LONGLONG Dma:{(LONGLONG)-1,(LONGLONG)0x100000000ULL,(LONGLONG)0xfffffff0}){
  Port Q;Q.C.Dma=Dma;T70_WAVECYCLIC_BUFFER V={};T70WaveCyclicStream *Stream=nullptr;
  assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==0&&T70WaveCyclicStream::Create(&Q,&V,48000,&Stream)==0);
  Calls=Fail=0;ConfirmStatus=0;RemoveStatus=STATUS_DEVICE_NOT_READY;
  T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_CONFIG_RESULT C={};
  assert(Stream->ConfigureDl1(Io,Confirm,Remove,nullptr,Execute,&Q,&C)!=0&&!Calls);
  assert(Stream->Close()==0&&Stream->Release()==0&&T70WaveCyclicBufferRelease(&V)==0);
 }
 for(unsigned Case=0;Case<5;Case++){
  Port Q;T70_WAVECYCLIC_BUFFER V={};T70WaveCyclicStream *Stream=nullptr;
  assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==0&&T70WaveCyclicStream::Create(&Q,&V,48000,&Stream)==0);
  Calls=Fail=0;ConfirmStatus=Case==0?0x103:0;ExecuteStatus=Case==1?0x103:Case==2?STATUS_DEVICE_DATA_ERROR:0;SkipExecute=Case==3;RemoveStatus=0;
  memset(Registers,0,sizeof Registers);T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_CONFIG_RESULT C={};
  auto Result=Stream->ConfigureDl1(Io,Confirm,Remove,nullptr,Execute,&Q,&C);
  if(Case<4)assert(Result!=0&&!Calls);
  else {assert(Result==0);Calls=0;ConfirmStatus=0x103;T70_AFE_START_RESULT Start={};
   assert(Stream->RequestStartDl1(Confirm,nullptr,&Start)==STATUS_DEVICE_NOT_READY&&!Calls);
   ConfirmStatus=0;assert(Stream->RequestStartDl1(Confirm,nullptr,&Start)==0&&Calls==9);}
  ExecuteStatus=0;SkipExecute=false;
  assert(Stream->Close()==0&&Stream->Release()==0&&T70WaveCyclicBufferRelease(&V)==0);
 }
 {T70_AFE_PREPARED Plan;assert(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x123400,192,96,&Plan));
  memset(Registers,0,sizeof Registers);Registers[0x1b04/4]=0x10;Registers[0x300/4]=24;Registers[0x2e4/4]=Plan.Format.RateCode;Registers[0x3a0/4]=1;
  Calls=Fail=0;T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};auto R=T70AfeDl1RequestStart(&Plan,&Io);
  assert(R.Completed&&R.Reads==5&&!R.Writes&&Calls==5); // source-value semantics only; not valid fresh RUN admission
 }

 {Port Q;T70_WAVECYCLIC_BUFFER V={};T70WaveCyclicStream *Stream=nullptr;
  assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==0&&T70WaveCyclicStream::Create(&Q,&V,48000,&Stream)==0);
  Calls=Fail=0;ConfirmStatus=ExecuteStatus=RemoveStatus=0;SkipExecute=false;memset(Registers,0,sizeof Registers);
  T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_CONFIG_RESULT C={};
  assert(Stream->ConfigureDl1(Io,Confirm,Remove,nullptr,Execute,&Q,&C)==0);
  for(unsigned Offset=0;Offset<192;Offset++){
   Calls=0;Registers[0x5c/4]=0x123400+Offset;Registers[0x58/4]=0;
   T70_AFE_POSITION P={999,999};assert(Stream->ObserveDmaPosition(&P)==0&&P.ByteOffset==(Offset&~15u)&&P.FrameOffset==P.ByteOffset/4&&Calls==2&&Trace[0]==0x5c&&Trace[1]==0x58);
  }
  for(unsigned Bad=0;Bad<5;Bad++){
   Calls=0;Fail=Bad<2?Bad+1:0;Registers[0x5c/4]=Bad==3?0x1233ff:Bad==4?0x1234c0:0x123400;Registers[0x58/4]=Bad==2?1:0;
   T70_AFE_POSITION P={999,999};assert(Stream->ObserveDmaPosition(&P)!=0&&P.ByteOffset==999&&P.FrameOffset==999);
  }
  Fail=0;Registers[0x58/4]=0;Registers[0x5c/4]=0x1234bf;T70_AFE_POSITION P={};Calls=0;
  assert(Stream->ObserveDmaPosition(&P)==0&&P.ByteOffset==176);Registers[0x5c/4]=0x123400;assert(Stream->ObserveDmaPosition(&P)==0&&!P.ByteOffset);
  ULONG Play=77;Calls=0;assert(Stream->GetPosition(&Play)==0&&!Play&&!Calls);
  assert(Stream->BindPositionExecutor(PositionExecute,&Q)==0);
  T70_AFE_START_RESULT Start={};assert(Stream->RequestStartDl1(Confirm,nullptr,&Start)==0);
  Calls=0;Registers[0x5c/4]=0x12343f;Play=77;assert(Stream->GetPosition(&Play)==0&&Play==48&&Calls==2);
  Irql=DISPATCH_LEVEL;Calls=0;Registers[0x5c/4]=0x123400;assert(Stream->GetPosition(&Play)==0&&!Play&&Calls==2&&Irql==DISPATCH_LEVEL);Calls=0;assert(Stream->ObserveDmaPosition(&P)==STATUS_INVALID_PARAMETER&&!Calls);Irql=0;
  Registers[0x58/4]=1;Play=77;assert(Stream->GetPosition(&Play)==STATUS_DEVICE_DATA_ERROR&&Play==77);
  assert(Stream->Close()==0&&Stream->Release()==0&&T70WaveCyclicBufferRelease(&V)==0);
 }
 {T70_AFE_PREPARED P;T70_AFE_FORMAT_IO I={Registers,ReadReg,WriteReg};T70_AFE_POSITION V={99,99};
  assert(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0xffffff40,192,96,&P));
  Calls=Fail=0;Registers[0x5c/4]=0xffffffff;Registers[0x58/4]=0;
  assert(T70AfeDl1ReadCur32(&P,&I,&V)==0&&V.ByteOffset==176);
  assert(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0xfffffff0,192,96,&P));Calls=0;V={99,99};
  assert(T70AfeDl1ReadCur32(&P,&I,&V)==STATUS_INVALID_PARAMETER&&!Calls&&V.ByteOffset==99);
 }

 for(unsigned Error=0;Error<=11;Error++){
  Port Q;T70_WAVECYCLIC_BUFFER V={};T70WaveCyclicStream *Stream=nullptr;
  assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==0&&T70WaveCyclicStream::Create(&Q,&V,48000,&Stream)==0);
  Calls=Fail=Prepared=Stopped=0;ConfirmStatus=ExecuteStatus=RemoveStatus=PrepareStatus=StoppedStatus=0;SkipExecute=false;memset(Registers,0,sizeof Registers);
  T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_CONFIG_RESULT Config={};
  assert(Stream->ConfigureDl1(Io,Confirm,Remove,nullptr,Execute,&Q,&Config)==0);
  assert(Stream->BindPositionExecutor(PositionExecute,&Q)==0);
  assert(Stream->BindStateControl(&Q,PrepareState,Confirm,StoppedState)==0);
  auto N=Calls;assert(Stream->SetState(KSSTATE_RUN)==STATUS_INVALID_DEVICE_STATE&&Calls==N);
  assert(Stream->SetState(KSSTATE_STOP)==0&&Stream->SetState(KSSTATE_ACQUIRE)==0&&Prepared==1);
  assert(Stream->SetState(KSSTATE_PAUSE)==0&&Stream->SetState(KSSTATE_RUN)==0);
  T70_AFE_START_RESULT Start={};assert(Stream->RequestStartDl1(Confirm,nullptr,&Start)==STATUS_INVALID_DEVICE_STATE);
  Calls=0;Registers[0x5c/4]=0x12343f;ULONG Position=999;
  assert(Stream->GetPosition(&Position)==0&&Position==48);
  Calls=0;Fail=Error<=8?Error:0;StoppedStatus=Error==9?0x103:Error==10?STATUS_DEVICE_DATA_ERROR:0;
  if(Error==11)Registers[0x1b08/4]=2;
  auto Status=Stream->SetState(KSSTATE_PAUSE);
  if(Error){assert(Status!=0);Position=999;assert(Stream->GetPosition(&Position)==STATUS_DEVICE_NOT_READY&&Position==999);assert(Stream->SetState(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY);
   RemoveStatus=STATUS_DEVICE_NOT_READY;assert(Stream->Close()!=0&&V.Uses==1);RemoveStatus=0;
  }else{
   assert(Status==0&&Stopped==1);Registers[0x5c/4]=0x123480;Calls=0;
   assert(Stream->GetPosition(&Position)==0&&Position==48&&!Calls);
   Registers[0x5c/4]=0x12343f;assert(Stream->SetState(KSSTATE_RUN)==0&&Prepared==1);
   assert(Stream->GetPosition(&Position)==0&&Position==48);assert(Stream->SetState(KSSTATE_PAUSE)==0);
   assert(Stream->SetState(KSSTATE_ACQUIRE)==0&&Stream->GetPosition(&Position)==0&&Position==48);
   assert(Stream->SetState(KSSTATE_STOP)==0&&Stream->GetPosition(&Position)==0&&!Position);
   assert(Stream->SetState(KSSTATE_ACQUIRE)==0&&Prepared==2&&Stream->SetState(KSSTATE_PAUSE)==0&&Stream->SetState(KSSTATE_RUN)==0);
   assert(Stream->GetPosition(&Position)==0&&!Position);
  }
  Fail=0;StoppedStatus=0;assert(Stream->Close()==0&&Stream->Release()==0&&T70WaveCyclicBufferRelease(&V)==0);
 }

 for(unsigned Error=0;Error<28;Error++){
  Port Q;T70_WAVECYCLIC_BUFFER V={};T70WaveCyclicStream *Stream=nullptr;
  assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==0&&T70WaveCyclicStream::Create(&Q,&V,48000,&Stream)==0);
  Calls=Fail=0;ConfirmStatus=ExecuteStatus=RemoveStatus=PrepareStatus=StoppedStatus=0;SkipExecute=false;memset(Registers,0,sizeof Registers);
  T70_AFE_FORMAT_IO Io={Registers,ReadReg,WriteReg};T70_AFE_CONFIG_RESULT Config={};
  assert(Stream->ConfigureDl1(Io,Confirm,Remove,nullptr,Execute,&Q,&Config)==0&&Stream->BindPositionExecutor(PositionExecute,&Q)==0&&Stream->BindStateControl(&Q,PrepareState,Confirm,StoppedState)==0);
  Calls=0;
  if(Error<12)Fail=Error+1;
  if(Error>=24){Registers[0x4c/4]|=0xf03000;Fail=13+Error-24;}
  if(Error==12)PrepareStatus=0x103;if(Error==13)PrepareStatus=STATUS_DEVICE_DATA_ERROR;
  auto Status=Stream->SetState(KSSTATE_ACQUIRE);
  if(Error>=14&&Error<24){assert(Status==0&&Stream->SetState(KSSTATE_PAUSE)==0);ConfirmStatus=Error==14?0x103:0;Calls=0;Fail=Error==14?0:Error-14;Status=Stream->SetState(KSSTATE_RUN);if(Error==14)assert(!Calls);else assert(Calls==Error-14);}
  assert(Status!=0&&Stream->SetState(KSSTATE_RUN)==STATUS_DEVICE_NOT_READY);
  RemoveStatus=STATUS_DEVICE_NOT_READY;assert(Stream->Close()!=0&&V.Uses==1);RemoveStatus=0;Fail=0;
  assert(Stream->Close()==0&&Stream->Release()==0&&T70WaveCyclicBufferRelease(&V)==0);
 }
 ResizeTests();
 NormalizeTests();
 PrefetchTests();
 FactoryTests();ServiceFactoryTests();
 puts("WaveCyclic concrete stream COM, lease, failure and PCM methods passed");
}
