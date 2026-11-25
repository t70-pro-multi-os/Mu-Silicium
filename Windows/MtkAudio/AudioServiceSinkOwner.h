// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_SERVICE_SINK_OWNER_H
#define T70_AUDIO_SERVICE_SINK_OWNER_H
#include <portcls.h>
/* One explicit group/sink registration per stable zeroed owner. PASSIVE Open /
 * Close, nonblocking serialization: concurrent/reentrant operation returns
 * NOT_READY. Caller pins owner storage through each call. No reset/reopen.
 * Proxy retains Parent and original Sink until its final COM Release, including
 * late group-held references. Parent/sink final Release must be <=DISPATCH safe.
 * Parent pins nonpaged callback code and sink state. Underlying group must obey
 * COM callback lifetime; no raw proxy use without a reference. Do not separately
 * register the same original sink or remove this proxy behind the owner.
 * Close closes callback admission, removes exactly this member once, and waits
 * by retry (never blocking) for admitted original RequestService calls to return.
 * RemoveMember does NOT imply callback rundown. Late callbacks to retained
 * proxy references are no-ops. Success covers ONLY this registered sink: not
 * other members, IRQ/work/timer drain, DMA stop, buffer release or device removal.
 * Informational/pending AddMember results are quarantined (no guessed ownership).
 */
/* Shared across all proxies for one stream/service domain. Stable nonpaged
 * storage pinned by each proxy's Parent. Pause/Resume/Seal are PASSIVE, externally
 * serialized lifecycle operations, nonblocking and retryable. Pause success
 * means no admitted Target::RequestService is still running. Queued or late
 * callbacks are suppressed while paused, but may forward after Resume: this
 * neither cancels opaque PortCls queues nor establishes separate queue epochs.
 * Seal is permanent. Local sink Close remains independently permanent.
 * No hardware stop, IRQ drain, or unrelated registration is covered. */
struct T70_AUDIO_SERVICE_GATE {volatile LONG State;};
NTSTATUS T70AudioServiceGatePause(T70_AUDIO_SERVICE_GATE *);
NTSTATUS T70AudioServiceGateResume(T70_AUDIO_SERVICE_GATE *);
NTSTATUS T70AudioServiceGateSeal(T70_AUDIO_SERVICE_GATE *);
struct T70_AUDIO_SERVICE_SINK_OWNER {
 volatile LONG Busy;
 ULONG State;
 PSERVICEGROUP Group;
 PSERVICESINK Proxy;
};
NTSTATUS T70AudioServiceSinkOpen(T70_AUDIO_SERVICE_SINK_OWNER *,PUNKNOWN Parent,
 PSERVICEGROUP Group,PSERVICESINK Original,T70_AUDIO_SERVICE_GATE *Shared=nullptr);
NTSTATUS T70AudioServiceSinkClose(T70_AUDIO_SERVICE_SINK_OWNER *);
#endif
