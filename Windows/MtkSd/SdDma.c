// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdDma.h"

#define MSDC_GPD_HWO 1U
#define MSDC_GPD_BDP 2U
#define MSDC_BD_EOL 1U
/* Use only the 16-bit length field and addresses below 4 GiB. */
#define MSDC_BD_MAX_ALIGNED_LENGTH 0xFFFCU

static bool
DmaRangeValid(uint64_t Address, uint32_t Length)
{
    return Length != 0 && (Address & 3U) == 0 && (Length & 3U) == 0 &&
           Address <= UINT32_MAX && Length - 1U <= UINT32_MAX - Address;
}

static void
WriteLe32(uint8_t *Bytes, uint32_t Value)
{
    for (unsigned Index = 0; Index < 4; ++Index) {
        Bytes[Index] = (uint8_t)(Value >> (Index * 8));
    }
}

static void
WriteDescriptor(uint8_t *Descriptor, uint32_t Flags, uint32_t Next,
                uint32_t Address, uint32_t Length)
{
    uint8_t Sum = 0;
    WriteLe32(Descriptor, Flags);
    WriteLe32(Descriptor + 4, Next);
    WriteLe32(Descriptor + 8, Address);
    WriteLe32(Descriptor + 12, Length);
    for (unsigned Index = 0; Index < T70_SD_DMA_BD_BYTES; ++Index) {
        Sum = (uint8_t)(Sum + Descriptor[Index]);
    }
    Descriptor[1] = (uint8_t)(0xFFU - Sum);
}

bool
T70SdBuildDmaChain(
    uint8_t *Table,
    size_t TableBytes,
    uint64_t TableAddress,
    const T70_SD_DMA_SEGMENT *Segments,
    size_t SegmentCount,
    uint32_t TransferBytes
    )
{
    uint32_t TotalBytes = 0;
    size_t Descriptors = 0;
    const uint32_t GpdBytes = 2U * T70_SD_DMA_GPD_BYTES;
    if (Table == NULL || Segments == NULL || SegmentCount == 0 ||
        SegmentCount > T70_SD_DMA_MAX_DESCRIPTORS) {
        return false;
    }

    for (size_t Index = 0; Index < SegmentCount; ++Index) {
        uint32_t Length = Segments[Index].Length;
        if (!DmaRangeValid(Segments[Index].Address, Length) || Length > TransferBytes - TotalBytes) {
            return false;
        }
        TotalBytes += Length;
        Descriptors += (Length - 1U) / MSDC_BD_MAX_ALIGNED_LENGTH + 1U;
        if (Descriptors > T70_SD_DMA_MAX_DESCRIPTORS) {
            return false;
        }
    }

    uint32_t RequiredBytes = GpdBytes + (uint32_t)Descriptors * T70_SD_DMA_BD_BYTES;
    if (TotalBytes != TransferBytes || TableBytes < RequiredBytes ||
        !DmaRangeValid(TableAddress, RequiredBytes)) {
        return false;
    }

    for (size_t Index = 0; Index < SegmentCount; ++Index) {
        uint64_t Address = Segments[Index].Address;
        uint64_t End = Address + Segments[Index].Length;
        if (Address < TableAddress + RequiredBytes && TableAddress < End) {
            return false;
        }
    }

    uint32_t Offset = GpdBytes;
    for (size_t Index = 0; Index < SegmentCount; ++Index) {
        uint64_t Address = Segments[Index].Address;
        uint32_t Remaining = Segments[Index].Length;
        while (Remaining != 0) {
            uint32_t Length = Remaining > MSDC_BD_MAX_ALIGNED_LENGTH ?
                MSDC_BD_MAX_ALIGNED_LENGTH : Remaining;
            uint32_t Next = Offset + T70_SD_DMA_BD_BYTES;
            bool Last = Next == RequiredBytes;
            WriteDescriptor(Table + Offset, Last ? MSDC_BD_EOL : 0,
                            Last ? 0 : (uint32_t)(TableAddress + Next),
                            (uint32_t)Address, Length);
            Offset = Next;
            Address += Length;
            Remaining -= Length;
        }
    }

    /*
     * The controller walks one general descriptor and stops at the second
     * descriptor, which is entirely zero so ownership remains with software.
     */
    for (unsigned Index = T70_SD_DMA_BD_BYTES; Index < GpdBytes; ++Index) {
        Table[Index] = 0;
    }
    WriteDescriptor(Table, MSDC_GPD_HWO | MSDC_GPD_BDP,
                    (uint32_t)(TableAddress + T70_SD_DMA_GPD_BYTES),
                    (uint32_t)(TableAddress + GpdBytes), 0);
    return true;
}

