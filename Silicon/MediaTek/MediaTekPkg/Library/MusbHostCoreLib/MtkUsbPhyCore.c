// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkUsbPhyCore.h>

#define CORE_SIGNATURE       0x54504859U
#define U2FREQ_BASE          0x100U
#define U2COM_BASE           0x300U
#define REG_U2PHYACR0        (U2COM_BASE + 0x000U)
#define REG_U2PHYACR1        (U2COM_BASE + 0x004U)
#define REG_U2PHYACR4        (U2COM_BASE + 0x020U)
#define REG_U2PHYACR5        (U2COM_BASE + 0x014U)
#define REG_U2PHYACR6        (U2COM_BASE + 0x018U)
#define REG_U2PHYDCR0        (U2COM_BASE + 0x060U)
#define REG_U2PHYDTM0        (U2COM_BASE + 0x068U)
#define REG_U2PHYDTM1        (U2COM_BASE + 0x06cU)
#define REG_FMCR0            (U2FREQ_BASE + 0x000U)
#define REG_FMOUT            (U2FREQ_BASE + 0x00cU)
#define REG_FMMONR1          (U2FREQ_BASE + 0x010U)

#define BIT_U2_FORCE_SUSPENDM (1U << 18)
#define BIT_U2_SUSPENDM       (1U << 3)
#define BIT_U2_FORCE_UART     (1U << 26)
#define BIT_U2_UART_ENABLE    (1U << 16)
#define BIT_U2_INTR_ENABLE    (1U << 5)
#define BIT_U2_100UA_SS       (1U << 11)
#define BIT_U2_BC11_ENABLE    (1U << 23)
#define BIT_U2_OTG_VBUSCMP    (1U << 20)
#define BIT_U2_SRCAL_ENABLE   (1U << 15)
#define BIT_U2_FRCK_ENABLE    (1U << 8)
#define BIT_U2_FM_VALID       (1U << 0)
#define BIT_U2_FREQDET_ENABLE (1U << 24)

#define U2_DTM0_INIT_CLEAR_MASK 0x04040000U
#define U2_DTM0_FORCE_MASK      0x00be3cf4U
#define U2_DTM0_PART_MASK       0x00be00d4U
#define U2_FMCR0_RETAIN_MASK    0xf3000000U
#define U2_FM_CYCLE_COUNT       0x00000400U
#define U2_FM_TIMEOUT_NS        200000ULL
#define U2_FM_MAX_POLLS         128U
#define U2_FM_POLL_DELAY_US     3U
#define U2_POWER_ON_DELAY_US    30U
#define U2_POWER_SETTLE_US      800U
#define U2_POWER_OFF_DELAY_US   2000U
#define U2_POWER_OFF_SETTLE_US  800U
#define U2_FINAL_DELAY_US       1U
#define U2_DEFAULT_REF_MHZ      26U
#define U2_DEFAULT_SLEW_COEF    28U
#define U2_FM_CYCLE_DIVISOR     1000U

static void
ResetCore(MTK_USB_PHY_CORE *Core)
{
  uint8_t *Bytes;
  size_t Index;

  Bytes = (uint8_t *)Core;
  for (Index = 0U; Index < sizeof(*Core); ++Index) {
    Bytes[Index] = 0U;
  }
}

static int
CallbacksValid(const MTK_USB_PHY_IO *Io)
{
  return (Io != NULL) &&
         (Io->Read32 != NULL) &&
         (Io->Write32 != NULL) &&
         (Io->NowNs != NULL) &&
         (Io->DelayUs != NULL) &&
         (Io->ResolveClock != NULL) &&
         (Io->ClockPrepare != NULL) &&
         (Io->ClockEnable != NULL) &&
         (Io->ClockDisable != NULL) &&
         (Io->ClockUnprepare != NULL) &&
         (Io->ReadNvmemCell != NULL);
}

