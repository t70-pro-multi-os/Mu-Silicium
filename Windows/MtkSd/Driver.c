// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntifs.h>
#include <acpiioct.h>
#include <sdport.h>
#include "SdCommand.h"
#include "SdHost.h"
#include "SdPower.h"

#define MSDC1_BASE 0x11240000ULL
#define MSDC1_REGISTER_BYTES 0x1000U
#define MSDC_CFG 0x000U
#define MSDC_PS 0x008U
#define MSDC_INT 0x00cU
#define MSDC_INTEN 0x010U
#define SDC_ACMD_RESP 0x080U
#define MSDC_DMA_CFG 0x09cU
#define MSDC_CARD_CLOCK 2U
#define MSDC_DAT0 (1U << 16)
#define MSDC_AUTO_STOP_OK (1U << 3)
#define MSDC_AUTO_STOP_TIMEOUT (1U << 4)
#define MSDC_AUTO_STOP_CRC (1U << 5)
#define MSDC_AUTO_STOP_MASK (MSDC_AUTO_STOP_OK | MSDC_AUTO_STOP_TIMEOUT | MSDC_AUTO_STOP_CRC)
#define SD_R1_ERRORS 0xfff9e008U
#define SD_R1_WP_VIOLATION (1U << 26)
#define SD_BUFFER_OFFSET 0x5000U
#define SD_MAX_DATA_BYTES 0x10000U
#define SD_COMMON_BYTES (SD_BUFFER_OFFSET + SD_MAX_DATA_BYTES)
#define SD_COMMAND_TIMEOUT_100NS 10000000ULL
#define SD_DATA_TIMEOUT_100NS 20000000ULL
#define SD_DMA_STOP_TIMEOUT_100NS 100000ULL
#define SD_DMA_QUIESCE_TIMEOUT_US 100000U
#define SD_DMA_QUIESCE_POLL_US 100U

C_ASSERT(T70_SD_DMA_TABLE_BYTES <= SD_BUFFER_OFFSET);

typedef struct {
    PVOID Registers;
    PDEVICE_OBJECT AcpiDevice;
    PDMA_ADAPTER DmaAdapter;
    PUCHAR CommonBuffer;
    PHYSICAL_ADDRESS LogicalAddress;
    MTK_SD_POWER Power;
    T70_SD_DMA_IO Io;
    T70_SD_HOST_IO Host;
    T70_SD_HOST_STATE SavedHost;
    T70_SD_COMMAND Command;
    T70_SD_DMA Dma;
    KSPIN_LOCK Lock;
    KMUTEX Control;
    KTIMER Timer;
    KDPC Dpc;
    PSDPORT_REQUEST ActiveRequest;
    ULONGLONG Deadline;
    ULONGLONG DrainDeadline;
    uint32_t CardClockHz;
    ULONG PendingClockHz;
    ULONG BusWidth;
    ULONG DataBytes;
    USHORT DataBlockSize;
    USHORT DataBlocksRemaining;
    NTSTATUS DataStatus;
    SDPORT_TRANSFER_DIRECTION DataDirection;
    BOOLEAN WaitDat0;
    BOOLEAN AutoStop;
    BOOLEAN AutoStopDone;
    BOOLEAN DmaDone;
    BOOLEAN HostClockOpen;
    BOOLEAN HostReady;
    BOOLEAN CardPresent;
    BOOLEAN CardRemoved;
    BOOLEAN Busy;
    BOOLEAN Faulted;
    BOOLEAN Removing;
    BOOLEAN Suspended;
    BOOLEAN ContextValid;
    BOOLEAN ResumeFailed;
} MTK_SD_SLOT;

static uint32_t
ReadRegister(void *Context, uint32_t Offset)
{
    return SdPortReadRegisterUlong(((MTK_SD_SLOT *)Context)->Registers, Offset);
}

static void
WriteRegister(void *Context, uint32_t Offset, uint32_t Value)
{
    SdPortWriteRegisterUlong(((MTK_SD_SLOT *)Context)->Registers, Offset, Value);
}

static void
Fence(void *Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeMemoryBarrier();
}

static void
Delay(void *Context, uint32_t Microseconds)
{
    UNREFERENCED_PARAMETER(Context);
    SdPortWait(Microseconds);
}

static NTSTATUS
AcpiComplete(PDEVICE_OBJECT Device, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static bool
AcpiInteger(MTK_SD_SLOT *Slot, const char Name[4], bool HasArgument, ULONG Argument, ULONGLONG *Value)
{
    union {
        ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER Input;
        ACPI_EVAL_OUTPUT_BUFFER Acpi;
        UCHAR Bytes[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 8];
    } Buffer = {0};
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;
    ULONG InputBytes;
    USHORT ValueBytes;

    if (Slot->AcpiDevice == NULL) {
        return false;
    }
    Buffer.Input.Signature = HasArgument ? ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE :
                                          ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(Buffer.Input.MethodName, Name, 4);
    Buffer.Input.IntegerArgument = Argument;
    InputBytes = HasArgument ? sizeof(Buffer.Input) : sizeof(ACPI_EVAL_INPUT_BUFFER);
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    // SDPORT also permits APC_LEVEL callers. Complete without a thread APC.
    Irp = IoAllocateIrp(Slot->AcpiDevice->StackSize, FALSE);
    if (Irp == NULL) {
        return false;
    }
    Irp->RequestorMode = KernelMode;
    Irp->Flags = IRP_BUFFERED_IO;
    Irp->AssociatedIrp.SystemBuffer = &Buffer;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    Stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_ACPI_EVAL_METHOD;
    Stack->Parameters.DeviceIoControl.InputBufferLength = InputBytes;
    Stack->Parameters.DeviceIoControl.OutputBufferLength = sizeof(Buffer);
    IoSetCompletionRoutine(Irp, AcpiComplete, &Event, TRUE, TRUE, TRUE);
    Status = IoCallDriver(Slot->AcpiDevice, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }
    IoStatus = Irp->IoStatus;
    IoFreeIrp(Irp);
    if (Status != STATUS_SUCCESS || IoStatus.Information < sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
        IoStatus.Information > sizeof(Buffer) ||
        Buffer.Acpi.Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Buffer.Acpi.Count != 1 ||
        Buffer.Acpi.Length > IoStatus.Information ||
        Buffer.Acpi.Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER) {
        return false;
    }
    ValueBytes = Buffer.Acpi.Argument[0].DataLength;
    if ((ValueBytes != sizeof(ULONG) && ValueBytes != sizeof(ULONGLONG)) ||
        Buffer.Acpi.Length < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument[0].Data) + ValueBytes) {
        return false;
    }
    *Value = 0;
    RtlCopyMemory(Value, Buffer.Acpi.Argument[0].Data, ValueBytes);
    return true;
}

