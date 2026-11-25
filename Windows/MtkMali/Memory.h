/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_MEMORY_H
#define T70_MALI_MEMORY_H
#include <ntddk.h>
#include "Wddm.h"
#define MALI_WDK_MAX_ALLOCATION (UINT64_C(1) << 30)
typedef struct {
    PMDL Mdl;
    void *Cpu;
    size_t Bytes;
    uint64_t Fences[MALI_JM_MAX_SLOTS];
    uint32_t Pins, GpuDirty, Quarantined;
} MALI_WDK_MEMORY;
/* Caller serializes allocation and every pin/retirement. Initialize to zero.
   CPU mappings are noncached, nonexecutable, private to this kernel owner.
   Do not access Cpu while pinned. Every submitted BO, including job chains and
   page tables, must stay pinned until completion or observed reset abortion.
   Uses direct physical 40-bit GPU addresses, not Windows DMA remapping. The
   future render miniport must establish that platform address model first. */
MALI_RESULT MtkMaliMemoryAllocate(MALI_WDK_MEMORY *Memory, size_t Bytes);
MALI_RESULT MtkMaliMemoryPhysical(const MALI_WDK_MEMORY *Memory, size_t Page, uint64_t *Pa);
MALI_RESULT MtkMaliMemoryPin(MALI_WDK_MEMORY *Memory, uint32_t Slot, uint64_t Fence, int GpuWrites);
#define MALI_WDK_MAX_JOB_BUFFERS 64u
typedef struct { MALI_WDK_MEMORY *Memory; int GpuWrites; } MALI_WDK_RESIDENT;
/* Submit only trusted, kernel-built job descriptors. The caller supplies every
   resident root/table/chain/shader/data BO, validates GPUVA ownership and holds
   the device lock. This is not a user IOCTL or untrusted command validator.
   Pins are committed only if MaliJmSubmit starts the job; every failed submit
   unwinds only the pins established by this call. Retire the same resident list
   using the matching real completion/reset events. */
MALI_RESULT MtkMaliMemorySubmit(MALI_JM *Device, uint32_t Slot, uint32_t As,
                               uint64_t Chain, uint64_t Affinity, uint64_t Fence,
                               const MALI_WDK_RESIDENT *Buffers, uint32_t Count);
/* Only feed events actually returned by MaliJmPoll/Reset. Failed jobs retain
   their memory until Reset returns the matching MaliJobAborted event. */
MALI_RESULT MtkMaliMemoryRetire(MALI_WDK_MEMORY *Memory, const MALI_EVENT *Event);
MALI_RESULT MtkMaliMemoryRelease(MALI_WDK_MEMORY *Memory);
/* Update only this owned, idle, single-page table. GPU TLB flush/bind remains
   required before reuse; this call performs real MDL CPU cache maintenance. */
MALI_RESULT MtkMaliMemoryUpdateTable(MALI_WDK_MEMORY *Memory, MALI_WDDM_BRIDGE *Bridge,
                                   const DXGKARG_BUILDPAGINGBUFFER *Build);
#endif
