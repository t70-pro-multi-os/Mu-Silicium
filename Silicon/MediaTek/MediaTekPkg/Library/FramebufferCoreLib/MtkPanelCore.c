/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Portable T70Pro along_cyx LK and shipping Android panel GPIO sequences.
 *
 * Evidence: analysis/lk/lk.bin, SHA-256
 * 05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4.
 * The original ARM64 lcm_init at image offset 0x5bd00 (size 0x1c8) and
 * lcm_suspend at 0x5bec8 (size 0x170) were run in Unicorn with fake GPIO MMIO.
 * Delay calls were intercepted only to record their microsecond arguments.
 * Neither function sends DSI commands. The unnamed pin 152 is in the LK
 * sequence but is not described by the effective panel1 DT, so the board must
 * explicitly validate its mapping before any GPIO write is attempted.
 *
 * Shipping Android panel: panel-along_cyx_cv101wu_lh1038b.ko, SHA-256
 * 28a4c73d1929f1bf53891c6a92340fe8b9904d7c3bda89f21be63c28e45056f3.
 * jdi_prepare (0x608), jdi_enable (0x6d4), jdi_unprepare (0x83c),
 * lcm_power_ctrl (0x960) and jdi_power_delay_work (0x8c). Its __const_udelay
 * argument is microseconds * 0x10c7 (Linux 6.12 asm-generic/delay.h).
 * These are independent lifecycle operations, not an invented LK resume.
 */
#include <Library/MtkPanelCore.h>

#define PANEL_SIGNATURE 0x504E4C31U
#define PANEL_MODE_GPIO MTK_PANEL_GPIO_MODE_GPIO
#define PANEL_DIRECTION_OUTPUT MTK_PANEL_GPIO_DIRECTION_OUTPUT

typedef enum {
  PanelActionMode = 1,
  PanelActionDirection,
  PanelActionValue,
  PanelActionDelay,
  PanelActionLatchedValue,
  PanelActionDriveDirection
} PANEL_ACTION_KIND;

typedef struct {
  PANEL_ACTION_KIND Kind;
  MTK_PANEL_U32 Pin;
  MTK_PANEL_U32 Value;
} PANEL_ACTION;

typedef struct {
  MTK_PANEL_U32 Pin;
  MTK_PANEL_GPIO_ROLE Role;
} PANEL_PIN_ROLE;

#define MODE(PinValue) { PanelActionMode, (PinValue), PANEL_MODE_GPIO }
#define DIR(PinValue)  { PanelActionDirection, (PinValue), PANEL_DIRECTION_OUTPUT }
#define VALUE(PinValue, Level) { PanelActionValue, (PinValue), (Level) }
#define DELAY(Usec) { PanelActionDelay, 0U, (Usec) }
/* gpiod_direction_output sets the output latch before enabling output.
 * Preload first so a rail does not briefly take the stale latch value. */
#define DRIVE(PinValue, Level) \
  MODE (PinValue), { PanelActionLatchedValue, (PinValue), (Level) }, \
  { PanelActionDriveDirection, (PinValue), (Level) }

static const PANEL_ACTION mLkInitActions[] = {
  MODE (MTK_PANEL_GPIO_EDP_RESET), DIR (MTK_PANEL_GPIO_EDP_RESET),
  VALUE (MTK_PANEL_GPIO_EDP_RESET, 1U),
  MODE (MTK_PANEL_GPIO_AVDD), DIR (MTK_PANEL_GPIO_AVDD), VALUE (MTK_PANEL_GPIO_AVDD, 1U),
  DELAY (MTK_PANEL_DELAY_10MS_US),
  MODE (MTK_PANEL_GPIO_VDD18), DIR (MTK_PANEL_GPIO_VDD18), VALUE (MTK_PANEL_GPIO_VDD18, 1U),
  DELAY (MTK_PANEL_DELAY_5MS_US),
  MODE (MTK_PANEL_GPIO_VDD33), DIR (MTK_PANEL_GPIO_VDD33), VALUE (MTK_PANEL_GPIO_VDD33, 1U),
  DELAY (MTK_PANEL_DELAY_20MS_US),
  MODE (MTK_PANEL_GPIO_LK_UNNAMED_152), DIR (MTK_PANEL_GPIO_LK_UNNAMED_152), VALUE (MTK_PANEL_GPIO_LK_UNNAMED_152, 1U),
  MODE (MTK_PANEL_GPIO_AVEE), DIR (MTK_PANEL_GPIO_AVEE), VALUE (MTK_PANEL_GPIO_AVEE, 1U),
  MODE (MTK_PANEL_GPIO_VDD12), DIR (MTK_PANEL_GPIO_VDD12), VALUE (MTK_PANEL_GPIO_VDD12, 1U),
  DELAY (MTK_PANEL_DELAY_50MS_US),
  MODE (MTK_PANEL_GPIO_EDP_RESET), DIR (MTK_PANEL_GPIO_EDP_RESET),
  VALUE (MTK_PANEL_GPIO_EDP_RESET, 0U),
  DELAY (MTK_PANEL_DELAY_100MS_US), DELAY (MTK_PANEL_DELAY_100MS_US),
  MODE (MTK_PANEL_GPIO_BACKLIGHT), DIR (MTK_PANEL_GPIO_BACKLIGHT),
  VALUE (MTK_PANEL_GPIO_BACKLIGHT, 1U),
  DELAY (MTK_PANEL_DELAY_50MS_US)
};

