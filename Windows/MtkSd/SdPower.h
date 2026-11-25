// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SD_POWER_H
#define T70_SD_POWER_H

#include <ntddk.h>
#include <prminterface.h>

typedef struct {
    PRM_INTERFACE Interface;
    ULONGLONG Lease;
    volatile LONG Busy;
    BOOLEAN ModuleLocked;
    BOOLEAN Settled;
    BOOLEAN Faulted;
} MTK_SD_POWER;

/* The slot supplies a zeroed, nonpaged context at initialization. */
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS MtkSdPowerInitialize(MTK_SD_POWER *Power);

/*
 * Call from a slot bus operation. Before power-off the host must retire all
 * commands, stop DMA and gate the card clock. Start the card clock only after
 * power-on returns success. Keep the context through orderly slot teardown.
 */
_IRQL_requires_max_(APC_LEVEL)
NTSTATUS MtkSdPowerSet(MTK_SD_POWER *Power, BOOLEAN Enable);

#endif
