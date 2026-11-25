// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdHost.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned Checks;
#define CHECK(Condition) do {                                               \
    ++Checks;                                                              \
    if (!(Condition)) {                                                     \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #Condition);       \
        exit(1);                                                            \
    }                                                                       \
} while (0)

enum {
    Config = 0, Int = 0x0c, IntEnable = 0x10, Fifo = 0x14, CardConfig = 0x30,
    Status = 0x3c, DmaConfig = 0x9c, Patch0 = 0xb0, Patch1 = 0xb4,
    Patch2 = 0xb8, EmmcConfig = 0x208, FifoConfig = 0x228, ClockStable = 0x80,
    CardClock = 2
};

typedef struct {
    uint32_t Registers[FifoConfig / 4 + 1];
    unsigned Writes;
    unsigned PadSetups;
    unsigned Delayed;
    unsigned ClockSwitches;
    unsigned ReadyAt;
    bool Source;
    bool ClockFault;
    bool GateFault;
    bool CoreFault;
    bool FifoFault;
    bool PadFault;
} HOST;

static uint32_t
Read(void *Context, uint32_t Offset)
{
    HOST *Host = Context;
    CHECK(Offset % 4 == 0 && Offset <= FifoConfig);
    return Host->Registers[Offset / 4];
}

static void
Write(void *Context, uint32_t Offset, uint32_t Value)
{
    HOST *Host = Context;
    uint32_t *Register;

    CHECK(Offset % 4 == 0 && Offset <= FifoConfig);
    ++Host->Writes;
    Register = &Host->Registers[Offset / 4];
    if (Offset == Int) {
        *Register &= ~Value;
        return;
    }
    if (Offset == Config) {
        CHECK(((Value ^ *Register) & 0x007fff00) == 0 || !Host->Source);
        if ((Value & CardClock) != 0) {
            CHECK(Host->Source && (*Register & ClockStable) != 0);
        }
        Value = (Value & ~ClockStable) | (*Register & ClockStable);
    }
    *Register = Value;
}

static void
Fence(void *Context)
{
    CHECK(Context != NULL);
}

static bool
Gate(void *Context, bool Enabled)
{
    HOST *Host = Context;
    ++Host->ClockSwitches;
    CHECK((Host->Registers[Config / 4] & CardClock) == 0);
    if (Host->GateFault) {
        return false;
    }
    Host->Source = Enabled;
    Host->ReadyAt = Host->Delayed + 3;
    Host->Registers[Config / 4] &= ~ClockStable;
    return true;
}

static bool
SetPads(void *Context)
{
    HOST *Host = Context;
    CHECK(Host->Source);
    ++Host->PadSetups;
    return !Host->PadFault;
}

static void
Delay(void *Context, uint32_t Microseconds)
{
    HOST *Host = Context;
    CHECK(Microseconds == 1);
    ++Host->Delayed;
    if (Host->Source && !Host->ClockFault && Host->Delayed >= Host->ReadyAt) {
        Host->Registers[Config / 4] |= ClockStable;
    }
    if (!Host->CoreFault) {
        Host->Registers[Config / 4] &= ~4U;
    }
    if (!Host->FifoFault) {
        Host->Registers[Fifo / 4] &= ~(1U << 31);
    }
}

