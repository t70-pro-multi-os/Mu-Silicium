// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdDma.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHECK(Condition) do { ++Checks; assert(Condition); } while (0)

static unsigned Checks;
static uint8_t Table[T70_SD_DMA_TABLE_BYTES];

static uint32_t
Le32(const uint8_t *Bytes)
{
    return Bytes[0] | (uint32_t)Bytes[1] << 8 |
           (uint32_t)Bytes[2] << 16 | (uint32_t)Bytes[3] << 24;
}

static void
CheckDescriptor(const uint8_t *Bytes, uint32_t Flags, uint32_t Next,
                uint32_t Address, uint32_t Length)
{
    unsigned Sum = 0;
    for (unsigned Index = 0; Index < 16; ++Index) {
        Sum += Bytes[Index];
    }
    CHECK((Sum & 0xff) == 0xff);
    CHECK((Le32(Bytes) & ~0xff00U) == Flags);
    CHECK(Le32(Bytes + 4) == Next);
    CHECK(Le32(Bytes + 8) == Address);
    CHECK(Le32(Bytes + 12) == Length);
}

static void
CheckChain(void)
{
    const T70_SD_DMA_SEGMENT Segments[] = {
        {0x40000, 0x10000}, {0x81000, 0x200}, {0xfffffe00, 0x200}
    };
    memset(Table, 0x5a, sizeof(Table));
    CHECK(T70SdBuildDmaChain(Table, sizeof(Table), 0x12000, Segments, 3, 0x10400));
    CheckDescriptor(Table, 3, 0x1201c, 0x12038, 0);
    for (unsigned Index = 16; Index < 56; ++Index) {
        CHECK(Table[Index] == 0);
    }
    CheckDescriptor(Table + 56, 0, 0x12048, 0x40000, 0xfffc);
    CheckDescriptor(Table + 72, 0, 0x12058, 0x4fffc, 4);
    CheckDescriptor(Table + 88, 0, 0x12068, 0x81000, 0x200);
    CheckDescriptor(Table + 104, 1, 0, 0xfffffe00, 0x200);
    for (size_t Index = 120; Index < sizeof(Table); ++Index) {
        CHECK(Table[Index] == 0x5a);
    }

    const T70_SD_DMA_SEGMENT Edge = {0x80000, 4};
    CHECK(T70SdBuildDmaChain(Table, 72, 0xffffffb8, &Edge, 1, 4));
    CheckDescriptor(Table, 3, 0xffffffd4, 0xfffffff0, 0);
    CheckDescriptor(Table + 56, 1, 0, 0x80000, 4);

    const T70_SD_DMA_SEGMENT Adjacent[] = {{0x10000 - 4, 4}, {0x10000 + 88, 4}};
    CHECK(T70SdBuildDmaChain(Table, 88, 0x10000, Adjacent, 2, 8));
    CheckDescriptor(Table + 56, 0, 0x10048, 0xfffc, 4);
    CheckDescriptor(Table + 72, 1, 0, 0x10058, 4);

    T70_SD_DMA_SEGMENT Large = {0x10000000, 1024U * 0xfffc};
    CHECK(T70SdBuildDmaChain(Table, sizeof(Table), 0x10000, &Large, 1, Large.Length));
    for (unsigned Index = 0; Index < 1024; ++Index) {
        uint32_t Offset = 56 + 16 * Index;
        bool Last = Index == 1023;
        CheckDescriptor(Table + Offset, Last, Last ? 0 : 0x10000 + Offset + 16,
                        0x10000000 + Index * 0xfffc, 0xfffc);
    }
    Large.Length += 4;
    uint8_t Previous[sizeof(Table)];
    memcpy(Previous, Table, sizeof(Table));
    CHECK(!T70SdBuildDmaChain(Table, sizeof(Table), 0x10000, &Large, 1, Large.Length));
    CHECK(memcmp(Previous, Table, sizeof(Table)) == 0);
}