static bool
AcpiResult(MTK_SD_SLOT *Slot, const char Name[4], bool HasArgument, ULONG Argument, ULONG Expected)
{
    ULONGLONG Value;
    return AcpiInteger(Slot, Name, HasArgument, Argument, &Value) && Value == Expected;
}

static bool
SetSourceClock(void *Context, bool Enabled)
{
    return AcpiResult(Context, "SCOP", true, Enabled ? 2 : 1, 1);
}

static bool
ConfigurePadSampling(void *Context)
{
    return AcpiResult(Context, "SDTC", false, 0, 1);
}

static bool
ResetController(MTK_SD_SLOT *Slot)
{
    ULONG Config;

    Slot->HostReady = FALSE;
    if (!AcpiResult(Slot, "SCOP", true, 0, 1)) {
        return false;
    }
    Slot->HostClockOpen = TRUE;
    Config = ReadRegister(Slot, MSDC_CFG);
    if (Config == MAXULONG || (ReadRegister(Slot, MSDC_DMA_CFG) & 1U) != 0) {
        return false;
    }
    WriteRegister(Slot, MSDC_INTEN, 0);
    WriteRegister(Slot, MSDC_CFG, Config & ~MSDC_CARD_CLOCK);
    Fence(Slot);
    if ((ReadRegister(Slot, MSDC_CFG) & MSDC_CARD_CLOCK) != 0 ||
        !SetSourceClock(Slot, false) || !SetSourceClock(Slot, true) ||
        !T70SdResetHost(&Slot->Host)) {
        return false;
    }
    Slot->Command = (T70_SD_COMMAND){0};
    Slot->CardClockHz = 0;
    Slot->BusWidth = 1;
    Slot->CardRemoved = FALSE;
    Slot->HostReady = TRUE;
    return true;
}

