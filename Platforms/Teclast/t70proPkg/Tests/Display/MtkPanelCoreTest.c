// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned Checks;
#define CHECK(x) do { Checks++; if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#include <Library/MtkPanelCore.h>

typedef enum {
  TraceMode = 1,
  TraceDirection,
  TraceValue,
  TraceDelay
} TRACE_KIND;

typedef struct {
  TRACE_KIND Kind;
  uint32_t Pin;
  uint32_t Value;
} TRACE_ENTRY;

typedef struct {
  MTK_PANEL_CORE *Core;
  uint32_t Mode[256];
  uint32_t Direction[256];
  uint32_t OutputLatch[256];
  TRACE_ENTRY Trace[256];
  size_t TraceCount;
  uint32_t ReadCalls;
  uint32_t ValidateCalls;
  uint32_t ModeWrites;
  uint32_t DirectionWrites;
  uint32_t ValueWrites;
  uint32_t ModeWritesByPin[256];
  uint32_t ValueWritesByPin[256];
  uint32_t DelayCalls;
  uint32_t FailModePin;
  uint32_t FailValuePin;
  uint8_t FailModeNoEffectOnce;
  uint8_t FailReadAfterValueOnce;
  uint8_t FailValueAfterApplyOnce;
  uint8_t FailNextRead;
  uint8_t FailDelayOnce;
  uint8_t DenyPin152;
  uint8_t ReenterOnce;
  MTK_PANEL_STATUS ReentrantStatus;
} PANEL_MOCK;

#define EXPECT_MODE(PinValue) { TraceMode, (PinValue), MTK_PANEL_GPIO_MODE_GPIO }
#define EXPECT_DIR(PinValue)  { TraceDirection, (PinValue), MTK_PANEL_GPIO_DIRECTION_OUTPUT }
#define EXPECT_VALUE(PinValue, Level) { TraceValue, (PinValue), (Level) }
#define EXPECT_DELAY(Usec) { TraceDelay, 0U, (Usec) }

static const TRACE_ENTRY mExpectedInit[] = {
  EXPECT_MODE (43U), EXPECT_DIR (43U), EXPECT_VALUE (43U, 1U),
  EXPECT_MODE (3U), EXPECT_DIR (3U), EXPECT_VALUE (3U, 1U),
  EXPECT_DELAY (10000U),
  EXPECT_MODE (155U), EXPECT_DIR (155U), EXPECT_VALUE (155U, 1U),
  EXPECT_DELAY (5000U),
  EXPECT_MODE (49U), EXPECT_DIR (49U), EXPECT_VALUE (49U, 1U),
  EXPECT_DELAY (20000U),
  EXPECT_MODE (152U), EXPECT_DIR (152U), EXPECT_VALUE (152U, 1U),
  EXPECT_MODE (153U), EXPECT_DIR (153U), EXPECT_VALUE (153U, 1U),
  EXPECT_MODE (36U), EXPECT_DIR (36U), EXPECT_VALUE (36U, 1U),
  EXPECT_DELAY (50000U),
  EXPECT_MODE (43U), EXPECT_DIR (43U), EXPECT_VALUE (43U, 0U),
  EXPECT_DELAY (100000U), EXPECT_DELAY (100000U),
  EXPECT_MODE (15U), EXPECT_DIR (15U), EXPECT_VALUE (15U, 1U),
  EXPECT_DELAY (50000U)
};

static const TRACE_ENTRY mExpectedSuspend[] = {
  EXPECT_MODE (15U), EXPECT_DIR (15U), EXPECT_VALUE (15U, 0U),
  EXPECT_DELAY (10000U),
  EXPECT_MODE (37U), EXPECT_DIR (37U), EXPECT_VALUE (37U, 0U),
  EXPECT_MODE (43U), EXPECT_DIR (43U), EXPECT_VALUE (43U, 0U),
  EXPECT_DELAY (1000U), EXPECT_DELAY (1000U),
  EXPECT_MODE (37U), EXPECT_DIR (37U), EXPECT_VALUE (37U, 0U),
  EXPECT_MODE (153U), EXPECT_DIR (153U), EXPECT_VALUE (153U, 0U),
  EXPECT_DELAY (1000U),
  EXPECT_MODE (49U), EXPECT_DIR (49U), EXPECT_VALUE (49U, 0U),
  EXPECT_DELAY (1000U), EXPECT_DELAY (1000U),
  EXPECT_MODE (3U), EXPECT_DIR (3U), EXPECT_VALUE (3U, 0U)
};

