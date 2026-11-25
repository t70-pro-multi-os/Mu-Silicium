/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MtkUsbHubCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(Condition) do { \
  ++Checks; \
  if (!(Condition)) { \
    fprintf (stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #Condition); \
    abort (); \
  } \
} while (0)

#define EVENT_CAPACITY 64U

typedef enum {
  EventRole = 1,
  EventRead,
  EventWrite,
  EventDelay
} EVENT_TYPE;

typedef struct {
  EVENT_TYPE Type;
  MTK_USB_HUB_GPIO Gpio;
  uint32_t Value;
} EVENT;

static unsigned Checks;
static EVENT Events[EVENT_CAPACITY];
static unsigned EventCount;
static unsigned RoleCalls;
static unsigned ReadCalls;
static unsigned WriteCalls;
static unsigned DelayCalls;
static unsigned FailRoleAt;
static unsigned FailReadAt;
static unsigned FailWriteAt;
static unsigned FailDelayAt;
static MTK_USB_HUB_STATUS RoleFailure = MtkUsbHubRoleError;
static MTK_USB_HUB_STATUS ReadFailure = MtkUsbHubGpioError;
static MTK_USB_HUB_STATUS WriteFailure = MtkUsbHubGpioError;
static MTK_USB_HUB_STATUS DelayFailure = MtkUsbHubDelayError;
static uint8_t IdSamples[EVENT_CAPACITY];
static unsigned IdSampleCount;
static unsigned IdSampleIndex;
static uint8_t OutputValues[MtkUsbHubGpioCount];

static void
Record(EVENT_TYPE Type, MTK_USB_HUB_GPIO Gpio, uint32_t Value)
{
  CHECK (EventCount < EVENT_CAPACITY);
  Events[EventCount].Type = Type;
  Events[EventCount].Gpio = Gpio;
  Events[EventCount].Value = Value;
  ++EventCount;
}

static MTK_USB_HUB_STATUS
FakeReadGpio(void *Context, MTK_USB_HUB_GPIO Gpio, uint8_t *Value)
{
  uint8_t Sample;

  CHECK (Context == (void *)(uintptr_t)0x70U);
  CHECK (Gpio == MtkUsbHubGpioId);
  CHECK (Value != NULL);
  ++ReadCalls;
  if (FailReadAt == ReadCalls) {
    Record (EventRead, Gpio, 0xffU);
    return ReadFailure;
  }
  if (IdSampleCount == 0U) {
    Sample = 0U;
  } else if (IdSampleIndex < IdSampleCount) {
    Sample = IdSamples[IdSampleIndex++];
  } else {
    Sample = IdSamples[IdSampleCount - 1U];
  }
  *Value = Sample;
  Record (EventRead, Gpio, Sample);
  return MtkUsbHubSuccess;
}

static MTK_USB_HUB_STATUS
FakeCheckHostRole(void *Context)
{
  CHECK (Context == (void *)(uintptr_t)0x70U);
  ++RoleCalls;
  Record (EventRole, MtkUsbHubGpioId, 0U);
  if (FailRoleAt == RoleCalls) return RoleFailure;
  return MtkUsbHubSuccess;
}

static MTK_USB_HUB_STATUS
FakeWriteGpio(void *Context, MTK_USB_HUB_GPIO Gpio, uint8_t Value)
{
  CHECK (Context == (void *)(uintptr_t)0x70U);
  CHECK ((Gpio > MtkUsbHubGpioId) && (Gpio < MtkUsbHubGpioCount));
  CHECK (Value <= 1U);
  ++WriteCalls;
  Record (EventWrite, Gpio, Value);
  if (FailWriteAt == WriteCalls) return WriteFailure;
  OutputValues[Gpio] = Value;
  return MtkUsbHubSuccess;
}

static MTK_USB_HUB_STATUS
FakeDelayUs(void *Context, uint32_t Microseconds)
{
  CHECK (Context == (void *)(uintptr_t)0x70U);
  ++DelayCalls;
  Record (EventDelay, MtkUsbHubGpioId, Microseconds);
  if (FailDelayAt == DelayCalls) return DelayFailure;
  return MtkUsbHubSuccess;
}

static const MTK_USB_HUB_IO FakeIo = {
  .Context = (void *)(uintptr_t)0x70U,
  .ReadGpio = FakeReadGpio,
  .CheckHostRole = FakeCheckHostRole,
  .WriteGpio = FakeWriteGpio,
  .DelayUs = FakeDelayUs
};

static void
ResetHarness(void)
{
  EventCount = RoleCalls = ReadCalls = WriteCalls = DelayCalls = 0U;
  FailRoleAt = FailReadAt = FailWriteAt = FailDelayAt = 0U;
  IdSampleCount = IdSampleIndex = 0U;
  memset (Events, 0, sizeof (Events));
  memset (IdSamples, 0, sizeof (IdSamples));
  memset (OutputValues, 0, sizeof (OutputValues));
}

static void
SetIdSequence(const uint8_t *Samples, unsigned Count)
{
  CHECK (Count <= EVENT_CAPACITY);
  IdSampleCount = Count;
  IdSampleIndex = 0U;
  if (Count != 0U) memcpy (IdSamples, Samples, Count);
}

static void
ExpectEvent(
  unsigned Index,
  EVENT_TYPE Type,
  MTK_USB_HUB_GPIO Gpio,
  uint32_t Value
  )
{
  CHECK (Index < EventCount);
  CHECK (Events[Index].Type == Type);
  CHECK (Events[Index].Gpio == Gpio);
  CHECK (Events[Index].Value == Value);
}

static void
InitPrepared(MTK_USB_HUB_CORE *Core)
{
  memset (Core, 0, sizeof (*Core));
  CHECK (MtkUsbHubCoreInitialize (Core, &FakeIo) == MtkUsbHubSuccess);
  CHECK (MtkUsbHubCorePrepareHost (Core) == MtkUsbHubSuccess);
  CHECK (Core->State == MTK_USB_HUB_STATE_PREPARED);
  CHECK (Core->HostPrepared == 1U);
  CHECK (WriteCalls == 0U && DelayCalls == 0U);
}

static void
CheckExactOnOffOrder(void)
{
  MTK_USB_HUB_CORE Core;
  unsigned Start;

  ResetHarness ();
  InitPrepared (&Core);
  EventCount = 0U;
  RoleCalls = ReadCalls = 0U;

  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubSuccess);
  CHECK (Core.State == MTK_USB_HUB_STATE_ON);
  CHECK (MtkUsbHubCoreIsOn (&Core) != 0U);
  CHECK (Core.OwnedMask ==
         ((1U << MtkUsbHubGpioMux) | (1U << MtkUsbHubGpioUsb5v) |
          (1U << MtkUsbHubGpioReset)));
  CHECK (Core.KnownMask == Core.OwnedMask);
  CHECK (Core.ValueMask == Core.OwnedMask);
  CHECK (EventCount == 14U);
  ExpectEvent (0U, EventRole, MtkUsbHubGpioId, 0U);
  ExpectEvent (1U, EventRead, MtkUsbHubGpioId, 0U);
  ExpectEvent (2U, EventRead, MtkUsbHubGpioId, 0U);
  ExpectEvent (3U, EventWrite, MtkUsbHubGpioMux, 1U);
  ExpectEvent (4U, EventDelay, MtkUsbHubGpioId, 1000U);
  ExpectEvent (5U, EventRole, MtkUsbHubGpioId, 0U);
  ExpectEvent (6U, EventRead, MtkUsbHubGpioId, 0U);
  ExpectEvent (7U, EventRead, MtkUsbHubGpioId, 0U);
  ExpectEvent (8U, EventWrite, MtkUsbHubGpioUsb5v, 1U);
  ExpectEvent (9U, EventDelay, MtkUsbHubGpioId, 1000U);
  ExpectEvent (10U, EventRole, MtkUsbHubGpioId, 0U);
  ExpectEvent (11U, EventRead, MtkUsbHubGpioId, 0U);
  ExpectEvent (12U, EventRead, MtkUsbHubGpioId, 0U);
  ExpectEvent (13U, EventWrite, MtkUsbHubGpioReset, 1U);

  Start = EventCount;
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubSuccess);
  CHECK (EventCount == Start + 3U);
  CHECK (WriteCalls == 3U && DelayCalls == 2U);

  EventCount = 0U;
  CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == MtkUsbHubSuccess);
  CHECK (Core.State == MTK_USB_HUB_STATE_OFF);
  CHECK (MtkUsbHubCoreIsOn (&Core) == 0U);
  CHECK (Core.KnownMask == Core.OwnedMask);
  CHECK (Core.ValueMask == 0U);
  CHECK (EventCount == 3U);
  ExpectEvent (0U, EventWrite, MtkUsbHubGpioReset, 0U);
  ExpectEvent (1U, EventWrite, MtkUsbHubGpioUsb5v, 0U);
  ExpectEvent (2U, EventWrite, MtkUsbHubGpioMux, 0U);

  Start = EventCount;
  CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == MtkUsbHubSuccess);
  CHECK (EventCount == Start);
}

