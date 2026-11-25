// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_MAPPING_H
#define T70_AFE_MAPPING_H
#include "AfeMappedIo.h"
typedef struct {
 T70_AFE_MAPPED_IO Mapping;
 uint32_t ActiveUses;
} T70_AFE_MAPPING_OWNER;
/* Owner must start all-zero, remain at a stable address and never be copied or
 * modified by callers while owned. Caller supplies an ALREADY ASSIGNED,
 * translated physical memory resource, not a guessed address from a DT alone.
 * No discovery/resource assignment is performed or claimed. Only the exact
 * board tuple is accepted; maps READWRITE|NOCACHE, never executable/combined.
 * Caller must ensure there are no incompatible cache aliases or other owners.
 * Caller serializes ALL these methods and mapping use (not internally locked).
 * Acquire does not make the device powered/access-ready. IRQL <= DISPATCH_LEVEL
 * and resident owner/code are required. No actual MMIO occurs in these methods.
 * Begin/End bracket the ENTIRE configure/RMW/observation, not each callback.
 * While leased, use &Owner.Mapping with AfeMappedIo. Release rejects active
 * leases. Balanced End only after all callbacks return; do not save/use copied
 * callback descriptors after End. No asynchronous drain/cancel guarantee.
 * Release is idempotent when empty; unmaps exactly once after leases end.
 * End without a lease fails. On failed Acquire/Release the owner is preserved.
 * Zero return means API operation completed, never playback readiness. */
NTSTATUS T70AfeMappingAcquire(T70_AFE_MAPPING_OWNER *Owner,uint64_t PhysicalBase,size_t Length);
NTSTATUS T70AfeMappingBegin(T70_AFE_MAPPING_OWNER *Owner);
NTSTATUS T70AfeMappingEnd(T70_AFE_MAPPING_OWNER *Owner);
NTSTATUS T70AfeMappingRelease(T70_AFE_MAPPING_OWNER *Owner);
#endif