static void
Record (
  PANEL_MOCK *Mock,
  TRACE_KIND Kind,
  uint32_t Pin,
  uint32_t Value
  )
{
  CHECK (Mock->TraceCount < (sizeof (Mock->Trace) / sizeof (Mock->Trace[0])));
  Mock->Trace[Mock->TraceCount].Kind = Kind;
  Mock->Trace[Mock->TraceCount].Pin = Pin;
  Mock->Trace[Mock->TraceCount].Value = Value;
  Mock->TraceCount++;
}

static MTK_PANEL_STATUS
MockValidateGpio (
  void *Context,
  uint32_t Pin,
  MTK_PANEL_GPIO_ROLE Role
  )
{
  PANEL_MOCK *Mock = (PANEL_MOCK *)Context;
  Mock->ValidateCalls++;
  if ((Pin == MTK_PANEL_GPIO_LK_UNNAMED_152) &&
      (Role == MtkPanelGpioLkUnnamed152) && (Mock->DenyPin152 != 0U)) {
    return MtkPanelMappingError;
  }

  return MtkPanelSuccess;
}

static MTK_PANEL_STATUS
MockReadGpio (
  void *Context,
  uint32_t Pin,
  uint32_t *Mode,
  uint32_t *Direction,
  uint32_t *OutputLatch
  )
{
  PANEL_MOCK *Mock = (PANEL_MOCK *)Context;
  Mock->ReadCalls++;
  if (Mock->FailNextRead != 0U) {
    Mock->FailNextRead = 0U;
    return MtkPanelIoError;
  }

  CHECK (Pin < 256U);
  *Mode = Mock->Mode[Pin];
  *Direction = Mock->Direction[Pin];
  *OutputLatch = Mock->OutputLatch[Pin];
  return MtkPanelSuccess;
}

static MTK_PANEL_STATUS
MockSetMode (
  void *Context,
  uint32_t Pin,
  uint32_t Mode
  )
{
  PANEL_MOCK *Mock = (PANEL_MOCK *)Context;
  Mock->ModeWrites++;
  CHECK (Pin < 256U);
  Mock->ModeWritesByPin[Pin]++;
  Record (Mock, TraceMode, Pin, Mode);
  if ((Mock->ReenterOnce != 0U) && (Mock->ModeWrites == 1U)) {
    Mock->ReenterOnce = 0U;
    Mock->ReentrantStatus = MtkPanelCoreRunLkInit (Mock->Core);
  }

  if ((Mock->FailModeNoEffectOnce != 0U) &&
      (Pin == Mock->FailModePin)) {
    Mock->FailModeNoEffectOnce = 0U;
    return MtkPanelIoError;
  }

  CHECK (Pin < 256U);
  Mock->Mode[Pin] = Mode;
  return MtkPanelSuccess;
}

static MTK_PANEL_STATUS
MockSetDirection (
  void *Context,
  uint32_t Pin,
  uint32_t Direction
  )
{
  PANEL_MOCK *Mock = (PANEL_MOCK *)Context;
  Mock->DirectionWrites++;
  Record (Mock, TraceDirection, Pin, Direction);
  CHECK (Pin < 256U);
  Mock->Direction[Pin] = Direction;
  return MtkPanelSuccess;
}

static MTK_PANEL_STATUS
MockSetValue (
  void *Context,
  uint32_t Pin,
  uint32_t Value
  )
{
  PANEL_MOCK *Mock = (PANEL_MOCK *)Context;
  Mock->ValueWrites++;
  CHECK (Pin < 256U);
  Mock->ValueWritesByPin[Pin]++;
  Record (Mock, TraceValue, Pin, Value);
  Mock->OutputLatch[Pin] = Value;
  if ((Mock->FailReadAfterValueOnce != 0U) &&
      (Pin == Mock->FailValuePin)) {
    Mock->FailReadAfterValueOnce = 0U;
    Mock->FailNextRead = 1U;
  }
  if ((Mock->FailValueAfterApplyOnce != 0U) &&
      (Pin == Mock->FailValuePin)) {
    Mock->FailValueAfterApplyOnce = 0U;
    Mock->FailNextRead = 1U;
    return MtkPanelIoError;
  }

  return MtkPanelSuccess;
}

