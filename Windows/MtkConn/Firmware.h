// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_CONN_FIRMWARE_H
#define T70_CONN_FIRMWARE_H

#include <ntddk.h>
#include <Library/MtkConnRom.h>

typedef struct {
    MTK_CONN_ROM_SET Images[2];
} T70_CONN_FIRMWARE;

/* The caller supplies zeroed storage and serializes calls. Images stay valid
 * until freed. T70ConnStart checks the selected stock images before use. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS T70ConnLoadFirmware(PDRIVER_OBJECT Driver, T70_CONN_FIRMWARE *Firmware);

_IRQL_requires_(PASSIVE_LEVEL)
VOID T70ConnFreeFirmware(T70_CONN_FIRMWARE *Firmware);

#endif
