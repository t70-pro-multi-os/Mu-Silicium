// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <wdf.h>
void WdfDriverMiniportUnload(WDFDRIVER);

WDFDRIVER WdfGetDriver();
NTSTATUS WdfDeviceMiniportCreate(WDFDRIVER,WDF_OBJECT_ATTRIBUTES *,PDEVICE_OBJECT,PDEVICE_OBJECT,PDEVICE_OBJECT,WDFDEVICE *);