static MTK_PANEL_STATUS
MockDelayUs (
  void *Context,
  uint32_t Microseconds
  )
{
  PANEL_MOCK *Mock = (PANEL_MOCK *)Context;
  Mock->DelayCalls++;
  Record (Mock, TraceDelay, 0U, Microseconds);
  if (Mock->FailDelayOnce != 0U) {
    Mock->FailDelayOnce = 0U;
    return MtkPanelDelayError;
  }

  return MtkPanelSuccess;
}

static MTK_PANEL_IO
MakeIo (
  PANEL_MOCK *Mock
  )
{
  MTK_PANEL_IO Io;
  Io.Context = Mock;
  Io.ValidateGpio = MockValidateGpio;
  Io.ReadGpio = MockReadGpio;
  Io.SetMode = MockSetMode;
  Io.SetDirection = MockSetDirection;
  Io.SetValue = MockSetValue;
  Io.DelayUs = MockDelayUs;
  return Io;
}

static void
CheckTrace (
  const PANEL_MOCK *Mock,
  const TRACE_ENTRY *Expected,
  size_t ExpectedCount
  )
{
  size_t Index;
  CHECK (Mock->TraceCount == ExpectedCount);
  for (Index = 0; Index < ExpectedCount; ++Index) {
    CHECK (Mock->Trace[Index].Kind == Expected[Index].Kind);
    CHECK (Mock->Trace[Index].Pin == Expected[Index].Pin);
    CHECK (Mock->Trace[Index].Value == Expected[Index].Value);
  }
}

static void
CheckKernelLevels (
  const PANEL_MOCK *Mock,
  unsigned Powered,
  unsigned Enabled
  )
{
  static const uint32_t Rails[] = { 3U, 155U, 49U, 153U, 36U, 15U };
  unsigned Index;
  for (Index = 0; Index < sizeof (Rails) / sizeof (Rails[0]); Index++) {
    CHECK (Mock->Mode[Rails[Index]] == 0U);
    CHECK (Mock->Direction[Rails[Index]] == 1U);
    CHECK (Mock->OutputLatch[Rails[Index]] == Powered);
  }
  CHECK (Mock->OutputLatch[43] == 0U);
  CHECK (Mock->OutputLatch[37] == Enabled);
  /* The kernel module does not change the touch reset line. */
  CHECK (Mock->OutputLatch[152] == 1U);
}

static void
TestKernelPowerCycles (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);
  unsigned Cycle;
  size_t Count;
  size_t Index;
  uint32_t TotalDelay;

  Mock.Core = &Core;
  Mock.OutputLatch[152] = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (MtkPanelCoreRunKernelEnable (&Core) == MtkPanelNotReady);
  CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelNotReady);
  CHECK (Mock.TraceCount == 0U);
  for (Cycle = 0; Cycle < 3U; Cycle++) {
    Mock.TraceCount = 0U;
    CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelSuccess);
    CHECK (Core.State == MtkPanelStateKernelPrepared);
    CheckKernelLevels (&Mock, 1U, 0U);
    Count = Mock.TraceCount;
    CHECK (Count == 25U);
    /* New output levels are preloaded before changing pin direction. */
    for (Index = 0U; Index < Count; Index++) {
      if (Mock.Trace[Index].Kind == TraceDirection) {
        CHECK (Index > 0U);
        CHECK (Mock.Trace[Index - 1U].Kind == TraceValue);
        CHECK (Mock.Trace[Index - 1U].Pin == Mock.Trace[Index].Pin);
      }
    }
    CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelSuccess);
    CHECK (Mock.TraceCount == Count);
    CHECK (!MtkPanelCoreLkInitComplete (&Core));
    CHECK (MtkPanelCoreRunKernelEnable (&Core) == MtkPanelSuccess);
    CHECK (Core.State == MtkPanelStateKernelEnabled);
    CHECK (Mock.Trace[Count].Kind == TraceDelay);
    CHECK (Mock.Trace[Count].Value == 500000U);
    CheckKernelLevels (&Mock, 1U, 1U);
    Count = Mock.TraceCount;
    CHECK (MtkPanelCoreRunKernelEnable (&Core) == MtkPanelSuccess);
    CHECK (Mock.TraceCount == Count);
    CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelNotReady);
    Mock.TraceCount = 0U;
    CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelSuccess);
    CHECK (Core.State == MtkPanelStateKernelUnprepared);
    CheckKernelLevels (&Mock, 0U, 0U);
    CHECK (Mock.OutputLatch[37] == 0U);
    CHECK (Mock.Trace[Mock.TraceCount - 1U].Kind == TraceDelay);
    CHECK (Mock.Trace[Mock.TraceCount - 1U].Value == 510000U);
    TotalDelay = 0U;
    for (Index = 0U; Index < Mock.TraceCount; Index++) {
      if (Mock.Trace[Index].Kind == TraceDelay) {
        TotalDelay += Mock.Trace[Index].Value;
      }
    }
    CHECK (TotalDelay == 595000U);
    Count = Mock.TraceCount;
    CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelSuccess);
    CHECK (Mock.TraceCount == Count);
    CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelNotReady);
  }
}

