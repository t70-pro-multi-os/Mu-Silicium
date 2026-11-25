/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "T70UfsHost.h"

#include <Library/MtkUfsCore.h>

#define T70_UFS_HOST_SIGNATURE 0x54303755U

static void
ClearBytes(void *Buffer, size_t Bytes)
{
  uint8_t *Out = (uint8_t *)Buffer;
  size_t Index;

  for (Index = 0; Index < Bytes; ++Index) {
    Out[Index] = 0U;
  }
}

static int
HostIsInitialized(const T70_UFS_HOST *Host)
{
  return (Host != NULL) && (Host->Signature == T70_UFS_HOST_SIGNATURE) &&
         (Host->Initialized == 1U);
}

static int
HandoffIsValid(
  const T70_UFS_HANDOFF_SNAPSHOT *Snapshot,
  uint64_t MappedMmioBase
  )
{
  if (Snapshot == NULL) {
    return 0;
  }

  if ((Snapshot->HandoffAddress != T70_UFS_HANDOFF_BASE) ||
      (Snapshot->Magic != T70_UFS_HANDOFF_MAGIC) ||
      (Snapshot->FdtStatus != 0U) ||
      (Snapshot->FdtNodeStatus != T70UfsFdtNodeEnabled) ||
      (Snapshot->HandoffReady != 1U) ||
      (Snapshot->HandoffMmioBase != T70_UFS_CONTROLLER_BASE) ||
      (Snapshot->HandoffLun != MTK_UFS_USER_LUN) ||
      (Snapshot->HandoffFormat != T70_UFS_HANDOFF_FORMAT) ||
      (Snapshot->FdtControllerBase != T70_UFS_CONTROLLER_BASE) ||
      (Snapshot->FdtControllerSize != T70_UFS_CONTROLLER_SIZE) ||
      (MappedMmioBase != T70_UFS_CONTROLLER_BASE)) {
    return 0;
  }

  if ((Snapshot->FdtAddress == 0U) ||
      ((Snapshot->FdtAddress & 7U) != 0U) ||
      (Snapshot->FdtSize < T70_UFS_FDT_HEADER_BYTES) ||
      (Snapshot->FdtSize > T70_UFS_HANDOFF_FDT_MAX) ||
      (Snapshot->FdtAddress > (UINT64_MAX - Snapshot->FdtSize))) {
    return 0;
  }

  return 1;
}

static int
TransportArgumentsAreValid(
  const MTK_UFS_TRANSPORT_IO *Io,
  volatile void *Dma,
  size_t DmaBytes,
  uint64_t DmaPhysical
  )
{
  return (Io != NULL) && (Io->Read32 != NULL) && (Io->Write32 != NULL) &&
         (Io->Fence != NULL) && (Io->StallUs != NULL) &&
         (Dma != NULL) && (DmaBytes >= MTK_UFS_DMA_BYTES) &&
         (((uintptr_t)Dma & 7U) == 0U) && ((DmaPhysical & 1023U) == 0U);
}

static int
PollCommand(
  T70_UFS_HOST *Host,
  MTK_UFS_IO_RESULT *Result
  )
{
  MTK_UFS_IO_RESULT Current = { 0 };
  MTK_UFS_IO_STATUS IoStatus;
  unsigned Poll;

  for (Poll = 0; Poll < T70_UFS_MAX_POLLS; ++Poll) {
    IoStatus = MtkUfsTransportPoll(&Host->Transport, &Current);
    if (IoStatus == MtkUfsIoSuccess) {
      *Result = Current;
      return 1;
    }
    if (IoStatus != MtkUfsIoPending) {
      return 0;
    }
    if ((Poll + 1U) < T70_UFS_MAX_POLLS) {
      Host->Transport.Io.StallUs(
        Host->Transport.Io.Context, T70_UFS_POLL_INTERVAL_US);
    }
  }

  /* The active command may still own its descriptor and payload. */
  Host->Transport.Faulted = 1U;
  return 0;
}

