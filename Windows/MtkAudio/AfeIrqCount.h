// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_IRQ_COUNT_H
#define T70_AFE_IRQ_COUNT_H
#include "AfePrepare.h"
#define T70_AFE_IRQ_COUNT_MAX UINT32_C(0x3ffff)
/* Pure unshifted count encoding: no subtract-one or channel multiplier.
 * Zero/overflow rejected, output unchanged on failure. Not IRQ allocation,
 * register selection or permission to start a stream. */
int T70AfeIrqCountField(uint64_t Frames,uint32_t *Count);
/* Revalidates the prepared snapshot then returns period frames as IRQ count.
 * Input remains immutable and output disjoint for the call. No I/O, pointer
 * retention or live resource ownership check. */
int T70AfeDl1IrqCount(const T70_AFE_PREPARED *Prepared,uint32_t *Count);
#endif
