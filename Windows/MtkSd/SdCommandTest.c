// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdCommand.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned Checks;
#define CHECK(Condition) do {                                              \
    ++Checks;                                                             \
    if (!(Condition)) {                                                    \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #Condition);      \
        exit(1);                                                           \
    }                                                                      \
} while (0)

enum {
    Pins = 0x08, Interrupt = 0x0c, Command = 0x34, Argument = 0x38,
    Status = 0x3c, Response0 = 0x40, BlockCount = 0x50
};

typedef struct {
    uint32_t Register[21];
    unsigned Writes;
    unsigned Commands;
    unsigned Responses;
    unsigned Fences;
    bool StickInterrupt;
} HOST;

static uint32_t
Read(void *Context, uint32_t Offset)
{
    HOST *Host = Context;
    CHECK((Offset & 3U) == 0 && Offset <= BlockCount);
    if (Offset >= Response0 && Offset < BlockCount) {
        ++Host->Responses;
    }
    return Host->Register[Offset / 4];
}

static void
Write(void *Context, uint32_t Offset, uint32_t Value)
{
    HOST *Host = Context;
    ++Host->Writes;
    if (Offset == Interrupt) {
        CHECK((Value & ~T70_SD_COMMAND_IRQ_MASK) == 0);
        if (!Host->StickInterrupt) {
            Host->Register[Offset / 4] &= ~Value;
        }
    } else {
        CHECK(Offset == Command || Offset == Argument || Offset == BlockCount);
        if (Offset == Command) {
            CHECK((Host->Register[Interrupt / 4] & T70_SD_COMMAND_IRQ_MASK) == 0);
            CHECK(Host->Fences >= 2 * (Host->Commands + 1));
            ++Host->Commands;
        }
        Host->Register[Offset / 4] = Value;
    }
}

static void
Fence(void *Context)
{
    HOST *Host = Context;
    ++Host->Fences;
}

static void
TestResponses(void)
{
    static const uint32_t RawResponse[] = {0, 0x80, 0x100, 0x180, 0x200, 0x380};
    HOST Host = {0};
    const T70_SD_DMA_IO Io = {&Host, Read, Write, Fence};
    T70_SD_COMMAND Slot = {0};
    T70_SD_COMMAND_REQUEST Request = {0};
    unsigned Type;
    unsigned Index;
    unsigned Reads;
    const uint32_t Unrelated = T70_SD_DMA_IRQ_COMPLETE | (1U << 3);

    Host.Register[Response0 / 4] = 0xfedcba98;
    Host.Register[Response0 / 4 + 1] = 0x76543210;
    Host.Register[Response0 / 4 + 2] = 0x01234567;
    Host.Register[Response0 / 4 + 3] = 0x89abcdef;
    Host.Register[Pins / 4] = 1U << 16;
    for (Type = 0; Type < sizeof(RawResponse) / sizeof(RawResponse[0]); ++Type) {
        for (Index = 0; Index <= 63; ++Index) {
            Request.Index = (uint8_t)Index;
            Request.Response = (T70_SD_COMMAND_RESPONSE)Type;
            Request.Argument = 0x10000 * Type + Index;
            Host.Register[Interrupt / 4] = Unrelated | T70_SD_COMMAND_IRQ_MASK;
            Reads = Host.Responses;
            CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
            CHECK(Host.Register[Argument / 4] == Request.Argument);
            CHECK(Host.Register[Command / 4] ==
                  (Index | RawResponse[Type] | (Index == 12U ? 0x4000U : 0)));
            CHECK(Host.Register[Interrupt / 4] == Unrelated);
            CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandUnavailable);
            CHECK(T70SdUpdateCommand(&Slot, 0, T70SdDmaKeepRunning) == T70SdCommandPending);
            if ((Index & 1U) != 0) {
                Host.Register[Interrupt / 4] |= T70_SD_COMMAND_IRQ_READY;
            }
            CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, T70SdDmaKeepRunning) ==
                  T70SdCommandComplete);
            CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, T70SdDmaKeepRunning) ==
                  T70SdCommandReady);
            CHECK(Host.Register[Interrupt / 4] == Unrelated);
            CHECK(!Slot.NeedsReset);
            if (Type == T70SdResponseLong) {
                CHECK(Host.Responses == Reads + 4);
                CHECK(Slot.ResponseWords[0] == 0x10fedcba && Slot.ResponseWords[1] == 0x67765432);
                CHECK(Slot.ResponseWords[2] == 0xef012345 && Slot.ResponseWords[3] == 0x0089abcd);
            } else {
                CHECK(Host.Responses == Reads + (Type != T70SdResponseNone));
                CHECK(Slot.ResponseWords[0] == (Type == T70SdResponseNone ? 0 : 0xfedcba98));
                CHECK(Slot.ResponseWords[1] == 0 && Slot.ResponseWords[2] == 0 && Slot.ResponseWords[3] == 0);
            }
        }
    }
}

