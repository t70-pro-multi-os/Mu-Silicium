/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include "CaptureBmp.h"
#include "VisualRows.h"
#include "SdFatExport.h"
#include "ManualExportChoice.h"
#include <Protocol/Usb2HostController.h>
#include <Protocol/UsbIo.h>
#include <Protocol/SdMmcPassThru.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsDeviceConfig.h>
#include <Protocol/ScsiIo.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>
#include <IndustryStandard/Ufs.h>
#include <Protocol/T70UsbStorageDiagnostics.h>
#include <Protocol/T70UsbTypecDiagnostics.h>
#include <Protocol/T70UfsReadOnlyBlockIoDiagnostics.h>
#include <Protocol/MsdcDiagnostics.h>
#include <Protocol/StockPowerReadOnly.h>
#include <Protocol/MtkPmicGauge.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DevicePathLib.h>
#include <Library/CacheMaintenanceLib.h>
#include "T70RamTestCore.h"

#define LOG_CHARS 32768
#define LINE_MAX 256
#define SUMMARY_LINES 6
#define SUMMARY_CHARS 1024
#define SCAN_CONNECT_USB       0x01U
#define SCAN_CONNECT_INTERNAL  0x02U
#define SCAN_TOPOLOGY_UPDATE   0x04U
#define UFS_UNIT_DESCRIPTOR_MAX_SIZE 255U
/* Exportable RAM text: no serial strings, disk contents, user filenames or SIM IDs. */
CHAR16 mDeviceCheckLog[LOG_CHARS];
UINTN mDeviceCheckLogChars;
STATIC UINTN mLines[LINE_MAX], mLineCount;
STATIC BOOLEAN mTruncated;
STATIC BOOLEAN mWritingSummary;
STATIC EFI_GUID mStorageDiagGuid = T70_USB_STORAGE_DIAGNOSTICS_GUID;
STATIC EFI_GUID mUfsDiagnosticsGuid = T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_GUID;
STATIC T70_RAM_TEST_LEASE mRamPendingLease;
STATIC EFI_STATUS mRamLastFreeStatus = EFI_SUCCESS;
STATIC EFI_STATUS mGaugeStatus = EFI_NOT_STARTED;
STATIC UINT16 mGaugeRaw;
STATIC EFI_STATUS mBatteryAdcStatus[] = { EFI_NOT_STARTED, EFI_NOT_STARTED };
STATIC UINT16 mBatteryAdcSample[ARRAY_SIZE (mBatteryAdcStatus)];
STATIC BOOLEAN mBatteryVoltageCalibrated;
STATIC BOOLEAN mBatteryTemperatureCalibrated;

STATIC VOID Line (CONST CHAR16 *Format, ...) {
  VA_LIST Args;
  UINTN Written;
  UINTN MaxLines = mWritingSummary ? LINE_MAX : LINE_MAX - SUMMARY_LINES;
  UINTN MaxChars = mWritingSummary ? LOG_CHARS : LOG_CHARS - SUMMARY_CHARS;
  if (mTruncated && !mWritingSummary) return;
  if (mLineCount >= MaxLines || mDeviceCheckLogChars + 256 > MaxChars) {
    mTruncated = TRUE; return;
  }
  mLines[mLineCount++] = mDeviceCheckLogChars;
  VA_START (Args, Format);
  Written = UnicodeVSPrint (mDeviceCheckLog + mDeviceCheckLogChars,
              (MaxChars - mDeviceCheckLogChars - 2) * sizeof (CHAR16), Format, Args);
  VA_END (Args);
  mDeviceCheckLogChars += Written;
  mDeviceCheckLog[mDeviceCheckLogChars++] = L'\r';
  mDeviceCheckLog[mDeviceCheckLogChars++] = L'\n';
  mDeviceCheckLog[mDeviceCheckLogChars] = 0;
}

STATIC EFI_HANDLE *Handles (EFI_GUID *Guid, UINTN *Count) {
  EFI_HANDLE *Result = NULL;
  *Count = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, Guid, NULL, Count, &Result))) *Count = 0;
  return Result;
}

STATIC BOOLEAN MessagePath (EFI_HANDLE Handle, UINT8 Type) {
  EFI_DEVICE_PATH_PROTOCOL *P = DevicePathFromHandle (Handle);
  if (P == NULL || !IsDevicePathValid (P, 0)) return FALSE;
  for (; !IsDevicePathEnd (P); P = NextDevicePathNode (P))
    if (DevicePathType (P) == MESSAGING_DEVICE_PATH &&
        (DevicePathSubType (P) == Type ||
         (Type == MSG_USB_DP && DevicePathSubType (P) == MSG_USB_CLASS_DP))) return TRUE;
  return FALSE;
}
#define UsbPath(H) MessagePath ((H), MSG_USB_DP)
#define SdPath(H) MessagePath ((H), MSG_SD_DP)
#define UfsPath(H) MessagePath ((H), MSG_UFS_DP)

STATIC CONST CHAR16 *StorageBus (EFI_HANDLE Handle) {
  if (UsbPath (Handle)) return L"USB";
  if (SdPath (Handle)) return L"SD";
  if (UfsPath (Handle)) return L"UFS";
  return L"other";
}

STATIC CONST CHAR16 *MsdcStageName (UINT32 Stage) {
  switch (Stage) {
  case MsdcDiagStageNone: return L"none";
  case MsdcDiagStageDriverConfig: return L"driver-config";
  case MsdcDiagStageCardDetect: return L"card-detect";
  case MsdcDiagStagePrivateAllocation: return L"private-alloc";
  case MsdcDiagStageDevicePathAllocation: return L"path-alloc";
  case MsdcDiagStageHostMmioLookup: return L"host-mmio";
  case MsdcDiagStageTopMmioLookup: return L"top-mmio";
  case MsdcDiagStageGpioSetup: return L"gpio-setup";
  case MsdcDiagStageClockEnable: return L"clock-enable";
  case MsdcDiagStageControllerReset: return L"controller-reset";
  case MsdcDiagStageFifoClear: return L"fifo-clear";
  case MsdcDiagStageHostConfiguration: return L"host-config";
  case MsdcDiagStagePollSetup: return L"poll-setup";
  case MsdcDiagStageControllerPublish: return L"controller-publish";
  case MsdcDiagStageCardPower: return L"card-power";
  case MsdcDiagStageIdentificationClock: return L"identify-clock";
  case MsdcDiagStageCardReset: return L"card-reset";
  case MsdcDiagStageInterfaceCondition: return L"interface-condition";
  case MsdcDiagStageOperatingCondition: return L"operating-condition";
  case MsdcDiagStageCid: return L"cid";
  case MsdcDiagStageRca: return L"rca";
  case MsdcDiagStageCapacity: return L"capacity";
  case MsdcDiagStageBusMode: return L"bus-mode";
  case MsdcDiagStageFinalCardDetect: return L"final-detect";
  case MsdcDiagStageReady: return L"ready";
  default: return L"unknown";
  }
}

STATIC EFI_STATUS MsdcGetSnapshot (MSDC_DIAGNOSTICS_RECORD *Snapshot) {
  MSDC_DIAGNOSTICS_PROTOCOL *Protocol;
  EFI_STATUS Status;
  UINT32 RequiredSize = 0;

  Status = gBS->LocateProtocol (&gMediaTekMsdcDiagnosticsProtocolGuid, NULL,
                                (VOID **)&Protocol);
  if (EFI_ERROR (Status)) return Status;
  ZeroMem (Snapshot, sizeof (*Snapshot));
  Status = Protocol->GetSnapshot (Protocol, sizeof (*Snapshot), &RequiredSize,
                                  Snapshot);
  if (EFI_ERROR (Status)) return Status;
  if ((RequiredSize != sizeof (*Snapshot)) ||
      (Snapshot->Revision != MSDC_DIAGNOSTICS_REVISION) ||
      (Snapshot->Size != sizeof (*Snapshot))) return EFI_INCOMPATIBLE_VERSION;
  return EFI_SUCCESS;
}

