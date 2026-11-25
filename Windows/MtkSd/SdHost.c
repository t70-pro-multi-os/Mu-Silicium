// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdHost.h"

#define MSDC_CFG 0x000U
#define MSDC_IOCON 0x004U
#define MSDC_PS 0x008U
#define MSDC_INT 0x00cU
#define MSDC_INTEN 0x010U
#define MSDC_FIFOCS 0x014U
#define SDC_CFG 0x030U
#define SDC_STS 0x03cU
#define MSDC_DMA_CFG 0x09cU
#define MSDC_PATCH_BIT0 0x0b0U
#define MSDC_PATCH_BIT1 0x0b4U
#define MSDC_PATCH_BIT2 0x0b8U
#define EMMC50_CFG0 0x208U
#define SDC_FIFO_CFG 0x228U

#define MSDC_CFG_MODE 1U
#define MSDC_CFG_CARD_CLOCK (1U << 1)
#define MSDC_CFG_RESET (1U << 2)
#define MSDC_CFG_PIO (1U << 3)
#define MSDC_CFG_CLOCK_STABLE (1U << 7)
#define MSDC_CFG_CLOCK_MASK (0x7fffU << 8)
#define MSDC_FIFO_CLEAR (1U << 31)
#define MSDC_DAT0 (1U << 16)
#define SDC_BUSY_MASK 3U
#define SDC_CFG_WIDTH_MASK (3U << 16)
#define SDC_CFG_SDIO (1U << 19)
#define SDC_CFG_SDIO_IRQ (1U << 20)
#define SDC_CFG_TIMEOUT_MASK (0xffU << 24)
#define MSDC_CONTEXT_MASK (MSDC_CFG_MODE | MSDC_CFG_PIO | MSDC_CFG_CLOCK_MASK | MSDC_CFG_CARD_CLOCK)

static const struct {
    uint32_t Register;
    uint32_t Mask;
} mTiming[] = {
    { MSDC_IOCON, UINT32_MAX },
    { SDC_CFG, SDC_CFG_WIDTH_MASK | SDC_CFG_SDIO | SDC_CFG_SDIO_IRQ | SDC_CFG_TIMEOUT_MASK },
    { MSDC_PATCH_BIT0, UINT32_MAX },
    { MSDC_PATCH_BIT1, UINT32_MAX },
    { MSDC_PATCH_BIT2, (1U << 28) | (1U << 15) | 0x0cU },
    { EMMC50_CFG0, 1U << 4 },
    { SDC_FIFO_CFG, 3U << 24 },
};

_Static_assert(sizeof(mTiming) / sizeof(mTiming[0]) ==
               sizeof(((T70_SD_HOST_STATE *)0)->Timing) / sizeof(uint32_t), "host timing register count");

static bool
ValidRegisters(const T70_SD_DMA_IO *Registers)
{
    return Registers != NULL && Registers->Read32 != NULL &&
           Registers->Write32 != NULL && Registers->Fence != NULL;
}

static bool
WaitRegister(const T70_SD_HOST_IO *Io, uint32_t Register, uint32_t Mask, uint32_t Value)
{
    const T70_SD_DMA_IO *Host = Io->Registers;
    unsigned Attempt;

    for (Attempt = 0; Attempt < 100; ++Attempt) {
        uint32_t RegisterValue = Host->Read32(Host->Context, Register);
        if (RegisterValue == UINT32_MAX) {
            return false;
        }
        if ((RegisterValue & Mask) == Value) {
            return true;
        }
        Io->DelayMicroseconds(Io->Context, 1);
    }
    return false;
}

static bool
ChangeBits(const T70_SD_DMA_IO *Host, uint32_t Register, uint32_t Mask, uint32_t Value)
{
    uint32_t Before = Host->Read32(Host->Context, Register);
    uint32_t After;

    if (Before == UINT32_MAX) {
        return false;
    }
    Host->Write32(Host->Context, Register, (Before & ~Mask) | Value);
    Host->Fence(Host->Context);
    After = Host->Read32(Host->Context, Register);
    return After != UINT32_MAX && (After & Mask) == Value;
}

