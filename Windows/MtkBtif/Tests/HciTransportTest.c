// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../HciTransport.h"
#include <Library/MtkBtifCore.h>

ULONG T70BtHciAssertions;
#define CHECK(Condition) do { ++T70BtHciAssertions; if (!(Condition)) return __LINE__; } while (0)
#define CONTEXT_HEADER FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data)

// The stand-alone test PE has no operating system runtime.
ULONG_PTR __security_cookie = 0x076d46cdf8a1b32ULL;
void __cdecl __security_check_cookie(ULONG_PTR Cookie) {
    if (Cookie != __security_cookie) __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

static T70_BT_HCI_TRANSPORT Hci;
static MTK_STP_DECODER Decoder;
static MTK_STP_FULL_LINK FullLink, PeerLink;
static MTK_BTIF_CORE Btif;
static ULONG Epoch;
static unsigned Registers[28], SentBytes, RejectTxAt = MAXULONG;
static UCHAR Wire[MTK_STP_FRAME_MAX], Prefix[32], SentWire[MTK_STP_FULL_RX_MAX+16], PeerWire[MTK_STP_FULL_RX_MAX+16], LargePacket[MTK_STP_TX_MAX];
static ULONG PeerWireLength;
static union {
    BTHX_HCI_READ_WRITE_CONTEXT Packet;
    UCHAR Raw[CONTEXT_HEADER + T70_BT_MAX_ACL_BYTES + 1];
} Input, Output;
static const UCHAR ResetWire[] = {0x80, 0, 4, 0, 1, 3, 0x0c, 0, 0, 0};
static const UCHAR ResetEvent[] = {4, 0x0e, 4, 1, 3, 0x0c, 0};
static const UCHAR AclPacket[] = {2, 1, 0x20, 2, 0, 0xb7, 0xf2};

#pragma function(memcpy, memset)
void *memcpy(void *To, const void *From, size_t Length) {
    volatile UCHAR *Target = To;
    const UCHAR *Source = From;
    for (size_t Index = 0; Index < Length; Index++) Target[Index] = Source[Index];
    return To;
}

void *memset(void *To, int Value, size_t Length) {
    volatile UCHAR *Target = To;
    for (size_t Index = 0; Index < Length; Index++) Target[Index] = (UCHAR)Value;
    return To;
}

static BOOLEAN Equal(const UCHAR *A, const UCHAR *B, SIZE_T Length) {
    for (SIZE_T Index = 0; Index < Length; Index++) {
        if (A[Index] != B[Index]) return FALSE;
    }
    return TRUE;
}

static int ReadRegister(void *Context, unsigned Region, unsigned Offset,
                        unsigned Width, unsigned *Value) {
    UNREFERENCED_PARAMETER(Context);
    if ((Region == 1 || Region == 2) && Offset == 8 && Width == 4) {
        *Value = 0;
        return 0;
    }
    if (Region || Width != 4 || Offset % 4 || Offset >= sizeof(Registers)) return 1;
    *Value = Offset == 8 ? Registers[1] & 2U : Registers[Offset / 4];
    return 0;
}

static int WriteRegister(void *Context, unsigned Region, unsigned Offset,
                         unsigned Width, unsigned Value) {
    UNREFERENCED_PARAMETER(Context);
    if (Region) return 1;
    if (Offset == 0 && Width == 1) {
        if (SentBytes == RejectTxAt) return 1;
        if (SentBytes < sizeof(Prefix)) Prefix[SentBytes] = (UCHAR)Value;
        if (SentBytes < sizeof(SentWire)) SentWire[SentBytes] = (UCHAR)Value;
        SentBytes++;
        return 0;
    }
    if (Width != 4 || Offset % 4 || Offset >= sizeof(Registers)) return 1;
    if (Offset != 8) Registers[Offset / 4] = Value;
    return 0;
}

static void Barrier(void *Context) {
    UNREFERENCED_PARAMETER(Context);
}

static MTK_STP_STATUS CaptureFull(void *Context, const UCHAR *Data, unsigned Length, unsigned *Written);

static int StartBtif(void) {
    const MTK_BTIF_IO Io = {NULL, ReadRegister, WriteRegister, Barrier};
    Registers[0x14 / 4] = 0x60;
    CHECK(MtkBtifInit(&Btif, &Io) == MtkBtifOk);
    CHECK(MtkBtifStart(&Btif, MTK_BTIF_TX_TRIGGER, MTK_BTIF_RX_TRIGGER) == MtkBtifOk);
    CHECK(MtkStpDecoderInit(&Decoder, MTK_STP_BTIF_MANDATORY) == MtkStpOk);
    CHECK(MtkStpFullInit(&FullLink, &Btif, MtkBtifFullStpWrite) == MtkStpOk);
    CHECK(MtkStpFullInit(&PeerLink, PeerWire, CaptureFull) == MtkStpOk);
    CHECK(T70BtHciOpen(&Hci, &FullLink, &Btif, &Epoch) == STATUS_SUCCESS);
    Hci.NowMs = 1000;
    return 0;
}

static NTSTATUS WritePacket(UCHAR Type, const UCHAR *Data, ULONG Length, PULONG Sent) {
    Input.Packet.Type = Type;
    Input.Packet.DataLen = Length;
    memcpy(Input.Packet.Data, Data, Length);
    return T70BtHciWrite(&Hci, Epoch, &Input.Packet, CONTEXT_HEADER + Length, Sent);
}

static MTK_STP_STATUS CaptureFull(void *Context, const UCHAR *Data,
                                  unsigned Length, unsigned *Written) {
    if (Context != PeerWire || Length > sizeof(PeerWire)) return MtkStpInvalid;
    memcpy(PeerWire, Data, Length);
    PeerWireLength = Length;
    *Written = Length;
    return MtkStpOk;
}

static int AckFullFrame(ULONG Offset, ULONG Length, const UCHAR *Expected,
                        ULONG ExpectedLength) {
    MTK_STP_FRAME Frame;
    unsigned Consumed;
    CHECK(Offset <= SentBytes && Length <= SentBytes - Offset);
    CHECK(MtkStpFullReceive(&PeerLink, SentWire + Offset, Length, &Consumed, &Frame)
          == MtkStpFrameReady && Consumed == Length);
    CHECK(Frame.Channel == T70_BT_STP_CHANNEL && Frame.Length == ExpectedLength &&
          Equal(Frame.Data, Expected, ExpectedLength));
    CHECK(MtkStpFullAccept(&PeerLink) == MtkStpOk);
    PeerWireLength = 0;
    CHECK(MtkStpFullService(&PeerLink, Hci.NowMs) == MtkStpOk && PeerWireLength != 0);
    CHECK(MtkStpFullReceive(&FullLink, PeerWire, PeerWireLength, &Consumed, &Frame)
          == MtkStpNeedMore && Consumed == PeerWireLength && FullLink.TxLength == 0);
    return 0;
}

static int DrainHciWrite(ULONG ExpectedBytes, ULONG *Offset, ULONG *WireBytes) {
    UCHAR Rx;
    MTK_BTIF_SERVICE Service;
    ULONG Written, Completed = 0;
    *Offset = SentBytes;
    while (Btif.Count) {
        NTSTATUS Status = T70BtBthxService(&Hci, Epoch, &Rx, 1, &Service, &Written);
        CHECK(Status == STATUS_SUCCESS && Service.Transmitted != 0);
        if (Written) Completed = Written;
    }
    *WireBytes = SentBytes - *Offset;
    CHECK(Completed == ExpectedBytes && Hci.TxRemaining == 0);
    return 0;
}

static int TestTransmit(void) {
    static const UCHAR ResetH4[] = {HciPacketCommand, 0x03, 0x0c, 0};
    ULONG Sent, Offset, WireBytes;
    int Line;
    SentBytes = 0;
    CHECK(WritePacket(HciPacketCommand, ResetH4 + 1, 3, &Sent) == STATUS_SUCCESS &&
          Sent == 3 && FullLink.TxLength != 0 && Hci.TxRemaining == Btif.Count);
    CHECK(WritePacket(HciPacketCommand, ResetH4 + 1, 3, &Sent) == STATUS_DEVICE_BUSY &&
          Sent == 0);
    if ((Line = DrainHciWrite(3, &Offset, &WireBytes))) return Line;
    CHECK(WireBytes == SentBytes && WireBytes > sizeof(ResetH4));
    if ((Line = AckFullFrame(Offset, WireBytes, ResetH4, sizeof(ResetH4)))) return Line;

    CHECK(WritePacket(HciPacketEvent, ResetH4 + 1, 3, &Sent) == STATUS_INVALID_PARAMETER);
    CHECK(WritePacket(HciPacketCommand, ResetH4 + 1, 2, &Sent) == STATUS_INVALID_PARAMETER);
    CHECK(WritePacket(HciPacketAclData, AclPacket + 1, 5, &Sent) == STATUS_INVALID_PARAMETER);
    Input.Packet.Type = HciPacketAclData;
    Input.Packet.DataLen = MAXULONG;
    CHECK(T70BtHciWrite(&Hci, Epoch, &Input.Packet, CONTEXT_HEADER + 6, &Sent)
          == STATUS_INVALID_PARAMETER && Sent == 0 && Btif.Count == 0);

    for (ULONG Index = 0; Index < sizeof(LargePacket); Index++) LargePacket[Index] = (UCHAR)Index;
    LargePacket[0] = HciPacketAclData;
    LargePacket[3] = (UCHAR)(T70_BT_MAX_ACL_BYTES - 4);
    LargePacket[4] = (UCHAR)((T70_BT_MAX_ACL_BYTES - 4) >> 8);
    SentBytes = 0;
    CHECK(WritePacket(HciPacketAclData, LargePacket + 1, T70_BT_MAX_ACL_BYTES, &Sent)
          == STATUS_SUCCESS && Sent == T70_BT_MAX_ACL_BYTES);
    CHECK(WritePacket(HciPacketCommand, ResetH4 + 1, 3, &Sent) == STATUS_DEVICE_BUSY);
    if ((Line = DrainHciWrite(T70_BT_MAX_ACL_BYTES, &Offset, &WireBytes))) return Line;
    CHECK(WireBytes <= sizeof(SentWire) && WireBytes > T70_BT_MAX_ACL_BYTES);
    if ((Line = AckFullFrame(Offset, WireBytes, LargePacket, sizeof(LargePacket)))) return Line;
    CHECK(!FullLink.Faulted && !Hci.Faulted);
    CHECK(MtkBtifStop(&Btif) == MtkBtifOk);
    return 0;
}
static int ReceiveStp(const UCHAR *Data, unsigned Length) {
    MTK_STP_FRAME Frame;
    unsigned Encoded, Count;
    ULONG Consumed;
    CHECK(MtkStpEncode(MTK_STP_BTIF_MANDATORY, 0, Data, Length,
                       Wire, sizeof(Wire), &Encoded) == MtkStpOk);
    for (unsigned Index = 0; Index < Encoded; Index++) {
        MTK_STP_STATUS Expected = Index + 1U == Encoded ? MtkStpFrameReady : MtkStpNeedMore;
        CHECK(MtkStpReceive(&Decoder, Wire + Index, 1, &Count, &Frame) == Expected && Count == 1);
    }
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_SUCCESS && Consumed == Length);
    return 0;
}

