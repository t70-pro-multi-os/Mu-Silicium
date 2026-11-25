/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_VM_H
#define T70_MALI_VM_H
#include "Allocation.h"
#define MALI_VM_TABLE_LIMIT 4096u
#define MALI_VM_HOLD_LIMIT 33u
/* Parent must establish direct system-physical addressing for this GPU. This
   enum is not evidence that MT6789/Windows IOMMU ownership was verified. */
typedef enum {MaliVmAddressingUnknown=0,MaliVmDirectSystemPhysical=1} MALI_VM_ADDRESS_MODEL;
struct MALI_VM_TABLE;
typedef struct MALI_VM_HOLD {
    struct MALI_VM_HOLD *Next;
    uint64_t Owner, Root;
    uint32_t Live;
} MALI_VM_HOLD;
typedef struct MALI_VM {
    uint32_t Signature, TableCount, HoldCount;
    MALI_ALLOCATION_BOOK *Book;
    struct MALI_VM_TABLE *Tables;
    MALI_VM_HOLD *Holds;
} MALI_VM;
typedef struct {
    HANDLE Allocation;
    uint64_t AllocationKey, AllocationOffset, Physical;
    uint32_t Flags;
} MALI_VM_PAGE;
/* PASSIVE, caller holds the DDI gate. Mirrors only complete successful OS CPU
   page-table updates; never maps/dereferences arbitrary physical addresses.
   Physical identity is obtained from the OS-provided resident table CPU VA.
   The parent must advertise ExplicitPageTableInvalidation and CPU_VIRTUAL.
   A hold freezes updates/eviction until the caller releases that submission. */
NTSTATUS MtkMaliVmInitialize(MALI_VM *,MALI_ALLOCATION_BOOK *,MALI_VM_ADDRESS_MODEL);
int MtkMaliVmUsesBook(const MALI_VM *,const MALI_ALLOCATION_BOOK *);
NTSTATUS MtkMaliVmShutdown(MALI_VM *);
NTSTATUS MtkMaliVmUpdate(MALI_VM *,MALI_WDDM_BRIDGE *,uint64_t Owner,DXGKARG_BUILDPAGINGBUFFER *);
NTSTATUS MtkMaliVmForgetProcess(MALI_VM *,uint64_t Owner);
NTSTATUS MtkMaliVmHold(MALI_VM *,MALI_VM_HOLD *,uint64_t Owner,uint64_t Root);
NTSTATUS MtkMaliVmDrop(MALI_VM *,MALI_VM_HOLD *);
NTSTATUS MtkMaliVmLookup(MALI_VM *,const MALI_VM_HOLD *,uint64_t GpuVa,MALI_VM_PAGE *);
int MtkMaliVmIsTablePage(const MALI_VM *,uint64_t Physical);
#endif
