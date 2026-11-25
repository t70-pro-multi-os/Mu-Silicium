/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "DpmaifDescriptor.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static void PutLe32(uint8_t *Bytes, uint32_t Value)
{
    Bytes[0] = (uint8_t)Value;
    Bytes[1] = (uint8_t)(Value >> 8);
    Bytes[2] = (uint8_t)(Value >> 16);
    Bytes[3] = (uint8_t)(Value >> 24);
}

static void TestDataPit(void)
{
    uint8_t Bytes[16];
    T70_DPMAIF_DATA_PIT Pit;
    uint32_t W0 = (1U << 1) | (1U << 2) | (0x123U << 3) | (0x4567U << 16);
    uint32_t W3 = 0xa5U | (5U << 8) | (0x12U << 11) | (1U << 16) |
                  (2U << 17) | (0x1bU << 19) | (0x2aU << 24) | (3U << 30);
    PutLe32(Bytes, W0); PutLe32(Bytes + 4, 0x89abcdefU);
    PutLe32(Bytes + 8, 0x00000012U); PutLe32(Bytes + 12, W3);
    memset(&Pit, 0xa5, sizeof(Pit));
    CHECK(T70DpmaifDecodeDataPit(Bytes, sizeof(Bytes), &Pit) == T70DpmaifSuccess);
    CHECK(Pit.PacketType == 0U && Pit.Continuation == 1U && Pit.BufferType == 1U);
    CHECK(Pit.BufferId == 0x123U && Pit.CombinedBufferId == 0xa123U &&
          Pit.DataLength == 0x4567U);
    CHECK(Pit.DataAddress == UINT64_C(0x1289abcdef));
    CHECK(Pit.PitSequence == 0xa5U && Pit.HighBufferId == 5U && Pit.Reserved2 == 0x12U);
    CHECK(Pit.Ignore == 1U && Pit.BufferInfo == 2U && Pit.HeaderOffset == 0x1bU);
    CHECK(Pit.UlqDone == 0x2aU && Pit.DlqDone == 3U);

    T70_DPMAIF_DATA_PIT Before = Pit;
    W0 |= 1U;
    PutLe32(Bytes, W0);
    CHECK(T70DpmaifDecodeDataPit(Bytes, sizeof(Bytes), &Pit) == T70DpmaifWrongType);
    CHECK(memcmp(&Pit, &Before, sizeof(Pit)) == 0);
    CHECK(T70DpmaifDecodeDataPit(Bytes, sizeof(Bytes) - 1U, &Pit) == T70DpmaifBufferSize);
    CHECK(memcmp(&Pit, &Before, sizeof(Pit)) == 0);
    CHECK(T70DpmaifDecodeDataPit(NULL, sizeof(Bytes), &Pit) == T70DpmaifInvalidParameter);
    CHECK(T70DpmaifDecodeDataPit(Bytes, sizeof(Bytes), NULL) == T70DpmaifInvalidParameter);
}

static void TestMessagePit(void)
{
    uint8_t Bytes[16];
    T70_DPMAIF_MESSAGE_PIT Pit;
    uint32_t W0 = 1U | (1U << 1) | (2U << 2) | (1U << 4) | (5U << 5) |
                  (0xaU << 8) | (0xbU << 12) | (0x77U << 16) |
                  (6U << 24) | (9U << 27) | (1U << 31);
    uint32_t W1 = 0x5a3cU | (0x12U << 16) | (5U << 21) | (6U << 24) | (0x1bU << 27);
    uint32_t W2 = 5U | (0x1234U << 3) | (2U << 16) | (0x2aU << 18) | (0x5cU << 24);
    uint32_t W3 = 0x96U | (3U << 8) | (0x13U << 11) | (1U << 16) |
                  (5U << 17) | (2U << 20) | (1U << 22) | (1U << 23) |
                  (0x2dU << 24) | (2U << 30);
    PutLe32(Bytes, W0); PutLe32(Bytes + 4, W1); PutLe32(Bytes + 8, W2); PutLe32(Bytes + 12, W3);
    CHECK(T70DpmaifDecodeMessagePit(Bytes, sizeof(Bytes), &Pit) == T70DpmaifSuccess);
    CHECK(Pit.PacketType == 1U && Pit.Continuation == 1U && Pit.Checksum == 2U &&
          Pit.Error == 1U && Pit.SourceQueue == 5U && Pit.HardwareChecksumIndex == 0xaU);
    CHECK(Pit.Reserved0 == 0xbU && Pit.ChannelId == 0x77U && Pit.NetworkType == 6U &&
          Pit.Reserved1 == 9U && Pit.DataPath == 1U);
    CHECK(Pit.CountLow == 0x5a3cU && Pit.Flow == 0x12U && Pit.Reserved2 == 5U &&
          Pit.Command == 6U && Pit.HighPriorityIndex == 0x1bU);
    CHECK(Pit.Reserved3 == 5U && Pit.VirtualBufferId == 0x1234U && Pit.Protocol == 2U &&
          Pit.Reserved4 == 0x2aU && Pit.Hash == 0x5cU);
    CHECK(Pit.PitSequence == 0x96U && Pit.HighBufferId == 3U && Pit.Reserved5 == 0x13U &&
          Pit.Ignore == 1U && Pit.Reserved6 == 5U && Pit.MulticastRoute == 2U &&
          Pit.Reserved7 == 1U && Pit.IpChecksum == 1U && Pit.UlqDone == 0x2dU &&
          Pit.DlqDone == 2U);

    T70_DPMAIF_MESSAGE_PIT Before = Pit;
    PutLe32(Bytes, W0 & ~1U);
    CHECK(T70DpmaifDecodeMessagePit(Bytes, sizeof(Bytes), &Pit) == T70DpmaifWrongType);
    CHECK(memcmp(&Pit, &Before, sizeof(Pit)) == 0);
    CHECK(T70DpmaifDecodeMessagePit(Bytes, 15U, &Pit) == T70DpmaifBufferSize);
    CHECK(memcmp(&Pit, &Before, sizeof(Pit)) == 0);
}

