/* SPDX-License-Identifier: GPL-2.0-only */
#include "BatteryCore.h"
#include "StockProfile.h"
#include <assert.h>
#include <stdio.h>

static unsigned Checks;

#define CHECK(Condition) do { \
  ++Checks; \
  if (!(Condition)) { \
    fprintf(stderr, "battery core check failed at line %d: %s\n", __LINE__, #Condition); \
    return __LINE__; \
  } \
} while (0)

static MTK_BATTERY_SAMPLE
Sample (
  uint16_t VoltageMv,
  int32_t CurrentStockUnits,
  int32_t TemperatureDeciC,
  uint64_t Time100ns
  )
{
  MTK_BATTERY_SAMPLE Value;
  Value.VoltageMv = VoltageMv;
  Value.CurrentStockUnits = CurrentStockUnits;
  Value.TemperatureDeciC = TemperatureDeciC;
  Value.Time100ns = Time100ns;
  Value.AcState = MtkBatteryAcUnknown;
  return Value;
}

int
main (
  void
  )
{
  MTK_BATTERY_CORE Core;
  MTK_BATTERY_STATE State;
  MTK_BATTERY_STATE BeforeState;
  MTK_BATTERY_CORE BeforeCore;
  MTK_BATTERY_SAMPLE Input;
  int32_t PreviousSoc;
  int Voltage;

  CHECK(sizeof(T70BatteryStockProfile) / sizeof(T70BatteryStockProfile[0]) == 5U);
  CHECK(sizeof(T70BatteryStockProfile[0]) / sizeof(T70BatteryStockProfile[0][0]) == 100U);
  CHECK(T70_BATTERY_PROFILE_SOURCE_SHA256[0] != '\0');
  CHECK(MtkBatteryCoreDesignCapacityMwh() > 5000);
  CHECK(MtkBatteryCoreDesignCapacityMwh() < 20000);

  MtkBatteryCoreInitialize(&Core);
  Input = Sample(4350, 0, 250, 100000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.StateOfCharge10000 == 10000);
  CHECK(State.RemainingCapacityMwh == State.FullCapacityMwh);

  MtkBatteryCoreInitialize(&Core);
  Input = Sample(2700, 0, 250, 100000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.StateOfCharge10000 == 0);
  CHECK(State.RemainingCapacityMwh == 0);

  PreviousSoc = -1;
  for (Voltage = 2900; Voltage <= 4350; Voltage += 10) {
    MtkBatteryCoreInitialize(&Core);
    Input = Sample((uint16_t)Voltage, 0, 250, 100000000U);
    CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
    CHECK(State.StateOfCharge10000 >= PreviousSoc);
    CHECK(State.StateOfCharge10000 >= 0 && State.StateOfCharge10000 <= 10000);
    PreviousSoc = State.StateOfCharge10000;
  }

  MtkBatteryCoreInitialize(&Core);
  Input = Sample(3800, 0, 250, 100000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  BeforeState = State;
  Input = Sample(3800, 36000, 250, 700000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.RemainingMahTenth == BeforeState.RemainingMahTenth + 600);
  CHECK(State.CurrentMa == 3600);
  CHECK(State.RateMw > 0);
  CHECK((State.PowerState & MTK_BATTERY_POWER_CHARGING) != 0);
  BeforeState = State;
  Input = Sample(3800, -36000, 250, 1300000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.RemainingMahTenth == BeforeState.RemainingMahTenth - 600);
  CHECK(State.RateMw < 0);
  CHECK((State.PowerState & MTK_BATTERY_POWER_DISCHARGING) != 0);

  MtkBatteryCoreInitialize(&Core);
  Input = Sample(3900, 10000, 250, 100000000U);
  Input.AcState = MtkBatteryAcOnline;
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.CurrentMa == 1000);
  CHECK((State.PowerState & (MTK_BATTERY_POWER_CHARGING |
        MTK_BATTERY_POWER_ONLINE)) ==
        (MTK_BATTERY_POWER_CHARGING | MTK_BATTERY_POWER_ONLINE));
  CHECK(!MtkBatteryCoreNotificationSatisfied(&State,
        MTK_BATTERY_POWER_CHARGING, 0, UINT32_MAX));
  CHECK(MtkBatteryCoreNotificationSatisfied(&State,
        MTK_BATTERY_POWER_CHARGING | MTK_BATTERY_POWER_ONLINE, 0, UINT32_MAX));
  CHECK(!MtkBatteryCoreNotificationSatisfied(&State,
        MTK_BATTERY_POWER_DISCHARGING, 0,
        UINT32_MAX));
  CHECK(!MtkBatteryCoreNotificationSatisfied(&State,
        MTK_BATTERY_POWER_CHARGING | MTK_BATTERY_POWER_DISCHARGING,
        0, UINT32_MAX));
  CHECK(!MtkBatteryCoreNotificationSatisfied(&State, 0, 0, UINT32_MAX));
  CHECK(!MtkBatteryCoreNotificationSatisfied(&State,
        0, (uint32_t)State.RemainingCapacityMwh + 1U,
        UINT32_MAX));
  Input = Sample(4200, 0, 250, Input.Time100ns + 10000000U);
  Input.AcState = MtkBatteryAcOnline;
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.PowerState == MTK_BATTERY_POWER_ONLINE);
  Input.AcState = MtkBatteryAcUnknown;
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.PowerState == MTK_BATTERY_POWER_ONLINE);
  Input.AcState = MtkBatteryAcOffline;
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.PowerState == 0);
  Input.AcState = MtkBatteryAcOnline;
  Input.CurrentStockUnits = -1000;
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(State.PowerState == MTK_BATTERY_POWER_DISCHARGING);
  {
    MTK_BATTERY_STATE Transition={0};
    Transition.PowerState=MTK_BATTERY_POWER_DISCHARGING;
    CHECK(MtkBatteryCoreNotificationSatisfied(&Transition,
          MTK_BATTERY_POWER_DISCHARGING,0,UINT32_MAX));
    Transition.PowerState|=MTK_BATTERY_POWER_CRITICAL;
    CHECK(!MtkBatteryCoreNotificationSatisfied(&Transition,
          MTK_BATTERY_POWER_DISCHARGING,0,UINT32_MAX));
    Transition.PowerState=0;
    CHECK(MtkBatteryCoreNotificationSatisfied(&Transition,0,0,UINT32_MAX));
  }

  BeforeCore = Core;
  BeforeState = State;
  Input = Sample(1000, 0, 250, 200000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreInvalid);
  CHECK(Core.Initialized == BeforeCore.Initialized);
  CHECK(Core.RemainingMahTenth == BeforeCore.RemainingMahTenth);
  CHECK(State.RemainingMahTenth == BeforeState.RemainingMahTenth);
  Input = Sample(3900, 0, 250, 90000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreInvalid);
  CHECK(Core.LastSampleTime100ns == BeforeCore.LastSampleTime100ns);

  MtkBatteryCoreInitialize(&Core);
  Input = Sample(3800, 0, -100, 100000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  MtkBatteryCoreSuspend(&Core);
  CHECK(Core.ReanchorOnNextSample != 0);
  Input = Sample(3900, 0, 250, 200000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK(Core.ReanchorOnNextSample == 0);
  CHECK(Core.StateOfCharge10000 > 0);

  MtkBatteryCoreInitialize(&Core);
  Input = Sample(2900, -10000, 250, 100000000U);
  CHECK(MtkBatteryCoreUpdate(&Core, &Input, &State) == MtkBatteryCoreSuccess);
  CHECK((State.PowerState & MTK_BATTERY_POWER_CRITICAL) != 0);

  printf("MtkBatteryCoreTest passed, checks=%u\n", Checks);
  return 0;
}
