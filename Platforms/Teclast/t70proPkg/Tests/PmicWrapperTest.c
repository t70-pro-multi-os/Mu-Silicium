// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Protocol/MtkPmicWrapper.h>
#include <Library/PmicWrapperImplLib.h>
EFI_GUID gMediaTekPmicWrapperProtocolGuid, gMediaTekPmicGaugeProtocolGuid;
EFI_GUID gMediaTekPmicBatteryAdcProtocolGuid;
static BOOLEAN GaugeSupported;
#define _PCD_VALUE_PcdMt6358GaugeSnapshot GaugeSupported
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/PmicWrapperDxe/PmicWrapperDxe.c"
#include "../../../../Silicon/MediaTek/MT6789Pkg/Library/PmicWrapperImplLib/PmicWrapperImplLib.c"
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 Compare,UINT32 Exchange) {
  return __sync_val_compare_and_swap(P,Compare,Exchange);
}
static UINT32 Fsm, Data=0x5678, Init=1;
static unsigned Commands, Clears, Writes, Reads, Mode;
static EFI_STATUS NestedStatus;
static int Nested;
static UINT16 GaugeCtrl, GaugeData, WriteData;
static unsigned GaugeWrites, GaugeStuck, GaugeNoLatch;
static UINT16 AdcRequest;
static unsigned AdcPolls, AdcReadyAt;
static UINT64 AdcStart;
static UINT16 EfuseClock, EfusePdn, EfuseEnable, EfuseAddress, EfuseTrigger;
static unsigned FuseWordsRead, FuseBusyPolls, FuseStuckBusy, FuseRestoreBroken;
static unsigned ChipAdcStuck, EfuseWrites;
static UINT64 FuseAddressTime, FuseTriggerTime;
static UINT16 BifControl, ThermistorRaw, BifRaw;
static unsigned BifWrites, BifSamples, BifStuck, BifRestoreBroken;

static void TemperatureWrite(UINT16 Address) {
  if (Address==AUXADC_BIF_CONTROL_REG) {
    CHECK((WriteData & ~AUXADC_BIF_DISABLE)==BIT9);
    BifWrites++;
    if (!BifRestoreBroken || !(WriteData & AUXADC_BIF_DISABLE)) BifControl=WriteData;
  } else {
    CHECK(Address==AUXADC_REQUEST_REG);
    CHECK(WriteData==BIT3 || WriteData==BIT11);
    if (WriteData==BIT11) {
      CHECK(!(BifControl & AUXADC_BIF_DISABLE) && GaugeWrites==3 && !GaugeCtrl);
      BifSamples++;
    }
    AdcRequest=WriteData;
    AdcStart=TimeNs;
  }
}

static UINT16 TemperatureRead(UINT16 Address) {
  if (Address==AUXADC_BIF_CONTROL_REG) return BifControl;
  CHECK(TimeNs>=AdcStart+80000);
  if (Address==AUXADC_BAT_TEMP_REG) {
    CHECK(AdcRequest==BIT3 && !GaugeCtrl);
    return AUXADC_READY | ThermistorRaw;
  }
  CHECK(Address==AUXADC_BIF_VOLTAGE_REG && AdcRequest==BIT11);
  return (BifStuck ? 0 : AUXADC_READY) | BifRaw;
}

