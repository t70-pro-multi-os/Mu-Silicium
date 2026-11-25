// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_CONFIGURE_MAPPED_H
#define T70_CONFIGURE_MAPPED_H
#include "AfeMapping.h"
#include "AfeConfigure.h"
typedef enum {T70MappedConfigInvalid,T70MappedConfigBeginFailed,
 T70MappedConfigConfigureFailed,T70MappedConfigEndFailed,
 T70MappedConfigCallbacksCompleted} T70_MAPPED_CONFIG_CODE;
typedef struct {
 T70_MAPPED_CONFIG_CODE Code;
 NTSTATUS BeginStatus,EndStatus;
 uint32_t BeginAttempted,ConfigureStarted,EndAttempted,LeaseEnded;
 T70_AFE_CONFIG_RESULT Configure; /* applicable only when ConfigureStarted */
} T70_MAPPED_CONFIG_RESULT;
/* Existing mapping only; no acquire/map/release/unmap is performed. Validate
 * snapshot before Begin; after a successful Begin, attempt End exactly once on
 * every normal configure return, including errors. Exceptions/bus faults are
 * not caught. On End failure report it separately while retaining Configure;
 * caller must resolve the outstanding lease, never assume it was released.
 * Caller externally serializes the ENTIRE wrapper, retains immutable Prepared
 * and Owner mapping and keeps IRQL valid. Callback use cannot mutate ownership.
 * Existing configure quiescence, DMA allocation lifetime and device-access
 * preconditions remain mandatory. No auto retry/rollback, no start/power.
 * Partial writes may remain, even a failing write may apply. LeaseEnded is a
 * software bookkeeping fact, not proof DMA stopped or hardware rollback.
 * CallbacksCompleted means only callbacks and End returned success. It is NOT
 * device-ready or permission to retry a previous partially applied configure.
 * Prepared and Owner must be disjoint ordinary resident objects. */
T70_MAPPED_CONFIG_RESULT T70AfeConfigureMapped(T70_AFE_MAPPING_OWNER *Owner,
                                             const T70_AFE_PREPARED *Prepared);
#endif
