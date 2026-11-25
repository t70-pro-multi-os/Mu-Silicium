/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Hardware.h"
#include "PinMap.h"
#define SIGNATURE 0x4D473031U
#define GPIO 0U
#define GPS_LNA_KIND 4U
#define EINT 1U
#define IOCFG_RT 7U
#define ACK 0x040U
#define MASK 0x080U
#define MASK_SET 0x0C0U
#define MASK_CLR 0x100U
#define SENS_SET 0x180U
#define SENS_CLR 0x1C0U
#define POL_SET 0x340U
#define POL_CLR 0x380U
#define DOMAIN 0x400U
static int Valid(const MG_CORE *C) { return C && C->Signature==SIGNATURE; }
static int Ready(const MG_CORE *C) { return Valid(C) && C->Active; }
uint32_t MgValidMask(unsigned Bank) { return Bank>=MG_BANKS?0:Bank==5?0x1FFFFFFFU:UINT32_MAX; }
static uint32_t IrqMask(unsigned Bank) { return Bank<3?UINT32_MAX:Bank==3?0x3FFFFFFFU:0; }
static MG_STATUS Read(MG_CORE *C,unsigned Space,unsigned Offset,uint32_t *Value) {
  if (Space>=MG_SPACES || (Offset&3U) || Offset>MG_MMIO_LENGTH-4) return MgInvalid;
  return C->Io.Read(C->Io.Context,Space,Offset,Value);
}
static MG_STATUS Write(MG_CORE *C,unsigned Space,unsigned Offset,uint32_t Value) {
  if (Space>=MG_SPACES || (Offset&3U) || Offset>MG_MMIO_LENGTH-4) return MgInvalid;
  return C->Io.Write(C->Io.Context,Space,Offset,Value);
}
static MG_STATUS Fault(MG_CORE *C,MG_STATUS S) { if (S!=MgOk) C->Active=0; return S; }
static MG_STATUS BankWrite(MG_CORE *C,unsigned Register,unsigned Bank,uint32_t Bits) {
  return Write(C,EINT,Register+Bank*4,Bits);
}
static MG_STATUS PmicDomain(MG_CORE *C) {
  uint32_t Value; MG_STATUS S=Read(C,EINT,DOMAIN+MG_PMIC_EINT_BANK*4U,&Value);
  return S!=MgOk?S:(Value&MG_PMIC_EINT_BIT)?MgOk:MgNotReady;
}
static MG_STATUS PmicSetBit(MG_CORE *C,unsigned ReadRegister,unsigned SetRegister,
                            unsigned ClearRegister,int Value) {
  uint32_t Current; MG_STATUS S;
  if (Value!=0 && Value!=1) return MgInvalid;
  S=PmicDomain(C); if (S!=MgOk) return S;
  S=Read(C,EINT,ReadRegister+MG_PMIC_EINT_BANK*4U,&Current);
  if (S!=MgOk) return S;
  if (!!(Current&MG_PMIC_EINT_BIT)!=Value) {
    S=PmicDomain(C); if (S!=MgOk) return S;
    S=BankWrite(C,Value?SetRegister:ClearRegister,MG_PMIC_EINT_BANK,MG_PMIC_EINT_BIT);
    if (S!=MgOk) return S;
    S=PmicDomain(C); if (S!=MgOk) return S;
    S=Read(C,EINT,ReadRegister+MG_PMIC_EINT_BANK*4U,&Current);
    if (S!=MgOk) return S;
  }
  return !!(Current&MG_PMIC_EINT_BIT)==Value?MgOk:MgIoError;
}
static MG_STATUS PmicSetMasked(MG_CORE *C,int Masked) {
  return PmicSetBit(C,MASK,MASK_SET,MASK_CLR,Masked);
}
static MG_STATUS PmicAcknowledge(MG_CORE *C) {
  MG_STATUS S=PmicDomain(C);
  return S==MgOk?BankWrite(C,ACK,MG_PMIC_EINT_BANK,MG_PMIC_EINT_BIT):S;
}
static MG_STATUS PmicSetSensitivity(MG_CORE *C,int Level) {
  return PmicSetBit(C,0x140U,SENS_SET,SENS_CLR,Level);
}
static MG_STATUS PmicSetPolarity(MG_CORE *C,int High) {
  return PmicSetBit(C,0x300U,POL_SET,POL_CLR,High);
}
static MG_STATUS PmicReadState(MG_CORE *C,uint8_t *Sensitivity,uint8_t *Polarity,
                               uint8_t *Masked) {
  uint32_t S,P,M; MG_STATUS R;
  if (!Sensitivity || !Polarity || !Masked) return MgInvalid;
  R=PmicDomain(C); if (R!=MgOk) return R;
  R=Read(C,EINT,0x140U+MG_PMIC_EINT_BANK*4U,&S); if (R!=MgOk) return R;
  R=Read(C,EINT,0x300U+MG_PMIC_EINT_BANK*4U,&P); if (R!=MgOk) return R;
  R=Read(C,EINT,MASK+MG_PMIC_EINT_BANK*4U,&M); if (R!=MgOk) return R;
  *Sensitivity=(uint8_t)!!(S&MG_PMIC_EINT_BIT);
  *Polarity=(uint8_t)!!(P&MG_PMIC_EINT_BIT);
  *Masked=(uint8_t)!!(M&MG_PMIC_EINT_BIT);
  return MgOk;
}
static MG_STATUS PmicVerifyTrigger(MG_CORE *C) {
  uint8_t Sensitivity,Polarity,Masked; MG_STATUS S;
  if (!C->PmicInterrupt.Saved) return MgInvalid;
  S=PmicReadState(C,&Sensitivity,&Polarity,&Masked);
  if (S!=MgOk) { (void)PmicSetMasked(C,1); return S; }
  if (Sensitivity!=1 || Polarity!=1) {
    (void)PmicSetMasked(C,1);
    return MgNotReady;
  }
  return MgOk;
}
static void PmicClearState(MG_CORE *C) {
  C->PmicInterrupt.Saved=0;
  C->PmicInterrupt.Sensitivity=C->PmicInterrupt.Polarity=C->PmicInterrupt.Masked=0;
  C->PmicInterrupt.ContextValid=C->PmicInterrupt.ContextSensitivity=0;
  C->PmicInterrupt.ContextPolarity=C->PmicInterrupt.ContextUnmasked=0;
}
static MG_STATUS PmicRestore(MG_CORE *C) {
  MG_VIRTUAL_IRQ *P=&C->PmicInterrupt; MG_STATUS S;
  if (!P->Saved) return MgInvalid;
  S=PmicSetMasked(C,1);
  if (S==MgOk) S=PmicAcknowledge(C);
  if (S==MgOk) S=PmicSetPolarity(C,P->Polarity);
  if (S==MgOk) S=PmicSetSensitivity(C,P->Sensitivity);
  if (S==MgOk) S=PmicSetMasked(C,P->Masked);
  if (S==MgOk) PmicClearState(C);
  return S;
}
static MG_STATUS PmicEnable(MG_CORE *C,MG_PULL Pull) {
  MG_VIRTUAL_IRQ *P=&C->PmicInterrupt; uint8_t Sensitivity,Polarity,Masked; MG_STATUS S;
  if (!Ready(C)) return MgNotReady;
  if (Pull!=MgPullDefault) return MgUnsupported;
  if (P->Saved) return MgBusy;
  S=PmicReadState(C,&Sensitivity,&Polarity,&Masked); if (S!=MgOk) return Fault(C,S);
  /* MgStart requires the line masked. Do not adopt an inherited/unowned live
   * interrupt even if its trigger happens to match the desired policy. */
  if (!Masked) return Fault(C,MgNotReady);
  P->Sensitivity=Sensitivity; P->Polarity=Polarity; P->Masked=Masked;
  P->ContextValid=P->ContextSensitivity=P->ContextPolarity=P->ContextUnmasked=0;
  P->Saved=1;
  S=PmicSetMasked(C,1);
  if (S==MgOk) S=PmicSetPolarity(C,1);
  if (S==MgOk) S=PmicSetSensitivity(C,1);
  if (S==MgOk) S=PmicAcknowledge(C);
  if (S==MgOk) S=PmicSetMasked(C,0);
  if (S!=MgOk) (void)PmicSetMasked(C,1);
  return Fault(C,S);
}
static MG_STATUS PmicDisable(MG_CORE *C) {
  if (!Valid(C)) return MgInvalid;
  return Fault(C,PmicRestore(C));
}
static MG_STATUS PmicSnapshot(MG_CORE *C) {
  MG_VIRTUAL_IRQ *P=&C->PmicInterrupt; uint8_t Sensitivity,Polarity,Masked; MG_STATUS S;
  if (!P->Saved) { P->ContextValid=0; return MgOk; }
  S=PmicReadState(C,&Sensitivity,&Polarity,&Masked);
  if (S!=MgOk) { (void)PmicSetMasked(C,1); return S; }
  if (Sensitivity!=1 || Polarity!=1) {
    (void)PmicSetMasked(C,1);
    return MgNotReady;
  }
  P->ContextSensitivity=Sensitivity; P->ContextPolarity=Polarity;
  P->ContextUnmasked=(uint8_t)!Masked; P->ContextValid=1;
  return MgOk;
}
static MG_STATUS PmicKeepMasked(MG_CORE *C) {
  return C->PmicInterrupt.Saved?PmicSetMasked(C,1):MgOk;
}
static MG_STATUS PmicRestoreContext(MG_CORE *C) {
  MG_VIRTUAL_IRQ *P=&C->PmicInterrupt; MG_STATUS S;
  if (!P->Saved) return MgOk;
  if (!P->ContextValid) return MgInvalid;
  S=PmicSetPolarity(C,P->ContextPolarity);
  if (S==MgOk) S=PmicSetSensitivity(C,P->ContextSensitivity);
  return S;
}
static MG_STATUS PmicRestoreContextMask(MG_CORE *C) {
  MG_VIRTUAL_IRQ *P=&C->PmicInterrupt;
  if (!P->Saved) return MgOk;
  if (!P->ContextValid) return MgInvalid;
  return PmicSetMasked(C,!P->ContextUnmasked);
}
static MG_STATUS Mode(MG_CORE *C,unsigned Pin,unsigned Value) {
  /* The bit above each 3-bit mode field enables its atomic masked write. */
  unsigned Offset=0x300U+(Pin/8U)*16U,Shift=(Pin%8U)*4U; uint32_t V;
  MG_STATUS S=Write(C,GPIO,Offset+12U,(8U|Value)<<Shift);
  if (S==MgOk) S=Read(C,GPIO,Offset,&V);
  return S!=MgOk?S:((V>>Shift)&7U)==Value?MgOk:MgIoError;
}
static MG_STATUS Direction(MG_CORE *C,unsigned Pin,unsigned Output) {
  unsigned Offset=(Pin/32U)*16U,Shift=Pin%32U; uint32_t V;
  MG_STATUS S=Write(C,GPIO,Offset+(Output?4U:8U),1U<<Shift);
  if (S==MgOk) S=Read(C,GPIO,Offset,&V);
  return S!=MgOk?S:((V>>Shift)&1U)==Output?MgOk:MgIoError;
}
static MG_STATUS Output(MG_CORE *C,unsigned Pin,unsigned Value) {
  unsigned Offset=0x100U+(Pin/32U)*16U,Shift=Pin%32U; uint32_t V;
  MG_STATUS S=Write(C,GPIO,Offset+(Value?4U:8U),1U<<Shift);
  if (S==MgOk) S=Read(C,GPIO,Offset,&V);
  return S!=MgOk?S:((V>>Shift)&1U)==Value?MgOk:MgIoError;
}
/* SET/CLR aliases preserve unrelated IOCFG bits. Schmitt fields are shared
 * descriptors and are tracked with one baseline and a compatible-user count. */
