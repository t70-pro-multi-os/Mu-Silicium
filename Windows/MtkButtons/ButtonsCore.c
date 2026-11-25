/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ButtonsCore.h"

#define MTK_BUTTONS_CORE_SIGNATURE 0x42373054U

static void MtkButtonsZero(void *Buffer, uint32_t Length) {
  uint8_t *Bytes = (uint8_t *)Buffer;
  uint32_t Index;
  for (Index = 0; Index < Length; ++Index) {
    Bytes[Index] = 0;
  }
}

static int MtkButtonsCoreValid(const MTK_BUTTONS_CORE *Core) {
  return Core != 0 && Core->Signature == MTK_BUTTONS_CORE_SIGNATURE;
}

static uint8_t MtkButtonsButtonBit(uint8_t Button) {
  switch (Button) {
    case MtkButtonPower: return MTK_BUTTON_STATE_POWER;
    case MtkButtonVolumeUp: return MTK_BUTTON_STATE_VOLUME_UP;
    case MtkButtonVolumeDown: return MTK_BUTTON_STATE_VOLUME_DOWN;
    default: return 0;
  }
}

MTK_BUTTONS_CORE_STATUS MtkButtonsCoreInitialize(MTK_BUTTONS_CORE *Core) {
  if (Core == 0) {
    return MtkButtonsCoreInvalid;
  }
  MtkButtonsZero(Core, (uint32_t)sizeof(*Core));
  Core->Signature = MTK_BUTTONS_CORE_SIGNATURE;
  Core->NextToken = 1;
  return MtkButtonsCoreSuccess;
}

static uint8_t MtkButtonsPhysicalState(
  const MTK_BUTTON_KPD_SAMPLE *KpdSample,
  uint16_t PmicTopStatus
  ) {
  uint8_t Pressed = 0;
  if ((KpdSample->StatusWord0 & 0x0001U) == 0) {
    Pressed |= MTK_BUTTON_STATE_VOLUME_UP;
  }
  if ((KpdSample->StatusWord0 & 0x0002U) == 0) {
    Pressed |= MTK_BUTTON_STATE_VOLUME_DOWN;
  }
  if ((PmicTopStatus & 0x0002U) == 0) {
    Pressed |= MTK_BUTTON_STATE_POWER;
  }
  return Pressed;
}

static void MtkButtonsAppend(
  MTK_BUTTON_ACTION *Actions,
  uint8_t *Count,
  uint8_t Button,
  uint8_t Pressed
  ) {
  Actions[*Count].Button = Button;
  Actions[*Count].Pressed = Pressed;
  Actions[*Count].Reserved = 0;
  ++*Count;
}

static int MtkButtonsTokensAvailable(
  const MTK_BUTTONS_CORE *Core,
  uint8_t Needed
  ) {
  if (Needed == 0) {
    return 1;
  }
  return Core->NextToken != 0 &&
         (uint32_t)Needed <= (uint32_t)(0xffffffffU - Core->NextToken);
}

static void MtkButtonsCommitBatch(
  MTK_BUTTONS_CORE *Core,
  MTK_BUTTON_ACTION *Pending,
  uint8_t Needed
  ) {
  uint8_t Index;
  for (Index = 0; Index < Needed; ++Index) {
    uint8_t Tail = (uint8_t)((Core->ActionHead + Core->ActionCount) %
                             MTK_BUTTON_ACTION_CAPACITY);
    Pending[Index].Token = Core->NextToken++;
    Core->Actions[Tail] = Pending[Index];
    ++Core->ActionCount;
  }
}

