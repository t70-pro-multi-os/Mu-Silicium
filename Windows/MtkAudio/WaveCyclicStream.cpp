// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicStream.h"
extern "C" {
#include "WaveRtFormat.h"
}
#include <string.h>
static constexpr ULONG Tag=0x53433754;
static const GUID UnknownId={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID StreamId={0xb4c90a28,0x5791,0x11d0,{0x86,0xf9,0,0xa0,0xc9,0x11,0xb5,0x44}};
// Buffer.Bytes owns the allocation; Channel.BufferSize is the current DMA ring.
// The owner serializes SetBufferSize with stream calls. Neither changes ownership.
static bool RingSize(const T70_WAVECYCLIC_BUFFER *B,uint64_t Base,ULONG Frame,ULONG *Size){
 if(!B||!B->Channel||!B->Cpu||!Frame)return false;
 auto C=B->Channel;ULONG N=C->BufferSize();auto A=C->PhysicalAddress();
 if(!N||N>B->Bytes||N%16||N%Frame||C->AllocatedBufferSize()<B->Bytes||
    C->MaximumBufferSize()<N||C->SystemAddress()!=B->Cpu||A.QuadPart<0||
    (uint64_t)A.QuadPart!=Base||Base>UINT32_MAX||N-1>UINT32_MAX-Base)return false;
 *Size=N;return true;
}
static bool SelectPeriod(ULONG Rate,ULONG Frame,uint64_t Base,ULONG Ring,uint64_t Wanted,T70_AFE_PREPARED *Out,bool NoSlower=false){
 uint64_t Best=0,Distance=UINT64_MAX;
 for(uint64_t K=2;K<=256;K++){
  if(Ring%K)continue;uint64_t Bytes=Ring/K;if(NoSlower&&Bytes>Wanted)continue;T70_AFE_PREPARED P;
  if(!T70AfeDl1Prepare(Rate,T70AfeS16Le,Frame/2,Base,Ring,Bytes,&P))continue;
  uint64_t D=Bytes>Wanted?Bytes-Wanted:Wanted-Bytes;
  if(D<Distance){Distance=D;Best=Bytes;}
 }
 return Best&&T70AfeDl1Prepare(Rate,T70AfeS16Le,Frame/2,Base,Ring,Best,Out);
}
T70WaveCyclicStream::T70WaveCyclicStream(PUNKNOWN P,T70_WAVECYCLIC_BUFFER *B,ULONG R)
 :Refs(2),Owner(P),Buffer(B),Rate(R),Frame(B->FrameBytes),Closed(false),NotificationBudget(0),HardwarePlan{},HardwareIo{},RemoveReferences(nullptr),HardwareContext(nullptr),ExecuteIo(nullptr),ExecuteContext(nullptr),PositionIo(nullptr),PositionContext(nullptr),HardwareTouched(false),Configured(false),StartAttempted(false),StartCompleted(false),StateContext(nullptr),PrepareFromStop(nullptr),ReadyToRun(nullptr),ConfirmStopped(nullptr),AbortRun(nullptr),State(KSSTATE_STOP),FrozenPosition(0),StateBound(false),StateFault(false),PausePending(false),PauseDrained(false){P->AddRef();}
NTSTATUS T70WaveCyclicStream::Create(PUNKNOWN P,T70_WAVECYCLIC_BUFFER *B,ULONG R,T70WaveCyclicStream **Out){
 if(!P||!B||!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 T70_AFE_PREPARED Plan;
 if(!T70AfeDl1Prepare(R,T70AfeS16Le,B->FrameBytes/2,0,B->Bytes,B->Bytes/2,&Plan))return STATUS_NOT_SUPPORTED;
 NTSTATUS S=T70WaveCyclicBufferBegin(B);if(S!=STATUS_SUCCESS)return S;
 void *M=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(T70WaveCyclicStream),Tag);
 if(!M){T70WaveCyclicBufferEnd(B);return STATUS_INSUFFICIENT_RESOURCES;}
 *Out=new(M) T70WaveCyclicStream(P,B,R);return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicStream::QueryInterface(REFIID I,void **O){
 if(!O)return STATUS_INVALID_PARAMETER;*O=nullptr;
 if(memcmp(&I,&UnknownId,sizeof I)&&memcmp(&I,&StreamId,sizeof I))return STATUS_NOINTERFACE;
 *O=static_cast<IMiniportWaveCyclicStream *>(this);AddRef();return STATUS_SUCCESS;
}
ULONG T70WaveCyclicStream::AddRef(){return (ULONG)InterlockedIncrement(&Refs);}
ULONG T70WaveCyclicStream::Release(){
 LONG N=InterlockedDecrement(&Refs);if(!N){auto P=Owner;this->~T70WaveCyclicStream();ExFreePoolWithTag(this,Tag);P->Release();}return (ULONG)N;
}
NTSTATUS T70WaveCyclicStream::Close(){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Closed)return STATUS_SUCCESS;
 // Destruction is not PAUSE: no resume position needs to survive. The actual
 // factory RemoveReferences performs stock STOP and CPU-user rundown directly.
 // Requiring PAUSE's CUR snapshot/provider here could prevent normal removal.

 if(HardwareTouched){
  NTSTATUS R=RemoveReferences(HardwareContext);if(R!=STATUS_SUCCESS)return NT_SUCCESS(R)?STATUS_DEVICE_NOT_READY:R;
  HardwareTouched=false;Configured=false;
 }
 NTSTATUS S=T70WaveCyclicBufferEnd(Buffer);if(S!=STATUS_SUCCESS)return S;
 Buffer=nullptr;Closed=true;Release();return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicStream::SetFormat(PKSDATAFORMAT F){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!F)return STATUS_INVALID_PARAMETER;
 if(Closed)return STATUS_INVALID_DEVICE_STATE;
 T70_AFE_PREPARED P;
 if((F->FormatSize!=82&&F->FormatSize!=104)||T70WaveRtPrepareBufferFormat(F,F->FormatSize,0,Buffer->Bytes,Buffer->Bytes/2,&P)!=T70WavePrepared||
 P.Rate!=Rate||P.Channels*2!=Frame)return STATUS_NOT_SUPPORTED;
 return STATUS_SUCCESS;
}
ULONG T70WaveCyclicStream::SetNotificationFreq(ULONG Interval,PULONG Size){
 if(!Size)return 0;*Size=0;
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||Closed||!StateBound||StateFault||PausePending||
    !Interval)return 0;
 // Choose an integral ring divisor. IRQ count is programmed from this plan
 // on the next RUN, not by a timer or while DMA is consuming the buffer.
 ULONG Ring;if(!RingSize(Buffer,HardwarePlan.DmaBase,Frame,&Ring))return 0;
 uint64_t Wanted=((uint64_t)Rate*Interval+999)/1000*Frame;
 if(Wanted>Buffer->Bytes/2)return 0;
 T70_AFE_PREPARED P;
 if(!SelectPeriod(Rate,Frame,HardwarePlan.DmaBase,Ring,Wanted,&P))return 0;
 uint64_t Best=P.PeriodBytes;
 // Outside STOP only an idempotent request for the already selected period
 // is supported. Do not change a live or paused plan, including prefetch timing.
 // Different periods still return zero; live cadence changes are not provided.
 if(State==KSSTATE_STOP){HardwarePlan=P;NotificationBudget=Best>Wanted?Best:Wanted;}
 else if(Best!=HardwarePlan.PeriodBytes||Ring!=HardwarePlan.BufferBytes)return 0;
 // Include both requested-interval bytes and the rounded actual IRQ period.
 uint64_t Budget=Best>Wanted?Best:Wanted;
 // Idempotent live queries cannot reduce the saved budget: a later STOP
 // resize may select a longer period within that existing commitment.
 if(State!=KSSTATE_STOP&&Budget<NotificationBudget)Budget=NotificationBudget;
 *Size=(ULONG)Budget;
 return (ULONG)((Best*1000+Rate*Frame-1)/(Rate*Frame));
}
NTSTATUS T70WaveCyclicStream::SetState(KSSTATE S){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Closed)return STATUS_INVALID_DEVICE_STATE;
 if(S<KSSTATE_STOP||S>KSSTATE_RUN)return STATUS_INVALID_PARAMETER;
 if(StateBound)return Transition(S);
 return S==KSSTATE_STOP&&!HardwareTouched?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}