static void EfuseWrite(UINT16 Address) {
  ++EfuseWrites;
  switch (Address) {
  case 0x012e: CHECK(WriteData==BIT2 && (EfusePdn & BIT2)); EfusePdn &= ~BIT2; break;
  case 0x010e: CHECK(WriteData==BIT4 && !(EfuseClock & BIT4)); EfuseClock |= BIT4; break;
  case 0x0110: CHECK(WriteData==BIT4 && !(EfusePdn & BIT2)); EfuseClock &= ~BIT4; break;
  case 0x012c: CHECK(WriteData==BIT2); EfusePdn |= BIT2; break;
  case OTP_READ_ENABLE:
    CHECK(!(EfusePdn & BIT2) && !(EfuseClock & BIT4) && WriteData<=1);
    if (!FuseRestoreBroken || WriteData==1) EfuseEnable=WriteData;
    break;
  case OTP_ROW:
    CHECK(EfuseEnable==1 && WriteData==0x50+2*FuseWordsRead && FuseWordsRead<4);
    EfuseAddress=WriteData;
    FuseAddressTime=TimeNs;
    break;
  case OTP_READ_TRIGGER:
    CHECK(EfuseAddress==0x50+2*FuseWordsRead && WriteData==(EfuseTrigger^1));
    CHECK(TimeNs>=FuseAddressTime+40000);
    EfuseTrigger=WriteData;
    FuseBusyPolls=0;
    FuseTriggerTime=TimeNs;
    break;
  case AUXADC_REQUEST_REG: {
    UINT16 V=0xa5a5;
    CHECK(WriteData==BIT0 || WriteData==BIT3 || WriteData==BIT4);
    CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_NOT_READY && V==0xa5a5);
    AdcRequest=WriteData;
    AdcStart=TimeNs;
    break;
  }
  default: CHECK(0);
  }
}

static UINT16 EfuseRead(UINT16 Address) {
  static const UINT16 DataWords[] = { 0x88cd, 32<<9, 15<<9, 0x9c00 };
  switch (Address) {
  case 0x010c: return EfuseClock;
  case 0x012a: return EfusePdn;
  case AUXADC_DIE_TEMP_TRIM: return 0x12f;
  case AUXADC_TEMP_REF_TRIM: return 1600;
  case AUXADC_TEMP_SLOPE_TRIM: return 14;
  case OTP_READ_ENABLE: return EfuseEnable;
  case OTP_READ_TRIGGER: return EfuseTrigger;
  case OTP_READ_BUSY:
    if (!FuseTriggerTime) return 0;
    CHECK(TimeNs>=FuseTriggerTime+50000);
    return FuseStuckBusy || (++FuseBusyPolls==1);
  case OTP_READ_DATA:
    CHECK(FuseWordsRead<ARRAY_SIZE(DataWords) && FuseBusyPolls>=2);
    CHECK(TimeNs>=FuseTriggerTime+93000);
    return DataWords[FuseWordsRead++];
  case AUXADC_BAT_VOLTAGE_REG: CHECK(AdcRequest==BIT0 && TimeNs>=AdcStart+1280000); return 0xe1c0;
  case AUXADC_CHIP_TEMP_REG: CHECK(AdcRequest==BIT4 && TimeNs>=AdcStart+80000); return ChipAdcStuck ? 0xdb5 : 0x8db5;
  case AUXADC_BAT_TEMP_REG: CHECK(AdcRequest==BIT3 && TimeNs>=AdcStart+80000); return 0x8a5b;
  default: CHECK(0); return 0;
  }
}

