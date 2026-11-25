#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/GpioImplLib.h>

#define GPIO_PIN_OFFSET(Pin)       (((Pin) / 32U) * 0x10U)
#define GPIO_MODE_OFFSET(Pin)      (0x300U + (((Pin) / 8U) * 0x10U))
#define GPIO_MODE_SHIFT(Pin)       (((Pin) % 8U) * 4U)

#define MSDC1_FIRST_PIN            71U
#define MSDC1_LAST_PIN             76U
#define MSDC1_DATA_CMD_IES_MASK    0x0000007CU
#define MSDC1_DRIVE_MASK           0x07FFFE00U
#define MSDC1_BIAS_MASK            0x0000003FU
#define MSDC1_INPUT_DIR_MASK       0x00001F00U

typedef struct {
  UINTN  Address;
  UINT32 Mask;
  UINT32 Original;
} MSDC1_PAD_REGISTER;

STATIC
UINT32
ReadMaskedRegister (
  IN UINTN Address
  )
{
  return MmioRead32 (Address);
}

STATIC
EFI_STATUS
WriteMaskedRegister (
  IN UINTN  Address,
  IN UINT32 Mask,
  IN UINT32 Value
  )
{
  UINT32 Current;
  UINT32 Updated;

  Current = ReadMaskedRegister (Address);
  Updated = (Current & ~Mask) | (Value & Mask);
  MmioWrite32 (Address, Updated);
  return ((ReadMaskedRegister (Address) & Mask) == (Value & Mask))
           ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
WriteMuxGroup (
  IN UINTN   PinctrlBase,
  IN UINT32  ModeOffset,
  IN UINT32  WriteMaskOffset,
  IN UINT32  MuxValue
  )
{
  UINT32 Pin;
  UINT32 WriteValue;
  UINT32 ExpectedMask;
  UINT32 ExpectedValue;
  UINT32 Current;

  WriteValue = 0;
  ExpectedMask = 0;
  ExpectedValue = 0;
  for (Pin = MSDC1_FIRST_PIN; Pin <= MSDC1_LAST_PIN; ++Pin) {
    if (GPIO_MODE_OFFSET (Pin) != ModeOffset) {
      continue;
    }

    WriteValue |= (8U | MuxValue) << GPIO_MODE_SHIFT (Pin);
    ExpectedMask |= 7U << GPIO_MODE_SHIFT (Pin);
    ExpectedValue |= MuxValue << GPIO_MODE_SHIFT (Pin);
  }

  MmioWrite32 (PinctrlBase + ModeOffset + WriteMaskOffset, WriteValue);
  Current = MmioRead32 (PinctrlBase + ModeOffset);
  return ((Current & ExpectedMask) == ExpectedValue)
           ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
RestoreMsdc1Pads (
  IN  UINTN                  PinctrlBase,
  IN  CONST MTK_GPIO_PLATFORM_INFO *PlatformInfo,
  IN  MSDC1_PAD_REGISTER    *Registers,
  IN  UINTN                  RegisterCount,
  IN  UINT32                 OriginalDirection,
  IN  UINT32                 OriginalMode71,
  IN  UINT32                 OriginalMode72
  )
{
  UINTN  Index;
  UINT32 Current;
  UINT32 DirectionMask;
  UINT32 SetBits;
  UINT32 ClearBits;
  UINT32 MuxValue;
  UINT32 MuxMask;
  UINT32 Pin;
  UINTN  ModeOffset;
  UINT32 OriginalMode;
  BOOLEAN Failed;

  Failed = FALSE;
  // Disconnect any partially committed MSDC1 mux first, then restore GPIO
  // direction and pad fields while the caller still owns all six pins.
  for (ModeOffset = 0x380U; ModeOffset <= 0x390U; ModeOffset += 0x10U) {
    OriginalMode = (ModeOffset == 0x380U) ? OriginalMode71 : OriginalMode72;
    MuxValue = 0;
    MuxMask = 0;
    for (Pin = MSDC1_FIRST_PIN; Pin <= MSDC1_LAST_PIN; ++Pin) {
      if (GPIO_MODE_OFFSET (Pin) == ModeOffset) {
        UINT32 Shift = GPIO_MODE_SHIFT (Pin);
        MuxValue |= (8U | ((OriginalMode >> Shift) & 7U)) << Shift;
        MuxMask |= 7U << Shift;
      }
    }
    if (MuxMask != 0) {
      MmioWrite32 (PinctrlBase + ModeOffset + PlatformInfo->ModeMaskWriteOffset,
                   MuxValue);
      if ((MmioRead32 (PinctrlBase + ModeOffset) & MuxMask) !=
          (OriginalMode & MuxMask)) {
        Failed = TRUE;
      }
    }
  }

  DirectionMask = MSDC1_INPUT_DIR_MASK;
  Current = MmioRead32 (PinctrlBase + PlatformInfo->DirOffset + GPIO_PIN_OFFSET (72));
  SetBits = OriginalDirection & DirectionMask & ~Current;
  ClearBits = Current & DirectionMask & ~OriginalDirection;
  if (SetBits != 0) {
    MmioWrite32 (PinctrlBase + PlatformInfo->DirOffset + GPIO_PIN_OFFSET (72) +
                 PlatformInfo->SetOffset, SetBits);
  }
  if (ClearBits != 0) {
    MmioWrite32 (PinctrlBase + PlatformInfo->DirOffset + GPIO_PIN_OFFSET (72) +
                 PlatformInfo->ResetOffset, ClearBits);
  }
  Current = MmioRead32 (PinctrlBase + PlatformInfo->DirOffset + GPIO_PIN_OFFSET (72));
  if ((Current & DirectionMask) != (OriginalDirection & DirectionMask)) {
    Failed = TRUE;
  }

  for (Index = 0; Index < RegisterCount; ++Index) {
    Current = MmioRead32 (Registers[Index].Address);
    MmioWrite32 (Registers[Index].Address,
                 (Current & ~Registers[Index].Mask) |
                 (Registers[Index].Original & Registers[Index].Mask));
    if ((MmioRead32 (Registers[Index].Address) & Registers[Index].Mask) !=
        (Registers[Index].Original & Registers[Index].Mask)) {
      Failed = TRUE;
    }
  }

  return Failed ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ConfigureMsdc1Pads (
  IN  UINTN                     PinctrlBase,
  IN  UINTN                     IoCfgRtBase,
  IN  CONST MTK_GPIO_PLATFORM_INFO *PlatformInfo,
  OUT BOOLEAN                  *CanReleaseLease
  )
{
  MSDC1_PAD_REGISTER Registers[5];
  UINTN RegisterCount;
  UINT32 OriginalDirection;
  UINT32 OriginalMode71;
  UINT32 OriginalMode72;
  EFI_STATUS Status;

  if ((PlatformInfo == NULL) || (CanReleaseLease == NULL) ||
      (PinctrlBase == 0) || (IoCfgRtBase == 0) ||
      (PlatformInfo->ModeMaskWriteOffset == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *CanReleaseLease = TRUE;
  RegisterCount = 5;
  Registers[0].Address = IoCfgRtBase + 0x0080U; // IES, pins 72-76 only.
  Registers[0].Mask = MSDC1_DATA_CMD_IES_MASK;
  Registers[1].Address = IoCfgRtBase + 0x0030U; // DRV, pins 71-76.
  Registers[1].Mask = MSDC1_DRIVE_MASK;
  Registers[2].Address = IoCfgRtBase + 0x00B0U; // PUPD, pins 71-76.
  Registers[2].Mask = MSDC1_BIAS_MASK;
  Registers[3].Address = IoCfgRtBase + 0x00E0U; // R0, pins 71-76.
  Registers[3].Mask = MSDC1_BIAS_MASK;
  Registers[4].Address = IoCfgRtBase + 0x00F0U; // R1, pins 71-76.
  Registers[4].Mask = MSDC1_BIAS_MASK;

  // Snapshot every touched register before the first write. RMW masks preserve
  // GPIO neighbors that share the same IOCFG register words.
  for (UINTN Index = 0; Index < RegisterCount; ++Index) {
    Registers[Index].Original = MmioRead32 (Registers[Index].Address);
  }
  OriginalDirection = MmioRead32 (PinctrlBase + PlatformInfo->DirOffset +
                                  GPIO_PIN_OFFSET (72));
  OriginalMode71 = MmioRead32 (PinctrlBase + GPIO_MODE_OFFSET (71));
  OriginalMode72 = MmioRead32 (PinctrlBase + GPIO_MODE_OFFSET (72));

  // Match the stock mt6789-pinctrl.dtsi: input-enable for CMD/DAT only,
  // GPIO direction input before mux, raw drive code 3 on all six pins, then
  // PUPD/R0/R1 values for the five pull-ups and the CLK pull-down.
  Status = WriteMaskedRegister (Registers[0].Address, Registers[0].Mask, 0x0000007CU);
  if (EFI_ERROR (Status)) { goto Rollback; }

  MmioWrite32 (PinctrlBase + PlatformInfo->DirOffset + GPIO_PIN_OFFSET (72) +
               PlatformInfo->ResetOffset, MSDC1_INPUT_DIR_MASK);
  if ((MmioRead32 (PinctrlBase + PlatformInfo->DirOffset + GPIO_PIN_OFFSET (72)) &
       MSDC1_INPUT_DIR_MASK) != 0) {
    Status = EFI_DEVICE_ERROR;
    goto Rollback;
  }

  Status = WriteMaskedRegister (Registers[1].Address, Registers[1].Mask,
                                0x036DB600U);
  if (EFI_ERROR (Status)) { goto Rollback; }
  Status = WriteMaskedRegister (Registers[2].Address, Registers[2].Mask, 0x00000001U);
  if (EFI_ERROR (Status)) { goto Rollback; }
  Status = WriteMaskedRegister (Registers[3].Address, Registers[3].Mask, 0x0000003EU);
  if (EFI_ERROR (Status)) { goto Rollback; }
  Status = WriteMaskedRegister (Registers[4].Address, Registers[4].Mask, 0x00000001U);
  if (EFI_ERROR (Status)) { goto Rollback; }

  // Mode 1 is committed last, after the electrical pad configuration passes
  // readback, so no partially prepared pin group becomes active as MSDC1.
  Status = WriteMuxGroup (PinctrlBase, GPIO_MODE_OFFSET (71),
                          PlatformInfo->ModeMaskWriteOffset, 1U);
  if (EFI_ERROR (Status)) { goto Rollback; }
  Status = WriteMuxGroup (PinctrlBase, GPIO_MODE_OFFSET (72),
                          PlatformInfo->ModeMaskWriteOffset, 1U);
  if (EFI_ERROR (Status)) { goto Rollback; }

  return EFI_SUCCESS;

Rollback:
  if (EFI_ERROR (RestoreMsdc1Pads (PinctrlBase, PlatformInfo, Registers,
                                   RegisterCount, OriginalDirection,
                                   OriginalMode71, OriginalMode72))) {
    *CanReleaseLease = FALSE;
  }
  return EFI_DEVICE_ERROR;
}

MTK_GPIO_PLATFORM_INFO gPlatformInfo = {
  .SetOffset     = 0x4,
  .ResetOffset   = 0x8,
  .DirOffset     = 0x0,
  .DataOutOffset = 0x100,
  .DataInOffset  = 0x200,
  .ModeOffset    = 0x300,
  .MaxPin        = 187,
  .ModeMaskWriteOffset = 0xc,
  .ConfigureMsdc1Pads = ConfigureMsdc1Pads,
};
