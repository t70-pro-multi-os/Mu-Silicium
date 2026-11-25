/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_WORKER_H
#define T70_MALI_WORKER_H
#include <ntifs.h>
#include "Ddi.h"
typedef struct {
    MALI_DDI_ADAPTER *Adapter;
    KEVENT Signal;
    EX_RUNDOWN_REF Calls;
    HANDLE Thread;
    LARGE_INTEGER Frequency;
    volatile LONG Initialized, StopRequested, LastStatus;
} MALI_DDI_WORKER;
/* Caller-zeroed nonpaged storage. Start/Stop are caller-serialized PASSIVE
   operations. DDI TimeoutTicks and Pump use KeQueryPerformanceCounter units.
   Device interrupts must already be connected before powering the JM core. */
NTSTATUS MtkMaliWorkerStart(MALI_DDI_WORKER *Worker, MALI_DDI_ADAPTER *Adapter);
/* PASSIVE or DISPATCH; call from the render driver's Wake hook and IRQ DPC,
   never directly from a DIRQL ISR. Coalesced wakes cannot be lost at idle. */
void MtkMaliWorkerWake(MALI_DDI_WORKER *Worker);
/* Joins the actual kernel thread and drains concurrent Wake calls. A failed
   wait preserves the live handle/storage for retry. This does not stop GPU
   DMA or release allocations. Quiesce/reset the GPU before power/removal;
   drain parent ISR/DDI callback sources before freeing Worker/Adapter. */
NTSTATUS MtkMaliWorkerStop(MALI_DDI_WORKER *Worker);
#endif
