// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_MUSB_PLATFORM_CORE_H
#define MTK_MUSB_PLATFORM_CORE_H

#include <stddef.h>
#include <stdint.h>

#define MTK_MUSB_PLATFORM_CORE_REVISION 1U
#define MTK_MUSB_PLATFORM_CONTROLLER_BASE 0x11200000ULL
#define MTK_MUSB_PLATFORM_CONTROLLER_BYTES 0x10000ULL
#define MTK_MUSB_PLATFORM_PHY_BASE 0x11F40000ULL
#define MTK_MUSB_PLATFORM_PHY_BYTES 0x10000ULL
#define MTK_MUSB_PLATFORM_PHY_ACR1 0x304U
#define MTK_MUSB_PLATFORM_PHY_ACR6 0x318U
#define MTK_MUSB_PLATFORM_PHY_DTM0 0x368U
#define MTK_MUSB_PLATFORM_PHY_DTM1 0x36CU
#define MTK_MUSB_PLATFORM_PHY_REG_BYTES 0x700ULL
#define MTK_MUSB_PLATFORM_GSIV 160U
#define MTK_MUSB_PLATFORM_ENDPOINTS 16U
#define MTK_MUSB_PLATFORM_PORTS 1U
#define MTK_MUSB_PLATFORM_FIFO_PROFILE_T70_V1 1U
#define MTK_MUSB_PLATFORM_EVENTS 0x30U
#define MTK_MUSB_PLATFORM_FIFO_TOTAL_BYTES 7808U
#define MTK_MUSB_PLATFORM_FIFO_RAM_BYTES 8192U
#define MTK_MUSB_PLATFORM_CLOCK_MASK 0x07U

#define MTK_MUSB_PLATFORM_READY_MMIO       (1U << 0)
#define MTK_MUSB_PLATFORM_READY_CLOCKS     (1U << 1)
#define MTK_MUSB_PLATFORM_READY_PHY        (1U << 2)
#define MTK_MUSB_PLATFORM_READY_HOST       (1U << 3)
#define MTK_MUSB_PLATFORM_READY_VBUS       (1U << 4)
#define MTK_MUSB_PLATFORM_READY_FIFO       (1U << 5)
#define MTK_MUSB_PLATFORM_READY_EVENTS     (1U << 6)
#define MTK_MUSB_PLATFORM_READY_REQUIRED   0x7FU
#define MTK_MUSB_PLATFORM_READY_ID_LOW_HUB (1U << 7)
#define MTK_MUSB_PLATFORM_READY_ID_LOW_REQUIRED \
  ((MTK_MUSB_PLATFORM_READY_REQUIRED & ~MTK_MUSB_PLATFORM_READY_VBUS) | \
   MTK_MUSB_PLATFORM_READY_ID_LOW_HUB)

#define MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC 0U
#define MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE 1U
#define MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB   2U
#define MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY   3U
/* The host runs before any connector supplies a device. Connector power is
 * managed separately and is no condition for the controller. */
#define MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST   4U
#define MTK_MUSB_PLATFORM_READY_HOST_FIRST_REQUIRED \
  (MTK_MUSB_PLATFORM_READY_REQUIRED & ~MTK_MUSB_PLATFORM_READY_VBUS)
#define MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW 3U

#define MTK_MUSB_PLATFORM_GPIO_ID_PIN      10U
#define MTK_MUSB_PLATFORM_GPIO_MUX_PIN     99U
#define MTK_MUSB_PLATFORM_GPIO_5V_PIN       4U
#define MTK_MUSB_PLATFORM_GPIO_RESET_PIN  104U
#define MTK_MUSB_PLATFORM_GPIO_OTG_PIN     86U
#define MTK_MUSB_PLATFORM_GPIO_COUNT        5U
#define MTK_MUSB_PLATFORM_GPIO_INPUT        0U
#define MTK_MUSB_PLATFORM_GPIO_OUTPUT       1U