static void TestRxWalk(void)
{
    uint8_t Message[16], Data[16];
    T70_DPMAIF_RX_STATE State = {0};
    T70_DPMAIF_RX_EVENT Event = {0};
    uint32_t MessageWord = 1U | (3U << 2) | (24U << 16) | (1U << 31);

    CHECK(T70DpmaifRxWalkInit(&State) == T70DpmaifSuccess);
    PutLe32(Message, MessageWord); PutLe32(Message + 4, 0); PutLe32(Message + 8, 0);
    PutLe32(Message + 12, 0);
    CHECK(T70DpmaifRxWalkStep(&State, Message, sizeof(Message), &Event) == T70DpmaifSuccess);
    CHECK(Event.Kind == T70DpmaifRxMessage && Event.HaveMessage &&
          Event.ChannelId == 24U && Event.Checksum == 3U && Event.DataPath == 1U);

    PutLe32(Data, (1U << 1) | (2U << 3) | (512U << 16));
    PutLe32(Data + 4, 0x1000U); PutLe32(Data + 8, 0U); PutLe32(Data + 12, 0U);
    CHECK(T70DpmaifRxWalkStep(&State, Data, sizeof(Data), &Event) == T70DpmaifSuccess);
    CHECK(Event.Kind == T70DpmaifRxData && Event.HaveMessage &&
          Event.ChannelId == 24U && !Event.PacketComplete &&
          Event.PacketSegments == 1U && Event.PacketBytes == 512U);

    PutLe32(Data, (7U << 3) | (128U << 16));
    PutLe32(Data + 4, 0x2000U);
    CHECK(T70DpmaifRxWalkStep(&State, Data, sizeof(Data), &Event) == T70DpmaifSuccess);
    CHECK(Event.PacketComplete && Event.PacketSegments == 2U &&
          Event.PacketBytes == 640U && Event.ChannelId == 24U);
    CHECK(!State.PacketSegments && !State.PacketBytes && !State.HaveMessage);

    State.PacketSegments = 1U; State.PacketBytes = 10U;
    T70_DPMAIF_RX_STATE BeforeState = State;
    T70_DPMAIF_RX_EVENT BeforeEvent = Event;
    CHECK(T70DpmaifRxWalkStep(&State, Message, sizeof(Message), &Event) ==
          T70DpmaifSequenceError);
    CHECK(memcmp(&State, &BeforeState, sizeof(State)) == 0 &&
          memcmp(&Event, &BeforeEvent, sizeof(Event)) == 0);
    CHECK(T70DpmaifRxWalkStep(&State, Data, 15U, &Event) == T70DpmaifBufferSize);
    CHECK(memcmp(&State, &BeforeState, sizeof(State)) == 0 &&
          memcmp(&Event, &BeforeEvent, sizeof(Event)) == 0);
    CHECK(T70DpmaifRxWalkInit(NULL) == T70DpmaifInvalidParameter);

    State.PacketSegments = 2U; State.PacketBytes = UINT32_MAX - 127U;
    BeforeState = State; BeforeEvent = Event;
    CHECK(T70DpmaifRxWalkStep(&State, Data, sizeof(Data), &Event) ==
          T70DpmaifSequenceError);
    CHECK(memcmp(&State, &BeforeState, sizeof(State)) == 0 &&
          memcmp(&Event, &BeforeEvent, sizeof(Event)) == 0);
    State.PacketSegments = UINT32_MAX; State.PacketBytes = 0;
    BeforeState = State; BeforeEvent = Event;
    CHECK(T70DpmaifRxWalkStep(&State, Data, sizeof(Data), &Event) ==
          T70DpmaifSequenceError);
    CHECK(memcmp(&State, &BeforeState, sizeof(State)) == 0 &&
          memcmp(&Event, &BeforeEvent, sizeof(Event)) == 0);
}