NTSTATUS T70WaveCyclicStream::GetPosition(PULONG P){
 if(!P||KeGetCurrentIrql()>DISPATCH_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Closed)return STATUS_INVALID_DEVICE_STATE;
 if(StateBound){if(StateFault||PausePending)return STATUS_DEVICE_NOT_READY;if(State==KSSTATE_STOP){*P=0;return STATUS_SUCCESS;}}
 if(Configured){ULONG Ring;if(!RingSize(Buffer,HardwarePlan.DmaBase,Frame,&Ring)||Ring!=HardwarePlan.BufferBytes)return STATUS_DEVICE_DATA_ERROR;}
 if(StateBound){if(StateFault)return STATUS_DEVICE_NOT_READY;if(State!=KSSTATE_RUN){*P=FrozenPosition;return STATUS_SUCCESS;}}
 if(!HardwareTouched||(Configured&&!StartAttempted)){*P=0;return STATUS_SUCCESS;}
 if(!StartCompleted||!PositionIo)return STATUS_DEVICE_NOT_READY;
 struct Observation {T70_AFE_PREPARED *Plan;T70_AFE_FORMAT_IO *Io;T70_AFE_POSITION Position;bool Ran;};
 Observation O={&HardwarePlan,&HardwareIo,{},false};
 NTSTATUS S=PositionIo(PositionContext,[](void *C)->NTSTATUS{
  auto X=(Observation *)C;X->Ran=true;return T70AfeDl1ReadCur32(X->Plan,X->Io,&X->Position);
 },&O);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(!O.Ran)return STATUS_DEVICE_NOT_READY;
 // Stock-derived uncompensated estimate. DMA prefetch/FIFO/codec may make it
 // lead actual sound; no undocumented delay is subtracted or invented.
 *P=O.Position.ByteOffset;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicStream::NormalizePhysicalPosition(PLONGLONG P){
 if(!P||*P<0)return STATUS_INVALID_PARAMETER;
 if(Closed)return STATUS_INVALID_DEVICE_STATE;
 uint64_t B=(uint64_t)*P,Den=(uint64_t)Rate*Frame;
 // PortCls supplies a byte count, not necessarily a complete PCM frame.
 // Preserve sub-frame time until the final integer 100ns truncation.
 uint64_t Q=B/Den,R=B%Den;
 if(Q>((uint64_t)INT64_MAX-R*10000000/Den)/10000000)return STATUS_INVALID_PARAMETER;
 *P=(LONGLONG)(Q*10000000+R*10000000/Den);return STATUS_SUCCESS;
}
void T70WaveCyclicStream::Silence(void *P,ULONG N){
 // S16 signed PCM. Alignment-safe for RAM-backed Device Memory as well.
 // No cache publication or peripheral-register access is implied.
 const unsigned char Zero[32]={};
 while(P&&N){ULONG K=N>sizeof(Zero)?sizeof(Zero):N;
  RtlCopyDeviceMemory(P,Zero,K);P=(unsigned char *)P+K;N-=K;}

}

NTSTATUS T70WaveCyclicStream::ConfigureDl1(const T70_AFE_FORMAT_IO &Io,NTSTATUS (*Access)(void *),
 NTSTATUS (*Remove)(void *),void *Context,NTSTATUS (*Execute)(void *,NTSTATUS (*)(void *),void *),void *ExecContext,T70_AFE_CONFIG_RESULT *Out){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Out||!Access||!Remove||!Execute||!ExecContext||!Io.Context||!Io.Read32||!Io.Write32)return STATUS_INVALID_PARAMETER;
 if(Closed||HardwareTouched)return STATUS_INVALID_DEVICE_STATE;
 if(Buffer->Channel->MaximumBufferSize()<Buffer->Bytes||Buffer->Channel->AllocatedBufferSize()<Buffer->Bytes||
    Buffer->Channel->BufferSize()!=Buffer->Bytes)return STATUS_DEVICE_DATA_ERROR;
 auto Address=Buffer->Channel->PhysicalAddress();T70_AFE_PREPARED P;
 if(Address.QuadPart<0||(uint64_t)Address.QuadPart>UINT32_MAX||
    Buffer->Bytes-1>UINT32_MAX-(uint64_t)Address.QuadPart||
    !T70AfeDl1Prepare(Rate,T70AfeS16Le,Frame/2,(uint64_t)Address.QuadPart,Buffer->Bytes,Buffer->Bytes/2,&P))return STATUS_DEVICE_DATA_ERROR;
 NTSTATUS S=Access(Context);if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 HardwarePlan=P;NotificationBudget=P.PeriodBytes;HardwareIo=Io;RemoveReferences=Remove;HardwareContext=Context;
 ExecuteIo=Execute;ExecuteContext=ExecContext;HardwareTouched=true;
 struct Transaction {T70_AFE_PREPARED *Plan;T70_AFE_FORMAT_IO *Io;T70_AFE_CONFIG_RESULT R;bool Ran;};
 Transaction T={&HardwarePlan,&HardwareIo,{},false};
 S=ExecuteIo(ExecuteContext,[](void *C)->NTSTATUS{auto X=(Transaction *)C;X->Ran=true;X->R=T70AfeDl1Configure(X->Plan,X->Io);return STATUS_SUCCESS;},&T);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(!T.Ran)return STATUS_DEVICE_NOT_READY;auto R=T.R;*Out=R;
 Configured=R.Code==T70AfeConfigCallbacksCompleted;
 if(Configured)return STATUS_SUCCESS;
 int32_t Error=R.AddressStarted&&R.Code==T70AfeConfigAddressFailed?R.Address.TransportStatus:R.Format.TransportStatus;
 return Error<0?(NTSTATUS)Error:STATUS_DEVICE_NOT_READY;
}
NTSTATUS T70WaveCyclicStream::RequestStartCore(NTSTATUS (*Ready)(void *),void *Context,T70_AFE_START_RESULT *Out){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Ready||!Out)return STATUS_INVALID_PARAMETER;
 if(Closed||!Configured||StartAttempted)return STATUS_INVALID_DEVICE_STATE;
 ULONG Ring;if(!RingSize(Buffer,HardwarePlan.DmaBase,Frame,&Ring)||Ring!=HardwarePlan.BufferBytes)return STATUS_DEVICE_DATA_ERROR;
 NTSTATUS S=Ready(Context);if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 StartAttempted=true;
 struct Transaction {T70_AFE_PREPARED *Plan;T70_AFE_FORMAT_IO *Io;T70_AFE_START_RESULT R;bool Ran;};
 Transaction T={&HardwarePlan,&HardwareIo,{},false};
 S=ExecuteIo(ExecuteContext,[](void *C)->NTSTATUS{auto X=(Transaction *)C;X->Ran=true;X->R=T70AfeDl1RequestStart(X->Plan,X->Io);return STATUS_SUCCESS;},&T);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(!T.Ran)return STATUS_DEVICE_NOT_READY;auto R=T.R;*Out=R;
 StartCompleted=R.Completed!=0;
 return R.Completed?STATUS_SUCCESS:(R.Status<0?(NTSTATUS)R.Status:STATUS_DEVICE_NOT_READY);
}

