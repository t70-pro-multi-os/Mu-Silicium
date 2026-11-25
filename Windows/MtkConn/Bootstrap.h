// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_CONN_BOOTSTRAP_H
#define T70_CONN_BOOTSTRAP_H

#include <ntddk.h>
#include <Library/MtkConnBoot.h>
#include "../MtkBtif/ConnPmic.h"
#include "../MtkGpio/ConnPinsInterface.h"
#include "../MtkMali/ConnPowerInterface.h"

typedef struct {
    PUCHAR Window;
    PULONG Reset;
    PUCHAR Emi;
    ULONGLONG EmiBase;
    MTK_CONN_POWER_INTERFACE Power;
    MTK_CONN_PINS_INTERFACE Pins;
    T70_CONN_PMIC Pmic;
    ULONGLONG PowerToken;
    ULONGLONG PinsToken;
    MTK_CONN_BOOT Boot;
    MTK_CONN_RAILS Rails;
    ULONG Adie;
    BOOLEAN BtifRestore;
    ULONG ScoIrqMaskBaseline;
    BOOLEAN ScoIrqMaskCaptured;
} T70_CONN_BOOTSTRAP;

/*
 * One WCN owner supplies zeroed storage and serializes all calls at PASSIVE_LEVEL.
 * Map the PnP-assigned CONN aperture and its dedicated RGU reset register.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnPrepareHardware(T70_CONN_BOOTSTRAP *Owner,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawWindow,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *TranslatedWindow,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawReset,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *TranslatedReset);

/*
 * Supply referenced provider interfaces and stable stock ROM image sets for
 * MT6631 and MT6635 in that order. The owner takes its own provider references.
 * Success reports the MCU idle handshake in Adie's variant. Radio transports
 * may start only after their own firmware and transport handshakes.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnStart(T70_CONN_BOOTSTRAP *Owner, const MTK_CONN_POWER_INTERFACE *Power,
    const MTK_CONN_PINS_INTERFACE *Pins, const MTK_CONN_ROM_SET Images[2]);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnCheck(const T70_CONN_BOOTSTRAP *Owner);

/* Clock operations are serialized with the sole client's access and Stop. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnEnableBtifClock(T70_CONN_BOOTSTRAP *Owner);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnRestoreBtifClock(T70_CONN_BOOTSTRAP *Owner);

/*
 * Drain all CONN clients before stopping. On failure, retain the owner, its
 * mappings and remaining leases for retry while the device and providers
 * remain accessible.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnStop(T70_CONN_BOOTSTRAP *Owner);

/*
 * Unmap PnP resources without touching their registers. Stop while the device
 * is still in D0 if possible. An unresolved lease returns STATUS_DEVICE_BUSY
 * and keeps the DMA backing pinned. Retain Owner and retry Stop only after
 * PrepareHardware validates and maps the reassigned resources.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnReleaseHardware(T70_CONN_BOOTSTRAP *Owner);

#endif
