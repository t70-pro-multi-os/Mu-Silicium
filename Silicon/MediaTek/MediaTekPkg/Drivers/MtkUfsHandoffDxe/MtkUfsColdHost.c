/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "MtkUfsColdHost.h"
#include <IndustryStandard/UfsHci.h>
#include <Pi/PiDxeCis.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/MtkClock.h>
#include <Protocol/MtkPmic.h>

#define COLD_UFS_BASE    0x11270000U
#define COLD_UFS_LENGTH  0x2300U
#define COLD_UFS_REFCLK_OFFSET 0x144U
// The PMIC protocol uses its driver name; the DT regulator-name remains "vemc".
#define COLD_UFS_PMIC_VCC "ldo_vemc"
#define COLD_INFRA_BASE  0x10001000U
#define COLD_INFRA_LENGTH 0x1000U

static BOOLEAN ColdBootServicesExited(CONST MTK_UFS_COLD_HOST *Host)
{
  return Host != NULL && (Host->IoClosed ||
         (Host->BootServicesExited != NULL && *Host->BootServicesExited));
}

static EFI_STATUS ToEfi(MTK_UFS_PLATFORM_STATUS Status)
{
  switch (Status) {
    case MtkUfsPlatformOk: return EFI_SUCCESS;
    case MtkUfsPlatformInvalid: return EFI_INVALID_PARAMETER;
    case MtkUfsPlatformUnsupported: return EFI_UNSUPPORTED;
    case MtkUfsPlatformTimeout: return EFI_TIMEOUT;
    case MtkUfsPlatformBusy: case MtkUfsPlatformWrongState: return EFI_NOT_READY;
    default: return EFI_DEVICE_ERROR;
  }
}

static MTK_UFS_PLATFORM_STATUS FromEfi(EFI_STATUS Status)
{
  if (!EFI_ERROR(Status)) return MtkUfsPlatformOk;
  return Status == EFI_TIMEOUT ? MtkUfsPlatformTimeout : MtkUfsPlatformIoError;
}

static MTK_UFS_PLATFORM_STATUS ColdRead(void *Context, MTK_UFS_MMIO_BANK Bank, uint32_t Offset, uint32_t *Value)
{
  MTK_UFS_COLD_HOST *Host = Context;
  if (Host == NULL) return MtkUfsPlatformInvalid;
  if (ColdBootServicesExited(Host)) return MtkUfsPlatformUnsupported;
  if (!Host->Configured || Value == NULL || Bank != MtkUfsControllerBank ||
      (Offset & 3U) || Offset > COLD_UFS_LENGTH - 4U) return MtkUfsPlatformInvalid;
  *Value = MmioRead32(COLD_UFS_BASE + Offset);
  return MtkUfsPlatformOk;
}

static MTK_UFS_PLATFORM_STATUS ColdWrite(void *Context, MTK_UFS_MMIO_BANK Bank, uint32_t Offset, uint32_t Value)
{
  MTK_UFS_COLD_HOST *Host = Context;
  UINTN Base;
  UINT32 Length;
  if (Host == NULL) return MtkUfsPlatformInvalid;
  if (ColdBootServicesExited(Host)) return MtkUfsPlatformUnsupported;
  if (!Host->Configured || (Bank != MtkUfsControllerBank && Bank != MtkUfsResetBank)) return MtkUfsPlatformInvalid;
  Base = Bank == MtkUfsControllerBank ? COLD_UFS_BASE : COLD_INFRA_BASE;
  Length = Bank == MtkUfsControllerBank ? COLD_UFS_LENGTH : COLD_INFRA_LENGTH;
  if ((Offset & 3U) || Offset > Length - 4U) return MtkUfsPlatformInvalid;
  MmioWrite32(Base + Offset, Value);
  MemoryFence();
  return MtkUfsPlatformOk;
}

static void ColdDelay(void *Context, uint32_t Delay)
{
  MTK_UFS_COLD_HOST *Host = Context;
  if (ColdBootServicesExited(Host)) return;
  MicroSecondDelay(Delay);
}

static MTK_UFS_PLATFORM_STATUS ColdSmc(void *Context, const uint64_t Args[8], int64_t *Result)
{
  MTK_UFS_COLD_HOST *Host = Context;
  ARM_SMC_ARGS SmcArgs;
  if (Host == NULL) return MtkUfsPlatformInvalid;
  if (ColdBootServicesExited(Host)) return MtkUfsPlatformUnsupported;
  if (!Host->Configured || Args == NULL || Result == NULL) return MtkUfsPlatformInvalid;
  ZeroMem(&SmcArgs, sizeof(SmcArgs));
  SmcArgs.Arg0 = Args[0]; SmcArgs.Arg1 = Args[1]; SmcArgs.Arg2 = Args[2]; SmcArgs.Arg3 = Args[3];
  SmcArgs.Arg4 = Args[4]; SmcArgs.Arg5 = Args[5]; SmcArgs.Arg6 = Args[6]; SmcArgs.Arg7 = Args[7];
  ArmCallSmc(&SmcArgs);
  *Result = (INT64)SmcArgs.Arg0;
  return MtkUfsPlatformOk;
}

static MTK_UFS_PLATFORM_STATUS ColdDme(void *Context, int Get, uint16_t Attribute, uint32_t *Value)
{
  MTK_UFS_COLD_HOST *Host = Context;
  EDKII_UIC_COMMAND Command;
  EFI_STATUS Status;
  if (Host == NULL) return MtkUfsPlatformInvalid;
  if (ColdBootServicesExited(Host)) return MtkUfsPlatformUnsupported;
  if (!Host->Configured || Host->Interface == NULL ||
      Host->Interface->UfsExecUicCommand == NULL || Value == NULL) return MtkUfsPlatformInvalid;
  ZeroMem(&Command, sizeof(Command));
  Command.Opcode = Get ? UfsUicDmeGet : UfsUicDmeSet;
  Command.Arg1 = (UINT32)Attribute << 16;
  Command.Arg3 = Get ? 0 : *Value;
  Status = Host->Interface->UfsExecUicCommand(Host->Interface, &Command);
  if (ColdBootServicesExited(Host)) return MtkUfsPlatformUnsupported;
  if (EFI_ERROR(Status)) return FromEfi(Status);
  if ((Command.Arg2 & 0xFFU) != 0) return MtkUfsPlatformIoError;
  if (Get) *Value = Command.Arg3;
  return MtkUfsPlatformOk;
}

