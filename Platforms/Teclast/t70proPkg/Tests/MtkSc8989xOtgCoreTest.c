/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "MediaTekTest.h"
#include <Library/MtkMusbPlatformCore.h>
#include <Library/MtkSc8989xOtgCore.h>

#define EVENT_CAPACITY 64U

typedef enum {
  EventRegister = 1,
  EventGpio
} EVENT_TYPE;

typedef struct {
  EVENT_TYPE Type;
  uint8_t Target;
  uint8_t Mask;
  uint8_t Value;
} EVENT;

typedef enum {
  MutateNone = 0,
  MutateDetach,
  MutateVbusPresent,
  MutateVsafe0vLost,
  MutateReg00OtherBit,
  MutateReg00TargetBack,
  MutateGpioMux,
  MutateCcOrientation,
  MutateRoleControl,
  MutatePartId,
  MutateBoostReassert
} MUTATION;

typedef struct {
  MTK_SC8989X_OTG_EVIDENCE Evidence;
  MTK_SC8989X_OTG_CORE Core;
  EVENT Events[EVENT_CAPACITY];
  unsigned EventCount;
  unsigned ReadCalls;
  unsigned WriteCalls;
  unsigned FailReadAt;
  unsigned FailWriteAt;
  uint8_t FailAfterSideEffect;
  unsigned MutationReadAt;
  MUTATION Mutation;
  unsigned ReenterReadAt;
  uint8_t Reentered;
  MTK_SC8989X_OTG_STATUS ReentryStatus;
  MTK_SC8989X_OTG_STATUS ReentryDisableStatus;
} FAKE_HARDWARE;

static void
RecordEvent(
  FAKE_HARDWARE *Hardware,
  EVENT_TYPE Type,
  uint8_t Target,
  uint8_t Mask,
  uint8_t Value
  )
{
  CHECK(Hardware->EventCount < EVENT_CAPACITY);
  Hardware->Events[Hardware->EventCount].Type = Type;
  Hardware->Events[Hardware->EventCount].Target = Target;
  Hardware->Events[Hardware->EventCount].Mask = Mask;
  Hardware->Events[Hardware->EventCount].Value = Value;
  ++Hardware->EventCount;
}

