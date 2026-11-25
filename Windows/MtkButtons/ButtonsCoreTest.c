/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ButtonsCore.c"

static uint32_t mChecks;
static uint32_t mFailureLine;

#define CHECK(Expression) do { \
  ++mChecks; \
  if (!(Expression) && mFailureLine == 0) mFailureLine = (uint32_t)__LINE__; \
} while (0)

static void MakeKpd(uint8_t Pressed, MTK_BUTTON_KPD_SAMPLE *Sample) {
  Sample->Enable = 1;
  Sample->Debounce = 1024;
  Sample->StatusWord0 = 0xffffU;
  if (Pressed & MTK_BUTTON_STATE_VOLUME_UP) {
    Sample->StatusWord0 &= (uint16_t)~0x0001U;
  }
  if (Pressed & MTK_BUTTON_STATE_VOLUME_DOWN) {
    Sample->StatusWord0 &= (uint16_t)~0x0002U;
  }
}

static uint16_t MakePmic(uint8_t PowerPressed) {
  return PowerPressed ? (uint16_t)~0x0002U : 0xffffU;
}

static void Reset(MTK_BUTTONS_CORE *Core) {
  CHECK(MtkButtonsCoreInitialize(Core) == MtkButtonsCoreSuccess);
}

static void Sample(
  MTK_BUTTONS_CORE *Core,
  uint8_t Pressed,
  uint32_t ValidFlags,
  MTK_BUTTONS_CORE_STATUS Expected
  ) {
  MTK_BUTTON_KPD_SAMPLE Kpd;
  MakeKpd(Pressed, &Kpd);
  CHECK(MtkButtonsCoreSample(Core, &Kpd, MakePmic((Pressed & MTK_BUTTON_STATE_POWER) != 0),
                             ValidFlags) == Expected);
}

static void Snapshot(
  MTK_BUTTONS_CORE *Core,
  uint8_t Pressed,
  uint8_t PowerEventMask,
  uint64_t Generation,
  uint32_t ValidFlags,
  MTK_BUTTONS_CORE_STATUS Expected
  ) {
  MTK_BUTTON_KPD_SAMPLE Kpd;
  MakeKpd(Pressed, &Kpd);
  CHECK(MtkButtonsCoreSamplePmicSnapshot(
          Core, &Kpd,
          MakePmic((Pressed & MTK_BUTTON_STATE_POWER) != 0),
          PowerEventMask, Generation, ValidFlags) == Expected);
}

static void CompleteOne(
  MTK_BUTTONS_CORE *Core,
  uint8_t Button,
  uint8_t Pressed
  ) {
  MTK_BUTTON_ACTION Action;
  CHECK(MtkButtonsCorePeekAction(Core, &Action) == MtkButtonsCoreSuccess);
  CHECK(Action.Button == Button && Action.Pressed == Pressed && Action.Token != 0);
  CHECK(MtkButtonsCoreCompleteAction(Core, &Action, 1) == MtkButtonsCoreSuccess);
}

static void TestPmicSnapshotShortTapAtUpLevel(void) {
  MTK_BUTTONS_CORE Core;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 1, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.LastSnapshotGeneration == 1 && Core.ActionCount == 0);

  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_PRESSED, 2,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.LastSnapshotGeneration == 2 && Core.PhysicalPressed == 0 &&
        Core.ActionCount == 2);
  CompleteOne(&Core, MtkButtonPower, 1);
  CompleteOne(&Core, MtkButtonPower, 0);
  CHECK(Core.ActionCount == 0 && Core.DeliveredPressed == 0);

  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_MASK, 3,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 2 && Core.LastSnapshotGeneration == 3);
  CompleteOne(&Core, MtkButtonPower, 1);
  CompleteOne(&Core, MtkButtonPower, 0);
  CHECK(Core.DeliveredPressed == 0 && Core.ActionCount == 0);
}

static void TestPmicSnapshotReleasePressAtDownLevel(void) {
  MTK_BUTTONS_CORE Core;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 1, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, 0, 2,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CompleteOne(&Core, MtkButtonPower, 1);

  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_RELEASED, 3,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 2 && Core.LastSnapshotGeneration == 3);
  CompleteOne(&Core, MtkButtonPower, 0);
  CompleteOne(&Core, MtkButtonPower, 1);
  CHECK(Core.DeliveredPressed == MTK_BUTTON_STATE_POWER);

  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_MASK, 4,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 2 && Core.LastSnapshotGeneration == 4);
  CompleteOne(&Core, MtkButtonPower, 0);
  CompleteOne(&Core, MtkButtonPower, 1);
  CHECK(Core.DeliveredPressed == MTK_BUTTON_STATE_POWER && Core.ActionCount == 0);
}