NTSTATUS T70WaveCyclicStream::ObserveDmaPosition(T70_AFE_POSITION *Out){
 if(!Out||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Closed||!Configured)return STATUS_INVALID_DEVICE_STATE;
 struct Observation {T70_AFE_PREPARED *Plan;T70_AFE_FORMAT_IO *Io;T70_AFE_POSITION Position;bool Ran;};
 Observation O={&HardwarePlan,&HardwareIo,{},false};
 NTSTATUS S=ExecuteIo(ExecuteContext,[](void *C)->NTSTATUS{
  auto X=(Observation *)C;X->Ran=true;return T70AfeDl1ReadCur32(X->Plan,X->Io,&X->Position);
 },&O);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(!O.Ran)return STATUS_DEVICE_NOT_READY;*Out=O.Position;return STATUS_SUCCESS;
}

NTSTATUS T70WaveCyclicStream::BindPositionExecutor(NTSTATUS (*Function)(void *,NTSTATUS (*)(void *),void *),void *Context){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Function||!Context)return STATUS_INVALID_PARAMETER;
 if(Closed||!Configured||StartAttempted||PositionIo)return STATUS_INVALID_DEVICE_STATE;
 PositionIo=Function;PositionContext=Context;return STATUS_SUCCESS;
}

NTSTATUS T70WaveCyclicStream::RequestStartDl1(NTSTATUS (*Ready)(void *),void *Context,T70_AFE_START_RESULT *Out){
 if(StateBound)return STATUS_INVALID_DEVICE_STATE;return RequestStartCore(Ready,Context,Out);
}
NTSTATUS T70WaveCyclicStream::BindStateControl(void *Context,NTSTATUS (*Prepare)(void *),NTSTATUS (*Ready)(void *),NTSTATUS (*Stopped)(void *),NTSTATUS (*Abort)(void *)){
 if(KeGetCurrentIrql()!=PASSIVE_LEVEL||!Context||!Prepare||!Ready||!Stopped)return STATUS_INVALID_PARAMETER;
 if(Closed||!Configured||!PositionIo||StartAttempted||StateBound)return STATUS_INVALID_DEVICE_STATE;
 StateContext=Context;PrepareFromStop=Prepare;ReadyToRun=Ready;ConfirmStopped=Stopped;AbortRun=Abort;StateBound=true;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicStream::FinishPause(){
 NTSTATUS S=PauseDrained?STATUS_SUCCESS:ConfirmStopped(StateContext);
 if(S==STATUS_SUCCESS)PauseDrained=true;
 T70_AFE_POSITION P={};
 if(S==STATUS_SUCCESS)S=ObserveDmaPosition(&P);
 if(S==STATUS_DEVICE_NOT_READY)return S; // Explicit nonblocking rundown retry.
 if(S!=STATUS_SUCCESS){StateFault=true;return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 FrozenPosition=P.ByteOffset;StartAttempted=false;StartCompleted=false;
 PausePending=false;State=KSSTATE_PAUSE;return STATUS_SUCCESS;
}
NTSTATUS T70WaveCyclicStream::Transition(KSSTATE Next){
 if(StateFault)return STATUS_DEVICE_NOT_READY;
 ULONG Ring=0;bool Valid=RingSize(Buffer,HardwarePlan.DmaBase,Frame,&Ring);
 if(State!=KSSTATE_STOP&&(!Valid||Ring!=HardwarePlan.BufferBytes)){
  // The public channel changed after preparation. Do not reprogram a live
  // ring or pretend the old END remains compatible. STOP at this safe entry;
  // setter-time interception requires the separate channel/stream owner.
  NTSTATUS Stop=STATUS_SUCCESS;
  if(State==KSSTATE_RUN&&!PausePending){
   struct Work {T70_AFE_FORMAT_IO *Io;bool Done;};Work W={&HardwareIo,false};
   Stop=ExecuteIo(ExecuteContext,[](void *C)->NTSTATUS{auto W=(Work *)C;auto R=T70AfeDl1RequestStop(W->Io);W->Done=R.Completed!=0;
    return W->Done?STATUS_SUCCESS:(R.Status<0?(NTSTATUS)R.Status:STATUS_DEVICE_NOT_READY);},&W);
   if(Stop==STATUS_SUCCESS&&!W.Done)Stop=STATUS_DEVICE_NOT_READY;
   if(Stop==STATUS_SUCCESS)Stop=ConfirmStopped(StateContext);
   if(Stop==STATUS_SUCCESS){StartAttempted=false;StartCompleted=false;}
  }
  StateFault=true;return Stop==STATUS_SUCCESS?STATUS_DEVICE_DATA_ERROR:(NT_SUCCESS(Stop)?STATUS_DEVICE_NOT_READY:Stop);
 }
 if(PausePending)return Next==KSSTATE_PAUSE?FinishPause():STATUS_DEVICE_NOT_READY;
 if(Next==State)return STATUS_SUCCESS;
 if((int)Next-(int)State!=1&&(int)State-(int)Next!=1)return STATUS_INVALID_DEVICE_STATE;
 NTSTATUS S=STATUS_SUCCESS;
 if(State==KSSTATE_STOP){
  T70_AFE_PREPARED P;
  // Preserve the last accepted notification budget across ring resizes.
  // A tiny ring may select a smaller period without permanently lowering
  // that budget; never exceed the previously returned byte budget.
  if(!Valid||!SelectPeriod(Rate,Frame,HardwarePlan.DmaBase,Ring,NotificationBudget,&P,true))return STATUS_DEVICE_DATA_ERROR;
  HardwarePlan=P;
  S=PrepareFromStop(StateContext);
  if(S==STATUS_SUCCESS){
   struct Work {T70_AFE_PREPARED *Plan;T70_AFE_FORMAT_IO *Io;bool Done;};Work W={&HardwarePlan,&HardwareIo,false};
   S=ExecuteIo(ExecuteContext,[](void *C)->NTSTATUS{auto W=(Work *)C;auto R=T70AfeDl1Configure(W->Plan,W->Io);
    W->Done=R.Code==T70AfeConfigCallbacksCompleted;
    if(W->Done){NTSTATUS S=T70AfeDl1PreparePrefetch(W->Plan,W->Io);W->Done=S==0;return S;}
    int32_t Error=R.Code==T70AfeConfigAddressFailed?R.Address.TransportStatus:R.Format.TransportStatus;
    return Error<0?(NTSTATUS)Error:STATUS_DEVICE_NOT_READY;
   },&W);
   if(S==STATUS_SUCCESS&&!W.Done)S=STATUS_DEVICE_NOT_READY;
  }
 }else if(Next==KSSTATE_RUN){
  T70_AFE_START_RESULT R={};S=RequestStartCore(ReadyToRun,StateContext,&R);
  if(S!=STATUS_SUCCESS&&AbortRun)(void)AbortRun(StateContext);
 }else if(State==KSSTATE_RUN){
  struct Work {T70_AFE_FORMAT_IO *Io;bool Done;};Work W={&HardwareIo,false};
  S=ExecuteIo(ExecuteContext,[](void *C)->NTSTATUS{auto W=(Work *)C;auto R=T70AfeDl1RequestStop(W->Io);W->Done=R.Completed!=0;
   return W->Done?STATUS_SUCCESS:(R.Status<0?(NTSTATUS)R.Status:STATUS_DEVICE_NOT_READY);
  },&W);
  if(S==STATUS_SUCCESS&&!W.Done)S=STATUS_DEVICE_NOT_READY;
  if(S==STATUS_SUCCESS){PausePending=true;PauseDrained=false;return FinishPause();}
 }
 if(S!=STATUS_SUCCESS){StateFault=true;return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 State=Next;if(State==KSSTATE_STOP)FrozenPosition=0;return STATUS_SUCCESS;
}
