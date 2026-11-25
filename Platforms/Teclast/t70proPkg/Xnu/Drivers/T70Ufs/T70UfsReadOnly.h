/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_READ_ONLY_H_
#define T70_UFS_READ_ONLY_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <Library/MtkUfsTransport.h>

#define T70_UFS_POLL_INTERVAL_US 10U
#define T70_UFS_TIMEOUT_US       1000000U
#define T70_UFS_MAX_POLLS        (T70_UFS_TIMEOUT_US / T70_UFS_POLL_INTERVAL_US)

typedef enum {
  T70UfsReadSuccess = 0,
  T70UfsReadInvalidArgument,
  T70UfsReadGeometryMismatch,
  T70UfsReadOutOfRange,
  T70UfsReadTooLarge,
  T70UfsReadBufferTooSmall,
  T70UfsReadNotReady,
  T70UfsReadBusy,
  T70UfsReadDeviceError,
  T70UfsReadDmaOwnershipUncertain
} T70_UFS_READ_STATUS;

/* This capacity follows the existing MediaTek EDK2 read-only LU2 policy.
 * It is not a runtime observation of the T70 Pro storage device. */
T70_UFS_READ_STATUS
T70UfsValidateRead(
  uint64_t LastLogicalBlock,
  uint32_t BlockSize,
  uint64_t LogicalBlock,
  uint64_t BlockCount,
  const void *Buffer,
  size_t BufferBytes,
  uint32_t *TransferBytes
  );

/*
 * Caller must construct Transport using MtkUfsTransportAttach only after
 * verifying the live MMIO range, exclusive controller ownership and the
 * physically contiguous DMA window.
 *
 * The transport is single-slot: serialize every operation using it. If
 * submission or polling cannot prove completion, the adapter returns
 * T70UfsReadDmaOwnershipUncertain. Do not release or reuse Transport->Dma
 * until MtkUfsTransportStop returns success. The adapter exposes reads only
 * and never frees the DMA allocation.
 */
T70_UFS_READ_STATUS
T70UfsReadBlocks(
  MTK_UFS_TRANSPORT *Transport,
  uint64_t LastLogicalBlock,
  uint32_t BlockSize,
  uint64_t LogicalBlock,
  uint64_t BlockCount,
  void *Buffer,
  size_t BufferBytes
  );

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