static void TestPmicSnapshotLevelChangeTakesPrecedence(void) {
  MTK_BUTTONS_CORE Core;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 1, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);

  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_MASK, 2,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 1);
  CompleteOne(&Core, MtkButtonPower, 1);

  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_MASK, 3,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 1);
  CompleteOne(&Core, MtkButtonPower, 0);
  CHECK(Core.LastSnapshotGeneration == 3 && Core.ActionCount == 0);
}

static void TestPmicSnapshotKeepsVolumeOrderAroundPowerTap(void) {
  MTK_BUTTONS_CORE Core;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 1, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Snapshot(&Core, MTK_BUTTON_STATE_VOLUME_DOWN, 0, 2,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CompleteOne(&Core, MtkButtonVolumeDown, 1);

  Snapshot(&Core, MTK_BUTTON_STATE_VOLUME_UP, MTK_BUTTON_POWER_EVENT_PRESSED, 3,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 4);
  CompleteOne(&Core, MtkButtonVolumeDown, 0);
  CompleteOne(&Core, MtkButtonPower, 1);
  CompleteOne(&Core, MtkButtonVolumeUp, 1);
  CompleteOne(&Core, MtkButtonPower, 0);
  CHECK(Core.DeliveredPressed == MTK_BUTTON_STATE_VOLUME_UP && Core.ActionCount == 0);
}

static void TestPmicSnapshotGenerationAndValidation(void) {
  MTK_BUTTONS_CORE Core, Before;
  MTK_BUTTON_KPD_SAMPLE Kpd;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 10, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Before = Core;

  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 10,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 9,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 0,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreInvalid);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, 0x02U, 11,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreInvalid);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, 0x80U, 11,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreInvalid);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 11,
           MTK_BUTTON_SAMPLE_KPD_VALID, MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 11,
           MTK_BUTTON_SAMPLE_PMIC_VALID, MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 11,
           0x80000003U, MtkButtonsCoreInvalid);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);

  MakeKpd(0, &Kpd);
  Kpd.Enable = 0;
  CHECK(MtkButtonsCoreSamplePmicSnapshot(
          &Core, &Kpd, MakePmic(0), MTK_BUTTON_POWER_EVENT_PRESSED, 11,
          MTK_BUTTON_SAMPLE_ALL_VALID) == MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Kpd.Enable = 1;
  Kpd.Debounce = 1023;
  CHECK(MtkButtonsCoreSamplePmicSnapshot(
          &Core, &Kpd, MakePmic(0), MTK_BUTTON_POWER_EVENT_PRESSED, 11,
          MTK_BUTTON_SAMPLE_ALL_VALID) == MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
}

static void TestPmicSnapshotGenerationSurvivesBaselineReset(void) {
  MTK_BUTTONS_CORE Core, Before;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 40, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(MtkButtonsCoreResetBaseline(&Core) == MtkButtonsCoreSuccess);
  CHECK(Core.BaselineValid == 0 && Core.LastSnapshotGeneration == 40);
  Before = Core;
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 40,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_PRESSED, 41,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.BaselineValid == 1 &&
        Core.SuppressUntilRelease == MTK_BUTTON_STATE_POWER && Core.ActionCount == 0 &&
        Core.LastSnapshotGeneration == 41);
}

static void TestPmicSnapshotBootHeldSuppression(void) {
  MTK_BUTTONS_CORE Core;
  Reset(&Core);
  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_MASK, 1,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.SuppressUntilRelease == MTK_BUTTON_STATE_POWER && Core.ActionCount == 0);

  Snapshot(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_POWER_EVENT_RELEASED, 2,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.SuppressUntilRelease == MTK_BUTTON_STATE_POWER && Core.ActionCount == 0);
  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_MASK, 3,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.SuppressUntilRelease == 0 && Core.PhysicalPressed == 0 &&
        Core.ActionCount == 0);

  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_PRESSED, 4,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 2);
  CompleteOne(&Core, MtkButtonPower, 1);
  CompleteOne(&Core, MtkButtonPower, 0);
}

