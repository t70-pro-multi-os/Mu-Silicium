// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdCommand.h"

#define MSDC_PS 0x008U
#define MSDC_INT 0x00cU
#define SDC_CMD 0x034U
#define SDC_ARG 0x038U
#define SDC_STS 0x03cU
#define SDC_RESP0 0x040U
#define SDC_BLK_NUM 0x050U

#define MSDC_PS_DAT0 (1U << 16)
#define SDC_STS_BUSY 1U
#define SDC_STS_CMDBUSY (1U << 1)
#define SDC_CMD_SINGLE_BLOCK (1U << 11)
#define SDC_CMD_MULTI_BLOCK (1U << 12)
#define SDC_CMD_WRITE (1U << 13)
#define SDC_CMD_STOP (1U << 14)
#define SDC_CMD_AUTO12 (1U << 28)
#define SD_STOP_TRANSMISSION 12U

static const uint8_t MsdcResponseType[] = {0, 1, 2, 3, 4, 7};

T70_SD_COMMAND_RESULT
T70SdIssueCommand(T70_SD_COMMAND *Command, const T70_SD_DMA_IO *Io,
                 const T70_SD_COMMAND_REQUEST *Request)
{
    uint32_t Status;
    uint32_t BusyMask = SDC_STS_CMDBUSY;
    uint32_t RawCommand;
    size_t Index;

    if (Command == NULL || Io == NULL || Io->Read32 == NULL ||
        Io->Write32 == NULL || Io->Fence == NULL || Request == NULL ||
        Request->Index > 63U || (unsigned)Request->Response > T70SdResponseBusy ||
        Request->BlockBytes > 0xfffU ||
        ((Request->Blocks == 0) != (Request->BlockBytes == 0)) ||
        (Request->AutoStop && Request->Blocks < 2U) ||
        (Request->Write && Request->Blocks == 0) ||
        (Request->Blocks != 0 && Request->Response != T70SdResponseShort) ||
        (Request->Index == SD_STOP_TRANSMISSION && Request->Blocks != 0)) {
        return T70SdCommandInvalid;
    }
    if (Command->Phase != T70SdCommandIdle || Command->NeedsReset) {
        return T70SdCommandUnavailable;
    }

    RawCommand = Request->Index | ((uint32_t)MsdcResponseType[Request->Response] << 7);
    if (Request->Index == SD_STOP_TRANSMISSION) {
        /* CMD12 stops an in-flight data transaction. Wait only for CMD. */
        RawCommand |= SDC_CMD_STOP;
    } else if (Request->Blocks != 0 || Request->Response == T70SdResponseBusy) {
        BusyMask |= SDC_STS_BUSY;
    }
    Status = Io->Read32(Io->Context, SDC_STS);
    if (Status == UINT32_MAX) {
        Command->NeedsReset = true;
        return T70SdCommandUnavailable;
    }
    if ((Status & BusyMask) != 0) {
        return T70SdCommandUnavailable;
    }

    Io->Write32(Io->Context, MSDC_INT, T70_SD_COMMAND_IRQ_MASK);
    Io->Fence(Io->Context);
    if ((Io->Read32(Io->Context, MSDC_INT) & T70_SD_COMMAND_IRQ_MASK) != 0) {
        Command->NeedsReset = true;
        return T70SdCommandUnavailable;
    }
    if (Request->Blocks != 0) {
        RawCommand |= Request->Blocks == 1 ? SDC_CMD_SINGLE_BLOCK : SDC_CMD_MULTI_BLOCK;
        RawCommand |= (uint32_t)Request->BlockBytes << 16;
        if (Request->Write) {
            RawCommand |= SDC_CMD_WRITE;
        }
        if (Request->AutoStop) {
            RawCommand |= SDC_CMD_AUTO12;
        }
        Io->Write32(Io->Context, SDC_BLK_NUM, Request->Blocks);
    }

    Command->Io = Io;
    Command->Phase = T70SdCommandResponse;
    Command->Response = Request->Response;
    for (Index = 0; Index < 4; ++Index) {
        Command->ResponseWords[Index] = 0;
    }
    Io->Write32(Io->Context, SDC_ARG, Request->Argument);
    Io->Fence(Io->Context);
    Io->Write32(Io->Context, SDC_CMD, RawCommand);
    return T70SdCommandPending;
}