static void
CheckPrepareGatesAndBoundedIdSamples(void)
{
  MTK_USB_HUB_CORE Core;
  const uint8_t Unstable[] = {0U, 1U};
  const uint8_t Deasserted[] = {1U, 1U};

  ResetHarness ();
  memset (&Core, 0, sizeof (Core));
  CHECK (MtkUsbHubCoreInitialize (&Core, &FakeIo) == MtkUsbHubSuccess);
  SetIdSequence (Unstable, sizeof (Unstable));
  CHECK (MtkUsbHubCorePrepareHost (&Core) == MtkUsbHubIdUnstable);
  CHECK (Core.HostPrepared == 0U && WriteCalls == 0U);

  ResetHarness ();
  memset (&Core, 0, sizeof (Core));
  CHECK (MtkUsbHubCoreInitialize (&Core, &FakeIo) == MtkUsbHubSuccess);
  SetIdSequence (Deasserted, sizeof (Deasserted));
  CHECK (MtkUsbHubCorePrepareHost (&Core) == MtkUsbHubNotReady);
  CHECK (Core.HostPrepared == 0U && WriteCalls == 0U);

  ResetHarness ();
  memset (&Core, 0, sizeof (Core));
  CHECK (MtkUsbHubCoreInitialize (&Core, &FakeIo) == MtkUsbHubSuccess);
  FailRoleAt = 1U;
  CHECK (MtkUsbHubCorePrepareHost (&Core) == RoleFailure);
  CHECK (Core.HostPrepared == 0U && ReadCalls == 0U);

  ResetHarness ();
  memset (&Core, 0, sizeof (Core));
  CHECK (MtkUsbHubCoreInitialize (&Core, &FakeIo) == MtkUsbHubSuccess);
  FailReadAt = 1U;
  CHECK (MtkUsbHubCorePrepareHost (&Core) == ReadFailure);
  CHECK (Core.HostPrepared == 0U && WriteCalls == 0U);

  ResetHarness ();
  InitPrepared (&Core);
  FailRoleAt = RoleCalls + 1U;
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == RoleFailure);
  CHECK (Core.State == MTK_USB_HUB_STATE_PREPARED);
  CHECK (WriteCalls == 0U);
}