static int
RunNop(T70_UFS_HOST *Host)
{
  MTK_UFS_IO_RESULT Result = { 0 };

  if (MtkUfsTransportNop(&Host->Transport) != MtkUfsIoSuccess) {
    return 0;
  }
  if (!PollCommand(Host, &Result)) {
    return 0;
  }

  return (Result.ScsiStatus == 0U) && (Result.SenseLength == 0U) &&
         (Result.Transferred == 0U);
}

static int
ReadCapacityData(
  T70_UFS_HOST *Host,
  const uint8_t *Cdb,
  size_t CdbLength,
  uint32_t TransferBytes,
  uint8_t *Data
  )
{
  MTK_UFS_IO_RESULT Result = { 0 };
  MTK_UFS_IO_STATUS IoStatus;
  uint32_t Index;

  if (!MtkUfsTransportReadCdb(Cdb, CdbLength, TransferBytes)) {
    return 0;
  }

  IoStatus = MtkUfsTransportScsi(
    &Host->Transport, Cdb, CdbLength, TransferBytes);
  if (IoStatus != MtkUfsIoSuccess) {
    return 0;
  }

  if (!PollCommand(Host, &Result) || (Result.ScsiStatus != 0U) ||
      (Result.SenseLength != 0U) ||
      (Result.Transferred != TransferBytes)) {
    return 0;
  }

  Host->Transport.Io.Fence(Host->Transport.Io.Context);
  for (Index = 0; Index < TransferBytes; ++Index) {
    Data[Index] = Host->Transport.Dma[MTK_UFS_DATA_OFFSET + Index];
  }
  return 1;
}

void
T70UfsHostInitialize(
  T70_UFS_HOST *Host
  )
{
  if (Host == NULL) {
    return;
  }

  ClearBytes(Host, sizeof(*Host));
  Host->Signature = T70_UFS_HOST_SIGNATURE;
  Host->Initialized = 1U;
}

T70_UFS_HOST_STATUS
T70UfsHostStop(
  T70_UFS_HOST *Host
  )
{
  MTK_UFS_IO_STATUS IoStatus;

  if (!HostIsInitialized(Host)) {
    return T70UfsHostInvalidArgument;
  }

  Host->Ready = 0U;
  Host->LastLogicalBlock = 0U;
  Host->BlockSize = 0U;

  if (!Host->Transport.Attached) {
    if (Host->DmaQuarantined) {
      Host->StopSucceeded = 0U;
      return T70UfsHostDmaOwnershipUncertain;
    }
    ClearBytes(&Host->Transport, sizeof(Host->Transport));
    Host->StopSucceeded = 1U;
    return T70UfsHostSuccess;
  }

  IoStatus = MtkUfsTransportStop(&Host->Transport);
  if (IoStatus != MtkUfsIoSuccess) {
    Host->StopSucceeded = 0U;
    Host->DmaQuarantined = 1U;
    return T70UfsHostStopFailed;
  }

  ClearBytes(&Host->Transport, sizeof(Host->Transport));
  Host->DmaQuarantined = 0U;
  Host->StopSucceeded = 1U;
  return T70UfsHostSuccess;
}

static T70_UFS_HOST_STATUS
FailAndStop(
  T70_UFS_HOST *Host,
  T70_UFS_HOST_STATUS Failure
  )
{
  T70_UFS_HOST_STATUS StopStatus;

  Host->Ready = 0U;
  Host->LastLogicalBlock = 0U;
  Host->BlockSize = 0U;
  StopStatus = T70UfsHostStop(Host);
  if (StopStatus != T70UfsHostSuccess) {
    return T70UfsHostDmaOwnershipUncertain;
  }
  return Failure;
}