static MTK_UFS_PLATFORM_STATUS ColdHibernate(void *Context, int Enter)
{
  MTK_UFS_COLD_HOST *Host = Context;
  EDKII_UIC_COMMAND Command;
  EFI_STATUS Status;
  if (Host == NULL) return MtkUfsPlatformInvalid;
  if (ColdBootServicesExited(Host)) return MtkUfsPlatformUnsupported;
  if (!Host->Configured || Host->Interface == NULL ||
      Host->Interface->UfsExecUicCommand == NULL || (Enter != 0 && Enter != 1)) return MtkUfsPlatformInvalid;
  ZeroMem(&Command, sizeof(Command));
  Command.Opcode = Enter ? UfsUicDmeHibernateEnter : UfsUicDmeHibernateExit;
  Status = Host->Interface->UfsExecUicCommand(Host->Interface, &Command);
  if (ColdBootServicesExited(Host)) return MtkUfsPlatformUnsupported;
  if (EFI_ERROR(Status)) return FromEfi(Status);
  return (Command.Arg2 & 0xFFU) == 0 ? MtkUfsPlatformOk : MtkUfsPlatformIoError;
}

static UINT32 Cell(CONST VOID *Bytes)
{
  CONST UINT8 *B = Bytes;
  return ((UINT32)B[0] << 24) | ((UINT32)B[1] << 16) | ((UINT32)B[2] << 8) | B[3];
}

static BOOLEAN CellsEqual(CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, CONST UINT32 *Expected, UINTN Count)
{
  INT32 Length;
  CONST UINT8 *B = FdtGetProp(Fdt, Node, Name, &Length);
  UINTN I;
  if (B == NULL || Length != Count * 4U) return FALSE;
  for (I = 0; I < Count; I++) if (Cell(B + I * 4U) != Expected[I]) return FALSE;
  return TRUE;
}

static BOOLEAN Compatible(CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name)
{
  INT32 Length;
  CONST CHAR8 *B = FdtGetProp(Fdt, Node, "compatible", &Length);
  return B != NULL && Length > 0 && FdtStringListContains(B, Length, Name);
}

static BOOLEAN Enabled(CONST VOID *Fdt, INT32 Node)
{
  INT32 Length;
  CONST CHAR8 *B = FdtGetProp(Fdt, Node, "status", &Length);
  return B == NULL ? Length == -FDT_ERR_NOTFOUND :
    (Length == 3 && CompareMem(B, "ok", 3) == 0) || (Length == 5 && CompareMem(B, "okay", 5) == 0);
}

static BOOLEAN StringEquals(CONST VOID *Fdt, INT32 Node, CONST CHAR8 *Name, CONST VOID *Expected, UINTN Size)
{
  INT32 Length;
  CONST VOID *B = FdtGetProp(Fdt, Node, Name, &Length);
  return B != NULL && Length == Size && CompareMem(B, Expected, Size) == 0;
}

static BOOLEAN Mapped(UINT64 Base, UINT64 Length)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  UINT64 Offset;
  EFI_STATUS Status = gDS->GetMemorySpaceDescriptor(Base, &D);
  if (EFI_ERROR(Status) || D.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo ||
      !(D.Attributes & EFI_MEMORY_UC) || D.BaseAddress > Base || D.BaseAddress > MAX_UINT64 - D.Length) return FALSE;
  Offset = Base - D.BaseAddress;
  return Offset < D.Length && Length <= D.Length - Offset;
}

static EFI_STATUS ColdRollbackResources(MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status = EFI_SUCCESS;
  EFI_TPL OldTpl;
  BOOLEAN IsOn;
  INTN I;
  if (Host == NULL || ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (!Host->CleanupPending || Host->CleanupMode != MtkUfsColdCleanupStoppedHost ||
      Host->Clock == NULL || Host->Clock->SetEnable == NULL || Host->Clock->IsEnabled == NULL) {
    return EFI_NOT_READY;
  }
  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (ColdBootServicesExited(Host)) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }
  if (Host->ClockOwned[5]) {
    /* TOP_UFS is a factor clock, not an owned gate. Never disable it. */
    Status = EFI_NOT_READY;
    goto Done;
  }
  Host->CleanupVccOffConfirmed = FALSE;
  /* VEMC is shared with the active MMC controller and has no PMIC reference
   * count. Never turn it off or discard the fact that this driver enabled it. */
  for (I = 4; I >= 0; I--) {
    if (!Host->ClockOwned[I]) continue;
    Status = Host->Clock->SetEnable(Host->ClockId[I], FALSE);
    if (ColdBootServicesExited(Host)) {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    Status = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(Status) || IsOn) {
      if (!EFI_ERROR(Status)) Status = EFI_DEVICE_ERROR;
      goto Done;
    }
    Host->ClockOwned[I] = FALSE;
  }

Done:
  gBS->RestoreTPL(OldTpl);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  return Status;
}

static EFI_STATUS ColdValidateResources(MTK_UFS_COLD_HOST *Host);

static BOOLEAN ColdHasOwnedResources(MTK_UFS_COLD_HOST *Host)
{
  UINTN I;
  if (Host == NULL) return FALSE;
  for (I = 0; I < ARRAY_SIZE(Host->ClockOwned); I++) {
    if (Host->ClockOwned[I]) return TRUE;
  }
  return FALSE;
}

static EFI_STATUS ColdValidateVccRetained(MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status;
  BOOLEAN IsOn;
  UINT32 Voltage;

  if (Host == NULL) return EFI_NOT_READY;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Pmic == NULL || Host->Pmic->RegulatorIsEnabled == NULL ||
      Host->Pmic->RegulatorGetVoltage == NULL) return EFI_NOT_READY;
  Status = Host->Pmic->RegulatorIsEnabled(COLD_UFS_PMIC_VCC, &IsOn);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (!IsOn) return EFI_NOT_READY;
  Status = Host->Pmic->RegulatorGetVoltage(COLD_UFS_PMIC_VCC, &Voltage);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (Voltage < 2900000U || Voltage > 3300000U) return EFI_UNSUPPORTED;
  Host->VccRetained = TRUE;
  return EFI_SUCCESS;
}