MTK_BUTTONS_CORE_STATUS
MtkButtonsCoreSample(
  MTK_BUTTONS_CORE *Core,
  const MTK_BUTTON_KPD_SAMPLE *KpdSample,
  uint16_t PmicTopStatus,
  uint32_t ValidFlags
  ) {
  uint8_t Physical, Old, Suppressed, NewSuppressed;
  uint8_t PressedEdges, ReleasedEdges;
  uint8_t Needed = 0;
  MTK_BUTTON_ACTION Pending[3];

  if (!MtkButtonsCoreValid(Core) || KpdSample == 0 ||
      (ValidFlags & ~MTK_BUTTON_SAMPLE_ALL_VALID) != 0) {
    return MtkButtonsCoreInvalid;
  }
  if (Core->Quarantined) {
    return MtkButtonsCoreQuarantined;
  }
  if (ValidFlags != MTK_BUTTON_SAMPLE_ALL_VALID) {
    return MtkButtonsCoreNotReady;
  }
  if ((KpdSample->Enable & 0x0001U) == 0 ||
      (KpdSample->Debounce & 0x3fffU) != 1024U) {
    return MtkButtonsCoreNotReady;
  }

  Physical = MtkButtonsPhysicalState(KpdSample, PmicTopStatus);
  if (!Core->BaselineValid) {
    Core->PhysicalPressed = Physical;
    Core->SuppressUntilRelease = Physical;
    Core->BaselineValid = 1;
    return MtkButtonsCoreSuccess;
  }

  Old = Core->PhysicalPressed;
  Suppressed = Core->SuppressUntilRelease;
  NewSuppressed = (uint8_t)(Suppressed & Physical);
  PressedEdges = (uint8_t)((Physical & (uint8_t)~Old) & (uint8_t)~Suppressed);
  ReleasedEdges = (uint8_t)((Old & (uint8_t)~Physical) & (uint8_t)~Suppressed);

  /* Release non-power controls first. A simultaneous power release is last. */
  if (ReleasedEdges & MTK_BUTTON_STATE_VOLUME_DOWN) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeDown, 0);
  }
  if (ReleasedEdges & MTK_BUTTON_STATE_VOLUME_UP) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeUp, 0);
  }
  /* Press power first so a chord becomes visible before the other new key. */
  if (PressedEdges & MTK_BUTTON_STATE_POWER) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonPower, 1);
  }
  if (PressedEdges & MTK_BUTTON_STATE_VOLUME_UP) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeUp, 1);
  }
  if (PressedEdges & MTK_BUTTON_STATE_VOLUME_DOWN) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeDown, 1);
  }
  if (ReleasedEdges & MTK_BUTTON_STATE_POWER) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonPower, 0);
  }

  if ((uint32_t)Core->ActionCount + Needed > MTK_BUTTON_ACTION_CAPACITY) {
    Core->Quarantined = 1;
    return MtkButtonsCoreQueueFull;
  }
  if (!MtkButtonsTokensAvailable(Core, Needed)) {
    Core->Quarantined = 1;
    return MtkButtonsCoreQuarantined;
  }

  MtkButtonsCommitBatch(Core, Pending, Needed);
  Core->PhysicalPressed = Physical;
  Core->SuppressUntilRelease = NewSuppressed;
  return MtkButtonsCoreSuccess;
}

