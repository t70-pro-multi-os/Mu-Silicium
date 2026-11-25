// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_PCM_RING_COPY_H
#define T70_PCM_RING_COPY_H
#include <stddef.h>
#include "AfePrepare.h"
typedef struct {
 size_t NextWriteOffset,BytesCopied,FramesCopied;
} T70_PCM_RING_PROGRESS;
/* Copies at most one logical ring's worth of frame-aligned PCM bytes.
 * AvailableBytes is an authoritative caller-supplied free-space snapshot, NOT
 * inferred from CUR, a read index or DMA. It must be frame-aligned and <= ring
 * size. No overwrite/drop/wait policy is invented. WriteOffset must be within
 * the logical ring even for an empty copy. RingCapacity covers the allocation;
 * only Prepared.BufferBytes bytes form the logical ring. Trailing storage is
 * untouched. Source must cover Bytes bytes; NULL is allowed only for Bytes=0.
 * Source must not overlap the logical ring; Progress must not overlap the entire RingCapacity allocation,
 * source or Prepared. Prepared must not overlap the logical ring.
 * All checks precede copying. A rejected call leaves ring and Progress unchanged.
 * Success publishes Progress after the copy; this is NOT atomic concurrency
 * publication. Caller owns valid ordinary CPU memory, immutable Prepared/source,
 * lifetimes and exclusive serialization through copy and metadata consumption.
 * This is not an MMIO accessor, DMA mapping/cache flush or a live-DMA ownership
 * protocol. Invalid actual pointers cannot be detected. No retained pointers,
 * allocations, DMA operations or format conversion. Returns 1/0 success/error. */
int T70PcmRingCopy(const T70_AFE_PREPARED *Prepared,void *Ring,size_t RingCapacity,
 size_t WriteOffset,size_t AvailableBytes,const void *Source,size_t Bytes,
 T70_PCM_RING_PROGRESS *Progress);
#endif
