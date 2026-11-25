// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_USB_PHY_CORE_H
#define MTK_USB_PHY_CORE_H

#include <stddef.h>
#include <stdint.h>

#define MTK_USB_PHY_CORE_REVISION 1U
#define MTK_USB_TPHY_T70_U2_TYPE 3U
#define MTK_USB_TPHY_VERSION_V2 2U
#define MTK_USB_TPHY_T70_U2_MIN_BYTES 0x700U
#define MTK_USB_TPHY_T70_U2_RESOURCE_BASE 0x11f40000ULL
#define MTK_USB_TPHY_T70_NVMEM_CELL "intr_cal"
#define MTK_USB_TPHY_T70_NVMEM_MASK 0x1fU

#define MTK_USB_TPHY_TUNING_EYE_VRT   (1U << 0)
#define MTK_USB_TPHY_TUNING_EYE_TERM  (1U << 1)
#define MTK_USB_TPHY_TUNING_DISC_TH   (1U << 2)
#define MTK_USB_TPHY_TUNING_RX_SQTH   (1U << 3)
#define MTK_USB_TPHY_TUNING_REV4      (1U << 4)
#define MTK_USB_TPHY_TUNING_T70_REQUIRED 0x1fU

#define MTK_USB_TPHY_STATE_COLD        0U
#define MTK_USB_TPHY_STATE_INITIALIZED 1U
#define MTK_USB_TPHY_STATE_POWERED     2U
#define MTK_USB_TPHY_STATE_FAULTED     3U

typedef enum {
  MtkUsbPhySuccess = 0,
  MtkUsbPhyInvalid,
  MtkUsbPhyUnsupported,
  MtkUsbPhyNotReady,
  MtkUsbPhyBusy,
  MtkUsbPhyTimeout,
  MtkUsbPhyIoError,
  MtkUsbPhyClockError,
  MtkUsbPhyNvmemError,
  MtkUsbPhyTimeError,
  MtkUsbPhyDelayError
} MTK_USB_PHY_STATUS;

typedef enum {
  MtkUsbPhyClockRef = 0,
  MtkUsbPhyClockDaRef = 1,
  MtkUsbPhyClockCount = 2
} MTK_USB_PHY_CLOCK_SLOT;

/*
 * MMIO callback offsets are relative to the mapped PHY resource. An MMIO
 * callback error does not promise that hardware was untouched; the core keeps
 * clock ownership and requires Exit after an initialization access failure.
 */
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_READ32)(void *Context, uint32_t Offset, uint32_t *Value);
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_WRITE32)(void *Context, uint32_t Offset, uint32_t Value);
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_NOW_NS)(void *Context, uint64_t *Value);
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_DELAY_US)(void *Context, uint32_t Microseconds);

/*
 * ResolveClock returns Success with Present=0 for a missing optional clock.
 * Provider/lookup errors must be returned unchanged. Action callbacks are
 * atomic: on error, the named action has not acquired or released ownership.
 */
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_CLOCK_RESOLVE)(
  void *Context,
  MTK_USB_PHY_CLOCK_SLOT Slot,
  uint8_t *Present
  );
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_CLOCK_ACTION)(
  void *Context,
  MTK_USB_PHY_CLOCK_SLOT Slot
  );

/*
 * ReadNvmemCell receives the DT cell name and cell mask. It returns the raw
 * provider value without logging it; the core masks and shifts it locally.
 */
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_READ_NVMEM_CELL)(
  void *Context,
  const char *Name,
  uint32_t Mask,
  uint32_t *RawValue
  );

/* Values match the shipped Linux enum phy_mode. Only USB2 base modes are
 * supported; speed-specific, charger and diagnostic submodes are separate. */
typedef enum {
  MtkUsbPhyModeHost = 1,
  MtkUsbPhyModeDevice = 6,
  MtkUsbPhyModeOtg = 11
} MTK_USB_PHY_USB_MODE;

/* Optional nonzero host tuning properties in the stock PHY child. The T70 DT
 * supplies only DisconnectThreshold=7; every other member is zero. */
typedef struct {
  uint32_t SlewControl;
  uint32_t EyeVrt;
  uint32_t EyeTerm;
  uint32_t Rev6;
  uint32_t DisconnectThreshold;
} MTK_USB_PHY_HOST_TUNING;

typedef struct {
  void *Context;
  uint64_t PhyBase;
  uint64_t PhyLength;
  MTK_USB_PHY_READ32 Read32;
  MTK_USB_PHY_WRITE32 Write32;
  MTK_USB_PHY_NOW_NS NowNs;
  MTK_USB_PHY_DELAY_US DelayUs;
  MTK_USB_PHY_CLOCK_RESOLVE ResolveClock;
  MTK_USB_PHY_CLOCK_ACTION ClockPrepare;
  MTK_USB_PHY_CLOCK_ACTION ClockEnable;
  MTK_USB_PHY_CLOCK_ACTION ClockDisable;
  MTK_USB_PHY_CLOCK_ACTION ClockUnprepare;
  MTK_USB_PHY_READ_NVMEM_CELL ReadNvmemCell;
} MTK_USB_PHY_IO;