static int ReadPacket(const UCHAR *H4, SIZE_T Length) {
    SIZE_T Returned;
    CHECK(T70BtHciRead(&Hci, Epoch, H4[0], &Output.Packet, sizeof(Output.Raw), &Returned)
          == STATUS_SUCCESS && Returned == CONTEXT_HEADER + Length - 1U);
    CHECK(Output.Packet.Type == H4[0] && Output.Packet.DataLen == Length - 1U);
    CHECK(Equal(Output.Packet.Data, H4 + 1, Length - 1U) && Output.Raw[Returned] == 0xaa);
    return 0;
}

static int TestReassembly(void) {
    int Line;
    SIZE_T Returned;
    UCHAR Combined[sizeof(ResetEvent) + sizeof(AclPacket)];
    memset(Output.Raw, 0xaa, sizeof(Output.Raw));
    memcpy(Combined, ResetEvent, sizeof(ResetEvent));
    memcpy(Combined + sizeof(ResetEvent), AclPacket, sizeof(AclPacket));
    for (unsigned Split = 1; Split < sizeof(Combined); Split++) {
        if ((Line = ReceiveStp(Combined, Split))) return Line;
        if ((Line = ReceiveStp(Combined + Split, sizeof(Combined) - Split))) return Line;
        CHECK(T70BtHciRead(&Hci, Epoch, HciPacketEvent, &Output.Packet, 10, &Returned)
              == STATUS_BUFFER_TOO_SMALL && Returned == 0);
        if ((Line = ReadPacket(AclPacket, sizeof(AclPacket)))) return Line;
        if ((Line = ReadPacket(ResetEvent, sizeof(ResetEvent)))) return Line;
        CHECK(T70BtHciRead(&Hci, Epoch, HciPacketAclData, &Output.Packet, sizeof(Output.Raw),
                           &Returned) == STATUS_NO_MORE_ENTRIES && Returned == 0);
    }
    if ((Line = ReceiveStp(LargePacket, sizeof(LargePacket)))) return Line;
    if ((Line = ReadPacket(LargePacket, sizeof(LargePacket)))) return Line;
    return 0;
}