static void
CheckInvalidChain(void)
{
    const struct {
        uint64_t TableAddress;
        size_t TableBytes;
        uint64_t Address;
        uint32_t Length;
        uint32_t Expected;
    } Cases[] = {
        {0x10000, 71, 0x20000, 4, 4},
        {0x10001, 72, 0x20000, 4, 4},
        {0xffffffbc, 72, 0x20000, 4, 4},
        {0x100000000, 72, 0x20000, 4, 4},
        {0x10000, 72, 0x20001, 4, 4},
        {0x10000, 72, 0x20000, 0, 0},
        {0x10000, 72, 0x20000, 3, 3},
        {0x10000, 72, 0xfffffffc, 8, 8},
        {0x10000, 72, 0x100000000, 4, 4},
        {0x10000, 72, 0x20000, 4, 0},
        {0x10000, 72, 0x20000, 4, 8},
        {0x10000, 72, 0x20000, 8, 4},
        {0x10000, 72, 0x10044, 4, 4},
        {0x10000, 72, 0xfffc, 8, 8},
        {0x10000, 72, 0x10000, 4, 4},
    };
    uint8_t Original[sizeof(Table)];
    memset(Table, 0x5a, sizeof(Table));
    memcpy(Original, Table, sizeof(Table));
    for (unsigned Index = 0; Index < sizeof(Cases) / sizeof(Cases[0]); ++Index) {
        T70_SD_DMA_SEGMENT Segment = {Cases[Index].Address, Cases[Index].Length};
        CHECK(!T70SdBuildDmaChain(Table, Cases[Index].TableBytes, Cases[Index].TableAddress,
                                     &Segment, 1, Cases[Index].Expected));
        CHECK(memcmp(Original, Table, sizeof(Table)) == 0);
    }
    const T70_SD_DMA_SEGMENT Segment = {0x20000, 4};
    CHECK(!T70SdBuildDmaChain(NULL, 72, 0x10000, &Segment, 1, 4));
    CHECK(!T70SdBuildDmaChain(Table, 72, 0x10000, NULL, 1, 4));
    CHECK(!T70SdBuildDmaChain(Table, 72, 0x10000, &Segment, 0, 4));
    CHECK(!T70SdBuildDmaChain(Table, 72, 0x10000, &Segment, 1025, 4));
    const T70_SD_DMA_SEGMENT BadSecond[] = {{0x20000, 4}, {0x30000, 0}};
    CHECK(!T70SdBuildDmaChain(Table, 88, 0x10000, BadSecond, 2, 4));
    CHECK(memcmp(Original, Table, sizeof(Table)) == 0);
}

enum {
    Config = 0x000,
    Interrupt = 0x00c,
    AddressHigh = 0x08c,
    Address = 0x090,
    Control = 0x098,
    DmaConfig = 0x09c,
    Active = 1,
    Start = 1,
    Stop = 2,
    CommandIrq = 0x100,
    DataIrqs = 0xed000,
};

typedef struct {
    uint32_t Registers[40];
    unsigned Writes;
    unsigned Fences;
    unsigned Starts;
    unsigned Stops;
    uint32_t EventsOnInactive;
} MSDC_MOCK;

static uint32_t
ReadMock(void *Context, uint32_t Offset)
{
    MSDC_MOCK *Mock = Context;
    CHECK(Offset / 4 < 40);
    uint32_t Value = Mock->Registers[Offset / 4];
    if (Offset == DmaConfig && (Value & Active) == 0) {
        Mock->Registers[Interrupt / 4] |= Mock->EventsOnInactive;
        Mock->EventsOnInactive = 0;
    }
    return Value;
}

static void
WriteMock(void *Context, uint32_t Offset, uint32_t Value)
{
    MSDC_MOCK *Mock = Context;
    ++Mock->Writes;
    switch (Offset) {
    case Interrupt:
        CHECK((Value & ~DataIrqs) == 0);
        Mock->Registers[Interrupt / 4] &= ~Value;
        return;
    case Control:
        if (Value & Start) {
            CHECK((Mock->Registers[DmaConfig / 4] & Active) == 0);
            CHECK((Value & 0x7100) == 0x6100);
            CHECK(Mock->Fences > Mock->Starts);
            ++Mock->Starts;
            Mock->Registers[DmaConfig / 4] |= Active;
            Value &= ~Start;
        }
        Mock->Stops += (Value & Stop) != 0;
        break;
    case Config:
    case AddressHigh:
    case Address:
    case DmaConfig:
        break;
    default:
        CHECK(false);
    }
    Mock->Registers[Offset / 4] = Value;
}

