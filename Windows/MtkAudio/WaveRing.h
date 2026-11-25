// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVE_RING_H
#define T70_WAVE_RING_H
#include "PcmRingCopy.h"
/* Strict 40-byte extensible PCM frontend for ordinary CPU buffers. Selects
 * direct S16 copy or explicit Windows24/32 normalization solely from a valid
 * descriptor matching Prepared. No format guessing/fallback or S32 support.
 * Six caller ranges must be disjoint; scratch is unused for S16 and may be
 * NULL/0. Supplied scratch is nevertheless range/overlap checked. For S24 it
 * must cover Bytes. Failure preserves ring and Progress; scratch disposable.
 * Caller provides valid immutable descriptor/snapshot/source and exclusive
 * access to scratch/ring/progress throughout call and metadata consumption.
 * AvailableBytes is caller-authoritative, not inferred DMA free space.
 * No retained pointers: scratch may be reused after return. No actual endpoint
 * consumer is installed by this optional API, nor DMA/cache/start operation.
 * Returns 1/0. Zero-byte requests still validate format and ring position. */
int T70WaveRingCopy(const void *Descriptor,size_t DescriptorBytes,
 const T70_AFE_PREPARED *Prepared,const void *Source,size_t Bytes,
 void *Scratch,size_t ScratchCapacity,void *Ring,size_t RingCapacity,
 size_t WriteOffset,size_t AvailableBytes,T70_PCM_RING_PROGRESS *Progress);
#endif