static MTK_SC8989X_OTG_STATUS
FakeReadFreshEvidence(
  void *Context,
  MTK_SC8989X_OTG_EVIDENCE *Evidence
  )
{
  FAKE_HARDWARE *Hardware = (FAKE_HARDWARE *)Context;

  CHECK(Hardware != NULL);
  CHECK(Evidence != NULL);
  ++Hardware->ReadCalls;
  if (Hardware->FailReadAt == Hardware->ReadCalls) {
    return MtkSc8989xOtgIoError;
  }

  if (Hardware->MutationReadAt == Hardware->ReadCalls) {
    switch (Hardware->Mutation) {
      case MutateDetach:
        Hardware->Evidence.Husb311CcStatus = 0U;
        break;
      case MutateVbusPresent:
        Hardware->Evidence.Husb311PowerStatus |=
          MTK_SC8989X_OTG_HUSB_VBUS_PRESENT;
        break;
      case MutateVsafe0vLost:
        Hardware->Evidence.Husb311VendorStatusReg97 &= (uint8_t)~0x02U;
        break;
      case MutateReg00OtherBit:
        Hardware->Evidence.ScReg00 ^= 0x01U;
        break;
      case MutateReg00TargetBack:
        Hardware->Evidence.ScReg00 |= MTK_SC8989X_OTG_REG00_CLEAR_MASK;
        break;
      case MutateGpioMux:
        Hardware->Evidence.Gpio48.Mode = 1U;
        break;
      case MutateCcOrientation:
        Hardware->Evidence.Husb311CcStatus =
          (uint8_t)(MTK_MUSB_PLATFORM_HUSB311_SRC_OPEN |
            (MTK_MUSB_PLATFORM_HUSB311_SRC_RD <<
             MTK_MUSB_PLATFORM_HUSB311_CC2_SHIFT));
        break;
      case MutateRoleControl:
        Hardware->Evidence.Husb311RoleControl |=
          MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP;
        break;
      case MutatePartId:
        Hardware->Evidence.ScPartIdRegister = (uint8_t)(4U << 3);
        break;
      case MutateBoostReassert:
        Hardware->Evidence.ScReg03 |=
          MTK_SC8989X_OTG_REG03_BOOST_MASK;
        break;
      case MutateNone:
      default:
        break;
    }
  }

  if ((Hardware->ReenterReadAt == Hardware->ReadCalls) &&
      (Hardware->Reentered == 0U)) {
    Hardware->Reentered = 1U;
    Hardware->ReentryStatus = MtkSc8989xOtgEnable(&Hardware->Core);
    Hardware->ReentryDisableStatus = MtkSc8989xOtgDisable(&Hardware->Core);
  }

  *Evidence = Hardware->Evidence;
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
FakeUpdateScRegisterBits(
  void *Context,
  uint8_t Register,
  uint8_t Mask,
  uint8_t Value
  )
{
  FAKE_HARDWARE *Hardware = (FAKE_HARDWARE *)Context;
  uint8_t *RegisterValue;

  CHECK(Hardware != NULL);
  CHECK((Value & (uint8_t)~Mask) == 0U);
  if (Register == MTK_SC8989X_OTG_REG_CHARGE_CONTROL) {
    CHECK(Mask == MTK_SC8989X_OTG_REG00_CLEAR_MASK);
    RegisterValue = &Hardware->Evidence.ScReg00;
  } else if (Register == MTK_SC8989X_OTG_REG_BOOST_LIMIT) {
    CHECK(Mask == MTK_SC8989X_OTG_BOOST_PROFILE_MASK);
    CHECK((Hardware->Evidence.ScReg03 & MTK_SC8989X_OTG_REG03_BOOST_MASK) == 0U);
    CHECK(Hardware->Evidence.Gpio86.PhysicalValue == 0U);
    RegisterValue = &Hardware->Evidence.ScReg0A;
  } else {
    CHECK(Register == MTK_SC8989X_OTG_REG_BOOST_CONTROL);
    CHECK((Mask == MTK_SC8989X_OTG_REG03_CLEAR_MASK) ||
          (Mask == MTK_SC8989X_OTG_REG03_BOOST_MASK));
    RegisterValue = &Hardware->Evidence.ScReg03;
  }

  ++Hardware->WriteCalls;
  RecordEvent(Hardware, EventRegister, Register, Mask, Value);
  if ((Hardware->FailWriteAt == Hardware->WriteCalls) &&
      (Hardware->FailAfterSideEffect == 0U)) {
    return MtkSc8989xOtgIoError;
  }

  *RegisterValue = (uint8_t)((*RegisterValue & (uint8_t)~Mask) |
                             (Value & Mask));
  if (Hardware->FailWriteAt == Hardware->WriteCalls) {
    return MtkSc8989xOtgIoError;
  }
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
FakeWriteGpio(
  void *Context,
  uint8_t Pin,
  uint8_t PhysicalValue
  )
{
  FAKE_HARDWARE *Hardware = (FAKE_HARDWARE *)Context;
  MTK_SC8989X_OTG_GPIO_STATE *Gpio;

  CHECK(Hardware != NULL);
  CHECK(PhysicalValue <= 1U);
  if (Pin == MTK_SC8989X_OTG_GPIO_ENABLE_PIN) {
    Gpio = &Hardware->Evidence.Gpio48;
  } else {
    CHECK(Pin == MTK_SC8989X_OTG_GPIO_OTG_PIN);
    Gpio = &Hardware->Evidence.Gpio86;
  }

  ++Hardware->WriteCalls;
  RecordEvent(Hardware, EventGpio, Pin, 0U, PhysicalValue);
  if ((Hardware->FailWriteAt == Hardware->WriteCalls) &&
      (Hardware->FailAfterSideEffect == 0U)) {
    return MtkSc8989xOtgIoError;
  }

  Gpio->PhysicalValue = PhysicalValue;
  if (Hardware->FailWriteAt == Hardware->WriteCalls) {
    return MtkSc8989xOtgIoError;
  }
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_IO
MakeIo(FAKE_HARDWARE *Hardware)
{
  MTK_SC8989X_OTG_IO Io;

  Io.Context = Hardware;
  Io.ReadFreshEvidence = FakeReadFreshEvidence;
  Io.UpdateScRegisterBits = FakeUpdateScRegisterBits;
  Io.WriteGpio = FakeWriteGpio;
  return Io;
}

static void
ResetHardware(FAKE_HARDWARE *Hardware)
{
  MTK_SC8989X_OTG_IO Io;

  memset(Hardware, 0, sizeof(*Hardware));
  Hardware->Evidence.ScPartIdRegister = (uint8_t)(2U << 3);
  Hardware->Evidence.ScReg00 = 0xA5U;
  Hardware->Evidence.ScReg03 = 0x55U;
  Hardware->Evidence.ScReg0A = 0xC3U;
  Hardware->Evidence.Husb311VendorId = 0x2E99U;
  Hardware->Evidence.Husb311DeviceId = 0x0311U;
  Hardware->Evidence.Husb311PowerStatus = MTK_SC8989X_OTG_HUSB_VBUS_DETECT;
  Hardware->Evidence.Husb311VendorStatusReg97 = 0x02U;
  Hardware->Evidence.Husb311CcStatus = MTK_MUSB_PLATFORM_HUSB311_SRC_RD;
  Hardware->Evidence.Husb311RoleControl =
    MTK_MUSB_PLATFORM_HUSB311_ROLE_RP |
    (MTK_MUSB_PLATFORM_HUSB311_ROLE_RP <<
     MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT);
  Hardware->Evidence.Gpio48.Mode = MTK_SC8989X_OTG_GPIO_MODE_GPIO;
  Hardware->Evidence.Gpio48.Direction =
    MTK_SC8989X_OTG_GPIO_DIRECTION_OUTPUT;
  Hardware->Evidence.Gpio48.PhysicalValue = 0U;
  Hardware->Evidence.Gpio86 = Hardware->Evidence.Gpio48;

  Io = MakeIo(Hardware);
  CHECK(MtkSc8989xOtgInitialize(&Hardware->Core, &Io) ==
        MtkSc8989xOtgSuccess);
}

static void
ExpectEvent(
  const FAKE_HARDWARE *Hardware,
  unsigned Index,
  EVENT_TYPE Type,
  uint8_t Target,
  uint8_t Mask,
  uint8_t Value
  )
{
  CHECK(Index < Hardware->EventCount);
  CHECK(Hardware->Events[Index].Type == Type);
  CHECK(Hardware->Events[Index].Target == Target);
  CHECK(Hardware->Events[Index].Mask == Mask);
  CHECK(Hardware->Events[Index].Value == Value);
}

static void
CheckOriginalState(const FAKE_HARDWARE *Hardware)
{
  CHECK(Hardware->Evidence.ScReg00 == 0xA5U);
  CHECK(Hardware->Evidence.ScReg03 == 0x55U);
  CHECK(Hardware->Evidence.ScReg0A == 0xC3U);
  CHECK(Hardware->Evidence.Gpio48.PhysicalValue == 0U);
  CHECK(Hardware->Evidence.Gpio86.PhysicalValue == 0U);
  CHECK(Hardware->Core.OwnedMask == 0U);
  CHECK(Hardware->Core.RestoredMask == 0U);
  CHECK(!MtkSc8989xOtgIsOutputEnabled(&Hardware->Core));
}

static int
TestSequenceAndDisable(void)
{
  FAKE_HARDWARE Hardware;

  ResetHardware(&Hardware);
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(MtkSc8989xOtgIsOutputEnabled(&Hardware.Core));
  CHECK(!MtkSc8989xOtgIsQuarantined(&Hardware.Core));
  CHECK(Hardware.WriteCalls == 5U);
  CHECK(Hardware.Evidence.ScReg00 == 0x25U);
  CHECK(Hardware.Evidence.ScReg03 == 0x65U);
  CHECK(Hardware.Evidence.Gpio48.PhysicalValue == 1U);
  CHECK(Hardware.Evidence.Gpio86.PhysicalValue == 1U);
  ExpectEvent(&Hardware, 0U, EventRegister,
              MTK_SC8989X_OTG_REG_CHARGE_CONTROL,
              MTK_SC8989X_OTG_REG00_CLEAR_MASK, 0U);
  ExpectEvent(&Hardware, 1U, EventGpio,
              MTK_SC8989X_OTG_GPIO_ENABLE_PIN, 0U, 1U);
  ExpectEvent(&Hardware, 2U, EventRegister,
              MTK_SC8989X_OTG_REG_BOOST_CONTROL,
              MTK_SC8989X_OTG_REG03_CLEAR_MASK, 0U);
  ExpectEvent(&Hardware, 3U, EventGpio,
              MTK_SC8989X_OTG_GPIO_OTG_PIN, 0U, 1U);
  ExpectEvent(&Hardware, 4U, EventRegister,
              MTK_SC8989X_OTG_REG_BOOST_CONTROL,
              MTK_SC8989X_OTG_REG03_BOOST_MASK,
              MTK_SC8989X_OTG_REG03_BOOST_MASK);

  /* Source detach cannot prevent this core from releasing its own outputs. */
  Hardware.Evidence.Husb311CcStatus = 0U;
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.WriteCalls == 10U);
  ExpectEvent(&Hardware, 5U, EventRegister,
              MTK_SC8989X_OTG_REG_BOOST_CONTROL,
              MTK_SC8989X_OTG_REG03_BOOST_MASK, 0U);
  ExpectEvent(&Hardware, 6U, EventGpio,
              MTK_SC8989X_OTG_GPIO_OTG_PIN, 0U, 0U);
  ExpectEvent(&Hardware, 7U, EventRegister,
              MTK_SC8989X_OTG_REG_BOOST_CONTROL,
              MTK_SC8989X_OTG_REG03_CLEAR_MASK,
              MTK_SC8989X_OTG_REG03_CLEAR_MASK);
  ExpectEvent(&Hardware, 8U, EventGpio,
              MTK_SC8989X_OTG_GPIO_ENABLE_PIN, 0U, 0U);
  ExpectEvent(&Hardware, 9U, EventRegister,
              MTK_SC8989X_OTG_REG_CHARGE_CONTROL,
              MTK_SC8989X_OTG_REG00_CLEAR_MASK,
              MTK_SC8989X_OTG_REG00_CLEAR_MASK);
  CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
  CheckOriginalState(&Hardware);
  return 1;
}

static void
CheckDeniedWithoutWrites(const MTK_SC8989X_OTG_EVIDENCE *Evidence)
{
  FAKE_HARDWARE Hardware;

  ResetHardware(&Hardware);
  Hardware.Evidence = *Evidence;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) != MtkSc8989xOtgSuccess);
  CHECK(Hardware.WriteCalls == 0U);
  CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
}

