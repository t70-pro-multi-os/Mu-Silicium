// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_MINIPORT_BOUNDARY_H
#define T70_WAVERT_MINIPORT_BOUNDARY_H
#include <portcls.h>
/* Actual interface admission boundary, still abstract for COM, filter/data-range
 * and real device description. No factory or registration is supplied here.
 * Provider context is immutable, nonpaged, externally serialized and retained
 * for this entire object lifetime. Callbacks must not reenter this object. Its real owner supplies resource lifetime,
 * teardown and stream construction; missing callbacks fail closed.
 * Initialize must validate actual resources and acquire necessary references;
 * on failure it must unwind itself. It may run once only, even after failure.
 * Create returns one owned, unstarted stream reference (or NULL on failure),
 * retaining PortStream/format metadata as needed. A non-NULL result on error is
 * also owned and releasable. It must be safe to Release before any publication.
 * Create must not start DMA or modify hardware. Success certifies the concrete
 * stream's resource/cache/recovery contracts, not playback. No existing T70
 * abstract stream satisfies this provider contract yet.
 * Initialize/Create are synchronous: only exact STATUS_SUCCESS certifies
 * completed work. Nonzero statuses must not leave queued asynchronous work;
 * failure cleanup and owned-reference rules above apply to those statuses too.
 * Admission also requires completed STOP and zero-position queries; pending is
 * not a usable initial stream state. These initial stream methods must also be
 * synchronous, leave no queued work and permit immediate Release on failure;
 * rejecting pending does NOT cancel a real asynchronous operation.
 * No retry of Initialize on the same object.
 * RenderPin must agree with the real filter descriptor; no topology invented.
 * Every method here is PASSIVE_LEVEL; output storage disjoint from all objects.
 */
struct T70_WAVERT_MINIPORT_PROVIDER {
 void *Context;
 NTSTATUS (*Initialize)(void *,PUNKNOWN,PRESOURCELIST,PPORTWAVERT);
 NTSTATUS (*Create)(void *,PPORTWAVERTSTREAM,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *);
};
class T70WaveRtMiniportBoundary:public IMiniportWaveRT {
public:
 T70WaveRtMiniportBoundary(ULONG RenderPin,const T70_WAVERT_MINIPORT_PROVIDER &Provider);
 STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN,PRESOURCELIST,PPORTWAVERT) override final;
 STDMETHODIMP_(NTSTATUS) NewStream(PMINIPORTWAVERTSTREAM *,PPORTWAVERTSTREAM,ULONG,BOOLEAN,PKSDATAFORMAT) override final;
protected:
 bool ResourceInitializationSucceeded()const{return Initialized;}
 ~T70WaveRtMiniportBoundary()=default;
private:
 T70WaveRtMiniportBoundary(const T70WaveRtMiniportBoundary &)=delete;
 T70WaveRtMiniportBoundary &operator=(const T70WaveRtMiniportBoundary &)=delete;
 ULONG Pin;T70_WAVERT_MINIPORT_PROVIDER Provider;bool Attempted,Initialized;
};
#endif
