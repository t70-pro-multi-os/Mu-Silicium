// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../ScoPacket.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void CopyTest(uint32_t ctl, unsigned payload, unsigned stride, unsigned slots) {
    uint8_t encoded[256], snapshot[256], decoded[256], before[256];
    size_t used = payload * slots;
    size_t span = (slots - 1) * stride + payload;
    for (unsigned i = 0; i < sizeof(encoded); ++i) encoded[i] = (uint8_t)(i * 7U);
    memset(snapshot, 0xa5, sizeof(snapshot));
    memset(decoded, 0xbc, sizeof(decoded));
    memcpy(before, snapshot, sizeof(before));
    assert(!T70ScoPackTx(ctl, encoded, used, snapshot, span - 1));
    assert(!T70ScoPackTx(ctl, encoded, used - 1, snapshot, span));
    assert(!T70ScoPackTx(ctl, snapshot, used, snapshot + 1, span));
    assert(!memcmp(snapshot, before, sizeof(before)));
    assert(T70ScoPackTx(ctl, encoded, used, snapshot, span));
    for (unsigned i = 0; i < sizeof(snapshot); ++i) {
        unsigned slot = i / stride, byte = i % stride;
        uint8_t expected = slot < slots && byte < payload ? encoded[slot * payload + byte] : 0xa5;
        assert(snapshot[i] == expected);
    }
    memcpy(before, decoded, sizeof(before));
    assert(!T70ScoUnpackRx(ctl, snapshot, span - 1, decoded, used));
    assert(!T70ScoUnpackRx(ctl, snapshot, span, decoded, used - 1));
    assert(!T70ScoUnpackRx(ctl, decoded, span, decoded + 1, used));
    assert(!memcmp(decoded, before, sizeof(before)));
    assert(T70ScoUnpackRx(ctl, snapshot, span, decoded, used));
    assert(!memcmp(decoded, encoded, used));
    for (size_t i = used; i < sizeof(decoded); ++i) assert(decoded[i] == 0xbc);
}

static void InterruptTransferTest(void) {
    uint8_t Sram[0x10000], Tx[180], Rx[180], ExpectedRx[180], BeforeSram[0x10000];
    static const unsigned Payload[] = {30, 60, 90, 120, 10, 20};
    static const unsigned Stride[] = {32, 60, 92, 120, 12, 20};
    static const unsigned Slots[] = {6, 3, 2, 1, 18, 9};
    T70_SCO_IRQ_PLAN Plan;
    for (unsigned Type = 0; Type < 6; ++Type) {
        uint32_t Control = UINT32_C(0x80000000) | (Type << 18) | UINT32_C(0x3ffff);
        size_t Packed = Payload[Type] * Slots[Type];
        size_t Span = (Slots[Type] - 1) * Stride[Type] + Payload[Type];
        memset(Sram, 0xa5, sizeof(Sram));
        for (unsigned Slot = 0; Slot < Slots[Type]; ++Slot) {
            for (unsigned Byte = 0; Byte < Payload[Type]; ++Byte) {
                uint8_t Value = (uint8_t)(Slot * 19U + Byte);
                Sram[0x1000U + Slot * Stride[Type] + Byte] = Value;
                ExpectedRx[Slot * Payload[Type] + Byte] = Value;
            }
        }
        for (size_t Index = 0; Index < Packed; ++Index) Tx[Index] = (uint8_t)(Index * 7U + Type);
        memset(Rx, 0xbc, sizeof(Rx));
        memset(&Plan, 0, sizeof(Plan));
        assert(T70ScoProcessInterrupt(Control, UINT32_C(0xa5011000), UINT32_C(0xb0023000),
                                      Sram, sizeof(Sram), Tx, Packed, Rx, Packed, &Plan));
        assert(Plan.RxOffset == 0x1000 && Plan.TxOffset == 0x3000 &&
               Plan.Packet.PayloadBytes == Payload[Type] &&
               Plan.Packet.SramStride == Stride[Type] && Plan.Packet.SlotCount == Slots[Type]);
        assert(!memcmp(Rx, ExpectedRx, Packed));
        for (unsigned Slot = 0; Slot < Slots[Type]; ++Slot) {
            for (unsigned Byte = 0; Byte < Stride[Type]; ++Byte) {
                uint8_t Expected = Byte < Payload[Type] ? Tx[Slot * Payload[Type] + Byte] : 0xa5;
                assert(Sram[0x3000U + Slot * Stride[Type] + Byte] == Expected);
            }
        }
        assert(Span <= sizeof(Sram));
    }

    memset(Sram, 0x5a, sizeof(Sram));
    memset(Rx, 0xbc, sizeof(Rx));
    memset(Tx, 0x69, sizeof(Tx));
    memset(&Plan, 0x3c, sizeof(Plan));
    memcpy(BeforeSram, Sram, sizeof(Sram));
    T70_SCO_IRQ_PLAN BeforePlan = Plan;
    assert(!T70ScoProcessInterrupt(UINT32_C(0x00000000), 0x1000, 0x3000,
                                   Sram, sizeof(Sram), Tx, 180, Rx, 180, &Plan));
    assert(!T70ScoProcessInterrupt(UINT32_C(0x80180000), 0x1000, 0x3000,
                                   Sram, sizeof(Sram), Tx, 180, Rx, 180, &Plan));
    assert(!T70ScoProcessInterrupt(UINT32_C(0x80000000), UINT32_C(0xdeadfeed), 0x3000,
                                   Sram, sizeof(Sram), Tx, 180, Rx, 180, &Plan));
    assert(!T70ScoProcessInterrupt(UINT32_C(0x80000000), 0x1000, 0xffff,
                                   Sram, sizeof(Sram), Tx, 180, Rx, 180, &Plan));
    assert(!T70ScoProcessInterrupt(UINT32_C(0x80000000), 0x1000, 0x3000,
                                   Sram, sizeof(Sram), Tx, 179, Rx, 180, &Plan));
    assert(!T70ScoProcessInterrupt(UINT32_C(0x80000000), 0x1000, 0x3000,
                                   Sram, sizeof(Sram), Tx, 180, Rx, 179, &Plan));
    assert(!T70ScoProcessInterrupt(UINT32_C(0x80000000), 0x1000, 0x3000,
                                   Sram, sizeof(Sram), Tx, 180, Tx, 180, &Plan));
    assert(!T70ScoProcessInterrupt(UINT32_C(0x80000000), 0x1000, 0x3000,
                                   Sram, 0x1000, Tx, 180, Rx, 180, &Plan));
    assert(!memcmp(BeforeSram, Sram, sizeof(Sram)) &&
           !memcmp(&BeforePlan, &Plan, sizeof(Plan)));
    for (unsigned Index = 0; Index < sizeof(Rx); ++Index) assert(Rx[Index] == 0xbc);
}