typedef struct {
    UCHAR Data[256];
    ULONG Length;
} WMT_FULL_CAPTURE;

static MTK_STP_STATUS CaptureWmtFull(void *Context, const UCHAR *Data,
                                     unsigned Length, unsigned *Written) {
    WMT_FULL_CAPTURE *Capture = Context;
    if (!Capture || !Data || !Written || Length > sizeof(Capture->Data))
        return MtkStpInvalid;
    memcpy(Capture->Data, Data, Length);
    Capture->Length = Length;
    *Written = Length;
    return MtkStpOk;
}

static int TestWmtFullControl(void) {
    static WMT_FULL_CAPTURE HostWire, PeerWireLocal;
    static MTK_STP_FULL_LINK Host, Peer;
    static MTK_WMT_FULL_CONTROL Control;
    MTK_STP_FRAME Frame;
    unsigned Consumed = 0;
    static const UCHAR EnableReply[] = {2, 6, 1, 0, 0};
    static const UCHAR RejectReply[] = {2, 6, 1, 0, 7};
    static const UCHAR FunctionOn[] = {1, 6, 2, 0, 0, 1};
    static const UCHAR FunctionOff[] = {1, 6, 2, 0, 0, 0};
    static const UCHAR OtherWmt[] = {2, 8, 4, 0, 0};

    memset(&HostWire, 0, sizeof(HostWire));
    memset(&PeerWireLocal, 0, sizeof(PeerWireLocal));
    memset(&Host, 0, sizeof(Host)); memset(&Peer, 0, sizeof(Peer));
    memset(&Control, 0, sizeof(Control));
    CHECK(MtkStpFullInit(&Host, &HostWire, CaptureWmtFull) == MtkStpOk);
    CHECK(MtkStpFullInit(&Peer, &PeerWireLocal, CaptureWmtFull) == MtkStpOk);
    CHECK(MtkWmtFullControlInit(&Control, &Host) == MtkWmtOk);
    CHECK(MtkWmtFullControlBegin(&Control, 0, 1, 1000) == MtkWmtPending &&
          Control.Pending && HostWire.Length != 0);
    CHECK(MtkWmtFullControlBegin(&Control, 0, 0, 1000) == MtkWmtBusy);
    CHECK(MtkStpFullReceive(&Peer, HostWire.Data, HostWire.Length, &Consumed, &Frame) ==
          MtkStpFrameReady && Frame.Channel == 4 && Frame.Length == sizeof(FunctionOn) &&
          Equal(Frame.Data, FunctionOn, sizeof(FunctionOn)));
    CHECK(MtkStpFullAccept(&Peer) == MtkStpOk);
    CHECK(MtkStpFullService(&Peer, 1000) == MtkStpOk && PeerWireLocal.Length == 4);
    CHECK(MtkStpFullReceive(&Host, PeerWireLocal.Data, PeerWireLocal.Length,
          &Consumed, &Frame) == MtkStpNeedMore && !Host.TxLength);
    PeerWireLocal.Length = 0;

    CHECK(MtkWmtFullControlEvent(&Control,
          &(MTK_STP_FRAME){4, sizeof(OtherWmt), OtherWmt}) == MtkWmtUnrelated && Control.Pending);
    CHECK(MtkStpFullTransmit(&Peer, 4, EnableReply, sizeof(EnableReply), 1001) == MtkStpOk);
    CHECK(MtkStpFullReceive(&Host, PeerWireLocal.Data, PeerWireLocal.Length,
          &Consumed, &Frame) == MtkStpFrameReady && Frame.Channel == 4 &&
          Equal(Frame.Data, EnableReply, sizeof(EnableReply)));
    CHECK(MtkWmtFullControlEvent(&Control, &Frame) == MtkWmtOk &&
          Control.Confirmed && !Control.Pending && !Control.Faulted &&
          Control.FirmwareStatus == 0);
    CHECK(MtkStpFullAccept(&Host) == MtkStpOk);
    CHECK(MtkStpFullService(&Host, 1001) == MtkStpOk && HostWire.Length == 4);
    CHECK(MtkStpFullReceive(&Peer, HostWire.Data, HostWire.Length,
          &Consumed, &Frame) == MtkStpNeedMore && !Peer.TxLength);
    HostWire.Length = 0; PeerWireLocal.Length = 0;

    CHECK(MtkWmtFullControlBegin(&Control, 0, 0, 1002) == MtkWmtPending &&
          !Control.Confirmed && Control.Pending);
    CHECK(MtkStpFullReceive(&Peer, HostWire.Data, HostWire.Length,
          &Consumed, &Frame) == MtkStpFrameReady && Frame.Channel == 4 &&
          Frame.Length == sizeof(FunctionOff) && Equal(Frame.Data, FunctionOff, sizeof(FunctionOff)));
    CHECK(MtkStpFullAccept(&Peer) == MtkStpOk);
    CHECK(MtkStpFullService(&Peer, 1002) == MtkStpOk);
    CHECK(MtkStpFullReceive(&Host, PeerWireLocal.Data, PeerWireLocal.Length,
          &Consumed, &Frame) == MtkStpNeedMore && !Host.TxLength);
    PeerWireLocal.Length = 0;
    CHECK(MtkStpFullTransmit(&Peer, 4, RejectReply, sizeof(RejectReply), 1003) == MtkStpOk);
    CHECK(MtkStpFullReceive(&Host, PeerWireLocal.Data, PeerWireLocal.Length,
          &Consumed, &Frame) == MtkStpFrameReady && Frame.Channel == 4 &&
          Equal(Frame.Data, RejectReply, sizeof(RejectReply)));
    CHECK(MtkWmtFullControlEvent(&Control, &Frame) == MtkWmtRejected &&
          !Control.Pending && !Control.Confirmed && Control.Faulted &&
          Control.FirmwareStatus == 7);
    CHECK(MtkStpFullAccept(&Host) == MtkStpOk);
    CHECK(MtkStpFullService(&Host, 1003) == MtkStpOk);
    CHECK(MtkStpFullReceive(&Peer, HostWire.Data, HostWire.Length,
          &Consumed, &Frame) == MtkStpNeedMore && !Peer.TxLength);
    HostWire.Length = 0; PeerWireLocal.Length = 0;

    CHECK(MtkWmtFullControlInit(&Control, &Host) == MtkWmtOk);
    CHECK(MtkWmtFullControlBegin(&Control, 0, 1, 2000) == MtkWmtPending);
    CHECK(MtkStpFullReceive(&Peer, HostWire.Data, HostWire.Length,
          &Consumed, &Frame) == MtkStpFrameReady && Frame.Channel == 4 &&
          Frame.Length == sizeof(FunctionOn) && Equal(Frame.Data, FunctionOn, sizeof(FunctionOn)));
    PeerWireLocal.Length = 0;
    CHECK(MtkStpFullAccept(&Peer) == MtkStpOk);
    CHECK(MtkStpFullService(&Peer, 2000) == MtkStpOk);
    CHECK(MtkStpFullReceive(&Host, PeerWireLocal.Data, PeerWireLocal.Length,
          &Consumed, &Frame) == MtkStpNeedMore && !Host.TxLength);
    CHECK(MtkWmtFullControlExpire(&Control, 3999) == MtkWmtPending && Control.Pending);
    CHECK(MtkWmtFullControlExpire(&Control, 4000) == MtkWmtUncertain &&
          !Control.Pending && !Control.Confirmed && Control.Faulted);
    CHECK(MtkWmtFullControlBegin(&Control, 0, 0, 4001) == MtkWmtUncertain);
    return 0;
}

