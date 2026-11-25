/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MtkUfsCore.h>

static uint16_t
ReadBigEndian16(
  const uint8_t *Data
  )
{
  return (uint16_t)(((uint16_t)Data[0] << 8) | Data[1]);
}

static uint32_t
ReadBigEndian32(
  const uint8_t *Data
  )
{
  return ((uint32_t)Data[0] << 24) |
         ((uint32_t)Data[1] << 16) |
         ((uint32_t)Data[2] << 8) |
         (uint32_t)Data[3];
}

static uint64_t
ReadBigEndian64(
  const uint8_t *Data
  )
{
  return ((uint64_t)ReadBigEndian32(Data) << 32) |
         (uint64_t)ReadBigEndian32(Data + 4);
}

static int
IsSupportedBlockSize(
  uint32_t BlockSize
  )
{
  return (BlockSize >= 512U) &&
         (BlockSize <= 65536U) &&
         ((BlockSize & (BlockSize - 1U)) == 0U);
}

MTK_UFS_CORE_STATUS
MtkUfsParseDescriptorHeader(
  const uint8_t *Data,
  size_t DataLength,
  MTK_UFS_DESCRIPTOR_VIEW *View
  )
{
  MTK_UFS_DESCRIPTOR_VIEW Parsed;

  if ((Data == NULL) || (View == NULL)) {
    return MtkUfsCoreInvalidArgument;
  }

  if (DataLength < 2U) {
    return MtkUfsCoreBufferTooShort;
  }

  if ((Data[0] < 2U) || ((size_t)Data[0] > DataLength)) {
    return MtkUfsCoreMalformed;
  }

  Parsed.Bytes = Data;
  Parsed.AvailableLength = DataLength;
  Parsed.DescriptorLength = Data[0];
  Parsed.DescriptorType = Data[1];
  *View = Parsed;
  return MtkUfsCoreSuccess;
}

MTK_UFS_CORE_STATUS
MtkUfsParseResponseUpiu(
  const uint8_t *Data,
  size_t DataLength,
  MTK_UFS_RESPONSE_VIEW *View
  )
{
  MTK_UFS_RESPONSE_VIEW Parsed;
  uint16_t SenseLength;
  size_t DataSegmentEnd;

  if ((Data == NULL) || (View == NULL)) {
    return MtkUfsCoreInvalidArgument;
  }

  if (DataLength < 16U) {
    return MtkUfsCoreBufferTooShort;
  }

  if ((Data[0] & 0x3FU) != MTK_UFS_UPIU_RESPONSE_TYPE) {
    return MtkUfsCoreMalformed;
  }

  Parsed.TransactionType = (uint8_t)(Data[0] & 0x3FU);
  Parsed.Flags = Data[1];
  Parsed.Lun = Data[2];
  Parsed.TaskTag = Data[3];
  Parsed.CommandSet = (uint8_t)(Data[4] & 0x0FU);
  Parsed.InitiatorId = (uint8_t)(Data[4] >> 4);
  Parsed.Response = Data[6];
  Parsed.ScsiStatus = Data[7];
  Parsed.DataSegmentLength = ReadBigEndian16(Data + 10);
  Parsed.ResidualTransferCount = ReadBigEndian32(Data + 12);
  Parsed.SenseData = NULL;
  Parsed.SenseDataLength = 0U;

  if (Parsed.DataSegmentLength == 0U) {
    *View = Parsed;
    return MtkUfsCoreSuccess;
  }

  if (Parsed.DataSegmentLength < MTK_UFS_UPIU_SENSE_LENGTH_SIZE) {
    return MtkUfsCoreMalformed;
  }

  if (DataLength < (MTK_UFS_UPIU_RESPONSE_FIXED_SIZE +
                    MTK_UFS_UPIU_SENSE_LENGTH_SIZE)) {
    return MtkUfsCoreBufferTooShort;
  }

  if ((size_t)Parsed.DataSegmentLength >
      (DataLength - MTK_UFS_UPIU_RESPONSE_FIXED_SIZE)) {
    return MtkUfsCoreMalformed;
  }

  DataSegmentEnd = MTK_UFS_UPIU_RESPONSE_FIXED_SIZE +
                   (size_t)Parsed.DataSegmentLength;
  SenseLength = ReadBigEndian16(Data + MTK_UFS_UPIU_RESPONSE_FIXED_SIZE);
  if (((size_t)SenseLength >
       ((size_t)Parsed.DataSegmentLength - MTK_UFS_UPIU_SENSE_LENGTH_SIZE)) ||
      ((size_t)SenseLength >
       (DataSegmentEnd - MTK_UFS_UPIU_RESPONSE_FIXED_SIZE -
        MTK_UFS_UPIU_SENSE_LENGTH_SIZE))) {
    return MtkUfsCoreMalformed;
  }

  Parsed.SenseData = Data + MTK_UFS_UPIU_RESPONSE_FIXED_SIZE +
                     MTK_UFS_UPIU_SENSE_LENGTH_SIZE;
  Parsed.SenseDataLength = SenseLength;
  *View = Parsed;
  return MtkUfsCoreSuccess;
}

