// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVECYCLIC_PORT_OWNER_H
#define T70_WAVECYCLIC_PORT_OWNER_H
#include "WaveCyclicMiniport.h"
struct T70_WAVECYCLIC_PORT_OWNER {
 T70WaveCyclicMiniport *Miniport;
 PPORT Port;
 NTSTATUS InitStatus,CleanupStatus;
 bool Attempted,Initialized;
 PDEVICE_OBJECT OpenDevice,RegisteredDevice;
 PUNREGISTERSUBDEVICE Unregister;
 bool Registered,PublicationClosed;
};
/* Stable zeroed owner; actual PortCls StartDevice arguments, not invented PDO
 * or resource list. Parent/provider context retained by the miniport, provider
 * owns resources it keeps beyond Init. Caller holds all startup inputs through
 * this synchronous call. PASSIVE, externally serialized, no reentry. Open itself
 * does not register/publish. Success means Port.Init only, NOT stream/IRQ ready.
 * Failure after port creation can RETAIN the owner if rundown cannot be proven;
 * inspect pointers/CleanupStatus and retry Close, never discard that state.
 * Confirm must prove provider-side users/callbacks/hardware references detached;
 * exact success only. It must also clean failed partial initialization. Never
 * fabricate this callback to claim a device stopped. The owner is one-shot,
 * including failed Init; use a fresh zeroed owner only after full Close.
 * Close breaks the miniport-to-port reference before releasing local port then
 * miniport. Explicit Publish/Withdraw owns registration; raw pointer exports
 * remain forbidden. */
NTSTATUS T70WaveCyclicPortOpen(T70_WAVECYCLIC_PORT_OWNER *,PDEVICE_OBJECT,PIRP,
 PUNKNOWN Parent,PUNKNOWN Adapter,PRESOURCELIST,
 const T70_WAVECYCLIC_MINIPORT_PROVIDER &,NTSTATUS (*Confirm)(void *),void *);
NTSTATUS T70WaveCyclicPortClose(T70_WAVECYCLIC_PORT_OWNER *,NTSTATUS (*Confirm)(void *),void *);
/* Explicit final publication after board/IRQ/provider admission. This registers
 * one KS wave filter, not a complete topology/render endpoint. Serialized with
 * NewStream admission and removal. Name is driver-static for device lifetime.
 * Withdraw removes discoverability only; existing streams still need rundown. */
NTSTATUS T70WaveCyclicPortPublish(T70_WAVECYCLIC_PORT_OWNER *,PDEVICE_OBJECT);
NTSTATUS T70WaveCyclicPortWithdraw(T70_WAVECYCLIC_PORT_OWNER *);
#endif
