/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_CONN_PINS_INTERFACE_H
#define MTK_CONN_PINS_INTERFACE_H
#include <wdm.h>

/* Discover the GPIO target by device interface and query the kernel interface
 * on that target. Keep the interface referenced until all leases are released.
 * Acquire snapshots the CONN pin fields without changing hardware. Read and
 * Write use the stock offsets and masked values. Quiesce CONN and restore each
 * inherited field before Release. If Release fails, keep the token for retry.
 * Both system sleep and orderly removal require releasing the pins first.
 * Surprise removal revokes the lease. Pin operations require PASSIVE_LEVEL. */
static const GUID GUID_DEVINTERFACE_MTK_CONN_PINS =
  {0x50df3f16,0x7311,0x440a,{0xb6,0xc2,0x34,0x5a,0x76,0xff,0x26,0x52}};
static const GUID GUID_MTK_CONN_PINS_INTERFACE_V1 =
  {0x779d8804,0xdd40,0x4815,{0xba,0x4b,0x34,0x5c,0x08,0xbc,0xb7,0x66}};
#define MTK_CONN_PINS_INTERFACE_VERSION 1U
typedef enum { MtkConnPinsGpio=0, MtkConnPinsIoCfg=7 } MTK_CONN_PIN_SPACE;
typedef struct {
  INTERFACE Header;
  NTSTATUS (NTAPI *Acquire)(PVOID Context,PULONGLONG Token);
  NTSTATUS (NTAPI *Read)(PVOID Context,ULONGLONG Token,MTK_CONN_PIN_SPACE Space,ULONG Offset,PULONG Value);
  NTSTATUS (NTAPI *Write)(PVOID Context,ULONGLONG Token,MTK_CONN_PIN_SPACE Space,ULONG Offset,ULONG Value);
  NTSTATUS (NTAPI *Release)(PVOID Context,ULONGLONG Token);
} MTK_CONN_PINS_INTERFACE;
#endif
