// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_MINIPORT_H
#define T70_WAVERT_MINIPORT_H
#include "WaveRtMiniportBoundary.h"
#include <stddef.h>
/* Concrete, nonaggregated COM object, NOT a registered endpoint. Owner retains
 * provider context/code/resources through all miniport/stream references and
 * performs actual rundown; its final Release must be safe at <=DISPATCH_LEVEL.
 * Streams must independently retain that owner. No cycle back to this miniport.
 * Output storage is disjoint from Owner/provider/object memory. Provider must
 * enforce single-stream admission, not rely on descriptor counts alone.
 * Create is PASSIVE_LEVEL; COM references require a currently held reference.
 * Init/NewStream/metadata are externally serialized and non-reentrant.
 * Fixed pin 0 is software render admission, not proof of a speaker route.
 * Descriptor storage is read-only to consumers and lives with this object.
 * DMA description reports the DL1 contiguous 34-bit capability after Init;
 * InterfaceTypeUndefined requires a real PDO to supply bus identity. This does
 * not create a DMA adapter or prove cache/mapping compatibility.
 * Format negotiation is metadata, not readiness. */
class T70WaveRtMiniport final:public T70WaveRtMiniportBoundary {
public:
 static NTSTATUS Create(PUNKNOWN Owner,const T70_WAVERT_MINIPORT_PROVIDER &,T70WaveRtMiniport **Out);
 STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID,void **) override;
 STDMETHODIMP_(ULONG) AddRef() override;
 STDMETHODIMP_(ULONG) Release() override;
 STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR *) override;
 STDMETHODIMP_(NTSTATUS) DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,PULONG) override;
 STDMETHODIMP_(NTSTATUS) GetDeviceDescription(PDEVICE_DESCRIPTION) override;
private:
 // Class-local nonallocating placement construction; no throwing CRT new.
 static void *operator new(size_t,void *Storage)noexcept{return Storage;}
 T70WaveRtMiniport(PUNKNOWN,const T70_WAVERT_MINIPORT_PROVIDER &);
 ~T70WaveRtMiniport()=default;
 volatile LONG Refs;PUNKNOWN Owner;
 KSDATARANGE_AUDIO Ranges[11];PKSDATARANGE RangePointers[11];
 PCPIN_DESCRIPTOR PinDescriptor;PCFILTER_DESCRIPTOR Filter;
};
#endif