/* Read/write/lock callbacks return nonzero on success, matching MusbHostCore. */
typedef int (*MTK_MUSB_PLATFORM_READ8)(void *Context, uint32_t Offset, uint8_t *Value);
typedef int (*MTK_MUSB_PLATFORM_READ16)(void *Context, uint32_t Offset, uint16_t *Value);
typedef int (*MTK_MUSB_PLATFORM_READ32)(void *Context, uint32_t Offset, uint32_t *Value);
typedef int (*MTK_MUSB_PLATFORM_WRITE8)(void *Context, uint32_t Offset, uint8_t Value);
typedef int (*MTK_MUSB_PLATFORM_WRITE16)(void *Context, uint32_t Offset, uint16_t Value);
typedef int (*MTK_MUSB_PLATFORM_WRITE32)(void *Context, uint32_t Offset, uint32_t Value);
typedef int (*MTK_MUSB_PLATFORM_TRY_LOCK)(void *Context);
typedef void (*MTK_MUSB_PLATFORM_UNLOCK)(void *Context);

typedef struct {
  void *Context;
  uint64_t ControllerLength;
  MTK_MUSB_PLATFORM_READ8 Read8;
  MTK_MUSB_PLATFORM_READ16 Read16;
  MTK_MUSB_PLATFORM_READ32 Read32;
  MTK_MUSB_PLATFORM_WRITE8 Write8;
  MTK_MUSB_PLATFORM_WRITE16 Write16;
  MTK_MUSB_PLATFORM_WRITE32 Write32;
  MTK_MUSB_PLATFORM_TRY_LOCK TryLock;
  MTK_MUSB_PLATFORM_UNLOCK Unlock;
} MTK_MUSB_PLATFORM_IO;

typedef enum {
  MtkMusbPlatformSuccess = 0,
  MtkMusbPlatformInvalid,
  MtkMusbPlatformUnsupported,
  MtkMusbPlatformNotReady,
  MtkMusbPlatformBusy,
  MtkMusbPlatformIoError,
  MtkMusbPlatformQuiescenceFailed,
  MtkMusbPlatformProfileMismatch,
  MtkMusbPlatformTimeout,
  MtkMusbPlatformConflict,
  MtkMusbPlatformPending
} MTK_MUSB_PLATFORM_STATUS;

typedef struct {
  uint32_t Number;
  uint32_t Mode;
  uint32_t Direction;
  uint32_t PhysicalInput;
} MTK_MUSB_PLATFORM_GPIO_SAMPLE;

typedef struct {
  uint64_t ControllerBase;
  uint64_t ControllerLength;
  uint64_t PhyBase;
  uint64_t PhyLength;
  uint32_t ControllerMapped;
  uint32_t PhyMapped;
  uint32_t ClockReadMask;
  uint32_t ClockEnabledMask;
  uint64_t ClockHz[3];
  uint64_t ExpectedClockHz[3];
  uint64_t RefClockHz;
  uint32_t RefClockEnabled;
  uint32_t TopUsbParent;
  uint32_t ExpectedTopUsbParent;
  uint64_t TopUsbFrequencyHz;
  uint64_t ExpectedTopUsbFrequencyHz;
  MTK_MUSB_PLATFORM_GPIO_SAMPLE Gpio[MTK_MUSB_PLATFORM_GPIO_COUNT];
  uint32_t PhyState;
  /* Set only when PhyState is FAULTED and PHY PowerOff cleanup is pending. */
  uint32_t PhyPowerCleanupPending;
  uint32_t PhyReadMask;
  uint32_t PhyDtm0;
  uint32_t PhyDtm1;
  uint32_t PhyAcr1;
  uint32_t PhyAcr6;
  uint32_t Sc8989xBoostReadValid;
  uint32_t Sc8989xBoostEnabled;
  uint32_t Husb311PowerStatusReadValid;
  uint32_t Husb311PowerStatus;
  uint32_t Husb311CcStatusReadValid;
  uint32_t Husb311CcStatus;
  uint32_t Husb311RoleControl;
  /* Zero is the legacy Type-C source path. The ID-low token is board-owned. */
  uint32_t HostPowerPath;
  uint64_t IdLowRoleToken;
  uint32_t InterruptVector;
  uint32_t SupportedEventMask;
  /* Copy of the actual EDK2 MUSB host-core port snapshot. */
  uint16_t HostPortStatus;
  uint16_t HostPortChangeStatus;
  uint32_t HostPortSnapshotValid;
  /* Optional Windows ACPI evidence. ClockIdentity must match across guards. */
  uint32_t AcpiColdFlags;
  uint32_t AcpiClockIdentity;
} MTK_MUSB_PLATFORM_EVIDENCE;