static const PANEL_ACTION mLkSuspendActions[] = {
  MODE (MTK_PANEL_GPIO_BACKLIGHT), DIR (MTK_PANEL_GPIO_BACKLIGHT),
  VALUE (MTK_PANEL_GPIO_BACKLIGHT, 0U),
  DELAY (MTK_PANEL_DELAY_10MS_US),
  MODE (MTK_PANEL_GPIO_EDP_ENABLE), DIR (MTK_PANEL_GPIO_EDP_ENABLE),
  VALUE (MTK_PANEL_GPIO_EDP_ENABLE, 0U),
  MODE (MTK_PANEL_GPIO_EDP_RESET), DIR (MTK_PANEL_GPIO_EDP_RESET),
  VALUE (MTK_PANEL_GPIO_EDP_RESET, 0U),
  DELAY (MTK_PANEL_DELAY_1MS_US), DELAY (MTK_PANEL_DELAY_1MS_US),
  MODE (MTK_PANEL_GPIO_EDP_ENABLE), DIR (MTK_PANEL_GPIO_EDP_ENABLE),
  VALUE (MTK_PANEL_GPIO_EDP_ENABLE, 0U),
  MODE (MTK_PANEL_GPIO_AVEE), DIR (MTK_PANEL_GPIO_AVEE), VALUE (MTK_PANEL_GPIO_AVEE, 0U),
  DELAY (MTK_PANEL_DELAY_1MS_US),
  MODE (MTK_PANEL_GPIO_VDD33), DIR (MTK_PANEL_GPIO_VDD33), VALUE (MTK_PANEL_GPIO_VDD33, 0U),
  DELAY (MTK_PANEL_DELAY_1MS_US), DELAY (MTK_PANEL_DELAY_1MS_US),
  MODE (MTK_PANEL_GPIO_AVDD), DIR (MTK_PANEL_GPIO_AVDD), VALUE (MTK_PANEL_GPIO_AVDD, 0U)
};

/* Exact non-optional GPIO operations in the shipping module's prepare and
 * unprepare paths. Consecutive 1ms delay calls are combined without crossing
 * any GPIO transition. GPIO 152 is not part of these kernel functions. */
static const PANEL_ACTION mKernelPrepareActions[] = {
  DRIVE (MTK_PANEL_GPIO_EDP_RESET, 1U),
  DELAY (MTK_PANEL_DELAY_2MS_US),
  DRIVE (MTK_PANEL_GPIO_AVDD, 1U),
  DRIVE (MTK_PANEL_GPIO_VDD18, 1U),
  DRIVE (MTK_PANEL_GPIO_VDD33, 1U),
  DRIVE (MTK_PANEL_GPIO_AVEE, 1U),
  DRIVE (MTK_PANEL_GPIO_VDD12, 1U),
  DRIVE (MTK_PANEL_GPIO_EDP_RESET, 0U),
  DRIVE (MTK_PANEL_GPIO_BACKLIGHT, 1U)
};

static const PANEL_ACTION mKernelEnableActions[] = {
  DELAY (MTK_PANEL_KERNEL_ENABLE_DELAY_US),
  DRIVE (MTK_PANEL_GPIO_EDP_ENABLE, 1U)
};

