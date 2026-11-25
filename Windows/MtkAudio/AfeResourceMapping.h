// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_RESOURCE_MAPPING_H
#define T70_AFE_RESOURCE_MAPPING_H
#include <portcls.h>
extern "C" {
#include "AfeMapping.h"
}
struct T70_AFE_RESOURCE_MAPPING {
 ULONG MemoryIndex; // Explicit index chosen by actual resource owner, not discovery.
 PRESOURCELIST Resources;
 T70_AFE_MAPPING_OWNER Afe;
};
/* Set MemoryIndex once; zero all other fields. Stable nonpaged owner, caller
 * is disjoint from Resources/Port and their storage; caller
 * serializes all initialization/use/release and excludes resource reassignment,
 * device removal and unload. Resource COM retention alone does NOT do that.
 * Actual already-assigned startup resources required. No inferred IRQ/DMA/cache
 * contract. Exact translated AND raw memory tuple, normal read/write flags only.
 * Mapping is NOCACHE via existing owner; caller excludes incompatible aliases.
 * No power/readiness or MMIO access is implied. Complete active I/O before End.
 * This function matches the miniport provider Initialize callback. Create must
 * still be a separate real stream provider, never a fabricated success.
 * Caller pins this context and performs release/rundown before its owner dies.
 */
NTSTATUS T70AfeResourceMappingInitialize(void *Context,PUNKNOWN Adapter,
 PRESOURCELIST Resources,PPORTWAVERT Port);
NTSTATUS T70AfeResourceMappingRelease(T70_AFE_RESOURCE_MAPPING *Owner);
#endif