static uint32_t FieldMask(const MG_FIELD *F) {
  if (!F || !F->Width || F->Width>8 || F->Bit+F->Width>32) return 0;
  return ((1U<<F->Width)-1U)<<F->Bit;
}
static MG_STATUS FieldRead(MG_CORE *C,const MG_FIELD *F,uint8_t *Value) {
  uint32_t V,Mask=FieldMask(F); MG_STATUS S;
  if (!Mask || !Value) return MgInvalid;
  S=Read(C,F->Space,F->Offset,&V);
  if (S==MgOk) *Value=(uint8_t)((V>>F->Bit)&((1U<<F->Width)-1U));
  return S;
}
static MG_STATUS FieldWrite(MG_CORE *C,const MG_FIELD *F,unsigned Value) {
  uint32_t Mask=FieldMask(F),Set,Clear; uint8_t ReadBack=0; MG_STATUS S;
  if (!Mask || Value>=(1U<<F->Width)) return MgInvalid;
  S=FieldRead(C,F,&ReadBack);
  if (S!=MgOk || ReadBack==Value) return S;
  Set=Value<<F->Bit; Clear=Mask&~Set;
  /* Match stock mtk_hw_set_value_race_free: assert set bits, then clear bits. */
  S=Set?Write(C,F->Space,F->Offset+4U,Set):MgOk;
  if (S==MgOk && Clear) S=Write(C,F->Space,F->Offset+8U,Clear);
  if (S==MgOk) S=FieldRead(C,F,&ReadBack);
  return S!=MgOk?S:ReadBack==Value?MgOk:MgIoError;
}
static int SameField(const MG_FIELD *A,const MG_FIELD *B) {
  return A->Offset==B->Offset && A->Space==B->Space &&
      A->Bit==B->Bit && A->Width==B->Width;
}
static int SharedFind(const MG_CORE *C,const MG_FIELD *F) {
  unsigned I;
  for (I=0;I<MG_PINS;I++)
    if (C->Shared[I].Valid && SameField(&C->Shared[I].Field,F)) return (int)I;
  return -1;
}
static int PadUses(const MG_PAD *P,const MG_FIELD *F) {
  return (P->First.Width && SameField(&P->First,F)) ||
      (P->Second.Width && SameField(&P->Second,F)) ||
      (P->Third.Width && SameField(&P->Third,F)) ||
      (P->Input.Width && SameField(&P->Input,F)) ||
      (P->Schmitt.Width && SameField(&P->Schmitt,F)) ||
      (P->Drive.Width && SameField(&P->Drive,F)) ||
      (P->Rsel.Width && SameField(&P->Rsel,F)) ||
      (P->Eh.Width && SameField(&P->Eh,F));
}
static MG_STATUS PinModeRead(MG_CORE *C,unsigned Pin,uint8_t *Value) {
  uint32_t V; MG_STATUS S=Read(C,GPIO,0x300U+(Pin/8U)*16U,&V);
  if (S==MgOk) *Value=(uint8_t)((V>>((Pin%8U)*4U))&7U);
  return S;
}
/* A pad field may be shared with a pin outside this driver's current set.
 * Do not change it while such a pin is muxed to a peripheral. GpioClx-owned
 * siblings are tracked here and may share only the same captured raw value. */