static const PANEL_ACTION mKernelUnprepareActions[] = {
  DRIVE (MTK_PANEL_GPIO_EDP_ENABLE, 0U),
  DELAY (MTK_PANEL_DELAY_50MS_US),
  DRIVE (MTK_PANEL_GPIO_BACKLIGHT, 0U),
  DELAY (MTK_PANEL_DELAY_10MS_US),
  DRIVE (MTK_PANEL_GPIO_EDP_RESET, 0U),
  DRIVE (MTK_PANEL_GPIO_VDD12, 0U),
  DRIVE (MTK_PANEL_GPIO_AVEE, 0U),
  DELAY (MTK_PANEL_DELAY_10MS_US),
  DRIVE (MTK_PANEL_GPIO_VDD33, 0U),
  DELAY (MTK_PANEL_DELAY_5MS_US),
  DRIVE (MTK_PANEL_GPIO_VDD18, 0U),
  DELAY (MTK_PANEL_DELAY_10MS_US),
  DRIVE (MTK_PANEL_GPIO_AVDD, 0U),
  DELAY (MTK_PANEL_DELAY_510MS_US)
};

static const PANEL_PIN_ROLE mPanelPins[] = {
  { MTK_PANEL_GPIO_EDP_ENABLE,      MtkPanelGpioEdpEnable },
  { MTK_PANEL_GPIO_EDP_RESET,       MtkPanelGpioEdpReset },
  { MTK_PANEL_GPIO_AVDD,            MtkPanelGpioAvdd },
  { MTK_PANEL_GPIO_VDD18,           MtkPanelGpioVdd18 },
  { MTK_PANEL_GPIO_VDD33,           MtkPanelGpioVdd33 },
  { MTK_PANEL_GPIO_LK_UNNAMED_152,  MtkPanelGpioLkUnnamed152 },
  { MTK_PANEL_GPIO_AVEE,            MtkPanelGpioAvee },
  { MTK_PANEL_GPIO_VDD12,           MtkPanelGpioVdd12 },
  { MTK_PANEL_GPIO_BACKLIGHT,       MtkPanelGpioBacklight }
};

static MTK_PANEL_U8
CoreValid (
  const MTK_PANEL_CORE *Core
  )
{
  return (MTK_PANEL_U8)(
    (Core != 0) &&
    (Core->Signature == PANEL_SIGNATURE) &&
    (Core->State >= MtkPanelStateIdle) &&
    (Core->State <= MtkPanelStateRunningKernelUnprepare) &&
    (Core->PendingOperation >= MtkPanelOperationNone) &&
    (Core->PendingOperation <= MtkPanelOperationKernelUnprepare));
}

static MTK_PANEL_STATUS
ValidatePinMappings (
  MTK_PANEL_CORE *Core
  )
{
  unsigned Index;
  MTK_PANEL_STATUS Status;

  for (Index = 0; Index < (sizeof (mPanelPins) / sizeof (mPanelPins[0])); ++Index) {
    Status = Core->Io.ValidateGpio (
               Core->Io.Context,
               mPanelPins[Index].Pin,
               mPanelPins[Index].Role
               );
    if (Status != MtkPanelSuccess) {
      return (mPanelPins[Index].Pin == MTK_PANEL_GPIO_LK_UNNAMED_152)
               ? MtkPanelMappingError
               : Status;
    }
  }

  return MtkPanelSuccess;
}

static const PANEL_ACTION *
GetActions (
  MTK_PANEL_OPERATION Operation,
  unsigned *ActionCount
  )
{
  if (Operation == MtkPanelOperationLkInit) {
    *ActionCount = sizeof (mLkInitActions) / sizeof (mLkInitActions[0]);
    return mLkInitActions;
  }

  if (Operation == MtkPanelOperationLkSuspend) {
    *ActionCount = sizeof (mLkSuspendActions) / sizeof (mLkSuspendActions[0]);
    return mLkSuspendActions;
  }

  if (Operation == MtkPanelOperationKernelPrepare) {
    *ActionCount = sizeof (mKernelPrepareActions) / sizeof (mKernelPrepareActions[0]);
    return mKernelPrepareActions;
  }

  if (Operation == MtkPanelOperationKernelEnable) {
    *ActionCount = sizeof (mKernelEnableActions) / sizeof (mKernelEnableActions[0]);
    return mKernelEnableActions;
  }

  if (Operation == MtkPanelOperationKernelUnprepare) {
    *ActionCount = sizeof (mKernelUnprepareActions) / sizeof (mKernelUnprepareActions[0]);
    return mKernelUnprepareActions;
  }

  *ActionCount = 0;
  return 0;
}

