/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_GPS_LNA_INTERFACE_H
#define MTK_GPS_LNA_INTERFACE_H
#include <wdm.h>
/* A kernel-only, board-specific interface, obtained with
 * WdfIoTargetQueryForInterface on the GPIO target. Keep the target and interface
 * referenced until all leases have been released; remote-target query-remove,
 * remove-complete and remove-canceled callbacks must release/reacquire it.
 * Surprise removal revokes all leases without touching missing hardware.
 * Acquire/Set/Release require PASSIVE_LEVEL. Reference/Dereference allow <=DISPATCH.
 * Acquire reads the inherited pin state without changing it. Tokens are opaque
 * and never reused during the provider device object's lifetime. Release restores
 * the inherited state; on failure keep the token and retry before GPIO stop.
 * This interface supplies no RF power, WMT startup or location service. Quiesce
 * the GNSS engine before releasing a DSP-controlled pin or entering system sleep.
 */
static const GUID GUID_MTK_GPS_LNA_INTERFACE_V1 =
  {0x01c202c6,0xd659,0x485b,{0xb6,0x84,0x82,0x2c,0x17,0x5a,0x29,0xf8}};
#define MTK_GPS_LNA_INTERFACE_VERSION 1U
#define MTK_GPS_LNA_L1_PIN 25U
#define MTK_GPS_LNA_L5_PIN 26U
/* L5 pinctrl exists in the DT; this does not establish fitted L5 RF hardware. */
typedef enum { MtkGpsLnaOff, MtkGpsLnaDsp, MtkGpsLnaOn } MTK_GPS_LNA_STATE;
typedef NTSTATUS (NTAPI *MTK_GPS_LNA_ACQUIRE)(PVOID Context,ULONG Pin,PULONGLONG Token);
typedef NTSTATUS (NTAPI *MTK_GPS_LNA_SET)(PVOID Context,ULONGLONG Token,MTK_GPS_LNA_STATE State);
typedef NTSTATUS (NTAPI *MTK_GPS_LNA_RELEASE)(PVOID Context,ULONGLONG Token);
typedef struct {
  INTERFACE Header;
  MTK_GPS_LNA_ACQUIRE Acquire;
  MTK_GPS_LNA_SET Set;
  MTK_GPS_LNA_RELEASE Release;
} MTK_GPS_LNA_INTERFACE;
#endif
