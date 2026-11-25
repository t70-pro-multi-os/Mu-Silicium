// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkHusb311TypecCore.h>
#include <Library/StockPowerCore.h>

#define HUSB311_TYPEC_SIGNATURE 0x48333131U
#define HUSB311_EXPECTED_VENDOR_ID 0x2E99U
#define HUSB311_EXPECTED_DEVICE_ID 0x0311U
#define HUSB311_ROLE_OPEN_UPDATE_MASK \
  (MTK_HUSB311_TYPEC_ROLE_DRP_MASK | MTK_HUSB311_TYPEC_ROLE_CC_MASK)
#define HUSB311_ROLE_WRITABLE_MASK 0x7FU
#define HUSB311_STOCK_OPEN_ROLE 0x2FU
#define HUSB311_SINK_RD_ROLE 0x0AU
#define HUSB311_ROLE_MAX_ATTEMPTS 5U
#define HUSB311_PARTIAL_ROLE_SETTLE_MS 20U

static void
ZeroBytes(
  void *Buffer,
  MTK_HUSB311_U32 Length
  )
{
  MTK_HUSB311_U8 *Bytes;
  MTK_HUSB311_U32 Index;

  Bytes = (MTK_HUSB311_U8 *)Buffer;
  for (Index = 0U; Index < Length; ++Index) {
    Bytes[Index] = 0U;
  }
}

static MTK_HUSB311_TYPEC_STATUS
ReadRegister(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 Length,
  MTK_HUSB311_U8 *Bytes
  )
{
  MTK_HUSB311_TYPEC_STATUS Status;

  if ((Core == 0) || (Core->Io.ReadRegisters == 0) || (Bytes == 0) ||
      (Length == 0U)) {
    return MtkHusb311TypecInvalid;
  }
  Core->Diagnostic.Register = Register;
  Core->Diagnostic.Length = Length;
  Core->Diagnostic.Value = 0xFFFFU;
  Status = Core->Io.ReadRegisters(Core->Io.Context, Register, Length, Bytes);
  Core->Diagnostic.ReadStatus = Status;
  if (Status == MtkHusb311TypecSuccess) {
    Core->Diagnostic.Value = Bytes[0];
    if (Length > 1U) {
      Core->Diagnostic.Value |= (MTK_HUSB311_U16)Bytes[1] << 8;
    }
  }
  return Status;
}

/* Select an already-read value that actually rejected the request. No I/O. */
static MTK_HUSB311_TYPEC_STATUS
RejectRegister(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 Length,
  MTK_HUSB311_U16 Value
  )
{
  Core->Diagnostic.Register = Register;
  Core->Diagnostic.Length = Length;
  Core->Diagnostic.Value = Value;
  Core->Diagnostic.ReadStatus = MtkHusb311TypecSuccess;
  return MtkHusb311TypecNotReady;
}

static MTK_HUSB311_TYPEC_STATUS
Read8(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 *Value
  )
{
  MTK_HUSB311_U8 Result;
  MTK_HUSB311_TYPEC_STATUS Status;

  if (Value == 0) {
    return MtkHusb311TypecInvalid;
  }
  Status = ReadRegister(Core, Register, 1U, &Result);
  if (Status == MtkHusb311TypecSuccess) {
    *Value = Result;
  }
  return Status;
}

static SP_POWER_STATUS
ReadStockAdapter(
  void *Context,
  SP_U8 FirstRegister,
  SP_U8 Length,
  SP_U8 *Bytes
  )
{
  MTK_HUSB311_TYPEC_CORE *Core;

  Core = (MTK_HUSB311_TYPEC_CORE *)Context;
  if (ReadRegister(Core, FirstRegister, Length, Bytes) ==
      MtkHusb311TypecSuccess) {
    return SpPowerOk;
  }
  return SpPowerIoError;
}

