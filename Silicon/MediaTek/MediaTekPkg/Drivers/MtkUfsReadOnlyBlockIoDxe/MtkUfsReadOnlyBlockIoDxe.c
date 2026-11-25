/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/ScsiIo.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/T70UfsReadOnlyBlockIoDiagnostics.h>
#include <Library/MtkUfsCore.h>

#define MTK_UFS_BLOCK_SIGNATURE          SIGNATURE_32('M', 'U', 'B', 'I')
#define MTK_UFS_SCSI_LU2                 2U
#define MTK_UFS_EXPECTED_LU2_BLOCK_SIZE  4096U
#define MTK_UFS_EXPECTED_LU2_BYTES       127959826432ULL
#define MTK_UFS_MAX_COMMAND_BYTES        0x40000U
#define MTK_UFS_MAX_TOTAL_READ_NS        120000000000ULL
#define MTK_UFS_MAX_COMMAND_TIMEOUT_100NS 300000000ULL
#define MTK_UFS_SCSI_TIMEOUT_100NS       300000000ULL
#define MTK_UFS_DMA_BOUNCE_MAX_ADDRESS   0xFFFFFFFFULL

STATIC EFI_GUID mMtkUfsSeedGuid = {
  0x5c568b4c, 0x8d91, 0x4e83, { 0x9d, 0x21, 0x6a, 0x30, 0x17, 0xa4, 0x72, 0x05 }
};

typedef struct {
  UINT32                       Signature;
  EFI_HANDLE                   Handle;
  EFI_SCSI_IO_PROTOCOL         *ScsiIo;
  UINT8                        Target[TARGET_MAX_BYTES];
  UINT64                       Lun;
  EFI_BLOCK_IO_MEDIA           Media;
  EFI_BLOCK_IO_PROTOCOL        BlockIo;
  BOOLEAN                      Started;
  BOOLEAN                      BlockIoUninstalled;
} MTK_UFS_BLOCK_DEVICE;

STATIC MTK_UFS_BLOCK_DEVICE mDevice;
STATIC EFI_DRIVER_BINDING_PROTOCOL mDriverBinding;
STATIC EFI_GUID mDiagnosticsGuid = T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_GUID;
STATIC T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS mDiagnostics;

STATIC
T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD *
MtkUfsGetDiagnosticRecord (
  IN EFI_HANDLE  ControllerHandle
  )
{
  UINTN  Index;
  T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD  *Record;

  for (Index = 0; Index < mDiagnostics.Count; Index++) {
    Record = &mDiagnostics.Records[Index];
    if (Record->Controller == ControllerHandle) {
      return Record;
    }
  }

  if (mDiagnostics.Count >= T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_MAX) {
    mDiagnostics.Dropped++;
    return NULL;
  }

  Record = &mDiagnostics.Records[mDiagnostics.Count++];
  ZeroMem (Record, sizeof (*Record));
  Record->Controller = ControllerHandle;
  Record->Lun = 0xFFU;
  Record->SupportedStatus = EFI_NOT_READY;
  Record->StartStatus = EFI_NOT_READY;
  Record->OpenStatus = EFI_NOT_READY;
  Record->Capacity10Status = EFI_NOT_READY;
  Record->Capacity16Status = EFI_NOT_READY;
  Record->InstallStatus = EFI_NOT_READY;
  return Record;
}

STATIC
EFI_STATUS
MtkUfsValidationFailure (
  IN OUT T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD  *Record OPTIONAL,
  IN     BOOLEAN                                       Starting,
  IN     UINT32                                        Stage,
  IN     EFI_STATUS                                    DetailStatus
  )
{
  if (Record != NULL) {
    if (Starting) {
      Record->StartStage = Stage;
      Record->StartStatus = EFI_UNSUPPORTED;
    } else {
      Record->SupportedStage = Stage;
      Record->SupportedStatus = EFI_UNSUPPORTED;
    }
    Record->StageStatus = DetailStatus;
  }

  return EFI_UNSUPPORTED;
}

STATIC
MTK_UFS_BLOCK_DEVICE *
MtkUfsBlockDeviceFromProtocol (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return BASE_CR (This, MTK_UFS_BLOCK_DEVICE, BlockIo);
}

STATIC
BOOLEAN
MtkUfsIsDiagnosticCandidate (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  VENDOR_DEVICE_PATH        *Vendor;

  if (DevicePath == NULL) {
    return FALSE;
  }

  if ((DevicePath->Type != HARDWARE_DEVICE_PATH) ||
      (DevicePath->SubType != HW_VENDOR_DP) ||
      (DevicePathNodeLength (DevicePath) != sizeof (VENDOR_DEVICE_PATH)))
  {
    return FALSE;
  }

  Vendor = (VENDOR_DEVICE_PATH *)DevicePath;
  if (!CompareGuid (&Vendor->Guid, &mMtkUfsSeedGuid)) {
    return FALSE;
  }

  Node = NextDevicePathNode (DevicePath);
  if (Node->Type != MESSAGING_DEVICE_PATH) {
    return FALSE;
  }
  if ((Node->SubType == MSG_UFS_DP) &&
      (DevicePathNodeLength (Node) != sizeof (UFS_DEVICE_PATH)))
  {
    return FALSE;
  }
  if ((Node->SubType == MSG_SCSI_DP) &&
      (DevicePathNodeLength (Node) != sizeof (SCSI_DEVICE_PATH)))
  {
    return FALSE;
  }
  if ((Node->SubType != MSG_UFS_DP) && (Node->SubType != MSG_SCSI_DP)) {
    return FALSE;
  }
  Node = NextDevicePathNode (Node);
  return (BOOLEAN)(IsDevicePathEnd (Node) &&
                   (Node->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE) &&
                   (DevicePathNodeLength (Node) == END_DEVICE_PATH_LENGTH));
}