static MG_STATUS FieldConsumersSafe(MG_CORE *C,unsigned Pin,const MG_FIELD *F,unsigned Value,int CheckBaseline) {
  unsigned Q; uint8_t M; MG_STATUS S;
  if (C->Suspended) {
    if (!C->Pin[Pin].ContextValid || C->Pin[Pin].ContextMode!=0) return MgBusy;
  } else if (CheckBaseline && C->Pin[Pin].Saved && C->Pin[Pin].Mode!=0) return MgBusy;
  else {
    S=PinModeRead(C,Pin,&M); if (S!=MgOk) return S;
    if (M!=0) return MgBusy;
  }
  for (Q=0;Q<MG_PINS;Q++) if (Q!=Pin && PadUses(&MgPads[Q],F)) {
    MG_PIN *Other=&C->Pin[Q]; const MG_PAD *Pad=&MgPads[Q]; int Matches=0;
    if (Other->Saved && CheckBaseline && !C->Suspended && Other->Mode!=0) return MgBusy;
    if (C->Suspended && Other->Saved) {
      if (!Other->ContextValid || Other->ContextMode!=0) return MgBusy;
      if (Pad->Schmitt.Width && SameField(&Pad->Schmitt,F)) {
        Matches=1; if (Other->ContextSchmitt!=Value) return MgBusy;
      }
      if (Pad->Drive.Width && SameField(&Pad->Drive,F)) {
        Matches=1; if (Other->ContextDrive!=Value) return MgBusy;
      }
      if (Pad->Rsel.Width && SameField(&Pad->Rsel,F)) {
        Matches=1; if (Other->ContextRsel!=Value) return MgBusy;
      }
      if (Pad->Eh.Width && SameField(&Pad->Eh,F)) {
        Matches=1; if (Other->ContextEh!=Value) return MgBusy;
      }
      if (!Matches) return MgBusy;
    } else {
      S=PinModeRead(C,Q,&M); if (S!=MgOk) return S;
      if (M!=0) return MgBusy;
    }
  }
  return MgOk;
}
static MG_STATUS FieldWriteGuarded(MG_CORE *C,unsigned Pin,const MG_FIELD *F,unsigned Value) {
  uint8_t Current; MG_STATUS S=FieldRead(C,F,&Current);
  if (S!=MgOk || Current==Value) return S;
  S=FieldConsumersSafe(C,Pin,F,Value,0);
  return S==MgOk?FieldWrite(C,F,Value):S;
}
static MG_STATUS SharedCanAcquire(const MG_CORE *C,unsigned Pin,unsigned Target) {
  const MG_FIELD *F=&MgPads[Pin].Schmitt; int I=SharedFind(C,F);
  if (!FieldMask(F)) return MgInvalid;
  return I>=0 && C->Shared[I].Target!=Target?MgBusy:MgOk;
}
static MG_STATUS SharedAcquire(MG_CORE *C,unsigned Pin,unsigned Target) {
  MG_PIN *P=&C->Pin[Pin]; const MG_FIELD *F=&MgPads[Pin].Schmitt;
  int Found=SharedFind(C,F); unsigned I; uint8_t Baseline; MG_STATUS S;
  if (Found>=0) {
    MG_SHARED_FIELD *Shared=&C->Shared[Found];
    if (Shared->Target!=Target || Shared->Users==UINT8_MAX) return MgBusy;
    if (Shared->Baseline!=Target) {
      S=FieldConsumersSafe(C,Pin,F,Target,1); if (S!=MgOk) return S;
    }
    Shared->Users++; P->SharedIndex=(uint8_t)(Found+1); P->SharedTarget=(uint8_t)Target;
    return MgOk;
  }
  for (I=0;I<MG_PINS;I++) if (!C->Shared[I].Valid) break;
  if (I==MG_PINS) return MgBusy;
  S=FieldRead(C,F,&Baseline); if (S!=MgOk) return S;
  if (Baseline!=Target) {
    S=FieldConsumersSafe(C,Pin,F,Target,1); if (S!=MgOk) return S;
  }
  C->Shared[I].Field=*F; C->Shared[I].Baseline=Baseline;
  C->Shared[I].Target=(uint8_t)Target; C->Shared[I].Users=1; C->Shared[I].Valid=1;
  C->SharedCount++; P->SharedIndex=(uint8_t)(I+1); P->SharedTarget=(uint8_t)Target;
  return Baseline==Target?MgOk:FieldWriteGuarded(C,Pin,F,Target);
}
static MG_STATUS SharedRelease(MG_CORE *C,unsigned Pin,int Preserve) {
  MG_PIN *P=&C->Pin[Pin]; unsigned I; MG_SHARED_FIELD *Shared; MG_STATUS S;
  if (!P->SharedIndex) return MgOk;
  I=(unsigned)P->SharedIndex-1U;
  if (I>=MG_PINS || !C->Shared[I].Valid || !C->Shared[I].Users) return MgInvalid;
  Shared=&C->Shared[I];
  if (Shared->Users>1) {
    Shared->Users--; P->SharedIndex=0; P->SharedTarget=0; return MgOk;
  }
  if (!Preserve) {
    S=FieldWriteGuarded(C,Pin,&Shared->Field,Shared->Baseline);
    if (S!=MgOk) return S;
  }
  Shared->Valid=0; Shared->Users=0; P->SharedIndex=0; P->SharedTarget=0;
  if (C->SharedCount) C->SharedCount--;
  return MgOk;
}
static MG_STATUS PadRead(MG_CORE *C,unsigned Pin,uint8_t *Value) {
  const MG_PAD *P=&MgPads[Pin]; uint8_t A=0,B=0,R=0,I=0; MG_STATUS S;
  S=FieldRead(C,&P->First,&A);
  if (S==MgOk) S=FieldRead(C,&P->Second,&B);
  if (S==MgOk && P->Type==4) S=FieldRead(C,&P->Third,&R);
  if (S==MgOk) S=FieldRead(C,&P->Input,&I);
  if (S==MgOk) *Value=(uint8_t)(A|(B<<1)|(R<<2)|(I<<3));
  return S;
}
static MG_STATUS PadPull(MG_CORE *C,unsigned Pin,unsigned A,unsigned B,unsigned R) {
  const MG_PAD *P=&MgPads[Pin]; MG_STATUS S;
  /* Break before make: never briefly enable opposite pulls together or
   * change PUPD direction while R0/R1 are still sourcing a pull. */
  if (P->Type==4) {
    S=FieldWrite(C,&P->Second,0);
    if (S==MgOk) S=FieldWrite(C,&P->Third,0);
    if (S==MgOk) S=FieldWrite(C,&P->First,A);
    if (S==MgOk && B) S=FieldWrite(C,&P->Second,1);
    if (S==MgOk && R) S=FieldWrite(C,&P->Third,1);
  } else {
    S=FieldWrite(C,&P->First,0);
    if (S==MgOk) S=FieldWrite(C,&P->Second,0);
    if (S==MgOk && A) S=FieldWrite(C,&P->First,1);
    if (S==MgOk && B) S=FieldWrite(C,&P->Second,1);
  }
  return S;
}
static MG_STATUS PadConfigure(MG_CORE *C,unsigned Pin,int Input,MG_PULL Pull) {
  const MG_PAD *P=&MgPads[Pin]; MG_STATUS S=MgOk; unsigned A,B,R;
  if (Input) S=FieldWrite(C,&P->Input,1);
  if (S!=MgOk || Pull==MgPullDefault) return S;
  A=Pull==MgPullUp; B=Pull==MgPullDown; R=0;
  if (P->Type==4) {
    A=Pull!=MgPullUp; B=(C->Pin[Pin].Pad>>1)&1U; R=(C->Pin[Pin].Pad>>2)&1U;
    /* Retain the inherited R1/R0 strength when it is enabled. If disabled,
     * R1:R0=01 is the stock driver's explicit basic enabled selection.
     * No resistance in ohms is claimed or RSEL strength changed. */
    if (Pull==MgPullNone) B=R=0;
    else if (!(B|R)) B=1;
  }
  return PadPull(C,Pin,A,B,R);
}
static MG_STATUS PadRestore(MG_CORE *C,unsigned Pin) {
  unsigned V=C->Pin[Pin].Pad;
  MG_STATUS S=PadPull(C,Pin,V&1U,(V>>1)&1U,(V>>2)&1U);
  if (S==MgOk) S=FieldWrite(C,&MgPads[Pin].Input,(V>>3)&1U);
  return S;
}
static MG_STATUS Save(MG_CORE *C,unsigned Pin) {
  uint32_t M,D,O; MG_STATUS S; MG_PIN *P=&C->Pin[Pin];
  if (P->Saved || P->Kind) return MgBusy;
  S=Read(C,GPIO,0x300U+(Pin/8U)*16U,&M); if (S!=MgOk) return S;
  S=Read(C,GPIO,(Pin/32U)*16U,&D); if (S!=MgOk) return S;
  S=Read(C,GPIO,0x100U+(Pin/32U)*16U,&O); if (S!=MgOk) return S;
  S=PadRead(C,Pin,&P->Pad); if (S!=MgOk) return S;
  P->Mode=(uint8_t)((M>>((Pin%8U)*4U))&7U);
  P->Direction=(uint8_t)((D>>(Pin%32U))&1U);
  P->Output=(uint8_t)((O>>(Pin%32U))&1U);
  P->Saved=1; P->ContextValid=0; P->SharedIndex=0; P->SharedTarget=0;
  return MgOk;
}
static MG_STATUS ContextFieldRead(MG_CORE *C,const MG_FIELD *F,uint8_t *Value) {
  if (!F->Width) { *Value=0; return MgOk; }
  return FieldRead(C,F,Value);
}
static MG_STATUS Snapshot(MG_CORE *C,unsigned Pin,MG_PIN *P) {
  const MG_PAD *Pad=&MgPads[Pin];
  uint32_t V,Bit=1U<<(Pin%32U); unsigned Bank=Pin/32U; MG_STATUS S;
  S=Read(C,GPIO,0x300U+(Pin/8U)*16U,&V); if (S!=MgOk) return S;
  P->ContextMode=(uint8_t)((V>>((Pin%8U)*4U))&7U);
  S=Read(C,GPIO,(Pin/32U)*16U,&V); if (S!=MgOk) return S;
  P->ContextDirection=(uint8_t)!!(V&Bit);
  S=Read(C,GPIO,0x100U+(Pin/32U)*16U,&V); if (S!=MgOk) return S;
  P->ContextOutput=(uint8_t)!!(V&Bit);
  S=PadRead(C,Pin,&P->ContextPad); if (S!=MgOk) return S;
  S=ContextFieldRead(C,&Pad->Schmitt,&P->ContextSchmitt); if (S!=MgOk) return S;
  S=ContextFieldRead(C,&Pad->Drive,&P->ContextDrive); if (S!=MgOk) return S;
  S=ContextFieldRead(C,&Pad->Rsel,&P->ContextRsel); if (S!=MgOk) return S;
  S=ContextFieldRead(C,&Pad->Eh,&P->ContextEh); if (S!=MgOk) return S;
  if (P->Kind==3) {
    S=Read(C,EINT,0x140U+Bank*4U,&V); if (S!=MgOk) return S;
    P->ContextSensitivity=(uint8_t)!!(V&Bit);
    S=Read(C,EINT,0x300U+Bank*4U,&V); if (S!=MgOk) return S;
    P->ContextPolarity=(uint8_t)!!(V&Bit);
    S=Read(C,EINT,MASK+Bank*4U,&V); if (S!=MgOk) return S;
    P->ContextUnmasked=(uint8_t)!(V&Bit);
  } else {
    P->ContextSensitivity=P->ContextPolarity=P->ContextUnmasked=0;
  }
  P->ContextValid=1; return MgOk;
}
static MG_STATUS Restore(MG_CORE *C,unsigned Pin) {
  MG_PIN *P=&C->Pin[Pin]; MG_STATUS S,T;
  if (!P->Saved) return MgOk;
  S=MgOk;
  if (P->IrqSaved) {
    uint32_t Bit=1U<<(Pin%32U); unsigned B=Pin/32U;
    S=BankWrite(C,P->Polarity?POL_SET:POL_CLR,B,Bit);
    T=BankWrite(C,P->Sensitivity?SENS_SET:SENS_CLR,B,Bit); if (S==MgOk) S=T;
  }
  T=Direction(C,Pin,0); if (T!=MgOk) return S==MgOk?T:S;
  /* Disconnect the DSP mux before releasing a possibly shared pad field. */
  if (P->Kind==GPS_LNA_KIND) {
    T=Mode(C,Pin,0); if (T!=MgOk) return S==MgOk?T:S;
  }
  T=PadRestore(C,Pin); if (T!=MgOk) return S==MgOk?T:S;
  T=Output(C,Pin,P->Output); if (S==MgOk) S=T;
  if (S==MgOk) S=SharedRelease(C,Pin,0);
  T=Mode(C,Pin,P->Mode); if (S==MgOk) S=T;
  /* Do not drive the pin after a failed data/mux restoration. */
  if (S==MgOk) S=Direction(C,Pin,P->Direction);
  if (S==MgOk) { P->Saved=0; P->Kind=0; P->IrqSaved=0; P->ContextValid=0; P->IoAlias=0; P->Pull=0; }
  return S;
}
MG_STATUS MgInitialize(MG_CORE *C,const MG_IO *Io) {
  unsigned I;
  if (!C || !Io || !Io->Read || !Io->Write) return MgInvalid;
  C->Io=*Io; C->Active=0; C->Suspended=0; C->SharedCount=0;
  for (I=0;I<MG_BANKS;I++) C->Owned[I]=0;
  for (I=0;I<MG_PINS;I++) {
    C->Pin[I].Mode=C->Pin[I].Direction=C->Pin[I].Output=C->Pin[I].Kind=0;
    C->Pin[I].Saved=C->Pin[I].IrqSaved=C->Pin[I].Sensitivity=C->Pin[I].Polarity=0;
    C->Pin[I].IoAlias=C->Pin[I].Pull=0;
    C->Pin[I].Pad=C->Pin[I].SharedIndex=C->Pin[I].SharedTarget=0;
    C->Pin[I].ContextValid=C->Pin[I].ContextMode=C->Pin[I].ContextDirection=0;
    C->Pin[I].ContextOutput=C->Pin[I].ContextPad=C->Pin[I].ContextSchmitt=0;
    C->Pin[I].ContextDrive=C->Pin[I].ContextRsel=C->Pin[I].ContextEh=0;
    C->Pin[I].ContextSensitivity=C->Pin[I].ContextPolarity=C->Pin[I].ContextUnmasked=0;
    C->Shared[I].Valid=C->Shared[I].Users=0;
  }
  PmicClearState(C);
  C->Signature=SIGNATURE; return MgOk;
}
MG_STATUS MgStart(MG_CORE *C) {
  unsigned B,I; uint32_t M,D; MG_STATUS S;
  if (!Valid(C)) return MgInvalid;
  if (C->Active || C->Suspended || C->SharedCount || C->PmicInterrupt.Saved || C->ConnPinsOwned)
    return MgBusy;
  for (I=0;I<MG_PINS;I++) if (C->Pin[I].Saved) return MgBusy;
  for (B=0;B<MG_BANKS;B++) if (C->Owned[B]) return MgBusy;
  /* All 160 EINT lines, including non-GPIO lines, must already be masked.
   * Do not take over an interrupt that firmware or a secure owner still uses. */
  for (B=0;B<5;B++) {
    S=Read(C,EINT,MASK+B*4,&M); if (S!=MgOk) return S;
    if (M!=UINT32_MAX) return MgNotReady;
    S=Read(C,EINT,DOMAIN+B*4,&D); if (S!=MgOk) return S;
    if ((D&IrqMask(B))!=IrqMask(B)) return MgNotReady;
  }
  /* EINT137 is a PMIC parent route, not a pad interrupt. Require its EINT
   * domain before publishing the GPIO controller as started. */
  S=Read(C,EINT,DOMAIN+MG_PMIC_EINT_BANK*4U,&D); if (S!=MgOk) return S;
  if (!(D&MG_PMIC_EINT_BIT)) return MgNotReady;
  C->Active=1; return MgOk;
}
MG_STATUS MgGpsLnaAcquire(MG_CORE *C,unsigned Pin) {
  uint8_t Schmitt; MG_STATUS S,T;
  if (!Ready(C)) return MgNotReady;
  if (Pin!=25 && Pin!=26) return MgInvalid;
  if (C->Pin[Pin].Saved || C->Pin[Pin].Kind) return MgBusy;
  S=FieldRead(C,&MgPads[Pin].Schmitt,&Schmitt); if (S!=MgOk) return S;
  S=SharedCanAcquire(C,Pin,Schmitt); if (S!=MgOk) return S;
  S=Save(C,Pin); if (S!=MgOk) return S;
  C->Pin[Pin].Kind=GPS_LNA_KIND;
  /* Keep the inherited Schmitt value while the DSP owns this pin. */
  S=SharedAcquire(C,Pin,Schmitt);
  if (S==MgBusy) {
    T=Restore(C,Pin); return T==MgOk?MgBusy:Fault(C,T);
  }
  return Fault(C,S);
}
MG_STATUS MgGpsLnaSet(MG_CORE *C,unsigned Pin,MG_GPS_LNA_STATE State) {
  MG_STATUS S;
  if (!Ready(C)) return MgNotReady;
  if ((Pin!=25 && Pin!=26) || (unsigned)State>MgGpsLnaOn) return MgInvalid;
  if (!C->Pin[Pin].Saved || C->Pin[Pin].Kind!=GPS_LNA_KIND) return MgInvalid;
  /* Preload the latch while GPIO is input; never drive an old GPIO value when
   * leaving DSP mode. All three registers are read back by the helpers. */
  S=Direction(C,Pin,0);
  if (State==MgGpsLnaDsp) {
    if (S==MgOk) S=Mode(C,Pin,2);
  } else {
    if (S==MgOk) S=Output(C,Pin,State==MgGpsLnaOn);
    if (S==MgOk) S=Mode(C,Pin,0);
    if (S==MgOk) S=Direction(C,Pin,1);
  }
  /* Retain the baseline/lease on error; Stop or Disconnect must recover it
   * before another owner or a new RF operation is allowed. */
  return Fault(C,S);
}
/* MtkConnBoot and MtkConnRails use these stock mux, drive and output fields.
 * Keep the GPIO aliases for DIR/DOUT, which also serve other pins at DIRQL. */
