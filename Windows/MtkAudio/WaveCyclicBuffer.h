// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVECYCLIC_BUFFER_H
#define T70_WAVECYCLIC_BUFFER_H
#include <portcls.h>
#include <stdint.h>
struct T70_WAVECYCLIC_BUFFER {
 IPortWaveCyclic *Port;
 IDmaChannel *Channel;
 void *Cpu;
 ULONG Bytes,FrameBytes,Uses;
};
/* Zero-initialized stable owner, explicit real initialized PortCls port.
 * PASSIVE_LEVEL, externally serialized including callbacks, no reentry.
 * This is an unpublished buffer: no device/port consumer may use it yet.
 * Source is resident ordinary RAM. CopyTo is delegated to the OS channel,
 * never memcpy to a potentially ARM Device Memory mapping. This establishes
 * neither a WaveCyclic miniport nor RUN/IRQ/cache-coherent WaveRT mapping.
 * DMA is restricted to 32 bits (a subset of AFE's 34), no address identity
 * assumed. Begin/End pins release only; not hardware quiescence proof.
 * Do not publish Channel to hardware until a separate lifetime owner exists. */
NTSTATUS T70WaveCyclicBufferAcquire(T70_WAVECYCLIC_BUFFER *,IPortWaveCyclic *,ULONG,ULONG);
NTSTATUS T70WaveCyclicBufferCopy(T70_WAVECYCLIC_BUFFER *,ULONG,const void *,ULONG);
NTSTATUS T70WaveCyclicBufferBegin(T70_WAVECYCLIC_BUFFER *);
NTSTATUS T70WaveCyclicBufferEnd(T70_WAVECYCLIC_BUFFER *);
NTSTATUS T70WaveCyclicBufferRelease(T70_WAVECYCLIC_BUFFER *);
#endif
