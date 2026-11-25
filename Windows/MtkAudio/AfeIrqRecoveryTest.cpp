// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeInterruptOwner.h"
#include "AfeIrqRecovery.h"
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
static thread_local unsigned Irql;
static NTSTATUS CreateStatus;static bool NullGroup;
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
LONG InterlockedCompareExchange(volatile LONG *P,LONG N,LONG V){__atomic_compare_exchange_n(P,&V,N,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return V;}
LONG InterlockedDecrement(volatile LONG *P){return __atomic_sub_fetch(P,1,__ATOMIC_SEQ_CST);}
struct Group:IServiceGroup {unsigned Refs=0;ULONG AddRef()override{return ++Refs;}ULONG Release()override{assert(Refs);return --Refs;}};static Group G;
NTSTATUS PcNewServiceGroup(PSERVICEGROUP *Out,PUNKNOWN O){assert(!O);if(CreateStatus)return CreateStatus;if(!NullGroup){G.Refs=1;*Out=&G;}return 0;}
struct Port:IPortWaveCyclic {
 unsigned Refs=1;std::atomic<unsigned> Calls{0};std::atomic<bool> Hold{false},Entered{false};
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{assert(Refs>1);return --Refs;}
 NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)override{assert(false);return -1;}
 NTSTATUS NewMasterDmaChannel(IDmaChannel **,PUNKNOWN,PRESOURCELIST,ULONG,BOOLEAN,BOOLEAN,DMA_WIDTH,DMA_SPEED)override{assert(false);return -1;}
 void Notify(IServiceGroup *Group)override{assert(Group==&G&&Refs==2&&G.Refs==1);++Calls;Entered=true;while(Hold)std::this_thread::yield();}
};

static unsigned ExpectedGroup=2;
static unsigned Connects,Disconnects,Drains,Faults;static NTSTATUS ConnectStatus,ReadyStatus,DrainStatus;
static unsigned Malformed;static bool Fire;static PKINTERRUPT Object=(PKINTERRUPT)(uintptr_t)0x1234;
static PDEVICE_OBJECT Pdo=(PDEVICE_OBJECT)(uintptr_t)0x2345;
NTSTATUS IoConnectInterruptEx(IO_CONNECT_INTERRUPT_PARAMETERS *P){
 ++Connects;assert(P->Version==CONNECT_FULLY_SPECIFIED_GROUP);
 auto &F=P->FullySpecified;assert(F.PhysicalDeviceObject==Pdo&&F.Vector==0x456&&F.Irql==7&&F.SynchronizeIrql==7&&F.Group==ExpectedGroup&&F.ProcessorEnableMask==4&&!F.ShareVector&&!F.FloatingSave&&!F.SpinLock&&F.InterruptMode==LevelSensitive);
 assert(F.ServiceRoutine==T70AfeDl1Interrupt);
 if(Fire){Irql=7;assert(!F.ServiceRoutine(Object,F.ServiceContext));Irql=0;KeFlushQueuedDpcs();}
 if(ConnectStatus)return ConnectStatus;
 *F.InterruptObject=Malformed==1?nullptr:Object;if(Malformed==2)P->Version=99;return 0;
}
void IoDisconnectInterruptEx(IO_DISCONNECT_INTERRUPT_PARAMETERS *P){++Disconnects;assert(P->Version==CONNECT_FULLY_SPECIFIED_GROUP&&P->ConnectionContext.InterruptObject==Object);}
static bool SyncFail;
static std::mutex InterruptLock;
static std::atomic<unsigned> SyncAttempts{0};
static volatile LONG *FaultBeforeSync;
BOOLEAN KeSynchronizeExecution(PKINTERRUPT I,BOOLEAN (*F)(void *),void *C){assert(I==Object);++SyncAttempts;std::lock_guard<std::mutex> Lock(InterruptLock);if(SyncFail)return FALSE;auto Previous=Irql;Irql=7;if(FaultBeforeSync){InterlockedCompareExchange(FaultBeforeSync,1,0);FaultBeforeSync=nullptr;}auto R=F(C);Irql=Previous;return R;}
static unsigned Reads,Writes;static bool ForbidIo;
static ULONG On=1,Pending=1;static bool ReadFailOnce,Stuck;
static NTSTATUS SourceRead(void *,ULONG A,ULONG *V){assert(Irql==7&&!ForbidIo);++Reads;if(ReadFailOnce){ReadFailOnce=false;return STATUS_DEVICE_DATA_ERROR;}*V=A==0x3a0?On:A==0x3b4?1:Pending;return 0;}
static NTSTATUS SourceWrite(void *,ULONG A,ULONG V){assert(Irql==7&&!ForbidIo);++Writes;if(A==0x3a0){if(!Stuck)On=V;}else {assert(A==0x3a8&&V==1);Pending=0;}return 0;}