static const struct {
  unsigned Space, Offset;
  uint32_t Mask, Target, Alternate;
  int AtomicBit;
} ConnPinFields[MG_CONN_PIN_FIELDS] = {
  {GPIO,     0x380U, 0x000f0000U, 0,           0, 0},
  {GPIO,     0x020U, 0x00000010U, 0x10U,       0, 1},
  {GPIO,     0x120U, 0x00000010U, 0x10U,       0, 1},
  {GPIO,     0x460U, 0x77777770U, 0x11111110U, 0x11111110U, 0},
  {GPIO,     0x470U, 0x00000077U, 0x11U,       0x01U, 0},
  {IOCFG_RT, 0x010U, 0x3ffffff8U, 0x1209U<<3,  0x1209U<<3, 0},
  {GPIO,     0x050U, 0x02000000U, 0x02000000U, 0, 1},
  {GPIO,     0x150U, 0x02000000U, 0x02000000U, 0, 1}
};
static int IsConnPin(unsigned Pin) {
  return Pin==68U || (Pin>=177U && Pin<=185U);
}
int MgConnPinsOwns(const MG_CORE *C,unsigned Pin) {
  return C->ConnPinsOwned && IsConnPin(Pin);
}
MG_STATUS MgConnPinsAcquire(MG_CORE *C) {
  if (!Ready(C)) return MgNotReady;
  if (C->ConnPinsOwned) return MgBusy;
  for (unsigned Pin=68;Pin<=185;Pin++) {
    if (IsConnPin(Pin) && (C->Pin[Pin].Saved || C->Pin[Pin].Kind)) return MgBusy;
  }
  for (unsigned I=0;I<MG_CONN_PIN_FIELDS;I++) {
    uint32_t Live;
    MG_STATUS S=Read(C,ConnPinFields[I].Space,ConnPinFields[I].Offset,&Live);
    if (S!=MgOk) return S;
    C->ConnPinBaseline[I]=Live&ConnPinFields[I].Mask;
  }
  C->ConnPinsOwned=1;
  return MgOk;
}
MG_STATUS MgConnPinsRead(MG_CORE *C,unsigned Space,unsigned Offset,uint32_t *Value) {
  if (!Value) return MgInvalid;
  if (!Valid(C) || !C->ConnPinsOwned) return MgNotReady;
  for (unsigned I=0;I<MG_CONN_PIN_FIELDS;I++) {
    if (ConnPinFields[I].Space==Space && ConnPinFields[I].Offset==Offset) {
      uint32_t Live;
      MG_STATUS S=Read(C,Space,Offset,&Live);
      if (S==MgOk) *Value=Live&ConnPinFields[I].Mask;
      return S;
    }
  }
  return MgInvalid;
}
MG_STATUS MgConnPinsWrite(MG_CORE *C,unsigned Space,unsigned Offset,uint32_t Value) {
  if (!Valid(C) || !C->ConnPinsOwned) return MgNotReady;
  for (unsigned I=0;I<MG_CONN_PIN_FIELDS;I++) {
    unsigned Register=ConnPinFields[I].Offset;
    uint32_t Mask=ConnPinFields[I].Mask,Live;
    if (ConnPinFields[I].Space!=Space) continue;
    if (Offset!=Register) {
      if (!ConnPinFields[I].AtomicBit || (Offset!=Register+4U && Offset!=Register+8U))
        continue;
      if (Value!=Mask) return MgInvalid;
      Value=Offset==Register+4U?Mask:0;
    }
    if (Value!=C->ConnPinBaseline[I]) {
      if (Value!=ConnPinFields[I].Target && Value!=ConnPinFields[I].Alternate)
        return MgInvalid;
      if (!C->Active) return MgNotReady;
    }
    MG_STATUS S=Read(C,Space,Register,&Live);
    if (S!=MgOk || (Live&Mask)==Value) return S;
    if (ConnPinFields[I].AtomicBit)
      S=Write(C,Space,Register+(Value?4U:8U),Mask);
    else
      S=Write(C,Space,Register,(Live&~Mask)|Value);
    if (S==MgOk) S=Read(C,Space,Register,&Live);
    return S!=MgOk?S:(Live&Mask)==Value?MgOk:MgIoError;
  }
  return MgInvalid;
}
MG_STATUS MgConnPinsRelease(MG_CORE *C) {
  if (!Valid(C) || !C->ConnPinsOwned) return MgNotReady;
  for (unsigned I=0;I<MG_CONN_PIN_FIELDS;I++) {
    uint32_t Live;
    MG_STATUS S=Read(C,ConnPinFields[I].Space,ConnPinFields[I].Offset,&Live);
    if (S!=MgOk) return S;
    if ((Live&ConnPinFields[I].Mask)!=C->ConnPinBaseline[I]) return MgBusy;
  }
  C->ConnPinsOwned=0;
  return MgOk;
}
MG_STATUS MgConnect(MG_CORE *C,unsigned Pin,int Kind) {
  return MgConnectEx(C,Pin,Kind,MgPullDefault);
}
MG_STATUS MgConnectEx(MG_CORE *C,unsigned Pin,int Kind,MG_PULL Pull) {
  MG_STATUS S,T;
  if (!Ready(C)) return MgNotReady;
  if (Pin>=MG_PINS || Kind<1 || Kind>3 || (unsigned)Pull>MgPullNone) return MgInvalid;
  if (Kind==3 && Pin>=MG_IRQ_PINS) return MgUnsupported;
  if (MgConnPinsOwns(C,Pin)) return MgBusy;
  /* A read-only OpRegion may share an interrupt input. Keep one pad snapshot
   * and never reconfigure it when adding the second connection. */
  if (C->Pin[Pin].Saved || C->Pin[Pin].Kind) {
    MG_PIN *P=&C->Pin[Pin];
    if (P->Saved && P->Pull==(unsigned)Pull && !P->IoAlias &&
        ((Kind==1 && P->Kind==3 && (C->Owned[Pin/32U]&(1U<<(Pin%32U)))) ||
         (Kind==3 && P->Kind==1))) {
      P->IoAlias=1; P->Kind=3; return MgOk;
    }
    return MgBusy;
  }
  S=SharedCanAcquire(C,Pin,Kind==2?0U:1U); if (S!=MgOk) return S;
  S=Save(C,Pin); if (S!=MgOk) return S;
  /* Put the pad in GPIO input mode before changing a shared Schmitt field. */
  S=Direction(C,Pin,0);
  if (S==MgOk) S=Mode(C,Pin,0);
  if (S==MgOk) S=SharedAcquire(C,Pin,Kind==2?0U:1U);
  if (S==MgBusy) {
    T=Restore(C,Pin);
    return T==MgOk?MgBusy:Fault(C,T);
  }
  if (S==MgOk) S=PadConfigure(C,Pin,Kind!=2,Pull);
  if (S==MgOk && Kind==2) S=Direction(C,Pin,1);
  if (S==MgOk) { C->Pin[Pin].Kind=(uint8_t)Kind; C->Pin[Pin].Pull=(uint8_t)Pull; }
  return Fault(C,S);
}
MG_STATUS MgDisconnect(MG_CORE *C,unsigned Pin,int Preserve) {
  MG_STATUS S=MgOk,T; unsigned B; uint32_t Bit;
  if (!Valid(C) || Pin>=MG_PINS || (Preserve!=0 && Preserve!=1)) return MgInvalid;
  if (!C->Pin[Pin].Saved) return MgInvalid;
  if (Preserve && C->Pin[Pin].Kind==GPS_LNA_KIND) return MgUnsupported;
  B=Pin/32U; Bit=1U<<(Pin%32U);
  if (C->Owned[B]&Bit) {
    S=BankWrite(C,MASK_SET,B,Bit);
    T=BankWrite(C,ACK,B,Bit); if (S==MgOk) S=T;
    if (S!=MgOk) return Fault(C,S);
    C->Owned[B]&=~Bit;
  }
  if (Preserve) {
    S=SharedRelease(C,Pin,1);
    if (S!=MgOk) return Fault(C,S);
    C->Pin[Pin].Saved=0; C->Pin[Pin].Kind=0; C->Pin[Pin].IrqSaved=0; C->Pin[Pin].ContextValid=0;
    C->Pin[Pin].IoAlias=C->Pin[Pin].Pull=0;
    return MgOk;
  }
  return Fault(C,Restore(C,Pin));
}
MG_STATUS MgDisconnectInputOutput(MG_CORE *C,unsigned Pin,int Preserve) {
  if (!Valid(C) || Pin>=MG_PINS || (Preserve!=0 && Preserve!=1) || !C->Pin[Pin].Saved) return MgInvalid;
  if (C->Pin[Pin].Kind==3 && C->Pin[Pin].IoAlias) {
    C->Pin[Pin].IoAlias=0; return MgOk;
  }
  if (C->Pin[Pin].Kind!=1 && C->Pin[Pin].Kind!=2) return MgInvalid;
  return MgDisconnect(C,Pin,Preserve);
}
MG_STATUS MgDisableInterrupt(MG_CORE *C,unsigned Pin) {
  MG_PIN *P; MG_STATUS S; unsigned B; uint32_t Bit;
  if (!Valid(C)) return MgInvalid;
  if (Pin==MG_PMIC_IRQ_PIN) return C->PmicInterrupt.Saved?PmicDisable(C):MgInvalid;
  if (Pin>=MG_IRQ_PINS) return MgInvalid;
  P=&C->Pin[Pin]; if (!P->Saved || P->Kind!=3) return MgInvalid;
  if (!P->IoAlias) return MgDisconnect(C,Pin,0);
  B=Pin/32U; Bit=1U<<(Pin%32U);
  S=BankWrite(C,MASK_SET,B,Bit);
  if (S==MgOk) S=BankWrite(C,ACK,B,Bit);
  if (S==MgOk && P->IrqSaved) S=BankWrite(C,P->Polarity?POL_SET:POL_CLR,B,Bit);
  if (S==MgOk && P->IrqSaved) S=BankWrite(C,P->Sensitivity?SENS_SET:SENS_CLR,B,Bit);
  if (S!=MgOk) return Fault(C,S);
  C->Owned[B]&=~Bit; P->Kind=1; P->IoAlias=0; P->IrqSaved=0;
  return MgOk;
}
MG_STATUS MgConfigureInterrupt(MG_CORE *C,unsigned Pin,int Level,int High) {
  unsigned B; uint32_t Bit,M; MG_STATUS S;
  if (!Ready(C)) return MgNotReady;
  if (Pin==MG_PMIC_IRQ_PIN) {
    if ((Level!=0 && Level!=1) || (High!=0 && High!=1)) return MgInvalid;
    if (!C->PmicInterrupt.Saved) return MgInvalid;
    if (!Level || !High) return MgUnsupported;
    return Fault(C,PmicVerifyTrigger(C));
  }
  if (Pin>=MG_IRQ_PINS || (Level!=0 && Level!=1) || (High!=0 && High!=1)) return MgInvalid;
  B=Pin/32U; Bit=1U<<(Pin%32U);
  if (C->Pin[Pin].Kind!=3 || !(C->Owned[B]&Bit)) return MgInvalid;
  S=Read(C,EINT,MASK+B*4,&M); if (S!=MgOk) return Fault(C,S);
  S=BankWrite(C,MASK_SET,B,Bit);
  if (S==MgOk) S=BankWrite(C,High?POL_SET:POL_CLR,B,Bit);
  if (S==MgOk) S=BankWrite(C,Level?SENS_SET:SENS_CLR,B,Bit);
  if (S==MgOk) S=BankWrite(C,ACK,B,Bit);
  if (S==MgOk && !(M&Bit)) S=BankWrite(C,MASK_CLR,B,Bit);
  if (S!=MgOk) (void)BankWrite(C,MASK_SET,B,Bit);
  return Fault(C,S);
}
MG_STATUS MgEnable(MG_CORE *C,unsigned Pin,int Level,int High) {
  return MgEnableEx(C,Pin,Level,High,MgPullDefault);
}
MG_STATUS MgEnableEx(MG_CORE *C,unsigned Pin,int Level,int High,MG_PULL Pull) {
  MG_STATUS S; unsigned B; uint32_t Bit,Sensitivity,Polarity;
  if (!Ready(C)) return MgNotReady;
  if (Pin==MG_PMIC_IRQ_PIN) {
    if ((Level!=0 && Level!=1) || (High!=0 && High!=1)) return MgInvalid;
    if (!Level || !High) return MgUnsupported;
    return PmicEnable(C,Pull);
  }
  if (Pin>=MG_IRQ_PINS || (Level!=0 && Level!=1) || (High!=0 && High!=1)) return MgUnsupported;
  S=MgConnectEx(C,Pin,3,Pull); if (S!=MgOk) return S;
  B=Pin/32U; Bit=1U<<(Pin%32U);
  S=Read(C,EINT,0x140U+B*4,&Sensitivity); if (S!=MgOk) return Fault(C,S);
  S=Read(C,EINT,0x300U+B*4,&Polarity); if (S!=MgOk) return Fault(C,S);
  C->Pin[Pin].Sensitivity=(uint8_t)!!(Sensitivity&Bit);
  C->Pin[Pin].Polarity=(uint8_t)!!(Polarity&Bit); C->Pin[Pin].IrqSaved=1;
  C->Owned[B]|=Bit;
  S=BankWrite(C,MASK_SET,B,Bit);
  if (S==MgOk) S=MgConfigureInterrupt(C,Pin,Level,High);
  if (S==MgOk) S=BankWrite(C,MASK_CLR,B,Bit);
  if (S!=MgOk) (void)BankWrite(C,MASK_SET,B,Bit);
  return Fault(C,S);
}
MG_STATUS MgMask(MG_CORE *C,unsigned Bank,uint32_t Bits) {
  MG_STATUS S;
  if (!Ready(C)) return MgNotReady;
  if (Bank==MG_PMIC_IRQ_BANK) {
    if (Bits&~(1U<<MG_PMIC_IRQ_NUMBER)) return MgInvalid;
    if (!Bits) return MgOk;
    if (!C->PmicInterrupt.Saved) return MgInvalid;
    S=PmicVerifyTrigger(C); if (S!=MgOk) return Fault(C,S);
    return Fault(C,PmicSetMasked(C,1));
  }
  if (Bank>=MG_BANKS || (Bits&~C->Owned[Bank])) return MgInvalid;
  return Bits?Fault(C,BankWrite(C,MASK_SET,Bank,Bits)):MgOk;
}
MG_STATUS MgAck(MG_CORE *C,unsigned Bank,uint32_t Bits) {
  MG_STATUS S;
  if (!Ready(C)) return MgNotReady;
  if (Bank==MG_PMIC_IRQ_BANK) {
    if (Bits&~(1U<<MG_PMIC_IRQ_NUMBER)) return MgInvalid;
    if (!Bits) return MgOk;
    if (!C->PmicInterrupt.Saved) return MgInvalid;
    S=PmicVerifyTrigger(C); if (S!=MgOk) return Fault(C,S);
    return Fault(C,PmicAcknowledge(C));
  }
  if (Bank>=MG_BANKS || (Bits&~C->Owned[Bank])) return MgInvalid;
  return Bits?Fault(C,BankWrite(C,ACK,Bank,Bits)):MgOk;
}
MG_STATUS MgUnmask(MG_CORE *C,unsigned Pin) {
  unsigned B; uint32_t Bit;
  if (!Ready(C)) return MgNotReady;
  if (Pin==MG_PMIC_IRQ_PIN) {
    MG_STATUS S;
    if (!C->PmicInterrupt.Saved) return MgInvalid;
    S=PmicVerifyTrigger(C); if (S!=MgOk) return Fault(C,S);
    return Fault(C,PmicSetMasked(C,0));
  }
  if (Pin>=MG_IRQ_PINS) return MgInvalid;
  B=Pin/32U; Bit=1U<<(Pin%32U);
  if (!(C->Owned[B]&Bit) || C->Pin[Pin].Kind!=3) return MgInvalid;
  return Fault(C,BankWrite(C,MASK_CLR,B,Bit));
}
MG_STATUS MgQuery(MG_CORE *C,unsigned Bank,uint32_t Enabled,uint32_t *Active,uint32_t *Unmasked) {
  uint32_t M,S; MG_STATUS R;
  if (!Ready(C)) return MgNotReady;
  if (Bank>=MG_BANKS || !Active || !Unmasked || (Enabled&~MgValidMask(Bank))) return MgInvalid;
  if (Bank==MG_PMIC_IRQ_BANK) {
    uint32_t LogicalBit=1U<<MG_PMIC_IRQ_NUMBER;
    if (!C->PmicInterrupt.Saved) {
      *Active=0; *Unmasked=0; return MgOk;
    }
    R=PmicVerifyTrigger(C); if (R!=MgOk) return Fault(C,R);
    R=Read(C,EINT,MASK+MG_PMIC_EINT_BANK*4U,&M); if (R!=MgOk) return Fault(C,R);
    R=Read(C,EINT,MG_PMIC_EINT_BANK*4U,&S); if (R!=MgOk) return Fault(C,R);
    *Unmasked=(M&MG_PMIC_EINT_BIT)?0:LogicalBit;
    *Active=(Enabled&LogicalBit) && (S&MG_PMIC_EINT_BIT) && !(M&MG_PMIC_EINT_BIT)?LogicalBit:0;
    return MgOk;
  }
  if (!IrqMask(Bank)) { *Active=0; *Unmasked=0; return MgOk; }
  R=Read(C,EINT,MASK+Bank*4,&M); if (R!=MgOk) return Fault(C,R);
  R=Read(C,EINT,Bank*4,&S); if (R!=MgOk) return Fault(C,R);
  *Unmasked=(~M)&C->Owned[Bank]; *Active=S&(*Unmasked)&Enabled; return MgOk;
}
MG_STATUS MgRead(MG_CORE *C,unsigned Bank,int Latch,uint32_t *Value) {
  uint32_t D,O=0,Input=0,OutputMask=0; unsigned P; MG_STATUS S;
  if (!Ready(C)) return MgNotReady;
  if (Bank>=MG_BANKS || !Value || (Latch!=0 && Latch!=1)) return MgInvalid;
  for (P=Bank*32U;P<MG_PINS && P<(Bank+1)*32U;P++) {
    if (C->Pin[P].Kind==2) OutputMask|=1U<<(P%32U);
    else if (C->Pin[P].Kind==1 || C->Pin[P].Kind==3) Input|=1U<<(P%32U);
  }
  S=Read(C,GPIO,0x200U+Bank*16U,&D); if (S!=MgOk) return Fault(C,S);
  if (Latch && OutputMask) { S=Read(C,GPIO,0x100U+Bank*16U,&O); if (S!=MgOk) return Fault(C,S); }
  *Value=Latch?(D&Input)|(O&OutputMask):D&(Input|OutputMask); return MgOk;
}
MG_STATUS MgWrite(MG_CORE *C,unsigned Bank,uint32_t Set,uint32_t Clear) {
  unsigned P; uint32_t Allowed=0; MG_STATUS S=MgOk;
  if (!Ready(C)) return MgNotReady;
  if (Bank>=MG_BANKS || (Set&Clear) || ((Set|Clear)&~MgValidMask(Bank))) return MgInvalid;
  for (P=Bank*32U;P<MG_PINS && P<(Bank+1)*32U;P++) if (C->Pin[P].Kind==2) Allowed|=1U<<(P%32U);
  if ((Set|Clear)&~Allowed) return MgInvalid;
  if (Clear) S=Write(C,GPIO,0x108U+Bank*16U,Clear);
  if (S==MgOk && Set) S=Write(C,GPIO,0x104U+Bank*16U,Set);
  return Fault(C,S);
}
MG_STATUS MgStop(MG_CORE *C) {
  unsigned B,P; MG_STATUS S=MgOk,T;
  if (!Valid(C)) return MgInvalid;
  if (C->ConnPinsOwned) return MgBusy;
  if (C->Suspended) return MgNotReady;
  C->Active=0;
  if (C->PmicInterrupt.Saved) {
    T=PmicRestore(C); if (S==MgOk) S=T;
  }
  for (B=0;B<MG_BANKS;B++) if (C->Owned[B]) {
    T=BankWrite(C,MASK_SET,B,C->Owned[B]); if (S==MgOk) S=T;
    if (T==MgOk) { T=BankWrite(C,ACK,B,C->Owned[B]); if (S==MgOk) S=T; }
    if (T==MgOk) C->Owned[B]=0;
  }
  for (P=0;P<MG_PINS;P++) if (C->Pin[P].Saved) {
    /* Leave IRQ pins owned and unmodified if masking/ACK failed. */
    if (C->Owned[P/32U]&(1U<<(P%32U))) continue;
    T=Restore(C,P); if (S==MgOk) S=T;
  }
  if (S==MgOk) C->Suspended=0;
  return S;
}

