#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/IoLib.h>
#include <Library/SynchronizationLib.h>

#include <Library/PcdLib.h>
#include <Library/PmicWrapperImplLib.h>
#include <Library/MtkGaugeCore.h>

#include <Protocol/MtkPmicWrapper.h>
#include <Protocol/MtkPmicGauge.h>

typedef enum {
  WacsFsmIdle = 0,
  WacsFsmWfVldClr = 6,
} WACS_FSM_STATE;

STATIC EFI_MEMORY_REGION_DESCRIPTOR mPmicWrapperRegion;
// Serialize the whole WACS command/response. Timer callbacks must not consume
// another caller's response. Fail busy rather than spin while preempting owner.
STATIC volatile UINT32 mWacsBusy;
STATIC MTK_GAUGE_CORE mGaugeCore;
#define FG_CTRL_REG        0x0D0AU
#define FG_CURRENT_REG     0x0D8AU
// MT6358/MT6366 AUXADC channels selected by the T70 stock device tree.
#define AUXADC_REQUEST_REG      0x1108U
#define AUXADC_BAT_VOLTAGE_REG  0x1088U
#define AUXADC_BAT_TEMP_REG     0x108EU
#define AUXADC_READY            BIT15
#define AUXADC_POLL_TIMEOUT_NS  32000000ULL

#define AUXADC_CHIP_TEMP_REG    0x1090U
#define AUXADC_BIF_VOLTAGE_REG  0x109EU
#define AUXADC_BIF_CONTROL_REG  0x0FA8U
#define AUXADC_BIF_DISABLE      BIT1
#define AUXADC_DIE_TEMP_TRIM    0x11CEU
#define AUXADC_TEMP_REF_TRIM    0x11D0U
#define AUXADC_TEMP_SLOPE_TRIM  0x11D2U
#define OTP_ROW                0x038AU
#define OTP_READ_TRIGGER       0x039AU
#define OTP_READ_ENABLE        0x03A0U
#define OTP_READ_DATA          0x03A2U
#define OTP_READ_BUSY          0x03A4U
#define OTP_POLL_TIMEOUT_NS    30000000ULL
#define BATTERY_TRIM_FUSE_ROW  0x0050U

STATIC MTK_GAUGE_VOLTAGE_TRIM mBatteryVoltageTrim;
STATIC BOOLEAN mBatteryVoltageTrimValid;
STATIC BOOLEAN mBatteryTrimRestoreFailed;
STATIC BOOLEAN mBatteryBifRestoreFailed;

STATIC
UINT32
PmicWrapperRead(
  IN  UINT16 Reg)
{
  return MmioRead32 (mPmicWrapperRegion.Address + gPlatformInfo.RegMap[Reg]);
}

STATIC
VOID
PmicWrapperWrite(
  IN  UINT16 Reg,
  IN  UINT32 Value)
{
  MmioWrite32 (mPmicWrapperRegion.Address + gPlatformInfo.RegMap[Reg], Value);
}

STATIC
WACS_FSM_STATE
WacsGetFsm ()
{
  UINT32 Value;

  // Read Data
  Value = PmicWrapperRead (PmicWrapperWacs2RData);

  // Get FSM State
  if (gPlatformInfo.ArbCapabilities) {
    Value = (Value >> 1) & 0x7;
  } else {
    Value = (Value >> 16) & 0x7;
  }

  return Value;
}

#define PWRAP_TIMEOUT_US        10000U
// Bound each leg as well as the number of FG latch polls.
#define PWRAP_GAUGE_TIMEOUT_US   1000U