static EFI_STATUS EFIAPI InstallWrapper(EFI_HANDLE *Handle, ...) {
  VA_LIST Args;
  (VOID)Handle;
  VA_START(Args, Handle);
  CHECK(VA_ARG(Args, EFI_GUID *)==&gMediaTekPmicWrapperProtocolGuid);
  CHECK(VA_ARG(Args, MTK_PMIC_WRAPPER_PROTOCOL *)==&mPmicWrapper);
  EFI_GUID *Next=VA_ARG(Args, EFI_GUID *);
  if (GaugeSupported) {
    CHECK(Next==&gMediaTekPmicGaugeProtocolGuid);
    CHECK(VA_ARG(Args, MTK_PMIC_GAUGE_PROTOCOL *)==&mGauge);
    CHECK(VA_ARG(Args, EFI_GUID *)==&gMediaTekPmicBatteryAdcProtocolGuid);
    CHECK(VA_ARG(Args, MTK_PMIC_BATTERY_ADC_PROTOCOL *)==&mBatteryAdc);
    Next=VA_ARG(Args, EFI_GUID *);
  }
  CHECK(Next==NULL);
  VA_END(Args);
  Installs++;
  return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN a) {
  Reads++;
  CHECK(a>=0x100000 && a<0x101000);
  if (a==0x100000) return Init;
  if (a==0x1008a8) return Fsm<<1;
  if (a==0x100894) return Mode>=8 ? GaugeData : Data;
  return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN a, UINT32 v) {
  CHECK(a>=0x100000 && a<0x101000); Writes++;
  if (a==0x1008a4) {
    Clears++; if (Mode!=4) Fsm=0;
    if (Mode==7 && !Nested) { UINT16 V=0xaaaa; unsigned R=Reads,W=Writes; Nested=1; NestedStatus=mPmicWrapper.Read(0x28,&V); CHECK(V==0xaaaa && Reads==R && Writes==W); }
  }
  if (a==0x100884) WriteData=(UINT16)v;
  if (a==0x100880) {
    UINT16 Address=(UINT16)v;
    if (Mode==8 || (Mode==11 && (Address==FG_CTRL_REG || Address==FG_CURRENT_REG))) {
      CHECK(mWacsBusy==1);
      if (v & BIT29) {
        CHECK(Address==FG_CTRL_REG);
        CHECK(WriteData==1 || WriteData==8 || WriteData==0x8008 || WriteData==0);
        GaugeWrites++;
        GaugeCtrl=(UINT16)(WriteData & 9);
        if ((WriteData==1 && !GaugeNoLatch) || (WriteData & 8 && GaugeStuck)) GaugeCtrl|=0x8000;
      } else {
        GaugeData=Address==FG_CTRL_REG ? GaugeCtrl : (Address==FG_CURRENT_REG ? (Mode==11 ? 0 : 0x813a) : Data);
        if (Address==FG_CURRENT_REG) {
          UINT16 V=0xaaaa;
          CHECK(mPmicWrapper.Read(0x28,&V)==EFI_NOT_READY && V==0xaaaa);
          CHECK(mPmicWrapper.Write(0x28,1)==EFI_NOT_READY);
          CHECK(mGauge.ReadCurrent(&V)==EFI_NOT_READY && V==0xaaaa);
        }
      }
    }
    if (Mode==10) {
      CHECK(mWacsBusy==1);
      if (v & BIT29) EfuseWrite(Address);
      else GaugeData=EfuseRead(Address);
    }
    if (Mode==11 && Address!=FG_CTRL_REG && Address!=FG_CURRENT_REG) {
      CHECK(mWacsBusy==1);
      if (v & BIT29) TemperatureWrite(Address);
      else GaugeData=TemperatureRead(Address);
    }
    if (Mode==9) {
      CHECK(mWacsBusy==1);
      if (v & BIT29) {
        UINT16 Again=0xaaaa;
        CHECK(Address==AUXADC_REQUEST_REG && (WriteData==BIT0 || WriteData==BIT3));
        CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,&Again)==EFI_NOT_READY && Again==0xaaaa);
        CHECK(mPmicWrapper.Write(0x28,1)==EFI_NOT_READY);
        AdcRequest=WriteData;
        AdcPolls=0;
        AdcStart=TimeNs;
      } else {
        CHECK(Address==(AdcRequest==BIT0 ? AUXADC_BAT_VOLTAGE_REG : AUXADC_BAT_TEMP_REG));
        CHECK(TimeNs-AdcStart>=(AdcRequest==BIT0 ? 1280000U : 80000U));
        AdcPolls++;
        GaugeData=(AdcReadyAt && AdcPolls>=AdcReadyAt) ? 0xda5b : 0x5a5b;
      }
    }
    Commands++; Fsm=(v & BIT29) ? (Mode==3?2:0) : (Mode==2?2:6);
    if (Mode==5 && !Nested) { unsigned R=Reads,W=Writes; Nested=1; NestedStatus=mPmicWrapper.Write(0x28,1); CHECK(Reads==R && Writes==W); }
    if (Mode==6 && !Nested) { UINT16 V=0xaaaa; unsigned R=Reads,W=Writes; Nested=1; NestedStatus=mPmicWrapper.Read(0x28,&V); CHECK(V==0xaaaa && Reads==R && Writes==W); }
  }
  return v;
}
static void Reset(unsigned mode) { CHECK(mWacsBusy==0); Fsm=0; Mode=mode; Commands=Clears=Writes=Reads=0; TimeNs=0; Nested=0; NestedStatus=EFI_SUCCESS; }
static void ResetTrim(void) {
  Reset(10);
  EfuseClock=BIT4|BIT7;
  EfusePdn=BIT2|BIT6;
  EfuseEnable=0;
  EfuseAddress=0;
  EfuseTrigger=1;
  FuseWordsRead=FuseBusyPolls=FuseStuckBusy=FuseRestoreBroken=ChipAdcStuck=EfuseWrites=0;
  FuseAddressTime=FuseTriggerTime=0;
  mBatteryTrimRestoreFailed=FALSE;
  mBatteryVoltageTrimValid=FALSE;
  Init=1;
}