static void
CheckOnWriteFailures(void)
{
  unsigned Failure;

  for (Failure = 1U; Failure <= 3U; ++Failure) {
    MTK_USB_HUB_CORE Core;
    uint8_t ExpectedOwned = 0U;
    uint8_t ExpectedKnown = 0U;

    ResetHarness ();
    InitPrepared (&Core);
    FailWriteAt = Failure;
    CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == WriteFailure);
    CHECK (Core.State == MTK_USB_HUB_STATE_QUARANTINED);
    CHECK (Core.LastStatus == WriteFailure);
    CHECK (MtkUsbHubCoreIsOn (&Core) == 0U);
    CHECK (WriteCalls == Failure);
    CHECK (DelayCalls == (Failure > 1U ? 1U : 0U) +
                         (Failure > 2U ? 1U : 0U));
    ExpectedOwned |= (uint8_t)(1U << MtkUsbHubGpioMux);
    if (Failure > 1U) {
      ExpectedKnown |= (uint8_t)(1U << MtkUsbHubGpioMux);
      ExpectedOwned |= (uint8_t)(1U << MtkUsbHubGpioUsb5v);
    }
    if (Failure > 2U) {
      ExpectedKnown |= (uint8_t)(1U << MtkUsbHubGpioUsb5v);
      ExpectedOwned |= (uint8_t)(1U << MtkUsbHubGpioReset);
    }
    CHECK (Core.OwnedMask == ExpectedOwned);
    CHECK (Core.KnownMask == ExpectedKnown);
    CHECK (Core.ValueMask == ExpectedKnown);
    CHECK ((Core.KnownMask & (1U << (Failure == 1U ? MtkUsbHubGpioMux :
                (Failure == 2U ? MtkUsbHubGpioUsb5v : MtkUsbHubGpioReset)))) == 0U);
    CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubQuarantined);

    FailWriteAt = 0U;
    {
      unsigned Start = EventCount;
      CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == MtkUsbHubSuccess);
      CHECK (Core.State == MTK_USB_HUB_STATE_OFF);
      CHECK (Core.KnownMask == Core.OwnedMask);
      CHECK (Core.ValueMask == 0U);
      CHECK (EventCount == Start + 3U);
    }
  }
}

