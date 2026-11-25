/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_PANEL_CORE_H
#define MTK_PANEL_CORE_H

#define MTK_PANEL_CORE_REVISION 2U

/* Keep this OS-neutral core freestanding; these widths are checked at build time. */
typedef unsigned char MTK_PANEL_U8;
typedef unsigned int MTK_PANEL_U32;
typedef char MTK_PANEL_TYPES_MUST_MATCH[
  (sizeof (MTK_PANEL_U8) == 1U && sizeof (MTK_PANEL_U32) == 4U) ? 1 : -1
  ];

/* EDP is the shipping LK/DT property spelling; it does not prove an eDP link. */
#define MTK_PANEL_GPIO_EDP_ENABLE       37U
#define MTK_PANEL_GPIO_EDP_RESET        43U
#define MTK_PANEL_GPIO_AVDD             3U
#define MTK_PANEL_GPIO_VDD18           155U
#define MTK_PANEL_GPIO_VDD33            49U
#define MTK_PANEL_GPIO_LK_UNNAMED_152  152U
#define MTK_PANEL_GPIO_AVEE            153U
#define MTK_PANEL_GPIO_VDD12            36U
#define MTK_PANEL_GPIO_BACKLIGHT        15U

#define MTK_PANEL_GPIO_MODE_GPIO         0U
#define MTK_PANEL_GPIO_DIRECTION_INPUT   0U
#define MTK_PANEL_GPIO_DIRECTION_OUTPUT  1U

#define MTK_PANEL_DELAY_10MS_US      10000U
#define MTK_PANEL_DELAY_5MS_US        5000U
#define MTK_PANEL_DELAY_20MS_US      20000U
#define MTK_PANEL_DELAY_50MS_US      50000U
#define MTK_PANEL_DELAY_100MS_US    100000U
#define MTK_PANEL_DELAY_1MS_US        1000U
#define MTK_PANEL_DELAY_2MS_US        2000U
#define MTK_PANEL_DELAY_510MS_US    510000U
/* The pinned shipping kernel is CONFIG_HZ=250. Its panel enable queues the
 * reset-high work for 125 jiffies, not 125 milliseconds. */
#define MTK_PANEL_KERNEL_ENABLE_DELAY_US 500000U

typedef enum {
  MtkPanelSuccess = 0,
  MtkPanelInvalid,
  MtkPanelBusy,
  MtkPanelNotReady,
  MtkPanelMappingError,
  MtkPanelIoError,
  MtkPanelDelayError,
  MtkPanelVerifyError,
  MtkPanelQuarantined
} MTK_PANEL_STATUS;

typedef enum {
  MtkPanelGpioEdpEnable = 0,
  MtkPanelGpioEdpReset,
  MtkPanelGpioAvdd,
  MtkPanelGpioVdd18,
  MtkPanelGpioVdd33,
  MtkPanelGpioLkUnnamed152,
  MtkPanelGpioAvee,
  MtkPanelGpioVdd12,
  MtkPanelGpioBacklight
} MTK_PANEL_GPIO_ROLE;

typedef MTK_PANEL_STATUS
(*MTK_PANEL_VALIDATE_GPIO)(
  void *Context,
  MTK_PANEL_U32 Pin,
  MTK_PANEL_GPIO_ROLE Role
  );

typedef MTK_PANEL_STATUS
(*MTK_PANEL_READ_GPIO)(
  void *Context,
  MTK_PANEL_U32 Pin,
  MTK_PANEL_U32 *Mode,
  MTK_PANEL_U32 *Direction,
  MTK_PANEL_U32 *OutputLatch
  );

typedef MTK_PANEL_STATUS
(*MTK_PANEL_SET_MODE)(
  void *Context,
  MTK_PANEL_U32 Pin,
  MTK_PANEL_U32 Mode
  );

typedef MTK_PANEL_STATUS
(*MTK_PANEL_SET_DIRECTION)(
  void *Context,
  MTK_PANEL_U32 Pin,
  MTK_PANEL_U32 Direction
  );

typedef MTK_PANEL_STATUS
(*MTK_PANEL_SET_VALUE)(
  void *Context,
  MTK_PANEL_U32 Pin,
  MTK_PANEL_U32 Value
  );

typedef MTK_PANEL_STATUS
(*MTK_PANEL_DELAY_US)(
  void *Context,
  MTK_PANEL_U32 Microseconds
  );