static MTK_USB_PHY_STATUS
ValidateConfig(
  const MTK_USB_PHY_IO *Io,
  const MTK_USB_PHY_CONFIG *Config
  )
{
  uint32_t ReferenceMHz;
  uint32_t Coefficient;
  uint32_t Product;

  if ((Io == NULL) || (Config == NULL) ||
      !CallbacksValid(Io) ||
      (Io->PhyLength < MTK_USB_TPHY_T70_U2_MIN_BYTES)) {
    return MtkUsbPhyInvalid;
  }

  if ((Io->PhyBase != MTK_USB_TPHY_T70_U2_RESOURCE_BASE) ||
      (Config->Revision != MTK_USB_PHY_CORE_REVISION) ||
      (Config->PhyType != MTK_USB_TPHY_T70_U2_TYPE) ||
      (Config->TphyVersion != MTK_USB_TPHY_VERSION_V2)) {
    return MtkUsbPhyUnsupported;
  }

  if ((Config->SetEfuseV1 != 1U) ||
      (Config->TuningPresent != MTK_USB_TPHY_TUNING_T70_REQUIRED) ||
      (Config->EyeVrt > 7U) ||
      (Config->EyeTerm > 7U) ||
      (Config->DiscTh > 15U) ||
      (Config->RxSqth > 15U) ||
      (Config->Rev4 > 1U) ||
      (Config->Reserved != 0U)) {
    return MtkUsbPhyInvalid;
  }

  if ((Config->SlewReferencePresent > 1U) ||
      (Config->SlewCoefficientPresent > 1U) ||
      (Config->SlewReferencePresent && (Config->SlewReferenceMHz == 0U)) ||
      (Config->SlewCoefficientPresent && (Config->SlewCoefficient == 0U))) {
    return MtkUsbPhyInvalid;
  }

  ReferenceMHz = Config->SlewReferencePresent ?
                 Config->SlewReferenceMHz : U2_DEFAULT_REF_MHZ;
  Coefficient = Config->SlewCoefficientPresent ?
                Config->SlewCoefficient : U2_DEFAULT_SLEW_COEF;
  if ((ReferenceMHz != 0U) &&
      (Coefficient > (0xffffffffU / ReferenceMHz))) {
    return MtkUsbPhyInvalid;
  }
  Product = ReferenceMHz * Coefficient;
  if (Product > ((0xffffffffU - 500U) / 1024U)) {
    return MtkUsbPhyInvalid;
  }

  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
ReadReg(
  MTK_USB_PHY_CORE *Core,
  uint32_t Offset,
  uint32_t *Value
  )
{
  if ((Core == NULL) || (Value == NULL) ||
      ((Offset & 3U) != 0U) ||
      ((uint64_t)Offset + sizeof(uint32_t) > Core->Io.PhyLength)) {
    return MtkUsbPhyInvalid;
  }

  return Core->Io.Read32(Core->Io.Context, Offset, Value);
}

static MTK_USB_PHY_STATUS
WriteReg(
  MTK_USB_PHY_CORE *Core,
  uint32_t Offset,
  uint32_t Value
  )
{
  if ((Core == NULL) ||
      ((Offset & 3U) != 0U) ||
      ((uint64_t)Offset + sizeof(uint32_t) > Core->Io.PhyLength)) {
    return MtkUsbPhyInvalid;
  }

  return Core->Io.Write32(Core->Io.Context, Offset, Value);
}

static MTK_USB_PHY_STATUS
UpdateBits(
  MTK_USB_PHY_CORE *Core,
  uint32_t Offset,
  uint32_t Mask,
  uint32_t Value
  )
{
  MTK_USB_PHY_STATUS Status;
  uint32_t OldValue;

  Status = ReadReg(Core, Offset, &OldValue);
  if (Status != MtkUsbPhySuccess) {
    return Status;
  }

  return WriteReg(Core, Offset, (OldValue & ~Mask) | (Value & Mask));
}

static MTK_USB_PHY_STATUS
SetBits(
  MTK_USB_PHY_CORE *Core,
  uint32_t Offset,
  uint32_t Bits
  )
{
  return UpdateBits(Core, Offset, Bits, Bits);
}

static MTK_USB_PHY_STATUS
ClearBits(
  MTK_USB_PHY_CORE *Core,
  uint32_t Offset,
  uint32_t Bits
  )
{
  return UpdateBits(Core, Offset, Bits, 0U);
}

static MTK_USB_PHY_STATUS
DelayUs(
  MTK_USB_PHY_CORE *Core,
  uint32_t Microseconds
  )
{
  return Core->Io.DelayUs(Core->Io.Context, Microseconds);
}

static uint32_t
ClockBit(MTK_USB_PHY_CLOCK_SLOT Slot)
{
  return 1U << (uint32_t)Slot;
}

static MTK_USB_PHY_STATUS
ReleaseClockResources(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS FirstError;
  MTK_USB_PHY_STATUS Status;
  int Slot;

  FirstError = MtkUsbPhySuccess;

  for (Slot = (int)MtkUsbPhyClockCount - 1; Slot >= 0; --Slot) {
    uint32_t Bit;

    Bit = ClockBit((MTK_USB_PHY_CLOCK_SLOT)Slot);
    if ((Core->ClockEnabledMask & Bit) == 0U) {
      continue;
    }

    Status = Core->Io.ClockDisable(
                        Core->Io.Context,
                        (MTK_USB_PHY_CLOCK_SLOT)Slot
                        );
    if (Status == MtkUsbPhySuccess) {
      Core->ClockEnabledMask &= (uint8_t)~Bit;
    } else if (FirstError == MtkUsbPhySuccess) {
      FirstError = Status;
    }
  }

  for (Slot = (int)MtkUsbPhyClockCount - 1; Slot >= 0; --Slot) {
    uint32_t Bit;

    Bit = ClockBit((MTK_USB_PHY_CLOCK_SLOT)Slot);
    if (((Core->ClockPreparedMask & Bit) == 0U) ||
        ((Core->ClockEnabledMask & Bit) != 0U)) {
      continue;
    }

    Status = Core->Io.ClockUnprepare(
                        Core->Io.Context,
                        (MTK_USB_PHY_CLOCK_SLOT)Slot
                        );
    if (Status == MtkUsbPhySuccess) {
      Core->ClockPreparedMask &= (uint8_t)~Bit;
    } else if (FirstError == MtkUsbPhySuccess) {
      FirstError = Status;
    }
  }

  return FirstError;
}

static MTK_USB_PHY_STATUS
ResolvePrepareEnableClocks(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;
  uint8_t Present;
  uint32_t Bit;
  uint32_t Slot;

  for (Slot = 0U; Slot < (uint32_t)MtkUsbPhyClockCount; ++Slot) {
    Present = 0U;
    Status = Core->Io.ResolveClock(
                         Core->Io.Context,
                         (MTK_USB_PHY_CLOCK_SLOT)Slot,
                         &Present
                         );
    if (Status != MtkUsbPhySuccess) {
      return Status;
    }
    if (Present > 1U) {
      return MtkUsbPhyInvalid;
    }
    if (Present != 0U) {
      Core->ClockPresentMask |= (uint8_t)(1U << Slot);
    }
  }

  for (Slot = 0U; Slot < (uint32_t)MtkUsbPhyClockCount; ++Slot) {
    Bit = 1U << Slot;
    if ((Core->ClockPresentMask & Bit) == 0U) {
      continue;
    }
    Status = Core->Io.ClockPrepare(
                         Core->Io.Context,
                         (MTK_USB_PHY_CLOCK_SLOT)Slot
                         );
    if (Status != MtkUsbPhySuccess) {
      return Status;
    }
    Core->ClockPreparedMask |= (uint8_t)Bit;
  }

  for (Slot = 0U; Slot < (uint32_t)MtkUsbPhyClockCount; ++Slot) {
    Bit = 1U << Slot;
    if ((Core->ClockPresentMask & Bit) == 0U) {
      continue;
    }
    Status = Core->Io.ClockEnable(
                         Core->Io.Context,
                         (MTK_USB_PHY_CLOCK_SLOT)Slot
                         );
    if (Status != MtkUsbPhySuccess) {
      return Status;
    }
    Core->ClockEnabledMask |= (uint8_t)Bit;
  }

  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
ApplyT70Efuse(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;
  uint32_t RawValue;
  uint32_t FieldValue;

  RawValue = 0U;
  Status = Core->Io.ReadNvmemCell(
                       Core->Io.Context,
                       MTK_USB_TPHY_T70_NVMEM_CELL,
                       MTK_USB_TPHY_T70_NVMEM_MASK,
                       &RawValue
                       );
  if (Status != MtkUsbPhySuccess) {
    return Status;
  }

  FieldValue = RawValue & MTK_USB_TPHY_T70_NVMEM_MASK;
  if (FieldValue == 0U) {
    return MtkUsbPhySuccess;
  }

  Core->RegistersTouched = 1U;
  return UpdateBits(
           Core,
           REG_U2PHYACR1,
           0x00f80000U,
           FieldValue << 19
           );
}

static MTK_USB_PHY_STATUS
InitializeU2Registers(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;

  Core->RegistersTouched = 1U;

  Status = UpdateBits(
             Core, REG_U2PHYDTM0,
             U2_DTM0_INIT_CLEAR_MASK, 0U
             );
  if (Status != MtkUsbPhySuccess) return Status;

  Status = UpdateBits(Core, REG_U2PHYDTM0, 0x00000030U, 0x00000010U);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_U2PHYDTM0, 0x00003c00U);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_U2PHYDTM1, BIT_U2_UART_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = SetBits(Core, REG_U2PHYACR0, BIT_U2_INTR_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_U2PHYACR5, BIT_U2_100UA_SS);
  if (Status != MtkUsbPhySuccess) return Status;

  /* Original generic-v2 init at .text+0xd08 and .text+0x1170. */
  Status = ClearBits(Core, REG_U2PHYACR4, 0x00000300U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYACR6, 0x00800000U);
  if (Status != MtkUsbPhySuccess) return Status;
  return UpdateBits(Core, REG_U2PHYACR6, 0x0000000fU, 2U);
}

static MTK_USB_PHY_STATUS
PollFrequencyMeter(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;
  uint64_t Started;
  uint64_t Now;
  uint32_t Value;
  uint32_t Poll;

  Status = Core->Io.NowNs(Core->Io.Context, &Started);
  if (Status != MtkUsbPhySuccess) {
    return Status;
  }

  for (Poll = 0U; Poll < U2_FM_MAX_POLLS; ++Poll) {
    Status = ReadReg(Core, REG_FMMONR1, &Value);
    if (Status != MtkUsbPhySuccess) {
      return Status;
    }
    if ((Value & BIT_U2_FM_VALID) != 0U) {
      return MtkUsbPhySuccess;
    }

    Status = Core->Io.NowNs(Core->Io.Context, &Now);
    if (Status != MtkUsbPhySuccess) {
      return Status;
    }
    if ((Now >= Started) && ((Now - Started) >= U2_FM_TIMEOUT_NS)) {
      return MtkUsbPhyTimeout;
    }

    Status = DelayUs(Core, U2_FM_POLL_DELAY_US);
    if (Status != MtkUsbPhySuccess) {
      return Status;
    }
  }

  return MtkUsbPhyTimeout;
}

static MTK_USB_PHY_STATUS
DisableCalibration(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS FirstError;
  MTK_USB_PHY_STATUS Status;

  FirstError = MtkUsbPhySuccess;

  Status = ClearBits(Core, REG_FMCR0, BIT_U2_FREQDET_ENABLE);
  if (Status != MtkUsbPhySuccess) {
    FirstError = Status;
  }

  Status = ClearBits(Core, REG_FMMONR1, BIT_U2_FRCK_ENABLE);
  if ((Status != MtkUsbPhySuccess) &&
      (FirstError == MtkUsbPhySuccess)) {
    FirstError = Status;
  }

  Status = ClearBits(Core, REG_U2PHYACR5, BIT_U2_SRCAL_ENABLE);
  if ((Status != MtkUsbPhySuccess) &&
      (FirstError == MtkUsbPhySuccess)) {
    FirstError = Status;
  }

  if (FirstError == MtkUsbPhySuccess) {
    Core->CalibrationActive = 0U;
  }

  return FirstError;
}

static MTK_USB_PHY_STATUS
CalibrateSlewRate(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;
  uint32_t Value;
  uint32_t Frequency;
  uint32_t ReferenceMHz;
  uint32_t Coefficient;
  uint32_t Calibration;
  uint32_t Product;
  uint32_t Numerator;

  Core->CalibrationActive = 1U;

  Status = SetBits(Core, REG_U2PHYACR5, BIT_U2_SRCAL_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = DelayUs(Core, U2_FINAL_DELAY_US);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = SetBits(Core, REG_FMMONR1, BIT_U2_FRCK_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ReadReg(Core, REG_FMCR0, &Value);
  if (Status != MtkUsbPhySuccess) return Status;
  Value = (Value & U2_FMCR0_RETAIN_MASK) | U2_FM_CYCLE_COUNT;
  Status = WriteReg(Core, REG_FMCR0, Value);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = SetBits(Core, REG_FMCR0, BIT_U2_FREQDET_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = PollFrequencyMeter(Core);
  // Stock continues with FMOUT (or default4) after the bounded poll expires.
  if ((Status != MtkUsbPhySuccess) && (Status != MtkUsbPhyTimeout)) return Status;

  Status = ReadReg(Core, REG_FMOUT, &Frequency);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_FMCR0, BIT_U2_FREQDET_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_FMMONR1, BIT_U2_FRCK_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;

  ReferenceMHz = Core->Config.SlewReferencePresent ?
                 Core->Config.SlewReferenceMHz :
                 U2_DEFAULT_REF_MHZ;
  Coefficient = Core->Config.SlewCoefficientPresent ?
                Core->Config.SlewCoefficient :
                U2_DEFAULT_SLEW_COEF;

  if (Frequency == 0U) {
    Calibration = 4U;
  } else {
    Product = ReferenceMHz * Coefficient;
    Numerator = Product << 10;
    Calibration = ((Numerator / Frequency) + 500U) /
                  U2_FM_CYCLE_DIVISOR;
  }

  Status = UpdateBits(
             Core,
             REG_U2PHYACR5,
             0x00007000U,
             (Calibration << 12) & 0x00007000U
             );
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_U2PHYACR5, BIT_U2_SRCAL_ENABLE);
  if (Status == MtkUsbPhySuccess) {
    Core->CalibrationActive = 0U;
  }
  return Status;
}

static MTK_USB_PHY_STATUS
ApplyT70Tuning(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;

  if (Core->Config.EyeVrt != 0U) {
    Status = UpdateBits(
               Core, REG_U2PHYACR1, 0x00007000U,
               Core->Config.EyeVrt << 12
               );
    if (Status != MtkUsbPhySuccess) return Status;
  }

  if (Core->Config.EyeTerm != 0U) {
    Status = UpdateBits(
               Core, REG_U2PHYACR1, 0x00000700U,
               Core->Config.EyeTerm << 8
               );
    if (Status != MtkUsbPhySuccess) return Status;
  }

  if (Core->Config.DiscTh != 0U) {
    Status = UpdateBits(
               Core, REG_U2PHYACR6, 0x000000f0U,
               Core->Config.DiscTh << 4
               );
    if (Status != MtkUsbPhySuccess) return Status;
  }

  if (Core->Config.Rev4 != 0U) {
    Status = SetBits(Core, REG_U2PHYACR6, 1U << 28);
    if (Status != MtkUsbPhySuccess) return Status;
  }

  if (Core->Config.RxSqth != 0U) {
    Status = UpdateBits(
               Core, REG_U2PHYACR6, 0x0000000fU,
               Core->Config.RxSqth
               );
    if (Status != MtkUsbPhySuccess) return Status;
  }

  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
PowerOnSequence(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;

  Core->RegistersTouched = 1U;

  Status = SetBits(Core, REG_U2PHYDTM0, BIT_U2_FORCE_SUSPENDM);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM0, BIT_U2_SUSPENDM);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = SetBits(Core, REG_U2PHYDTM0, BIT_U2_SUSPENDM);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = DelayUs(Core, U2_POWER_ON_DELAY_US);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_U2PHYDTM0, BIT_U2_FORCE_SUSPENDM);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM0, BIT_U2_SUSPENDM);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM0, BIT_U2_FORCE_UART);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM1, BIT_U2_UART_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYACR4, 0x00000300U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM0, BIT_U2_FORCE_SUSPENDM);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM0, U2_DTM0_FORCE_MASK);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_U2PHYACR6, BIT_U2_BC11_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = SetBits(Core, REG_U2PHYACR6, BIT_U2_OTG_VBUSCMP);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = SetBits(Core, REG_U2PHYDTM1, 0x00000024U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM1, 0x00000010U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = UpdateBits(
             Core, REG_U2PHYACR6, 0xc0000000U, 0x40000000U
             );
  if (Status != MtkUsbPhySuccess) return Status;
  Status = DelayUs(Core, U2_POWER_SETTLE_US);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = CalibrateSlewRate(Core);
  if (Status != MtkUsbPhySuccess) return Status;

  return ApplyT70Tuning(Core);
}