MTK_BUTTONS_CORE_STATUS
MtkButtonsCoreSamplePmicSnapshot(
  MTK_BUTTONS_CORE *Core,
  const MTK_BUTTON_KPD_SAMPLE *KpdSample,
  uint16_t TopStatus,
  uint8_t PowerEventMask,
  uint64_t SnapshotGeneration,
  uint32_t ValidFlags
  ) {
  uint8_t Physical, Old, Suppressed, NewSuppressed;
  uint8_t PressedEdges, ReleasedEdges;
  uint8_t PowerUp = 0, PowerDown = 0, PowerTapDownUp = 0;
  uint8_t PowerTapUpDown = 0;
  uint8_t Needed = 0;
  MTK_BUTTON_ACTION Pending[5];

  if (!MtkButtonsCoreValid(Core) || KpdSample == 0 || SnapshotGeneration == 0 ||
      (ValidFlags & ~MTK_BUTTON_SAMPLE_ALL_VALID) != 0 ||
      (PowerEventMask & (uint8_t)~MTK_BUTTON_POWER_EVENT_MASK) != 0) {
    return MtkButtonsCoreInvalid;
  }
  if (Core->Quarantined) {
    return MtkButtonsCoreQuarantined;
  }
  if (ValidFlags != MTK_BUTTON_SAMPLE_ALL_VALID ||
      (KpdSample->Enable & 0x0001U) == 0 ||
      (KpdSample->Debounce & 0x3fffU) != 1024U) {
    return MtkButtonsCoreNotReady;
  }
  if (SnapshotGeneration <= Core->LastSnapshotGeneration) {
    return MtkButtonsCoreNotReady;
  }

  Physical = MtkButtonsPhysicalState(KpdSample, TopStatus);
  if (!Core->BaselineValid) {
    Core->PhysicalPressed = Physical;
    Core->SuppressUntilRelease = Physical;
    Core->BaselineValid = 1;
    Core->LastSnapshotGeneration = SnapshotGeneration;
    return MtkButtonsCoreSuccess;
  }

  Old = Core->PhysicalPressed;
  Suppressed = Core->SuppressUntilRelease;
  NewSuppressed = (uint8_t)(Suppressed & Physical);
  PressedEdges = (uint8_t)((Physical & (uint8_t)~Old) & (uint8_t)~Suppressed);
  ReleasedEdges = (uint8_t)((Old & (uint8_t)~Physical) & (uint8_t)~Suppressed);

  if ((Suppressed & MTK_BUTTON_STATE_POWER) == 0 &&
      ((Old ^ Physical) & MTK_BUTTON_STATE_POWER) == 0) {
    if ((Old & MTK_BUTTON_STATE_POWER) == 0 &&
        (PowerEventMask & MTK_BUTTON_POWER_EVENT_PRESSED) != 0) {
      PowerTapDownUp = 1;
    } else if ((Old & MTK_BUTTON_STATE_POWER) != 0 &&
               (PowerEventMask & MTK_BUTTON_POWER_EVENT_RELEASED) != 0) {
      PowerTapUpDown = 1;
    }
  }
  PowerUp = (uint8_t)(((PressedEdges & MTK_BUTTON_STATE_POWER) != 0) ||
                      PowerTapDownUp);
  PowerDown = (uint8_t)(((ReleasedEdges & MTK_BUTTON_STATE_POWER) != 0) ||
                        PowerTapDownUp);

  /* Keep volume release order; a captured down/down power cycle is release-first. */
  if (ReleasedEdges & MTK_BUTTON_STATE_VOLUME_DOWN) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeDown, 0);
  }
  if (ReleasedEdges & MTK_BUTTON_STATE_VOLUME_UP) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeUp, 0);
  }
  if (PowerTapUpDown) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonPower, 0);
    MtkButtonsAppend(Pending, &Needed, MtkButtonPower, 1);
  } else if (PowerUp) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonPower, 1);
  }
  if (PressedEdges & MTK_BUTTON_STATE_VOLUME_UP) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeUp, 1);
  }
  if (PressedEdges & MTK_BUTTON_STATE_VOLUME_DOWN) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeDown, 1);
  }
  if (!PowerTapUpDown && PowerDown) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonPower, 0);
  }

  if ((uint32_t)Core->ActionCount + Needed > MTK_BUTTON_ACTION_CAPACITY) {
    Core->Quarantined = 1;
    return MtkButtonsCoreQueueFull;
  }
  if (!MtkButtonsTokensAvailable(Core, Needed)) {
    Core->Quarantined = 1;
    return MtkButtonsCoreQuarantined;
  }

  MtkButtonsCommitBatch(Core, Pending, Needed);
  Core->PhysicalPressed = Physical;
  Core->SuppressUntilRelease = NewSuppressed;
  Core->LastSnapshotGeneration = SnapshotGeneration;
  return MtkButtonsCoreSuccess;
}

MTK_BUTTONS_CORE_STATUS
MtkButtonsCorePeekAction(
  const MTK_BUTTONS_CORE *Core,
  MTK_BUTTON_ACTION *Action
  ) {
  if (!MtkButtonsCoreValid(Core) || Action == 0) {
    return MtkButtonsCoreInvalid;
  }
  if (Core->Quarantined) {
    return MtkButtonsCoreQuarantined;
  }
  if (Core->ActionCount == 0) {
    return MtkButtonsCoreNotReady;
  }
  *Action = Core->Actions[Core->ActionHead];
  return MtkButtonsCoreSuccess;
}

