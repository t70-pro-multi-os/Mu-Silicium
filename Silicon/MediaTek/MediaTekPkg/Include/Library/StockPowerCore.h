// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef STOCK_POWER_CORE_H
#define STOCK_POWER_CORE_H

/*
 * Read-only portable decoders for status behavior observed in the shipped
 * SC8989x charger and HUSB311 TCPC Linux modules.
 *
 * The caller supplies a register-read callback. It must write the one-byte
 * register pointer and perform a repeated-start read. This interface contains
 * no register-write operation.
 */
typedef unsigned char  SP_U8;
typedef unsigned short SP_U16;
typedef unsigned int   SP_U32;

typedef enum {
  SpPowerOk = 0,
  SpPowerInvalid,
  SpPowerUnsupported,
  SpPowerIoError
} SP_POWER_STATUS;

typedef SP_POWER_STATUS (*SP_READ_REGS)(
  void *Context,
  SP_U8 FirstRegister,
  SP_U8 Length,
  SP_U8 *Bytes
);

typedef enum {
  SpPowerSupplyUnknown = 0,
  SpPowerSupplyCharging = 1,
  SpPowerSupplyDischarging = 2,
  SpPowerSupplyNotCharging = 3,
  SpPowerSupplyFull = 4
} SP_POWER_SUPPLY_STATUS;

typedef struct {
  SP_U8 PartCode;
  SP_U8 StockDriverRecognizesCode;
} SP_SC8989X_IDENTITY;

typedef struct {
  SP_U8 PartCode;
  SP_U8 StatusB;
  SP_U8 ChargeStateCode;
  SP_U8 VbusStateCode;
  SP_U8 PowerGood;
  SP_POWER_SUPPLY_STATUS ChargeStatus;
} SP_SC8989X_CHARGE_STATE;

typedef struct {
  SP_U8 PartCode;
  SP_U8 StatusB;
  SP_U8 StatusC;
  SP_U8 ChargeStateCode;
  SP_U8 VbusStateCode;
  SP_U8 PowerGood;
  SP_U8 Reg0CBit3;
  SP_POWER_SUPPLY_STATUS ChargeStatus;
} SP_SC8989X_STATUS;

typedef struct {
  SP_U8 PartCode;
  SP_U8 BoostEnabled;
} SP_SC8989X_BOOST_STATE;

typedef enum {
  SpAdcMicroVolts = 1,
  SpAdcMicroAmps = 2
} SP_ADC_UNIT;

typedef struct {
  SP_U16 VendorId;
  SP_U16 DeviceId;
  SP_U8 StockDriverRecognizesId;
} SP_HUSB311_IDENTITY;

typedef struct {
  SP_U8 PowerStatus;
  SP_U8 VbusPresent;
  SP_U8 VendorStatusReg97;
  SP_U8 Reg97Bit1;
} SP_HUSB311_POWER_STATUS;

typedef struct {
  SP_U8 CcStatus;
  SP_U8 CcStatusBit5;
  SP_U8 Cc1Field;
  SP_U8 Cc2Field;
  SP_U8 DriverCc1Value;
  SP_U8 DriverCc2Value;
  SP_U8 CcAugmentationCode;
  SP_U8 RoleControlWasRead;
  SP_U8 RoleControl;
} SP_HUSB311_CC_STATUS;

typedef struct {
  SP_U32 AlertStatus;
  SP_U32 AlertMask;
} SP_HUSB311_ALERT_STATUS;

#define SP_SC8989X_REG_PART_ID        0x14U
#define SP_SC8989X_REG_BOOST_CONTROL  0x03U
#define SP_SC8989X_BOOST_ENABLE_MASK  0x20U
#define SP_SC8989X_REG_STATUS_B       0x0BU
#define SP_SC8989X_REG_STATUS_C       0x0CU
#define SP_HUSB311_REG_VENDOR_ID       0x00U
#define SP_HUSB311_REG_DEVICE_ID      0x02U
#define SP_HUSB311_REG_ALERT          0x10U
#define SP_HUSB311_REG_ALERT_MASK     0x12U
#define SP_HUSB311_REG_CC_STATUS      0x1DU
#define SP_HUSB311_REG_POWER_STATUS  0x1EU
#define SP_HUSB311_REG_FAULT_STATUS  0x1FU
#define SP_HUSB311_REG_ROLE_CONTROL   0x1AU
#define SP_HUSB311_REG_VENDOR_STATUS  0x97U
#define SP_HUSB311_REG_VENDOR_ALERT   0x98U
#define SP_HUSB311_REG_VENDOR_MASK    0x99U

SP_POWER_STATUS
SpReadSc8989xIdentity(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_IDENTITY *Identity
  );

/* Reports the driver's reg 0x03 bit 5 boost-enable control readback. This is
 * not a measurement of voltage at the connector. */
SP_POWER_STATUS
SpReadSc8989xBoostState(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_BOOST_STATE *State
  );

/* Reads charge state without consuming the latched faults in status C. */
SP_POWER_STATUS
SpReadSc8989xChargeState(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_CHARGE_STATE *State
  );

SP_POWER_STATUS
SpReadSc8989xStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_STATUS *Status
  );

/* Pure scaling observed at the stock driver's VBUS, IBUS, and IBAT call sites.
 * It does not read or enable the charger's ADC. */
SP_POWER_STATUS
SpSc8989xDecodeAdc(
  SP_U8 Channel,
  SP_U32 RawValue,
  SP_U32 *ScaledValue,
  SP_ADC_UNIT *Unit
  );

SP_POWER_STATUS
SpReadHusb311Identity(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_IDENTITY *Identity
  );

SP_POWER_STATUS
SpReadHusb311PowerStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_POWER_STATUS *Status
  );

/* Stock HUSB311 safe0V uses POWER_STATUS bit 2 clear and HT_STATUS bit 1 set.
 * The caller must check TCPC initialization, detection, and role readiness. */
int
SpHusb311VbusSafe0V(
  SP_U8 PowerStatus,
  SP_U8 VendorStatusReg97
  );

SP_POWER_STATUS
SpReadHusb311CcStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_CC_STATUS *Status
  );

SP_POWER_STATUS
SpReadHusb311Alerts(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_ALERT_STATUS *Status
  );

SP_POWER_STATUS
SpReadHusb311FaultStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_U8 *RawFaultStatus
  );

#endif