static MG_STATUS RestoreContextPad(MG_CORE *C,unsigned Pin) {
  MG_PIN *P=&C->Pin[Pin]; const MG_PAD *Pad=&MgPads[Pin]; MG_STATUS S;
  S=PadPull(C,Pin,P->ContextPad&1U,(P->ContextPad>>1)&1U,(P->ContextPad>>2)&1U);
  if (S==MgOk) S=FieldWrite(C,&Pad->Input,(P->ContextPad>>3)&1U);
  if (S==MgOk && Pad->Drive.Width) S=FieldWriteGuarded(C,Pin,&Pad->Drive,P->ContextDrive);
  if (S==MgOk && Pad->Rsel.Width) S=FieldWriteGuarded(C,Pin,&Pad->Rsel,P->ContextRsel);
  if (S==MgOk && Pad->Eh.Width) S=FieldWriteGuarded(C,Pin,&Pad->Eh,P->ContextEh);
  return S;
}
static void ClearContext(MG_CORE *C) {
  unsigned P;
  for (P=0;P<MG_PINS;P++) C->Pin[P].ContextValid=0;
  C->PmicInterrupt.ContextValid=0;
}
static MG_STATUS SetOwnedMask(MG_CORE *C,unsigned Bank,uint32_t Unmasked) {
  uint32_t Owned=C->Owned[Bank],Value; MG_STATUS S=MgOk;
  if (!Owned) return MgOk;
  if (Owned&~Unmasked) S=BankWrite(C,MASK_SET,Bank,Owned&~Unmasked);
  if (S==MgOk && Unmasked) S=BankWrite(C,MASK_CLR,Bank,Unmasked);
  if (S==MgOk) S=Read(C,EINT,MASK+Bank*4U,&Value);
  return S!=MgOk?S:(Value&Owned)==(Owned&~Unmasked)?MgOk:MgIoError;
}
static MG_STATUS KeepOwnedMasked(MG_CORE *C) {
  unsigned B; MG_STATUS S=MgOk,T;
  for (B=0;B<MG_BANKS;B++) {
    T=SetOwnedMask(C,B,0); if (S==MgOk) S=T;
  }
  T=PmicKeepMasked(C); if (S==MgOk) S=T;
  return S;
}
static MG_STATUS RestoreInterruptMasks(MG_CORE *C) {
  unsigned B,P; MG_STATUS S=MgOk,T;
  for (B=0;B<MG_BANKS;B++) if (C->Owned[B]) {
    uint32_t Unmasked=0;
    for (P=B*32U;P<MG_IRQ_PINS && P<(B+1U)*32U;P++) if (C->Owned[B]&(1U<<(P%32U))) {
      if (!C->Pin[P].ContextValid || C->Pin[P].Kind!=3) return MgInvalid;
      if (C->Pin[P].ContextUnmasked) Unmasked|=1U<<(P%32U);
    }
    T=SetOwnedMask(C,B,Unmasked); if (S==MgOk) S=T;
  }
  if (S==MgOk) S=PmicRestoreContextMask(C);
  return S;
}
MG_STATUS MgSuspend(MG_CORE *C) { return MgSuspendWake(C,0); }
MG_STATUS MgSuspendWake(MG_CORE *C,const uint32_t WakeMask[MG_BANKS]) {
  unsigned P,B; uint32_t Wake[MG_BANKS]; MG_STATUS S;
  if (!Valid(C)) return MgInvalid;
  if (C->Suspended || C->ConnPinsOwned) return MgBusy;
  if (!C->Active) return MgNotReady;
  /* GPIO snapshots cannot hand off DSP/RF power ownership across suspend. */
  for (P=0;P<MG_PINS;P++) if (C->Pin[P].Kind==GPS_LNA_KIND) return MgUnsupported;
  for (B=0;B<MG_BANKS;B++) {
    Wake[B]=WakeMask?WakeMask[B]:0;
    if ((Wake[B]&~C->Owned[B]) || (Wake[B]&~IrqMask(B))) return MgInvalid;
  }
  ClearContext(C);
  for (P=0;P<MG_PINS;P++) if (C->Pin[P].Saved) {
    S=Snapshot(C,P,&C->Pin[P]);
    if (S!=MgOk) { ClearContext(C); return S; }
    if (C->Pin[P].SharedIndex && C->Pin[P].ContextSchmitt!=C->Pin[P].SharedTarget) {
      ClearContext(C); return MgBusy;
    }
  }
  S=PmicSnapshot(C);
  if (S!=MgOk) {
    ClearContext(C);
    return C->PmicInterrupt.Saved?Fault(C,S):S;
  }
  /* Match stock EINT suspend ordering for owned pins only. Do not ACK: a
   * cover edge pending while masked must still be delivered after resume. */
  for (B=0;B<MG_BANKS;B++) {
    S=SetOwnedMask(C,B,Wake[B]);
    if (S!=MgOk) {
      if (RestoreInterruptMasks(C)!=MgOk) { C->Active=0; C->Suspended=1; }
      else ClearContext(C);
      return S;
    }
  }
  S=PmicKeepMasked(C);
  if (S!=MgOk) {
    if (RestoreInterruptMasks(C)!=MgOk) { C->Active=0; C->Suspended=1; }
    else ClearContext(C);
    return S;
  }
  C->Active=0; C->Suspended=1; return MgOk;
}
/* Stock pinctrl-mtk-v2 resumes this retained domain by restoring the EINT
 * masks. Check the retained GPIO/IOCFG settings before unmasking; do not
 * reprogram pad direction or drive merely because the OS entered D3. */
