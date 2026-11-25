/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_DPMAIF_LAYOUT_H
#define T70_DPMAIF_LAYOUT_H

#include <stdint.h>
#include "DpmaifHwCore.h"

typedef struct {
  uint32_t PitEntries;
  uint32_t RequestBatEntries;
  uint32_t FragmentBatEntries;
  uint32_t TxDrbEntries[T70_DPMAIF_HW_TX_QUEUES];
  uint8_t FragmentEnabled;
} T70_DPMAIF_QUEUE_COUNTS;

typedef struct {
  uint32_t PitBytes;
  uint32_t RequestBatBytes;
  uint32_t FragmentBatBytes;
  uint32_t TxDrbBytes[T70_DPMAIF_HW_TX_QUEUES];
  uint32_t TotalBytes;
} T70_DPMAIF_QUEUE_MEMORY_SIZES;

typedef struct {
  uint64_t PitAddress;
  uint64_t RequestBatAddress;
  uint64_t FragmentBatAddress;
  uint64_t TxDrbAddress[T70_DPMAIF_HW_TX_QUEUES];
} T70_DPMAIF_QUEUE_ADDRESSES;

typedef struct {
  T70_DPMAIF_QUEUE_CONFIG Queues;
  T70_DPMAIF_QUEUE_MEMORY_SIZES Sizes;
  uint32_t BytesUsed;
} T70_DPMAIF_QUEUE_LAYOUT;

/* Dimensions follow the stock DPMAIF v3 descriptors and T70 kernel defaults. */
T70_DPMAIF_RING_STATUS T70DpmaifQueueMemorySizes(
  const T70_DPMAIF_QUEUE_COUNTS *Counts,
  T70_DPMAIF_QUEUE_MEMORY_SIZES *Sizes
  );

/* Each DMA table is independently allocated, as in the kernel DMA pools.
 * Addresses must be non-overlapping, 64-byte aligned, and inside the 36-bit aperture. */
T70_DPMAIF_RING_STATUS T70DpmaifBuildQueueLayout(
  const T70_DPMAIF_QUEUE_COUNTS *Counts,
  const T70_DPMAIF_QUEUE_ADDRESSES *Addresses,
  T70_DPMAIF_QUEUE_LAYOUT *Layout
  );

#endif