static void TestPmicSnapshotBatchCapacityAndTokenExhaustion(void) {
  MTK_BUTTONS_CORE Core, Before;
  uint32_t Cycle;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 1, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  for (Cycle = 0; Cycle < 31U; ++Cycle) {
    uint8_t Pressed = (Cycle & 1U) ? 0 : MTK_BUTTON_STATE_VOLUME_UP;
    Snapshot(&Core, Pressed, 0, (uint64_t)Cycle + 2U,
             MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  }
  CHECK(Core.ActionCount == 31 && Core.LastSnapshotGeneration == 32 &&
        Core.PhysicalPressed == MTK_BUTTON_STATE_VOLUME_UP);
  Before = Core;
  Snapshot(&Core, MTK_BUTTON_STATE_VOLUME_UP, MTK_BUTTON_POWER_EVENT_PRESSED, 33,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreQueueFull);
  Before.Quarantined = 1;
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);

  Reset(&Core);
  Snapshot(&Core, 0, 0, 1, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Core.NextToken = 0xffffffffU;
  Before = Core;
  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_PRESSED, 2,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreQuarantined);
  Before.Quarantined = 1;
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);

  Reset(&Core);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Core.NextToken = 0xffffffffU;
  Sample(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_SAMPLE_ALL_VALID,
         MtkButtonsCoreQuarantined);
  CHECK(Core.Quarantined == 1 && Core.ActionCount == 0 &&
        Core.PhysicalPressed == 0 && Core.NextToken == 0xffffffffU);
}

static void TestPmicSnapshotFailedToggleIsNotReissued(void) {
  MTK_BUTTONS_CORE Core;
  MTK_BUTTON_ACTION Action;
  MTK_BUTTONS_CORE Before;
  Reset(&Core);
  Snapshot(&Core, 0, 0, 1, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_PRESSED, 2,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 2 && Core.LastSnapshotGeneration == 2);
  CHECK(MtkButtonsCorePeekAction(&Core, &Action) == MtkButtonsCoreSuccess);
  CHECK(Action.Button == MtkButtonPower && Action.Pressed == 1);
  CHECK(MtkButtonsCoreCompleteAction(&Core, &Action, 0) == MtkButtonsCoreIoUncertain);
  Before = Core;
  Snapshot(&Core, 0, MTK_BUTTON_POWER_EVENT_MASK, 3,
           MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreQuarantined);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  CHECK(Core.LastSnapshotGeneration == 2 && Core.ActionCount == 2);
}

static void TestInitialHeldKeysAreSuppressed(void) {
  MTK_BUTTONS_CORE Core;
  MTK_BUTTON_ACTION Action;
  Reset(&Core);
  Sample(&Core, MTK_BUTTON_STATE_POWER | MTK_BUTTON_STATE_VOLUME_UP |
                MTK_BUTTON_STATE_VOLUME_DOWN,
         MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.BaselineValid == 1 && Core.SuppressUntilRelease == 7);
  CHECK(Core.ActionCount == 0 && Core.DeliveredPressed == 0);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.SuppressUntilRelease == 0 && Core.ActionCount == 0);
  CHECK(MtkButtonsCorePeekAction(&Core, &Action) == MtkButtonsCoreNotReady);
  Sample(&Core, MTK_BUTTON_STATE_VOLUME_UP, MTK_BUTTON_SAMPLE_ALL_VALID,
         MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 1);
  CompleteOne(&Core, MtkButtonVolumeUp, 1);
  CHECK(Core.DeliveredPressed == MTK_BUTTON_STATE_VOLUME_UP);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CompleteOne(&Core, MtkButtonVolumeUp, 0);
  CHECK(Core.DeliveredPressed == 0 && Core.ActionCount == 0);
}

static void TestConcurrentOrderingAndPowerBit(void) {
  MTK_BUTTONS_CORE Core;
  MTK_BUTTON_ACTION Action, Stale;
  MTK_BUTTON_KPD_SAMPLE Kpd;
  Reset(&Core);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);

  /* Both KPD bits are active-low; PMIC bit 1 independently marks power. */
  MakeKpd(MTK_BUTTON_STATE_VOLUME_UP | MTK_BUTTON_STATE_VOLUME_DOWN, &Kpd);
  CHECK(Kpd.StatusWord0 == 0xfffcU && Kpd.Enable == 1 && Kpd.Debounce == 1024);
  CHECK(MtkButtonsCoreSample(&Core, &Kpd, MakePmic(1), MTK_BUTTON_SAMPLE_ALL_VALID) ==
        MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 3);
  CHECK(MtkButtonsCorePeekAction(&Core, &Action) == MtkButtonsCoreSuccess);
  Stale = Action;
  CHECK(MtkButtonsCoreCompleteAction(&Core, &Action, 1) == MtkButtonsCoreSuccess);
  CHECK(MtkButtonsCoreCompleteAction(&Core, &Stale, 1) == MtkButtonsCoreInvalid);
  CHECK(Core.DeliveredPressed == MTK_BUTTON_STATE_POWER);
  CompleteOne(&Core, MtkButtonVolumeUp, 1);
  CompleteOne(&Core, MtkButtonVolumeDown, 1);
  CHECK(Core.DeliveredPressed == 7 && Core.ActionCount == 0);

  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 3);
  CompleteOne(&Core, MtkButtonVolumeDown, 0);
  CompleteOne(&Core, MtkButtonVolumeUp, 0);
  CompleteOne(&Core, MtkButtonPower, 0);
  CHECK(Core.DeliveredPressed == 0 && Core.ActionCount == 0);
}

