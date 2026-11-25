/* SPDX-License-Identifier: GPL-2.0-only */
#include "BatteryCore.h"
#include <limits.h>
#include <stddef.h>

#include "StockProfile.h"

#define MTK_BATTERY_SOURCE_CURRENT_TO_MA 10
#define MTK_BATTERY_MAX_SAMPLE_GAP_100NS UINT64_C(3000000000)
#define MTK_BATTERY_CHARGE_CURRENT_THRESHOLD_MA 20
#define MTK_BATTERY_CRITICAL_SOC_10000 200
#define MTK_BATTERY_CRITICAL_VOLTAGE_MV 3350

static int32_t
Interpolate (
  int32_t X1,
  int32_t Y1,
  int32_t X2,
  int32_t Y2,
  int32_t X
  )
{
  int64_t Denominator;
  int64_t Value;

  Denominator = (int64_t)X2 - X1;
  if (Denominator == 0) {
    return Y2;
  }

  Value = (int64_t)(Y2 - Y1) * ((int64_t)X - X1);
  Value /= Denominator;
  Value += Y1;
  if (Value > INT32_MAX) {
    return INT32_MAX;
  }
  if (Value < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)Value;
}

static void
BuildTemperatureProfile (
  int32_t TemperatureDeciC,
  int32_t Mah[MTK_BATTERY_PROFILE_POINTS],
  int32_t VoltageTenthMv[MTK_BATTERY_PROFILE_POINTS]
  )
{
  int32_t TemperatureC;
  unsigned HighIndex;
  unsigned LowIndex;
  unsigned Index;

  TemperatureC = TemperatureDeciC / 10;
  if (TemperatureC >= T70BatteryProfileTemperatureC[0]) {
    HighIndex = 0;
    LowIndex = 0;
  } else if (TemperatureC <= T70BatteryProfileTemperatureC[MTK_BATTERY_PROFILE_COUNT - 1]) {
    HighIndex = MTK_BATTERY_PROFILE_COUNT - 1;
    LowIndex = HighIndex;
  } else {
    for (LowIndex = 1; LowIndex < MTK_BATTERY_PROFILE_COUNT; ++LowIndex) {
      if (TemperatureC >= T70BatteryProfileTemperatureC[LowIndex]) {
        break;
      }
    }
    HighIndex = LowIndex - 1;
  }

  for (Index = 0; Index < MTK_BATTERY_PROFILE_POINTS; ++Index) {
    if (HighIndex == LowIndex) {
      Mah[Index] = T70BatteryStockProfile[HighIndex][Index].MahTenth;
      VoltageTenthMv[Index] =
        T70BatteryStockProfile[HighIndex][Index].VoltageTenthMv;
    } else {
      int32_t HighTemperature = T70BatteryProfileTemperatureC[HighIndex];
      int32_t LowTemperature = T70BatteryProfileTemperatureC[LowIndex];
      Mah[Index] = Interpolate(
        HighTemperature,
        T70BatteryStockProfile[HighIndex][Index].MahTenth,
        LowTemperature,
        T70BatteryStockProfile[LowIndex][Index].MahTenth,
        TemperatureC);
      VoltageTenthMv[Index] = Interpolate(
        HighTemperature,
        T70BatteryStockProfile[HighIndex][Index].VoltageTenthMv,
        LowTemperature,
        T70BatteryStockProfile[LowIndex][Index].VoltageTenthMv,
        TemperatureC);
    }
  }
}

static int32_t
OcvToSoc (
  const int32_t Mah[MTK_BATTERY_PROFILE_POINTS],
  const int32_t VoltageTenthMv[MTK_BATTERY_PROFILE_POINTS],
  int32_t VoltageMv,
  int32_t *FullMahTenth
  )
{
  int32_t Voltage;
  int32_t DepthMahTenth;
  int32_t Percentage;
  unsigned Index;

  *FullMahTenth = Mah[MTK_BATTERY_PROFILE_POINTS - 1];
  if (*FullMahTenth <= 0) {
    return -1;
  }

  Voltage = VoltageMv * 10;
  for (Index = 0; Index < MTK_BATTERY_PROFILE_POINTS; ++Index) {
    if (VoltageTenthMv[Index] <= Voltage) {
      break;
    }
  }

  if (Index == 0) {
    DepthMahTenth = Mah[0];
  } else if (Index >= MTK_BATTERY_PROFILE_POINTS) {
    DepthMahTenth = Mah[MTK_BATTERY_PROFILE_POINTS - 1];
  } else {
    DepthMahTenth = Interpolate(
      VoltageTenthMv[Index - 1], Mah[Index - 1],
      VoltageTenthMv[Index], Mah[Index], Voltage);
  }

  Percentage = (int32_t)(((int64_t)DepthMahTenth * 10000) / *FullMahTenth);
  if (Percentage < 0) {
    Percentage = 0;
  } else if (Percentage > 10000) {
    Percentage = 10000;
  }
  return 10000 - Percentage;
}

