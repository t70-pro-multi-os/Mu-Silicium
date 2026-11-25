// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_POSITION_H
#define T70_AFE_POSITION_H
#include "AfePrepare.h"
typedef struct {uint32_t ByteOffset,FrameOffset;} T70_AFE_POSITION;
/* Convert an already coherently read full CUR address, without reading MMIO.
 * Revalidates every prepared field. Rejects out-of-range/34-bit overflow,
 * underflow and offsets not divisible by 16 or frame size; never modulo-wraps
 * or rounds down. Strict offset alignment is this API's fail-closed policy,
 * not proof of a hardware base-alignment rule. Legitimate intermediate CUR
 * values may be rejected; suitability for live playback is not established.
 * Base itself maps to offset 0.
 * One observation cannot detect laps or distinguish a wrap from initial zero.
 * Caller ensures CUR halves are coherent and belong to this exact allocation.
 * Input remains immutable, output disjoint, no pointers retained. Output is
 * unchanged on failure. See AFE-POSITION-EVIDENCE.md. */
int T70AfeDl1Position(const T70_AFE_PREPARED *Prepared,uint64_t Current,
                      T70_AFE_POSITION *Position);
/* Separate stock-compatible rounding for an already in-range CUR: rounds the
 * byte offset down to 16 before converting to whole frames. Does NOT round the
 * absolute DMA address. Out-of-range/invalid snapshots still fail unchanged.
 * This reproduces stock arithmetic on the accepted domain, not its unchecked
 * overflow/error-to-zero behavior or an MMIO sampling implementation. */
int T70AfeDl1NormalizedPosition(const T70_AFE_PREPARED *Prepared,uint64_t Current,
                                T70_AFE_POSITION *Position);
#endif