static void
TestBusyAndStop(void)
{
    HOST Host = {0};
    const T70_SD_DMA_IO Io = {&Host, Read, Write, Fence};
    T70_SD_COMMAND Slot = {0};
    T70_SD_COMMAND_REQUEST Request = {0, 0, 0, 7, T70SdResponseBusy, false, false};
    const T70_SD_DMA_STOP Stops[] = {T70SdDmaCancel, T70SdDmaMediaRemoved, T70SdDmaDeadline};
    const T70_SD_COMMAND_RESULT Results[] = {T70SdCommandCanceled, T70SdCommandRemoved, T70SdCommandTimeout};
    size_t Index;

    Host.Register[Status / 4] = 1;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandUnavailable);
    CHECK(Host.Writes == 0);
    Request.Index = 12;
    Host.Register[Status / 4] = 3;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandUnavailable);
    Host.Register[Status / 4] = 1;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
    Host.Register[Interrupt / 4] = T70_SD_COMMAND_IRQ_READY;
    Host.Register[Response0 / 4] = 0x900;
    CHECK(T70SdUpdateCommand(&Slot, 0, T70SdDmaKeepRunning) == T70SdCommandPending);
    CHECK(Slot.Phase == T70SdCommandCardBusy && Host.Responses == 1);
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandUnavailable);
    CHECK(T70SdUpdateCommand(&Slot, 0, T70SdDmaKeepRunning) == T70SdCommandPending);
    Host.Register[Response0 / 4] = 0xbad;
    Host.Register[Pins / 4] = 1U << 16;
    CHECK(T70SdUpdateCommand(&Slot, 0, T70SdDmaKeepRunning) == T70SdCommandComplete);
    CHECK(Slot.ResponseWords[0] == 0x900 && Host.Responses == 1);

    for (Index = 0; Index < sizeof(Stops) / sizeof(Stops[0]); ++Index) {
        unsigned Writes;
        Host.Register[Pins / 4] = 0;
        CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
        if ((Index & 1U) != 0) {
            CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, T70SdDmaKeepRunning) ==
                  T70SdCommandPending);
        }
        Writes = Host.Writes;
        CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, Stops[Index]) == Results[Index]);
        CHECK(Slot.NeedsReset && Host.Writes == Writes);
        CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, Stops[Index]) == T70SdCommandReady);
        CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandUnavailable);
        CHECK(Host.Writes == Writes);
        Slot = (T70_SD_COMMAND){0};
    }
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
    Host.Register[Pins / 4] = UINT32_MAX;
    CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, T70SdDmaKeepRunning) ==
          T70SdCommandUnavailable);
    CHECK(Slot.NeedsReset);
}

