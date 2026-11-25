// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_CUR32_H
#define T70_AFE_CUR32_H
#include "AfePosition.h"
#include "AfeFormatUpdate.h"
/* Stock CUR-low(0x5c), CUR-high(0x58) order for a retained allocation ENTIRELY
 * below 4GiB. While DMA remains inside this allocation, high cannot change even
 * at ring wrap, so the sampled low word is sufficient; high must read zero.
 * This is NOT a general coherent 34-bit reader. Reject crossing/high buffers
 * before I/O, reject nonzero high and out-of-allocation CUR. No modulo salvage.
 * Caller prevents reset/reconfiguration/allocation changes and owns live ordered
 * resident I/O. ISR serialization does not stop DMA. Output is raw normalized
 * DMA offset, NOT DAC play position, RUN proof, elapsed laps or drain evidence.
 * Every error preserves output. Zero transport status means complete read.
 */
int32_t T70AfeDl1ReadCur32(const T70_AFE_PREPARED *,const T70_AFE_FORMAT_IO *,T70_AFE_POSITION *);
#endif