STATIC VOID MsdcDiagnosticHistory (VOID) {
  MSDC_DIAGNOSTICS_RECORD Snapshot;
  EFI_STATUS Status = MsdcGetSnapshot (&Snapshot);
  CONST CHAR16 *Detected;

  if (EFI_ERROR (Status)) {
    Line (L"SD cached snapshot: %r", Status);
    return;
  }
  Detected = !Snapshot.DetectSampleValid ? L"unknown" :
             (Snapshot.CardPresent ? L"present" : L"absent");
  Line (L"SD host%u detect=%s/%r samples=%u path=%r",
        Snapshot.HostIndex, Detected, Snapshot.CardDetectStatus,
        Snapshot.DetectSamples, Snapshot.DevicePathStatus);
  Line (L"SD driver=%s/%r init=%s/%r ready=%u busy=%u attempts=%u fail=%u",
        MsdcStageName (Snapshot.LastDriverStage), Snapshot.LastDriverStatus,
        MsdcStageName (Snapshot.LastInitStage), Snapshot.LastInitStatus,
        Snapshot.MediaReady, Snapshot.Initializing,
        Snapshot.IdentifyAttempts, Snapshot.IdentifyFailures);
  if (Snapshot.MediaReady)
    Line (L"SD media type=%u last=%lx", Snapshot.CardType, Snapshot.LastBlock);
}

STATIC VOID SdHosts (BOOLEAN Connect) {
  UINTN N, I, Attempts; EFI_HANDLE *H = Handles (&gEfiSdMmcPassThruProtocolGuid, &N);
  Line (L"SD/MMC host protocols: %u", N);
  for (I = 0; I < N; I++) {
    EFI_SD_MMC_PASS_THRU_PROTOCOL *P; UINT8 Slot = 0xFF; EFI_STATUS S;
    if (Connect) {
      S = gBS->ConnectController (H[I], NULL, NULL, TRUE);
      Line (L"SD host connect H%p: %r", H[I], S);
    }
    S = gBS->HandleProtocol (H[I], &gEfiSdMmcPassThruProtocolGuid, (VOID **)&P);
    if (EFI_ERROR (S)) { Line (L"SD host H%p: %r", H[I], S); continue; }
    for (Attempts = 0; Attempts < 256; Attempts++) {
      EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
      S = P->GetNextSlot (P, &Slot);
      if (S == EFI_NOT_FOUND) break;
      if (EFI_ERROR (S)) { Line (L"SD slot query: %r", S); break; }
      S = P->BuildDevicePath (P, Slot, &Path);
      Line (L"SD host H%p slot=%u media-path=%r", H[I], Slot, S);
      if (S == EFI_NOT_FOUND)
        Line (L"SD slot=%u path missing: card absent or initialization not ready", Slot);
      else if (EFI_ERROR (S))
        Line (L"SD slot=%u path query failed: %r", Slot, S);
      if (Path != NULL) FreePool (Path);
    }
  }
  if (H != NULL) FreePool (H);
}

STATIC CONST CHAR16 *UfsDiagStageName (UINT32 Stage) {
  switch (Stage) {
  case T70UfsDiagStageScsiIo: return L"scsi-io";
  case T70UfsDiagStageDevicePath: return L"device-path";
  case T70UfsDiagStageLuPath: return L"lu-path";
  case T70UfsDiagStageDeviceType: return L"device-type";
  case T70UfsDiagStageDeviceLocation: return L"location";
  case T70UfsDiagStageTarget: return L"target";
  case T70UfsDiagStageLun: return L"lun";
  case T70UfsDiagStageExistingBlockIo: return L"existing-blockio";
  case T70UfsDiagStageSupported: return L"supported";
  case T70UfsDiagStageOpenScsi: return L"open-scsi";
  case T70UfsDiagStageIoAlignment: return L"io-align";
  case T70UfsDiagStageCapacityAllocate: return L"capacity-alloc";
  case T70UfsDiagStageCapacity10: return L"capacity10";
  case T70UfsDiagStageCapacity10Decode: return L"capacity10-decode";
  case T70UfsDiagStageCapacity16: return L"capacity16";
  case T70UfsDiagStageCapacity16Decode: return L"capacity16-decode";
  case T70UfsDiagStageCapacityFree: return L"capacity-free";
  case T70UfsDiagStageGeometry: return L"geometry";
  case T70UfsDiagStageInstallBlockIo: return L"install-blockio";
  case T70UfsDiagStageReady: return L"ready";
  default: return L"not-attempted";
  }
}

STATIC VOID UfsUnitInfo (EFI_HANDLE Handle, BOOLEAN Connect) {
  EFI_SCSI_IO_PROTOCOL *Scsi;
  UINT8 DeviceType = 0xFF, Target[TARGET_MAX_BYTES], LunBytes[sizeof (UINT64)];
  UINT8 *TargetPointer = Target;
  UINT64 Lun = 0;
  BOOLEAN TargetZero = FALSE, LunValid = FALSE;
  EFI_STATUS ScsiStatus, TypeStatus = EFI_NOT_READY, LocationStatus = EFI_NOT_READY, ConnectStatus = EFI_NOT_STARTED;
  UINTN Index;

  ZeroMem (Target, sizeof (Target));
  ZeroMem (LunBytes, sizeof (LunBytes));
  ScsiStatus = gBS->HandleProtocol (Handle, &gEfiScsiIoProtocolGuid, (VOID **)&Scsi);
  if (!EFI_ERROR (ScsiStatus)) {
    TypeStatus = Scsi->GetDeviceType (Scsi, &DeviceType);
    LocationStatus = Scsi->GetDeviceLocation (Scsi, &TargetPointer, &Lun);
    if (!EFI_ERROR (LocationStatus)) {
      TargetZero = TRUE;
      for (Index = 0; Index < TARGET_MAX_BYTES; Index++)
        if (Target[Index] != 0U) TargetZero = FALSE;
      CopyMem (LunBytes, &Lun, sizeof (LunBytes));
      LunValid = (BOOLEAN)(LunBytes[0] == 0U && LunBytes[2] == 0U &&
                           LunBytes[3] == 0U && LunBytes[4] == 0U &&
                           LunBytes[5] == 0U && LunBytes[6] == 0U &&
                           LunBytes[7] == 0U);
    }
  }
  if (Connect) ConnectStatus = gBS->ConnectController (Handle, NULL, NULL, TRUE);
  Line (L"UFS child H%p LU=%u S=%r T=%u/%r L=%r P0=%u C=%r",
        Handle, LunValid ? LunBytes[1] : 0xFFU, ScsiStatus,
        DeviceType, TypeStatus, LocationStatus, TargetZero, ConnectStatus);
}

STATIC VOID UfsUnitDescriptors (EFI_HANDLE Handle) {
  EFI_UFS_DEVICE_CONFIG_PROTOCOL *Config;
  EFI_STATUS Status;
  UINTN Index;
  UINT32 DescriptorSize;
  UINT32 RequiredSize = OFFSET_OF (UFS_UNIT_DESC, LogicBlkSize) + sizeof (UINT8);
  UINT8 Descriptor[UFS_UNIT_DESCRIPTOR_MAX_SIZE];
  BOOLEAN BoardKnown;

  Status = gBS->HandleProtocol (Handle, &gEfiUfsDeviceConfigProtocolGuid,
                                (VOID **)&Config);
  if (EFI_ERROR (Status) || (Config == NULL) || (Config->RwUfsDescriptor == NULL)) {
    Line (L"UFS unit descriptor protocol H%p: %r", Handle,
          EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED);
    return;
  }

  // Read only the normal-LU descriptors. A 255-byte bounded buffer accommodates
  // descriptor extensions; no device, string, serial, flag, or write request is made.
  for (Index = 0; Index < 8; Index++) {
    BoardKnown = (BOOLEAN)(Index < 3U);
    ZeroMem (Descriptor, sizeof (Descriptor));
    DescriptorSize = sizeof (Descriptor);
    Status = Config->RwUfsDescriptor (Config, TRUE, UfsUnitDesc,
                                      (UINT8)Index, 0, Descriptor, &DescriptorSize);
    if (EFI_ERROR (Status)) {
      if (BoardKnown) Line (L"UFS unit LU%u query=%r", Index, Status);
      continue;
    }

    if ((DescriptorSize > sizeof (Descriptor)) || (DescriptorSize < RequiredSize)) {
      if (BoardKnown) Line (L"UFS unit LU%u format=size returned=%u", Index, DescriptorSize);
      continue;
    }
    if ((Descriptor[0] < RequiredSize) || (Descriptor[0] > DescriptorSize)) {
      if (BoardKnown) Line (L"UFS unit LU%u format=length returned=%u declared=%u",
                            Index, DescriptorSize, Descriptor[0]);
      continue;
    }
    if ((Descriptor[1] != UfsUnitDesc) || (Descriptor[2] != Index)) {
      if (BoardKnown) Line (L"UFS unit LU%u format=header", Index);
      continue;
    }
    if (Descriptor[3] > 2U) {
      if (BoardKnown) Line (L"UFS unit LU%u format=enable-value", Index);
      continue;
    }

    // Show the known board LUs even when disabled; for other indices show only
    // enabled descriptors, keeping expected absent LUs quiet.
    if (!BoardKnown && (Descriptor[3] == 0U)) continue;
    Line (L"UFS unit LU%u enable=%02x boot=%02x block-shift=%u",
          Index, Descriptor[3], Descriptor[4], Descriptor[10]);
  }
}