static int
TestPreconditions(void)
{
  FAKE_HARDWARE Hardware;
  MTK_SC8989X_OTG_EVIDENCE Evidence;
  MTK_SC8989X_OTG_IO Io;

  ResetHardware(&Hardware);
  CHECK(MtkSc8989xOtgInitialize(&Hardware.Core, NULL) ==
        MtkSc8989xOtgInvalid);
  CHECK(MtkSc8989xOtgInitialize(NULL, &Hardware.Core.Io) ==
        MtkSc8989xOtgInvalid);

  ResetHardware(&Hardware);
  Hardware.Evidence.ScPartIdRegister = (uint8_t)(1U << 3);
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311VendorId = 0U;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311PowerStatus |= MTK_SC8989X_OTG_HUSB_INITIALIZED;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311PowerStatus &=
    (uint8_t)~MTK_SC8989X_OTG_HUSB_VBUS_DETECT;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311PowerStatus |= MTK_SC8989X_OTG_HUSB_VBUS_PRESENT;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311VendorStatusReg97 = 0U;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311CcStatus =
    MTK_MUSB_PLATFORM_HUSB311_SRC_RD |
    MTK_MUSB_PLATFORM_HUSB311_CC_LOOKING_FOR_CONNECTION;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311RoleControl |=
    (uint8_t)(MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_RESERVED <<
              MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_SHIFT);
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.ScReg03 |= MTK_SC8989X_OTG_REG03_BOOST_MASK;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Gpio48.PhysicalValue = 1U;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Gpio86.Direction = 0U;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.Gpio48.Mode = 2U;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.ScReg0A = 0x73U;
  Evidence = Hardware.Evidence;
  CheckDeniedWithoutWrites(&Evidence);

  ResetHardware(&Hardware);
  Hardware.Evidence.ScReg0A = 0xCBU;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.Evidence.ScReg0A == 0xCBU);
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.Evidence.ScReg0A == 0xCBU);

  ResetHardware(&Hardware);
  Io = MakeIo(&Hardware);
  Io.WriteGpio = NULL;
  Hardware.Core = (MTK_SC8989X_OTG_CORE){0};
  CHECK(MtkSc8989xOtgInitialize(&Hardware.Core, &Io) ==
        MtkSc8989xOtgNotReady);
  return 1;
}

