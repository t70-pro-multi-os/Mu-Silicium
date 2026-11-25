/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_GPIO_TEST_MMIO_H
#define MTK_GPIO_TEST_MMIO_H
#include "Hardware.h"
static uint32_t Registers[MG_SPACES][MG_MMIO_LENGTH/4];
static unsigned Operations,Writes,FailOperation,FailOperation2,DropPadWrite,DropEintWrite,DropGpioWrite,FirstEintWrite,GpioOperations;
static MG_STATUS FakeRead(void *Context,unsigned Space,unsigned Offset,uint32_t *Value) {
  (void)Context; CHECK(Space<MG_SPACES && !(Offset&3U) && Offset<MG_MMIO_LENGTH);
  if (++Operations==FailOperation || Operations==FailOperation2) return MgIoError;
  if (Space==0) GpioOperations++;
  *Value=Registers[Space][Offset/4U]; return MgOk;
}
static MG_STATUS FakeWrite(void *Context,unsigned Space,unsigned Offset,uint32_t Value) {
  unsigned Index,Bit;
  (void)Context; CHECK(Space<MG_SPACES && !(Offset&3U) && Offset<MG_MMIO_LENGTH);
  if (Space==1 && !FirstEintWrite) FirstEintWrite=Operations+1;
  if (++Operations==FailOperation || Operations==FailOperation2) return MgIoError;
  if (Space==0) GpioOperations++;
  Writes++;
  if (Space==1 && DropEintWrite) { DropEintWrite=0; return MgOk; }
  if (Space>=2 && DropPadWrite) { DropPadWrite=0; return MgOk; }
  if (!Space) {
    if (DropGpioWrite && !--DropGpioWrite) return MgOk;
    Index=Offset&~15U;
    if ((Offset&15U)==4U) Registers[0][Index/4U]|=Value;
    else if ((Offset&15U)==8U) Registers[0][Index/4U]&=~Value;
    else if ((Offset&15U)==12U && Offset>=0x300U && Offset<0x480U) {
      for (Bit=0;Bit<32;Bit+=4) if (Value&(8U<<Bit)) {
        Registers[0][Index/4U]=(Registers[0][Index/4U]&~(7U<<Bit))|(Value&(7U<<Bit));
      }
    } else if (Offset==0x380U || Offset==0x460U || Offset==0x470U) {
      uint32_t Mask=Offset==0x380U?0xf0000U:Offset==0x460U?0x77777770U:0x77U;
      CHECK(((Registers[0][Index/4U]^Value)&~Mask)==0);
      Registers[0][Index/4U]=Value;
    } else CHECK(0);
  } else if (Space>=2) {
    Index=Offset&~15U;
    if ((Offset&15U)==4U) Registers[Space][Index/4U]|=Value;
    else if ((Offset&15U)==8U) Registers[Space][Index/4U]&=~Value;
    else if (Space==7U && Offset==0x10U) {
      CHECK(((Registers[Space][Index/4U]^Value)&~0x3ffffff8U)==0);
      Registers[Space][Index/4U]=Value;
    }
    else CHECK(0);
  } else {
    Index=Offset&~0x1FU;
    if (Index==0x40U) Registers[1][(Offset-0x40U)/4U]&=~Value;
    else if (Index==0xC0U || Index==0x180U || Index==0x340U) Registers[1][(Offset-0x40U)/4U]|=Value;
    else if (Index==0x100U || Index==0x1C0U || Index==0x380U) Registers[1][(Offset-0x80U)/4U]&=~Value;
    else CHECK(0);
  }
  return MgOk;
}
static void ResetRegisters(void) {
  unsigned I,S;
  Operations=Writes=FailOperation=FailOperation2=DropPadWrite=DropEintWrite=DropGpioWrite=FirstEintWrite=GpioOperations=0;
  for (I=0;I<MG_MMIO_LENGTH/4;I++) { Registers[0][I]=0; Registers[1][I]=0; }
  for (S=2;S<MG_SPACES;S++) for (I=0;I<MG_MMIO_LENGTH/4;I++) Registers[S][I]=(I%4==0)?(0x659A5A96U^(S*0x112233U)):0;
  for (I=0;I<6;I++) {
    Registers[0][I*4]=0x55AA55AAU;
    Registers[0][0x100/4+I*4]=0xA55AA55AU;
    Registers[0][0x200/4+I*4]=0x96A596A5U;
  }
  for (I=0;I<24;I++) Registers[0][0x300/4+I*4]=0x76543210U;
  for (I=0;I<5;I++) { Registers[1][0x80/4+I]=UINT32_MAX; Registers[1][0x400/4+I]=UINT32_MAX;
    Registers[1][0x140/4+I]=0x55AA55AAU; Registers[1][0x300/4+I]=0xA55AA55AU; }
}
#endif
