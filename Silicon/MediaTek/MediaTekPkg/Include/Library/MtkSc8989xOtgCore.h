/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_SC8989X_OTG_CORE_H
#define MTK_SC8989X_OTG_CORE_H

#include <stdint.h>

#define MTK_SC8989X_OTG_GPIO_ENABLE_PIN 48U
#define MTK_SC8989X_OTG_GPIO_OTG_PIN   86U
#define MTK_SC8989X_OTG_GPIO_MODE_GPIO 0U
#define MTK_SC8989X_OTG_GPIO_DIRECTION_OUTPUT 1U
#define MTK_SC8989X_OTG_REG_CHARGE_CONTROL 0x00U
#define MTK_SC8989X_OTG_REG_BOOST_CONTROL  0x03U
#define MTK_SC8989X_OTG_REG_BOOST_LIMIT    0x0AU
#define MTK_SC8989X_OTG_REG_PART_ID        0x14U
#define MTK_SC8989X_OTG_REG00_CLEAR_MASK   0x80U
#define MTK_SC8989X_OTG_REG03_CLEAR_MASK   0x10U
#define MTK_SC8989X_OTG_REG03_BOOST_MASK   0x20U
#define MTK_SC8989X_OTG_BOOST_PROFILE_MASK 0xF7U
#define MTK_SC8989X_OTG_BOOST_PROFILE_T70  0xC3U
#define MTK_SC8989X_OTG_HUSB_VBUS_PRESENT  0x04U
#define MTK_SC8989X_OTG_HUSB_VBUS_DETECT   0x08U
#define MTK_SC8989X_OTG_HUSB_INITIALIZED   0x40U

typedef enum {
  MtkSc8989xOtgSuccess = 0,
  MtkSc8989xOtgInvalid,
  MtkSc8989xOtgNotReady,
  MtkSc8989xOtgBusy,
  MtkSc8989xOtgIoError,
  MtkSc8989xOtgConflict,
  MtkSc8989xOtgUnsupported,
  MtkSc8989xOtgQuarantined
} MTK_SC8989X_OTG_STATUS;

typedef struct {
  uint32_t Mode;
  uint8_t Direction;
  uint8_t PhysicalValue;
} MTK_SC8989X_OTG_GPIO_STATE;

typedef struct {
  uint8_t ScPartIdRegister;
  uint8_t ScReg00;
  uint8_t ScReg03;
  uint8_t ScReg0A;
  uint16_t Husb311VendorId;
  uint16_t Husb311DeviceId;
  uint8_t Husb311PowerStatus;
  uint8_t Husb311VendorStatusReg97;
  uint8_t Husb311CcStatus;
  uint8_t Husb311RoleControl;
  MTK_SC8989X_OTG_GPIO_STATE Gpio48;
  MTK_SC8989X_OTG_GPIO_STATE Gpio86;
} MTK_SC8989X_OTG_EVIDENCE;

/* ReadFreshEvidence must collect the raw live values above on every call.
 * It returns an error if a required value cannot be read; it must not replace
 * the evidence with a caller-supplied readiness Boolean.
 * UpdateScRegisterBits changes only Mask bits, preserving all other current
 * bits under the caller's exclusive I2C ownership. A failed write may have
 * reached hardware. WriteGpio changes only the output value; it must not
 * silently change pin mux or direction. */
typedef MTK_SC8989X_OTG_STATUS
(*MTK_SC8989X_OTG_READ_EVIDENCE)(
  void *Context,
  MTK_SC8989X_OTG_EVIDENCE *Evidence
  );

typedef MTK_SC8989X_OTG_STATUS
(*MTK_SC8989X_OTG_UPDATE_SC_REGISTER_BITS)(
  void *Context,
  uint8_t Register,
  uint8_t Mask,
  uint8_t Value
  );

typedef MTK_SC8989X_OTG_STATUS
(*MTK_SC8989X_OTG_WRITE_GPIO)(
  void *Context,
  uint8_t Pin,
  uint8_t PhysicalValue
  );

typedef struct {
  void *Context;
  MTK_SC8989X_OTG_READ_EVIDENCE ReadFreshEvidence;
  MTK_SC8989X_OTG_UPDATE_SC_REGISTER_BITS UpdateScRegisterBits;
  MTK_SC8989X_OTG_WRITE_GPIO WriteGpio;
} MTK_SC8989X_OTG_IO;

typedef enum {
  MtkSc8989xOtgStateUninitialized = 0,
  MtkSc8989xOtgStateIdle,
  MtkSc8989xOtgStateTransition,
  MtkSc8989xOtgStateOutputEnabled,
  MtkSc8989xOtgStateQuarantined
} MTK_SC8989X_OTG_STATE;

typedef struct {
  MTK_SC8989X_OTG_IO Io;
  uint32_t Signature;
  MTK_SC8989X_OTG_STATE State;
  uint8_t OwnedMask;
  uint8_t RestoredMask;
  uint8_t InitialScPartIdRegister;
  uint8_t InitialHusb311CcStatus;
  uint8_t InitialHusb311RoleControl;
  uint8_t OriginalScReg00;
  uint8_t OriginalScReg03;
  uint8_t OriginalScReg0A;
  uint8_t OriginalGpio48;
  uint8_t OriginalGpio86;
  MTK_SC8989X_OTG_STATUS LastStatus;
} MTK_SC8989X_OTG_CORE;

/* Zero-initialize Core first. Initialization validates callbacks only and
 * does not access hardware. */
MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgInitialize(
  MTK_SC8989X_OTG_CORE *Core,
  const MTK_SC8989X_OTG_IO *Io
  );

/* Enable performs the guarded stock write sequence. Success means SC8989x
 * boost-control bit 5 read back enabled; it does not prove connector VBUS or
 * USB DFP/host readiness. */
MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgEnable(
  MTK_SC8989X_OTG_CORE *Core
  );

/* For a freshly owned T70 source attachment with boost off and safe0V.
 * Program the stock DT selectors before arming the boost output. */
MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgEnableCold(
  MTK_SC8989X_OTG_CORE *Core
  );

/* Restore only bits and pins this core attempted to change, in reverse order.
 * This is allowed after detach so a source can be shut down. */
MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgDisable(
  MTK_SC8989X_OTG_CORE *Core
  );

/* Retry cleanup after a partial write or external change quarantined Core. */
MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgRetryCleanup(
  MTK_SC8989X_OTG_CORE *Core
  );

uint8_t
MtkSc8989xOtgIsOutputEnabled(
  const MTK_SC8989X_OTG_CORE *Core
  );

uint8_t
MtkSc8989xOtgIsQuarantined(
  const MTK_SC8989X_OTG_CORE *Core
  );

#endif
