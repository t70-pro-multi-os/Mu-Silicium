// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_CYCLIC_START_H
#define T70_AUDIO_CYCLIC_START_H
#include <portcls.h>
/* Called from the actual PortCls StartDevice. Current physical-access admission
 * remains unavailable, so software ownership is acquired/released without MMIO.
 * No endpoint registration or successful hardware start is claimed. */
struct T70_AUDIO_CYCLIC_DEVICE {
 void *Runtime=nullptr; // owns the initial Runtime reference until Release
 NTSTATUS CleanupStatus=STATUS_SUCCESS;
 bool Removing=false;
 bool SurpriseRemoved=false;
};
// Slot belongs to the real PortCls FDO extension. PnP/Start lifecycle calls are
// serialized by the adapter dispatch path; no copying, external writes or use
// after forwarding REMOVE. Current scope is failed/unpublished pre-MMIO starts.
NTSTATUS T70AudioCyclicStart(T70_AUDIO_CYCLIC_DEVICE *,PDEVICE_OBJECT,PIRP,PRESOURCELIST);
NTSTATUS T70AudioCyclicRelease(T70_AUDIO_CYCLIC_DEVICE *);
// Queries preserve active state on veto so CANCEL can resume unchanged.
NTSTATUS T70AudioCyclicQueryRelease(T70_AUDIO_CYCLIC_DEVICE *);
// No-MMIO surprise path; IRQ removal is not proof of DMA quiescence.
NTSTATUS T70AudioCyclicSurpriseRemove(T70_AUDIO_CYCLIC_DEVICE *);
// Final/surprise removal of the CURRENT unpublished, pre-MMIO runtime only.
// Never force-releases an IRQ/mapping/stream owner. Not an active-DMA remover.
NTSTATUS T70AudioCyclicRemoveUnpublished(T70_AUDIO_CYCLIC_DEVICE *);
// Normal final removal may execute stock STOP; surprise must never do so.
NTSTATUS T70AudioCyclicRemove(T70_AUDIO_CYCLIC_DEVICE *);

// Final REMOVE only, after Remove's normal cleanup attempt. Transfers retained
// runtime to a pre-created OS-pinned thread, or quarantine if hardware/rundown is
// unproved. Success means ownership transfer, NOT completed device cleanup.
// Token consumes the former slot reference. Always call ForwardReturned exactly
// once after PcDispatchIrp returns (including PENDING); it is NOT an IRP completion
// fence. Worker uses no IRP, extension or registration API, only proven CPU state.
NTSTATUS T70AudioCyclicRetire(T70_AUDIO_CYCLIC_DEVICE *,void **Token);
void T70AudioCyclicForwardReturned(void *Token);
#endif