static int
TestWriteFailures(void)
{
  unsigned Failure;
  uint8_t After;

  for (Failure = 1U; Failure <= 5U; ++Failure) {
    for (After = 0U; After <= 1U; ++After) {
      FAKE_HARDWARE Hardware;

      ResetHardware(&Hardware);
      Hardware.FailWriteAt = Failure;
      Hardware.FailAfterSideEffect = After;
      CHECK(MtkSc8989xOtgEnable(&Hardware.Core) !=
            MtkSc8989xOtgSuccess);
      if (MtkSc8989xOtgIsQuarantined(&Hardware.Core)) {
        CHECK(Hardware.Core.OwnedMask != 0U);
        CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
              MtkSc8989xOtgSuccess);
      }
      CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
      CheckOriginalState(&Hardware);
    }
  }
  return 1;
}

static int
TestReadFailures(void)
{
  FAKE_HARDWARE Baseline;
  unsigned ReadCount;
  unsigned Failure;

  ResetHardware(&Baseline);
  CHECK(MtkSc8989xOtgEnable(&Baseline.Core) == MtkSc8989xOtgSuccess);
  ReadCount = Baseline.ReadCalls;

  for (Failure = 1U; Failure <= ReadCount; ++Failure) {
    FAKE_HARDWARE Hardware;

    ResetHardware(&Hardware);
    Hardware.FailReadAt = Failure;
    CHECK(MtkSc8989xOtgEnable(&Hardware.Core) !=
          MtkSc8989xOtgSuccess);
    if (MtkSc8989xOtgIsQuarantined(&Hardware.Core)) {
      CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
            MtkSc8989xOtgSuccess);
    }
    CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
    CheckOriginalState(&Hardware);
  }
  return 1;
}

