// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "HciTransport.h"

// bt_drv_connac1x.ko __bt_write and BT_read pass the H4 stream through
// unchanged on STP channel zero. BTHX packet types are the H4 indicators.
C_ASSERT(HciPacketCommand == 1 && HciPacketAclData == 2 && HciPacketEvent == 4);
C_ASSERT(FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) == 5);
static const BTHX_CAPABILITIES BthxCapabilities = {
    T70_BT_MAX_ACL_BYTES, ScoSupportNone, 0, FALSE, FALSE
};

static USHORT BtHciHeaderBytes(UCHAR Type) {
    switch (Type) {
    case HciPacketCommand: return 3;
    case HciPacketAclData: return 4;
    case HciPacketEvent: return 2;
    default: return 0;
    }
}

// Packet points to the HCI header, excluding the H4 indicator.
static ULONG BtHciPacketBytes(UCHAR Type, const UCHAR *Packet, USHORT Header) {
    ULONG Length = Packet[Header - 1];
    if (Type == HciPacketAclData) Length = Packet[2] | (Length << 8);
    return Header + Length;
}

static NTSTATUS BtHciState(const T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch) {
    if (!Hci || !Epoch) return STATUS_INVALID_PARAMETER;
    if (!Hci->Link || !Hci->Btif || Hci->Epoch != Epoch) return STATUS_DEVICE_NOT_CONNECTED;
    if (Hci->Faulted || Hci->Link->Faulted || Hci->Btif->Faulted) return STATUS_DEVICE_DATA_ERROR;
    return STATUS_SUCCESS;
}

static void BtHciWipeReceive(T70_BT_HCI_TRANSPORT *Hci) {
    RtlSecureZeroMemory(Hci->Receive, sizeof(Hci->Receive));
    RtlSecureZeroMemory(&Hci->Events, sizeof(Hci->Events));
    RtlSecureZeroMemory(&Hci->Acl, sizeof(Hci->Acl));
    Hci->Used = Hci->Expected = 0;
}

static NTSTATUS BtHciFault(T70_BT_HCI_TRANSPORT *Hci, NTSTATUS Status) {
    Hci->Faulted = TRUE;
    BtHciWipeReceive(Hci);
    return Status;
}

NTSTATUS T70BtHciOpen(T70_BT_HCI_TRANSPORT *Hci, MTK_STP_FULL_LINK *Link,
                          MTK_BTIF_CORE *Btif, PULONG Epoch) {
    if (!Hci || !Link || !Btif || !Epoch || !Link->Write) return STATUS_INVALID_PARAMETER;
    if (Hci->Link) return STATUS_DEVICE_BUSY;
    if (Link->Context != Btif || Link->Write != MtkBtifFullStpWrite ||
        Link->Faulted || !Btif->Running || !Btif->Owned || Btif->Faulted)
        return STATUS_DEVICE_NOT_READY;
    if (Hci->Epoch == MAXULONG) return STATUS_DEVICE_NOT_READY;
    Hci->Faulted = FALSE;
    Hci->Link = Link;
    Hci->Btif = Btif;
    *Epoch = ++Hci->Epoch;
    return STATUS_SUCCESS;
}


NTSTATUS T70BtHciClose(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch) {
    if (!Hci || !Epoch) return STATUS_INVALID_PARAMETER;
    if (Hci->Epoch != Epoch) return STATUS_DEVICE_NOT_CONNECTED;
    Hci->Link = NULL;
    Hci->Btif = NULL;
    Hci->TxRemaining = Hci->PendingTxBytes = 0;
    BtHciWipeReceive(Hci);
    return STATUS_SUCCESS;
}

static T70_BT_HCI_QUEUE *BtHciQueue(T70_BT_HCI_TRANSPORT *Hci, UCHAR Type) {
    return Type == HciPacketEvent ? &Hci->Events : &Hci->Acl;
}

static BOOLEAN BtHciPublishPacket(T70_BT_HCI_TRANSPORT *Hci) {
    T70_BT_HCI_QUEUE *Queue;
    T70_BT_HCI_PACKET *Packet;
    if (!Hci->Expected || Hci->Used != Hci->Expected) return TRUE;
    Queue = BtHciQueue(Hci, Hci->Receive[0]);
    if (Queue->Count == T70_BT_PENDING_PACKETS) return FALSE;
    Packet = &Queue->Packets[(Queue->Head + Queue->Count) % T70_BT_PENDING_PACKETS];
    Packet->Length = Hci->Used - 1;
    RtlCopyMemory(Packet->Bytes, Hci->Receive + 1, Packet->Length);
    Queue->Count++;
    RtlSecureZeroMemory(Hci->Receive, Hci->Used);
    Hci->Used = Hci->Expected = 0;
    return TRUE;
}