typedef struct {
  uint32_t Flags;
  uint32_t MissingFlags;
  uint32_t HostPowerPath;
  /* Copy of the actual EDK2 MUSB host-core port snapshot. */
  uint16_t HostPortStatus;
  uint16_t HostPortChangeStatus;
  uint8_t DeviceControl;
  uint8_t UsbInterruptStatus;
  uint8_t HostPortSnapshotValid;
  uint8_t Reserved;
} MTK_MUSB_PLATFORM_READY_RESULT;

#define MTK_MUSB_PLATFORM_PHY_STATE_UNOBSERVED 0U
#define MTK_MUSB_PLATFORM_PHY_POWERED 2U
#define MTK_MUSB_PLATFORM_PHY_STATE_FAULTED 3U
#define MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING 1U
#define MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN 0U
#define MTK_MUSB_PLATFORM_SC8989X_BOOST_ENABLED 1U
#define MTK_MUSB_PLATFORM_HUSB311_REG_POWER_STATUS 0x1EU
#define MTK_MUSB_PLATFORM_HUSB311_REG_CC_STATUS 0x1DU
#define MTK_MUSB_PLATFORM_HUSB311_REG_ROLE_CONTROL 0x1AU
#define MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING (1U << 6)
#define MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED (1U << 3)
#define MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT (1U << 2)
#define MTK_MUSB_PLATFORM_HUSB311_CC_LOOKING_FOR_CONNECTION (1U << 5)
#define MTK_MUSB_PLATFORM_HUSB311_CC_CONNECT_RESULT (1U << 4)
#define MTK_MUSB_PLATFORM_HUSB311_CC_RESERVED_MASK (3U << 6)
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_DRP (1U << 6)
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_RESERVED_MASK (1U << 7)
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_SHIFT 4U
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_MASK (3U << MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_SHIFT)
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_RP_VALUE_RESERVED 3U
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_CC1_MASK 0x03U
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_MASK (0x03U << 2)
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_CC1_SHIFT 0U
#define MTK_MUSB_PLATFORM_HUSB311_CC1_SHIFT 0U
#define MTK_MUSB_PLATFORM_HUSB311_CC2_SHIFT 2U
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_CC2_SHIFT 2U
#define MTK_MUSB_PLATFORM_HUSB311_CC_STATE_MASK 0x03U
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_RP 0x01U
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_RD 0x02U
#define MTK_MUSB_PLATFORM_HUSB311_ROLE_OPEN 0x03U
#define MTK_MUSB_PLATFORM_HUSB311_SRC_OPEN 0x00U
#define MTK_MUSB_PLATFORM_HUSB311_SRC_RA 0x01U
#define MTK_MUSB_PLATFORM_HUSB311_SRC_RD 0x02U

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformCheckQuiescent(
  const MTK_MUSB_PLATFORM_IO *Io,
  uint32_t RequireSessionClear
  );

/* First blocking register, with the indexed endpoint (ff for common registers).
 * Offset=ffffffff means no blocking register was observed. */
typedef struct {
  uint32_t Offset, Value, Mask, Endpoint;
} MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC;