static EFI_STATUS ColdVerifyStoppedRoots(MTK_UFS_COLD_HOST *Host, BOOLEAN RequireRefClockOff)
{
  static CONST UINT32 Offsets[] = {
    UFS_HC_ENABLE_OFFSET, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTMRLRSR_OFFSET,
    UFS_HC_UTRLDBR_OFFSET, UFS_HC_UTMRLDBR_OFFSET,
    UFS_HC_UTRLBA_OFFSET, UFS_HC_UTRLBAU_OFFSET,
    UFS_HC_UTMRLBA_OFFSET, UFS_HC_UTMRLBAU_OFFSET,
    UFS_HC_IE_OFFSET
  };
  UINT32 Value;
  UINTN I;
  MTK_UFS_PLATFORM_STATUS PlatformStatus;

  if (Host == NULL || ColdBootServicesExited(Host) || !Host->Configured) return EFI_UNSUPPORTED;
  for (I = 0; I < ARRAY_SIZE(Offsets); I++) {
    PlatformStatus = Host->Platform.Io.Read32(Host->Platform.Io.Context,
                                             MtkUfsControllerBank, Offsets[I], &Value);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (PlatformStatus != MtkUfsPlatformOk) return EFI_DEVICE_ERROR;
    if (Value == MAX_UINT32) return EFI_DEVICE_ERROR;
    if (Value != 0) return EFI_NOT_READY;
  }
  if (RequireRefClockOff) {
    PlatformStatus = Host->Platform.Io.Read32(Host->Platform.Io.Context,
                                             MtkUfsControllerBank, COLD_UFS_REFCLK_OFFSET, &Value);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (PlatformStatus != MtkUfsPlatformOk || Value == MAX_UINT32) return EFI_DEVICE_ERROR;
    if ((Value & 3U) != 0) return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS ColdEnsureCleanupProofResources(MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status, ReadStatus;
  BOOLEAN IsOn;
  UINTN I;

  if (Host == NULL) return EFI_NOT_READY;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Clock == NULL || Host->Clock->IsEnabled == NULL || Host->Clock->SetEnable == NULL) return EFI_NOT_READY;
  /* VEMC is retained and only observed here; never change this shared rail
   * during a failed-resource cleanup retry. */
  Status = ColdValidateVccRetained(Host);
  if (EFI_ERROR(Status)) return Status;
  Status = Host->Clock->IsEnabled(Host->ClockId[5], &IsOn);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (!IsOn) return EFI_NOT_READY;

  /* Restore any missing host gate so the full ten-root proof is readable.
   * Keep ownership until that proof permits release. */
  for (I = 0; I < 5; I++) {
    Status = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
    if (IsOn) continue;

    Host->ClockOwned[I] = TRUE;
    Status = Host->Clock->SetEnable(Host->ClockId[I], TRUE);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    ReadStatus = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(ReadStatus)) return ReadStatus;
    if (!IsOn) return EFI_DEVICE_ERROR;
    if (EFI_ERROR(Status)) return Status;
  }

  Status = ColdValidateResources(Host);
  if (EFI_ERROR(Status)) return Status;
  return ColdVerifyStoppedRoots(Host, FALSE);
}

static VOID ColdFinishCleanup(MTK_UFS_COLD_HOST *Host)
{
  Host->Configured = FALSE;
  Host->RefClockWaitConfigured = FALSE;
  Host->CleanupPending = FALSE;
  Host->CleanupVccOffConfirmed = FALSE;
  Host->CleanupMode = MtkUfsColdCleanupNone;
  /* Shared-resource history and a ReleasedNeedsInit marker are persistent state. */
}

EFI_STATUS MtkUfsReleaseUnstartedColdHost(MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status;
  UINTN I;
  BOOLEAN HasOwnedClock = FALSE;
  BOOLEAN ReleasedRestart = FALSE;
  if (Host == NULL) return EFI_INVALID_PARAMETER;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  ReleasedRestart = (BOOLEAN)(Host->StopPhase == MtkUfsColdStopReleasedNeedsInit &&
                              Host->Platform.State == MtkUfsPlatformStopped);
  if (Host->Signature != MTK_UFS_COLD_HOST_SIGNATURE || !Host->Configured ||
      (Host->Platform.State != MtkUfsPlatformIdle && !ReleasedRestart) ||
      (Host->StopPhase != MtkUfsColdStopNone && !ReleasedRestart) ||
      Host->Interface != NULL || Host->Suspended) return EFI_NOT_READY;

  if (Host->CleanupPending) {
    if (Host->CleanupMode != MtkUfsColdCleanupOwnedOnly &&
        Host->CleanupMode != MtkUfsColdCleanupStoppedHost) return EFI_NOT_READY;
    for (I = 0; I < ARRAY_SIZE(Host->ClockOwned); I++) {
      if (Host->ClockOwned[I]) HasOwnedClock = TRUE;
    }
    if (Host->CleanupMode == MtkUfsColdCleanupOwnedOnly && HasOwnedClock) {
      /* No gate has been released yet. Re-establish powered MMIO and prove all
       * ten HCI roots zero before allowing the first disable. */
      Status = ColdEnsureCleanupProofResources(Host);
      if (EFI_ERROR(Status)) return Status;
      Host->CleanupMode = MtkUfsColdCleanupStoppedHost;
    }
    if (!HasOwnedClock) {
      ColdFinishCleanup(Host);
      return EFI_SUCCESS;
    }
    if (Host->CleanupMode != MtkUfsColdCleanupStoppedHost) return EFI_NOT_READY;
    /* A StoppedHost mode is saved only after the full ten-root proof. Do not
     * touch controller MMIO during retries once any gate may already be off. */
    Status = ColdRollbackResources(Host);
    if (ColdHasOwnedResources(Host)) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
    ColdFinishCleanup(Host);
    return EFI_SUCCESS;
  }

  for (I = 0; I < ARRAY_SIZE(Host->ClockOwned); I++) {
    if (Host->ClockOwned[I]) HasOwnedClock = TRUE;
  }
  if (HasOwnedClock &&
      (Host->Clock == NULL || Host->Clock->SetEnable == NULL || Host->Clock->IsEnabled == NULL)) return EFI_NOT_READY;

  if (HasOwnedClock) {
    /* Software Idle alone does not prove that firmware left the controller
     * stopped. A previously stopped host keeps its ten-root proof marker;
     * a fresh unstarted host must still be powered and have empty queues. */
    if (ReleasedRestart) {
      Status = ColdValidateVccRetained(Host);
      if (!EFI_ERROR(Status)) Status = ColdVerifyStoppedRoots(Host, TRUE);
    } else {
      Status = ColdValidateResources(Host);
      if (!EFI_ERROR(Status)) Status = ColdVerifyStoppedRoots(Host, FALSE);
    }
    if (EFI_ERROR(Status)) return Status;
  } else if (ReleasedRestart) {
    Status = ColdValidateVccRetained(Host);
    if (!EFI_ERROR(Status)) Status = ColdVerifyStoppedRoots(Host, TRUE);
    if (EFI_ERROR(Status)) return Status;
  }

  Host->CleanupMode = MtkUfsColdCleanupStoppedHost;
  Host->CleanupPending = TRUE;
  Host->CleanupVccOffConfirmed = FALSE;
  Status = ColdRollbackResources(Host);
  if (ColdHasOwnedResources(Host)) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
  ColdFinishCleanup(Host);
  return EFI_SUCCESS;
}

