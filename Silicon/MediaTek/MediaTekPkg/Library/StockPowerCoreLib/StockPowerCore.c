// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/StockPowerCore.h>

#define SC8989X_DRIVER_PART_MASK 0x94U
#define HUSB311_EXPECTED_VENDOR_ID 0x2E99U
#define HUSB311_EXPECTED_DEVICE_ID 0x0311U

static SP_POWER_STATUS
ReadRegisters(
  SP_READ_REGS Read,
  void *Context,
  SP_U8 Register,
  SP_U8 Length,
  SP_U8 *Bytes
  )
{
  if ((Read == 0) || (Bytes == 0) || (Length == 0)) {
    return SpPowerInvalid;
  }
  return Read(Context, Register, Length, Bytes);
}

static SP_U16
ReadLe16(
  const SP_U8 *Bytes
  )
{
  return (SP_U16)((SP_U16)Bytes[0] | ((SP_U16)Bytes[1] << 8));
}

static SP_POWER_SUPPLY_STATUS
DecodeChargeStatus(
  SP_U8 ChargeStateCode,
  SP_U8 VbusStateCode
  )
{
  static const SP_POWER_SUPPLY_STATUS Statuses[4] = {
    SpPowerSupplyNotCharging,
    SpPowerSupplyCharging,
    SpPowerSupplyCharging,
    SpPowerSupplyFull
  };

  if (VbusStateCode == 7U) {
    return SpPowerSupplyDischarging;
  }
  return Statuses[ChargeStateCode & 3U];
}

SP_POWER_STATUS
SpReadSc8989xIdentity(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_IDENTITY *Identity
  )
{
  SP_SC8989X_IDENTITY Result;
  SP_U8 PartRegister;
  SP_POWER_STATUS Status;

  if (Identity == 0) {
    return SpPowerInvalid;
  }
  Status = ReadRegisters(Read, Context, SP_SC8989X_REG_PART_ID, 1, &PartRegister);
  if (Status != SpPowerOk) {
    return Status;
  }

  Result.PartCode = (SP_U8)((PartRegister >> 3) & 7U);
  Result.StockDriverRecognizesCode =
    (SP_U8)((SC8989X_DRIVER_PART_MASK & (1U << Result.PartCode)) != 0);
  *Identity = Result;
  return SpPowerOk;
}

SP_POWER_STATUS
SpReadSc8989xBoostState(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_BOOST_STATE *State
  )
{
  SP_SC8989X_IDENTITY Identity;
  SP_SC8989X_BOOST_STATE Result;
  SP_U8 RegisterValue;
  SP_POWER_STATUS Status;

  if ((Read == 0) || (State == 0)) {
    return SpPowerInvalid;
  }

  Status = SpReadSc8989xIdentity(Read, Context, &Identity);
  if (Status != SpPowerOk) {
    return Status;
  }
  if (!Identity.StockDriverRecognizesCode) {
    return SpPowerUnsupported;
  }

  Status = ReadRegisters(
    Read,
    Context,
    SP_SC8989X_REG_BOOST_CONTROL,
    1,
    &RegisterValue
    );
  if (Status != SpPowerOk) {
    return Status;
  }

  Result.PartCode = Identity.PartCode;
  Result.BoostEnabled =
    (SP_U8)((RegisterValue & SP_SC8989X_BOOST_ENABLE_MASK) != 0U);
  *State = Result;
  return SpPowerOk;
}

SP_POWER_STATUS
SpReadSc8989xChargeState(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_CHARGE_STATE *State
  )
{
  SP_SC8989X_IDENTITY Identity;
  SP_SC8989X_CHARGE_STATE Result;
  SP_POWER_STATUS ReadStatus;

  if ((Read == 0) || (State == 0)) {
    return SpPowerInvalid;
  }
  ReadStatus = SpReadSc8989xIdentity(Read, Context, &Identity);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  if (!Identity.StockDriverRecognizesCode) {
    return SpPowerUnsupported;
  }

  ReadStatus = ReadRegisters(Read, Context, SP_SC8989X_REG_STATUS_B, 1, &Result.StatusB);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  Result.PartCode = Identity.PartCode;
  Result.ChargeStateCode = (SP_U8)((Result.StatusB >> 3) & 3U);
  Result.VbusStateCode = (SP_U8)((Result.StatusB >> 5) & 7U);
  Result.PowerGood = (SP_U8)((Result.StatusB >> 2) & 1U);
  Result.ChargeStatus = DecodeChargeStatus(Result.ChargeStateCode, Result.VbusStateCode);
  *State = Result;
  return SpPowerOk;
}