bool
T70SdResetHost(const T70_SD_HOST_IO *Io)
{
    const T70_SD_DMA_IO *Host;
    uint32_t Config;
    uint32_t Interrupts;

    if (Io == NULL || !ValidRegisters(Io->Registers) || Io->ConfigurePadSampling == NULL ||
        Io->DelayMicroseconds == NULL) {
        return false;
    }
    Host = Io->Registers;
    Config = Host->Read32(Host->Context, MSDC_CFG);
    if (Config == UINT32_MAX || (Host->Read32(Host->Context, MSDC_DMA_CFG) & 1U) != 0) {
        return false;
    }

    Host->Write32(Host->Context, MSDC_INTEN, 0);
    Config = (Config | MSDC_CFG_MODE | MSDC_CFG_PIO) & ~MSDC_CFG_CARD_CLOCK;
    Host->Write32(Host->Context, MSDC_CFG, Config);
    Host->Write32(Host->Context, MSDC_CFG, Config | MSDC_CFG_RESET);
    Host->Fence(Host->Context);
    if (!WaitRegister(Io, MSDC_CFG, MSDC_CFG_RESET, 0)) {
        return false;
    }
    Host->Write32(Host->Context, MSDC_FIFOCS, MSDC_FIFO_CLEAR);
    Host->Fence(Host->Context);
    if (!WaitRegister(Io, MSDC_FIFOCS, MSDC_FIFO_CLEAR, 0)) {
        return false;
    }

    Interrupts = Host->Read32(Host->Context, MSDC_INT);
    if (Interrupts == UINT32_MAX) {
        return false;
    }
    Host->Write32(Host->Context, MSDC_INT, Interrupts);
    Host->Write32(Host->Context, MSDC_IOCON, 0);

    /* MT6789 MSDC1's async FIFO, stop-clock fix and enhanced RX. */
    Host->Write32(Host->Context, MSDC_PATCH_BIT0, 0x403c0446U);
    Host->Write32(Host->Context, MSDC_PATCH_BIT1, 0xffff4309U);
    return Io->ConfigurePadSampling(Io->Context) &&
           Host->Read32(Host->Context, MSDC_PATCH_BIT0) == 0x403c0446U &&
           Host->Read32(Host->Context, MSDC_PATCH_BIT1) == 0xffff4309U &&
           ChangeBits(Host, MSDC_PATCH_BIT2, (1U << 28) | (1U << 15) | 0x0cU, (1U << 28) | 0x0cU) &&
           ChangeBits(Host, EMMC50_CFG0, 1U << 4, 1U << 4) &&
           ChangeBits(Host, SDC_FIFO_CFG, 3U << 24, 0) &&
           ChangeBits(Host, SDC_CFG, SDC_CFG_WIDTH_MASK | SDC_CFG_SDIO | SDC_CFG_SDIO_IRQ, SDC_CFG_SDIO) &&
           ChangeBits(Host, MSDC_CFG, MSDC_CFG_MODE | MSDC_CFG_CARD_CLOCK | MSDC_CFG_PIO,
                      MSDC_CFG_MODE | MSDC_CFG_PIO) &&
           Host->Read32(Host->Context, MSDC_INTEN) == 0;
}

bool
T70SdSetCardClock(const T70_SD_HOST_IO *Io, uint32_t RequestedHz, uint32_t *ActualHz)
{
    const T70_SD_DMA_IO *Host;
    uint32_t Divider;
    uint32_t Frequency;
    uint32_t Cycles;
    uint32_t Timeout;

    if (Io == NULL || !ValidRegisters(Io->Registers) || Io->SetSourceClock == NULL ||
        Io->DelayMicroseconds == NULL || ActualHz == NULL || RequestedHz > T70_SD_MAX_HZ) {
        return false;
    }
    Host = Io->Registers;
    if ((Host->Read32(Host->Context, SDC_STS) & SDC_BUSY_MASK) != 0 ||
        (Host->Read32(Host->Context, MSDC_DMA_CFG) & 1U) != 0) {
        return false;
    }

    if (RequestedHz == 0) {
        if (!ChangeBits(Host, MSDC_CFG, MSDC_CFG_CARD_CLOCK, 0) ||
            !Io->SetSourceClock(Io->Context, false)) {
            return false;
        }
        *ActualHz = 0;
        return true;
    }
    if (RequestedHz >= T70_SD_SOURCE_HZ / 2) {
        Divider = 0;
        Frequency = T70_SD_SOURCE_HZ / 2;
    } else {
        uint32_t Scale = 4 * RequestedHz;
        Divider = (T70_SD_SOURCE_HZ + Scale - 1) / Scale;
        if (Divider > 0xfffU) {
            return false;
        }
        Frequency = T70_SD_SOURCE_HZ / (4 * Divider);
    }
    if (!ChangeBits(Host, MSDC_CFG, MSDC_CFG_CARD_CLOCK, 0) ||
        !Io->SetSourceClock(Io->Context, false) ||
        !ChangeBits(Host, MSDC_CFG, MSDC_CFG_CLOCK_MASK, Divider << 8) ||
        !Io->SetSourceClock(Io->Context, true)) {
        return false;
    }
    if (!WaitRegister(Io, MSDC_CFG, MSDC_CFG_CLOCK_STABLE, MSDC_CFG_CLOCK_STABLE)) {
        Io->SetSourceClock(Io->Context, false);
        return false;
    }

    /* The firmware programs a 100 ms data timeout plus three 2^20-cycle units. */
    Cycles = (Frequency + 9U) / 10U;
    Timeout = ((Cycles + ((1U << 20) - 1)) >> 20) + 2;
    if (!ChangeBits(Host, SDC_CFG, SDC_CFG_TIMEOUT_MASK, Timeout << 24) ||
        !ChangeBits(Host, MSDC_CFG, MSDC_CFG_CARD_CLOCK, MSDC_CFG_CARD_CLOCK)) {
        Io->SetSourceClock(Io->Context, false);
        return false;
    }
    *ActualHz = Frequency;
    return true;
}

