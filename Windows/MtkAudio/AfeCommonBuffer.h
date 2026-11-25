// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_COMMON_BUFFER_H
#define T70_AFE_COMMON_BUFFER_H
#include <ntddk.h>
#include "AfePrepare.h"
typedef struct {
 PDMA_ADAPTER Adapter;
 PFREE_COMMON_BUFFER Free;
 void *VirtualAddress;
 PHYSICAL_ADDRESS LogicalAddress;
 ULONG Length;
 uint32_t ActiveUses;
 T70_AFE_PREPARED Prepared;
} T70_AFE_COMMON_BUFFER;
/* Explicit caller-owned adapter obtained for THIS device using version-3 DMA
 * description; retained and immutable through Release. Not auto discovery.
 * Stable zeroed owner, disjoint from adapter/table, never copied/edited while
 * owned. All supplied objects are valid accessible resident memory. PASSIVE_LEVEL only;
 * caller serializes all methods, leases and parent teardown. Begin/End protects
 * only this owner's Release. Caller must separately stop hardware and exclude
 * all users before Release; no DMA stop/cancel/cache operations are performed.
 * Requests CacheEnabled=FALSE and NUMA node 0. On ARM64 this may be Device Memory:
 * do NOT pass VirtualAddress to ordinary memcpy/ring APIs or infer cache type.
 * This owner never reads/writes buffer contents. LogicalAddress, not CPU physical
 * address, feeds Prepared. Success means allocated/validated, NOT playable.
 * Rejected allocation is freed with its exact returned tuple. No PutDmaAdapter.
 */
NTSTATUS T70AfeCommonBufferAcquire(T70_AFE_COMMON_BUFFER *Owner,PDMA_ADAPTER Adapter,
 uint32_t Rate,uint32_t Format,uint32_t Channels,uint64_t Bytes,uint64_t Period);
NTSTATUS T70AfeCommonBufferBegin(T70_AFE_COMMON_BUFFER *Owner);
NTSTATUS T70AfeCommonBufferEnd(T70_AFE_COMMON_BUFFER *Owner);
NTSTATUS T70AfeCommonBufferRelease(T70_AFE_COMMON_BUFFER *Owner);
#endif