static int
TestFreshSourceEvidence(void)
{
  FAKE_HARDWARE Hardware;

  /* Loss of either raw safe0V condition at any pre-boost sample must
   * prevent the final boost write, including after GPIO86 goes HIGH. */
  for (unsigned Read = 2U; Read <= 10U; ++Read) {
    for (unsigned Kind = 0U; Kind < 2U; ++Kind) {
      ResetHardware(&Hardware);
      Hardware.MutationReadAt = Read;
      Hardware.Mutation = Kind ? MutateVsafe0vLost : MutateVbusPresent;
      CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgNotReady);
      CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
      for (unsigned Event = 0U; Event < Hardware.EventCount; ++Event) {
        CHECK(!(Hardware.Events[Event].Type == EventRegister &&
                Hardware.Events[Event].Target == MTK_SC8989X_OTG_REG_BOOST_CONTROL &&
                Hardware.Events[Event].Mask == MTK_SC8989X_OTG_REG03_BOOST_MASK &&
                Hardware.Events[Event].Value != 0U));
      }
      CheckOriginalState(&Hardware);
    }
  }

  ResetHardware(&Hardware);
  Hardware.Evidence.Husb311VendorStatusReg97 = 0xFFU;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.Evidence.Husb311VendorStatusReg97 == 0xFFU);
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);

  ResetHardware(&Hardware);
  Hardware.MutationReadAt = 4U;
  Hardware.Mutation = MutateDetach;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) ==
        MtkSc8989xOtgNotReady);
  CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
  CheckOriginalState(&Hardware);

  ResetHardware(&Hardware);
  Hardware.MutationReadAt = 4U;
  Hardware.Mutation = MutateCcOrientation;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) ==
        MtkSc8989xOtgNotReady);
  CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
  CheckOriginalState(&Hardware);

  ResetHardware(&Hardware);
  Hardware.MutationReadAt = 4U;
  Hardware.Mutation = MutateRoleControl;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) ==
        MtkSc8989xOtgNotReady);
  CHECK(Hardware.Core.State == MtkSc8989xOtgStateIdle);
  CheckOriginalState(&Hardware);

  ResetHardware(&Hardware);
  Hardware.MutationReadAt = 4U;
  Hardware.Mutation = MutatePartId;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  CHECK(Hardware.Core.OwnedMask != 0U);
  Hardware.Evidence.ScPartIdRegister = (uint8_t)(2U << 3);
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);

  ResetHardware(&Hardware);
  Hardware.ReenterReadAt = 2U;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.Reentered == 1U);
  CHECK(Hardware.ReentryStatus == MtkSc8989xOtgBusy);
  CHECK(Hardware.ReentryDisableStatus == MtkSc8989xOtgBusy);
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);
  return 1;
}

