// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_ENDPOINT_H
#define T70_AUDIO_ENDPOINT_H
#include "WaveCyclicPortOwner.h"
#include "T70TopologyMiniport.h"
/* Stable zeroed owner, PASSIVE and serialized with startup/removal/NewStream.
 * Wave/Parent/device remain alive until Close succeeds. Only call Open after
 * real platform admission; this metadata does not prove hardware readiness.
 * Withdraw first stops external discovery, NOT existing streams/DMA/IRQ.
 * Close may run only after the board's actual stream/IRQ rundown succeeds. */
struct T70_AUDIO_ENDPOINT {
 PDEVICE_OBJECT Device;
 T70_WAVECYCLIC_PORT_OWNER *Wave;
 T70TopologyMiniport *Miniport;
 PPORT Port;
 PUNREGISTERSUBDEVICE Unregister;
 PUNREGISTERPHYSICALCONNECTION Unlink;
 bool Attempted,Registered,Connected,Closing;
 NTSTATUS CleanupStatus;
};
NTSTATUS T70AudioEndpointOpen(T70_AUDIO_ENDPOINT *,PDEVICE_OBJECT,PIRP,PUNKNOWN,
 PRESOURCELIST,T70_WAVECYCLIC_PORT_OWNER *);
NTSTATUS T70AudioEndpointOpenWithVolume(T70_AUDIO_ENDPOINT *,PDEVICE_OBJECT,PIRP,PUNKNOWN,
 PRESOURCELIST,T70_WAVECYCLIC_PORT_OWNER *,const T70_TOPOLOGY_VOLUME_BINDING *);
// Internal common startup path; creator is synchronous and transfers one COM ref
// only on exact success. No endpoint is published before miniport creation.
typedef NTSTATUS (*T70_TOPOLOGY_CREATOR)(PUNKNOWN,const void *,T70TopologyMiniport **);
NTSTATUS T70AudioEndpointOpenUsing(T70_AUDIO_ENDPOINT *,PDEVICE_OBJECT,PIRP,PUNKNOWN,
 PRESOURCELIST,T70_WAVECYCLIC_PORT_OWNER *,T70_TOPOLOGY_CREATOR,const void *);
NTSTATUS T70AudioEndpointWithdraw(T70_AUDIO_ENDPOINT *);
NTSTATUS T70AudioEndpointClose(T70_AUDIO_ENDPOINT *);
#endif
