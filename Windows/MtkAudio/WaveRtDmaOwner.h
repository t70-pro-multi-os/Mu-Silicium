// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_DMA_OWNER_H
#define T70_WAVERT_DMA_OWNER_H
#include "WaveRtMdlOwner.h"
/* Explicit retained real version-3-capable DMA adapter for THIS device only.
 * Adapter/operations and MDL owner live unchanged through release. PASSIVE,
 * external serialization/non-reentrancy, disjoint owner/provider storage.
 * No inference from DMA_ADAPTER.Version; runtime operations size/pointers gate.
 * Acquire requires no existing backing leases, preventing a second mapping
 * through this owner while another mapping/borrow is live. Bind before publish.
 * Acquire may return an error WITH retained cleanup ownership: always inspect
 * LeaseHeld and call Release when conditions permit. Never zero a live owner.
 * Mapping does not establish cache coherency, DMA idle, hardware configuration,
 * user-map suitability or playback readiness. No hardware access/start occurs.
 * Before Release, caller MUST prove all device references are stopped/removed.
 * Mapped logical range is distinct from MDL CPU physical addresses. Retained MDL
 * lease blocks page release until FreeCommonBuffer bookkeeping is removed. */
struct T70_WAVERT_DMA_OWNER {
 T70_WAVERT_MDL_OWNER *Backing;
 PDMA_ADAPTER Adapter;
 PFREE_COMMON_BUFFER Free;
 T70_WAVERT_MDL_VIEW View;
 PHYSICAL_ADDRESS Logical;
 bool LeaseHeld,Mapped;
 uint32_t ActiveUses;
};
NTSTATUS T70WaveRtDmaAcquire(T70_WAVERT_DMA_OWNER *Owner,T70_WAVERT_MDL_OWNER *Backing,PDMA_ADAPTER Adapter);
/* Hold across any register configuration, including partial failure. End only
 * after independent hardware-reference recovery; it does not itself stop DMA. */
NTSTATUS T70WaveRtDmaBeginUse(T70_WAVERT_DMA_OWNER *Owner);
NTSTATUS T70WaveRtDmaEndUse(T70_WAVERT_DMA_OWNER *Owner);
NTSTATUS T70WaveRtDmaRelease(T70_WAVERT_DMA_OWNER *Owner);
#endif