EFI_STATUS MtkUfsColdReleaseStartedHost(MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status;

  if (Host == NULL) return EFI_INVALID_PARAMETER;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Signature != MTK_UFS_COLD_HOST_SIGNATURE || Host->Interface != NULL ||
      Host->Suspended || Host->Platform.State != MtkUfsPlatformStopped ||
      Host->Platform.RefClock != MtkUfsRefClockOff ||
      Host->Platform.SramResume != MtkUfsSramNoRestore) return EFI_NOT_READY;

  if (Host->StopPhase == MtkUfsColdStopReleasedNeedsInit && !ColdHasOwnedResources(Host)) {
    return EFI_SUCCESS;
  }
  if (!Host->Configured ||
      (Host->StopPhase != MtkUfsColdStopHceProven &&
       Host->StopPhase != MtkUfsColdStopGateReleasePending) ||
      !Host->StopProofValid || Host->StopGeneration == 0 ||
      Host->StopProofGeneration != Host->StopGeneration) return EFI_NOT_READY;
  if (Host->Clock == NULL || Host->Clock->SetEnable == NULL || Host->Clock->IsEnabled == NULL) return EFI_NOT_READY;

  if (Host->StopPhase == MtkUfsColdStopHceProven) {
    /* This is the only point that reads controller MMIO for the release. Save
     * the proof/pending state before the first gate change so retries never
     * access a controller whose clocks may already be off. */
    Status = ColdValidateResources(Host);
    if (!EFI_ERROR(Status)) Status = ColdVerifyStoppedRoots(Host, TRUE);
    if (EFI_ERROR(Status)) return Status;
    Host->StopPhase = MtkUfsColdStopGateReleasePending;
    Host->CleanupMode = MtkUfsColdCleanupStoppedHost;
    Host->CleanupPending = TRUE;
    Host->CleanupVccOffConfirmed = FALSE;
  } else {
    /* The retained shared rail is checked on each retry, but HCI MMIO is
     * deliberately forbidden after a partial gate release. */
    Status = ColdValidateVccRetained(Host);
    if (EFI_ERROR(Status)) return Status;
  }

  if (Host->ClockOwned[5]) return EFI_NOT_READY;
  Status = ColdRollbackResources(Host);
  if (ColdHasOwnedResources(Host)) return EFI_ERROR(Status) ? Status : EFI_DEVICE_ERROR;
  if (EFI_ERROR(Status)) return Status;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;

  /* Consuming the token is final. Platform.Stopped and VccRetained survive
   * so the next Configure/Start can reacquire only the gates and explicitly
   * RestartStopped after a fresh ten-register proof. */
  Host->StopProofValid = FALSE;
  Host->StopProofGeneration = 0;
  Host->StopPhase = MtkUfsColdStopReleasedNeedsInit;
  ColdFinishCleanup(Host);
  return EFI_SUCCESS;
}

static EFI_STATUS ColdValidateResources(MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status;
  BOOLEAN IsOn;
  UINT64 Frequency;
  UINT32 Voltage;
  UINTN I;
  if (Host == NULL) return EFI_INVALID_PARAMETER;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Signature != MTK_UFS_COLD_HOST_SIGNATURE || !Host->Configured ||
      Host->Clock == NULL || Host->Pmic == NULL || Host->Clock->IsEnabled == NULL ||
      Host->Clock->GetFrequency == NULL || Host->Pmic->RegulatorIsEnabled == NULL ||
      Host->Pmic->RegulatorGetVoltage == NULL) return EFI_INVALID_PARAMETER;

  /* Retained-host readiness is a pure observation: do not restore a resource
   * that is off, since the OS handoff path cannot take ownership of it. */
  for (I = 0; I < ARRAY_SIZE(Host->ClockId); I++) {
    Status = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
    if (!IsOn) return EFI_NOT_READY;
    Status = Host->Clock->GetFrequency(Host->ClockId[I], &Frequency);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
    if (Frequency == 0 || Frequency > 1000000000ULL) return EFI_UNSUPPORTED;
  }
  Status = Host->Pmic->RegulatorIsEnabled(COLD_UFS_PMIC_VCC, &IsOn);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (!IsOn) return EFI_NOT_READY;
  Host->VccRetained = TRUE;
  Status = Host->Pmic->RegulatorGetVoltage(COLD_UFS_PMIC_VCC, &Voltage);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (Voltage < 2900000U || Voltage > 3300000U) return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}

static EFI_STATUS ColdEnableResources(MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status;
  BOOLEAN IsOn;
  UINT32 Voltage;
  UINTN I;
  if (Host == NULL) return EFI_NOT_READY;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Clock == NULL || Host->Pmic == NULL || Host->Clock->SetEnable == NULL ||
      Host->Clock->IsEnabled == NULL || Host->Clock->GetFrequency == NULL ||
      Host->Pmic->RegulatorSetEnable == NULL || Host->Pmic->RegulatorIsEnabled == NULL ||
      Host->Pmic->RegulatorGetVoltage == NULL) return EFI_NOT_READY;
  /* Validate the retained selector before any acquisition, especially before
   * turning an off rail on. A later read-back cannot undo an unsafe voltage. */
  Status = Host->Pmic->RegulatorGetVoltage(COLD_UFS_PMIC_VCC, &Voltage);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (Voltage < 2900000U || Voltage > 3300000U) return EFI_UNSUPPORTED;
  if (Host->StopPhase == MtkUfsColdStopReleasedNeedsInit) {
    /* A stopped cold host retained the shared rail across cleanup. If another
     * controller/external actor removed it, refuse restart before acquiring
     * any UFS gates; the retained VEMC supply policy must be restored. */
    Status = ColdValidateVccRetained(Host);
    if (EFI_ERROR(Status)) return Status;
  }
  Status = Host->Pmic->RegulatorIsEnabled(COLD_UFS_PMIC_VCC, &IsOn);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (IsOn) Host->VccRetained = TRUE;
  if (!IsOn) {
    /* Enable the shared rail before acquiring gates, so partial gate failures
     * still leave powered MMIO available for a stopped-root proof. */
    Host->VccRetained = TRUE;
    Host->VccOwned = TRUE;
    Status = Host->Pmic->RegulatorSetEnable(COLD_UFS_PMIC_VCC, TRUE);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    /* The PMIC wrapper may report a bus error after the enable write landed. */
    if (Host->VccRampDelayUs != 0) MicroSecondDelay(Host->VccRampDelayUs);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
    Status = Host->Pmic->RegulatorIsEnabled(COLD_UFS_PMIC_VCC, &IsOn);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
    if (!IsOn) return EFI_DEVICE_ERROR;
  }

  Status = Host->Clock->IsEnabled(Host->ClockId[5], &IsOn);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status)) return Status;
  if (!IsOn) {
    EFI_STATUS ReadStatus;
    /* The factor inherits its selected parent's gate. Enable through the
     * clock provider, retaining this shared path like VEMC during cleanup. */
    Host->TopClockEnableRequested = TRUE;
    Status = Host->Clock->SetEnable(Host->ClockId[5], TRUE);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    ReadStatus = Host->Clock->IsEnabled(Host->ClockId[5], &IsOn);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
    if (EFI_ERROR(ReadStatus)) return ReadStatus;
    if (!IsOn) return EFI_DEVICE_ERROR;
  }

  /* Only the five leaf gates can be released by this host. */
  for (I = 0; I < 5; I++) {
    Status = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
    if (!IsOn) {
      /* Treat a failed setter as possibly applied; retain ownership until a
       * read-back proves off or a complete stopped-root proof permits release. */
      Host->ClockOwned[I] = TRUE;
      Status = Host->Clock->SetEnable(Host->ClockId[I], TRUE);
      if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
      if (EFI_ERROR(Status)) {
        EFI_STATUS ReadStatus = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
        if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
        if (!EFI_ERROR(ReadStatus) && !IsOn) Host->ClockOwned[I] = FALSE;
        return Status;
      }
      Status = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
      if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
      if (EFI_ERROR(Status)) return Status;
      if (!IsOn) return EFI_DEVICE_ERROR;
    }
  }
  return ColdValidateResources(Host);
}

