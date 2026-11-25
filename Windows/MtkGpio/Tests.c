/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "Hardware.h"
#include "PinMap.h"
static unsigned Checks;
#define CHECK(X) do { Checks++; assert(X); } while (0)
#include "TestMmio.h"
static MG_CORE Core;
static MG_IO Io={0,FakeRead,FakeWrite};
static uint32_t Original[MG_MMIO_LENGTH/4];
static void SetMode(unsigned Pin,unsigned Value);
static void Start(void) {
  unsigned P;
  ResetRegisters();
  for (P=0;P<MG_PINS;P++) SetMode(P,0);
  memcpy(Original,Registers[0],sizeof(Original));
  CHECK(MgInitialize(&Core,&Io)==MgOk); CHECK(MgStart(&Core)==MgOk); CHECK(Writes==0);
}
static void Pins(void) {
  unsigned P,B; uint32_t Bit,V,Before,Expected;
  for (P=0;P<MG_PINS;P++) {
    Start(); B=P/32; Bit=1U<<(P%32);
    CHECK(MgConnect(&Core,P,1)==MgOk);
    CHECK(!(Registers[0][B*4]&Bit));
    CHECK(((Registers[0][0x300/4+(P/8)*4]>>((P%8)*4))&7)==0);
    CHECK(MgRead(&Core,B,0,&V)==MgOk && V==(Registers[0][0x200/4+B*4]&Bit));
    Before=Writes; CHECK(MgWrite(&Core,B,Bit,0)==MgInvalid && Writes==Before);
    CHECK(MgConnect(&Core,P,2)==MgBusy);
    CHECK(MgDisconnect(&Core,P,0)==MgOk && !memcmp(Original,Registers[0],sizeof(Original)));
    CHECK(MgConnect(&Core,P,2)==MgOk && (Registers[0][B*4]&Bit));
    Before=Registers[0][0x100/4+B*4];
    CHECK(MgWrite(&Core,B,Bit,0)==MgOk && Registers[0][0x100/4+B*4]==(Before|Bit));
    CHECK(MgRead(&Core,B,1,&V)==MgOk && V==Bit);
    CHECK(MgRead(&Core,B,0,&V)==MgOk && V==(Registers[0][0x200/4+B*4]&Bit));
    CHECK(MgWrite(&Core,B,0,Bit)==MgOk && Registers[0][0x100/4+B*4]==(Before&~Bit));
    CHECK(MgRead(&Core,B,1,&V)==MgOk && V==0);
    CHECK(MgDisconnect(&Core,P,0)==MgOk && !memcmp(Original,Registers[0],sizeof(Original)));
    CHECK(MgStop(&Core)==MgOk && !Core.Active);
  }
  Start(); Before=Writes;
  CHECK(MgConnect(&Core,188,1)==MgInvalid && MgConnect(&Core,999,2)==MgInvalid);
  CHECK(MgEnable(&Core,126,1,1)==MgUnsupported && MgEnable(&Core,187,1,1)==MgUnsupported);
  CHECK(MgRead(&Core,6,0,&Expected)==MgInvalid && MgWrite(&Core,5,0x10000000U,0)==MgInvalid);
  for (B=0;B<MG_BANKS;B++) CHECK(MgMask(&Core,B,0)==MgOk && MgAck(&Core,B,0)==MgOk && Writes==Before);
  CHECK(Writes==Before && MG_TOTAL_PINS==189U && MgValidMask(5)==0x1FFFFFFFU && MgValidMask(6)==0);
}
static void Interrupts(void) {
  unsigned P,L,H,B; uint32_t Bit,A,E,Neighbor,Sensitivity,Polarity;
  for (P=0;P<MG_IRQ_PINS;P++) for (L=0;L<2;L++) for (H=0;H<2;H++) {
    Start(); B=P/32; Bit=1U<<(P%32); Neighbor=Bit==1?2:1;
    Registers[1][B]=Bit|Neighbor;
    Sensitivity=Registers[1][0x140/4+B]; Polarity=Registers[1][0x300/4+B];
    CHECK(MgEnable(&Core,P,L,H)==MgOk && Core.Pin[P].Kind==3);
    CHECK(!(Registers[1][0x80/4+B]&Bit));
    CHECK(!!(Registers[1][0x140/4+B]&Bit)==L);
    CHECK(!!(Registers[1][0x300/4+B]&Bit)==H);
    CHECK(!(Registers[1][B]&Bit) && (Registers[1][B]&Neighbor));
    Registers[1][B]|=Bit;
    CHECK(MgQuery(&Core,B,UINT32_MAX,&A,&E)==MgOk && A==Bit && E==Bit);
    CHECK(MgMask(&Core,B,Bit)==MgOk);
    CHECK(MgQuery(&Core,B,UINT32_MAX,&A,&E)==MgOk && A==0 && E==0);
    CHECK(MgUnmask(&Core,P)==MgOk && (Registers[1][B]&Bit));
    CHECK(MgConfigureInterrupt(&Core,P,!L,!H)==MgOk);
    CHECK(!!(Registers[1][0x140/4+B]&Bit)==!L && !!(Registers[1][0x300/4+B]&Bit)==!H);
    Registers[1][B]|=Bit; CHECK(MgAck(&Core,B,Bit)==MgOk && !(Registers[1][B]&Bit));
    CHECK(MgDisconnect(&Core,P,0)==MgOk && !Core.Owned[B]);
    CHECK(Registers[1][0x80/4+B]&Bit);
    CHECK(Registers[1][0x140/4+B]==Sensitivity && Registers[1][0x300/4+B]==Polarity);
    CHECK(!memcmp(Original,Registers[0],sizeof(Original)));
  }
  Start(); Registers[1][4]=UINT32_MAX;
  CHECK(MgQuery(&Core,4,UINT32_MAX,&A,&E)==MgOk && A==0 && E==0);
  CHECK(MgEnable(&Core,9,1,0)==MgOk);
  CHECK(MgStop(&Core)==MgOk && Registers[1][4]==UINT32_MAX);
}
static void PmicInterrupts(void) {
  const unsigned H=MG_PMIC_EINT_BANK; const uint32_t Bit=MG_PMIC_EINT_BIT;
  const uint32_t Logical=1U<<MG_PMIC_IRQ_NUMBER,Neighbor=1U<<8;
  uint32_t A,E,V,Before,BaseSensitivity,BasePolarity,BaseDomain,GpioBankData;
  Start();
  BaseSensitivity=Registers[1][0x140/4+H]&~Bit;
  BasePolarity=Registers[1][0x300/4+H]&~Bit;
  BaseDomain=Registers[1][0x400/4+H];
  Registers[1][0x140/4+H]=BaseSensitivity;
  Registers[1][0x300/4+H]=BasePolarity;
  Registers[1][H]=Bit|Neighbor;
  GpioOperations=0; Before=Operations;
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,0,1,MgPullDefault)==MgUnsupported && Operations==Before);
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,0,MgPullDefault)==MgUnsupported && Operations==Before);
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullUp)==MgUnsupported && Operations==Before);
  CHECK(MgConnect(&Core,MG_PMIC_IRQ_PIN,1)==MgInvalid && Operations==Before);
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  CHECK(Core.PmicInterrupt.Saved && Core.PmicInterrupt.Masked && GpioOperations==0);
  CHECK(!(Registers[1][0x80/4+H]&Bit));
  CHECK((Registers[1][0x80/4+H]&~Bit)==(UINT32_MAX&~Bit));
  CHECK((Registers[1][0x140/4+H]&Bit) && (Registers[1][0x300/4+H]&Bit));
  CHECK(Registers[1][H]==Neighbor && Registers[1][0x400/4+H]==BaseDomain);
  CHECK(Registers[0][0x300/4+(137/8)*4]==Original[0x300/4+(137/8)*4]);
  CHECK(!Core.Pin[137].Saved && !Core.Pin[137].Kind && !Core.Owned[4]);
  Before=Writes;
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgBusy && Writes==Before);
  CHECK(MgConfigureInterrupt(&Core,MG_PMIC_IRQ_PIN,1,1)==MgOk && Writes==Before);
  CHECK(MgConfigureInterrupt(&Core,MG_PMIC_IRQ_PIN,0,1)==MgUnsupported && Writes==Before);
  CHECK(MgConfigureInterrupt(&Core,MG_PMIC_IRQ_PIN,1,0)==MgUnsupported && Writes==Before);
  CHECK(MgMask(&Core,5,Logical)==MgOk && (Registers[1][0x80/4+H]&Bit));
  CHECK(MgQuery(&Core,5,Logical,&A,&E)==MgOk && A==0 && E==0);
  CHECK(MgUnmask(&Core,MG_PMIC_IRQ_PIN)==MgOk && !(Registers[1][0x80/4+H]&Bit));
  Registers[1][H]|=Bit;
  CHECK(MgQuery(&Core,5,Logical,&A,&E)==MgOk && A==Logical && E==Logical);
  CHECK(MgQuery(&Core,5,0,&A,&E)==MgOk && A==0 && E==Logical);
  CHECK(MgMask(&Core,5,Logical)==MgOk && MgQuery(&Core,5,Logical,&A,&E)==MgOk && A==0 && E==0);
  Registers[1][H]|=Bit|Neighbor;
  CHECK(MgAck(&Core,5,Logical)==MgOk && Registers[1][H]==Neighbor);
  CHECK(MgMask(&Core,5,1U<<27)==MgInvalid && MgAck(&Core,5,1U<<27)==MgInvalid);
  CHECK(MgMask(&Core,4,Bit)==MgInvalid && MgAck(&Core,4,Bit)==MgInvalid);
  CHECK(MgRead(&Core,5,0,&V)==MgOk && !(V&Logical));
  GpioBankData=Registers[0][0x200/4+5];
  Registers[0][0x200/4+5]|=Logical;
  CHECK(MgRead(&Core,5,0,&V)==MgOk && !(V&Logical));
  Registers[0][0x200/4+5]=GpioBankData;
  Before=Writes; CHECK(MgWrite(&Core,5,Logical,0)==MgInvalid && Writes==Before);
  GpioOperations=0;
  CHECK(MgDisableInterrupt(&Core,MG_PMIC_IRQ_PIN)==MgOk && !Core.PmicInterrupt.Saved);
  CHECK((Registers[1][0x80/4+H]&Bit) && Registers[1][0x140/4+H]==BaseSensitivity);
  CHECK(Registers[1][0x300/4+H]==BasePolarity && Registers[1][0x400/4+H]==BaseDomain);
  CHECK(Registers[1][H]==Neighbor && GpioOperations==0);
  CHECK(!memcmp(Original,Registers[0],sizeof(Original)));
  CHECK(MgStop(&Core)==MgOk);

  Start();
  BaseSensitivity=Registers[1][0x140/4+H]&~Bit;
  BasePolarity=Registers[1][0x300/4+H]&~Bit;
  Registers[1][0x140/4+H]=BaseSensitivity;
  Registers[1][0x300/4+H]=BasePolarity;
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  CHECK(MgUnmask(&Core,MG_PMIC_IRQ_PIN)==MgOk);
  Registers[1][H]|=Bit;
  CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved && !Core.Active);
  CHECK((Registers[1][0x80/4+H]&Bit) && Registers[1][0x140/4+H]==BaseSensitivity);
  CHECK(Registers[1][0x300/4+H]==BasePolarity && !Core.Pin[137].Saved);
}
static void PmicFailures(void) {
  const unsigned H=MG_PMIC_EINT_BANK; const uint32_t Bit=MG_PMIC_EINT_BIT;
  unsigned N,Count; uint32_t A=0x1234,E=0x5678,Before,Domain;
  ResetRegisters(); Registers[1][0x410/4]&=~Bit;
  CHECK(MgInitialize(&Core,&Io)==MgOk);
  CHECK(MgStart(&Core)==MgNotReady && !Core.Active && Writes==0);
  ResetRegisters(); Registers[1][0x80/4+H]&=~Bit;
  CHECK(MgInitialize(&Core,&Io)==MgOk);
  CHECK(MgStart(&Core)==MgNotReady && !Core.Active && Writes==0);

  Start(); Domain=Registers[1][0x400/4+H];
  Registers[1][0x140/4+H]&=~Bit; Registers[1][0x300/4+H]&=~Bit;
  Operations=0; CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk); Count=Operations;
  for (N=1;N<=Count;N++) {
    Start(); Registers[1][0x140/4+H]&=~Bit; Registers[1][0x300/4+H]&=~Bit;
    Operations=0; FailOperation=N;
    CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgIoError);
    FailOperation=0;
    CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved);
    CHECK((Registers[1][0x80/4+H]&Bit) && !(Registers[1][0x140/4+H]&Bit));
    CHECK(!(Registers[1][0x300/4+H]&Bit) && Registers[1][0x400/4+H]==Domain);
  }

  Start(); Registers[1][0x140/4+H]&=~Bit; Registers[1][0x300/4+H]&=~Bit;
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  Operations=0; CHECK(MgStop(&Core)==MgOk); Count=Operations;
  for (N=1;N<=Count;N++) {
    Start(); Registers[1][0x140/4+H]&=~Bit; Registers[1][0x300/4+H]&=~Bit;
    CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
    Operations=0; FailOperation=N;
    CHECK(MgStop(&Core)==MgIoError && Core.PmicInterrupt.Saved && !Core.Active);
    FailOperation=0; CHECK(MgStart(&Core)==MgBusy);
    CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved);
    CHECK((Registers[1][0x80/4+H]&Bit) && !(Registers[1][0x140/4+H]&Bit));
    CHECK(!(Registers[1][0x300/4+H]&Bit) && Registers[1][0x400/4+H]==Domain);
  }

  Start(); Registers[1][0x140/4+H]&=~Bit; Registers[1][0x300/4+H]&=~Bit;
  DropEintWrite=1;
  CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgIoError &&
        !Core.Active && Core.PmicInterrupt.Saved);
  CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved &&
        !(Registers[1][0x140/4+H]&Bit) && !(Registers[1][0x300/4+H]&Bit));

  Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  Registers[1][0x400/4+H]&=~Bit; Before=Operations;
  CHECK(MgQuery(&Core,5,1U<<MG_PMIC_IRQ_NUMBER,&A,&E)==MgNotReady &&
        A==0x1234 && E==0x5678 && Operations>Before && !Core.Active);
  Before=Operations; CHECK(MgStop(&Core)==MgNotReady && Core.PmicInterrupt.Saved && Operations>Before);
  CHECK(MgStart(&Core)==MgBusy);
  Registers[1][0x400/4+H]|=Bit;
  CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved);

  Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  Registers[1][0x300/4+H]&=~Bit;
  CHECK(MgSuspend(&Core)==MgNotReady && !Core.Active && !Core.Suspended &&
        Core.PmicInterrupt.Saved && (Registers[1][0x80/4+H]&Bit));
  CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved);

  Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  Registers[1][0x400/4+H]&=~Bit;
  CHECK(MgMask(&Core,5,1U<<MG_PMIC_IRQ_NUMBER)==MgNotReady && !Core.Active);
  CHECK(MgStop(&Core)==MgNotReady && Core.PmicInterrupt.Saved);
  Registers[1][0x400/4+H]|=Bit;
  CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved);

  Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  CHECK(MgSuspend(&Core)==MgOk && Core.Suspended && Core.PmicInterrupt.ContextValid);
  CHECK(Registers[1][0x80/4+H]&Bit);
  CHECK(MgResume(&Core)==MgOk && Core.Active && !Core.Suspended &&
        !(Registers[1][0x80/4+H]&Bit) && !Core.PmicInterrupt.ContextValid);
  CHECK(MgStop(&Core)==MgOk);
  Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  Operations=0; CHECK(MgSuspend(&Core)==MgOk); Count=Operations;
  CHECK(MgResume(&Core)==MgOk && MgStop(&Core)==MgOk);
  for (N=1;N<=Count;N++) {
    Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
    Operations=0; FailOperation=N;
    CHECK(MgSuspend(&Core)==MgIoError && !Core.Suspended && !Core.PmicInterrupt.ContextValid);
    FailOperation=0; CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved);
  }
  Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
  CHECK(MgSuspend(&Core)==MgOk); Operations=0; CHECK(MgResume(&Core)==MgOk); Count=Operations;
  CHECK(MgStop(&Core)==MgOk);
  for (N=1;N<=Count;N++) {
    Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
    CHECK(MgSuspend(&Core)==MgOk); Operations=0; FailOperation=N;
    CHECK(MgResume(&Core)==MgIoError && !Core.Active && Core.Suspended &&
          Core.PmicInterrupt.ContextValid);
    FailOperation=0; CHECK(MgResume(&Core)==MgOk && Core.Active && !Core.Suspended);
    CHECK(MgStop(&Core)==MgOk && !Core.PmicInterrupt.Saved);
  }
  {
    uint32_t Wake[MG_BANKS]={0};
    Start(); CHECK(MgEnableEx(&Core,MG_PMIC_IRQ_PIN,1,1,MgPullDefault)==MgOk);
    Wake[MG_PMIC_IRQ_BANK]=1U<<MG_PMIC_IRQ_NUMBER; Before=Operations;
    CHECK(MgSuspendWake(&Core,Wake)==MgInvalid && Core.Active && !Core.Suspended && Operations==Before);
    CHECK(MgStop(&Core)==MgOk);
  }
}
static void Failures(void) {
  unsigned N,Count,P; uint32_t V=0x1234,A=0x1234,E=0x5678;
  for (N=0;N<5;N++) {
    ResetRegisters(); CHECK(MgInitialize(&Core,&Io)==MgOk);
    Registers[1][0x80/4+N]&=~1U;
    CHECK(MgStart(&Core)==MgNotReady && !Core.Active && Writes==0);
  }
  ResetRegisters(); CHECK(MgInitialize(&Core,&Io)==MgOk);
  Registers[1][0x400/4]=0; CHECK(MgStart(&Core)==MgNotReady && Writes==0);
  Start(); Operations=0; CHECK(MgEnable(&Core,9,1,0)==MgOk); Count=Operations;
  for (N=1;N<=Count;N++) {
    Start(); Operations=0; FailOperation=N;
    CHECK(MgEnable(&Core,9,1,0)==MgIoError);
    FailOperation=0;
    CHECK(MgStop(&Core)==MgOk);
    CHECK(!memcmp(Original,Registers[0],sizeof(Original)));
    CHECK(Registers[1][0x80/4]==UINT32_MAX);
  }
  Start(); CHECK(MgConnect(&Core,152,2)==MgOk); CHECK(MgEnable(&Core,9,1,0)==MgOk);
  Operations=0; CHECK(MgStop(&Core)==MgOk); Count=Operations;
  for (N=1;N<=Count;N++) {
    Start(); CHECK(MgConnect(&Core,152,2)==MgOk); CHECK(MgEnable(&Core,9,1,0)==MgOk);
    Operations=0; FailOperation=N; CHECK(MgStop(&Core)==MgIoError); FailOperation=0;
    CHECK(MgStop(&Core)==MgOk && !memcmp(Original,Registers[0],sizeof(Original)));
  }
  Start(); FailOperation=Operations+1;
  CHECK(MgRead(&Core,0,0,&V)==MgIoError && V==0x1234);
  CHECK(MgEnable(&Core,9,1,0)==MgNotReady); FailOperation=0; CHECK(MgStop(&Core)==MgOk);
  Start(); CHECK(MgEnable(&Core,9,1,0)==MgOk); FailOperation=Operations+2;
  CHECK(MgQuery(&Core,0,UINT32_MAX,&A,&E)==MgIoError && A==0x1234 && E==0x5678);
  FailOperation=0; CHECK(MgStop(&Core)==MgOk);
  Start(); CHECK(MgConnect(&Core,152,2)==MgOk); CHECK(MgDisconnect(&Core,152,1)==MgOk);
  CHECK(Core.Pin[152].Kind==0 && Core.Pin[152].Saved==0);
  for (P=0;P<MG_PINS;P++) CHECK(MgDisconnect(&Core,P,0)==MgInvalid);
  CHECK(MgInitialize(0,&Io)==MgInvalid && MgInitialize(&Core,0)==MgInvalid);
}
static uint32_t PadOriginal[MG_SPACES][MG_MMIO_LENGTH/4];
static unsigned RawField(const MG_FIELD *F) {
  uint32_t Mask=(1U<<F->Width)-1U;
  return (Registers[F->Space][F->Offset/4]>>F->Bit)&Mask;
}
static unsigned Field(const MG_FIELD *F) { return RawField(F)&1U; }
static void SetRawField(const MG_FIELD *F,unsigned V) {
  uint32_t Mask=((1U<<F->Width)-1U)<<F->Bit;
  uint32_t *R=&Registers[F->Space][F->Offset/4];
  *R=(*R&~Mask)|((V<<F->Bit)&Mask);
}
static int SameDescriptor(const MG_FIELD *A,const MG_FIELD *B);
static void SetField(const MG_FIELD *F,unsigned V) { SetRawField(F,V); }
static void SetMode(unsigned Pin,unsigned Value) {
  unsigned Offset=0x300U+(Pin/8U)*16U,Shift=(Pin%8U)*4U;
  uint32_t *R=&Registers[0][Offset/4U];
  *R=(*R&~(7U<<Shift))|((Value&7U)<<Shift);
}
static void SafeSchmittGroup(unsigned Pin) {
  unsigned Q;
  for (Q=0;Q<MG_PINS;Q++)
    if (SameDescriptor(&MgPads[Pin].Schmitt,&MgPads[Q].Schmitt)) SetMode(Q,0);
}
static int SameDescriptor(const MG_FIELD *A,const MG_FIELD *B) {
  return A->Offset==B->Offset && A->Space==B->Space && A->Bit==B->Bit && A->Width==B->Width;
}
static void SharedPair(unsigned *A,unsigned *B) {
  unsigned P,Q;
  for (P=0;P<MG_PINS;P++) for (Q=P+1;Q<MG_PINS;Q++)
    if (SameDescriptor(&MgPads[P].Schmitt,&MgPads[Q].Schmitt)) { *A=P; *B=Q; return; }
  CHECK(0); *A=*B=0;
}
static void Pads(void) {
  unsigned P,Pull,N,Count,Before,S,I;
  for (P=0;P<MG_PINS;P++) for (Pull=0;Pull<4;Pull++) for (I=0;I<4;I++) {
    const MG_PAD *Pad=&MgPads[P];
    Start(); SetField(&Pad->First,I&1); SetField(&Pad->Second,(I>>1)&1);
    if (Pad->Type==4) SetField(&Pad->Third,I&1);
    memcpy(PadOriginal,Registers,sizeof(Registers));
    CHECK(MgConnectEx(&Core,P,1,(MG_PULL)Pull)==MgOk && Field(&Pad->Input)==1);
    if (Pull!=MgPullDefault) {
      if (Pad->Type==4) {
        CHECK(Field(&Pad->First)==(Pull!=MgPullUp));
        CHECK((Field(&Pad->Second)|Field(&Pad->Third))==(Pull!=MgPullNone));
        if (Pull!=MgPullNone && I) CHECK(Field(&Pad->Second)==((I>>1)&1) && Field(&Pad->Third)==(I&1));
      } else CHECK(Field(&Pad->First)==(Pull==MgPullUp) && Field(&Pad->Second)==(Pull==MgPullDown));
    } else {
      CHECK(Field(&Pad->First)==(I&1) && Field(&Pad->Second)==((I>>1)&1));
      if (Pad->Type==4) CHECK(Field(&Pad->Third)==(I&1));
    }
    CHECK(MgDisconnect(&Core,P,0)==MgOk);
    CHECK(!memcmp(Registers,PadOriginal,sizeof(Registers)));
    CHECK(MgStop(&Core)==MgOk);
  }
  /* Inject a failure at each save/configure/restore operation for both pad
   * types. Stop must restore the exact snapshot, without claiming recovery. */
  for (P=9;P<=54;P+=45) {
    Start(); Operations=0; CHECK(MgConnectEx(&Core,P,1,MgPullUp)==MgOk); Count=Operations;
    for (N=1;N<=Count;N++) {
      Start(); memcpy(PadOriginal,Registers,sizeof(Registers)); Operations=0; FailOperation=N;
      CHECK(MgConnectEx(&Core,P,1,MgPullUp)==MgIoError); FailOperation=0;
      CHECK(MgStop(&Core)==MgOk && !memcmp(Registers,PadOriginal,sizeof(Registers)));
    }
    Start(); CHECK(MgConnectEx(&Core,P,1,MgPullDown)==MgOk); Operations=0;
    CHECK(MgDisconnect(&Core,P,0)==MgOk); Count=Operations;
    for (N=1;N<=Count;N++) {
      Start(); memcpy(PadOriginal,Registers,sizeof(Registers)); CHECK(MgConnectEx(&Core,P,1,MgPullDown)==MgOk);
      Operations=0; FailOperation=N; CHECK(MgDisconnect(&Core,P,0)==MgIoError); FailOperation=0;
      CHECK(MgStop(&Core)==MgOk && !memcmp(Registers,PadOriginal,sizeof(Registers)));
    }
  }
  /* A posted MMIO write that silently fails must not be accepted as ready. */
  Start(); SetField(&MgPads[9].Input,0); memcpy(PadOriginal,Registers,sizeof(Registers));
  DropPadWrite=1;
  CHECK(MgConnectEx(&Core,9,1,MgPullUp)==MgIoError && !Core.Active && Core.Pin[9].Saved);
  CHECK(MgStop(&Core)==MgOk && !memcmp(Registers,PadOriginal,sizeof(Registers)));
  /* Pins from different logical banks can share registers, but no field.
   * Interleaved per-pin alias writes must preserve every other bit. */
  Start(); memcpy(PadOriginal,Registers,sizeof(Registers));
  for (P=0;P<MG_PINS;P++) CHECK(MgConnectEx(&Core,P,1,(MG_PULL)(P%4))==MgOk);
  for (P=MG_PINS;P>0;P--) CHECK(MgDisconnect(&Core,P-1,0)==MgOk);
  CHECK(!memcmp(Registers,PadOriginal,sizeof(Registers)));
  Start(); Before=Writes; CHECK(MgConnectEx(&Core,9,1,(MG_PULL)4)==MgInvalid && Writes==Before);
  CHECK(MgConnectEx(&Core,9,1,(MG_PULL)-1)==MgInvalid && Writes==Before);
  /* Unused IOCFG RSEL/drive/Schmitt bits remain byte-for-byte intact. */
  for (S=2;S<MG_SPACES;S++) for (I=0;I<MG_MMIO_LENGTH/4;I++) CHECK(Registers[S][I]==PadOriginal[S][I]);
}

