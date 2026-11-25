// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_SD_HOST_H
#define T70_SD_HOST_H

#include "SdDma.h"

#define T70_SD_SOURCE_HZ 26000000U
#define T70_SD_INITIAL_HZ 400000U
#define T70_SD_MAX_HZ 25000000U

typedef struct {
    const T70_SD_DMA_IO *Registers;
    void *Context;
    bool (*SetSourceClock)(void *Context, bool Enabled);
    bool (*ConfigurePadSampling)(void *Context);
    void (*DelayMicroseconds)(void *Context, uint32_t Microseconds);
} T70_SD_HOST_IO;

typedef struct {
    uint32_t Config;
    uint32_t Timing[7];
} T70_SD_HOST_STATE;

/*
 * The caller serializes commands, IRQs and clock changes and owns the card
 * power. HCLK and the source must be available for reset, and DMA must
 * have stopped. Reset starts the controller in one-bit mode with no card clock
 * or interrupts. Clock changes use the board's isolated 26 MHz source.
 */
bool T70SdResetHost(const T70_SD_HOST_IO *Io);
bool T70SdSetCardClock(const T70_SD_HOST_IO *Io, uint32_t RequestedHz, uint32_t *ActualHz);
bool T70SdSetBusWidth(const T70_SD_HOST_IO *Io, uint32_t Width);

/* With board power and HCLK retained, stop and restore the card clock.
 * These functions use MMIO only and require an idle command and DMA engine. */
bool T70SdSaveHostState(const T70_SD_HOST_IO *Io, bool ClockRunning, T70_SD_HOST_STATE *State);
bool T70SdRestoreHostState(const T70_SD_HOST_IO *Io, const T70_SD_HOST_STATE *State);

#endif