T70_UFS_HOST_STATUS
T70UfsHostStart(
  T70_UFS_HOST *Host,
  const T70_UFS_HANDOFF_SNAPSHOT *Snapshot,
  uint64_t MappedMmioBase,
  const MTK_UFS_TRANSPORT_IO *Io,
  volatile void *Dma,
  size_t DmaBytes,
  uint64_t DmaPhysical
  )
{
  MTK_UFS_IO_STATUS IoStatus;
  MTK_UFS_CORE_STATUS CoreStatus;
  uint8_t Cdb[16] = { 0 };
  uint8_t CapacityData[32] = { 0 };
  uint64_t LastLogicalBlock = 0U;
  uint32_t BlockSize = 0U;
  int RequiresReadCapacity16 = 0;

  if (!HostIsInitialized(Host)) {
    return T70UfsHostInvalidArgument;
  }

  if (Host->DmaQuarantined || Host->Transport.Attached ||
      Host->Transport.Active || Host->Transport.Faulted) {
    return T70UfsHostDmaOwnershipUncertain;
  }

  if (!HandoffIsValid(Snapshot, MappedMmioBase)) {
    return T70UfsHostInvalidHandoff;
  }

  if (!TransportArgumentsAreValid(Io, Dma, DmaBytes, DmaPhysical)) {
    return T70UfsHostInvalidArgument;
  }

  Host->Ready = 0U;
  Host->DmaQuarantined = 0U;
  Host->StopSucceeded = 0U;
  Host->LastLogicalBlock = 0U;
  Host->BlockSize = 0U;

  IoStatus = MtkUfsTransportAttach(
    &Host->Transport, Io, Dma, DmaBytes, DmaPhysical);
  if (IoStatus != MtkUfsIoSuccess) {
    return FailAndStop(Host, T70UfsHostAttachFailed);
  }

  if (!RunNop(Host)) {
    return FailAndStop(Host, T70UfsHostNopFailed);
  }

  Cdb[0] = 0x25U; /* READ CAPACITY(10) */
  if (!ReadCapacityData(Host, Cdb, 10U, 8U, CapacityData)) {
    return FailAndStop(Host, T70UfsHostCapacityFailed);
  }

  CoreStatus = MtkUfsDecodeReadCapacity10(
    CapacityData, 8U, &LastLogicalBlock, &BlockSize,
    &RequiresReadCapacity16);
  if (CoreStatus != MtkUfsCoreSuccess) {
    return FailAndStop(Host, T70UfsHostCapacityFailed);
  }

  if (RequiresReadCapacity16) {
    ClearBytes(Cdb, sizeof(Cdb));
    ClearBytes(CapacityData, sizeof(CapacityData));
    Cdb[0] = 0x9EU; /* SERVICE ACTION IN(16) */
    Cdb[1] = 0x10U; /* READ CAPACITY(16) */
    Cdb[13] = 32U;
    if (!ReadCapacityData(Host, Cdb, sizeof(Cdb), sizeof(CapacityData),
                         CapacityData)) {
      return FailAndStop(Host, T70UfsHostCapacityFailed);
    }

    CoreStatus = MtkUfsDecodeReadCapacity16(
      CapacityData, sizeof(CapacityData), &LastLogicalBlock, &BlockSize);
    if (CoreStatus != MtkUfsCoreSuccess) {
      return FailAndStop(Host, T70UfsHostCapacityFailed);
    }
  }

  if ((BlockSize != MTK_UFS_BLOCK_BYTES) ||
      !MtkUfsCapacityMatches(
        LastLogicalBlock, BlockSize, MTK_UFS_USER_BYTES)) {
    return FailAndStop(Host, T70UfsHostGeometryMismatch);
  }

  Host->LastLogicalBlock = LastLogicalBlock;
  Host->BlockSize = BlockSize;
  Host->Ready = 1U;
  return T70UfsHostSuccess;
}

T70_UFS_READ_STATUS
T70UfsHostReadBlocks(
  T70_UFS_HOST *Host,
  uint64_t LogicalBlock,
  uint64_t BlockCount,
  void *Buffer,
  size_t BufferBytes
  )
{
  T70_UFS_READ_STATUS ReadStatus;

  if (!HostIsInitialized(Host)) {
    return T70UfsReadNotReady;
  }
  if (Host->DmaQuarantined) {
    return T70UfsReadDmaOwnershipUncertain;
  }
  if (!Host->Ready) {
    return T70UfsReadNotReady;
  }

  ReadStatus = T70UfsReadBlocks(
    &Host->Transport, Host->LastLogicalBlock, Host->BlockSize,
    LogicalBlock, BlockCount, Buffer, BufferBytes);
  if (ReadStatus == T70UfsReadDmaOwnershipUncertain) {
    Host->Ready = 0U;
    Host->DmaQuarantined = 1U;
    Host->StopSucceeded = 0U;
  }
  return ReadStatus;
}
