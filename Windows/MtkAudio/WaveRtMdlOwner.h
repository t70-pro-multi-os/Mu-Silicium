// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_WAVERT_MDL_OWNER_H
#define T70_WAVERT_MDL_OWNER_H
#include <portcls.h>
#include <stdint.h>
/* Explicit real PortStream provider, PASSIVE_LEVEL, externally serialized and non-reentrant.
 * Zero owner, nonpaged stable/disjoint storage. No destructor auto-release.
 * Dedicated whole pages only, <=196608 bytes, all zeroed before publication.
 * Cached CPU mapping is NOT a DMA/coherency promise. No logical DMA address is
 * produced; NEVER use MDL CPU physical addresses as DL1 device addresses.
 * View must be valid aligned ordinary output storage disjoint from Owner,
 * MDL/PFN storage, PortStream object and CPU mapping. Only owner/CPU overlap
 * is checked dynamically; opaque provider allocation sizes are not guessed.
 * Begin/End bracket ALL borrowed MDL/CPU access. Caller must retain a lease
 * for any OS/user mapping or independent DMA mapping/reference, and may end it
 * only after those references are actually gone. Counters cannot prove rundown.
 * No user mapping, DMA registration, PortCls AllocateAudioBuffer success,
 * hardware programming or stream readiness is supplied by this owner. */
struct T70_WAVERT_MDL_OWNER {
 IPortWaveRTStream *Port;
 PMDL Mdl;
 void *Cpu;
 ULONG Bytes;
 uint32_t Uses;
};
struct T70_WAVERT_MDL_VIEW {PMDL Mdl;void *Cpu;ULONG Bytes;MEMORY_CACHING_TYPE CpuCache;};
NTSTATUS T70WaveRtMdlAcquire(T70_WAVERT_MDL_OWNER *Owner,IPortWaveRTStream *Port,ULONG Bytes);
NTSTATUS T70WaveRtMdlBegin(T70_WAVERT_MDL_OWNER *Owner,T70_WAVERT_MDL_VIEW *View);
NTSTATUS T70WaveRtMdlEnd(T70_WAVERT_MDL_OWNER *Owner);
NTSTATUS T70WaveRtMdlRelease(T70_WAVERT_MDL_OWNER *Owner);
#endif
