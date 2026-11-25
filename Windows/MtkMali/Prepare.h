/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_PREPARE_H
#define T70_MALI_PREPARE_H
#include "Ddi.h"
#include "Commands.h"
/* These are real parent render-KMD/VidMm integration points, never defaults.
   Begin retains the exact process/context/root and ALL page-table/mapping
   lifetimes under the parent's VM lock. CopyDma copies the submitted GPUVA
   range from those resident pages into a KMD-owned snapshot, without probing
   arbitrary physical/user pointers. ReserveShadow creates a separate private
   GPU mapping with exclusive backing per live token and no user-writable alias,
   returning its physical execution root in the final output argument.
   PinWrite checks allocation ownership,
   bounds/protection, physical aliases and residency and retains each target.
   Seal finishes cache maintenance for commands, targets and tables, protects
   the shadow against CPU/user aliases and orders stores. Compute containment
   uses the private MMU root; job/status pages remain GPU-writable.
   GPU's own job-status writes remain permitted. End frees/zeros the shadow and
   snapshot backing, finishes GPU-to-CPU target cache maintenance and drops refs
   only on rejection, completion, preemption or
   observed reset; a failed Begin returning a token must also be unwound.
   AcquireHandleData alone does NOT establish residency. No callbacks reenter
   DDI or wait for this GPU under its Gate. PASSIVE, caller-serialized except Wake. */
typedef struct {
    void *Context;
    NTSTATUS (*Begin)(void *,HANDLE,uint64_t,uint64_t,uint64_t,uint32_t,void **);
    NTSTATUS (*CopyDma)(void *,void *,void *,uint32_t);
    NTSTATUS (*ReserveShadow)(void *,void *,uint32_t,void **,uint64_t *,uint64_t *);
    NTSTATUS (*PinWrite)(void *,void *,uint64_t,uint32_t);
    NTSTATUS (*Seal)(void *,void *,const void *,uint32_t);
    void (*End)(void *,void *,MALI_DDI_RELEASE_REASON);
    void (*Wake)(void *);
    NTSTATUS (*PinResource)(void *,void *,uint64_t,uint32_t,uint32_t);
} MALI_PREPARE_OPS;
typedef struct { void *Token; uint32_t Live; } MALI_PREPARE_LEASE;
typedef struct {
    uint32_t Signature;
    MALI_JM *Core;
    MALI_PREPARE_OPS Ops;
    MALI_PREPARE_LEASE Leases[MALI_DDI_RESIDENTS];
    uint8_t Input[MALI_COMMAND_MAX_DMA], Private[MALI_COMMAND_MAX_PRIVATE];
    MALI_COMMAND_WORKSPACE Work;
    void *CurrentToken;
    NTSTATUS PinStatus, LastReleaseStatus;
} MALI_PREPARE_SERVICE;
/* New caller-owned nonpaged storage; do not reinitialize a live service.
   Pass the returned Hooks directly to MtkMaliDdiInitialize, and route Wake to
   MtkMaliWorkerWake in the parent. This implements bounded command preparation,
   not the parent allocation/VM callbacks or a general shader validator. */
NTSTATUS MtkMaliPrepareInitialize(MALI_PREPARE_SERVICE *,MALI_JM *,const MALI_PREPARE_OPS *,MALI_DDI_HOOKS *);
NTSTATUS MtkMaliPrepareShutdown(MALI_PREPARE_SERVICE *);
#endif
