/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_UFS_CORE_H_
#define MTK_UFS_CORE_H_

/*
 * Portable UFS wire and handoff helpers.
 * No UEFI or Linux dependencies: this can also be built into a Windows
 * transport.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTK_UFS_UPIU_HEADER_SIZE          12U
#define MTK_UFS_UPIU_RESPONSE_TYPE        0x21U
#define MTK_UFS_UPIU_RESPONSE_FIXED_SIZE  32U
#define MTK_UFS_UPIU_SENSE_LENGTH_SIZE    2U

#define MTK_UFS_HCE_ENABLED               (1U << 0)
#define MTK_UFS_HCS_DEVICE_PRESENT        (1U << 0)
#define MTK_UFS_HCS_UIC_COMMAND_READY     (1U << 3)

typedef enum {
  MtkUfsCoreSuccess = 0,
  MtkUfsCoreInvalidArgument,
  MtkUfsCoreBufferTooShort,
  MtkUfsCoreMalformed
} MTK_UFS_CORE_STATUS;

typedef struct {
  const uint8_t *Bytes;
  size_t AvailableLength;
  uint8_t DescriptorLength;
  uint8_t DescriptorType;
} MTK_UFS_DESCRIPTOR_VIEW;

typedef struct {
  uint8_t TransactionType;
  uint8_t Flags;
  uint8_t Lun;
  uint8_t TaskTag;
  uint8_t CommandSet;
  uint8_t InitiatorId;
  uint8_t Response;
  uint8_t ScsiStatus;
  uint16_t DataSegmentLength;
  uint32_t ResidualTransferCount;
  const uint8_t *SenseData;
  size_t SenseDataLength;
} MTK_UFS_RESPONSE_VIEW;

typedef enum {
  MtkUfsHandoffReadyForNop = 0,
  MtkUfsHandoffControllerDisabled,
  MtkUfsHandoffUicNotReady,
  MtkUfsHandoffDeviceAbsent,
  MtkUfsHandoffRequestsOutstanding
} MTK_UFS_HANDOFF_STATUS;

/*
 * HCS exposes Device Present and UIC Command Ready, but not the UFS link
 * state. This check only establishes that a non-destructive NOP probe may be
 * attempted; it does not prove that cold PHY/clock/reset setup has completed.
 */
MTK_UFS_CORE_STATUS
MtkUfsParseDescriptorHeader(
  const uint8_t *Data,
  size_t DataLength,
  MTK_UFS_DESCRIPTOR_VIEW *View
  );

MTK_UFS_CORE_STATUS
MtkUfsParseResponseUpiu(
  const uint8_t *Data,
  size_t DataLength,
  MTK_UFS_RESPONSE_VIEW *View
  );

MTK_UFS_HANDOFF_STATUS
MtkUfsAssessHandoff(
  uint32_t HostControllerEnable,
  uint32_t HostControllerStatus,
  uint32_t TransferRequestDoorbell,
  uint32_t TaskManagementDoorbell
  );

/*
 * Narrow SCSI allowlist for this driver's read-only Block I/O path. It is not
 * a filter for callers that use EFI_EXT_SCSI_PASS_THRU directly.
 */
int
MtkUfsScsiCdbIsReadOnly(
  const uint8_t *Cdb,
  size_t CdbLength
  );

MTK_UFS_CORE_STATUS
MtkUfsDecodeReadCapacity10(
  const uint8_t *Data,
  size_t DataLength,
  uint64_t *LastLogicalBlock,
  uint32_t *BlockSize,
  int *RequiresReadCapacity16
  );

int
MtkUfsCapacityMatches(
  uint64_t LastLogicalBlock,
  uint32_t BlockSize,
  uint64_t ExpectedCapacityBytes
  );

MTK_UFS_CORE_STATUS
MtkUfsDecodeReadCapacity16(
  const uint8_t *Data,
  size_t DataLength,
  uint64_t *LastLogicalBlock,
  uint32_t *BlockSize
  );

#ifdef __cplusplus
}
#endif

#endif
