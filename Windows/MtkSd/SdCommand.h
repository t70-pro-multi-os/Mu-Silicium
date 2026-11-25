// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SD_COMMAND_H
#define T70_SD_COMMAND_H

#include "SdDma.h"

#define T70_SD_COMMAND_IRQ_READY (1U << 8)
#define T70_SD_COMMAND_IRQ_TIMEOUT (1U << 9)
#define T70_SD_COMMAND_IRQ_CRC (1U << 10)
#define T70_SD_COMMAND_IRQ_MASK \
    (T70_SD_COMMAND_IRQ_READY | T70_SD_COMMAND_IRQ_TIMEOUT | T70_SD_COMMAND_IRQ_CRC)

typedef enum {
    T70SdResponseNone = 0,
    T70SdResponseShort,
    T70SdResponseLong,
    T70SdResponseR3,
    T70SdResponseR4,
    T70SdResponseBusy
} T70_SD_COMMAND_RESPONSE;

typedef struct {
    uint32_t Argument;
    uint16_t BlockBytes;
    uint16_t Blocks;
    uint8_t Index;
    T70_SD_COMMAND_RESPONSE Response;
    bool AutoStop;
    bool Write;
} T70_SD_COMMAND_REQUEST;

typedef enum {
    T70SdCommandIdle = 0,
    T70SdCommandResponse,
    T70SdCommandCardBusy
} T70_SD_COMMAND_PHASE;

typedef enum {
    T70SdCommandReady,
    T70SdCommandPending,
    T70SdCommandComplete,
    T70SdCommandTimeout,
    T70SdCommandCrcError,
    T70SdCommandCanceled,
    T70SdCommandRemoved,
    T70SdCommandUnavailable,
    T70SdCommandInvalid
} T70_SD_COMMAND_RESULT;

typedef struct {
    const T70_SD_DMA_IO *Io;
    T70_SD_COMMAND_PHASE Phase;
    T70_SD_COMMAND_RESPONSE Response;
    uint32_t ResponseWords[4];
    bool NeedsReset;
} T70_SD_COMMAND;

/*
 * Zero-initialize Command and serialize calls with controller interrupt
 * handling. The caller retains Io and clocks and prepares the data path
 * before issuing a data command. Stop is the same lifecycle signal as for DMA.
 * The caller owns INTEN and must retire captured events before reuse.
 * When NeedsReset is set, reset the controller and drain its interrupts
 * before zeroing Command and issuing another request.
 */
T70_SD_COMMAND_RESULT
T70SdIssueCommand(T70_SD_COMMAND *Command, const T70_SD_DMA_IO *Io,
                 const T70_SD_COMMAND_REQUEST *Request);

/*
 * Delivers each terminal result once. For R1b/R5b, call again while DAT0 is
 * low, even without another interrupt. On success ResponseWords contains
 * the little-endian SDHCI response payload expected by SDPORT, with the
 * CRC/end byte removed from a long response. Read it before issuing again.
 */
T70_SD_COMMAND_RESULT
T70SdUpdateCommand(T70_SD_COMMAND *Command, uint32_t CapturedEvents,
                  T70_SD_DMA_STOP Stop);

#endif