MTK_BUTTONS_CORE_STATUS
MtkButtonsCoreCompleteAction(
  MTK_BUTTONS_CORE *Core,
  const MTK_BUTTON_ACTION *Action,
  uint8_t ExactWriteSucceeded
  ) {
  MTK_BUTTON_ACTION *Head;
  uint8_t Bit;

  if (!MtkButtonsCoreValid(Core) || Action == 0 || ExactWriteSucceeded > 1) {
    return MtkButtonsCoreInvalid;
  }
  if (Core->Quarantined) {
    return MtkButtonsCoreQuarantined;
  }
  if (Core->ActionCount == 0) {
    return MtkButtonsCoreNotReady;
  }
  Head = &Core->Actions[Core->ActionHead];
  if (Head->Token != Action->Token || Head->Button != Action->Button ||
      Head->Pressed != Action->Pressed || Head->Reserved != Action->Reserved) {
    return MtkButtonsCoreInvalid;
  }
  if (!ExactWriteSucceeded) {
    Core->Quarantined = 1;
    return MtkButtonsCoreIoUncertain;
  }

  Bit = MtkButtonsButtonBit(Head->Button);
  if (Bit == 0 || Head->Pressed > 1 ||
      (Head->Pressed && (Core->DeliveredPressed & Bit)) ||
      (!Head->Pressed && !(Core->DeliveredPressed & Bit))) {
    Core->Quarantined = 1;
    return MtkButtonsCoreIoUncertain;
  }
  if (Head->Pressed) {
    Core->DeliveredPressed |= Bit;
  } else {
    Core->DeliveredPressed &= (uint8_t)~Bit;
  }
  MtkButtonsZero(Head, (uint32_t)sizeof(*Head));
  Core->ActionHead = (uint8_t)((Core->ActionHead + 1U) %
                               MTK_BUTTON_ACTION_CAPACITY);
  --Core->ActionCount;
  return MtkButtonsCoreSuccess;
}

MTK_BUTTONS_CORE_STATUS MtkButtonsCoreQueueReleaseAll(MTK_BUTTONS_CORE *Core) {
  MTK_BUTTON_ACTION Pending[3];
  uint8_t Needed = 0;
  uint8_t Delivered;

  if (!MtkButtonsCoreValid(Core)) {
    return MtkButtonsCoreInvalid;
  }
  if (Core->Quarantined) {
    return MtkButtonsCoreQuarantined;
  }
  if (Core->ActionCount != 0) {
    return MtkButtonsCoreNotReady;
  }

  Delivered = Core->DeliveredPressed;
  if (Delivered & MTK_BUTTON_STATE_VOLUME_DOWN) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeDown, 0);
  }
  if (Delivered & MTK_BUTTON_STATE_VOLUME_UP) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonVolumeUp, 0);
  }
  if (Delivered & MTK_BUTTON_STATE_POWER) {
    MtkButtonsAppend(Pending, &Needed, MtkButtonPower, 0);
  }

  if ((uint32_t)Core->ActionCount + Needed > MTK_BUTTON_ACTION_CAPACITY ||
      !MtkButtonsTokensAvailable(Core, Needed)) {
    Core->Quarantined = 1;
    return MtkButtonsCoreQuarantined;
  }

  MtkButtonsCommitBatch(Core, Pending, Needed);

  /* After resume, don't synthesize a fresh press for a key held over D0. */
  Core->SuppressUntilRelease = Core->PhysicalPressed;
  return MtkButtonsCoreSuccess;
}

MTK_BUTTONS_CORE_STATUS MtkButtonsCoreResetBaseline(MTK_BUTTONS_CORE *Core) {
  if (!MtkButtonsCoreValid(Core)) {
    return MtkButtonsCoreInvalid;
  }
  if (Core->Quarantined) {
    return MtkButtonsCoreQuarantined;
  }
  if (Core->ActionCount != 0 || Core->DeliveredPressed != 0) {
    return MtkButtonsCoreNotReady;
  }
  Core->BaselineValid = 0;
  Core->PhysicalPressed = 0;
  Core->SuppressUntilRelease = 0;
  return MtkButtonsCoreSuccess;
}