static void TestBatAndTx(void)
{
    uint8_t BatBytes[8], TxDataBytes[8], TxMessageBytes[8];
    T70_DPMAIF_BAT Bat;
    T70_DPMAIF_TX_DATA TxData;
    T70_DPMAIF_TX_MESSAGE TxMessage;
    PutLe32(BatBytes, 0x12345678U); PutLe32(BatBytes + 4, 0x000000abU);
    CHECK(T70DpmaifDecodeBat(BatBytes, sizeof(BatBytes), &Bat) == T70DpmaifSuccess);
    CHECK(Bat.BufferAddress == UINT64_C(0xab12345678) && Bat.Reserved == 0U);
    PutLe32(BatBytes + 4, 0x123456abU);
    CHECK(T70DpmaifDecodeBat(BatBytes, sizeof(BatBytes), &Bat) == T70DpmaifSuccess);
    CHECK(Bat.BufferAddress == UINT64_C(0xab12345678) && Bat.Reserved == 0x123456U);
    CHECK(T70DpmaifDecodeBat(BatBytes, 7U, &Bat) == T70DpmaifBufferSize);
    {
        uint8_t EncodedBat[8], BeforeBat[8];
        T70_DPMAIF_BAT ToEncode = {UINT64_C(0xab12345678), 0};
        CHECK(T70DpmaifEncodeBat(&ToEncode, EncodedBat, sizeof(EncodedBat)) ==
              T70DpmaifSuccess);
        CHECK(T70DpmaifDecodeBat(EncodedBat, sizeof(EncodedBat), &Bat) ==
              T70DpmaifSuccess);
        CHECK(Bat.BufferAddress == ToEncode.BufferAddress && Bat.Reserved == 0U);
        memcpy(BeforeBat, EncodedBat, sizeof(BeforeBat));
        ToEncode.BufferAddress = UINT64_C(1) << 40;
        CHECK(T70DpmaifEncodeBat(&ToEncode, EncodedBat, sizeof(EncodedBat)) ==
              T70DpmaifInvalidParameter);
        CHECK(memcmp(BeforeBat, EncodedBat, sizeof(BeforeBat)) == 0);
        ToEncode.BufferAddress = UINT64_C(0x1234);
        ToEncode.Reserved = 1U;
        CHECK(T70DpmaifEncodeBat(&ToEncode, EncodedBat, sizeof(EncodedBat)) ==
              T70DpmaifInvalidParameter);
        CHECK(memcmp(BeforeBat, EncodedBat, sizeof(BeforeBat)) == 0);
    }

    PutLe32(TxDataBytes, (1U << 2) | (0x15U << 3) | (0xcdU << 8) | (0x4321U << 16));
    PutLe32(TxDataBytes + 4, 0x89abcdefU);
    CHECK(T70DpmaifDecodeTxData(TxDataBytes, sizeof(TxDataBytes), &TxData) == T70DpmaifSuccess);
    CHECK(TxData.Type == 0U && TxData.Continuation == 1U && TxData.Reserved == 0x15U);
    CHECK(TxData.AddressExtension == 0xcdU && TxData.DataLength == 0x4321U &&
          TxData.AddressLow == 0x89abcdefU && TxData.DataAddress == UINT64_C(0xcd89abcdef));
    T70_DPMAIF_TX_DATA Before = TxData;
    PutLe32(TxDataBytes, 1U);
    CHECK(T70DpmaifDecodeTxData(TxDataBytes, sizeof(TxDataBytes), &TxData) == T70DpmaifWrongType);
    CHECK(memcmp(&TxData, &Before, sizeof(TxData)) == 0);

    PutLe32(TxMessageBytes, 1U | (1U << 2) | (0x123U << 3) | (0x7654U << 16));
    PutLe32(TxMessageBytes + 4, 0x3210U | (0x77U << 16) | (6U << 24) |
            (1U << 27) | (1U << 28) | (2U << 30));
    CHECK(T70DpmaifDecodeTxMessage(TxMessageBytes, sizeof(TxMessageBytes), &TxMessage) == T70DpmaifSuccess);
    CHECK(TxMessage.Type == 1U && TxMessage.Continuation == 1U &&
          TxMessage.Reserved0 == 0x123U && TxMessage.PacketLength == 0x7654U);
    CHECK(TxMessage.CountLow == 0x3210U && TxMessage.ChannelId == 0x77U &&
          TxMessage.NetworkType == 6U && TxMessage.Reserved1 == 1U &&
          TxMessage.Ipv4Checksum == 1U && TxMessage.L4Checksum == 0U &&
          TxMessage.Reserved2 == 2U);

    {
        uint8_t Encoded[8], Before[8];
        T70_DPMAIF_TX_DATA Data = {
            .Type = 0, .Continuation = 1, .AddressExtension = 0xcd,
            .DataLength = 0x4321, .AddressLow = 0x89abcdefU,
            .DataAddress = UINT64_C(0xcd89abcdef)
        };
        T70_DPMAIF_TX_DATA DecodedData;
        T70_DPMAIF_TX_MESSAGE Message = {
            .Type = 1, .Continuation = 1, .PacketLength = 0x7654,
            .CountLow = 0x3210, .ChannelId = 0x77, .NetworkType = 0,
            .Ipv4Checksum = 1, .L4Checksum = 1
        };
        T70_DPMAIF_TX_MESSAGE DecodedMessage;

        memset(Encoded, 0xa5, sizeof(Encoded));
        CHECK(T70DpmaifEncodeTxData(&Data, Encoded, sizeof(Encoded)) ==
              T70DpmaifSuccess);
        CHECK(T70DpmaifDecodeTxData(Encoded, sizeof(Encoded), &DecodedData) ==
              T70DpmaifSuccess);
        CHECK(DecodedData.Type == Data.Type &&
              DecodedData.Continuation == Data.Continuation &&
              DecodedData.AddressExtension == Data.AddressExtension &&
              DecodedData.DataLength == Data.DataLength &&
              DecodedData.DataAddress == Data.DataAddress);
        memcpy(Before, Encoded, sizeof(Before));
        Data.DataAddress++;
        CHECK(T70DpmaifEncodeTxData(&Data, Encoded, sizeof(Encoded)) ==
              T70DpmaifInvalidParameter);
        CHECK(memcmp(Before, Encoded, sizeof(Before)) == 0);
        Data.DataAddress--;
        CHECK(T70DpmaifEncodeTxData(&Data, Encoded, 7U) ==
              T70DpmaifBufferSize);
        CHECK(memcmp(Before, Encoded, sizeof(Before)) == 0);

        memset(Encoded, 0xa5, sizeof(Encoded));
        CHECK(T70DpmaifEncodeTxMessage(&Message, Encoded, sizeof(Encoded)) ==
              T70DpmaifSuccess);
        CHECK(T70DpmaifDecodeTxMessage(Encoded, sizeof(Encoded),
              &DecodedMessage) == T70DpmaifSuccess);
        CHECK(DecodedMessage.Type == Message.Type &&
              DecodedMessage.Continuation == Message.Continuation &&
              DecodedMessage.PacketLength == Message.PacketLength &&
              DecodedMessage.CountLow == Message.CountLow &&
              DecodedMessage.ChannelId == Message.ChannelId &&
              DecodedMessage.NetworkType == Message.NetworkType &&
              DecodedMessage.Ipv4Checksum == Message.Ipv4Checksum &&
              DecodedMessage.L4Checksum == Message.L4Checksum);
        memcpy(Before, Encoded, sizeof(Before));
        Message.Reserved1 = 1U;
        CHECK(T70DpmaifEncodeTxMessage(&Message, Encoded, sizeof(Encoded)) ==
              T70DpmaifInvalidParameter);
        CHECK(memcmp(Before, Encoded, sizeof(Before)) == 0);
        Message.Reserved1 = 0;
        Message.Type = 0;
        CHECK(T70DpmaifEncodeTxMessage(&Message, Encoded, sizeof(Encoded)) ==
              T70DpmaifWrongType);
    }
}

int main(void)
{
    TestDataPit();
    TestMessagePit();
    TestBatAndTx();
    TestRxWalk();
    if (Failure) {
        printf("DpmaifDescriptorTest failed at line %u after %u checks\n", Failure, Checks);
        return 1;
    }
    printf("T70 DPMAIF v3 descriptors and PIT packet walk: %u field and failure checks passed\n", Checks);
    return 0;
}
