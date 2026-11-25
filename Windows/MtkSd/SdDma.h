// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SD_DMA_H
#define T70_SD_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define T70_SD_DMA_MAX_DESCRIPTORS 1024U
#define T70_SD_DMA_GPD_BYTES 28U
#define T70_SD_DMA_BD_BYTES 16U
#define T70_SD_DMA_TABLE_BYTES (2U * T70_SD_DMA_GPD_BYTES + T70_SD_DMA_MAX_DESCRIPTORS * T70_SD_DMA_BD_BYTES)

typedef struct {
    uint64_t Address;
    uint32_t Length;
} T70_SD_DMA_SEGMENT;

/*
 * Build the MTK MSDC descriptor chain for a card-to-memory transfer. The
 * caller owns the stopped DMA engine, coherent table and mapped segments.
 * Rejected inputs leave the table untouched. Publish memory before DMA start.
 */
bool
T70SdBuildDmaChain(
    uint8_t *Table,
    size_t TableBytes,
    uint64_t TableAddress,
    const T70_SD_DMA_SEGMENT *Segments,
    size_t SegmentCount,
    uint32_t TransferBytes
    );

#define T70_SD_DMA_IRQ_COMPLETE (1U << 12)
#define T70_SD_DMA_IRQ_TIMEOUT (1U << 14)
#define T70_SD_DMA_IRQ_CRC (1U << 15)
#define T70_SD_DMA_IRQ_BD_CHECKSUM (1U << 17)
#define T70_SD_DMA_IRQ_GPD_CHECKSUM (1U << 18)
#define T70_SD_DMA_IRQ_PROTECT (1U << 19)
#define T70_SD_DMA_IRQ_MASK (T70_SD_DMA_IRQ_COMPLETE | T70_SD_DMA_IRQ_TIMEOUT | \
                             T70_SD_DMA_IRQ_CRC | T70_SD_DMA_IRQ_BD_CHECKSUM | \
                             T70_SD_DMA_IRQ_GPD_CHECKSUM | T70_SD_DMA_IRQ_PROTECT)

typedef enum {
    T70SdDmaIdle = 0,
    T70SdDmaArmed,
    T70SdDmaRunning,
    T70SdDmaDraining
} T70_SD_DMA_PHASE;

typedef enum {
    T70SdDmaReady,
    T70SdDmaPending,
    T70SdDmaComplete,
    T70SdDmaTimeout,
    T70SdDmaCrcError,
    T70SdDmaHardwareError,
    T70SdDmaCanceled,
    T70SdDmaRemoved,
    T70SdDmaUnavailable,
    T70SdDmaInvalid
} T70_SD_DMA_RESULT;

typedef enum {
    T70SdDmaKeepRunning = 0,
    T70SdDmaCancel,
    T70SdDmaMediaRemoved,
    T70SdDmaDeadline
} T70_SD_DMA_STOP;

typedef struct {
    void *Context;
    uint32_t (*Read32)(void *Context, uint32_t Offset);
    void (*Write32)(void *Context, uint32_t Offset, uint32_t Value);
    void (*Fence)(void *Context);
} T70_SD_DMA_IO;

typedef struct {
    T70_SD_DMA_IO Io;
    T70_SD_DMA_PHASE Phase;
    T70_SD_DMA_RESULT Result;
} T70_SD_DMA;

/*
 * Initialize Transfer to zero and serialize all calls with the slot's
 * command and interrupt handling. Retain clocks, DMA table and mappings
 * while Phase is non-idle. The caller handles command response and card
 * lifecycle, interrupt delivery, and any DMA mapping/cache maintenance.
 * Captured events belong to the current request and must be retired before
 * arming a successor. If STOP stalls, retain the buffers until platform
 * recovery has quiesced the engine and Update reports the terminal result.
 */
T70_SD_DMA_RESULT
T70SdArmDma(
    T70_SD_DMA *Transfer,
    const T70_SD_DMA_IO *Io,
    uint8_t *Table,
    size_t TableBytes,
    uint64_t TableAddress,
    const T70_SD_DMA_SEGMENT *Segments,
    size_t SegmentCount,
    uint32_t TransferBytes
    );

/* Start after a successful data command response. */
T70_SD_DMA_RESULT T70SdStartDma(T70_SD_DMA *Transfer);

/* A terminal result is returned once, after the engine has stopped. */
T70_SD_DMA_RESULT T70SdUpdateDma(T70_SD_DMA *Transfer, uint32_t CapturedEvents,
                                   T70_SD_DMA_STOP Stop);

#endif