static void SharedFields(void) {
  unsigned A,B,Baseline,Before,Slot;
  SharedPair(&A,&B);
  Start(); SafeSchmittGroup(A); SetField(&MgPads[A].Schmitt,0);
  Baseline=Field(&MgPads[A].Schmitt); memcpy(PadOriginal,Registers,sizeof(Registers));
  CHECK(MgConnectEx(&Core,A,1,MgPullDefault)==MgOk);
  CHECK(Field(&MgPads[A].Schmitt)==1);
  CHECK(MgConnectEx(&Core,B,1,MgPullDefault)==MgOk);
  CHECK(Core.Pin[A].SharedIndex==Core.Pin[B].SharedIndex);
  Slot=Core.Pin[A].SharedIndex-1U;
  CHECK(Core.Shared[Slot].Users==2 && Core.Shared[Slot].Baseline==Baseline);
  CHECK(MgDisconnect(&Core,A,0)==MgOk && Core.Shared[Slot].Users==1);
  CHECK(Field(&MgPads[B].Schmitt)==1);
  CHECK(MgDisconnect(&Core,B,0)==MgOk && Core.SharedCount==0);
  CHECK(Field(&MgPads[A].Schmitt)==Baseline);
  CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)));

  Start(); SafeSchmittGroup(A); SetField(&MgPads[A].Schmitt,1);
  CHECK(MgConnectEx(&Core,A,2,MgPullDefault)==MgOk);
  Before=Writes;
  CHECK(MgConnectEx(&Core,B,1,MgPullDefault)==MgBusy && Writes==Before);
  CHECK(!Core.Pin[B].Saved && Core.Active);
  CHECK(MgDisconnect(&Core,A,0)==MgOk && MgStop(&Core)==MgOk);

  Start(); SafeSchmittGroup(A); SetField(&MgPads[A].Schmitt,0);
  SetMode(B,1); memcpy(PadOriginal,Registers,sizeof(Registers));
  CHECK(MgConnectEx(&Core,A,1,MgPullDefault)==MgBusy && Core.Active && !Core.Pin[A].Saved);
  CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)));

  Start(); SafeSchmittGroup(A); SetField(&MgPads[A].Schmitt,0);
  CHECK(MgConnectEx(&Core,A,1,MgPullDefault)==MgOk);
  CHECK(MgDisconnect(&Core,A,1)==MgOk && Core.SharedCount==0);
  CHECK(Core.Pin[A].Saved==0 && Field(&MgPads[A].Schmitt)==1);
  CHECK(MgStop(&Core)==MgOk);
}
static void Power(void) {
  unsigned Out; uint32_t Bit;
  const unsigned Input=9;
  for (Out=152;Out<MG_PINS;Out++)
    if (!SameDescriptor(&MgPads[Out].Schmitt,&MgPads[Input].Schmitt)) break;
  CHECK(Out<MG_PINS);
  Start(); SafeSchmittGroup(Out); SafeSchmittGroup(Input);
  SetField(&MgPads[Out].Schmitt,1); SetField(&MgPads[Input].Schmitt,0);
  CHECK(MgConnectEx(&Core,Out,2,MgPullDown)==MgOk);
  CHECK(MgEnableEx(&Core,Input,1,0,MgPullUp)==MgOk);
  Bit=1U<<(Out%32U);
  CHECK(MgWrite(&Core,Out/32U,Bit,0)==MgOk);
  CHECK(MgSuspend(&Core)==MgOk && !Core.Active && Core.Suspended);
  CHECK((Registers[1][0x80/4+Input/32U]&(1U<<(Input%32U)))!=0);
  CHECK(MgStart(&Core)==MgBusy); /* a suspended owner must resume in place */
  CHECK(MgSuspend(&Core)==MgBusy);
  CHECK(Core.Pin[Out].ContextValid && Core.Pin[Input].ContextValid);
  {
    unsigned ExpectedOutput=Core.Pin[Out].ContextOutput;
    unsigned ExpectedMode=Core.Pin[Out].ContextMode;
    unsigned ExpectedDirection=Core.Pin[Out].ContextDirection;
    unsigned ExpectedPad=Core.Pin[Out].ContextPad;
    unsigned ExpectedSm=Core.Pin[Out].ContextSchmitt;
    unsigned ExpectedDrive=Core.Pin[Out].ContextDrive;
    unsigned ExpectedRsel=Core.Pin[Out].ContextRsel;
    unsigned ExpectedEh=Core.Pin[Out].ContextEh;
    unsigned ExpectedSensitivity=Core.Pin[Input].ContextSensitivity;
    unsigned ExpectedPolarity=Core.Pin[Input].ContextPolarity;
    memset(Registers,0,sizeof(Registers));
    CHECK(MgResume(&Core)==MgOk && Core.Active && !Core.Suspended);
    CHECK(((Registers[0][0x300/4+(Out/8)*4]>>((Out%8)*4))&7U)==ExpectedMode);
    CHECK(!!(Registers[0][(Out/32)*4]&(1U<<(Out%32)))==ExpectedDirection);
    CHECK(!!(Registers[0][0x100/4+(Out/32)*4]&(1U<<(Out%32)))==ExpectedOutput);
    CHECK(RawField(&MgPads[Out].First)==(ExpectedPad&1U));
    CHECK(RawField(&MgPads[Out].Input)==((ExpectedPad>>3)&1U));
    CHECK(RawField(&MgPads[Out].Schmitt)==ExpectedSm);
    if (MgPads[Out].Drive.Width) CHECK(RawField(&MgPads[Out].Drive)==ExpectedDrive);
    if (MgPads[Out].Rsel.Width) CHECK(RawField(&MgPads[Out].Rsel)==ExpectedRsel);
    if (MgPads[Out].Eh.Width) CHECK(RawField(&MgPads[Out].Eh)==ExpectedEh);
    CHECK(!!(Registers[1][0x140/4+Input/32U]&(1U<<(Input%32U)))==ExpectedSensitivity);
    CHECK(!!(Registers[1][0x300/4+Input/32U]&(1U<<(Input%32U)))==ExpectedPolarity);
    CHECK(!(Registers[1][0x80/4+Input/32U]&(1U<<(Input%32U))));
  }
  CHECK(MgStop(&Core)==MgOk && !Core.Active && !Core.Suspended);
}


