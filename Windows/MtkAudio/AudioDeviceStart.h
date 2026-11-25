// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_DEVICE_START_H
#define T70_AUDIO_DEVICE_START_H
#include "AudioResourceOwner.h"
#include "DmaAdapterOwner.h"
#include "WaveRtPortBind.h"
struct T70_AUDIO_DEVICE_START {
 T70AudioResourceOwner *Resources;
 T70WaveRtMiniport *Miniport;
 PPORT Port;
 T70_DMA_ADAPTER_OWNER Dma;
 bool Prepared;
};
/* Explicit actual device/start IRP/resource list/PDO from an integrating driver.
 * Device must ALREADY be a valid PortCls device; this routine is not callable
 * with a fabricated plain device object. Its existing PDO association is checked
 * before resource acquisition. Platform keeps that association alive/excluded
 * from removal through the query and subsequent retained adapter acquisition.
 * No discovery or registration. Zeroed nonpaged stable owner, PASSIVE/external
 * serialization and OS removal/unload exclusion, all inputs valid and disjoint.
 * Prepare success means only resource/port/adapter ownership established.
 * The installed stream factory always fails NOT_READY: no cache, latency,
 * routing or start provider exists. No endpoint may be advertised as ready.
 * Prepared records completed initialization, NOT ongoing health. Once Release
 * is attempted, all subsequent access is recovery-only, even if it fails with
 * Prepared still true (the adapter may already be gone). Do not resume usage.
 * On error inspect retained fields and retry Release; never erase live state.
 * After success Release requires actual external quiescence proof. Leases from
 * Resources or Dma must cover all downstream use; dropping them is not proof.
 * No copying; retain owner storage and recovery path until Release succeeds.
 */
NTSTATUS T70AudioDevicePrepare(T70_AUDIO_DEVICE_START *,PDEVICE_OBJECT Device,PIRP StartIrp,
 PDEVICE_OBJECT Pdo,PRESOURCELIST Resources,ULONG MemoryIndex);
NTSTATUS T70AudioDeviceRelease(T70_AUDIO_DEVICE_START *,NTSTATUS (*ConfirmQuiesced)(void *),void *);
#endif