static void
CheckOnDelayFailures(void)
{
  unsigned Failure;

  for (Failure = 1U; Failure <= 2U; ++Failure) {
    MTK_USB_HUB_CORE Core;

    ResetHarness ();
    InitPrepared (&Core);
    FailDelayAt = Failure;
    DelayFailure = (Failure == 2U) ? MtkUsbHubTimeout : MtkUsbHubDelayError;
    CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == DelayFailure);
    CHECK (Core.State == MTK_USB_HUB_STATE_QUARANTINED);
    CHECK (Core.LastStatus == DelayFailure);
    CHECK (MtkUsbHubCoreIsOn (&Core) == 0U);
    CHECK (DelayCalls == Failure);
    CHECK (WriteCalls == Failure);
    CHECK ((Core.OwnedMask & (1U << MtkUsbHubGpioMux)) != 0U);
    CHECK ((Core.OwnedMask & (1U << MtkUsbHubGpioUsb5v)) ==
           (Failure == 2U ? (1U << MtkUsbHubGpioUsb5v) : 0U));
    CHECK ((Core.OwnedMask & (1U << MtkUsbHubGpioReset)) == 0U);
    CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubQuarantined);
  }
  DelayFailure = MtkUsbHubDelayError;
}

static void
CheckOffWriteFailures(void)
{
  unsigned Failure;

  for (Failure = 1U; Failure <= 3U; ++Failure) {
    MTK_USB_HUB_CORE Core;
    uint8_t FailedBit;
    unsigned Start;

    ResetHarness ();
    InitPrepared (&Core);
    CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubSuccess);
    FailedBit = (uint8_t)(1U << (Failure == 1U ? MtkUsbHubGpioReset :
                 (Failure == 2U ? MtkUsbHubGpioUsb5v : MtkUsbHubGpioMux)));
    FailWriteAt = WriteCalls + Failure;
    Start = EventCount;
    CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == WriteFailure);
    CHECK (Core.State == MTK_USB_HUB_STATE_QUARANTINED);
    CHECK (Core.LastStatus == WriteFailure);
    CHECK (WriteCalls == 3U + Failure);
    CHECK (EventCount == Start + Failure);
    CHECK (Core.OwnedMask ==
           ((1U << MtkUsbHubGpioMux) | (1U << MtkUsbHubGpioUsb5v) |
            (1U << MtkUsbHubGpioReset)));
    CHECK (Core.KnownMask == (Core.OwnedMask & (uint8_t)~FailedBit));
    CHECK ((Core.ValueMask & FailedBit) == 0U);
    CHECK (Core.ValueMask ==
           (Failure == 1U ?
             ((1U << MtkUsbHubGpioMux) | (1U << MtkUsbHubGpioUsb5v)) :
             (Failure == 2U ? (1U << MtkUsbHubGpioMux) : 0U)));
    CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubQuarantined);

    FailWriteAt = 0U;
    Start = EventCount;
    CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == MtkUsbHubSuccess);
    CHECK (Core.State == MTK_USB_HUB_STATE_OFF);
    CHECK (Core.KnownMask == Core.OwnedMask);
    CHECK (Core.ValueMask == 0U);
    CHECK (EventCount == Start + 3U);
  }
}

static void
CheckReadFailureAfterMux(void)
{
  MTK_USB_HUB_CORE Core;

  ResetHarness ();
  InitPrepared (&Core);
  /* Two preparation reads, two entry checks, then fail the first post-delay read. */
  FailReadAt = ReadCalls + 3U;
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == ReadFailure);
  CHECK (Core.State == MTK_USB_HUB_STATE_QUARANTINED);
  CHECK (Core.LastStatus == ReadFailure);
  CHECK (WriteCalls == 1U && DelayCalls == 1U);
  CHECK (Core.OwnedMask == (1U << MtkUsbHubGpioMux));
  CHECK (Core.KnownMask == Core.OwnedMask);
  CHECK (OutputValues[MtkUsbHubGpioMux] == 1U);
  CHECK (OutputValues[MtkUsbHubGpioUsb5v] == 0U);
}