static void ResumeFailures(void) {
  unsigned Out,N,Count; const unsigned Input=9;
  for (Out=152;Out<MG_PINS;Out++)
    if (!SameDescriptor(&MgPads[Out].Schmitt,&MgPads[Input].Schmitt)) break;
  CHECK(Out<MG_PINS);
  Start(); SafeSchmittGroup(Out); SafeSchmittGroup(Input);
  SetField(&MgPads[Out].Schmitt,1); SetField(&MgPads[Input].Schmitt,0);
  CHECK(MgConnectEx(&Core,Out,2,MgPullDown)==MgOk);
  CHECK(MgEnableEx(&Core,Input,1,0,MgPullUp)==MgOk);
  CHECK(MgSuspend(&Core)==MgOk); memset(Registers,0,sizeof(Registers));
  Operations=0; CHECK(MgResume(&Core)==MgOk); Count=Operations; CHECK(MgStop(&Core)==MgOk);
  for (N=1;N<=Count;N++) {
    Start(); SafeSchmittGroup(Out); SafeSchmittGroup(Input);
    SetField(&MgPads[Out].Schmitt,1); SetField(&MgPads[Input].Schmitt,0);
    CHECK(MgConnectEx(&Core,Out,2,MgPullDown)==MgOk);
    CHECK(MgEnableEx(&Core,Input,1,0,MgPullUp)==MgOk);
    CHECK(MgSuspend(&Core)==MgOk); memset(Registers,0,sizeof(Registers));
    Operations=0; FailOperation=N;
    CHECK(MgResume(&Core)==MgIoError && !Core.Active && Core.Suspended);
    FailOperation=0;
    CHECK(MgResume(&Core)==MgOk && Core.Active && !Core.Suspended);
    CHECK(MgStop(&Core)==MgOk);
  }
}