MTK_UFS_HANDOFF_STATUS
MtkUfsAssessHandoff(
  uint32_t HostControllerEnable,
  uint32_t HostControllerStatus,
  uint32_t TransferRequestDoorbell,
  uint32_t TaskManagementDoorbell
  )
{
  if ((HostControllerEnable & MTK_UFS_HCE_ENABLED) == 0U) {
    return MtkUfsHandoffControllerDisabled;
  }

  if ((HostControllerStatus & MTK_UFS_HCS_UIC_COMMAND_READY) == 0U) {
    return MtkUfsHandoffUicNotReady;
  }

  if ((HostControllerStatus & MTK_UFS_HCS_DEVICE_PRESENT) == 0U) {
    return MtkUfsHandoffDeviceAbsent;
  }

  if ((TransferRequestDoorbell != 0U) || (TaskManagementDoorbell != 0U)) {
    return MtkUfsHandoffRequestsOutstanding;
  }

  return MtkUfsHandoffReadyForNop;
}

int
MtkUfsScsiCdbIsReadOnly(
  const uint8_t *Cdb,
  size_t CdbLength
  )
{
  if ((Cdb == NULL) || (CdbLength == 0U)) {
    return 0;
  }

  switch (Cdb[0]) {
    case 0x08: /* READ(6) */
      return CdbLength >= 6U;
    case 0x25: /* READ CAPACITY(10) */
      return (CdbLength >= 10U) && (Cdb[1] == 0U);
    case 0x28: /* READ(10) */
      return CdbLength >= 10U;
    case 0x9E: /* READ CAPACITY(16) */
      return (CdbLength >= 16U) && ((Cdb[1] & 0x1FU) == 0x10U);
    case 0xA8: /* READ(12) */
      return CdbLength >= 12U;
    case 0x88: /* READ(16) */
      return CdbLength >= 16U;
    default:
      return 0;
  }
}

MTK_UFS_CORE_STATUS
MtkUfsDecodeReadCapacity10(
  const uint8_t *Data,
  size_t DataLength,
  uint64_t *LastLogicalBlock,
  uint32_t *BlockSize,
  int *RequiresReadCapacity16
  )
{
  uint32_t LastLba;
  uint32_t ParsedBlockSize;

  if ((Data == NULL) || (LastLogicalBlock == NULL) ||
      (BlockSize == NULL) || (RequiresReadCapacity16 == NULL)) {
    return MtkUfsCoreInvalidArgument;
  }

  if (DataLength < 8U) {
    return MtkUfsCoreBufferTooShort;
  }

  LastLba = ReadBigEndian32(Data);
  ParsedBlockSize = ReadBigEndian32(Data + 4);
  if (!IsSupportedBlockSize(ParsedBlockSize)) {
    return MtkUfsCoreMalformed;
  }

  *LastLogicalBlock = LastLba;
  *BlockSize = ParsedBlockSize;
  *RequiresReadCapacity16 = (LastLba == UINT32_MAX);
  return MtkUfsCoreSuccess;
}

MTK_UFS_CORE_STATUS
MtkUfsDecodeReadCapacity16(
  const uint8_t *Data,
  size_t DataLength,
  uint64_t *LastLogicalBlock,
  uint32_t *BlockSize
  )
{
  uint64_t LastLba;
  uint32_t ParsedBlockSize;

  if ((Data == NULL) || (LastLogicalBlock == NULL) || (BlockSize == NULL)) {
    return MtkUfsCoreInvalidArgument;
  }

  if (DataLength < 12U) {
    return MtkUfsCoreBufferTooShort;
  }

  LastLba = ReadBigEndian64(Data);
  ParsedBlockSize = ReadBigEndian32(Data + 8);
  if (!IsSupportedBlockSize(ParsedBlockSize)) {
    return MtkUfsCoreMalformed;
  }

  *LastLogicalBlock = LastLba;
  *BlockSize = ParsedBlockSize;
  return MtkUfsCoreSuccess;
}

int
MtkUfsCapacityMatches(
  uint64_t LastLogicalBlock,
  uint32_t BlockSize,
  uint64_t ExpectedCapacityBytes
  )
{
  uint64_t BlockCount;

  if ((ExpectedCapacityBytes == 0U) ||
      !IsSupportedBlockSize(BlockSize) ||
      (LastLogicalBlock == UINT64_MAX))
  {
    return 0;
  }

  BlockCount = LastLogicalBlock + 1U;
  if (BlockCount > (UINT64_MAX / BlockSize)) {
    return 0;
  }

  return ((BlockCount * BlockSize) == ExpectedCapacityBytes);
}
