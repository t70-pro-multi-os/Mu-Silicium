/* SPDX-License-Identifier: GPL-2.0-only */
/* Little-endian DPMAIF v3 descriptor decoding from the T70 kernel reference. */
#include "DpmaifDescriptor.h"

static uint32_t ReadLe32(const uint8_t *Bytes)
{
    return (uint32_t)Bytes[0] |
           ((uint32_t)Bytes[1] << 8) |
           ((uint32_t)Bytes[2] << 16) |
           ((uint32_t)Bytes[3] << 24);
}

static void WriteLe32(uint8_t *Bytes, uint32_t Value)
{
    Bytes[0] = (uint8_t)Value;
    Bytes[1] = (uint8_t)(Value >> 8);
    Bytes[2] = (uint8_t)(Value >> 16);
    Bytes[3] = (uint8_t)(Value >> 24);
}

static T70_DPMAIF_STATUS ReadEntry(const uint8_t *Entry, size_t Bytes,
                                  size_t Required, uint32_t *Words,
                                  size_t WordCount)
{
    if (!Entry || !Words) return T70DpmaifInvalidParameter;
    if (Bytes != Required || WordCount * sizeof(uint32_t) != Required)
        return T70DpmaifBufferSize;
    for (size_t Index = 0; Index < WordCount; ++Index)
        Words[Index] = ReadLe32(Entry + Index * sizeof(uint32_t));
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifDecodeDataPit(const uint8_t *Entry, size_t Bytes,
                                         T70_DPMAIF_DATA_PIT *Pit)
{
    uint32_t W[4];
    T70_DPMAIF_DATA_PIT Result;
    T70_DPMAIF_STATUS Status;
    if (!Pit) return T70DpmaifInvalidParameter;
    Status = ReadEntry(Entry, Bytes, T70_DPMAIF_DATA_PIT_BYTES, W, 4);
    if (Status != T70DpmaifSuccess) return Status;
    if (W[0] & 1U) return T70DpmaifWrongType;

    Result.PacketType = (uint8_t)(W[0] & 1U);
    Result.Continuation = (uint8_t)((W[0] >> 1) & 1U);
    Result.BufferType = (uint8_t)((W[0] >> 2) & 1U);
    Result.BufferId = (uint16_t)((W[0] >> 3) & 0x1fffU);
    Result.CombinedBufferId = (uint16_t)(Result.BufferId | (((W[3] >> 8) & 7U) << 13));
    Result.DataLength = (uint16_t)(W[0] >> 16);
    Result.DataAddress = ((uint64_t)W[2] << 32) | W[1];
    Result.PitSequence = (uint8_t)W[3];
    Result.HighBufferId = (uint8_t)((W[3] >> 8) & 7U);
    Result.Reserved2 = (uint8_t)((W[3] >> 11) & 0x1fU);
    Result.Ignore = (uint8_t)((W[3] >> 16) & 1U);
    Result.BufferInfo = (uint8_t)((W[3] >> 17) & 3U);
    Result.HeaderOffset = (uint8_t)((W[3] >> 19) & 0x1fU);
    Result.UlqDone = (uint8_t)((W[3] >> 24) & 0x3fU);
    Result.DlqDone = (uint8_t)(W[3] >> 30);
    *Pit = Result;
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifDecodeMessagePit(const uint8_t *Entry, size_t Bytes,
                                            T70_DPMAIF_MESSAGE_PIT *Pit)
{
    uint32_t W[4];
    T70_DPMAIF_MESSAGE_PIT Result;
    T70_DPMAIF_STATUS Status;
    if (!Pit) return T70DpmaifInvalidParameter;
    Status = ReadEntry(Entry, Bytes, T70_DPMAIF_MESSAGE_PIT_BYTES, W, 4);
    if (Status != T70DpmaifSuccess) return Status;
    if (!(W[0] & 1U)) return T70DpmaifWrongType;

    Result.PacketType = (uint8_t)(W[0] & 1U);
    Result.Continuation = (uint8_t)((W[0] >> 1) & 1U);
    Result.Checksum = (uint8_t)((W[0] >> 2) & 3U);
    Result.Error = (uint8_t)((W[0] >> 4) & 1U);
    Result.SourceQueue = (uint8_t)((W[0] >> 5) & 7U);
    Result.HardwareChecksumIndex = (uint8_t)((W[0] >> 8) & 0xfU);
    Result.Reserved0 = (uint8_t)((W[0] >> 12) & 0xfU);
    Result.ChannelId = (uint8_t)(W[0] >> 16);
    Result.NetworkType = (uint8_t)((W[0] >> 24) & 7U);
    Result.Reserved1 = (uint8_t)((W[0] >> 27) & 0xfU);
    Result.DataPath = (uint8_t)(W[0] >> 31);
    Result.CountLow = (uint16_t)W[1];
    Result.Flow = (uint8_t)((W[1] >> 16) & 0x1fU);
    Result.Reserved2 = (uint8_t)((W[1] >> 21) & 7U);
    Result.Command = (uint8_t)((W[1] >> 24) & 7U);
    Result.HighPriorityIndex = (uint8_t)(W[1] >> 27);
    Result.Reserved3 = (uint8_t)(W[2] & 7U);
    Result.VirtualBufferId = (uint16_t)((W[2] >> 3) & 0x1fffU);
    Result.Protocol = (uint8_t)((W[2] >> 16) & 3U);
    Result.Reserved4 = (uint8_t)((W[2] >> 18) & 0x3fU);
    Result.Hash = (uint8_t)(W[2] >> 24);
    Result.PitSequence = (uint8_t)W[3];
    Result.HighBufferId = (uint8_t)((W[3] >> 8) & 7U);
    Result.Reserved5 = (uint8_t)((W[3] >> 11) & 0x1fU);
    Result.Ignore = (uint8_t)((W[3] >> 16) & 1U);
    Result.Reserved6 = (uint8_t)((W[3] >> 17) & 7U);
    Result.MulticastRoute = (uint8_t)((W[3] >> 20) & 3U);
    Result.Reserved7 = (uint8_t)((W[3] >> 22) & 1U);
    Result.IpChecksum = (uint8_t)((W[3] >> 23) & 1U);
    Result.UlqDone = (uint8_t)((W[3] >> 24) & 0x3fU);
    Result.DlqDone = (uint8_t)(W[3] >> 30);
    *Pit = Result;
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifDecodeBat(const uint8_t *Entry, size_t Bytes,
                                     T70_DPMAIF_BAT *Bat)
{
    uint32_t W[2];
    T70_DPMAIF_BAT Result;
    T70_DPMAIF_STATUS Status;
    if (!Bat) return T70DpmaifInvalidParameter;
    Status = ReadEntry(Entry, Bytes, T70_DPMAIF_BAT_BYTES, W, 2);
    if (Status != T70DpmaifSuccess) return Status;
    Result.BufferAddress = ((uint64_t)(W[1] & 0xffU) << 32) | W[0];
    Result.Reserved = W[1] >> 8;
    *Bat = Result;
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifEncodeBat(
    const T70_DPMAIF_BAT *Bat, uint8_t *Entry, size_t Bytes)
{
    uint32_t High;

    if (!Bat || !Entry) return T70DpmaifInvalidParameter;
    if (Bytes != T70_DPMAIF_BAT_BYTES) return T70DpmaifBufferSize;
    if (Bat->BufferAddress >= (UINT64_C(1) << 40) || Bat->Reserved != 0U)
        return T70DpmaifInvalidParameter;

    High = (uint32_t)(Bat->BufferAddress >> 32);
    WriteLe32(Entry, (uint32_t)Bat->BufferAddress);
    WriteLe32(Entry + 4, High);
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifDecodeTxData(const uint8_t *Entry, size_t Bytes,
                                        T70_DPMAIF_TX_DATA *Descriptor)
{
    uint32_t W[2];
    T70_DPMAIF_TX_DATA Result;
    T70_DPMAIF_STATUS Status;
    if (!Descriptor) return T70DpmaifInvalidParameter;
    Status = ReadEntry(Entry, Bytes, T70_DPMAIF_TX_DATA_BYTES, W, 2);
    if (Status != T70DpmaifSuccess) return Status;
    Result.Type = (uint8_t)(W[0] & 3U);
    if (Result.Type != 0U) return T70DpmaifWrongType;
    Result.Continuation = (uint8_t)((W[0] >> 2) & 1U);
    Result.Reserved = (uint8_t)((W[0] >> 3) & 0x1fU);
    Result.AddressExtension = (uint8_t)((W[0] >> 8) & 0xffU);
    Result.DataLength = (uint16_t)(W[0] >> 16);
    Result.AddressLow = W[1];
    Result.DataAddress = ((uint64_t)Result.AddressExtension << 32) | W[1];
    *Descriptor = Result;
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifDecodeTxMessage(const uint8_t *Entry, size_t Bytes,
                                           T70_DPMAIF_TX_MESSAGE *Descriptor)
{
    uint32_t W[2];
    T70_DPMAIF_TX_MESSAGE Result;
    T70_DPMAIF_STATUS Status;
    if (!Descriptor) return T70DpmaifInvalidParameter;
    Status = ReadEntry(Entry, Bytes, T70_DPMAIF_TX_MESSAGE_BYTES, W, 2);
    if (Status != T70DpmaifSuccess) return Status;
    Result.Type = (uint8_t)(W[0] & 3U);
    if (Result.Type != 1U) return T70DpmaifWrongType;
    Result.Continuation = (uint8_t)((W[0] >> 2) & 1U);
    Result.Reserved0 = (uint16_t)((W[0] >> 3) & 0x1fffU);
    Result.PacketLength = (uint16_t)(W[0] >> 16);
    Result.CountLow = (uint16_t)W[1];
    Result.ChannelId = (uint8_t)(W[1] >> 16);
    Result.NetworkType = (uint8_t)((W[1] >> 24) & 7U);
    Result.Reserved1 = (uint8_t)((W[1] >> 27) & 1U);
    Result.Ipv4Checksum = (uint8_t)((W[1] >> 28) & 1U);
    Result.L4Checksum = (uint8_t)((W[1] >> 29) & 1U);
    Result.Reserved2 = (uint8_t)(W[1] >> 30);
    *Descriptor = Result;
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifEncodeTxData(
    const T70_DPMAIF_TX_DATA *Descriptor, uint8_t *Entry, size_t Bytes)
{
    uint32_t W0, W1;
    uint64_t Address;

    if (!Descriptor || !Entry) return T70DpmaifInvalidParameter;
    if (Bytes != T70_DPMAIF_TX_DATA_BYTES) return T70DpmaifBufferSize;
    if (Descriptor->Type != 0U) return T70DpmaifWrongType;
    if (Descriptor->Continuation > 1U || Descriptor->Reserved != 0U ||
        Descriptor->DataLength == 0U) return T70DpmaifInvalidParameter;

    Address = ((uint64_t)Descriptor->AddressExtension << 32) |
        Descriptor->AddressLow;
    if (Descriptor->DataAddress != Address) return T70DpmaifInvalidParameter;

    W0 = (uint32_t)Descriptor->Type |
        ((uint32_t)Descriptor->Continuation << 2) |
        ((uint32_t)Descriptor->AddressExtension << 8) |
        ((uint32_t)Descriptor->DataLength << 16);
    W1 = Descriptor->AddressLow;
    WriteLe32(Entry, W0);
    WriteLe32(Entry + 4, W1);
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifEncodeTxMessage(
    const T70_DPMAIF_TX_MESSAGE *Descriptor, uint8_t *Entry, size_t Bytes)
{
    uint32_t W0, W1;

    if (!Descriptor || !Entry) return T70DpmaifInvalidParameter;
    if (Bytes != T70_DPMAIF_TX_MESSAGE_BYTES) return T70DpmaifBufferSize;
    if (Descriptor->Type != 1U) return T70DpmaifWrongType;
    if (Descriptor->Continuation != 1U || Descriptor->Reserved0 != 0U ||
        Descriptor->Reserved1 != 0U || Descriptor->Reserved2 != 0U ||
        Descriptor->NetworkType > 7U || Descriptor->Ipv4Checksum > 1U ||
        Descriptor->L4Checksum > 1U || Descriptor->PacketLength == 0U) {
        return T70DpmaifInvalidParameter;
    }

    W0 = (uint32_t)Descriptor->Type |
        ((uint32_t)Descriptor->Continuation << 2) |
        ((uint32_t)Descriptor->PacketLength << 16);
    W1 = (uint32_t)Descriptor->CountLow |
        ((uint32_t)Descriptor->ChannelId << 16) |
        ((uint32_t)Descriptor->NetworkType << 24) |
        ((uint32_t)Descriptor->Ipv4Checksum << 28) |
        ((uint32_t)Descriptor->L4Checksum << 29);
    WriteLe32(Entry, W0);
    WriteLe32(Entry + 4, W1);
    return T70DpmaifSuccess;
}

T70_DPMAIF_STATUS T70DpmaifRxWalkInit(T70_DPMAIF_RX_STATE *State)
{
    if (!State) return T70DpmaifInvalidParameter;
    *State = (T70_DPMAIF_RX_STATE){0};
    return T70DpmaifSuccess;
}

/* Stock RX processing takes channel/checksum from MSG PIT entries. A normal
 * PIT with c_bit==0 completes the packet. The caller owns BAT lookup, DMA
 * synchronization, ring indices and packet delivery. */
T70_DPMAIF_STATUS T70DpmaifRxWalkStep(T70_DPMAIF_RX_STATE *State,
                                      const uint8_t *Entry, size_t Bytes,
                                      T70_DPMAIF_RX_EVENT *Event)
{
    uint32_t First;
    T70_DPMAIF_RX_STATE Next;
    T70_DPMAIF_RX_EVENT Result = {0};
    T70_DPMAIF_STATUS Status;
    if (!State || !Event || !Entry) return T70DpmaifInvalidParameter;
    if (Bytes != T70_DPMAIF_DATA_PIT_BYTES) return T70DpmaifBufferSize;
    First = ReadLe32(Entry);
    Next = *State;
    if (First & 1U) {
        T70_DPMAIF_MESSAGE_PIT Message;
        if (Next.PacketSegments) return T70DpmaifSequenceError;
        Status = T70DpmaifDecodeMessagePit(Entry, Bytes, &Message);
        if (Status != T70DpmaifSuccess) return Status;
        Next.HaveMessage = 1U;
        Next.ChannelId = Message.ChannelId;
        Next.Checksum = Message.Checksum;
        Next.DataPath = Message.DataPath;
        Result.Kind = T70DpmaifRxMessage;
        Result.Message = Message;
    } else {
        T70_DPMAIF_DATA_PIT Data;
        if (Next.PacketSegments == UINT32_MAX) return T70DpmaifSequenceError;
        Status = T70DpmaifDecodeDataPit(Entry, Bytes, &Data);
        if (Status != T70DpmaifSuccess) return Status;
        if (Data.DataLength > UINT32_MAX - Next.PacketBytes)
            return T70DpmaifSequenceError;
        ++Next.PacketSegments;
        Next.PacketBytes += Data.DataLength;
        Result.Kind = T70DpmaifRxData;
        Result.Data = Data;
        Result.PacketSegments = Next.PacketSegments;
        Result.PacketBytes = Next.PacketBytes;
        Result.PacketComplete = Data.Continuation == 0U;
        if (Next.HaveMessage) {
            Result.HaveMessage = 1U;
            Result.ChannelId = Next.ChannelId;
            Result.Checksum = Next.Checksum;
            Result.DataPath = Next.DataPath;
        }
        if (Result.PacketComplete) {
            Next.PacketSegments = 0;
            Next.PacketBytes = 0;
            Next.HaveMessage = 0;
        }
    }
    if (Result.Kind == T70DpmaifRxMessage) {
        Result.HaveMessage = 1U;
        Result.ChannelId = Next.ChannelId;
        Result.Checksum = Next.Checksum;
        Result.DataPath = Next.DataPath;
    }
    *State = Next;
    *Event = Result;
    return T70DpmaifSuccess;
}

