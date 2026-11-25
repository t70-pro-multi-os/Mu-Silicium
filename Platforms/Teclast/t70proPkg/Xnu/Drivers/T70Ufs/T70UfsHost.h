/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_HOST_H_
#define T70_UFS_HOST_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "T70UfsReadOnly.h"

#define T70_UFS_HANDOFF_BASE       0x43F7F000ULL
#define T70_UFS_HANDOFF_MAGIC      0x0000000148303754ULL
#define T70_UFS_CONTROLLER_BASE    0x11270000U
#define T70_UFS_CONTROLLER_SIZE    0x2300U
#define T70_UFS_HANDOFF_FORMAT     1U
#define T70_UFS_HANDOFF_FDT_MAX   (1024U * 1024U)
#define T70_UFS_FDT_HEADER_BYTES   40U

typedef enum {
  T70UfsFdtNodeInvalid = -1,
  T70UfsFdtNodeDisabled = 0,
  T70UfsFdtNodeEnabled = 1
} T70_UFS_FDT_NODE_STATUS;

typedef struct {
  /* Raw fixed-address firmware handoff fields, copied by the provider. */
  uint64_t HandoffAddress;
  uint64_t Magic;
  uint64_t FdtAddress;
  uint64_t FdtSize;
  uint64_t FdtStatus;
  int32_t FdtNodeStatus;
  uint32_t HandoffReady;
  uint32_t HandoffMmioBase;
  uint32_t HandoffLun;
  uint32_t HandoffFormat;
  /* Set only after the runtime FDT parser validates compatible/reg/ranges. */
  uint32_t FdtControllerBase;
  uint32_t FdtControllerSize;
} T70_UFS_HANDOFF_SNAPSHOT;

typedef enum {
  T70UfsHostSuccess = 0,
  T70UfsHostInvalidArgument,
  T70UfsHostInvalidHandoff,
  T70UfsHostAttachFailed,
  T70UfsHostNopFailed,
  T70UfsHostCapacityFailed,
  T70UfsHostGeometryMismatch,
  T70UfsHostStopFailed,
  T70UfsHostDmaOwnershipUncertain
} T70_UFS_HOST_STATUS;

/*
 * Must be initialized once before Start. The caller serializes every operation
 * on a host. Dma remains owned by the transport until Stop succeeds; if
 * DmaQuarantined is set, the provider must retain it and must not reuse it.
 */
typedef struct {
  MTK_UFS_TRANSPORT Transport;
  uint64_t LastLogicalBlock;
  uint32_t BlockSize;
  uint32_t Signature;
  uint8_t Ready;
  uint8_t DmaQuarantined;
  uint8_t StopSucceeded;
  uint8_t Initialized;
} T70_UFS_HOST;

/* Zero the host state. Does not access hardware. */
void
T70UfsHostInitialize(
  T70_UFS_HOST *Host
  );

/*
 * Validates the retained-link proof and all handoff/FDT fields before invoking
 * any MMIO callback. After that gate, attaches the existing read-only transport,
 * polls a bounded NOP, reads LU2 capacity, and publishes Ready only for the
 * existing 4 KiB / MTK_UFS_USER_BYTES policy geometry.
 *
 * Dma must be uncached, physically contiguous, and retained by the caller
 * until HostStop succeeds. Serialize the whole start and all later operations.
 */
T70_UFS_HOST_STATUS
T70UfsHostStart(
  T70_UFS_HOST *Host,
  const T70_UFS_HANDOFF_SNAPSHOT *Snapshot,
  uint64_t MappedMmioBase,
  const MTK_UFS_TRANSPORT_IO *Io,
  volatile void *Dma,
  size_t DmaBytes,
  uint64_t DmaPhysical
  );

/* Read through the fixed read-only LU2 adapter using the validated geometry. */
T70_UFS_READ_STATUS
T70UfsHostReadBlocks(
  T70_UFS_HOST *Host,
  uint64_t LogicalBlock,
  uint64_t BlockCount,
  void *Buffer,
  size_t BufferBytes
  );

/*
 * Stops the controller and records the real transport result. DMA may be
 * released only when this returns T70UfsHostSuccess, StopSucceeded is set,
 * and DmaQuarantined is clear.
 */
T70_UFS_HOST_STATUS
T70UfsHostStop(
  T70_UFS_HOST *Host
  );

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
