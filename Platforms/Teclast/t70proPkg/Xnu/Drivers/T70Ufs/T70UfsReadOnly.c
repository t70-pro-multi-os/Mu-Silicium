/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T70UfsReadOnly.h"
#include <Library/MtkUfsCore.h>

static void
StoreBe32(uint8_t *Data, uint32_t Value)
{
  Data[0] = (uint8_t)(Value >> 24);
  Data[1] = (uint8_t)(Value >> 16);
  Data[2] = (uint8_t)(Value >> 8);
  Data[3] = (uint8_t)Value;
}

static void
StoreBe64(uint8_t *Data, uint64_t Value)
{
  StoreBe32(Data, (uint32_t)(Value >> 32));
  StoreBe32(Data + 4, (uint32_t)Value);
}

static int
RangesOverlap(uintptr_t First, size_t FirstLength, uintptr_t Second, size_t SecondLength)
{
  if ((FirstLength > UINTPTR_MAX - First) ||
      (SecondLength > UINTPTR_MAX - Second)) {
    return 1;
  }

  return (First < Second + SecondLength) &&
         (Second < First + FirstLength);
}

T70_UFS_READ_STATUS
T70UfsValidateRead(
  uint64_t LastLogicalBlock,
  uint32_t BlockSize,
  uint64_t LogicalBlock,
  uint64_t BlockCount,
  const void *Buffer,
  size_t BufferBytes,
  uint32_t *TransferBytes
  )
{
  uint64_t Transfer;

  if (TransferBytes != NULL) {
    *TransferBytes = 0;
  }
  if ((Buffer == NULL) || (TransferBytes == NULL) || (BlockCount == 0U)) {
    return T70UfsReadInvalidArgument;
  }

  if ((BlockSize != MTK_UFS_BLOCK_BYTES) ||
      !MtkUfsCapacityMatches(LastLogicalBlock, BlockSize, MTK_UFS_USER_BYTES)) {
    return T70UfsReadGeometryMismatch;
  }

  if ((LogicalBlock > LastLogicalBlock) ||
      ((BlockCount - 1U) > (LastLogicalBlock - LogicalBlock))) {
    return T70UfsReadOutOfRange;
  }

  if (BlockCount > (MTK_UFS_DATA_BYTES / BlockSize)) {
    return T70UfsReadTooLarge;
  }

  Transfer = BlockCount * BlockSize;
  if ((Transfer > UINT32_MAX) || (Transfer > BufferBytes)) {
    return T70UfsReadBufferTooSmall;
  }

  *TransferBytes = (uint32_t)Transfer;
  return T70UfsReadSuccess;
}

T70_UFS_READ_STATUS
T70UfsReadBlocks(
  MTK_UFS_TRANSPORT *Transport,
  uint64_t LastLogicalBlock,
  uint32_t BlockSize,
  uint64_t LogicalBlock,
  uint64_t BlockCount,
  void *Buffer,
  size_t BufferBytes
  )
{
  uint8_t Cdb[16] = { 0 };
  uint32_t TransferBytes = 0;
  MTK_UFS_IO_RESULT Result = { 0 };
  MTK_UFS_IO_STATUS IoStatus;
  uintptr_t DmaAddress;
  uintptr_t BufferAddress;
  unsigned Poll;

  if (Transport == NULL) {
    return T70UfsReadInvalidArgument;
  }

  if (!Transport->Attached || Transport->Faulted || (Transport->Dma == NULL) ||
      (Transport->Io.StallUs == NULL) || (Transport->Io.Fence == NULL) ||
      (((uintptr_t)Transport->Dma & 7U) != 0U) ||
      ((Transport->Physical & 1023U) != 0U)) {
    return T70UfsReadNotReady;
  }
  if (Transport->Active || Transport->UicPending ||
      Transport->Hibernate8CommandDone) {
    return T70UfsReadBusy;
  }

  {
    T70_UFS_READ_STATUS Validation = T70UfsValidateRead(
      LastLogicalBlock, BlockSize, LogicalBlock, BlockCount,
      Buffer, BufferBytes, &TransferBytes);
    if (Validation != T70UfsReadSuccess) {
      return Validation;
    }
  }

  DmaAddress = (uintptr_t)Transport->Dma;
  BufferAddress = (uintptr_t)Buffer;
  if (RangesOverlap(DmaAddress, MTK_UFS_DMA_BYTES,
                    BufferAddress, TransferBytes)) {
    return T70UfsReadInvalidArgument;
  }

  Cdb[0] = 0x88U; /* READ(16) */
  StoreBe64(Cdb + 2, LogicalBlock);
  StoreBe32(Cdb + 10, (uint32_t)BlockCount);
  if (!MtkUfsTransportReadCdb(Cdb, sizeof(Cdb), TransferBytes)) {
    return T70UfsReadOutOfRange;
  }

  IoStatus = MtkUfsTransportScsi(Transport, Cdb, sizeof(Cdb), TransferBytes);
  if (IoStatus != MtkUfsIoSuccess) {
    /*
     * A faulted or active transport cannot prove that the controller released
     * its descriptor. Keep the DMA allocation quarantined until Stop succeeds.
     */
    if (Transport->Faulted || Transport->Active) {
      Transport->Faulted = 1U;
      return T70UfsReadDmaOwnershipUncertain;
    }
    return T70UfsReadDeviceError;
  }

  for (Poll = 0; Poll < T70_UFS_MAX_POLLS; ++Poll) {
    IoStatus = MtkUfsTransportPoll(Transport, &Result);
    if (IoStatus == MtkUfsIoSuccess) {
      size_t Index;
      if ((Result.ScsiStatus != 0U) || (Result.SenseLength != 0U) ||
          (Result.Transferred != TransferBytes)) {
        return T70UfsReadDeviceError;
      }
      Transport->Io.Fence(Transport->Io.Context);
      for (Index = 0; Index < TransferBytes; ++Index) {
        ((uint8_t *)Buffer)[Index] =
          Transport->Dma[MTK_UFS_DATA_OFFSET + Index];
      }
      return T70UfsReadSuccess;
    }
    if (IoStatus != MtkUfsIoPending) {
      /*
       * Polling did not prove completion. Preserve the active descriptor and
       * fault the transport so callers cannot reuse DMA before Stop succeeds.
       */
      Transport->Faulted = 1U;
      return T70UfsReadDmaOwnershipUncertain;
    }
    if ((Poll + 1U) < T70_UFS_MAX_POLLS) {
      Transport->Io.StallUs(Transport->Io.Context, T70_UFS_POLL_INTERVAL_US);
    }
  }

  /* The controller may still own its PRDT after the bounded wait. */
  Transport->Faulted = 1U;
  return T70UfsReadDmaOwnershipUncertain;
}