static MTK_USB_PHY_STATUS
PowerOffSequence(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;

  Status = ClearBits(Core, REG_U2PHYDTM0, BIT_U2_FORCE_UART);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM1, BIT_U2_UART_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYACR4, 0x00000300U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYACR6, BIT_U2_BC11_ENABLE);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYACR6, BIT_U2_OTG_VBUSCMP);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM1, 0x00000024U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = SetBits(Core, REG_U2PHYDTM1, 0x00000010U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = SetBits(
             Core, REG_U2PHYDTM0,
             BIT_U2_FORCE_SUSPENDM | BIT_U2_SUSPENDM
             );
  if (Status != MtkUsbPhySuccess) return Status;
  Status = DelayUs(Core, U2_POWER_OFF_DELAY_US);
  if (Status != MtkUsbPhySuccess) return Status;

  Status = ClearBits(Core, REG_U2PHYDTM0, 0x00003c00U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = SetBits(Core, REG_U2PHYDTM0, U2_DTM0_PART_MASK);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = SetBits(Core, REG_U2PHYACR6, 0x40000000U);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = DelayUs(Core, U2_POWER_OFF_SETTLE_US);
  if (Status != MtkUsbPhySuccess) return Status;
  Status = ClearBits(Core, REG_U2PHYDTM0, BIT_U2_SUSPENDM);
  if (Status != MtkUsbPhySuccess) return Status;

  return DelayUs(Core, U2_FINAL_DELAY_US);
}


MTK_USB_PHY_STATUS
MtkUsbPhyCoreBuildResumeProfile(
  const MTK_USB_PHY_RESUME_SNAPSHOT *Snapshot,
  MTK_USB_PHY_CONFIG *Config,
  MTK_USB_PHY_HOST_TUNING *HostTuning
  )
{
  MTK_USB_PHY_CONFIG LocalConfig = {0};
  MTK_USB_PHY_HOST_TUNING LocalHostTuning = {0};

  if ((Snapshot == NULL) || (Config == NULL) || (HostTuning == NULL)) {
    return MtkUsbPhyInvalid;
  }
  if (((Snapshot->Acr1 & ~0x00f87700U) != 0U) ||
      ((Snapshot->Acr5 & ~0x00007000U) != 0U) ||
      ((Snapshot->Acr6 & ~0xd01000ffU) != 0U)) {
    return MtkUsbPhyInvalid;
  }

  LocalConfig.Revision = MTK_USB_PHY_CORE_REVISION;
  LocalConfig.PhyType = MTK_USB_TPHY_T70_U2_TYPE;
  LocalConfig.TphyVersion = MTK_USB_TPHY_VERSION_V2;
  LocalConfig.TuningPresent = MTK_USB_TPHY_TUNING_T70_REQUIRED;
  LocalConfig.EyeVrt = (Snapshot->Acr1 >> 12) & 7U;
  LocalConfig.EyeTerm = (Snapshot->Acr1 >> 8) & 7U;
  LocalConfig.DiscTh = (Snapshot->Acr6 >> 4) & 0x0fU;
  LocalConfig.RxSqth = Snapshot->Acr6 & 0x0fU;
  LocalConfig.Rev4 = (Snapshot->Acr6 >> 28) & 1U;
  LocalConfig.SetEfuseV1 = 1U;

  LocalHostTuning.SlewControl = (Snapshot->Acr5 >> 12) & 7U;
  LocalHostTuning.EyeVrt = LocalConfig.EyeVrt;
  LocalHostTuning.EyeTerm = LocalConfig.EyeTerm;
  LocalHostTuning.Rev6 = (Snapshot->Acr6 >> 30) & 3U;
  LocalHostTuning.DisconnectThreshold = LocalConfig.DiscTh;

  *Config = LocalConfig;
  *HostTuning = LocalHostTuning;
  return MtkUsbPhySuccess;
}

MTK_USB_PHY_STATUS
MtkUsbPhyCoreInitialize(
  MTK_USB_PHY_CORE *Core,
  const MTK_USB_PHY_IO *Io,
  const MTK_USB_PHY_CONFIG *Config
  )
{
  MTK_USB_PHY_STATUS Status;
  MTK_USB_PHY_STATUS CleanupStatus;

  if (Core == NULL) {
    return MtkUsbPhyInvalid;
  }

  if ((Core->Signature == CORE_SIGNATURE) &&
      (Core->State != MTK_USB_TPHY_STATE_COLD)) {
    return MtkUsbPhyBusy;
  }
  if ((Core->Signature != 0U) &&
      (Core->Signature != CORE_SIGNATURE)) {
    return MtkUsbPhyInvalid;
  }

  Status = ValidateConfig(Io, Config);
  if (Status != MtkUsbPhySuccess) {
    return Status;
  }

  ResetCore(Core);
  Core->Io = *Io;
  Core->Config = *Config;
  Core->Signature = CORE_SIGNATURE;
  Core->State = MTK_USB_TPHY_STATE_COLD;

  Status = ResolvePrepareEnableClocks(Core);
  if (Status != MtkUsbPhySuccess) {
    goto Rollback;
  }

  Status = ApplyT70Efuse(Core);
  if (Status != MtkUsbPhySuccess) {
    goto Rollback;
  }

  Status = InitializeU2Registers(Core);
  if (Status != MtkUsbPhySuccess) {
    goto Rollback;
  }

  Status = ApplyT70Tuning(Core);
  if (Status != MtkUsbPhySuccess) {
    goto Rollback;
  }

  Core->State = MTK_USB_TPHY_STATE_INITIALIZED;
  return MtkUsbPhySuccess;

Rollback:
  if (Core->RegistersTouched != 0U) {
    Core->State = MTK_USB_TPHY_STATE_FAULTED;
    return Status;
  }

  CleanupStatus = ReleaseClockResources(Core);
  if (CleanupStatus == MtkUsbPhySuccess) {
    ResetCore(Core);
  } else {
    Core->State = MTK_USB_TPHY_STATE_FAULTED;
  }
  return Status;
}

MTK_USB_PHY_STATUS
MtkUsbPhyCorePowerOn(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;
  MTK_USB_PHY_STATUS CleanupStatus;

  if ((Core == NULL) || (Core->Signature != CORE_SIGNATURE)) {
    return MtkUsbPhyInvalid;
  }
  if (Core->State != MTK_USB_TPHY_STATE_INITIALIZED) {
    return (Core->State == MTK_USB_TPHY_STATE_POWERED) ?
           MtkUsbPhyBusy : MtkUsbPhyNotReady;
  }

  Core->PowerCleanupPending = 1U;
  Status = PowerOnSequence(Core);
  if (Status == MtkUsbPhySuccess) {
    Core->PowerCleanupPending = 0U;
    Core->State = MTK_USB_TPHY_STATE_POWERED;
    return MtkUsbPhySuccess;
  }

  if (Core->CalibrationActive) {
    (void)DisableCalibration(Core);
  }
  CleanupStatus = PowerOffSequence(Core);
  if ((CleanupStatus == MtkUsbPhySuccess) &&
      (Core->CalibrationActive == 0U)) {
    Core->PowerCleanupPending = 0U;
    Core->State = MTK_USB_TPHY_STATE_INITIALIZED;
  } else {
    Core->PowerCleanupPending = 1U;
    Core->State = MTK_USB_TPHY_STATE_FAULTED;
  }

  return Status;
}

static MTK_USB_PHY_STATUS
ApplyHostTuning(
  MTK_USB_PHY_CORE *Core,
  const MTK_USB_PHY_HOST_TUNING *Tuning
  )
{
  MTK_USB_PHY_STATUS Status;

  if (Tuning->SlewControl != 0U) {
    Status = UpdateBits(Core, REG_U2PHYACR5, 0x00007000U,
                        Tuning->SlewControl << 12);
    if (Status != MtkUsbPhySuccess) return Status;
  }
  if (Tuning->EyeVrt != 0U) {
    Status = UpdateBits(Core, REG_U2PHYACR1, 0x00007000U,
                        Tuning->EyeVrt << 12);
    if (Status != MtkUsbPhySuccess) return Status;
  }
  if (Tuning->EyeTerm != 0U) {
    Status = UpdateBits(Core, REG_U2PHYACR1, 0x00000700U,
                        Tuning->EyeTerm << 8);
    if (Status != MtkUsbPhySuccess) return Status;
  }
  if (Tuning->Rev6 != 0U) {
    Status = UpdateBits(Core, REG_U2PHYACR6, 0xc0000000U,
                        Tuning->Rev6 << 30);
    if (Status != MtkUsbPhySuccess) return Status;
  }
  if (Tuning->DisconnectThreshold != 0U) {
    Status = UpdateBits(Core, REG_U2PHYACR6, 0x000000f0U,
                        Tuning->DisconnectThreshold << 4);
    if (Status != MtkUsbPhySuccess) return Status;
  }
  return MtkUsbPhySuccess;
}

MTK_USB_PHY_STATUS
MtkUsbPhyCoreSetUsbMode(
  MTK_USB_PHY_CORE *Core,
  MTK_USB_PHY_USB_MODE Mode,
  const MTK_USB_PHY_HOST_TUNING *HostTuning
  )
{
  MTK_USB_PHY_STATUS Status;
  uint32_t Value;

  if ((Core == NULL) || (Core->Signature != CORE_SIGNATURE)) {
    return MtkUsbPhyInvalid;
  }
  if ((Mode != MtkUsbPhyModeHost) &&
      (Mode != MtkUsbPhyModeDevice) &&
      (Mode != MtkUsbPhyModeOtg)) {
    return MtkUsbPhyUnsupported;
  }
  if (Mode == MtkUsbPhyModeHost) {
    if ((HostTuning == NULL) ||
        (HostTuning->SlewControl > 7U) ||
        (HostTuning->EyeVrt > 7U) ||
        (HostTuning->EyeTerm > 7U) ||
        (HostTuning->Rev6 > 3U) ||
        (HostTuning->DisconnectThreshold > 15U)) {
      return MtkUsbPhyInvalid;
    }
  } else if (HostTuning != NULL) {
    return MtkUsbPhyInvalid;
  }
  if (Core->State != MTK_USB_TPHY_STATE_POWERED) {
    return MtkUsbPhyNotReady;
  }

  /* Stock mtk_phy_set_mode first snapshots DTM1, then applies host/device
   * tuning, and finally writes that snapshot's ID bits. Preserve that order. */
  Status = ReadReg(Core, REG_U2PHYDTM1, &Value);
  if (Status != MtkUsbPhySuccess) goto Fault;
  if (Mode == MtkUsbPhyModeHost) {
    Status = ApplyHostTuning(Core, HostTuning);
    if (Status != MtkUsbPhySuccess) goto Fault;
    Value = (Value & ~0x00000002U) | 0x00000200U;
  } else if (Mode == MtkUsbPhyModeDevice) {
    Status = ApplyT70Tuning(Core);
    if (Status != MtkUsbPhySuccess) goto Fault;
    Value |= 0x00000202U;
  } else {
    Value &= ~0x00000202U;
  }
  Status = WriteReg(Core, REG_U2PHYDTM1, Value);
  if (Status != MtkUsbPhySuccess) goto Fault;
  return MtkUsbPhySuccess;

Fault:
  /* An MMIO callback may have modified hardware before failing. Do not claim
   * a usable PHY, replay mode writes or release clocks on this path. */
  Core->State = MTK_USB_TPHY_STATE_FAULTED;
  Core->PowerCleanupPending = 1U;
  return Status;
}

MTK_USB_PHY_STATUS
MtkUsbPhyCoreSetMusbSession(
  MTK_USB_PHY_CORE *Core,
  MTK_USB_PHY_MUSB_SESSION *Session,
  MTK_USB_PHY_MUSB_OPERATION Operation
  )
{
  MTK_USB_PHY_STATUS Status;
  uint32_t Value;
  const uint32_t Mask = 0x00003f3fU;

  if ((Core == NULL) || (Core->Signature != CORE_SIGNATURE) ||
      (Session == NULL)) return MtkUsbPhyInvalid;
  if ((Operation != MtkUsbPhyMusbInvalid) &&
      (Operation != MtkUsbPhyMusbHost) &&
      (Operation != MtkUsbPhyMusbRestore) &&
      (Operation != MtkUsbPhyMusbAcquire)) return MtkUsbPhyUnsupported;
  if (Core->State != MTK_USB_TPHY_STATE_POWERED) return MtkUsbPhyNotReady;
  if ((((Operation == MtkUsbPhyMusbInvalid) ||
        (Operation == MtkUsbPhyMusbAcquire)) && (Session->Owned != 0U)) ||
      ((Operation == MtkUsbPhyMusbHost) && (Session->Owned != 1U)) ||
      ((Operation == MtkUsbPhyMusbRestore) &&
       ((Session->Owned == 0U) || (Session->Owned > 2U)))) return MtkUsbPhyBusy;

  if (Operation == MtkUsbPhyMusbRestore) {
    Status = UpdateBits(Core, REG_U2PHYDTM1, Mask, Session->OriginalDtm1 & Mask);
    if (Status != MtkUsbPhySuccess) goto Fault;
    Status = ReadReg(Core, REG_U2PHYDTM1, &Value);
    if (Status != MtkUsbPhySuccess) goto Fault;
    if ((Value & Mask) != (Session->OriginalDtm1 & Mask)) {
      Status = MtkUsbPhyIoError;
      goto Fault;
    }
    *Session = (MTK_USB_PHY_MUSB_SESSION){0};
    return MtkUsbPhySuccess;
  }

  if (Operation == MtkUsbPhyMusbAcquire) {
    Status = ReadReg(Core, REG_U2PHYDTM1, &Value);
    if (Status != MtkUsbPhySuccess) goto Fault;
    Session->OriginalDtm1 = Value;
    Session->Owned = 1U;
    return MtkUsbPhySuccess;
  }

  if (Operation == MtkUsbPhyMusbInvalid) {
    Status = ReadReg(Core, REG_U2PHYDTM1, &Value);
    if (Status != MtkUsbPhySuccess) goto Fault;
    Session->OriginalDtm1 = Value;
    Session->Owned = 1U; /* Own even a write callback that fails after effect. */
    /* Original set_usb_phy_mode(0): three distinct read/modify/write pairs. */
    Status = SetBits(Core, REG_U2PHYDTM1, 0x11U);
    if (Status != MtkUsbPhySuccess) goto Fault;
    Status = ClearBits(Core, REG_U2PHYDTM1, 0x2eU);
  } else {
    Session->Owned = 2U;
    /* Generic HOST tuning has already run; original helper follows it. */
    Status = ClearBits(Core, REG_U2PHYDTM1, 0x12U);
    if (Status != MtkUsbPhySuccess) goto Fault;
    Status = SetBits(Core, REG_U2PHYDTM1, 0x2dU);
  }
  if (Status != MtkUsbPhySuccess) goto Fault;
  Status = SetBits(Core, REG_U2PHYDTM1, 0x3f00U);
  if (Status != MtkUsbPhySuccess) goto Fault;
  return MtkUsbPhySuccess;

Fault:
  Core->State = MTK_USB_TPHY_STATE_FAULTED;
  Core->PowerCleanupPending = 1U;
  return Status;
}

MTK_USB_PHY_STATUS
MtkUsbPhyCorePowerOff(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;

  if ((Core == NULL) || (Core->Signature != CORE_SIGNATURE)) {
    return MtkUsbPhyInvalid;
  }
  if ((Core->State != MTK_USB_TPHY_STATE_POWERED) &&
      !((Core->State == MTK_USB_TPHY_STATE_FAULTED) &&
        (Core->PowerCleanupPending != 0U))) {
    return MtkUsbPhyNotReady;
  }

  if (Core->CalibrationActive) {
    Status = DisableCalibration(Core);
    if (Status != MtkUsbPhySuccess) {
      Core->State = MTK_USB_TPHY_STATE_FAULTED;
      Core->PowerCleanupPending = 1U;
      return Status;
    }
  }

  Status = PowerOffSequence(Core);
  if (Status != MtkUsbPhySuccess) {
    Core->State = MTK_USB_TPHY_STATE_FAULTED;
    Core->PowerCleanupPending = 1U;
    return Status;
  }

  Core->State = MTK_USB_TPHY_STATE_INITIALIZED;
  Core->PowerCleanupPending = 0U;
  return MtkUsbPhySuccess;
}

MTK_USB_PHY_STATUS
MtkUsbPhyCoreExit(MTK_USB_PHY_CORE *Core)
{
  MTK_USB_PHY_STATUS Status;

  if ((Core == NULL) || (Core->Signature != CORE_SIGNATURE)) {
    return MtkUsbPhyInvalid;
  }
  if (Core->State == MTK_USB_TPHY_STATE_POWERED) {
    return MtkUsbPhyBusy;
  }
  if ((Core->State == MTK_USB_TPHY_STATE_FAULTED) &&
      (Core->PowerCleanupPending != 0U)) {
    return MtkUsbPhyBusy;
  }

  /* generic-tphy-v2 has pdata byte[0] == 0. Its stock exit path skips
   * DCR0/DTM0 cleanup MMIO and releases only the acquired clock resources. */
  Status = ReleaseClockResources(Core);
  if (Status != MtkUsbPhySuccess) {
    Core->State = MTK_USB_TPHY_STATE_FAULTED;
    return Status;
  }

  ResetCore(Core);
  return MtkUsbPhySuccess;
}

uint8_t
MtkUsbPhyCoreIsInitialized(const MTK_USB_PHY_CORE *Core)
{
  return (uint8_t)(
    (Core != NULL) &&
    (Core->Signature == CORE_SIGNATURE) &&
    ((Core->State == MTK_USB_TPHY_STATE_INITIALIZED) ||
     (Core->State == MTK_USB_TPHY_STATE_POWERED))
    );
}

uint8_t
MtkUsbPhyCoreIsPowered(const MTK_USB_PHY_CORE *Core)
{
  return (uint8_t)(
    (Core != NULL) &&
    (Core->Signature == CORE_SIGNATURE) &&
    (Core->State == MTK_USB_TPHY_STATE_POWERED)
    );
}
