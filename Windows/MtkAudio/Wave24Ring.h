// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVE24_RING_H
#define T70_WAVE24_RING_H
#include "PcmRingCopy.h"
/* Ordinary CPU memory only. Prepared must be a validated S24Le32 snapshot and
 * Descriptor the matching strict Windows 24/32 PCM descriptor. No S16 fallback.
 * All six caller storage ranges (descriptor, snapshot, source, full scratch,
 * full ring, progress) must be disjoint. ScratchCapacity >= Bytes; each frame
 * remains four bytes per channel. Whole-input validation precedes scratch writes.
 * Failure preserves ring and Progress. Scratch is disposable workspace; it may
 * contain converted data on a later failure. Success publishes Progress only
 * after copying, not atomically with concurrent consumers.
 * Caller owns valid memory and exclusive serialization of the entire operation,
 * immutable descriptor/snapshot/source, authoritative AvailableBytes and scratch
 * lifetime through return. No pointers retained; scratch can then be reused.
 * No mapping, DMA ownership, cache operation, hardware or playback readiness.
 * Returns 1/0; zero bytes still validates descriptor, geometry and ring offset. */
int T70Wave24RingCopy(const void *Descriptor,size_t DescriptorBytes,
 const T70_AFE_PREPARED *Prepared,const void *Source,size_t Bytes,
 void *Scratch,size_t ScratchCapacity,void *Ring,size_t RingCapacity,
 size_t WriteOffset,size_t AvailableBytes,T70_PCM_RING_PROGRESS *Progress);
#endif