static void
CheckIdAndRoleChangeAfterMux(void)
{
  MTK_USB_HUB_CORE Core;
  const uint8_t IdChange[] = {0U, 0U, 0U, 1U};
  unsigned Start;

  ResetHarness ();
  InitPrepared (&Core);
  SetIdSequence (IdChange, sizeof (IdChange));
  /* PrepareHost already consumed two samples. */
  Start = EventCount;
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubIdUnstable);
  CHECK (Core.State == MTK_USB_HUB_STATE_QUARANTINED);
  CHECK (Core.OwnedMask == (1U << MtkUsbHubGpioMux));
  CHECK (Core.KnownMask == Core.OwnedMask);
  CHECK (WriteCalls == 1U && DelayCalls == 1U);
  CHECK (EventCount > Start);
  CHECK (OutputValues[MtkUsbHubGpioMux] == 1U);
  CHECK (OutputValues[MtkUsbHubGpioUsb5v] == 0U);
  CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == MtkUsbHubSuccess);

  ResetHarness ();
  InitPrepared (&Core);
  FailRoleAt = RoleCalls + 2U;
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == RoleFailure);
  CHECK (Core.State == MTK_USB_HUB_STATE_QUARANTINED);
  CHECK (WriteCalls == 1U && DelayCalls == 1U);
  CHECK (OutputValues[MtkUsbHubGpioMux] == 1U);
  CHECK (OutputValues[MtkUsbHubGpioUsb5v] == 0U);
}

static void
CheckPreparedOwnershipAndPhysicalOnMeaning(void)
{
  MTK_USB_HUB_CORE Core;
  const uint8_t IdHigh[] = {1U, 1U};
  uint8_t SavedOwned;
  uint8_t SavedKnown;
  uint8_t SavedValue;
  unsigned SavedWrites;

  ResetHarness ();
  InitPrepared (&Core);
  FailRoleAt = RoleCalls + 1U;
  CHECK (MtkUsbHubCorePrepareHost (&Core) == RoleFailure);
  CHECK (Core.HostPrepared == 1U);
  CHECK (Core.State == MTK_USB_HUB_STATE_PREPARED);
  FailRoleAt = 0U;
  CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == MtkUsbHubSuccess);
  CHECK (Core.State == MTK_USB_HUB_STATE_OFF);

  ResetHarness ();
  InitPrepared (&Core);
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubSuccess);
  SavedOwned = Core.OwnedMask;
  SavedKnown = Core.KnownMask;
  SavedValue = Core.ValueMask;
  SavedWrites = WriteCalls;
  SetIdSequence (IdHigh, sizeof (IdHigh));
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubNotReady);
  CHECK (MtkUsbHubCoreIsOn (&Core) != 0U);
  CHECK (Core.OwnedMask == SavedOwned);
  CHECK (Core.KnownMask == SavedKnown);
  CHECK (Core.ValueMask == SavedValue);
  CHECK (WriteCalls == SavedWrites);

  SetIdSequence (NULL, 0U);
  FailRoleAt = RoleCalls + 1U;
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == RoleFailure);
  CHECK (MtkUsbHubCoreIsOn (&Core) != 0U);
  CHECK (Core.OwnedMask == SavedOwned && Core.KnownMask == SavedKnown);
  CHECK (Core.ValueMask == SavedValue && WriteCalls == SavedWrites);
  FailRoleAt = 0U;
  CHECK (MtkUsbHubCoreSetHub (&Core, 0U) == MtkUsbHubSuccess);
  CHECK (Core.State == MTK_USB_HUB_STATE_OFF);
}

static void
CheckInvalidApiAndNoWriteInit(void)
{
  MTK_USB_HUB_CORE Core;
  MTK_USB_HUB_CORE BadCore;
  MTK_USB_HUB_IO Missing;

  ResetHarness ();
  memset (&Core, 0, sizeof (Core));
  CHECK (MtkUsbHubCoreInitialize (&Core, &FakeIo) == MtkUsbHubSuccess);
  CHECK (EventCount == 0U && WriteCalls == 0U && DelayCalls == 0U);
  CHECK (MtkUsbHubCoreSetHub (&Core, 2U) == MtkUsbHubInvalid);
  CHECK (MtkUsbHubCoreSetHub (&Core, 1U) == MtkUsbHubNotReady);

  memset (&BadCore, 0, sizeof (BadCore));
  Missing = FakeIo;
  Missing.ReadGpio = NULL;
  CHECK (MtkUsbHubCoreInitialize (&BadCore, &Missing) == MtkUsbHubNotReady);
  CHECK (EventCount == 0U);
}

int
main(void)
{
  CheckInvalidApiAndNoWriteInit ();
  CheckPrepareGatesAndBoundedIdSamples ();
  CheckExactOnOffOrder ();
  CheckOnWriteFailures ();
  CheckOnDelayFailures ();
  CheckOffWriteFailures ();
  CheckReadFailureAfterMux ();
  CheckIdAndRoleChangeAfterMux ();
  CheckPreparedOwnershipAndPhysicalOnMeaning ();
  printf ("MtkUsbHubCoreTest checks: %u\n", Checks);
  return 0;
}
