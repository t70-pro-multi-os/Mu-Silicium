// SPDX-License-Identifier: BSD-2-Clause-Patent
#define main RecoveryFixtureMain
#include "AfeIrqRecoveryTest.cpp"
#undef main
#include "WaveCyclicBoard.h"
#include <cstring>
static unsigned Maps,Unmaps,Claims,Releases,Detaches;
static NTSTATUS WithdrawStatus;static unsigned WithdrawCalls;
static bool MapFail,QiFail,Partial;static NTSTATUS ClaimStatus,DetachStatus,DomainReleaseStatus,PortStatus;
alignas(16) static ULONG Registers[0x800];
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS A,size_t N,ULONG F){assert(A.QuadPart==0x11210000&&N==0x2000&&F==(PAGE_READWRITE|PAGE_NOCACHE));++Maps;return MapFail?nullptr:Registers;}
void MmUnmapIoSpace(void *V,size_t N){assert(V==Registers&&N==0x2000);++Unmaps;}
ULONG READ_REGISTER_ULONG(volatile ULONG *P){assert(P>=Registers&&P<Registers+0x800);return *P;}
void WRITE_REGISTER_ULONG(volatile ULONG *P,ULONG V){assert(P>=Registers&&P<Registers+0x800);if(P==Registers+0x3a8/4)Registers[0x3a4/4]&=~V;else *P=V;}
struct BoardPort:Port {
 NTSTATUS QueryInterface(const GUID &G,void **O)override{assert(G.Data1==0xb4c90a26);if(QiFail)return STATUS_NOT_SUPPORTED;AddRef();*O=this;return 0;}
};static BoardPort BP;
// PortCls initialization and miniport lifetime have separate real-code tests.
// This test substitutes that already-tested edge, not the IRQ/mapping owners.
NTSTATUS T70WaveCyclicPortOpen(T70_WAVECYCLIC_PORT_OWNER *O,PDEVICE_OBJECT,PIRP,PUNKNOWN,PUNKNOWN,PRESOURCELIST,
 const T70_WAVECYCLIC_MINIPORT_PROVIDER &,NTSTATUS (*)(void *),void *){
 O->Attempted=true;if(PortStatus&&!Partial)return PortStatus;BP.AddRef();O->Port=&BP;O->Initialized=true;if(Partial){O->Miniport=(T70WaveCyclicMiniport *)(uintptr_t)1;O->CleanupStatus=STATUS_DEVICE_NOT_READY;return PortStatus;}return 0;}
NTSTATUS T70WaveCyclicPortClose(T70_WAVECYCLIC_PORT_OWNER *O,NTSTATUS (*C)(void *),void *Context){
 if(O->Port){auto S=C(Context);if(S)return S;O->Port->Release();O->Port=nullptr;O->Miniport=nullptr;}return 0;}
