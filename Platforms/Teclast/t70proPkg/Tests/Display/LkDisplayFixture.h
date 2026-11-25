// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_LK_DISPLAY_FIXTURE_H
#define T70_LK_DISPLAY_FIXTURE_H
#include <Library/FdtLib.h>
#include <Library/MtkDisplayCore.h>

typedef struct {
  UINT32 Spm[0x400], Infra[0x400], Display[0x5000];
  UINTN Reads, DisplayReads, Writes;
  UINTN FailAddress, FailVisit, FailMatches;
  UINTN ChangeAddress, ChangeVisit, ChangeMatches;
  UINT32 ChangeValue;
  UINTN WriteMode;
} LK_DISPLAY_FIXTURE;

static UINT32 *LkFixtureRegister(LK_DISPLAY_FIXTURE *F, UINTN Address) {
  CHECK((Address & 3U)==0);
  if (Address>=0x10006000 && Address<0x10007000) return &F->Spm[(Address-0x10006000)/4];
  if (Address>=0x10001000 && Address<0x10002000) return &F->Infra[(Address-0x10001000)/4];
  CHECK(Address>=0x14000000 && Address<0x14014000);
  return &F->Display[(Address-0x14000000)/4];
}
static EFI_STATUS EFIAPI LkFixtureRead(VOID *Context, UINTN Address, UINT32 *Value) {
  LK_DISPLAY_FIXTURE *F=Context;
  F->Reads++; if (Address>=0x14000000) F->DisplayReads++;
  if (Address==F->FailAddress && ++F->FailMatches==F->FailVisit) return EFI_DEVICE_ERROR;
  if (Address==F->ChangeAddress && ++F->ChangeMatches==F->ChangeVisit)
    *LkFixtureRegister(F,Address)=F->ChangeValue;
  *Value=*LkFixtureRegister(F,Address); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LkFixtureWrite(VOID *Context, UINTN Address, UINT32 Value) {
  LK_DISPLAY_FIXTURE *F=Context;
  CHECK((Address>=0x14005044 && Address<=0x140050A4 && (Address-0x14005044)%0x20==0) ||
        (Address>=0x14006044 && Address<=0x14006064 && (Address-0x14006044)%0x20==0));
  F->Writes++;
  if (F->WriteMode==0 || F->WriteMode==3) *LkFixtureRegister(F,Address)=Value;
  return F->WriteMode==1 || F->WriteMode==3 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static void LkFixtureInitialize(LK_DISPLAY_FIXTURE *F, UINT32 Controller, UINT32 Layer,
                                BOOLEAN ColorPath, BOOLEAN Opaque) {
  memset(F,0,sizeof(*F));
  F->Spm[0x16C/4]=F->Spm[0x170/4]=0x200000; F->Spm[0x354/4]=0xD;
  *LkFixtureRegister(F,0x14007010)=1;
  *LkFixtureRegister(F,0x14013000)=1; *LkFixtureRegister(F,0x14013014)=1;
  *LkFixtureRegister(F,0x14013018)=0x3C; // four lanes, original LK TXRX field
  *LkFixtureRegister(F,0x1401301C)=0x30000|64U*3U;
  *LkFixtureRegister(F,0x1401302C)=32;
  *LkFixtureRegister(F,0x14013038)=(32U<<16)|64U;
  *LkFixtureRegister(F,0x14013020)=8; *LkFixtureRegister(F,0x14013024)=32;
  *LkFixtureRegister(F,0x14013028)=230;
  *LkFixtureRegister(F,0x14013050)=16; *LkFixtureRegister(F,0x14013054)=88;
  *LkFixtureRegister(F,0x14013058)=108;
  *LkFixtureRegister(F,Controller==0x14005000?0x14000F18:0x14000F14)=1;
  *LkFixtureRegister(F,0x14000F28)=Controller==0x14005000?0:2;
  *LkFixtureRegister(F,0x14000F0C)=ColorPath?1:0;
  *LkFixtureRegister(F,0x14000F30)=ColorPath?1:0;
  *LkFixtureRegister(F,0x14000F20)=ColorPath?1:0;
  *LkFixtureRegister(F,Controller+0xC)=0x400001;
  *LkFixtureRegister(F,Controller+0x20)=(32U<<16)|64U;
  *LkFixtureRegister(F,Controller+0x24)=0x5000001;
  *LkFixtureRegister(F,Controller+0x2C)=1U<<Layer;
  *LkFixtureRegister(F,Controller+0x30+Layer*0x20)=0x21FF;
  *LkFixtureRegister(F,Controller+0x38+Layer*0x20)=(32U<<16)|64U;
  *LkFixtureRegister(F,Controller+0x44+Layer*0x20)=260|(Opaque?0x10000000U:0);
  *LkFixtureRegister(F,Controller+0xC0+Layer*0x20)=1;
  *LkFixtureRegister(F,Controller+0xF40+Layer*0x20)=0x90000000;
  *LkFixtureRegister(F,0x14007014)=64; *LkFixtureRegister(F,0x14007018)=32;
}
static void LkFixtureCell(VOID *Fdt, INT32 Node, CONST CHAR8 *Name, UINT32 Value) {
  UINT8 B[4]={(UINT8)(Value>>24),(UINT8)(Value>>16),(UINT8)(Value>>8),(UINT8)Value};
  CHECK(FdtSetProp(Fdt,Node,Name,B,4)==0);
}
static void LkFixtureReg(VOID *Fdt, INT32 Node, UINT32 Base, UINT32 Size) {
  UINT8 B[16]={0,0,0,0,(UINT8)(Base>>24),(UINT8)(Base>>16),(UINT8)(Base>>8),(UINT8)Base,
    0,0,0,0,(UINT8)(Size>>24),(UINT8)(Size>>16),(UINT8)(Size>>8),(UINT8)Size};
  CHECK(FdtSetProp(Fdt,Node,"reg",B,sizeof(B))==0);
}
static void LkFixtureTree(VOID *Fdt, UINTN Capacity) {
  static const struct { CONST CHAR8 *Name,*Compatible; UINT32 Base; } Nodes[]={
    {"power-controller@10006000","mediatek,mt6789-scpsys",0x10006000},
    {"infracfg@10001000","mediatek,mt6789-infracfg_ao",0x10001000},
    {"mmsys@14000000","mediatek,mt6789-mmsys",0x14000000},
    {"larb0@14003000","mediatek,smi_larb0",0x14003000},
    {"larb1@14004000","mediatek,smi_larb1",0x14004000},
    {"ovl0@14005000","mediatek,disp_ovl0",0x14005000},
    {"ovl2l@14006000","mediatek,disp_ovl0_2l",0x14006000},
    {"rdma0@14007000","mediatek,disp_rdma0",0x14007000},
    {"dsi0@14013000","mediatek,dsi0",0x14013000}
  };
  CHECK(FdtCreateEmptyTree(Fdt,(INT32)Capacity)==0);
  LkFixtureCell(Fdt,0,"#address-cells",2); LkFixtureCell(Fdt,0,"#size-cells",2);
  for (UINTN I=0;I<ARRAY_SIZE(Nodes);I++) {
    INT32 N=FdtAddSubnode(Fdt,0,Nodes[I].Name); CHECK(N>=0);
    CHECK(FdtSetPropString(Fdt,N,"compatible",Nodes[I].Compatible)==0);
    LkFixtureReg(Fdt,N,Nodes[I].Base,0x1000);
  }
  INT32 N=FdtAddSubnode(Fdt,0,"memory@80000000"); CHECK(N>=0);
  CHECK(FdtSetPropString(Fdt,N,"device_type","memory")==0); LkFixtureReg(Fdt,N,0x80000000,0x20000000);
  N=FdtAddSubnode(Fdt,0,"chosen"); CHECK(N>=0);
  LkFixtureCell(Fdt,N,"atag,videolfb-fb_base_h",0);
  LkFixtureCell(Fdt,N,"atag,videolfb-fb_base_l",0x90000000);
  LkFixtureCell(Fdt,N,"atag,videolfb-vramSize",0x20000);
  LkFixtureCell(Fdt,N,"atag,videolfb-islcmfound",1);
  LkFixtureCell(Fdt,N,"atag,videolfb-islcm_inited",0); // exact stock DT, not updated by LK
  N=FdtAddSubnode(Fdt,0,"reserved-memory"); CHECK(N>=0);
  LkFixtureCell(Fdt,N,"#address-cells",2); LkFixtureCell(Fdt,N,"#size-cells",2);
  CHECK(FdtSetPropEmpty(Fdt,N,"ranges")==0);
  N=FdtAddSubnode(Fdt,0,"modem"); CHECK(N>=0);
  UINT8 Info[16]={0,0x10,0,0x80,0,0,0,0,0,0x10,0,0,2,0,0,0};
  CHECK(FdtSetProp(Fdt,N,"ccci,modem_info",Info,sizeof(Info))==0);
}
// Actual fixed register snapshot from RAM trial 18; power/DSI/RAM are models.
// Candidate b23ee713e6ab8f88fdcaa7be5f0521140f2f97531cf4a6a6115c6ab61284b08b.
static void LkFixturePhysical18(LK_DISPLAY_FIXTURE *F) {
  static const UINT32 Offsets[]={0x0C,0x20,0x24,0x2C,0x2D0,0x324,
                                0x50,0x58,0x5C,0x60,0x64,0x6C,0xE0,0xF60};
  static const UINT32 Values[]={0x400001,0x057808C0,0x07000005,2,0,0,
                               0x21FF,0x057808C0,0,0x100000,0x2300,0,0x5A0001,0xFE648000};
  LkFixtureInitialize(F,0x14005000,1,FALSE,FALSE);
  for(UINTN I=0;I<ARRAY_SIZE(Offsets);I++) *LkFixtureRegister(F,0x14005000+Offsets[I])=Values[I];
  *LkFixtureRegister(F,0x14007014)=2240;*LkFixtureRegister(F,0x14007018)=1400;
  *LkFixtureRegister(F,0x1401301C)=0x30000|2240U*3;
  *LkFixtureRegister(F,0x1401302C)=1400;*LkFixtureRegister(F,0x14013038)=0x057808C0;
}
static void LkFixturePhysical18Tree(VOID *Fdt,UINTN Capacity) {
  LkFixtureTree(Fdt,Capacity);
  LkFixtureReg(Fdt,FdtPathOffset(Fdt,"/memory@80000000"),0x80000000,0x80000000);
  // Deliberately modeled RAM/video bounds enclosing the observed scanout.
  INT32 Chosen=FdtPathOffset(Fdt,"/chosen");CHECK(Chosen>=0);
  LkFixtureCell(Fdt,Chosen,"atag,videolfb-fb_base_l",0xFE400000);
  LkFixtureCell(Fdt,Chosen,"atag,videolfb-vramSize",0x1C00000);
}
#endif
