// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_IRQ_SOURCE_H
#define T70_AFE_IRQ_SOURCE_H
#include "AfeInterruptOwner.h"
struct T70_AFE_IRQ_SOURCE {
 T70_AFE_INTERRUPT_OWNER *Owner;
 void (*ScheduleRecovery)(void *,NTSTATUS);
 void *RecoveryContext;
 NTSTATUS MaskStatus;
 ULONG Completed;
};
/* Stable nonpaged source and owner, externally serialized lifetime. Exclusive
 * whole AFE domain and no firmware/DSP semaphore users are REQUIRED. I/O is the
 * owner's ordered DIRQL-safe provider. No DMA stop, power, or restart is implied.
 * Set Handler.OnFault=T70AfeIrqSourceFault, FaultContext=source before Connect.
 * ScheduleRecovery is an actual preallocated DIRQL-safe scheduler supplied by
 * the parent; it must NOT close inline, and waits for Connect to finish. This
 * module masks synchronously, but does not fabricate a worker/rundown provider.
 * MaskStatus/Completed are read only after ISR synchronization/disconnection.
 * Close callback runs at PASSIVE and synchronizes with the connected ISR.
 * Failed I/O can have partial effects; retain references, never infer quiescence.
 */
NTSTATUS T70AfeIrqSourceMask(void *);
void T70AfeIrqSourceFault(void *,NTSTATUS);
#endif