static void
TestKernelUncertainActions (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);
  uint32_t ModeWrites;
  uint32_t ValueWrites;
  uint32_t DelayCalls;

  Mock.Core = &Core;
  Mock.OutputLatch[152] = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  Mock.FailReadAfterValueOnce = 1U;
  Mock.FailValuePin = 43U;
  CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelIoError);
  CHECK (Core.State == MtkPanelStateQuarantined);
  CHECK (Core.PendingOperation == MtkPanelOperationKernelPrepare);
  CHECK (Core.ActionIndex == 1U);
  CHECK (Mock.OutputLatch[43] == 1U);
  CHECK (Mock.Direction[43] == 0U);
  CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelQuarantined);
  ModeWrites = Mock.ModeWritesByPin[43];
  ValueWrites = Mock.ValueWritesByPin[43];
  CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelSuccess);
  /* GPIO43 occurs again at the end, but the uncertain first write is not
   * issued twice. Both mode and value are written exactly twice in prepare. */
  CHECK (ModeWrites == 1U && Mock.ModeWritesByPin[43] == 2U);
  CHECK (ValueWrites == 1U && Mock.ValueWritesByPin[43] == 2U);
  Mock.FailDelayOnce = 1U;
  ModeWrites = Mock.ModeWrites;
  ValueWrites = Mock.ValueWrites;
  CHECK (MtkPanelCoreRunKernelEnable (&Core) == MtkPanelDelayError);
  CHECK (Core.PendingOperation == MtkPanelOperationKernelEnable);
  CHECK (Core.ActionIndex == 0U);
  CHECK (Mock.ModeWrites == ModeWrites && Mock.ValueWrites == ValueWrites);
  CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelQuarantined);
  CHECK (MtkPanelCoreRunKernelEnable (&Core) == MtkPanelSuccess);
  Mock.FailDelayOnce = 1U;
  CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelDelayError);
  CHECK (Core.PendingOperation == MtkPanelOperationKernelUnprepare);
  CHECK (Mock.OutputLatch[37] == 0U);
  ValueWrites = Mock.ValueWritesByPin[37];
  DelayCalls = Mock.DelayCalls;
  CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelQuarantined);
  CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelSuccess);
  CHECK (Mock.ValueWritesByPin[37] == ValueWrites);
  CHECK (Mock.DelayCalls == DelayCalls + 6U);
  CheckKernelLevels (&Mock, 0U, 0U);
}

static void
TestKernelMappingsBeforeWrites (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);
  Mock.Core = &Core;
  Mock.DenyPin152 = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelMappingError);
  CHECK (Mock.TraceCount == 0U && Core.State == MtkPanelStateIdle);
  Mock.DenyPin152 = 0U;
  CHECK (MtkPanelCoreRunKernelPrepare (&Core) == MtkPanelSuccess);
  Mock.DenyPin152 = 1U;
  CHECK (MtkPanelCoreRunKernelEnable (&Core) == MtkPanelMappingError);
  CHECK (Core.State == MtkPanelStateKernelPrepared);
  CHECK (MtkPanelCoreRunKernelUnprepare (&Core) == MtkPanelMappingError);
  CHECK (Core.State == MtkPanelStateKernelPrepared);
}

