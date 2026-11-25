// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Protocol/MtkPmicWrapper.h>
EFI_GUID gMediaTekPmicWrapperProtocolGuid, gMediaTekPmicProtocolGuid;
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/MT6358PmicDxe/MT6358PmicDxe.c"
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 Compare,UINT32 Exchange) {
  return __sync_val_compare_and_swap(P,Compare,Exchange);
}
static UINT16 Regs[0x10000/2];
static unsigned Reads, Writes, FailRead, FailWrite;
static unsigned InjectRead;
static EFI_STATUS NestedStatus;
static EFI_STATUS EFIAPI Read(UINT16 a, UINT16 *v) {
  Reads++; if (Reads==FailRead) return EFI_TIMEOUT; *v=Regs[a/2];
  if (InjectRead && Reads==InjectRead) {
    InjectRead=0;
    unsigned BeforeReads=Reads, BeforeWrites=Writes;
    NestedStatus=RegulatorSetEnable("vmch_eint_high",TRUE);
    CHECK(Reads==BeforeReads && Writes==BeforeWrites);
    FailWrite=Writes+1;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(UINT16 a, UINT16 v) {
  Writes++; Regs[a/2]=v; // Model a write that reached the PMIC before timing out.
  return Writes==FailWrite ? EFI_TIMEOUT : EFI_SUCCESS;
}
static MTK_PMIC_WRAPPER_PROTOCOL Wrapper={Read,Write};
static void Reset(void) { CHECK(mRegulatorBusy==0); memset(Regs,0,sizeof(Regs)); Reads=Writes=FailRead=FailWrite=0; mPmicWrapper=&Wrapper; InjectRead=0; NestedStatus=EFI_DEVICE_ERROR; }
int main(void) {
  UINT32 uv; BOOLEAN enabled;
  Reset(); InjectRead=2;
  CHECK(RegulatorSetEnable("vmch_eint_low",TRUE)==EFI_TIMEOUT);
  printf("nested=%llu final_eint=%x final_ldo=%x\n",(unsigned long long)NestedStatus,Regs[0x1d70/2],Regs[0x1cd8/2]);
  CHECK(NestedStatus==EFI_NOT_READY);
  CHECK(Regs[0x1d70/2]==0 && Regs[0x1cd8/2]==0);
  CHECK(mRegulatorBusy==0);
  FailWrite=0;
  CHECK(RegulatorSetEnable("vmch_eint_high",TRUE)==EFI_SUCCESS);
  CHECK((Regs[0x1d70/2] & 5)==5 && (Regs[0x1cd8/2] & 1)==1);
  Reset(); mRegulatorBusy=1; uv=0xdeadbeef; enabled=TRUE;
  CHECK(RegulatorSetEnable("ldo_vmch",TRUE)==EFI_NOT_READY);
  CHECK(RegulatorSetVoltage("ldo_vmch",3300000)==EFI_NOT_READY);
  CHECK(RegulatorIsEnabled("ldo_vmch",&enabled)==EFI_NOT_READY && enabled==TRUE);
  CHECK(RegulatorGetVoltage("ldo_vmch",&uv)==EFI_NOT_READY && uv==0xdeadbeef);
  CHECK(Reads==0 && Writes==0 && mRegulatorBusy==1);
  mRegulatorBusy=0;
  Reset(); Regs[0x1e48/2]=0xa8ff;
  CHECK(RegulatorSetVoltage("ldo_vmch",3300000)==EFI_SUCCESS);
  CHECK(Regs[0x1e48/2]==0xadff);
  CHECK(RegulatorGetVoltage("ldo_vmch",&uv)==EFI_SUCCESS && uv==3300000);
  CHECK(RegulatorSetVoltage("ldo_vmch",3100000)==EFI_INVALID_PARAMETER && Writes==1);
  // Stock VMC selects voltage with all twelve bits, including fine adjustment.
  Reset(); Regs[0x1e4c/2]=0xb406;
  CHECK(RegulatorGetVoltage("ldo_vmc",&uv)==EFI_SUCCESS && uv==1860000 && Writes==0);
  CHECK(RegulatorSetVoltage("ldo_vmc",3300000)==EFI_SUCCESS);
  CHECK(Regs[0x1e4c/2]==0xbd00);
  CHECK(RegulatorGetVoltage("ldo_vmc",&uv)==EFI_SUCCESS && uv==3300000);
  CHECK(RegulatorSetVoltage("ldo_vmc",1860000)==EFI_SUCCESS && Regs[0x1e4c/2]==0xb406);
  // Decode every wire selector; reject holes and preserve upper register bits.
  for (unsigned selector=0;selector<0x1000;selector++) {
    UINT32 expected=0;
    if (selector>=0x400 && selector<=0x40a) expected=1800000+(selector-0x400)*10000;
    if (selector>=0xa00 && selector<=0xa09) expected=2900000+(selector-0xa00)*10000;
    if (selector>=0xb00 && selector<=0xb0a) expected=3000000+(selector-0xb00)*10000;
    if (selector==0xd00) expected=3300000;
    Reset(); Regs[0x1e4c/2]=0x9000|selector; uv=0xdeadbeef;
    CHECK(RegulatorGetVoltage("ldo_vmc",&uv)==(expected ? EFI_SUCCESS : EFI_DEVICE_ERROR));
    CHECK(uv==(expected ? expected : 0xdeadbeef) && Writes==0);
    if (expected) {
      CHECK(RegulatorSetVoltage("ldo_vmc",expected)==EFI_SUCCESS);
      CHECK(Regs[0x1e4c/2]==(0x9000|selector));
    }
  }
  Reset(); Regs[0x1e4c/2]=0x4406;
  CHECK(RegulatorSetVoltage("ldo_vmc",3300001)==EFI_INVALID_PARAMETER && Reads==0 && Writes==0);
  CHECK(RegulatorSetVoltage("ldo_vmc",1900001)==EFI_INVALID_PARAMETER && Reads==0 && Writes==0);
  Reset(); Regs[0x1e4c/2]=0xa406; FailWrite=1;
  CHECK(RegulatorSetVoltage("ldo_vmc",3300000)==EFI_TIMEOUT && Regs[0x1e4c/2]==0xad00);
  Reset(); FailRead=1; uv=0xdeadbeef;
  CHECK(RegulatorGetVoltage("ldo_vmc",&uv)==EFI_TIMEOUT && uv==0xdeadbeef && Writes==0);

  CHECK(RegulatorSetVoltage("buck_vcore",499999)==EFI_INVALID_PARAMETER);
  CHECK(RegulatorSetVoltage("buck_vcore",500001)==EFI_INVALID_PARAMETER);
  CHECK(RegulatorSetVoltage("buck_vcore",1300000)==EFI_INVALID_PARAMETER);
  CHECK(RegulatorGetVoltage("ldo_vmc",NULL)==EFI_INVALID_PARAMETER);
  CHECK(RegulatorIsEnabled("ldo_vmc",NULL)==EFI_INVALID_PARAMETER);
  Reset(); FailRead=1;
  CHECK(RegulatorSetVoltage("ldo_vmc",3300000)==EFI_TIMEOUT && Writes==0);
  Reset(); FailWrite=1;
  CHECK(RegulatorSetVoltage("ldo_vmc",3300000)==EFI_TIMEOUT);
  Reset(); Regs[0x1d70/2]=0xa4; Regs[0x1cd8/2]=0xb0;
  CHECK(RegulatorSetEnable("vmch_eint_low",TRUE)==EFI_SUCCESS);
  CHECK(Regs[0x1d70/2]==0xa1 && Regs[0x1cd8/2]==0xb1);
  CHECK(RegulatorIsEnabled("vmch_eint_low",&enabled)==EFI_SUCCESS && enabled);
  CHECK(RegulatorSetEnable("vmch_eint_low",FALSE)==EFI_SUCCESS && TimeNs>=1500000);
  CHECK(Regs[0x1d70/2]==0xa0 && Regs[0x1cd8/2]==0xb0);
  for (unsigned n=1;n<=3;n++) {
    Reset(); Regs[0x1d70/2]=0xa4; Regs[0x1cd8/2]=0xb0; FailWrite=n;
    CHECK(RegulatorSetEnable("vmch_eint_low",TRUE)==EFI_TIMEOUT);
    CHECK(Regs[0x1d70/2]==0xa4 && Regs[0x1cd8/2]==0xb0);
  }
  for (unsigned n=1;n<=2;n++) {
    Reset(); FailRead=n;
    CHECK(RegulatorSetEnable("vmch_eint_low",TRUE)==EFI_TIMEOUT && Writes==0);
  }
  Reset(); Regs[0x1d70/2]=BIT0;
  CHECK(RegulatorIsEnabled("vmch_eint_low",&enabled)==EFI_DEVICE_ERROR && Writes==0);
  Reset(); Regs[0x1d70/2]=BIT0|BIT2; Regs[0x1cd8/2]=BIT0;
  CHECK(RegulatorIsEnabled("vmch_eint_low",&enabled)==EFI_ACCESS_DENIED && Writes==0);
  CHECK(RegulatorSetEnable("vmch_eint_low",TRUE)==EFI_ACCESS_DENIED && Writes==0);
  CHECK(RegulatorSetEnable("vmch_eint_low",FALSE)==EFI_ACCESS_DENIED && Writes==0);
  printf("PMIC: %u checks passed (voltage selection, failures, restoration)\n",Checks);
}