static NTSTATUS
AllocateDma(MTK_SD_SLOT *Slot, PDEVICE_OBJECT Fdo)
{
    DEVICE_DESCRIPTION Description = {0};
    ULONG MapRegisters = 0;
    PDEVICE_OBJECT Pdo = IoGetDeviceAttachmentBaseRef(Fdo);

    if (Pdo == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Description.Version = DEVICE_DESCRIPTION_VERSION;
    Description.Master = TRUE;
    Description.Dma32BitAddresses = TRUE;
    Description.InterfaceType = Internal;
    Description.MaximumLength = SD_COMMON_BYTES;
    Slot->DmaAdapter = IoGetDmaAdapter(Pdo, &Description, &MapRegisters);
    ObDereferenceObject(Pdo);
    if (Slot->DmaAdapter == NULL || MapRegisters < BYTES_TO_PAGES(SD_COMMON_BYTES)) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Slot->CommonBuffer = Slot->DmaAdapter->DmaOperations->AllocateCommonBuffer(
        Slot->DmaAdapter, SD_COMMON_BYTES, &Slot->LogicalAddress, FALSE);
    if (Slot->CommonBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if ((ULONGLONG)Slot->LogicalAddress.QuadPart > MAXULONG - (SD_COMMON_BYTES - 1) ||
        (Slot->LogicalAddress.LowPart & 3U) != 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

static void
ReleaseDma(MTK_SD_SLOT *Slot)
{
    if (Slot->CommonBuffer != NULL) {
        Slot->DmaAdapter->DmaOperations->FreeCommonBuffer(
            Slot->DmaAdapter, SD_COMMON_BYTES, Slot->LogicalAddress, Slot->CommonBuffer, FALSE);
        Slot->CommonBuffer = NULL;
    }
    if (Slot->DmaAdapter != NULL) {
        Slot->DmaAdapter->DmaOperations->PutDmaAdapter(Slot->DmaAdapter);
        Slot->DmaAdapter = NULL;
    }
}

static void
ResetDataState(MTK_SD_SLOT *Slot)
{
    Slot->DataBytes = 0;
    Slot->DataBlockSize = 0;
    Slot->DataBlocksRemaining = 0;
    Slot->DataStatus = STATUS_PENDING;
    Slot->DataDirection = SdTransferDirectionUndefined;
    Slot->WaitDat0 = FALSE;
    Slot->DrainDeadline = 0;
    Slot->AutoStop = FALSE;
    Slot->AutoStopDone = FALSE;
    Slot->DmaDone = FALSE;
}

static T70_SD_DMA_RESULT
ArmData(MTK_SD_SLOT *Slot, ULONG Length, BOOLEAN AutoStop)
{
    const ULONGLONG DataAddress = (ULONGLONG)Slot->LogicalAddress.QuadPart + SD_BUFFER_OFFSET;
    T70_SD_DMA_SEGMENT Segments[2];
    ULONG FirstBytes = min(Length, 0xfffcU);
    ULONG Events;

    Events = ReadRegister(Slot, MSDC_INT);
    if (Events == MAXULONG) {
        return T70SdDmaUnavailable;
    }
    WriteRegister(Slot, MSDC_INT, Events & MSDC_AUTO_STOP_MASK);
    Segments[0] = (T70_SD_DMA_SEGMENT){DataAddress, FirstBytes};
    Segments[1] = (T70_SD_DMA_SEGMENT){DataAddress + FirstBytes, Length - FirstBytes};
    T70_SD_DMA_RESULT Result = T70SdArmDma(
        &Slot->Dma, &Slot->Io, Slot->CommonBuffer, SD_BUFFER_OFFSET,
        (ULONGLONG)Slot->LogicalAddress.QuadPart, Segments, FirstBytes == Length ? 1 : 2, Length);
    if (Result == T70SdDmaReady) {
        ResetDataState(Slot);
        Slot->AutoStop = AutoStop;
        Slot->DataBytes = Length;
    }
    return Result;
}

static NTSTATUS
CommandStatus(T70_SD_COMMAND_RESULT Result)
{
    switch (Result) {
    case T70SdCommandComplete: return STATUS_SUCCESS;
    case T70SdCommandTimeout: return STATUS_IO_TIMEOUT;
    case T70SdCommandCrcError: return STATUS_CRC_ERROR;
    case T70SdCommandCanceled: return STATUS_CANCELLED;
    case T70SdCommandRemoved: return STATUS_NO_MEDIA_IN_DEVICE;
    default: return STATUS_IO_DEVICE_ERROR;
    }
}

static NTSTATUS
DataStatus(T70_SD_DMA_RESULT Result)
{
    switch (Result) {
    case T70SdDmaComplete: return STATUS_SUCCESS;
    case T70SdDmaTimeout: return STATUS_IO_TIMEOUT;
    case T70SdDmaCrcError: return STATUS_CRC_ERROR;
    case T70SdDmaCanceled: return STATUS_CANCELLED;
    case T70SdDmaRemoved: return STATUS_NO_MEDIA_IN_DEVICE;
    default: return STATUS_IO_DEVICE_ERROR;
    }
}

static void
ServiceData(MTK_SD_SLOT *Slot, T70_SD_DMA_STOP Stop)
{
    T70_SD_DMA_RESULT Result;

    if (Slot->DataBytes == 0) {
        return;
    }
    if (Stop == T70SdDmaKeepRunning && Slot->DataStatus == STATUS_PENDING &&
        Slot->AutoStop) {
        ULONG Events = ReadRegister(Slot, MSDC_INT);
        if (Events == MAXULONG) {
            Slot->DataStatus = STATUS_IO_DEVICE_ERROR;
        } else {
            WriteRegister(Slot, MSDC_INT, Events & MSDC_AUTO_STOP_MASK);
            if ((Events & MSDC_AUTO_STOP_TIMEOUT) != 0) {
                Slot->DataStatus = STATUS_IO_TIMEOUT;
            } else if ((Events & MSDC_AUTO_STOP_CRC) != 0) {
                Slot->DataStatus = STATUS_CRC_ERROR;
            } else if ((Events & MSDC_AUTO_STOP_OK) != 0) {
                if ((ReadRegister(Slot, SDC_ACMD_RESP) & SD_R1_ERRORS) != 0) {
                    Slot->DataStatus = STATUS_DEVICE_DATA_ERROR;
                } else {
                    Slot->AutoStopDone = TRUE;
                }
            }
        }
    }
    if (!Slot->DmaDone) {
        Result = T70SdUpdateDma(&Slot->Dma, 0, Slot->DataStatus == STATUS_PENDING ? Stop : T70SdDmaCancel);
        if (Result != T70SdDmaPending && Result != T70SdDmaReady) {
            Slot->DmaDone = TRUE;
            if (Result != T70SdDmaComplete && Slot->DataStatus == STATUS_PENDING) {
                Slot->DataStatus = DataStatus(Result);
            }
        }
    }
    if (Stop != T70SdDmaKeepRunning &&
        (Slot->DataStatus == STATUS_PENDING || Stop == T70SdDmaMediaRemoved)) {
        Slot->DataStatus = Stop == T70SdDmaDeadline ? STATUS_IO_TIMEOUT :
                           Stop == T70SdDmaMediaRemoved ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_CANCELLED;
    }
    if (Slot->DataStatus == STATUS_PENDING && Slot->DmaDone &&
        (!Slot->AutoStop || Slot->AutoStopDone)) {
        if (!Slot->WaitDat0) {
            Slot->DataStatus = STATUS_SUCCESS;
        } else {
            ULONG Pins = ReadRegister(Slot, MSDC_PS);
            if (Pins == MAXULONG) {
                Slot->DataStatus = STATUS_IO_DEVICE_ERROR;
            } else if ((Pins & MSDC_DAT0) != 0) {
                Slot->DataStatus = STATUS_SUCCESS;
            }
        }
    }
    if (Slot->DataStatus != STATUS_PENDING && Slot->DataStatus != STATUS_SUCCESS) {
        Slot->Faulted = TRUE;
    }
}

static void
StartWrite(MTK_SD_SLOT *Slot, const SDPORT_COMMAND *Command)
{
    const T70_SD_DMA_STOP Stop = KeQueryInterruptTime() >= Slot->Deadline ?
                                 T70SdDmaDeadline : T70SdDmaKeepRunning;

    // Retire a timeout or card error before giving the controller the payload.
    ServiceData(Slot, Stop);
    if (Slot->DataStatus != STATUS_PENDING) {
        return;
    }
    RtlCopyMemory(Slot->CommonBuffer + SD_BUFFER_OFFSET, Command->DataBuffer, Slot->DataBytes);
    // The DMA START barrier publishes both the descriptors and the copied data.
    if (T70SdStartDma(&Slot->Dma) != T70SdDmaPending) {
        Slot->DataStatus = STATUS_IO_DEVICE_ERROR;
        ServiceData(Slot, T70SdDmaCancel);
    }
}

static void
Schedule(MTK_SD_SLOT *Slot)
{
    LARGE_INTEGER Due;
    Due.QuadPart = -10000; /* 1 ms polling fallback, bounded by the request deadline. */
    KeSetTimer(&Slot->Timer, Due, &Slot->Dpc);
}

static void
CompleteRequest(PSDPORT_REQUEST Request, NTSTATUS Status)
{
    Request->RequiredEvents = 0;
    Request->Status = Status;
    SdPortCompleteRequest(Request, Status);
}

static NTSTATUS
CopyReadBlock(MTK_SD_SLOT *Slot, SDPORT_COMMAND *Command)
{
    ULONG Offset = Slot->DataBytes - (ULONG)Slot->DataBlocksRemaining * Slot->DataBlockSize;

    Fence(Slot);
    RtlCopyMemory(Command->DataBuffer, Slot->CommonBuffer + SD_BUFFER_OFFSET + Offset,
                  Slot->DataBlockSize);
    Slot->DataBlocksRemaining--;
    Command->BlockCount--;
    if (Slot->DataBlocksRemaining != 0) {
        Command->DataBuffer += Slot->DataBlockSize;
        return STATUS_MORE_PROCESSING_REQUIRED;
    }
    return STATUS_SUCCESS;
}

static void
ServiceRequest(KDPC *Dpc, void *Context, void *Argument1, void *Argument2)
{
    MTK_SD_SLOT *Slot = Context;
    PSDPORT_REQUEST Completion = NULL;
    NTSTATUS Status = STATUS_PENDING;
    T70_SD_DMA_STOP Stop;
    ULONGLONG Now;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);
    KeAcquireSpinLockAtDpcLevel(&Slot->Lock);
    if (Slot->Busy || Slot->Removing) {
        KeReleaseSpinLockFromDpcLevel(&Slot->Lock);
        return;
    }
    if (Slot->Suspended || Slot->ResumeFailed) {
        Completion = Slot->ActiveRequest;
        Slot->ActiveRequest = NULL;
        Status = Slot->CardRemoved ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_DEVICE_POWER_FAILURE;
        T70SdUpdateCommand(&Slot->Command, 0, T70SdDmaCancel);
        ResetDataState(Slot);
        goto Exit;
    }
    Now = KeQueryInterruptTime();
    Stop = Slot->CardRemoved || !Slot->CardPresent ? T70SdDmaMediaRemoved :
           Now >= Slot->Deadline &&
           (Slot->Command.Phase != T70SdCommandIdle || Slot->DataStatus == STATUS_PENDING) ?
           T70SdDmaDeadline : T70SdDmaKeepRunning;

    if (Slot->Command.Phase != T70SdCommandIdle) {
        T70_SD_COMMAND_RESULT Result = T70SdUpdateCommand(&Slot->Command, 0, Stop);
        if (Result != T70SdCommandPending) {
            Status = CommandStatus(Result);
            if (Status == STATUS_SUCCESS && Slot->DataBytes != 0) {
                ULONG Response = Slot->Command.ResponseWords[0];
                if ((Response & SD_R1_ERRORS) != 0) {
                    Status = Slot->DataDirection == SdTransferDirectionWrite &&
                             (Response & SD_R1_WP_VIOLATION) != 0 ?
                             STATUS_MEDIA_WRITE_PROTECTED : STATUS_DEVICE_DATA_ERROR;
                } else if (Slot->DataDirection == SdTransferDirectionRead &&
                           T70SdStartDma(&Slot->Dma) != T70SdDmaPending) {
                    Status = STATUS_IO_DEVICE_ERROR;
                } else {
                    Slot->Deadline = Now + SD_DATA_TIMEOUT_100NS;
                    Stop = T70SdDmaKeepRunning;
                }
            }
            if (Status != STATUS_SUCCESS && Slot->DataBytes != 0) {
                T70SdUpdateDma(&Slot->Dma, 0, T70SdDmaCancel);
                ResetDataState(Slot);
                Slot->Faulted = TRUE;
            }
            Slot->Faulted |= Slot->Command.NeedsReset;
            Completion = Slot->ActiveRequest;
            Slot->ActiveRequest = NULL;
        }
    }

    if (Slot->Command.Phase == T70SdCommandIdle && Slot->DataBytes != 0) {
        ServiceData(Slot, Stop);
        if (Slot->Dma.Phase == T70SdDmaDraining) {
            if (Slot->DrainDeadline == 0) {
                Slot->DrainDeadline = Now + SD_DMA_STOP_TIMEOUT_100NS;
            } else if (Now >= Slot->DrainDeadline && Slot->DataStatus == STATUS_PENDING) {
                Slot->DataStatus = STATUS_IO_TIMEOUT;
                Slot->Faulted = TRUE;
            }
        }
        if (Slot->DataStatus != STATUS_PENDING && Slot->ActiveRequest != NULL &&
            (Slot->Dma.Phase == T70SdDmaIdle || Now >= Slot->DrainDeadline)) {
            Completion = Slot->ActiveRequest;
            Slot->ActiveRequest = NULL;
            Status = Slot->DataStatus;
            if (Status == STATUS_SUCCESS) {
                if (Slot->DataDirection == SdTransferDirectionWrite) {
                    Completion->Command.DataBuffer += Slot->DataBytes - Slot->DataBlockSize;
                    Completion->Command.BlockCount = 0;
                } else {
                    Status = CopyReadBlock(Slot, &Completion->Command);
                }
            }
            if (Status != STATUS_MORE_PROCESSING_REQUIRED) {
                ResetDataState(Slot);
            }
        }
    }
    if (Slot->Command.Phase != T70SdCommandIdle ||
        (Slot->DataBytes != 0 && (Slot->DataStatus == STATUS_PENDING ||
          (Slot->Dma.Phase == T70SdDmaDraining && Now < Slot->DrainDeadline)))) {
        Schedule(Slot);
    }
Exit:
    KeReleaseSpinLockFromDpcLevel(&Slot->Lock);
    if (Completion != NULL) {
        CompleteRequest(Completion, Status);
    }
}

static bool
IsRestrictedCommand(UCHAR Index)
{
    switch (Index) {
    case 26: /* PROGRAM_CID */
    case 27: /* PROGRAM_CSD */
    case 28: /* SET_WRITE_PROT */
    case 29: /* CLR_WRITE_PROT */
    case 32: /* ERASE_WR_BLK_START */
    case 33: /* ERASE_WR_BLK_END */
    case 38: /* ERASE */
        return true;
    default:
        return false;
    }
}

static NTSTATUS
IssueCommand(MTK_SD_SLOT *Slot, PSDPORT_REQUEST Request)
{
    const SDPORT_COMMAND *Input = &Request->Command;
    const bool BlockWrite = Input->Index == 24 || Input->Index == 25;
    T70_SD_COMMAND_REQUEST Command = {0};
    ULONG Length = 0;
    T70_SD_COMMAND_RESULT Result;

    if (Slot->Faulted || !Slot->HostReady || !Slot->Power.Settled || Slot->CardClockHz == 0 || Slot->DataBytes != 0 ||
        Slot->Dma.Phase != T70SdDmaIdle) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (IsRestrictedCommand(Input->Index) ||
        (Input->Index == 52 && (Input->Argument & (1U << 31)) != 0)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    switch (Input->ResponseType) {
    case SdResponseTypeNone: Command.Response = T70SdResponseNone; break;
    case SdResponseTypeR1:
    case SdResponseTypeR5:
    case SdResponseTypeR6: Command.Response = T70SdResponseShort; break;
    case SdResponseTypeR1B:
    case SdResponseTypeR5B: Command.Response = T70SdResponseBusy; break;
    case SdResponseTypeR2: Command.Response = T70SdResponseLong; break;
    case SdResponseTypeR3: Command.Response = T70SdResponseR3; break;
    case SdResponseTypeR4: Command.Response = T70SdResponseR4; break;
    default: return STATUS_INVALID_PARAMETER;
    }
    Command.Index = Input->Index;
    Command.Argument = Input->Argument;
    if (Request->Type == SdRequestTypeCommandWithTransfer) {
        if (Input->TransferDirection != (BlockWrite ? SdTransferDirectionWrite : SdTransferDirectionRead) ||
            Input->TransferMethod != SdTransferMethodPio || Input->UseAutoCmd23 ||
            (Input->UseAutoCmd12 && (Input->BlockCount < 2 || Input->TransferType == SdTransferTypeMultiBlockNoStop)) ||
            Input->BlockSize == 0 || Input->BlockSize > 512 || Input->BlockCount == 0 ||
            (Input->TransferType == SdTransferTypeSingleBlock && Input->BlockCount != 1) ||
            (Input->TransferType != SdTransferTypeSingleBlock && Input->TransferType != SdTransferTypeMultiBlock &&
             Input->TransferType != SdTransferTypeMultiBlockNoStop)) {
            return STATUS_INVALID_PARAMETER;
        }
        if (BlockWrite && (Input->Class == SdCommandClassApp || Input->ResponseType != SdResponseTypeR1 ||
            (Input->Index == 24 && Input->TransferType != SdTransferTypeSingleBlock) ||
            (Input->Index == 25 && (Input->TransferType == SdTransferTypeSingleBlock || Input->BlockCount < 2)))) {
            return STATUS_INVALID_PARAMETER;
        }
        Length = (ULONG)Input->BlockSize * Input->BlockCount;
        if (Length > SD_MAX_DATA_BYTES || (Input->Length != 0 && Length != Input->Length) || (Length & 3U) != 0) {
            return STATUS_INVALID_BUFFER_SIZE;
        }
        Command.BlockBytes = Input->BlockSize;
        Command.Blocks = Input->BlockCount;
        Command.AutoStop = Input->UseAutoCmd12 != FALSE;
        Command.Write = BlockWrite;
        if (ArmData(Slot, Length, Input->UseAutoCmd12) != T70SdDmaReady) {
            return STATUS_IO_DEVICE_ERROR;
        }
        Slot->DataBlockSize = Input->BlockSize;
        Slot->DataBlocksRemaining = Input->BlockCount;
        Slot->DataDirection = Input->TransferDirection;
        // Without automatic or predefined stop, SDPORT sends CMD12 before the
        // card's final busy wait. Single-block writes must finish programming.
        Slot->WaitDat0 = Command.AutoStop || (BlockWrite && Input->TransferType != SdTransferTypeMultiBlock);
    } else if (Request->Type != SdRequestTypeCommandNoTransfer || Input->TransferType != SdTransferTypeNone ||
               BlockWrite || Input->TransferDirection == SdTransferDirectionWrite) {
        return STATUS_INVALID_PARAMETER;
    }

    Result = T70SdIssueCommand(&Slot->Command, &Slot->Io, &Command);
    if (Result != T70SdCommandPending) {
        if (Length != 0) {
            T70SdUpdateDma(&Slot->Dma, 0, T70SdDmaCancel);
            ResetDataState(Slot);
        }
        Slot->Faulted |= Slot->Command.NeedsReset;
        return Result == T70SdCommandInvalid ? STATUS_INVALID_PARAMETER : STATUS_DEVICE_BUSY;
    }
    Slot->Deadline = KeQueryInterruptTime() + SD_COMMAND_TIMEOUT_100NS;
    return STATUS_PENDING;
}

static NTSTATUS
SlotIssueRequest(PVOID PrivateExtension, PSDPORT_REQUEST Request)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    NTSTATUS Status;

    if (Request == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLockAtDpcLevel(&Slot->Lock);
    if (Slot->Removing || !Slot->CardPresent) {
        Status = STATUS_NO_MEDIA_IN_DEVICE;
    } else if (Slot->Suspended || Slot->ResumeFailed || Slot->CardRemoved) {
        Status = STATUS_DEVICE_NOT_READY;
    } else if (Slot->Busy || Slot->ActiveRequest != NULL) {
        Status = STATUS_DEVICE_BUSY;
    } else if (Request->Type == SdRequestTypeStartTransfer) {
        const SDPORT_COMMAND *Input = &Request->Command;
        if (Slot->DataBytes == 0 || Input->DataBuffer == NULL ||
            Input->BlockSize != Slot->DataBlockSize || Input->BlockCount != Slot->DataBlocksRemaining ||
            Input->TransferDirection != Slot->DataDirection || Input->TransferMethod != SdTransferMethodPio ||
            (Slot->DataDirection == SdTransferDirectionWrite && Input->Length != 0 && Input->Length != Slot->DataBytes)) {
            Status = STATUS_INVALID_PARAMETER;
        } else {
            if (Slot->DataDirection == SdTransferDirectionWrite && Slot->Dma.Phase == T70SdDmaArmed) {
                StartWrite(Slot, Input);
            }
            Status = STATUS_PENDING;
        }
    } else {
        Status = IssueCommand(Slot, Request);
    }
    if (Status == STATUS_PENDING) {
        Request->Status = STATUS_PENDING;
        Request->RequiredEvents = SDPORT_EVENT_CARD_RESPONSE;
        if (Request->Type == SdRequestTypeStartTransfer) {
            Request->RequiredEvents = SDPORT_EVENT_CARD_RW_END;
        }
        Slot->ActiveRequest = Request;
        if (Request->Type == SdRequestTypeStartTransfer && Slot->DataStatus != STATUS_PENDING) {
            KeInsertQueueDpc(&Slot->Dpc, NULL, NULL);
        } else {
            Schedule(Slot);
        }
    }
    KeReleaseSpinLockFromDpcLevel(&Slot->Lock);
    return Status;
}

/* The port serializes bus operations, but the timer DPC can still be active. */
static bool
Quiesce(MTK_SD_SLOT *Slot, PSDPORT_REQUEST *Aborted)
{
    KIRQL Irql;
    ULONG ElapsedUs;

    KeAcquireSpinLock(&Slot->Lock, &Irql);
    *Aborted = Slot->ActiveRequest;
    Slot->ActiveRequest = NULL;
    KeCancelTimer(&Slot->Timer);
    T70SdUpdateCommand(&Slot->Command, 0, T70SdDmaCancel);
    KeReleaseSpinLock(&Slot->Lock, Irql);
    for (ElapsedUs = 0; ElapsedUs < SD_DMA_QUIESCE_TIMEOUT_US;
         ElapsedUs += SD_DMA_QUIESCE_POLL_US) {
        KeAcquireSpinLock(&Slot->Lock, &Irql);
        T70SdUpdateDma(&Slot->Dma, 0, T70SdDmaCancel);
        if (Slot->Dma.Phase == T70SdDmaIdle) {
            ResetDataState(Slot);
            KeReleaseSpinLock(&Slot->Lock, Irql);
            return true;
        }
        KeReleaseSpinLock(&Slot->Lock, Irql);
        SdPortWait(SD_DMA_QUIESCE_POLL_US);
    }
    return false;
}

static NTSTATUS
SetCardPower(MTK_SD_SLOT *Slot, BOOLEAN On)
{
    NTSTATUS Status;

    if (On) {
        if ((!Slot->HostReady || Slot->Faulted || Slot->CardRemoved) && !ResetController(Slot)) {
            return STATUS_DEVICE_NOT_READY;
        }
        Status = MtkSdPowerSet(&Slot->Power, TRUE);
        if (Status != STATUS_SUCCESS) {
            return Status;
        }
        if (Slot->PendingClockHz != 0 &&
            !T70SdSetCardClock(&Slot->Host, Slot->PendingClockHz, &Slot->CardClockHz)) {
            return STATUS_IO_DEVICE_ERROR;
        }
        Slot->Faulted = FALSE;
        return STATUS_SUCCESS;
    }
    if (Slot->HostClockOpen && !T70SdSetCardClock(&Slot->Host, 0, &Slot->CardClockHz)) {
        return STATUS_IO_DEVICE_ERROR;
    }
    Status = MtkSdPowerSet(&Slot->Power, FALSE);
    if (Status == STATUS_SUCCESS) {
        Slot->HostReady = FALSE;
    }
    return Status;
}

static NTSTATUS
SlotBusOperation(PVOID PrivateExtension, PSDPORT_BUS_OPERATION Operation)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    PSDPORT_REQUEST Aborted = NULL;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    KIRQL Irql;
    BOOLEAN Reset;
    BOOLEAN PoweredOff;
    BOOLEAN ClockOff;
    BOOLEAN RecoverContext;

    if (Operation == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    Reset = Operation->Type == SdResetHost || Operation->Type == SdResetHw;
    PoweredOff = (Operation->Type == SdSetVoltage && Operation->Parameters.Voltage == SdBusVoltageOff) ||
                 (Operation->Type == SdSetPower && !Operation->Parameters.PowerEnabled);
    ClockOff = Operation->Type == SdSetClock && Operation->Parameters.FrequencyKhz == 0;
    KeWaitForSingleObject(&Slot->Control, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&Slot->Lock, &Irql);
    if ((Slot->Suspended && !Reset && !PoweredOff && !ClockOff) ||
        (Slot->ResumeFailed && !Reset && !PoweredOff)) {
        KeReleaseSpinLock(&Slot->Lock, Irql);
        KeReleaseMutex(&Slot->Control, FALSE);
        return STATUS_DEVICE_NOT_READY;
    }
    if (Slot->Removing || (!Reset && !PoweredOff &&
        (Slot->ActiveRequest != NULL || Slot->DataBytes != 0))) {
        KeReleaseSpinLock(&Slot->Lock, Irql);
        KeReleaseMutex(&Slot->Control, FALSE);
        return STATUS_DEVICE_BUSY;
    }
    Slot->Busy = TRUE;
    RecoverContext = Slot->ResumeFailed;
    if (Reset || PoweredOff || ClockOff) {
        Slot->ContextValid = FALSE;
        Slot->ResumeFailed = FALSE;
    }
    KeReleaseSpinLock(&Slot->Lock, Irql);
    if ((Reset || PoweredOff) && !Quiesce(Slot, &Aborted)) {
        Status = STATUS_IO_TIMEOUT;
        goto Exit;
    }

    switch (Operation->Type) {
    case SdResetHost:
    case SdResetHw: {
        ULONG Clock = Slot->PendingClockHz;
        ULONG Width = Slot->BusWidth;
        const BOOLEAN Full = Operation->Type == SdResetHw || Operation->Parameters.ResetType == SdResetTypeAll;
        if (!ResetController(Slot)) {
            Status = STATUS_IO_DEVICE_ERROR;
            break;
        }
        if (Full) {
            Slot->PendingClockHz = 0;
        } else if (!T70SdSetBusWidth(&Slot->Host, Width) ||
                   (Slot->Power.Settled && !T70SdSetCardClock(&Slot->Host, Clock, &Slot->CardClockHz))) {
            Status = STATUS_IO_DEVICE_ERROR;
            break;
        } else {
            Slot->BusWidth = Width;
        }
        Slot->Faulted = FALSE;
        Status = STATUS_SUCCESS;
        break;
    }
    case SdSetClock:
        if (Operation->Parameters.FrequencyKhz > T70_SD_MAX_HZ / 1000 ||
            (Operation->Parameters.FrequencyKhz != 0 && Operation->Parameters.FrequencyKhz < 2)) {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
        Slot->PendingClockHz = Operation->Parameters.FrequencyKhz * 1000;
        Status = STATUS_SUCCESS;
        if (Slot->Power.Settled &&
            !T70SdSetCardClock(&Slot->Host, Slot->PendingClockHz, &Slot->CardClockHz)) {
            Status = STATUS_IO_DEVICE_ERROR;
        }
        break;
    case SdSetVoltage:
        if (Operation->Parameters.Voltage == SdBusVoltage33 || PoweredOff) {
            Status = SetCardPower(Slot, !PoweredOff);
        }
        break;
    case SdSetPower:
        Status = SetCardPower(Slot, !PoweredOff);
        break;
    case SdSetBusWidth:
        if (Slot->HostClockOpen && T70SdSetBusWidth(&Slot->Host, Operation->Parameters.BusWidth)) {
            Slot->BusWidth = Operation->Parameters.BusWidth;
            Status = STATUS_SUCCESS;
        }
        break;
    case SdSetBusSpeed:
        if (Operation->Parameters.BusSpeed == SdBusSpeedNormal) {
            Status = STATUS_SUCCESS;
        }
        break;
    case SdSetSignalingVoltage:
        if (Operation->Parameters.SignalingVoltage == SdSignalingVoltage33) {
            Status = STATUS_SUCCESS;
        }
        break;
    case SdSetBlockGapInterrupt:
        if (!Operation->Parameters.BlockGapIntEnabled) {
            Status = STATUS_SUCCESS;
        }
        break;
    case SdSetPresetValue:
        if (!Operation->Parameters.PresetValueEnabled) {
            Status = STATUS_SUCCESS;
        }
        break;
    default:
        break;
    }

Exit:
    KeAcquireSpinLock(&Slot->Lock, &Irql);
    Slot->Faulted |= Status == STATUS_IO_DEVICE_ERROR || Status == STATUS_IO_TIMEOUT;
    Slot->ResumeFailed |= (RecoverContext || (Slot->Suspended && ClockOff)) && Status != STATUS_SUCCESS;
    Slot->Busy = FALSE;
    KeReleaseSpinLock(&Slot->Lock, Irql);
    KeReleaseMutex(&Slot->Control, FALSE);
    if (Aborted != NULL) {
        CompleteRequest(Aborted, STATUS_CANCELLED);
    }
    return Status;
}

/* SDPORT permits these callbacks at DISPATCH_LEVEL. The normal bus
 * operations own the ACPI clocks, pad setup and PMIC power lease. */
static VOID
SlotSaveContext(PVOID PrivateExtension)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    KIRQL Irql;

    KeAcquireSpinLock(&Slot->Lock, &Irql);
    if (!Slot->Removing && !Slot->Suspended) {
        Slot->Suspended = TRUE;
        if (Slot->Busy) {
            Slot->ResumeFailed = TRUE;
        } else if (Slot->ActiveRequest != NULL || Slot->DataBytes != 0 || Slot->Dma.Phase != T70SdDmaIdle) {
            Slot->ResumeFailed = TRUE;
            KeCancelTimer(&Slot->Timer);
            /* A draining DMA keeps its mapping until a bus reset or cleanup
               can observe STOP complete. The DPC only retires the request. */
            if (Slot->HostClockOpen && ReadRegister(Slot, MSDC_CFG) != MAXULONG) {
                T70SdUpdateDma(&Slot->Dma, 0, T70SdDmaCancel);
            }
            KeInsertQueueDpc(&Slot->Dpc, NULL, NULL);
        } else if (Slot->HostReady && Slot->Power.Settled && !Slot->ResumeFailed) {
            Slot->ContextValid = !Slot->Faulted &&
                T70SdSaveHostState(&Slot->Host, Slot->CardClockHz != 0, &Slot->SavedHost);
            Slot->ResumeFailed = !Slot->ContextValid;
        }
    }
    KeReleaseSpinLock(&Slot->Lock, Irql);
}

static VOID
SlotRestoreContext(PVOID PrivateExtension)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    KIRQL Irql;

    KeAcquireSpinLock(&Slot->Lock, &Irql);
    if (!Slot->Removing && Slot->Suspended) {
        if (Slot->ContextValid) {
            Slot->ResumeFailed = !T70SdRestoreHostState(&Slot->Host, &Slot->SavedHost);
        }
        Slot->ContextValid = FALSE;
        Slot->Suspended = FALSE;
    }
    KeReleaseSpinLock(&Slot->Lock, Irql);
}

static BOOLEAN
SlotCardPresent(PVOID PrivateExtension)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    ULONGLONG State = 0;
    BOOLEAN Present, Known;
    KIRQL Irql;

    KeWaitForSingleObject(&Slot->Control, Executive, KernelMode, FALSE, NULL);
    Known = Slot->Removing || (AcpiInteger(Slot, "SDCD", false, 0, &State) && State <= 1);
    KeAcquireSpinLock(&Slot->Lock, &Irql);
    // SDPORT takes a Boolean. Keep the last observation until SDCD can prove a change.
    Present = Known ? State == 1 : Slot->CardPresent;
    if (!Present && Slot->CardPresent) {
        Slot->CardRemoved = TRUE;
        if (Slot->ContextValid) {
            Slot->ContextValid = FALSE;
            Slot->ResumeFailed = TRUE;
        }
        if (Slot->ActiveRequest != NULL || Slot->DataBytes != 0) {
            KeInsertQueueDpc(&Slot->Dpc, NULL, NULL);
        }
    }
    Slot->CardPresent = Present;
    KeReleaseSpinLock(&Slot->Lock, Irql);
    KeReleaseMutex(&Slot->Control, FALSE);
    return Present;
}

static BOOLEAN
SlotWriteProtected(PVOID PrivateExtension)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    BOOLEAN Protected;
    KIRQL Irql;

    if (Slot->AcpiDevice == NULL) {
        return TRUE;
    }
    // The microSD slot has card-detect GPIO7 and no write-protect switch.
    KeAcquireSpinLock(&Slot->Lock, &Irql);
    Protected = Slot->Removing || Slot->CardRemoved || !Slot->CardPresent;
    KeReleaseSpinLock(&Slot->Lock, Irql);
    return Protected;
}

static VOID
SlotGetResponse(PVOID PrivateExtension, PSDPORT_COMMAND Command, PVOID Buffer)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    KIRQL Irql;
    if (Command == NULL || Buffer == NULL || Command->ResponseType == SdResponseTypeNone) {
        return;
    }
    KeAcquireSpinLock(&Slot->Lock, &Irql);
    RtlCopyMemory(Buffer, Slot->Command.ResponseWords,
                  Command->ResponseType == SdResponseTypeR2 ? sizeof(Slot->Command.ResponseWords) : sizeof(ULONG));
    KeReleaseSpinLock(&Slot->Lock, Irql);
}