static int32_t
NominalVoltageMv (void)
{
  return T70BatteryStockProfile[1][MTK_BATTERY_PROFILE_POINTS / 2].VoltageTenthMv / 10;
}

int32_t
MtkBatteryCoreDesignCapacityMwh (
  void
  )
{
  int32_t Mah = T70BatteryStockProfile[1][MTK_BATTERY_PROFILE_POINTS - 1].MahTenth / 10;
  int32_t Voltage = NominalVoltageMv();

  if (Mah <= 0 || Voltage <= 0) {
    return 0;
  }
  return (int32_t)(((int64_t)Mah * Voltage) / 1000);
}

void
MtkBatteryCoreInitialize (
  MTK_BATTERY_CORE *Core
  )
{
  if (Core == NULL) {
    return;
  }
  Core->RemainingMahTenth = 0;
  Core->StateOfCharge10000 = 0;
  Core->CoulombRemainder = 0;
  Core->LastSampleTime100ns = 0;
  Core->Initialized = 0;
  Core->ReanchorOnNextSample = 0;
}

void
MtkBatteryCoreSuspend (
  MTK_BATTERY_CORE *Core
  )
{
  if (Core == NULL) {
    return;
  }
  Core->LastSampleTime100ns = 0;
  Core->CoulombRemainder = 0;
  Core->ReanchorOnNextSample = 1;
}