MG_STATUS MgResumeRetained(MG_CORE *C) {
  MG_STATUS S;
  if (!Valid(C)) return MgInvalid;
  if (!C->Suspended || C->Active) return MgNotReady;
  for (unsigned Bank=0;Bank<5;Bank++) {
    uint32_t Mask,Domain;
    uint32_t Required=Bank==MG_PMIC_EINT_BANK?MG_PMIC_EINT_BIT:IrqMask(Bank);
    S=Read(C,EINT,DOMAIN+Bank*4U,&Domain); if (S!=MgOk) return S;
    S=Read(C,EINT,MASK+Bank*4U,&Mask); if (S!=MgOk) return S;
    if ((Domain&Required)!=Required || Mask!=UINT32_MAX) return MgNotReady;
  }
  for (unsigned Pin=0;Pin<MG_PINS;Pin++) {
    const MG_PIN *Saved=&C->Pin[Pin];
    if (!Saved->Saved) continue;
    MG_PIN Live={0};
    Live.Kind=Saved->Kind;
    S=Snapshot(C,Pin,&Live); if (S!=MgOk) return S;
    if (!Saved->ContextValid || Live.ContextMode!=Saved->ContextMode ||
        Live.ContextDirection!=Saved->ContextDirection || Live.ContextOutput!=Saved->ContextOutput ||
        Live.ContextPad!=Saved->ContextPad || Live.ContextSchmitt!=Saved->ContextSchmitt ||
        Live.ContextDrive!=Saved->ContextDrive || Live.ContextRsel!=Saved->ContextRsel ||
        Live.ContextEh!=Saved->ContextEh || Live.ContextPolarity!=Saved->ContextPolarity ||
        Live.ContextSensitivity!=Saved->ContextSensitivity || Live.ContextUnmasked)
      return MgNotReady;
  }
  if (C->PmicInterrupt.Saved) {
    const MG_VIRTUAL_IRQ *Saved=&C->PmicInterrupt;
    uint8_t Sensitivity,Polarity,Masked;
    S=PmicReadState(C,&Sensitivity,&Polarity,&Masked); if (S!=MgOk) return S;
    if (!Saved->ContextValid || Sensitivity!=Saved->ContextSensitivity ||
        Polarity!=Saved->ContextPolarity || !Masked)
      return MgNotReady;
  }
  S=RestoreInterruptMasks(C);
  if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
  C->Active=1; C->Suspended=0; ClearContext(C);
  return MgOk;
}
MG_STATUS MgResume(MG_CORE *C) {
  unsigned P,B,I; MG_STATUS S;
  if (!Valid(C)) return MgInvalid;
  if (!C->Suspended) return MgNotReady;
  S=KeepOwnedMasked(C); if (S!=MgOk) return S;
  for (P=0;P<MG_PINS;P++) if (C->Pin[P].Saved) {
    MG_PIN *Pin=&C->Pin[P];
    if (!Pin->ContextValid) return MgInvalid;
    S=Direction(C,P,0); if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
    S=RestoreContextPad(C,P); if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
  }
  for (I=0;I<MG_PINS;I++) if (C->Shared[I].Valid) {
    unsigned Owner;
    for (Owner=0;Owner<MG_PINS;Owner++)
      if (C->Pin[Owner].SharedIndex==I+1U) break;
    if (Owner==MG_PINS) { (void)KeepOwnedMasked(C); return MgInvalid; }
    S=FieldWriteGuarded(C,Owner,&C->Shared[I].Field,C->Shared[I].Target);
    if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
  }
  for (P=0;P<MG_PINS;P++) if (C->Pin[P].Saved) {
    MG_PIN *Pin=&C->Pin[P]; uint32_t Bit=1U<<(P%32U); B=P/32U;
    S=Output(C,P,Pin->ContextOutput); if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
    S=Mode(C,P,Pin->ContextMode); if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
    if (Pin->Kind==3) {
      S=BankWrite(C,Pin->ContextPolarity?POL_SET:POL_CLR,B,Bit);
      if (S==MgOk) S=BankWrite(C,Pin->ContextSensitivity?SENS_SET:SENS_CLR,B,Bit);
      if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
    }
    S=Direction(C,P,Pin->ContextDirection);
    if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
  }
  S=PmicRestoreContext(C);
  if (S==MgOk) S=RestoreInterruptMasks(C);
  if (S!=MgOk) { (void)KeepOwnedMasked(C); return S; }
  C->Active=1; C->Suspended=0; ClearContext(C);
  return MgOk;
}
