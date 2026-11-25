/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_UFS_PLATFORM_H_
#define MTK_UFS_PLATFORM_H_
#include <stddef.h>
#include <stdint.h>

/* Platform sequencing recovered from the shipped T70Pro UFS module. The caller
 * must own the controller and supply powered, clocked, uncached MMIO. These
 * operations reset hardware; they do not issue SCSI or write storage media. */
typedef enum {
  MtkUfsPlatformOk = 0,
  MtkUfsPlatformInvalid,
  MtkUfsPlatformUnsupported,
  MtkUfsPlatformIoError,
  MtkUfsPlatformTimeout,
  MtkUfsPlatformBusy,
  MtkUfsPlatformWrongState
} MTK_UFS_PLATFORM_STATUS;

typedef enum { MtkUfsControllerBank, MtkUfsResetBank } MTK_UFS_MMIO_BANK;
typedef enum {
  MtkUfsPlatformIdle,
  MtkUfsPlatformHostPrepared,
  MtkUfsPlatformHostEnabled,
  MtkUfsPlatformLinkPrepared,
  MtkUfsPlatformLinkReady,
  MtkUfsPlatformLinkSuspended,
  MtkUfsPlatformFault,
  /* A consumer has powered the device down and the link entered Hibern8. */
  MtkUfsPlatformStopPrepared,
  /* HCE, run-stop, doorbells, list bases and IE are proven zero. */
  MtkUfsPlatformHceStopped,
  /* Refclock is off after a proven HCE stop; board gates may be released. */
  MtkUfsPlatformStopped
} MTK_UFS_PLATFORM_STATE;
typedef enum { MtkUfsRefClockUnknown, MtkUfsRefClockOff, MtkUfsRefClockOn } MTK_UFS_REF_CLOCK_STATE;
typedef enum {
  MtkUfsSramNoRestore,
  MtkUfsSramPowerOnRequired,
  MtkUfsSramRetained
} MTK_UFS_SRAM_RESUME;

typedef struct {
  void *Context;
  MTK_UFS_PLATFORM_STATUS (*Read32)(void *, MTK_UFS_MMIO_BANK, uint32_t, uint32_t *);
  MTK_UFS_PLATFORM_STATUS (*Write32)(void *, MTK_UFS_MMIO_BANK, uint32_t, uint32_t);
  void (*DelayUs)(void *, uint32_t);
  /* The callback must implement an ordered write / device memory barrier. */
  MTK_UFS_PLATFORM_STATUS (*Smc)(void *, const uint64_t Args[8], int64_t *Result);
  /* A bounded local UIC DME GET/SET. Attribute selector is zero. */
  MTK_UFS_PLATFORM_STATUS (*Dme)(void *, int Get, uint16_t Attribute, uint32_t *Value);
  /* Execute a synchronous UIC Hibern8 enter (Enter=1) or exit (Enter=0). */
  MTK_UFS_PLATFORM_STATUS (*Hibernate)(void *, int Enter);
} MTK_UFS_PLATFORM_IO;

typedef struct {
  uint32_t AssertOffset;
  uint32_t DeassertOffset;
  uint32_t Mask;
} MTK_UFS_RESET_LINE;

typedef struct {
  uint32_t ControllerLength;
  uint32_t ResetLength;
  uint32_t ControllerId; /* stock DT 'id'; defaults to 0, not the IP version */
  uint32_t RefClockGatingWaitUs;
  uint32_t RefClockUngatingWaitUs;
  /* HCI, UniPro, crypto; write-one SET/CLR aliases, no read/modify/write. */
  MTK_UFS_RESET_LINE Reset[3];
  /* T70 only: cmd32=-1 may retain SRAM while VCC/domain power stays on.
   * This is clock-gated Hibern8, not a rail-off/D3 recovery policy. */
  uint32_t AllowSramRetention;
} MTK_UFS_PLATFORM_CONFIG;

typedef struct {
  MTK_UFS_PLATFORM_IO Io;
  MTK_UFS_PLATFORM_CONFIG Config;
  MTK_UFS_PLATFORM_STATE State;
  MTK_UFS_REF_CLOCK_STATE RefClock;
  MTK_UFS_SRAM_RESUME SramResume;
  MTK_UFS_PLATFORM_STATUS RecoveryStatus;
  int64_t LastSmcResult;
  uint32_t IpVersion;
  uint32_t Signature;
} MTK_UFS_PLATFORM;

#define MTK_UFS_SMC_FUNCTION       UINT64_C(0xC2000276)
#define MTK_UFS_REFCLK_TIMEOUT_US  3000U
#define MTK_UFS_HOST_TIMEOUT_US    1000000U
#define MTK_UFS_HCE_DELAY_US       600U

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformInitialize(MTK_UFS_PLATFORM *, const MTK_UFS_PLATFORM_IO *, const MTK_UFS_PLATFORM_CONFIG *);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformSetRefClock(MTK_UFS_PLATFORM *, int Enable);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformResetDevice(MTK_UFS_PLATFORM *);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPreHce(MTK_UFS_PLATFORM *);
/* Explicit new Start only: caller has retired old requests, holds powered MMIO,
 * and has no unfinished SRAM/sleep transaction. Proves all DMA roots stopped. */
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformRestartStopped(MTK_UFS_PLATFORM *);
/* Full device shutdown only: caller completed device-WLUN POWERDOWN first.
 * PrepareStop enters Hibern8/DME powerdown but leaves HCE/refclock/gates on.
 * HostStopped proves all ten HCI roots zero, then disables the refclock; it
 * never issues SRAM-off SMC32. */
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPrepareStop(MTK_UFS_PLATFORM *);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformHostStopped(MTK_UFS_PLATFORM *);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPostHce(MTK_UFS_PLATFORM *);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPreLink(MTK_UFS_PLATFORM *);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPostLink(MTK_UFS_PLATFORM *);
/* Caller must quiesce UFS commands before suspending the active link. */
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformSuspend(MTK_UFS_PLATFORM *);
MTK_UFS_PLATFORM_STATUS MtkUfsPlatformResume(MTK_UFS_PLATFORM *);
#endif