static void
ReadResponse(T70_SD_COMMAND *Command)
{
    const T70_SD_DMA_IO *Io = Command->Io;
    uint32_t Word;
    size_t Index;

    if (Command->Response == T70SdResponseNone) {
        return;
    }
    Word = Io->Read32(Io->Context, SDC_RESP0);
    if (Command->Response != T70SdResponseLong) {
        Command->ResponseWords[0] = Word;
        return;
    }

    /* MSDC leaves CRC/end in the low byte of the 128-bit register bank. */
    for (Index = 0; Index < 3; ++Index) {
        uint32_t Next = Io->Read32(Io->Context, SDC_RESP0 + 4U * ((uint32_t)Index + 1U));
        Command->ResponseWords[Index] = (Word >> 8) | (Next << 24);
        Word = Next;
    }
    Command->ResponseWords[3] = Word >> 8;
}

static T70_SD_COMMAND_RESULT
FinishCommand(T70_SD_COMMAND *Command, T70_SD_COMMAND_RESULT Result, bool NeedsReset)
{
    Command->Phase = T70SdCommandIdle;
    Command->Io = NULL;
    Command->NeedsReset = NeedsReset;
    return Result;
}

T70_SD_COMMAND_RESULT
T70SdUpdateCommand(T70_SD_COMMAND *Command, uint32_t CapturedEvents,
                  T70_SD_DMA_STOP Stop)
{
    const T70_SD_DMA_IO *Io;
    uint32_t Events;
    uint32_t Pins;

    if (Command == NULL || (unsigned)Stop > T70SdDmaDeadline) {
        return T70SdCommandInvalid;
    }
    if (Command->Phase == T70SdCommandIdle) {
        return T70SdCommandReady;
    }
    if (Stop == T70SdDmaMediaRemoved) {
        return FinishCommand(Command, T70SdCommandRemoved, true);
    }
    if (Stop == T70SdDmaCancel) {
        return FinishCommand(Command, T70SdCommandCanceled, true);
    }
    if (Stop == T70SdDmaDeadline) {
        return FinishCommand(Command, T70SdCommandTimeout, true);
    }

    Io = Command->Io;
    if (Command->Phase == T70SdCommandResponse) {
        Events = Io->Read32(Io->Context, MSDC_INT);
        if (Events == UINT32_MAX) {
            return FinishCommand(Command, T70SdCommandUnavailable, true);
        }
        Events = (Events | CapturedEvents) & T70_SD_COMMAND_IRQ_MASK;
        if (Events == 0) {
            return T70SdCommandPending;
        }
        Io->Write32(Io->Context, MSDC_INT, Events);
        if ((Events & T70_SD_COMMAND_IRQ_TIMEOUT) != 0) {
            return FinishCommand(Command, T70SdCommandTimeout, false);
        }
        if ((Events & T70_SD_COMMAND_IRQ_CRC) != 0) {
            return FinishCommand(Command, T70SdCommandCrcError, false);
        }
        ReadResponse(Command);
        if (Command->Response != T70SdResponseBusy) {
            return FinishCommand(Command, T70SdCommandComplete, false);
        }
        Command->Phase = T70SdCommandCardBusy;
    }
    Pins = Io->Read32(Io->Context, MSDC_PS);
    if (Pins == UINT32_MAX) {
        return FinishCommand(Command, T70SdCommandUnavailable, true);
    }
    if ((Pins & MSDC_PS_DAT0) == 0) {
        return T70SdCommandPending;
    }
    return FinishCommand(Command, T70SdCommandComplete, false);
}