bool
T70SdSetBusWidth(const T70_SD_HOST_IO *Io, uint32_t Width)
{
    const T70_SD_DMA_IO *Host;

    if (Io == NULL || !ValidRegisters(Io->Registers) || (Width != 1U && Width != 4U)) {
        return false;
    }
    Host = Io->Registers;
    return (Host->Read32(Host->Context, SDC_STS) & SDC_BUSY_MASK) == 0 &&
           ChangeBits(Host, SDC_CFG, SDC_CFG_WIDTH_MASK, Width == 4U ? (1U << 16) : 0);
}

static bool
HostIdle(const T70_SD_DMA_IO *Host)
{
    return (Host->Read32(Host->Context, SDC_STS) & SDC_BUSY_MASK) == 0 &&
           (Host->Read32(Host->Context, MSDC_DMA_CFG) & 1U) == 0 &&
           Host->Read32(Host->Context, MSDC_INTEN) == 0;
}

bool
T70SdSaveHostState(const T70_SD_HOST_IO *Io, bool ClockRunning, T70_SD_HOST_STATE *State)
{
    if (Io == NULL || !ValidRegisters(Io->Registers) || State == NULL) {
        return false;
    }
    const T70_SD_DMA_IO *Host = Io->Registers;
    uint32_t Config = Host->Read32(Host->Context, MSDC_CFG);
    if ((Config & (MSDC_CFG_MODE | MSDC_CFG_RESET)) != MSDC_CFG_MODE ||
        ((Config & MSDC_CFG_CARD_CLOCK) != 0) != ClockRunning || !HostIdle(Host)) {
        return false;
    }
    if (ClockRunning) {
        uint32_t Pins = Host->Read32(Host->Context, MSDC_PS);
        if ((Config & MSDC_CFG_CLOCK_STABLE) == 0 || Pins == UINT32_MAX || (Pins & MSDC_DAT0) == 0) {
            return false;
        }
    }
    for (size_t Index = 0; Index < sizeof(mTiming) / sizeof(mTiming[0]); ++Index) {
        uint32_t Value = Host->Read32(Host->Context, mTiming[Index].Register);
        if (Value == UINT32_MAX) {
            return false;
        }
        State->Timing[Index] = Value & mTiming[Index].Mask;
    }
    State->Config = Config & MSDC_CONTEXT_MASK;
    return ChangeBits(Host, MSDC_CFG, MSDC_CFG_CARD_CLOCK, 0);
}

bool
T70SdRestoreHostState(const T70_SD_HOST_IO *Io, const T70_SD_HOST_STATE *State)
{
    if (Io == NULL || !ValidRegisters(Io->Registers) || State == NULL) {
        return false;
    }
    const T70_SD_DMA_IO *Host = Io->Registers;
    uint32_t Config = Host->Read32(Host->Context, MSDC_CFG);
    uint32_t Output = State->Config & MSDC_CFG_CARD_CLOCK;
    if (Config == UINT32_MAX || (Config & MSDC_CFG_RESET) || !HostIdle(Host) ||
        (Output && (Config & MSDC_CFG_CLOCK_STABLE) == 0) ||
        !ChangeBits(Host, MSDC_CFG, MSDC_CONTEXT_MASK, State->Config & ~MSDC_CFG_CARD_CLOCK)) {
        return false;
    }
    for (size_t Index = 0; Index < sizeof(mTiming) / sizeof(mTiming[0]); ++Index) {
        if (!ChangeBits(Host, mTiming[Index].Register, mTiming[Index].Mask, State->Timing[Index])) {
            return false;
        }
    }
    Config = Host->Read32(Host->Context, MSDC_CFG);
    return Config != UINT32_MAX && (!Output ||
           ((Config & MSDC_CFG_CLOCK_STABLE) && ChangeBits(Host, MSDC_CFG, MSDC_CFG_CARD_CLOCK, Output)));
}
