// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeInterruptOwner.h"
#include "AfeIrqSource.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <cstdio>
static unsigned Irql;
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

static unsigned Connects,Disconnects,Masks,Drains,Faults;static NTSTATUS ConnectStatus,ReadyStatus,MaskStatus,DrainStatus;
static unsigned Malformed;static bool Fire;static PKINTERRUPT Object=(PKINTERRUPT)(uintptr_t)0x1234;
static PDEVICE_OBJECT Pdo=(PDEVICE_OBJECT)(uintptr_t)0x2345;
NTSTATUS IoConnectInterruptEx(IO_CONNECT_INTERRUPT_PARAMETERS *P){
 ++Connects;assert(P->Version==CONNECT_FULLY_SPECIFIED_GROUP);
 auto &F=P->FullySpecified;assert(F.PhysicalDeviceObject==Pdo&&F.Vector==0x456&&F.Irql==7&&F.SynchronizeIrql==7&&F.Group==2&&F.ProcessorEnableMask==4&&!F.ShareVector&&!F.FloatingSave&&!F.SpinLock&&F.InterruptMode==LevelSensitive);
 assert(F.ServiceRoutine==T70AfeDl1Interrupt);
 if(Fire)assert(!F.ServiceRoutine(Object,F.ServiceContext));
 if(ConnectStatus)return ConnectStatus;
 *F.InterruptObject=Malformed==1?nullptr:Object;if(Malformed==2)P->Version=99;return 0;
}
void IoDisconnectInterruptEx(IO_DISCONNECT_INTERRUPT_PARAMETERS *P){++Disconnects;assert(P->Version==CONNECT_FULLY_SPECIFIED_GROUP&&P->ConnectionContext.InterruptObject==Object);}
BOOLEAN KeSynchronizeExecution(PKINTERRUPT I,BOOLEAN (*F)(void *),void *C){assert(I==Object);Irql=7;auto R=F(C);Irql=0;return R;}
static ULONG On=1,Pending=1;
static NTSTATUS SourceRead(void *,ULONG A,ULONG *V){assert(Irql==7);*V=A==0x3a0?On:A==0x3b4?1:Pending;return 0;}
static NTSTATUS SourceWrite(void *,ULONG A,ULONG V){assert(Irql==7);if(A==0x3a0)On=V;else {assert(A==0x3a8&&V==1);Pending=0;}return 0;}
static void Schedule(void *,NTSTATUS S){assert(Irql==7&&S==STATUS_DEVICE_DATA_ERROR);++Faults;}
static NTSTATUS Ready(void *){return ReadyStatus;}
static NTSTATUS Mask(void *){++Masks;return MaskStatus;}
static NTSTATUS Drain(void *){++Drains;return DrainStatus;}
static NTSTATUS Read(void *,ULONG,ULONG *){return STATUS_DEVICE_DATA_ERROR;}
static NTSTATUS Write(void *,ULONG,ULONG){assert(false);return -1;}
static void Fault(void *,NTSTATUS S){assert(S==STATUS_DEVICE_DATA_ERROR);++Faults;}
int main(){
 Port P;Group Parent;Parent.Refs=1;
 T70_AFE_INTERRUPT_RESOURCE R={Pdo,266,0x456,7,2,4};
 T70_AFE_DL1_INTERRUPT H={nullptr,Read,Write,nullptr,true,0,0,0,Fault,nullptr};
 for(unsigned T=0;T<5;T++){
  T70_AFE_INTERRUPT_OWNER O={};Connects=Disconnects=Masks=Drains=Faults=0;Fire=T==4||T==2;
  ReadyStatus=T==0?STATUS_DEVICE_NOT_READY:0;CreateStatus=T==1?STATUS_INSUFFICIENT_RESOURCES:0;
  ConnectStatus=T==2?STATUS_NOT_SUPPORTED:0;MaskStatus=DrainStatus=0;
  auto S=T70AfeInterruptConnect(&O,&Parent,&P,R,H,Ready,nullptr);
  if(T<2)assert(S!=0&&!O.Parent&&!O.Interrupt&&P.Refs==1&&Parent.Refs==1);
  else if(T==2){assert(S!=0&&O.Parent&&!O.Interrupt&&Faults==1&&P.Refs==2);
   DrainStatus=STATUS_DEVICE_NOT_READY;assert(T70AfeInterruptClose(&O,Mask,Drain,nullptr)!=0&&O.Parent&&!Masks);
   DrainStatus=0;}
  else{
   assert(S==(Fire?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS)&&O.Interrupt==Object&&Parent.Refs==2&&P.Refs==2);
   assert(Faults==(Fire?1u:0u));
   MaskStatus=0x103;assert(T70AfeInterruptClose(&O,Mask,Drain,nullptr)==STATUS_DEVICE_NOT_READY&&!Disconnects&&O.Interrupt);
   MaskStatus=0;DrainStatus=STATUS_DEVICE_NOT_READY;
   assert(T70AfeInterruptClose(&O,Mask,Drain,nullptr)==STATUS_DEVICE_NOT_READY&&Disconnects==1&&!O.Interrupt&&O.Parent);
   auto M=Masks;DrainStatus=0;assert(T70AfeInterruptClose(&O,Mask,Drain,nullptr)==0&&Masks==M&&Disconnects==1);
  }
  assert(T70AfeInterruptClose(&O,Mask,Drain,nullptr)==0&&Parent.Refs==1&&P.Refs==1&&!G.Refs);
 }
 T70_AFE_INTERRUPT_OWNER O={};Connects=0;R.RawGsi=234;
 assert(T70AfeInterruptConnect(&O,&Parent,&P,R,H,Ready,nullptr)!=0&&!Connects&&!O.Attempted);
 R.RawGsi=266;H.OnFault=nullptr;assert(T70AfeInterruptConnect(&O,&Parent,&P,R,H,Ready,nullptr)!=0&&!Connects);
 H.OnFault=Fault;ReadyStatus=CreateStatus=ConnectStatus=0;Fire=false;
 for(unsigned Bad=1;Bad<=2;Bad++){
  Malformed=Bad;T70_AFE_INTERRUPT_OWNER Q={};
  assert(T70AfeInterruptConnect(&Q,&Parent,&P,R,H,Ready,nullptr)==STATUS_DEVICE_DATA_ERROR&&Q.ConnectionUnknown);
  auto D=Disconnects;assert(T70AfeInterruptClose(&Q,Mask,Drain,nullptr)==STATUS_DEVICE_NOT_READY&&Disconnects==D&&Q.Parent);
  // Fixture-only disposal: production intentionally has no guessed recovery.
  P.Refs=1;Parent.Refs=1;G.Refs=0;
 }
 Malformed=0;Fire=false;T70_AFE_INTERRUPT_OWNER Real={};
 T70_AFE_IRQ_SOURCE Source={&Real,Schedule,nullptr,0,0};
 H.Read32=SourceRead;H.Write32=SourceWrite;H.OnFault=T70AfeIrqSourceFault;H.FaultContext=&Source;
 assert(T70AfeInterruptConnect(&Real,&Parent,&P,R,H,Ready,nullptr)==0);
 Irql=7;H.OnFault(H.FaultContext,STATUS_DEVICE_DATA_ERROR);Irql=0;
 assert(Source.MaskStatus==0&&!On&&!Pending&&Real.Parent);
 DrainStatus=STATUS_DEVICE_NOT_READY;auto D=Disconnects;
 assert(T70AfeInterruptClose(&Real,T70AfeIrqSourceMask,Drain,&Source)==STATUS_DEVICE_NOT_READY);
 assert(Disconnects==D+1&&!Real.Interrupt&&Real.Parent);
 DrainStatus=0;assert(T70AfeInterruptClose(&Real,T70AfeIrqSourceMask,Drain,&Source)==0);
 assert(Disconnects==D+1&&Parent.Refs==1&&P.Refs==1&&!G.Refs);
 puts("AFE OS IRQ owner: exact translated resources, pre-return ISR, mask/disconnect/drain lifetime passed");
}