static void
TestOriginalLkSequences (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);

  Mock.Core = &Core;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (Core.State == MtkPanelStateIdle);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelSuccess);
  CheckTrace (&Mock, mExpectedInit, sizeof (mExpectedInit) / sizeof (mExpectedInit[0]));
  CHECK (MtkPanelCoreLkInitComplete (&Core));
  CHECK (Core.State == MtkPanelStateLkInitComplete);
  CHECK (Mock.ValidateCalls == 9U);
  CHECK (Mock.Mode[152] == 0U && Mock.Direction[152] == 1U && Mock.OutputLatch[152] == 1U);
  CHECK (Mock.Mode[155] == 0U && Mock.Direction[155] == 1U && Mock.OutputLatch[155] == 1U);
  CHECK (Mock.Mode[36] == 0U && Mock.Direction[36] == 1U && Mock.OutputLatch[36] == 1U);

  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelSuccess);
  CHECK (Mock.TraceCount == sizeof (mExpectedInit) / sizeof (mExpectedInit[0]));
  Mock.TraceCount = 0U;
  CHECK (MtkPanelCoreRunLkSuspend (&Core) == MtkPanelSuccess);
  CheckTrace (&Mock, mExpectedSuspend, sizeof (mExpectedSuspend) / sizeof (mExpectedSuspend[0]));
  CHECK (Core.State == MtkPanelStateLkSuspendComplete);
  CHECK (Mock.OutputLatch[37] == 0U && Mock.OutputLatch[43] == 0U);
  CHECK (Mock.OutputLatch[153] == 0U && Mock.OutputLatch[49] == 0U && Mock.OutputLatch[3] == 0U);
  /* Stock suspend leaves these lines high; it is not full rail-off. */
  CHECK (Mock.OutputLatch[152] == 1U && Mock.OutputLatch[155] == 1U && Mock.OutputLatch[36] == 1U);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelNotReady);
  CHECK (Mock.TraceCount == sizeof (mExpectedSuspend) / sizeof (mExpectedSuspend[0]));
}

static void
TestPin152MustBeMappedBeforeWrites (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);

  Mock.Core = &Core;
  Mock.DenyPin152 = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelMappingError);
  CHECK (Core.State == MtkPanelStateIdle && Core.ActionIndex == 0U);
  CHECK (Mock.ValidateCalls == 6U);
  CHECK (Mock.TraceCount == 0U && Mock.ReadCalls == 0U);

  Mock.DenyPin152 = 0U;
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelSuccess);
  CheckTrace (&Mock, mExpectedInit, sizeof (mExpectedInit) / sizeof (mExpectedInit[0]));
}

static void
TestAppliedWriteWithReadFailureRetriesAtSameAction (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);
  size_t PrefixCount;
  uint32_t PinWrites;

  Mock.Core = &Core;
  Mock.FailValuePin = MTK_PANEL_GPIO_VDD18;
  Mock.FailValueAfterApplyOnce = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelIoError);
  CHECK (Core.State == MtkPanelStateQuarantined);
  CHECK (Core.PendingOperation == MtkPanelOperationLkInit);
  CHECK (Core.ActionIndex == 9U && Core.ActionAttempted == 1U);
  CHECK (Mock.OutputLatch[MTK_PANEL_GPIO_VDD18] == 1U);
  PrefixCount = Mock.TraceCount;
  PinWrites = Mock.ValueWritesByPin[MTK_PANEL_GPIO_VDD18];

  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelSuccess);
  CHECK (Core.State == MtkPanelStateLkInitComplete);
  CHECK (Mock.TraceCount > PrefixCount);
  CHECK (Mock.ValueWritesByPin[MTK_PANEL_GPIO_VDD18] == PinWrites);
  CHECK (Mock.OutputLatch[MTK_PANEL_GPIO_VDD18] == 1U);
  CheckTrace (&Mock, mExpectedInit, sizeof (mExpectedInit) / sizeof (mExpectedInit[0]));
}