STATIC VOID UfsBindHistory (VOID) {
  T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS *Source, Copy;
  EFI_STATUS Status;
  UINTN Index, SummaryIndex;
  EFI_TPL OldTpl;

  Status = gBS->LocateProtocol (&mUfsDiagnosticsGuid, NULL, (VOID **)&Source);
  if (EFI_ERROR (Status)) {
    Line (L"UFS bind diagnostics unavailable: %r", Status);
    return;
  }
  if (Source->Version != T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_VERSION) {
    Line (L"UFS bind diagnostics version unsupported: %u", Source->Version);
    return;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  CopyMem (&Copy, Source, sizeof (Copy));
  gBS->RestoreTPL (OldTpl);
  if (Copy.Count > T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_MAX) {
    Line (L"UFS bind diagnostics invalid count: %u", Copy.Count);
    return;
  }
  Line (L"UFS bind diagnostics records=%u dropped=%u", Copy.Count, Copy.Dropped);
  if (EFI_ERROR (Copy.DriverBindingInstallStatus) ||
      ((Copy.DiagnosticsUninstallStatus != EFI_NOT_STARTED) &&
       EFI_ERROR (Copy.DiagnosticsUninstallStatus)))
    Line (L"UFS driver entry binding=%r diag-remove=%r",
          Copy.DriverBindingInstallStatus, Copy.DiagnosticsUninstallStatus);
  SummaryIndex = Copy.Count;
  for (Index = 0; Index < Copy.Count; Index++) {
    T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD *R = &Copy.Records[Index];
    if (((R->Flags & (T70_UFS_DIAG_START_SEEN | T70_UFS_DIAG_LUN_VALID)) ==
         (T70_UFS_DIAG_START_SEEN | T70_UFS_DIAG_LUN_VALID)) &&
        (R->Lun == 2U)) {
      SummaryIndex = Index;
      break;
    }
  }
  if (SummaryIndex == Copy.Count) {
    for (Index = 0; Index < Copy.Count; Index++) {
      T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD *R = &Copy.Records[Index];
      if ((R->SupportedStage == T70UfsDiagStageSupported) ||
          (((R->Flags & T70_UFS_DIAG_LUN_VALID) != 0U) && (R->Lun == 2U))) {
        SummaryIndex = Index;
        break;
      }
    }
  }
  if ((SummaryIndex == Copy.Count) && (Copy.Count != 0U)) SummaryIndex = 0U;
  if (SummaryIndex < Copy.Count) {
    T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD *R = &Copy.Records[SummaryIndex];
    BOOLEAN Preferred = (BOOLEAN)(
      (((R->Flags & T70_UFS_DIAG_LUN_VALID) != 0U) && (R->Lun == 2U)) ||
      (R->SupportedStage == T70UfsDiagStageSupported));
    UINT8 Lun = (R->Flags & T70_UFS_DIAG_LUN_VALID) ? R->Lun : 0xFFU;
    if (!Preferred) {
      UINT32 Stage = ((R->Flags & T70_UFS_DIAG_START_SEEN) != 0U) ?
                     R->StartStage : R->SupportedStage;
      EFI_STATUS Detail = ((R->Flags & T70_UFS_DIAG_START_SEEN) != 0U) ?
                          R->StartStatus : R->SupportedStatus;
      Line (L"UFS bind reject LU=%u stage=%s status=%r detail=%r", Lun,
            UfsDiagStageName (Stage), Detail, R->StageStatus);
    } else if ((R->Flags & T70_UFS_DIAG_START_SEEN) != 0U) {
      if (EFI_ERROR (R->StartStatus) || (R->StartStage != T70UfsDiagStageReady))
        Line (L"UFS bind issue LU=%u stage=%s start=%r detail=%r", Lun,
              UfsDiagStageName (R->StartStage), R->StartStatus, R->StageStatus);
      else
        Line (L"UFS bind first LU=%u start=%r/%s", Lun, R->StartStatus,
              UfsDiagStageName (R->StartStage));
    } else if (EFI_ERROR (R->SupportedStatus)) {
      Line (L"UFS bind issue LU=%u stage=%s supported=%r detail=%r", Lun,
            UfsDiagStageName (R->SupportedStage), R->SupportedStatus,
            R->StageStatus);
    } else {
      Line (L"UFS bind issue LU=%u stage=not-started supported=%r", Lun,
            R->SupportedStatus);
    }
  }
  for (Index = 0; Index < Copy.Count; Index++) {
    T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD *R = &Copy.Records[Index];
    Line (L"UFS bind H%p LU=%u start=%r/%s detail=%r",
          R->Controller, (R->Flags & T70_UFS_DIAG_LUN_VALID) ? R->Lun : 0xFFU,
          R->StartStatus, UfsDiagStageName (R->StartStage), R->StageStatus);
    Line (L" supported=%r/%s open=%r C10=%r C16=%r install=%r size=%u last=%lx",
          R->SupportedStatus, UfsDiagStageName (R->SupportedStage),
          R->OpenStatus, R->Capacity10Status, R->Capacity16Status,
          R->InstallStatus, R->BlockSize, R->LastBlock);
  }
}

STATIC UINTN MsdcStateHash (CONST MSDC_DIAGNOSTICS_RECORD *Snapshot, UINTN *Completed) {
  UINTN Value = Snapshot->DetectSampleValid;
  Value = Value * 33 + Snapshot->CardPresent;
  Value = Value * 33 + Snapshot->MediaReady;
  if (!Snapshot->Initializing) {
    *Completed = Snapshot->LastDriverStage;
    *Completed = *Completed * 33 + Snapshot->LastInitStage;
    *Completed = *Completed * 33 + (UINTN)Snapshot->CardDetectStatus;
    *Completed = *Completed * 33 + (UINTN)Snapshot->LastDriverStatus;
    *Completed = *Completed * 33 + (UINTN)Snapshot->LastInitStatus;
    *Completed = *Completed * 33 + (UINTN)Snapshot->DevicePathStatus;
  }
  return Value * 33 + *Completed;
}

/* Compare handle identities as well as counts: replacement can keep the count. */
STATIC UINTN Topology (VOID) {
  EFI_GUID *Guid[] = { &gEfiUsb2HcProtocolGuid, &gEfiUsbIoProtocolGuid,
                      &gEfiBlockIoProtocolGuid, &gEfiSimpleFileSystemProtocolGuid,
                      &gEfiSdMmcPassThruProtocolGuid, &gEdkiiUfsHostControllerProtocolGuid,
                      &gEfiScsiIoProtocolGuid };
  UINTN I, J, N, Value = 0;
  MSDC_DIAGNOSTICS_RECORD Snapshot;
  STATIC UINTN CompletedSdState;
  for (I = 0; I < ARRAY_SIZE (Guid); I++) {
    EFI_HANDLE *H = Handles (Guid[I], &N);
    Value = Value * 33 + N;
    for (J = 0; J < N; J++) Value = Value * 33 + (UINTN)H[J];
    if (H != NULL) FreePool (H);
  }
  // Repeated failed retries must not clear the screen at every timer tick.
  if (!EFI_ERROR (MsdcGetSnapshot (&Snapshot)))
    Value = Value * 33 + MsdcStateHash (&Snapshot, &CompletedSdState);
  return Value;
}

STATIC VOID CountProtocol (CHAR16 *Name, EFI_GUID *Guid) {
  UINTN N; EFI_HANDLE *H = Handles (Guid, &N);
  Line (L"%s: %u", Name, N); if (H != NULL) FreePool (H);
}

STATIC VOID HostHistory (VOID) {
  EFI_GUID Guid = T70_USB_HOST_DIAGNOSTICS_GUID;
  EFI_GUID TypecGuid = T70_USB_TYPEC_DIAGNOSTICS_GUID;
  T70_USB_HOST_DIAGNOSTICS *P;
  T70_USB_TYPEC_DIAGNOSTICS *Typec;
  T70_USB_HOST_SNAPSHOT D;
  MTK_HUSB311_TYPEC_DIAGNOSTIC Failure;
  UINT8 GuardStep;
  EFI_STATUS S = gBS->LocateProtocol (&Guid, NULL, (VOID **)&P);
  if (EFI_ERROR (S)) { Line (L"Host preparation diagnostics: %r", S); return; }
  if (P->Version != T70_USB_HOST_DIAG_VERSION || P->Read == NULL) { Line (L"Host diagnostics version unsupported"); return; }
  S = P->Read (&D);
  if (EFI_ERROR (S)) { Line (L"Host snapshot: %r", S); return; }
  Line (L"Host prep reason=%u status=%r step=%04x", D.Reason, D.Status, D.BusStep);
  Line (L" failed=%u ready=%u connected=%u inherited-devctl=%02x id=%02x", D.Failed, D.Ready, D.Connected, D.Devctl, D.Id);
  if (D.ColdStep != 0U) {
    Line (L" First cold start failure: step=%02x path=%u guard=%u reset=%u",
      D.ColdStep, D.ColdPath, D.ColdGuard, D.ColdReset);
    if (D.ColdRegistersValid)
      Line (L" Before cleanup: DEVCTL=%02x POWER=%02x DTM1=%08x",
        D.ColdDevctl, D.ColdPower, D.ColdDtm1);
  }
  if (D.IdleOffset != T70_USB_HOST_NO_IDLE_OFFSET)
    Line (L" USB idle check reg=%04x ep=%02x value=%08x mask=%08x",
      D.IdleOffset, D.IdleEndpoint, D.IdleValue, D.IdleMask);
  if (D.SenseStage != 0)
    Line (L" sense stage=%u reg=%02x saved=%02x expected=%02x live=%02x %r",
      D.SenseStage, D.SenseRegister, D.SenseSaved, D.SenseExpected, D.SenseLive, D.SenseStatus);
  if (!D.Ready && !EFI_ERROR (gBS->LocateProtocol (&TypecGuid, NULL, (VOID **)&Typec)) &&
      !EFI_ERROR (Typec->Read (&Failure, &GuardStep)) && Failure.Step != 0U) {
    Line (L" Type-C step=%02x status=%02x reg=%02x/%u val=%04x io=%02x guard=%02x/%02x",
      Failure.Step, Failure.Status, Failure.Register, Failure.Length, Failure.Value,
      Failure.ReadStatus, Failure.GuardFlags, GuardStep);
    if (Failure.OpenAttempts != 0U)
      Line (L" Type-C CC open attempts=%u write=%02x", Failure.OpenAttempts, Failure.OpenWriteStatus);
  }
}

STATIC VOID ReadGauge (VOID) {
  MTK_PMIC_GAUGE_PROTOCOL *Gauge;
  MTK_PMIC_BATTERY_ADC_PROTOCOL *Adc;
  EFI_STATUS Status;

  mGaugeStatus = gBS->LocateProtocol (&gMediaTekPmicGaugeProtocolGuid, NULL, (VOID **)&Gauge);
  if (!EFI_ERROR (mGaugeStatus)) mGaugeStatus = Gauge->ReadCurrent (&mGaugeRaw);
  Status = gBS->LocateProtocol (&gMediaTekPmicBatteryAdcProtocolGuid, NULL, (VOID **)&Adc);
  mBatteryAdcStatus[MtkPmicBatteryAdcVoltage] = EFI_ERROR (Status) ? Status :
    Adc->Read (MtkPmicBatteryAdcCalibratedVoltage, &mBatteryAdcSample[MtkPmicBatteryAdcVoltage]);
  mBatteryVoltageCalibrated = !EFI_ERROR (mBatteryAdcStatus[MtkPmicBatteryAdcVoltage]);
  if (!EFI_ERROR (Status) && !mBatteryVoltageCalibrated) {
    mBatteryAdcStatus[MtkPmicBatteryAdcVoltage] =
      Adc->Read (MtkPmicBatteryAdcVoltage, &mBatteryAdcSample[MtkPmicBatteryAdcVoltage]);
  }
  mBatteryAdcStatus[MtkPmicBatteryAdcTemperature] = EFI_ERROR (Status) ? Status :
    Adc->Read (MtkPmicBatteryAdcTemperatureDeciK, &mBatteryAdcSample[MtkPmicBatteryAdcTemperature]);
  mBatteryTemperatureCalibrated = !EFI_ERROR (mBatteryAdcStatus[MtkPmicBatteryAdcTemperature]);
  if (!EFI_ERROR (Status) && !mBatteryTemperatureCalibrated) {
    mBatteryAdcStatus[MtkPmicBatteryAdcTemperature] =
      Adc->Read (MtkPmicBatteryAdcTemperature, &mBatteryAdcSample[MtkPmicBatteryAdcTemperature]);
  }
}

STATIC VOID PowerStatus (VOID) {
  STATIC CONST CHAR16 *CONST ChargeNames[] = {
    L"unknown", L"charging", L"discharging", L"not charging", L"charge complete"
  };
  STOCK_POWER_READ_ONLY_PROTOCOL *Power;
  SP_SC8989X_CHARGE_STATE Charge;
  SP_SC8989X_BOOST_STATE Boost;
  SP_HUSB311_POWER_STATUS Typec;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gStockPowerReadOnlyProtocolGuid, NULL, (VOID **)&Power);
  if (EFI_ERROR (Status)) {
    Line (L"Power status unavailable (%r)", Status);
    return;
  }
  if ((Power == NULL) || (Power->Revision < STOCK_POWER_READ_ONLY_CHARGE_STATE_REVISION) ||
      (Power->GetSc8989xChargeState == NULL) || (Power->GetSc8989xBoostState == NULL) ||
      (Power->GetHusb311PowerStatus == NULL)) {
    Line (L"Power status unavailable (unsupported interface)");
    return;
  }

  Status = Power->GetSc8989xChargeState (Power, &Charge);
  if (EFI_ERROR (Status)) {
    Line (L"Charger state unavailable (%r)", Status);
  } else {
    UINTN Index = (UINTN)Charge.ChargeStatus;
    Line (L"Charger %s, input good %s, source code %u",
          Index < ARRAY_SIZE (ChargeNames) ? ChargeNames[Index] : ChargeNames[0],
          Charge.PowerGood ? L"yes" : L"no", Charge.VbusStateCode);
  }

  Status = Power->GetSc8989xBoostState (Power, &Boost);
  if (EFI_ERROR (Status)) {
    Line (L"USB boost control unavailable (%r)", Status);
  } else {
    Line (L"USB boost control %s", Boost.BoostEnabled ? L"enabled" : L"disabled");
  }

  Status = Power->GetHusb311PowerStatus (Power, &Typec);
  if (EFI_ERROR (Status)) {
    Line (L"USB-C VBUS status unavailable (%r)", Status);
  } else {
    Line (L"USB-C VBUS %s", Typec.VbusPresent ? L"detected" : L"not detected");
  }
}