static void WakeFixture(void) {
  static const unsigned Pins[]={0,9,33,65,97,125}; unsigned I;
  Start();
  for (I=0;I<sizeof(Pins)/sizeof(Pins[0]);I++) CHECK(MgEnable(&Core,Pins[I],1,1)==MgOk);
  CHECK(MgConnect(&Core,0,1)==MgOk);
  /* Unowned mask and pending bits in every EINT port must survive. */
  for (I=0;I<5;I++) {
    Registers[1][0x80/4+I]&=~(1U<<10);
    Registers[1][I]=0x5A5A5A5AU;
  }
}
static void WakeSuspend(void) {
  uint32_t Wake[MG_BANKS]={1,2,0,1U<<29,0,0},V,A,E;
  unsigned B,P,N,Count,FirstWrite,Before;
  WakeFixture(); CHECK(MgMask(&Core,0,1)==MgOk);
  memcpy(PadOriginal,Registers,sizeof(Registers)); Before=Operations;
  for (B=0;B<MG_BANKS;B++) {
    uint32_t Invalid[MG_BANKS]={0}; Invalid[B]=1U<<31;
    CHECK(MgSuspendWake(&Core,Invalid)==MgInvalid && Operations==Before && Core.Active && !Core.Suspended);
  }
  CHECK(MgSuspendWake(NULL,Wake)==MgInvalid && Operations==Before);
  CHECK(MgSuspendWake(&Core,Wake)==MgOk && !Core.Active && Core.Suspended);
  for (B=0;B<MG_BANKS;B++)
    CHECK((Registers[1][0x80/4+B]&Core.Owned[B])==(Core.Owned[B]&~Wake[B]));
  CHECK(!(Registers[1][0x80/4]&1)); /* explicitly armed despite its runtime mask */
  CHECK(Registers[1][0]==PadOriginal[1][0]); /* no ACK of a pending edge */
  Before=Operations;
  CHECK(MgSuspendWake(&Core,Wake)==MgBusy && MgRead(&Core,0,0,&V)==MgNotReady && Operations==Before);
  CHECK(MgResume(&Core)==MgOk && Core.Active && !Core.Suspended);
  CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)));
  CHECK(MgQuery(&Core,0,UINT32_MAX,&A,&E)==MgOk && !(E&1) && (E&(1U<<9)));
  CHECK(Core.Pin[0].IoAlias && Core.Pin[0].Kind==3);
  CHECK(MgStop(&Core)==MgOk);

  WakeFixture(); memcpy(PadOriginal,Registers,sizeof(Registers));
  Operations=FirstEintWrite=0;
  CHECK(MgSuspendWake(&Core,Wake)==MgOk); Count=Operations; FirstWrite=FirstEintWrite;
  CHECK(FirstWrite>0 && FirstWrite<Count);
  CHECK(MgResume(&Core)==MgOk && MgStop(&Core)==MgOk);
  for (N=1;N<=Count;N++) {
    WakeFixture(); memcpy(PadOriginal,Registers,sizeof(Registers));
    Operations=0; FailOperation=N;
    CHECK(MgSuspendWake(&Core,Wake)==MgIoError && Core.Active && !Core.Suspended);
    CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)));
    for (P=0;P<MG_PINS;P++) CHECK(!Core.Pin[P].ContextValid);
    FailOperation=0;
    CHECK(MgSuspendWake(&Core,Wake)==MgOk && MgResume(&Core)==MgOk && MgStop(&Core)==MgOk);
  }
  /* A second error during recovery must retain the snapshot and block I/O. */
  WakeFixture(); memcpy(PadOriginal,Registers,sizeof(Registers));
  Operations=0; FailOperation=FirstWrite; FailOperation2=FirstWrite+1;
  CHECK(MgSuspendWake(&Core,Wake)==MgIoError && !Core.Active && Core.Suspended);
  CHECK(Core.Pin[0].ContextValid && Core.Pin[125].ContextValid);
  Before=Operations;
  CHECK(MgRead(&Core,0,0,&V)==MgNotReady && MgStop(&Core)==MgNotReady && Operations==Before);
  FailOperation=FailOperation2=0;
  CHECK(MgResume(&Core)==MgOk && !memcmp(PadOriginal,Registers,sizeof(Registers)));
  CHECK(MgStop(&Core)==MgOk);

  /* A bus write can appear successful without reaching the mask register. */
  for (N=0;N<2;N++) {
    Start(); CHECK(MgEnable(&Core,0,1,1)==MgOk);
    if (N) CHECK(MgMask(&Core,0,1)==MgOk);
    memcpy(PadOriginal,Registers,sizeof(Registers));
    DropEintWrite=1; Wake[0]=N?1:0; Wake[1]=Wake[3]=0;
    CHECK(MgSuspendWake(&Core,Wake)==MgIoError && Core.Active && !Core.Suspended);
    CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)));
    CHECK(MgStop(&Core)==MgOk);
  }
}

