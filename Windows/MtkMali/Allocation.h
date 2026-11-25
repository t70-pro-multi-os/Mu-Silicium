/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_ALLOCATION_H
#define T70_MALI_ALLOCATION_H
#include "Wddm.h"
#include <dispmprt.h>
#include "AllocationAbi.h"
struct MALI_ALLOCATION;
struct MALI_ALLOCATION_RESOURCE;
struct MALI_ALLOCATION_OPEN;
struct MALI_ALLOCATION_REFERENCE;
typedef struct {
    uint32_t Signature, AllocationCount, ResourceCount, OpenCount, ReferenceCount;
    uint32_t ShareBackingEnabled;
    NTSTATUS BackingFeatureStatus;
    uint64_t NextReference, NextAllocation;
    struct MALI_ALLOCATION *Allocations;
    struct MALI_ALLOCATION_RESOURCE *Resources;
    struct MALI_ALLOCATION_OPEN *Opens;
    struct MALI_ALLOCATION_REFERENCE *References;
    DXGKCB_GETHANDLEDATA GetHandleData;
} MALI_ALLOCATION_BOOK;
typedef struct {
    HANDLE Allocation, Device, Process;
    uint64_t AllocationBytes, Offset, Bytes, Key, BackingBytes;
    void *Backing;
    uint32_t CpuVisible, ReadOnly, Write;
} MALI_ALLOCATION_VIEW;
typedef struct {uint64_t Key, Bytes, RoundedBytes;void *Backing;} MALI_ALLOCATION_IDENTITY;
/* Trusted kernel allocation identity; never exposed to UMD. Same address after
   object reuse has a different Key. Caller holds the adapter gate. */
NTSTATUS MtkMaliAllocationIdentity(MALI_ALLOCATION_BOOK *,HANDLE,MALI_ALLOCATION_IDENTITY *);
NTSTATUS MtkMaliAllocationAcquireObject(MALI_ALLOCATION_BOOK *,HANDLE Device,HANDLE Process,
    HANDLE Allocation,uint64_t Key,uint64_t Offset,uint64_t Bytes,BOOLEAN Write,uint64_t *Token);
/* All calls require PASSIVE and the parent's adapter gate. Initialization is
   before exposure to callers, after Dxgkrnl callbacks/feature negotiation exist.
   Adapter-owned allocation/resource metadata deliberately contains no creator
   process/device pointer. VidMm owns physical memory, placement and residency. */
void MtkMaliAllocationInitialize(MALI_ALLOCATION_BOOK *Book, const DXGKRNL_INTERFACE *Dxgk);
NTSTATUS MtkMaliAllocationCreate(MALI_ALLOCATION_BOOK *Book, DXGKARG_CREATEALLOCATION *Args);
NTSTATUS MtkMaliAllocationDestroy(MALI_ALLOCATION_BOOK *Book, const DXGKARG_DESTROYALLOCATION *Args);
NTSTATUS MtkMaliAllocationOpen(MALI_ALLOCATION_BOOK *Book, HANDLE Device, HANDLE Process,
    const DXGKARG_OPENALLOCATION *Args);
NTSTATUS MtkMaliAllocationClose(MALI_ALLOCATION_BOOK *Book, HANDLE Device,
    const DXGKARG_CLOSEALLOCATION *Args);
NTSTATUS MtkMaliAllocationDescribe(MALI_ALLOCATION_BOOK *Book, DXGKARG_DESCRIBEALLOCATION *Args);
NTSTATUS MtkMaliAllocationSetBacking(MALI_ALLOCATION_BOOK *Book,
    const DXGKARG_SETALLOCATIONBACKINGSTORE *Args);
int MtkMaliAllocationHasDevice(const MALI_ALLOCATION_BOOK *Book, HANDLE Device);
int MtkMaliAllocationHasObjects(const MALI_ALLOCATION_BOOK *Book);
/* Resolves the OS's per-device handle via GetHandleData(DeviceSpecific=TRUE),
   checks process/device rights and bounds, then retains the open and allocation.
   This is an OBJECT reference, not a VidMm make-resident or physical-page pin.
   Tokens are monotonic; an old/double-released token cannot release a new lease.
   The VM backend must ALSO hold GPU mappings, residency and physical-alias checks. */
NTSTATUS MtkMaliAllocationAcquire(MALI_ALLOCATION_BOOK *Book, HANDLE Device, HANDLE Process,
    D3DKMT_HANDLE Allocation, uint64_t Offset, uint64_t Bytes, BOOLEAN Write, uint64_t *Token);
NTSTATUS MtkMaliAllocationReferenceView(MALI_ALLOCATION_BOOK *Book, uint64_t Token,
    MALI_ALLOCATION_VIEW *View);
/* Copies CPU-authored shared backing bytes into a private KMD buffer. Caller
   synchronizes producers and CPU/GPU caches. This is not an arbitrary physical
   address mapper and does not establish GPU residency or cache coherence. */
NTSTATUS MtkMaliAllocationCopyBacking(MALI_ALLOCATION_BOOK *Book, uint64_t Token,
    uint64_t Offset, void *Destination, size_t Bytes);
NTSTATUS MtkMaliAllocationRelease(MALI_ALLOCATION_BOOK *Book, uint64_t Token);
#endif
