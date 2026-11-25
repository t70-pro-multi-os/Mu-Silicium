// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_IRQ_FIELDS_H
#define T70_AFE_IRQ_FIELDS_H
#include "AfeIrqCount.h"
#include "AfeFormatUpdate.h"
typedef struct {uint32_t Id,CountOffset,CountShift,CountMask,RateOffset,RateShift,RateMask;} T70_AFE_IRQ_DESCRIPTOR;
typedef enum {T70AfeIrqFieldsInvalid,T70AfeIrqFieldsReadFailed,T70AfeIrqFieldsWriteFailed,T70AfeIrqFieldsCompleted} T70_AFE_IRQ_FIELDS_CODE;
typedef struct {T70_AFE_IRQ_FIELDS_CODE Code;int32_t TransportStatus;uint32_t FailedField,ReadsAttempted,WritesAttempted,WritesCompleted;} T70_AFE_IRQ_FIELDS_RESULT;
/* Exact stock descriptor whitelist, not an IRQ allocator/ownership proof.
 * Caller must own the assigned IRQ and serialize shared-register RMWs across
 * ALL users, with quiescent DMA and valid ordered provider access. This call
 * does not establish those facts. Count then rate only; no enable/ack/start.
 * Stock trigger enables memif BEFORE this subset: this is NOT that trigger or
 * evidence that a pre-start configuration sequence is hardware validated.
 * Zero callback result means complete success. First error stops; failed write
 * may have applied, completed writes remain. No rollback/retry/readback claim.
 * Inputs immutable, context alive/serialized, no retained pointers/allocation.
 * Invalid snapshot/descriptor/callback causes zero I/O. */
T70_AFE_IRQ_FIELDS_RESULT T70AfeDl1UpdateIrqFields(const T70_AFE_PREPARED *Prepared,
 const T70_AFE_IRQ_DESCRIPTOR *Descriptor,const T70_AFE_FORMAT_IO *Io);
#endif