STATIC VOID StorageHistory (VOID) {
  T70_USB_STORAGE_DIAGNOSTICS *D, Copy;
  EFI_STATUS S = gBS->LocateProtocol (&mStorageDiagGuid, NULL, (VOID **)&D);
  UINTN I; EFI_TPL Old;
#ifdef T70PRO_RAM_DIAGNOSTICS
  STATIC EFI_GUID UsbBootTraceGuid = T70_USB_BOOT_READ_TRACE_GUID;
  CONST T70_USB_BOOT_READ_TRACE *Trace;
  T70_USB_BOOT_READ_TRACE Boot;
  if (!EFI_ERROR (gBS->LocateProtocol (&UsbBootTraceGuid, NULL, (VOID **)&Trace))) {
    Old = gBS->RaiseTPL (TPL_NOTIFY);
    Boot = *Trace;
    gBS->RestoreTPL (Old);
    if (Boot.Attempt != 0) {
      Line (L"USB boot reads=%u hive=%u first-error=%r", Boot.Reads, Boot.HiveReads, Boot.FirstError);
      if (EFI_ERROR (Boot.FirstError)) {
        Line (L" USB boot LBA=%Lx bytes=%Lu media=%x", Boot.ErrorLba, (UINT64)Boot.ErrorBytes, Boot.ErrorMediaId);
      }
    }
  }
#endif
  if (EFI_ERROR (S)) { Line (L"Storage history: %r", S); return; }
  if (D->Version != T70_USB_DIAG_VERSION) { Line (L"Storage history version unsupported"); return; }
  Old = gBS->RaiseTPL (TPL_NOTIFY); CopyMem (&Copy, D, sizeof (Copy)); gBS->RestoreTPL (Old);
  if (Copy.Count > T70_USB_DIAG_MAX) { Line (L"Storage history invalid count"); return; }
  Line (L"Storage bind attempts: %u dropped=%u", Copy.Count, Copy.Dropped);
  for (I = 0; I < Copy.Count; I++) {
    T70_USB_STORAGE_RECORD *R = &Copy.Records[I];
    Line (L"H%p stage=%u bind=%r", R->Controller, R->Stage, R->BindStatus);
    Line (L" cmd=%02x lun=%u status=%r result=%x", R->Opcode, R->Lun, R->CommandStatus, R->CommandResult);
    Line (L" last-error cmd=%02x %r sense=%02x/%02x/%02x", R->ErrorOpcode, R->LastError, R->SenseKey, R->Asc, R->Ascq);
  }
}