static NTSTATUS
GetSlotCount(PSD_MINIPORT Miniport, PUCHAR Count)
{
    if (Miniport->ConfigurationInfo.BusType != SdBusTypeAcpi) {
        return STATUS_NOT_SUPPORTED;
    }
    *Count = 1;
    return STATUS_SUCCESS;
}

static VOID
SlotCapabilities(PVOID PrivateExtension, PSDPORT_CAPABILITIES Capabilities)
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->SpecVersion = 2;
    Capabilities->MaximumOutstandingRequests = 1;
    Capabilities->MaximumBlockSize = 512;
    Capabilities->MaximumBlockCount = SD_MAX_DATA_BYTES / 512;
    Capabilities->BaseClockFrequencyKhz = T70_SD_SOURCE_HZ / 1000;
    Capabilities->PioTransferMaxThreshold = SD_MAX_DATA_BYTES;
    Capabilities->Flags.UsePioForRead = TRUE;
    Capabilities->Flags.UsePioForWrite = TRUE;
    Capabilities->Supported.Voltage33V = 1;
    Capabilities->Supported.AutoCmd12 = 1;
    Capabilities->Supported.SaveContext = 1;
}

static NTSTATUS
SlotInitialize(PVOID PrivateExtension, PHYSICAL_ADDRESS PhysicalBase, PVOID VirtualBase,
               ULONG Length, BOOLEAN CrashdumpMode)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    PSDPORT_SLOT_EXTENSION Extension = CONTAINING_RECORD(PrivateExtension, SDPORT_SLOT_EXTENSION, PrivateExtension);
    PDEVICE_OBJECT Fdo = Extension->Miniport->ConfigurationInfo.DeviceObject;
    NTSTATUS Status;

    if (CrashdumpMode || PhysicalBase.QuadPart != MSDC1_BASE || VirtualBase == NULL ||
        Length != MSDC1_REGISTER_BYTES || Fdo == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    RtlZeroMemory(Slot, sizeof(*Slot));
    Slot->Registers = VirtualBase;
    Slot->Io = (T70_SD_DMA_IO){Slot, ReadRegister, WriteRegister, Fence};
    Slot->Host = (T70_SD_HOST_IO){&Slot->Io, Slot, SetSourceClock, ConfigurePadSampling, Delay};
    Slot->AcpiDevice = IoGetLowerDeviceObject(Fdo);
    if (Slot->AcpiDevice == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    KeInitializeSpinLock(&Slot->Lock);
    KeInitializeMutex(&Slot->Control, 0);
    KeInitializeTimer(&Slot->Timer);
    KeInitializeDpc(&Slot->Dpc, ServiceRequest, Slot);
    Slot->BusWidth = 1;
    ResetDataState(Slot);
    Status = MtkSdPowerInitialize(&Slot->Power);
    if (Status == STATUS_SUCCESS) {
        Status = AllocateDma(Slot, Fdo);
    }
    if (Status != STATUS_SUCCESS) {
        ReleaseDma(Slot);
        ObDereferenceObject(Slot->AcpiDevice);
        Slot->AcpiDevice = NULL;
        return Status;
    }
    Slot->CardPresent = AcpiResult(Slot, "SDCD", false, 0, 1);
    return STATUS_SUCCESS;
}

static VOID
Cleanup(PSD_MINIPORT Miniport)
{
    PSDPORT_SLOT_EXTENSION Extension = Miniport->SlotExtensionList[0];
    MTK_SD_SLOT *Slot;
    KIRQL Irql;
    PSDPORT_REQUEST Aborted;
    BOOLEAN Idle;

    if (Extension == NULL) {
        return;
    }
    Slot = (MTK_SD_SLOT *)Extension->PrivateExtension;
    if (Slot->AcpiDevice == NULL) {
        return;
    }
    KeWaitForSingleObject(&Slot->Control, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&Slot->Lock, &Irql);
    Slot->Busy = TRUE;
    Slot->Removing = TRUE;
    KeReleaseSpinLock(&Slot->Lock, Irql);
    Idle = Quiesce(Slot, &Aborted);
    if (Idle && SetCardPower(Slot, FALSE) == STATUS_SUCCESS && Slot->HostClockOpen) {
        AcpiResult(Slot, "SCOP", true, 3, 1);
    }
    KeReleaseMutex(&Slot->Control, FALSE);
    KeRemoveQueueDpc(&Slot->Dpc);
    KeFlushQueuedDpcs();
    if (Aborted != NULL) {
        CompleteRequest(Aborted, STATUS_DEVICE_REMOVED);
    }
    /* A stuck bus master must retain its DMA mapping through surprise removal. */
    if (Idle) {
        ReleaseDma(Slot);
    }
    ObDereferenceObject(Slot->AcpiDevice);
    Slot->AcpiDevice = NULL;
}

static BOOLEAN
SlotInterrupt(PVOID PrivateExtension, PULONG Events, PULONG Errors,
              PBOOLEAN CardChange, PBOOLEAN SdioInterrupt, PBOOLEAN Tuning)
{
    MTK_SD_SLOT *Slot = PrivateExtension;
    *Events = *Errors = 0;
    *CardChange = *SdioInterrupt = *Tuning = FALSE;

    /*
     * The controller interrupt source mask remains zero. The only armed
     * interrupt is GPIO7's ACPI both-edge card-detect line. SDPORT calls this
     * callback for that GPIO event and queries SDCD later at PASSIVE_LEVEL.
     * Do not touch the clock-gated MSDC registers or call ACPI/GPIO services
     * from this high-IRQL callback.
     */
    if (!Slot->Removing) {
        *CardChange = TRUE;
        return TRUE;
    }
    return FALSE;
}

static VOID
SlotToggleEvents(PVOID PrivateExtension, ULONG EventMask, BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(EventMask);
    UNREFERENCED_PARAMETER(Enable);
}

static VOID
SlotClearEvents(PVOID PrivateExtension, ULONG EventMask)
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(EventMask);
}