static NTSTATUS Ready(void *){return ReadyStatus;}

static NTSTATUS Drain(void *){++Drains;return DrainStatus;}


static void Fault(void *,NTSTATUS S){assert(S==STATUS_DEVICE_DATA_ERROR);++Faults;}

struct HostWait {std::mutex Mutex;std::condition_variable Cv;bool Signaled=false;bool IsMutex=false;};
static std::vector<HostWait*> Waits;
struct IO_WORKITEM{PDEVICE_OBJECT Device;};
static bool FailAllocate;static unsigned Queues,Frees;static KDPC *PendingDpc;
static std::vector<std::thread> Workers;
PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT D){return FailAllocate?nullptr:new IO_WORKITEM{D};}
void IoFreeWorkItem(PIO_WORKITEM W){++Frees;delete W;}
void IoQueueWorkItem(PIO_WORKITEM W,void (*F)(PDEVICE_OBJECT,void *),WORK_QUEUE_TYPE,void *C){
 assert(Irql==DISPATCH_LEVEL);++Queues;auto D=W->Device;
 Workers.emplace_back([=]{Irql=0;F(D,C);});
}
void KeInitializeDpc(KDPC *D,void (*F)(KDPC *,void *,void *,void *),void *C){D->Routine=F;D->Context=C;}
BOOLEAN KeInsertQueueDpc(KDPC *D,void *,void *){if(PendingDpc==D)return FALSE;assert(!PendingDpc);PendingDpc=D;return TRUE;}
void KeFlushQueuedDpcs(){if(PendingDpc){auto D=PendingDpc;PendingDpc=nullptr;Irql=DISPATCH_LEVEL;D->Routine(D,D->Context,nullptr,nullptr);Irql=0;}}
void KeInitializeMutex(KMUTEX *M,ULONG){auto H=new HostWait;H->IsMutex=true;M->Host=H;Waits.push_back(H);}
void KeInitializeEvent(KEVENT *E,EVENT_TYPE,BOOLEAN){auto H=new HostWait;E->Host=H;Waits.push_back(H);}
NTSTATUS KeWaitForSingleObject(void *P,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,void *){
 auto H=*(HostWait**)P;if(H->IsMutex){H->Mutex.lock();return 0;}
 std::unique_lock<std::mutex>L(H->Mutex);H->Cv.wait(L,[&]{return H->Signaled;});return 0;
}
LONG KeReleaseMutex(KMUTEX *M,BOOLEAN){((HostWait*)M->Host)->Mutex.unlock();return 0;}
LONG KeSetEvent(KEVENT *E,LONG,BOOLEAN){auto H=(HostWait*)E->Host;std::lock_guard<std::mutex>L(H->Mutex);H->Signaled=true;H->Cv.notify_all();return 0;}
int main(){
 Port P;Group Parent;Parent.Refs=1;
 T70_AFE_INTERRUPT_RESOURCE Resource={Pdo,266,0x456,7,2,4};
 T70_AFE_DL1_INTERRUPT H={nullptr,SourceRead,SourceWrite,nullptr,true,0,0,0,Fault,nullptr};
 T70_AFE_IRQ_RECOVERY Bad={};FailAllocate=true;
 assert(T70AfeIrqRecoveryInit(&Bad,&Parent,Pdo)==STATUS_INSUFFICIENT_RESOURCES&&Parent.Refs==1&&!Bad.Initialized);
 FailAllocate=false;
 for(unsigned Case=0;Case<7;Case++){
  T70_AFE_IRQ_RECOVERY R={};On=Pending=1;Queues=Frees=0;ConnectStatus=0;DrainStatus=0;Fire=Case>=4;Stuck=Case==6;ReadFailOnce=Fire;if(Case==5)ConnectStatus=STATUS_NOT_SUPPORTED;
  assert(T70AfeIrqRecoveryInit(&R,&Parent,Pdo)==0&&Parent.Refs==2);
  assert(T70AfeIrqRecoveryInit(&R,&Parent,Pdo)!=0);
  auto Connected=T70AfeIrqRecoveryConnect(&R,&P,Resource,H,Ready,nullptr);
  assert(Connected==(Case==5?STATUS_NOT_SUPPORTED:Case>=4?STATUS_DEVICE_NOT_READY:0));
  unsigned Transactions=0;
  auto Operation=[](void *C)->NTSTATUS{assert(Irql==7);++*(unsigned*)C;return STATUS_SUCCESS;};
  auto Executed=T70AfeIrqRecoveryExecute(&R,Operation,&Transactions);
  assert((Executed==0)==(Case<4)&&Transactions==(Case<4?1u:0u));
  if(Case==0){
   auto N=Transactions;Irql=DISPATCH_LEVEL;assert(T70AfeIrqRecoveryObserve(&R,Operation,&Transactions)==0&&Transactions==N+1&&Irql==DISPATCH_LEVEL);Irql=0;
   R.ObservationGate=0x3fffffff;assert(T70AfeIrqRecoveryObserve(&R,Operation,&Transactions)!=0);R.ObservationGate=0;
  }
  if(Case==0){auto N=Transactions;FaultBeforeSync=&R.Owner.Handler.Faulted;
   assert(T70AfeIrqRecoveryExecute(&R,Operation,&Transactions)==STATUS_DEVICE_NOT_READY&&Transactions==N);
   // Fixture only: remaining case validates independent clean shutdown.
   R.Owner.Handler.Faulted=0;
  }
  if(Case&&Case<4){
   Irql=7;R.Owner.Handler.OnFault(R.Owner.Handler.FaultContext,STATUS_DEVICE_DATA_ERROR);
   R.Owner.Handler.OnFault(R.Owner.Handler.FaultContext,STATUS_NOT_SUPPORTED);Irql=0;
   assert(R.OriginalError==STATUS_DEVICE_DATA_ERROR);
   if(Case==2)KeFlushQueuedDpcs(); // work competes with Close for mutex
  }
  if(Case==3)DrainStatus=STATUS_DEVICE_NOT_READY;
  auto S=T70AfeIrqRecoveryClose(&R,Drain,nullptr);
  if(Case==6){assert(S!=0&&R.Owner.Interrupt&&R.Parent&&!Frees);auto Count=Transactions;assert(T70AfeIrqRecoveryExecute(&R,Operation,&Transactions)!=0&&Transactions==Count);KeFlushQueuedDpcs();for(auto &W:Workers)W.join();Workers.clear();assert(R.RecoveryStatus!=0&&R.OriginalError==STATUS_DEVICE_DATA_ERROR);Stuck=false;S=T70AfeIrqRecoveryClose(&R,Drain,nullptr);}
  if(Case==3){assert(S!=0&&R.Parent&&!R.Closed&&!R.Owner.Interrupt);DrainStatus=0;S=T70AfeIrqRecoveryClose(&R,Drain,nullptr);}
  assert(S==0&&R.Closed&&!R.Work&&!R.Parent&&Parent.Refs==1&&P.Refs==1&&!G.Refs);
  auto Before=Transactions;assert(T70AfeIrqRecoveryExecute(&R,Operation,&Transactions)!=0&&Transactions==Before);
  assert(Frees==1&&Queues==(Case?1u:0u));
  assert(T70AfeIrqRecoveryClose(&R,Drain,nullptr)==0&&Frees==1);
  for(auto &W:Workers)W.join();Workers.clear();
 }
 {
  T70_AFE_IRQ_RECOVERY R={};On=Pending=1;Stuck=Fire=ReadFailOnce=false;ConnectStatus=DrainStatus=0;
  assert(T70AfeIrqRecoveryInit(&R,&Parent,Pdo)==0&&T70AfeIrqRecoveryConnect(&R,&P,Resource,H,Ready,nullptr)==0);
  std::atomic<bool> In{false},Leave{false};struct WaitContext {std::atomic<bool> *In,*Leave;};WaitContext C={&In,&Leave};
  auto Blocking=[](void *X)->NTSTATUS{auto C=(WaitContext *)X;*C->In=true;while(!*C->Leave)std::this_thread::yield();return 0;};
  std::thread Observer([&]{Irql=DISPATCH_LEVEL;assert(T70AfeIrqRecoveryObserve(&R,Blocking,&C)==0);});
  while(!In)std::this_thread::yield();auto D=Disconnects;
  assert(T70AfeIrqRecoveryClose(&R,Drain,nullptr)==STATUS_DEVICE_NOT_READY&&R.Owner.Interrupt&&Disconnects==D&&R.Parent);
  unsigned N=0;auto No=[](void *X)->NTSTATUS{++*(unsigned*)X;return 0;};
  assert(T70AfeIrqRecoveryObserve(&R,No,&N)!=0&&!N);
  Leave=true;Observer.join();assert(T70AfeIrqRecoveryClose(&R,Drain,nullptr)==0&&Disconnects==D+1);
 }
 // Removal waits for an already executing synchronized observation, then
 // prevents subsequent callbacks. The memory reservation may need a retry.
 {
  T70_AFE_IRQ_RECOVERY R={};On=Pending=1;Stuck=Fire=ReadFailOnce=false;ConnectStatus=DrainStatus=0;
  assert(T70AfeIrqRecoveryInit(&R,&Parent,Pdo)==0&&T70AfeIrqRecoveryConnect(&R,&P,Resource,H,Ready,nullptr)==0);
  std::atomic<bool> In{false},Leave{false},Finished{false};
  struct State{std::atomic<bool> *In,*Leave;};State C={&In,&Leave};
  auto Blocking=[](void *X)->NTSTATUS{auto C=(State*)X;*C->In=true;while(!*C->Leave)std::this_thread::yield();return 0;};
  std::thread Observer([&]{Irql=DISPATCH_LEVEL;assert(T70AfeIrqRecoveryObserve(&R,Blocking,&C)==0);});
  while(!In)std::this_thread::yield();NTSTATUS Result=STATUS_DEVICE_NOT_READY;
  std::thread Removal([&]{Result=T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr);Finished=true;});
  while(!InterlockedCompareExchange(&R.RemovalRequested,0,0))std::this_thread::yield();
  assert(!Finished);Leave=true;Observer.join();Removal.join();
  assert(Result==0||Result==STATUS_DEVICE_NOT_READY);
  assert(T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr)==0&&R.Closed&&Parent.Refs==1);
 }
 // Worker owns Mutex first and waits at the actual IRQ synchronization edge.
 // Removal publication must overtake that queued mask before its first MMIO.
 {
  T70_AFE_IRQ_RECOVERY R={};On=Pending=1;Stuck=Fire=ReadFailOnce=false;ConnectStatus=DrainStatus=0;
  assert(T70AfeIrqRecoveryInit(&R,&Parent,Pdo)==0&&T70AfeIrqRecoveryConnect(&R,&P,Resource,H,Ready,nullptr)==0);
  Irql=7;R.Owner.Handler.OnFault(R.Owner.Handler.FaultContext,STATUS_DEVICE_DATA_ERROR);Irql=0;
  InterruptLock.lock();auto BeforeSync=SyncAttempts.load();KeFlushQueuedDpcs();
  while(SyncAttempts.load()==BeforeSync)std::this_thread::yield();
  NTSTATUS Removed=STATUS_DEVICE_NOT_READY;
  std::thread Removal([&]{Removed=T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr);});
  while(!InterlockedCompareExchange(&R.Owner.Removed,0,0))std::this_thread::yield();
  auto BeforeReads=Reads,BeforeWrites=Writes;ForbidIo=true;InterruptLock.unlock();
  Removal.join();for(auto &W:Workers)W.join();Workers.clear();
  assert(Removed==0&&R.Closed&&Reads==BeforeReads&&Writes==BeforeWrites);
  assert(R.OriginalError==STATUS_DEVICE_DATA_ERROR);ForbidIo=false;
 }
 // Synchronization failure cannot fall back to normal source-mask teardown.
 {
  T70_AFE_IRQ_RECOVERY R={};On=Pending=1;Stuck=Fire=ReadFailOnce=false;ConnectStatus=DrainStatus=0;
  assert(T70AfeIrqRecoveryInit(&R,&Parent,Pdo)==0&&T70AfeIrqRecoveryConnect(&R,&P,Resource,H,Ready,nullptr)==0);
  SyncFail=true;ForbidIo=true;auto D=Disconnects;
  assert(T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr)==STATUS_DEVICE_NOT_READY);
  assert(T70AfeIrqRecoveryClose(&R,Drain,nullptr)==STATUS_DEVICE_NOT_READY);
  assert(R.Parent&&R.Owner.Interrupt&&Disconnects==D&&!R.Closed);
  SyncFail=false;assert(T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr)==0&&R.Closed);
  ForbidIo=false;
 }
 // Surprise removal must never run source masking, even with queued recovery.
 for(unsigned Case=0;Case<3;Case++){
  T70_AFE_IRQ_RECOVERY R={};On=Pending=1;Stuck=Fire=ReadFailOnce=false;ConnectStatus=DrainStatus=0;
  assert(T70AfeIrqRecoveryInit(&R,&Parent,Pdo)==0&&T70AfeIrqRecoveryConnect(&R,&P,Resource,H,Ready,nullptr)==0);
  if(Case==1){Irql=7;R.Owner.Handler.OnFault(R.Owner.Handler.FaultContext,STATUS_DEVICE_DATA_ERROR);Irql=0;}
  if(Case==2)DrainStatus=STATUS_DEVICE_NOT_READY;
  auto BeforeReads=Reads,BeforeWrites=Writes,D=Disconnects;ForbidIo=true;
  auto S=T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr);
  assert(R.Owner.Removed&&R.RemovalRequested&&!R.Owner.Interrupt&&!R.Owner.SourceMasked&&Disconnects==D+1);
  if(Case==2){assert(S==STATUS_DEVICE_NOT_READY&&R.Parent&&R.Work&&!R.Closed);DrainStatus=0;S=T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr);}
  assert(S==0&&R.Closed&&Parent.Refs==1&&P.Refs==1&&!G.Refs&&Reads==BeforeReads&&Writes==BeforeWrites);
  unsigned N=0;auto No=[](void *X)->NTSTATUS{++*(unsigned*)X;return 0;};
  assert(T70AfeIrqRecoveryExecute(&R,No,&N)!=0&&T70AfeIrqRecoveryObserve(&R,No,&N)!=0&&!N);
  Irql=7;assert(!T70AfeDl1Interrupt(Object,&R.Owner.Handler));Irql=0;
  assert(T70AfeIrqRecoverySurpriseRemove(&R,Drain,nullptr)==0&&Disconnects==D+1);
  for(auto &W:Workers)W.join();Workers.clear();ForbidIo=false;
 }
 for(auto W:Waits)delete W;
 puts("AFE recovery: real source/owner, DPC->work, first fault, close contention and drain retry passed");
 return 0;
}
