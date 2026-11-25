// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_TOPOLOGY_MINIPORT_H
#define T70_TOPOLOGY_MINIPORT_H
#include <portcls.h>
#include <stddef.h>
/* Logical render topology: wave bridge input 0 -> speaker output 1. This is
 * metadata, not an assertion that clocks, amps or playback are ready. Create
 * remains control-free; only CreateWithVolume adds an explicitly bound volume
 * node. No mute, jack-presence or physical-speaker-direction claims.
 * Parent/code remains retained through final COM Release. Non-COM methods and
 * Close are PASSIVE_LEVEL with internal exclusive nonblocking admission;
 * caller retains a reference through each method. Descriptor pointers are owned
 * by this object and remain valid only while that reference is retained.
 * Close permanently denies new calls; an admitted call may finish and makes
 * concurrent Close return NOT_READY until explicit retry. Immutable descriptors
 * remain valid under the caller COM reference even after Close.
 * Init borrows adapter/resource/port arguments, never stores or calls them.
 * No reverse Port reference: there is no miniport/port reference cycle here.
 * Close stops metadata admission; actual subdevice unregister and outstanding
 * PortCls callback/rundown are the integrating owner's separate responsibility.
 * Final Release and Parent final Release must be nonpaged at <=DISPATCH_LEVEL. */
struct T70_TOPOLOGY_VOLUME_BINDING;
class T70TopologyMiniport final:public IMiniportTopology {
public:
 static NTSTATUS Create(PUNKNOWN Parent,T70TopologyMiniport **);
 static NTSTATUS CreateWithVolume(PUNKNOWN,const T70_TOPOLOGY_VOLUME_BINDING *,T70TopologyMiniport **);
 static NTSTATUS VolumeProperty(PPCPROPERTY_REQUEST);
 NTSTATUS Close();
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID,void **) override;
 STDMETHODIMP_(ULONG) AddRef() override;
 STDMETHODIMP_(ULONG) Release() override;
 STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN,PRESOURCELIST,PPORTTOPOLOGY) override;
 STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR *) override;
 STDMETHODIMP_(NTSTATUS) DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,PULONG) override;
private:
 T70TopologyMiniport(const T70TopologyMiniport &)=delete;
 T70TopologyMiniport &operator=(const T70TopologyMiniport &)=delete;
 static void *operator new(size_t,void *P)noexcept{return P;}
 explicit T70TopologyMiniport(PUNKNOWN);
 ~T70TopologyMiniport()=default;
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
  Parent->AddRef();return true;
 }
 void EndClose(){auto P=Parent;(void)InterlockedCompareExchange(&Calls,2,6);P->Release();}
 volatile LONG Refs;PUNKNOWN Parent;bool Initialized,Closed;
 KSDATARANGE BridgeRange;PKSDATARANGE RangePointer;
 PCPIN_DESCRIPTOR Pins[2];PCCONNECTION_DESCRIPTOR Connection;
 PCFILTER_DESCRIPTOR Filter;
 struct {void *Owner;const void *Acf;size_t Bytes;unsigned short Profile;unsigned char Map[2];LONG Minimum[2];} Volume{};
 PCNODE_DESCRIPTOR VolumeNode{};
 PCCONNECTION_DESCRIPTOR VolumeConnections[2]{};
};
#endif