static VOID
SlotRequestDpc(PVOID PrivateExtension, PSDPORT_REQUEST Request, ULONG Events, ULONG Errors)
{
    UNREFERENCED_PARAMETER(PrivateExtension);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Events);
    UNREFERENCED_PARAMETER(Errors);
}

NTSTATUS
DriverEntry(PVOID DriverObject, PVOID RegistryPath)
{
    SDPORT_INITIALIZATION_DATA Initialization = {0};

    Initialization.StructureSize = sizeof(Initialization);
    Initialization.GetSlotCount = GetSlotCount;
    Initialization.GetSlotCapabilities = SlotCapabilities;
    Initialization.Interrupt = SlotInterrupt;
    Initialization.IssueRequest = SlotIssueRequest;
    Initialization.GetResponse = SlotGetResponse;
    Initialization.RequestDpc = SlotRequestDpc;
    Initialization.ToggleEvents = SlotToggleEvents;
    Initialization.ClearEvents = SlotClearEvents;
    Initialization.SaveContext = SlotSaveContext;
    Initialization.RestoreContext = SlotRestoreContext;
    Initialization.Initialize = SlotInitialize;
    Initialization.IssueBusOperation = SlotBusOperation;
    Initialization.GetCardDetectState = SlotCardPresent;
    Initialization.GetWriteProtectState = SlotWriteProtected;
    Initialization.Cleanup = Cleanup;
    Initialization.PrivateExtensionSize = sizeof(MTK_SD_SLOT);
    return SdPortInitialize(DriverObject, RegistryPath, &Initialization);
}