STATIC
BOOLEAN
MtkUfsHasLu2DevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL *Node;
  VENDOR_DEVICE_PATH *Vendor;
  BOOLEAN Lu2;

  if (DevicePath == NULL || DevicePath->Type != HARDWARE_DEVICE_PATH ||
      DevicePath->SubType != HW_VENDOR_DP ||
      DevicePathNodeLength (DevicePath) != sizeof (VENDOR_DEVICE_PATH)) return FALSE;
  Vendor = (VENDOR_DEVICE_PATH *)DevicePath;
  if (!CompareGuid (&Vendor->Guid, &mMtkUfsSeedGuid)) return FALSE;

  Node = NextDevicePathNode (DevicePath);
  if (Node->Type != MESSAGING_DEVICE_PATH) return FALSE;
  // UfsPassThruBuildDevicePath creates MSG_UFS_DP, even though ScsiBus
  // publishes EFI_SCSI_IO on that child. MSG_SCSI_DP is a legacy test/adapter
  // route; both must still name only physical target 0, normal LU2.
  Lu2 = FALSE;
  if (Node->SubType == MSG_UFS_DP && DevicePathNodeLength (Node) == sizeof (UFS_DEVICE_PATH)) {
    UFS_DEVICE_PATH *Ufs = (UFS_DEVICE_PATH *)Node;
    Lu2 = (BOOLEAN)(Ufs->Pun == 0 && Ufs->Lun == MTK_UFS_SCSI_LU2);
  } else if (Node->SubType == MSG_SCSI_DP && DevicePathNodeLength (Node) == sizeof (SCSI_DEVICE_PATH)) {
    SCSI_DEVICE_PATH *Scsi = (SCSI_DEVICE_PATH *)Node;
    Lu2 = (BOOLEAN)(Scsi->Pun == 0 && Scsi->Lun == MTK_UFS_SCSI_LU2);
  }
  if (!Lu2) return FALSE;
  Node = NextDevicePathNode (Node);
  return (BOOLEAN)(IsDevicePathEnd (Node) &&
                   Node->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE &&
                   DevicePathNodeLength (Node) == END_DEVICE_PATH_LENGTH);
}

