// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioCyclicStart.h"
#include "AudioDomainOwner.h"
#include "AudioExportOwner.h"
#include "AudioEndpoint.h"
#include "WaveCyclicFactory.h"
#include <string.h>
#include <wdf.h>
#include <wdfminiport.h>
extern "C" {
#include "AwSpbResources.h"
}
static constexpr ULONG Tag=0x53433741;
class Runtime final:public IUnknown {
 LONG Refs=2; // slot/retirement token + pre-created system thread
 KSPIN_LOCK RefLock;
 KEVENT Changed;
 enum {Live,RetireAwait,RetireCpu,ExitWait,Quarantined};
 LONG Mode=Live;
 bool CpuEligible=false;
 NTSTATUS RetirementStatus=STATUS_SUCCESS;
 static void Thread(void *);
 NTSTATUS CleanupCpu();
 void RequestExit(){KIRQL Q;KeAcquireSpinLock(&RefLock,&Q);Mode=ExitWait;KeSetEvent(&Changed,IO_NO_INCREMENT,FALSE);KeReleaseSpinLock(&RefLock,Q);}
 static void *operator new(size_t,void *P) noexcept{return P;}
 Runtime()=default;
 ~Runtime()=default;
public:
 T70_AUDIO_DOMAIN_OWNER Domain;
 T70_AUDIO_EXPORT_OWNER Exports={};
 T70_WAVECYCLIC_BOARD Board={};
 T70_WAVECYCLIC_FACTORY Factory={};
 T70_AUDIO_ENDPOINT Endpoint={};
 PDEVICE_OBJECT Device=nullptr,Pdo=nullptr;
 // Framework parent for synchronous amplifier requests during Start.
 WDFDEVICE FrameworkDevice=nullptr;
 void ReleaseFramework(){
  if(FrameworkDevice){auto F=FrameworkDevice;FrameworkDevice=nullptr;WdfObjectDelete(F);}
 }
 NTSTATUS PrepareFramework(){
  if(FrameworkDevice)return STATUS_INVALID_DEVICE_STATE;
  // This wrapper sends no requests down the PortCls device stack, so the
  // optional next-lower object is deliberately absent, not guessed from PDO.
  return WdfDeviceMiniportCreate(WdfGetDriver(),WDF_NO_OBJECT_ATTRIBUTES,
    Device,nullptr,Pdo,&FrameworkDevice);
 }
 static NTSTATUS Create(PDEVICE_OBJECT Device,Runtime **Out){
  void *P=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(Runtime),Tag);if(!P)return STATUS_INSUFFICIENT_RESOURCES;
  auto O=new(P) Runtime;O->Device=Device;
  KeInitializeSpinLock(&O->RefLock);KeInitializeEvent(&O->Changed,NotificationEvent,FALSE);
  OBJECT_ATTRIBUTES A;InitializeObjectAttributes(&A,nullptr,OBJ_KERNEL_HANDLE,nullptr,nullptr);
  HANDLE H=nullptr;NTSTATUS S=IoCreateSystemThread(Device,&H,THREAD_ALL_ACCESS,&A,nullptr,nullptr,Thread,O);
  if(S!=STATUS_SUCCESS){O->~Runtime();ExFreePoolWithTag(O,Tag);return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
  // Valid kernel handle from successful creation; OS pins Device/code until
  // Thread returns, independently of our COM refs and the domain claim.
  (void)ZwClose(H);*Out=O;return STATUS_SUCCESS;
 }
 STDMETHODIMP_(ULONG) AddRef()override{KIRQL Q;KeAcquireSpinLock(&RefLock,&Q);LONG N=++Refs;KeReleaseSpinLock(&RefLock,Q);return (ULONG)N;}
 STDMETHODIMP_(ULONG) Release()override{
  KIRQL Q;KeAcquireSpinLock(&RefLock,&Q);LONG N=--Refs;
  // Signal before dropping the lock: final worker inspection cannot free the
  // event while this release is still using it. Thread owns the last reference.
  KeSetEvent(&Changed,IO_NO_INCREMENT,FALSE);KeReleaseSpinLock(&RefLock,Q);return (ULONG)N;
 }
 void ReleaseSlot(){RequestExit();Release();}
 static NTSTATUS Retire(T70_AUDIO_CYCLIC_DEVICE *,void **);
 static void Forwarded(void *);
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID I,void **Out)override{
  static const GUID Unknown={0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
  if(!Out)return STATUS_INVALID_PARAMETER;*Out=nullptr;
  if(memcmp(&I,&Unknown,sizeof I))return STATUS_NOINTERFACE;
  *Out=static_cast<IUnknown *>(this);AddRef();return STATUS_SUCCESS;
 }
 static NTSTATUS Claim(void *C){auto O=(Runtime *)C;return T70AudioDomainAcquire(&O->Domain,O->Device,O->Pdo);}
 static NTSTATUS Unclaim(void *C){return T70AudioDomainRelease(&((Runtime *)C)->Domain);}
 static NTSTATUS PauseService(void *C){return T70AudioExportPauseServices(&((Runtime *)C)->Exports);}
 static NTSTATUS ResumeService(void *C){return T70AudioExportResumeServices(&((Runtime *)C)->Exports);}
 static NTSTATUS DrainService(void *C){return T70AudioExportDrainServices(&((Runtime *)C)->Exports);}
 static NTSTATUS Drain(void *C){return T70AudioExportDrain(&((Runtime *)C)->Exports);}
 static NTSTATUS Detach(void *C){
  auto O=(Runtime *)C;
  NTSTATUS S=T70WaveCyclicFactoryClose(&O->Factory);
  return S==STATUS_SUCCESS?Drain(C):S;
 }
 static NTSTATUS Publish(void *C,PMINIPORTWAVECYCLICSTREAM S,PDMACHANNEL D,PSERVICEGROUP G,
   PMINIPORTWAVECYCLICSTREAM *OS,PDMACHANNEL *OD,PSERVICEGROUP *OG){
  auto O=(Runtime *)C;return T70AudioExportWrap(&O->Exports,O,S,D,G,OS,OD,OG);
 }
 static NTSTATUS StartAccessReady(void *C){
  auto O=static_cast<Runtime *>(C);
  auto R=O->Board.Resources;
  if(!R||R->NumberOfEntriesOfType(CmResourceTypeConnection)!=2)
   return STATUS_DEVICE_NOT_READY;
  T70_AW_SPB_RESOURCE Resources[2]={};
  for(unsigned Channel=0;Channel<2;Channel++){
   auto Raw=R->FindUntranslatedEntry(CmResourceTypeConnection,Channel);
   auto Translated=R->FindTranslatedEntry(CmResourceTypeConnection,Channel);
   if(!Raw||!Translated||Raw->Type!=CmResourceTypeConnection||
      Translated->Type!=CmResourceTypeConnection||
      Raw->u.Connection.Class!=CM_RESOURCE_CONNECTION_CLASS_SERIAL||
      Raw->u.Connection.Type!=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C||
      Raw->u.Connection.IdLowPart!=Translated->u.Connection.IdLowPart||
      Raw->u.Connection.IdHighPart!=Translated->u.Connection.IdHighPart)
    return STATUS_DEVICE_DATA_ERROR;
   // AFE0.AMPS declares 0x34, then 0x35. Acquire also checks each ID's address.
   Resources[Channel]={O->FrameworkDevice,Translated,0x11e01000,
                       (uint16_t)(0x34+Channel),(uint16_t)Channel};
  }

  T70_AW_SPB_RESOURCES Amplifiers={};
  NTSTATUS Status=T70AwSpbResourcesAcquire(&Amplifiers,Resources);
  if(Status!=STATUS_SUCCESS)return Status;
  for(unsigned Channel=0;Channel<2;Channel++){
   USHORT Identity;
   Status=T70AwSpbIdentify88261(Amplifiers.Target[Channel].Target,&Identity);
   if(Status!=STATUS_SUCCESS)break;
  }
  NTSTATUS Close=T70AwSpbResourcesRelease(&Amplifiers);
  if(Close!=STATUS_SUCCESS)return Close;
  return Status==STATUS_SUCCESS?AccessReady(C):Status;
 }
 static NTSTATUS AccessReady(void *){
  // CPU CAS is NOT power/clock/firmware exclusion. No such real board provider
  // is installed. Fail BEFORE mapping or IRQ setup, not after fake playback.
  return STATUS_DEVICE_NOT_READY;
 }
 static NTSTATUS PhysicalTransition(void *){return STATUS_DEVICE_NOT_READY;}
};
NTSTATUS T70AudioCyclicRelease(T70_AUDIO_CYCLIC_DEVICE *Slot){
 if(!Slot||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 auto O=(Runtime *)Slot->Runtime;if(!O){Slot->CleanupStatus=STATUS_SUCCESS;return STATUS_SUCCESS;}
 // NewStream may still be preparing a provider object before it publishes
 // any export proxy. Drain that admission before touching Factory/board state.
 NTSTATUS Admission=O->Board.Port.Miniport?O->Board.Port.Miniport->QuiesceCalls():STATUS_SUCCESS;
 if(Admission!=STATUS_SUCCESS){Slot->CleanupStatus=Admission;return Admission;}
 // Stop new device-facing proxy work before the stock STOP/rundown path.
 // This does not claim that opaque PortCls service callbacks have drained.
 Admission=T70AudioExportQuiesce(&O->Exports);
 if(Admission!=STATUS_SUCCESS){Slot->CleanupStatus=Admission;return Admission;}
 O->Board.Started=false;
 NTSTATUS S=T70AudioEndpointWithdraw(&O->Endpoint);
 if(S==STATUS_SUCCESS)S=T70WaveCyclicBoardClose(&O->Board);
 if(S==STATUS_SUCCESS)S=T70AudioEndpointClose(&O->Endpoint);
 if(S!=STATUS_SUCCESS&&NT_SUCCESS(S))S=STATUS_DEVICE_NOT_READY;
 Slot->CleanupStatus=S;
 if(S!=STATUS_SUCCESS)return S; // initial ref and retry handle stay in FDO
 O->ReleaseFramework();
 Slot->Runtime=nullptr;O->ReleaseSlot();return STATUS_SUCCESS;
}
static bool ActiveRuntime(Runtime *O){
 auto &B=O->Board;
 return B.Started||B.Mapping.Mapping.VirtualBase||B.Lease||B.Irq.Initialized||
  O->Factory.Stream||O->Factory.Buffer.Channel||O->Factory.Published||
  O->Endpoint.Registered||O->Endpoint.Connected||B.Port.Registered;
}
NTSTATUS T70AudioCyclicQueryRelease(T70_AUDIO_CYCLIC_DEVICE *Slot){
 if(!Slot||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 auto O=(Runtime *)Slot->Runtime;
 // QUERY may be cancelled by another device. Never irreversibly unregister,
 // stop a live stream or close admission until restart/cancel is implemented.
 if(O&&ActiveRuntime(O))return STATUS_DEVICE_NOT_READY;
 return T70AudioCyclicRelease(Slot);
}
NTSTATUS T70AudioCyclicSurpriseRemove(T70_AUDIO_CYCLIC_DEVICE *Slot){
 if(!Slot||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Slot->Removing=true;Slot->SurpriseRemoved=true;auto O=(Runtime *)Slot->Runtime;
 if(!O)return STATUS_SUCCESS;
 O->Board.Started=false;O->Factory.Closing=true;
 if(O->Board.Irq.Initialized){
  NTSTATUS S=T70AfeIrqRecoverySurpriseRemove(&O->Board.Irq,Runtime::DrainService,O);
  if(S!=STATUS_SUCCESS){Slot->CleanupStatus=S;return S;}
 }
 // Disconnect is not proof of DMA completion. Never use normal MMIO Close or
 // free a buffer still referenced by hardware. Production admission remains
 // sealed until bus quiescence plus final REMOVE/unload ownership is supplied.
 if(ActiveRuntime(O)){Slot->CleanupStatus=STATUS_DEVICE_NOT_READY;return Slot->CleanupStatus;}
 return T70AudioCyclicRemoveUnpublished(Slot);
}
NTSTATUS T70AudioCyclicRemove(T70_AUDIO_CYCLIC_DEVICE *Slot){
 if(!Slot||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Slot->Removing=true;
 if(!Slot->SurpriseRemoved){
  NTSTATUS S=T70AudioCyclicRelease(Slot);
  if(S==STATUS_SUCCESS)return S;
  if(Slot->Runtime&&ActiveRuntime((Runtime *)Slot->Runtime))return S;
 }
 // Retained active state must not be force-freed on failure. The current
 // AccessReady gate stays sealed until asynchronous final-remove ownership is
 // available for a failed STOP or references that outlive this dispatch.
 return T70AudioCyclicRemoveUnpublished(Slot);
}
NTSTATUS T70AudioCyclicRemoveUnpublished(T70_AUDIO_CYCLIC_DEVICE *Slot){
 if(!Slot||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 Slot->Removing=true;
 auto Existing=(Runtime *)Slot->Runtime;
 if(Existing&&ActiveRuntime(Existing)){Slot->CleanupStatus=STATUS_DEVICE_NOT_READY;return Slot->CleanupStatus;}
 NTSTATUS S=T70AudioCyclicRelease(Slot);if(S==STATUS_SUCCESS)return S;
 auto O=(Runtime *)Slot->Runtime;if(!O)return S;
 auto &B=O->Board;
 // AccessReady is sealed to reject. Confirm the exact absence of anything that
 // could need device access, notification drainage or asynchronous teardown.
 if(O->Endpoint.Port||O->Endpoint.Miniport||O->Endpoint.Registered||O->Endpoint.Connected||
    O->Endpoint.Unregister||O->Endpoint.Unlink||B.Mapping.Mapping.VirtualBase||B.Lease||B.Irq.Initialized||B.Port.Port||
    B.Port.Miniport||B.Port.Registered||B.Port.Unregister||B.BeforeIrqClose||O->Factory.Stream||O->Factory.Buffer.Channel||
    O->Factory.Published||T70AudioExportDrain(&O->Exports)!=STATUS_SUCCESS)return S;
 // No hardware resource exists. A retained failed-start owner can release the
 // actual software claim/references directly; do not fail/veto final REMOVE.
 S=T70AudioDomainRelease(&O->Domain);if(S!=STATUS_SUCCESS)return S;
 B.DomainHeld=false;
 if(B.Resources){auto R=B.Resources;B.Resources=nullptr;R->Release();}
 if(B.Parent){auto P=B.Parent;B.Parent=nullptr;P->Release();}
 O->ReleaseFramework();
 Slot->Runtime=nullptr;Slot->CleanupStatus=STATUS_SUCCESS;O->ReleaseSlot();return STATUS_SUCCESS;
}
NTSTATUS T70AudioCyclicStart(T70_AUDIO_CYCLIC_DEVICE *Slot,PDEVICE_OBJECT D,PIRP I,PRESOURCELIST R){
 if(!Slot||!D||!I||!R||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 if(Slot->Removing)return STATUS_INVALID_DEVICE_STATE;
 NTSTATUS S=T70AudioCyclicRelease(Slot);if(S!=STATUS_SUCCESS)return S;
 PDEVICE_OBJECT P=nullptr;S=PcGetPhysicalDeviceObject(D,&P);
 if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;
 if(!P)return STATUS_NO_SUCH_DEVICE;
 Runtime *O=nullptr;S=Runtime::Create(D,&O);if(S!=STATUS_SUCCESS)return S;
 Slot->Runtime=O; // transfer the initial ref BEFORE any fallible board step
 O->Device=D;O->Pdo=P;O->Factory.Board=&O->Board;
 O->Factory.Contract={O,Runtime::AccessReady,Runtime::DrainService,
   Runtime::PhysicalTransition,Runtime::PhysicalTransition,Runtime::PhysicalTransition,
   Runtime::Drain,Runtime::Publish,Runtime::PauseService,Runtime::ResumeService};
 T70_WAVECYCLIC_BOARD_CONTRACT C={O,Runtime::Claim,Runtime::Unclaim,
   Runtime::DrainService,Runtime::Detach,Runtime::StartAccessReady};
 S=O->PrepareFramework();
 if(S==STATUS_SUCCESS)S=T70WaveCyclicBoardOpen(&O->Board,D,P,I,O,O,R,T70WaveCyclicFactoryProvider(&O->Factory),C);
 if(S==STATUS_SUCCESS){
  S=T70AudioEndpointOpen(&O->Endpoint,D,I,O,R,&O->Board.Port);
  if(S==STATUS_SUCCESS)return STATUS_SUCCESS; // persistent slot owns runtime
 }
 // Current physical admission cannot succeed. Failed cleanup remains reachable
 // from this FDO slot and can be retried by Start/query-remove/remove.
 NTSTATUS Cleanup=T70AudioCyclicRelease(Slot);
 return Cleanup==STATUS_SUCCESS?S:Cleanup;
}

// Called only after the original board close reached post-IRQ export waiting.
// No registration APIs, device-extension access, MMIO or hardware retry here.
NTSTATUS Runtime::CleanupCpu(){
 NTSTATUS S=Detach(this);if(S!=STATUS_SUCCESS)return S;
 auto &P=Board.Port;
 if(P.Miniport){S=P.Miniport->Close([](void *)->NTSTATUS{return STATUS_SUCCESS;},nullptr);if(S)return S;}
 auto PU=P.Unregister;auto PP=P.Port;auto PM=P.Miniport;
 P.Unregister=nullptr;P.Port=nullptr;P.Miniport=nullptr;P.Initialized=false;P.OpenDevice=nullptr;
 if(PU)PU->Release();if(PP)PP->Release();if(PM)PM->Release();
 if(Endpoint.Miniport){S=Endpoint.Miniport->Close();if(S)return S;}
 auto EU=Endpoint.Unlink;auto ER=Endpoint.Unregister;auto EP=Endpoint.Port;auto EM=Endpoint.Miniport;
 Endpoint.Unlink=nullptr;Endpoint.Unregister=nullptr;Endpoint.Port=nullptr;Endpoint.Miniport=nullptr;
 Endpoint.Device=nullptr;Endpoint.Wave=nullptr;
 if(EU)EU->Release();if(ER)ER->Release();if(EP)EP->Release();if(EM)EM->Release();
 // The mapping/domain were already released before forwarding REMOVE.
 if(Board.Lease||Board.Mapping.Mapping.VirtualBase||Board.DomainHeld)return STATUS_INVALID_DEVICE_STATE;
 if(Board.Resources){auto R=Board.Resources;Board.Resources=nullptr;R->Release();}
 if(Board.Parent){auto P=Board.Parent;Board.Parent=nullptr;P->Release();}
 return STATUS_SUCCESS;
}
void Runtime::Thread(void *Context){
 auto O=static_cast<Runtime *>(Context);
 for(;;){
  KIRQL Q;KeAcquireSpinLock(&O->RefLock,&Q);KeClearEvent(&O->Changed);
  LONG State=O->Mode;
  if(State==ExitWait&&O->Refs==1){O->Refs=0;KeReleaseSpinLock(&O->RefLock,Q);
   O->~Runtime();ExFreePoolWithTag(O,Tag);return; // OS releases its device/code pin after return
  }
  KeReleaseSpinLock(&O->RefLock,Q);
  if(State==RetireCpu){
   // Every exported proxy contributes a Parent ref; its final Release wakes us.
   LONG Gate=InterlockedCompareExchange(&O->Exports.Gate,0,0);
   if((Gate&0x3fffffff)==0){
    NTSTATUS S=O->CleanupCpu();
    KeAcquireSpinLock(&O->RefLock,&Q);O->RetirementStatus=S;
    if(S==STATUS_SUCCESS)O->Mode=ExitWait;
    else if(S!=STATUS_DEVICE_NOT_READY)O->Mode=Quarantined;
    // NOT_READY is CPU-only rundown, not an uncertain hardware failure.
    // Keep any release notification raised during CleanupCpu, but do not
    // self-signal/retry: the remaining admitted method's Parent Release wakes us.
    if(S==STATUS_SUCCESS)KeSetEvent(&O->Changed,IO_NO_INCREMENT,FALSE);
    KeReleaseSpinLock(&O->RefLock,Q);
   }
  }
  // A dedicated OS-pinned thread, not a system worker-pool thread. Quarantine
  // waits without automatic retries or resource release after uncertain STOP.
  (void)KeWaitForSingleObject(&O->Changed,Executive,KernelMode,FALSE,nullptr);
 }
}
NTSTATUS Runtime::Retire(T70_AUDIO_CYCLIC_DEVICE *Slot,void **Token){
 if(!Slot||!Token||KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
 *Token=nullptr;auto O=static_cast<Runtime *>(Slot->Runtime);if(!O)return STATUS_SUCCESS;
 Slot->Removing=true;
 // No SPB target/request is currently owned by this wrapper. Delete it before
 // PortCls can delete the underlying FDO; never defer this to the CPU worker.
 O->ReleaseFramework();
 auto &B=O->Board;
 NTSTATUS Calls=B.Port.Miniport?B.Port.Miniport->QuiesceCalls():STATUS_SUCCESS;
 if(Calls!=STATUS_SUCCESS)Slot->CleanupStatus=Calls;
 bool Eligible=Calls==STATUS_SUCCESS&&!Slot->SurpriseRemoved&&O->Factory.Closing&&!B.BeforeIrqClose&&!B.Started&&
  (!B.Irq.Initialized||B.Irq.Closed)&&!B.Irq.Owner.Interrupt&&!B.Irq.Work&&!B.Irq.Owner.ConnectionUnknown&&
  !O->Endpoint.Connected&&!O->Endpoint.Registered&&!B.Port.Registered&&
  O->Endpoint.Closing&&B.Port.PublicationClosed&&
  T70AudioExportQuiesce(&O->Exports)==STATUS_SUCCESS&&
  T70AudioExportDrainServices(&O->Exports)==STATUS_SUCCESS;
 // Finish device-resource teardown before the final REMOVE is forwarded. The
 // pre-created thread's OS reference remains; no new device reference is taken.
 if(Eligible){
  NTSTATUS S=STATUS_SUCCESS;
  if(B.Lease){S=T70AfeMappingEnd(&B.Mapping);if(S==STATUS_SUCCESS)B.Lease=false;}
  if(S==STATUS_SUCCESS)S=T70AfeMappingRelease(&B.Mapping);
  if(S==STATUS_SUCCESS&&B.DomainHeld){S=T70AudioDomainRelease(&O->Domain);if(S==STATUS_SUCCESS)B.DomainHeld=false;}
  if(S!=STATUS_SUCCESS){Eligible=false;Slot->CleanupStatus=NT_SUCCESS(S)?STATUS_DEVICE_NOT_READY:S;}
 }
 KIRQL Q;KeAcquireSpinLock(&O->RefLock,&Q);
 // A valid serialized slot normally has Live mode. On unexpected state do
 // not leave the only handle in an extension about to disappear: transfer it
 // into quarantined retirement, without retrying cleanup or claiming success.
 if(O->Mode!=Live){Eligible=false;Slot->CleanupStatus=STATUS_INVALID_DEVICE_STATE;}
 O->CpuEligible=Eligible;O->RetirementStatus=Slot->CleanupStatus;O->Mode=RetireAwait;
 KeReleaseSpinLock(&O->RefLock,Q);
 Slot->Runtime=nullptr;*Token=O; // transfer slot reference; thread waits for Forwarded
 return STATUS_SUCCESS;
}
void Runtime::Forwarded(void *Token){
 if(!Token)return;auto O=static_cast<Runtime *>(Token);
 KIRQL Q;KeAcquireSpinLock(&O->RefLock,&Q);
 if(O->Mode==RetireAwait)O->Mode=O->CpuEligible?RetireCpu:Quarantined;
 KeSetEvent(&O->Changed,IO_NO_INCREMENT,FALSE);KeReleaseSpinLock(&O->RefLock,Q);
 O->Release(); // consume token, no FDO/extension access after dispatch
}
NTSTATUS T70AudioCyclicRetire(T70_AUDIO_CYCLIC_DEVICE *S,void **T){return Runtime::Retire(S,T);}
void T70AudioCyclicForwardReturned(void *T){Runtime::Forwarded(T);}