static int
TestEnabledStateReentry(void)
{
  FAKE_HARDWARE Hardware;

  ResetHardware(&Hardware);
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  Hardware.ReenterReadAt = Hardware.ReadCalls + 1U;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.Reentered == 1U);
  CHECK(Hardware.ReentryStatus == MtkSc8989xOtgBusy);
  CHECK(Hardware.ReentryDisableStatus == MtkSc8989xOtgBusy);
  CHECK(Hardware.Core.State == MtkSc8989xOtgStateOutputEnabled);
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);
  return 1;
}

static int
TestExternalChangeQuarantine(void)
{
  FAKE_HARDWARE Hardware;

  ResetHardware(&Hardware);
  Hardware.MutationReadAt = 3U;
  Hardware.Mutation = MutateReg00OtherBit;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  CHECK(MtkSc8989xOtgIsQuarantined(&Hardware.Core));
  CHECK(Hardware.Core.OwnedMask == 0x01U);
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  Hardware.Evidence.ScReg00 = (uint8_t)(Hardware.Evidence.ScReg00 ^ 0x01U);
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);

  ResetHardware(&Hardware);
  Hardware.MutationReadAt = 3U;
  Hardware.Mutation = MutateReg00TargetBack;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);

  ResetHardware(&Hardware);
  Hardware.MutationReadAt = 3U;
  Hardware.Mutation = MutateGpioMux;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  Hardware.Evidence.Gpio48.Mode = MTK_SC8989X_OTG_GPIO_MODE_GPIO;
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);
  return 1;
}

static int
TestCleanupRestoredVectorReassertion(void)
{
  FAKE_HARDWARE Hardware;

  ResetHardware(&Hardware);
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  Hardware.MutationReadAt = Hardware.ReadCalls + 4U;
  Hardware.Mutation = MutateBoostReassert;
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  CHECK(Hardware.Core.OwnedMask == 0x1FU);
  CHECK(Hardware.Core.RestoredMask != 0U);
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);
  return 1;
}

