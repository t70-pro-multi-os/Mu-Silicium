// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_RESOURCE_OWNER_H
#define T70_AUDIO_RESOURCE_OWNER_H
#include "AfeResourceMapping.h"
#include <stddef.h>
class T70AudioResourceOwner;
struct T70_AUDIO_RESOURCE_USE {T70AudioResourceOwner *Owner;};
/* Explicit COM owner, not an OS removal/interrupt registration implementation.
 * All operations except AddRef/Release are PASSIVE, externally serialized and
 * non-reentrant. COM methods require an existing reference; final Release may
 * be <=DISPATCH only after Close released all resources. No cycles to miniport.
 * OS removal/unload exclusion must still be provided by the actual adapter.
 * Open pins the object; failed Close deliberately retains its mapping and
 * references. This does not itself prevent kernel-image unload.
 * Integrator must retain a recovery reference and retry Close, not lose it.
 * ConfirmQuiesced returns exact STATUS_SUCCESS only for completed proof; no
 * pending/informational result permits release. It must prove no DMA/IRQ/MMIO/client references remain, including
 * independent common-buffer/MDL owners. No stop or free is inferred here.
 * Uses bracket entire stream/DMA owner activity; EndUse only after that client
 * relinquishes all resource references. Close additionally requires the real
 * global quiescence callback, so dropping leases alone cannot permit unmap.
 * A lease is zeroed, unique, uncopied, disjoint storage; no stale use after End.
 * Caller retains returned AFE pointer only within a live lease. */
class T70AudioResourceOwner final:public IUnknown {
public:
 static NTSTATUS Create(ULONG MemoryIndex,T70AudioResourceOwner **Out);
 static NTSTATUS Initialize(void *,PUNKNOWN,PRESOURCELIST,PPORTWAVERT);
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID,void **) override;
 STDMETHODIMP_(ULONG) AddRef() override;
 STDMETHODIMP_(ULONG) Release() override;
 NTSTATUS BeginUse(T70_AUDIO_RESOURCE_USE *,T70_AFE_MAPPING_OWNER **);
 NTSTATUS EndUse(T70_AUDIO_RESOURCE_USE *);
 NTSTATUS Close(NTSTATUS (*ConfirmQuiesced)(void *),void *Context);
private:
 explicit T70AudioResourceOwner(ULONG);
 ~T70AudioResourceOwner()=default;
 static void *operator new(size_t,void *Storage)noexcept{return Storage;}
 T70AudioResourceOwner(const T70AudioResourceOwner &)=delete;
 T70AudioResourceOwner &operator=(const T70AudioResourceOwner &)=delete;
 volatile LONG Refs;T70_AFE_RESOURCE_MAPPING Resource;
 ULONG Users;bool Open,Closing,Closed,Quiesced;
};
#endif