/* Inspect an inherited peripheral without changing its IRQ/DMA request setup.
 * Valid masks and DMA request configuration alone are not active transfers.
 * Nonempty/ready FIFOs, DMA/QMU engines, power transitions and SETUPEND block. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformCheckIdleDevice(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic
  );

/* Retire valid IRQ masks, then idle endpoint DMA requests in stock order.
 * Fresh idle checks precede writes; WZC status is preserved. No DMA engine,
 * FIFO, toggle or data write. Partial failure stays stopped without retry. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRetireIdleDevice(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic
  );

/* Disconnect eligibility is distinct from idle: peripheral DATAEND may be
 * pending, but ready/nonempty FIFOs, SETUPEND, DMA/QMU and transitions block.
 * Only a board-owned, stable physical disconnect request may use these APIs.
 * DATAEND is neither acknowledged nor cleared while the device is connected. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformCheckDeviceForDisconnect(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic
  );
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRetireDeviceForDisconnect(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic
  );

/* Request the stock controller reset only after SOFTCONN/SESSION and masks/
 * DMA requests are down, and every FIFO/DMA/QMU is idle except DATAEND.
 * No DMA cancellation or FIFO flush. The caller must delay, verify strict
 * idle and acknowledge observed interrupts before attempting host setup.
 * A write may have reached hardware on failure; never retry it. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRequestDisconnectedDeviceReset(
  const MTK_MUSB_PLATFORM_IO *Io, MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic
  );

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(
  const MTK_MUSB_PLATFORM_IO *Io,
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC *Diagnostic
  );

/* After masks, endpoint CSR, DMA and QMU prove idle, acknowledge only the
 * observed TX/RX completion bits (16-bit W1C) and DMA bits (8-bit W1C).
 * Common USB port events remain for MusbHostCore. A nonzero readback fails. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformAcknowledgeIdleInterrupts(
  const MTK_MUSB_PLATFORM_IO *Io
  );

/* Configure only the exact stock host FIFO table, after a fresh idle check.
 * A failed write can leave a partial table; it never resets the controller. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformConfigureHostFifo(
  const MTK_MUSB_PLATFORM_IO *Io
  );

/* Repair a missing FIFO profile during OS resume. The caller has drained
 * requests and verified the live clocks and USB PHY. Requires idle DMA,
 * endpoints and interrupts, plus an active host session before and after
 * programming. Only INDEX and the FIFO layout registers are written. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformRestoreRetainedHostFifo(
  const MTK_MUSB_PLATFORM_IO *Io
  );

/* Reset an idle, powered host whose live clock and board route have been
 * verified by the caller. The caller owns IRQ masking and restores PIO
 * configuration after reset. The reset does not program VBUS or SESSION. */
/* Stop the endpoints a previous owner left active and install the host FIFO
 * layout. Session, VBUS, PHY and role stay as they are. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformTakeOverHost(
  const MTK_MUSB_PLATFORM_IO *Io
  );

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformResetRetainedHost(
  const MTK_MUSB_PLATFORM_IO *Io
  );

/* Check live USB2 host signal selection, thresholds and BC1.1 state. */
/* Validate retained host signaling independently of the analog tuning fields. */
int
MtkMusbPlatformHostPhyLinkReady(
  uint32_t Acr6,
  uint32_t Dtm0,
  uint32_t Dtm1
  );

int
MtkMusbPlatformHostPhyConfigured(
  uint32_t Acr1,
  uint32_t Acr6,
  uint32_t Dtm0,
  uint32_t Dtm1
  );

MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformVerifyHostFifo(
  const MTK_MUSB_PLATFORM_IO *Io
  );

/* Evidence gate plus fresh controller readback. Inherited PHY state may be
 * unobserved by this core when all raw host-mode register checks pass. The
 * port status fields must come from the actual EDK2 host-core snapshot. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformEvaluateReady(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_EVIDENCE *Evidence,
  MTK_MUSB_PLATFORM_READY_RESULT *Result
  );

/* Classify one successful raw HUSB311 CC_STATUS/ROLE_CONTROL sample. This
 * identifies a source attachment to one sink (one SRC.Rd) with a valid role
 * configuration; callers must establish sample freshness/stability and must
 * not treat the result as proof of PD data role (DFP). */
int
MtkMusbPlatformHusb311SourceAttached(
  uint8_t CcStatus,
  uint8_t RoleControl
  );

#define MTK_MUSB_PLATFORM_USB_DATA_ROLE_UNKNOWN 0U
#define MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP     1U
#define MTK_MUSB_PLATFORM_USB_DATA_ROLE_UFP     2U
#define MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST     1U
#define MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG      11U
#define MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID  12U

typedef enum {
  MtkMusbPlatformColdBeforeSession = 1,
  MtkMusbPlatformColdAfterSession,
  MtkMusbPlatformColdBeforePower,
  MtkMusbPlatformColdAfterPower,
  MtkMusbPlatformColdBeforeStop,
  MtkMusbPlatformColdAfterStop
} MTK_MUSB_PLATFORM_COLD_PHASE;

typedef struct {
  uint64_t ExclusiveOwnerToken;
  uint64_t DataRoleToken;
  uint64_t SampleSequence;
  uint32_t UsbDataRole;
  uint32_t PhyUsbMode;
  /* Stop may carry FAULTED plus PowerCleanupPending with mode UNKNOWN. */
  MTK_MUSB_PLATFORM_EVIDENCE Platform;
} MTK_MUSB_PLATFORM_COLD_EVIDENCE;

