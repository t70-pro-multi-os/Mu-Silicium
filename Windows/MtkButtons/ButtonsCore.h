/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_BUTTONS_CORE_H_
#define MTK_BUTTONS_CORE_H_

#include <stdint.h>

#define MTK_BUTTON_ACTION_CAPACITY 32U

/* Values are the documented GPIO button notify interface enum order. */
typedef enum {
  MtkButtonPower = 0,
  MtkButtonWindows = 1,
  MtkButtonVolumeUp = 2,
  MtkButtonVolumeDown = 3,
  MtkButtonRotationLock = 4
} MTK_BUTTON_ID;

typedef enum {
  MtkButtonsCoreSuccess = 0,
  MtkButtonsCoreInvalid,
  MtkButtonsCoreNotReady,
  MtkButtonsCoreQueueFull,
  MtkButtonsCoreIoUncertain,
  MtkButtonsCoreQuarantined
} MTK_BUTTONS_CORE_STATUS;

#define MTK_BUTTON_SAMPLE_KPD_VALID   0x01U
#define MTK_BUTTON_SAMPLE_PMIC_VALID  0x02U
#define MTK_BUTTON_SAMPLE_ALL_VALID  (MTK_BUTTON_SAMPLE_KPD_VALID | MTK_BUTTON_SAMPLE_PMIC_VALID)

#define MTK_BUTTON_POWER_EVENT_PRESSED  0x01U
#define MTK_BUTTON_POWER_EVENT_RELEASED 0x04U
#define MTK_BUTTON_POWER_EVENT_MASK \
  (MTK_BUTTON_POWER_EVENT_PRESSED | MTK_BUTTON_POWER_EVENT_RELEASED)

#define MTK_BUTTON_STATE_POWER       0x01U
#define MTK_BUTTON_STATE_VOLUME_UP   0x02U
#define MTK_BUTTON_STATE_VOLUME_DOWN 0x04U

typedef struct {
  uint32_t Token;
  uint8_t Button;
  uint8_t Pressed;
  uint16_t Reserved;
} MTK_BUTTON_ACTION;

typedef struct {
  uint16_t Enable;
  uint16_t Debounce;
  uint16_t StatusWord0;
} MTK_BUTTON_KPD_SAMPLE;

typedef struct {
  uint32_t Signature;
  uint32_t NextToken;
  uint8_t BaselineValid;
  uint8_t Quarantined;
  uint8_t PhysicalPressed;
  uint8_t SuppressUntilRelease;
  uint8_t DeliveredPressed;
  uint8_t ActionHead;
  uint8_t ActionCount;
  uint8_t Reserved;
  MTK_BUTTON_ACTION Actions[MTK_BUTTON_ACTION_CAPACITY];
  uint64_t LastSnapshotGeneration;
} MTK_BUTTONS_CORE;

/*
 * Initialize a single-caller core. The caller must serialize Sample/Peek/
 * CompleteAction across ISR/DPC/work-item and PnP teardown paths.
 */
MTK_BUTTONS_CORE_STATUS MtkButtonsCoreInitialize(MTK_BUTTONS_CORE *Core);

/*
 * Consume one coherent sample. The caller supplies raw 16-bit reads from KPD
 * +0x24, +0x18, and +0x04, respectively. A ready KPD requires bit 0 set in
 * Enable and (Debounce & 0x3fff) == 1024. StatusWord0 bit 0 is active-low
 * volume-up and bit 1 is active-low volume-down. PMIC TOPSTATUS 0x28 bit 1 is
 * active-low for power. Both validity bits are required;
 * failed/partial samples and hardware-not-ready values are no-ops so the
 * caller can retain deferred input without inventing edges.
 * The first valid sample seeds current held keys and suppresses them until
 * release, preventing boot-held keys from becoming synthetic presses.
 */
MTK_BUTTONS_CORE_STATUS
MtkButtonsCoreSample(
  MTK_BUTTONS_CORE *Core,
  const MTK_BUTTON_KPD_SAMPLE *KpdSample,
  uint16_t PmicTopStatus,
  uint32_t ValidFlags
  );

/*
 * Consume a PMIC event snapshot together with the KPD and TOPSTATUS samples.
 * PowerEventMask contains only status bits 0/2 from PMIC 0x91c (mask 0x05):
 * they indicate that at least one press/release event was captured, not an
 * event count. The event bits and TOPSTATUS are not an atomic hardware sample.
 * SnapshotGeneration must be nonzero and strictly newer than the last sample
 * consumed successfully by this core. Partial, stale, invalid, or unqueueable
 * samples do not advance that generation; duplicate/stale generations return
 * NotReady. This function does not acknowledge broker metadata or clear PMIC
 * W1C status; the caller owns that ordering and must not requeue after an
 * ambiguous acknowledgement.
 */
MTK_BUTTONS_CORE_STATUS
MtkButtonsCoreSamplePmicSnapshot(
  MTK_BUTTONS_CORE *Core,
  const MTK_BUTTON_KPD_SAMPLE *KpdSample,
  uint16_t TopStatus,
  uint8_t PowerEventMask,
  uint64_t SnapshotGeneration,
  uint32_t ValidFlags
  );

/* Read the next toggle action without consuming it. */
MTK_BUTTONS_CORE_STATUS
MtkButtonsCorePeekAction(
  const MTK_BUTTONS_CORE *Core,
  MTK_BUTTON_ACTION *Action
  );

/*
 * Commit only after the in-box button notification write returned exact
 * success. Any failed/ambiguous write quarantines the core and is never
 * replayed: the notify interface toggles state, so retry could undo it.
 */
MTK_BUTTONS_CORE_STATUS
MtkButtonsCoreCompleteAction(
  MTK_BUTTONS_CORE *Core,
  const MTK_BUTTON_ACTION *Action,
  uint8_t ExactWriteSucceeded
  );

/* Queue releases for every button currently known down before leaving D0. */
MTK_BUTTONS_CORE_STATUS MtkButtonsCoreQueueReleaseAll(MTK_BUTTONS_CORE *Core);

/* After all release toggles succeed, sample a fresh baseline on the next D0. */
MTK_BUTTONS_CORE_STATUS MtkButtonsCoreResetBaseline(MTK_BUTTONS_CORE *Core);

#endif
