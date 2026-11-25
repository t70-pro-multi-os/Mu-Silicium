// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_CONN_BTIF_INTERFACE_H
#define T70_CONN_BTIF_INTERFACE_H

#include <ntddk.h>

static const GUID GUID_DEVINTERFACE_T70_CONN_BTIF =
    {0x144355e1,0x3df1,0x4ca5,{0x9a,0x8d,0xc3,0xba,0x9b,0x16,0xf2,0xc1}};
static const GUID GUID_T70_CONN_BTIF_INTERFACE_V1 =
    {0x02e2384f,0x809c,0x4077,{0x91,0x9a,0x38,0x25,0xe1,0x69,0xab,0x56}};
static const GUID GUID_T70_CONN_BTIF_INTERFACE_V2 =
    {0x456dc751,0x1724,0x43f1,{0xa0,0x55,0x90,0x57,0x17,0xd8,0x35,0xa6}};
#define T70_CONN_BTIF_INTERFACE_VERSION 2U

typedef NTSTATUS (NTAPI T70_CONN_BTIF_ACCESS)(PVOID Context);

/*
 * Hold an interface reference from Acquire until Release. All operations run
 * at PASSIVE_LEVEL. Access holds the supplier's hardware lock while it calls
 * the client. Use a bounded callback for register I/O, with no WDF lifecycle
 * calls, waits on other workers or calls back into this interface.
 * Drain client work before Release. Quiesced means the peer and host FIFO can
 * stop. Otherwise withdraw BTIF access and leave clock recovery to CONN Stop.
 * Release consumes the token even if restoration returns an error. PnP
 * resource withdrawal can revoke it first without touching hardware.
 */
typedef struct {
    INTERFACE Header;
    NTSTATUS (NTAPI *Acquire)(PVOID Context, PULONGLONG Token);
    NTSTATUS (NTAPI *Access)(PVOID Context, ULONGLONG Token, T70_CONN_BTIF_ACCESS *Operation,
                             PVOID OperationContext);
    NTSTATUS (NTAPI *Release)(PVOID Context, ULONGLONG Token, BOOLEAN Quiesced);
    /* Fresh is true only for the first claim since the last MCU boot. A later
     * client must know the returned session and retain the full link state. */
    NTSTATUS (NTAPI *ClaimSession)(PVOID Context, ULONGLONG Token, PULONGLONG Session,
                                   BOOLEAN *Fresh);
} T70_CONN_BTIF_INTERFACE;

#endif
