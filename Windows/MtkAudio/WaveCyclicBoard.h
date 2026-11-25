// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVECYCLIC_BOARD_H
#define T70_WAVECYCLIC_BOARD_H
#include "WaveCyclicPortOwner.h"
#include "AfeIrqRecovery.h"
extern "C" {
#include "AfeMapping.h"
}
struct T70_WAVECYCLIC_BOARD_CONTRACT {
 void *Context;
 NTSTATUS (*ClaimDomain)(void *);
 NTSTATUS (*ReleaseDomain)(void *);
 NTSTATUS (*DrainService)(void *);
 NTSTATUS (*DetachStreams)(void *);
 // If supplied, ClaimDomain establishes software ownership only. This separate
 // callback must prove physical access/firmware exclusion BEFORE any mapping.
 NTSTATUS (*AccessReady)(void *)=nullptr;
};
struct T70_WAVECYCLIC_BOARD {
 T70_WAVECYCLIC_PORT_OWNER Port;
 T70_AFE_IRQ_RECOVERY Irq;
 T70_AFE_MAPPING_OWNER Mapping;
 T70_WAVECYCLIC_BOARD_CONTRACT Contract;
 PUNKNOWN Parent;
 PRESOURCELIST Resources;
 NTSTATUS StartStatus,CleanupStatus;
 bool Attempted,DomainHeld,Lease,Started;
 NTSTATUS (*BeforeIrqClose)(void *);void *StreamOwner;
};
/* Actual StartDevice inputs and explicit actual PDO; no automatic discovery.
 * Stable zeroed nonpaged state. Parent owns Device/PDO/module/provider lifetimes.
 * All calls externally serialized, no reentry. No raw pointers exported or
 * subdevice published here. ClaimDomain must establish exclusive WHOLE AFE MCU
 * domain. With AccessReady, physical access is checked separately; without it
 * ClaimDomain must also establish no firmware/DSP semaphore clients and powered/access-ready MMIO,
 * no incompatible mappings, and prevent other producers for the entire lease.
 * It is a real ownership provider, not a boolean assumption; failure cleans its
 * own partial claim. No power or handoff implementation is invented here.
 * Source ON/status are checked disabled before connection, never enabled here.
 * Stream provider must not start/publish during initialization. DetachStreams
 * proves DMA/hardware/client references stopped before mapping can be unmapped;
 * DrainService covers PortCls notification/exported group work, not just ISR.
 * ReleaseDomain runs after unmap and must not require this mapping. Failures
 * retain remaining ownership for explicit Close. Success is port+masked IRQ
 * initialization only, not RUN admission, publication, or audible playback.
 */
NTSTATUS T70WaveCyclicBoardOpen(T70_WAVECYCLIC_BOARD *,PDEVICE_OBJECT,PDEVICE_OBJECT,
 PIRP,PUNKNOWN,PUNKNOWN,PRESOURCELIST,const T70_WAVECYCLIC_MINIPORT_PROVIDER &,
 const T70_WAVECYCLIC_BOARD_CONTRACT &);
NTSTATUS T70WaveCyclicBoardClose(T70_WAVECYCLIC_BOARD *);
#endif
