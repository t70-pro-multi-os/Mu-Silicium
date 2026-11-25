// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_BT_HCI_TRANSPORT_H
#define T70_BT_HCI_TRANSPORT_H

#include <ntddk.h>
#include <bthxddi.h>
#include <Library/MtkBtifCore.h>
#include <Library/MtkStpFull.h>

#define T70_BT_STP_CHANNEL 0U
#define T70_BT_MAX_ACL_BYTES (MTK_STP_TX_MAX-1U)
#define T70_BT_PENDING_PACKETS 4U

typedef struct {
    USHORT Length;
    UCHAR Bytes[T70_BT_MAX_ACL_BYTES];
} T70_BT_HCI_PACKET;

typedef struct {
    ULONG Head, Count;
    T70_BT_HCI_PACKET Packets[T70_BT_PENDING_PACKETS];
} T70_BT_HCI_QUEUE;

typedef struct {
    MTK_STP_FULL_LINK *Link;
    MTK_BTIF_CORE *Btif;
    ULONG Epoch;
    ULONGLONG NowMs;
    BOOLEAN Faulted;
    USHORT Used, Expected;
    ULONG TxRemaining, PendingTxBytes;
    UCHAR Receive[MTK_STP_TX_MAX], Transmit[MTK_STP_TX_MAX];
    T70_BT_HCI_QUEUE Events, Acl;
} T70_BT_HCI_TRANSPORT;

// The transport owner starts with zero-initialized storage and serializes all
// calls with STP. Open binds the post-WMT full-mode stream and a running BTIF
// owner. The caller refreshes NowMs from a monotonic clock before each transmit
// and full-link service. Close discards local packets and revokes the epoch.
NTSTATUS T70BtHciOpen(T70_BT_HCI_TRANSPORT *Hci, MTK_STP_FULL_LINK *Link,
                          MTK_BTIF_CORE *Btif, PULONG Epoch);
NTSTATUS T70BtHciClose(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch);

// Split/coalesced H4 bytes arrive in validated channel-zero STP frames.
// DEVICE_BUSY retains a complete packet until a read frees a slot. Consumed
// identifies the accepted prefix, including the retained packet. Keep and
// retry only the unconsumed suffix before feeding any later bytes.
NTSTATUS T70BtHciReceive(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch,
                        const MTK_STP_FRAME *Frame, PULONG Consumed);

// Buffers must be kernel-accessible and must not alias transport state.
// The caller owns WDF request lifetime, cancellation and power admission.
// Empty/short reads preserve the queue. The IOCTL counts exclude the H4 byte.
NTSTATUS T70BtHciRead(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch, UCHAR Type,
                     PBTHX_HCI_READ_WRITE_CONTEXT Output, SIZE_T OutputBytes,
                     SIZE_T *Returned);
NTSTATUS T70BtHciWrite(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch,
                      const BTHX_HCI_READ_WRITE_CONTEXT *Input, SIZE_T InputBytes,
                      PULONG Written);

// BTHX static version/capability requests do not need an open session; route
// them through the child's non-power-managed queue. HCI reads/writes need a
// live BTIF session. The caller validates kernel origin, device lifetime and
// request buffers. STATUS_NO_MORE_ENTRIES means a read can
// wait for data. STATUS_DEVICE_BUSY means a write can retry after TX progress.
// STATUS_PENDING reserves one write until its entire STP frame enters the FIFO.
// The caller then returns the service result as a ULONG in the write's output.
// SCO bypass must be established by the platform before advertising it.
NTSTATUS T70BtBthxRequest(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch, ULONG Code,
                         const void *Input, SIZE_T InputBytes,
                         void *Output, SIZE_T OutputBytes, SIZE_T *Returned,
                         BOOLEAN ScoBypassVerified);

// Use this for every hardware service pass while a BTHX write is pending.
// A nonzero Written completes the pending write once. A hardware service error
// invalidates the stream and the caller must fail pending requests. RX bytes
// are dispatched separately via the STP decoder, under the same ownership.
NTSTATUS T70BtBthxService(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch,
                         UCHAR *Rx, ULONG Capacity, MTK_BTIF_SERVICE *Service, PULONG Written);

// Cancel an accepted write once. The queued frame still drains in order and
// holds the write slot until then. Close revokes the entire session instead.
BOOLEAN T70BtBthxCancelWrite(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch);

#endif