STATIC VOID ReadHeader (EFI_BLOCK_IO_PROTOCOL *B) {
  UINTN Size = B->Media->BlockSize, Align = B->Media->IoAlign, Pages;
  UINT8 *Buffer; EFI_STATUS S;
  if (!B->Media->MediaPresent) { Line (L" sector read: skipped (no media)"); return; }
  if (Size < 512 || Size > 65536 || Align > 65536 || (Align != 0 && (Align & (Align - 1)) != 0)) {
    Line (L" sector read: unsupported block size/alignment"); return;
  }
  Pages = EFI_SIZE_TO_PAGES (Size);
  Buffer = AllocateAlignedPages (Pages, MAX (Align, EFI_PAGE_SIZE));
  if (Buffer == NULL) { Line (L" sector read: out of memory"); return; }
  S = B->ReadBlocks (B, B->Media->MediaId, 0, Size, Buffer);
  if (S == EFI_MEDIA_CHANGED && B->Media->BlockSize == Size) S = B->ReadBlocks (B, B->Media->MediaId, 0, Size, Buffer);
  Line (L" LBA0 read=%r signature55AA=%u", S,
    !EFI_ERROR (S) && Buffer[510] == 0x55 && Buffer[511] == 0xaa);
  if (!EFI_ERROR (S) && !B->Media->LogicalPartition && B->Media->LastBlock >= 1) {
    S = B->ReadBlocks (B, B->Media->MediaId, 1, Size, Buffer);
    Line (L" LBA1 read=%r GPT-signature=%u", S,
      !EFI_ERROR (S) && CompareMem (Buffer, "EFI PART", 8) == 0);
  }
  FreeAlignedPages (Buffer, Pages);
}

