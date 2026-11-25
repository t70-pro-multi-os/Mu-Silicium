/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MtkMusbPlatformCore.h>
#include <Library/MtkSc8989xOtgCore.h>
#include <Library/StockPowerCore.h>
#include <stddef.h>

#define OTG_SIGNATURE 0x53434F54U
#define SC_DRIVER_PART_MASK 0x94U
#define HUSB311_VENDOR_ID 0x2E99U
#define HUSB311_DEVICE_ID 0x0311U

#define OWN_REG00_BIT7  0x01U
#define OWN_GPIO48      0x02U
#define OWN_REG03_BIT4  0x04U
#define OWN_GPIO86      0x08U
#define OWN_REG03_BIT5  0x10U
#define OWN_BOOST_PROFILE 0x20U

static const uint8_t mRestoreOrder[] = {
  OWN_REG03_BIT5, OWN_GPIO86, OWN_REG03_BIT4, OWN_GPIO48, OWN_REG00_BIT7,
  OWN_BOOST_PROFILE
};

static uint8_t
CoreValid(const MTK_SC8989X_OTG_CORE *Core)
{
  return (uint8_t)(
    (Core != NULL) &&
    (Core->Signature == OTG_SIGNATURE) &&
    (Core->State >= MtkSc8989xOtgStateIdle) &&
    (Core->State <= MtkSc8989xOtgStateQuarantined) &&
    ((Core->RestoredMask & (uint8_t)~Core->OwnedMask) == 0U));
}

static uint8_t
ScPartSupported(uint8_t PartIdRegister)
{
  uint8_t PartCode = (uint8_t)((PartIdRegister >> 3) & 7U);
  return (uint8_t)((SC_DRIVER_PART_MASK & (1U << PartCode)) != 0U);
}

static uint8_t
MatchesT70BoostProfile(uint8_t Reg0A)
{
  /* Stock DT uses votg=12 (bits 7:4) and iboost=3 (bits 2:0). */
  return (uint8_t)((Reg0A & MTK_SC8989X_OTG_BOOST_PROFILE_MASK) ==
    MTK_SC8989X_OTG_BOOST_PROFILE_T70);
}

static MTK_SC8989X_OTG_STATUS
ReadEvidence(
  MTK_SC8989X_OTG_CORE *Core,
  MTK_SC8989X_OTG_EVIDENCE *Evidence
  )
{
  if ((Core->Io.ReadFreshEvidence == NULL) || (Evidence == NULL)) {
    return MtkSc8989xOtgInvalid;
  }
  *Evidence = (MTK_SC8989X_OTG_EVIDENCE){0};
  return Core->Io.ReadFreshEvidence(Core->Io.Context, Evidence);
}

static MTK_SC8989X_OTG_STATUS
CheckIdentityAndSource(
  const MTK_SC8989X_OTG_EVIDENCE *Evidence,
  uint8_t RequireSource,
  uint8_t RequireVsafe0V
  )
{
  if ((Evidence == NULL) || !ScPartSupported(Evidence->ScPartIdRegister) ||
      (Evidence->Husb311VendorId != HUSB311_VENDOR_ID) ||
      (Evidence->Husb311DeviceId != HUSB311_DEVICE_ID)) {
    return MtkSc8989xOtgUnsupported;
  }

  if (((Evidence->Husb311PowerStatus &
        MTK_SC8989X_OTG_HUSB_INITIALIZED) != 0U) ||
      ((Evidence->Husb311PowerStatus &
        MTK_SC8989X_OTG_HUSB_VBUS_DETECT) == 0U)) {
    return MtkSc8989xOtgNotReady;
  }

  if ((RequireVsafe0V != 0U) &&
      !SpHusb311VbusSafe0V(Evidence->Husb311PowerStatus,
                         Evidence->Husb311VendorStatusReg97)) {
    return MtkSc8989xOtgNotReady;
  }

  if ((RequireSource != 0U) &&
      !MtkMusbPlatformHusb311SourceAttached(
         Evidence->Husb311CcStatus,
         Evidence->Husb311RoleControl)) {
    return MtkSc8989xOtgNotReady;
  }
  return MtkSc8989xOtgSuccess;
}