static void InputInterruptSharing(void) {
  unsigned Order,Release,Pull,N,Count,Before; uint32_t V,A,E;
  for (Order=0;Order<2;Order++) for (Release=0;Release<2;Release++) for (Pull=0;Pull<4;Pull++) {
    Start();
    if (Order) {
      CHECK(MgConnectEx(&Core,0,1,(MG_PULL)Pull)==MgOk);
      CHECK(MgEnableEx(&Core,0,1,1,(MG_PULL)Pull)==MgOk);
    } else {
      CHECK(MgEnableEx(&Core,0,1,1,(MG_PULL)Pull)==MgOk); Before=Writes;
      CHECK(MgConnectEx(&Core,0,1,(MG_PULL)Pull)==MgOk && Writes==Before);
    }
    CHECK(Core.Pin[0].Kind==3 && Core.Pin[0].IoAlias && Core.Owned[0]==1 && Core.SharedCount==1);
    Before=Writes;
    CHECK(MgConnectEx(&Core,0,1,(MG_PULL)Pull)==MgBusy && Writes==Before);
    CHECK(MgConnectEx(&Core,0,2,(MG_PULL)Pull)==MgBusy && Writes==Before);
    CHECK(MgEnableEx(&Core,0,1,1,(MG_PULL)Pull)==MgBusy && Writes==Before);
    Registers[0][0x200/4]=0; CHECK(MgRead(&Core,0,0,&V)==MgOk && V==0);
    Registers[0][0x200/4]=1; CHECK(MgRead(&Core,0,0,&V)==MgOk && V==1);
    CHECK(MgWrite(&Core,0,1,0)==MgInvalid && Writes==Before);
    if (Release) {
      CHECK(MgDisableInterrupt(&Core,0)==MgOk && Core.Pin[0].Kind==1 && !Core.Owned[0]);
      CHECK(MgRead(&Core,0,0,&V)==MgOk && V==1 && Core.Pin[0].Saved);
      CHECK(MgDisconnectInputOutput(&Core,0,0)==MgOk && !Core.Pin[0].Saved);
    } else {
      CHECK(MgDisconnectInputOutput(&Core,0,1)==MgOk && Writes==Before && Core.Pin[0].Kind==3);
      Registers[1][0]|=1; CHECK(MgQuery(&Core,0,UINT32_MAX,&A,&E)==MgOk && A==1 && E==1);
      CHECK(MgDisableInterrupt(&Core,0)==MgOk && !Core.Pin[0].Saved && !Core.Owned[0]);
    }
    CHECK(Registers[1][0x140/4]==0x55AA55AAU && Registers[1][0x300/4]==0xA55AA55AU);
    // Ignore the synthetic GPIO input level when comparing the inherited image.
    Registers[0][0x200/4]=Original[0x200/4];
    CHECK(!memcmp(Original,Registers[0],sizeof(Original)) && !Core.SharedCount);
    CHECK(MgStop(&Core)==MgOk);
  }
  Start(); CHECK(MgEnableEx(&Core,0,1,1,MgPullDefault)==MgOk); Before=Writes;
  CHECK(MgConnectEx(&Core,0,1,MgPullUp)==MgBusy && Writes==Before && !Core.Pin[0].IoAlias);
  CHECK(MgDisconnectInputOutput(&Core,0,0)==MgInvalid && Core.Owned[0]==1 && Writes==Before);
  CHECK(MgConnectEx(&Core,0,1,MgPullDefault)==MgOk);
  Operations=0; CHECK(MgDisableInterrupt(&Core,0)==MgOk); Count=Operations;
  CHECK(MgDisconnectInputOutput(&Core,0,0)==MgOk);
  for (N=1;N<=Count;N++) {
    Start(); CHECK(MgEnable(&Core,0,1,1)==MgOk); CHECK(MgConnect(&Core,0,1)==MgOk);
    Operations=0; FailOperation=N;
    CHECK(MgDisableInterrupt(&Core,0)==MgIoError && !Core.Active && Core.Pin[0].IoAlias && Core.Owned[0]==1);
    FailOperation=0; CHECK(MgStop(&Core)==MgOk && !Core.Pin[0].Saved && !Core.Owned[0]);
  }
  Start(); CHECK(MgConnect(&Core,0,1)==MgOk); Before=Writes;
  CHECK(MgEnableEx(&Core,0,1,1,MgPullUp)==MgBusy && Writes==Before && Core.Pin[0].Kind==1);
  CHECK(MgEnable(&Core,0,1,1)==MgOk); CHECK(MgSuspend(&Core)==MgOk); CHECK(MgResume(&Core)==MgOk);
  CHECK(Core.Pin[0].IoAlias && Core.Pin[0].Kind==3 && Core.Owned[0]==1);
  CHECK(MgStop(&Core)==MgOk && !Core.Pin[0].IoAlias && !Core.Pin[0].Saved);
  CHECK(MgDisableInterrupt(&Core,0)==MgInvalid && MgDisableInterrupt(&Core,126)==MgInvalid);
  CHECK(MgDisconnectInputOutput(&Core,188,0)==MgInvalid && MgDisconnectInputOutput(&Core,0,2)==MgInvalid);
}