static MTK_PANEL_U8
GpioMatchesAction (
  const PANEL_ACTION *Action,
  MTK_PANEL_U32 Mode,
  MTK_PANEL_U32 Direction,
  MTK_PANEL_U32 OutputLatch
  )
{
  if (Action->Kind == PanelActionMode) {
    return (MTK_PANEL_U8)(Mode == Action->Value);
  }

  if (Action->Kind == PanelActionDirection) {
    return (MTK_PANEL_U8)(
      (Mode == PANEL_MODE_GPIO) &&
      (Direction == Action->Value));
  }

  if (Action->Kind == PanelActionLatchedValue) {
    return (MTK_PANEL_U8)(
      (Mode == PANEL_MODE_GPIO) &&
      (OutputLatch == Action->Value));
  }

  if ((Action->Kind == PanelActionValue) ||
      (Action->Kind == PanelActionDriveDirection)) {
    return (MTK_PANEL_U8)(
      (Mode == PANEL_MODE_GPIO) &&
      (Direction == PANEL_DIRECTION_OUTPUT) &&
      (OutputLatch == Action->Value));
  }

  return 0U;
}

static MTK_PANEL_STATUS
ReadAndVerifyAction (
  MTK_PANEL_CORE *Core,
  const PANEL_ACTION *Action,
  MTK_PANEL_U8 *Matches
  )
{
  MTK_PANEL_STATUS Status;
  MTK_PANEL_U32 Mode;
  MTK_PANEL_U32 Direction;
  MTK_PANEL_U32 OutputLatch;

  Status = Core->Io.ReadGpio (
             Core->Io.Context,
             Action->Pin,
             &Mode,
             &Direction,
             &OutputLatch
             );
  if (Status != MtkPanelSuccess) {
    return Status;
  }

  *Matches = GpioMatchesAction (Action, Mode, Direction, OutputLatch);
  return MtkPanelSuccess;
}

static MTK_PANEL_STATUS
ApplyAction (
  MTK_PANEL_CORE *Core,
  const PANEL_ACTION *Action
  )
{
  MTK_PANEL_STATUS Status;
  MTK_PANEL_STATUS ReadStatus;
  MTK_PANEL_U8 Matches;

  if (Action->Kind == PanelActionDelay) {
    Core->ActionAttempted = 1U;
    Status = Core->Io.DelayUs (Core->Io.Context, Action->Value);
    if (Status != MtkPanelSuccess) {
      return Status;
    }

    return MtkPanelSuccess;
  }

  /* If a previous setter returned uncertainly, first read its effect. These
   * writes set absolute values, so an observed target is safe to accept. */
  if (Core->ActionAttempted != 0U) {
    ReadStatus = ReadAndVerifyAction (Core, Action, &Matches);
    if (ReadStatus != MtkPanelSuccess) {
      return ReadStatus;
    }

    if (Matches != 0U) {
      return MtkPanelSuccess;
    }
  }

  Core->ActionAttempted = 1U;
  switch (Action->Kind) {
    case PanelActionMode:
      Status = Core->Io.SetMode (Core->Io.Context, Action->Pin, Action->Value);
      break;
    case PanelActionDirection:
      Status = Core->Io.SetDirection (Core->Io.Context, Action->Pin, Action->Value);
      break;
    case PanelActionDriveDirection:
      Status = Core->Io.SetDirection (
                          Core->Io.Context, Action->Pin, PANEL_DIRECTION_OUTPUT);
      break;
    case PanelActionValue:
    case PanelActionLatchedValue:
      Status = Core->Io.SetValue (Core->Io.Context, Action->Pin, Action->Value);
      break;
    default:
      return MtkPanelInvalid;
  }

  ReadStatus = ReadAndVerifyAction (Core, Action, &Matches);
  if ((ReadStatus == MtkPanelSuccess) && (Matches != 0U)) {
    /* A write error may be returned after hardware applied the value. Readback
     * is the authoritative completion signal for this idempotent action. */
    return MtkPanelSuccess;
  }

  if (ReadStatus != MtkPanelSuccess) {
    return ReadStatus;
  }

  if (Status != MtkPanelSuccess) {
    return Status;
  }

  return MtkPanelVerifyError;
}