STATIC
BOOLEAN
MtkUfsTargetIsZero (
  IN CONST UINT8  *Target
  )
{
  UINTN  Index;

  for (Index = 0; Index < TARGET_MAX_BYTES; Index++) {
    if (Target[Index] != 0U) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
EFI_STATUS
MtkUfsValidateExpectedDevice (
  IN EFI_HANDLE          ControllerHandle,
  IN BOOLEAN             Starting,
  IN OUT T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD *Diagnostic OPTIONAL,
  OUT EFI_SCSI_IO_PROTOCOL **ScsiIo OPTIONAL,
  OUT UINT8              *Target OPTIONAL,
  OUT UINT64             *Lun OPTIONAL
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_SCSI_IO_PROTOCOL      *Interface;
  UINT8                     DeviceType;
  UINT8                     TargetBuffer[TARGET_MAX_BYTES];
  UINT8                     *TargetPointer;
  UINT64                    DeviceLun;
  UINT8                     LunBytes[sizeof (DeviceLun)];

  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEfiScsiIoProtocolGuid,
                  (VOID **)&Interface
                  );
  if (EFI_ERROR (Status)) {
    return MtkUfsValidationFailure (
             Diagnostic, Starting, T70UfsDiagStageScsiIo, Status);
  }

  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&DevicePath
                  );
  if (EFI_ERROR (Status)) {
    return MtkUfsValidationFailure (
             Diagnostic, Starting, T70UfsDiagStageDevicePath, Status);
  }
  if (!MtkUfsHasLu2DevicePath (DevicePath)) {
    return MtkUfsValidationFailure (
             Diagnostic, Starting, T70UfsDiagStageLuPath, EFI_UNSUPPORTED);
  }

  Status = Interface->GetDeviceType (Interface, &DeviceType);
  if (EFI_ERROR (Status) || (DeviceType != EFI_SCSI_IO_TYPE_DISK)) {
    return MtkUfsValidationFailure (
             Diagnostic, Starting, T70UfsDiagStageDeviceType,
             EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED);
  }

  ZeroMem (TargetBuffer, sizeof (TargetBuffer));
  TargetPointer = TargetBuffer;
  DeviceLun = 0;
  Status = Interface->GetDeviceLocation (Interface, &TargetPointer, &DeviceLun);
  if (EFI_ERROR (Status)) {
    return MtkUfsValidationFailure (
             Diagnostic, Starting, T70UfsDiagStageDeviceLocation, Status);
  }

  /*
   * Basecore UfsPassThru stores a normal UFS LUN in byte 1 of its UINT64 LUN
   * value. Compare the byte representation to avoid relying on host integer
   * endianness in this validation.
   */
  CopyMem (LunBytes, &DeviceLun, sizeof (LunBytes));
  if ((LunBytes[0] == 0U) &&
      (LunBytes[2] == 0U) && (LunBytes[3] == 0U) &&
      (LunBytes[4] == 0U) && (LunBytes[5] == 0U) &&
      (LunBytes[6] == 0U) && (LunBytes[7] == 0U) &&
      (Diagnostic != NULL))
  {
    Diagnostic->Lun = LunBytes[1];
    Diagnostic->Flags |= T70_UFS_DIAG_LUN_VALID;
  }
  if (!MtkUfsTargetIsZero (TargetBuffer)) {
    return MtkUfsValidationFailure (
             Diagnostic, Starting, T70UfsDiagStageTarget, EFI_UNSUPPORTED);
  }
  if ((LunBytes[0] != 0U) || (LunBytes[1] != MTK_UFS_SCSI_LU2)) {
    return MtkUfsValidationFailure (
             Diagnostic, Starting, T70UfsDiagStageLun, EFI_UNSUPPORTED);
  }

  {
    UINTN  Index;

    for (Index = 2; Index < sizeof (LunBytes); Index++) {
      if (LunBytes[Index] != 0U) {
        return MtkUfsValidationFailure (
                 Diagnostic, Starting, T70UfsDiagStageLun, EFI_UNSUPPORTED);
      }
    }
  }

  if (ScsiIo != NULL) {
    *ScsiIo = Interface;
  }

  if (Target != NULL) {
    CopyMem (Target, TargetBuffer, TARGET_MAX_BYTES);
  }

  if (Lun != NULL) {
    *Lun = DeviceLun;
  }

  if (Diagnostic != NULL) {
    Diagnostic->StageStatus = EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsAllocateLowPages (
  IN  UINTN   Bytes,
  OUT VOID    **Buffer,
  OUT UINTN   *Pages
  )
{
  EFI_PHYSICAL_ADDRESS  MaximumAddress;
  EFI_STATUS            Status;
  UINTN                 RequiredPages;

  if ((Bytes == 0U) || (Buffer == NULL) || (Pages == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  RequiredPages = EFI_SIZE_TO_PAGES (Bytes);
  MaximumAddress = MTK_UFS_DMA_BOUNCE_MAX_ADDRESS;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  RequiredPages,
                  &MaximumAddress
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((MaximumAddress > MTK_UFS_DMA_BOUNCE_MAX_ADDRESS) ||
      ((EFI_PAGES_TO_SIZE (RequiredPages) - 1U) >
       (MTK_UFS_DMA_BOUNCE_MAX_ADDRESS - MaximumAddress)))
  {
    gBS->FreePages (MaximumAddress, RequiredPages);
    return EFI_UNSUPPORTED;
  }

  *Buffer = (VOID *)(UINTN)MaximumAddress;
  *Pages = RequiredPages;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsExecuteReadCommand (
  IN     MTK_UFS_BLOCK_DEVICE  *Device,
  IN     UINT8                 *Cdb,
  IN     UINT8                 CdbLength,
  IN OUT VOID                  *DataBuffer,
  IN     UINT32                DataLength,
  IN     UINT64                Timeout100ns,
  OUT    UINT32                *Transferred OPTIONAL
  )
{
  EFI_SCSI_IO_SCSI_REQUEST_PACKET  Packet;
  UINT8                            Sense[32];
  EFI_STATUS                       Status;

  if ((Device == NULL) || (Cdb == NULL) ||
      !MtkUfsScsiCdbIsReadOnly (Cdb, CdbLength) ||
      ((DataLength != 0U) && (DataBuffer == NULL)))
  {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Packet, sizeof (Packet));
  ZeroMem (Sense, sizeof (Sense));
  Packet.Timeout = Timeout100ns;
  Packet.InDataBuffer = DataBuffer;
  Packet.InTransferLength = DataLength;
  Packet.SenseData = Sense;
  Packet.SenseDataLength = sizeof (Sense);
  Packet.Cdb = Cdb;
  Packet.CdbLength = CdbLength;
  Packet.DataDirection = EFI_SCSI_IO_DATA_DIRECTION_READ;

  Status = Device->ScsiIo->ExecuteScsiCommand (Device->ScsiIo, &Packet, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Packet.HostAdapterStatus != EFI_SCSI_IO_STATUS_HOST_ADAPTER_OK) ||
      (Packet.TargetStatus != EFI_SCSI_IO_STATUS_TARGET_GOOD) ||
      (Packet.InTransferLength != DataLength))
  {
    return EFI_DEVICE_ERROR;
  }

  if (Transferred != NULL) {
    *Transferred = Packet.InTransferLength;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsReadCapacity (
  IN OUT MTK_UFS_BLOCK_DEVICE  *Device,
  IN OUT T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD *Diagnostic OPTIONAL
  )
{
  EFI_STATUS  Status;
  VOID        *Data;
  UINTN       Pages;
  UINT8       Cdb10[10];
  UINT8       Cdb16[16];
  UINT8       Capacity10[8];
  UINT8       Capacity16[32];
  uint64_t    LastBlock;
  uint32_t    BlockSize;
  int         NeedsCapacity16;

  if ((Device->ScsiIo->IoAlign > EFI_PAGE_SIZE) ||
      ((Device->ScsiIo->IoAlign > 1U) &&
       ((Device->ScsiIo->IoAlign & (Device->ScsiIo->IoAlign - 1U)) != 0U)))
  {
    if (Diagnostic != NULL) {
      Diagnostic->StartStage = T70UfsDiagStageIoAlignment;
      Diagnostic->StartStatus = EFI_UNSUPPORTED;
      Diagnostic->StageStatus = EFI_UNSUPPORTED;
    }
    return EFI_UNSUPPORTED;
  }

  if (Diagnostic != NULL) {
    Diagnostic->StartStage = T70UfsDiagStageCapacityAllocate;
  }
  Status = MtkUfsAllocateLowPages (sizeof (Capacity16), &Data, &Pages);
  if (EFI_ERROR (Status)) {
    if (Diagnostic != NULL) {
      Diagnostic->StartStatus = Status;
      Diagnostic->StageStatus = Status;
    }
    return Status;
  }

  ZeroMem (Cdb10, sizeof (Cdb10));
  Cdb10[0] = 0x25U;
  ZeroMem (Data, EFI_PAGES_TO_SIZE (Pages));
  if (Diagnostic != NULL) {
    Diagnostic->Flags |= T70_UFS_DIAG_CAPACITY10_ATTEMPTED;
    Diagnostic->StartStage = T70UfsDiagStageCapacity10;
  }
  Status = MtkUfsExecuteReadCommand (
             Device,
             Cdb10,
             sizeof (Cdb10),
             Data,
             sizeof (Capacity10),
             MTK_UFS_SCSI_TIMEOUT_100NS,
             NULL
             );
  if (Diagnostic != NULL) {
    Diagnostic->Capacity10Status = Status;
    Diagnostic->StageStatus = Status;
  }
  if (!EFI_ERROR (Status)) {
    CopyMem (Capacity10, Data, sizeof (Capacity10));
    Status = (MtkUfsDecodeReadCapacity10 (
                Capacity10,
                sizeof (Capacity10),
                &LastBlock,
                &BlockSize,
                &NeedsCapacity16
                ) == MtkUfsCoreSuccess) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
    if (EFI_ERROR (Status) && (Diagnostic != NULL)) {
      Diagnostic->StartStage = T70UfsDiagStageCapacity10Decode;
    }
    if (!EFI_ERROR (Status) && (Diagnostic != NULL)) {
      Diagnostic->BlockSize = BlockSize;
      Diagnostic->LastBlock = LastBlock;
    }
    if (Diagnostic != NULL) {
      Diagnostic->Capacity10Status = Status;
      Diagnostic->StageStatus = Status;
    }
  }

  if (!EFI_ERROR (Status) && NeedsCapacity16) {
    ZeroMem (Cdb16, sizeof (Cdb16));
    Cdb16[0] = 0x9EU;
    Cdb16[1] = 0x10U;
    Cdb16[10] = 0U;
    Cdb16[11] = 0U;
    Cdb16[12] = 0U;
    Cdb16[13] = sizeof (Capacity16);
    ZeroMem (Data, EFI_PAGES_TO_SIZE (Pages));
    if (Diagnostic != NULL) {
      Diagnostic->Flags |= T70_UFS_DIAG_CAPACITY16_ATTEMPTED;
      Diagnostic->StartStage = T70UfsDiagStageCapacity16;
    }
    Status = MtkUfsExecuteReadCommand (
               Device,
               Cdb16,
               sizeof (Cdb16),
               Data,
               sizeof (Capacity16),
               MTK_UFS_SCSI_TIMEOUT_100NS,
               NULL
               );
    if (Diagnostic != NULL) {
      Diagnostic->Capacity16Status = Status;
      Diagnostic->StageStatus = Status;
    }
    if (!EFI_ERROR (Status)) {
      CopyMem (Capacity16, Data, sizeof (Capacity16));
      Status = (MtkUfsDecodeReadCapacity16 (
                  Capacity16,
                  sizeof (Capacity16),
                  &LastBlock,
                  &BlockSize
                  ) == MtkUfsCoreSuccess) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
      if (EFI_ERROR (Status) && (Diagnostic != NULL)) {
        Diagnostic->StartStage = T70UfsDiagStageCapacity16Decode;
      }
      if (!EFI_ERROR (Status) && (Diagnostic != NULL)) {
        Diagnostic->BlockSize = BlockSize;
        Diagnostic->LastBlock = LastBlock;
      }
      if (Diagnostic != NULL) {
        Diagnostic->Capacity16Status = Status;
        Diagnostic->StageStatus = Status;
      }
    }
  }

  {
    EFI_STATUS  FreeStatus;

    FreeStatus = gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)Data, Pages);
    if (!EFI_ERROR (Status) && EFI_ERROR (FreeStatus)) {
      Status = FreeStatus;
      if (Diagnostic != NULL) {
        Diagnostic->StartStage = T70UfsDiagStageCapacityFree;
        Diagnostic->StartStatus = Status;
        Diagnostic->StageStatus = Status;
      }
    }
  }

  if (EFI_ERROR (Status)) {
    if (Diagnostic != NULL) {
      Diagnostic->StartStatus = Status;
      Diagnostic->StageStatus = Status;
    }
    return Status;
  }

  if (Diagnostic != NULL) {
    Diagnostic->StartStage = T70UfsDiagStageGeometry;
    Diagnostic->BlockSize = BlockSize;
    Diagnostic->LastBlock = LastBlock;
  }
  if ((BlockSize != MTK_UFS_EXPECTED_LU2_BLOCK_SIZE) ||
      !MtkUfsCapacityMatches (
         LastBlock,
         BlockSize,
         MTK_UFS_EXPECTED_LU2_BYTES
         ))
  {
    DEBUG ((
      DEBUG_ERROR,
      "MTK UFS LU2 capacity does not match the captured main-LU geometry\n"
      ));
    if (Diagnostic != NULL) {
      Diagnostic->StartStatus = EFI_UNSUPPORTED;
      Diagnostic->StageStatus = EFI_UNSUPPORTED;
    }
    return EFI_UNSUPPORTED;
  }

  Device->Media.MediaId = 1U;
  Device->Media.RemovableMedia = FALSE;
  Device->Media.MediaPresent = TRUE;
  Device->Media.LogicalPartition = FALSE;
  Device->Media.ReadOnly = TRUE;
  Device->Media.WriteCaching = FALSE;
  Device->Media.BlockSize = BlockSize;
  Device->Media.IoAlign = 0U;
  Device->Media.LastBlock = LastBlock;
  Device->Media.LowestAlignedLba = 0U;
  Device->Media.LogicalBlocksPerPhysicalBlock = 1U;
  Device->Media.OptimalTransferLengthGranularity = 1U;
  if (Diagnostic != NULL) {
    Diagnostic->StartStatus = EFI_SUCCESS;
    Diagnostic->StageStatus = EFI_SUCCESS;
  }
  return EFI_SUCCESS;
}

STATIC
UINT64
MtkUfsElapsedTicks (
  IN UINT64  Start,
  IN UINT64  Now,
  IN UINT64  CounterStart,
  IN UINT64  CounterEnd
  )
{
  if (CounterStart <= CounterEnd) {
    if (Now >= Start) {
      return Now - Start;
    }

    return (CounterEnd - Start) + (Now - CounterStart) + 1U;
  }

  if (Now <= Start) {
    return Start - Now;
  }

  return (Start - CounterEnd) + (CounterStart - Now) + 1U;
}

STATIC
EFI_STATUS
MtkUfsGetRemainingTimeout (
  IN  UINT64  StartCounter,
  IN  UINT64  CounterStart,
  IN  UINT64  CounterEnd,
  OUT UINT64  *Timeout100ns
  )
{
  UINT64  ElapsedTicks;
  UINT64  ElapsedNs;
  UINT64  RemainingNs;
  UINT64  Remaining100ns;

  if (Timeout100ns == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ElapsedTicks = MtkUfsElapsedTicks (
                   StartCounter,
                   GetPerformanceCounter (),
                   CounterStart,
                   CounterEnd
                   );
  ElapsedNs = GetTimeInNanoSecond (ElapsedTicks);
  if (ElapsedNs >= MTK_UFS_MAX_TOTAL_READ_NS) {
    return EFI_TIMEOUT;
  }

  RemainingNs = MTK_UFS_MAX_TOTAL_READ_NS - ElapsedNs;
  Remaining100ns = RemainingNs / 100U;
  if (Remaining100ns == 0U) {
    return EFI_TIMEOUT;
  }

  if (Remaining100ns > MTK_UFS_MAX_COMMAND_TIMEOUT_100NS) {
    Remaining100ns = MTK_UFS_MAX_COMMAND_TIMEOUT_100NS;
  }

  *Timeout100ns = Remaining100ns;
  return EFI_SUCCESS;
}

STATIC
VOID
MtkUfsBuildReadCdb (
  IN  EFI_LBA  Lba,
  IN  UINT32   BlockCount,
  OUT UINT8    *Cdb,
  OUT UINT8    *CdbLength
  )
{
  ZeroMem (Cdb, 16U);

  if ((Lba <= MAX_UINT32) &&
      ((UINT64)BlockCount <= (MAX_UINT32 - Lba + 1U)))
  {
    Cdb[0] = 0x28U;
    Cdb[2] = (UINT8)(Lba >> 24);
    Cdb[3] = (UINT8)(Lba >> 16);
    Cdb[4] = (UINT8)(Lba >> 8);
    Cdb[5] = (UINT8)Lba;
    Cdb[7] = (UINT8)(BlockCount >> 8);
    Cdb[8] = (UINT8)BlockCount;
    *CdbLength = 10U;
  } else {
    Cdb[0] = 0x88U;
    Cdb[2] = (UINT8)(Lba >> 56);
    Cdb[3] = (UINT8)(Lba >> 48);
    Cdb[4] = (UINT8)(Lba >> 40);
    Cdb[5] = (UINT8)(Lba >> 32);
    Cdb[6] = (UINT8)(Lba >> 24);
    Cdb[7] = (UINT8)(Lba >> 16);
    Cdb[8] = (UINT8)(Lba >> 8);
    Cdb[9] = (UINT8)Lba;
    Cdb[10] = (UINT8)(BlockCount >> 24);
    Cdb[11] = (UINT8)(BlockCount >> 16);
    Cdb[12] = (UINT8)(BlockCount >> 8);
    Cdb[13] = (UINT8)BlockCount;
    *CdbLength = 16U;
  }
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  MTK_UFS_BLOCK_DEVICE  *Device;
  EFI_STATUS            Status;
  EFI_STATUS            FreeStatus;
  EFI_PHYSICAL_ADDRESS  CounterStart;
  EFI_PHYSICAL_ADDRESS  CounterEnd;
  UINT64                StartCounter;
  UINT64                Frequency;
  UINT64                RemainingTimeout;
  UINT64                BlockCount;
  UINT64                BlocksLeft;
  UINT64                CurrentLba;
  UINTN                 Pages;
  UINTN                 MaxChunkBytes;
  UINTN                 ChunkBytes;
  UINT32                ChunkBlocks;
  UINT8                 Cdb[16];
  UINT8                 CdbLength;
  UINT8                 *Destination;
  VOID                  *BounceBuffer;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Device = MtkUfsBlockDeviceFromProtocol (This);
  if ((Device->Signature != MTK_UFS_BLOCK_SIGNATURE) || !Device->Started) {
    return EFI_DEVICE_ERROR;
  }

  if (MediaId != Device->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (BufferSize == 0U) {
    return EFI_SUCCESS;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((BufferSize % Device->Media.BlockSize) != 0U)
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  // DiskIo/FAT can submit a whole contiguous file. Bound each SCSI command,
  // not the caller's total read, while rejecting wrapping destination ranges.
  if (BufferSize > MAX_UINTN - (UINTN)Buffer) {
    return EFI_INVALID_PARAMETER;
  }

  BlockCount = BufferSize / Device->Media.BlockSize;
  if ((Lba > Device->Media.LastBlock) ||
      (BlockCount > (Device->Media.LastBlock - Lba + 1U)))
  {
    return EFI_INVALID_PARAMETER;
  }

  MaxChunkBytes = MTK_UFS_MAX_COMMAND_BYTES;
  if (MaxChunkBytes > BufferSize) {
    MaxChunkBytes = BufferSize;
  }

  Status = MtkUfsAllocateLowPages (MaxChunkBytes, &BounceBuffer, &Pages);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Frequency = GetPerformanceCounterProperties (&CounterStart, &CounterEnd);
  if (Frequency == 0U) {
    gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)BounceBuffer, Pages);
    return EFI_DEVICE_ERROR;
  }

  StartCounter = GetPerformanceCounter ();
  Destination = (UINT8 *)Buffer;
  CurrentLba = Lba;
  BlocksLeft = BlockCount;
  Status = EFI_SUCCESS;

  while (BlocksLeft != 0U) {
    UINT64  MaxBlocksPerCommand;

    MaxBlocksPerCommand = MTK_UFS_MAX_COMMAND_BYTES / Device->Media.BlockSize;
    if (MaxBlocksPerCommand == 0U) {
      Status = EFI_UNSUPPORTED;
      break;
    }

    ChunkBlocks = (UINT32)((BlocksLeft < MaxBlocksPerCommand) ?
                           BlocksLeft : MaxBlocksPerCommand);
    ChunkBytes = (UINTN)ChunkBlocks * Device->Media.BlockSize;
    MtkUfsBuildReadCdb (CurrentLba, ChunkBlocks, Cdb, &CdbLength);
    Status = MtkUfsGetRemainingTimeout (
               StartCounter,
               CounterStart,
               CounterEnd,
               &RemainingTimeout
               );
    if (EFI_ERROR (Status)) {
      break;
    }

    if (RemainingTimeout > MTK_UFS_SCSI_TIMEOUT_100NS) {
      RemainingTimeout = MTK_UFS_SCSI_TIMEOUT_100NS;
    }

    Status = MtkUfsExecuteReadCommand (
               Device,
               Cdb,
               CdbLength,
               BounceBuffer,
               (UINT32)ChunkBytes,
               RemainingTimeout,
               NULL
               );
    if (EFI_ERROR (Status)) {
      break;
    }

    CopyMem (Destination, BounceBuffer, ChunkBytes);
    Destination += ChunkBytes;
    CurrentLba += ChunkBlocks;
    BlocksLeft -= ChunkBlocks;
  }

  FreeStatus = gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)BounceBuffer, Pages);
  if (!EFI_ERROR (Status) && EFI_ERROR (FreeStatus)) {
    Status = FreeStatus;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  MTK_UFS_BLOCK_DEVICE  *Device;
  UINT64                BlockCount;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Device = MtkUfsBlockDeviceFromProtocol (This);
  if ((Device->Signature != MTK_UFS_BLOCK_SIGNATURE) || !Device->Started) {
    return EFI_DEVICE_ERROR;
  }

  if (MediaId != Device->Media.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((BufferSize == 0U) ||
      ((BufferSize % Device->Media.BlockSize) != 0U))
  {
    return EFI_BAD_BUFFER_SIZE;
  }

  BlockCount = BufferSize / Device->Media.BlockSize;
  if ((Lba > Device->Media.LastBlock) ||
      (BlockCount > (Device->Media.LastBlock - Lba + 1U)))
  {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)ExtendedVerification;
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsBlockDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD  *Diagnostic;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&DevicePath
                  );
  if (EFI_ERROR (Status) || !MtkUfsIsDiagnosticCandidate (DevicePath)) {
    return EFI_UNSUPPORTED;
  }

  Diagnostic = MtkUfsGetDiagnosticRecord (ControllerHandle);
  if ((Diagnostic != NULL) && ((Diagnostic->Flags & T70_UFS_DIAG_START_SEEN) != 0U)) {
    // Keep the last Start failure detail intact when the bus retries Supported.
    Diagnostic = NULL;
  }
  if (Diagnostic != NULL) {
    Diagnostic->Flags |= T70_UFS_DIAG_SUPPORTED_SEEN;
    Diagnostic->SupportedStage = T70UfsDiagStageNone;
    Diagnostic->SupportedStatus = EFI_NOT_READY;
    Diagnostic->StageStatus = EFI_NOT_READY;
  }

  (VOID)RemainingDevicePath;
  if (mDevice.Started && (mDevice.Handle == ControllerHandle)) {
    if (Diagnostic != NULL) {
      Diagnostic->SupportedStage = T70UfsDiagStageReady;
      Diagnostic->SupportedStatus = EFI_ALREADY_STARTED;
    }
    return EFI_ALREADY_STARTED;
  }

  Status = MtkUfsValidateExpectedDevice (
             ControllerHandle, FALSE, Diagnostic, NULL, NULL, NULL);
  if (EFI_ERROR (Status)) {
    if (Diagnostic != NULL) {
      Diagnostic->SupportedStatus = Status;
    }
    return EFI_UNSUPPORTED;
  }

  /*
   * Refuse a child already claimed by the generic writable disk driver.
   * A successfully installed read-only instance must not coexist with another
   * Block I/O producer on the same SCSI LU.
   */
  {
    EFI_BLOCK_IO_PROTOCOL  *ExistingBlockIo;

    Status = gBS->HandleProtocol (
                    ControllerHandle,
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&ExistingBlockIo
                    );
    if (!EFI_ERROR (Status)) {
      if (Diagnostic != NULL) {
        Diagnostic->SupportedStage = T70UfsDiagStageExistingBlockIo;
        Diagnostic->SupportedStatus = EFI_ACCESS_DENIED;
        Diagnostic->StageStatus = Status;
      }
      return EFI_ACCESS_DENIED;
    }
  }

  if (Diagnostic != NULL) {
    Diagnostic->SupportedStage = T70UfsDiagStageSupported;
    Diagnostic->SupportedStatus = EFI_SUCCESS;
    Diagnostic->StageStatus = EFI_SUCCESS;
  }
  return EFI_SUCCESS;
}

STATIC
VOID
MtkUfsCleanupFailedStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE ControllerHandle
  )
{
  EFI_STATUS Status;

  Status = gBS->CloseProtocol (
                  ControllerHandle,
                  &gEfiScsiIoProtocolGuid,
                  This->DriverBindingHandle,
                  ControllerHandle
                  );
  if (EFI_ERROR (Status)) {
    // No BlockIo was published, but our SCSI claim still exists. Retain the
    // context so DisconnectController/Stop can retry cleanup safely.
    mDevice.Started = TRUE;
    mDevice.BlockIoUninstalled = TRUE;
  } else {
    ZeroMem (&mDevice, sizeof (mDevice));
  }
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsBlockDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS            Status;
  EFI_SCSI_IO_PROTOCOL  *ScsiIo;
  EFI_BLOCK_IO_PROTOCOL *ExistingBlockIo;
  T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTIC_RECORD  *Diagnostic;

  if ((This == NULL) || mDevice.Started) {
    return EFI_UNSUPPORTED;
  }

  (VOID)RemainingDevicePath;

  Diagnostic = MtkUfsGetDiagnosticRecord (ControllerHandle);
  if (Diagnostic != NULL) {
    Diagnostic->Flags = (Diagnostic->Flags & T70_UFS_DIAG_SUPPORTED_SEEN) |
                        T70_UFS_DIAG_START_SEEN;
    Diagnostic->Lun = 0xFFU;
    Diagnostic->BlockSize = 0U;
    Diagnostic->LastBlock = 0U;
    Diagnostic->StartStage = T70UfsDiagStageNone;
    Diagnostic->StartStatus = EFI_NOT_READY;
    Diagnostic->StageStatus = EFI_NOT_READY;
    Diagnostic->OpenStatus = EFI_NOT_READY;
    Diagnostic->Capacity10Status = EFI_NOT_READY;
    Diagnostic->Capacity16Status = EFI_NOT_READY;
    Diagnostic->InstallStatus = EFI_NOT_READY;
  }

  Status = MtkUfsValidateExpectedDevice (
             ControllerHandle,
             TRUE,
             Diagnostic,
             &ScsiIo,
             mDevice.Target,
             &mDevice.Lun
             );
  if (EFI_ERROR (Status)) {
    if (Diagnostic != NULL) {
      Diagnostic->StartStatus = Status;
    }
    return Status;
  }

  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&ExistingBlockIo
                  );
  if (!EFI_ERROR (Status)) {
    if (Diagnostic != NULL) {
      Diagnostic->StartStage = T70UfsDiagStageExistingBlockIo;
      Diagnostic->StartStatus = EFI_ACCESS_DENIED;
      Diagnostic->StageStatus = Status;
    }
    return EFI_ACCESS_DENIED;
  }
  if (Diagnostic != NULL) {
    Diagnostic->StageStatus = Status;
    Diagnostic->StartStage = T70UfsDiagStageOpenScsi;
  }

  if (Diagnostic != NULL) {
    Diagnostic->Flags |= T70_UFS_DIAG_OPEN_ATTEMPTED;
  }
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiScsiIoProtocolGuid,
                  (VOID **)&ScsiIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (Diagnostic != NULL) {
    Diagnostic->OpenStatus = Status;
    Diagnostic->StageStatus = Status;
  }
  if (EFI_ERROR (Status)) {
    if (Diagnostic != NULL) {
      Diagnostic->StartStage = T70UfsDiagStageOpenScsi;
      Diagnostic->StartStatus = Status;
    }
    return Status;
  }

  ZeroMem (&mDevice.Media, sizeof (mDevice.Media));
  mDevice.Signature = MTK_UFS_BLOCK_SIGNATURE;
  mDevice.Handle = ControllerHandle;
  mDevice.ScsiIo = ScsiIo;
  mDevice.BlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION;
  mDevice.BlockIo.Media = &mDevice.Media;
  mDevice.BlockIo.Reset = MtkUfsReset;
  mDevice.BlockIo.ReadBlocks = MtkUfsReadBlocks;
  mDevice.BlockIo.WriteBlocks = MtkUfsWriteBlocks;
  mDevice.BlockIo.FlushBlocks = MtkUfsFlushBlocks;

  Status = MtkUfsReadCapacity (&mDevice, Diagnostic);
  if (EFI_ERROR (Status)) {
    MtkUfsCleanupFailedStart (This, ControllerHandle);
    if (Diagnostic != NULL) {
      Diagnostic->StartStatus = Status;
    }
    return Status;
  }

  if (Diagnostic != NULL) {
    Diagnostic->Flags |= T70_UFS_DIAG_INSTALL_ATTEMPTED;
    Diagnostic->StartStage = T70UfsDiagStageInstallBlockIo;
  }
  // Publish a usable interface: protocol notifications may run before the
  // install call returns. Failure retains state only if SCSI cleanup fails.
  mDevice.Started = TRUE;
  Status = gBS->InstallProtocolInterface (
                  &ControllerHandle,
                  &gEfiBlockIoProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDevice.BlockIo
                  );
  if (Diagnostic != NULL) {
    Diagnostic->InstallStatus = Status;
    Diagnostic->StageStatus = Status;
  }
  if (EFI_ERROR (Status)) {
    MtkUfsCleanupFailedStart (This, ControllerHandle);
    if (Diagnostic != NULL) {
      Diagnostic->StartStatus = Status;
      Diagnostic->StartStage = T70UfsDiagStageInstallBlockIo;
    }
    return Status;
  }

  if (Diagnostic != NULL) {
    Diagnostic->StartStage = T70UfsDiagStageReady;
    Diagnostic->StartStatus = EFI_SUCCESS;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsBlockDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS  Status;

  if ((This == NULL) || !mDevice.Started ||
      (mDevice.Handle != ControllerHandle))
  {
    return EFI_UNSUPPORTED;
  }

  if (NumberOfChildren != 0U) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)ChildHandleBuffer;

  // A failed CloseProtocol leaves our SCSI claim alive, but BlockIo has
  // already gone. A subsequent Stop must retry only the remaining cleanup.
  if (!mDevice.BlockIoUninstalled) {
    Status = gBS->UninstallProtocolInterface (
                    ControllerHandle,
                    &gEfiBlockIoProtocolGuid,
                    &mDevice.BlockIo
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mDevice.BlockIoUninstalled = TRUE;
  }

  Status = gBS->CloseProtocol (
                  ControllerHandle,
                  &gEfiScsiIoProtocolGuid,
                  This->DriverBindingHandle,
                  ControllerHandle
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&mDevice, sizeof (mDevice));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MtkUfsReadOnlyBlockIoEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  ZeroMem (&mDevice, sizeof (mDevice));
  ZeroMem (&mDiagnostics, sizeof (mDiagnostics));
  mDiagnostics.Version = T70_UFS_READ_ONLY_BLOCK_IO_DIAGNOSTICS_VERSION;
  mDiagnostics.DriverBindingInstallStatus = EFI_NOT_STARTED;
  mDiagnostics.DiagnosticsUninstallStatus = EFI_NOT_STARTED;
  (VOID)SystemTable;
  mDriverBinding.Supported = MtkUfsBlockDriverSupported;
  mDriverBinding.Start = MtkUfsBlockDriverStart;
  mDriverBinding.Stop = MtkUfsBlockDriverStop;
  mDriverBinding.Version = 0xFFFFFFF0;
  mDriverBinding.ImageHandle = ImageHandle;
  mDriverBinding.DriverBindingHandle = ImageHandle;

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &mDiagnosticsGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDiagnostics
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gEfiDriverBindingProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDriverBinding
                  );
  mDiagnostics.DriverBindingInstallStatus = Status;
  if (EFI_ERROR (Status)) {
    EFI_STATUS  UninstallStatus;

    UninstallStatus = gBS->UninstallProtocolInterface (
                        ImageHandle,
                        &mDiagnosticsGuid,
                        &mDiagnostics
                        );
    mDiagnostics.DiagnosticsUninstallStatus = UninstallStatus;
    if (EFI_ERROR (UninstallStatus)) {
      // Keep the image resident while its diagnostic protocol remains installed.
      return EFI_SUCCESS;
    }
  }
  return Status;
}