SP_POWER_STATUS
SpReadSc8989xStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_SC8989X_STATUS *Status
  )
{
  SP_SC8989X_CHARGE_STATE State;
  SP_SC8989X_STATUS Result;
  SP_POWER_STATUS ReadStatus;

  if (Status == 0) {
    return SpPowerInvalid;
  }
  ReadStatus = SpReadSc8989xChargeState(Read, Context, &State);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  ReadStatus = ReadRegisters(Read, Context, SP_SC8989X_REG_STATUS_C, 1, &Result.StatusC);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }

  Result.PartCode = State.PartCode;
  Result.StatusB = State.StatusB;
  Result.ChargeStateCode = State.ChargeStateCode;
  Result.VbusStateCode = State.VbusStateCode;
  Result.PowerGood = State.PowerGood;
  Result.Reg0CBit3 = (SP_U8)((Result.StatusC >> 3) & 1U);
  Result.ChargeStatus = State.ChargeStatus;
  *Status = Result;
  return SpPowerOk;
}

SP_POWER_STATUS
SpSc8989xDecodeAdc(
  SP_U8 Channel,
  SP_U32 RawValue,
  SP_U32 *ScaledValue,
  SP_ADC_UNIT *Unit
  )
{
  SP_U32 Result;
  SP_ADC_UNIT ResultUnit;

  if ((ScaledValue == 0) || (Unit == 0)) {
    return SpPowerInvalid;
  }

  switch (Channel) {
    case 2:
      if (RawValue > (0xFFFFFFFFU / 1000U)) {
        return SpPowerInvalid;
      }
      Result = RawValue * 1000U;
      ResultUnit = SpAdcMicroVolts;
      break;
    case 4:
      if (RawValue > (0xFFFFFFFFU / 1000U)) {
        return SpPowerInvalid;
      }
      Result = RawValue * 1000U;
      ResultUnit = SpAdcMicroAmps;
      break;
    case 3:
      Result = RawValue;
      ResultUnit = SpAdcMicroAmps;
      break;
    default:
      return SpPowerUnsupported;
  }

  *ScaledValue = Result;
  *Unit = ResultUnit;
  return SpPowerOk;
}

SP_POWER_STATUS
SpReadHusb311Identity(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_IDENTITY *Identity
  )
{
  SP_HUSB311_IDENTITY Result;
  SP_U8 Bytes[2];
  SP_POWER_STATUS Status;

  if (Identity == 0) {
    return SpPowerInvalid;
  }
  Status = ReadRegisters(Read, Context, SP_HUSB311_REG_VENDOR_ID, 2, Bytes);
  if (Status != SpPowerOk) {
    return Status;
  }
  Result.VendorId = ReadLe16(Bytes);

  Status = ReadRegisters(Read, Context, SP_HUSB311_REG_DEVICE_ID, 2, Bytes);
  if (Status != SpPowerOk) {
    return Status;
  }
  Result.DeviceId = ReadLe16(Bytes);
  Result.StockDriverRecognizesId = (SP_U8)(
    (Result.VendorId == HUSB311_EXPECTED_VENDOR_ID) &&
    (Result.DeviceId == HUSB311_EXPECTED_DEVICE_ID));
  *Identity = Result;
  return SpPowerOk;
}

SP_POWER_STATUS
SpReadHusb311PowerStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_POWER_STATUS *Status
  )
{
  SP_HUSB311_POWER_STATUS Result;
  SP_U8 Value;
  SP_POWER_STATUS ReadStatus;

  if (Status == 0) {
    return SpPowerInvalid;
  }
  ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_POWER_STATUS, 1, &Value);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  Result.PowerStatus = Value;
  Result.VbusPresent = (SP_U8)((Value >> 2) & 1U);

  ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_VENDOR_STATUS, 1, &Value);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  Result.VendorStatusReg97 = Value;
  Result.Reg97Bit1 = (SP_U8)((Value >> 1) & 1U);

  *Status = Result;
  return SpPowerOk;
}

