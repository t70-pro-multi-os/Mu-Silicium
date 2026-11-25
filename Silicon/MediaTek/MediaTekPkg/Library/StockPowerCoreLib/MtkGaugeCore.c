// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkGaugeCore.h>
#define FG_CTRL 0x0d0aU
#define FG_CURRENT 0x0d8aU
#define FG_POLLS 1000U
static MTK_GAUGE_RESULT Wait(const MTK_GAUGE_IO *Io, int Set)
{
  unsigned N; uint16_t Value;
  for (N=0; N<FG_POLLS; N++) {
    if (!Io->Read16(Io->Context, FG_CTRL, &Value)) return MtkGaugeIoError;
    if (((Value & 0x8000U)!=0) == Set) return MtkGaugeOk;
  }
  return MtkGaugeTimeout;
}
static int Cleanup(const MTK_GAUGE_IO *Io)
{
  uint16_t Value;
  if (!Io->Read16(Io->Context, FG_CTRL, &Value) ||
      !Io->Write16(Io->Context, FG_CTRL, (uint16_t)((Value & ~9U)|8U))) return 0;
  if (Wait(Io, 0) != MtkGaugeOk) return 0;
  if (!Io->Read16(Io->Context, FG_CTRL, &Value) ||
      !Io->Write16(Io->Context, FG_CTRL, (uint16_t)(Value & ~8U))) return 0;
  return Io->Read16(Io->Context, FG_CTRL, &Value) && (Value & 0x8009U)==0;
}
static MTK_GAUGE_RESULT ReadCurrentSnapshot(const MTK_GAUGE_IO *Io,uint16_t *Sample)
{
  MTK_GAUGE_RESULT Result;
  uint16_t Value,Current=0;
  /* Require an idle zero control register before the stock full-register
   * latch command. Do not overwrite an inherited/foreign control state. */
  if (!Io->Read16(Io->Context,FG_CTRL,&Value)) return MtkGaugeIoError;
  if (Value!=0U) return MtkGaugeBusy;
  Result=Io->Write16(Io->Context,FG_CTRL,1U)?Wait(Io,1):MtkGaugeIoError;
  if (Result==MtkGaugeOk && !Io->Read16(Io->Context,FG_CURRENT,&Current))
    Result=MtkGaugeIoError;
  if (!Cleanup(Io)) return MtkGaugeQuarantined;
  if (Result==MtkGaugeOk) *Sample=Current;
  return Result;
}

MTK_GAUGE_RESULT MtkGaugeReadCurrentInLease(
  MTK_GAUGE_CORE *Core,const MTK_GAUGE_IO *Io,uint16_t *Raw)
{
  MTK_GAUGE_RESULT Result;
  uint16_t Sample=0;
  if (!Core || !Io || !Raw || !Io->Read16 || !Io->Write16)
    return MtkGaugeInvalid;
  if (Core->Quarantined || Core->Owned) return MtkGaugeQuarantined;
  Core->Owned=1;
  Result=ReadCurrentSnapshot(Io,&Sample);
  if (Result==MtkGaugeQuarantined) {
    Core->Quarantined=1;
    return Result;
  }
  Core->Owned=0;
  if (Result==MtkGaugeOk) *Raw=Sample;
  return Result;
}

MTK_GAUGE_RESULT MtkGaugeReadCurrent(
  MTK_GAUGE_CORE *Core,const MTK_GAUGE_IO *Io,uint16_t *Raw)
{
  MTK_GAUGE_RESULT Result;
  uint16_t Sample=0;
  if (!Core || !Io || !Raw || !Io->Acquire || !Io->Release ||
      !Io->Read16 || !Io->Write16)
    return MtkGaugeInvalid;
  if (Core->Quarantined || Core->Owned) return MtkGaugeQuarantined;
  if (!Io->Acquire(Io->Context)) return MtkGaugeBusy;
  Core->Owned=1;
  Result=ReadCurrentSnapshot(Io,&Sample);
  if (Result==MtkGaugeQuarantined) {
    Core->Quarantined=1;
    return Result;
  }
  if (!Io->Release(Io->Context)) {
    Core->Quarantined=1;
    return MtkGaugeQuarantined;
  }
  Core->Owned=0;
  if (Result==MtkGaugeOk) *Raw=Sample;
  return Result;
}

MTK_GAUGE_RESULT MtkGaugeConvertCurrent(uint16_t Raw, uint32_t Resistance, uint32_t Tune, int32_t *Out)
{
  int64_t Value;
  if (!Out || !Resistance || !Tune) return MtkGaugeInvalid;
  Value=(int64_t)(Raw<0x8000U ? Raw : (Raw^0xffffU))*381470/100000;
  /* Match truncation at each stock arithmetic step, rather than combining
   * fractions. Wider intermediates reject overflow instead of wrapping. */
  if (Resistance!=100) Value=Value*100/Resistance;
  if (Raw>=0x8000U) Value=-Value;
  Value=Value*Tune;
  if (Value>INT32_MAX || Value<INT32_MIN) return MtkGaugeInvalid;
  Value=Value/1000;
  if (Value>INT32_MAX || Value<INT32_MIN) return MtkGaugeInvalid;
  *Out=(int32_t)Value;
  return MtkGaugeOk;
}

