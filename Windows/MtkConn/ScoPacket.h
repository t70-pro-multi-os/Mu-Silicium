// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SCO_PACKET_H
#define T70_SCO_PACKET_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t PayloadBytes, SramStride;
    uint8_t SlotCount, TxBlocks, RxBlocks, ValidRxBlocks;
} T70_SCO_PACKET;

typedef struct {
    T70_SCO_PACKET Packet;
    uint16_t RxOffset, TxOffset;
} T70_SCO_IRQ_PLAN;

// Decode one stock IRQ control word; leave result untouched on failure.
// No MMIO access, stream start or interrupt acknowledgement is performed.
bool T70ScoDecodePacket(uint32_t control, T70_SCO_PACKET *result);

// Validate the byte range of a firmware packet pointer in the 64 KiB SCO
// SRAM bank; return a relative offset, never a mapped pointer.
bool T70ScoSramOffset(uint32_t control, uint32_t packet_pointer, uint16_t *offset);

// Copy between ordinary, disjoint memory buffers (not volatile mapped MMIO).
// Buffers carry encoded SCO data, not decoded PCM.
// On failure no bytes are written. TX does not overwrite per-slot SRAM padding.
bool T70ScoUnpackRx(uint32_t control, const uint8_t *snapshot, size_t snapshot_bytes,
                    uint8_t *encoded, size_t encoded_bytes);
bool T70ScoPackTx(uint32_t control, const uint8_t *encoded, size_t encoded_bytes,
                  uint8_t *snapshot, size_t snapshot_bytes);

// Atomically validate one captured IRQ/control word and both SRAM addresses,
// copy one TX batch into SRAM and one RX batch out. It does not acknowledge the
// interrupt or touch MMIO; the caller owns those actions and must serialize calls.
bool T70ScoProcessInterrupt(uint32_t control, uint32_t rx_pointer, uint32_t tx_pointer,
                           uint8_t *sram, size_t sram_bytes,
                           const uint8_t *encoded_tx, size_t encoded_tx_bytes,
                           uint8_t *encoded_rx, size_t encoded_rx_bytes,
                           T70_SCO_IRQ_PLAN *plan);

#endif