static void TestVoltageTrim(void) {
  UINT16 V=0xa5a5;

  ResetTrim();
  Init=0;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_NOT_READY && V==0xa5a5 && Commands==0);
  Init=1;
  CHECK(mPmicWrapper.Write(OTP_READ_TRIGGER,1)==EFI_ACCESS_DENIED && Commands==0);
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_SUCCESS && V==4211);
  CHECK(EfuseClock==(BIT4|BIT7) && EfusePdn==(BIT2|BIT6) && EfuseTrigger==1 && EfuseEnable==0);
  CHECK(FuseWordsRead==4 && !mWacsBusy && mBatteryVoltageTrimValid);
  unsigned Prior=Commands;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_SUCCESS && V==4211);
  CHECK(Commands==Prior+4 && FuseWordsRead==4);
  ChipAdcStuck=1; V=0xa5a5;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_TIMEOUT && V==0xa5a5 && !mWacsBusy);
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,&V)==EFI_SUCCESS && V==0x61c0);
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcTemperature,&V)==EFI_SUCCESS && V==0xa5b);

  ResetTrim();
  EfuseClock=BIT7; EfusePdn=BIT6;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_SUCCESS && V==4211);
  CHECK(EfuseClock==BIT7 && EfusePdn==BIT6 && EfuseWrites==12);

  ResetTrim();
  EfuseEnable=1; V=0xa5a5;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_NOT_READY && V==0xa5a5);
  CHECK(FuseWordsRead==0 && EfuseEnable==1 && EfuseClock==(BIT4|BIT7) && EfusePdn==(BIT2|BIT6));

  ResetTrim();
  FuseStuckBusy=1; V=0xa5a5;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_TIMEOUT && V==0xa5a5);
  CHECK(TimeNs>=30090000 && TimeNs<30120000 && FuseWordsRead==0 && EfuseEnable==0);
  CHECK(EfuseClock==(BIT4|BIT7) && EfusePdn==(BIT2|BIT6) && !mBatteryVoltageTrimValid && !mWacsBusy);

  ResetTrim();
  FuseRestoreBroken=1; V=0xa5a5;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_DEVICE_ERROR && V==0xa5a5);
  CHECK(EfuseEnable==1 && EfuseClock==(BIT4|BIT7) && EfusePdn==(BIT2|BIT6));
  CHECK(mBatteryTrimRestoreFailed && !mBatteryVoltageTrimValid && !mWacsBusy);
  Prior=EfuseWrites;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcCalibratedVoltage,&V)==EFI_DEVICE_ERROR && V==0xa5a5 && EfuseWrites==Prior);
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,&V)==EFI_SUCCESS && V==0x61c0);
}