static int TestBthxBeforeTransportStart(void) {
    BTHX_VERSION Version = {0};
    BTHX_CAPABILITIES Capabilities;
    UCHAR Type = HciPacketEvent;
    SIZE_T Returned = 99;

    CHECK(T70BtBthxRequest(NULL, 0, IOCTL_BTHX_GET_VERSION, NULL, 0,
          &Version, sizeof(Version), &Returned, FALSE) == STATUS_SUCCESS &&
          Returned == sizeof(Version) && Version.Version == BTHX_DDI_VERSION_1);
    CHECK(T70BtBthxRequest(NULL, 0, IOCTL_BTHX_SET_VERSION, &Version, sizeof(Version),
          NULL, 0, &Returned, FALSE) == STATUS_SUCCESS && !Returned);
    CHECK(T70BtBthxRequest(NULL, 0, IOCTL_BTHX_QUERY_CAPABILITIES, NULL, 0,
          &Capabilities, sizeof(Capabilities), &Returned, FALSE) == STATUS_SUCCESS &&
          Returned == sizeof(Capabilities) && Capabilities.ScoSupport == ScoSupportNone &&
          !Capabilities.MaxScoChannels);
    CHECK(T70BtBthxRequest(NULL, 0, IOCTL_BTHX_QUERY_CAPABILITIES, NULL, 0,
          &Capabilities, sizeof(Capabilities), &Returned, TRUE) == STATUS_SUCCESS &&
          Returned == sizeof(Capabilities) && !Capabilities.IsDeviceIdleCapable &&
          !Capabilities.IsDeviceWakeCapable);
    CHECK(T70BtBthxRequest(NULL, 0, IOCTL_BTHX_READ_HCI, &Type, sizeof(Type),
          &Output.Packet, sizeof(Output.Raw), &Returned, FALSE) == STATUS_INVALID_PARAMETER &&
          !Returned);
    return 0;
}