int
main(void)
{
    HOST Host = {0};
    const T70_SD_DMA_IO Registers = {&Host, Read, Write, Fence};
    const T70_SD_HOST_IO Io = {&Registers, &Host, Gate, SetPads, Delay};
    uint32_t Actual = UINT32_MAX;
    uint32_t Rate;
    unsigned Writes;
    unsigned Delayed;

    CHECK(Gate(&Host, true));
    Host.Registers[IntEnable / 4] = 0x1234;
    Host.Registers[Int / 4] = 0x9f00;
    Host.Registers[CardConfig / 4] = 0x97930030;
    Host.Registers[FifoConfig / 4] = 0x53a45678;
    Host.Registers[Patch2 / 4] = 0x204080f0;
    CHECK(T70SdResetHost(&Io));
    CHECK(Host.PadSetups == 1 && Host.Delayed == 2);
    CHECK(Host.Registers[Config / 4] == 9);
    CHECK(Host.Registers[Int / 4] == 0 && Host.Registers[IntEnable / 4] == 0);
    CHECK(Host.Registers[Patch0 / 4] == 0x403c0446);
    CHECK(Host.Registers[Patch1 / 4] == 0xffff4309);
    CHECK(Host.Registers[Patch2 / 4] == 0x304000fc);
    CHECK(Host.Registers[EmmcConfig / 4] == 0x10);
    CHECK(Host.Registers[CardConfig / 4] == 0x97880030);
    CHECK(Host.Registers[FifoConfig / 4] == 0x50a45678);
    CHECK(T70SdSetBusWidth(&Io, 4));
    CHECK(Host.Registers[CardConfig / 4] == 0x97890030);
    CHECK(!T70SdSetBusWidth(&Io, 8));

    CHECK(T70SdSetCardClock(&Io, 400000, &Actual));
    CHECK(Actual == 382352 && (Host.Registers[Config / 4] & 0xfff00) == 0x1100);
    CHECK((Host.Registers[Config / 4] & (ClockStable | CardClock)) == (ClockStable | CardClock));
    CHECK(Host.Registers[CardConfig / 4] == 0x03890030);
    CHECK(T70SdSetCardClock(&Io, 25000000, &Actual));
    CHECK(Actual == 13000000 && (Host.Registers[Config / 4] & 0xfff00) == 0);
    CHECK((Host.Registers[CardConfig / 4] >> 24) == 4);
    for (Rate = 1588; Rate < 25000000; Rate += 8191) {
        CHECK(T70SdSetCardClock(&Io, Rate, &Actual));
        CHECK(Actual != 0 && Actual <= Rate);
        CHECK(Actual == 13000000 ||
              (uint64_t)(Actual + 1) * (4U * ((Host.Registers[Config / 4] >> 8) & 0xfffU)) > 26000000U);
    }
    CHECK(T70SdSetCardClock(&Io, 0, &Actual) && Actual == 0 && !Host.Source);

    Writes = Host.Writes;
    CHECK(!T70SdSetCardClock(&Io, 1587, &Actual));
    CHECK(!T70SdSetCardClock(&Io, 25000001, &Actual));
    Host.Registers[Status / 4] = 2;
    CHECK(!T70SdSetCardClock(&Io, 400000, &Actual));
    CHECK(!T70SdSetBusWidth(&Io, 1));
    Host.Registers[Status / 4] = 0;
    Host.Registers[DmaConfig / 4] = 1;
    CHECK(!T70SdSetCardClock(&Io, 400000, &Actual));
    CHECK(!T70SdResetHost(&Io));
    CHECK(Host.Writes == Writes && Actual == 0);
    Host.Registers[DmaConfig / 4] = 0;

    Host.ClockFault = true;
    Delayed = Host.Delayed;
    CHECK(!T70SdSetCardClock(&Io, 400000, &Actual));
    CHECK(Host.Delayed == Delayed + 100 && !Host.Source && Actual == 0);
    CHECK((Host.Registers[Config / 4] & CardClock) == 0);
    Host.ClockFault = false;
    Host.GateFault = true;
    CHECK(!T70SdSetCardClock(&Io, 400000, &Actual));
    CHECK(!Host.Source && Actual == 0);
    Host.GateFault = false;
    CHECK(Gate(&Host, true));

    Delayed = Host.Delayed;
    Host.CoreFault = true;
    CHECK(!T70SdResetHost(&Io));
    CHECK(Host.Delayed == Delayed + 100 && Host.PadSetups == 1);
    Host.CoreFault = false;
    Host.FifoFault = true;
    Delayed = Host.Delayed;
    CHECK(!T70SdResetHost(&Io));
    CHECK(Host.Delayed == Delayed + 101 && Host.PadSetups == 1);
    Host.FifoFault = false;
    Host.PadFault = true;
    CHECK(!T70SdResetHost(&Io));
    CHECK(Host.PadSetups == 2);
    Host.PadFault = false;
    CHECK(T70SdResetHost(&Io));
    CHECK(T70SdSetCardClock(&Io, 400000, &Actual) && Actual == 382352);
    CHECK(Host.PadSetups == 3);
    printf("MSDC clock and reset checks passed: %u\n", Checks);
    return 0;
}
