// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_EXPORT_OWNER_H
#define T70_AUDIO_EXPORT_OWNER_H
#include <portcls.h>
#include "AudioServiceSinkOwner.h"
/* Stable zeroed nonpaged storage owned by Parent. Wrap/Drain callers retain
 * Parent and owner storage through return; proxies each retain Parent until
 * their final Release. Gate counts live proxy OBJECTS, not underlying COM refs.
 * No copying/reset while used. Wrap PASSIVE; proxy methods preserve the original
 * interface IRQL/contracts; final Release/Parent final Release <=DISPATCH safe.
 * Wrap borrows the three real interfaces, returning three independently owned
 * proxy references only on complete success. Inputs/output slots/owner must be
 * valid, disjoint and stable through return. Internal/raw interfaces must never
 * be separately exported. Pointer/adapter getters retain their original borrowed
 * lifetime requirements; retaining a raw pointer after Release is unsupported.
 * Drain atomically closes admission and succeeds only after every exported
 * proxy's final Release (including QueryInterface refs). It does not wait and
 * cannot reopen. NOT hardware STOP, PortCls unregister, service-member rundown,
 * or queued notification drain: perform those separately before freeing state.
 * Service-group calls forward to the ORIGINAL group; IRQ Notify must continue
 * targeting that group, not allocate an unrelated replacement group. */
struct T70_AUDIO_EXPORT_OWNER {volatile LONG Gate,WorkGate,CollectionGate,ReclaimBusy;void *volatile Members;T70_AUDIO_SERVICE_GATE ServiceGate;};
/* Permanently denies new proxy work and Wrap, without invalidating COM refs.
 * Nonblocking PASSIVE: NOT_READY until admitted method forwarding has returned.
 * Status methods return DEVICE_NOT_READY; scalar/pointer getters return zero /
 * null; void methods (including FreeBuffer/Silence/copies) become no-ops. Output
 * arguments remain unchanged. Owner must later perform actual buffer teardown.
 * RemoveMember and CancelDelayedService remain forwarded for PortCls cleanup;
 * they are NOT covered by this work gate. DrainServices separately closes and
 * drains tracked original sink callbacks; CancelDelayedService is not rundown.
 * QI/AddRef/Release continue to work. Existing raw pointer borrowers, original
 * group sink callbacks and hardware/IRQ activity are NOT drained by Quiesce.
 * Keep all underlying state alive until final proxy Drain plus real service,
 * pointer-user and hardware rundown. Never reset/reopen this permanent gate.
 */
/* Temporary shared sink admission, not COM/member removal. PASSIVE lifecycle
 * calls externally serialized with each other and permanent teardown. Pause
 * suppresses new original RequestService calls and returns NOT_READY until
 * admitted calls return; queued PortCls DPCs are not cancelled. AddMember stays
 * available while paused and inherits this same gate. Resume allows even late
 * queued callbacks as CURRENT service work, so current buffers/provider state
 * must already be valid. Permanent Quiesce/Drain seals it and cannot resume. */
NTSTATUS T70AudioExportPauseServices(T70_AUDIO_EXPORT_OWNER *);
NTSTATUS T70AudioExportResumeServices(T70_AUDIO_EXPORT_OWNER *);
NTSTATUS T70AudioExportQuiesce(T70_AUDIO_EXPORT_OWNER *);
// Quiesces proxy work and removes/drains every tracked service sink. Does not
// drain IRQ/DPC/work producers or borrowed pointers, nor stop DMA. All members
// must be registered exclusively through these proxies. Nodes remain owned
// until final proxy Drain; group/sink references are released on successful
// member rundown to avoid a sink→exported-proxy ownership cycle.
NTSTATUS T70AudioExportDrainServices(T70_AUDIO_EXPORT_OWNER *);
NTSTATUS T70AudioExportWrap(T70_AUDIO_EXPORT_OWNER *,PUNKNOWN Parent,
 PMINIPORTWAVECYCLICSTREAM,PDMACHANNEL,PSERVICEGROUP,
 PMINIPORTWAVECYCLICSTREAM *,PDMACHANNEL *,PSERVICEGROUP *);
NTSTATUS T70AudioExportDrain(T70_AUDIO_EXPORT_OWNER *);
#endif
