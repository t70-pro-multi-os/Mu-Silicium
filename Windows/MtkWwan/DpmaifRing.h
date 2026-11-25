/* SPDX-License-Identifier: GPL-2.0-only */
/* Stock DPMAIF v3 ring index contracts, separated from DMA/NDIS ownership. */
#ifndef T70_DPMAIF_RING_H
#define T70_DPMAIF_RING_H

#include <stdint.h>

typedef enum {
  T70DpmaifRingSuccess = 0,
  T70DpmaifRingInvalid,
  T70DpmaifRingNoSpace,
  T70DpmaifRingStale
} T70_DPMAIF_RING_STATUS;

typedef struct {
  uint32_t Capacity;
  uint32_t HardwareWriteIndex;
  uint32_t ReadIndex;
  uint32_t ReleaseIndex;
} T70_DPMAIF_RX_RING;

typedef struct {
  uint32_t Capacity;
  uint32_t HardwareReadIndex;
  uint32_t WriteIndex;
  uint32_t ReleaseIndex;
} T70_DPMAIF_TX_RING;

/* Exact one-empty-slot arithmetic used by stock BAT and DRB producers. */
T70_DPMAIF_RING_STATUS T70DpmaifRingReadable(
  uint32_t Capacity, uint32_t ReadIndex, uint32_t WriteIndex, uint32_t *Count);
T70_DPMAIF_RING_STATUS T70DpmaifRingWritable(
  uint32_t Capacity, uint32_t ReleaseIndex, uint32_t WriteIndex, uint32_t *Count);
T70_DPMAIF_RING_STATUS T70DpmaifRingAdvance(
  uint32_t Capacity, uint32_t Index, uint32_t Count, uint32_t *NextIndex);

T70_DPMAIF_RING_STATUS T70DpmaifRxRingInitialize(
  T70_DPMAIF_RX_RING *Ring, uint32_t Capacity);
T70_DPMAIF_RING_STATUS T70DpmaifRxRingObserveHardwareWrite(
  T70_DPMAIF_RX_RING *Ring, uint32_t HardwareWriteIndex, uint32_t *Available);
T70_DPMAIF_RING_STATUS T70DpmaifRxRingAvailable(
  const T70_DPMAIF_RX_RING *Ring, uint32_t *Available);
T70_DPMAIF_RING_STATUS T70DpmaifRxRingConsume(
  T70_DPMAIF_RX_RING *Ring, uint32_t Count, uint32_t *NewReadIndex);
T70_DPMAIF_RING_STATUS T70DpmaifRxRingRelease(
  T70_DPMAIF_RX_RING *Ring, uint32_t Count, uint32_t *NewReleaseIndex);

T70_DPMAIF_RING_STATUS T70DpmaifTxRingInitialize(
  T70_DPMAIF_TX_RING *Ring, uint32_t Capacity);
T70_DPMAIF_RING_STATUS T70DpmaifTxRingFreeSlots(
  const T70_DPMAIF_TX_RING *Ring, uint32_t *FreeSlots);
T70_DPMAIF_RING_STATUS T70DpmaifTxRingReserve(
  const T70_DPMAIF_TX_RING *Ring, uint32_t Count,
  uint32_t *StartIndex, uint32_t *NextIndex);
T70_DPMAIF_RING_STATUS T70DpmaifTxRingCommit(
  T70_DPMAIF_TX_RING *Ring, uint32_t ExpectedStartIndex, uint32_t Count,
  uint32_t *NewWriteIndex);
T70_DPMAIF_RING_STATUS T70DpmaifTxRingObserveHardwareRead(
  T70_DPMAIF_TX_RING *Ring, uint32_t HardwareReadIndex, uint32_t *Completed);
T70_DPMAIF_RING_STATUS T70DpmaifTxRingRelease(
  T70_DPMAIF_TX_RING *Ring, uint32_t Count, uint32_t *NewReleaseIndex);

#endif
