// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_START_REQUEST_H
#define T70_AFE_START_REQUEST_H
#include "AfeIrqCount.h"
#include "AfeFormatUpdate.h"
typedef struct {uint32_t Valid,Reads,Writes,Completed,Stage;int32_t Status;} T70_AFE_START_RESULT;
/* Stock DL1 normal period-wakeup render trigger subset (not no-period-wakeup
 * mode or firmware-managed conditional memifs): memif SET bit4 -> enable-register observation ->
 * IRQ0 count/rate -> IRQ0 ON. Caller has configured retained DMA addresses,
 * prefilled/cache-visible data, valid clocks/codec/route, exclusive domain/IRQ0,
 * connected service, IRQ0 initially masked, and actual recovery/quiescence providers. Entire sequence
 * must be synchronized against ISR and other RMWs; no callbacks may sleep.
 * No power/handoff/idle proof is provided here. Alias readback outside 0/0x10
 * is rejected rather than replaying unknown bits. First error stops; failed
 * write may take effect. Completed means callbacks only, NOT working playback.
 * No rollback, automatic retry, buffer release or RUN-state publication. */
T70_AFE_START_RESULT T70AfeDl1RequestStart(const T70_AFE_PREPARED *,const T70_AFE_FORMAT_IO *);
#endif
