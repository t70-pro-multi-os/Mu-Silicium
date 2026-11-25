// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_BUFFERED_STREAM_BASE_H
#define T70_WAVERT_BUFFERED_STREAM_BASE_H
#include "WaveRtStreamBase.h"
struct T70_WAVERT_BUFFER_TRANSACTION {
 T70_WAVERT_MDL_OWNER Pages;
 T70_WAVERT_DMA_OWNER Dma;
 T70_STREAM_CONFIG_RESULT Configure;
 PMDL Published;
 ULONG PublishedBytes;
 bool LifetimeHeld,Terminal,FreeRequested,HardwareReleased;
 NTSTATUS LastStatus;
};
/* ABSTRACT, not registered. A real provider must implement every pure method,
 * COM lifetime and hardware latency. No default-success policy/recovery exists.
 * Zeroed external transaction and all resources outlive failure/recovery AND
 * driver unload is blocked while LifetimeHeld. Provider pins this stream too;
 * Never invoke the base ConfigureMapped directly on this derived stream.
 * current caller independently retains a reference across every method call.
 * PASSIVE_LEVEL, nonpaged objects, complete external serialization/no reentry.
 * Outputs are valid aligned ordinary storage disjoint from each other and all
 * owners/provider objects. One allocation attempt per stream after pin succeeds.
 * A failed Free is quarantined, NOT complete; PortCls need not call it again.
 * Integrator must schedule Recover explicitly and cannot destroy/unload pinned
 * objects/code. This is not a usable stream without that actual provider.
 * Safety callbacks are synchronous: only exact STATUS_SUCCESS certifies complete
 * pin/cache/recovery work. Pending/informational statuses cannot authorize I/O
 * or release. Callbacks must not queue asynchronous work. Failed Pin has no
 * retained pin; failed recovery preserves all device references for retry. */
class T70WaveRtBufferedStreamBase:public T70WaveRtStreamBase{
public:
 T70WaveRtBufferedStreamBase(const T70_AFE_PREPARED &Plan,T70_WAVERT_BUFFER_TRANSACTION *Transaction,
  IPortWaveRTStream *Port,PDMA_ADAPTER Adapter,T70_AFE_MAPPING_OWNER *Afe);
 STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT Format) override;
 STDMETHODIMP_(NTSTATUS) AllocateAudioBuffer(ULONG Requested,PMDL *Mdl,ULONG *Actual,
  ULONG *Offset,MEMORY_CACHING_TYPE *Cache) override final;
 STDMETHODIMP_(void) FreeAudioBuffer(PMDL Mdl,ULONG Bytes) override final;
 NTSTATUS Recover();
protected:
 ~T70WaveRtBufferedStreamBase()=default;
 /* Must retain stream/transaction/adapter/AFE/PortStream AND recovery code until
  * Unpin. Excludes device teardown/unload; a mock counter is not a real pin. */
 virtual NTSTATUS PinRecoveryLifetime()=0;
 virtual void UnpinRecoveryLifetime()=0;
 /* Prove actual ARM64 CPU/client access, cache/coherency/visibility and mapping
  * suitability of THIS allocation/adapter. An enum match alone is insufficient. */
 virtual NTSTATUS VerifyClientMapping(const T70_WAVERT_DMA_OWNER &Dma)=0;
 /* Success must prove ALL device references removed, not just a stop request.
  * Do not end software leases or free the owner in this callback. */
 virtual NTSTATUS RemoveHardwareReferences()=0;
private:
 using T70WaveRtStreamBase::ConfigureMapped; // transaction owns configuration
 T70_WAVERT_BUFFER_TRANSACTION *T;
 IPortWaveRTStream *Port;
 PDMA_ADAPTER Adapter;
 T70_AFE_MAPPING_OWNER *Afe;
 NTSTATUS FailBeforeConfigure(NTSTATUS Status);
};
#endif