static void LnaFixture(unsigned Pin,unsigned M,unsigned D,unsigned O,unsigned Schmitt) {
  uint32_t Bit=1U<<Pin;
  Start(); SetMode(Pin,M);
  Registers[0][0]=(Registers[0][0]&~Bit)|(D?Bit:0);
  Registers[0][0x100/4]=(Registers[0][0x100/4]&~Bit)|(O?Bit:0);
  SetField(&MgPads[Pin].Schmitt,Schmitt);
  memcpy(PadOriginal,Registers,sizeof(Registers));
}
static void GpsLna(void) {
  unsigned P,M,D,O,H,I,N,State,Count,Before,Latch; uint32_t V,Bit;
  static uint32_t Expected[MG_SPACES][MG_MMIO_LENGTH/4];
  for (P=25;P<=26;P++) for (M=0;M<8;M++) for (D=0;D<2;D++)
      for (O=0;O<2;O++) for (H=0;H<2;H++) {
    LnaFixture(P,M,D,O,H); Before=Writes; Bit=1U<<P;
    CHECK(MgGpsLnaAcquire(&Core,P)==MgOk && Core.Pin[P].Saved && Core.Pin[P].Kind==4);
    CHECK(Writes==Before && !memcmp(PadOriginal,Registers,sizeof(Registers)));
    CHECK(MgGpsLnaAcquire(&Core,P)==MgBusy && MgConnect(&Core,P,1)==MgBusy);
    CHECK(MgEnable(&Core,P,1,1)==MgBusy && MgDisconnect(&Core,P,1)==MgUnsupported);
    CHECK(MgWrite(&Core,0,Bit,0)==MgInvalid && Writes==Before);
    CHECK(MgRead(&Core,0,0,&V)==MgOk && V==0 && MgRead(&Core,0,1,&V)==MgOk && V==0);
    Before=Operations;
    CHECK(MgSuspend(&Core)==MgUnsupported && MgSuspendWake(&Core,0)==MgUnsupported);
    CHECK(Core.Active && !Core.Suspended && Operations==Before && !Core.Pin[P].ContextValid);
    memcpy(Expected,PadOriginal,sizeof(Expected)); Latch=O;
    for (I=0;I<9;I++) {
      State=(I+M)%3;
      CHECK(MgGpsLnaSet(&Core,P,(MG_GPS_LNA_STATE)State)==MgOk);
      Expected[0][0]=(Expected[0][0]&~Bit)|(State==MgGpsLnaDsp?0:Bit);
      Expected[0][0x300/4+(P/8)*4]=(Expected[0][0x300/4+(P/8)*4]&~(7U<<((P%8)*4)))|
          ((State==MgGpsLnaDsp?2U:0U)<<((P%8)*4));
      if (State!=MgGpsLnaDsp) Latch=State==MgGpsLnaOn;
      Expected[0][0x100/4]=(Expected[0][0x100/4]&~Bit)|(Latch?Bit:0);
      CHECK(!memcmp(Expected,Registers,sizeof(Registers)));
    }
    CHECK(MgDisconnect(&Core,P,0)==MgOk && !Core.Pin[P].Saved && !Core.SharedCount);
    CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)) && MgStop(&Core)==MgOk);
  }
  Start(); Before=Operations;
  CHECK(MgGpsLnaAcquire(0,25)==MgNotReady && MgGpsLnaSet(0,25,MgGpsLnaOff)==MgNotReady);
  CHECK(MgGpsLnaAcquire(&Core,24)==MgInvalid && MgGpsLnaAcquire(&Core,188)==MgInvalid);
  CHECK(MgGpsLnaSet(&Core,25,MgGpsLnaOff)==MgInvalid && MgGpsLnaSet(&Core,26,(MG_GPS_LNA_STATE)-1)==MgInvalid);
  CHECK(MgGpsLnaSet(&Core,27,MgGpsLnaDsp)==MgInvalid && Operations==Before);
  CHECK(MgGpsLnaAcquire(&Core,25)==MgOk && MgGpsLnaAcquire(&Core,26)==MgOk && Core.SharedCount==2);
  CHECK(MgGpsLnaSet(&Core,25,MgGpsLnaDsp)==MgOk && MgGpsLnaSet(&Core,26,MgGpsLnaOn)==MgOk);
  CHECK(MgStop(&Core)==MgOk && !Core.SharedCount && !memcmp(Original,Registers[0],sizeof(Original)));
  for (P=25;P<=26;P++) {
    LnaFixture(P,2,1,1,1); Operations=0; CHECK(MgGpsLnaAcquire(&Core,P)==MgOk); Count=Operations;
    for (N=1;N<=Count;N++) {
      LnaFixture(P,2,1,1,1); Operations=0; FailOperation=N;
      CHECK(MgGpsLnaAcquire(&Core,P)==MgIoError); FailOperation=0;
      CHECK(MgStop(&Core)==MgOk && !memcmp(PadOriginal,Registers,sizeof(Registers)));
    }
    for (State=0;State<3;State++) {
      LnaFixture(P,State==MgGpsLnaDsp?0:2,1,1,1);
      CHECK(MgGpsLnaAcquire(&Core,P)==MgOk); Operations=0;
      CHECK(MgGpsLnaSet(&Core,P,(MG_GPS_LNA_STATE)State)==MgOk); Count=Operations;
      for (N=1;N<=Count;N++) {
        LnaFixture(P,State==MgGpsLnaDsp?0:2,1,1,1); CHECK(MgGpsLnaAcquire(&Core,P)==MgOk);
        Operations=0; FailOperation=N;
        CHECK(MgGpsLnaSet(&Core,P,(MG_GPS_LNA_STATE)State)==MgIoError && !Core.Active);
        CHECK(Core.Pin[P].Saved && Core.Pin[P].Kind==4); Before=Operations;
        CHECK(MgGpsLnaSet(&Core,P,MgGpsLnaOff)==MgNotReady && Operations==Before);
        FailOperation=0; CHECK(MgStop(&Core)==MgOk && !Core.Pin[P].Saved && !Core.SharedCount);
        CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)));
      }
    }
    for (State=0;State<3;State++) for (N=1;N<=(State==MgGpsLnaDsp?2U:4U);N++) {
      LnaFixture(P,State==MgGpsLnaDsp?0:2,1,State==MgGpsLnaOn?0:1,1);
      CHECK(MgGpsLnaAcquire(&Core,P)==MgOk); DropGpioWrite=N;
      CHECK(MgGpsLnaSet(&Core,P,(MG_GPS_LNA_STATE)State)==MgIoError && !Core.Active);
      CHECK(MgStop(&Core)==MgOk && !memcmp(PadOriginal,Registers,sizeof(Registers)));
    }
    LnaFixture(P,7,1,0,1); CHECK(MgGpsLnaAcquire(&Core,P)==MgOk && MgGpsLnaSet(&Core,P,MgGpsLnaDsp)==MgOk);
    Operations=0; CHECK(MgDisconnect(&Core,P,0)==MgOk); Count=Operations;
    for (N=1;N<=Count;N++) {
      LnaFixture(P,7,1,0,1); CHECK(MgGpsLnaAcquire(&Core,P)==MgOk && MgGpsLnaSet(&Core,P,MgGpsLnaDsp)==MgOk);
      Operations=0; FailOperation=N;
      CHECK(MgDisconnect(&Core,P,0)==MgIoError && !Core.Active && Core.Pin[P].Saved);
      FailOperation=0; CHECK(MgStop(&Core)==MgOk && !Core.Pin[P].Saved && !Core.SharedCount);
      CHECK(!memcmp(PadOriginal,Registers,sizeof(Registers)));
    }
  }
}

int main(void) {
  Pins(); Interrupts(); PmicInterrupts(); PmicFailures(); Failures(); Pads(); SharedFields(); Power(); ResumeFailures(); WakeSuspend(); InputInterruptSharing(); GpsLna();
  printf("GPIO/EINT hardware: %u checks passed (188 physical GPIOs, 126 pad interrupt routes, logical PMIC EINT137 route, inherited EINT restoration, suspend/resume, pad isolation, aliases, ownership failures)\n",Checks); return 0;
}