static MTK_PANEL_STATUS
RunSequence (
  MTK_PANEL_CORE *Core,
  MTK_PANEL_OPERATION Operation
  )
{
  MTK_PANEL_STATUS Status;
  const PANEL_ACTION *Actions;
  unsigned ActionCount;
  MTK_PANEL_U32 FinalState;
  MTK_PANEL_STATE RunningState;

  if (!CoreValid (Core)) {
    return MtkPanelInvalid;
  }

  if (Core->Busy != 0U) {
    return MtkPanelBusy;
  }

  Core->Busy = 1U;
  if (Operation == MtkPanelOperationLkInit) {
    FinalState = MtkPanelStateLkInitComplete;
    RunningState = MtkPanelStateRunningLkInit;
    if (Core->State == MtkPanelStateLkInitComplete) {
      Status = MtkPanelSuccess;
      goto Done;
    }
    if ((Core->State == MtkPanelStateLkSuspendComplete) ||
        ((Core->State != MtkPanelStateIdle) &&
         (Core->State != MtkPanelStateQuarantined))) {
      Status = MtkPanelNotReady;
      goto Done;
    }
  } else if (Operation == MtkPanelOperationLkSuspend) {
    FinalState = MtkPanelStateLkSuspendComplete;
    RunningState = MtkPanelStateRunningLkSuspend;
    if (Core->State == MtkPanelStateLkSuspendComplete) {
      Status = MtkPanelSuccess;
      goto Done;
    }
    if (((Core->State != MtkPanelStateLkInitComplete) &&
         (Core->State != MtkPanelStateQuarantined)) ||
        ((Core->State == MtkPanelStateQuarantined) &&
         (Core->PendingOperation != MtkPanelOperationLkSuspend))) {
      Status = (Core->State == MtkPanelStateQuarantined)
                 ? MtkPanelQuarantined : MtkPanelNotReady;
      goto Done;
    }
  } else if (Operation == MtkPanelOperationKernelPrepare) {
    FinalState = MtkPanelStateKernelPrepared;
    RunningState = MtkPanelStateRunningKernelPrepare;
    if (Core->State == MtkPanelStateKernelPrepared) {
      Status = MtkPanelSuccess;
      goto Done;
    }
    if ((Core->State != MtkPanelStateIdle) &&
        (Core->State != MtkPanelStateLkInitComplete) &&
        (Core->State != MtkPanelStateLkSuspendComplete) &&
        (Core->State != MtkPanelStateKernelUnprepared) &&
        (Core->State != MtkPanelStateQuarantined)) {
      Status = MtkPanelNotReady;
      goto Done;
    }
  } else if (Operation == MtkPanelOperationKernelEnable) {
    FinalState = MtkPanelStateKernelEnabled;
    RunningState = MtkPanelStateRunningKernelEnable;
    if (Core->State == MtkPanelStateKernelEnabled) {
      Status = MtkPanelSuccess;
      goto Done;
    }
    if ((Core->State != MtkPanelStateKernelPrepared) &&
        (Core->State != MtkPanelStateQuarantined)) {
      Status = MtkPanelNotReady;
      goto Done;
    }
  } else if (Operation == MtkPanelOperationKernelUnprepare) {
    FinalState = MtkPanelStateKernelUnprepared;
    RunningState = MtkPanelStateRunningKernelUnprepare;
    if (Core->State == MtkPanelStateKernelUnprepared) {
      Status = MtkPanelSuccess;
      goto Done;
    }
    if ((Core->State != MtkPanelStateKernelPrepared) &&
        (Core->State != MtkPanelStateKernelEnabled) &&
        (Core->State != MtkPanelStateLkInitComplete) &&
        (Core->State != MtkPanelStateLkSuspendComplete) &&
        (Core->State != MtkPanelStateQuarantined)) {
      Status = MtkPanelNotReady;
      goto Done;
    }
  } else {
    Status = MtkPanelInvalid;
    goto Done;
  }

  if ((Core->State == MtkPanelStateQuarantined) &&
      (Core->PendingOperation != Operation)) {
    Status = MtkPanelQuarantined;
    goto Done;
  }

  Status = ValidatePinMappings (Core);
  if (Status != MtkPanelSuccess) {
    Core->LastStatus = Status;
    goto Done;
  }

  if (Core->PendingOperation == MtkPanelOperationNone) {
    Core->PendingOperation = Operation;
    Core->ActionIndex = 0U;
    Core->ActionAttempted = 0U;
  }

  Core->State = RunningState;
  Actions = GetActions (Operation, &ActionCount);
  if ((Actions == 0) || (Core->ActionIndex >= ActionCount)) {
    Core->State = MtkPanelStateQuarantined;
    Core->LastStatus = MtkPanelInvalid;
    Status = MtkPanelInvalid;
    goto Done;
  }

  while (Core->ActionIndex < ActionCount) {
    const PANEL_ACTION *Action = &Actions[Core->ActionIndex];

    Status = ApplyAction (Core, Action);
    if (Status != MtkPanelSuccess) {
      Core->State = MtkPanelStateQuarantined;
      Core->LastStatus = Status;
      goto Done;
    }

    Core->ActionIndex++;
    Core->ActionAttempted = 0U;
  }

  Core->State = (MTK_PANEL_STATE)FinalState;
  Core->PendingOperation = MtkPanelOperationNone;
  Core->ActionIndex = 0U;
  Core->ActionAttempted = 0U;
  Core->LastStatus = MtkPanelSuccess;
  Status = MtkPanelSuccess;

Done:
  Core->Busy = 0U;
  if (Core->State != MtkPanelStateQuarantined) {
    Core->LastStatus = Status;
  }

  return Status;
}