#define MSDC_CFG 0x000U
#define MSDC_INT 0x00CU
#define MSDC_DMA_ADDRESS_HIGH 0x08CU
#define MSDC_DMA_ADDRESS 0x090U
#define MSDC_DMA_CONTROL 0x098U
#define MSDC_DMA_CONFIG 0x09CU
#define MSDC_CFG_MODE (1U << 0)
#define MSDC_CFG_RESET (1U << 2)
#define MSDC_CFG_PIO (1U << 3)
#define MSDC_DMA_ACTIVE (1U << 0)
#define MSDC_DMA_CHECKSUM_ENABLE (1U << 1)
#define MSDC_DMA_START (1U << 0)
#define MSDC_DMA_STOP (1U << 1)
#define MSDC_DMA_COMMAND_BITS 7U
#define MSDC_DMA_DESCRIPTOR_MODE (1U << 8)
#define MSDC_DMA_BURST_MASK (7U << 12)
#define MSDC_DMA_BURST_64_BYTES (6U << 12)

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
    )
{
    if (Transfer == NULL || Io == NULL || Io->Read32 == NULL ||
        Io->Write32 == NULL || Io->Fence == NULL) {
        return T70SdDmaInvalid;
    }
    if (Transfer->Phase != T70SdDmaIdle) {
        return T70SdDmaUnavailable;
    }

    uint32_t Config = Io->Read32(Io->Context, MSDC_CFG);
    uint32_t DmaConfig = Io->Read32(Io->Context, MSDC_DMA_CONFIG);
    uint32_t Control = Io->Read32(Io->Context, MSDC_DMA_CONTROL);
    if ((Config & (MSDC_CFG_MODE | MSDC_CFG_RESET)) != MSDC_CFG_MODE ||
        (DmaConfig & MSDC_DMA_ACTIVE) != 0 || (Control & MSDC_DMA_STOP) != 0) {
        return T70SdDmaUnavailable;
    }
    if (!T70SdBuildDmaChain(Table, TableBytes, TableAddress, Segments, SegmentCount, TransferBytes)) {
        return T70SdDmaInvalid;
    }

    Transfer->Io = *Io;
    Transfer->Phase = T70SdDmaArmed;
    Transfer->Result = T70SdDmaPending;
    Io->Write32(Io->Context, MSDC_INT, T70_SD_DMA_IRQ_MASK);
    Io->Write32(Io->Context, MSDC_CFG, Config & ~MSDC_CFG_PIO);
    Io->Write32(Io->Context, MSDC_DMA_CONFIG, DmaConfig | MSDC_DMA_CHECKSUM_ENABLE);
    Control &= ~(MSDC_DMA_COMMAND_BITS | MSDC_DMA_BURST_MASK);
    Control |= MSDC_DMA_DESCRIPTOR_MODE | MSDC_DMA_BURST_64_BYTES;
    Io->Write32(Io->Context, MSDC_DMA_CONTROL, Control);
    uint32_t AddressHigh = Io->Read32(Io->Context, MSDC_DMA_ADDRESS_HIGH);
    Io->Write32(Io->Context, MSDC_DMA_ADDRESS_HIGH, AddressHigh & ~0xFU);
    Io->Write32(Io->Context, MSDC_DMA_ADDRESS, (uint32_t)TableAddress);
    return T70SdDmaReady;
}

T70_SD_DMA_RESULT
T70SdStartDma(T70_SD_DMA *Transfer)
{
    if (Transfer == NULL || Transfer->Phase != T70SdDmaArmed) {
        return T70SdDmaInvalid;
    }
    const T70_SD_DMA_IO *Io = &Transfer->Io;
    uint32_t Control = Io->Read32(Io->Context, MSDC_DMA_CONTROL);
    if (Control == UINT32_MAX || (Control & MSDC_DMA_COMMAND_BITS) != 0) {
        return T70SdDmaUnavailable;
    }
    Io->Fence(Io->Context);
    Transfer->Phase = T70SdDmaRunning;
    Io->Write32(Io->Context, MSDC_DMA_CONTROL, (Control & ~MSDC_DMA_COMMAND_BITS) | MSDC_DMA_START);
    return T70SdDmaPending;
}

