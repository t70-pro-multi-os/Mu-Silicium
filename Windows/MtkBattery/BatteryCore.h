/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_WINDOWS_BATTERY_CORE_H
#define T70_WINDOWS_BATTERY_CORE_H

#include <stdint.h>

#define MTK_BATTERY_PROFILE_COUNT 5U
#define MTK_BATTERY_PROFILE_POINTS 100U
#define MTK_BATTERY_POWER_CHARGING 0x04U
#define MTK_BATTERY_POWER_DISCHARGING 0x02U
#define MTK_BATTERY_POWER_ONLINE 0x01U
#define MTK_BATTERY_POWER_CRITICAL 0x08U

typedef enum {
  MtkBatteryCoreSuccess = 0,
  MtkBatteryCoreInvalid,
  MtkBatteryCoreNotReady
} MTK_BATTERY_CORE_RESULT;

typedef struct {
  int32_t MahTenth;
  int32_t VoltageTenthMv;
  int32_t ResistanceMilliohm;
} MTK_BATTERY_PROFILE_POINT;

typedef enum {
  MtkBatteryAcUnknown,
  MtkBatteryAcOffline,
  MtkBatteryAcOnline
} MTK_BATTERY_AC_STATE;

typedef struct {
  uint16_t VoltageMv;
  int32_t CurrentStockUnits; /* T70 stock gauge unit: 0.1 mA. */
  int32_t TemperatureDeciC;
  uint64_t Time100ns;
  MTK_BATTERY_AC_STATE AcState;
} MTK_BATTERY_SAMPLE;

typedef struct {
  int32_t FullMahTenth;
  int32_t RemainingMahTenth;
  int32_t StateOfCharge10000;
  int32_t FullCapacityMwh;
  int32_t RemainingCapacityMwh;
  int32_t VoltageMv;
  int32_t CurrentMa;
  int32_t RateMw;
  int32_t TemperatureDeciC;
  uint32_t PowerState;
} MTK_BATTERY_STATE;

typedef struct {
  int32_t RemainingMahTenth;
  int32_t StateOfCharge10000;
  int64_t CoulombRemainder;
  uint64_t LastSampleTime100ns;
  uint8_t Initialized;
  uint8_t ReanchorOnNextSample;
  MTK_BATTERY_AC_STATE AcState;
} MTK_BATTERY_CORE;

void MtkBatteryCoreInitialize(MTK_BATTERY_CORE *Core);
void MtkBatteryCoreSuspend(MTK_BATTERY_CORE *Core);
MTK_BATTERY_CORE_RESULT MtkBatteryCoreUpdate(
  MTK_BATTERY_CORE *Core,
  const MTK_BATTERY_SAMPLE *Sample,
  MTK_BATTERY_STATE *State);
int32_t MtkBatteryCoreDesignCapacityMwh(void);
int MtkBatteryCoreNotificationSatisfied(
  const MTK_BATTERY_STATE *State,
  uint32_t PowerStateMask,
  uint32_t LowCapacityMwh,
  uint32_t HighCapacityMwh
  );

#endif
