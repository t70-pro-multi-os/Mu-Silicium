// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicFactory.h"
extern "C" {
#include "WaveRtFormat.h"
}
static NTSTATUS Exact(NTSTATUS S){return S==0?S:(NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S);}
static NTSTATUS ReleaseStream(T70_WAVECYCLIC_FACTORY *F){
 NTSTATUS S=0;
 if(F->Stream){S=F->Stream->Close();if(S)goto Done;}
 if(F->Published){S=Exact(F->Contract.DrainExports(F->Contract.Context));if(S)goto Done;F->Published=false;}
 if(F->Stream){auto P=F->Stream;F->Stream=nullptr;P->Release();}
 S=T70WaveCyclicBufferRelease(&F->Buffer);
Done:F->CleanupStatus=S;return S;
}
NTSTATUS T70WaveCyclicFactoryQuiesce(T70_WAVECYCLIC_FACTORY *F){
 if(!F||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 F->Closing=true;
 // Stop hardware and admitted CPU/service users while the IRQ executor exists.
 // Keep exported objects, channel allocation and Port alive for later Close.
 NTSTATUS S=F->Stream?F->Stream->Close():STATUS_SUCCESS;
 F->CleanupStatus=S;return S;
}
NTSTATUS T70WaveCyclicFactoryClose(T70_WAVECYCLIC_FACTORY *F){
 if(!F||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 F->Closing=true;
 NTSTATUS S=ReleaseStream(F);if(S)return S;
 if(F->Port){auto P=F->Port;F->Port=nullptr;P->Release();}
 return STATUS_SUCCESS;
}
static NTSTATUS Init(void *C,PUNKNOWN,PRESOURCELIST R,IPortWaveCyclic *P){
 auto F=(T70_WAVECYCLIC_FACTORY *)C;
 if(!F||!F->Board||!P||!R||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 auto B=F->Board;auto &K=F->Contract;
 if(F->Initialized||F->Closing||B->BeforeIrqClose||R!=B->Resources||!B->Parent||
    !K.Context||!K.Access||!K.Remove||!K.Prepare||!K.Ready||!K.Stopped||!K.DrainExports)return STATUS_DEVICE_NOT_READY;
 F->Initialized=true;P->AddRef();F->Port=P;
 B->BeforeIrqClose=[](void *X)->NTSTATUS{return T70WaveCyclicFactoryQuiesce((T70_WAVECYCLIC_FACTORY *)X);};
 B->StreamOwner=F;return STATUS_SUCCESS;
}
static NTSTATUS Access(void *C){auto F=(T70_WAVECYCLIC_FACTORY *)C;return F->Contract.Access(F->Contract.Context);}
static NTSTATUS Prepare(void *C){
 auto F=(T70_WAVECYCLIC_FACTORY *)C;
 NTSTATUS S=T70WaveCyclicServicePause(&F->Board->Irq.Owner.Service);
 if(S==STATUS_SUCCESS&&F->Contract.PauseCallbacks)S=Exact(F->Contract.PauseCallbacks(F->Contract.Context));
 return S==STATUS_SUCCESS?Exact(F->Contract.Prepare(F->Contract.Context)):S;
}
static NTSTATUS Stopped(void *C){
 auto F=(T70_WAVECYCLIC_FACTORY *)C;
 // Stream has already executed stock STOP. Pause closes only submission;
 // downstream queued callbacks/position policy remain the provider's job.
 NTSTATUS S=T70WaveCyclicServicePause(&F->Board->Irq.Owner.Service);
 if(S==STATUS_SUCCESS&&F->Contract.PauseCallbacks)S=Exact(F->Contract.PauseCallbacks(F->Contract.Context));
 return S==STATUS_SUCCESS?Exact(F->Contract.Stopped(F->Contract.Context)):S;
}
static NTSTATUS Ready(void *C){
 auto F=(T70_WAVECYCLIC_FACTORY *)C;
 NTSTATUS S=Exact(F->Contract.Ready(F->Contract.Context));
 if(S==STATUS_SUCCESS&&F->Contract.ResumeCallbacks)S=Exact(F->Contract.ResumeCallbacks(F->Contract.Context));
 return S==STATUS_SUCCESS?T70WaveCyclicServiceResume(&F->Board->Irq.Owner.Service):S;
}
static NTSTATUS AbortRun(void *C){
 auto F=(T70_WAVECYCLIC_FACTORY *)C;
 NTSTATUS S=T70WaveCyclicServicePause(&F->Board->Irq.Owner.Service);
 NTSTATUS Callbacks=F->Contract.PauseCallbacks?Exact(F->Contract.PauseCallbacks(F->Contract.Context)):STATUS_SUCCESS;
 return S==STATUS_SUCCESS?Callbacks:S;
}
static NTSTATUS Remove(void *C){
 auto F=(T70_WAVECYCLIC_FACTORY *)C;auto B=F->Board;
 struct Stop {T70_AFE_FORMAT_IO Io;bool Done;};Stop W={{&B->Mapping.Mapping,T70AfeMappedRead32,T70AfeMappedWrite32},false};
 NTSTATUS S=T70AfeIrqRecoveryExecute(&B->Irq,[](void *V)->NTSTATUS{
  auto W=(Stop *)V;auto R=T70AfeDl1RequestStop(&W->Io);W->Done=R.Completed!=0;
  return W->Done?STATUS_SUCCESS:(R.Status<0?(NTSTATUS)R.Status:STATUS_DEVICE_NOT_READY);
 },&W);
 if(S||!W.Done)return S<0?S:STATUS_DEVICE_NOT_READY;
 return Exact(F->Contract.Remove(F->Contract.Context));
}
static NTSTATUS Create(void *C,IPortWaveCyclic *P,PKSDATAFORMAT Format,
 PMINIPORTWAVECYCLICSTREAM *Out,PDMACHANNEL *D,PSERVICEGROUP *G){
 auto F=(T70_WAVECYCLIC_FACTORY *)C;
 if(!F||!Format||!Out||!D||!G||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 auto B=F->Board;
 if(!F->Initialized||F->Closing||F->Stream||P!=F->Port||!B||!B->Started||
    !B->Lease||!B->Irq.Owner.Interrupt||B->Irq.Owner.Handler.Faulted||
    !B->Irq.Owner.Service.Group||B->Irq.Owner.Service.Port!=P)return STATUS_DEVICE_NOT_READY;
 // Decode using a legal placeholder geometry, then allocate an actual OS
 // channel for two rounded 10 ms periods. No CPU/device address identity guess.
 T70_AFE_PREPARED Plan;
 if((Format->FormatSize!=82&&Format->FormatSize!=104)||T70WaveRtPrepareBufferFormat(Format,Format->FormatSize,0,192,96,&Plan)!=T70WavePrepared||Plan.SampleFormat!=T70AfeS16Le)return STATUS_NOT_SUPPORTED;
 ULONG Frame=Plan.Channels*2;
 ULONG Period=(ULONG)((((uint64_t)Plan.Rate*10+999)/1000*Frame+15)&~15ull);
 NTSTATUS S=T70WaveCyclicBufferAcquire(&F->Buffer,P,Period*2,Frame);if(S)return S;
 S=T70WaveCyclicStream::Create(B->Parent,&F->Buffer,Plan.Rate,&F->Stream);if(S)goto Failed;
 {
  T70_AFE_FORMAT_IO Io={&B->Mapping.Mapping,T70AfeMappedRead32,T70AfeMappedWrite32};
  T70_AFE_CONFIG_RESULT Result={};
  S=F->Stream->ConfigureDl1(Io,Access,Remove,F,T70AfeIrqRecoveryExecute,&B->Irq,&Result);if(S)goto Failed;
  S=F->Stream->BindPositionExecutor(T70AfeIrqRecoveryObserve,&B->Irq);if(S)goto Failed;
  S=F->Stream->BindStateControl(F,Prepare,Ready,Stopped,AbortRun);if(S)goto Failed;
  ULONG Size=0;if(!F->Stream->SetNotificationFreq(10,&Size)||!Size){S=STATUS_DEVICE_NOT_READY;goto Failed;}
 }
 if(F->Contract.Publish){
  PMINIPORTWAVECYCLICSTREAM PublicS=nullptr;PDMACHANNEL PublicD=nullptr;PSERVICEGROUP PublicG=nullptr;
  S=Exact(F->Contract.Publish(F->Contract.Context,F->Stream,F->Buffer.Channel,
    B->Irq.Owner.Service.Group,&PublicS,&PublicD,&PublicG));
  if(S==STATUS_SUCCESS&&(!PublicS||!PublicD||!PublicG))S=STATUS_DEVICE_NOT_READY;
  if(S!=STATUS_SUCCESS){if(PublicS)PublicS->Release();if(PublicD)PublicD->Release();if(PublicG)PublicG->Release();goto Failed;}
  *Out=PublicS;*D=PublicD;*G=PublicG;
 }else{
  F->Stream->AddRef();F->Buffer.Channel->AddRef();B->Irq.Owner.Service.Group->AddRef();
  *Out=F->Stream;*D=F->Buffer.Channel;*G=B->Irq.Owner.Service.Group;
 }
 F->Published=true;return STATUS_SUCCESS;
Failed:
 // A failed, unpublished NewStream is not device removal. Keep the initialized
 // port/hook for a later explicit request if all partial resources were safely
 // released. Cleanup failure retains ownership and closes admission until the
 // real device owner explicitly completes Close. Never retry hardware here.
 if(ReleaseStream(F)!=STATUS_SUCCESS)F->Closing=true;
 return S;
}
T70_WAVECYCLIC_MINIPORT_PROVIDER T70WaveCyclicFactoryProvider(T70_WAVECYCLIC_FACTORY *F){return {F,Init,Create};}