int
SpHusb311VbusSafe0V(
  SP_U8 PowerStatus,
  SP_U8 VendorStatusReg97
  )
{
  return ((PowerStatus & 0x04U) == 0U) &&
    ((VendorStatusReg97 & 0x02U) != 0U);
}

SP_POWER_STATUS
SpReadHusb311CcStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_CC_STATUS *Status
  )
{
  SP_HUSB311_CC_STATUS Result;
  SP_U8 Value;
  SP_POWER_STATUS ReadStatus;

  if (Status == 0) {
    return SpPowerInvalid;
  }
  ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_CC_STATUS, 1, &Value);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }

  Result.CcStatus = Value;
  Result.CcStatusBit5 = (SP_U8)((Value >> 5) & 1U);
  if (Result.CcStatusBit5) {
    Result.Cc1Field = 0;
    Result.Cc2Field = 0;
    Result.DriverCc1Value = 0x0FU;
    Result.DriverCc2Value = 0x0FU;
    Result.CcAugmentationCode = 0;
    Result.RoleControlWasRead = 0;
    Result.RoleControl = 0;
  } else {
    Result.Cc1Field = (SP_U8)(Value & 3U);
    Result.Cc2Field = (SP_U8)((Value >> 2) & 3U);
    ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_ROLE_CONTROL, 1, &Value);
    if (ReadStatus != SpPowerOk) {
      return ReadStatus;
    }
    Result.RoleControlWasRead = 1;
    Result.RoleControl = Value;
    if ((Value & 0x40U) != 0) {
      Result.CcAugmentationCode = (SP_U8)((Result.CcStatus >> 4) & 1U);
    } else {
      Result.CcAugmentationCode = (SP_U8)((Value & 3U) != 1U);
    }
    Result.DriverCc1Value = (Result.Cc1Field == 0U) ? 0U :
      (SP_U8)(Result.Cc1Field | (Result.CcAugmentationCode << 2));
    Result.DriverCc2Value = (Result.Cc2Field == 0U) ? 0U :
      (SP_U8)(Result.Cc2Field | (Result.CcAugmentationCode << 2));
  }

  *Status = Result;
  return SpPowerOk;
}

SP_POWER_STATUS
SpReadHusb311Alerts(
  SP_READ_REGS Read,
  void *Context,
  SP_HUSB311_ALERT_STATUS *Status
  )
{
  SP_HUSB311_ALERT_STATUS Result;
  SP_U8 Bytes[2];
  SP_U8 VendorByte;
  SP_POWER_STATUS ReadStatus;

  if (Status == 0) {
    return SpPowerInvalid;
  }
  ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_ALERT, 2, Bytes);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  Result.AlertStatus = ReadLe16(Bytes);

  ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_VENDOR_ALERT, 1, &VendorByte);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  Result.AlertStatus |= ((SP_U32)VendorByte << 16);

  ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_ALERT_MASK, 2, Bytes);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  Result.AlertMask = ReadLe16(Bytes);

  ReadStatus = ReadRegisters(Read, Context, SP_HUSB311_REG_VENDOR_MASK, 1, &VendorByte);
  if (ReadStatus != SpPowerOk) {
    return ReadStatus;
  }
  Result.AlertMask |= ((SP_U32)VendorByte << 16);

  *Status = Result;
  return SpPowerOk;
}

SP_POWER_STATUS
SpReadHusb311FaultStatus(
  SP_READ_REGS Read,
  void *Context,
  SP_U8 *RawFaultStatus
  )
{
  SP_U8 Value;
  SP_POWER_STATUS Status;

  if (RawFaultStatus == 0) {
    return SpPowerInvalid;
  }
  Status = ReadRegisters(Read, Context, SP_HUSB311_REG_FAULT_STATUS, 1, &Value);
  if (Status != SpPowerOk) {
    return Status;
  }
  *RawFaultStatus = Value;
  return SpPowerOk;
}