static T70_SD_DMA_RESULT
DmaResult(uint32_t Events, T70_SD_DMA_STOP Stop)
{
    if (Stop == T70SdDmaMediaRemoved) {
        return T70SdDmaRemoved;
    }
    if (Stop == T70SdDmaCancel) {
        return T70SdDmaCanceled;
    }
    if ((Events & (T70_SD_DMA_IRQ_BD_CHECKSUM | T70_SD_DMA_IRQ_GPD_CHECKSUM |
                   T70_SD_DMA_IRQ_PROTECT)) != 0) {
        return T70SdDmaHardwareError;
    }
    if ((Events & T70_SD_DMA_IRQ_CRC) != 0) {
        return T70SdDmaCrcError;
    }
    if (Stop == T70SdDmaDeadline || (Events & T70_SD_DMA_IRQ_TIMEOUT) != 0) {
        return T70SdDmaTimeout;
    }
    return (Events & T70_SD_DMA_IRQ_COMPLETE) != 0 ? T70SdDmaComplete : T70SdDmaPending;
}

T70_SD_DMA_RESULT
T70SdUpdateDma(T70_SD_DMA *Transfer, uint32_t CapturedEvents, T70_SD_DMA_STOP Stop)
{
    if (Transfer == NULL || (unsigned)Stop > T70SdDmaDeadline) {
        return T70SdDmaInvalid;
    }
    if (Transfer->Phase == T70SdDmaIdle) {
        return T70SdDmaReady;
    }
    const T70_SD_DMA_IO *Io = &Transfer->Io;
    uint32_t Events = Io->Read32(Io->Context, MSDC_INT);
    if (Transfer->Phase == T70SdDmaArmed && Events == UINT32_MAX) {
        Transfer->Phase = T70SdDmaIdle;
        return T70SdDmaUnavailable;
    }
    Events = (Events | CapturedEvents) & T70_SD_DMA_IRQ_MASK;
    T70_SD_DMA_RESULT Result = DmaResult(Events, Stop);
    if (Transfer->Phase == T70SdDmaArmed && Stop == T70SdDmaKeepRunning &&
        (Result == T70SdDmaPending || Result == T70SdDmaComplete)) {
        return T70SdDmaPending;
    }
    if (Events != 0) {
        Io->Write32(Io->Context, MSDC_INT, Events);
    }
    if (Transfer->Phase == T70SdDmaArmed) {
        Transfer->Phase = T70SdDmaIdle;
        return Result;
    }
    if (Transfer->Phase == T70SdDmaRunning) {
        if (Result == T70SdDmaPending) {
            return Result;
        }
        uint32_t Control = Io->Read32(Io->Context, MSDC_DMA_CONTROL);
        Transfer->Phase = T70SdDmaDraining;
        Transfer->Result = Result;
        Io->Write32(Io->Context, MSDC_DMA_CONTROL, (Control & ~MSDC_DMA_COMMAND_BITS) | MSDC_DMA_STOP);
    }

    bool Stopped = (Io->Read32(Io->Context, MSDC_DMA_CONTROL) & MSDC_DMA_STOP) == 0 &&
                   (Io->Read32(Io->Context, MSDC_DMA_CONFIG) & MSDC_DMA_ACTIVE) == 0;
    if (Stopped) {
        /* The engine can raise its final error while accepting STOP. */
        uint32_t FinalEvents = Io->Read32(Io->Context, MSDC_INT) & T70_SD_DMA_IRQ_MASK;
        if (FinalEvents != 0) {
            Io->Write32(Io->Context, MSDC_INT, FinalEvents);
            Result = DmaResult(Events | FinalEvents, Stop);
        }
    }
    if (Result == T70SdDmaRemoved ||
        (Result == T70SdDmaCanceled && Transfer->Result != T70SdDmaRemoved) ||
        (Transfer->Result == T70SdDmaComplete &&
         Result != T70SdDmaPending && Result != T70SdDmaComplete)) {
        Transfer->Result = Result;
    }
    if (!Stopped) {
        return T70SdDmaPending;
    }
    Io->Fence(Io->Context);
    Transfer->Phase = T70SdDmaIdle;
    return Transfer->Result;
}