static int TestBthxBuffers(void) {
    BTHX_VERSION Version = {BTHX_DDI_VERSION_1};
    BTHX_CAPABILITIES Capabilities;
    UCHAR Type = HciPacketEvent;
    SIZE_T Returned;
    int Line;

    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_GET_VERSION, NULL, 0,
          &Version, sizeof(Version) - 1, &Returned, FALSE) == STATUS_BUFFER_TOO_SMALL && !Returned);
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_GET_VERSION, NULL, 0,
          &Version, sizeof(Version), &Returned, FALSE) == STATUS_SUCCESS &&
          Returned == sizeof(Version) && Version.Version == BTHX_DDI_VERSION_1);
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_SET_VERSION, &Version, sizeof(Version),
          NULL, 0, &Returned, FALSE) == STATUS_SUCCESS && !Returned);
    Version.Version++;
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_SET_VERSION, &Version, sizeof(Version),
          NULL, 0, &Returned, FALSE) == STATUS_NOT_SUPPORTED && !Returned);
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_QUERY_CAPABILITIES, NULL, 0,
          &Capabilities, sizeof(Capabilities), &Returned, FALSE) == STATUS_SUCCESS &&
          Returned == sizeof(Capabilities) && Capabilities.ScoSupport == ScoSupportNone &&
          !Capabilities.MaxScoChannels);
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_QUERY_CAPABILITIES, NULL, 0,
          &Capabilities, sizeof(Capabilities), &Returned, TRUE) == STATUS_SUCCESS &&
          Returned == sizeof(Capabilities) && Capabilities.MaxAclTransferInSize == T70_BT_MAX_ACL_BYTES &&
          Capabilities.ScoSupport == ScoSupportNone && Capabilities.MaxScoChannels == 0 &&
          !Capabilities.IsDeviceIdleCapable && !Capabilities.IsDeviceWakeCapable);
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_READ_HCI, &Type, sizeof(Type),
          &Output.Packet, sizeof(Output.Raw), &Returned, FALSE) == STATUS_NO_MORE_ENTRIES && !Returned);
    if ((Line = ReceiveStp(ResetEvent, sizeof(ResetEvent)))) return Line;
    if ((Line = ReceiveStp(AclPacket, sizeof(AclPacket)))) return Line;
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_READ_HCI, &Type, sizeof(Type),
          &Output.Packet, CONTEXT_HEADER + 256, &Returned, FALSE) == STATUS_BUFFER_TOO_SMALL &&
          !Returned && Hci.Events.Count == 1 && Hci.Acl.Count == 1);
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_READ_HCI, &Type, sizeof(Type),
          &Output.Packet, CONTEXT_HEADER + 257, &Returned, FALSE) == STATUS_SUCCESS &&
          Returned == CONTEXT_HEADER + sizeof(ResetEvent) - 1 && !Hci.Events.Count && Hci.Acl.Count == 1 &&
          Output.Packet.Type == HciPacketEvent && Equal(Output.Packet.Data, ResetEvent + 1, sizeof(ResetEvent) - 1));
    Type = HciPacketAclData;
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_READ_HCI, &Type, sizeof(Type),
          &Output.Packet, CONTEXT_HEADER + T70_BT_MAX_ACL_BYTES - 1, &Returned, FALSE)
          == STATUS_BUFFER_TOO_SMALL && !Returned && Hci.Acl.Count == 1);
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_READ_HCI, &Type, sizeof(Type),
          &Output.Packet, sizeof(Output.Raw), &Returned, FALSE) == STATUS_SUCCESS &&
          Returned == CONTEXT_HEADER + sizeof(AclPacket) - 1 && !Hci.Acl.Count &&
          Output.Packet.Type == HciPacketAclData && Equal(Output.Packet.Data, AclPacket + 1, sizeof(AclPacket) - 1));
    Type = HciPacketCommand;
    CHECK(T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_READ_HCI, &Type, sizeof(Type),
          &Output.Packet, sizeof(Output.Raw), &Returned, FALSE) == STATUS_INVALID_PARAMETER && !Returned);
    CHECK(T70BtBthxRequest(&Hci, Epoch, 0, NULL, 0, NULL, 0, &Returned, FALSE)
          == STATUS_INVALID_DEVICE_REQUEST && !Returned);
    return 0;
}