/* Cached PHY tuning from a previously validated host readback. This replay
 * profile avoids a second raw NVMEM calibration read during OS resume. */
typedef struct {
  uint32_t Acr1;
  uint32_t Acr5;
  uint32_t Acr6;
} MTK_USB_PHY_RESUME_SNAPSHOT;

typedef struct {
  uint32_t Revision;
  uint32_t PhyType;
  uint32_t TphyVersion;
  uint32_t TuningPresent;
  uint32_t EyeVrt;
  uint32_t EyeTerm;
  uint32_t DiscTh;
  uint32_t RxSqth;
  uint32_t Rev4;
  uint8_t SetEfuseV1;
  uint8_t SlewReferencePresent;
  uint8_t SlewCoefficientPresent;
  uint8_t Reserved;
  uint32_t SlewReferenceMHz;
  uint32_t SlewCoefficient;
} MTK_USB_PHY_CONFIG;

/*
 * Caller must zero-initialize this object and serialize all calls. MMIO
 * offsets are relative to IO.PhyBase. This core handles only the T70 U2 PHY
 * path (DT PHY argument 3, generic-tphy-v2); it does not choose USB role or
 * publish a hardware-ready marker.
 */
typedef struct {
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  uint32_t Signature;
  uint8_t State;
  uint8_t ClockPresentMask;
  uint8_t ClockPreparedMask;
  uint8_t ClockEnabledMask;
  uint8_t PowerCleanupPending;
  uint8_t CalibrationActive;
  uint8_t RegistersTouched;
} MTK_USB_PHY_CORE;

MTK_USB_PHY_STATUS
MtkUsbPhyCoreBuildResumeProfile(
  const MTK_USB_PHY_RESUME_SNAPSHOT *Snapshot,
  MTK_USB_PHY_CONFIG *Config,
  MTK_USB_PHY_HOST_TUNING *HostTuning
  );

MTK_USB_PHY_STATUS
MtkUsbPhyCoreInitialize(
  MTK_USB_PHY_CORE *Core,
  const MTK_USB_PHY_IO *Io,
  const MTK_USB_PHY_CONFIG *Config
  );

MTK_USB_PHY_STATUS
MtkUsbPhyCorePowerOn(
  MTK_USB_PHY_CORE *Core
  );

/* Caller must own a powered PHY and quiesce the controller first. This only
 * programs the PHY's ID/USB2 tuning; it does not negotiate Type-C, source VBUS
 * or prove an active host role. Host requires a validated host-tuning object;
 * Device/OTG require NULL. The T70 has no bc12/special-settings DT flag.
 * Any failed access invalidates the powered state until PowerOff succeeds. */
MTK_USB_PHY_STATUS
MtkUsbPhyCoreSetUsbMode(
  MTK_USB_PHY_CORE *Core,
  MTK_USB_PHY_USB_MODE Mode,
  const MTK_USB_PHY_HOST_TUNING *HostTuning
  );

/* MUSB's separate set_usb_phy_mode helper forces session signals after the
 * generic PHY mode call. Caller owns the idle controller and keeps this
 * zero-initialized record until restore or full PHY shutdown succeeds.
 * ACQUIRE snapshots without changing the live signals so HOST can follow the
 * generic PHY mode call without an intermediate INVALID write. Owned is 1
 * after ACQUIRE/INVALID, 2 after HOST was attempted. */
typedef struct {
  uint32_t OriginalDtm1;
  uint8_t Owned;
} MTK_USB_PHY_MUSB_SESSION;

typedef enum {
  MtkUsbPhyMusbInvalid = 0,
  MtkUsbPhyMusbHost,
  MtkUsbPhyMusbRestore,
  MtkUsbPhyMusbAcquire
} MTK_USB_PHY_MUSB_OPERATION;

MTK_USB_PHY_STATUS
MtkUsbPhyCoreSetMusbSession(
  MTK_USB_PHY_CORE *Core,
  MTK_USB_PHY_MUSB_SESSION *Session,
  MTK_USB_PHY_MUSB_OPERATION Operation
  );

MTK_USB_PHY_STATUS
MtkUsbPhyCorePowerOff(
  MTK_USB_PHY_CORE *Core
  );

MTK_USB_PHY_STATUS
MtkUsbPhyCoreExit(
  MTK_USB_PHY_CORE *Core
  );

uint8_t
MtkUsbPhyCoreIsInitialized(
  const MTK_USB_PHY_CORE *Core
  );

uint8_t
MtkUsbPhyCoreIsPowered(
  const MTK_USB_PHY_CORE *Core
  );

#endif
