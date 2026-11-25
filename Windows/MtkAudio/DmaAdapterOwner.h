// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_DMA_ADAPTER_OWNER_H
#define T70_DMA_ADAPTER_OWNER_H
#include "WaveRtMiniport.h"
#include "WaveRtDmaOwner.h"
struct T70_DMA_ADAPTER_OWNER {
 T70WaveRtMiniport *Miniport;
 PDEVICE_OBJECT Pdo;
 PDMA_ADAPTER Adapter;
 ULONG MapRegisters,Users;
 bool Closing;
};
struct T70_DMA_ADAPTER_USE {T70_DMA_ADAPTER_OWNER *Owner;};
/* Zeroed, nonpaged, stable, uncopied owner/lease storage. All calls PASSIVE,
 * externally serialized/non-reentrant, with actual OS remove/unload exclusion.
 * Explicit PDO must be the live device corresponding to Miniport; identity is
 * supplied by the platform, never discovered or inferred from a pointer.
 * Open retains PDO and miniport. Object refs do not prove hardware presence.
 * Begin/End leases bracket the ENTIRE DMA buffer/mapping/transfer lifetime.
 * End only after freeing those allocations and all hardware references. Never
 * retain the returned adapter after End. Close also requires real quiescence
 * proof. Failed Close retains resources and blocks new uses; caller must keep
 * owner storage and recovery capability. No destructor/automatic release.
 * A successful Open creates neither cache coherence nor a playback stream.
 * MapRegisters is recorded, not a promise any requested transfer is possible;
 * consumers still validate their actual DMA operations and allocation results.
 * All argument/output storage is valid, disjoint from owner and other inputs.
 */
NTSTATUS T70DmaAdapterOpen(T70_DMA_ADAPTER_OWNER *,T70WaveRtMiniport *,PDEVICE_OBJECT);
NTSTATUS T70DmaAdapterBegin(T70_DMA_ADAPTER_OWNER *,T70_DMA_ADAPTER_USE *,PDMA_ADAPTER *);
NTSTATUS T70DmaAdapterEnd(T70_DMA_ADAPTER_OWNER *,T70_DMA_ADAPTER_USE *);
NTSTATUS T70DmaAdapterClose(T70_DMA_ADAPTER_OWNER *,NTSTATUS (*ConfirmQuiesced)(void *),void *);
/* Actual MDL mapping bound to the adapter lease. Externally serialized with
 * adapter and backing owners. Keep this storage alive until Release succeeds.
 * Mapping can be passed to the existing DL1 configure path, whose ActiveUses
 * must remain held after any register application until independent recovery.
 * Acquire failure can retain BOTH leases: inspect and retry Release, never zero.
 * Release requires the same independently proven no-device-reference condition
 * as T70WaveRtDmaRelease. It does not establish that condition itself. */
struct T70_WAVERT_OWNED_DMA {
 T70_DMA_ADAPTER_USE AdapterUse;
 T70_WAVERT_DMA_OWNER Mapping;
};
NTSTATUS T70WaveRtOwnedDmaAcquire(T70_WAVERT_OWNED_DMA *,T70_WAVERT_MDL_OWNER *,T70_DMA_ADAPTER_OWNER *);
NTSTATUS T70WaveRtOwnedDmaRelease(T70_WAVERT_OWNED_DMA *);
#endif