MTK_PANEL_STATUS
MtkPanelCoreInitialize (
  MTK_PANEL_CORE *Core,
  const MTK_PANEL_IO *Io
  )
{
  if ((Core == 0) || (Io == 0)) {
    return MtkPanelInvalid;
  }

  if (Core->Signature == PANEL_SIGNATURE) {
    return MtkPanelBusy;
  }

  if (Core->Signature != 0U) {
    return MtkPanelInvalid;
  }

  if ((Io->ValidateGpio == 0) || (Io->ReadGpio == 0) ||
      (Io->SetMode == 0) || (Io->SetDirection == 0) ||
      (Io->SetValue == 0) || (Io->DelayUs == 0)) {
    return MtkPanelNotReady;
  }

  Core->Io = *Io;
  Core->Signature = PANEL_SIGNATURE;
  Core->State = MtkPanelStateIdle;
  Core->PendingOperation = MtkPanelOperationNone;
  Core->ActionIndex = 0U;
  Core->ActionAttempted = 0U;
  Core->Busy = 0U;
  Core->Reserved[0] = 0U;
  Core->Reserved[1] = 0U;
  Core->LastStatus = MtkPanelSuccess;
  return MtkPanelSuccess;
}

MTK_PANEL_STATUS
MtkPanelCoreRunLkInit (
  MTK_PANEL_CORE *Core
  )
{
  return RunSequence (Core, MtkPanelOperationLkInit);
}

MTK_PANEL_STATUS
MtkPanelCoreRunLkSuspend (
  MTK_PANEL_CORE *Core
  )
{
  return RunSequence (Core, MtkPanelOperationLkSuspend);
}

MTK_PANEL_U8
MtkPanelCoreLkInitComplete (
  const MTK_PANEL_CORE *Core
  )
{
  return (MTK_PANEL_U8)(
    CoreValid (Core) &&
    (Core->State == MtkPanelStateLkInitComplete));
}

MTK_PANEL_STATUS
MtkPanelCoreRunKernelPrepare (
  MTK_PANEL_CORE *Core
  )
{
  return RunSequence (Core, MtkPanelOperationKernelPrepare);
}

MTK_PANEL_STATUS
MtkPanelCoreRunKernelEnable (
  MTK_PANEL_CORE *Core
  )
{
  return RunSequence (Core, MtkPanelOperationKernelEnable);
}

MTK_PANEL_STATUS
MtkPanelCoreRunKernelUnprepare (
  MTK_PANEL_CORE *Core
  )
{
  return RunSequence (Core, MtkPanelOperationKernelUnprepare);
}