static EFI_STATUS PrepareColdHost(CONST VOID *Fdt, MTK_UFS_COLD_HOST *Host)
{
  static CONST CHAR8 ClockNames[] = "ufs\0ufs_mp\0unipro_tick\0unipro_sys\0ufs_aes\0ufs_ck";
  static CONST CHAR8 ResetNames[] = "hci_rst\0unipro_rst\0crypto_rst";
  static CONST CHAR8 *Names[] = { "IFRAO_UFS", "IFRAO_UFS_SAP_BCLK", "IFRAO_UNIPRO_TICK", "IFRAO_UNIPRO_SYSCLK", "IFRAO_UFS_AES", "TOP_UFS" };
  static CONST UINT32 ResetBits[] = { 0x120, 0, 0x124, 0, 0, 0, 0x1C, 0x130, 15, 0x134, 15, 0, 0, 0x1C,
    0x140, 7, 0x144, 7, 0, 0, 0x1C, 0x150, 21, 0x154, 21, 0, 0, 0x1C, 0x120, 7, 0x124, 7, 0, 0, 0x1C };
  static CONST UINT32 UfsReg[] = { 0, COLD_UFS_BASE, 0, COLD_UFS_LENGTH };
  static CONST UINT32 InfraReg[] = { 0, COLD_INFRA_BASE, 0, COLD_INFRA_LENGTH };
  static CONST UINT32 TopReg[] = { 0, 0x10000000, 0, 0x1000 };
  MTK_UFS_PLATFORM_CONFIG Config;
  MTK_UFS_PLATFORM_IO Io;
  INT32 Ufs, Infra, Top, Reset, Vcc, Length;
  UINT32 InfraHandle, TopHandle, ResetHandle, Clocks[12], Resets[6], One = 1, Id;
  CONST UINT8 *B;
  UINTN I;
  EFI_STATUS Status;
  BOOLEAN PreserveStopped = FALSE;
  BOOLEAN SavedVccOwned = FALSE;
  BOOLEAN SavedVccRetained = FALSE;
  BOOLEAN SavedTopClockEnableRequested = FALSE;
  BOOLEAN *SavedBootServicesExited;
  UINT32 SavedStopGeneration = 0;
  if (Fdt == NULL || Host == NULL) return EFI_INVALID_PARAMETER;
  SavedBootServicesExited = Host->BootServicesExited;
  if (Host->Signature != 0 && Host->Signature != MTK_UFS_COLD_HOST_SIGNATURE)
    return EFI_INVALID_PARAMETER;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Signature == MTK_UFS_COLD_HOST_SIGNATURE) {
    PreserveStopped = (BOOLEAN)(Host->StopPhase == MtkUfsColdStopReleasedNeedsInit &&
                                Host->Platform.State == MtkUfsPlatformStopped &&
                                Host->Platform.RefClock == MtkUfsRefClockOff &&
                                Host->Platform.SramResume == MtkUfsSramNoRestore);
    if (Host->CleanupPending || Host->CleanupVccOffConfirmed ||
        Host->CleanupMode != MtkUfsColdCleanupNone || Host->Suspended ||
        Host->Interface != NULL ||
        (Host->Platform.State != MtkUfsPlatformIdle && !PreserveStopped) ||
        (Host->StopPhase != MtkUfsColdStopNone && !PreserveStopped) ||
        ColdHasOwnedResources(Host) || Host->ClockOwned[5]) return EFI_NOT_READY;
    SavedVccOwned = Host->VccOwned;
    SavedVccRetained = Host->VccRetained;
    SavedTopClockEnableRequested = Host->TopClockEnableRequested;
    SavedStopGeneration = Host->StopGeneration;
  }
  ZeroMem(Host, sizeof(*Host));
  Host->Signature = MTK_UFS_COLD_HOST_SIGNATURE;
  Host->VccOwned = SavedVccOwned;
  Host->VccRetained = SavedVccRetained;
  Host->TopClockEnableRequested = SavedTopClockEnableRequested;
  Host->StopGeneration = SavedStopGeneration;
  Host->StopPhase = PreserveStopped ? MtkUfsColdStopReleasedNeedsInit : MtkUfsColdStopNone;
  Host->BootServicesExited = SavedBootServicesExited;
  if (PreserveStopped) {
    Host->Platform.State = MtkUfsPlatformStopped;
    Host->Platform.RefClock = MtkUfsRefClockOff;
    Host->Platform.SramResume = MtkUfsSramNoRestore;
  }
  if (FdtTotalSize(Fdt) > 0x200000U || FdtCheckFull(Fdt, FdtTotalSize(Fdt)) != 0) return EFI_COMPROMISED_DATA;
  Ufs = FdtPathOffset(Fdt, "/soc/ufshci@11270000");
  Infra = FdtPathOffset(Fdt, "/syscon@10001000");
  Top = FdtPathOffset(Fdt, "/syscon@10000000");
  Reset = FdtPathOffset(Fdt, "/syscon@10001000/reset-controller");
  if (Ufs < 0 || Infra < 0 || Top < 0 || Reset < 0 || !Compatible(Fdt, 0, "mediatek,MT6789") ||
      !Compatible(Fdt, Ufs, "mediatek,mt8183-ufshci") || !Compatible(Fdt, Infra, "mediatek,mt6789-infracfg_ao") ||
      !Compatible(Fdt, Top, "mediatek,mt6789-topckgen") || !Compatible(Fdt, Reset, "ti,syscon-reset") ||
      !Enabled(Fdt, Ufs) || !Enabled(Fdt, Infra) || !Enabled(Fdt, Top) || !Enabled(Fdt, Reset) ||
      !CellsEqual(Fdt, Ufs, "reg", UfsReg, 4) || !CellsEqual(Fdt, Infra, "reg", InfraReg, 4) ||
      !CellsEqual(Fdt, Top, "reg", TopReg, 4) || !CellsEqual(Fdt, Infra, "#clock-cells", &One, 1) ||
      !CellsEqual(Fdt, Top, "#clock-cells", &One, 1) || !CellsEqual(Fdt, Reset, "#reset-cells", &One, 1) ||
      !CellsEqual(Fdt, Reset, "ti,reset-bits", ResetBits, ARRAY_SIZE(ResetBits)) ||
      !StringEquals(Fdt, Ufs, "clock-names", ClockNames, sizeof(ClockNames)) ||
      !StringEquals(Fdt, Ufs, "reset-names", ResetNames, sizeof(ResetNames)) ||
      FdtGetProp(Fdt, Ufs, "phys", &Length) != NULL) return EFI_UNSUPPORTED;
  InfraHandle = FdtGetPhandle(Fdt, Infra); TopHandle = FdtGetPhandle(Fdt, Top); ResetHandle = FdtGetPhandle(Fdt, Reset);
  if (InfraHandle == 0 || TopHandle == 0 || ResetHandle == 0 || InfraHandle == TopHandle) return EFI_UNSUPPORTED;
  for (I = 0; I < 6; I++) Clocks[I * 2] = I == 5 ? TopHandle : InfraHandle;
  Clocks[1] = 0x2F; Clocks[3] = 0x2B; Clocks[5] = 0x2A; Clocks[7] = 0x29; Clocks[9] = 0x30; Clocks[11] = 0x27;
  for (I = 0; I < 3; I++) { Resets[I * 2] = ResetHandle; Resets[I * 2 + 1] = (UINT32)I + 1; }
  if (!CellsEqual(Fdt, Ufs, "clocks", Clocks, 12) || !CellsEqual(Fdt, Ufs, "resets", Resets, 6)) return EFI_UNSUPPORTED;
  B = FdtGetProp(Fdt, Ufs, "vcc-supply", &Length);
  if (B == NULL || Length != 4) return EFI_UNSUPPORTED;
  Vcc = FdtNodeOffsetByPhandle(Fdt, Cell(B));
  if (Vcc < 0 || !Enabled(Fdt, Vcc) || !StringEquals(Fdt, Vcc, "regulator-name", "vemc", sizeof("vemc"))) return EFI_UNSUPPORTED;
  Id = 2900000;
  if (!CellsEqual(Fdt, Vcc, "regulator-min-microvolt", &Id, 1)) return EFI_UNSUPPORTED;
  Id = 3300000;
  if (!CellsEqual(Fdt, Vcc, "regulator-max-microvolt", &Id, 1)) return EFI_UNSUPPORTED;
  B = FdtGetProp(Fdt, Vcc, "regulator-enable-ramp-delay", &Length);
  if (B == NULL || Length != 4 || Cell(B) != 60U) return EFI_UNSUPPORTED;
  Host->VccRampDelayUs = Cell(B);
  ZeroMem(&Config, sizeof(Config));
  B = FdtGetProp(Fdt, Ufs, "id", &Length);
  if (B != NULL) {
    if (Length != 4 || Cell(B) != 0) return EFI_UNSUPPORTED;
  } else if (Length != -FDT_ERR_NOTFOUND) return EFI_COMPROMISED_DATA;
  Config.ControllerLength = COLD_UFS_LENGTH; Config.ResetLength = COLD_INFRA_LENGTH;
  Config.RefClockUngatingWaitUs = 32;
  Config.AllowSramRetention = 1;
  for (I = 0; I < 3; I++) {
    UINTN R = (I + 1) * 7;
    Config.Reset[I].AssertOffset = ResetBits[R]; Config.Reset[I].DeassertOffset = ResetBits[R + 2];
    Config.Reset[I].Mask = 1U << ResetBits[R + 1];
  }
  if (!Mapped(COLD_UFS_BASE, COLD_UFS_LENGTH) || !Mapped(COLD_INFRA_BASE, COLD_INFRA_LENGTH) ||
      !Mapped(0x10000000, 0x1000)) return EFI_UNSUPPORTED;
  Status = gBS->LocateProtocol(&gMediaTekClockProtocolGuid, NULL, (VOID **)&Host->Clock);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status) || Host->Clock == NULL || Host->Clock->GetId == NULL || Host->Clock->IsEnabled == NULL || Host->Clock->GetFrequency == NULL) return EFI_NOT_READY;
  Status = gBS->LocateProtocol(&gMediaTekPmicProtocolGuid, NULL, (VOID **)&Host->Pmic);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (EFI_ERROR(Status) || Host->Pmic == NULL || Host->Pmic->RegulatorIsEnabled == NULL || Host->Pmic->RegulatorGetVoltage == NULL) return EFI_NOT_READY;
  for (I = 0; I < ARRAY_SIZE(Names); I++) {
    Status = Host->Clock->GetId(Names[I], &Host->ClockId[I]);
    if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
    if (EFI_ERROR(Status)) return Status;
  }
  ZeroMem(&Io, sizeof(Io));
  Io.Context = Host; Io.Read32 = ColdRead; Io.Write32 = ColdWrite; Io.DelayUs = ColdDelay;
  Io.Smc = ColdSmc; Io.Dme = ColdDme; Io.Hibernate = ColdHibernate;
  Status = ToEfi(MtkUfsPlatformInitialize(&Host->Platform, &Io, &Config));
  if (EFI_ERROR(Status)) return Status;
  if (PreserveStopped) {
    /* Keep an explicit stopped marker. Only RestartStopped may reinitialize
     * the platform after gates are reacquired and all roots read back zero. */
    Host->Platform.State = MtkUfsPlatformStopped;
    Host->Platform.RefClock = MtkUfsRefClockOff;
    Host->Platform.SramResume = MtkUfsSramNoRestore;
  }
  Host->Configured = TRUE;
  return EFI_SUCCESS;
}