static void
TestDataAndErrors(void)
{
    HOST Host = {0};
    const T70_SD_DMA_IO Io = {&Host, Read, Write, Fence};
    T70_SD_COMMAND Slot = {0};
    T70_SD_COMMAND_REQUEST Request = {0x345, 512, 1, 17, T70SdResponseShort, false, false};
    unsigned Events;

    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
    CHECK(Host.Register[BlockCount / 4] == 1 && Host.Register[Command / 4] == 0x02000891);
    CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, T70SdDmaKeepRunning) ==
          T70SdCommandComplete);
    Request.Index = 18;
    Request.Blocks = 65535;
    Request.AutoStop = true;
    for (Events = 2; Events < 8; ++Events) {
        unsigned Reads = Host.Responses;
        CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
        CHECK(Host.Register[BlockCount / 4] == 65535 && Host.Register[Command / 4] == 0x12001092);
        Host.Register[Interrupt / 4] = (Events << 8) | T70_SD_DMA_IRQ_CRC;
        CHECK(T70SdUpdateCommand(&Slot, 0, T70SdDmaKeepRunning) ==
              ((Events & 2U) != 0 ? T70SdCommandTimeout : T70SdCommandCrcError));
        CHECK(Host.Responses == Reads && !Slot.NeedsReset);
        CHECK(Host.Register[Interrupt / 4] == T70_SD_DMA_IRQ_CRC);
        CHECK(T70SdUpdateCommand(&Slot, 0, T70SdDmaKeepRunning) == T70SdCommandReady);
    }

    Request.BlockBytes = 4096;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandInvalid);
    Request.BlockBytes = 512;
    Request.Blocks = 0;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandInvalid);
    Request.Blocks = 1;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandInvalid);
    Request.AutoStop = false;
    Request.Index = 64;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandInvalid);
    Request.Index = 17;
    Request.Response = (T70_SD_COMMAND_RESPONSE)99;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandInvalid);
    Request.Response = T70SdResponseNone;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandInvalid);
    Request.Response = T70SdResponseShort;
    Host.Register[Status / 4] = UINT32_MAX;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandUnavailable && Slot.NeedsReset);
    Host.Register[Status / 4] = 0;
    Slot = (T70_SD_COMMAND){0};
    Host.StickInterrupt = true;
    Host.Register[Interrupt / 4] |= T70_SD_COMMAND_IRQ_READY;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandUnavailable && Slot.NeedsReset);
    CHECK(Host.Commands == 7);
    Host.StickInterrupt = false;
    Slot = (T70_SD_COMMAND){0};
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
    Host.Register[Interrupt / 4] = UINT32_MAX;
    CHECK(T70SdUpdateCommand(&Slot, 0, T70SdDmaKeepRunning) == T70SdCommandUnavailable);
    CHECK(Slot.NeedsReset);
}

static void
TestWriteCommands(void)
{
    HOST Host = {0};
    const T70_SD_DMA_IO Io = {&Host, Read, Write, Fence};
    T70_SD_COMMAND Slot = {0};
    T70_SD_COMMAND_REQUEST Request = {0x999, 512, 1, 24, T70SdResponseShort, false, true};

    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
    CHECK(Host.Register[BlockCount / 4] == 1 && Host.Register[Command / 4] == 0x02002898);
    CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, T70SdDmaKeepRunning) ==
          T70SdCommandComplete);
    Request.Index = 25;
    Request.Blocks = 2;
    Request.AutoStop = true;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandPending);
    CHECK(Host.Register[BlockCount / 4] == 2 && Host.Register[Command / 4] == 0x12003099);
    CHECK(T70SdUpdateCommand(&Slot, T70_SD_COMMAND_IRQ_READY, T70SdDmaKeepRunning) ==
          T70SdCommandComplete);
    Request.Blocks = Request.BlockBytes = 0;
    Request.AutoStop = false;
    CHECK(T70SdIssueCommand(&Slot, &Io, &Request) == T70SdCommandInvalid);
    CHECK(Host.Commands == 2);
}

int
main(void)
{
    TestResponses();
    TestBusyAndStop();
    TestDataAndErrors();
    TestWriteCommands();
    printf("MSDC command checks passed: %u\n", Checks);
    return 0;
}