static MTK_HUSB311_TYPEC_STATUS
ReadIdentity(
  MTK_HUSB311_TYPEC_CORE *Core
  )
{
  SP_HUSB311_IDENTITY Identity;

  if (SpReadHusb311Identity(ReadStockAdapter, Core, &Identity) != SpPowerOk) {
    return MtkHusb311TypecIoError;
  }
  if ((Identity.VendorId != HUSB311_EXPECTED_VENDOR_ID) ||
      (Identity.DeviceId != HUSB311_EXPECTED_DEVICE_ID) ||
      (Identity.StockDriverRecognizesId == 0U)) {
    return Identity.VendorId != HUSB311_EXPECTED_VENDOR_ID ?
      RejectRegister(Core, MTK_HUSB311_TYPEC_REG_VENDOR_ID, 2U, Identity.VendorId) :
      RejectRegister(Core, MTK_HUSB311_TYPEC_REG_DEVICE_ID, 2U, Identity.DeviceId);
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
ReadSafe0V(
  MTK_HUSB311_TYPEC_CORE *Core,
  int *Safe
  )
{
  SP_HUSB311_POWER_STATUS Power;

  if (Safe == 0) {
    return MtkHusb311TypecInvalid;
  }
  if (SpReadHusb311PowerStatus(ReadStockAdapter, Core, &Power) != SpPowerOk) {
    return MtkHusb311TypecIoError;
  }
  *Safe = SpHusb311VbusSafe0V(Power.PowerStatus, Power.VendorStatusReg97);
  if (!*Safe) {
    (void)((Power.PowerStatus & 0x04U) != 0U ?
      RejectRegister(Core, MTK_HUSB311_TYPEC_REG_POWER_STATUS, 1U, Power.PowerStatus) :
      RejectRegister(Core, MTK_HUSB311_TYPEC_REG_VENDOR_STATUS, 1U, Power.VendorStatusReg97));
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
ReadCc(
  MTK_HUSB311_TYPEC_CORE *Core,
  SP_HUSB311_CC_STATUS *Cc
  )
{
  if (Cc == 0) {
    return MtkHusb311TypecInvalid;
  }
  if (SpReadHusb311CcStatus(ReadStockAdapter, Core, Cc) != SpPowerOk) {
    return MtkHusb311TypecIoError;
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
ReadAlert(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U16 *Alert
  )
{
  MTK_HUSB311_U8 Bytes[2];
  MTK_HUSB311_U8 VendorAlert;
  MTK_HUSB311_U16 TxResult = 0U;
  MTK_HUSB311_TYPEC_STATUS Status;
  unsigned Pass;

  if (Alert == 0) {
    return MtkHusb311TypecInvalid;
  }
  for (Pass = 0U; Pass < 2U; ++Pass) {
    Status = ReadRegister(Core, MTK_HUSB311_TYPEC_REG_ALERT, 2U, Bytes);
    if (Status != MtkHusb311TypecSuccess) {
      return Status;
    }
    *Alert = (MTK_HUSB311_U16)((MTK_HUSB311_U16)Bytes[0] |
                              ((MTK_HUSB311_U16)Bytes[1] << 8));
    Status = Read8(Core, MTK_HUSB311_TYPEC_REG_VENDOR_ALERT, &VendorAlert);
    if (Status != MtkHusb311TypecSuccess) {
      return Status;
    }
    if ((VendorAlert & (MTK_HUSB311_U8)~MTK_HUSB311_TYPEC_ALERT_VBUS_80_MASK) != 0U) {
      return RejectRegister(Core, MTK_HUSB311_TYPEC_REG_VENDOR_ALERT, 1U, VendorAlert);
    }
    if (((*Alert & (MTK_HUSB311_U16)~(
           MTK_HUSB311_TYPEC_ALERT_ALLOWED_MASK |
           MTK_HUSB311_TYPEC_ALERT_TX_RESULT_MASK)) != 0U) ||
        ((Pass != 0U) && ((*Alert & MTK_HUSB311_TYPEC_ALERT_TX_RESULT_MASK) != TxResult))) {
      return RejectRegister(Core, MTK_HUSB311_TYPEC_REG_ALERT, 2U, *Alert);
    }
    if (VendorAlert == 0U) {
      return MtkHusb311TypecSuccess;
    }
    if (Pass != 0U) {
      return RejectRegister(Core, MTK_HUSB311_TYPEC_REG_VENDOR_ALERT, 1U, VendorAlert);
    }
    TxResult = (MTK_HUSB311_U16)(*Alert & MTK_HUSB311_TYPEC_ALERT_TX_RESULT_MASK);
    /* A latched VBUS event is not a voltage sample or a source-role token. */
    Status = Core->Io.UpdateRegister(Core->Io.Context,
      MTK_HUSB311_TYPEC_REG_VENDOR_ALERT,
      MTK_HUSB311_TYPEC_ALERT_VBUS_80_MASK, MTK_HUSB311_TYPEC_ALERT_VBUS_80_MASK);
    if (Status != MtkHusb311TypecSuccess) {
      return Status;
    }
  }
  return MtkHusb311TypecNotReady;
}

static MTK_HUSB311_TYPEC_STATUS
ReadProtocolState(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U16 *TxResult
  )
{
  MTK_HUSB311_U8 TcpcControl;
  MTK_HUSB311_U8 PowerControl;
  MTK_HUSB311_U8 Fault;
  MTK_HUSB311_U16 Alert;
  MTK_HUSB311_TYPEC_STATUS Status;

  if (TxResult == 0) {
    return MtkHusb311TypecInvalid;
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_TCPC_CONTROL, &TcpcControl);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_POWER_CONTROL, &PowerControl);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if ((TcpcControl & MTK_HUSB311_TYPEC_TCPC_BIST_MASK) != 0U) {
    return RejectRegister(Core, MTK_HUSB311_TYPEC_REG_TCPC_CONTROL, 1U, TcpcControl);
  }
  if ((PowerControl & MTK_HUSB311_TYPEC_POWER_VCONN_MASK) != 0U) {
    return RejectRegister(Core, MTK_HUSB311_TYPEC_REG_POWER_CONTROL, 1U, PowerControl);
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_FAULT_STATUS, &Fault);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if (Fault != 0U) {
    return MtkHusb311TypecNotReady;
  }
  Status = ReadAlert(Core, &Alert);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  *TxResult = (MTK_HUSB311_U16)(Alert &
                                  MTK_HUSB311_TYPEC_ALERT_TX_RESULT_MASK);
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
CheckGuards(
  MTK_HUSB311_TYPEC_CORE *Core,
  int RequireSourceOff
  )
{
  MTK_HUSB311_TYPEC_GUARDS Guards;
  MTK_HUSB311_TYPEC_STATUS Status;

  if ((Core == 0) || (Core->Io.ReadGuards == 0)) {
    return MtkHusb311TypecInvalid;
  }
  ZeroBytes(&Guards, (MTK_HUSB311_U32)sizeof(Guards));
  Status = Core->Io.ReadGuards(Core->Io.Context, &Guards);
  Core->Diagnostic.GuardFlags = Status == MtkHusb311TypecSuccess ?
    (MTK_HUSB311_U8)((Guards.PowerBusLeaseHeld == 1U) |
      ((Guards.TcpcInitialized == 1U) << 1) |
      ((Guards.CcDetectionEnabled == 1U) << 2) |
      ((Guards.ScBoostEnabled == 0U) << 3) |
      ((Guards.HusbVbusSafe0V == 1U) << 4) |
      (((Guards.MusbDevCtl & MTK_HUSB311_TYPEC_MUSB_SESSION_MASK) == 0U) << 5) |
      ((Guards.MusbControllerIdle == 1U) << 6)) : 0xFFU;
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if ((Guards.PowerBusLeaseHeld != 1U) ||
      (Guards.TcpcInitialized != 1U) ||
      (Guards.CcDetectionEnabled != 1U)) {
    return MtkHusb311TypecNotReady;
  }
  if (RequireSourceOff) {
    if ((Guards.ScBoostEnabled != 0U) ||
        (Guards.HusbVbusSafe0V != 1U) ||
        ((Guards.IndependentControllerRole != 1U) &&
         (((Guards.MusbDevCtl & MTK_HUSB311_TYPEC_MUSB_SESSION_MASK) != 0U) ||
          (Guards.MusbControllerIdle != 1U)))) {
      return MtkHusb311TypecNotReady;
    }
  }
  return MtkHusb311TypecSuccess;
}

static void
SetConflict(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 Mask,
  MTK_HUSB311_U8 Expected,
  MTK_HUSB311_U8 Target,
  MTK_HUSB311_U8 Field,
  MTK_HUSB311_U8 RoleMask,
  int WriteAttempted,
  int Restoring
  )
{
  if (Core->ConflictActive == 0U) {
    Core->ConflictRegister = Register;
    Core->ConflictMask = Mask;
    Core->ConflictExpected = (MTK_HUSB311_U8)(Expected & Mask);
    Core->ConflictTarget = (MTK_HUSB311_U8)(Target & Mask);
    Core->ConflictField = Field;
    Core->ConflictRoleMask = RoleMask;
    Core->ConflictWriteAttempted = (MTK_HUSB311_U8)(WriteAttempted != 0);
    Core->ConflictRestoring = (MTK_HUSB311_U8)(Restoring != 0);
    Core->ConflictActive = 1U;
  }
  Core->LastError = MtkHusb311TypecConflict;
  Core->State = MtkHusb311TypecStateQuarantined;
}

static int
OnlyRpLevelChanged(
  MTK_HUSB311_U8 Before,
  MTK_HUSB311_U8 Target,
  MTK_HUSB311_U8 Observed
  )
{
  return (Observed != Before) &&
         (((Observed ^ Before) & ~MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK) == 0U) &&
         (((Observed ^ Target) & MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK) == 0U);
}

static MTK_HUSB311_TYPEC_STATUS
UpdateExact(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 Mask,
  MTK_HUSB311_U8 ExpectedBefore,
  MTK_HUSB311_U8 Target,
  MTK_HUSB311_U8 Field,
  MTK_HUSB311_U8 RoleMask,
  int Restoring,
  MTK_HUSB311_U8 *After,
  MTK_HUSB311_U8 *AppliedMask
  )
{
  MTK_HUSB311_U8 Before;
  MTK_HUSB311_U8 Readback;
  MTK_HUSB311_TYPEC_STATUS UpdateStatus;
  MTK_HUSB311_TYPEC_STATUS Status;
  int FullRoleWrite;
  int Opening;
  int StartingSource;
  int MayApplyRpOnly;

  if ((Core == 0) || (After == 0) || (AppliedMask == 0) ||
      (Core->Io.UpdateRegister == 0)) {
    return MtkHusb311TypecInvalid;
  }
  *AppliedMask = 0U;
  *After = ExpectedBefore;
  Status = Read8(Core, Register, &Before);
  if (Status != MtkHusb311TypecSuccess) {
    Core->State = MtkHusb311TypecStateQuarantined;
    Core->LastError = Status;
    return MtkHusb311TypecQuarantined;
  }
  *After = Before;
  if ((Before & Mask) != (ExpectedBefore & Mask)) {
    SetConflict(Core, Register, Mask, ExpectedBefore, Target, Field,
                RoleMask, 0, Restoring);
    return MtkHusb311TypecConflict;
  }
  if ((Before & Mask) == (Target & Mask)) {
    *After = Before;
    return MtkHusb311TypecSuccess;
  }

  FullRoleWrite = !Restoring &&
                  (Register == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL) &&
                  (Mask == HUSB311_ROLE_WRITABLE_MASK);
  Opening = FullRoleWrite && (Target == HUSB311_STOCK_OPEN_ROLE);
  StartingSource = FullRoleWrite && (Core->State == MtkHusb311TypecStatePreparing) &&
                   (Target == MTK_HUSB311_TYPEC_ROLE_SOURCE_RP_DFT) &&
                   ((Before & ~MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK) ==
                    MTK_HUSB311_TYPEC_ROLE_OPEN_FIELDS);
  UpdateStatus = Core->Io.UpdateRegister(
                   Core->Io.Context, Register, Mask,
                   (MTK_HUSB311_U8)(Target & Mask));
  if (Opening && (Core->State == MtkHusb311TypecStatePreparing)) {
    ++Core->Diagnostic.OpenAttempts;
    Core->Diagnostic.OpenWriteStatus = (MTK_HUSB311_U8)UpdateStatus;
  }
  MayApplyRpOnly = (UpdateStatus == MtkHusb311TypecSuccess) &&
                   (Core->State == MtkHusb311TypecStatePreparing) &&
                   Opening && (Before == HUSB311_SINK_RD_ROLE);
  Status = Read8(Core, Register, &Readback);
  if ((Opening || StartingSource) && (Status == MtkHusb311TypecSuccess) &&
      ((Readback == Before) ||
       (MayApplyRpOnly && OnlyRpLevelChanged(Before, Target, Readback))) &&
      ((UpdateStatus == MtkHusb311TypecSuccess) ||
       (UpdateStatus == MtkHusb311TypecIoError))) {
    /* Let a late write become observable before deciding whether to repeat it. */
    Status = Core->Io.DelayMs(Core->Io.Context,
               Readback != Before ? HUSB311_PARTIAL_ROLE_SETTLE_MS : 1U);
    if (Status == MtkHusb311TypecSuccess) {
      Status = Read8(Core, Register, &Readback);
    }
  }
  if (Status != MtkHusb311TypecSuccess) {
    SetConflict(Core, Register, Mask, Before, Target, Field, RoleMask,
                1, Restoring);
    Core->LastError = Status;
    return MtkHusb311TypecQuarantined;
  }
  *After = Readback;
  if ((Readback & Mask) == (Target & Mask)) {
    *AppliedMask = Mask;
    if (UpdateStatus != MtkHusb311TypecSuccess) {
      Core->LastError = UpdateStatus;
      return MtkHusb311TypecIoError;
    }
    return MtkHusb311TypecSuccess;
  }
  if (MayApplyRpOnly && OnlyRpLevelChanged(Before, Target, Readback)) {
    /* The inactive Rp field changed while both CC pins stayed at Rd or Open. */
    *AppliedMask = MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK;
    Core->LastError = MtkHusb311TypecIoError;
    return Core->LastError;
  }
  if ((Readback & Mask) == (Before & Mask)) {
    Core->LastError = (Opening || StartingSource) &&
                      (UpdateStatus != MtkHusb311TypecSuccess)
                        ? UpdateStatus : MtkHusb311TypecIoError;
    return Core->LastError;
  }
  SetConflict(Core, Register, Mask, Before, Target, Field, RoleMask,
              1, Restoring);
  return MtkHusb311TypecConflict;
}

static void
RecordChangedField(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Field,
  MTK_HUSB311_U8 RoleMask,
  MTK_HUSB311_U8 After
  )
{
  Core->OwnedFields = (MTK_HUSB311_U8)(Core->OwnedFields | Field);
  if (Field == MTK_HUSB311_TYPEC_TOKEN_FIELD_RX) {
    Core->ExpectedRxDetect = After;
  } else if (Field == MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE) {
    Core->RoleOwnedMask = (MTK_HUSB311_U8)(Core->RoleOwnedMask | RoleMask);
    Core->ExpectedRoleControl = After;
  } else if (Field == MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER) {
    Core->ExpectedMessageHeader = After;
  }
}

static void
RecordRestoredField(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Field,
  MTK_HUSB311_U8 RoleMask,
  MTK_HUSB311_U8 After
  )
{
  if (Field == MTK_HUSB311_TYPEC_TOKEN_FIELD_RX) {
    Core->OwnedFields = (MTK_HUSB311_U8)(Core->OwnedFields & ~Field);
    Core->ExpectedRxDetect = After;
  } else if (Field == MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE) {
    Core->RoleOwnedMask = (MTK_HUSB311_U8)(Core->RoleOwnedMask & ~RoleMask);
    if (Core->RoleOwnedMask == 0U) {
      Core->OwnedFields = (MTK_HUSB311_U8)(Core->OwnedFields & ~Field);
    }
    Core->ExpectedRoleControl = After;
  } else if (Field == MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER) {
    Core->OwnedFields = (MTK_HUSB311_U8)(Core->OwnedFields & ~Field);
    Core->ExpectedMessageHeader = After;
  }
}

static MTK_HUSB311_TYPEC_STATUS
CheckQuietState(
  MTK_HUSB311_TYPEC_CORE *Core,
  int RequireSourceOff,
  MTK_HUSB311_U16 ExpectedTxResult,
  int CompareTxResult,
  MTK_HUSB311_U16 *ObservedTxResult
  )
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U16 TxResult;
  int Safe;

  Status = CheckGuards(Core, RequireSourceOff);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = ReadIdentity(Core);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = ReadProtocolState(Core, &TxResult);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if (CompareTxResult && (TxResult != ExpectedTxResult)) {
    return MtkHusb311TypecNotReady;
  }
  if (ObservedTxResult != 0) {
    *ObservedTxResult = TxResult;
  }
  if (RequireSourceOff) {
    Status = ReadSafe0V(Core, &Safe);
    if (Status != MtkHusb311TypecSuccess) {
      return Status;
    }
    if (!Safe) {
      return MtkHusb311TypecNotReady;
    }
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
CheckOpenFields(
  MTK_HUSB311_TYPEC_CORE *Core
  )
{
  MTK_HUSB311_U8 Role;
  MTK_HUSB311_U8 Rx;
  MTK_HUSB311_TYPEC_STATUS Status;

  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, &Role);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_RX_DETECT, &Rx);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if ((Role != Core->ExpectedRoleControl) ||
      ((Role & HUSB311_ROLE_OPEN_UPDATE_MASK) !=
       MTK_HUSB311_TYPEC_ROLE_OPEN_FIELDS) || (Rx != 0U)) {
    return MtkHusb311TypecConflict;
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
CheckOriginalReservedRoleBit(
  MTK_HUSB311_TYPEC_CORE *Core
  )
{
  MTK_HUSB311_U8 Role;
  MTK_HUSB311_TYPEC_STATUS Status;

  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, &Role);
  if (Status != MtkHusb311TypecSuccess) {
    Core->LastError = Status;
    Core->State = MtkHusb311TypecStateQuarantined;
    return MtkHusb311TypecQuarantined;
  }
  if ((Role & MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK) !=
      (Core->OriginalRoleControl & MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK)) {
    SetConflict(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL,
                MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK,
                Core->OriginalRoleControl, Role,
                MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE,
                MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK, 0, 0);
    return MtkHusb311TypecQuarantined;
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
PollOpenAndQuiet(MTK_HUSB311_TYPEC_CORE *Core)
{
  MTK_HUSB311_U16 TxResult;
  MTK_HUSB311_TYPEC_STATUS Status;

  Status = CheckQuietState(Core, 1, 0U, 0, &TxResult);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  /* TCPCI disables CC status reporting while both verified pins are open. */
  Status = CheckOpenFields(Core);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Core->CleanupElapsedMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  if (TxResult != Core->CleanupTxResult) {
    Core->CleanupTxResult = TxResult;
    Core->CleanupQuietMs = 0U;
  } else {
    Core->CleanupQuietMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  }
  if (Core->CleanupQuietMs >= MTK_HUSB311_TYPEC_TX_QUIESCENCE_MS) {
    return MtkHusb311TypecSuccess;
  }
  return (Core->CleanupElapsedMs >= MTK_HUSB311_TYPEC_TX_QUIESCENCE_TIMEOUT_MS) ?
    MtkHusb311TypecTimeout : MtkHusb311TypecPending;
}

static MTK_HUSB311_TYPEC_STATUS
ApplyUpdate(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 Mask,
  MTK_HUSB311_U8 Before,
  MTK_HUSB311_U8 Target,
  MTK_HUSB311_U8 Field,
  MTK_HUSB311_U8 RoleMask,
  MTK_HUSB311_U8 *Expected,
  int *Restoring
  )
{
  MTK_HUSB311_U8 After;
  MTK_HUSB311_U8 AppliedMask;
  MTK_HUSB311_TYPEC_STATUS Status;

  After = (Expected != 0) ? *Expected : Before;
  Status = UpdateExact(Core, Register, Mask, Before, Target, Field, RoleMask,
                       (Restoring != 0) && *Restoring, &After, &AppliedMask);
  if (AppliedMask != 0U) {
    if ((Restoring != 0) && *Restoring) {
      RecordRestoredField(Core, Field, RoleMask, After);
    } else {
      RecordChangedField(Core, Field, (MTK_HUSB311_U8)(RoleMask & AppliedMask), After);
    }
  } else if ((Status == MtkHusb311TypecSuccess) &&
             ((Target & Mask) == (Before & Mask)) &&
             (Restoring != 0) && *Restoring) {
    RecordRestoredField(Core, Field, RoleMask, After);
  }
  if ((Expected != 0) &&
      ((Status == MtkHusb311TypecSuccess) ||
       (Status == MtkHusb311TypecIoError))) {
    *Expected = After;
  }
  return Status;
}

static MTK_HUSB311_TYPEC_STATUS
SetRxDetect(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Before,
  MTK_HUSB311_U8 Target,
  int Restoring
  )
{
  return ApplyUpdate(Core, MTK_HUSB311_TYPEC_REG_RX_DETECT, 0xFFU,
                     Before, Target,
                     MTK_HUSB311_TYPEC_TOKEN_FIELD_RX, 0U,
                     &Core->ExpectedRxDetect, &Restoring);
}

static MTK_HUSB311_TYPEC_STATUS
SetRoleControl(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Mask,
  MTK_HUSB311_U8 Before,
  MTK_HUSB311_U8 Target,
  int Restoring
  )
{
  return ApplyUpdate(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, Mask,
                     Before, Target,
                     MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE, Mask,
                     &Core->ExpectedRoleControl, &Restoring);
}

static MTK_HUSB311_TYPEC_STATUS
SetRoleWithRetry(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Before,
  MTK_HUSB311_U8 Target
  )
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U8 Attempt;

  for (Attempt = 1U; ; ++Attempt) {
    Status = SetRoleControl(Core, HUSB311_ROLE_WRITABLE_MASK, Before, Target, 0);
    if ((Status != MtkHusb311TypecIoError) ||
        (Core->State != MtkHusb311TypecStatePreparing) ||
        (Attempt == HUSB311_ROLE_MAX_ATTEMPTS)) {
      return Status;
    }
    if ((Core->ExpectedRoleControl != Before) &&
        !OnlyRpLevelChanged(Before, Target, Core->ExpectedRoleControl)) {
      return Status;
    }
    Before = Core->ExpectedRoleControl;
    Status = CheckQuietState(Core, 1, Core->TxResultBaseline, 1, 0);
    if (Status != MtkHusb311TypecSuccess) {
      return Status;
    }
    if (Target == MTK_HUSB311_TYPEC_ROLE_SOURCE_RP_DFT) {
      Status = CheckOpenFields(Core);
      if (Status != MtkHusb311TypecSuccess) {
        return Status;
      }
    }
  }
}

static MTK_HUSB311_TYPEC_STATUS
OpenRole(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Before
  )
{
  MTK_HUSB311_U8 Target;

  if ((Before & HUSB311_ROLE_OPEN_UPDATE_MASK) ==
      MTK_HUSB311_TYPEC_ROLE_OPEN_FIELDS) {
    return SetRoleControl(Core, HUSB311_ROLE_OPEN_UPDATE_MASK, Before, Before, 0);
  }
  /* Match the stock HUSB311 set_cc(OPEN) value. */
  Target = (MTK_HUSB311_U8)((Before & MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK) |
                           HUSB311_STOCK_OPEN_ROLE);
  return SetRoleWithRetry(Core, Before, Target);
}

static MTK_HUSB311_TYPEC_STATUS
SetMessageHeader(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 Before,
  MTK_HUSB311_U8 Target,
  int Restoring
  )
{
  return ApplyUpdate(Core, MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER,
                     MTK_HUSB311_TYPEC_HEADER_ROLE_MASK, Before, Target,
                     MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER, 0U,
                     &Core->ExpectedMessageHeader, &Restoring);
}

static void
ClearToIdle(
  MTK_HUSB311_TYPEC_CORE *Core
  )
{
  Core->State = MtkHusb311TypecStateIdle;
  Core->OriginalRoleControl = 0U;
  Core->OriginalRxDetect = 0U;
  Core->OriginalMessageHeader = 0U;
  Core->ExpectedRoleControl = 0U;
  Core->ExpectedRxDetect = 0U;
  Core->ExpectedMessageHeader = 0U;
  Core->RoleOwnedMask = 0U;
  Core->OwnedFields = 0U;
  Core->AttachedCcPin = MTK_HUSB311_TYPEC_CC_PIN_NONE;
  Core->TxResultBaseline = 0U;
  Core->ConflictActive = 0U;
  Core->ConflictRegister = 0U;
  Core->ConflictMask = 0U;
  Core->ConflictExpected = 0U;
  Core->ConflictTarget = 0U;
  Core->ConflictField = 0U;
  Core->ConflictRoleMask = 0U;
  Core->ConflictWriteAttempted = 0U;
  Core->ConflictRestoring = 0U;
  Core->ActiveToken = 0U;
}

static MTK_HUSB311_TYPEC_STATUS
QuarantineCleanup(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_TYPEC_STATUS Status)
{
  Core->LastError = Status;
  Core->State = MtkHusb311TypecStateQuarantined;
  return MtkHusb311TypecQuarantined;
}

static MTK_HUSB311_TYPEC_STATUS
ReadCleanupRole(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_U8 *Role)
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U8 Mask;
  MTK_HUSB311_U8 Expected;

  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, Role);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if ((Core->OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE) == 0U) {
    Mask = HUSB311_ROLE_WRITABLE_MASK;
    Expected = Core->OriginalRoleControl;
  } else {
    Mask = (Core->RoleOwnedMask == MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK) ?
      0xFFU : Core->RoleOwnedMask;
    Expected = Core->ExpectedRoleControl;
  }
  if ((*Role & Mask) != (Expected & Mask)) {
    SetConflict(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, Mask, Expected,
                Core->OriginalRoleControl, MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE,
                Core->RoleOwnedMask, 0, 1);
    return MtkHusb311TypecConflict;
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
CleanupOwned(MTK_HUSB311_TYPEC_CORE *Core)
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U8 Current;
  MTK_HUSB311_U8 Target;
  MTK_HUSB311_U16 TxResult;

  if (Core->ConflictActive != 0U) {
    Core->State = MtkHusb311TypecStateQuarantined;
    return MtkHusb311TypecQuarantined;
  }
  if (Core->OwnedFields == 0U) {
    ClearToIdle(Core);
    return MtkHusb311TypecSuccess;
  }

  Status = CheckOriginalReservedRoleBit(Core);
  if (Status != MtkHusb311TypecSuccess) {
    return MtkHusb311TypecQuarantined;
  }
  if (Core->State == MtkHusb311TypecStateCleaning) {
    Status = PollOpenAndQuiet(Core);
    if (Status == MtkHusb311TypecPending) {
      return Status;
    }
    if (Status != MtkHusb311TypecSuccess) {
      return QuarantineCleanup(Core, Status);
    }
  } else {
    Core->State = MtkHusb311TypecStateCleaning;
    Core->CleanupFirstError = MtkHusb311TypecSuccess;
    Status = CheckQuietState(Core, 1, 0U, 0, &TxResult);
    if (Status != MtkHusb311TypecSuccess) {
      return QuarantineCleanup(Core, Status);
    }

    if ((Core->OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER) != 0U) {
      Target = (MTK_HUSB311_U8)(Core->OriginalMessageHeader &
                                  MTK_HUSB311_TYPEC_HEADER_ROLE_MASK);
      Status = SetMessageHeader(Core, Core->ExpectedMessageHeader, Target, 1);
      if (Status != MtkHusb311TypecSuccess) {
        if ((Status != MtkHusb311TypecIoError) ||
            ((Core->OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER) != 0U)) {
          return QuarantineCleanup(Core, Status);
        }
        Core->CleanupFirstError = Status;
      }
    }

    if (((Core->OwnedFields & (MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE |
                               MTK_HUSB311_TYPEC_TOKEN_FIELD_RX)) != 0U) &&
        (Core->RoleOwnedMask != MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK)) {
      Status = ReadCleanupRole(Core, &Current);
      if (Status != MtkHusb311TypecSuccess) {
        return QuarantineCleanup(Core, Status);
      }
      Status = OpenRole(Core, Current);
      if (Status != MtkHusb311TypecSuccess) {
        // Accept an I/O error only if a read-back already confirmed open CC.
        if ((Status != MtkHusb311TypecIoError) ||
            (CheckOpenFields(Core) != MtkHusb311TypecSuccess)) {
          return QuarantineCleanup(Core, Status);
        }
        Core->CleanupFirstError = Status;
      }
      Status = ReadProtocolState(Core, &Core->CleanupTxResult);
      if (Status != MtkHusb311TypecSuccess) {
        return QuarantineCleanup(Core, Status);
      }
      Core->CleanupElapsedMs = 0U;
      Core->CleanupQuietMs = 0U;
      return MtkHusb311TypecPending;
    }
  }

  if ((Core->OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE) != 0U) {
    Status = ReadCleanupRole(Core, &Current);
    if (Status != MtkHusb311TypecSuccess) {
      return QuarantineCleanup(Core, Status);
    }
    Target = (MTK_HUSB311_U8)(Core->OriginalRoleControl & Core->RoleOwnedMask);
    Status = SetRoleControl(Core, Core->RoleOwnedMask, Current, Target, 1);
    if (Status != MtkHusb311TypecSuccess) {
      if ((Status != MtkHusb311TypecIoError) ||
          ((Core->OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE) != 0U)) {
        return QuarantineCleanup(Core, Status);
      }
      Core->CleanupFirstError = Status;
    }
  }

  if ((Core->OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_RX) != 0U) {
    Status = SetRxDetect(Core, Core->ExpectedRxDetect, Core->OriginalRxDetect, 1);
    if (Status != MtkHusb311TypecSuccess) {
      if ((Status != MtkHusb311TypecIoError) ||
          ((Core->OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_RX) != 0U)) {
        return QuarantineCleanup(Core, Status);
      }
      Core->CleanupFirstError = Status;
    }
  }

  if ((Core->OwnedFields != 0U) || (Core->ConflictActive != 0U)) {
    return QuarantineCleanup(Core, MtkHusb311TypecNotReady);
  }
  Status = CheckOriginalReservedRoleBit(Core);
  if (Status != MtkHusb311TypecSuccess) {
    return MtkHusb311TypecQuarantined;
  }
  ClearToIdle(Core);
  Core->LastError = Core->CleanupFirstError;
  return Core->LastError;
}

static MTK_HUSB311_TYPEC_STATUS
FinishProbeCleanup(MTK_HUSB311_TYPEC_CORE *Core)
{
  MTK_HUSB311_TYPEC_STATUS Status = CleanupOwned(Core);
  return ((Status == MtkHusb311TypecPending) ||
          (Status == MtkHusb311TypecQuarantined)) ? Status : Core->ProbeFailure;
}

static MTK_HUSB311_TYPEC_STATUS
AbortAcquire(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TYPEC_STATUS Reason
  )
{
  if (Core->FirstFailure.Step == 0U) {
    Core->Diagnostic.Status = Reason;
    Core->FirstFailure = Core->Diagnostic;
  }
  Core->ProbeFailure = Reason;
  return FinishProbeCleanup(Core);
}

static MTK_HUSB311_TYPEC_STATUS
CheckRoleAndRx(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_U8 RoleMask,
  MTK_HUSB311_U8 ExpectedRole,
  MTK_HUSB311_U8 ExpectedRx
  )
{
  MTK_HUSB311_U8 Role;
  MTK_HUSB311_U8 Rx;
  MTK_HUSB311_TYPEC_STATUS Status;

  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, &Role);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_RX_DETECT, &Rx);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if (((Role & RoleMask) != (ExpectedRole & RoleMask)) ||
      ((Role & MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK) !=
       (ExpectedRole & MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK)) ||
      (Rx != ExpectedRx)) {
    return MtkHusb311TypecConflict;
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
SampleSourceAttach(
  MTK_HUSB311_TYPEC_CORE *Core,
  int FastOpenProbe
  )
{
  MTK_HUSB311_U8 CurrentPin;
  SP_HUSB311_CC_STATUS Cc;
  MTK_HUSB311_TYPEC_STATUS Status;
  int Safe;

  Status = CheckQuietState(Core, 1, Core->TxResultBaseline, 1, 0);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = CheckRoleAndRx(Core, HUSB311_ROLE_WRITABLE_MASK,
                          Core->ExpectedRoleControl,
                          Core->ExpectedRxDetect);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = ReadSafe0V(Core, &Safe);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if (!Safe) {
    return MtkHusb311TypecNotReady;
  }
  Status = ReadCc(Core, &Cc);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if (Cc.CcStatusBit5) {
    CurrentPin = MTK_HUSB311_TYPEC_CC_PIN_NONE;
  } else if ((Cc.DriverCc1Value == MTK_HUSB311_TYPEC_CC_RD) &&
             (Cc.DriverCc2Value == MTK_HUSB311_TYPEC_CC_OPEN)) {
    CurrentPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  } else if ((Cc.DriverCc2Value == MTK_HUSB311_TYPEC_CC_RD) &&
             (Cc.DriverCc1Value == MTK_HUSB311_TYPEC_CC_OPEN)) {
    CurrentPin = MTK_HUSB311_TYPEC_CC_PIN_2;
  } else if ((Cc.DriverCc1Value == MTK_HUSB311_TYPEC_CC_OPEN) &&
             (Cc.DriverCc2Value == MTK_HUSB311_TYPEC_CC_OPEN)) {
    CurrentPin = MTK_HUSB311_TYPEC_CC_PIN_NONE;
  } else {
    return RejectRegister(Core, MTK_HUSB311_TYPEC_REG_CC_STATUS, 1U, Cc.CcStatus);
  }
  if (CurrentPin == MTK_HUSB311_TYPEC_CC_PIN_NONE) {
    // A valid Rd must stay on the same pin for a full debounce.
    if (FastOpenProbe && Core->ProbeElapsedMs >= (Cc.CcStatusBit5 ?
        MTK_HUSB311_TYPEC_ATTACH_DEBOUNCE_MS : 100U)) {
      (void)RejectRegister(Core, MTK_HUSB311_TYPEC_REG_CC_STATUS, 1U, Cc.CcStatus);
      return MtkHusb311TypecTimeout;
    }
    Core->ProbeStableMs = 0U;
  } else if (CurrentPin != Core->ProbeCcPin) {
    Core->ProbeStableMs = 0U;
  } else {
    Core->ProbeStableMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  }
  Core->ProbeCcPin = CurrentPin;
  if (Core->ProbeStableMs >= MTK_HUSB311_TYPEC_ATTACH_DEBOUNCE_MS) {
    return MtkHusb311TypecSuccess;
  }
  Core->ProbeElapsedMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  return Core->ProbeElapsedMs < MTK_HUSB311_TYPEC_ATTACH_TIMEOUT_MS ?
    MtkHusb311TypecPending : MtkHusb311TypecTimeout;
}

static MTK_HUSB311_TYPEC_STATUS
ResolveConflict(
  MTK_HUSB311_TYPEC_CORE *Core
  )
{
  MTK_HUSB311_U8 Current;
  MTK_HUSB311_TYPEC_STATUS Status;

  if (Core->ConflictActive == 0U) {
    return MtkHusb311TypecSuccess;
  }
  Status = Read8(Core, Core->ConflictRegister, &Current);
  if (Status != MtkHusb311TypecSuccess) {
    return MtkHusb311TypecQuarantined;
  }
  if ((Current & Core->ConflictMask) ==
      (Core->ConflictExpected & Core->ConflictMask)) {
    Core->ConflictActive = 0U;
    return MtkHusb311TypecSuccess;
  }
  if (Core->ConflictWriteAttempted &&
      ((Current & Core->ConflictMask) ==
       (Core->ConflictTarget & Core->ConflictMask))) {
    if (Core->ConflictRestoring) {
      RecordRestoredField(Core, Core->ConflictField,
                          Core->ConflictRoleMask, Current);
    } else {
      RecordChangedField(Core, Core->ConflictField,
                         Core->ConflictRoleMask, Current);
    }
    Core->ConflictActive = 0U;
    return MtkHusb311TypecSuccess;
  }
  return MtkHusb311TypecQuarantined;
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecInitialize(
  MTK_HUSB311_TYPEC_CORE *Core,
  const MTK_HUSB311_TYPEC_IO *Io
  )
{
  if ((Core == 0) || (Io == 0) || (Io->ReadRegisters == 0) ||
      (Io->UpdateRegister == 0) || (Io->DelayMs == 0) ||
      (Io->ReadGuards == 0)) {
    return MtkHusb311TypecInvalid;
  }
  if (Core->Signature == HUSB311_TYPEC_SIGNATURE) {
    return MtkHusb311TypecBusy;
  }
  ZeroBytes(Core, (MTK_HUSB311_U32)sizeof(*Core));
  Core->Signature = HUSB311_TYPEC_SIGNATURE;
  Core->Io = *Io;
  Core->State = MtkHusb311TypecStateIdle;
  return MtkHusb311TypecSuccess;
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecBeginSourceProbe(MTK_HUSB311_TYPEC_CORE *Core)
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U8 Role;
  MTK_HUSB311_U8 Rx;
  MTK_HUSB311_U8 Header;
  MTK_HUSB311_U8 TcpcControl;
  MTK_HUSB311_U8 PowerControl;
  MTK_HUSB311_U8 Fault;
  MTK_HUSB311_U16 Alert;
  int Safe;

  if ((Core == 0) || (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  if (Core->State == MtkHusb311TypecStateQuarantined) {
    return MtkHusb311TypecQuarantined;
  }
  if (Core->State != MtkHusb311TypecStateIdle) {
    return MtkHusb311TypecBusy;
  }
  ZeroBytes(&Core->Diagnostic, (MTK_HUSB311_U32)sizeof(Core->Diagnostic));
  Core->State = MtkHusb311TypecStatePreparing;
  Core->LastError = MtkHusb311TypecSuccess;

  Core->Diagnostic.Step = 1U;
  Status = CheckGuards(Core, 1);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Core->Diagnostic.Step = 2U;
  Status = ReadIdentity(Core);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Core->Diagnostic.Step = 3U;
  Status = ReadSafe0V(Core, &Safe);
  if ((Status != MtkHusb311TypecSuccess) || !Safe) {
    return AbortAcquire(Core, Status == MtkHusb311TypecSuccess
                               ? MtkHusb311TypecNotReady : Status);
  }
  Core->Diagnostic.Step = 4U;
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_TCPC_CONTROL, &TcpcControl);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_POWER_CONTROL, &PowerControl);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  if ((TcpcControl & MTK_HUSB311_TYPEC_TCPC_BIST_MASK) != 0U) {
    return AbortAcquire(Core, RejectRegister(
      Core, MTK_HUSB311_TYPEC_REG_TCPC_CONTROL, 1U, TcpcControl));
  }
  if ((PowerControl & MTK_HUSB311_TYPEC_POWER_VCONN_MASK) != 0U) {
    return AbortAcquire(Core, RejectRegister(
      Core, MTK_HUSB311_TYPEC_REG_POWER_CONTROL, 1U, PowerControl));
  }
  Core->Diagnostic.Step = 5U;
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_FAULT_STATUS, &Fault);
  if ((Status != MtkHusb311TypecSuccess) || (Fault != 0U)) {
    return AbortAcquire(Core, Status == MtkHusb311TypecSuccess
                               ? MtkHusb311TypecNotReady : Status);
  }
  Core->Diagnostic.Step = 6U;
  Status = ReadAlert(Core, &Alert);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  /* Servicing an event may race with attachment; resample before role writes. */
  Status = CheckGuards(Core, 1);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Status = ReadSafe0V(Core, &Safe);
  if ((Status != MtkHusb311TypecSuccess) || !Safe) {
    return AbortAcquire(Core, Status == MtkHusb311TypecSuccess
                               ? MtkHusb311TypecNotReady : Status);
  }
  Core->TxResultBaseline = (MTK_HUSB311_U16)(
    Alert & MTK_HUSB311_TYPEC_ALERT_TX_RESULT_MASK);
  Core->Diagnostic.Step = 7U;
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, &Role);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  if ((Role & MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK) != 0U) {
    return AbortAcquire(Core, MtkHusb311TypecNotReady);
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_RX_DETECT, &Rx);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER, &Header);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Core->OriginalRoleControl = Role;
  Core->OriginalRxDetect = Rx;
  Core->OriginalMessageHeader = Header;
  Core->ExpectedRoleControl = Role;
  Core->ExpectedRxDetect = Rx;
  Core->ExpectedMessageHeader = Header;
  Core->AttachedCcPin = MTK_HUSB311_TYPEC_CC_PIN_NONE;

  Core->Diagnostic.Step = 8U;
  if (Rx != 0U) {
    Status = SetRxDetect(Core, Rx, 0U, 0);
    if (Status != MtkHusb311TypecSuccess) {
      return AbortAcquire(Core, Status);
    }
  }
  Core->Diagnostic.Step = 9U;
  Status = OpenRole(Core, Role);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Core->Diagnostic.Step = 10U;
  Core->ProbeElapsedMs = 0U;
  Core->ProbeStableMs = 0U;
  Core->ProbeCcPin = MTK_HUSB311_TYPEC_CC_PIN_NONE;
  Core->ProbeWaitingForAttach = 0U;
  return MtkHusb311TypecPending;
}

static MTK_HUSB311_TYPEC_STATUS
AdvertiseSource(MTK_HUSB311_TYPEC_CORE *Core)
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U8 Role;
  MTK_HUSB311_U8 Target;
  Core->Diagnostic.Step = 11U;
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, &Role);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  if (Role != Core->ExpectedRoleControl) {
    SetConflict(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, 0xFFU,
                Core->ExpectedRoleControl, Role,
                MTK_HUSB311_TYPEC_TOKEN_FIELD_ROLE, 0xFFU, 0, 0);
    return AbortAcquire(Core, MtkHusb311TypecConflict);
  }
  Target = MTK_HUSB311_TYPEC_ROLE_SOURCE_RP_DFT;
  /*
   * Stock HUSB311 keeps Rp level unchanged when enabling the CC pulls.
   * Select our default current while both CC pins are still open. A write
   * that changes the level and the pulls together can latch only the level.
   */
  if ((Role & MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK) !=
      (Target & MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK)) {
    Status = SetRoleControl(Core, MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK,
                            Role, Target, 0);
    if (Status != MtkHusb311TypecSuccess) {
      return AbortAcquire(Core, Status);
    }
    if (Core->ExpectedRoleControl != MTK_HUSB311_TYPEC_ROLE_OPEN_FIELDS) {
      return AbortAcquire(Core, MtkHusb311TypecConflict);
    }
    Status = CheckOpenFields(Core);
    if (Status != MtkHusb311TypecSuccess) {
      return AbortAcquire(Core, Status);
    }
    Status = CheckQuietState(Core, 1, Core->TxResultBaseline, 1, 0);
    if (Status != MtkHusb311TypecSuccess) {
      return AbortAcquire(Core, Status);
    }
  }
  Status = SetRoleWithRetry(Core, Core->ExpectedRoleControl, Target);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  if (Core->ExpectedRoleControl !=
      MTK_HUSB311_TYPEC_ROLE_SOURCE_RP_DFT) {
    return AbortAcquire(Core, MtkHusb311TypecConflict);
  }
  Core->Diagnostic.Step = 12U;
  Core->ProbeWaitingForAttach = 1U;
  Core->ProbeElapsedMs = 0U;
  return MtkHusb311TypecPending;
}

static MTK_HUSB311_TYPEC_STATUS
FinishSourceProbe(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_TOKEN *Token)
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U8 Header;
  MTK_HUSB311_U8 Target;
  MTK_HUSB311_U8 After;
  MTK_HUSB311_U8 AppliedMask;

  Core->Diagnostic.Step = 13U;
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER, &Header);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  if ((Header & MTK_HUSB311_TYPEC_HEADER_ROLE_MASK) !=
      (Core->OriginalMessageHeader &
       MTK_HUSB311_TYPEC_HEADER_ROLE_MASK)) {
    SetConflict(Core, MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER,
                MTK_HUSB311_TYPEC_HEADER_ROLE_MASK,
                Core->OriginalMessageHeader, Header,
                MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER, 0U, 0, 0);
    (void)AbortAcquire(Core, MtkHusb311TypecConflict);
    return MtkHusb311TypecQuarantined;
  }
  Target = (MTK_HUSB311_U8)((Header &
            (MTK_HUSB311_U8)~MTK_HUSB311_TYPEC_HEADER_ROLE_MASK) |
            MTK_HUSB311_TYPEC_HEADER_DFP_SOURCE);
  Status = UpdateExact(Core, MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER,
                       MTK_HUSB311_TYPEC_HEADER_ROLE_MASK,
                       Header, Target,
                       MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER, 0U,
                       0, &After, &AppliedMask);
  if (AppliedMask != 0U) {
    RecordChangedField(Core, MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER,
                       0U, After);
  }
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Core->Diagnostic.Step = 14U;
  Status = CheckQuietState(Core, 1, Core->TxResultBaseline, 1, 0);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Status = CheckRoleAndRx(Core, HUSB311_ROLE_WRITABLE_MASK,
                          Core->ExpectedRoleControl,
                          Core->ExpectedRxDetect);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  {
    SP_HUSB311_CC_STATUS Cc;
    Status = ReadCc(Core, &Cc);
    if ((Status != MtkHusb311TypecSuccess) || Cc.CcStatusBit5 ||
        ((Core->ProbeCcPin == MTK_HUSB311_TYPEC_CC_PIN_1) &&
         ((Cc.DriverCc1Value != MTK_HUSB311_TYPEC_CC_RD) ||
          (Cc.DriverCc2Value != MTK_HUSB311_TYPEC_CC_OPEN))) ||
        ((Core->ProbeCcPin == MTK_HUSB311_TYPEC_CC_PIN_2) &&
         ((Cc.DriverCc2Value != MTK_HUSB311_TYPEC_CC_RD) ||
          (Cc.DriverCc1Value != MTK_HUSB311_TYPEC_CC_OPEN)))) {
      return AbortAcquire(Core, Status == MtkHusb311TypecSuccess
                                 ? MtkHusb311TypecConflict : Status);
    }
  }

  if (Core->TokenCounter == ~(MTK_HUSB311_TOKEN)0U) {
    return AbortAcquire(Core, MtkHusb311TypecNotReady);
  }
  Core->TokenCounter++;
  if (Core->TokenCounter == 0U) {
    return AbortAcquire(Core, MtkHusb311TypecNotReady);
  }
  Core->ActiveToken = Core->TokenCounter;
  Core->AttachedCcPin = Core->ProbeCcPin;
  Core->State = MtkHusb311TypecStateOwned;
  Core->LastError = MtkHusb311TypecSuccess;
  *Token = Core->ActiveToken;
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
PollSourceProbe(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN *Token,
  int FastOpenProbe
  )
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U16 TxResult;

  if ((Core == 0) || (Token == 0) ||
      (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  *Token = 0U;
  if (Core->State == MtkHusb311TypecStateCleaning) {
    return FinishProbeCleanup(Core);
  }
  if (Core->State != MtkHusb311TypecStatePreparing) {
    return MtkHusb311TypecNotReady;
  }
  if (Core->ProbeWaitingForAttach != 0U) {
    Status = SampleSourceAttach(Core, FastOpenProbe);
    if (Status == MtkHusb311TypecSuccess) {
      return FinishSourceProbe(Core, Token);
    }
    return Status == MtkHusb311TypecPending ? Status : AbortAcquire(Core, Status);
  }
  Status = CheckQuietState(Core, 1, Core->TxResultBaseline, 0, &TxResult);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  if (TxResult != Core->TxResultBaseline) {
    return AbortAcquire(Core, MtkHusb311TypecNotReady);
  }
  Status = CheckOpenFields(Core);
  if (Status != MtkHusb311TypecSuccess) {
    return AbortAcquire(Core, Status);
  }
  Core->ProbeElapsedMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  if (Core->ProbeElapsedMs < MTK_HUSB311_TYPEC_TX_QUIESCENCE_MS) {
    return MtkHusb311TypecPending;
  }
  Core->LastError = MtkHusb311TypecSuccess;
  return AdvertiseSource(Core);
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecPollSourceProbe(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_TOKEN *Token)
{
  return PollSourceProbe(Core, Token, 1);
}

static MTK_HUSB311_TYPEC_STATUS
RunCleanup(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_TYPEC_STATUS Status)
{
  while (Status == MtkHusb311TypecPending) {
    Status = Core->Io.DelayMs(Core->Io.Context, MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS);
    if (Status != MtkHusb311TypecSuccess) {
      return QuarantineCleanup(Core, Status);
    }
    Status = CleanupOwned(Core);
  }
  return Status;
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecCancelSourceProbe(MTK_HUSB311_TYPEC_CORE *Core)
{
  if ((Core == 0) || (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  if (Core->State != MtkHusb311TypecStatePreparing) {
    return MtkHusb311TypecNotReady;
  }
  return RunCleanup(Core, CleanupOwned(Core));
}

static MTK_HUSB311_TYPEC_STATUS
AcquireSourceInternal(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_TOKEN *Token,
                      int FastOpenProbe)
{
  MTK_HUSB311_TYPEC_STATUS Status;

  if ((Core == 0) || (Token == 0) ||
      (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  *Token = 0U;
  Status = MtkHusb311TypecBeginSourceProbe(Core);
  while (Status == MtkHusb311TypecPending) {
    Status = Core->Io.DelayMs(Core->Io.Context, MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS);
    if (Status == MtkHusb311TypecSuccess) {
      Status = PollSourceProbe(Core, Token, FastOpenProbe);
    } else if (Core->State == MtkHusb311TypecStateCleaning) {
      return QuarantineCleanup(Core, Status);
    } else {
      Status = AbortAcquire(Core, Status);
    }
  }
  return Status;
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecAcquireSource(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_TOKEN *Token)
{
  return AcquireSourceInternal(Core, Token, 0);
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecProbeSource(MTK_HUSB311_TYPEC_CORE *Core, MTK_HUSB311_TOKEN *Token)
{
  return AcquireSourceInternal(Core, Token, 1);
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecValidateSource(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  )
{
  MTK_HUSB311_TYPEC_STATUS Status;
  MTK_HUSB311_U8 Role;
  MTK_HUSB311_U8 Rx;
  MTK_HUSB311_U8 Header;
  MTK_HUSB311_U16 TxResult;
  SP_HUSB311_CC_STATUS Cc;

  if ((Core == 0) || (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  if (!MtkHusb311TypecTokenActive(Core, Token)) {
    return MtkHusb311TypecStaleToken;
  }
  Status = CheckGuards(Core, 0);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = ReadIdentity(Core);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = ReadProtocolState(Core, &TxResult);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if (TxResult != (MTK_HUSB311_U16)(Core->TxResultBaseline &
                                   MTK_HUSB311_TYPEC_ALERT_TX_RESULT_MASK)) {
    return MtkHusb311TypecConflict;
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL, &Role);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_RX_DETECT, &Rx);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  Status = Read8(Core, MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER, &Header);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if ((Role != MTK_HUSB311_TYPEC_ROLE_SOURCE_RP_DFT) ||
      ((Role & Core->RoleOwnedMask) !=
       (Core->ExpectedRoleControl & Core->RoleOwnedMask)) ||
      (Rx != Core->ExpectedRxDetect) || (Rx != 0U) ||
      ((Header & MTK_HUSB311_TYPEC_HEADER_ROLE_MASK) !=
       MTK_HUSB311_TYPEC_HEADER_DFP_SOURCE)) {
    return MtkHusb311TypecConflict;
  }
  Status = ReadCc(Core, &Cc);
  if (Status != MtkHusb311TypecSuccess) {
    return Status;
  }
  if (Cc.CcStatusBit5 ||
      ((Core->AttachedCcPin == MTK_HUSB311_TYPEC_CC_PIN_1) &&
       ((Cc.DriverCc1Value != MTK_HUSB311_TYPEC_CC_RD) ||
        (Cc.DriverCc2Value != MTK_HUSB311_TYPEC_CC_OPEN))) ||
      ((Core->AttachedCcPin == MTK_HUSB311_TYPEC_CC_PIN_2) &&
       ((Cc.DriverCc2Value != MTK_HUSB311_TYPEC_CC_RD) ||
        (Cc.DriverCc1Value != MTK_HUSB311_TYPEC_CC_OPEN)))) {
    return MtkHusb311TypecConflict;
  }
  return MtkHusb311TypecSuccess;
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecStepCleanup(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  )
{
  MTK_HUSB311_TYPEC_STATUS Status;

  if ((Core == 0) || (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  if (Core->State == MtkHusb311TypecStateOwned) {
    if (!MtkHusb311TypecTokenActive(Core, Token)) {
      return MtkHusb311TypecStaleToken;
    }
    Status = CheckGuards(Core, 1);
    if (Status != MtkHusb311TypecSuccess) {
      Core->LastError = Status;
      return Status;
    }
  }
  if (Core->State == MtkHusb311TypecStateQuarantined) {
    Status = CheckGuards(Core, 1);
    if (Status != MtkHusb311TypecSuccess) {
      return QuarantineCleanup(Core, Status);
    }
    Status = ResolveConflict(Core);
    if (Status != MtkHusb311TypecSuccess) {
      return MtkHusb311TypecQuarantined;
    }
  }
  return CleanupOwned(Core);
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecReleaseSource(
  MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  )
{
  if ((Core == 0) || (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  if (!MtkHusb311TypecTokenActive(Core, Token)) {
    return MtkHusb311TypecStaleToken;
  }
  return RunCleanup(Core, MtkHusb311TypecStepCleanup(Core, Token));
}

MTK_HUSB311_TYPEC_STATUS
MtkHusb311TypecRetryCleanup(MTK_HUSB311_TYPEC_CORE *Core)
{
  if ((Core == 0) || (Core->Signature != HUSB311_TYPEC_SIGNATURE)) {
    return MtkHusb311TypecInvalid;
  }
  if (Core->State != MtkHusb311TypecStateQuarantined) {
    return MtkHusb311TypecNotReady;
  }
  return RunCleanup(Core, MtkHusb311TypecStepCleanup(Core, 0U));
}

int
MtkHusb311TypecTokenActive(
  const MTK_HUSB311_TYPEC_CORE *Core,
  MTK_HUSB311_TOKEN Token
  )
{
  return (Core != 0) && (Core->Signature == HUSB311_TYPEC_SIGNATURE) &&
         (Core->State == MtkHusb311TypecStateOwned) &&
         (Token != 0U) && (Core->ActiveToken == Token);
}