MTK_BATTERY_CORE_RESULT
MtkBatteryCoreUpdate (
  MTK_BATTERY_CORE *Core,
  const MTK_BATTERY_SAMPLE *Sample,
  MTK_BATTERY_STATE *State
  )
{
  int32_t Mah[MTK_BATTERY_PROFILE_POINTS];
  int32_t VoltageTenthMv[MTK_BATTERY_PROFILE_POINTS];
  MTK_BATTERY_CORE NextCore;
  MTK_BATTERY_STATE NextState;
  int32_t FullMahTenth;
  int32_t OcvSoc;
  int32_t CurrentMa;
  int32_t NominalMv;
  uint64_t Elapsed100ns;
  int64_t ElapsedSeconds;
  int64_t DeltaMahTenth;

  if (Core == NULL || Sample == NULL || State == NULL) {
    return MtkBatteryCoreInvalid;
  }
  if (Sample->VoltageMv < 2500 || Sample->VoltageMv > 5000 ||
      Sample->TemperatureDeciC < -500 || Sample->TemperatureDeciC > 800 ||
      Sample->CurrentStockUnits < -100000 || Sample->CurrentStockUnits > 100000) {
    return MtkBatteryCoreInvalid;
  }
  if (Core->Initialized && Core->LastSampleTime100ns != 0 &&
      Sample->Time100ns != 0 && Sample->Time100ns < Core->LastSampleTime100ns) {
    return MtkBatteryCoreInvalid;
  }

  BuildTemperatureProfile(Sample->TemperatureDeciC, Mah, VoltageTenthMv);
  OcvSoc = OcvToSoc(Mah, VoltageTenthMv, Sample->VoltageMv, &FullMahTenth);
  if (OcvSoc < 0 || FullMahTenth <= 0) {
    return MtkBatteryCoreNotReady;
  }

  NextCore = *Core;
  if (!NextCore.Initialized || NextCore.ReanchorOnNextSample) {
    NextCore.StateOfCharge10000 = OcvSoc;
    NextCore.RemainingMahTenth =
      (int32_t)(((int64_t)FullMahTenth * OcvSoc) / 10000);
    NextCore.CoulombRemainder = 0;
    NextCore.ReanchorOnNextSample = 0;
  } else if (NextCore.LastSampleTime100ns != 0 && Sample->Time100ns != 0) {
    Elapsed100ns = Sample->Time100ns - NextCore.LastSampleTime100ns;
    if (Elapsed100ns > MTK_BATTERY_MAX_SAMPLE_GAP_100NS) {
      NextCore.StateOfCharge10000 = OcvSoc;
      NextCore.RemainingMahTenth =
        (int32_t)(((int64_t)FullMahTenth * OcvSoc) / 10000);
      NextCore.CoulombRemainder = 0;
    } else {
      ElapsedSeconds = (int64_t)(Elapsed100ns / UINT64_C(10000000));
      CurrentMa = Sample->CurrentStockUnits;
      DeltaMahTenth = (int64_t)CurrentMa * ElapsedSeconds +
        NextCore.CoulombRemainder;
      DeltaMahTenth /= 3600;
      NextCore.CoulombRemainder =
        (int64_t)CurrentMa * ElapsedSeconds +
        NextCore.CoulombRemainder - DeltaMahTenth * 3600;
      NextCore.RemainingMahTenth += (int32_t)DeltaMahTenth;
    }
  }

  if (NextCore.RemainingMahTenth < 0) {
    NextCore.RemainingMahTenth = 0;
    NextCore.CoulombRemainder = 0;
  } else if (NextCore.RemainingMahTenth > FullMahTenth) {
    NextCore.RemainingMahTenth = FullMahTenth;
    NextCore.CoulombRemainder = 0;
  }
  NextCore.StateOfCharge10000 =
    (int32_t)(((int64_t)NextCore.RemainingMahTenth * 10000) / FullMahTenth);
  NextCore.LastSampleTime100ns = Sample->Time100ns;
  NextCore.Initialized = 1;

  CurrentMa = Sample->CurrentStockUnits / MTK_BATTERY_SOURCE_CURRENT_TO_MA;
  NominalMv = NominalVoltageMv();
  if (NominalMv <= 0) {
    return MtkBatteryCoreNotReady;
  }

  NextState.FullMahTenth = FullMahTenth;
  NextState.RemainingMahTenth = NextCore.RemainingMahTenth;
  NextState.StateOfCharge10000 = NextCore.StateOfCharge10000;
  NextState.FullCapacityMwh =
    (int32_t)(((int64_t)(FullMahTenth / 10) * NominalMv) / 1000);
  NextState.RemainingCapacityMwh =
    (int32_t)(((int64_t)NextState.FullCapacityMwh *
      NextState.StateOfCharge10000) / 10000);
  NextState.VoltageMv = Sample->VoltageMv;
  NextState.CurrentMa = CurrentMa;
  NextState.RateMw =
    (int32_t)(((int64_t)CurrentMa * Sample->VoltageMv) / 1000);
  NextState.TemperatureDeciC = Sample->TemperatureDeciC;
  if (Sample->AcState == MtkBatteryAcOffline || Sample->AcState == MtkBatteryAcOnline) {
    NextCore.AcState = Sample->AcState;
  }
  NextState.PowerState = 0;
  if (CurrentMa >= MTK_BATTERY_CHARGE_CURRENT_THRESHOLD_MA) {
    NextState.PowerState |= MTK_BATTERY_POWER_CHARGING;
  } else if (CurrentMa <= -MTK_BATTERY_CHARGE_CURRENT_THRESHOLD_MA) {
    NextState.PowerState |= MTK_BATTERY_POWER_DISCHARGING;
  }
  if (NextCore.AcState == MtkBatteryAcOnline &&
      !(NextState.PowerState & MTK_BATTERY_POWER_DISCHARGING)) {
    NextState.PowerState |= MTK_BATTERY_POWER_ONLINE;
  }
  if (NextState.StateOfCharge10000 <= MTK_BATTERY_CRITICAL_SOC_10000 &&
      Sample->VoltageMv <= MTK_BATTERY_CRITICAL_VOLTAGE_MV) {
    NextState.PowerState |= MTK_BATTERY_POWER_CRITICAL;
  }

  *Core = NextCore;
  *State = NextState;
  return MtkBatteryCoreSuccess;
}

int
MtkBatteryCoreNotificationSatisfied (
  const MTK_BATTERY_STATE *State,
  uint32_t PowerStateMask,
  uint32_t LowCapacityMwh,
  uint32_t HighCapacityMwh
  )
{
  if (State == NULL) {
    return 0;
  }
  if (State->PowerState != PowerStateMask) {
    return 0;
  }
  if (LowCapacityMwh != 0 &&
      (uint32_t)State->RemainingCapacityMwh < LowCapacityMwh) {
    return 0;
  }
  if (HighCapacityMwh != 0 && HighCapacityMwh != UINT32_MAX &&
      (uint32_t)State->RemainingCapacityMwh > HighCapacityMwh) {
    return 0;
  }
  return 1;
}
