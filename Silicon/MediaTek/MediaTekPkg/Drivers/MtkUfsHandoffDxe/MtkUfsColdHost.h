/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_UFS_COLD_HOST_H_
#define MTK_UFS_COLD_HOST_H_
#include <Uefi.h>
#include <Library/MtkUfsPlatform.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Protocol/MtkClock.h>
#include <Protocol/MtkPmic.h>

#define MTK_UFS_COLD_HOST_SIGNATURE  0x55464348U

typedef enum {
  MtkUfsColdCleanupNone = 0,
  /* Acquisition failed; a fresh ten-root proof is required before gates can be released. */
  MtkUfsColdCleanupOwnedOnly,
  /* All ten HCI roots and powered MMIO were verified before release. */
  MtkUfsColdCleanupStoppedHost
} MTK_UFS_COLD_CLEANUP_MODE;

typedef enum {
  MtkUfsColdStopNone = 0,
  MtkUfsColdStopPrepared,
  MtkUfsColdStopHceProven,
  MtkUfsColdStopGateReleasePending,
  MtkUfsColdStopReleasedNeedsInit
} MTK_UFS_COLD_STOP_PHASE;

typedef struct {
  UINT32 Signature;
  MTK_UFS_PLATFORM Platform;
  EDKII_UFS_HC_DRIVER_INTERFACE *Interface;
  MTK_CLOCK_PROTOCOL *Clock;
  MTK_PMIC_PROTOCOL *Pmic;
  UINT32 ClockId[6];
  BOOLEAN ClockOwned[6];
  /* Historical shared-parent enable request; cleanup never disables TOP_UFS. */
  BOOLEAN TopClockEnableRequested;
  /* Shared VEMC retained-enable policy/history; this provider never disables it. */
  BOOLEAN VccRetained;
  /* Historical: this driver requested VEMC enable. Preserve that record through cleanup. */
  BOOLEAN VccOwned;
  BOOLEAN Suspended;
  BOOLEAN RefClockWaitConfigured;
  BOOLEAN CleanupPending;
  BOOLEAN CleanupVccOffConfirmed;
  MTK_UFS_COLD_CLEANUP_MODE CleanupMode;
  UINT32 VccRampDelayUs;
  BOOLEAN Configured;
  MTK_UFS_COLD_STOP_PHASE StopPhase;
  BOOLEAN StopProofValid;
  UINT32 StopGeneration;
  UINT32 StopProofGeneration;
  /* Set by the DXE provider so no cold-host MMIO or clock/PMIC writes can
   * continue if ExitBootServices interrupts an operation. */
  BOOLEAN *BootServicesExited;
  /* Latched at ExitBootServices callback entry, before its own handoff work. */
  BOOLEAN IoClosed;
} MTK_UFS_COLD_HOST;

/* Cold init enables the DT-declared UFS parent, gates and VEMC, then runs the
 * binary-grounded controller reset/refclock/HCE/link sequence. The T70 DTB
 * has no UFS PHY phandle or VA09-control property, so no separate PHY block
 * is touched. */
/* Callers must zero-initialize the object before Configure or Validate. */
EFI_STATUS MtkUfsConfigureColdHost(CONST VOID *Fdt, MTK_UFS_COLD_HOST *Host);
/*
 * Cleanup-pending retries must be serialized against any UFS start or I/O path
 * until the saved owned resources have all been released.
 */
/* Releases owned gates only after an unstarted/stopped-host proof; shared parent/VEMC stay on. */
EFI_STATUS MtkUfsReleaseUnstartedColdHost(MTK_UFS_COLD_HOST *Host);
/* Releases only this host's five gated clocks after the v4 consumer has
 * stopped HCE and the provider has minted a one-shot proof. Shared VEMC is
 * retained. Retries after a partial gate release do not access HCI MMIO. */
EFI_STATUS MtkUfsColdReleaseStartedHost(MTK_UFS_COLD_HOST *Host);
/* Read-only retained-host probe: never enables clocks/regulators or resets UFS. */
EFI_STATUS MtkUfsValidateRetainedHost(CONST VOID *Fdt, MTK_UFS_COLD_HOST *Host);
EFI_STATUS MtkUfsColdHostCallback(MTK_UFS_COLD_HOST *Host, EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase,
                                EDKII_UFS_HC_DRIVER_INTERFACE *Interface);
/* Caller must serialize against UFS pass-through I/O before entering suspend. */
EFI_STATUS MtkUfsColdHostSetManufacturer(MTK_UFS_COLD_HOST *Host, UINT16 ManufacturerId);
EFI_STATUS MtkUfsColdHostSuspend(MTK_UFS_COLD_HOST *Host, EDKII_UFS_HC_DRIVER_INTERFACE *Interface);
EFI_STATUS MtkUfsColdHostResume(MTK_UFS_COLD_HOST *Host, EDKII_UFS_HC_DRIVER_INTERFACE *Interface);
#endif