static NTSTATUS SubmitBthxWrite(SIZE_T OutputLength, SIZE_T *Returned) {
    Input.Packet.Type = HciPacketCommand;
    Input.Packet.DataLen = 3;
    memcpy(Input.Packet.Data, ResetWire + 5, 3);
    return T70BtBthxRequest(&Hci, Epoch, IOCTL_BTHX_WRITE_HCI, &Input.Packet,
                            CONTEXT_HEADER + 3, Output.Raw, OutputLength, Returned, FALSE);
}

static int TestBthxWrite(void) {
    static const UCHAR ResetH4[] = {HciPacketCommand, 0x03, 0x0c, 0};
    SIZE_T Returned;
    ULONG Offset, WireBytes;
    int Line;
    SentBytes = 0;
    Output.Raw[0] = 0x5a;
    CHECK(SubmitBthxWrite(sizeof(ULONG) - 1, &Returned) == STATUS_BUFFER_TOO_SMALL &&
          !Returned && !Btif.Count && !Hci.TxRemaining);
    CHECK(SubmitBthxWrite(sizeof(ULONG), &Returned) == STATUS_PENDING && !Returned &&
          Hci.TxRemaining == Btif.Count && Output.Raw[0] == 0x5a);
    CHECK(SubmitBthxWrite(sizeof(ULONG), &Returned) == STATUS_DEVICE_BUSY && !Returned);
    CHECK(T70BtBthxCancelWrite(&Hci, Epoch));
    CHECK(!T70BtBthxCancelWrite(&Hci, Epoch));
    if ((Line = DrainHciWrite(0, &Offset, &WireBytes))) return Line;
    if ((Line = AckFullFrame(Offset, WireBytes, ResetH4, sizeof(ResetH4)))) return Line;

    SentBytes = 0;
    CHECK(SubmitBthxWrite(sizeof(ULONG), &Returned) == STATUS_PENDING);
    if ((Line = DrainHciWrite(3, &Offset, &WireBytes))) return Line;
    if ((Line = AckFullFrame(Offset, WireBytes, ResetH4, sizeof(ResetH4)))) return Line;
    return 0;
}
static int TestReceivePressure(void) {
    UCHAR Events[(T70_BT_PENDING_PACKETS + 2U) * 4U];
    ULONG Consumed;
    int Line;
    for (ULONG Index = 0; Index < sizeof(Events) / 4; Index++) {
        Events[Index * 4] = HciPacketEvent;
        Events[Index * 4 + 1] = 0xff;
        Events[Index * 4 + 2] = 1;
        Events[Index * 4 + 3] = (UCHAR)Index;
    }
    MTK_STP_FRAME Frame = {0, sizeof(Events), Events};
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_DEVICE_BUSY
          && Consumed == sizeof(Events) - 4U);
    Frame.Data += Consumed;
    Frame.Length -= Consumed;
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_DEVICE_BUSY && !Consumed);
    memset(Output.Raw, 0xaa, sizeof(Output.Raw));
    if ((Line = ReadPacket(Events, 4))) return Line;
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_DEVICE_BUSY && Consumed == 4);
    for (ULONG Index = 1; Index < sizeof(Events) / 4; Index++) {
        if ((Line = ReadPacket(Events + Index * 4, 4))) return Line;
    }
    CHECK(Hci.Used == 0 && Hci.Events.Count == 0);
    return 0;
}