/* Start phases keep one latched power path and token: Type-C requires the
 * separate DFP token plus source evidence; ID-low uses HOST_ID_LOW, its own
 * token, and physical hub/SC evidence. A Type-C CC attachment alone is not
 * proof of DFP. OwnerToken must identify the same exclusive controller owner
 * and SampleSequence must strictly increase. Stop phases may attest only
 * owner/clock/PHY state after the external route has disappeared. */
typedef int (*MTK_MUSB_PLATFORM_COLD_GUARD)(
  void *Context,
  uint32_t Phase,
  MTK_MUSB_PLATFORM_COLD_EVIDENCE *Evidence
  );

typedef int (*MTK_MUSB_PLATFORM_COLD_SET_PHY_MODE)(
  void *Context,
  uint32_t Mode
  );

typedef int (*MTK_MUSB_PLATFORM_COLD_TIME_US)(
  void *Context,
  uint64_t *Microseconds
  );

typedef struct {
  void *Context;
  MTK_MUSB_PLATFORM_COLD_GUARD FreshGuard;
  MTK_MUSB_PLATFORM_COLD_SET_PHY_MODE SetPhyMode;
  MTK_MUSB_PLATFORM_COLD_TIME_US GetTimeUs;
} MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS;

#define MTK_MUSB_PLATFORM_COLD_OWNS_SESSION (1U << 0)
#define MTK_MUSB_PLATFORM_COLD_OWNS_POWER   (1U << 1)
#define MTK_MUSB_PLATFORM_COLD_OWNS_PHY     (1U << 2)

/* Read-only ACPI contracts for Windows cold resume. Each flag may be set
 * only after the named method has performed its live register readback. */
#define MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID       (1U << 0)
#define MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID       (1U << 1)
#define MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE      (1U << 2)
#define MTK_MUSB_PLATFORM_ACPI_ID_LOW_ROUTE      (1U << 3)
#define MTK_MUSB_PLATFORM_ACPI_FLAGS_MASK        0x0FU
/* Controller settling budget across scheduled polls, with no synchronous wait. */
#define MTK_MUSB_PLATFORM_COLD_TIMEOUT_MS   500U
#define MTK_MUSB_PLATFORM_COLD_POLL_US      1000U
#define MTK_MUSB_PLATFORM_SWRST_BABBLE_SET   0x80U
#define MTK_MUSB_PLATFORM_SWRST_BABBLE_CLEAR 0x40U

typedef enum {
  MtkMusbPlatformColdStateUninitialized = 0,
  MtkMusbPlatformColdStateStarting,
  MtkMusbPlatformColdStateNeedsStop,
  MtkMusbPlatformColdStateStarted,
  MtkMusbPlatformColdStateStopping
} MTK_MUSB_PLATFORM_COLD_STATE_KIND;

/* StartStep identifies the last attempted operation, including failures before
 * ownership. GuardPhase/GuardReason identify the most recent fresh check.
 * These diagnostics survive an unowned start failure; a new start clears them. */
typedef enum {
  MtkMusbPlatformColdStartGuard = 1,
  MtkMusbPlatformColdStartRegisters,
  MtkMusbPlatformColdStartAck,
  MtkMusbPlatformColdStartQuiescent,
  MtkMusbPlatformColdStartRecheck,
  MtkMusbPlatformColdStartSession,
  MtkMusbPlatformColdStartAfterSession,
  MtkMusbPlatformColdStartPhyHost,
  MtkMusbPlatformColdStartWaitHost,
  MtkMusbPlatformColdStartBeforePower,
  MtkMusbPlatformColdStartAckPower,
  MtkMusbPlatformColdStartPower,
  MtkMusbPlatformColdStartAfterPower,
  MtkMusbPlatformColdStartReady,
  MtkMusbPlatformColdStartSessionClear,
  MtkMusbPlatformColdStartPhyInvalid,
  MtkMusbPlatformColdStartSettle,
  MtkMusbPlatformColdStartReset,
  MtkMusbPlatformColdStartPhySettle
} MTK_MUSB_PLATFORM_COLD_START_STEP;