static int
TestCleanupRetryAfterSideEffectFailure(void)
{
  FAKE_HARDWARE Hardware;

  ResetHardware(&Hardware);
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  Hardware.FailWriteAt = Hardware.WriteCalls + 1U;
  Hardware.FailAfterSideEffect = 1U;
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) ==
        MtkSc8989xOtgQuarantined);
  CHECK(MtkSc8989xOtgIsQuarantined(&Hardware.Core));
  CHECK(Hardware.Core.OwnedMask == 0x1FU);
  CHECK(MtkSc8989xOtgRetryCleanup(&Hardware.Core) ==
        MtkSc8989xOtgSuccess);
  CheckOriginalState(&Hardware);
  return 1;
}

static int
TestColdBoostProfile(void)
{
  FAKE_HARDWARE Hardware;

  ResetHardware(&Hardware);
  Hardware.Evidence.ScReg0A = 0x7BU;
  CHECK(MtkSc8989xOtgEnable(&Hardware.Core) == MtkSc8989xOtgUnsupported);
  CHECK(Hardware.WriteCalls == 0U);
  CHECK(MtkSc8989xOtgEnableCold(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.Evidence.ScReg0A == 0xCBU && Hardware.WriteCalls == 6U);
  ExpectEvent(&Hardware, 0U, EventRegister, 0x0AU, 0xF7U, 0xC3U);
  ExpectEvent(&Hardware, 5U, EventRegister, 0x03U, 0x20U, 0x20U);
  CHECK(MtkSc8989xOtgDisable(&Hardware.Core) == MtkSc8989xOtgSuccess);
  CHECK(Hardware.Evidence.ScReg0A == 0x7BU && Hardware.WriteCalls == 12U);
  ExpectEvent(&Hardware, 6U, EventRegister, 0x03U, 0x20U, 0U);
  ExpectEvent(&Hardware, 11U, EventRegister, 0x0AU, 0xF7U, 0x73U);

  for (unsigned Failure = 1U; Failure <= 6U; ++Failure) {
    ResetHardware(&Hardware);
    Hardware.Evidence.ScReg0A = 0x7BU;
    Hardware.FailWriteAt = Failure;
    Hardware.FailAfterSideEffect = 1U;
    CHECK(MtkSc8989xOtgEnableCold(&Hardware.Core) == MtkSc8989xOtgIoError);
    CHECK(Hardware.Evidence.ScReg0A == 0x7BU && Hardware.Core.OwnedMask == 0U);
    CHECK(Hardware.Evidence.ScReg03 == 0x55U);
    CHECK(MtkSc8989xOtgEnableCold(&Hardware.Core) == MtkSc8989xOtgSuccess);
    CHECK(MtkSc8989xOtgDisable(&Hardware.Core) == MtkSc8989xOtgSuccess);
    CHECK(Hardware.Evidence.ScReg0A == 0x7BU);
  }

  ResetHardware(&Hardware);
  Hardware.Evidence.ScReg0A = 0x7BU;
  Hardware.Evidence.Husb311PowerStatus |= MTK_SC8989X_OTG_HUSB_VBUS_PRESENT;
  CHECK(MtkSc8989xOtgEnableCold(&Hardware.Core) == MtkSc8989xOtgNotReady);
  CHECK(Hardware.WriteCalls == 0U);
  return 1;
}

int
main(void)
{
  (VOID)Install; (VOID)Installs;
  if (!TestSequenceAndDisable() || !TestColdBoostProfile() || !TestPreconditions() ||
      !TestWriteFailures() || !TestReadFailures() ||
      !TestFreshSourceEvidence() || !TestEnabledStateReentry() ||
      !TestExternalChangeQuarantine() ||
      !TestCleanupRestoredVectorReassertion() ||
      !TestCleanupRetryAfterSideEffectFailure()) {
    return 1;
  }
  printf("SC8989x OTG core: %u checks passed (guarded writes, raw T70 limits, per-step source evidence, side-effect errors, reverse ownership cleanup and final-vector quarantine)\n",
         Checks);
  return 0;
}