NTSTATUS T70WaveCyclicPortWithdraw(T70_WAVECYCLIC_PORT_OWNER *O){O->PublicationClosed=true;++WithdrawCalls;return WithdrawStatus;}
struct Resources:IResourceList {
 ULONG Refs=1;CM_PARTIAL_RESOURCE_DESCRIPTOR M{},RM{},I{},RI{};
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 ULONG NumberOfEntriesOfType(CM_RESOURCE_TYPE T)override{return T==CmResourceTypeMemory||T==CmResourceTypeInterrupt?1:0;}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindTranslatedEntry(CM_RESOURCE_TYPE T,ULONG)override{return T==CmResourceTypeMemory?&M:&I;}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindUntranslatedEntry(CM_RESOURCE_TYPE T,ULONG)override{return T==CmResourceTypeMemory?&RM:&RI;}
};
static NTSTATUS Claim(void *){++Claims;return ClaimStatus;}
static NTSTATUS ReleaseDomain(void *){++Releases;return DomainReleaseStatus;}
static NTSTATUS Detach(void *){++Detaches;return DetachStatus;}
int main(){
 ExpectedGroup=0;Resources R;Group Parent;Parent.Refs=1;
 R.M.Type=R.RM.Type=CmResourceTypeMemory;R.M.Flags=R.RM.Flags=CM_RESOURCE_MEMORY_READ_WRITE;
 R.M.u.Memory.Start.QuadPart=R.RM.u.Memory.Start.QuadPart=0x11210000;R.M.u.Memory.Length=R.RM.u.Memory.Length=0x2000;
 R.I.Type=R.RI.Type=CmResourceTypeInterrupt;R.I.Flags=R.RI.Flags=CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
 R.I.ShareDisposition=R.RI.ShareDisposition=CmResourceShareDeviceExclusive;
 R.I.u.Interrupt.Level=7;R.I.u.Interrupt.Vector=0x456;R.I.u.Interrupt.Affinity=4;R.RI.u.Interrupt.Vector=266;
 T70_WAVECYCLIC_MINIPORT_PROVIDER Provider={};
 T70_WAVECYCLIC_BOARD_CONTRACT C={nullptr,Claim,ReleaseDomain,Drain,Detach};
 for(unsigned Bad=0;Bad<4;Bad++){
  auto SavedI=R.I;auto SavedM=R.M;
  if(Bad==0)R.I.Flags=1;if(Bad==1)R.I.ShareDisposition=3;
  if(Bad==2)R.M.u.Memory.Start.QuadPart++;if(Bad==3)R.M.u.Memory.Length--;
  T70_WAVECYCLIC_BOARD B={};auto N=Maps;
  assert(T70WaveCyclicBoardOpen(&B,Pdo,Pdo,(PIRP)(uintptr_t)1,&Parent,nullptr,&R,Provider,C)==STATUS_DEVICE_DATA_ERROR&&!B.Attempted&&Maps==N);
  R.I=SavedI;R.M=SavedM;
 }
 for(unsigned Case=0;Case<10;Case++){
  T70_WAVECYCLIC_BOARD B={};Maps=Unmaps=Claims=Releases=Detaches=0;
  ClaimStatus=DetachStatus=DomainReleaseStatus=PortStatus=DrainStatus=ConnectStatus=0;MapFail=QiFail=Fire=Partial=false;
  std::memset(Registers,0,sizeof Registers);Registers[0x3b4/4]=1;
  if(Case==0)R.RI.u.Interrupt.Vector=234;
  if(Case==1)ClaimStatus=STATUS_DEVICE_NOT_READY;
  if(Case==2)MapFail=true;if(Case==3)PortStatus=STATUS_NOT_SUPPORTED;if(Case==4)QiFail=true;
  if(Case==5)Registers[0x3a0/4]=1;if(Case==6)ConnectStatus=STATUS_NOT_SUPPORTED;
  if(Case==9){Partial=true;PortStatus=STATUS_NOT_SUPPORTED;DetachStatus=STATUS_DEVICE_NOT_READY;}
  auto S=T70WaveCyclicBoardOpen(&B,Pdo,Pdo,(PIRP)(uintptr_t)1,&Parent,nullptr,&R,Provider,C);
  if(Case<7||Case==9)assert(S!=0);else assert(S==0&&B.Started&&B.Lease&&B.DomainHeld&&B.Irq.Owner.Interrupt);
  if(Case==0){assert(!Maps&&!Claims&&!B.Attempted);R.RI.u.Interrupt.Vector=266;}
  if(Case==7){
   NTSTATUS Stop=STATUS_DEVICE_NOT_READY;B.StreamOwner=&Stop;
   B.BeforeIrqClose=[](void *X)->NTSTATUS{return *(NTSTATUS *)X;};
   auto Interrupt=B.Irq.Owner.Interrupt;
   WithdrawStatus=STATUS_DEVICE_NOT_READY;auto Count=WithdrawCalls;
   assert(T70WaveCyclicBoardClose(&B)!=0&&WithdrawCalls==Count+1&&!B.Started&&
     B.BeforeIrqClose&&B.Irq.Owner.Interrupt==Interrupt&&!Unmaps&&!Detaches);
   WithdrawStatus=0;
   assert(T70WaveCyclicBoardClose(&B)!=0&&B.Irq.Owner.Interrupt==Interrupt&&!Unmaps&&!Detaches);
   Stop=0;DrainStatus=STATUS_DEVICE_NOT_READY;
   assert(T70WaveCyclicBoardClose(&B)!=0&&!B.BeforeIrqClose&&!B.StreamOwner&&!Unmaps);
   DrainStatus=0;
  }
  if(Case==7){DrainStatus=STATUS_DEVICE_NOT_READY;assert(T70WaveCyclicBoardClose(&B)!=0&&!Unmaps&&B.DomainHeld&&!B.Started);DrainStatus=0;DetachStatus=STATUS_DEVICE_NOT_READY;assert(T70WaveCyclicBoardClose(&B)!=0&&!Unmaps&&B.Lease&&!B.Started);DetachStatus=0;}
  if(Case==9){assert(!Unmaps&&B.Parent&&B.Port.Miniport&&B.DomainHeld&&!Detaches);assert(T70WaveCyclicBoardClose(&B)!=0&&!B.Started&&!Unmaps);DetachStatus=0;}
  if(Case==8){DomainReleaseStatus=STATUS_DEVICE_NOT_READY;assert(T70WaveCyclicBoardClose(&B)!=0&&Unmaps==1&&B.DomainHeld&&B.Parent);DomainReleaseStatus=0;}
  assert(T70WaveCyclicBoardClose(&B)==0&&!B.Parent&&!B.Resources&&!B.Mapping.Mapping.VirtualBase&&!B.DomainHeld);
  assert(Parent.Refs==1&&R.Refs==1&&BP.Refs==1&&Maps==Unmaps+(MapFail?1u:0u));
  auto N=Unmaps;assert(T70WaveCyclicBoardClose(&B)==0&&Unmaps==N);
 }
 // Software claim success cannot stand in for physical MMIO admission.
 {
  T70_WAVECYCLIC_BOARD B={};auto K=C;Maps=Unmaps=Claims=Releases=0;
  ClaimStatus=DomainReleaseStatus=0;
  K.AccessReady=[](void *)->NTSTATUS{return STATUS_DEVICE_NOT_READY;};
  assert(T70WaveCyclicBoardOpen(&B,Pdo,Pdo,(PIRP)(uintptr_t)1,&Parent,nullptr,&R,Provider,K)==STATUS_DEVICE_NOT_READY);
  assert(Claims==1&&Releases==1&&!Maps&&!Unmaps&&!B.DomainHeld&&Parent.Refs==1&&R.Refs==1);
 }
 for(auto W:Waits)delete W;
 puts("WaveCyclic board: real resource validation, mapped IRQ/recovery initialization and staged failure cleanup passed");
}
