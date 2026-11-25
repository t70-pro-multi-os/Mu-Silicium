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
STATIC BOOLEAN mSdDetectConfigured;
STATIC UINT64 mSdPadLeaseToken;
STATIC BOOLEAN mSdPadLeaseUnsafe;
STATIC BOOLEAN mSdPadsConfigured;

STATIC CONST UINT32 mSdPadPins[] = {71, 72, 73, 74, 75, 76};

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

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "IFRAO_MSDC0_SRC" : "IFRAO_MSDC1_SRC", &ClockId);
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
  UINT32 Parent, Child, First, Second;
  BOOLEAN FirstWasEnabled;

  // Operate on the mux gate itself. The 1:1 factor has no hardware gate.
  Status = mClock->GetId (Index == 0 ? "TOP_MSDC50_0_SEL" : "TOP_MSDC30_1_SEL", &Parent);
  if (EFI_ERROR (Status)) return Status;
  Status = mClock->GetId (Index == 0 ? "IFRAO_MSDC0" : "IFRAO_MSDC1", &Child);
  if (EFI_ERROR (Status)) return Status;
  First = Enable ? Parent : Child;
  Second = Enable ? Child : Parent;
  Status = mClock->IsEnabled (First, &FirstWasEnabled);
  if (EFI_ERROR (Status)) return Status;
  Status = mClock->SetEnable (First, Enable);
  if (EFI_ERROR (Status)) return Status;
  Status = mClock->SetEnable (Second, Enable);
  if (EFI_ERROR (Status) && FirstWasEnabled != Enable &&
      EFI_ERROR (mClock->SetEnable (First, FirstWasEnabled))) {
    return EFI_DEVICE_ERROR;
  }
  return Status;
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
    CONST CHAR8 *Rails[] = { Enable ? Vmch : "ldo_vmc", Enable ? "ldo_vmc" : Vmch };
    BOOLEAN WasEnabled[2], EnableAttempted[2] = { FALSE, FALSE };
    BOOLEAN VoltageAttempted[2] = { FALSE, FALSE };
    UINT32 OldUv[2];
    BOOLEAN RestoreFailed = FALSE;
    if (FixedPcdGet32 (PcdSdVmchEint) > 2) { return EFI_INVALID_PARAMETER; }
    for (UINTN I = 0; I < ARRAY_SIZE (Rails); I++) {
      Status = mPmic->RegulatorIsEnabled (Rails[I], &WasEnabled[I]);
      if (EFI_ERROR (Status)) return Status;
      if (!Enable) continue;
      Status = mPmic->RegulatorGetVoltage (Rails[I], &OldUv[I]);
      if (EFI_ERROR (Status)) return Status;
      if (WasEnabled[I] && OldUv[I] != 3300000) return EFI_ACCESS_DENIED;
    }
    for (UINTN I = 0; I < ARRAY_SIZE (Rails); I++) {
      if (WasEnabled[I] == Enable) continue;
      if (Enable && OldUv[I] != 3300000) {
        VoltageAttempted[I] = TRUE;
        Status = mPmic->RegulatorSetVoltage (Rails[I], 3300000);
        if (EFI_ERROR (Status)) goto RestoreRails;
      }
      EnableAttempted[I] = TRUE;
      Status = mPmic->RegulatorSetEnable (Rails[I], Enable);
      if (EFI_ERROR (Status)) goto RestoreRails;
    }
    return EFI_SUCCESS;

RestoreRails:
    // Reverse the power sequence. Only restore voltage after confirming the
    // rail is off, including when a failed enable may have reached the PMIC.
    for (UINTN I = ARRAY_SIZE (Rails); I-- > 0;) {
      if (EnableAttempted[I] &&
          EFI_ERROR (mPmic->RegulatorSetEnable (Rails[I], WasEnabled[I]))) {
        RestoreFailed = TRUE;
        continue;
      }
      if (VoltageAttempted[I] &&
          EFI_ERROR (mPmic->RegulatorSetVoltage (Rails[I], OldUv[I]))) {
        RestoreFailed = TRUE;
      }
    }
    return RestoreFailed ? EFI_DEVICE_ERROR : Status;
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
    Status = mGpio->SetMode (58, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (59, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (60, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (61, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (62, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (63, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (64, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (65, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (56, 1);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetMode (55, 1);
    if (EFI_ERROR (Status)) { return Status; }
  }
  else {
    UINT64 PadToken;
    BOOLEAN CanReleaseLease;
    EFI_STATUS ReleaseStatus;

    if ((mGpio->AcquirePins == NULL) || (mGpio->ReleasePins == NULL) ||
        (mGpio->ConfigureMsdc1PadsWithLease == NULL)) {
      return EFI_UNSUPPORTED;
    }

    // A failed release is retried before any new lease.  A lease retained
    // after an unverifiable rollback permanently quarantines this pin group
    // for the current boot rather than allowing another driver to reuse it.
    if (mSdPadLeaseToken != 0) {
      if (mSdPadLeaseUnsafe) {
        return EFI_DEVICE_ERROR;
      }
      ReleaseStatus = mGpio->ReleasePins (mSdPadLeaseToken);
      if (EFI_ERROR (ReleaseStatus)) {
        return ReleaseStatus;
      }
      mSdPadLeaseToken = 0;
      if (mSdPadsConfigured) {
        return EFI_SUCCESS;
      }
    }

    if (mSdPadsConfigured) {
      return EFI_SUCCESS;
    }

    PadToken = 0;
    Status = mGpio->AcquirePins (mSdPadPins, ARRAY_SIZE (mSdPadPins), &PadToken);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    CanReleaseLease = TRUE;
    Status = mGpio->ConfigureMsdc1PadsWithLease (PadToken, &CanReleaseLease);
    if (EFI_ERROR (Status) || !CanReleaseLease) {
      if (!CanReleaseLease) {
        mSdPadLeaseToken = PadToken;
        mSdPadLeaseUnsafe = TRUE;
        return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
      }
      ReleaseStatus = mGpio->ReleasePins (PadToken);
      if (EFI_ERROR (ReleaseStatus)) {
        mSdPadLeaseToken = PadToken;
        mSdPadLeaseUnsafe = FALSE;
        return ReleaseStatus;
      }
      return Status;
    }

    mSdPadsConfigured = TRUE;
    ReleaseStatus = mGpio->ReleasePins (PadToken);
    if (EFI_ERROR (ReleaseStatus)) {
      mSdPadLeaseToken = PadToken;
      mSdPadLeaseUnsafe = FALSE;
      return ReleaseStatus;
    }
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
  *Present = FALSE;
  if (!mSdDetectConfigured) {
    Status = mGpio->SetMode (Pin, 0);
    if (EFI_ERROR (Status)) { return Status; }
    Status = mGpio->SetDir (Pin, FALSE);
    if (EFI_ERROR (Status)) { return Status; }
    mSdDetectConfigured = TRUE;
  }
  // Timer/transfer sampling must not repeatedly reprogram the detect pin.
  Status = mGpio->GetDir (Pin, &State);
  if (EFI_ERROR (Status)) { return Status; }
  if (State) { return EFI_ACCESS_DENIED; }
  Status = mGpio->GetState (Pin, &State);
  if (EFI_ERROR (Status)) { return Status; }
  *Present = FixedPcdGetBool (PcdSdDetectActiveLow) ? !State : State;
  return EFI_SUCCESS;
}