typedef enum {
  MtkMusbPlatformColdGuardPassed = 0,
  MtkMusbPlatformColdGuardFresh,
  MtkMusbPlatformColdGuardSequence,
  MtkMusbPlatformColdGuardOwner,
  MtkMusbPlatformColdGuardPowerPath,
  MtkMusbPlatformColdGuardClockPhy,
  MtkMusbPlatformColdGuardFaultCleanup,
  MtkMusbPlatformColdGuardPhyMode,
  MtkMusbPlatformColdGuardSource,
  MtkMusbPlatformColdGuardRoleToken
} MTK_MUSB_PLATFORM_COLD_GUARD_REASON;

/* Reset sub-checks retain the last successful register read, so a physical
 * conflict identifies its actual gate rather than only the whole reset. */
typedef enum {
  MtkMusbPlatformResetNone = 0,
  MtkMusbPlatformResetIdle,
  MtkMusbPlatformResetStartPower,
  MtkMusbPlatformResetSession,
  MtkMusbPlatformResetRearmIdle,
  MtkMusbPlatformResetRearmPower,
  MtkMusbPlatformResetDmaClear,
  MtkMusbPlatformResetDmaConfig,
  MtkMusbPlatformResetController,
  MtkMusbPlatformResetBabble,
  MtkMusbPlatformResetFinalIdle,
  MtkMusbPlatformResetComplete
} MTK_MUSB_PLATFORM_COLD_RESET_CHECK;

/* Caller keeps this record alive until StopColdHost succeeds. Start failure
 * after an attempted write leaves ownership here for cleanup/retry. */
typedef struct {
  uint32_t Signature;
  uint32_t Kind;
  uint32_t OwnedMask;
  uint32_t LastStatus;
  uint32_t StartStep;
  uint32_t GuardPhase;
  uint32_t GuardReason;
  uint64_t ExclusiveOwnerToken;
  uint64_t DataRoleToken;
  uint64_t IdLowRoleToken;
  uint64_t LastGuardSequence;
  uint32_t ClockIdentity;
  uint32_t HostPowerPath;
  uint8_t OriginalSession;
  uint8_t OriginalPower;
  uint8_t ExpectedPower;
  uint8_t PowerOwnedBits;
  uint8_t SessionAttempted;
  uint8_t SessionVerified;
  uint8_t PowerAttempted;
  uint8_t PowerVerified;
  uint8_t PhyHostAttempted;
  uint8_t PhyHostVerified;
  uint8_t PhyOtgAttempted;
  uint8_t PhyOtgVerified;
  /* Latched from fresh stop evidence; the board must PowerOff this PHY next. */
  uint8_t PhyFaultedCleanup;
  uint8_t PhyInvalidAttempted;
  uint8_t PhyInvalidVerified;
  uint32_t ResetCheck;
  uint32_t ResetOffset;
  uint32_t ResetValue;
  uint64_t WaitSinceUs;
} MTK_MUSB_PLATFORM_COLD_HOST_STATE;

/* Cold path only: requires a stopped, disconnected controller. Follow the
 * stock settle, SESSION-clear, forced INVALID, hold, SESSION-set, generic/
 * forced HOST, idle DMA/reset/babble initialization, then POWER sequence.
 * Readback, fresh clock/role evidence and inactive endpoints/DMA/QMU remain
 * mandatory; no write synthesizes HOSTMODE/BDEVICE/VBUS or enables IRQs.
 * Type-C DFP and ID-low hub roles retain separate live evidence and tokens.
 * Call again after MtkMusbPlatformPending. StopColdHost also cancels a
 * pending start and restores any changes already made. Serialize both calls.
 * Invoke StopColdHost after a failed start whenever State->OwnedMask != 0. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformAdvanceColdHost(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  );

/* Stop retries owned SESSION/POWER changes after a faulted PHY and skips its
 * mode write so the board can call PowerOff/Exit. A healthy PHY returns to
 * OTG as before. Active endpoint, DMA, QMU or interrupt state blocks cleanup. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformStopColdHost(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS *Callbacks,
  MTK_MUSB_PLATFORM_COLD_HOST_STATE *State
  );

/* Board-only pre-publication preparation: preserve an inherited host session
 * and repair the FIFO table only when every other readiness condition passes.
 * Caller must collect fresh evidence and exclude all controller consumers. */
MTK_MUSB_PLATFORM_STATUS
MtkMusbPlatformConfigureInheritedHostFifo(
  const MTK_MUSB_PLATFORM_IO *Io,
  const MTK_MUSB_PLATFORM_EVIDENCE *Evidence
  );

#endif
