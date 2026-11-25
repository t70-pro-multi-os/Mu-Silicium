// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_STREAM_BASE_H
#define T70_WAVERT_STREAM_BASE_H
#include <portcls.h>
#include "WaveRtDmaOwner.h"
extern "C" {
#include "WaveRtFormat.h"
#include "ConfigureMapped.h"
}
/* Actual PortCls interface boundary, deliberately ABSTRACT. A concrete owner
 * must implement COM lifetime, Allocate/FreeAudioBuffer and GetHWLatency.
 * No fabricated MDL or latency and no creatable/registered stream are supplied.
 * InitialPlan is validated arithmetic geometry, NOT an allocated DMA buffer.
 * Calls and destruction must be externally serialized; object is nonpaged.
 * GetPosition permits PASSIVE_LEVEL or DISPATCH_LEVEL; other methods here are
 * PASSIVE_LEVEL. A real subclass may not advertise support
 * or publish the stream until its buffer/rundown/hardware contracts are met.
 * State is deliberately final/fail-closed in this integration stage. Actual
 * running requires revising this class together with SetFormat/buffer lifetime;
 * this base never enables DMA or claims an unknown hardware state is stopped. */
struct T70_STREAM_CONFIG_RESULT {
 bool BeginAttempted,EndAttempted,DmaLeaseHeld;
 NTSTATUS BeginStatus,EndStatus;
 T70_MAPPED_CONFIG_RESULT Mapped;
};
class T70WaveRtStreamBase : public IMiniportWaveRTStream {
public:
 explicit T70WaveRtStreamBase(const T70_AFE_PREPARED &InitialPlan);
 STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT DataFormat) override;
 STDMETHODIMP_(NTSTATUS) SetState(KSSTATE State) override final;
 STDMETHODIMP_(NTSTATUS) GetPosition(PKSAUDIO_POSITION Position) override;
 STDMETHODIMP_(NTSTATUS) GetPositionRegister(KSRTAUDIO_HWREGISTER *Register) override;
 STDMETHODIMP_(NTSTATUS) GetClockRegister(KSRTAUDIO_HWREGISTER *Register) override;
 /* Metadata copy only; caller supplies aligned disjoint Out. */
 /* Explicit real resource owner calls this preparation step, not SetState.
  * Requires quiescent, exclusive, device-access-ready AFE. Actual callback
  * writes may partially apply. Once started, DMA-use lease is retained and
  * SetFormat/configure retry permanently blocked on this stream object.
  * Caller must retain both owners, resolve AFE End failure and independently
  * remove hardware buffer references before DmaEndUse; no automatic release.
  * Result is callback progress, never readiness or DMA-stop proof. */
 T70_STREAM_CONFIG_RESULT ConfigureMapped(T70_WAVERT_DMA_OWNER *Dma,T70_AFE_MAPPING_OWNER *Afe);
 bool CopyPlan(T70_AFE_PREPARED *Out) const;
protected:
 ~T70WaveRtStreamBase() = default;
private:
 T70_AFE_PREPARED Plan;
 bool Valid;
 bool ConfigurationAttempted;
 T70WaveRtStreamBase(const T70WaveRtStreamBase &) = delete;
 T70WaveRtStreamBase &operator=(const T70WaveRtStreamBase &) = delete;
};
#endif