static void
FenceMock(void *Context)
{
    MSDC_MOCK *Mock = Context;
    ++Mock->Fences;
}

static T70_SD_DMA_RESULT
Arm(T70_SD_DMA *Transfer, MSDC_MOCK *Mock)
{
    const T70_SD_DMA_IO Io = {Mock, ReadMock, WriteMock, FenceMock};
    const T70_SD_DMA_SEGMENT Segment = {0x40000, 512};
    return T70SdArmDma(Transfer, &Io, Table, sizeof(Table), 0x12000, &Segment, 1, 512);
}

static void
CheckArming(void)
{
    MSDC_MOCK Mock = {0};
    T70_SD_DMA Transfer = {0};
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaUnavailable);
    Mock.Registers[Config / 4] = 0xd;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaUnavailable);
    Mock.Registers[Config / 4] = 0x40009;
    Mock.Registers[DmaConfig / 4] = Active;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaUnavailable);
    Mock.Registers[DmaConfig / 4] = 0;
    Mock.Registers[Control / 4] = Stop;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaUnavailable);
    CHECK(Mock.Writes == 0);

    Mock.Registers[DmaConfig / 4] = 1U << 13;
    Mock.Registers[Control / 4] = 0x8425;
    Mock.Registers[AddressHigh / 4] = 0x5f;
    Mock.Registers[Interrupt / 4] = CommandIrq | DataIrqs;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaReady);
    CHECK(Transfer.Phase == T70SdDmaArmed);
    CHECK(Mock.Registers[Config / 4] == 0x40001);
    CHECK(Mock.Registers[DmaConfig / 4] == 0x2002);
    CHECK(Mock.Registers[Control / 4] == 0xe520);
    CHECK(Mock.Registers[AddressHigh / 4] == 0x50);
    CHECK(Mock.Registers[Address / 4] == 0x12000);
    CHECK(Mock.Registers[Interrupt / 4] == CommandIrq);
    CHECK(Mock.Writes == 6 && Mock.Starts == 0 && Mock.Fences == 0);
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaUnavailable);
    CHECK(T70SdUpdateDma(&Transfer, 0, (T70_SD_DMA_STOP)-1) == T70SdDmaInvalid);
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaPending);
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaCancel) == T70SdDmaCanceled);
    CHECK(Transfer.Phase == T70SdDmaIdle);
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaReady);
    CHECK(T70SdStartDma(&Transfer) == T70SdDmaInvalid);
    CHECK(Mock.Writes == 6 && Mock.Starts == 0 && Mock.Stops == 0);
}

static void
CheckBeforeStart(void)
{
    MSDC_MOCK Mock = {0};
    T70_SD_DMA Transfer = {0};
    Mock.Registers[Config / 4] = 1;

    CHECK(Arm(&Transfer, &Mock) == T70SdDmaReady);
    Mock.Registers[Control / 4] = UINT32_MAX;
    CHECK(T70SdStartDma(&Transfer) == T70SdDmaUnavailable && Mock.Starts == 0);
    Mock.Registers[Interrupt / 4] = UINT32_MAX;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaUnavailable);
    CHECK(Transfer.Phase == T70SdDmaIdle && Mock.Writes == 6);

    Mock.Registers[Control / 4] = 0;
    Mock.Registers[Interrupt / 4] = 0;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaReady);
    Mock.Registers[Interrupt / 4] = T70_SD_DMA_IRQ_COMPLETE | T70_SD_DMA_IRQ_TIMEOUT;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaTimeout);
    CHECK(Transfer.Phase == T70SdDmaIdle && Mock.Starts == 0 && Mock.Stops == 0);
}