static int TestEpochAndFaults(void) {
    const UCHAR TooLong[] = {HciPacketAclData, 1, 0, 0xff, 0xff};
    ULONG Previous = Epoch, Consumed, Sent;
    SIZE_T Returned;
    MTK_STP_FRAME Frame = {4, sizeof(ResetEvent), ResetEvent};
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_NOT_SUPPORTED &&
          !Consumed && !Hci.Faulted);
    Frame.Channel = 0;
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_SUCCESS);
    Frame.Length = 2;
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_SUCCESS);
    CHECK(T70BtHciClose(&Hci, Epoch) == STATUS_SUCCESS && !Hci.Used && !Hci.Events.Count);
    CHECK(MtkBtifStop(&Btif) == MtkBtifOk);
    RtlSecureZeroMemory(&Btif, sizeof(Btif));
    RtlSecureZeroMemory(Registers, sizeof(Registers));
    RejectTxAt = MAXULONG;
    if (StartBtif()) return __LINE__;
    CHECK(Epoch != Previous);
    CHECK(T70BtHciClose(&Hci, Previous) == STATUS_DEVICE_NOT_CONNECTED);
    CHECK(T70BtHciReceive(&Hci, Previous, &Frame, &Consumed) == STATUS_DEVICE_NOT_CONNECTED &&
          !Consumed && !Hci.Used);
    CHECK(T70BtHciRead(&Hci, Epoch, HciPacketEvent, &Output.Packet, sizeof(Output.Raw), &Returned)
          == STATUS_NO_MORE_ENTRIES && Returned == 0);
    Frame.Length = sizeof(TooLong);
    Frame.Data = TooLong;
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_DEVICE_DATA_ERROR &&
          Consumed == sizeof(TooLong) && !Hci.Used);
    CHECK(T70BtHciReceive(&Hci, Epoch, &Frame, &Consumed) == STATUS_DEVICE_DATA_ERROR && !Consumed);
    CHECK(T70BtHciClose(&Hci, Epoch) == STATUS_SUCCESS);
    CHECK(MtkBtifStop(&Btif) == MtkBtifOk);
    RtlSecureZeroMemory(&Btif, sizeof(Btif));
    RtlSecureZeroMemory(Registers, sizeof(Registers));
    if (StartBtif()) return __LINE__;
    SentBytes = 0;
    RejectTxAt = 2;
    CHECK(WritePacket(HciPacketCommand, ResetEvent + 4, 3, &Sent) == STATUS_SUCCESS && Sent == 3);
    UCHAR Rx;
    MTK_BTIF_SERVICE Service;
    ULONG Written = 0;
    CHECK(T70BtBthxService(&Hci, Epoch, &Rx, 1, &Service, &Written) == STATUS_IO_DEVICE_ERROR &&
          !Written && Service.Transmitted == 2 && Btif.Uncertain && Hci.Faulted);
    CHECK(WritePacket(HciPacketCommand, ResetEvent + 4, 3, &Sent) == STATUS_DEVICE_DATA_ERROR);
    CHECK(T70BtHciClose(&Hci, Epoch) == STATUS_SUCCESS);
    return 0;
}

int TestMain(void) {
    int Line;
    if ((Line = TestWmtFullControl())) return Line;
    if ((Line = TestBthxBeforeTransportStart())) return Line;
    if ((Line = StartBtif())) return Line;
    if ((Line = TestTransmit())) return Line;
    if ((Line = TestReassembly())) return Line;
    if ((Line = TestReceivePressure())) return Line;
    CHECK(T70BtHciClose(&Hci, Epoch) == STATUS_SUCCESS);
    if ((Line = StartBtif())) return Line;
    if ((Line = TestBthxBuffers())) return Line;
    if ((Line = TestBthxWrite())) return Line;
    return TestEpochAndFaults();
}
