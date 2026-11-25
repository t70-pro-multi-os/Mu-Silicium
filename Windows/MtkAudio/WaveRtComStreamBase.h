// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_COM_STREAM_BASE_H
#define T70_WAVERT_COM_STREAM_BASE_H
#include "WaveRtBufferedStreamBase.h"
#include <stddef.h>
/* Nonaggregated COM ownership for the actual buffered WaveRT interface.
 * Derived factories validate inputs/plan and allocate with this class's new.
 * Parent is non-NULL, outlives construction, and owns provider context/resources
 * independently of the miniport. No parent->stream reference cycle is allowed.
 * Every concurrent caller holds a reference before entering. Non-COM methods
 * still require the buffered base's external serialization/rundown contract.
 * Parent final Release and the derived destructor must be nonpaged and safe at
 * <=DISPATCH_LEVEL. The destructor must NOT recover/free live DMA resources:
 * PinRecoveryLifetime must hold a stream reference until recovery completes.
 * This reference is not an OS remove lock or permission to unload driver code.
 * Hardware latency/cache/recovery remain pure obligations, never fake defaults.
 */
class T70WaveRtComStreamBase:public T70WaveRtBufferedStreamBase {
public:
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID,void **) override final;
 STDMETHODIMP_(ULONG) AddRef() override final;
 STDMETHODIMP_(ULONG) Release() override final;
 static void *operator new(size_t Bytes) noexcept;
 static void operator delete(void *Memory) noexcept;
protected:
 T70WaveRtComStreamBase(PUNKNOWN Parent,const T70_AFE_PREPARED &Plan,
  T70_WAVERT_BUFFER_TRANSACTION *Transaction,IPortWaveRTStream *Port,
  PDMA_ADAPTER Adapter,T70_AFE_MAPPING_OWNER *Afe);
 virtual ~T70WaveRtComStreamBase()=default;
private:
 volatile LONG Refs;
 PUNKNOWN Parent;
};
#endif
