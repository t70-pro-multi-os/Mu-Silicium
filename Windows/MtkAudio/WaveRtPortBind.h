// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_PORT_BIND_H
#define T70_WAVERT_PORT_BIND_H
#include <portcls.h>
/* Explicit startup objects, not discovery. Device/Irp/Resources are the actual
 * PortCls startup arguments; Miniport is a real, uninitialized WaveRT miniport.
 * Caller owns valid references and serializes the whole call at PASSIVE_LEVEL.
 * Out is disjoint ordinary storage, not inside any supplied object. On success
 * caller receives one owned port reference and must Release it after its own
 * lifecycle/rundown. On failure Out is preserved and the created port released.
 * Init invokes real miniport code: its failure cleanup must be implemented by
 * that miniport. This helper cannot undo its hardware effects or guarantee retry.
 * No subdevice registration/publication, stream factory, or hardware readiness
 * is implied. Caller must not publish until all concrete providers are ready. */
NTSTATUS T70WaveRtPortBind(PDEVICE_OBJECT Device,PIRP Irp,PUNKNOWN Miniport,
 PUNKNOWN Adapter,PRESOURCELIST Resources,PPORT *Out);
#endif
