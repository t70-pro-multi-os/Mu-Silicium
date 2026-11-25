// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_FORMAT_H
#define T70_AFE_FORMAT_H
#include <stdint.h>
/* Portable IDs, not ALSA enum numbers. S24 uses a 32-bit storage container. */
typedef enum {T70AfeS16Le=1,T70AfeS24Le32=2,T70AfeS32Le=3} T70_AFE_SAMPLE_FORMAT;
typedef struct {
  uint32_t RateCode,HdMode,AlignmentCode,Mono,SampleBytes,FrameBytes;
} T70_AFE_FORMAT;
/* DL1 (stock memif ID 0) only. Rejects unknown rate/format/channel combinations,
 * including S32 until its runtime-dependent policy is established. Produces
 * unshifted field values, not a register image/write plan. No MMIO or DMA.
 * Advertised frontend capability is NOT an end-to-end speaker route guarantee.
 * Output is unchanged on failure. See AFE-FORMAT-EVIDENCE.md. */
int T70AfeDl1Format(uint32_t Rate,uint32_t Format,uint32_t Channels,
                    T70_AFE_FORMAT *Result);
#endif