static void TestPartialSampleIsNoOp(void) {
  MTK_BUTTONS_CORE Core, Before;
  MTK_BUTTON_KPD_SAMPLE Kpd;
  Reset(&Core);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Sample(&Core, MTK_BUTTON_STATE_VOLUME_UP, MTK_BUTTON_SAMPLE_ALL_VALID,
         MtkButtonsCoreSuccess);
  Before = Core;
  MakeKpd(0, &Kpd);
  CHECK(MtkButtonsCoreSample(&Core, &Kpd, MakePmic(0), MTK_BUTTON_SAMPLE_KPD_VALID) ==
        MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  CHECK(MtkButtonsCoreSample(&Core, &Kpd, MakePmic(0), MTK_BUTTON_SAMPLE_PMIC_VALID) ==
        MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  CHECK(MtkButtonsCoreSample(&Core, &Kpd, MakePmic(0), 0x80000003U) ==
        MtkButtonsCoreInvalid);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Kpd.Enable = 0;
  CHECK(MtkButtonsCoreSample(&Core, &Kpd, MakePmic(0), MTK_BUTTON_SAMPLE_ALL_VALID) ==
        MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Kpd.Enable = 1;
  Kpd.Debounce = 0;
  CHECK(MtkButtonsCoreSample(&Core, &Kpd, MakePmic(0), MTK_BUTTON_SAMPLE_ALL_VALID) ==
        MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  Kpd.Debounce = 1023;
  CHECK(MtkButtonsCoreSample(&Core, &Kpd, MakePmic(0), MTK_BUTTON_SAMPLE_ALL_VALID) ==
        MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
}

static void TestUncertainToggleIsNotReplayed(void) {
  MTK_BUTTONS_CORE Core;
  MTK_BUTTON_ACTION Action, Again;
  Reset(&Core);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Sample(&Core, MTK_BUTTON_STATE_POWER, MTK_BUTTON_SAMPLE_ALL_VALID,
         MtkButtonsCoreSuccess);
  CHECK(MtkButtonsCorePeekAction(&Core, &Action) == MtkButtonsCoreSuccess);
  CHECK(Action.Button == MtkButtonPower && Action.Pressed == 1);
  CHECK(MtkButtonsCoreCompleteAction(&Core, &Action, 0) == MtkButtonsCoreIoUncertain);
  CHECK(Core.Quarantined == 1 && Core.ActionCount == 1);
  CHECK(Core.DeliveredPressed == 0);
  CHECK(MtkButtonsCorePeekAction(&Core, &Again) == MtkButtonsCoreQuarantined);
  CHECK(MtkButtonsCoreCompleteAction(&Core, &Action, 1) == MtkButtonsCoreQuarantined);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreQuarantined);
  CHECK(Core.ActionCount == 1 && Core.DeliveredPressed == 0);
}

static void TestReleaseAllBeforeLeavingD0(void) {
  MTK_BUTTONS_CORE Core, Before;
  Reset(&Core);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Sample(&Core, MTK_BUTTON_STATE_POWER | MTK_BUTTON_STATE_VOLUME_UP |
                MTK_BUTTON_STATE_VOLUME_DOWN,
         MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Before = Core;
  CHECK(MtkButtonsCoreQueueReleaseAll(&Core) == MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  CompleteOne(&Core, MtkButtonPower, 1);
  CompleteOne(&Core, MtkButtonVolumeUp, 1);
  CompleteOne(&Core, MtkButtonVolumeDown, 1);
  CHECK(Core.DeliveredPressed == 7 && Core.ActionCount == 0);

  CHECK(MtkButtonsCoreQueueReleaseAll(&Core) == MtkButtonsCoreSuccess);
  CHECK(Core.SuppressUntilRelease == 7 && Core.PhysicalPressed == 7 &&
        Core.ActionCount == 3);
  CompleteOne(&Core, MtkButtonVolumeDown, 0);
  CompleteOne(&Core, MtkButtonVolumeUp, 0);
  CompleteOne(&Core, MtkButtonPower, 0);
  CHECK(Core.DeliveredPressed == 0 && Core.PhysicalPressed == 7 &&
        Core.SuppressUntilRelease == 7 && Core.ActionCount == 0);
  Sample(&Core, 7, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 0 && Core.SuppressUntilRelease == 7);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.ActionCount == 0 && Core.SuppressUntilRelease == 0);
}

static void TestResetBaselineRequiresDeliveredAllUp(void) {
  MTK_BUTTONS_CORE Core, Before;
  Reset(&Core);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  Sample(&Core, MTK_BUTTON_STATE_VOLUME_UP, MTK_BUTTON_SAMPLE_ALL_VALID,
         MtkButtonsCoreSuccess);
  Before = Core;
  CHECK(MtkButtonsCoreResetBaseline(&Core) == MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);
  CompleteOne(&Core, MtkButtonVolumeUp, 1);
  Before = Core;
  CHECK(MtkButtonsCoreResetBaseline(&Core) == MtkButtonsCoreNotReady);
  CHECK(memcmp(&Core, &Before, sizeof(Core)) == 0);

  CHECK(MtkButtonsCoreQueueReleaseAll(&Core) == MtkButtonsCoreSuccess);
  CompleteOne(&Core, MtkButtonVolumeUp, 0);
  CHECK(MtkButtonsCoreResetBaseline(&Core) == MtkButtonsCoreSuccess);
  CHECK(Core.BaselineValid == 0 && Core.PhysicalPressed == 0 &&
        Core.SuppressUntilRelease == 0 && Core.DeliveredPressed == 0 &&
        Core.ActionCount == 0);
  Sample(&Core, MTK_BUTTON_STATE_VOLUME_DOWN | MTK_BUTTON_STATE_POWER,
         MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.BaselineValid == 1 &&
        Core.SuppressUntilRelease ==
          (MTK_BUTTON_STATE_VOLUME_DOWN | MTK_BUTTON_STATE_POWER) &&
        Core.ActionCount == 0);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  CHECK(Core.SuppressUntilRelease == 0 && Core.ActionCount == 0);
}

static void TestQueueOverflowQuarantinesWithoutPartialBatch(void) {
  MTK_BUTTONS_CORE Core;
  uint32_t Cycle;
  Reset(&Core);
  Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  for (Cycle = 0; Cycle < MTK_BUTTON_ACTION_CAPACITY / 2U; ++Cycle) {
    Sample(&Core, MTK_BUTTON_STATE_VOLUME_UP, MTK_BUTTON_SAMPLE_ALL_VALID,
           MtkButtonsCoreSuccess);
    Sample(&Core, 0, MTK_BUTTON_SAMPLE_ALL_VALID, MtkButtonsCoreSuccess);
  }
  CHECK(Core.ActionCount == MTK_BUTTON_ACTION_CAPACITY);
  CHECK(Core.PhysicalPressed == 0);
  Sample(&Core, MTK_BUTTON_STATE_VOLUME_UP, MTK_BUTTON_SAMPLE_ALL_VALID,
         MtkButtonsCoreQueueFull);
  CHECK(Core.Quarantined == 1 && Core.ActionCount == MTK_BUTTON_ACTION_CAPACITY);
  CHECK(Core.PhysicalPressed == 0);
}

int TestMain(void) {
  TestPmicSnapshotShortTapAtUpLevel();
  TestPmicSnapshotReleasePressAtDownLevel();
  TestPmicSnapshotLevelChangeTakesPrecedence();
  TestPmicSnapshotKeepsVolumeOrderAroundPowerTap();
  TestPmicSnapshotGenerationAndValidation();
  TestPmicSnapshotGenerationSurvivesBaselineReset();
  TestPmicSnapshotBootHeldSuppression();
  TestPmicSnapshotBatchCapacityAndTokenExhaustion();
  TestPmicSnapshotFailedToggleIsNotReissued();
  TestInitialHeldKeysAreSuppressed();
  TestConcurrentOrderingAndPowerBit();
  TestPartialSampleIsNoOp();
  TestUncertainToggleIsNotReplayed();
  TestReleaseAllBeforeLeavingD0();
  TestResetBaselineRequiresDeliveredAllUp();
  TestQueueOverflowQuarantinesWithoutPartialBatch();
  return (int)(mFailureLine ? mFailureLine : 0);
}