static uint8_t
GpioUsable(const MTK_SC8989X_OTG_GPIO_STATE *Gpio)
{
  return (uint8_t)(
    (Gpio != NULL) &&
    (Gpio->Mode == MTK_SC8989X_OTG_GPIO_MODE_GPIO) &&
    (Gpio->Direction == MTK_SC8989X_OTG_GPIO_DIRECTION_OUTPUT) &&
    (Gpio->PhysicalValue <= 1U));
}

static uint8_t
ExpectedBit(uint8_t Original, uint8_t Owned, uint8_t Target)
{
  return (Owned != 0U) ? Target : Original;
}

static MTK_SC8989X_OTG_STATUS
CheckInitialEvidence(
  const MTK_SC8989X_OTG_EVIDENCE *Evidence,
  uint8_t ConfigureBoostProfile
  )
{
  MTK_SC8989X_OTG_STATUS Status;

  Status = CheckIdentityAndSource(Evidence, 1U, 1U);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  if (!ConfigureBoostProfile && !MatchesT70BoostProfile(Evidence->ScReg0A)) {
    return MtkSc8989xOtgUnsupported;
  }
  if ((Evidence->ScReg03 & MTK_SC8989X_OTG_REG03_BOOST_MASK) != 0U) {
    return MtkSc8989xOtgNotReady;
  }
  if (!GpioUsable(&Evidence->Gpio48) ||
      !GpioUsable(&Evidence->Gpio86) ||
      (Evidence->Gpio48.PhysicalValue != 0U) ||
      (Evidence->Gpio86.PhysicalValue != 0U)) {
    return MtkSc8989xOtgNotReady;
  }
  return MtkSc8989xOtgSuccess;
}

static uint8_t
OriginalBit00(const MTK_SC8989X_OTG_CORE *Core)
{
  return (uint8_t)((Core->OriginalScReg00 &
                    MTK_SC8989X_OTG_REG00_CLEAR_MASK) != 0U);
}

static uint8_t
OriginalBit03Clear(const MTK_SC8989X_OTG_CORE *Core)
{
  return (uint8_t)((Core->OriginalScReg03 &
                    MTK_SC8989X_OTG_REG03_CLEAR_MASK) != 0U);
}

static uint8_t
OriginalBit03Boost(const MTK_SC8989X_OTG_CORE *Core)
{
  return (uint8_t)((Core->OriginalScReg03 &
                    MTK_SC8989X_OTG_REG03_BOOST_MASK) != 0U);
}