MTK_GAUGE_RESULT MtkGaugeCalibrateVoltage(uint16_t BatteryAdc, uint16_t ChipAdc,
                                         const MTK_GAUGE_VOLTAGE_TRIM *Trim, uint16_t *Millivolts)
{
  if (!Trim || !Millivolts || BatteryAdc > 0x7fffU || ChipAdc > 0xfffU)
    return MtkGaugeInvalid;

  // mt635x-auxadc.ko: processed BATADC, then mt6358_batadc_cali().
  const unsigned FuseFlags = Trim->Fuse[0];
  const int FactoryTemperatureSet = (Trim->DieTemperature & 0x100U) != 0;
  const int32_t DieTemperature = Trim->DieTemperature & 0x3fU;
  const int32_t Reference = FactoryTemperatureSet ? Trim->TemperatureReference & 0xfffU : 1600;
  int32_t Slope = FactoryTemperatureSet ? Trim->TemperatureSlope & 0x3fU : 0;
  if ((Trim->TemperatureSlope & 0x100U) && FactoryTemperatureSet)
    Slope = -Slope;
  const int32_t CalibrationTemperature = !FactoryTemperatureSet ? 50 :
    ((DieTemperature < 38 || DieTemperature > 60) ? 53 : DieTemperature);
  const int32_t ChipMv = (ChipAdc * 1800U) >> 12;
  const int32_t ReferenceMv = (Reference * 1800) >> 12;
  const int32_t ChipTemperature = CalibrationTemperature * 5 -
    ((ChipMv - ReferenceMv) * 10000) / (1681 + Slope * 10);
  const int32_t Nominal = ((BatteryAdc * 5400U) >> 15) * 10;
  int32_t Corrected = Nominal;

  // Bit 2 selects programmed gain coefficients. Keep the PMIC defaults for
  // the gain stage when that bit is clear.
  const int GainProgrammed = (FuseFlags & 0x04U) != 0;
  if (!GainProgrammed || (FuseFlags & 0x40U)) {
    const int32_t Gain = GainProgrammed ? (FuseFlags >> 8) : 106;
    const int64_t Delta = (int64_t)(ChipTemperature - 250) * Nominal *
      (317220 + Gain * 11960) / INT64_C(255000000000);
    Corrected += GainProgrammed && (FuseFlags & 0x08U) ? -Delta : Delta;
  }

  if (FuseFlags & 0x80U) {
    const int32_t Cold = (int32_t)((Trim->Fuse[3] >> 10) & 7U) * 25 - 100;
    const int32_t Hot = (int32_t)(Trim->Fuse[3] >> 13) * 25 + 600;
    if (ChipTemperature < Cold) {
      const int64_t Delta = (int64_t)(ChipTemperature - Cold) * Corrected *
        (127 + (Trim->Fuse[1] >> 9) * 8) / 127000000;
      Corrected += (FuseFlags & 0x10U) ? -Delta : Delta;
    } else if (ChipTemperature > Hot) {
      const int64_t Delta = (int64_t)(ChipTemperature - Hot) * Corrected *
        (127 + (Trim->Fuse[2] >> 9) * 8) / 127000000;
      Corrected += (FuseFlags & 0x20U) ? Delta : -Delta;
    }
  }

  const int32_t Voltage = (Corrected + (Corrected > 0 ? 5 : -5)) / 10;
  if (Voltage < 0 || Voltage > UINT16_MAX)
    return MtkGaugeInvalid;
  *Millivolts = (uint16_t)Voltage;
  return MtkGaugeOk;
}

MTK_GAUGE_RESULT MtkGaugeBatteryTemperature(uint16_t ThermistorAdc, uint16_t BifAdc,
                                           uint16_t CurrentRaw, int32_t *DeciCelsius)
{
  /* mt6358_battery.ko fg_temp_table, -35 to 65 degrees in 5-degree steps. */
  static const uint32_t Ohms[] = {
    148171, 113347, 87559, 68237, 53650, 42506, 33892, 27219,
    22021, 17926, 14674, 12081, 10000, 8315, 6948, 5834, 4917,
    4161, 3535, 3014, 2586
  };
  int32_t Current, CorrectedMv, ThermistorMv, BifMv, Resistance, Cold;
  unsigned Index;

  if (!DeciCelsius || ThermistorAdc > 0x0fff || BifAdc > 0x0fff ||
      MtkGaugeConvertCurrent(CurrentRaw, 50, 1010, &Current) != MtkGaugeOk) {
    return MtkGaugeInvalid;
  }
  ThermistorMv = ThermistorAdc * 3600 / 4096;
  BifMv = BifAdc * 3600 / 4096;
  // Stock force_get_tbat_internal uses COM_R_FG_VALUE=0 and
  // COM_FG_METER_RESISTANCE=100. The correction applies to both inputs.
  CorrectedMv = ThermistorMv - (Current / 10) * 100 / 10000;
  if (ThermistorMv <= 0 || BifMv <= ThermistorMv || CorrectedMv <= 0) {
    return MtkGaugeInvalid;
  }
  // Stock default RBAT_PULL_UP_R is 24000 ohms.
  Resistance = 24000 * CorrectedMv / (BifMv - ThermistorMv);
  if (Resistance >= (int32_t)Ohms[0]) {
    *DeciCelsius = -350;
    return MtkGaugeOk;
  }
  for (Index = 1; Index < sizeof(Ohms) / sizeof(Ohms[0]); Index++) {
    int32_t HotOhms = (int32_t)Ohms[Index];
    int32_t ColdOhms = (int32_t)Ohms[Index - 1];
    if (Resistance >= HotOhms) {
      Cold = -350 + ((int32_t)Index - 1) * 50;
      *DeciCelsius = ((Resistance - HotOhms) * Cold +
                     (ColdOhms - Resistance) * (Cold + 50)) / (ColdOhms - HotOhms);
      return MtkGaugeOk;
    }
  }
  *DeciCelsius = 650;
  return MtkGaugeOk;
}
