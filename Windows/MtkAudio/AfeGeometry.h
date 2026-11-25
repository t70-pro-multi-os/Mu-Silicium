// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_GEOMETRY_H
#define T70_AFE_GEOMETRY_H
#include <stdint.h>
#define T70_AFE_DMA_MAX UINT64_C(0x3ffffffff)
typedef struct {
  uint64_t EndInclusive;
  uint32_t BaseLow,BaseHigh,EndLow,EndHigh;
  uint32_t BufferFrames,PeriodFrames,Periods;
} T70_AFE_GEOMETRY;
/* Pure arithmetic for the observed stock runtime limits, not DMA setup or
 * hardware approval. FrameBytes is caller-supplied storage width, not proof of
 * a supported format/channel count. Base is a device DMA address, not a CPU
 * pointer. No base-address alignment requirement is inferred from length step.
 * Allocation, address translation, alignment, cache ownership, memif routing,
 * format/rate and live state still require separate validation. No I/O.
 * Output unchanged on any failure. See AFE-GEOMETRY-EVIDENCE.md. */
int T70AfeBufferGeometry(uint64_t Base,uint64_t BufferBytes,uint64_t PeriodBytes,
                         uint32_t FrameBytes,T70_AFE_GEOMETRY *Geometry);
#endif
