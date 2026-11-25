// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVECYCLIC_MINIPORT_H
#define T70_WAVECYCLIC_MINIPORT_H
#include <portcls.h>
#include <stddef.h>
/* Explicit synchronous, non-reentrant providers. Owner retains Context/code and
 * device resources; non-COM admission is internally exclusive and nonblocking. Init failure
 * cleans its own partial state. Create returns independently owned references
 * for each non-NULL output, even on failure. Failure outputs must already be
 * stopped, unpublished and safe to Release (no outstanding hardware/self pins).
 * Success requires actual hardware/notification providers (the standalone
 * unpublished software WaveCyclicStream is insufficient). It certifies
 * render format admission, initial STOP/zero position and
 * channel/service-group compatibility. This boundary cannot prove hardware
 * state. Streams independently retain their owner. No owner/port reference cycle.
 * Provider enforces single-stream admission; metadata is not enforcement.
 * Explicit Close is required to break Port/miniport reference cycles. Caller
 * holds a reference throughout Close; its synchronous confirmation proves all
 * device-facing stream/callback work is detached and device access is excluded.
 * Independently retained, stopped CPU-only COM objects may outlive Close.
 * Close closes admission atomically and returns NOT_READY while a method runs;
 * caller retries after that method returns. It never waits inside a callback. Failure retains
 * Port and permanently closes admission; do not rely on final Release to break
 * a cycle. Output storage is disjoint from owner/provider/object/format storage.
 * Owner/Port final Release is nonpaged and safe at <=DISPATCH_LEVEL.
 */
struct T70_WAVECYCLIC_MINIPORT_PROVIDER {
 void *Context;
 NTSTATUS (*Initialize)(void *,PUNKNOWN,PRESOURCELIST,IPortWaveCyclic *);
 NTSTATUS (*Create)(void *,IPortWaveCyclic *,PKSDATAFORMAT,
  PMINIPORTWAVECYCLICSTREAM *,PDMACHANNEL *,PSERVICEGROUP *);
};
class T70WaveCyclicMiniport final:public IMiniportWaveCyclic {
public:
 static NTSTATUS Create(PUNKNOWN,const T70_WAVECYCLIC_MINIPORT_PROVIDER &,T70WaveCyclicMiniport **);
 // Permanently close provider/metadata admission without releasing Port.
 NTSTATUS QuiesceCalls();
 NTSTATUS Close(NTSTATUS (*ConfirmDetached)(void *),void *);
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID,void **) override;
 STDMETHODIMP_(ULONG) AddRef() override;
 STDMETHODIMP_(ULONG) Release() override;
 STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR *) override;
 STDMETHODIMP_(NTSTATUS) DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,PULONG) override;
 STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN,PRESOURCELIST,PPORTWAVECYCLIC) override;
 STDMETHODIMP_(NTSTATUS) NewStream(PMINIPORTWAVECYCLICSTREAM *,PUNKNOWN,POOL_TYPE,ULONG,BOOLEAN,PKSDATAFORMAT,PDMACHANNEL *,PSERVICEGROUP *) override;
private:
 T70WaveCyclicMiniport(const T70WaveCyclicMiniport &)=delete;
 T70WaveCyclicMiniport &operator=(const T70WaveCyclicMiniport &)=delete;
 static void *operator new(size_t,void *P) noexcept{return P;}
 T70WaveCyclicMiniport(PUNKNOWN,const T70_WAVECYCLIC_MINIPORT_PROVIDER &);
 ~T70WaveCyclicMiniport()=default;
 // Gate: bit 0 active method, bit 1 admission closed, bit 2 Close owner.
 // Exclusive method admission also serializes Init versus metadata/Create.
 volatile LONG Calls=0;
 struct Call {
  volatile LONG *Gate;PUNKNOWN Parent;bool Entered;
  Call(volatile LONG *G,PUNKNOWN P):Gate(G),Parent(P),Entered(InterlockedCompareExchange(G,1,0)==0){if(Entered)Parent->AddRef();}
  ~Call(){if(Entered){auto P=Parent;InterlockedDecrement(Gate);P->Release();}}
 };
 bool BeginClose(){
  LONG S=InterlockedCompareExchange(&Calls,0,0);
  for(;;){LONG N=S|2;LONG P=InterlockedCompareExchange(&Calls,N,S);if(P==S)break;S=P;}
  if(InterlockedCompareExchange(&Calls,6,2)!=2)return false;
  Owner->AddRef();return true;
 }
 void EndClose(){auto P=Owner;(void)InterlockedCompareExchange(&Calls,2,6);P->Release();}
 volatile LONG Refs;PUNKNOWN Owner;IPortWaveCyclic *Port;
 T70_WAVECYCLIC_MINIPORT_PROVIDER Provider;bool Attempted,Initialized,Closing;
 KSDATARANGE_AUDIO Ranges[11];PKSDATARANGE Pointers[11];
 PCPIN_DESCRIPTOR Pins[2];KSDATARANGE Bridge;PKSDATARANGE BridgePointer;
 PCNODE_DESCRIPTOR Dac;PCCONNECTION_DESCRIPTOR Connections[2];PCFILTER_DESCRIPTOR Filter;
};
#endif
