// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVECYCLIC_SERVICE_H
#define T70_WAVECYCLIC_SERVICE_H
#include <portcls.h>
struct T70_WAVECYCLIC_SERVICE {
 IPortWaveCyclic *Port;
 PSERVICEGROUP Group;
 volatile LONG Gate;
 bool Attempted;
};
/* Stable zeroed nonpaged storage, all lifetime operations PASSIVE and serialized.
 * Acquire creates the actual PortCls service group, retaining the explicit port.
 * The same Group must be returned with the matching real stream (independent
 * AddRef for that output), not a synthetic group for unrelated IRQs.
 * NotifyConfirmed is ISR-safe: caller has already identified an OWNED real DMA
 * progress interrupt. Device-specific acknowledge ordering is the ISR's
 * responsibility (MT6789 stock notifies before clearing). No register
 * status, ack, elapsed periods, timer, or DMA position is inferred here.
 * This file is NOT an ISR and must not be called from a periodic mock timer.
 * Gate prevents Close racing an active Notify. Caller keeps owner storage alive
 * until ISR disconnected and all contenders drained; gate is not image rundown.
 * Close seals admission, then requires exact successful confirmation that ISR,
 * downstream queued service work and all exported Group users are detached.
 * It retains references on failed/in-progress proof. Never call Close in ISR.
 * NotifyConfirmed true means submission OR intentional paused suppression, not
 * completion of queued service. False denotes closing/invalid/overflow state.
 * Pause stops new submissions and waits by retry for already-entered Notify
 * calls only. It does NOT drain already queued PortCls service callbacks.
 * Resume reopens this software admission only; the caller separately establishes
 * actual hardware/service readiness before invoking it. Pause/Resume/Close are
 * PASSIVE lifecycle operations serialized by the integrating owner. */
NTSTATUS T70WaveCyclicServiceAcquire(T70_WAVECYCLIC_SERVICE *,IPortWaveCyclic *);
bool T70WaveCyclicServiceNotifyConfirmed(T70_WAVECYCLIC_SERVICE *);
NTSTATUS T70WaveCyclicServicePause(T70_WAVECYCLIC_SERVICE *);
NTSTATUS T70WaveCyclicServiceResume(T70_WAVECYCLIC_SERVICE *);
NTSTATUS T70WaveCyclicServiceClose(T70_WAVECYCLIC_SERVICE *,NTSTATUS (*ConfirmDetached)(void *),void *);
#endif
