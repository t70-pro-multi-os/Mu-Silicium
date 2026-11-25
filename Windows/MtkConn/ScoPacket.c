// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "ScoPacket.h"
#include <string.h>

// Verified against the stock T70 mtk-btcvsd.ko packet geometry and masks.
static const struct {
    uint8_t payload, slots, tx_blocks, rx_blocks;
    uint32_t masks[6];
} Profiles[] = {
    {30,   6, 3, 6, {1, 2, 4, 8, 16, 32}},
    {60,   3, 3, 6, {1, 1, 2, 2, 4, 4}},
    {90,   2, 3, 6, {1, 1, 1, 2, 2, 2}},
    {120,  1, 2, 4, {1, 1, 1, 1}},
    {10,  18, 3, 6, {7, 56, 448, 3584, 28672, 229376}},
    {20,   9, 3, 6, {3, 6, 24, 48, 192, 384}},
};

bool T70ScoDecodePacket(uint32_t control, T70_SCO_PACKET *result) {
    unsigned type = (control >> 18) & 7U;
    if (!result || !(control & UINT32_C(0x80000000)) ||
        type >= sizeof(Profiles) / sizeof(Profiles[0])) {
        return false;
    }
    T70_SCO_PACKET packet = {0};
    packet.PayloadBytes = Profiles[type].payload;
    packet.SramStride = (uint16_t)((packet.PayloadBytes + 3U) & ~3U);
    packet.SlotCount = Profiles[type].slots;
    packet.TxBlocks = Profiles[type].tx_blocks;
    packet.RxBlocks = Profiles[type].rx_blocks;
    for (unsigned i = 0; i < packet.RxBlocks; ++i) {
        uint32_t mask = Profiles[type].masks[i];
        if ((control & mask) == mask) packet.ValidRxBlocks |= (uint8_t)(1U << i);
    }
    *result = packet;
    return true;
}

static size_t SramSpan(const T70_SCO_PACKET *packet) {
    return (packet->SlotCount - 1U) * packet->SramStride + packet->PayloadBytes;
}

bool T70ScoSramOffset(uint32_t control, uint32_t packet_pointer, uint16_t *offset) {
    T70_SCO_PACKET packet;
    if (!offset || packet_pointer == UINT32_C(0xdeadfeed) ||
        !T70ScoDecodePacket(control, &packet)) {
        return false;
    }
    unsigned start = packet_pointer & UINT32_C(0xffff);
    unsigned alignment = packet.PayloadBytes == packet.SramStride ? 4U : 2U;
    size_t span = SramSpan(&packet);
    if (start % alignment || span > UINT32_C(0x10000) - start) return false;
    *offset = (uint16_t)start;
    return true;
}

static bool BuffersFit(const T70_SCO_PACKET *packet, const uint8_t *linear, size_t linear_bytes,
                       const uint8_t *sram, size_t sram_bytes) {
    size_t packed = (size_t)packet->PayloadBytes * packet->SlotCount;
    size_t span = SramSpan(packet);
    if (!linear || !sram || linear_bytes < packed || sram_bytes < span) return false;
    uintptr_t a = (uintptr_t)linear, b = (uintptr_t)sram;
    // Check overlap by subtracting the smaller address, without wraparound.
    return a <= b ? b - a >= packed : a - b >= span;
}

bool T70ScoUnpackRx(uint32_t control, const uint8_t *snapshot, size_t snapshot_bytes,
                    uint8_t *encoded, size_t encoded_bytes) {
    T70_SCO_PACKET packet;
    if (!T70ScoDecodePacket(control, &packet) ||
        !BuffersFit(&packet, encoded, encoded_bytes, snapshot, snapshot_bytes)) return false;
    for (unsigned slot = 0; slot < packet.SlotCount; ++slot)
        for (unsigned byte = 0; byte < packet.PayloadBytes; ++byte)
            encoded[slot * packet.PayloadBytes + byte] = snapshot[slot * packet.SramStride + byte];
    return true;
}

bool T70ScoPackTx(uint32_t control, const uint8_t *encoded, size_t encoded_bytes,
                  uint8_t *snapshot, size_t snapshot_bytes) {
    T70_SCO_PACKET packet;
    if (!T70ScoDecodePacket(control, &packet) ||
        !BuffersFit(&packet, encoded, encoded_bytes, snapshot, snapshot_bytes)) return false;
    for (unsigned slot = 0; slot < packet.SlotCount; ++slot)
        for (unsigned byte = 0; byte < packet.PayloadBytes; ++byte)
            snapshot[slot * packet.SramStride + byte] = encoded[slot * packet.PayloadBytes + byte];
    return true;
}

static bool RangesOverlap(const void *A, size_t ABytes, const void *B, size_t BBytes) {
    uintptr_t AAddress = (uintptr_t)A, BAddress = (uintptr_t)B;
    if (!ABytes || !BBytes) return false;
    return AAddress <= BAddress ? BAddress - AAddress < ABytes : AAddress - BAddress < BBytes;
}

bool T70ScoProcessInterrupt(uint32_t control, uint32_t rx_pointer, uint32_t tx_pointer,
                            uint8_t *sram, size_t sram_bytes,
                            const uint8_t *encoded_tx, size_t encoded_tx_bytes,
                            uint8_t *encoded_rx, size_t encoded_rx_bytes,
                            T70_SCO_IRQ_PLAN *plan) {
    T70_SCO_IRQ_PLAN Sample = {0};
    uint8_t RxCopy[180];
    size_t Packed, Span;
    if (!sram || !encoded_tx || !encoded_rx || !plan ||
        !T70ScoDecodePacket(control, &Sample.Packet) ||
        !T70ScoSramOffset(control, rx_pointer, &Sample.RxOffset) ||
        !T70ScoSramOffset(control, tx_pointer, &Sample.TxOffset))
        return false;
    Packed = (size_t)Sample.Packet.PayloadBytes * Sample.Packet.SlotCount;
    Span = SramSpan(&Sample.Packet);
    if (Packed > sizeof(RxCopy) || encoded_tx_bytes < Packed || encoded_rx_bytes < Packed ||
        Sample.RxOffset > sram_bytes || Span > sram_bytes - Sample.RxOffset ||
        Sample.TxOffset > sram_bytes || Span > sram_bytes - Sample.TxOffset ||
        RangesOverlap(sram, sram_bytes, encoded_tx, Packed) ||
        RangesOverlap(sram, sram_bytes, encoded_rx, Packed) ||
        RangesOverlap(encoded_tx, Packed, encoded_rx, Packed) ||
        RangesOverlap(plan, sizeof(*plan), sram, sram_bytes) ||
        RangesOverlap(plan, sizeof(*plan), encoded_tx, Packed) ||
        RangesOverlap(plan, sizeof(*plan), encoded_rx, Packed))
        return false;

    if (!T70ScoUnpackRx(control, sram + Sample.RxOffset, Span, RxCopy, Packed) ||
        !T70ScoPackTx(control, encoded_tx, Packed, sram + Sample.TxOffset, Span))
        return false;
    memcpy(encoded_rx, RxCopy, Packed);
    *plan = Sample;
    return true;
}
