// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioDeviceStart.h"
#include "AudioDriver.cpp"
#include "WaveCyclicBoard.h"
#include "AudioEndpoint.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <sys/wait.h>
extern "C" {
#include "AwSpbResources.h"
}
static thread_local unsigned Irql;
static std::atomic<unsigned> Checks;
static unsigned Fail,Allocations,Frees,Maps,Unmaps,PdoRefs,Puts,DmaGetCalls,BoardCalls;
static unsigned Closes,Withdraws;
static T70_WAVECYCLIC_BOARD *LastBoard;
static NTSTATUS DispatchStatus;
static NTSTATUS CloseStatus;static unsigned Forwarded,Completed;static bool PoisonRemove;
static NTSTATUS PdoStatus;static PDEVICE_OBJECT AssociatedPdo,ExpectedDevice;
#define C(X) do{Checks++;assert(X);}while(0)
static unsigned FrameworkCreates,FrameworkDeletes,FrameworkLive;
static bool FailFramework;
static unsigned AmplifierOpens,AmplifierCloses,AmplifierReads,BadAmplifier;
static NTSTATUS AmplifierStartStatus=STATUS_DEVICE_NOT_READY;
extern "C" NTSTATUS T70AwSpbResourcesAcquire(T70_AW_SPB_RESOURCES *O,const T70_AW_SPB_RESOURCE R[2]){
 C(!O->Open&&FrameworkLive==1);
 for(unsigned Channel=0;Channel<2;Channel++){
  C(R[Channel].Device==(void *)(uintptr_t)0x5678&&R[Channel].Connection);
  C(R[Channel].Connection->u.Connection.IdLowPart==Channel+2&&
    R[Channel].Connection->u.Connection.IdHighPart==1);
  C(R[Channel].ControllerBase==0x11e01000&&R[Channel].Address==0x34+Channel&&R[Channel].Channel==Channel);
  O->Target[Channel].Target=(void *)(uintptr_t)(0xa0+Channel);
 }
 O->Open=1;AmplifierOpens++;return STATUS_SUCCESS;
}
extern "C" NTSTATUS T70AwSpbIdentify88261(WDFIOTARGET T,USHORT *Id){
 auto Channel=(uintptr_t)T-0xa0;
 C(Channel<2&&Id&&FrameworkLive==1);
 AmplifierReads++;
 if(BadAmplifier==Channel+1)return STATUS_NO_SUCH_DEVICE;
 *Id=0x2113;return STATUS_SUCCESS;
}
extern "C" NTSTATUS T70AwSpbResourcesRelease(T70_AW_SPB_RESOURCES *O){
 C(O->Open&&FrameworkLive==1&&O->Target[0].Target&&O->Target[1].Target);
 O->Open=0;AmplifierCloses++;return STATUS_SUCCESS;
}
WDFDRIVER WdfGetDriver(){return (WDFDRIVER)(uintptr_t)0x1234;}
NTSTATUS WdfDeviceMiniportCreate(WDFDRIVER Driver,WDF_OBJECT_ATTRIBUTES *,PDEVICE_OBJECT Fdo,
 PDEVICE_OBJECT Lower,PDEVICE_OBJECT Pdo,WDFDEVICE *Out){
 C(Driver==WdfGetDriver()&&Fdo==ExpectedDevice&&Pdo==AssociatedPdo&&!Lower);
 if(FailFramework)return STATUS_INSUFFICIENT_RESOURCES;
 C(!FrameworkLive);FrameworkCreates++;FrameworkLive++;*Out=(WDFDEVICE)(uintptr_t)0x5678;return STATUS_SUCCESS;
}
void WdfObjectDelete(void *Object){C(Object==(void *)(uintptr_t)0x5678&&FrameworkLive==1);FrameworkLive--;FrameworkDeletes++;}
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){++Allocations;if(Fail==Allocations)return nullptr;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Frees;free(P);}
LONG InterlockedIncrement(volatile LONG *P){return __atomic_add_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
LONG InterlockedCompareExchange(volatile LONG *P,LONG V,LONG E){__atomic_compare_exchange_n(P,&E,V,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return E;}
void ObReferenceObject(void *P){C(P==ExpectedDevice||P==AssociatedPdo);PdoRefs++;}
void ObDereferenceObject(void *){C(PdoRefs>0);PdoRefs--;}
NTSTATUS PcGetPhysicalDeviceObject(PDEVICE_OBJECT D,PDEVICE_OBJECT *Out){C(D==ExpectedDevice);*Out=AssociatedPdo;return PdoStatus;}
struct HostEvent {std::mutex Lock;std::condition_variable Cv;bool Set=false,Waiting=false;};
static std::vector<HostEvent*> HostEvents;static std::vector<std::mutex*> HostLocks;
struct HostThread {void (*Routine)(void*);void *Context;std::thread *Running=nullptr;};
static std::vector<HostThread*> HostThreads;static bool FailThread;
static std::atomic<unsigned> BlockedWaits{0};
static unsigned ThreadPins,ThreadCreates;static bool EarlyThread;static std::atomic<unsigned> Waiting{0};
void KeInitializeSpinLock(KSPIN_LOCK *L){auto P=new std::mutex;HostLocks.push_back(P);*L=(uintptr_t)P;}
void KeAcquireSpinLock(KSPIN_LOCK *L,KIRQL *Q){*Q=(KIRQL)Irql;((std::mutex*)*L)->lock();Irql=DISPATCH_LEVEL;}
void KeReleaseSpinLock(KSPIN_LOCK *L,KIRQL Q){((std::mutex*)*L)->unlock();Irql=Q;}
void KeInitializeEvent(KEVENT *E,EVENT_TYPE,BOOLEAN B){auto H=new HostEvent;H->Set=B;E->Host=H;HostEvents.push_back(H);}
void KeClearEvent(KEVENT *E){auto H=(HostEvent*)E->Host;std::lock_guard<std::mutex>L(H->Lock);H->Set=false;}
LONG KeSetEvent(KEVENT *E,LONG,BOOLEAN){auto H=(HostEvent*)E->Host;std::lock_guard<std::mutex>L(H->Lock);H->Set=true;H->Cv.notify_all();return 0;}
NTSTATUS KeWaitForSingleObject(void *E,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,void *){auto H=(HostEvent*)((KEVENT*)E)->Host;std::unique_lock<std::mutex>L(H->Lock);Waiting++;if(!H->Set)BlockedWaits++;H->Waiting=true;H->Cv.wait(L,[&]{return H->Set;});H->Waiting=false;return 0;}
NTSTATUS IoCreateSystemThread(void *D,PHANDLE H,ULONG Access,POBJECT_ATTRIBUTES A,HANDLE Process,void *Id,void (*F)(void*),void *Context){
 C(D==ExpectedDevice&&Access==THREAD_ALL_ACCESS&&A->Attributes==OBJ_KERNEL_HANDLE&&!Process&&!Id);ThreadCreates++;
 if(FailThread)return STATUS_INSUFFICIENT_RESOURCES;
 auto T=new HostThread{F,Context};HostThreads.push_back(T);ThreadPins++;*H=T;
 if(EarlyThread){auto W=Waiting.load();T->Running=new std::thread([=]{F(Context);});while(Waiting==W)std::this_thread::yield();}
 return 0;
}
NTSTATUS ZwClose(HANDLE H){C(H!=nullptr);return 0;}
static void RunThreads(){auto Threads=HostThreads;HostThreads.clear();for(auto T:Threads){if(T->Running){T->Running->join();delete T->Running;}else T->Routine(T->Context);ThreadPins--;delete T;}}
static unsigned MiniportQuiesces;
NTSTATUS T70WaveCyclicMiniport::QuiesceCalls(){MiniportQuiesces++;return STATUS_DEVICE_NOT_READY;}
NTSTATUS T70WaveCyclicMiniport::Close(NTSTATUS (*)(void*),void*){assert(false);return STATUS_DEVICE_NOT_READY;}
ULONG T70TopologyMiniport::Release(){assert(false);return 0;}
ULONG T70WaveCyclicMiniport::Release(){assert(false);return 0;}
NTSTATUS T70TopologyMiniport::Close(){assert(false);return STATUS_DEVICE_NOT_READY;}
extern "C" NTSTATUS T70AfeMappingEnd(T70_AFE_MAPPING_OWNER *O){C(O->ActiveUses>0);--O->ActiveUses;return 0;}
extern "C" NTSTATUS T70AfeMappingRelease(T70_AFE_MAPPING_OWNER *O){C(!O->ActiveUses&&!O->Mapping.VirtualBase);return 0;}
class Resources:public IResourceList{
public:ULONG Refs=1,Count=1,IrqCount=1,ConnectionCount=2;
 bool RawMissing=false,TranslatedMissing=false;
 CM_PARTIAL_RESOURCE_DESCRIPTOR D={},RawIrq={},Irq={},RawConnections[2]={},Connections[2]={};
 Resources(){D.Type=CmResourceTypeMemory;D.u.Memory.Start.QuadPart=0x11210000;D.u.Memory.Length=8192;
 RawIrq.Type=Irq.Type=CmResourceTypeInterrupt;
 RawIrq.ShareDisposition=Irq.ShareDisposition=CmResourceShareDeviceExclusive;
 RawIrq.u.Interrupt.Vector=266;Irq.u.Interrupt.Vector=0x456;Irq.u.Interrupt.Level=7;
 for(unsigned Channel=0;Channel<2;Channel++){
  auto &Connection=Connections[Channel];
  Connection.Type=CmResourceTypeConnection;
  Connection.u.Connection.Class=CM_RESOURCE_CONNECTION_CLASS_SERIAL;
  Connection.u.Connection.Type=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C;
  Connection.u.Connection.IdLowPart=Channel+2;
  Connection.u.Connection.IdHighPart=1;
  RawConnections[Channel]=Connection;
 }
 }
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 ULONG NumberOfEntriesOfType(CM_RESOURCE_TYPE T)override{
  if(T==CmResourceTypeConnection)return ConnectionCount;
  return T==CmResourceTypeMemory?Count:IrqCount;
 }
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindTranslatedEntry(CM_RESOURCE_TYPE T,ULONG Index)override{
  if(T==CmResourceTypeConnection)return Index<2?&Connections[Index]:nullptr;
  return T==CmResourceTypeMemory?&D:TranslatedMissing?nullptr:&Irq;
 }
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindUntranslatedEntry(CM_RESOURCE_TYPE T,ULONG Index)override{
  if(T==CmResourceTypeConnection)return Index<2?&RawConnections[Index]:nullptr;
  return T==CmResourceTypeMemory?&D:RawMissing?nullptr:&RawIrq;
 }
};
// Only the separately tested board/mapping/IRQ edge is substituted. DriverEntry,
// runtime owner, PDO/domain ownership and export gate are actual production code.
#include "WaveCyclicFactory.h"
#include "AudioEndpoint.h"
static bool PublishFixture;
static T70_WAVECYCLIC_FACTORY *LastFactory;
#define TESTREFS ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
struct FixtureStream:IMiniportWaveCyclicStream {TESTREFS
 NTSTATUS SetFormat(PKSDATAFORMAT)override{return STATUS_NOT_SUPPORTED;}ULONG SetNotificationFreq(ULONG,PULONG)override{return 0;}
 NTSTATUS SetState(KSSTATE)override{return STATUS_NOT_SUPPORTED;}NTSTATUS GetPosition(PULONG)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS NormalizePhysicalPosition(PLONGLONG)override{return STATUS_NOT_SUPPORTED;}void Silence(void *,ULONG)override{}
};
struct FixtureDma:IDmaChannel {TESTREFS
 NTSTATUS AllocateBuffer(ULONG,PHYSICAL_ADDRESS*)override{return STATUS_NOT_SUPPORTED;}void FreeBuffer()override{assert(false);}
 void *SystemAddress()override{return nullptr;}ULONG AllocatedBufferSize()override{return 0;}void SetBufferSize(ULONG)override{}
 void CopyTo(void*,void*,ULONG)override{assert(false);}
};
struct FixtureGroup:IServiceGroup {TESTREFS};
static FixtureStream FixtureS;static FixtureDma FixtureD;static FixtureGroup FixtureG;
static PMINIPORTWAVECYCLICSTREAM PublicS;static PDMACHANNEL PublicD;static PSERVICEGROUP PublicG;
static std::atomic<bool> CpuCloseBlocked{false};
static std::atomic<unsigned> CpuCloseCalls{0};
NTSTATUS T70WaveCyclicFactoryClose(T70_WAVECYCLIC_FACTORY *F){
 C(F&&!F->Stream&&!F->Buffer.Channel);CpuCloseCalls++;
 return CpuCloseBlocked?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS;
}
T70_WAVECYCLIC_MINIPORT_PROVIDER T70WaveCyclicFactoryProvider(T70_WAVECYCLIC_FACTORY *F){
 C(F&&F->Board&&F->Contract.Publish&&F->Contract.DrainExports);LastFactory=F;return {F,nullptr,nullptr};
}
NTSTATUS T70WaveCyclicBoardClose(T70_WAVECYCLIC_BOARD *O){
 Closes++;if(CloseStatus)return CloseStatus;
 if(O->DomainHeld){C(O->Contract.ReleaseDomain(O->Contract.Context)==0);O->DomainHeld=false;}
 if(O->Resources){O->Resources->Release();O->Resources=nullptr;}
 if(O->Parent){O->Parent->Release();O->Parent=nullptr;}return 0;
}
NTSTATUS T70WaveCyclicBoardOpen(T70_WAVECYCLIC_BOARD *O,PDEVICE_OBJECT D,PDEVICE_OBJECT P,
 PIRP,PUNKNOWN Parent,PUNKNOWN,PRESOURCELIST R,const T70_WAVECYCLIC_MINIPORT_PROVIDER &,
 const T70_WAVECYCLIC_BOARD_CONTRACT &K){
 LastBoard=O;BoardCalls++;C(D==ExpectedDevice&&P==AssociatedPdo&&K.AccessReady);
 auto M=R->FindTranslatedEntry(CmResourceTypeMemory,0);
 if(M->u.Memory.Start.QuadPart!=0x11210000)return STATUS_DEVICE_DATA_ERROR;
 O->Contract=K;Parent->AddRef();O->Parent=Parent;R->AddRef();O->Resources=R;
 NTSTATUS S=K.ClaimDomain(K.Context);if(S==0){O->DomainHeld=true;C(PdoRefs==2);S=K.AccessReady(K.Context);}
 C(S==AmplifierStartStatus);
 if(PublishFixture){
  C(LastFactory->Contract.Publish(LastFactory->Contract.Context,&FixtureS,&FixtureD,&FixtureG,&PublicS,&PublicD,&PublicG)==0);
  LastFactory->Published=true;LastFactory->Closing=true;
  // Deliberately fake the post-STOP/pre-COM-close boundary; never physical I/O.
  O->Irq.Initialized=true;O->Irq.Closed=true;O->Port.PublicationClosed=true;
  return S;
 }
 C(K.DetachStreams(K.Context)==0);
 C(K.DrainService(K.Context)==STATUS_SUCCESS); // idempotent after export reclaim
 T70WaveCyclicBoardClose(O);return S;
}

NTSTATUS T70AudioEndpointOpen(T70_AUDIO_ENDPOINT *,PDEVICE_OBJECT,PIRP,PUNKNOWN,PRESOURCELIST,T70_WAVECYCLIC_PORT_OWNER *){assert(false);return STATUS_DEVICE_NOT_READY;}
NTSTATUS T70AudioEndpointWithdraw(T70_AUDIO_ENDPOINT *O){Withdraws++;C(!O->Port&&!O->Connected&&!O->Registered);O->Closing=true;return 0;}
NTSTATUS T70AudioEndpointClose(T70_AUDIO_ENDPOINT *O){C(!O->Port&&!O->Connected&&!O->Registered);return 0;}
NTSTATUS T70AfeIrqRecoverySurpriseRemove(T70_AFE_IRQ_RECOVERY *,NTSTATUS (*)(void *),void *){assert(false);return STATUS_DEVICE_NOT_READY;}
static PDRIVER_ADD_DEVICE InstalledAdd;static PDRIVER_UNLOAD InstalledUnload;
static PCPFNSTARTDEVICE InstalledStart;static unsigned Added,Unloaded;
extern "C" NTSTATUS T70PortClsInitialize(T70_PORTCLS_OWNER *O,PDRIVER_OBJECT D,PUNICODE_STRING,
 PDRIVER_ADD_DEVICE A,PDRIVER_UNLOAD U){O->Driver=D;InstalledAdd=A;InstalledUnload=U;return 0;}
extern "C" NTSTATUS T70PortClsFinishUnload(T70_PORTCLS_OWNER *O){C(O->Driver!=nullptr);O->Driver=nullptr;Unloaded++;return 0;}
NTSTATUS PcAddAdapterDevice(PDRIVER_OBJECT D,PDEVICE_OBJECT P,PCPFNSTARTDEVICE S,ULONG N,ULONG Bytes){
 C(D&&P&&S&&N==2&&Bytes==PORT_CLASS_DEVICE_EXTENSION_SIZE+sizeof(T70_AUDIO_CYCLIC_DEVICE));InstalledStart=S;Added++;return 0;
}
NTSTATUS PcDispatchIrp(PDEVICE_OBJECT D,PIRP I){
 Forwarded++;if(PoisonRemove&&I->Stack.MinorFunction==IRP_MN_REMOVE_DEVICE)D->DeviceExtension=nullptr;
 return DispatchStatus;
}
void IoCompleteRequest(PIRP I,char Boost){C(Boost==IO_NO_INCREMENT&&I->IoStatus.Status==(NT_SUCCESS(CloseStatus)?STATUS_DEVICE_NOT_READY:CloseStatus)&&!I->IoStatus.Information);Completed++;}
int main(){
 DRIVER_OBJECT Driver={};UNICODE_STRING Registry={};Resources R;
 alignas(16) unsigned char Extension[PORT_CLASS_DEVICE_EXTENSION_SIZE+sizeof(T70_AUDIO_CYCLIC_DEVICE)]={};
 _DEVICE_OBJECT Device={Extension},Pdo={nullptr};_IRP StartIrp={};
 ExpectedDevice=&Device;AssociatedPdo=&Pdo;
 C(DriverEntry(&Driver,&Registry)==0&&InstalledAdd&&InstalledUnload);
 C(InstalledAdd(&Driver,ExpectedDevice)==0&&Added==1&&InstalledStart);

 for(Fail=0;Fail<=1;Fail++){
  Allocations=Frees=Maps=Unmaps=0;
  NTSTATUS S=InstalledStart(ExpectedDevice,&StartIrp,&R);
  C(S==(Fail?STATUS_INSUFFICIENT_RESOURCES:STATUS_DEVICE_NOT_READY));
  C(R.Refs==1&&!Maps&&!Unmaps&&!PdoRefs&&!Puts&&!DmaGetCalls);
  RunThreads();C(Frees==(Fail?0u:1u));
 }
 Fail=0;
 unsigned Reads=AmplifierReads,Opens=AmplifierOpens;
 R.ConnectionCount=1;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY);
 RunThreads();C(AmplifierReads==Reads&&AmplifierOpens==Opens&&!FrameworkLive);
 R.ConnectionCount=2;
 R.RawConnections[1].u.Connection.IdHighPart++;
 AmplifierStartStatus=STATUS_DEVICE_DATA_ERROR;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==AmplifierStartStatus);
 RunThreads();C(AmplifierReads==Reads&&AmplifierOpens==Opens&&!FrameworkLive);
 R.RawConnections[1].u.Connection.IdHighPart--;
 BadAmplifier=2;AmplifierStartStatus=STATUS_NO_SUCH_DEVICE;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==AmplifierStartStatus);
 RunThreads();C(AmplifierReads==Reads+2&&AmplifierOpens==Opens+1&&AmplifierOpens==AmplifierCloses&&!FrameworkLive);
 BadAmplifier=0;AmplifierStartStatus=STATUS_DEVICE_NOT_READY;
 FailThread=true;auto BeforeBoard=BoardCalls;auto BeforeThread=ThreadCreates;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_INSUFFICIENT_RESOURCES);
 C(BoardCalls==BeforeBoard&&ThreadCreates==BeforeThread+1&&!ThreadPins);FailThread=false;
 FailFramework=true;BeforeBoard=BoardCalls;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_INSUFFICIENT_RESOURCES);
 RunThreads();C(BoardCalls==BeforeBoard&&!FrameworkLive&&!ThreadPins&&!PdoRefs);FailFramework=false;
 EarlyThread=true;
 for(unsigned I=0;I<25;I++){C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY);RunThreads();C(!ThreadPins&&!PdoRefs);}
 EarlyThread=false;
 // A second Start reacquires the real software claim after failed admission.
 Fail=0;Allocations=Frees=0;C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY);
 RunThreads();C(Allocations==1&&Frees==1&&!PdoRefs);
 Fail=0;
 for(unsigned Bad=0;Bad<13;Bad++){
  Resources BadResources;
  switch(Bad){
   case 0:BadResources.IrqCount=0;break;
   case 1:BadResources.IrqCount=2;break;
   case 2:BadResources.RawMissing=true;break;
   case 3:BadResources.TranslatedMissing=true;break;
   case 4:BadResources.RawIrq.Type=CmResourceTypeMemory;break;
   case 5:BadResources.Irq.Type=CmResourceTypeMemory;break;
   case 6:BadResources.RawIrq.Flags=1;break; // edge
   case 7:BadResources.Irq.Flags=1;break;
   case 8:BadResources.RawIrq.Flags=2;break; // MSI
   case 9:BadResources.Irq.Flags=2;break;
   case 10:BadResources.RawIrq.ShareDisposition=3;break;
   case 11:BadResources.Irq.ShareDisposition=3;break;
   case 12:BadResources.RawIrq.u.Interrupt.Vector=234;break; // unconverted SPI
  }
  unsigned Before=Allocations,BeforeMaps=Maps;
  C(InstalledStart(ExpectedDevice,&StartIrp,&BadResources)==STATUS_DEVICE_DATA_ERROR);
  C(Allocations==Before&&Maps==BeforeMaps&&BadResources.Refs==1&&!DmaGetCalls);
 }
 for(unsigned Count: {0u,2u}){
  R.Count=Count;unsigned Before=Allocations;
  C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_DATA_ERROR);
  C(Allocations==Before&&!DmaGetCalls);
 }
 R.Count=1;R.D.u.Memory.Start.QuadPart++;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_DATA_ERROR);
 C(R.Refs==1&&Maps==Unmaps);
 C(InstalledStart(nullptr,&StartIrp,&R)==STATUS_INVALID_PARAMETER);
 RunThreads();
 // Retain initial Runtime ownership in the FDO across failed cleanup.
 R.D.u.Memory.Start.QuadPart=0x11210000;
 auto Slot=DeviceState(ExpectedDevice);auto Dispatch=Driver.MajorFunction[IRP_MJ_PNP];
 C(Slot&&Dispatch);CloseStatus=STATUS_DEVICE_NOT_READY;unsigned Before=Frees;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY);
 C(Slot->Runtime&&Slot->CleanupStatus==CloseStatus&&Frees==Before&&PdoRefs==2);
 auto Retained=Slot->Runtime;unsigned BeforeAlloc=Allocations;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY);
 C(Slot->Runtime==Retained&&Allocations==BeforeAlloc);
 // Queries of a simulated live board must not perform destructive cleanup.
 _IRP Q={};LastBoard->Started=true;auto C0=Closes,W0=Withdraws;
 for(auto Minor:{IRP_MN_QUERY_STOP_DEVICE,IRP_MN_QUERY_REMOVE_DEVICE}){
  Q.Stack.MinorFunction=Minor;C(Dispatch(ExpectedDevice,&Q)==STATUS_DEVICE_NOT_READY);
  C(Closes==C0&&Withdraws==W0&&LastBoard->Started&&Slot->Runtime==Retained&&!Slot->Removing);
 }
 Q.Stack.MinorFunction=IRP_MN_CANCEL_STOP_DEVICE;C(Dispatch(ExpectedDevice,&Q)==0&&LastBoard->Started);
 LastBoard->Started=false;Completed=Forwarded=0;
 Q.Stack.MinorFunction=IRP_MN_QUERY_REMOVE_DEVICE;
 C(Dispatch(ExpectedDevice,&Q)==STATUS_DEVICE_NOT_READY&&Completed==1&&!Forwarded&&Slot->Runtime==Retained);
 CloseStatus=0x103;C(Dispatch(ExpectedDevice,&Q)==STATUS_DEVICE_NOT_READY&&Completed==2&&!Forwarded&&Slot->Runtime==Retained);
 CloseStatus=0;C(Dispatch(ExpectedDevice,&Q)==0&&Forwarded==1&&!Slot->Runtime&&!PdoRefs);RunThreads();C(Frees==Before+1);
 Q.Stack.MinorFunction=IRP_MN_CANCEL_REMOVE_DEVICE;C(Dispatch(ExpectedDevice,&Q)==0&&Forwarded==2&&!Slot->Removing);
 // Failed START can be followed directly by final REMOVE, with no surprise IRP.
 CloseStatus=STATUS_DEVICE_NOT_READY;C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY);C(Slot->Runtime);
 // Leave generic Close failing: final removal releases only proven CPU-only state.
 Q.Stack.MinorFunction=IRP_MN_REMOVE_DEVICE;PoisonRemove=true;
 C(Dispatch(ExpectedDevice,&Q)==0&&Forwarded==3&&!Device.DeviceExtension&&!PdoRefs);
 Device.DeviceExtension=Extension;CloseStatus=0;C(!Slot->Runtime&&Slot->Removing);
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_INVALID_DEVICE_STATE);
 // A normal final REMOVE enters real runtime Release, unlike surprise.
 // The fake board edge models a completed normal STOP and releases its refs.
 *Slot={};CloseStatus=STATUS_DEVICE_NOT_READY;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY&&Slot->Runtime);
 LastBoard->Started=true;C0=Closes;CloseStatus=0;
 Q.Stack.MinorFunction=IRP_MN_REMOVE_DEVICE;
 C(Dispatch(ExpectedDevice,&Q)==0&&!Device.DeviceExtension&&!Slot->Runtime&&!PdoRefs);
 C(Closes==C0+1&&!Slot->SurpriseRemoved);
 Device.DeviceExtension=Extension;
 // Unexpected mapped state is never sent through normal MMIO close after
 // surprise. This is containment only; actual physical admission is sealed.
 *Slot={};CloseStatus=STATUS_DEVICE_NOT_READY;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY&&Slot->Runtime);
 LastBoard->Mapping.Mapping.VirtualBase=(void *)(uintptr_t)0x1000;
 C0=Closes;W0=Withdraws;Q.Stack.MinorFunction=IRP_MN_SURPRISE_REMOVAL;
 C(Dispatch(ExpectedDevice,&Q)==0&&Slot->Removing&&Slot->Runtime);
 C(Closes==C0&&Withdraws==W0&&Slot->CleanupStatus==STATUS_DEVICE_NOT_READY);
 // Remove the fake marker, not a device allocation; real DMA cannot be freed
 // by asserting this in production. Finish the actual CPU-only fixture.
 LastBoard->Mapping.Mapping.VirtualBase=nullptr;
 Q.Stack.MinorFunction=IRP_MN_REMOVE_DEVICE;C(Dispatch(ExpectedDevice,&Q)==0&&!Device.DeviceExtension&&!PdoRefs);
 Device.DeviceExtension=Extension;CloseStatus=0;
 // Separate never-started device: surprise/remove forwards without allocation.
 *Slot={};Q.Stack.MinorFunction=IRP_MN_SURPRISE_REMOVAL;C(Dispatch(ExpectedDevice,&Q)==0&&Slot->Removing);
 Q.Stack.MinorFunction=IRP_MN_REMOVE_DEVICE;C(Dispatch(ExpectedDevice,&Q)==0&&!Device.DeviceExtension);
 Device.DeviceExtension=Extension;
 Irql=2;C(InstalledAdd(&Driver,ExpectedDevice)==STATUS_INVALID_PARAMETER);Irql=0;
 RunThreads();
 // Live exported COM references survive final REMOVE and a pending PortCls
 // dispatch result. The extension disappears before heap-only retirement runs.
 *Slot={};Device.DeviceExtension=Extension;CloseStatus=STATUS_DEVICE_NOT_READY;PublishFixture=true;
 C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY&&Slot->Runtime&&PublicS);
 PublishFixture=false;auto OwnerRef=LastBoard->Parent;OwnerRef->AddRef(); // late sink-like Parent reference
 // A provider admitted before proxy publication must block STOP/withdraw.
 alignas(T70WaveCyclicMiniport) unsigned char FakeMiniport[sizeof(T70WaveCyclicMiniport)]={};
 LastBoard->Port.Miniport=reinterpret_cast<T70WaveCyclicMiniport *>(FakeMiniport);
 auto BeforeQuiesce=MiniportQuiesces;auto BeforeStop=Closes;auto BeforeWithdraw=Withdraws;
 C(T70AudioCyclicRelease(Slot)==STATUS_DEVICE_NOT_READY);
 C(MiniportQuiesces==BeforeQuiesce+1&&Closes==BeforeStop&&Withdraws==BeforeWithdraw&&Slot->Runtime);
 LastBoard->Port.Miniport=nullptr;
 DispatchStatus=0x103;Q.Stack.MinorFunction=IRP_MN_REMOVE_DEVICE;
 C(Dispatch(ExpectedDevice,&Q)==0x103&&!Device.DeviceExtension&&!Slot->Runtime);
 C(HostThreads.size()==1&&ThreadPins==1);
 auto Task=HostThreads.back();HostThreads.clear();std::atomic<bool> Exited{false};
 std::thread Retired([&]{Task->Routine(Task->Context);Exited=true;});
 C(!Exited);CpuCloseBlocked=true;auto CallsBefore=CpuCloseCalls.load();
 Irql=DISPATCH_LEVEL;PublicS->Release();PublicD->Release();PublicG->Release();Irql=0;
 while(CpuCloseCalls==CallsBefore)std::this_thread::yield();
 // NOT_READY must sleep, rather than quarantine or self-signal a retry loop.
 OwnerRef->AddRef();OwnerRef->Release();
 auto Event=HostEvents.back();unsigned StableCalls=0;
 for(;;){
  {std::lock_guard<std::mutex> L(Event->Lock);
   if(Event->Waiting&&!Event->Set){StableCalls=CpuCloseCalls.load();break;}}
  std::this_thread::yield();
 }
 for(unsigned I=0;I<10000;I++)std::this_thread::yield();
 C(CpuCloseCalls==StableCalls&&!Exited);
 CpuCloseBlocked=false;
 // Export refs may have drained, but Parent refs still protect the Runtime.
 C(!Exited);Irql=DISPATCH_LEVEL;OwnerRef->Release();Irql=0;
 Retired.join();C(Exited&&!PdoRefs&&R.Refs==1&&FixtureS.Refs==1&&FixtureD.Refs==1&&FixtureG.Refs==1);
 ThreadPins--;delete Task;DispatchStatus=0;CloseStatus=0;Device.DeviceExtension=Extension;
 // Quarantine deliberately retains lifetime indefinitely. Isolate its fixture
 // in a child so the parent test never pretends to release unproved hardware.
 {
  pid_t Child=fork();C(Child>=0);
  if(!Child){
   *Slot={};CloseStatus=STATUS_DEVICE_NOT_READY;
   C(InstalledStart(ExpectedDevice,&StartIrp,&R)==STATUS_DEVICE_NOT_READY&&Slot->Runtime);
   LastBoard->Mapping.Mapping.VirtualBase=(void*)(uintptr_t)0x1000;
   auto BeforeClose=Closes;Q.Stack.MinorFunction=IRP_MN_SURPRISE_REMOVAL;
   C(Dispatch(ExpectedDevice,&Q)==0&&Slot->Runtime&&Closes==BeforeClose);
   Q.Stack.MinorFunction=IRP_MN_REMOVE_DEVICE;
   C(Dispatch(ExpectedDevice,&Q)==0&&!Device.DeviceExtension&&!Slot->Runtime);
   C(ThreadPins==1&&HostThreads.size()==1&&PdoRefs==2&&Closes==BeforeClose);
   _exit(0); // no fictional rundown of the intentionally quarantined object
  }
  int Status=0;C(waitpid(Child,&Status,0)==Child&&WIFEXITED(Status)&&WEXITSTATUS(Status)==0);
 }
 RunThreads();C(!ThreadPins);
 for(auto E:HostEvents)delete E;for(auto L:HostLocks)delete L;
 C(FrameworkCreates>0&&FrameworkCreates==FrameworkDeletes&&!FrameworkLive);
 InstalledUnload(&Driver);C(Unloaded==1&&!DriverOwner.Driver);
 printf("Audio DriverEntry/start: %u checks; real cyclic/domain/export owner, fake board edge; physical access unavailable\n",Checks.load());
}