STATIC VOID Scan (UINT32 Flags) {
  EFI_HANDLE *H; UINTN N, I, J, UsbMass = 0, UsbBlocks = 0, UsbFs = 0, SdBlocks = 0, SdFs = 0;
  UINTN UfsHosts = 0, UfsUnits = 0, UfsBlocks = 0, UfsFs = 0;
  UINTN UsbBootFiles = 0;
  BOOLEAN ConnectUsb = (BOOLEAN)((Flags & SCAN_CONNECT_USB) != 0U);
  BOOLEAN ConnectInternal = (BOOLEAN)((Flags & SCAN_CONNECT_INTERNAL) != 0U);
  BOOLEAN TopologyUpdate = (BOOLEAN)((Flags & SCAN_TOPOLOGY_UPDATE) != 0U);
  EFI_STATUS S;
  mDeviceCheckLogChars = mLineCount = 0; mTruncated = FALSE; mDeviceCheckLog[0] = 0;
  Line (L"T70 Device Check v3 - auto SD/UFS; RAM test on M");
  Line (L"Disk inspection is read-only. M writes only newly allocated RAM; no disk writes.");
  Line (L"RAM test covers 16 MiB, not all RAM or a hardware certification.");
  PowerStatus ();
  if (mGaugeStatus == EFI_SUCCESS) Line (L"Battery gauge current snapshot: 0x%04x (raw)", mGaugeRaw);
  else if (mGaugeStatus != EFI_NOT_STARTED) Line (L"Battery gauge snapshot: %r", mGaugeStatus);
  if (mBatteryAdcStatus[MtkPmicBatteryAdcVoltage] != EFI_NOT_STARTED) {
    if (mBatteryVoltageCalibrated) {
      Line (L"Battery voltage: %u mV", mBatteryAdcSample[MtkPmicBatteryAdcVoltage]);
    } else if (!EFI_ERROR (mBatteryAdcStatus[MtkPmicBatteryAdcVoltage])) {
      Line (L"Battery voltage ADC: 0x%04x (raw)", mBatteryAdcSample[MtkPmicBatteryAdcVoltage]);
    } else {
      Line (L"Battery voltage ADC: %r", mBatteryAdcStatus[MtkPmicBatteryAdcVoltage]);
    }
    if (mBatteryTemperatureCalibrated) {
      INT32 Celsius = (INT32)mBatteryAdcSample[MtkPmicBatteryAdcTemperature] - 2732;
      UINT32 Magnitude = (UINT32)(Celsius < 0 ? -Celsius : Celsius);

      Line (L"Battery temperature: %s%u.%u C", Celsius < 0 ? L"-" : L"", Magnitude / 10, Magnitude % 10);
    } else if (!EFI_ERROR (mBatteryAdcStatus[MtkPmicBatteryAdcTemperature])) {
      Line (L"Battery temperature ADC: 0x%04x (raw)", mBatteryAdcSample[MtkPmicBatteryAdcTemperature]);
    } else {
      Line (L"Battery temperature ADC: %r", mBatteryAdcStatus[MtkPmicBatteryAdcTemperature]);
    }
  }
  SdHosts (ConnectInternal);
  MsdcDiagnosticHistory ();
  H = Handles (&gEdkiiUfsHostControllerProtocolGuid, &UfsHosts);
  Line (L"UFS host controllers: %u", UfsHosts);
  for (I = 0; I < UfsHosts; I++) {
    if (ConnectInternal) {
      S = gBS->ConnectController (H[I], NULL, NULL, TRUE);
      Line (L"UFS host connect H%p: %r", H[I], S);
    }
    if (!TopologyUpdate) UfsUnitDescriptors (H[I]);
  }
  if (H != NULL) FreePool (H);
  H = Handles (&gEfiScsiIoProtocolGuid, &N);
  for (I = 0; I < N; I++) if (UfsPath (H[I])) {
    UfsUnits++;
    UfsUnitInfo (H[I], ConnectInternal);
  }
  if (H != NULL) FreePool (H);
  Line (L"UFS SCSI units: %u", UfsUnits);
  UfsBindHistory ();
  H = Handles (&gEfiBlockIoProtocolGuid, &N);
  for (I = 0; I < N; I++) if (UfsPath (H[I])) UfsBlocks++;
  if (H != NULL) FreePool (H);
  Line (L"UFS early counts hosts=%u units=%u BlockIO=%u", UfsHosts, UfsUnits, UfsBlocks);
  UfsBlocks = 0;
  if (ConnectUsb) {
    H = Handles (&gEfiUsb2HcProtocolGuid, &N);
    for (I = 0; I < N; I++) {
      S = gBS->ConnectController (H[I], NULL, NULL, TRUE);
      Line (L"USB host connect: %r", S);
    }
    if (H != NULL) FreePool (H);
  }
  HostHistory ();
  CountProtocol (L"USB host controllers", &gEfiUsb2HcProtocolGuid);
  CountProtocol (L"Text input protocols (not device count)", &gEfiSimpleTextInProtocolGuid);
  CountProtocol (L"Extended text input protocols", &gEfiSimpleTextInputExProtocolGuid);
  CountProtocol (L"Absolute pointer protocols", &gEfiAbsolutePointerProtocolGuid);
  CountProtocol (L"Relative pointer protocols", &gEfiSimplePointerProtocolGuid);
  CountProtocol (L"Display outputs", &gEfiGraphicsOutputProtocolGuid);
  H = Handles (&gEfiUsbIoProtocolGuid, &N);
  Line (L"USB interfaces: %u", N);
  for (I = 0; I < N; I++) {
    EFI_USB_IO_PROTOCOL *U;
    EFI_USB_INTERFACE_DESCRIPTOR D;
    S = gBS->HandleProtocol (H[I], &gEfiUsbIoProtocolGuid, (VOID **)&U);
    if (!EFI_ERROR (S)) S = U->UsbGetInterfaceDescriptor (U, &D);
    if (EFI_ERROR (S)) { Line (L"USB H%p descriptor: %r", H[I], S); continue; }
    Line (L"USB H%p if=%u alt=%u class=%02x sub=%02x proto=%02x", H[I], D.InterfaceNumber, D.AlternateSetting, D.InterfaceClass, D.InterfaceSubClass, D.InterfaceProtocol);
    for (J = 0; J < D.NumEndpoints; J++) {
      EFI_USB_ENDPOINT_DESCRIPTOR E;
      S = U->UsbGetEndpointDescriptor (U, (UINT8)J, &E);
      if (EFI_ERROR (S)) Line (L" ep%u descriptor: %r", J, S);
      else Line (L" ep=%02x type=%u packet=%u interval=%u", E.EndpointAddress, E.Attributes & 3, E.MaxPacketSize, E.Interval);
    }
    if (D.InterfaceClass == 8) {
      UsbMass++;
      if (D.InterfaceProtocol != 0x50 && D.InterfaceProtocol != 0 && D.InterfaceProtocol != 1)
        Line (L" storage transport unsupported by current UEFI");
      if (ConnectUsb) {
        Print (L"Connecting storage interface %u...\r\n", I);
        S = gBS->ConnectController (H[I], NULL, NULL, TRUE);
        Line (L" connect: %r", S);
      }
    }
  }
  if (H != NULL) FreePool (H);
  StorageHistory ();
  H = Handles (&gEfiBlockIoProtocolGuid, &N);
  Line (L"BlockIO handles: %u", N);
  for (I = 0; I < N; I++) {
    EFI_BLOCK_IO_PROTOCOL *B; BOOLEAN USB = UsbPath (H[I]);
    S = gBS->HandleProtocol (H[I], &gEfiBlockIoProtocolGuid, (VOID **)&B);
    if (EFI_ERROR (S) || B->Media == NULL) { Line (L"Block H%p: invalid protocol %r", H[I], S); continue; }
    if (USB) UsbBlocks++;
    if (SdPath (H[I])) SdBlocks++;
    if (UfsPath (H[I])) UfsBlocks++;
    if (TopologyUpdate && !USB) continue;
    Line (L"Block H%p bus=%s part=%u", H[I], StorageBus (H[I]), B->Media->LogicalPartition);
    if (!mTruncated) ReadHeader (B);
    Line (L" present=%u readonly=%u block=%u last=%lx media-id=%u", B->Media->MediaPresent, B->Media->ReadOnly, B->Media->BlockSize, B->Media->LastBlock, B->Media->MediaId);
    if ((ConnectUsb && USB) ||
        (ConnectInternal && (SdPath (H[I]) || UfsPath (H[I])))) {
      S = gBS->ConnectController (H[I], NULL, NULL, TRUE);
      Line (L" partition/filesystem connect: %r", S);
    }
  }
  if (H != NULL) FreePool (H);
  H = Handles (&gEfiSimpleFileSystemProtocolGuid, &N);
  Line (L"Filesystems: %u", N);
  for (I = 0; I < N; I++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs; EFI_FILE_PROTOCOL *Root = NULL, *Boot = NULL;
    BOOLEAN USB = UsbPath (H[I]);
    if (USB) UsbFs++;
    if (SdPath (H[I])) SdFs++;
    if (UfsPath (H[I])) UfsFs++;
    if (!USB && (TopologyUpdate || mTruncated)) continue;
    S = gBS->HandleProtocol (H[I], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (!EFI_ERROR (S)) S = Fs->OpenVolume (Fs, &Root);
    Line (L"FS H%p bus=%s root=%r", H[I], StorageBus (H[I]), S);
    if (!EFI_ERROR (S) && Root != NULL) {
      /* Inspect only the standard removable boot path, never enumerate personal files. */
      S = Root->Open (Root, &Boot, L"\\EFI\\BOOT\\BOOTAA64.EFI", EFI_FILE_MODE_READ, 0);
      Line (L" BOOTAA64.EFI open: %r", S);
      if (!EFI_ERROR (S) && Boot != NULL) {
        if (USB) UsbBootFiles++;
        Boot->Close (Boot);
      }
      Root->Close (Root);
    }
  }
  if (H != NULL) FreePool (H);
  mWritingSummary = TRUE;
  if (mTruncated) Line (L"Some detail lines were omitted.");
  if (TopologyUpdate) Line (L"R: run the full SD/UFS inspection.");
  Line (L"SD SUMMARY blocks=%u filesystems=%u", SdBlocks, SdFs);
  Line (L"UFS SUMMARY hosts=%u units=%u blocks=%u filesystems=%u", UfsHosts, UfsUnits, UfsBlocks, UfsFs);
  Line (L"SUMMARY mass-interfaces=%u usb-blocks=%u usb-filesystems=%u", UsbMass, UsbBlocks, UsbFs);
  if (UsbMass == 0) Line (L"USB: no storage interface; SD results are listed separately.");
  else if (UsbBlocks == 0) Line (L"STOP: storage interface present; driver/transport/media failed.");
  else if (UsbFs == 0) Line (L"STOP: disk exists; inspect media/partition/filesystem support.");
  else Line (L"USB BOOTAA64.EFI found on %u of %u filesystems.", UsbBootFiles, UsbFs);
  mWritingSummary = FALSE;
}

#define RAM_TEST_BYTES  (16U * 1024U * 1024U)
#define RAM_TEST_WORDS  (RAM_TEST_BYTES / sizeof (UINT64))

typedef struct {
  UINTN LastPercent;
  UINTN Row;
} RAM_TEST_UI;

STATIC VOID RamFlush (VOID *Context, volatile uint64_t *Buffer, size_t Bytes) {
  (VOID)Context;
  WriteBackInvalidateDataCacheRange ((VOID *)Buffer, (UINTN)Bytes);
}

STATIC int RamReleasePages (VOID *Context, uintptr_t Address, size_t Pages) {
  (VOID)Context;
  mRamLastFreeStatus = gBS->FreePages ((EFI_PHYSICAL_ADDRESS)Address, (UINTN)Pages);
  return !EFI_ERROR (mRamLastFreeStatus);
}

STATIC int RamProgress (VOID *Context, unsigned Pattern, int Verify, size_t Completed, size_t Total) {
  RAM_TEST_UI *Ui = (RAM_TEST_UI *)Context;
  EFI_INPUT_KEY Key;
  UINTN Percent;
  (VOID)Pattern;
  if (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key)) &&
      (Key.ScanCode == SCAN_ESC || Key.UnicodeChar == 0x1b)) return 1;
  Percent = Total == 0 ? 0 : (UINTN)(((UINT64)Completed * 100) / Total);
  if (Percent != Ui->LastPercent) {
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, Ui->Row);
    Print (L"Progress %3u%%  %s  (%u/6 patterns)                                      ",
           Percent, Verify ? L"reading" : L"writing", Pattern + 1);
    Ui->LastPercent = Percent;
  }
  return 0;
}