static EFI_STATUS ColdValidateIpVersion(MTK_UFS_COLD_HOST *Host)
{
  UINT32 Raw;
  MTK_UFS_PLATFORM_STATUS Status;
  if (Host == NULL || ColdBootServicesExited(Host) || !Host->Configured) return EFI_UNSUPPORTED;
  Status = Host->Platform.Io.Read32(Host->Platform.Io.Context, MtkUfsControllerBank, 0x2240U, &Raw);
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Status != MtkUfsPlatformOk) return EFI_DEVICE_ERROR;
  if (Raw == 0 || Raw == MAX_UINT32) return EFI_DEVICE_ERROR;
  /* Apply the shipped IP encoding used by the reset/pre-HCE sequence. */
  if (((Raw | 0x01000000U) >> 24) == 1U) Raw = (Raw & 0x00FFFFFFU) | 0x10000000U;
  return Raw == 0x10480000U ? EFI_UNSUPPORTED : EFI_SUCCESS;
}

EFI_STATUS MtkUfsConfigureColdHost(CONST VOID *Fdt, MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status;
  if (Host == NULL) return EFI_INVALID_PARAMETER;
  Status = PrepareColdHost(Fdt, Host);
  if (EFI_ERROR(Status)) return Status;
  if (Host->Clock->SetEnable == NULL || Host->Pmic->RegulatorSetEnable == NULL) {
    ColdFinishCleanup(Host);
    return EFI_NOT_READY;
  }
  Status = ColdEnableResources(Host);
  if (EFI_ERROR(Status)) {
    EFI_STATUS CleanupStatus;
    Host->CleanupMode = MtkUfsColdCleanupOwnedOnly;
    Host->CleanupPending = TRUE;
    Host->CleanupVccOffConfirmed = FALSE;
    if (!ColdHasOwnedResources(Host)) {
      ColdFinishCleanup(Host);
      return Status;
    }
    /* Never disable a gate until powered MMIO and all ten stopped roots have
     * been independently proven. Keep ownership pending if that cannot be read. */
    CleanupStatus = ColdEnsureCleanupProofResources(Host);
    if (!EFI_ERROR(CleanupStatus)) {
      Host->CleanupMode = MtkUfsColdCleanupStoppedHost;
      CleanupStatus = ColdRollbackResources(Host);
    }
    if (ColdHasOwnedResources(Host)) return EFI_ERROR(CleanupStatus) ? CleanupStatus : EFI_DEVICE_ERROR;
    ColdFinishCleanup(Host);
    return Status;
  }
  Status = ColdValidateIpVersion(Host);
  if (EFI_ERROR(Status)) {
    /* The IP read follows full resource acquisition. An unsupported IP does
     * not prove HCE/queues are stopped: use the guarded release and preserve
     * ownership if the newly accessible host might still be running. */
    EFI_STATUS Cleanup = MtkUfsReleaseUnstartedColdHost(Host);
    return EFI_ERROR(Cleanup) ? EFI_DEVICE_ERROR : Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS MtkUfsValidateRetainedHost(CONST VOID *Fdt, MTK_UFS_COLD_HOST *Host)
{
  EFI_STATUS Status;
  if (Host == NULL) return EFI_INVALID_PARAMETER;
  Status = PrepareColdHost(Fdt, Host);
  if (EFI_ERROR(Status)) return Status;
  Status = ColdValidateResources(Host);
  if (!EFI_ERROR(Status)) Status = ColdValidateIpVersion(Host);
  if (EFI_ERROR(Status)) {
    /* This is a probe only; there is nothing to unwind or disable. */
    Host->Configured = FALSE;
    return Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS MtkUfsColdHostCallback(MTK_UFS_COLD_HOST *Host, EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase,
                                EDKII_UFS_HC_DRIVER_INTERFACE *Interface)
{
  MTK_UFS_PLATFORM_STATUS Status;
  EFI_STATUS ResourceStatus;
  if (Host == NULL || !Host->Configured || Interface == NULL || Interface->UfsHcProtocol == NULL ||
      Interface->UfsExecUicCommand == NULL || (UINT32)Phase > EdkiiUfsHcHostStopped) return EFI_INVALID_PARAMETER;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->CleanupPending || Host->CleanupVccOffConfirmed || Host->Suspended ||
      Host->Interface != NULL) return EFI_NOT_READY;
  if ((UINT32)Phase <= (UINT32)EdkiiUfsHcPostLinkStartup &&
      Host->StopPhase != MtkUfsColdStopNone &&
      Host->StopPhase != MtkUfsColdStopReleasedNeedsInit) return EFI_NOT_READY;
  if ((Phase == EdkiiUfsHcPrepareStop || Phase == EdkiiUfsHcHostStopped) &&
      Host->StopPhase == MtkUfsColdStopReleasedNeedsInit) return EFI_NOT_READY;
  if (Phase == EdkiiUfsHcPrepareStop && !Host->RefClockWaitConfigured) return EFI_NOT_READY;
  ResourceStatus = ColdValidateResources(Host);
  if (EFI_ERROR(ResourceStatus)) return ResourceStatus;
  Host->Interface = Interface;
  switch (Phase) {
    case EdkiiUfsHcPreHce:
      if (Host->Platform.State != MtkUfsPlatformIdle) {
        Status = MtkUfsPlatformRestartStopped(&Host->Platform);
      } else {
        Status = MtkUfsPlatformResetDevice(&Host->Platform);
        if (Status == MtkUfsPlatformOk) Status = MtkUfsPlatformSetRefClock(&Host->Platform, 1);
        if (Status == MtkUfsPlatformOk) Status = MtkUfsPlatformPreHce(&Host->Platform);
      }
      if (Status == MtkUfsPlatformOk) {
        Host->StopPhase = MtkUfsColdStopNone;
        Host->StopProofValid = FALSE;
        Host->StopProofGeneration = 0;
      }
      break;
    case EdkiiUfsHcPostHce: Status = MtkUfsPlatformPostHce(&Host->Platform); break;
    case EdkiiUfsHcPreLinkStartup: Status = MtkUfsPlatformPreLink(&Host->Platform); break;
    case EdkiiUfsHcPostLinkStartup: Status = MtkUfsPlatformPostLink(&Host->Platform); break;
    case EdkiiUfsHcPrepareStop:
      if (Host->StopPhase != MtkUfsColdStopNone ||
          Host->Platform.State != MtkUfsPlatformLinkReady ||
          Host->StopGeneration == MAX_UINT32) {
        Status = MtkUfsPlatformBusy;
        break;
      }
      Host->StopProofValid = FALSE;
      Host->StopProofGeneration = 0;
      Status = MtkUfsPlatformPrepareStop(&Host->Platform);
      if (Status == MtkUfsPlatformOk) {
        Host->StopGeneration++;
        Host->StopPhase = MtkUfsColdStopPrepared;
      }
      break;
    case EdkiiUfsHcHostStopped:
      if ((Host->StopPhase != MtkUfsColdStopPrepared &&
           Host->StopPhase != MtkUfsColdStopHceProven) || Host->StopGeneration == 0) {
        Status = MtkUfsPlatformWrongState;
        break;
      }
      Status = MtkUfsPlatformHostStopped(&Host->Platform);
      if (Status == MtkUfsPlatformOk) {
        Host->StopProofGeneration = Host->StopGeneration;
        Host->StopProofValid = TRUE;
        Host->StopPhase = MtkUfsColdStopHceProven;
      }
      break;
    default: Status = MtkUfsPlatformInvalid;
  }
  /* UfsPassThru owns its interface; never retain its stack/lifetime on return. */
  Host->Interface = NULL;
  return ToEfi(Status);
}


EFI_STATUS MtkUfsColdHostSetManufacturer(MTK_UFS_COLD_HOST *Host, UINT16 ManufacturerId)
{
  if (Host == NULL) return EFI_INVALID_PARAMETER;
  if (!Host->Configured || Host->Suspended ||
      Host->Platform.State != MtkUfsPlatformLinkReady) return EFI_NOT_READY;
  /* These are the exact per-vendor refclock delays used by the shipped
   * ufs_mtk_apply_dev_quirks(); unknown IDs use its default. */
  if (ManufacturerId == 0x01CEU) Host->Platform.Config.RefClockGatingWaitUs = 1U;       /* Samsung */
  else if (ManufacturerId == 0x01ADU) Host->Platform.Config.RefClockGatingWaitUs = 30U; /* SK hynix */
  else if (ManufacturerId == 0x0198U) Host->Platform.Config.RefClockGatingWaitUs = 100U; /* Toshiba */
  else Host->Platform.Config.RefClockGatingWaitUs = 32U;
  Host->RefClockWaitConfigured = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS MtkUfsColdHostSuspend(MTK_UFS_COLD_HOST *Host, EDKII_UFS_HC_DRIVER_INTERFACE *Interface)
{
  BOOLEAN Disabled[6] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE };
  EFI_STATUS Status, Cleanup = EFI_SUCCESS;
  BOOLEAN IsOn;
  INTN I;

  if (Host == NULL) return EFI_INVALID_PARAMETER;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Signature != MTK_UFS_COLD_HOST_SIGNATURE || !Host->Configured || Host->Suspended ||
      Interface == NULL || Interface->UfsHcProtocol == NULL || Interface->UfsExecUicCommand == NULL ||
      Host->Clock == NULL) return EFI_INVALID_PARAMETER;
  if (!Host->RefClockWaitConfigured) return EFI_NOT_READY;
  Status = ColdValidateResources(Host);
  if (EFI_ERROR(Status)) return Status;
  Host->Interface = Interface;
  Status = ToEfi(MtkUfsPlatformSuspend(&Host->Platform));
  if (ColdBootServicesExited(Host)) {
    Host->Suspended = TRUE;
    Status = EFI_UNSUPPORTED;
    goto Done;
  }
  if (EFI_ERROR(Status)) goto Done;

  /* Linux releases the six DT clocks after Hibern8/refclock handling. TOP_UFS
   * inherits a shared parent that must remain enabled; only the five gates
   * acquired by this host are released here. VEMC
   * remains on: this is a retained-link clock suspend, not full device power-off. */
  for (I = 0; I < 5; I++) {
    if (!Host->ClockOwned[I]) continue;
    Status = Host->Clock->SetEnable(Host->ClockId[I], FALSE);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(Status)) {
      EFI_STATUS ReadStatus = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
      if (ColdBootServicesExited(Host)) {
        Host->Suspended = TRUE;
        Status = EFI_UNSUPPORTED;
        goto Done;
      }
      if (EFI_ERROR(ReadStatus) || !IsOn) Disabled[I] = TRUE;
      goto Restore;
    }
    Status = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(Status)) {
      Disabled[I] = TRUE;
      goto Restore;
    }
    if (IsOn) {
      Status = EFI_DEVICE_ERROR;
      goto Restore;
    }
    Disabled[I] = TRUE;
  }
  Host->Suspended = TRUE;
  Status = EFI_SUCCESS;
  goto Done;

Restore:
  for (I = 4; I >= 0; I--) {
    if (!Disabled[I]) continue;
    EFI_STATUS RestoreStatus = Host->Clock->SetEnable(Host->ClockId[I], TRUE);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(RestoreStatus)) Cleanup = EFI_DEVICE_ERROR;
    RestoreStatus = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(RestoreStatus) || !IsOn) Cleanup = EFI_DEVICE_ERROR;
  }
  if (Host->Platform.State == MtkUfsPlatformLinkSuspended) {
    /* A failed gate restoration may leave MMIO inaccessible. Preserve the
     * suspended state for an explicit retry instead of touching the HCI. */
    EFI_STATUS ResumeStatus = ColdValidateResources(Host);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (!EFI_ERROR(ResumeStatus)) ResumeStatus = ToEfi(MtkUfsPlatformResume(&Host->Platform));
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(ResumeStatus)) {
      Host->Suspended = TRUE;
      Cleanup = EFI_DEVICE_ERROR;
    }
  }
  if (EFI_ERROR(Cleanup)) Status = EFI_DEVICE_ERROR;

