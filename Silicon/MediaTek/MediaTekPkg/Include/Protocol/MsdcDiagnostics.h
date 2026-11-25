/** @file
  Cached, read-only diagnostics for the MediaTek MSDC controller.

  This protocol returns a fixed-size snapshot. GetSnapshot does not sample
  GPIO, access controller registers, issue SD commands, or access disk data.
  Call GetSnapshot only from TPL_APPLICATION through TPL_NOTIFY.
**/
#ifndef __MEDIATEK_MSDC_DIAGNOSTICS_PROTOCOL_H__
#define __MEDIATEK_MSDC_DIAGNOSTICS_PROTOCOL_H__

#include <Uefi.h>

#define MSDC_DIAGNOSTICS_REVISION  1U

typedef enum {
  MsdcDiagStageNone = 0,
  MsdcDiagStageDriverConfig,
  MsdcDiagStageCardDetect,
  MsdcDiagStagePrivateAllocation,
  MsdcDiagStageDevicePathAllocation,
  MsdcDiagStageHostMmioLookup,
  MsdcDiagStageTopMmioLookup,
  MsdcDiagStageGpioSetup,
  MsdcDiagStageClockEnable,
  MsdcDiagStageControllerReset,
  MsdcDiagStageFifoClear,
  MsdcDiagStageHostConfiguration,
  MsdcDiagStagePollSetup,
  MsdcDiagStageControllerPublish,
  MsdcDiagStageCardPower,
  MsdcDiagStageIdentificationClock,
  MsdcDiagStageCardReset,
  MsdcDiagStageInterfaceCondition,
  MsdcDiagStageOperatingCondition,
  MsdcDiagStageCid,
  MsdcDiagStageRca,
  MsdcDiagStageCapacity,
  MsdcDiagStageBusMode,
  MsdcDiagStageFinalCardDetect,
  MsdcDiagStageReady
} MSDC_DIAGNOSTIC_STAGE;

typedef struct {
  UINT32     Revision;
  UINT32     Size;
  UINT32     HostIndex;
  UINT32     LastDriverStage;
  UINT32     LastInitStage;
  UINT32     IdentifyAttempts;
  UINT32     IdentifyFailures;
  UINT32     DetectSamples;
  UINT32     CardType;          // 0=unknown, 1=eMMC, 2=SD
  UINT64     LastBlock;
  EFI_STATUS CardDetectStatus;  // status returned by the last GPIO sample
  EFI_STATUS LastDriverStatus;  // controller-driver setup status
  EFI_STATUS LastInitStatus;    // status for the last identification attempt
  EFI_STATUS DevicePathStatus;  // status before MsdcBuildDevicePath maps to NOT_FOUND
  BOOLEAN    DetectSampleValid;
  BOOLEAN    CardPresent;       // mapped result, meaningful when sample-valid
  BOOLEAN    MediaReady;
  BOOLEAN    Initializing;
} MSDC_DIAGNOSTICS_RECORD;

typedef struct _MSDC_DIAGNOSTICS_PROTOCOL MSDC_DIAGNOSTICS_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *MSDC_DIAGNOSTICS_GET_SNAPSHOT)(
  IN  CONST MSDC_DIAGNOSTICS_PROTOCOL *This,
  IN  UINT32                          BufferSize,
  OUT UINT32                          *RequiredSize,
  OUT MSDC_DIAGNOSTICS_RECORD         *Snapshot
  );

struct _MSDC_DIAGNOSTICS_PROTOCOL {
  MSDC_DIAGNOSTICS_GET_SNAPSHOT GetSnapshot;
};

extern EFI_GUID gMediaTekMsdcDiagnosticsProtocolGuid;

#endif
