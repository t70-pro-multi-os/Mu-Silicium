// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Library/MtkGaugeCore.h>
typedef struct { unsigned Lease, Calls, FailAt, Writes, NoSet, NoClear, ReleaseFail, AcquireFail; uint16_t Ctrl, Current; } HW;
static int Acquire(void *P) { HW *H=P; if(H->AcquireFail)return 0;assert(!H->Lease);H->Lease=1;return 1; }
static int Release(void *P) { HW *H=P;assert(H->Lease);if(H->ReleaseFail)return 0;H->Lease=0;return 1; }
static int Read(void *P,uint16_t A,uint16_t *V) { HW *H=P;assert(H->Lease);if(++H->Calls==H->FailAt)return 0;assert(A==0xd0a||A==0xd8a);*V=A==0xd0a?H->Ctrl:H->Current;return 1; }
static int Write(void *P,uint16_t A,uint16_t V) {
 HW *H=P;assert(H->Lease&&A==0xd0a);H->Writes++;H->Ctrl=V;
 if(V&1){if(!H->NoSet)H->Ctrl|=0x8000;}else if(V&8){if(!H->NoClear)H->Ctrl&=~0x8000;}
 // Failure may still have changed hardware.
 return ++H->Calls!=H->FailAt;
}
static MTK_GAUGE_IO Io(HW *H){MTK_GAUGE_IO I={H,Acquire,Release,Read,Write};return I;}
int main(void){unsigned Checks=0;int32_t Out;uint16_t Raw;MTK_GAUGE_RESULT R;
 for(unsigned V=0;V<65536;V++){
  int64_t Mag=(V<32768?V:(V^65535));int64_t Ref=Mag*381470/100000;
  Ref=Ref*100/50;if(V>=32768)Ref=-Ref;Ref=Ref*1010/1000;
  assert(MtkGaugeConvertCurrent((uint16_t)V,50,1010,&Out)==MtkGaugeOk&&Out==Ref);Checks++;
 }
 Out=123;assert(MtkGaugeConvertCurrent(1,0,1010,&Out)==MtkGaugeInvalid&&Out==123);
 assert(MtkGaugeConvertCurrent(32767,1,UINT32_MAX,&Out)==MtkGaugeInvalid&&Out==123);Checks+=2;
 HW H={0};H.Current=0x8123;MTK_GAUGE_CORE C={0};MTK_GAUGE_IO I=Io(&H);Raw=0x1234;
 assert(MtkGaugeReadCurrent(&C,&I,&Raw)==MtkGaugeOk&&Raw==H.Current&&!H.Lease&&!C.Owned&&!H.Ctrl);unsigned Total=H.Calls;Checks++;
 // The composite Windows telemetry path supplies an already-held lease.
 memset(&H,0,sizeof(H));memset(&C,0,sizeof(C));H.Lease=1;H.Current=0x4567;I=Io(&H);Raw=0x1234;
 assert(MtkGaugeReadCurrentInLease(&C,&I,&Raw)==MtkGaugeOk&&Raw==H.Current&&H.Lease&&
        !C.Owned&&!C.Quarantined&&!H.Ctrl);Checks++;
 assert(Release(&H)&&!H.Lease);Checks++;
 memset(&H,0,sizeof(H));memset(&C,0,sizeof(C));H.Lease=1;H.Current=0x8123;H.NoClear=1;I=Io(&H);Raw=0x1234;
 R=MtkGaugeReadCurrentInLease(&C,&I,&Raw);
 assert(R==MtkGaugeQuarantined&&Raw==0x1234&&H.Lease&&C.Owned&&C.Quarantined);
 unsigned InLeaseCalls=H.Calls;
 assert(MtkGaugeReadCurrentInLease(&C,&I,&Raw)==MtkGaugeQuarantined&&H.Calls==InLeaseCalls&&H.Lease);
 Checks+=2;
 assert(Release(&H)&&!H.Lease);Checks++;
 memset(&H,0,sizeof(H));memset(&C,0,sizeof(C));H.Lease=1;H.Ctrl=0x10;I=Io(&H);Raw=0x1234;
 assert(MtkGaugeReadCurrentInLease(&C,&I,&Raw)==MtkGaugeBusy&&Raw==0x1234&&H.Lease&&!C.Owned&&!H.Writes);
 assert(Release(&H)&&!H.Lease);Checks+=2;
 for(unsigned F=1;F<=Total;F++){
  memset(&H,0,sizeof(H));memset(&C,0,sizeof(C));I=Io(&H);H.FailAt=F;Raw=0x1234;
  R=MtkGaugeReadCurrent(&C,&I,&Raw);assert(R!=MtkGaugeOk&&Raw==0x1234);
  if(R==MtkGaugeQuarantined){assert(H.Lease&&C.Owned&&C.Quarantined);unsigned Calls=H.Calls;assert(MtkGaugeReadCurrent(&C,&I,&Raw)==R&&H.Calls==Calls);}
  else assert(!H.Lease&&!C.Owned&&!H.Ctrl);
  Checks++;
 }
 for(unsigned Mode=0;Mode<5;Mode++){
  memset(&H,0,sizeof(H));memset(&C,0,sizeof(C));I=Io(&H);Raw=0x1234;
  if(Mode==0)H.NoSet=1;
  if(Mode==1)H.NoClear=1;
  if(Mode==2)H.ReleaseFail=1;
  if(Mode==3)H.Ctrl=0x10;
  if(Mode==4)H.AcquireFail=1;
  R=MtkGaugeReadCurrent(&C,&I,&Raw);assert(R!=MtkGaugeOk&&Raw==0x1234&&H.Calls<1020);
  if(Mode==0)assert(R==MtkGaugeTimeout&&!H.Lease&&!H.Ctrl);
  if(Mode==1||Mode==2)assert(R==MtkGaugeQuarantined&&H.Lease&&C.Owned);
  if(Mode>=3)assert(R==MtkGaugeBusy&&!H.Lease&&!H.Writes);
  Checks++;
 }
 printf("MT6358 gauge transaction/conversion: %u checks PASS (mock hardware)\n",Checks);return 0;
}
