#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/MsdcImplLib.h>

#include <Protocol/MtkGpio.h>
#include <Protocol/MtkClock.h>
#include <Protocol/MtkPmic.h>

MSDC_PLATFORM_INFO gPlatformInfo = {
  .HostMask = FixedPcdGet32 (PcdMsdcHostMask),
  .ReadOnly = FixedPcdGetBool (PcdMsdcReadOnly),
  .MaxClockHz = FixedPcdGet32 (PcdMsdcMaxClockHz),
  .UseTop = TRUE,
  .MsdcPadTuneReg = 0xf0,
  .TuningStep = {32, 64},
  .AsyncFifo = TRUE,
  .BusyCheck = TRUE,
  .StopClkFix = TRUE,
  .EnhanceRx = TRUE
};

STATIC MTK_GPIO_PROTOCOL *mGpio = NULL;
STATIC MTK_CLOCK_PROTOCOL *mClock = NULL;
STATIC MTK_PMIC_PROTOCOL *mPmic = NULL;

EFI_STATUS
GetSourceClockRate (
  UINT32 Index,
  UINTN *Hz)
{
  if ((Index > 1) || ((gPlatformInfo.HostMask & (1U << Index)) == 0) ||
      ((Index == 0) && !FixedPcdGetBool (PcdStorageIsEMMC))) {
    return EFI_UNSUPPORTED;
  }

  EFI_STATUS Status;
  UINT32 ClockId;

  if (Hz == NULL) { return EFI_INVALID_PARAMETER; }

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "TOP_MSDC50_0" : "TOP_MSDC30_1", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    return Status;
  }

  // Get Clock Frequency
  Status = mClock->GetFrequency(ClockId, Hz);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Frequency! Status = %r\n", Status));
    return Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
