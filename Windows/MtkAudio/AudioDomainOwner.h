// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_DOMAIN_OWNER_H
#define T70_AUDIO_DOMAIN_OWNER_H
#include <portcls.h>
/* CPU/software ownership only. Does NOT prove power, clock, MMIO access or
 * absence of firmware/DSP masters. Stable nonpaged owner and real OS device/PDO;
 * serialized lifecycle at PASSIVE. A global CAS excludes a second instance of
 * this driver from claiming the single T70 AFE. Actual hardware-access admission
 * is a separate mandatory contract before mapping or reading registers. */
struct T70_AUDIO_DOMAIN_OWNER {
 PDEVICE_OBJECT Device=nullptr,Pdo=nullptr;
 bool Held=false;
};
NTSTATUS T70AudioDomainAcquire(T70_AUDIO_DOMAIN_OWNER *,PDEVICE_OBJECT,PDEVICE_OBJECT);
NTSTATUS T70AudioDomainRelease(T70_AUDIO_DOMAIN_OWNER *);
#endif