static MTK_SC8989X_OTG_STATUS
CheckLiveEvidence(
  const MTK_SC8989X_OTG_CORE *Core,
  const MTK_SC8989X_OTG_EVIDENCE *Evidence
  )
{
  MTK_SC8989X_OTG_STATUS Status;
  uint8_t Reg00Bit;
  uint8_t Reg03Bit4;
  uint8_t Reg03Bit5;
  uint8_t Gpio48Value;
  uint8_t Gpio86Value;
  uint8_t AllowVbusPresent;
  uint8_t ExpectedProfile;

  Status = CheckIdentityAndSource(Evidence, 1U, 0U);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  if ((Evidence->ScPartIdRegister != Core->InitialScPartIdRegister) ||
      (Evidence->Husb311CcStatus != Core->InitialHusb311CcStatus) ||
      (Evidence->Husb311RoleControl != Core->InitialHusb311RoleControl)) {
    return MtkSc8989xOtgNotReady;
  }
  ExpectedProfile = Core->OriginalScReg0A;
  if ((Core->OwnedMask & OWN_BOOST_PROFILE) != 0U) {
    ExpectedProfile = (uint8_t)((ExpectedProfile & ~MTK_SC8989X_OTG_BOOST_PROFILE_MASK) |
      MTK_SC8989X_OTG_BOOST_PROFILE_T70);
  }
  if (Evidence->ScReg0A != ExpectedProfile) {
    return MtkSc8989xOtgConflict;
  }

  if (((Evidence->ScReg00 ^ Core->OriginalScReg00) &
       (uint8_t)~MTK_SC8989X_OTG_REG00_CLEAR_MASK) != 0U ||
      ((Evidence->ScReg03 ^ Core->OriginalScReg03) &
       (uint8_t)~(MTK_SC8989X_OTG_REG03_CLEAR_MASK |
                  MTK_SC8989X_OTG_REG03_BOOST_MASK)) != 0U) {
    return MtkSc8989xOtgConflict;
  }

  Reg00Bit = ExpectedBit(
    OriginalBit00(Core),
    (uint8_t)((Core->OwnedMask & OWN_REG00_BIT7) != 0U),
    0U
    );
  Reg03Bit4 = ExpectedBit(
    OriginalBit03Clear(Core),
    (uint8_t)((Core->OwnedMask & OWN_REG03_BIT4) != 0U),
    0U
    );
  Reg03Bit5 = ExpectedBit(
    OriginalBit03Boost(Core),
    (uint8_t)((Core->OwnedMask & OWN_REG03_BIT5) != 0U),
    1U
    );
  if (((((Evidence->ScReg00 & MTK_SC8989X_OTG_REG00_CLEAR_MASK) != 0U) ? 1U : 0U) !=
        Reg00Bit) ||
      ((((Evidence->ScReg03 & MTK_SC8989X_OTG_REG03_CLEAR_MASK) != 0U) ? 1U : 0U) !=
        Reg03Bit4) ||
      ((((Evidence->ScReg03 & MTK_SC8989X_OTG_REG03_BOOST_MASK) != 0U) ? 1U : 0U) !=
        Reg03Bit5)) {
    return MtkSc8989xOtgConflict;
  }

  if (!GpioUsable(&Evidence->Gpio48) || !GpioUsable(&Evidence->Gpio86)) {
    return MtkSc8989xOtgConflict;
  }
  Gpio48Value = ExpectedBit(
    Core->OriginalGpio48,
    (uint8_t)((Core->OwnedMask & OWN_GPIO48) != 0U),
    1U
    );
  Gpio86Value = ExpectedBit(
    Core->OriginalGpio86,
    (uint8_t)((Core->OwnedMask & OWN_GPIO86) != 0U),
    1U
    );
  if ((Evidence->Gpio48.PhysicalValue != Gpio48Value) ||
      (Evidence->Gpio86.PhysicalValue != Gpio86Value)) {
    return MtkSc8989xOtgConflict;
  }

  /* GPIO86 alone does not authorize connector power. Require safe0V
   * until the final boost-control write is actually attempted. */
  AllowVbusPresent = (uint8_t)((Core->OwnedMask & OWN_REG03_BIT5) != 0U);
  if ((AllowVbusPresent == 0U) &&
      !SpHusb311VbusSafe0V(Evidence->Husb311PowerStatus,
                         Evidence->Husb311VendorStatusReg97)) {
    return MtkSc8989xOtgNotReady;
  }
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
CheckRestoreEvidence(
  const MTK_SC8989X_OTG_CORE *Core,
  const MTK_SC8989X_OTG_EVIDENCE *Evidence
  )
{
  if ((Evidence == NULL) || !ScPartSupported(Evidence->ScPartIdRegister) ||
      (Evidence->ScPartIdRegister != Core->InitialScPartIdRegister)) {
    return MtkSc8989xOtgUnsupported;
  }
  if (((Evidence->ScReg0A ^ Core->OriginalScReg0A) &
       (uint8_t)~MTK_SC8989X_OTG_BOOST_PROFILE_MASK) != 0U ||
      (((Evidence->ScReg00 ^ Core->OriginalScReg00) &
        (uint8_t)~MTK_SC8989X_OTG_REG00_CLEAR_MASK) != 0U) ||
      (((Evidence->ScReg03 ^ Core->OriginalScReg03) &
        (uint8_t)~(MTK_SC8989X_OTG_REG03_CLEAR_MASK |
                   MTK_SC8989X_OTG_REG03_BOOST_MASK)) != 0U) ||
      !GpioUsable(&Evidence->Gpio48) ||
      !GpioUsable(&Evidence->Gpio86)) {
    return MtkSc8989xOtgConflict;
  }
  return MtkSc8989xOtgSuccess;
}

static uint8_t
ActionValue(
  const MTK_SC8989X_OTG_EVIDENCE *Evidence,
  uint8_t Action
  )
{
  switch (Action) {
    case OWN_REG00_BIT7:
      return (uint8_t)((Evidence->ScReg00 &
                        MTK_SC8989X_OTG_REG00_CLEAR_MASK) != 0U);
    case OWN_GPIO48:
      return Evidence->Gpio48.PhysicalValue;
    case OWN_REG03_BIT4:
      return (uint8_t)((Evidence->ScReg03 &
                        MTK_SC8989X_OTG_REG03_CLEAR_MASK) != 0U);
    case OWN_GPIO86:
      return Evidence->Gpio86.PhysicalValue;
    case OWN_BOOST_PROFILE:
      return (uint8_t)(Evidence->ScReg0A & MTK_SC8989X_OTG_BOOST_PROFILE_MASK);
    default:
      return (uint8_t)((Evidence->ScReg03 &
                        MTK_SC8989X_OTG_REG03_BOOST_MASK) != 0U);
  }
}

static uint8_t
OriginalActionValue(
  const MTK_SC8989X_OTG_CORE *Core,
  uint8_t Action
  )
{
  switch (Action) {
    case OWN_REG00_BIT7:
      return OriginalBit00(Core);
    case OWN_GPIO48:
      return Core->OriginalGpio48;
    case OWN_REG03_BIT4:
      return OriginalBit03Clear(Core);
    case OWN_GPIO86:
      return Core->OriginalGpio86;
    case OWN_BOOST_PROFILE:
      return (uint8_t)(Core->OriginalScReg0A & MTK_SC8989X_OTG_BOOST_PROFILE_MASK);
    default:
      return OriginalBit03Boost(Core);
  }
}

static uint8_t
TargetActionValue(uint8_t Action)
{
  if (Action == OWN_BOOST_PROFILE) return MTK_SC8989X_OTG_BOOST_PROFILE_T70;
  return (uint8_t)((Action == OWN_GPIO48) || (Action == OWN_GPIO86) ||
                   (Action == OWN_REG03_BIT5));
}

static MTK_SC8989X_OTG_STATUS
CheckCleanupVector(
  const MTK_SC8989X_OTG_CORE *Core,
  const MTK_SC8989X_OTG_EVIDENCE *Evidence
  )
{
  for (unsigned Index = 0;
       Index < sizeof(mRestoreOrder) / sizeof(mRestoreOrder[0]);
       ++Index) {
    uint8_t Action = mRestoreOrder[Index];
    uint8_t Current = ActionValue(Evidence, Action);
    uint8_t Original = OriginalActionValue(Core, Action);
    uint8_t Target = TargetActionValue(Action);
    uint8_t Owned = (uint8_t)((Core->OwnedMask & Action) != 0U);
    uint8_t Restored = (uint8_t)((Core->RestoredMask & Action) != 0U);

    if (Owned == 0U) {
      if (Current != Original) return MtkSc8989xOtgConflict;
    } else if (Restored != 0U) {
      if (Current != Original) return MtkSc8989xOtgConflict;
    } else if ((Current != Original) && (Current != Target)) {
      return MtkSc8989xOtgConflict;
    }
  }
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
ReconcileRestoredMask(
  MTK_SC8989X_OTG_CORE *Core,
  const MTK_SC8989X_OTG_EVIDENCE *Evidence
  )
{
  MTK_SC8989X_OTG_STATUS Status;

  Status = CheckRestoreEvidence(Core, Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  for (unsigned Index = 0;
       Index < sizeof(mRestoreOrder) / sizeof(mRestoreOrder[0]);
       ++Index) {
    uint8_t Action = mRestoreOrder[Index];
    uint8_t Current = ActionValue(Evidence, Action);
    uint8_t Original = OriginalActionValue(Core, Action);
    uint8_t Target = TargetActionValue(Action);

    if ((Core->OwnedMask & Action) == 0U) {
      if (Current != Original) return MtkSc8989xOtgConflict;
    } else if (Current == Original) {
      Core->RestoredMask |= Action;
    } else if (Current == Target) {
      Core->RestoredMask &= (uint8_t)~Action;
    } else {
      return MtkSc8989xOtgConflict;
    }
  }
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
RestoreAction(
  MTK_SC8989X_OTG_CORE *Core,
  uint8_t Action
  )
{
  MTK_SC8989X_OTG_EVIDENCE Evidence;
  MTK_SC8989X_OTG_STATUS Status;
  uint8_t Current;
  uint8_t Original;
  uint8_t Target;

  if ((Core->OwnedMask & Action) == 0U ||
      (Core->RestoredMask & Action) != 0U) {
    return MtkSc8989xOtgSuccess;
  }

  Status = ReadEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckRestoreEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckCleanupVector(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  Current = ActionValue(&Evidence, Action);
  Original = OriginalActionValue(Core, Action);
  Target = TargetActionValue(Action);
  if (Current == Original) {
    Core->RestoredMask |= Action;
    return MtkSc8989xOtgSuccess;
  }
  if (Current != Target) return MtkSc8989xOtgConflict;

  if (Action == OWN_BOOST_PROFILE) {
    if ((Evidence.ScReg03 & MTK_SC8989X_OTG_REG03_BOOST_MASK) != 0U ||
        Evidence.Gpio86.PhysicalValue != 0U) return MtkSc8989xOtgConflict;
    Status = Core->Io.UpdateScRegisterBits(Core->Io.Context,
      MTK_SC8989X_OTG_REG_BOOST_LIMIT, MTK_SC8989X_OTG_BOOST_PROFILE_MASK, Original);
  } else if ((Action == OWN_REG00_BIT7) || (Action == OWN_REG03_BIT4) ||
             (Action == OWN_REG03_BIT5)) {
    uint8_t Register;
    uint8_t Mask;
    uint8_t Value;

    if (Action == OWN_REG00_BIT7) {
      Register = MTK_SC8989X_OTG_REG_CHARGE_CONTROL;
      Mask = MTK_SC8989X_OTG_REG00_CLEAR_MASK;
    } else if (Action == OWN_REG03_BIT4) {
      Register = MTK_SC8989X_OTG_REG_BOOST_CONTROL;
      Mask = MTK_SC8989X_OTG_REG03_CLEAR_MASK;
    } else {
      Register = MTK_SC8989X_OTG_REG_BOOST_CONTROL;
      Mask = MTK_SC8989X_OTG_REG03_BOOST_MASK;
    }
    Value = (Original != 0U) ? Mask : 0U;
    Status = Core->Io.UpdateScRegisterBits(
      Core->Io.Context, Register, Mask, Value);
  } else {
    uint8_t Pin = (Action == OWN_GPIO48)
      ? MTK_SC8989X_OTG_GPIO_ENABLE_PIN : MTK_SC8989X_OTG_GPIO_OTG_PIN;
    Status = Core->Io.WriteGpio(Core->Io.Context, Pin, Original);
  }
  if (Status != MtkSc8989xOtgSuccess) return Status;

  Status = ReadEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckRestoreEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckCleanupVector(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  if (ActionValue(&Evidence, Action) != Original) {
    return MtkSc8989xOtgConflict;
  }

  Core->RestoredMask |= Action;
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
RestoreOwned(MTK_SC8989X_OTG_CORE *Core)
{
  MTK_SC8989X_OTG_EVIDENCE Evidence;
  MTK_SC8989X_OTG_STATUS Status;

  if (Core->OwnedMask == 0U) {
    Core->RestoredMask = 0U;
    return MtkSc8989xOtgSuccess;
  }

  Status = ReadEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = ReconcileRestoredMask(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  for (unsigned Index = 0;
       Index < sizeof(mRestoreOrder) / sizeof(mRestoreOrder[0]);
       ++Index) {
    Status = RestoreAction(Core, mRestoreOrder[Index]);
    if (Status != MtkSc8989xOtgSuccess) return Status;
  }

  Status = ReadEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckRestoreEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckCleanupVector(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  for (unsigned Index = 0;
       Index < sizeof(mRestoreOrder) / sizeof(mRestoreOrder[0]);
       ++Index) {
    uint8_t Action = mRestoreOrder[Index];
    if (((Core->OwnedMask & Action) != 0U) &&
        ((Core->RestoredMask & Action) == 0U)) {
      return MtkSc8989xOtgConflict;
    }
  }

  Core->OwnedMask = 0U;
  Core->RestoredMask = 0U;
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
FailEnable(
  MTK_SC8989X_OTG_CORE *Core,
  MTK_SC8989X_OTG_STATUS Cause
  )
{
  MTK_SC8989X_OTG_STATUS Cleanup;

  if ((Cause == MtkSc8989xOtgConflict) ||
      (Cause == MtkSc8989xOtgUnsupported)) {
    Core->State = MtkSc8989xOtgStateQuarantined;
    Core->LastStatus = Cause;
    return MtkSc8989xOtgQuarantined;
  }

  Cleanup = RestoreOwned(Core);
  if (Cleanup != MtkSc8989xOtgSuccess) {
    Core->State = MtkSc8989xOtgStateQuarantined;
    Core->LastStatus = Cleanup;
    return MtkSc8989xOtgQuarantined;
  }

  Core->State = MtkSc8989xOtgStateIdle;
  Core->LastStatus = Cause;
  return Cause;
}

static MTK_SC8989X_OTG_STATUS
ApplyRegister(
  MTK_SC8989X_OTG_CORE *Core,
  MTK_SC8989X_OTG_EVIDENCE *Evidence,
  uint8_t Action,
  uint8_t Register,
  uint8_t Mask,
  uint8_t Value
  )
{
  MTK_SC8989X_OTG_STATUS Status;

  Status = ReadEvidence(Core, Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckLiveEvidence(Core, Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  if (ActionValue(Evidence, Action) == TargetActionValue(Action)) {
    return MtkSc8989xOtgSuccess;
  }

  Core->OwnedMask |= Action;
  Status = Core->Io.UpdateScRegisterBits(
    Core->Io.Context, Register, Mask, Value);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  Status = ReadEvidence(Core, Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  return CheckLiveEvidence(Core, Evidence);
}

static MTK_SC8989X_OTG_STATUS
ApplyGpio(
  MTK_SC8989X_OTG_CORE *Core,
  MTK_SC8989X_OTG_EVIDENCE *Evidence,
  uint8_t Action,
  uint8_t Pin,
  uint8_t Value
  )
{
  MTK_SC8989X_OTG_STATUS Status;
  uint8_t Current;

  Status = ReadEvidence(Core, Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  Status = CheckLiveEvidence(Core, Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  Current = (Action == OWN_GPIO48)
    ? Evidence->Gpio48.PhysicalValue : Evidence->Gpio86.PhysicalValue;
  if (Current == Value) return MtkSc8989xOtgSuccess;

  Core->OwnedMask |= Action;
  Status = Core->Io.WriteGpio(Core->Io.Context, Pin, Value);
  if (Status != MtkSc8989xOtgSuccess) return Status;

  Status = ReadEvidence(Core, Evidence);
  if (Status != MtkSc8989xOtgSuccess) return Status;
  return CheckLiveEvidence(Core, Evidence);
}

MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgInitialize(
  MTK_SC8989X_OTG_CORE *Core,
  const MTK_SC8989X_OTG_IO *Io
  )
{
  if ((Core == NULL) || (Io == NULL)) return MtkSc8989xOtgInvalid;
  if (Core->Signature == OTG_SIGNATURE) return MtkSc8989xOtgBusy;
  if (Core->Signature != 0U) return MtkSc8989xOtgInvalid;
  if ((Io->ReadFreshEvidence == NULL) ||
      (Io->UpdateScRegisterBits == NULL) ||
      (Io->WriteGpio == NULL)) {
    return MtkSc8989xOtgNotReady;
  }

  Core->Io = *Io;
  Core->Signature = OTG_SIGNATURE;
  Core->State = MtkSc8989xOtgStateIdle;
  Core->OwnedMask = 0U;
  Core->RestoredMask = 0U;
  Core->InitialScPartIdRegister = 0U;
  Core->InitialHusb311CcStatus = 0U;
  Core->InitialHusb311RoleControl = 0U;
  Core->OriginalScReg00 = 0U;
  Core->OriginalScReg03 = 0U;
  Core->OriginalScReg0A = 0U;
  Core->OriginalGpio48 = 0U;
  Core->OriginalGpio86 = 0U;
  Core->LastStatus = MtkSc8989xOtgSuccess;
  return MtkSc8989xOtgSuccess;
}

static MTK_SC8989X_OTG_STATUS
EnableSource(
  MTK_SC8989X_OTG_CORE *Core,
  uint8_t ConfigureBoostProfile
  )
{
  MTK_SC8989X_OTG_EVIDENCE Evidence;
  MTK_SC8989X_OTG_STATUS Status;

  if (!CoreValid(Core)) return MtkSc8989xOtgInvalid;
  if (Core->State == MtkSc8989xOtgStateTransition) return MtkSc8989xOtgBusy;
  if (Core->State == MtkSc8989xOtgStateQuarantined) {
    return MtkSc8989xOtgQuarantined;
  }
  if (Core->State == MtkSc8989xOtgStateOutputEnabled) {
    Core->State = MtkSc8989xOtgStateTransition;
    Status = ReadEvidence(Core, &Evidence);
    if (Status == MtkSc8989xOtgSuccess) {
      Status = CheckLiveEvidence(Core, &Evidence);
    }
    if (Status == MtkSc8989xOtgSuccess) {
      Core->State = MtkSc8989xOtgStateOutputEnabled;
      Core->LastStatus = Status;
      return Status;
    }
    Core->State = MtkSc8989xOtgStateQuarantined;
    Core->LastStatus = Status;
    return MtkSc8989xOtgQuarantined;
  }

  Core->State = MtkSc8989xOtgStateTransition;
  Status = ReadEvidence(Core, &Evidence);
  if (Status != MtkSc8989xOtgSuccess) {
    Core->State = MtkSc8989xOtgStateIdle;
    Core->LastStatus = Status;
    return Status;
  }

  Status = CheckInitialEvidence(&Evidence, ConfigureBoostProfile);
  if (Status != MtkSc8989xOtgSuccess) {
    Core->State = MtkSc8989xOtgStateIdle;
    Core->LastStatus = Status;
    return Status;
  }

  Core->OriginalScReg00 = Evidence.ScReg00;
  Core->OriginalScReg03 = Evidence.ScReg03;
  Core->OriginalScReg0A = Evidence.ScReg0A;
  Core->OriginalGpio48 = Evidence.Gpio48.PhysicalValue;
  Core->OriginalGpio86 = Evidence.Gpio86.PhysicalValue;
  Core->InitialScPartIdRegister = Evidence.ScPartIdRegister;
  Core->InitialHusb311CcStatus = Evidence.Husb311CcStatus;
  Core->InitialHusb311RoleControl = Evidence.Husb311RoleControl;
  Core->OwnedMask = 0U;
  Core->RestoredMask = 0U;

  if (ConfigureBoostProfile && !MatchesT70BoostProfile(Evidence.ScReg0A)) {
    Status = ApplyRegister(
      Core, &Evidence, OWN_BOOST_PROFILE,
      MTK_SC8989X_OTG_REG_BOOST_LIMIT,
      MTK_SC8989X_OTG_BOOST_PROFILE_MASK, MTK_SC8989X_OTG_BOOST_PROFILE_T70);
  }
  if (Status == MtkSc8989xOtgSuccess) {
    Status = ApplyRegister(
      Core, &Evidence, OWN_REG00_BIT7,
      MTK_SC8989X_OTG_REG_CHARGE_CONTROL,
      MTK_SC8989X_OTG_REG00_CLEAR_MASK, 0U);
  }
  if (Status == MtkSc8989xOtgSuccess) {
    Status = ApplyGpio(
      Core, &Evidence, OWN_GPIO48,
      MTK_SC8989X_OTG_GPIO_ENABLE_PIN, 1U);
  }
  if (Status == MtkSc8989xOtgSuccess) {
    Status = ApplyRegister(
      Core, &Evidence, OWN_REG03_BIT4,
      MTK_SC8989X_OTG_REG_BOOST_CONTROL,
      MTK_SC8989X_OTG_REG03_CLEAR_MASK, 0U);
  }
  if (Status == MtkSc8989xOtgSuccess) {
    Status = ApplyGpio(
      Core, &Evidence, OWN_GPIO86,
      MTK_SC8989X_OTG_GPIO_OTG_PIN, 1U);
  }
  if (Status == MtkSc8989xOtgSuccess) {
    Status = ApplyRegister(
      Core, &Evidence, OWN_REG03_BIT5,
      MTK_SC8989X_OTG_REG_BOOST_CONTROL,
      MTK_SC8989X_OTG_REG03_BOOST_MASK,
      MTK_SC8989X_OTG_REG03_BOOST_MASK);
  }
  if (Status != MtkSc8989xOtgSuccess) return FailEnable(Core, Status);

  Core->State = MtkSc8989xOtgStateOutputEnabled;
  Core->LastStatus = MtkSc8989xOtgSuccess;
  return MtkSc8989xOtgSuccess;
}

MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgEnable(MTK_SC8989X_OTG_CORE *Core)
{
  return EnableSource(Core, 0U);
}

MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgEnableCold(MTK_SC8989X_OTG_CORE *Core)
{
  return EnableSource(Core, 1U);
}

MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgDisable(MTK_SC8989X_OTG_CORE *Core)
{
  MTK_SC8989X_OTG_STATUS Status;

  if (!CoreValid(Core)) return MtkSc8989xOtgInvalid;
  if (Core->State == MtkSc8989xOtgStateTransition) return MtkSc8989xOtgBusy;
  if (Core->State == MtkSc8989xOtgStateQuarantined) {
    return MtkSc8989xOtgQuarantined;
  }
  if (Core->State == MtkSc8989xOtgStateIdle) return MtkSc8989xOtgSuccess;

  Core->State = MtkSc8989xOtgStateTransition;
  Status = RestoreOwned(Core);
  if (Status != MtkSc8989xOtgSuccess) {
    Core->State = MtkSc8989xOtgStateQuarantined;
    Core->LastStatus = Status;
    return MtkSc8989xOtgQuarantined;
  }

  Core->State = MtkSc8989xOtgStateIdle;
  Core->LastStatus = MtkSc8989xOtgSuccess;
  return MtkSc8989xOtgSuccess;
}

MTK_SC8989X_OTG_STATUS
MtkSc8989xOtgRetryCleanup(MTK_SC8989X_OTG_CORE *Core)
{
  MTK_SC8989X_OTG_STATUS Status;

  if (!CoreValid(Core)) return MtkSc8989xOtgInvalid;
  if (Core->State == MtkSc8989xOtgStateTransition) return MtkSc8989xOtgBusy;
  if (Core->State != MtkSc8989xOtgStateQuarantined) {
    return MtkSc8989xOtgNotReady;
  }

  Core->State = MtkSc8989xOtgStateTransition;
  Status = RestoreOwned(Core);
  if (Status != MtkSc8989xOtgSuccess) {
    Core->State = MtkSc8989xOtgStateQuarantined;
    Core->LastStatus = Status;
    return MtkSc8989xOtgQuarantined;
  }

  Core->State = MtkSc8989xOtgStateIdle;
  Core->LastStatus = MtkSc8989xOtgSuccess;
  return MtkSc8989xOtgSuccess;
}

uint8_t
MtkSc8989xOtgIsOutputEnabled(const MTK_SC8989X_OTG_CORE *Core)
{
  return (uint8_t)(CoreValid(Core) &&
                   (Core->State == MtkSc8989xOtgStateOutputEnabled));
}

uint8_t
MtkSc8989xOtgIsQuarantined(const MTK_SC8989X_OTG_CORE *Core)
{
  return (uint8_t)(CoreValid(Core) &&
                   (Core->State == MtkSc8989xOtgStateQuarantined));
}
