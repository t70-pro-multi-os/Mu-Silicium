/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_UFS_POWER_H_
#define MTK_UFS_POWER_H_

#include <stdint.h>

/* Shared fixed-size wire ABI. PRM and the private Storport request have
 * independent generation counters. No caller supplies MMIO/SMC addresses. */
#define MTK_UFS_PRM_HANDLER_GUID \
  { 0xc9aef12d, 0xe26c, 0x4be7, { 0xb3, 0x45, 0x62, 0xc5, 0xf3, 0x06, 0xd1, 0x84 } }
#define MTK_UFS_POWER_VERSION             1U
#define MTK_UFS_POWER_REQUEST_BYTES       64U
#define MTK_UFS_POWER_DEVICE_DOWN_ACKED   1U
#define MTK_UFS_POWER_CAP_RETAINED_VEMC   1U
#define MTK_UFS_POWER_CAP_LEAF_GATES      2U
#define MTK_UFS_POWER_CAP_PROGRESS        4U

/* Repeating an in-progress operation advances its saved transaction; it
 * must never resend a completed/uncertain SCSI or UIC command. */
typedef enum {
  MtkUfsPowerQuery = 0,
  MtkUfsPowerPrepareStop = 1,
  MtkUfsPowerRelease = 2,
  MtkUfsPowerRestore = 3
} MTK_UFS_POWER_OPERATION;

typedef enum {
  MtkUfsPowerDormant = 0,
  MtkUfsPowerActive,
  MtkUfsPowerStopPreparing,
  MtkUfsPowerStopPrepared,
  MtkUfsPowerReleased,
  MtkUfsPowerRestorePreparing,
  MtkUfsPowerFault
} MTK_UFS_POWER_STATE;

typedef enum {
  MtkUfsPowerSuccess = 0,
  MtkUfsPowerInProgress,
  MtkUfsPowerInvalid,
  MtkUfsPowerUnsupported,
  MtkUfsPowerWrongState,
  MtkUfsPowerIoError,
  MtkUfsPowerTimeout,
  MtkUfsPowerStaleGeneration,
  MtkUfsPowerBusy
} MTK_UFS_POWER_RESULT;

/* SRB_IO_CONTROL has this exact 8-byte signature and vendor control code.
 * The filter sends IOCTL_SCSI_MINIPORT to the lower Storport FDO. */
#define MTK_UFS_POWER_SRB_SIGNATURE       "HT70UFSP"
#define MTK_UFS_POWER_SRB_CONTROL_CODE    UINT32_C(0x48554650)
typedef enum {
  MtkUfsMiniPowerQuery = 0x100,
  MtkUfsMiniPowerPrepareDevice,
  MtkUfsMiniPowerStopHost,
  MtkUfsMiniPowerRestartHost
} MTK_UFS_MINIPORT_POWER_OPERATION;

typedef enum {
  MtkUfsMiniPowerActive = 0x100,
  MtkUfsMiniPowerDevicePreparing,
  MtkUfsMiniPowerDevicePrepared,
  MtkUfsMiniPowerHostStopping,
  MtkUfsMiniPowerHostStopped,
  MtkUfsMiniPowerHostRestarting,
  MtkUfsMiniPowerFault
} MTK_UFS_MINIPORT_POWER_STATE;

typedef struct {
  uint32_t Size;
  uint16_t Version;
  uint16_t Operation;
  uint32_t Flags;
  uint32_t State;          /* Output; caller initializes to zero. */
  uint64_t Generation;     /* Query: zero; mutations: latest endpoint token. */
  uint32_t Result;         /* Output; caller initializes to zero. */
  uint32_t Capabilities;   /* Output; caller initializes to zero. */
  uint64_t Reserved[4];    /* Must all be zero. */
} MTK_UFS_POWER_REQUEST;

#if defined(__cplusplus)
static_assert(sizeof(MTK_UFS_POWER_REQUEST) == MTK_UFS_POWER_REQUEST_BYTES, "UFS power wire size");
#else
_Static_assert(sizeof(MTK_UFS_POWER_REQUEST) == MTK_UFS_POWER_REQUEST_BYTES, "UFS power wire size");
#endif

#endif