static void
TestNoEffectWriteRetry (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);
  TRACE_ENTRY Expected[sizeof (mExpectedInit) / sizeof (mExpectedInit[0]) + 1U];
  size_t Index;
  size_t ExpectedCount = sizeof (mExpectedInit) / sizeof (mExpectedInit[0]);

  Mock.Core = &Core;
  Mock.Mode[MTK_PANEL_GPIO_EDP_RESET] = 3U;
  Mock.FailModePin = MTK_PANEL_GPIO_EDP_RESET;
  Mock.FailModeNoEffectOnce = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelIoError);
  CHECK (Core.State == MtkPanelStateQuarantined && Core.ActionIndex == 0U);
  CHECK (Mock.ModeWrites == 1U && Mock.Mode[MTK_PANEL_GPIO_EDP_RESET] == 3U);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelSuccess);
  CHECK (Core.State == MtkPanelStateLkInitComplete);
  CHECK (Mock.ModeWritesByPin[MTK_PANEL_GPIO_EDP_RESET] == 3U);
  CHECK (Mock.Mode[MTK_PANEL_GPIO_EDP_RESET] == 0U);
  Expected[0] = mExpectedInit[0];
  Expected[1] = mExpectedInit[0];
  for (Index = 1U; Index < ExpectedCount; ++Index) {
    Expected[Index + 1U] = mExpectedInit[Index];
  }
  CheckTrace (&Mock, Expected, ExpectedCount + 1U);
}

static void
TestDelayFailureRetriesWithoutRepeatingGpioWrites (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);
  uint32_t WritesBeforeRetry;
  size_t TraceBeforeRetry;
  TRACE_ENTRY Expected[sizeof (mExpectedInit) / sizeof (mExpectedInit[0]) + 1U];
  size_t Index;
  size_t ExpectedCount = sizeof (mExpectedInit) / sizeof (mExpectedInit[0]);

  Mock.Core = &Core;
  Mock.FailDelayOnce = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelDelayError);
  CHECK (Core.State == MtkPanelStateQuarantined && Core.ActionIndex == 6U);
  CHECK (Mock.DelayCalls == 1U);
  WritesBeforeRetry = Mock.ModeWrites + Mock.DirectionWrites + Mock.ValueWrites;
  TraceBeforeRetry = Mock.TraceCount;

  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelSuccess);
  CHECK (Mock.DelayCalls == 8U);
  CHECK (Mock.ModeWrites + Mock.DirectionWrites + Mock.ValueWrites > WritesBeforeRetry);
  CHECK (Mock.TraceCount > TraceBeforeRetry);
  CHECK (Mock.Trace[6].Kind == TraceDelay && Mock.Trace[6].Value == 10000U);
  CHECK (Mock.Trace[7].Kind == TraceDelay && Mock.Trace[7].Value == 10000U);
  for (Index = 0U; Index <= 6U; ++Index) {
    Expected[Index] = mExpectedInit[Index];
  }
  Expected[7] = mExpectedInit[6];
  for (Index = 7U; Index < ExpectedCount; ++Index) {
    Expected[Index + 1U] = mExpectedInit[Index];
  }
  CheckTrace (&Mock, Expected, ExpectedCount + 1U);
}

static void
TestReentrantCallIsRejected (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);

  Mock.Core = &Core;
  Mock.ReenterOnce = 1U;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelSuccess);
  CHECK (MtkPanelCoreRunLkInit (&Core) == MtkPanelSuccess);
  CHECK (Mock.ReentrantStatus == MtkPanelBusy);
  CheckTrace (&Mock, mExpectedInit, sizeof (mExpectedInit) / sizeof (mExpectedInit[0]));
}

static void
TestInitializationRequiresEveryIoCallback (
  void
  )
{
  MTK_PANEL_CORE Core = { 0 };
  PANEL_MOCK Mock = { 0 };
  MTK_PANEL_IO Io = MakeIo (&Mock);

  Io.ReadGpio = NULL;
  CHECK (MtkPanelCoreInitialize (&Core, &Io) == MtkPanelNotReady);
  CHECK (Core.Signature == 0U && Mock.TraceCount == 0U);
  CHECK (MtkPanelCoreInitialize (NULL, &Io) == MtkPanelInvalid);
}

int
main (
  void
  )
{
  TestOriginalLkSequences ();
  TestKernelPowerCycles ();
  TestKernelUncertainActions ();
  TestKernelMappingsBeforeWrites ();
  TestPin152MustBeMappedBeforeWrites ();
  TestAppliedWriteWithReadFailureRetriesAtSameAction ();
  TestNoEffectWriteRetry ();
  TestDelayFailureRetriesWithoutRepeatingGpioWrites ();
  TestReentrantCallIsRejected ();
  TestInitializationRequiresEveryIoCallback ();
  printf ("Panel core: %u checks passed (LK and shipping panel power, latch-before-output, readback and retries)\n", Checks);
  return 0;
}
