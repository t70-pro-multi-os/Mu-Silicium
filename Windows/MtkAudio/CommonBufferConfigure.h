// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_COMMON_BUFFER_CONFIGURE_H
#define T70_COMMON_BUFFER_CONFIGURE_H
#include "AfeCommonBuffer.h"
#include "ConfigureMapped.h"
typedef enum {T70BufferConfigInvalid,T70BufferConfigBeginFailed,
 T70BufferConfigNotStarted,T70BufferConfigLeaseRetained} T70_BUFFER_CONFIG_CODE;
typedef struct {
 T70_BUFFER_CONFIG_CODE Code;
 NTSTATUS BufferBeginStatus,BufferEndStatus;
 uint32_t BufferBeginAttempted,BufferEndAttempted,BufferLeaseHeld,MappedCalled;
 T70_MAPPED_CONFIG_RESULT Mapped;
} T70_BUFFER_CONFIG_RESULT;
/* Existing explicit owners only. No allocation or CPU buffer access. Require
 * PASSIVE_LEVEL, live retained adapter/mapping and externally serialized,
 * quiescent/exclusive device throughout call and subsequent failure recovery.
 * Buffer must have no leases before entry; snapshot must match allocated logical
 * address and length. Owners reside in disjoint ordinary resident memory.
 * Once configure starts, retain the buffer lease EVEN ON FAILURE: attempted
 * address writes may leave hardware referencing it. Caller may End that lease
 * only after separately proving quiescence and removing/replacing any hardware
 * references. Never infer that from this result. Release remains blocked until
 * End; no automatic retry/rollback/free, DMA start or cache operations.
 * If configure never starts, attempt End and report an unresolved lease if it
 * fails. Mapping End failures remain visible in Mapped and require separate
 * recovery. All exceptions/bus faults remain outside this normal-return API.
 * LeaseRetained is not success: inspect Mapped for precise partial operations.
 * It does not authorize using Device Memory with ordinary PCM ring helpers. */
T70_BUFFER_CONFIG_RESULT T70AfeConfigureCommonBufferMapped(
 T70_AFE_COMMON_BUFFER *Buffer,T70_AFE_MAPPING_OWNER *Mapping);
#endif