SourceClockControl (
  UINT32 Index,
  BOOLEAN Enable)
{
  if ((Index > 1) || ((gPlatformInfo.HostMask & (1U << Index)) == 0) ||
      ((Index == 0) && !FixedPcdGetBool (PcdStorageIsEMMC))) {
    return EFI_UNSUPPORTED;
  }

  EFI_STATUS Status;
  UINT32 ClockId;

  if (Index == 0) {
    // Get Clock Id
    Status = mClock->GetId("INFRA_FAES_FDE", &ClockId);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
      return Status;
    }

    // Enable Clock
    Status = mClock->SetEnable(ClockId, Enable);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
      return Status;
    }
  }

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "INFRA_MSDC0_SRC" : "INFRA_MSDC1_SRC", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    return Status;
  }

  // Enable Clock
  Status = mClock->SetEnable(ClockId, Enable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
    return Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
ClockControl (
  UINT32 Index,
  BOOLEAN Enable)
{
  if ((Index > 1) || ((gPlatformInfo.HostMask & (1U << Index)) == 0) ||
      ((Index == 0) && !FixedPcdGetBool (PcdStorageIsEMMC))) {
    return EFI_UNSUPPORTED;
  }

  EFI_STATUS Status;
  UINT32 ClockId;

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "TOP_MSDC50_0" : "TOP_MSDC30_1", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    return Status;
  }

  // Enable Clock
  Status = mClock->SetEnable(ClockId, Enable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
    return Status;
  }

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "INFRA_MSDC0" : "INFRA_MSDC1", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    return Status;
  }

  // Enable Clock
  Status = mClock->SetEnable(ClockId, Enable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
    return Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
PowerControl (
  UINT32 Index,
  BOOLEAN Enable)
{
  if ((Index > 1) || ((gPlatformInfo.HostMask & (1U << Index)) == 0) ||
      ((Index == 0) && !FixedPcdGetBool (PcdStorageIsEMMC))) {
    return EFI_UNSUPPORTED;
  }

  EFI_STATUS Status;

  if (Index == 0 && FixedPcdGetBool(PcdStorageIsEMMC)) {
    // Enable VEMC LDO
    Status = mPmic->RegulatorSetEnable("ldo_vemc", Enable);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Enable LDO VEMC! Status = %r\n", Status));
      return Status;
    }
  } else {
    CONST CHAR8 *Vmch = FixedPcdGet32 (PcdSdVmchEint) == 1 ? "vmch_eint_low" :
                       FixedPcdGet32 (PcdSdVmchEint) == 2 ? "vmch_eint_high" : "ldo_vmch";
    BOOLEAN OldVmch, OldVmc;
    UINT32 OldVmchUv, OldVmcUv;
    EFI_STATUS Restore = EFI_SUCCESS;
    EFI_STATUS Next;
    if (FixedPcdGet32 (PcdSdVmchEint) > 2) { return EFI_INVALID_PARAMETER; }
    Status = mPmic->RegulatorIsEnabled (Vmch, &OldVmch);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mPmic->RegulatorIsEnabled ("ldo_vmc", &OldVmc);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mPmic->RegulatorGetVoltage (Vmch, &OldVmchUv);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mPmic->RegulatorGetVoltage ("ldo_vmc", &OldVmcUv);
    if (EFI_ERROR (Status)) { return Status; }
    // Never change the voltage of a supply inherited in the enabled state.
    if (Enable && ((OldVmch && OldVmchUv != 3300000) ||
                   (OldVmc && OldVmcUv != 3300000))) { return EFI_ACCESS_DENIED; }
    if (Enable) {
      Status = mPmic->RegulatorSetVoltage (Vmch, 3300000);
      if (!EFI_ERROR (Status)) { Status = mPmic->RegulatorSetVoltage ("ldo_vmc", 3300000); }
    } else { Status = EFI_SUCCESS; }
    if (!EFI_ERROR (Status)) { Status = mPmic->RegulatorSetEnable (Vmch, Enable); }
    if (!EFI_ERROR (Status)) { Status = mPmic->RegulatorSetEnable ("ldo_vmc", Enable); }
    if (!EFI_ERROR (Status)) { return EFI_SUCCESS; }
    // Restore the previous supply state, including a possibly completed failed write.
    Next = mPmic->RegulatorSetEnable ("ldo_vmc", OldVmc);
    if (EFI_ERROR (Next)) { Restore = Next; }
    Next = mPmic->RegulatorSetEnable (Vmch, OldVmch);
    if (EFI_ERROR (Next)) { Restore = Next; }
    Next = mPmic->RegulatorSetVoltage ("ldo_vmc", OldVmcUv);
    if (EFI_ERROR (Next)) { Restore = Next; }
    Next = mPmic->RegulatorSetVoltage (Vmch, OldVmchUv);
    if (EFI_ERROR (Next)) { Restore = Next; }
    return EFI_ERROR (Restore) ? EFI_DEVICE_ERROR : Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
InitGpio (
  UINT32 Index)
{
  EFI_STATUS Status;
  if ((Index > 1) || ((gPlatformInfo.HostMask & (1U << Index)) == 0) ||
      ((Index == 0) && !FixedPcdGetBool (PcdStorageIsEMMC))) {
    return EFI_UNSUPPORTED;
  }

  if (Index == 0 && FixedPcdGetBool(PcdStorageIsEMMC)) {
    // Setup EMMC GPIO
    Status = mGpio->SetMode (123, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (128, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (125, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (132, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (126, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (129, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (127, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (130, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (122, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (124, 1);
    if (EFI_ERROR (Status)) { return Status; }
  }
  else {
    // Setup SDCard GPIO
    Status = mGpio->SetMode (161, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (162, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (163, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (164, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (170, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (171, 1);
    if (EFI_ERROR (Status)) { return Status; }
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MsdcLibConstructor (VOID)
{
  EFI_STATUS Status;

  // Locate Gpio Protocol
  Status = gBS->LocateProtocol (&gMediaTekGpioProtocolGuid, NULL, (VOID **)&mGpio);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Locate GPIO Protocol! Status = %r\n", Status));
    return Status;
  }

  // Locate Clock Protocol
  Status = gBS->LocateProtocol (&gMediaTekClockProtocolGuid, NULL, (VOID **)&mClock);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Locate Clock Protocol! Status = %r\n", Status));
    return Status;
  }

  // Locate PMIC Protocol
  Status = gBS->LocateProtocol (&gMediaTekPmicProtocolGuid, NULL, (VOID **)&mPmic);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Locate PMIC Protocol! Status = %r\n", Status));
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
GetCardPresent (UINT32 Index, BOOLEAN *Present)
{
  EFI_STATUS Status;
  UINT32 Pin;
  BOOLEAN State;

  if ((Present == NULL) || (Index > 1)) { return EFI_INVALID_PARAMETER; }
  *Present = FALSE;
  if ((gPlatformInfo.HostMask & (1U << Index)) == 0) { return EFI_UNSUPPORTED; }
  *Present = TRUE;
  if (Index == 0) { return FixedPcdGetBool (PcdStorageIsEMMC) ? EFI_SUCCESS : EFI_UNSUPPORTED; }
  Pin = FixedPcdGet32 (PcdSdDetectPin);
  if (Pin == MAX_UINT32) { return EFI_SUCCESS; }
  Status = mGpio->SetMode (Pin, 0);
  if (EFI_ERROR (Status)) { return Status; }
  Status = mGpio->SetDir (Pin, FALSE);
  if (EFI_ERROR (Status)) { return Status; }
  Status = mGpio->GetState (Pin, &State);
  if (EFI_ERROR (Status)) { return Status; }
  *Present = FixedPcdGetBool (PcdSdDetectActiveLow) ? !State : State;
  return EFI_SUCCESS;
}
