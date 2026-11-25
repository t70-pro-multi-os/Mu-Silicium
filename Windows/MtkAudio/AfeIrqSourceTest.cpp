// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <initializer_list>
#include "AfeIrqSource.h"
#include <cassert>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>
static thread_local unsigned Irql;
LONG InterlockedCompareExchange(volatile LONG *P,LONG N,LONG V){__atomic_compare_exchange_n(P,&V,N,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return V;}
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
static std::mutex Lock;
static unsigned Syncs;
static std::atomic<bool> Trying{false};
BOOLEAN KeSynchronizeExecution(PKINTERRUPT I,BOOLEAN (*F)(void *),void *C){
 assert(I);++Syncs;Trying=true;std::lock_guard<std::mutex> L(Lock);Irql=7;auto R=F(C);Irql=0;return R;
}
struct Io{ULONG On=1,Enable=1,Status=1;unsigned Calls=0,Fail=0,Scheduled=0;bool Stuck=false;};
static NTSTATUS Read(void *C,ULONG A,ULONG *V){auto &I=*(Io*)C;assert(Irql==7);
 if(++I.Calls==I.Fail)return STATUS_DEVICE_DATA_ERROR;
 if(A==0x3a0)*V=I.On;else if(A==0x3b4)*V=I.Enable;else {assert(A==0x3a4);*V=I.Status;}return 0;}
static NTSTATUS Write(void *C,ULONG A,ULONG V){auto &I=*(Io*)C;assert(Irql==7);
 if(++I.Calls==I.Fail)return STATUS_DEVICE_DATA_ERROR;
 if(A==0x3a0){assert(V==(I.On&~1u));if(!I.Stuck)I.On=V;}
 else {assert(A==0x3a8&&V==1);I.Status&=~1u;}return 0;}
static void Schedule(void *C,NTSTATUS E){assert(Irql==7&&E==STATUS_DEVICE_DATA_ERROR);++((Io*)C)->Scheduled;}
int main(){
 for(unsigned F=0;F<=6;F++){
  Io I;I.Fail=F;T70_AFE_INTERRUPT_OWNER O={};O.Interrupt=(PKINTERRUPT)(uintptr_t)1;
  O.Handler.IoContext=&I;O.Handler.Read32=Read;O.Handler.Write32=Write;O.Handler.DomainExclusive=true;
  T70_AFE_IRQ_SOURCE S={&O,Schedule,&I,0,0};
  auto R=T70AfeIrqSourceMask(&S);assert(R==(F?STATUS_DEVICE_DATA_ERROR:0));
  assert(I.Calls==(F?F:6)&&S.Completed==(F?(F<=2?0:F<=4?1:2):3));
  assert(O.Interrupt&&!I.Scheduled); // failures never disconnect or release
 }
 Io I;T70_AFE_INTERRUPT_OWNER O={};O.Interrupt=(PKINTERRUPT)(uintptr_t)1;
 O.Handler.IoContext=&I;O.Handler.Read32=Read;O.Handler.Write32=Write;O.Handler.DomainExclusive=true;
 T70_AFE_IRQ_SOURCE S={&O,Schedule,&I,0,0};
 for(ULONG Other: {2u,0x80000000u}){I={};I.On|=Other;
  assert(T70AfeIrqSourceMask(&S)==STATUS_DEVICE_NOT_READY&&I.Calls==3&&I.On==Other&&I.Status==1);}
 I={};I.Stuck=true;assert(T70AfeIrqSourceMask(&S)!=0&&I.Calls==3);
 I={};I.Enable|=2;I.Status|=2;assert(T70AfeIrqSourceMask(&S)!=0&&S.Completed==2&&I.Status==2);
 I={};I.On=0x78000001;assert(T70AfeIrqSourceMask(&S)==0&&I.On==0x78000000);
 I={};O.Interrupt=nullptr;unsigned N=Syncs;Irql=7;
 T70AfeIrqSourceFault(&S,STATUS_DEVICE_DATA_ERROR);Irql=0;
 assert(S.MaskStatus==0&&I.Scheduled==1&&Syncs==N); // pre-Connect-return ISR
 I={};I.Fail=2;Irql=7;T70AfeIrqSourceFault(&S,STATUS_DEVICE_DATA_ERROR);Irql=0;
 assert(S.MaskStatus==STATUS_DEVICE_DATA_ERROR&&I.Scheduled==1&&I.On==1);
 I={};assert(T70AfeIrqSourceMask(&S)!=0&&!I.Calls);
 O.Interrupt=(PKINTERRUPT)(uintptr_t)1;
 // Simulated OS ISR critical section prevents close-side register access.
 std::atomic<bool> Entered{false},Leave{false};
 std::thread ISR([&]{std::lock_guard<std::mutex> L(Lock);Entered=true;while(!Leave)std::this_thread::yield();});
 while(!Entered)std::this_thread::yield();
 Trying=false;std::thread Close([&]{assert(T70AfeIrqSourceMask(&S)==0);});
 while(!Trying)std::this_thread::yield();
 assert(I.Calls==0);Leave=true;ISR.join();Close.join();assert(I.Calls==6);
 puts("AFE IRQ0 source: stock disable/clear, failures, preserved other bits, synchronized close and pre-return fault passed");
}