static void
CheckStop(uint32_t Events, T70_SD_DMA_STOP Reason, uint32_t LateEvents, T70_SD_DMA_RESULT Expected)
{
    MSDC_MOCK Mock = {0};
    T70_SD_DMA Transfer = {0};
    Mock.Registers[Config / 4] = 9;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaReady);
    CHECK(T70SdStartDma(&Transfer) == T70SdDmaPending);
    CHECK(T70SdUpdateDma(&Transfer, CommandIrq, T70SdDmaKeepRunning) == T70SdDmaPending);
    CHECK(Transfer.Phase == T70SdDmaRunning && Mock.Stops == 0);
    Mock.Registers[Interrupt / 4] = CommandIrq;
    CHECK(T70SdUpdateDma(&Transfer, Events, Reason) == T70SdDmaPending);
    CHECK(Transfer.Phase == T70SdDmaDraining && Mock.Stops == 1);
    CHECK(Mock.Registers[Interrupt / 4] == CommandIrq);
    CHECK(T70SdStartDma(&Transfer) == T70SdDmaInvalid);
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaUnavailable);

    Mock.Registers[DmaConfig / 4] &= ~Active;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaPending);
    CHECK(Mock.Fences == 1);
    Mock.Registers[Control / 4] &= ~Stop;
    Mock.Registers[DmaConfig / 4] |= Active;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaPending);
    Mock.Registers[DmaConfig / 4] &= ~Active;
    Mock.EventsOnInactive = LateEvents;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == Expected);
    CHECK(Transfer.Phase == T70SdDmaIdle && Mock.Fences == 2 && Mock.Stops == 1);
    CHECK(Mock.Registers[Interrupt / 4] == CommandIrq);
    CHECK(T70SdUpdateDma(&Transfer, Events, Reason) == T70SdDmaReady);

    Mock.Registers[Interrupt / 4] |= 1U << 15;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaReady);
    CHECK(T70SdStartDma(&Transfer) == T70SdDmaPending);
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaPending);
    Mock.Registers[Interrupt / 4] |= 1U << 12;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaPending);
    Mock.Registers[DmaConfig / 4] &= ~Active;
    Mock.Registers[Control / 4] &= ~Stop;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaKeepRunning) == T70SdDmaComplete);
    CHECK(Mock.Fences == 4 && Mock.Stops == 2 && Mock.Starts == 2);
}

static void
CheckRemovalAfterComplete(void)
{
    MSDC_MOCK Mock = {0};
    T70_SD_DMA Transfer = {0};
    Mock.Registers[Config / 4] = 1;
    CHECK(Arm(&Transfer, &Mock) == T70SdDmaReady);
    CHECK(T70SdStartDma(&Transfer) == T70SdDmaPending);
    CHECK(T70SdUpdateDma(&Transfer, 1U << 12, T70SdDmaKeepRunning) == T70SdDmaPending);
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaMediaRemoved) == T70SdDmaPending);
    Mock.Registers[DmaConfig / 4] &= ~Active;
    Mock.Registers[Control / 4] &= ~Stop;
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaCancel) == T70SdDmaRemoved);
    CHECK(Mock.Stops == 1);
    CHECK(T70SdUpdateDma(&Transfer, 0, T70SdDmaMediaRemoved) == T70SdDmaReady);
}

int
main(void)
{
    CheckChain();
    CheckInvalidChain();
    CheckArming();
    CheckBeforeStart();
    CheckStop(1U << 12, T70SdDmaKeepRunning, 0, T70SdDmaComplete);
    CheckStop((1U << 12) | (1U << 15), T70SdDmaKeepRunning, 0, T70SdDmaCrcError);
    CheckStop(1U << 12, T70SdDmaKeepRunning, 1U << 15, T70SdDmaCrcError);
    CheckStop(1U << 14, T70SdDmaKeepRunning, 0, T70SdDmaTimeout);
    CheckStop(0, T70SdDmaDeadline, 0, T70SdDmaTimeout);
    CheckStop(0, T70SdDmaCancel, 0, T70SdDmaCanceled);
    CheckStop(0, T70SdDmaMediaRemoved, 0, T70SdDmaRemoved);
    CheckStop(1U << 17, T70SdDmaKeepRunning, 0, T70SdDmaHardwareError);
    CheckStop(1U << 18, T70SdDmaKeepRunning, 0, T70SdDmaHardwareError);
    CheckStop(1U << 19, T70SdDmaKeepRunning, 0, T70SdDmaHardwareError);
    CheckRemovalAfterComplete();
    printf("T70 MSDC DMA: %u checks passed\n", Checks);
    return 0;
}
