/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_USB_HUB_CORE_H
#define MTK_USB_HUB_CORE_H

#include <stddef.h>
#include <stdint.h>

#define MTK_USB_HUB_CORE_REVISION 1U
#define MTK_USB_HUB_ID_SAMPLES 2U
#define MTK_USB_HUB_SETTLE_US 1000U
#define MTK_USB_HUB_GPIO_ID_PIN 10U
#define MTK_USB_HUB_GPIO_MUX_PIN 99U
#define MTK_USB_HUB_GPIO_USB5V_PIN 4U
#define MTK_USB_HUB_GPIO_RESET_PIN 104U

typedef enum {
  MtkUsbHubSuccess = 0,
  MtkUsbHubInvalid,
  MtkUsbHubNotReady,
  MtkUsbHubBusy,
  MtkUsbHubIdUnstable,
  MtkUsbHubRoleError,
  MtkUsbHubGpioError,
  MtkUsbHubDelayError,
  MtkUsbHubTimeout,
  MtkUsbHubQuarantined
} MTK_USB_HUB_STATUS;

/* Logical extcon GPIO roles; ID is input-only in this core. Pin numbers are
 * the exact T70 DT/binary mapping used by the platform callback. */
typedef enum {
  MtkUsbHubGpioId = 0,
  MtkUsbHubGpioMux,
  MtkUsbHubGpioUsb5v,
  MtkUsbHubGpioReset,
  MtkUsbHubGpioCount
} MTK_USB_HUB_GPIO;

typedef MTK_USB_HUB_STATUS
(*MTK_USB_HUB_READ_GPIO)(
  void *Context,
  MTK_USB_HUB_GPIO Gpio,
  uint8_t *Value
  );

/*
 * This is a read-only caller gate. Return Success only when the caller has
 * independently authorized the requested host path and delegated hub GPIO
 * control. It need not assert that the role switch is already active: stock
 * detect_cable calls set_hub before updating the role switch. The core never
 * selects a USB role or toggles Type-C/PD/VBUS policy.
 */
typedef MTK_USB_HUB_STATUS
(*MTK_USB_HUB_CHECK_HOST_ROLE)(
  void *Context
  );

typedef MTK_USB_HUB_STATUS
(*MTK_USB_HUB_WRITE_GPIO)(
  void *Context,
  MTK_USB_HUB_GPIO Gpio,
  uint8_t Value
  );

typedef MTK_USB_HUB_STATUS
(*MTK_USB_HUB_DELAY_US)(
  void *Context,
  uint32_t Microseconds
  );

typedef struct {
  void *Context;
  MTK_USB_HUB_READ_GPIO ReadGpio;
  MTK_USB_HUB_CHECK_HOST_ROLE CheckHostRole;
  MTK_USB_HUB_WRITE_GPIO WriteGpio;
  MTK_USB_HUB_DELAY_US DelayUs;
} MTK_USB_HUB_IO;

#define MTK_USB_HUB_STATE_INITIALIZED 1U
#define MTK_USB_HUB_STATE_PREPARED    2U
#define MTK_USB_HUB_STATE_OFF         3U
#define MTK_USB_HUB_STATE_ON          4U
#define MTK_USB_HUB_STATE_QUARANTINED 5U

typedef struct {
  MTK_USB_HUB_IO Io;
  uint32_t Signature;
  uint8_t State;
  uint8_t HostPrepared;
  uint8_t OwnedMask;
  uint8_t KnownMask;
  uint8_t ValueMask;
  uint8_t Reserved[3];
  MTK_USB_HUB_STATUS LastStatus;
} MTK_USB_HUB_CORE;

/*
 * Zero-initialize Core before Initialize. Initialize validates callbacks only;
 * it does not read or write GPIOs and does not change the role.
 */
MTK_USB_HUB_STATUS
MtkUsbHubCoreInitialize(
  MTK_USB_HUB_CORE *Core,
  const MTK_USB_HUB_IO *Io
  );

/*
 * Check the caller's host-role gate and sample GPIO10 ID twice. The stock DT
 * GPIO flags cell is zero; stock detect_cable requests hub-host when the
 * logical GPIO value is 0.
 * The two-sample consistency check is extra bounded protection, not the stock
 * driver debounce. This marks the path prepared without touching output GPIOs.
 */
MTK_USB_HUB_STATUS
MtkUsbHubCorePrepareHost(
  MTK_USB_HUB_CORE *Core
  );

/*
 * Apply the shipped extcon set_hub output order:
 * on: GPIO99 mux=1, 1000us, GPIO4 5V=1, 1000us, GPIO104 reset=1.
 * off: GPIO104 reset=0, GPIO4 5V=0, GPIO99 mux=0.
 *
 * PrepareHost is the caller's explicit delegation of output GPIO control to
 * this core. That delegation remains sticky so off/recovery stays available
 * after ID or role changes. A failed callback stops the sequence, records the
 * attempted line as owned-but-unknown, and quarantines the core because the
 * GPIO may have changed. SetHub(false) retries the complete off sequence and
 * clears quarantine only after every output write succeeds.
 *
 * IsOn is true only after a complete on sequence; false, including during
 * quarantine, does not prove the GPIOs are physically low. It does not promise
 * that the connector still has host role or that 5V is electrically present.
 * A later role/ID mismatch blocks another on request but preserves the known
 * output state until the caller explicitly turns it off.
 */
MTK_USB_HUB_STATUS
MtkUsbHubCoreSetHub(
  MTK_USB_HUB_CORE *Core,
  uint8_t Enable
  );

uint8_t
MtkUsbHubCoreIsPrepared(
  const MTK_USB_HUB_CORE *Core
  );

uint8_t
MtkUsbHubCoreIsOn(
  const MTK_USB_HUB_CORE *Core
  );

#endif