int main(void) {
    InterruptTransferTest();
    const unsigned payload[] = {30, 60, 90, 120, 10, 20};
    const unsigned stride[] = {32, 60, 92, 120, 12, 20};
    const unsigned slots[] = {6, 3, 2, 1, 18, 9};
    const unsigned valid_rx[] = {63, 63, 63, 15, 63, 63};
    unsigned checks = 0;
    for (unsigned type = 0; type < 8; ++type) {
        uint32_t ctl = UINT32_C(0x80000000) | (type << 18);
        T70_SCO_PACKET got = {.PayloadBytes = 999};
        assert(!T70ScoDecodePacket(ctl & UINT32_C(0x7fffffff), &got) &&
               got.PayloadBytes == 999);
        if (type >= 6) {
            assert(!T70ScoDecodePacket(ctl, &got) && got.PayloadBytes == 999);
            continue;
        }
        assert(T70ScoDecodePacket(ctl, &got));
        assert(got.PayloadBytes == payload[type] && got.SramStride == stride[type] &&
               got.SlotCount == slots[type] && got.TxBlocks == (type == 3 ? 2 : 3) &&
               got.RxBlocks == (type == 3 ? 4 : 6) && !got.ValidRxBlocks);
        assert(T70ScoDecodePacket(ctl | UINT32_C(0x3ffff), &got));
        assert(got.ValidRxBlocks == valid_rx[type]);
        CopyTest(ctl, payload[type], stride[type], slots[type]);
        for (unsigned start = 0; start < 0x10000; ++start) {
            uint16_t offset = 123;
            unsigned span = (slots[type] - 1) * stride[type] + payload[type];
            bool expected = start % (stride[type] == payload[type] ? 4 : 2) == 0 &&
                            start + span <= 0x10000;
            assert(T70ScoSramOffset(ctl, UINT32_C(0xa5010000) | start, &offset) == expected);
            assert(offset == (expected ? start : 123));
            ++checks;
        }
    }
    T70_SCO_PACKET packet;
    uint16_t offset = 77;
    assert(!T70ScoSramOffset(UINT32_C(0x80000000), UINT32_C(0xdeadfeed), &offset) && offset == 77);
    assert(T70ScoDecodePacket(UINT32_C(0x80100000) | 3, &packet) && !packet.ValidRxBlocks);
    assert(T70ScoDecodePacket(UINT32_C(0x80100000) | 7, &packet) && packet.ValidRxBlocks == 1);
    assert(T70ScoDecodePacket(UINT32_C(0x80140000) | 6, &packet) && packet.ValidRxBlocks == 2);
    assert(!T70ScoDecodePacket(UINT32_C(0x80000000), NULL));
    printf("Stock SCO packet profiles: %u checked SRAM addresses, 6 TX/RX round trips and atomic IRQ batches passed\n", checks);
    return 0;
}
