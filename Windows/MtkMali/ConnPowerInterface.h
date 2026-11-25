/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_CONN_POWER_INTERFACE_H
#define MTK_CONN_POWER_INTERFACE_H
#include <wdm.h>
#include <Library/MtkConnPower.h>

/* Discover the SoC by its device interface and query its kernel interface.
 * All calls, including reference management, require PASSIVE_LEVEL.
 * Hold a reference for the full lease. Read and write use the stock
 * MTK_CONN_POWER_IO offsets and masked CONN fields in SPM and INFRACFG.
 * SPM offset 0 accepts the stock keyed enable, and INFRACFG 0xc14
 * accepts the CONN status clear. The shared SPM clock stays enabled.
 * A BTIF consumer proves its parent clocks by writing the dedicated Clear
 * alias, even if the gate was inherited on. Read returns the masked status.
 * After quiescing BTIF, restore only a gate inherited off using the Set
 * alias. A mismatched gate keeps the lease active. APDMA is shared.
 * The client owns the contiguous EMI reservation until it restores the
 * inherited fields and releases the lease. A failed release can be retried
 * with the same token. Surprise removal revokes the token. */
static const GUID GUID_DEVINTERFACE_MTK_CONN_POWER =
    {0x0c9eb5e4,0x29e9,0x4c7c,{0xab,0x25,0xa5,0x59,0x1f,0xc1,0xfd,0xfd}};
static const GUID GUID_MTK_CONN_POWER_INTERFACE_V1 =
    {0x7dde8fff,0xc230,0x4723,{0xbf,0xa1,0xea,0x25,0x9b,0x84,0xad,0x7e}};
#define MTK_CONN_POWER_INTERFACE_VERSION 1U
#define MTK_CONN_BTIF_GATE_SET 0x80U
#define MTK_CONN_BTIF_GATE_CLEAR 0x84U
#define MTK_CONN_BTIF_GATE_STATUS 0x90U
#define MTK_CONN_BTIF_GATE_MASK (1U << 31)
typedef struct {
    INTERFACE Header;
    NTSTATUS (NTAPI *Acquire)(PVOID Context,ULONGLONG EmiBase,PULONGLONG Token);
    NTSTATUS (NTAPI *Read)(PVOID Context,ULONGLONG Token,MTK_CONN_REGION Region,ULONG Offset,PULONG Value);
    NTSTATUS (NTAPI *Write)(PVOID Context,ULONGLONG Token,MTK_CONN_REGION Region,ULONG Offset,ULONG Value);
    NTSTATUS (NTAPI *Release)(PVOID Context,ULONGLONG Token);
} MTK_CONN_POWER_INTERFACE;
#endif
