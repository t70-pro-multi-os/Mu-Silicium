// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVECYCLIC_FACTORY_H
#define T70_WAVECYCLIC_FACTORY_H
#include "WaveCyclicBoard.h"
#include "WaveCyclicStream.h"
struct T70_WAVECYCLIC_STREAM_CONTRACT {
 void *Context;
 NTSTATUS (*Access)(void *),(*Remove)(void *),(*Prepare)(void *),(*Ready)(void *),(*Stopped)(void *);
 NTSTATUS (*DrainExports)(void *);
 // Optional real export owner: borrowed internal interfaces -> independently
 // retained public interfaces. Failure leaves no live output references.
 NTSTATUS (*Publish)(void *,PMINIPORTWAVECYCLICSTREAM,PDMACHANNEL,PSERVICEGROUP,
  PMINIPORTWAVECYCLICSTREAM *,PDMACHANNEL *,PSERVICEGROUP *)=nullptr;
 // Optional for legacy/test providers; production supplies actual shared sink
 // admission. These do not cancel queued DPCs. Resume requires valid buffers
 // for any late callback, and neither callback proves hardware pause position.
 NTSTATUS (*PauseCallbacks)(void *)=nullptr;
 NTSTATUS (*ResumeCallbacks)(void *)=nullptr;
};
struct T70_WAVECYCLIC_FACTORY {
 T70_WAVECYCLIC_BOARD *Board;
 T70_WAVECYCLIC_STREAM_CONTRACT Contract;
 T70_WAVECYCLIC_BUFFER Buffer;
 T70WaveCyclicStream *Stream;
 IPortWaveCyclic *Port;
 bool Initialized,Closing,Published;
 NTSTATUS CleanupStatus;
};
/* Stable zeroed nonpaged storage owned by Board.Parent. Set Board and Contract
 * before BoardOpen; pass Provider to BoardOpen. Remove is platform CPU/queued
 * user rundown AFTER this factory executes stock normal STOP; it need not poll
 * an undocumented idle bit. Prepare/Stopped must still supply actual Windows
 * pause/resume position policy, not just acknowledge that STOP returned.
 * No implicit power/cache/idle
 * providers. All lifetime/stream methods externally serialized, no reentry.
 * Initialize installs Quiesce as the pre-IRQ-close hook: stock STOP and actual
 * CPU/service rundown complete BEFORE IRQ teardown. It does not wait for export
 * references or free the channel/Port. The integrating owner MUST call final
 * Close from its post-IRQ DetachStreams path before unmapping/releasing storage.
 * Owner must retain this storage through all exported COM references. DrainExports is
 * real PortCls/client rundown, not a refcount guess. Failure keeps ownership and
 * blocks another stream; retry Close explicitly. Single stream per board open.
 * The SAME IRQ-owned service group is returned, never a detached synthetic one.
 * Create failures publish no outputs, including cleanup failures; retained
 * unpublished stream is reachable here until Close succeeds. Successfully
 * cleaned unpublished creation failures keep the initialized factory usable for
 * a later explicit NewStream; they do not tear down the device port. No automatic
 * endpoint registration, clock/route setup, or fabricated resource supply. */
T70_WAVECYCLIC_MINIPORT_PROVIDER T70WaveCyclicFactoryProvider(T70_WAVECYCLIC_FACTORY *);
/* Permanent admission close. Successful Quiesce leaves Stream/Buffer/Port and
 * Published ownership intact; repeated calls do not repeat a completed STOP.
 * Final Close remains retryable until exported references have drained. */
NTSTATUS T70WaveCyclicFactoryQuiesce(T70_WAVECYCLIC_FACTORY *);
NTSTATUS T70WaveCyclicFactoryClose(T70_WAVECYCLIC_FACTORY *);
#endif
