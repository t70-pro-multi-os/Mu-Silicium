#include "MsdcDxe.h"
#include <Protocol/BlockIo.h>

STATIC MSDC_DIAGNOSTICS_RECORD mMsdcDiagnostics = {
  .Revision = MSDC_DIAGNOSTICS_REVISION,
  .Size = sizeof (MSDC_DIAGNOSTICS_RECORD),
  .HostIndex = 1,
  .LastDriverStage = MsdcDiagStageNone,
  .LastInitStage = MsdcDiagStageNone,
  .CardType = UnknownCard,
  .CardDetectStatus = EFI_NOT_READY,
  .LastDriverStatus = EFI_NOT_READY,
  .LastInitStatus = EFI_NOT_READY,
  .DevicePathStatus = EFI_NOT_READY
};

STATIC
EFI_STATUS
EFIAPI
MsdcGetDiagnosticsSnapshot (
  IN  CONST MSDC_DIAGNOSTICS_PROTOCOL *This,
  IN  UINT32                          BufferSize,
  OUT UINT32                          *RequiredSize,
  OUT MSDC_DIAGNOSTICS_RECORD         *Snapshot
  )
{
  EFI_TPL OldTpl;

  if ((This == NULL) || (RequiredSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  *RequiredSize = sizeof (mMsdcDiagnostics);
  if (Snapshot == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BufferSize < sizeof (mMsdcDiagnostics)) {
    return EFI_BUFFER_TOO_SMALL;
  }

  // Call at TPL_APPLICATION through TPL_NOTIFY. Raising to NOTIFY blocks the
  // media timer (TPL_CALLBACK) without lowering a caller already at NOTIFY.
  // Copy only the cached record; do not sample GPIO or touch the controller.
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  CopyMem (Snapshot, &mMsdcDiagnostics, sizeof (mMsdcDiagnostics));
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC MSDC_DIAGNOSTICS_PROTOCOL mMsdcDiagnosticsProtocol = {
  MsdcGetDiagnosticsSnapshot
};

STATIC
VOID
MsdcRecordInitStatus (
  IN MSDC_DIAGNOSTIC_STAGE Stage,
  IN EFI_STATUS            Status
  )
{
  if (mMsdcDiagnostics.HostIndex != 1) {
    return;
  }
  mMsdcDiagnostics.LastInitStage = Stage;
  mMsdcDiagnostics.LastInitStatus = Status;
}

STATIC
VOID
MsdcRecordDriverStatus (
  IN MSDC_DIAGNOSTIC_STAGE Stage,
  IN EFI_STATUS            Status
  )
{
  if (mMsdcDiagnostics.HostIndex != 1) {
    return;
  }
  mMsdcDiagnostics.LastDriverStage = Stage;
  mMsdcDiagnostics.LastDriverStatus = Status;
}

STATIC
VOID
MsdcRecordCardDetect (
  IN EFI_STATUS Status,
  IN BOOLEAN    Present
  )
{
  if (mMsdcDiagnostics.DetectSamples != MAX_UINT32) {
    mMsdcDiagnostics.DetectSamples++;
  }
  mMsdcDiagnostics.CardDetectStatus = Status;
  mMsdcDiagnostics.DetectSampleValid = TRUE;
  mMsdcDiagnostics.CardPresent = !EFI_ERROR (Status) && Present;
}

STATIC
VOID
MsdcSyncMediaDiagnostics (
  IN MSDC_PRIVATE_DATA *Private
  )
{
  if (Private->Index != 1) {
    return;
  }
  mMsdcDiagnostics.HostIndex = Private->Index;
  mMsdcDiagnostics.CardType = (UINT32)Private->SdInfo.CardType;
  mMsdcDiagnostics.LastBlock = Private->SdInfo.LastBlock;
  mMsdcDiagnostics.MediaReady = Private->MediaReady;
  mMsdcDiagnostics.Initializing = Private->Initializing;
}

MSDC_PRIVATE_DATA gMSDCPrivateDataTemplate = {
  MSDC_PRIVATE_SIGNATURE, // Signature
  NULL, // ControllerHandle
  {
    sizeof (UINT32),
    MsdcPassThru, // PassThru
    MsdcGetNextSlot, // GetNextSlot
    MsdcBuildDevicePath, // BuildDevicePath
    MsdcGetSlotNumber, // GetSlotNumber
    MsdcResetDevice  // ResetDevice
  }, // PassThru
  0, // Index
  0, // MsdcMmioReg
  0, // TopMmioReg
  {0}, // HostData
  {0}, // SdInfo
  0, 0 // Command timing
};

typedef struct {
  VENDOR_DEVICE_PATH Mmc;
  EFI_DEVICE_PATH    End;
} MSDC_DEVICE_PATH;

MSDC_DEVICE_PATH gMSDCDevicePathTemplate = {
  {  // Mmc (VENDOR_DEVICE_PATH)
    {  // Header
      HARDWARE_DEVICE_PATH,      // Type
      HW_VENDOR_DP,              // SubType
      {                          // Length (UINT8 array style)
        (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
        (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)
      }
    },
    // Vendor GUID
    { 0xb615f1f5, 0x5088, 0x43cd, { 0x80, 0x9c, 0xa1, 0x6e, 0x52, 0x48, 0x7d, 0x00 } }
  },
  {  // End (EFI_DEVICE_PATH)
    END_DEVICE_PATH_TYPE,        // Type
    END_ENTIRE_DEVICE_PATH_SUBTYPE,  // SubType
    {                            // Length (UINT8 array style)
      sizeof(EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

EMMC_DEVICE_PATH gEMMCDevicePathTemplate = {
  {
    MESSAGING_DEVICE_PATH,
    MSG_EMMC_DP,
    {
      (UINT8)(sizeof (EMMC_DEVICE_PATH)),
      (UINT8)((sizeof (EMMC_DEVICE_PATH)) >> 8)
    }
  },
  0
};

SD_DEVICE_PATH gSDDevicePathTemplate = {
  {
    MESSAGING_DEVICE_PATH,
    MSG_SD_DP,
    {
      (UINT8)(sizeof (SD_DEVICE_PATH)),
      (UINT8)((sizeof (SD_DEVICE_PATH)) >> 8)
    }
  },
  0
};

#define MSDC_WAIT_US 100000
#define MSDC_DEFAULT_TIMEOUT_NS 1000000000ULL

STATIC
BOOLEAN
MsdcExpired (MSDC_PRIVATE_DATA *Private)
{
  return GetTimeInNanoSecond (GetPerformanceCounter ()) - Private->StartNs >= Private->TimeoutNs;
}

STATIC
EFI_STATUS
MsdcWaitMask (MSDC_PRIVATE_DATA *Private, UINT32 Offset, UINT32 Mask, BOOLEAN Set)
{
  UINT32 Value;
  UINTN Elapsed;
  for (Elapsed = 0; Elapsed < MSDC_WAIT_US; Elapsed += 10) {
    MsdcRead (Private, Offset, &Value);
    if (((Value & Mask) != 0) == Set) { return EFI_SUCCESS; }
    MicroSecondDelay (10);
  }
  return EFI_TIMEOUT;
}

// Slot 1 has the board GPIO card detect. A failed/absent sample invalidates
// the old card immediately. Its upper-layer child must be disconnected before
// a later insertion is initialized; it must never retain the previous capacity.
STATIC
VOID
MsdcInvalidateMedia (MSDC_PRIVATE_DATA *Private)
{
  if (Private->MediaReady || Private->MediaAnnounced || Private->ChildConnectionPending) {
    if (!Private->ChildWithdrawalPending) {
      Private->ChildRetryCountdown = 0;
      Private->ChildRetryDelay = 0;
    }
    Private->ChildWithdrawalPending = TRUE;
  }
  Private->MediaReady = FALSE;
  Private->ChildConnectionPending = FALSE;
  Private->SdInfo.CardType = UnknownCard;
  Private->SdInfo.LastBlock = 0;
  Private->StableSamples = 0;
  MsdcSyncMediaDiagnostics (Private);
}

typedef enum {
  MsdcChildUnknown,
  MsdcChildAbsent,
  MsdcChildUnusable,
  MsdcChildReady
} MSDC_CHILD_STATE;

STATIC
BOOLEAN
MsdcDevicePathsMatch (
  EFI_DEVICE_PATH_PROTOCOL *First,
  EFI_DEVICE_PATH_PROTOCOL *Second
  )
{
  UINTN FirstSize;
  UINTN SecondSize;

  if ((First == NULL) || (Second == NULL)) {
    return FALSE;
  }

  FirstSize = GetDevicePathSize (First);
  SecondSize = GetDevicePathSize (Second);
  return (FirstSize == SecondSize) && (CompareMem (First, Second, FirstSize) == 0);
}

// ReinstallProtocolInterface reports whether protocol publication succeeded,
// not whether the DXE core managed to start the SD child. Find the exact slot
// child and require both the parent/child open relation and usable Block I/O.
STATIC
EFI_STATUS
MsdcGetChildState (
  MSDC_PRIVATE_DATA *Private,
  MSDC_CHILD_STATE *ChildState
  )
{
  EFI_STATUS Status;
  EFI_STATUS LocateStatus;
  EFI_DEVICE_PATH_PROTOCOL *ControllerPath;
  EFI_DEVICE_PATH_PROTOCOL *ExpectedPath;
  EFI_DEVICE_PATH_PROTOCOL *RemainingPath;
  EFI_DEVICE_PATH_PROTOCOL *ChildPath;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *OpenInfo;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_HANDLE LocatedChild;
  EFI_HANDLE RelatedChild;
  UINTN OpenInfoCount;
  UINTN Index;
  BOOLEAN HasChildRelation;
  BOOLEAN HasMatchingRelation;
  BOOLEAN AmbiguousChild;
  BOOLEAN PathExists;

  if ((Private == NULL) || (ChildState == NULL) || (Private->ControllerHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *ChildState = MsdcChildUnknown;
  ControllerPath = NULL;
  ExpectedPath = NULL;
  OpenInfo = NULL;
  LocatedChild = NULL;
  RelatedChild = NULL;
  OpenInfoCount = 0;
  HasChildRelation = FALSE;
  HasMatchingRelation = FALSE;
  AmbiguousChild = FALSE;
  PathExists = FALSE;

  Status = gBS->HandleProtocol (
                  Private->ControllerHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&ControllerPath
                  );
  if (EFI_ERROR (Status) || (ControllerPath == NULL)) {
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  ExpectedPath = AppendDevicePathNode (
                   ControllerPath,
                   (EFI_DEVICE_PATH_PROTOCOL *)&gSDDevicePathTemplate
                   );
  if (ExpectedPath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->OpenProtocolInformation (
                  Private->ControllerHandle,
                  &gEfiSdMmcPassThruProtocolGuid,
                  &OpenInfo,
                  &OpenInfoCount
                  );
  if (EFI_ERROR (Status) || ((OpenInfoCount != 0) && (OpenInfo == NULL))) {
    if (EFI_ERROR (Status)) {
      // Failed enumeration does not transfer ownership of its output buffer.
      OpenInfo = NULL;
    }
    if (!EFI_ERROR (Status)) {
      Status = EFI_COMPROMISED_DATA;
    }
    goto Cleanup;
  }

  for (Index = 0; Index < OpenInfoCount; Index++) {
    if ((OpenInfo[Index].Attributes & EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER) == 0) {
      continue;
    }

    HasChildRelation = TRUE;
    if (OpenInfo[Index].ControllerHandle == NULL) {
      continue;
    }

    ChildPath = NULL;
    Status = gBS->HandleProtocol (
                    OpenInfo[Index].ControllerHandle,
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&ChildPath
                    );
    if (EFI_ERROR (Status) || (ChildPath == NULL) ||
        !MsdcDevicePathsMatch (ChildPath, ExpectedPath)) {
      continue;
    }

    if (HasMatchingRelation && (RelatedChild != OpenInfo[Index].ControllerHandle)) {
      // Two child handles claiming this single-slot path are ambiguous.
      AmbiguousChild = TRUE;
      continue;
    }

    HasMatchingRelation = TRUE;
    RelatedChild = OpenInfo[Index].ControllerHandle;
  }

  RemainingPath = ExpectedPath;
  LocateStatus = gBS->LocateDevicePath (
                    &gEfiDevicePathProtocolGuid,
                    &RemainingPath,
                    &LocatedChild
                    );
  if (!EFI_ERROR (LocateStatus) && (LocatedChild != NULL) && IsDevicePathEnd (RemainingPath)) {
    PathExists = TRUE;
  } else if (EFI_ERROR (LocateStatus) && (LocateStatus != EFI_NOT_FOUND)) {
    Status = LocateStatus;
    goto Cleanup;
  }

  if (!PathExists && !HasChildRelation) {
    *ChildState = MsdcChildAbsent;
    Status = EFI_SUCCESS;
    goto Cleanup;
  }

  *ChildState = MsdcChildUnusable;
  if (AmbiguousChild || !PathExists || !HasMatchingRelation ||
      (LocatedChild != RelatedChild)) {
    Status = EFI_SUCCESS;
    goto Cleanup;
  }

  BlockIo = NULL;
  Status = gBS->HandleProtocol (
                  RelatedChild,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo
                  );
  if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL)) {
    Status = EFI_SUCCESS;
    goto Cleanup;
  }

  if (BlockIo->Media->MediaPresent &&
      (BlockIo->Media->BlockSize == BLOCK_SIZE) &&
      (Private->SdInfo.LastBlock != 0) &&
      (BlockIo->Media->LastBlock == Private->SdInfo.LastBlock)) {
    *ChildState = MsdcChildReady;
  }
  Status = EFI_SUCCESS;

Cleanup:
  if (OpenInfo != NULL) {
    FreePool (OpenInfo);
  }
  if (ExpectedPath != NULL) {
    FreePool (ExpectedPath);
  }
  return Status;
}

STATIC
BOOLEAN
MsdcChildRetryDue (MSDC_PRIVATE_DATA *Private)
{
  if (Private->ChildRetryCountdown != 0) {
    Private->ChildRetryCountdown--;
    return FALSE;
  }

  return TRUE;
}

STATIC
VOID
MsdcScheduleChildRetry (MSDC_PRIVATE_DATA *Private)
{
  if (Private->ChildRetryDelay == 0) {
    Private->ChildRetryDelay = 1;
  }

  Private->ChildRetryCountdown = Private->ChildRetryDelay;
  if (Private->ChildRetryDelay < 50) {
    Private->ChildRetryDelay = (UINT8)MIN (50, Private->ChildRetryDelay * 2);
  }
}

STATIC
VOID
MsdcResetChildRetry (MSDC_PRIVATE_DATA *Private)
{
  Private->ChildRetryCountdown = 0;
  Private->ChildRetryDelay = 0;
}

STATIC
EFI_STATUS
MsdcCheckMedia (MSDC_PRIVATE_DATA *Private)
{
  BOOLEAN Present = FALSE;
  EFI_STATUS Status;
  if (Private->Stopped) { return EFI_UNSUPPORTED; }
  if (Private->Index != 1) { return EFI_SUCCESS; }
  Status = GetCardPresent (Private->Index, &Present);
  MsdcRecordCardDetect (Status, Present);
  if (EFI_ERROR (Status) || !Present) {
    MsdcInvalidateMedia (Private);
    // Removal starts a fresh insertion; an electrical failure retains backoff.
    if (!EFI_ERROR (Status)) {
      Private->IdentifyRetryCountdown = 0;
      Private->IdentifyRetryDelay = 0;
    }
    return EFI_ERROR (Status) ? Status : EFI_NO_MEDIA;
  }
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
MsdcReadOnlyCommand (MSDC_PRIVATE_DATA *Private, EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet)
{
  UINT16 Command = Packet->SdMmcCmdBlk->CommandIndex;
  if (Packet->OutTransferLength != 0) { return FALSE; }
  // CMD6 is safe here only for the SD function/bus-width commands, not eMMC EXT_CSD writes.
  if (Command == 6) { return Private->Index == 1; }
  switch (Command) {
  case 0: case 1: case 2: case 3: case 7: case 8: case 9: case 10:
  case 12: case 13: case 16: case 17: case 18: case 41: case 51: case 55: case 58:
    return TRUE;
  default:
    return FALSE;
  }
}

EFI_STATUS
MsdcPassThru (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  UINT8 Slot,
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,
  EFI_EVENT Event)
{
  MSDC_PRIVATE_DATA *Private;
  EFI_STATUS Status;
  EFI_TPL OldTpl;

  if ((This == NULL) || (Packet == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Packet->SdMmcCmdBlk == NULL) || (Packet->SdMmcStatusBlk == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Packet->OutDataBuffer == NULL) && (Packet->OutTransferLength != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Packet->InDataBuffer == NULL) && (Packet->InTransferLength != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Slot != 0) { return EFI_INVALID_PARAMETER; }
  // This host is synchronous-only: the standard requires ignoring Event.
  if ((Packet->SdMmcCmdBlk->CommandIndex > 63) ||
      ((Packet->InTransferLength != 0) && (Packet->OutTransferLength != 0))) {
    return EFI_INVALID_PARAMETER;
  }
  Private = MSDC_PRIVATE_FROM_THIS (This);
  if (Private->Stopped) {
    Packet->TransactionStatus = EFI_UNSUPPORTED;
    return EFI_UNSUPPORTED;
  }
  if (gPlatformInfo.ReadOnly && !MsdcReadOnlyCommand (Private, Packet)) {
    return EFI_WRITE_PROTECTED;
  }
  // The media timer runs at TPL_CALLBACK. Keep synchronous PIO and slot
  // identification serialized with it, including the final presence sample.
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  if (Private->CommandActive) {
    Status = EFI_NOT_READY;
  } else {
    Private->CommandActive = TRUE;
    Status = MsdcCheckMedia (Private);
    if (!EFI_ERROR (Status) && !Private->MediaReady && !Private->Initializing) {
      Status = EFI_NOT_READY;
    }
    if (!EFI_ERROR (Status)) {
      Private->StartNs = GetTimeInNanoSecond (GetPerformanceCounter ());
      // Honor the caller's microsecond transfer budget (large SD reads
      // legitimately exceed five seconds). Zero still uses a bounded default;
      // saturate the conversion rather than wrapping very large requests.
      Private->TimeoutNs = Packet->Timeout == 0 ? MSDC_DEFAULT_TIMEOUT_NS :
                          MIN (Packet->Timeout, MAX_UINT64 / 1000ULL) * 1000ULL;
      Status = MsdcSendCmd (Private, Packet);
      if (!EFI_ERROR (Status)) { Status = MsdcCheckMedia (Private); }
    }
    Private->CommandActive = FALSE;
  }
  Packet->TransactionStatus = Status;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
MsdcGetNextSlot (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  UINT8 *Slot)
{
  if (This == NULL || Slot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (*Slot == 0xFF) {
    *Slot = 0;
    return EFI_SUCCESS;
  }

  return *Slot == 0 ? EFI_NOT_FOUND : EFI_INVALID_PARAMETER;
}

EFI_STATUS
MsdcBuildDevicePath (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  UINT8 Slot,
  EFI_DEVICE_PATH_PROTOCOL **DevicePath)
{
  MSDC_PRIVATE_DATA *Private;
  EFI_DEVICE_PATH_PROTOCOL *Node;
  EFI_STATUS Status;
  EFI_STATUS DiagnosticStatus;
  EFI_TPL OldTpl;

  if (This == NULL || DevicePath == NULL || Slot != 0) {
    return EFI_INVALID_PARAMETER;
  }
  *DevicePath = NULL;
  Private = MSDC_PRIVATE_FROM_THIS (This);
  if (Private->Stopped) {
    if (Private->Index == 1) { mMsdcDiagnostics.DevicePathStatus = EFI_NOT_FOUND; }
    return EFI_NOT_FOUND;
  }
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  Status = MsdcCheckMedia (Private);
  if (EFI_ERROR (Status)) {
    DiagnosticStatus = Status;
    Status = EFI_NOT_FOUND;
  } else if (!Private->MediaReady) {
    DiagnosticStatus = EFI_NOT_READY;
    Status = EFI_NOT_FOUND;
  } else {
    Node = NULL;
    if (Private->SdInfo.CardType == EmmcCard) {
      Node = AllocateCopyPool (sizeof (gEMMCDevicePathTemplate), &gEMMCDevicePathTemplate);
    } else if (Private->SdInfo.CardType == SdCard) {
      Node = AllocateCopyPool (sizeof (gSDDevicePathTemplate), &gSDDevicePathTemplate);
    } else {
      Status = EFI_NOT_FOUND;
    }
    if (!EFI_ERROR (Status)) {
      if (Node == NULL) { Status = EFI_OUT_OF_RESOURCES; }
      else { *DevicePath = Node; }
    }
    DiagnosticStatus = Status;
  }
  if (Private->Index == 1) { mMsdcDiagnostics.DevicePathStatus = DiagnosticStatus; }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
MsdcGetSlotNumber (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  EFI_DEVICE_PATH_PROTOCOL *DevicePath,
  UINT8 *Slot)
{
  MSDC_PRIVATE_DATA *Private;

  if ((This == NULL) || (DevicePath == NULL) || (Slot == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = MSDC_PRIVATE_FROM_THIS (This);

  if ((DevicePath->Type != MESSAGING_DEVICE_PATH) ||
      (DevicePath->SubType != (Private->Index == 0 ? MSG_EMMC_DP : MSG_SD_DP)) ||
      (DevicePathNodeLength (DevicePath) != (Private->Index == 0 ?
                                           sizeof (EMMC_DEVICE_PATH) : sizeof (SD_DEVICE_PATH))))
  {
    return EFI_UNSUPPORTED;
  }

  if (((SD_DEVICE_PATH *)DevicePath)->SlotNumber != 0) {
    return EFI_UNSUPPORTED;
  }
  *Slot = 0;

  return EFI_SUCCESS;
}

EFI_STATUS
MsdcResetDevice (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  UINT8 Slot)
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  if (This == NULL || Slot != 0) { return EFI_INVALID_PARAMETER; }
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  Status = MsdcCheckMedia (MSDC_PRIVATE_FROM_THIS (This));
  gBS->RestoreTPL (OldTpl);
  return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
}

EFI_STATUS
MsdcReset (MSDC_PRIVATE_DATA *Private)
{
  MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_RST);
  return MsdcWaitMask (Private, MSDC_CFG, MSDC_CFG_RST, FALSE);
}

EFI_STATUS
MsdcClearFifo (MSDC_PRIVATE_DATA *Private)
{
  MsdcSetBits (Private, MSDC_FIFOCS, MSDC_FIFOCS_CLR);
  return MsdcWaitMask (Private, MSDC_FIFOCS, MSDC_FIFOCS_CLR, FALSE);
}

VOID
MsdcClearInterrupts (
  MSDC_PRIVATE_DATA* Private)
{
  UINT32 Reg;
  MsdcRead (Private, MSDC_INT, &Reg);
  MsdcWrite (Private, MSDC_INT, Reg);
}

VOID
MsdcSetTimeout (
  MSDC_PRIVATE_DATA* Private)
{
  UINT32 CfgReg, Timeout, ClkNs;

  ClkNs = 1000000000 / Private->HostData.Sclk;
  Timeout = (Private->HostData.TimeoutNs + ClkNs - 1) / ClkNs + Private->HostData.TimeoutClks;
  Timeout = (Timeout + (1 << SCLK_CYCLES_SHIFT) - 1) >> SCLK_CYCLES_SHIFT;
  Timeout = Timeout > 1 ? Timeout - 1 : 0;
  Timeout = Timeout > 255 ? 255 : Timeout;

  MsdcRead (Private, SDC_CFG, &CfgReg);
  // Clear BIT24:BIT31
  CfgReg &= ~(0xffU << 24);

  CfgReg |= Timeout << 24;
  MsdcWrite (Private, SDC_CFG, CfgReg);
}

VOID
MsdcSetBusWidth (
  MSDC_PRIVATE_DATA* Private,
  UINT32 Width)
{
  UINT32 CfgReg, BusWidth;

  switch (Width) {
    case 1:
      BusWidth = MSDC_BUS_WIDTH_1;
      break;
    case 4:
      BusWidth = MSDC_BUS_WIDTH_4;
      break;
    case 8:
      BusWidth = MSDC_BUS_WIDTH_8;
      break;
    default:
      BusWidth = MSDC_BUS_WIDTH_1;
      break;
  }

  MsdcRead (Private, SDC_CFG, &CfgReg);
  // Clear BIT16:BIT17
  CfgReg &= ~(3 << SDC_CFG_BUS_WIDTH_SHIFT);

  CfgReg |= BusWidth << SDC_CFG_BUS_WIDTH_SHIFT;
  MsdcWrite (Private, SDC_CFG, CfgReg);
}

EFI_STATUS
MsdcSetMclk (
  MSDC_PRIVATE_DATA* Private,
  UINT32 Hz)
{
  EFI_STATUS Status;
  UINTN SourceClock, NewSclk;
  UINT32 Div, Mode, OldCfg, CfgReg;
  BOOLEAN DividerWritten = FALSE;
  if ((Hz == 0) || (Hz > 200000000)) { return EFI_INVALID_PARAMETER; }
  Status = GetSourceClockRate (Private->Index, &SourceClock);
  if (EFI_ERROR (Status)) { return Status; }
  if ((SourceClock == 0) || (SourceClock > MAX_UINT32)) { return EFI_DEVICE_ERROR; }

  if (Hz >= SourceClock) {
    // Ignore divisor
    Div = 0;
    Mode = MSDC_MCLK_NO_DIV;
    NewSclk = SourceClock;
  } else {
    // Divisor mode
    Mode = MSDC_MCLK_DIV;
    if (Hz >= (SourceClock >> 1)) {
      Div = 0; /* Will divide source clock 1/2 */
      NewSclk = SourceClock >> 1;
    } else {
      Div = (SourceClock + ((Hz << 2) - 1)) / (Hz << 2);
      if (Div > 0xfffU) { return EFI_UNSUPPORTED; }
      NewSclk = (SourceClock >> 2) / Div;
    }
  }

  MsdcRead (Private, MSDC_CFG, &OldCfg);
  CfgReg = OldCfg & ~MSDC_CFG_CCKPD;
  // Clear CCKMD (BIT20:21)
  CfgReg &= ~(3 << 20);
  // Clear CCKDIV (BIT8:19)
  CfgReg &= ~(0xfff << 8);
  // Clear hs400 div
  CfgReg &= ~(MSDC_CFG_HS400CKMD);
  // Set values
  CfgReg |= (Mode << 20);
  CfgReg |= (Div << 8);

  // Keep card clock gated through the entire divider change.
  MsdcWrite (Private, MSDC_CFG, OldCfg & ~MSDC_CFG_CCKPD);
  Status = SourceClockControl (Private->Index, FALSE);
  if (EFI_ERROR (Status)) { goto RestoreClock; }
  MsdcWrite (Private, MSDC_CFG, CfgReg);
  DividerWritten = TRUE;
  Status = SourceClockControl (Private->Index, TRUE);
  if (EFI_ERROR (Status)) { goto RestoreClock; }
  Status = MsdcWaitMask (Private, MSDC_CFG, MSDC_CFG_CCKSB, TRUE);
  if (EFI_ERROR (Status)) { goto RestoreClock; }

  MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_CCKPD);
  Private->HostData.Sclk = NewSclk;
  MsdcSetTimeout (Private);
  return EFI_SUCCESS;

RestoreClock:
  if (!DividerWritten || !EFI_ERROR (SourceClockControl (Private->Index, FALSE))) {
    if (DividerWritten) MsdcWrite (Private, MSDC_CFG, OldCfg & ~MSDC_CFG_CCKPD);
    if (!EFI_ERROR (SourceClockControl (Private->Index, TRUE)) &&
        !EFI_ERROR (MsdcWaitMask (Private, MSDC_CFG, MSDC_CFG_CCKSB, TRUE))) {
      if (OldCfg & MSDC_CFG_CCKPD) MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_CCKPD);
      return Status;
    }
  }
  Private->HostData.Sclk = 0;
  return Status;
}

VOID
MsdcCheckBusy (
  MSDC_PRIVATE_DATA* Private,
  BOOLEAN WaitData,
  BOOLEAN *IsBusy)
{
  UINT32 Reg;
  UINT32 Mask = SDC_STS_CMDBUSY | (WaitData ? SDC_STS_BUSY : 0);
  MsdcRead (Private, SDC_STS, &Reg);
  *IsBusy = (Reg & Mask) != 0;
}

#define MSDC_INT_CMDSTS (MSDC_INT_CMDRDY  | MSDC_INT_CMDTMO  | MSDC_INT_CMDCRCERR | \
                         MSDC_INT_ACMDRDY | MSDC_INT_ACMDTMO | MSDC_INT_ACMDCRCERR)
#define MSDC_INT_DATSTS (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_XFER_COMPL)

EFI_STATUS
MsdcIntTrackError (
  MSDC_PRIVATE_DATA* Private,
  UINT32 IntMask,
  UINT32 IntStatus)
{
  // Clear interrupts
  MsdcWrite (Private, MSDC_INT, IntStatus & IntMask);

  // A corrupt response must not be classified as an absent CMD8 response.
  if (IntStatus & MSDC_INT_CMDCRCERR) {
    return EFI_CRC_ERROR;
  }

  if (IntStatus & MSDC_INT_CMDTMO) {
    return EFI_TIMEOUT;
  }

  if (IntStatus & MSDC_INT_ACMDTMO) {
    return EFI_TIMEOUT;
  }

  if (IntStatus & MSDC_INT_ACMDCRCERR) {
    return EFI_CRC_ERROR;
  }

  if (IntStatus & MSDC_INT_DATTMO) {
    return EFI_TIMEOUT;
  }

  if (IntStatus & MSDC_INT_DATCRCERR) {
    return EFI_CRC_ERROR;
  }

  return EFI_DEVICE_ERROR;
}

EFI_STATUS
MsdcPollInterrupts (
  MSDC_PRIVATE_DATA* Private,
  UINT32 ExpectedInterrupts,
  UINT32 SuccessInterrupts)
{
  UINT32 IntStatus;
  EFI_STATUS Status;

  while (TRUE) {
    Status = MsdcCheckMedia (Private);
    if (EFI_ERROR (Status)) { return Status; }
    MsdcRead (Private, MSDC_INT, &IntStatus);
    // ACMDRDY is a separate successful auto-command notification. Leave it
    // pending unless explicitly requested as success; it must neither fail nor
    // complete the main-command poll. Timeout/CRC errors still take priority.
    if (IntStatus & ExpectedInterrupts & ~(SuccessInterrupts | MSDC_INT_ACMDRDY)) {
      return MsdcIntTrackError (Private, ExpectedInterrupts, IntStatus);
    }
    if (IntStatus & SuccessInterrupts) { break; }
    if (MsdcExpired (Private)) { return EFI_TIMEOUT; }
    MicroSecondDelay (1);
  }

  if (IntStatus & SuccessInterrupts) {
    MsdcWrite (Private, MSDC_INT, IntStatus & SuccessInterrupts);
    return EFI_SUCCESS;
  }

  return MsdcIntTrackError (Private, ExpectedInterrupts, IntStatus);
}

VOID
MsdcFifoRxBytes (
  MSDC_PRIVATE_DATA* Private,
  UINT32 *RxBytes)
{
  UINT32 FifoCs;

  MsdcRead (Private, MSDC_FIFOCS, &FifoCs);
  *RxBytes = FifoCs & 0xff;
}

VOID
MsdcFifoTxBytes (
  MSDC_PRIVATE_DATA* Private,
  UINT32 *TxBytes)
{
  UINT32 FifoCs;

  MsdcRead (Private, MSDC_FIFOCS, &FifoCs);
  *TxBytes = (FifoCs >> 16) & 0xff;
}

VOID
MsdcFifoRead (
  MSDC_PRIVATE_DATA* Private,
  UINT8 *ByteBuffer,
  UINT32 BufferLength)
{
  UINT32 RemainSize = BufferLength;

  // Pointer align
  while ((UINTN)ByteBuffer % 4 != 0 && RemainSize > 0) {
    *ByteBuffer = MmioRead8 (Private->MsdcMmioReg + MSDC_RXDATA);
    ByteBuffer++;
    RemainSize--;
  }

  while (RemainSize >= 4) {
    *(UINT32 *)ByteBuffer = MmioRead32 (Private->MsdcMmioReg + MSDC_RXDATA);
    ByteBuffer += 4;
    RemainSize -= 4;
  }

  while (RemainSize) {
    *ByteBuffer = MmioRead8 (Private->MsdcMmioReg + MSDC_RXDATA);
    ByteBuffer++;
    RemainSize--;
  }
}

VOID
MsdcFifoWrite (
  MSDC_PRIVATE_DATA* Private,
  UINT8 *ByteBuffer,
  UINT32 BufferLength)
{
  UINT32 RemainSize = BufferLength;

  // Pointer align
  while ((UINTN)ByteBuffer % 4 != 0 && RemainSize > 0) {
    MmioWrite8 (Private->MsdcMmioReg + MSDC_TXDATA, *ByteBuffer);
    ByteBuffer++;
    RemainSize--;
  }

  while (RemainSize >= 4) {
    MmioWrite32 (Private->MsdcMmioReg + MSDC_TXDATA, *(UINT32 *)ByteBuffer);
    ByteBuffer += 4;
    RemainSize -= 4;
  }

  while (RemainSize) {
    MmioWrite8 (Private->MsdcMmioReg + MSDC_TXDATA, *ByteBuffer);
    ByteBuffer++;
    RemainSize--;
  }
}

EFI_STATUS
MsdcPioRead (
  MSDC_PRIVATE_DATA* Private,
  VOID  *Buffer,
  UINT32 BufferLength)
{
  UINT32 IntStatus, ChunkSize, RemainSize, RxBytes, DrainPolls = 0;
  BOOLEAN Complete = FALSE;
  UINT8 *ByteBuffer = (UINT8 *)Buffer;
  EFI_STATUS Status;

  RemainSize = BufferLength;

  MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_DATSTS);

  while (TRUE) {
    Status = MsdcCheckMedia (Private);
    if (EFI_ERROR (Status)) { return Status; }
    if (MsdcExpired (Private)) { return Complete ? EFI_ABORTED : EFI_TIMEOUT; }
    MicroSecondDelay (1);
    MsdcRead (Private, MSDC_INT, &IntStatus);

    if (IntStatus & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_ACMDTMO | MSDC_INT_ACMDCRCERR)) {
      return MsdcIntTrackError (Private, MSDC_INT_DATSTS | MSDC_INT_CMDSTS, IntStatus);
    }

    MsdcFifoRxBytes (Private, &RxBytes);
    ChunkSize = MIN (RemainSize, RxBytes);

    if ((RxBytes > MSDC_FIFO_SIZE) || (RxBytes > RemainSize)) { return EFI_DEVICE_ERROR; }

    if (ChunkSize != 0) {
      MsdcFifoRead (Private, ByteBuffer, ChunkSize);
      ByteBuffer += ChunkSize;
      RemainSize -= ChunkSize;
      DrainPolls = 0;
    }

    if (IntStatus & MSDC_INT_XFER_COMPL) {
      MsdcWrite (Private, MSDC_INT, MSDC_INT_XFER_COMPL);
      Complete = TRUE;
    }
    if (Complete) {
      if (!RemainSize) { break; }
      // In async FIFO mode, completion can reach INT before the last RX bytes.
      if (!gPlatformInfo.AsyncFifo || ++DrainPolls >= 1000) { return EFI_ABORTED; }
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
MsdcPioWrite (
  MSDC_PRIVATE_DATA* Private,
  VOID  *Buffer,
  UINT32 BufferLength)
{
  UINT32 IntStatus, ChunkSize, RemainSize, TxBytes;
  UINT8 *ByteBuffer = (UINT8 *)Buffer;
  EFI_STATUS Status;

  RemainSize = BufferLength;

  MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_DATSTS);

  while (TRUE) {
    Status = MsdcCheckMedia (Private);
    if (EFI_ERROR (Status)) { return Status; }
    if (MsdcExpired (Private)) { return EFI_TIMEOUT; }
    MicroSecondDelay (1);
    MsdcRead (Private, MSDC_INT, &IntStatus);

    if (IntStatus & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_ACMDTMO | MSDC_INT_ACMDCRCERR)) {
      return MsdcIntTrackError (Private, MSDC_INT_DATSTS | MSDC_INT_CMDSTS, IntStatus);
    }

    if (IntStatus & MSDC_INT_XFER_COMPL) {
      MsdcWrite (Private, MSDC_INT, IntStatus & MSDC_INT_XFER_COMPL);

      if (RemainSize) {
        DEBUG ((DEBUG_ERROR, "MsdcDxe: Data not fully wrote!\n"));
        return EFI_ABORTED;
      }

      break;
    }

    ChunkSize = RemainSize > MSDC_FIFO_SIZE ? MSDC_FIFO_SIZE : RemainSize;

    MsdcFifoTxBytes (Private, &TxBytes);
    if (TxBytes > MSDC_FIFO_SIZE) { return EFI_DEVICE_ERROR; }
    if (MSDC_FIFO_SIZE - TxBytes >= ChunkSize) {
      MsdcFifoWrite (Private, ByteBuffer, ChunkSize);
      ByteBuffer += ChunkSize;
      RemainSize -= ChunkSize;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
MsdcSendCmd (
  MSDC_PRIVATE_DATA* Private,
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet)
{
  BOOLEAN IsBusy, IsDataTransfer, IsRead;
  EFI_STATUS Status;
  UINT32 RawCmd, RspType, BlkLen = 0;
  EFI_SD_MMC_COMMAND_BLOCK *CommandBlk = Packet->SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK  *SdMmcStatusBlk = Packet->SdMmcStatusBlk;

  RawCmd = CommandBlk->CommandIndex;
  IsDataTransfer = FALSE;
  IsRead = TRUE;

  if (CommandBlk->CommandType != SdMmcCommandTypeBc) {
    switch (CommandBlk->ResponseType) {
      case SdMmcResponseTypeR1:
      case SdMmcResponseTypeR5:
      case SdMmcResponseTypeR6:
      case SdMmcResponseTypeR7:
        RspType = 1;
        break;
      case SdMmcResponseTypeR2:
        RspType = 2;
        break;
      case SdMmcResponseTypeR3:
        RspType = 3;
        break;
      case SdMmcResponseTypeR4:
        RspType = 4;
        break;
      case SdMmcResponseTypeR1b:
        RspType = 7;
        break;
      case SdMmcResponseTypeR5b:
      default:
        return EFI_INVALID_PARAMETER;
    }
    RawCmd |= RspType << SDC_CMD_RSP_TYPE_SHIFT;
  }

  if (CommandBlk->CommandType == SdMmcCommandTypeAdtc) {
    UINT32 Length = Packet->InTransferLength != 0 ? Packet->InTransferLength : Packet->OutTransferLength;
    UINT32 BlockSize = Length < BLOCK_SIZE ? Length : BLOCK_SIZE;
    if ((Length == 0) || (Length % BlockSize != 0)) { return EFI_BAD_BUFFER_SIZE; }
    IsDataTransfer = TRUE;
    IsRead = Packet->InTransferLength != 0;
    BlkLen = Length / BlockSize;
    RawCmd |= (BlkLen == 1 ? SDC_CMD_SINGLE_BLK : SDC_CMD_MULTIPLE_BLK);
    if (!IsRead) { RawCmd |= SDC_CMD_RW; }
    if ((BlkLen > 1) && (Private->SdInfo.CardType == SdCard)) { RawCmd |= SDC_CMD_AUTO12; }
    RawCmd |= BlockSize << SDC_CMD_BLK_SIZE_SHIFT;
  } else if ((Packet->InTransferLength != 0) || (Packet->OutTransferLength != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (CommandBlk->CommandIndex == SD_STOP_TRANSMISSION) { RawCmd |= SDC_CMD_STOP_CMD; }

  do {
    Status = MsdcCheckMedia (Private);
    if (EFI_ERROR (Status)) { return Status; }
    MsdcCheckBusy (Private, IsDataTransfer ||
      ((CommandBlk->CommandType != SdMmcCommandTypeBc) &&
       (CommandBlk->ResponseType == SdMmcResponseTypeR1b)), &IsBusy);
    if (MsdcExpired (Private)) { return EFI_TIMEOUT; }
    MicroSecondDelay (10);
  } while (IsBusy);

  // Retire stale status before a new synchronous transaction.
  MsdcClearInterrupts (Private);
  if (IsDataTransfer) {
    Status = MsdcClearFifo (Private);
    if (EFI_ERROR (Status)) { return Status; }
    MsdcWrite (Private, SDC_BLK_NUM, BlkLen);
  }
  // Disable interrupts because we use polling.
  MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_CMDSTS);

  MsdcWrite (Private, SDC_ARG, CommandBlk->CommandArgument);
  MsdcWrite (Private, SDC_CMD, RawCmd);

  Status = MsdcPollInterrupts (Private, MSDC_INT_CMDSTS, MSDC_INT_CMDRDY);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  if (CommandBlk->CommandType != SdMmcCommandTypeBc) {
    if (CommandBlk->ResponseType == SdMmcResponseTypeR2) {
      MsdcRead (Private, SDC_RESP0, &SdMmcStatusBlk->Resp0);
      MsdcRead (Private, SDC_RESP1, &SdMmcStatusBlk->Resp1);
      MsdcRead (Private, SDC_RESP2, &SdMmcStatusBlk->Resp2);
      MsdcRead (Private, SDC_RESP3, &SdMmcStatusBlk->Resp3);

      // Per SDHCI spec, R2 response registers contains [127:8] bits only
      CopyMem(&SdMmcStatusBlk->Resp0, (UINT8*)&SdMmcStatusBlk->Resp0 + 1, 15);
      ((UINT8 *)&SdMmcStatusBlk->Resp0)[15] = 0;
    } else {
      MsdcRead (Private, SDC_RESP0, &SdMmcStatusBlk->Resp0);
    }
  }

  if (IsDataTransfer) {
    if (IsRead) {
      Status = MsdcPioRead (Private, Packet->InDataBuffer, Packet->InTransferLength);
      if (EFI_ERROR(Status)) {
        return Status;
      }
    } else {
      Status = MsdcPioWrite (Private, Packet->OutDataBuffer, Packet->OutTransferLength);
      if (EFI_ERROR(Status)) {
        return Status;
      }
    }
  }

  if ((CommandBlk->CommandType != SdMmcCommandTypeBc) &&
      (CommandBlk->ResponseType == SdMmcResponseTypeR1b)) {
    UINT32 Pins;
    // MSDC_PS data pins occupy bits [23:16]; DAT0 high ends R1b busy.
    do {
      Status = MsdcCheckMedia (Private);
      if (EFI_ERROR (Status)) { return Status; }
      MsdcRead (Private, MSDC_PS, &Pins);
      if ((Pins & BIT16) != 0) { break; }
      if (MsdcExpired (Private)) { return EFI_TIMEOUT; }
      MicroSecondDelay (10);
    } while (TRUE);
  }
  return Status;
}

EFI_STATUS MsdcInit (
  MSDC_PRIVATE_DATA *Private)
{
  EFI_STATUS Status;
  Status = InitGpio (Private->Index);
  MsdcRecordInitStatus (MsdcDiagStageGpioSetup, Status);
  if (EFI_ERROR (Status)) { return Status; }
  Status = ClockControl (Private->Index, TRUE);
  MsdcRecordInitStatus (MsdcDiagStageClockEnable, Status);
  if (EFI_ERROR (Status)) { return Status; }

  // Configure to SD/MMC mode, Clock free running, PIO mode
  MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_MODE | MSDC_CFG_CCKPD | MSDC_CFG_PIO);

  // SW Reset
  Status = MsdcReset (Private);
  MsdcRecordInitStatus (MsdcDiagStageControllerReset, Status);
  if (EFI_ERROR (Status)) { return Status; }

  // Clear FIFO
  Status = MsdcClearFifo (Private);
  MsdcRecordInitStatus (MsdcDiagStageFifoClear, Status);
  if (EFI_ERROR (Status)) { return Status; }

  // Mask all interrupts before clearing inherited status.
  MsdcWrite (Private, MSDC_INTEN, 0);
  MsdcClearInterrupts (Private);

  // Enable SDIO mode, otherwise cmd5 won't work
  // This bit disables R4 response CRC check for SDIO card
  MsdcSetBits (Private, SDC_CFG, SDC_CFG_SDIO);
  // Disable detecting SDIO interrupts
  MsdcClrBits (Private, SDC_CFG, SDC_CFG_SDIOIDE);

  if (gPlatformInfo.UseTop) {
    MsdcTopWrite (Private, TOP_CTRL, 0);
    MsdcTopWrite (Private, TOP_CMD, 0);
  } else {
    MsdcWrite (Private, gPlatformInfo.MsdcPadTuneReg, 0);
  }

  MsdcWrite (Private, MSDC_IOCON, 0);

  // Quirks
  MsdcWrite (Private, MSDC_PATCH_BIT0, 0x403c0446);
  MsdcWrite (Private, MSDC_PATCH_BIT1, 0xffff4089);

  MsdcSetBits (Private, EMMC50_CFG0, EMMC50_CFG0_CRCSTSSEL);

  if (gPlatformInfo.BusyCheck) {
    MsdcClrBits (Private, MSDC_PATCH_BIT1, MSDC_PB1_BUSYCHECKSEL);
  }

  if (gPlatformInfo.StopClkFix) {
    MsdcSetBits (Private, MSDC_PATCH_BIT1, BIT8 | BIT9);
    MsdcClrBits (Private, SDC_FIFO_CFG, SDC_FIFO_CFG_WRVALIDSEL);
    MsdcClrBits (Private, SDC_FIFO_CFG, SDC_FIFO_CFG_RDVALIDSEL);
  }

  if (gPlatformInfo.AsyncFifo)
  {
    MsdcClrSetBits(Private, MSDC_PATCH_BIT2, MSDC_PB2_RESPWAIT, 3 << MSDC_PB2_RESPWAIT_SHIFT);

    MsdcClrBits (Private, MSDC_PATCH_BIT2, MSDC_PB2_CFGRESP);
    MsdcSetBits (Private, MSDC_PATCH_BIT2, MSDC_PB2_CFGCRCSTS);

    if (gPlatformInfo.EnhanceRx)
    {
      if (gPlatformInfo.UseTop) {
        MsdcTopSetBits (Private, TOP_CTRL, SDC_RX_ENH_EN);
      } else {
        MsdcSetBits (Private, SDC_ADV_CFG0, SDC_RX_ENHANCE_EN);
      }
    } else {
      MsdcClrSetBits(Private, MSDC_PATCH_BIT2, MSDC_PB2_RESPSTSENSEL | MSDC_PB2_CRCSTSENSEL, (2 << 16) | (2 << 29));
    }
  }

  // Data tune
  if (gPlatformInfo.UseTop) {
    MsdcTopSetBits (Private, TOP_CTRL, PAD_DAT_RD_RXDLY_SEL);
    MsdcTopClrBits (Private, TOP_CTRL, DATA_K_VALUE_SEL);
    MsdcTopSetBits (Private, TOP_CMD, PAD_CMD_RD_RXDLY_SEL);
    if (gPlatformInfo.TuningStep[Private->Index] > 32)
    {
      MsdcTopSetBits (Private, TOP_CTRL, PAD_DAT_RD_RXDLY2_SEL);
      MsdcTopSetBits (Private, TOP_CMD, PAD_CMD_RD_RXDLY2_SEL);
    }
  } else {
    MsdcSetBits (Private, gPlatformInfo.MsdcPadTuneReg, MSDC_PAD_TUNE_RD_SEL | MSDC_PAD_TUNE_CMD_SEL);
  }

  // Set default data timeout
  Private->HostData.TimeoutNs = 100000000;
  Private->HostData.TimeoutClks = 3 * (1 << SCLK_CYCLES_SHIFT);

  // Set default bus width
  MsdcSetBusWidth (Private, 1);
  MsdcRecordInitStatus (MsdcDiagStageHostConfiguration, EFI_SUCCESS);
  return EFI_SUCCESS;
}

EFI_STATUS
CardReset (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = SD_GO_IDLE_STATE;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeBc;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

EFI_STATUS
SdSendIfCond (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru)
{
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex    = SD_SEND_IF_COND;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR7;
  SdMmcCmdBlk.CommandArgument = 0x1AA;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  // R7 must echo the voltage and check pattern sent in CMD8.  An R1
  // illegal-command response is not a valid R7, even if its low bits match.
  if ((SdMmcStatusBlk.Resp0 & (0xFFFU | BIT22)) != 0x1AA) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
SdCardSendOpCond (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT16                         Rca,
  UINT32                         VoltageWindow,
  BOOLEAN                        S18R,
  BOOLEAN                        Xpc,
  BOOLEAN                        Hcs,
  UINT32                        *Ocr)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;
  UINT32 Switch;
  UINT32 MaxPower;
  UINT32 HostCapacity;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex    = SD_APP_CMD;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // APP_CMD cannot authorize ACMD41 with ILLEGAL_COMMAND or COM_CRC_ERROR.
  if ((SdMmcStatusBlk.Resp0 & (BIT5 | BIT22 | BIT23)) != BIT5) {
    return EFI_DEVICE_ERROR;
  }

  SdMmcCmdBlk.CommandIndex = SD_SEND_OP_COND;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR3;

  Switch       = S18R ? BIT24 : 0;
  MaxPower     = Xpc ? BIT28 : 0;
  HostCapacity = Hcs ? BIT30 : 0;

  SdMmcCmdBlk.CommandArgument = (VoltageWindow & 0xFFFFFF) | Switch | \
                                MaxPower | HostCapacity;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);

  if (!EFI_ERROR (Status)) {
    *Ocr = SdMmcStatusBlk.Resp0;
  }

  return Status;
}

EFI_STATUS
EMMCSendOpCond (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT32                         VoltageWindow,
  BOOLEAN                        Hcs,
  UINT32                        *Ocr)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;
  UINT32 HostCapacity;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = EMMC_SEND_OP_COND;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR3;

  HostCapacity = Hcs ? BIT30 : 0;

  SdMmcCmdBlk.CommandArgument = (VoltageWindow & 0xFFFFFF) | HostCapacity;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);

  if (!EFI_ERROR (Status)) {
    *Ocr = SdMmcStatusBlk.Resp0;
  }

  return Status;
}

EFI_STATUS
CardAllSendCid (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = SD_ALL_SEND_CID;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR2;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

EFI_STATUS
SdCardSetRca (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT16                        *Rca)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = SD_SET_RELATIVE_ADDR;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeBcr;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR6;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    // R6 carries COM_CRC_ERROR, ILLEGAL_COMMAND and ERROR in bits 15:13.
    // RCA zero is reserved and must not be used to select a card.
    if (((SdMmcStatusBlk.Resp0 & (BIT15 | BIT14 | BIT13)) != 0) ||
        ((SdMmcStatusBlk.Resp0 >> 16) == 0)) {
      return EFI_DEVICE_ERROR;
    }
    *Rca = (UINT16)(SdMmcStatusBlk.Resp0 >> 16);
  }

  return Status;
}

EFI_STATUS
EMMCSetRca (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT16                         Rca)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = EMMC_SET_RELATIVE_ADDR;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

EFI_STATUS
CardSelect (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT16                         Rca)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = SD_SELECT_DESELECT_CARD;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAc;
  if (Rca != 0) {
    SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1b;
  }

  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);
  if (!EFI_ERROR (Status) && (Rca != 0) &&
      ((MSDC_PRIVATE_FROM_THIS (PassThru))->SdInfo.CardType == SdCard) &&
      ((SdMmcStatusBlk.Resp0 & (BIT23 | BIT22 | BIT19)) != 0)) {
    // R1b busy release is not proof that the card accepted CMD7.
    return EFI_DEVICE_ERROR;
  }

  return Status;
}

EFI_STATUS
CardSendStatus (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT16                         Rca,
  UINT32                        *DevStatus)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex    = SD_SEND_STATUS;
  SdMmcCmdBlk.CommandType     = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType    = SdMmcResponseTypeR1;
  SdMmcCmdBlk.CommandArgument = (UINT32)Rca << 16;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);
  if (!EFI_ERROR (Status)) {
    *DevStatus = SdMmcStatusBlk.Resp0;
  }

  return Status;
}

EFI_STATUS
SdCardSwitch (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT8                          AccessMode,
  UINT8                          CommandSystem,
  UINT8                          DriveStrength,
  UINT8                          PowerLimit,
  BOOLEAN                        Mode,
  UINT8                         *SwitchResp)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;
  UINT32 ModeValue;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = SD_SWITCH_FUNC;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAdtc;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1;

  ModeValue                   = Mode ? BIT31 : 0;
  SdMmcCmdBlk.CommandArgument = (AccessMode & 0xF) |
                                ((CommandSystem & 0xF) << 4) |
                                ((DriveStrength & 0xF) << 8) |
                                ((PowerLimit & 0xF) << 12) |
                                (0xFF << 16) | ModeValue;

  Packet.InDataBuffer     = SwitchResp;
  Packet.InTransferLength = 64;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

EFI_STATUS
EMMCSwitch (
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru,
  UINT8 Access,
  UINT8 Index,
  UINT8 Value,
  UINT8 CmdSet)
{
  EFI_SD_MMC_COMMAND_BLOCK SdMmcCmdBlk;
  EFI_SD_MMC_STATUS_BLOCK SdMmcStatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  EFI_STATUS Status;

  ZeroMem (&SdMmcCmdBlk, sizeof (SdMmcCmdBlk));
  ZeroMem (&SdMmcStatusBlk, sizeof (SdMmcStatusBlk));
  ZeroMem (&Packet, sizeof (Packet));

  Packet.SdMmcCmdBlk    = &SdMmcCmdBlk;
  Packet.SdMmcStatusBlk = &SdMmcStatusBlk;

  SdMmcCmdBlk.CommandIndex = EMMC_SWITCH;
  SdMmcCmdBlk.CommandType  = SdMmcCommandTypeAc;
  SdMmcCmdBlk.ResponseType = SdMmcResponseTypeR1b;

  SdMmcCmdBlk.CommandArgument = (Access << 24) |
                                (Index << 16) |
                                (Value << 8) |
                                CmdSet;

  Status = MsdcPassThru (PassThru, 0, &Packet, NULL);

  return Status;
}

EFI_STATUS
CardSetBusMode (
  MSDC_PRIVATE_DATA *Private,
  UINT16             Rca)
{
  EFI_STATUS Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru;
  UINT8 SwitchResp[64];
  UINT32 DevStatus;

  PassThru = &Private->PassThru;

  Status = CardSelect(PassThru, Rca);
  if (EFI_ERROR(Status))
  {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: CardSelect failed with status %r\n", Status));
    return Status;
  }

  if ((Private->SdInfo.CardType == SdCard) && (gPlatformInfo.MaxClockHz > 25000000))
  {
    Status = SdCardSwitch (PassThru, 1, 0xF, 0xF, 0xF, TRUE, SwitchResp);
    if (EFI_ERROR(Status))
    {
      DEBUG ((DEBUG_ERROR, "MsdcDxe: SdCardSwitch failed with status %r\n", Status));
      return Status;
    }
    if ((SwitchResp[16] & 0xF) != 1) { return EFI_UNSUPPORTED; }
  } else if (Private->SdInfo.CardType == EmmcCard) {
    Status = EMMCSwitch(PassThru, 3, 185, 1, 0);
    if (EFI_ERROR(Status))
    {
      DEBUG ((DEBUG_ERROR, "MsdcDxe: EMMCSwitch failed with status %r\n", Status));
      return Status;
    }
  }

  if (Private->SdInfo.CardType == SdCard) {
    // Require READY_FOR_DATA and TRAN, not merely a successful CMD13 transport.
    for (UINTN Attempt = 0; Attempt < 100; Attempt++) {
      Status = CardSendStatus (PassThru, Rca, &DevStatus);
      if (EFI_ERROR (Status)) { return Status; }
      if ((DevStatus & (BIT23 | BIT22 | BIT21 | BIT20 | BIT19)) != 0) {
        return EFI_DEVICE_ERROR;
      }
      if (((DevStatus & BIT8) != 0) && (((DevStatus >> 9) & 0xFU) == 4)) {
        return MsdcSetMclk (Private, gPlatformInfo.MaxClockHz);
      }
      if (Attempt < 99) { MicroSecondDelay (10000); }
    }
    return EFI_TIMEOUT;
  }

  Status = CardSendStatus(PassThru, Rca, &DevStatus);
  if (EFI_ERROR(Status))
  {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: CardSendStatus failed with status %r\n", Status));
    return Status;
  }

  if (DevStatus & BIT7) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: Got switch error, DevStatus is 0x%x\n", DevStatus));
    return EFI_DEVICE_ERROR;
  }

  return MsdcSetMclk (Private, gPlatformInfo.MaxClockHz);
}

EFI_STATUS
EmmcIdentification (
  MSDC_PRIVATE_DATA *Private)
{
  EFI_STATUS Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru;
  UINT32 Ocr;
  UINTN Attempts = 0;

  PassThru = &Private->PassThru;

  Status = PowerControl (Private->Index, TRUE);
  if (EFI_ERROR (Status)) { return Status; }
  MicroSecondDelay (20000);
  Status = MsdcSetMclk (Private, 400000);
  if (EFI_ERROR (Status)) { return Status; }

  Status = CardReset (PassThru);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: CardReset failed with status %r\n", Status));
    return Status;
  }

  Ocr = 0;

  do {
    if (++Attempts > 10) { return EFI_TIMEOUT; }
    Status = EMMCSendOpCond (PassThru, Ocr, TRUE, &Ocr);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MsdcDxe: EMMCSendOpCond failed with status %r\n", Status));
      return Status;
    }

    MicroSecondDelay (100000);
  } while ((Ocr & BIT31) == 0);

  Status = CardAllSendCid (PassThru);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: CardAllSendCid failed with status %r\n", Status));
    return Status;
  }

  Status = EMMCSetRca (PassThru, 1);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: EMMCSetRca failed with status %r\n", Status));
    return Status;
  }

  Private->SdInfo.CardType = EmmcCard;
  DEBUG ((DEBUG_ERROR, "MsdcDxe: Found EMMC device at controller with index %d\n", Private->Index));

  Status = CardSetBusMode (Private, 1);

  return Status;
}

STATIC
EFI_STATUS
MsdcReadSdLastBlock (
  MSDC_PRIVATE_DATA *Private,
  UINT16 Rca,
  BOOLEAN SectorAddressing
  )
{
  EFI_SD_MMC_COMMAND_BLOCK Command;
  EFI_SD_MMC_STATUS_BLOCK Response;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  SD_CSD Csd;
  SD_CSD2 *Csd2;
  UINT64 BlockCount;
  UINT64 CapacityBytes;
  UINT64 CSize;
  EFI_STATUS Status;

  ZeroMem (&Command, sizeof (Command));
  ZeroMem (&Response, sizeof (Response));
  ZeroMem (&Packet, sizeof (Packet));
  ZeroMem (&Csd, sizeof (Csd));

  Packet.SdMmcCmdBlk = &Command;
  Packet.SdMmcStatusBlk = &Response;
  Packet.Timeout = MSDC_DEFAULT_TIMEOUT_NS / 1000;
  Command.CommandIndex = SD_SEND_CSD;
  Command.CommandType = SdMmcCommandTypeAc;
  Command.ResponseType = SdMmcResponseTypeR2;
  Command.CommandArgument = (UINT32)Rca << 16;

  Status = MsdcPassThru (&Private->PassThru, 0, &Packet, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Match SdDxe's SdGetCsd response alignment before decoding the wire fields.
  CopyMem (((UINT8 *)&Csd) + 1, &Response.Resp0, sizeof (Csd) - 1);
  // Ready ACMD41 CCS determines address units; reject inconsistent geometry.
  if (Csd.CsdStructure != (SectorAddressing ? 1U : 0U)) {
    return EFI_UNSUPPORTED;
  }
  if (Csd.CsdStructure == 0) {
    CSize = (((UINT64)Csd.CSizeHigh << 2) | Csd.CSizeLow) + 1;
    CapacityBytes = CSize * (1ULL << (Csd.CSizeMul + 2)) * (1ULL << Csd.ReadBlLen);
    // Standard-capacity cards use byte-addressed 32-bit command arguments.
    if ((CapacityBytes > 0x100000000ULL) ||
        (Csd.ReadBlLen < 9) || (Csd.ReadBlLen > 11) ||
        ((CapacityBytes % BLOCK_SIZE) != 0)) {
      return EFI_UNSUPPORTED;
    }
    BlockCount = CapacityBytes / BLOCK_SIZE;
  } else if (Csd.CsdStructure == 1) {
    Csd2 = (SD_CSD2 *)(VOID *)&Csd;
    CSize = (((UINT64)Csd2->CSizeHigh << 16) | Csd2->CSizeLow) + 1;
    BlockCount = CSize * 1024;
  } else {
    return EFI_UNSUPPORTED;
  }

  if (BlockCount == 0) {
    return EFI_UNSUPPORTED;
  }

  Private->SdInfo.LastBlock = BlockCount - 1;
  return EFI_SUCCESS;
}

EFI_STATUS
SdCardIdentification (
  MSDC_PRIVATE_DATA *Private)
{
  EFI_STATUS Status;
  EFI_SD_MMC_PASS_THRU_PROTOCOL *PassThru;
  UINT32 Ocr;
  UINTN Attempts = 0;
  UINT16 Rca;
  BOOLEAN S18r;
  BOOLEAN Xpc;
  BOOLEAN Hcs;

  PassThru = &Private->PassThru;

  Status = PowerControl (Private->Index, TRUE);
  MsdcRecordInitStatus (MsdcDiagStageCardPower, Status);
  if (EFI_ERROR (Status)) { return Status; }
  MicroSecondDelay (20000);
  Status = MsdcSetMclk (Private, 400000);
  MsdcRecordInitStatus (MsdcDiagStageIdentificationClock, Status);
  if (EFI_ERROR (Status)) { return Status; }

  Status = CardReset (PassThru);
  MsdcRecordInitStatus (MsdcDiagStageCardReset, Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: CardReset failed with status %r\n", Status));
    return Status;
  }

  MicroSecondDelay (10000);

  Status = SdSendIfCond (PassThru);
  MsdcRecordInitStatus (MsdcDiagStageInterfaceCondition, Status);
  // Pre-2.0 SD cards do not implement CMD8.  Only a missing response permits
  // that path; CRC failures and invalid R7 replies still reject the card.
  Hcs = !EFI_ERROR (Status);
  if (EFI_ERROR (Status) && (Status != EFI_TIMEOUT)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: SdSendIfCond failed with status %r\n", Status));
    return Status;
  }

  Status = SdCardSendOpCond (PassThru, 0, 0, FALSE, FALSE, FALSE, &Ocr);
  MsdcRecordInitStatus (MsdcDiagStageOperatingCondition, Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: SdCardSendOpCond failed with status %r\n", Status));
    return Status;
  }

  S18r = FALSE;
  Xpc = Hcs;

  do {
    if (++Attempts > 100) {
      MsdcRecordInitStatus (MsdcDiagStageOperatingCondition, EFI_TIMEOUT);
      return EFI_TIMEOUT;
    }
    Status = SdCardSendOpCond (PassThru, 0, Ocr, S18r, Xpc, Hcs, &Ocr);
    MsdcRecordInitStatus (MsdcDiagStageOperatingCondition, Status);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MsdcDxe: SdCardSendOpCond failed with status %r\n", Status));
      return Status;
    }
    MicroSecondDelay(10000);
  } while ((Ocr & BIT31) == 0);

  Status = CardAllSendCid (PassThru);
  MsdcRecordInitStatus (MsdcDiagStageCid, Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: CardAllSendCid failed with status %r\n", Status));
    return Status;
  }

  Status = SdCardSetRca (PassThru, &Rca);
  MsdcRecordInitStatus (MsdcDiagStageRca, Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: CardAllSendCid failed with status %r\n", Status));
    return Status;
  }

  Private->SdInfo.CardType = SdCard;
  DEBUG ((DEBUG_ERROR, "MsdcDxe: Found SD device at controller with index %d\n", Private->Index));

  Status = MsdcReadSdLastBlock (Private, Rca, (BOOLEAN)(Hcs && ((Ocr & BIT30) != 0)));
  MsdcRecordInitStatus (MsdcDiagStageCapacity, Status);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MsdcDxe: Read SD capacity failed with status %r\n", Status));
    return Status;
  }

  Status = CardSetBusMode (Private, Rca);
  MsdcRecordInitStatus (MsdcDiagStageBusMode, Status);

  return Status;
}

CARD_DETECT_ROUTINE CardDetectRoutineTable[] = {
  EmmcIdentification,
  SdCardIdentification,
  NULL
};

STATIC
EFI_STATUS
MsdcIdentifyMedia (MSDC_PRIVATE_DATA *Private)
{
  EFI_STATUS Status;
  if ((Private->Index == 1) && (mMsdcDiagnostics.IdentifyAttempts != MAX_UINT32)) {
    mMsdcDiagnostics.IdentifyAttempts++;
  }
  MsdcInvalidateMedia (Private);
  Private->Initializing = TRUE;
  MsdcSyncMediaDiagnostics (Private);
  Status = MsdcInit (Private);
  if (!EFI_ERROR (Status)) {
    Status = Private->Index == 0 ? EmmcIdentification (Private) :
                                  SdCardIdentification (Private);
  }
  if (!EFI_ERROR (Status)) {
    Status = MsdcCheckMedia (Private);
    if (Private->Index == 1) {
      MsdcRecordInitStatus (MsdcDiagStageFinalCardDetect, Status);
    }
  }
  Private->Initializing = FALSE;
  if (EFI_ERROR (Status)) {
    if ((Private->Index == 1) && (mMsdcDiagnostics.IdentifyFailures != MAX_UINT32)) {
      mMsdcDiagnostics.IdentifyFailures++;
    }
    MsdcInvalidateMedia (Private);
    // A present but unusable card must not stall the UI every three ticks.
    Private->IdentifyRetryDelay = Private->IdentifyRetryDelay == 0 ? 10 :
      (UINT8)MIN (100, Private->IdentifyRetryDelay * 2);
    Private->IdentifyRetryCountdown = Private->IdentifyRetryDelay;
  } else {
    Private->IdentifyRetryCountdown = 0;
    Private->IdentifyRetryDelay = 0;
    Private->MediaReady = TRUE;
    Private->ChildConnectionPending = TRUE;
    if (Private->Index == 1) {
      MsdcRecordInitStatus (MsdcDiagStageReady, EFI_SUCCESS);
    }
  }
  MsdcSyncMediaDiagnostics (Private);
  return Status;
}

STATIC
VOID
EFIAPI
MsdcExitBootServices (EFI_EVENT Event, VOID *Context)
{
  MSDC_PRIVATE_DATA *Private = Context;
  // Do not call boot services or modify rails/GPIO/controller state here.
  Private->Stopped = TRUE;
}

STATIC
VOID
EFIAPI
MsdcPollMedia (EFI_EVENT Event, VOID *Context)
{
  MSDC_PRIVATE_DATA *Private = Context;
  EFI_STATUS Status;
  EFI_STATUS ChildStatus;
  MSDC_CHILD_STATE ChildState;

  if (Private->Stopped || Private->PollActive || Private->CommandActive ||
      Private->ControllerHandle == NULL) { return; }
  Private->PollActive = TRUE;
  Status = MsdcCheckMedia (Private);

  if (Private->ChildWithdrawalPending) {
    // Do not identify another card until the exact old slot child is gone.
    ChildStatus = MsdcGetChildState (Private, &ChildState);
    if (!EFI_ERROR (ChildStatus) && (ChildState == MsdcChildAbsent)) {
      Private->ChildWithdrawalPending = FALSE;
      Private->MediaAnnounced = FALSE;
      MsdcResetChildRetry (Private);
      goto Done;
    }

    if (!MsdcChildRetryDue (Private)) {
      goto Done;
    }

    if (EFI_ERROR (ChildStatus)) {
      MsdcScheduleChildRetry (Private);
      goto Done;
    }

    // Reinstall disconnects the old parent/child relationship. Its success
    // is not enough: confirm the old child handle actually disappeared.
    Status = gBS->ReinstallProtocolInterface (
                    Private->ControllerHandle,
                    &gEfiSdMmcPassThruProtocolGuid,
                    &Private->PassThru,
                    &Private->PassThru
                    );
    if (!EFI_ERROR (Status)) {
      ChildStatus = MsdcGetChildState (Private, &ChildState);
      if (!EFI_ERROR (ChildStatus) && (ChildState == MsdcChildAbsent)) {
        Private->ChildWithdrawalPending = FALSE;
        Private->MediaAnnounced = FALSE;
        MsdcResetChildRetry (Private);
        goto Done;
      }
    }

    MsdcScheduleChildRetry (Private);
    goto Done;
  }

  if (EFI_ERROR (Status)) { goto Done; }

  if (!Private->MediaReady) {
    if (Private->IdentifyRetryCountdown != 0) {
      Private->IdentifyRetryCountdown--;
      goto Done;
    }
    // Three consecutive timer samples, 20 ms apart, before touching a new card.
    if (++Private->StableSamples < 3) { goto Done; }
    Status = MsdcIdentifyMedia (Private);
    if (EFI_ERROR (Status)) { goto Done; }
  }

  ChildStatus = MsdcGetChildState (Private, &ChildState);
  if (!EFI_ERROR (ChildStatus) && (ChildState == MsdcChildReady)) {
    Private->MediaAnnounced = TRUE;
    Private->ChildConnectionPending = FALSE;
    MsdcResetChildRetry (Private);
    goto Done;
  }

  Private->ChildConnectionPending = TRUE;
  if (!MsdcChildRetryDue (Private)) {
    goto Done;
  }
  if (EFI_ERROR (ChildStatus)) {
    MsdcScheduleChildRetry (Private);
    goto Done;
  }

  // A successful Reinstall can still hide a failed SD driver Start. Keep
  // retrying at a bounded backoff until this exact child is usable.
  Private->MediaAnnounced = TRUE;
  Status = gBS->ReinstallProtocolInterface (
                  Private->ControllerHandle,
                  &gEfiSdMmcPassThruProtocolGuid,
                  &Private->PassThru,
                  &Private->PassThru
                  );
  if (EFI_ERROR (Status)) {
    // A failed reinstall can be partially applied. Withdraw the identity and
    // re-identify only after any old child is confirmed gone.
    MsdcInvalidateMedia (Private);
    MsdcScheduleChildRetry (Private);
    goto Done;
  }

  ChildStatus = MsdcGetChildState (Private, &ChildState);
  if (!EFI_ERROR (ChildStatus) && (ChildState == MsdcChildReady)) {
    Private->ChildConnectionPending = FALSE;
    MsdcResetChildRetry (Private);
  } else {
    MsdcScheduleChildRetry (Private);
  }
Done:
  Private->PollActive = FALSE;
}

EFI_STATUS
InitMsdc (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = EFI_NOT_FOUND;
  EFI_STATUS Result = EFI_NOT_FOUND;
  MSDC_PRIVATE_DATA *Private;
  MSDC_DEVICE_PATH *DevicePath;
  EFI_MEMORY_REGION_DESCRIPTOR Region;
  CHAR8 Name[16];
  BOOLEAN Present;
  UINTN Index;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gMediaTekMsdcDiagnosticsProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mMsdcDiagnosticsProtocol
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((gPlatformInfo.HostMask == 0) || ((gPlatformInfo.HostMask & ~3U) != 0) ||
      (gPlatformInfo.MaxClockHz < 400000) || (gPlatformInfo.MaxClockHz > 50000000)) {
    MsdcRecordDriverStatus (MsdcDiagStageDriverConfig, EFI_INVALID_PARAMETER);
    return EFI_SUCCESS;
  }
  for (Index = 0; Index < 2; Index++) {
    if ((gPlatformInfo.HostMask & (1U << Index)) == 0) { continue; }
    mMsdcDiagnostics.HostIndex = (UINT32)Index;
    Status = GetCardPresent (Index, &Present);
    if (Index == 1) {
      MsdcRecordCardDetect (Status, Present);
      MsdcRecordDriverStatus (MsdcDiagStageCardDetect, Status);
    }
    if (EFI_ERROR (Status)) {
      if (Status != EFI_UNSUPPORTED) { Result = Status; }
      continue;
    }
    Private = AllocateCopyPool (sizeof (*Private), &gMSDCPrivateDataTemplate);
    if (Private == NULL) {
      MsdcRecordDriverStatus (MsdcDiagStagePrivateAllocation, EFI_OUT_OF_RESOURCES);
      return EFI_SUCCESS;
    }
    DevicePath = AllocateCopyPool (sizeof (*DevicePath), &gMSDCDevicePathTemplate);
    if (DevicePath == NULL) {
      MsdcRecordDriverStatus (MsdcDiagStageDevicePathAllocation, EFI_OUT_OF_RESOURCES);
      FreePool (Private);
      return EFI_SUCCESS;
    }
    DevicePath->Mmc.Guid.Data4[7] = Index;
    Private->Index = Index;
    AsciiSPrint (Name, sizeof (Name), "MSDC-%u", (UINT32)Index);
    Status = LocateMemoryRegionByName (Name, &Region);
    MsdcRecordDriverStatus (MsdcDiagStageHostMmioLookup, Status);
    if (EFI_ERROR (Status)) { goto Failed; }
    Private->MsdcMmioReg = Region.Address;
    if (gPlatformInfo.UseTop) {
      AsciiSPrint (Name, sizeof (Name), "MSDC Top-%u", (UINT32)Index);
      Status = LocateMemoryRegionByName (Name, &Region);
      MsdcRecordDriverStatus (MsdcDiagStageTopMmioLookup, Status);
      if (EFI_ERROR (Status)) { goto Failed; }
      Private->TopMmioReg = Region.Address;
    }
    if (Present) {
      Status = MsdcIdentifyMedia (Private);
      if (EFI_ERROR (Status) && Index == 0) { goto Failed; }
      // A removable card which failed identification may be replaced later.
    }
    if (Index == 1) {
      Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_NOTIFY,
                 MsdcExitBootServices, Private, &gEfiEventExitBootServicesGuid,
                 &Private->ExitEvent);
      MsdcRecordDriverStatus (MsdcDiagStagePollSetup, Status);
      if (EFI_ERROR (Status)) {
        // Failed creation does not transfer ownership of the output handle.
        Private->ExitEvent = NULL;
        goto Failed;
      }
      Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                 MsdcPollMedia, Private, &Private->ConnectEvent);
      MsdcRecordDriverStatus (MsdcDiagStagePollSetup, Status);
      if (EFI_ERROR (Status)) {
        Private->ConnectEvent = NULL;
        goto Failed;
      }
      // Arm before publishing: a setup failure must not leave a live interface
      // pointing at freed memory. The callback ignores an unpublished handle.
      Status = gBS->SetTimer (Private->ConnectEvent, TimerPeriodic, 200000);
      MsdcRecordDriverStatus (MsdcDiagStagePollSetup, Status);
      if (EFI_ERROR (Status)) { goto Failed; }
    }
    Status = gBS->InstallMultipleProtocolInterfaces (
      &Private->ControllerHandle, &gEfiDevicePathProtocolGuid, DevicePath,
      &gEfiSdMmcPassThruProtocolGuid, &Private->PassThru, NULL);
    MsdcRecordDriverStatus (MsdcDiagStageControllerPublish, Status);
    if (EFI_ERROR (Status)) { goto Failed; }
    Private->MediaAnnounced = Private->MediaReady;
    Private->ChildConnectionPending = Private->MediaReady;
    Result = EFI_SUCCESS;
    continue;
Failed:
    // Preserve inherited rails/clocks on failure; shared parents may serve other devices.
    if (Private->ConnectEvent != NULL) { gBS->CloseEvent (Private->ConnectEvent); }
    if (Private->ExitEvent != NULL) { gBS->CloseEvent (Private->ExitEvent); }
    FreePool (DevicePath);
    FreePool (Private);
    if (Result != EFI_SUCCESS) { Result = Status; }
  }
  // The diagnostics protocol was installed on ImageHandle before probing.
  // Return success after recording host failures so this image remains loaded
  // and its cached protocol callbacks cannot become dangling pointers.
  return EFI_ERROR (Result) ? EFI_SUCCESS : Result;
}