STATIC VOID RunRamTest (VOID) {
  EFI_PHYSICAL_ADDRESS Address = 0;
  EFI_STATUS AllocateStatus, TestReleaseStatus = EFI_SUCCESS;
  EFI_STATUS FreeStatus;
  T70_RAM_TEST_RESULT Result;
  T70_RAM_TEST_STATUS TestStatus;
  RAM_TEST_UI Ui = { MAX_UINTN, 5 };
  UINTN Pages = EFI_SIZE_TO_PAGES (RAM_TEST_BYTES);
  UINTN Rows = 25, Columns = 80;
  BOOLEAN Released = FALSE;

  if (!T70RamTestPrepareToAllocate (&mRamPendingLease, RamReleasePages, NULL)) {
    Line (L"RAM test: not run; prior pages still allocated base=%lx pages=%u retry=%r",
          mRamPendingLease.Address, mRamPendingLease.Pages, mRamLastFreeStatus);
    Print (L"Prior test pages are still allocated; release retry failed: %r.\r\n",
           mRamLastFreeStatus);
    return;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"T70 allocated-memory check\r\n\r\n");
  Print (L"Tests only 16 MiB newly allocated by UEFI.\r\n");
  Print (L"Six patterns: 0, 1, address, inverse address, walking 1/0.\r\n");
  Print (L"One round = 6 write passes + 6 verify passes. Not all RAM.\r\n");
  Print (L"ESC stops the check; allocated pages are released afterward.\r\n\r\n");
  Print (L"Allocating...\r\n");
  gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows);
  Ui.Row = Rows > 7 ? Rows - 2 : 5;
  AllocateStatus = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, Pages, &Address);
  if (EFI_ERROR (AllocateStatus)) {
    Line (L"RAM test: not run; AllocatePages failed: %r", AllocateStatus);
    Print (L"\r\nAllocation failed: %r\r\n", AllocateStatus);
    return;
  }

  Line (L"RAM allocation: base=%lx bytes=%u pages=%u", Address, (UINTN)RAM_TEST_BYTES, Pages);
  Line (L"RAM patterns: 6, rounds=1, passes=12 (6 writes + 6 verifies)");
  TestStatus = T70RamTestRun ((volatile uint64_t *)(UINTN)Address, RAM_TEST_WORDS,
                              RamProgress, RamFlush, &Ui, &Result);
  mRamPendingLease.Address = (uintptr_t)Address;
  mRamPendingLease.Pages = Pages;
  Released = T70RamTestPrepareToAllocate (&mRamPendingLease, RamReleasePages, NULL);
  FreeStatus = mRamLastFreeStatus;
  if (!Released) TestReleaseStatus = FreeStatus;

  Line (L"RAM progress: %u/%u word operations", Result.CompletedOperations, Result.TotalOperations);
  if (TestStatus == T70RamTestMismatch) {
    Line (L"RAM mismatch: address=%lx pattern=%u phase=verify expected=%016lx actual=%016lx errors=%u",
          Result.ErrorAddress, Result.Pattern + 1, Result.Expected, Result.Actual, Result.ErrorCount);
  }
  if (TestStatus == T70RamTestPass && Released) {
    Line (L"RAM outcome: PASS; 16 MiB allocation only; not whole-RAM certification");
    Print (L"\r\nRAM check passed. 16 MiB only; not all installed RAM.\r\n");
  } else if (TestStatus == T70RamTestCancelled) {
    Line (L"RAM outcome: CANCELLED; errors=0");
    Print (L"\r\nRAM check cancelled.\r\n");
  } else if (TestStatus == T70RamTestMismatch) {
    Line (L"RAM outcome: FAIL; errors=%u", Result.ErrorCount);
    Print (L"\r\nRAM mismatch found at %lx.\r\n", Result.ErrorAddress);
  } else if (TestStatus == T70RamTestPass) {
    Line (L"RAM outcome: INCOMPLETE; page release failed, PASS withheld");
    Print (L"\r\nRAM writes and reads completed, but page release failed.\r\n");
  } else {
    Line (L"RAM outcome: INCOMPLETE; core status=%u", TestStatus);
    Print (L"\r\nRAM check incomplete.\r\n");
  }
  Line (L"RAM page release: %r", TestReleaseStatus);
  if (EFI_ERROR (TestReleaseStatus)) {
    Line (L"RAM outcome downgraded: PASS is not reported because FreePages failed");
    Print (L"Page release failed: %r. PASS withheld.\r\n", TestReleaseStatus);
  }
}

STATIC UINTN mPageColumns = 80;

STATIC UINTN VisualRowCount (VOID) {
  T70_VISUAL_CURSOR Cursor = { 0, 0 };
  CHAR16 Row[512]; size_t Written; UINTN Count = 0;
  while (T70VisualRow (mDeviceCheckLog, mDeviceCheckLogChars,
           (unsigned)mPageColumns, &Cursor, Row, ARRAY_SIZE (Row), &Written) == 1) Count++;
  return Count;
}

STATIC UINTN LastVisualPage (UINTN Rows) {
  UINTN Count = VisualRowCount ();
  return Count > Rows ? ((Count - 1) / Rows) * Rows : 0;
}

STATIC VOID Page (UINTN First, UINTN Rows) {
  T70_VISUAL_CURSOR Cursor = { 0, 0 };
  CHAR16 Row[512]; size_t Written; UINTN Index = 0, Shown = 0;
  UINTN Count = VisualRowCount ();
  if (First >= Count) First = LastVisualPage (Rows);
  gST->ConOut->ClearScreen (gST->ConOut);
  while (T70VisualRow (mDeviceCheckLog, mDeviceCheckLogChars,
           (unsigned)mPageColumns, &Cursor, Row, ARRAY_SIZE (Row), &Written) == 1) {
    if (Index++ < First) continue;
    Print (L"%s\r\n", Row);
    if (++Shown == Rows) break;
  }
  Print (L"Up/Down: pages  R: refresh  C: reconnect  ESC: exit\r\n");
  Print (L"P: RAM capture  L: log  B: BMP  M: RAM test  G: battery\r\n");
  // No terminal newline on the final footer: never scroll the bottom row.
  Print (L"Rows %u-%u / %u%s", Count ? First + 1 : 0,
         First + Shown, Count, mTruncated ? L" [TRUNCATED]" : L"");
}