Done:
  Host->Interface = NULL;
  return Status;
}

EFI_STATUS MtkUfsColdHostResume(MTK_UFS_COLD_HOST *Host, EDKII_UFS_HC_DRIVER_INTERFACE *Interface)
{
  BOOLEAN Enabled[6] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE };
  EFI_STATUS Status, Cleanup = EFI_SUCCESS, ReadStatus;
  BOOLEAN IsOn;
  INTN I;

  if (Host == NULL) return EFI_INVALID_PARAMETER;
  if (ColdBootServicesExited(Host)) return EFI_UNSUPPORTED;
  if (Host->Signature != MTK_UFS_COLD_HOST_SIGNATURE || !Host->Configured || !Host->Suspended ||
      Interface == NULL || Interface->UfsHcProtocol == NULL || Interface->UfsExecUicCommand == NULL ||
      Host->Clock == NULL) return EFI_INVALID_PARAMETER;
  if (Host->Platform.State != MtkUfsPlatformLinkSuspended ||
      Host->Platform.RefClock != MtkUfsRefClockOff) return EFI_NOT_READY;
  Host->Interface = Interface;
  for (I = 0; I < 5; I++) {
    if (!Host->ClockOwned[I]) continue;
    Status = Host->Clock->SetEnable(Host->ClockId[I], TRUE);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    /* A setter can fail after its write landed. Read back even on failure and
     * conservatively disable if the read itself is inconclusive. */
    ReadStatus = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(ReadStatus) || IsOn) Enabled[I] = TRUE;
    if (EFI_ERROR(Status)) goto Rollback;
    if (EFI_ERROR(ReadStatus)) { Status = ReadStatus; goto Rollback; }
    if (!IsOn) { Status = EFI_DEVICE_ERROR; goto Rollback; }
  }
  /* Verify borrowed gates and the retained VEMC rail before the first HCI/UIC
   * access. On drift, roll back only the gates this resume just enabled. */
  Status = ColdValidateResources(Host);
  if (ColdBootServicesExited(Host)) {
    Host->Suspended = TRUE;
    Status = EFI_UNSUPPORTED;
    goto Done;
  }
  if (EFI_ERROR(Status)) goto Rollback;
  /* Once link resume starts, leave clocks on after failure: the controller may
   * need them for diagnosis/recovery and the platform core records Fault. */
  Status = ToEfi(MtkUfsPlatformResume(&Host->Platform));
  if (ColdBootServicesExited(Host)) {
    Host->Suspended = TRUE;
    Status = EFI_UNSUPPORTED;
  } else if (!EFI_ERROR(Status)) Host->Suspended = FALSE;
  goto Done;

Rollback:
  for (I = 4; I >= 0; I--) {
    if (!Enabled[I]) continue;
    EFI_STATUS DisableStatus = Host->Clock->SetEnable(Host->ClockId[I], FALSE);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(DisableStatus)) Cleanup = EFI_DEVICE_ERROR;
    ReadStatus = Host->Clock->IsEnabled(Host->ClockId[I], &IsOn);
    if (ColdBootServicesExited(Host)) {
      Host->Suspended = TRUE;
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
    if (EFI_ERROR(ReadStatus) || IsOn) Cleanup = EFI_DEVICE_ERROR;
  }
  if (EFI_ERROR(Cleanup)) Status = EFI_DEVICE_ERROR;

Done:
  Host->Interface = NULL;
  return Status;
}