NTSTATUS T70BtHciReceive(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch,
                        const MTK_STP_FRAME *Frame, PULONG Consumed) {
    NTSTATUS Status = BtHciState(Hci, Epoch);
    if (!Consumed) return STATUS_INVALID_PARAMETER;
    *Consumed = 0;
    if (Status != STATUS_SUCCESS) return Status;
    if (!Frame || Frame->Length > MTK_STP_RX_MAX || (!Frame->Data && Frame->Length))
        return STATUS_INVALID_PARAMETER;
    if (Frame->Channel != T70_BT_STP_CHANNEL) return STATUS_NOT_SUPPORTED;
    if (!BtHciPublishPacket(Hci)) return STATUS_DEVICE_BUSY;

    while (*Consumed < Frame->Length) {
        UCHAR Byte = Frame->Data[(*Consumed)++];
        if (!Hci->Used && Byte != HciPacketAclData && Byte != HciPacketEvent)
            return BtHciFault(Hci, STATUS_DEVICE_DATA_ERROR);
        Hci->Receive[Hci->Used++] = Byte;
        UCHAR Type = Hci->Receive[0];
        USHORT Header = BtHciHeaderBytes(Type);
        if (Hci->Used == Header + 1U) {
            ULONG PacketBytes = BtHciPacketBytes(Type, Hci->Receive + 1, Header);
            if (PacketBytes > T70_BT_MAX_ACL_BYTES)
                return BtHciFault(Hci, STATUS_DEVICE_DATA_ERROR);
            Hci->Expected = (USHORT)(PacketBytes + 1U);
        }
        if (!BtHciPublishPacket(Hci)) return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS T70BtHciRead(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch, UCHAR Type,
                     PBTHX_HCI_READ_WRITE_CONTEXT Output, SIZE_T OutputBytes,
                     SIZE_T *Returned) {
    T70_BT_HCI_QUEUE *Queue;
    T70_BT_HCI_PACKET *Packet;
    SIZE_T Bytes;
    NTSTATUS Status = BtHciState(Hci, Epoch);
    if (!Returned || !Output || (Type != HciPacketAclData && Type != HciPacketEvent))
        return STATUS_INVALID_PARAMETER;
    *Returned = 0;
    if (Status != STATUS_SUCCESS) return Status;
    Queue = BtHciQueue(Hci, Type);
    if (!Queue->Count) return STATUS_NO_MORE_ENTRIES;
    Packet = &Queue->Packets[Queue->Head];
    Bytes = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + Packet->Length;
    if (OutputBytes < Bytes) return STATUS_BUFFER_TOO_SMALL;
    Output->Type = Type;
    Output->DataLen = Packet->Length;
    RtlCopyMemory(Output->Data, Packet->Bytes, Packet->Length);
    *Returned = Bytes;
    RtlSecureZeroMemory(Packet->Bytes, Packet->Length);
    Packet->Length = 0;
    Queue->Head = (Queue->Head + 1U) % T70_BT_PENDING_PACKETS;
    Queue->Count--;
    (void)BtHciPublishPacket(Hci);
    return STATUS_SUCCESS;
}

NTSTATUS T70BtHciWrite(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch,
                      const BTHX_HCI_READ_WRITE_CONTEXT *Input, SIZE_T InputBytes,
                      PULONG Written) {
    MTK_STP_STATUS Sent;
    ULONG Before;
    const SIZE_T Offset = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data);
    NTSTATUS Status = BtHciState(Hci, Epoch);
    if (!Written) return STATUS_INVALID_PARAMETER;
    *Written = 0;
    if (!Input || InputBytes < Offset) return STATUS_INVALID_PARAMETER;
    if (Status != STATUS_SUCCESS) return Status;
    UCHAR Type = Input->Type;
    ULONG Bytes = Input->DataLen;
    USHORT Header = BtHciHeaderBytes(Type);
    if ((Type != HciPacketCommand && Type != HciPacketAclData) ||
        Bytes < Header || Bytes > T70_BT_MAX_ACL_BYTES || InputBytes - Offset < Bytes ||
        BtHciPacketBytes(Type, Input->Data, Header) != Bytes)
        return STATUS_INVALID_PARAMETER;

    Hci->Transmit[0] = Type;
    RtlCopyMemory(Hci->Transmit + 1, Input->Data, Bytes);
    Before = Hci->Btif->Count;
    Sent = MtkStpFullTransmit(Hci->Link, T70_BT_STP_CHANNEL, Hci->Transmit,
                              Bytes + 1U, Hci->NowMs);
    RtlSecureZeroMemory(Hci->Transmit, Bytes + 1U);
    if (Sent == MtkStpFull) return STATUS_DEVICE_BUSY;
    if (Sent != MtkStpOk) return BtHciFault(Hci, STATUS_IO_DEVICE_ERROR);
    if (Hci->Btif->Count < Before) return BtHciFault(Hci, STATUS_DATA_ERROR);
    Hci->TxRemaining = Hci->Btif->Count;
    if (!Hci->TxRemaining) return BtHciFault(Hci, STATUS_DATA_ERROR);
    Hci->PendingTxBytes = Bytes;
    *Written = Bytes;
    return STATUS_SUCCESS;
}

static MTK_BTIF_CORE *BtHciBtif(const T70_BT_HCI_TRANSPORT *Hci) {
    if (!Hci->Link || !Hci->Btif || !Hci->Link->Context ||
        Hci->Link->Context != Hci->Btif || Hci->Link->Write != MtkBtifFullStpWrite ||
        !Hci->Btif->Running || !Hci->Btif->Owned)
        return NULL;
    return Hci->Btif;
}

NTSTATUS T70BtBthxRequest(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch, ULONG Code,
                         const void *Input, SIZE_T InputBytes,
                         void *Output, SIZE_T OutputBytes, SIZE_T *Returned,
                         BOOLEAN ScoBypassVerified) {
    NTSTATUS Status;
    MTK_BTIF_CORE *Btif;
    ULONG Written;
    UCHAR Type;
    SIZE_T Maximum;
    BTHX_VERSION Version = {BTHX_DDI_VERSION_1};

    if (!Returned) return STATUS_INVALID_PARAMETER;
    *Returned = 0;
    // The BTHX child receives these stateless requests before device start.
    switch (Code) {
    case IOCTL_BTHX_GET_VERSION:
        if (!Output || OutputBytes < sizeof(Version)) return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(Output, &Version, sizeof(Version));
        *Returned = sizeof(Version);
        return STATUS_SUCCESS;
    case IOCTL_BTHX_SET_VERSION:
        if (!Input || InputBytes < sizeof(Version)) return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(&Version, Input, sizeof(Version));
        return Version.Version == BTHX_DDI_VERSION_1 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
    case IOCTL_BTHX_QUERY_CAPABILITIES:
        if (!Output || OutputBytes < sizeof(BthxCapabilities)) return STATUS_BUFFER_TOO_SMALL;
        if (BthxCapabilities.ScoSupport == ScoSupportHCIBypass && !ScoBypassVerified) return STATUS_NOT_SUPPORTED;
        RtlCopyMemory(Output, &BthxCapabilities, sizeof(BthxCapabilities));
        *Returned = sizeof(BthxCapabilities);
        return STATUS_SUCCESS;
    default:
        break;
    }

    Status = BtHciState(Hci, Epoch);
    if (Status != STATUS_SUCCESS) return Status;
    Btif = BtHciBtif(Hci);
    if (!Btif) return STATUS_DEVICE_NOT_READY;
    if (Btif->Faulted) return STATUS_DEVICE_DATA_ERROR;
    switch (Code) {
    case IOCTL_BTHX_READ_HCI:
        if (!Input || InputBytes < sizeof(Type)) return STATUS_BUFFER_TOO_SMALL;
        Type = *(const UCHAR *)Input;
        if (Type != HciPacketEvent && Type != HciPacketAclData) return STATUS_INVALID_PARAMETER;
        Maximum = Type == HciPacketEvent ? 257U : T70_BT_MAX_ACL_BYTES;
        if (!Output || OutputBytes < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + Maximum)
            return STATUS_BUFFER_TOO_SMALL;
        return T70BtHciRead(Hci, Epoch, Type, Output, OutputBytes, Returned);
    case IOCTL_BTHX_WRITE_HCI:
        if (!Output || OutputBytes < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
        if (Hci->TxRemaining) return STATUS_DEVICE_BUSY;
        Status = T70BtHciWrite(Hci, Epoch, Input, InputBytes, &Written);
        if (Status != STATUS_SUCCESS) return Status;
        // Completion waits until the accepted full-mode frame has left BTIF.
        return STATUS_PENDING;
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS T70BtBthxService(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch,
                         UCHAR *Rx, ULONG Capacity, MTK_BTIF_SERVICE *Service, PULONG Written) {
    NTSTATUS Status;
    MTK_BTIF_STATUS Result;
    MTK_BTIF_CORE *Btif;

    if (!Written) return STATUS_INVALID_PARAMETER;
    *Written = 0;
    Status = BtHciState(Hci, Epoch);
    if (Status == STATUS_DEVICE_DATA_ERROR) Hci->TxRemaining = Hci->PendingTxBytes = 0;
    if (Status != STATUS_SUCCESS) return Status;
    Btif = BtHciBtif(Hci);
    if (!Btif) return STATUS_DEVICE_NOT_READY;
    Result = MtkBtifService(Btif, Rx, Capacity, Service);
    if (Result == MtkBtifInvalid) return STATUS_INVALID_PARAMETER;
    if (Result != MtkBtifOk && Result != MtkBtifMore && Result != MtkBtifNotOurs) {
        Hci->TxRemaining = Hci->PendingTxBytes = 0;
        return BtHciFault(Hci, STATUS_IO_DEVICE_ERROR);
    }
    if (Hci->TxRemaining && Service->Transmitted >= Hci->TxRemaining) {
        *Written = Hci->PendingTxBytes;
        Hci->TxRemaining = Hci->PendingTxBytes = 0;
    } else if (Hci->TxRemaining) {
        Hci->TxRemaining -= Service->Transmitted;
    }
    return STATUS_SUCCESS;
}

BOOLEAN T70BtBthxCancelWrite(T70_BT_HCI_TRANSPORT *Hci, ULONG Epoch) {
    if (!Hci || !Hci->Link || Hci->Epoch != Epoch || !Hci->PendingTxBytes)
        return FALSE;
    Hci->PendingTxBytes = 0;
    return TRUE;
}