EFI_STATUS EFIAPI UefiMain (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_INPUT_KEY Key; EFI_STATUS S; UINTN First = 0, Columns = 80, Rows = 25, Event;
  BOOLEAN ReportOnly = FALSE;
  EFI_EVENT Events[2], Poll = NULL;
  UINTN EventCount = 1, LastTopology;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *SavedScreen = NULL;
  VOID *ScreenshotBmp = NULL;
  UINTN ScreenshotSize = 0;
  T70_SD_FAT_IDENTITY ExportTargets[9];
  UINTN ExportCount = 0, ExportSize = 0;
  VOID *ExportData = NULL;
  T70_EXPORT_CHOICE ExportChoice = { T70ChoiceIdle, 0, 0 };
  T70_EXPORT_KIND ExportKind = T70ExportLog;
  UINT32 ExportNumber = 0;
  UINTN Width = 0, Height = 0;
  EFI_SIMPLE_TEXT_OUTPUT_MODE SavedMode;
  (VOID)SystemTable;
  S = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (!EFI_ERROR (S) && Loaded->LoadOptions != NULL && Loaded->LoadOptionsSize == sizeof (L"--report") &&
      CompareMem (Loaded->LoadOptions, L"--report", sizeof (L"--report")) == 0) ReportOnly = TRUE;
  Scan (SCAN_CONNECT_INTERNAL);
  if (ReportOnly) { Print (L"%s", mDeviceCheckLog); return EFI_SUCCESS; }
  if (gST->ConIn == NULL || gST->ConOut == NULL) return EFI_NOT_READY;
  SavedMode = *gST->ConOut->Mode;
  // The caller can retain its UI drawing state across StartImage. Preserve its
  // actual pixels, not just text mode, so leaving this full-screen app restores it.
  S = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (!EFI_ERROR (S) && Gop->Mode != NULL && Gop->Mode->Info != NULL) {
    Width = Gop->Mode->Info->HorizontalResolution; Height = Gop->Mode->Info->VerticalResolution;
    if (Width != 0 && Height != 0 && Width <= 4096 && Height <= 4096) {
      SavedScreen = AllocatePool (Width * Height * sizeof (*SavedScreen));
      if (SavedScreen != NULL && EFI_ERROR (Gop->Blt (Gop, SavedScreen, EfiBltVideoToBltBuffer, 0, 0, 0, 0, Width, Height, 0))) {
        FreePool (SavedScreen); SavedScreen = NULL;
      }
    }
  }
  gST->ConOut->QueryMode (gST->ConOut, gST->ConOut->Mode->Mode, &Columns, &Rows);
  if (Columns < 80 || Columns > 512 || Rows < 16 || Rows > 512) {
    if (SavedScreen != NULL) FreePool (SavedScreen);
    Print (L"DeviceCheck requires 80..512 columns and 16..512 rows.\r\n");
    return EFI_UNSUPPORTED;
  }
  mPageColumns = Columns;
  Rows -= 4;
  Events[0] = gST->ConIn->WaitForKey;
  S = gBS->CreateEvent (EVT_TIMER, TPL_APPLICATION, NULL, NULL, &Poll);
  if (!EFI_ERROR (S)) {
    S = gBS->SetTimer (Poll, TimerPeriodic, 5000000ULL);
    if (!EFI_ERROR (S)) { Events[1] = Poll; EventCount = 2; }
    else { gBS->CloseEvent (Poll); Poll = NULL; }
  }
  LastTopology = Topology ();
  Page (First, Rows);
  for (;;) {
    S = gBS->WaitForEvent (EventCount, Events, &Event);
    if (EFI_ERROR (S)) break;
    if (Event == 1) {
      if (ExportChoice.State == T70ChoiceSelect || ExportChoice.State == T70ChoiceConfirm) continue;
      UINTN Current = Topology ();
      if (Current != LastTopology) {
        // Refresh removable USB details without probing every SD/UFS sector.
        Scan (SCAN_TOPOLOGY_UPDATE);
        LastTopology = Topology ();
        if (First > LastVisualPage (Rows)) First = LastVisualPage (Rows);
        Page (First, Rows);
      }
      continue;
    }
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) continue;
    if (ExportChoice.State == T70ChoiceSelect || ExportChoice.State == T70ChoiceConfirm) {
      T70ChoiceKey (&ExportChoice, Key.ScanCode == SCAN_ESC ? 27U : Key.UnicodeChar);
      if (ExportChoice.State == T70ChoiceConfirm) {
        T70_SD_FAT_IDENTITY *Target = &ExportTargets[ExportChoice.Index];
        gST->ConOut->ClearScreen (gST->ConOut);
        Print (L"Selected SD FAT #%u: partition=%u media=%u\r\n",
               ExportChoice.Index + 1, Target->PartitionPath.PartitionNumber, Target->MediaId);
        Print (L"Start=%lx blocks=%lx block-bytes=%u\r\n",
               Target->PartitionPath.PartitionStart, Target->LastBlock + 1, Target->BlockSize);
        Print (L"Save %s: %u bytes. RAM original is retained.\r\n",
               ExportKind == T70ExportLog ? L"UTF-16LE log" : L"BMP screenshot", ExportSize);
        Print (L"Y: create a NEW file on THIS volume. Any other key cancels.\r\n");
        continue;
      }
      if (ExportChoice.State == T70ChoiceApproved) {
        EFI_STATUS Detail = EFI_SUCCESS;
        T70_EXPORT_RESULT Result;
        if (ExportNumber == MAX_UINT32) {
          Line (L"Export refused: filename sequence exhausted.");
        } else {
          Result = T70ExportSelectedSdFat (gBS, &ExportTargets[ExportChoice.Index],
                                          ExportKind, ++ExportNumber, ExportData, ExportSize, &Detail);
          if (Result.Status == T70ExportOk) {
            Line (L"Saved T70-%08x.%s: %u bytes; Flush/Close succeeded.",
                  ExportNumber, ExportKind == T70ExportLog ? L"txt" : L"bmp", Result.Written);
          } else {
            CONST CHAR16 *Names[] = { L"ok", L"rejected", L"create failed", L"write failed",
                                     L"short write", L"flush failed", L"close failed" };
            Line (L"Export FAILED: %s detail=%r",
                  Result.Status <= T70ExportCloseFailed ? Names[Result.Status] : L"unknown", Detail);
            Line (L"written=%u created=%u flush=%u close-error=%d",
                  Result.Written, Result.Created, Result.FlushSucceeded, Result.CloseError);
            Line (L"RAM retained; partial new file may remain. No overwrite/retry.");
          }
        }
      } else {
        Line (L"Manual export cancelled; no file saved.");
      }
      if (ExportData != NULL) FreePool (ExportData);
      ExportData = NULL; ExportSize = 0; ExportChoice.State = T70ChoiceIdle;
      First = LastVisualPage (Rows);
      Page (First, Rows);
      continue;
    }
    if (Key.ScanCode == SCAN_ESC || Key.UnicodeChar == 0x1b) break;
    if (Key.ScanCode == SCAN_DOWN) { if (First + Rows < VisualRowCount ()) First += Rows; }
    else if (Key.ScanCode == SCAN_UP) First = First > Rows ? First - Rows : 0;
    else if (Key.UnicodeChar == L'l' || Key.UnicodeChar == L'L' ||
             Key.UnicodeChar == L'b' || Key.UnicodeChar == L'B') {
      EFI_STATUS DiscoverStatus;
      ExportKind = (Key.UnicodeChar == L'l' || Key.UnicodeChar == L'L') ?
                     T70ExportLog : T70ExportScreenshot;
      if (ExportKind == T70ExportScreenshot && ScreenshotBmp == NULL) {
        Line (L"No RAM screenshot. Press P first; nothing was saved.");
      } else {
        ExportCount = 0;
        DiscoverStatus = T70DiscoverSdFatTargets (gBS, ExportTargets, 9, &ExportCount);
        if (EFI_ERROR (DiscoverStatus) || ExportCount == 0 || ExportCount > 9) {
          Line (L"No validated writable SD FAT target: %r. RO remains enforced.", DiscoverStatus);
        } else {
          ExportSize = ExportKind == T70ExportLog ?
                         (mDeviceCheckLogChars + 1) * sizeof (CHAR16) : ScreenshotSize;
          ExportData = AllocatePool (ExportSize);
          if (ExportData == NULL) {
            Line (L"Export snapshot allocation failed; no file saved.");
            ExportSize = 0;
          } else {
            if (ExportKind == T70ExportLog) {
              ((CHAR16 *)ExportData)[0] = 0xfeff;
              CopyMem ((CHAR16 *)ExportData + 1, mDeviceCheckLog,
                       mDeviceCheckLogChars * sizeof (CHAR16));
            } else {
              CopyMem (ExportData, ScreenshotBmp, ScreenshotSize);
            }
            T70ChoiceStart (&ExportChoice, (unsigned)ExportCount);
            gST->ConOut->ClearScreen (gST->ConOut);
            Print (L"Select SD FAT destination. No file has been written.\r\n");
            for (UINTN I = 0; I < ExportCount; I++) {
              Print (L"%u: partition=%u media=%u blocks=%lx\r\n", I + 1,
                     ExportTargets[I].PartitionPath.PartitionNumber,
                     ExportTargets[I].MediaId, ExportTargets[I].LastBlock + 1);
            }
            Print (L"Press listed number, then Y to confirm. Any other key cancels.\r\n");
            continue;
          }
        }
      }
      First = LastVisualPage (Rows);
    }
    else if (Key.UnicodeChar == L'p' || Key.UnicodeChar == L'P') {
      VOID *NewScreenshot = NULL;
      UINTN NewSize = 0;
      EFI_STATUS CaptureStatus;
      // Capture before logging or repainting: this is the visible diagnostic page,
      // never the pre-app SavedScreen used solely for exit restoration.
      CaptureStatus = CaptureBmp (gBS, Gop, &NewScreenshot, &NewSize);
      if (!EFI_ERROR (CaptureStatus)) {
        if (ScreenshotBmp != NULL) FreePool (ScreenshotBmp);
        ScreenshotBmp = NewScreenshot; ScreenshotSize = NewSize;
        Line (L"Screenshot: captured %u bytes in RAM; no file saved.", ScreenshotSize);
      } else {
        Line (L"Screenshot capture failed: %r; previous RAM capture retained.", CaptureStatus);
      }
      First = LastVisualPage (Rows);
    }
    else if (Key.UnicodeChar == L'm' || Key.UnicodeChar == L'M') {
      RunRamTest ();
      First = LastVisualPage (Rows);
    } else if (Key.UnicodeChar == L'g' || Key.UnicodeChar == L'G') {
      ReadGauge ();
      Scan (0U);
      LastTopology = Topology (); First = 0;
    } else if (Key.UnicodeChar == L'r' || Key.UnicodeChar == L'R' || Key.UnicodeChar == L'c' || Key.UnicodeChar == L'C') {
      Scan ((Key.UnicodeChar == L'c' || Key.UnicodeChar == L'C') ?
            (SCAN_CONNECT_USB | SCAN_CONNECT_INTERNAL) : 0U);
      LastTopology = Topology (); First = 0;
    } else continue;
    Page (First, Rows);
  }
  if (Poll != NULL) gBS->CloseEvent (Poll);
  if (ScreenshotBmp != NULL) FreePool (ScreenshotBmp);
  if (ExportData != NULL) FreePool (ExportData);
  gST->ConOut->SetAttribute (gST->ConOut, SavedMode.Attribute);
  gST->ConOut->SetCursorPosition (gST->ConOut, SavedMode.CursorColumn, SavedMode.CursorRow);
  gST->ConOut->EnableCursor (gST->ConOut, SavedMode.CursorVisible);
  if (SavedScreen != NULL) {
    Gop->Blt (Gop, SavedScreen, EfiBltBufferToVideo, 0, 0, 0, 0, Width, Height, 0);
    FreePool (SavedScreen);
  }
  return EFI_ERROR (S) ? S : EFI_SUCCESS;
}