typedef struct {
  void *Context;
  MTK_PANEL_VALIDATE_GPIO ValidateGpio;
  MTK_PANEL_READ_GPIO ReadGpio;
  MTK_PANEL_SET_MODE SetMode;
  MTK_PANEL_SET_DIRECTION SetDirection;
  MTK_PANEL_SET_VALUE SetValue;
  MTK_PANEL_DELAY_US DelayUs;
} MTK_PANEL_IO;

typedef enum {
  MtkPanelStateUninitialized = 0,
  MtkPanelStateIdle,
  MtkPanelStateRunningLkInit,
  MtkPanelStateLkInitComplete,
  MtkPanelStateRunningLkSuspend,
  MtkPanelStateLkSuspendComplete,
  MtkPanelStateQuarantined,
  MtkPanelStateKernelPrepared,
  MtkPanelStateKernelEnabled,
  MtkPanelStateKernelUnprepared,
  MtkPanelStateRunningKernelPrepare,
  MtkPanelStateRunningKernelEnable,
  MtkPanelStateRunningKernelUnprepare
} MTK_PANEL_STATE;

typedef enum {
  MtkPanelOperationNone = 0,
  MtkPanelOperationLkInit,
  MtkPanelOperationLkSuspend,
  MtkPanelOperationKernelPrepare,
  MtkPanelOperationKernelEnable,
  MtkPanelOperationKernelUnprepare
} MTK_PANEL_OPERATION;

typedef struct {
  MTK_PANEL_IO Io;
  MTK_PANEL_U32 Signature;
  MTK_PANEL_STATE State;
  MTK_PANEL_OPERATION PendingOperation;
  MTK_PANEL_U32 ActionIndex;
  MTK_PANEL_U8 ActionAttempted;
  MTK_PANEL_U8 Busy;
  MTK_PANEL_U8 Reserved[2];
  MTK_PANEL_STATUS LastStatus;
} MTK_PANEL_CORE;

/* Zero-initialize Core before calling Initialize. The caller must hold the
 * board's pin lease and serialize the entire sequence, including retries;
 * Busy is a reentry guard, not a cross-CPU lock. ValidateGpio must verify both
 * the board mapping and caller's ownership for every listed role, and reject
 * an unconfirmed mapping for unnamed LK GPIO 152. ReadGpio reports current
 * mode, direction and output latch for each write check. */
MTK_PANEL_STATUS
MtkPanelCoreInitialize(
  MTK_PANEL_CORE *Core,
  const MTK_PANEL_IO *Io
  );

/* Reproduce the exact along_cyx LCM init sequence. Success means every
 * requested GPIO setting and delay was accepted and read back; it is not DSI
 * initialization or proof of visible pixels. A completed state records that
 * this sequence ran; it is not a live power-ready assertion. A failed or
 * uncertain action stays at that action and the same operation can be retried. */
MTK_PANEL_STATUS
MtkPanelCoreRunLkInit(
  MTK_PANEL_CORE *Core
  );

/* Reproduce the LK suspend callback. Stock LK's suspend path leaves GPIOs
 * 152, 155 and 36 high, so completion does not mean all rails are off. The
 * stock resume callback is an empty return; this core does not claim a resume
 * sequence. */
MTK_PANEL_STATUS
MtkPanelCoreRunLkSuspend(
  MTK_PANEL_CORE *Core
  );

MTK_PANEL_U8
MtkPanelCoreLkInitComplete(
  const MTK_PANEL_CORE *Core
  );

/* Shipping Android panel lifecycle, from the pinned along_cyx module. These
 * operations leave GPIO 152 unchanged; they still require the complete board
 * lease because the caller serializes display/touch ownership as one unit.
 * Prepare performs the rail-on sequence and raises the backlight supply.
 * It does not configure DSI or start scanout. GPIO 37 remains low after
 * unprepare until the separate enable operation completes. Do not infer
 * a verified visible resume from successful rail preparation. */
MTK_PANEL_STATUS
MtkPanelCoreRunKernelPrepare(
  MTK_PANEL_CORE *Core
  );

/* Call only after the host has configured and started its display stream.
 * This serialized synchronous equivalent waits at least the stock 125/HZ
 * interval and raises GPIO 37 (edp_en). No deferred callback can outlive the call;
 * the caller holds its lease and checks shutdown around every IO callback. */
MTK_PANEL_STATUS
MtkPanelCoreRunKernelEnable(
  MTK_PANEL_CORE *Core
  );

/* The caller has stopped the display stream before removing panel power.
 * Serial execution replaces the stock disable/cancel-delayed-work step;
 * there is no queued reset-high callback after this operation. */
MTK_PANEL_STATUS
MtkPanelCoreRunKernelUnprepare(
  MTK_PANEL_CORE *Core
  );

#endif
