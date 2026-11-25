// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_STREAM_H
#define T70_WAVERT_STREAM_H
#include "WaveRtComStreamBase.h"
/* Explicit integrating driver's providers. All are required, synchronous, and
 * must not queue work. No default-success or guessed hardware latency exists.
 * Owner retains Context, Adapter, Afe and provider code until final Release.
 * Describe certifies this format's actual latency and admission prerequisites,
 * without performing hardware I/O. Describe is a read-only query: it acquires
 * no reservation/reference on ANY return. Its latency remains valid for this
 * stream lifetime; actual resource admission happens in Pin. Pin excludes
 * device teardown/unload through recovery; stream itself supplies its COM pin.
 * Failed Pin retains nothing. Verify proves THIS allocation's client mapping.
 * Remove proves all device references removed, not merely a disable request.
 * Context/admission/stream methods are externally serialized; no reentry.
 * All inputs/output storage are valid, disjoint and stable through Create.
 * Callback storage is copied, not borrowed. Owner/Port final Release must be
 * safe at <=DISPATCH_LEVEL. The integrating owner must arrange explicit Recover
 * after partial configure failures; PortCls need not call Free again.
 */
struct T70_WAVERT_STREAM_PROVIDER {
 PUNKNOWN Owner;
 void *Context;
 PDMA_ADAPTER Adapter;
 T70_AFE_MAPPING_OWNER *Afe;
 ULONG InitialBufferBytes,PeriodBytes;
 NTSTATUS (*Describe)(void *,const T70_AFE_PREPARED *,KSRTAUDIO_HWLATENCY *);
 NTSTATUS (*Pin)(void *);
 void (*Unpin)(void *);
 NTSTATUS (*Verify)(void *,const T70_WAVERT_DMA_OWNER *);
 NTSTATUS (*Remove)(void *);
};
class T70WaveRtStream final:public T70WaveRtComStreamBase {
public:
 // Directly usable as T70_WAVERT_MINIPORT_PROVIDER.Create. Context is a live
 // T70_WAVERT_STREAM_PROVIDER, pin 0 render, valid KS format from PortCls.
 static NTSTATUS Create(void *,PPORTWAVERTSTREAM,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *);
 STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT) override final;
 STDMETHODIMP_(NTSTATUS) GetPosition(PKSAUDIO_POSITION) override final;
 STDMETHODIMP_(void) GetHWLatency(KSRTAUDIO_HWLATENCY *) override final;
private:
 T70WaveRtStream(const T70_WAVERT_STREAM_PROVIDER &,PPORTWAVERTSTREAM,
  const T70_AFE_PREPARED &,const KSRTAUDIO_HWLATENCY &);
 ~T70WaveRtStream()override;
 NTSTATUS PinRecoveryLifetime()override;
 void UnpinRecoveryLifetime()override;
 NTSTATUS VerifyClientMapping(const T70_WAVERT_DMA_OWNER &)override;
 NTSTATUS RemoveHardwareReferences()override;
 T70_WAVERT_BUFFER_TRANSACTION Transaction;
 T70_WAVERT_STREAM_PROVIDER Provider;
 PPORTWAVERTSTREAM Port;
 KSRTAUDIO_HWLATENCY Latency;
};
#endif
