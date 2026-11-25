/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_RUNTIME_H
#define T70_MALI_RUNTIME_H
#include "Worker.h"
#include "Residency.h"
/* Parent allocates one NEW, zeroed nonpaged object; never put this on the
   kernel stack. Adapter is first so the actual DDI hAdapter is this address.
   Start/Stop are serialized PASSIVE PnP operations. Before Start, parent has
   exclusively acquired/powered the board, mapped its GPU and started Core.
   Managed Core interrupts connect only after the worker is running. Parent
   must route JOB/MMU/GPU through the ISR and supply the three actual OS
   translated interrupt descriptors. The DDI derives a common locked DIRQL.
   Retain live nonpaged MMIO/power until OS ISR/DPC rundown after Stop.
   AddressModel is a separately proven platform property, never inferred here.
   Stop refuses live OS objects or hardware work; parent drains DDI/ISR entry
   sources first. No GPU/board power is removed by this software bundle. */
typedef struct {
    MALI_DDI_ADAPTER Adapter;
    MALI_VM Vm;
    MALI_PREPARE_SERVICE Prepare;
    MALI_RESIDENCY Residency;
    MALI_DDI_WORKER Worker;
    uint32_t Stages,Suspended;
    NTSTATUS StartStatus,CleanupStatus,PowerStatus,PowerCleanupStatus;
} MALI_RUNTIME;
typedef struct {
    MALI_VM_ADDRESS_MODEL AddressModel;
    ULONG InterruptMessage;
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *Interrupts;
    UINT InterruptCount;
    uint64_t TimeoutTicks;
    SIZE_T ApertureCommitLimit;
} MALI_RUNTIME_CONFIG;
/* A failed Start attempts full cleanup. Stages != 0 means the caller MUST
   retain this object/Core/callbacks and retry Stop, even though Start failed.
   Success exposes the real preparation/VM/MDL/worker bundle, not DriverEntry,
   PnP resource ownership, board power/IRQ or a user-mode graphics driver. */
NTSTATUS MtkMaliRuntimeStart(MALI_RUNTIME *,MALI_JM *,const DXGKRNL_INTERFACE *,
    const MALI_WDDM_CACHE_OPS *,const MALI_RUNTIME_CONFIG *);
/* Success: all software references/threads drained and Stages == 0. A failed
   stop preserves every unfinished stage for retry. Does not assert GPU reset
   or turn off physical power; parent owns those transitions. */
NTSTATUS MtkMaliRuntimeStop(MALI_RUNTIME *);
/* Non-destructive, serialized PASSIVE power path. Suspend refuses outstanding
   work, masks/disconnects IRQs, drains the worker, flushes/stops Core, retaining
   every process/device/context/allocation/VM/interface. Success permits the
   parent to perform its separately validated board power-down and OS callback
   rundown. On failure retain the bundle, board lease and MMIO; Suspended may
   already be set with admissions blocked. Never use Stop as Suspend.
   Before Resume the parent restores its exclusive board/cache/IRQ resources.
   Core identity, worker and IRQ restore precede admission. A failed restore
   attempts non-destructive cleanup; PowerCleanupStatus records that result.
   This does not itself implement physical board power, miniport D3 or wake. */
NTSTATUS MtkMaliRuntimeSuspend(MALI_RUNTIME *);
NTSTATUS MtkMaliRuntimeResume(MALI_RUNTIME *);
#endif