int main(void) {
  UINT16 value=0xa5a5;
  Services.InstallMultipleProtocolInterfaces=InstallWrapper;
  Init=0; CHECK(InitPmicWrapper(NULL,NULL)==EFI_NOT_READY && Installs==0);
  Init=1; CHECK(InitPmicWrapper(NULL,NULL)==EFI_SUCCESS && Installs==1);
  Reset(0); CHECK(mPmicWrapper.Read(0x28,&value)==EFI_SUCCESS && value==0x5678 && Clears==1);
  Reset(0); Fsm=6; CHECK(mPmicWrapper.Read(0x28,&value)==EFI_SUCCESS && Clears==2);
  Reset(0); CHECK(mPmicWrapper.Read(0x29,&value)==EFI_INVALID_PARAMETER && Commands==0);
  CHECK(mPmicWrapper.Read(0x28,NULL)==EFI_INVALID_PARAMETER && Commands==0);
  Reset(1); Fsm=2; CHECK(mPmicWrapper.Read(0x28,&value)==EFI_TIMEOUT && Commands==0 && TimeNs==10000000);
  Reset(2); value=0xa5a5; CHECK(mPmicWrapper.Read(0x28,&value)==EFI_TIMEOUT && value==0xa5a5 && Commands==1);
  Reset(3); CHECK(mPmicWrapper.Write(0x28,1)==EFI_TIMEOUT && Commands==1);
  Reset(4); Fsm=6; CHECK(mPmicWrapper.Read(0x28,&value)==EFI_TIMEOUT && Clears==1 && Commands==0);
  Reset(0); CHECK(mPmicWrapper.Write(0x28,1)==EFI_SUCCESS);
  Reset(5); value=0xa5a5; EFI_STATUS OuterStatus=mPmicWrapper.Read(0x28,&value);
  printf("REENTRANT outer=%llu nested=%llu commands=%u value=%x\n",(unsigned long long)OuterStatus,(unsigned long long)NestedStatus,Commands,value);
  CHECK(OuterStatus==EFI_SUCCESS && value==0x5678 && NestedStatus==EFI_NOT_READY && Commands==1);
  Reset(6); CHECK(mPmicWrapper.Write(0x28,1)==EFI_SUCCESS && NestedStatus==EFI_NOT_READY && Commands==1 && mWacsBusy==0);
  Reset(7); value=0xa5a5; CHECK(mPmicWrapper.Read(0x28,&value)==EFI_SUCCESS && value==0x5678 && NestedStatus==EFI_NOT_READY && Commands==1 && Clears==1 && mWacsBusy==0);
  GaugeSupported=TRUE;
  Reset(8); CHECK(InitPmicWrapper(NULL,NULL)==EFI_SUCCESS && Installs==2);
  Init=0; value=0xa5a5;
  CHECK(mGauge.ReadCurrent(&value)==EFI_NOT_READY && value==0xa5a5 && Commands==0);
  Init=1;
  CHECK(mGauge.ReadCurrent(NULL)==EFI_INVALID_PARAMETER && Commands==0);
  CHECK(mPmicWrapper.Read(FG_CTRL_REG,&value)==EFI_ACCESS_DENIED && value==0xa5a5 && Commands==0);
  CHECK(mPmicWrapper.Write(FG_CTRL_REG,1)==EFI_ACCESS_DENIED && Commands==0);
  for (unsigned I=0;I<2;I++) {
    Reset(8); GaugeWrites=0; value=0xa5a5;
    CHECK(mGauge.ReadCurrent(&value)==EFI_SUCCESS && value==0x813a && GaugeWrites==3);
    CHECK(!GaugeCtrl && !mWacsBusy);
  }
  Reset(8); GaugeCtrl=0x10; GaugeWrites=0; value=0xa5a5;
  CHECK(mGauge.ReadCurrent(&value)==EFI_NOT_READY && value==0xa5a5 && !GaugeWrites && !mWacsBusy);
  Reset(8); GaugeCtrl=0; GaugeNoLatch=1; GaugeWrites=0;
  CHECK(mGauge.ReadCurrent(&value)==EFI_TIMEOUT && value==0xa5a5 && GaugeWrites==3);
  CHECK(!GaugeCtrl && !mWacsBusy);
  Reset(8); GaugeNoLatch=0; GaugeStuck=1;
  CHECK(mGauge.ReadCurrent(&value)==EFI_DEVICE_ERROR && value==0xa5a5 && !mWacsBusy);
  unsigned Previous=Commands;
  CHECK(mGauge.ReadCurrent(&value)==EFI_DEVICE_ERROR && value==0xa5a5 && Commands==Previous);
  CHECK(mPmicWrapper.Write(FG_CTRL_REG,0)==EFI_ACCESS_DENIED && Commands==Previous);
  CHECK(mPmicWrapper.Read(0x28,&value)==EFI_SUCCESS && value==Data);
  Reset(9); value=0xa5a5;
  CHECK(mBatteryAdc.Read((MTK_PMIC_BATTERY_ADC_CHANNEL)4,&value)==EFI_INVALID_PARAMETER);
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,NULL)==EFI_INVALID_PARAMETER);
  CHECK(value==0xa5a5 && Reads==0 && Writes==0);
  const UINT16 AdcRegisters[] = { AUXADC_REQUEST_REG, AUXADC_BAT_VOLTAGE_REG, AUXADC_BAT_TEMP_REG };
  for (unsigned I=0; I<ARRAY_SIZE(AdcRegisters); I++) {
    CHECK(mPmicWrapper.Read(AdcRegisters[I],&value)==EFI_ACCESS_DENIED && value==0xa5a5);
    CHECK(mPmicWrapper.Write(AdcRegisters[I],1)==EFI_ACCESS_DENIED);
  }
  Init=0;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,&value)==EFI_NOT_READY && value==0xa5a5 && Commands==0);
  Init=1;

  AdcReadyAt=3;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,&value)==EFI_SUCCESS && value==0x5a5b);
  CHECK(AdcRequest==BIT0 && AdcPolls==3 && Commands==4 && !mWacsBusy);
  Reset(9); AdcReadyAt=1;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcTemperature,&value)==EFI_SUCCESS && value==0x0a5b);
  CHECK(AdcRequest==BIT3 && AdcPolls==1 && Commands==2 && !mWacsBusy);

  Reset(9); AdcReadyAt=0; value=0xa5a5;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,&value)==EFI_TIMEOUT && value==0xa5a5 && !mWacsBusy);
  CHECK(TimeNs>=33280000 && TimeNs<33306000);
  AdcReadyAt=1;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcTemperature,&value)==EFI_SUCCESS && value==0x0a5b);
  Reset(3); value=0xa5a5;
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcVoltage,&value)==EFI_TIMEOUT && value==0xa5a5);
  CHECK(Commands==1 && TimeNs==1000000 && !mWacsBusy);
  Reset(2);
  CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcTemperature,&value)==EFI_TIMEOUT && value==0xa5a5);
  CHECK(Commands==2 && TimeNs==1080000 && !mWacsBusy);
  TestVoltageTrim();

  for (unsigned Case=0; Case<6; Case++) {
    Reset(11);
    mGaugeCore=(MTK_GAUGE_CORE){0};
    mBatteryBifRestoreFailed=FALSE;
    GaugeCtrl=GaugeWrites=GaugeStuck=0;
    GaugeNoLatch=Case==4;
    BifControl=BIT9 | (Case==1 ? 0 : AUXADC_BIF_DISABLE);
    BifWrites=BifSamples=0;
    ThermistorRaw=Case==5 ? 0 : 1024;
    BifRaw=3072;
    BifStuck=Case==2;
    BifRestoreBroken=Case==3;
    UINT16 Incoming=BifControl, Temperature=0xa5a5;
    CHECK(mPmicWrapper.Read(AUXADC_BIF_VOLTAGE_REG,&Temperature)==EFI_ACCESS_DENIED);
    CHECK(mPmicWrapper.Write(AUXADC_BIF_CONTROL_REG,0)==EFI_ACCESS_DENIED && Commands==0);
    EFI_STATUS Expected=Case<2 ? EFI_SUCCESS : (Case==2 || Case==4) ? EFI_TIMEOUT : EFI_DEVICE_ERROR;
    CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcTemperatureDeciK,&Temperature)==Expected);
    CHECK(Temperature==(Case<2 ? 2933 : 0xa5a5) && !mWacsBusy && !GaugeCtrl);
    CHECK(BifWrites==((Case==1 || Case==4) ? 0 : 2) && BifSamples==(Case!=4));
    CHECK(BifControl==(Case==3 ? BIT9 : Incoming));
    CHECK(mBatteryBifRestoreFailed==(Case==3));
    if (mBatteryBifRestoreFailed) {
      unsigned Prior=Commands;
      CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcTemperatureDeciK,&Temperature)==EFI_DEVICE_ERROR && Commands==Prior);
      CHECK(mBatteryAdc.Read(MtkPmicBatteryAdcTemperature,&Temperature)==EFI_SUCCESS && Temperature==1024);
    }
  }
  printf("PWRAP: %u checks passed (including bounded timeout paths)\n",Checks);
}
