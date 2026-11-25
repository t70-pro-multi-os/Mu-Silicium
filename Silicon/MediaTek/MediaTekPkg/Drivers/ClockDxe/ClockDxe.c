#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

#include <Library/ClockImplLib.h>

#include <Protocol/MtkClock.h>

#define BIT(x) (1U << (x))
#define GENMASK(h, l) (((~0ULL) - ((1ULL << (l)) - 1)) & (~0ULL >> (63 - (h))))

#define REG_CON0 0

#define CON0_PWR_ON BIT(0)
#define CON0_ISO_EN BIT(1)
#define CON1_PCW_CHG BIT(31)

#define POSTDIV_MASK GENMASK(2, 0)

STATIC EFI_MEMORY_REGION_DESCRIPTOR mClockControllersRegions[ClkMax];

STATIC BOOLEAN ClockControllerRequired (IN MTK_CLOCK_CONTROLLER_TYPE Controller) {
  for (UINTN Index = 0; Index < gClockCount; Index++) {
    CONST MTK_CLOCK_DESC *Clock = &gClocks[Index];
    if (Clock->Controller == Controller &&
        (Clock->Type == ClockTypePll || Clock->Type == ClockTypeMux ||
         Clock->Type == ClockTypeMuxGate || Clock->Type == ClockTypeGate)) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC EFI_STATUS LocateOptionalClockControllerRegion (
  IN CONST CHAR8                 *Name,
  IN MTK_CLOCK_CONTROLLER_TYPE   Controller
  )
{
  if (Controller >= ClkMax) {
    return EFI_INVALID_PARAMETER;
  }

  if (!ClockControllerRequired (Controller)) {
    // Some MediaTek SoCs share this driver without exposing these wrappers.
    mClockControllersRegions[Controller].Address = 0;
    mClockControllersRegions[Controller].Length  = 0;
    return EFI_SUCCESS;
  }

  return LocateMemoryRegionByName ((CHAR8 *)Name, &mClockControllersRegions[Controller]);
}

STATIC UINT32 PllGateOffset (CONST MTK_PLL_DESC *Pll) {
  return Pll->EnableMask ? Pll->EnableOffset : Pll->BaseOffset + REG_CON0;
}
STATIC UINT32 PllGateMask (CONST MTK_PLL_DESC *Pll) {
  return Pll->EnableMask ? Pll->EnableMask : Pll->EnMask;
}

EFI_STATUS
ClockGetFrequency(
  IN  UINT32 ClockId,
  OUT UINT64 *Frequency);

EFI_STATUS
ClockGetParent(
  IN  UINT32 ClockId,
  OUT UINT32 *ParentId);

EFI_STATUS
ClockIsEnabled (IN UINT32 ClockId, OUT BOOLEAN *Enabled);

STATIC
UINT32
ClockRead(
  IN MTK_CLOCK_CONTROLLER_TYPE Controller,
  IN UINT32 Reg)
{
  if ((UINTN)Controller >= ClkMax) {
    DEBUG ((EFI_D_ERROR, "Invalid clock controller %d\n", Controller));
    ASSERT_EFI_ERROR (EFI_INVALID_PARAMETER);
    return 0;
  }

  return MmioRead32 (mClockControllersRegions[Controller].Address + Reg);
}

STATIC
VOID
ClockWrite(
  IN MTK_CLOCK_CONTROLLER_TYPE Controller,
  IN UINT32 Reg,
  IN UINT32 Value)
{
  if ((UINTN)Controller >= ClkMax) {
    DEBUG ((EFI_D_ERROR, "Invalid clock controller %d\n", Controller));
    ASSERT_EFI_ERROR (EFI_INVALID_PARAMETER);
    return;
  }

  MmioWrite32 (mClockControllersRegions[Controller].Address + Reg, Value);
}

STATIC
CONST
MTK_CLOCK_DESC *
ClockGetByName(
  IN CONST CHAR8 *ClockName)
{
  if (ClockName == NULL) {
    return NULL;
  }

  for (UINTN Index = 0; Index < gClockCount; Index++) {
    if (AsciiStrCmp(gClocks[Index].Name, ClockName) == 0) {
      return &gClocks[Index];
    }
  }

  return NULL;
}

STATIC
CONST
MTK_CLOCK_DESC *
ClockGetById(
  IN CONST UINT32 ClockId)
{
  for (UINTN Index = 0; Index < gClockCount; Index++) {
    if (gClocks[Index].Id == ClockId) {
      return &gClocks[Index];
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
PllCalcValues(
  IN CONST MTK_CLOCK_DESC* Clock,
  IN UINT64 Frequency,
  OUT UINT32 *PcwResult,
  OUT UINT32 *PostDivResult
  )
{
  EFI_STATUS Status;
  UINT64 ParentFrequency;
  UINT64 Vco, Pcw;
  UINT32 PostDiv, FractionBits;

  // Check if parameters are valid
  if (Clock == NULL || PcwResult == NULL || PostDivResult == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // Check if Clock is PLL
  if (Clock->Type != ClockTypePll) {
    return EFI_INVALID_PARAMETER;
  }

  // If Frequency is bigger than FMax, set it to FMax
  if (Frequency > Clock->Pll.FMax) {
    Frequency = Clock->Pll.FMax;
  }

  // The PLL has a maximum post-divider of 16 and a minimum VCO frequency.
  if (Frequency < (Clock->Pll.FMin + 15) / 16) {
    return EFI_UNSUPPORTED;
  }
  for (PostDiv = 1; Frequency * PostDiv < Clock->Pll.FMin; PostDiv <<= 1) {}
  Vco = Frequency * PostDiv;

  // Get Parent Clock Frequency
  Status = ClockGetFrequency(Clock->Pll.Parent, &ParentFrequency);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ParentFrequency == 0) {
    return EFI_NOT_READY;
  }
  FractionBits = Clock->Pll.PcwBits - Clock->Pll.PcwiBits;
  if (Vco > (MAX_UINT64 >> FractionBits)) {
    return EFI_UNSUPPORTED;
  }
  Pcw = (Vco << FractionBits) / ParentFrequency;
  if (Pcw > GENMASK(Clock->Pll.PcwBits - 1, 0)) {
    return EFI_UNSUPPORTED;
  }

  // Return Pcw and PostDiv
  *PcwResult = Pcw;
  *PostDivResult = PostDiv;
  return EFI_SUCCESS;
}

EFI_STATUS
ClockGetId(
  IN  CONST CHAR8 *ClockName,
  OUT UINT32 *ClockId)
{
  CONST MTK_CLOCK_DESC *Clock = ClockGetByName(ClockName);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock name \"%a\" is provided\n", ClockName));
    return EFI_INVALID_PARAMETER;
  }

  if (ClockId == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *ClockId = Clock->Id;
  return EFI_SUCCESS;
}

EFI_STATUS
ClockGetName(
  IN  UINT32 ClockId,
  OUT CONST CHAR8 **ClockName)
{
  CONST MTK_CLOCK_DESC *Clock = ClockGetById(ClockId);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock id %u is provided\n", ClockId));
    return EFI_INVALID_PARAMETER;
  }

  if (ClockName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *ClockName = Clock->Name;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ClockSetEnableLocked(
  IN UINT32 ClockId,
  IN BOOLEAN Enable)
{
  EFI_STATUS Status;
  UINT32 Value;
  BOOLEAN Enabled;

  // Find Clock by Id
  CONST MTK_CLOCK_DESC *Clock = ClockGetById(ClockId);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock id %u is provided\n", ClockId));
    return EFI_INVALID_PARAMETER;
  }

  switch (Clock->Type){
  case ClockTypeFixed:
    // Fixed Clock is always enabled
    return EFI_SUCCESS;
  case ClockTypePll:
    // Boot firmware may already be using this PLL for another device.
    Status = ClockIsEnabled (ClockId, &Enabled);
    if (EFI_ERROR (Status)) { return Status; }
    if (Enabled == Enable) { return EFI_SUCCESS; }
    if (Enable) {
      Status = ClockSetEnableLocked (Clock->Pll.Parent, TRUE);
      if (EFI_ERROR (Status)) { return Status; }
    }

    if (Enable) {
      // Enable PLL Power
      Value = ClockRead(Clock->Controller, Clock->Pll.PowerOffset);
      Value |= CON0_PWR_ON;
      ClockWrite(Clock->Controller, Clock->Pll.PowerOffset, Value);
      MicroSecondDelay(1);

      // Clear PLL ISO
      Value = ClockRead(Clock->Controller, Clock->Pll.PowerOffset);
      Value &= ~CON0_ISO_EN;
      ClockWrite(Clock->Controller, Clock->Pll.PowerOffset, Value);
      MicroSecondDelay(1);

      // The shipped MT6789 handler sets optional divided outputs first.
      if (Clock->Pll.EnableMask && Clock->Pll.EnMask) {
        Value = ClockRead(Clock->Controller, PllGateOffset(&Clock->Pll));
        Value |= Clock->Pll.EnMask;
        ClockWrite(Clock->Controller, PllGateOffset(&Clock->Pll), Value);
      }
      Value = ClockRead(Clock->Controller, PllGateOffset(&Clock->Pll));
      Value |= PllGateMask(&Clock->Pll);
      ClockWrite(Clock->Controller, PllGateOffset(&Clock->Pll), Value);
      if (Clock->Pll.TunerEnableMask) {
        Value = ClockRead(Clock->Controller, Clock->Pll.TunerEnableOffset);
        Value |= Clock->Pll.TunerEnableMask;
        ClockWrite(Clock->Controller, Clock->Pll.TunerEnableOffset, Value);
      }
      MicroSecondDelay(20);

      if (Clock->Pll.ResetBarMask) {
        // If pll have reset bar mask write it
        Value = ClockRead(Clock->Controller, Clock->Pll.BaseOffset + REG_CON0);
        Value |= Clock->Pll.ResetBarMask;
        ClockWrite(Clock->Controller, Clock->Pll.BaseOffset + REG_CON0, Value);
      }
    } else {
      if (Clock->Pll.ResetBarMask) {
        // If pll have reset bar mask clear it
        Value = ClockRead(Clock->Controller, Clock->Pll.BaseOffset + REG_CON0);
        Value &= ~Clock->Pll.ResetBarMask;
        ClockWrite(Clock->Controller, Clock->Pll.BaseOffset + REG_CON0, Value);
      }

      if (Clock->Pll.TunerEnableMask) {
        Value = ClockRead(Clock->Controller, Clock->Pll.TunerEnableOffset);
        Value &= ~Clock->Pll.TunerEnableMask;
        ClockWrite(Clock->Controller, Clock->Pll.TunerEnableOffset, Value);
      }
      // The shipped handler clears the gate, then optional divided outputs
      // in a second RMW. USB CON0 bit0 is PCW and is never a gate here.
      Value = ClockRead(Clock->Controller, PllGateOffset(&Clock->Pll));
      Value &= ~PllGateMask(&Clock->Pll);
      ClockWrite(Clock->Controller, PllGateOffset(&Clock->Pll), Value);
      if (Clock->Pll.EnableMask && Clock->Pll.EnMask) {
        Value = ClockRead(Clock->Controller, PllGateOffset(&Clock->Pll));
        Value &= ~Clock->Pll.EnMask;
        ClockWrite(Clock->Controller, PllGateOffset(&Clock->Pll), Value);
      }

      // Enable PLL ISO
      Value = ClockRead(Clock->Controller, Clock->Pll.PowerOffset);
      Value |= CON0_ISO_EN;
      ClockWrite(Clock->Controller, Clock->Pll.PowerOffset, Value);

      // Disable PLL Power
      Value = ClockRead(Clock->Controller, Clock->Pll.PowerOffset);
      Value &= ~CON0_PWR_ON;
      ClockWrite(Clock->Controller, Clock->Pll.PowerOffset, Value);
    }
    return EFI_SUCCESS;
  case ClockTypeMux:
  case ClockTypeMuxGate:
    if (!Enable && (Clock->Type == ClockTypeMux)) { return EFI_UNSUPPORTED; }
    // Get Parent Clock
    Status = ClockGetParent(Clock->Id, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Parents are shared. Disabling this gate must not disable its parent.
    if (Enable) {
      Status = ClockSetEnableLocked (Value, TRUE);
      if (EFI_ERROR (Status)) { return Status; }
    }

    // If Clock is MuxGate enable or disable it
    if (Clock->Type == ClockTypeMuxGate) {
      ClockWrite(Clock->Controller, Enable ? Clock->MuxGate.ClearOffset : Clock->MuxGate.SetOffset, BIT(Clock->MuxGate.GateShift));
    }
    return EFI_SUCCESS;
  case ClockTypeFactors:
    // Enable Parent Clock
    return Enable ? ClockSetEnableLocked (Clock->Factor.Parent, TRUE) : EFI_UNSUPPORTED;
  case ClockTypeGate:
    // Enable Parent Clock
    if (Enable) {
      Status = ClockSetEnableLocked (Clock->Gate.Parent, TRUE);
      if (EFI_ERROR (Status)) { return Status; }
    }

    // Invert enable  if gate is inverted
    if (Clock->Gate.Inverted) {
      Enable = !Enable;
    }

    // If SetOffset, ClearOffset and StatusOffset are same there is no set and clear registers only status register
    if (Clock->Gate.SetOffset == Clock->Gate.ClearOffset && Clock->Gate.StatusOffset == Clock->Gate.ClearOffset) {
      // Read status register
      Value = ClockRead(Clock->Controller, Clock->Gate.StatusOffset);

      if (!Enable) {
        // Gate status bits are active low (inversion was applied above).
        // Set disable bit
        Value |= BIT(Clock->Gate.GateShift);
      } else {
        // Clear enable bit
        Value &= ~BIT(Clock->Gate.GateShift);
      }

      // Write status register
      ClockWrite(Clock->Controller, Clock->Gate.StatusOffset, Value);
      return EFI_SUCCESS;
    }

    // Write set or clear register to enable or disable clock
    ClockWrite(Clock->Controller, Enable ? Clock->Gate.ClearOffset : Clock->Gate.SetOffset, BIT(Clock->Gate.GateShift));
    return EFI_SUCCESS;
  default:
    DEBUG ((EFI_D_ERROR, "Invalid clock type %d\n", Clock->Type));
    return EFI_INVALID_PARAMETER;
  }
}

EFI_STATUS
ClockSetEnable (IN UINT32 ClockId, IN BOOLEAN Enable)
{
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = ClockSetEnableLocked (ClockId, Enable);
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
ClockIsEnabled(
  IN  UINT32 ClockId,
  OUT BOOLEAN *Enabled)
{
  UINT32 Value;

  // Find Clock by Id
  CONST MTK_CLOCK_DESC *Clock = ClockGetById(ClockId);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock id %u is provided\n", ClockId));
    return EFI_INVALID_PARAMETER;
  }

  // Check if Enabled is not NULL
  if (Enabled == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Clock->Type){
  case ClockTypeMux:
  case ClockTypeFixed:
    // Mux And Fixed is always enabled
    *Enabled = TRUE;
    return EFI_SUCCESS;
  case ClockTypePll:
    // Match the descriptor's actual gate, not an unrelated PCW bit.
    if (PllGateMask(&Clock->Pll) == 0) { return EFI_UNSUPPORTED; }
    Value = ClockRead(Clock->Controller, PllGateOffset(&Clock->Pll));
    *Enabled = (Value & PllGateMask(&Clock->Pll)) != 0;
    return EFI_SUCCESS;
  case ClockTypeMuxGate:
    // Read Mux Gate Enable Status
    Value = ClockRead(Clock->Controller, Clock->MuxGate.MuxOffset);
    *Enabled = (Value & BIT(Clock->MuxGate.GateShift)) == 0;
    return EFI_SUCCESS;
  case ClockTypeFactors:
    // Get Parent Clock Enable Status
    return ClockIsEnabled(Clock->Factor.Parent, Enabled);
  case ClockTypeGate:
    // Read Gate Enable Status
    Value = ClockRead(Clock->Controller, Clock->Gate.StatusOffset);
    *Enabled = (Value & BIT(Clock->Gate.GateShift)) == 0;
    if (Clock->Gate.Inverted) {
      *Enabled = !*Enabled;
    }
    return EFI_SUCCESS;
  default:
    DEBUG ((EFI_D_ERROR, "Invalid clock type %d\n", Clock->Type));
    return EFI_INVALID_PARAMETER;
  }
}

EFI_STATUS
ClockGetFrequency(
  IN  UINT32 ClockId,
  OUT UINT64 *Frequency)
{
  EFI_STATUS Status;

  // Find Clock by Id
  CONST MTK_CLOCK_DESC *Clock = ClockGetById(ClockId);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock id %u is provided\n", ClockId));
    return EFI_INVALID_PARAMETER;
  }

  // Check if Frequency is not NULL
  if (Frequency == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Clock->Type){
  case ClockTypeFixed:
    // Return Fixed Frequency
    *Frequency = Clock->Fixed;
    return EFI_SUCCESS;
  case ClockTypePll: {
    // Read PostDiv and Pcw values
    UINT32 PostDiv = ClockRead(Clock->Controller, Clock->Pll.PostDivOffset);
    UINT32 Pcw = ClockRead(Clock->Controller, Clock->Pll.PcwOffset);
    UINT32 PcwfBits;
    UINT64 Vco;

    // Get PostDiv and Pcw values
    PostDiv = BIT((PostDiv >> Clock->Pll.PostDivShift) & POSTDIV_MASK);
    Pcw = (Pcw >> Clock->Pll.PcwShift) & GENMASK(Clock->Pll.PcwBits - 1, 0);
    PcwfBits = Clock->Pll.PcwBits - Clock->Pll.PcwiBits;

    // Get parent frequency
    Status = ClockGetFrequency(Clock->Pll.Parent, &Vco);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Calculate Vco frequency
    Vco *= Pcw;
    Vco >>= PcwfBits;

    // Calculate output frequency
    *Frequency = (Vco + PostDiv - 1) / PostDiv;
    *Frequency = ((*Frequency + 50000) / 100000) * 100000;
    return EFI_SUCCESS;
  }
  case ClockTypeMux:
  case ClockTypeMuxGate: {
    UINT32 Parent;

    // Get current parent
    Status = ClockGetParent(Clock->Id, &Parent);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Get parent frequency
    Status = ClockGetFrequency(Parent, Frequency);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    return EFI_SUCCESS;
  }
  case ClockTypeFactors:
    // Get parent frequency
    Status = ClockGetFrequency(Clock->Factor.Parent, Frequency);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Calculate output frequency
    *Frequency *= Clock->Factor.Mult;
    *Frequency /= Clock->Factor.Div;
    return EFI_SUCCESS;
  case ClockTypeGate:
    // Get parent frequency
    return ClockGetFrequency(Clock->Gate.Parent, Frequency);
  default:
    DEBUG ((EFI_D_ERROR, "Invalid clock type %d\n", Clock->Type));
    return EFI_INVALID_PARAMETER;
  }
}

EFI_STATUS
ClockSetFrequency(
  IN UINT32 ClockId,
  IN UINT64 Frequency)
{
  EFI_STATUS Status;
  UINT32 Value;
  UINT32 PostDiv;
  UINT32 Pcw, PcwMask;
  UINT32 PcwChgOffset;
  EFI_TPL OldTpl;

  // Find Clock by Id
  CONST MTK_CLOCK_DESC *Clock = ClockGetById(ClockId);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock id %u is provided\n", ClockId));
    return EFI_INVALID_PARAMETER;
  }

  // Check if Clock is PLL
  if (Clock->Type != ClockTypePll) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  // Calculate PLL values
  Status = PllCalcValues(Clock, Frequency, &Pcw, &PostDiv);
  if (EFI_ERROR (Status)) {
    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  PcwChgOffset = Clock->Pll.PcwChgOffset ? Clock->Pll.PcwChgOffset : Clock->Pll.PostDivOffset;

  // Update PostDiv
  Value = ClockRead(Clock->Controller, Clock->Pll.PostDivOffset);
  Value &= ~(POSTDIV_MASK << Clock->Pll.PostDivShift);
  if (PcwChgOffset == Clock->Pll.PostDivOffset) {
    Value &= ~CON1_PCW_CHG;
  }
  Value |= LowBitSet32(PostDiv) << Clock->Pll.PostDivShift;

  // If PostDiv offset matches Pcw offset it should be written only after PostDiv and Pcw bits are updated
  if (Clock->Pll.PostDivOffset != Clock->Pll.PcwOffset) {
    ClockWrite(Clock->Controller, Clock->Pll.PostDivOffset, Value);
    Value = ClockRead(Clock->Controller, Clock->Pll.PcwOffset);
  }

  // Update Pcw
  PcwMask = (UINT32)GENMASK(Clock->Pll.PcwBits - 1, 0);
  Value &= ~(PcwMask << Clock->Pll.PcwShift);
  Value |= (Pcw & PcwMask) << Clock->Pll.PcwShift;
  ClockWrite(Clock->Controller, Clock->Pll.PcwOffset, Value);

  Value = ClockRead(Clock->Controller, PcwChgOffset);
  ClockWrite(Clock->Controller, PcwChgOffset, Value | CON1_PCW_CHG);
  MicroSecondDelay(20);
  gBS->RestoreTPL (OldTpl);

  return EFI_SUCCESS;
}

EFI_STATUS
ClockGetParent(
  IN  UINT32 ClockId,
  OUT UINT32 *ParentId)
{
  UINT32 Value;

  // Find Clock by Id
  CONST MTK_CLOCK_DESC *Clock = ClockGetById(ClockId);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock id %u is provided\n", ClockId));
    return EFI_INVALID_PARAMETER;
  }

  // Check if Clock is MuxGate or Mux
  if (Clock->Type != ClockTypeMuxGate && Clock->Type != ClockTypeMux) {
    return EFI_UNSUPPORTED;
  }

  // Check if parent is not NULL
  if (ParentId == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // Read Current Mux Value
  Value = ClockRead(Clock->Controller, Clock->MuxGate.MuxOffset);
  Value = (Value >> Clock->MuxGate.MuxShift) & GENMASK(Clock->MuxGate.MuxWidth - 1, 0);

  // Check is Mux Value is valid
  if (Value >= Clock->MuxGate.ParentCount) {
    return EFI_DEVICE_ERROR;
  }

  // Return parent
  *ParentId = Clock->MuxGate.Parents[Value];
  return EFI_SUCCESS;
}

EFI_STATUS
ClockSetParent(
  IN UINT32 ClockId,
  IN UINT32 ParentId)
{
  UINT32 Value;
  UINT32 Orig;
  UINTN Index;
  UINT32 Mask;
  EFI_TPL OldTpl;

  // Find Clock by Id
  CONST MTK_CLOCK_DESC *Clock = ClockGetById(ClockId);
  if (Clock == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid clock id %u is provided\n", ClockId));
    return EFI_INVALID_PARAMETER;
  }

  // Check if Clock is MuxGate or Mux
  if (Clock->Type != ClockTypeMuxGate && Clock->Type != ClockTypeMux) {
    return EFI_UNSUPPORTED;
  }

  if ((Clock->MuxGate.MuxWidth == 0) ||
      (Clock->MuxGate.MuxShift >= 32) ||
      (Clock->MuxGate.MuxWidth > 32 - Clock->MuxGate.MuxShift) ||
      ((Clock->MuxGate.UpdateOffset != MAX_UINT32) &&
       (Clock->MuxGate.UpdateShift >= 32))) {
    return EFI_INVALID_PARAMETER;
  }

  // Generate mask
  Mask = GENMASK(Clock->MuxGate.MuxWidth - 1, 0);

  // Find specified parent
  for (Index = 0; Index < Clock->MuxGate.ParentCount; Index++) {
    if (ParentId == Clock->MuxGate.Parents[Index]) {
      break;
    }
  }

  // Check if parent is found
  if (Index == Clock->MuxGate.ParentCount) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  // Read Current Mux Value
  Orig = ClockRead(Clock->Controller, Clock->MuxGate.MuxOffset);

  // Update Mux Value
  Value = (Orig & ~(Mask << Clock->MuxGate.MuxShift)) | (Index << Clock->MuxGate.MuxShift);

  // Check if the value is changed
  if (Orig != Value) {
    // Write new Mux Value
    ClockWrite(Clock->Controller, Clock->MuxGate.ClearOffset, Mask << Clock->MuxGate.MuxShift);
    ClockWrite(Clock->Controller, Clock->MuxGate.SetOffset, Index << Clock->MuxGate.MuxShift);

    // Check if clock has update offset if so write it
    if (Clock->MuxGate.UpdateOffset != MAX_UINT32) {
      ClockWrite(Clock->Controller, Clock->MuxGate.UpdateOffset, BIT(Clock->MuxGate.UpdateShift));
    }
  }

  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC MTK_CLOCK_PROTOCOL mClock = {
  ClockGetId,
  ClockGetName,
  ClockSetEnable,
  ClockIsEnabled,
  ClockGetFrequency,
  ClockSetFrequency,
  ClockGetParent,
  ClockSetParent,
};

EFI_STATUS
EFIAPI
InitClockDriver (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  // Locate AP Mixed Memory Region
  Status = LocateMemoryRegionByName ("AP Mixed", &mClockControllersRegions[ClkApMixed]);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate AP Mixed Memory Region! Status = %r\n", Status));
    return Status;
  }

  // Locate Top Ck Gen Memory Region
  Status = LocateMemoryRegionByName ("Top Ck Gen", &mClockControllersRegions[ClkTopCkGen]);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate Top Ck Gen Memory Region! Status = %r\n", Status));
    return Status;
  }

  // Locate Infra Cfg AO Memory Region
  Status = LocateMemoryRegionByName ("Infra Cfg AO", &mClockControllersRegions[ClkInfraCfgAo]);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate Infra Cfg AO Memory Region! Status = %r\n", Status));
    return Status;
  }

  Status = LocateOptionalClockControllerRegion ("I2C Wrap W", ClkImpIicWrapW);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate I2C Wrap W Memory Region! Status = %r\n", Status));
    return Status;
  }

  Status = LocateOptionalClockControllerRegion ("I2C Wrap C", ClkImpIicWrapC);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate I2C Wrap C Memory Region! Status = %r\n", Status));
    return Status;
  }

  // Register Clock Protocol
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gMediaTekClockProtocolGuid, &mClock, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register Clock Protocol! Status = %r\n", Status));
    return Status;
  }

  return Status;
}
