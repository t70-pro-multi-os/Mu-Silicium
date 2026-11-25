/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_BLOCK_REQUEST_H_
#define T70_UFS_BLOCK_REQUEST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <Library/MtkUfsCore.h>
#include "T70UfsReadOnly.h"

typedef enum {
  T70UfsBlockRequestSuccess = 0,
  T70UfsBlockRequestInvalid,
  T70UfsBlockRequestNotWritable,
  T70UfsBlockRequestGeometryMismatch,
  T70UfsBlockRequestOutOfRange,
  T70UfsBlockRequestBufferTooSmall,
  T70UfsBlockRequestOverflow
} T70_UFS_BLOCK_REQUEST_STATUS;

/* Shared by the IOBlockStorageDevice method and host method checks. */
static inline T70_UFS_BLOCK_REQUEST_STATUS
T70UfsBlockPlanRequest(
  bool ReadDirection,
  uint64_t LastLogicalBlock,
  uint32_t BlockBytes,
  uint64_t LogicalBlock,
  uint64_t BlockCount,
  size_t BufferBytes,
  uint64_t *TransferBytes
  )
{
  uint64_t Transfer;

  if (TransferBytes != NULL) {
    *TransferBytes = 0;
  }
  if ((TransferBytes == NULL) || (BlockCount == 0U)) {
    return T70UfsBlockRequestInvalid;
  }
  if (!ReadDirection) {
    return T70UfsBlockRequestNotWritable;
  }
  if ((BlockBytes != MTK_UFS_BLOCK_BYTES) ||
      !MtkUfsCapacityMatches(LastLogicalBlock, BlockBytes, MTK_UFS_USER_BYTES)) {
    return T70UfsBlockRequestGeometryMismatch;
  }
  if ((LogicalBlock > LastLogicalBlock) ||
      ((BlockCount - 1U) > (LastLogicalBlock - LogicalBlock))) {
    return T70UfsBlockRequestOutOfRange;
  }
  if (BlockCount > (UINT64_MAX / BlockBytes)) {
    return T70UfsBlockRequestOverflow;
  }

  Transfer = BlockCount * BlockBytes;
  if (Transfer > (uint64_t)BufferBytes) {
    return T70UfsBlockRequestBufferTooSmall;
  }
  *TransferBytes = Transfer;
  return T70UfsBlockRequestSuccess;
}

#endif