STATIC
EFI_STATUS
WacsWaitFor (
  IN WACS_FSM_STATE Fsm,
  IN UINTN          Timeout)
{
  UINTN Elapsed;

  for (Elapsed = 0; Elapsed < Timeout; Elapsed += 10) {
    if (WacsGetFsm () == Fsm) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
WacsCommand (
  IN UINT32  Address,
  IN UINT32  Data,
  IN BOOLEAN IsWrite,
  IN UINTN   Timeout)
{
  EFI_STATUS Status;
  UINT32 Command;

  if ((Address & 1) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  // Recover an unread response before issuing a new transaction.
  if (WacsGetFsm () == WacsFsmWfVldClr) {
    PmicWrapperWrite (PmicWrapperWacs2VldClr, 1);
  }
  Status = WacsWaitFor (WacsFsmIdle, Timeout);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (gPlatformInfo.ArbCapabilities) {
    Command = Address;
    if (IsWrite) {
      PmicWrapperWrite (PmicWrapperSwinf2WData31, Data);
      Command |= BIT29;
    }
  } else {
    Command = ((UINT32)IsWrite << 31) | ((Address >> 1) << 16) | Data;
  }
  PmicWrapperWrite (PmicWrapperWacs2Cmd, Command);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WacsReadLocked (
  IN  UINT16  Address,
  OUT UINT16 *Value,
  IN  UINTN   Timeout)
{
  EFI_STATUS Status;

  Status = WacsCommand (Address, 0, FALSE, Timeout);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = WacsWaitFor (WacsFsmWfVldClr, Timeout);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  *Value = (UINT16)PmicWrapperRead (
    gPlatformInfo.ArbCapabilities ? PmicWrapperSwinf2RData31 : PmicWrapperWacs2RData);
  PmicWrapperWrite (PmicWrapperWacs2VldClr, 1);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
WacsWriteLocked (
  IN UINT16 Address,
  IN UINT16 Value,
  IN UINTN  Timeout)
{
  EFI_STATUS Status = WacsCommand (Address, Value, TRUE, Timeout);

  return EFI_ERROR (Status) ? Status : WacsWaitFor (WacsFsmIdle, Timeout);
}

STATIC
BOOLEAN
PmicDiagnosticRegisterReserved (
  IN UINT16 Address)
{
  return FixedPcdGetBool (PcdMt6358GaugeSnapshot) &&
         (Address == FG_CTRL_REG || Address == FG_CURRENT_REG ||
          Address == AUXADC_REQUEST_REG || Address == AUXADC_BAT_VOLTAGE_REG ||
          Address == AUXADC_BAT_TEMP_REG || Address == AUXADC_CHIP_TEMP_REG ||
          Address == AUXADC_BIF_VOLTAGE_REG || Address == AUXADC_BIF_CONTROL_REG ||
          Address == OTP_ROW || Address == OTP_READ_TRIGGER || Address == OTP_READ_ENABLE ||
          Address == OTP_READ_DATA || Address == OTP_READ_BUSY);
}

STATIC
EFI_STATUS
EFIAPI
PmicWrapperImplRead (
  IN  UINT16  Address,
  OUT UINT16 *Value)
{
  EFI_STATUS Status;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (PmicDiagnosticRegisterReserved (Address)) {
    return EFI_ACCESS_DENIED;
  }
  if (InterlockedCompareExchange32 (&mWacsBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  Status = WacsReadLocked (Address, Value, PWRAP_TIMEOUT_US);
  (VOID)InterlockedCompareExchange32 (&mWacsBusy, 1, 0);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
PmicWrapperImplWrite (
  IN UINT16 Address,
  IN UINT16 Value)
{
  EFI_STATUS Status;

  if (PmicDiagnosticRegisterReserved (Address)) {
    return EFI_ACCESS_DENIED;
  }
  if (InterlockedCompareExchange32 (&mWacsBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  Status = WacsWriteLocked (Address, Value, PWRAP_TIMEOUT_US);
  (VOID)InterlockedCompareExchange32 (&mWacsBusy, 1, 0);
  return Status;
}

STATIC
int
GaugeLease (
  VOID *Context)
{
  // The caller keeps WACS locked across the entire core call. The two gauge
  // registers stay private to this core even if latch cleanup fails.
  (VOID)Context;
  return 1;
}

STATIC
int
GaugeRead (
  VOID *Context,
  UINT16 Address,
  UINT16 *Value)
{
  (VOID)Context;
  return !EFI_ERROR (WacsReadLocked (Address, Value, PWRAP_GAUGE_TIMEOUT_US));
}

STATIC
int
GaugeWrite (
  VOID *Context,
  UINT16 Address,
  UINT16 Value)
{
  (VOID)Context;
  return !EFI_ERROR (WacsWriteLocked (Address, Value, PWRAP_GAUGE_TIMEOUT_US));
}

STATIC
EFI_STATUS
ReadGaugeCurrentLocked (
  OUT UINT16 *Raw)
{
  STATIC CONST MTK_GAUGE_IO Io = { NULL, GaugeLease, GaugeLease, GaugeRead, GaugeWrite };

  switch (MtkGaugeReadCurrent (&mGaugeCore, &Io, Raw)) {
  case MtkGaugeOk:      return EFI_SUCCESS;
  case MtkGaugeBusy:    return EFI_NOT_READY;
  case MtkGaugeTimeout: return EFI_TIMEOUT;
  default:              return EFI_DEVICE_ERROR;
  }
}

STATIC
EFI_STATUS
EFIAPI
PmicGaugeReadCurrent (
  OUT UINT16 *Raw)
{
  EFI_STATUS Status = EFI_NOT_READY;

  if (Raw == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (InterlockedCompareExchange32 (&mWacsBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  if (PmicWrapperRead (PmicWrapperInitDone2) == 1) {
    Status = ReadGaugeCurrentLocked (Raw);
  }
  (VOID)InterlockedCompareExchange32 (&mWacsBusy, 1, 0);
  return Status;
}

typedef struct {
  UINT16 Request;
  UINT16 Output;
  UINT16 Mask;
  UINT16 ConvertUs;
} PMIC_AUXADC_CHANNEL;

STATIC CONST PMIC_AUXADC_CHANNEL mBatteryVoltageAdc = { BIT0, AUXADC_BAT_VOLTAGE_REG, 0x7FFF, 1280 };
STATIC CONST PMIC_AUXADC_CHANNEL mBatteryTempAdc = { BIT3, AUXADC_BAT_TEMP_REG, 0x0FFF, 80 };
STATIC CONST PMIC_AUXADC_CHANNEL mChipTempAdc = { BIT4, AUXADC_CHIP_TEMP_REG, 0x0FFF, 80 };
STATIC CONST PMIC_AUXADC_CHANNEL mBifVoltageAdc = { BIT11, AUXADC_BIF_VOLTAGE_REG, 0x0FFF, 80 };

STATIC
EFI_STATUS
VerifyPmicBitsLocked (
  IN UINT16 Address,
  IN UINT16 Mask,
  IN UINT16 ExpectedBits
  )
{
  UINT16 Value;
  EFI_STATUS Status = WacsReadLocked (Address, &Value, PWRAP_GAUGE_TIMEOUT_US);

  if (EFI_ERROR (Status)) {
    return Status;
  }
  return (Value & Mask) == ExpectedBits ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
ReadBatteryFuseWordLocked (
  IN     UINT16 Row,
  IN OUT UINT16 *Trigger,
  OUT    UINT16 *Sample
  )
{
  UINT16 Busy;
  UINT64 Start;
  EFI_STATUS Status;

  Status = WacsWriteLocked (OTP_ROW, Row, PWRAP_GAUGE_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MicroSecondDelay (40);
  *Trigger ^= 1;
  Status = WacsWriteLocked (OTP_READ_TRIGGER, *Trigger, PWRAP_GAUGE_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MicroSecondDelay (50);
  Start = GetPerformanceCounter ();
  do {
    Status = WacsReadLocked (OTP_READ_BUSY, &Busy, PWRAP_GAUGE_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if ((Busy & BIT0) == 0) {
      MicroSecondDelay (30);
      return WacsReadLocked (OTP_READ_DATA, Sample, PWRAP_GAUGE_TIMEOUT_US);
    }
    MicroSecondDelay (13);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < OTP_POLL_TIMEOUT_NS);
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
ReadBatteryFusesLocked (
  OUT UINT16 Fuse[4]
  )
{
  // Clock controls and read sequence from the stock mediatek,mt6358-efuse
  // match data. The T70 DT's cali-efuse-offset=1 selects rows 0x50-0x56.
  STATIC CONST struct { UINT16 Base; UINT16 Mask; } ClockGates[] = {
    { 0x012A, BIT2 },
    { 0x010C, BIT4 }
  };
  UINT16 Restore[ARRAY_SIZE (ClockGates)] = { 0 };
  BOOLEAN RestoreReadEnable = FALSE;
  EFI_STATUS Status = EFI_SUCCESS;
  EFI_STATUS WriteStatus;
  UINT16 Value;
  UINT16 Trigger;
  UINTN Index;

  if (mBatteryTrimRestoreFailed) {
    return EFI_DEVICE_ERROR;
  }
  for (Index = 0; Index < ARRAY_SIZE (ClockGates); Index++) {
    Status = WacsReadLocked (ClockGates[Index].Base, &Value, PWRAP_GAUGE_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      break;
    }
    Restore[Index] = Value & ClockGates[Index].Mask;
    if (Restore[Index] != 0) {
      Status = WacsWriteLocked (ClockGates[Index].Base + 4, Restore[Index], PWRAP_GAUGE_TIMEOUT_US);
      if (!EFI_ERROR (Status)) {
        Status = VerifyPmicBitsLocked (ClockGates[Index].Base, Restore[Index], 0);
      }
      if (EFI_ERROR (Status)) {
        break;
      }
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = WacsReadLocked (OTP_READ_ENABLE, &Value, PWRAP_GAUGE_TIMEOUT_US);
    if (!EFI_ERROR (Status) && Value != 0) {
      Status = EFI_NOT_READY;
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = WacsReadLocked (OTP_READ_BUSY, &Value, PWRAP_GAUGE_TIMEOUT_US);
    if (!EFI_ERROR (Status) && (Value & BIT0) != 0) {
      Status = EFI_NOT_READY;
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = WacsReadLocked (OTP_READ_TRIGGER, &Trigger, PWRAP_GAUGE_TIMEOUT_US);
    if (!EFI_ERROR (Status) && Trigger > 1) {
      Status = EFI_DEVICE_ERROR;
    }
  }
  if (!EFI_ERROR (Status)) {
    // Record ownership before writing. A timed-out WACS write can have landed.
    RestoreReadEnable = TRUE;
    Status = WacsWriteLocked (OTP_READ_ENABLE, 1, PWRAP_GAUGE_TIMEOUT_US);
    if (!EFI_ERROR (Status)) {
      Status = VerifyPmicBitsLocked (OTP_READ_ENABLE, 0xFFFF, 1);
    }
    for (Index = 0; Index < ARRAY_SIZE (mBatteryVoltageTrim.Fuse) && !EFI_ERROR (Status); Index++) {
      Status = ReadBatteryFuseWordLocked (BATTERY_TRIM_FUSE_ROW + (UINT16)(2 * Index), &Trigger, &Fuse[Index]);
    }
  }

  // Try each restoration even if an earlier write timed out. The readback
  // determines whether the controller may be used for another trim request.
  if (RestoreReadEnable) {
    WriteStatus = WacsWriteLocked (OTP_READ_ENABLE, 0, PWRAP_GAUGE_TIMEOUT_US);
    if (EFI_ERROR (VerifyPmicBitsLocked (OTP_READ_ENABLE, 0xFFFF, 0))) {
      mBatteryTrimRestoreFailed = TRUE;
    }
    if (!EFI_ERROR (Status) && EFI_ERROR (WriteStatus)) {
      Status = WriteStatus;
    }
  }
  for (Index = 0; Index < ARRAY_SIZE (ClockGates); Index++) {
    if (Restore[Index] != 0) {
      WriteStatus = WacsWriteLocked (ClockGates[Index].Base + 2, Restore[Index], PWRAP_GAUGE_TIMEOUT_US);
      if (EFI_ERROR (VerifyPmicBitsLocked (ClockGates[Index].Base, Restore[Index], Restore[Index]))) {
        mBatteryTrimRestoreFailed = TRUE;
      }
      if (!EFI_ERROR (Status) && EFI_ERROR (WriteStatus)) {
        Status = WriteStatus;
      }
    }
  }
  return mBatteryTrimRestoreFailed ? EFI_DEVICE_ERROR : Status;
}

STATIC
EFI_STATUS
ReadBatteryVoltageTrimLocked (VOID)
{
  EFI_STATUS Status = WacsReadLocked (AUXADC_DIE_TEMP_TRIM, &mBatteryVoltageTrim.DieTemperature, PWRAP_GAUGE_TIMEOUT_US);
  if (!EFI_ERROR (Status) && (mBatteryVoltageTrim.DieTemperature & BIT8) != 0) {
    Status = WacsReadLocked (AUXADC_TEMP_REF_TRIM, &mBatteryVoltageTrim.TemperatureReference, PWRAP_GAUGE_TIMEOUT_US);
    if (!EFI_ERROR (Status)) {
      Status = WacsReadLocked (AUXADC_TEMP_SLOPE_TRIM, &mBatteryVoltageTrim.TemperatureSlope, PWRAP_GAUGE_TIMEOUT_US);
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = ReadBatteryFusesLocked (mBatteryVoltageTrim.Fuse);
  }
  mBatteryVoltageTrimValid = !EFI_ERROR (Status);
  return Status;
}

STATIC
EFI_STATUS
ReadAuxAdcLocked (
  IN  CONST PMIC_AUXADC_CHANNEL *Channel,
  OUT UINT16                    *Raw
  )
{
  EFI_STATUS Status;
  UINT64 Start;
  UINT16 Sample;

  // Only the requested channel's trigger bit is written.
  Status = WacsWriteLocked (AUXADC_REQUEST_REG, Channel->Request, PWRAP_GAUGE_TIMEOUT_US);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  MicroSecondDelay (Channel->ConvertUs);
  Start = GetPerformanceCounter ();
  do {
    Status = WacsReadLocked (Channel->Output, &Sample, PWRAP_GAUGE_TIMEOUT_US);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if ((Sample & AUXADC_READY) != 0) {
      *Raw = Sample & Channel->Mask;
      return EFI_SUCCESS;
    }
    MicroSecondDelay (26);
  } while (GetTimeInNanoSecond (GetPerformanceCounter () - Start) < AUXADC_POLL_TIMEOUT_NS);
  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
ReadCalibratedBatteryVoltageLocked (
  OUT UINT16 *Millivolts
  )
{
  UINT16 Raw;
  UINT16 Chip;
  EFI_STATUS Status;

  if (!mBatteryVoltageTrimValid) {
    Status = ReadBatteryVoltageTrimLocked ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  Status = ReadAuxAdcLocked (&mBatteryVoltageAdc, &Raw);
  if (!EFI_ERROR (Status)) {
    Status = ReadAuxAdcLocked (&mChipTempAdc, &Chip);
  }
  if (!EFI_ERROR (Status) && MtkGaugeCalibrateVoltage (Raw, Chip, &mBatteryVoltageTrim, Millivolts) != MtkGaugeOk) {
    return EFI_DEVICE_ERROR;
  }
  return Status;
}

STATIC
EFI_STATUS
ReadBifVoltageLocked (
  OUT UINT16 *Raw)
{
  UINT16 Original;
  EFI_STATUS Status = WacsReadLocked (AUXADC_BIF_CONTROL_REG, &Original, PWRAP_GAUGE_TIMEOUT_US);
  EFI_STATUS Restore;

  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((Original & AUXADC_BIF_DISABLE) == 0) {
    return ReadAuxAdcLocked (&mBifVoltageAdc, Raw);
  }

  // mt6358_vbif_conv clears bit 1 during conversion. Keep the incoming value
  // and check its restoration even when a request timed out mid-write.
  Status = WacsWriteLocked (AUXADC_BIF_CONTROL_REG, Original & ~AUXADC_BIF_DISABLE, PWRAP_GAUGE_TIMEOUT_US);
  if (!EFI_ERROR (Status)) {
    Status = VerifyPmicBitsLocked (AUXADC_BIF_CONTROL_REG, AUXADC_BIF_DISABLE, 0);
  }
  if (!EFI_ERROR (Status)) {
    Status = ReadAuxAdcLocked (&mBifVoltageAdc, Raw);
  }
  Restore = WacsWriteLocked (AUXADC_BIF_CONTROL_REG, Original, PWRAP_GAUGE_TIMEOUT_US);
  if (EFI_ERROR (VerifyPmicBitsLocked (AUXADC_BIF_CONTROL_REG, AUXADC_BIF_DISABLE, AUXADC_BIF_DISABLE))) {
    mBatteryBifRestoreFailed = TRUE;
    return EFI_DEVICE_ERROR;
  }
  return EFI_ERROR (Status) ? Status : Restore;
}

STATIC
EFI_STATUS
ReadBatteryTemperatureLocked (
  OUT UINT16 *DeciKelvin)
{
  UINT16 Thermistor;
  UINT16 Current;
  UINT16 Reference;
  INT32 Temperature;
  EFI_STATUS Status;

  if (mBatteryBifRestoreFailed) {
    return EFI_DEVICE_ERROR;
  }
  Status = ReadAuxAdcLocked (&mBatteryTempAdc, &Thermistor);
  if (!EFI_ERROR (Status)) {
    Status = ReadGaugeCurrentLocked (&Current);
  }
  if (!EFI_ERROR (Status)) {
    Status = ReadBifVoltageLocked (&Reference);
  }
  if (!EFI_ERROR (Status)) {
    if (MtkGaugeBatteryTemperature (Thermistor, Reference, Current, &Temperature) != MtkGaugeOk) {
      return EFI_DEVICE_ERROR;
    }
    *DeciKelvin = (UINT16)(Temperature + 2732);
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
PmicBatteryAdcRead (
  IN  MTK_PMIC_BATTERY_ADC_CHANNEL Channel,
  OUT UINT16                     *Sample)
{
  EFI_STATUS Status = EFI_NOT_READY;

  if (Sample == NULL || (UINTN)Channel > MtkPmicBatteryAdcTemperatureDeciK) {
    return EFI_INVALID_PARAMETER;
  }
  if (InterlockedCompareExchange32 (&mWacsBusy, 0, 1) != 0) {
    return EFI_NOT_READY;
  }
  if (PmicWrapperRead (PmicWrapperInitDone2) == 1) {
    if (Channel == MtkPmicBatteryAdcCalibratedVoltage) {
      Status = ReadCalibratedBatteryVoltageLocked (Sample);
    } else if (Channel == MtkPmicBatteryAdcTemperatureDeciK) {
      Status = ReadBatteryTemperatureLocked (Sample);
    } else {
      Status = ReadAuxAdcLocked (Channel == MtkPmicBatteryAdcVoltage ? &mBatteryVoltageAdc : &mBatteryTempAdc, Sample);
    }
  }
  (VOID)InterlockedCompareExchange32 (&mWacsBusy, 1, 0);
  return Status;
}

STATIC MTK_PMIC_GAUGE_PROTOCOL mGauge = { PmicGaugeReadCurrent };
STATIC MTK_PMIC_BATTERY_ADC_PROTOCOL mBatteryAdc = { PmicBatteryAdcRead };

STATIC MTK_PMIC_WRAPPER_PROTOCOL mPmicWrapper = {
  PmicWrapperImplRead,
  PmicWrapperImplWrite
};

EFI_STATUS
EFIAPI
InitPmicWrapper (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  UINT32     InitState;

  // Locate PMIC Wrapper Memory Region
  Status = LocateMemoryRegionByName ("PMIC Wrapper", &mPmicWrapperRegion);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate PMIC Wrapper Memory Region! Status = %r\n", Status));
    return Status;
  }

  // Read init state of PMIC Wrapper
  InitState = PmicWrapperRead (PmicWrapperInitDone2);
  if (InitState != 1) {
    DEBUG ((EFI_D_ERROR, "PMIC Wrapper Not Initialized!\n"));
    return EFI_NOT_READY;
  }

  // Register PMIC Wrapper Protocol
  if (FixedPcdGetBool (PcdMt6358GaugeSnapshot)) {
    Status = gBS->InstallMultipleProtocolInterfaces (
      &ImageHandle, &gMediaTekPmicWrapperProtocolGuid, &mPmicWrapper,
      &gMediaTekPmicGaugeProtocolGuid, &mGauge,
      &gMediaTekPmicBatteryAdcProtocolGuid, &mBatteryAdc, NULL);
  } else {
    Status = gBS->InstallMultipleProtocolInterfaces (
      &ImageHandle, &gMediaTekPmicWrapperProtocolGuid, &mPmicWrapper, NULL);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Register PMIC Wrapper Protocol! Status = %r\n", Status));
    return Status;
  }

  return Status;
}