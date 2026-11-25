// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntifs.h>
#include <acpiioct.h>
#include <sdport.h>
#include "SdPower.h"

static void LockAtDpc(PKSPIN_LOCK);
static void UnlockAtDpc(PKSPIN_LOCK);
static KIRQL Lock(PKSPIN_LOCK);
static void UnlockSlot(PKSPIN_LOCK, KIRQL);
static LONG ReleaseControl(PKMUTEX, BOOLEAN);
static void InitializeLock(PKSPIN_LOCK);
static void InitializeControl(PKMUTEX, ULONG);
static void InitializeTimer(PKTIMER);
static void InitializeDpc(PRKDPC, PKDEFERRED_ROUTINE, PVOID);
static void InitializeEvent(PRKEVENT, EVENT_TYPE, BOOLEAN);
static BOOLEAN SetTimer(PKTIMER, LARGE_INTEGER, PKDPC);
static BOOLEAN CancelTimer(PKTIMER);
static BOOLEAN QueueDpc(PRKDPC, PVOID, PVOID);
static BOOLEAN DequeueDpc(PRKDPC);
static void FlushDpcs(void);
static ULONGLONG QueryTime(void);
static PDEVICE_OBJECT GetPdo(PDEVICE_OBJECT);
static PDMA_ADAPTER GetAdapter(PDEVICE_OBJECT, PDEVICE_DESCRIPTION, PULONG);
static void Dereference(PVOID);
static PIRP AllocateIrp(CCHAR, BOOLEAN);
static void FreeIrp(PIRP);
static LONG SignalIo(PKEVENT, KPRIORITY, BOOLEAN);
static NTSTATUS CallLower(PDEVICE_OBJECT, PIRP);
static NTSTATUS WaitForIo(PVOID, KWAIT_REASON, KPROCESSOR_MODE, BOOLEAN, PLARGE_INTEGER);

#undef KeAcquireSpinLock
#undef KeQueryInterruptTime
#undef IoCallDriver
#undef ObDereferenceObject
#define KeAcquireSpinLock(LockAddress, OldIrql) (*(OldIrql) = Lock(LockAddress))
#define KeAcquireSpinLockAtDpcLevel LockAtDpc
#define KeReleaseSpinLockFromDpcLevel UnlockAtDpc
#define KeReleaseSpinLock UnlockSlot
#define KeReleaseMutex ReleaseControl
#define KeInitializeMutex InitializeControl
#define KeSetEvent SignalIo
#define KeInitializeSpinLock InitializeLock
#define KeInitializeTimer InitializeTimer
#define KeInitializeDpc InitializeDpc
#define KeInitializeEvent InitializeEvent
#define KeSetTimer SetTimer
#define KeCancelTimer CancelTimer
#define KeInsertQueueDpc QueueDpc
#define KeRemoveQueueDpc DequeueDpc
#define KeFlushQueuedDpcs FlushDpcs
#define KeQueryInterruptTime QueryTime
#define IoGetLowerDeviceObject GetPdo
#define IoGetDeviceAttachmentBaseRef GetPdo
#define IoGetDmaAdapter GetAdapter
#define ObDereferenceObject Dereference
#define IoAllocateIrp AllocateIrp
#define IoFreeIrp FreeIrp
#define IoCallDriver CallLower
#define KeWaitForSingleObject WaitForIo
#include "Driver.c"

#define TEST_COMMAND 0x034U
#define TEST_RESPONSE 0x040U
#define TEST_DMA_ADDRESS 0x090U
#define TEST_DMA_CONTROL 0x098U
#define TEST_CLK_STABLE (1U << 7)
#define TEST_SDC_CFG 0x030U
#define TEST_PATCH0 0x0b0U

static SDPORT_INITIALIZATION_DATA Api;
static SD_MINIPORT Miniport;
static DECLSPEC_ALIGN(MEMORY_ALLOCATION_ALIGNMENT)
    UCHAR SlotStorage[FIELD_OFFSET(SDPORT_SLOT_EXTENSION, PrivateExtension) + sizeof(MTK_SD_SLOT)];
static MTK_SD_SLOT *Slot;
static DEVICE_OBJECT Fdo, Pdo;
static DMA_ADAPTER Adapter;
static DMA_OPERATIONS DmaOperations;
static IRP IoctlIrp;
static IO_STACK_LOCATION IoctlStack;
static BOOLEAN IoctlOwned, IoctlDone;
static ULONG IoctlAllocations, IoctlFrees;
static ULONG MmioReads;
static PKDEFERRED_ROUTINE TimerDpc;
static PVOID TimerContext;
static ULONGLONG Now;
static KIRQL Irql;
static BOOLEAN Card, UnknownCardState, PendingIo, BadOutput, StopStuck, BadAddress, ShortMap;
static BOOLEAN Hclk, Source, Timer, Queued, Locked, ControlHeld;
static BOOLEAN MmioUnavailable, DropPatchWrite, SaveDuringReset;
static ULONG MethodFault, RailCalls, References, Allocations, Frees, AdapterPuts, Commands, Starts, Stops, Completions;
static ULONG Writes[4], PadWrites, Waits, MmioWrites, Failure;
static ULONG Registers[MSDC1_REGISTER_BYTES / sizeof(ULONG)];
static DECLSPEC_ALIGN(16) UCHAR Common[SD_COMMON_BYTES];
static UCHAR ReadBuffer[1 + SD_MAX_DATA_BYTES + 1];
static UCHAR WriteBuffer[1 + SD_MAX_DATA_BYTES + 1];
static ULONG PendingWriteBytes;
static SDPORT_REQUEST Request, Other;
static PSDPORT_REQUEST Completed;
static NTSTATUS CompletionStatus;

__declspec(dllexport) volatile ULONG MtkSdAssertionCount;
#define CHECK(Condition) do { \
    ++MtkSdAssertionCount; \
    if (!(Condition) && Failure == 0) { Failure = __LINE__; } \
} while (0)

static void
LockAtDpc(PKSPIN_LOCK Address)
{
    CHECK(Address == &Slot->Lock && !Locked && Irql == DISPATCH_LEVEL);
    Locked = TRUE;
}

static void
UnlockAtDpc(PKSPIN_LOCK Address)
{
    CHECK(Address == &Slot->Lock && Locked && Irql == DISPATCH_LEVEL);
    Locked = FALSE;
}

static KIRQL
Lock(PKSPIN_LOCK Address)
{
    KIRQL Old = Irql;
    CHECK(Old <= DISPATCH_LEVEL);
    Irql = DISPATCH_LEVEL;
    LockAtDpc(Address);
    return Old;
}

static void
UnlockSlot(PKSPIN_LOCK Address, KIRQL Old)
{
    UnlockAtDpc(Address);
    Irql = Old;
}

static LONG
ReleaseControl(PKMUTEX Mutex, BOOLEAN Wait)
{
    CHECK(Mutex == &Slot->Control && ControlHeld && !Locked && Irql <= APC_LEVEL && !Wait);
    ControlHeld = FALSE;
    return 0;
}

static void InitializeLock(PKSPIN_LOCK Address) { CHECK(Address == &Slot->Lock && Irql == PASSIVE_LEVEL); }
static void InitializeControl(PKMUTEX Address, ULONG Level) { CHECK(Address == &Slot->Control && Level == 0 && Irql == PASSIVE_LEVEL); }
static void InitializeTimer(PKTIMER TimerObject) { CHECK(TimerObject == &Slot->Timer && Irql == PASSIVE_LEVEL); }
static void InitializeEvent(PRKEVENT Event, EVENT_TYPE Type, BOOLEAN Signaled)
{
    CHECK(Event != NULL && Type == NotificationEvent && !Signaled && Irql <= APC_LEVEL);
}

static void
InitializeDpc(PRKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
{
    CHECK(Dpc == &Slot->Dpc && Routine != NULL && Context == Slot && Irql == PASSIVE_LEVEL);
    TimerDpc = Routine;
    TimerContext = Context;
}

static BOOLEAN
SetTimer(PKTIMER Object, LARGE_INTEGER Due, PKDPC Dpc)
{
    BOOLEAN Previous = Timer;
    CHECK(Object == &Slot->Timer && Due.QuadPart == -10000 && Dpc == &Slot->Dpc && Locked);
    Timer = TRUE;
    return Previous;
}

static BOOLEAN
CancelTimer(PKTIMER Object)
{
    BOOLEAN Previous = Timer;
    CHECK(Object == &Slot->Timer && Locked);
    Timer = FALSE;
    return Previous;
}

static BOOLEAN
QueueDpc(PRKDPC Dpc, PVOID A, PVOID B)
{
    BOOLEAN WasQueued = Queued;
    CHECK(Dpc == &Slot->Dpc && A == NULL && B == NULL && Locked);
    Queued = TRUE;
    return !WasQueued;
}

static BOOLEAN
DequeueDpc(PRKDPC Dpc)
{
    BOOLEAN WasQueued = Queued;
    CHECK(Dpc == &Slot->Dpc && Irql == PASSIVE_LEVEL && !Locked && !ControlHeld);
    Queued = FALSE;
    return WasQueued;
}

static void FlushDpcs(void) { CHECK(Irql == PASSIVE_LEVEL && !Queued && !Timer && !Locked); }
static ULONGLONG QueryTime(void) { return Now; }

static PDEVICE_OBJECT
GetPdo(PDEVICE_OBJECT Device)
{
    CHECK(Device == &Fdo && Irql == PASSIVE_LEVEL);
    ++References;
    return &Pdo;
}

static void Dereference(PVOID Object) { CHECK(Object == &Pdo && References != 0); --References; }

static PVOID
AllocateCommon(PDMA_ADAPTER Dma, ULONG Length, PPHYSICAL_ADDRESS Logical, BOOLEAN Cached)
{
    CHECK(Dma == &Adapter && Length == SD_COMMON_BYTES && !Cached && Irql == PASSIVE_LEVEL);
    ++Allocations;
    Logical->QuadPart = BadAddress ? 0xffff0000LL : 0x80000000LL;
    return Common;
}

static void
FreeCommon(PDMA_ADAPTER Dma, ULONG Length, PHYSICAL_ADDRESS Logical, PVOID Buffer, BOOLEAN Cached)
{
    CHECK(Dma == &Adapter && Length == SD_COMMON_BYTES && Logical.QuadPart != 0 && Buffer == Common);
    CHECK(!Cached && Irql == PASSIVE_LEVEL && (Registers[MSDC_DMA_CFG / 4] & 1U) == 0);
    ++Frees;
}

static void PutAdapter(PDMA_ADAPTER Dma) { CHECK(Dma == &Adapter && Frees == Allocations); ++AdapterPuts; }

static PDMA_ADAPTER
GetAdapter(PDEVICE_OBJECT Device, PDEVICE_DESCRIPTION Description, PULONG Maps)
{
    CHECK(Device == &Pdo && Irql == PASSIVE_LEVEL && References == 2);
    CHECK(Description->Version == DEVICE_DESCRIPTION_VERSION && Description->Master &&
          Description->Dma32BitAddresses && Description->MaximumLength == SD_COMMON_BYTES);
    *Maps = ShortMap ? 1 : 32;
    return &Adapter;
}

static PIRP
AllocateIrp(CCHAR StackSize, BOOLEAN ChargeQuota)
{
    CHECK(StackSize == Pdo.StackSize && StackSize == 1 && !ChargeQuota && !IoctlOwned);
    RtlZeroMemory(&IoctlIrp, sizeof(IoctlIrp));
    RtlZeroMemory(&IoctlStack, sizeof(IoctlStack));
    IoctlIrp.Tail.Overlay.CurrentStackLocation = &IoctlStack + 1;
    IoctlIrp.StackCount = StackSize;
    IoctlIrp.CurrentLocation = StackSize + 1;
    IoctlOwned = TRUE;
    IoctlDone = FALSE;
    ++IoctlAllocations;
    return &IoctlIrp;
}

static void
FreeIrp(PIRP Irp)
{
    CHECK(Irp == &IoctlIrp && IoctlOwned && IoctlDone);
    IoctlOwned = FALSE;
    ++IoctlFrees;
}

static LONG
SignalIo(PKEVENT Event, KPRIORITY Increment, BOOLEAN Wait)
{
    CHECK(Event == IoctlStack.Context && Increment == IO_NO_INCREMENT && !Wait && IoctlOwned && !IoctlDone);
    IoctlDone = TRUE;
    return 0;
}

static void
CompleteIo(void)
{
    CHECK(IoctlStack.CompletionRoutine != NULL && !IoctlDone);
    CHECK(IoctlStack.CompletionRoutine(&Pdo, &IoctlIrp, IoctlStack.Context) == STATUS_MORE_PROCESSING_REQUIRED);
}

static NTSTATUS
CallLower(PDEVICE_OBJECT Device, PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetNextIrpStackLocation(Irp);
    PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER Input = Irp->AssociatedIrp.SystemBuffer;
    PACPI_EVAL_OUTPUT_BUFFER IoOutput = Irp->AssociatedIrp.SystemBuffer;
    ULONG InBytes = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG Value = 1;
    CHECK(Device == &Pdo && Irp == &IoctlIrp && IoctlOwned && !Irp->Tail.Overlay.Thread);
    CHECK(Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL && Stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_ACPI_EVAL_METHOD);
    CHECK(!Locked && Irql <= APC_LEVEL && Stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(*IoOutput));
    if (Input->MethodNameAsUlong == 'POCS') {
        ULONG Operation = Input->IntegerArgument;
        CHECK(InBytes == sizeof(*Input) && Input->Signature == ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE);
        CHECK(Operation <= 3 && (Operation == 0 || (Registers[MSDC_CFG / 4] & MSDC_CARD_CLOCK) == 0));
        ++Writes[Operation];
        if (MethodFault == Operation + 1) {
            Value = 0;
        } else {
            if (SaveDuringReset && Operation == 0) {
                SaveDuringReset = FALSE;
                const ULONG Before = MmioWrites;
                Api.SaveContext(Slot);
                CHECK(Slot->Suspended && Slot->ResumeFailed && MmioWrites == Before);
            }
            Hclk = Operation != 3;
            if (Operation != 0) {
                Source = Operation == 2;
            }
        }
    } else {
        CHECK(InBytes == sizeof(ACPI_EVAL_INPUT_BUFFER) && Input->Signature == ACPI_EVAL_INPUT_BUFFER_SIGNATURE);
        if (Input->MethodNameAsUlong == 'DCDS') {
            Value = UnknownCardState ? 2 : Card ? 1 : 0;
        } else {
            CHECK(Input->MethodNameAsUlong == 'CTDS' && Hclk && Source);
            ++PadWrites;
        }
    }
    IoOutput->Signature = BadOutput ? 0 : ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE;
    IoOutput->Length = sizeof(*IoOutput);
    IoOutput->Count = 1;
    IoOutput->Argument[0].Type = ACPI_METHOD_ARGUMENT_INTEGER;
    IoOutput->Argument[0].DataLength = sizeof(ULONG);
    IoOutput->Argument[0].Argument = Value;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = IoOutput->Length;
    if (PendingIo) {
        return STATUS_PENDING;
    }
    CompleteIo();
    return STATUS_SUCCESS;
}

static NTSTATUS
WaitForIo(PVOID Event, KWAIT_REASON Reason, KPROCESSOR_MODE Mode, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
{
    CHECK(Reason == Executive && Mode == KernelMode && !Alertable && Timeout == NULL);
    CHECK(Irql <= APC_LEVEL && !Locked);
    if (Event == &Slot->Control) {
        CHECK(!ControlHeld);
        ControlHeld = TRUE;
        return STATUS_SUCCESS;
    }
    CHECK(Event == IoctlStack.Context && PendingIo && !IoctlDone);
    ++Waits;
    CompleteIo();
    return STATUS_SUCCESS;
}

NTSTATUS
MtkSdPowerInitialize(MTK_SD_POWER *Power)
{
    CHECK(Power == &Slot->Power && Irql == PASSIVE_LEVEL && !Power->Settled);
    return STATUS_SUCCESS;
}

NTSTATUS
MtkSdPowerSet(MTK_SD_POWER *Power, BOOLEAN Enable)
{
    CHECK(Power == &Slot->Power && Irql <= APC_LEVEL && !Locked);
    if (!Enable) {
        CHECK((Registers[MSDC_CFG / 4] & MSDC_CARD_CLOCK) == 0);
        CHECK((Registers[MSDC_DMA_CFG / 4] & 1U) == 0);
    }
    ++RailCalls;
    Power->Settled = Enable;
    return STATUS_SUCCESS;
}

NTSTATUS
SdPortInitialize(PVOID Object, PVOID Path, PSDPORT_INITIALIZATION_DATA Initialization)
{
    CHECK(Object == &Fdo && Path == NULL && Initialization->StructureSize == sizeof(Api));
    Api = *Initialization;
    return STATUS_SUCCESS;
}

void
SdPortCompleteRequest(PSDPORT_REQUEST Which, NTSTATUS Status)
{
    CHECK(Which == &Request && !Locked && !ControlHeld && Which->RequiredEvents == 0 && Which->Status == Status);
    CHECK(Which != Slot->ActiveRequest);
    Completed = Which;
    CompletionStatus = Status;
    ++Completions;
}

ULONG
SdPortReadRegisterUlong(PVOID Base, ULONG Offset)
{
    CHECK(Base == Registers && (Offset & 3U) == 0 && Offset < MSDC1_REGISTER_BYTES && Hclk);
    if (MmioUnavailable) {
        return MAXULONG;
    }
    ++MmioReads;
    if (Offset == MSDC_CFG && Source) {
        return Registers[Offset / 4] | TEST_CLK_STABLE;
    }
    return Registers[Offset / 4];
}

void
SdPortWriteRegisterUlong(PVOID Base, ULONG Offset, ULONG Data)
{
    CHECK(Base == Registers && (Offset & 3U) == 0 && Offset < MSDC1_REGISTER_BYTES && Hclk);
    CHECK(!MmioUnavailable);
    ++MmioWrites;
    if (DropPatchWrite && Offset == TEST_PATCH0) {
        return;
    }
    if (Offset == MSDC_INT) {
        Registers[Offset / 4] &= ~Data;
        return;
    }
    if (Offset == MSDC_CFG) {
        Data &= ~(TEST_CLK_STABLE | (1U << 2));
    } else if (Offset == 0x014U) {
        Data &= ~(1U << 31);
    } else if (Offset == TEST_COMMAND) {
        CHECK(Source && Slot->Power.Settled && Slot->CardClockHz != 0);
        ++Commands;
    } else if (Offset == TEST_DMA_CONTROL && (Data & 1U)) {
        CHECK(Source && Slot->Power.Settled && Registers[TEST_DMA_ADDRESS / 4] == 0x80000000U);
        CHECK((Common[0] & 1U) != 0);
        if ((Registers[TEST_COMMAND / 4] & (1U << 13)) != 0) {
            CHECK(PendingWriteBytes > 0 && PendingWriteBytes <= SD_MAX_DATA_BYTES);
            for (ULONG I = 0; I < PendingWriteBytes; ++I) {
                CHECK(Common[SD_BUFFER_OFFSET + I] == WriteBuffer[1 + I]);
            }
            PendingWriteBytes = 0;
        }
        ++Starts;
        Registers[MSDC_DMA_CFG / 4] |= 1U;
        Data &= ~1U;
    } else if (Offset == TEST_DMA_CONTROL && (Data & 2U)) {
        ++Stops;
        if (!StopStuck) {
            Registers[MSDC_DMA_CFG / 4] &= ~1U;
            Data &= ~2U;
        }
    }
    Registers[Offset / 4] = Data;
}

void SdPortWait(ULONG Microseconds) { CHECK(Irql <= DISPATCH_LEVEL && !Locked); Now += Microseconds * 10ULL; }

static void
Tick(ULONG Milliseconds)
{
    Now += Milliseconds * 10000ULL;
    CHECK(Irql == PASSIVE_LEVEL);
    if (Timer || Queued) {
        Timer = Queued = FALSE;
        Irql = DISPATCH_LEVEL;
        TimerDpc(&Slot->Dpc, TimerContext, NULL, NULL);
        Irql = PASSIVE_LEVEL;
    }
}

static NTSTATUS
Issue(void)
{
    NTSTATUS Status;
    Irql = DISPATCH_LEVEL;
    Status = Api.IssueRequest(Slot, &Request);
    Irql = PASSIVE_LEVEL;
    return Status;
}

static NTSTATUS
Bus(SDPORT_BUS_OPERATION_TYPE Type, ULONG Value)
{
    SDPORT_BUS_OPERATION Operation = {0};
    Operation.Type = Type;
    switch (Type) {
    case SdSetClock: Operation.Parameters.FrequencyKhz = Value; break;
    case SdSetPower: Operation.Parameters.PowerEnabled = (BOOLEAN)Value; break;
    case SdSetVoltage: Operation.Parameters.Voltage = (SDPORT_BUS_VOLTAGE)Value; break;
    case SdSetBusWidth: Operation.Parameters.BusWidth = (SDPORT_BUS_WIDTH)Value; break;
    case SdResetHost: Operation.Parameters.ResetType = (SDPORT_RESET_TYPE)Value; break;
    default: break;
    }
    return Api.IssueBusOperation(Slot, &Operation);
}

static NTSTATUS
Initialize(ULONGLONG Base, ULONG RegisterBytes, BOOLEAN Dump)
{
    PHYSICAL_ADDRESS Address;
    Address.QuadPart = Base;
    return Api.Initialize(Slot, Address, Registers, RegisterBytes, Dump);
}

static void
Setup(void)
{
    PSDPORT_SLOT_EXTENSION Extension = (PSDPORT_SLOT_EXTENSION)SlotStorage;
    Slot = (MTK_SD_SLOT *)Extension->PrivateExtension;
    Extension->Miniport = &Miniport;
    Miniport.SlotExtensionList[0] = Extension;
    Miniport.ConfigurationInfo.DeviceObject = &Fdo;
    Miniport.ConfigurationInfo.BusType = SdBusTypeAcpi;
    Adapter.DmaOperations = &DmaOperations;
    DmaOperations.AllocateCommonBuffer = AllocateCommon;
    DmaOperations.FreeCommonBuffer = FreeCommon;
    DmaOperations.PutDmaAdapter = PutAdapter;
    Card = TRUE;
    Pdo.StackSize = 1;
    PendingIo = TRUE;
    Now = 1;
    Registers[MSDC_PS / 4] = MSDC_DAT0;
}

static void CardDetectInterrupt(void);

static void
CheckResources(void)
{
    SDPORT_CAPABILITIES Capabilities;
    UCHAR Count = 0;
    ULONG WaitsBeforeCardQuery, IoctlBefore;
    CHECK(DriverEntry(&Fdo, NULL) == STATUS_SUCCESS);
    CHECK(Api.PrivateExtensionSize == sizeof(*Slot) && !Api.CrashdumpSupported);
    CHECK(Api.GetSlotCount(&Miniport, &Count) == STATUS_SUCCESS && Count == 1);
    Api.GetSlotCapabilities(Slot, &Capabilities);
    CHECK(Capabilities.MaximumOutstandingRequests == 1 && Capabilities.MaximumBlockSize == 512);
    CHECK(Capabilities.Supported.Voltage33V && !Capabilities.Supported.Voltage18V && !Capabilities.Supported.ScatterGatherDma);
    CHECK(Capabilities.Supported.SaveContext && Api.SaveContext != NULL && Api.RestoreContext != NULL);
    CHECK(Capabilities.Flags.UsePioForRead && Capabilities.Flags.UsePioForWrite);
    CHECK(Api.GetWriteProtectState(Slot));
    CHECK(Initialize(0x11230000, MSDC1_REGISTER_BYTES, FALSE) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(Initialize(MSDC1_BASE, 0x800, FALSE) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(Initialize(MSDC1_BASE, MSDC1_REGISTER_BYTES, TRUE) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(References == 0 && Allocations == 0);
    ShortMap = TRUE;
    CHECK(Initialize(MSDC1_BASE, MSDC1_REGISTER_BYTES, FALSE) == STATUS_INSUFFICIENT_RESOURCES);
    CHECK(Allocations == 0 && Frees == 0 && AdapterPuts == 1 && References == 0);
    ShortMap = FALSE;
    BadAddress = TRUE;
    CHECK(Initialize(MSDC1_BASE, MSDC1_REGISTER_BYTES, FALSE) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(Frees == 1 && AdapterPuts == 2 && References == 0);
    BadAddress = FALSE;
    CHECK(Initialize(MSDC1_BASE, MSDC1_REGISTER_BYTES, FALSE) == STATUS_SUCCESS);
    CHECK(Slot->CardPresent && References == 1 && Waits == 1 && RailCalls == 0);
    CHECK(!Api.GetWriteProtectState(Slot));
    BadOutput = TRUE;
    CHECK(Api.GetCardDetectState(Slot) && !Slot->CardRemoved);
    BadOutput = FALSE;
    UnknownCardState = TRUE;
    CHECK(Api.GetCardDetectState(Slot) && !Slot->CardRemoved && !Api.GetWriteProtectState(Slot));
    UnknownCardState = FALSE;
    WaitsBeforeCardQuery = Waits;
    Irql = APC_LEVEL;
    CHECK(Api.GetCardDetectState(Slot) && Irql == APC_LEVEL &&
          Waits == WaitsBeforeCardQuery + 1);
    Irql = PASSIVE_LEVEL;
    IoctlBefore = IoctlAllocations;
    PendingIo = FALSE;
    CHECK(Api.GetCardDetectState(Slot) &&
          IoctlAllocations == IoctlBefore + 1 && IoctlFrees == IoctlAllocations);
    PendingIo = TRUE;
    CardDetectInterrupt();
    PendingIo = FALSE;
}

static void
CardDetectInterrupt(void)
{
    ULONG Reads = MmioReads;
    ULONG WritesBefore = MmioWrites;
    ULONG Before = Completions, WaitsBefore = Waits;
    ULONG ReadsAfterDetect, WritesAfterDetect, WaitsAfterDetect;
    ULONG Events = 0xaaaaaaaaU;
    ULONG Errors = 0xbbbbbbbbU;
    BOOLEAN Changed = FALSE;
    BOOLEAN Sdio = TRUE;
    BOOLEAN Tuning = TRUE;
    NTSTATUS State;

    /* ACPI GpioInt GPIO7 reached the shared SDPORT notification callback. */
    Irql = (KIRQL)4; // Model the GPIO and host DIRQL callback.
    CHECK(Api.Interrupt(Slot, &Events, &Errors, &Changed, &Sdio, &Tuning));
    CHECK(Events == 0 && Errors == 0 && Changed && !Sdio && !Tuning);
    CHECK(MmioReads == Reads && MmioWrites == WritesBefore && Completions == Before);

    /* SDPORT calls the passive card-state method after the notification. */
    Irql = PASSIVE_LEVEL;
    State = Api.GetCardDetectState(Slot) ? STATUS_SUCCESS : STATUS_NO_MEDIA_IN_DEVICE;
    CHECK(State == STATUS_SUCCESS && Waits == WaitsBefore + 1);
    Card = FALSE;
    CHECK(!Api.GetCardDetectState(Slot) && Slot->CardRemoved &&
          Api.GetWriteProtectState(Slot));
    CHECK(Issue() == STATUS_NO_MEDIA_IN_DEVICE);
    Card = TRUE;
    CHECK(Api.GetCardDetectState(Slot) && Slot->CardPresent && Slot->CardRemoved);
    CHECK(Api.GetWriteProtectState(Slot) && Issue() == STATUS_DEVICE_NOT_READY);
    CHECK(Waits == WaitsBefore + 3);
    ReadsAfterDetect = MmioReads;
    WritesAfterDetect = MmioWrites;
    WaitsAfterDetect = Waits;

    /* The GPIO event still asks SDPORT to rescan, and removal suppresses it. */
    Events = Errors = 0xdeadbeefU;
    Changed = Sdio = Tuning = TRUE;
    Slot->Removing = TRUE;
    CHECK(!Api.Interrupt(Slot, &Events, &Errors, &Changed, &Sdio, &Tuning));
    CHECK(Events == 0 && Errors == 0 && !Changed && !Sdio && !Tuning);
    Slot->Removing = FALSE;
    CHECK(MmioReads == ReadsAfterDetect && MmioWrites == WritesAfterDetect &&
          Waits == WaitsAfterDetect && Completions == Before);
}

static void
PowerAndReset(void)
{
    CHECK(Bus(SdSetClock, 400) == STATUS_SUCCESS);
    CHECK(Bus(SdSetVoltage, SdBusVoltage18) == STATUS_NOT_SUPPORTED);
    MethodFault = 3; /* SCOP(2) */
    NTSTATUS FailedPower = Bus(SdSetPower, TRUE);
    CHECK(FailedPower == STATUS_DEVICE_NOT_READY);
    CHECK(!Slot->HostReady);
    CHECK(RailCalls == 0);
    MethodFault = 0;
    CHECK(Bus(SdSetPower, TRUE) == STATUS_SUCCESS && Slot->Power.Settled && RailCalls == 1);
    CHECK(Slot->CardClockHz == 382352 && Hclk && Source && PadWrites == 1);
    CHECK((Registers[MSDC_CFG / 4] & MSDC_CARD_CLOCK) != 0);
}

static void
CheckCompletion(ULONG Before, NTSTATUS Status)
{
    CHECK(Completions == Before + 1 && Completed == &Request && CompletionStatus == Status);
    Tick(1);
    CHECK(Completions == Before + 1);
}

static void
Command(UCHAR Index, SDPORT_RESPONSE_TYPE Response)
{
    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = SdRequestTypeCommandNoTransfer;
    Request.Command.TransferType = SdTransferTypeNone;
    Request.Command.Index = Index;
    Request.Command.ResponseType = Response;
}

static void
CheckCommands(void)
{
    ULONG Before = Completions;
    ULONG Response;
    Command(8, SdResponseTypeR1);
    Request.Command.Argument = 0x1AA;
    CHECK(Issue() == STATUS_PENDING);
    UnknownCardState = TRUE;
    CHECK(Api.GetCardDetectState(Slot) && Slot->ActiveRequest == &Request &&
          !Slot->CardRemoved && Completions == Before);
    UnknownCardState = FALSE;
    CHECK(Api.GetCardDetectState(Slot));
    Irql = DISPATCH_LEVEL;
    CHECK(Api.IssueRequest(Slot, &Other) == STATUS_DEVICE_BUSY);
    Irql = PASSIVE_LEVEL;
    Tick(1);
    CHECK(Completions == Before);
    Registers[TEST_RESPONSE / 4] = 0x1AA;
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY;
    Tick(1);
    CheckCompletion(Before, STATUS_SUCCESS);
    Api.GetResponse(Slot, &Request.Command, &Response);
    CHECK(Response == 0x1AA && Registers[MSDC_INT / 4] == 0);

    Before = Completions;
    Command(7, SdResponseTypeR1B);
    Registers[MSDC_PS / 4] = 0;
    CHECK(Issue() == STATUS_PENDING);
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY;
    Tick(1);
    CHECK(Completions == Before && Slot->Command.Phase == T70SdCommandCardBusy);
    Registers[MSDC_PS / 4] = MSDC_DAT0;
    Tick(1);
    CheckCompletion(Before, STATUS_SUCCESS);

    Before = Completions;
    CHECK(Issue() == STATUS_PENDING);
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY | T70_SD_COMMAND_IRQ_CRC;
    Tick(1);
    CheckCompletion(Before, STATUS_CRC_ERROR);

    ULONG PreviousCommands = Commands;
    Command(24, SdResponseTypeR1);
    CHECK(Issue() == STATUS_INVALID_PARAMETER);
    Command(27, SdResponseTypeR1);
    CHECK(Issue() == STATUS_MEDIA_WRITE_PROTECTED);
    Command(52, SdResponseTypeR5);
    Request.Command.Argument = 1U << 31;
    CHECK(Issue() == STATUS_MEDIA_WRITE_PROTECTED && Commands == PreviousCommands);
}

static void
PrepareRead(USHORT Bytes, USHORT Blocks, BOOLEAN AutoStop)
{
    Command(Blocks == 1 ? 17 : 18, SdResponseTypeR1);
    Request.Type = SdRequestTypeCommandWithTransfer;
    Request.Command.TransferType = Blocks == 1 ? SdTransferTypeSingleBlock : SdTransferTypeMultiBlock;
    Request.Command.TransferMethod = SdTransferMethodPio;
    Request.Command.TransferDirection = SdTransferDirectionRead;
    Request.Command.BlockSize = Bytes;
    Request.Command.BlockCount = Blocks;
    Request.Command.DataBuffer = ReadBuffer + 1;
    Request.Command.UseAutoCmd12 = AutoStop;
    RtlFillMemory(ReadBuffer, sizeof(ReadBuffer), 0xCC);
    for (ULONG I = 0; I < (ULONG)Bytes * Blocks; I++) {
        Common[SD_BUFFER_OFFSET + I] = (UCHAR)(I / Bytes + I % Bytes);
    }
}

static void
BeginRead(USHORT Bytes, USHORT Blocks, BOOLEAN AutoStop)
{
    ULONG Before = Completions;
    ULONG Started = Starts;
    PrepareRead(Bytes, Blocks, AutoStop);
    CHECK(Issue() == STATUS_PENDING && Starts == Started);
    Registers[TEST_RESPONSE / 4] = 0;
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY;
    Tick(1);
    CHECK(Completions == Before + 1 && Starts == Started + 1 && CompletionStatus == STATUS_SUCCESS);
    Request.Type = SdRequestTypeStartTransfer;
    Request.Command.Index = 0;
    CHECK(Issue() == STATUS_PENDING);
}

static void
ReadAndStop(void)
{
    BeginRead(512, 128, TRUE);
    const ULONG Before = Completions;
    Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_COMPLETE;
    Registers[MSDC_PS / 4] = 0;
    Tick(1);
    CHECK(Completions == Before && ReadBuffer[1] == 0xCC && Stops == 1);
    Registers[MSDC_INT / 4] = MSDC_AUTO_STOP_OK;
    Tick(1);
    CHECK(Completions == Before && ReadBuffer[1] == 0xCC);
    Registers[MSDC_PS / 4] = MSDC_DAT0;
    Tick(1);
    for (USHORT Block = 0; Block < 128; ++Block) {
        CHECK(Completions == Before + Block + 1);
        CHECK(CompletionStatus == (Block == 127 ? STATUS_SUCCESS : STATUS_MORE_PROCESSING_REQUIRED));
        CHECK(Request.Command.BlockCount == 127 - Block);
        for (USHORT I = 0; I < 512; ++I) {
            CHECK(ReadBuffer[1 + Block * 512 + I] == (UCHAR)(Block + I));
        }
        if (Block != 127) {
            CHECK(ReadBuffer[1 + (Block + 1) * 512] == 0xCC && Issue() == STATUS_PENDING);
            Tick(0);
        }
    }
    CHECK(ReadBuffer[0] == 0xCC && ReadBuffer[1 + SD_MAX_DATA_BYTES] == 0xCC);
    CHECK(Slot->Dma.Phase == T70SdDmaIdle && Slot->DataBytes == 0 && !Timer && !Queued);
    CHECK(Issue() == STATUS_INVALID_PARAMETER);
}

static void
ReadErrors(void)
{
    ULONG Before = Completions;
    PrepareRead(512, 1, FALSE);
    Request.Command.TransferDirection = SdTransferDirectionWrite;
    CHECK(Issue() == STATUS_INVALID_PARAMETER && Completions == Before);
    PrepareRead(512, 1, FALSE);
    Request.Command.Length = 511;
    CHECK(Issue() == STATUS_INVALID_BUFFER_SIZE && Completions == Before);

    BeginRead(8, 1, FALSE);
    Before = Completions;
    Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_COMPLETE;
    Tick(1);
    CheckCompletion(Before, STATUS_SUCCESS);
    CHECK(ReadBuffer[0] == 0xCC && ReadBuffer[1] == 0 && ReadBuffer[8] == 7 && ReadBuffer[9] == 0xCC);

    BeginRead(512, 1, FALSE);
    Before = Completions;
    Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_COMPLETE | T70_SD_DMA_IRQ_CRC;
    Tick(1);
    CheckCompletion(Before, STATUS_CRC_ERROR);
    CHECK(ReadBuffer[1] == 0xCC && Slot->Faulted);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    BeginRead(512, 2, TRUE);
    Before = Completions;
    Registers[MSDC_INT / 4] = MSDC_AUTO_STOP_OK | T70_SD_DMA_IRQ_COMPLETE;
    Registers[SDC_ACMD_RESP / 4] = 1U << 22;
    Tick(1);
    CheckCompletion(Before, STATUS_DEVICE_DATA_ERROR);
    Registers[SDC_ACMD_RESP / 4] = 0;
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    BeginRead(512, 1, FALSE);
    Before = Completions;
    Card = FALSE;
    CHECK(!Api.GetCardDetectState(Slot));
    Card = TRUE;
    CHECK(Api.GetCardDetectState(Slot));
    Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_COMPLETE;
    Tick(0);
    CheckCompletion(Before, STATUS_NO_MEDIA_IN_DEVICE);
    CHECK(ReadBuffer[1] == 0xCC && Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);
}

static void
PrepareWrite(USHORT Blocks, BOOLEAN AutoStop)
{
    Command(Blocks == 1 ? 24 : 25, SdResponseTypeR1);
    Request.Type = SdRequestTypeCommandWithTransfer;
    Request.Command.TransferType = Blocks == 1 ? SdTransferTypeSingleBlock : SdTransferTypeMultiBlock;
    Request.Command.TransferMethod = SdTransferMethodPio;
    Request.Command.TransferDirection = SdTransferDirectionWrite;
    Request.Command.BlockSize = 512;
    Request.Command.BlockCount = Blocks;
    Request.Command.Length = 512U * Blocks;
    Request.Command.Argument = 123;
    Request.Command.UseAutoCmd12 = AutoStop;
    // The payload becomes available with StartTransfer.
    Request.Command.DataBuffer = NULL;
    RtlFillMemory(Common + SD_BUFFER_OFFSET, Request.Command.Length, 0xCC);
    for (ULONG I = 0; I < 512U * Blocks; ++I) {
        WriteBuffer[1 + I] = (UCHAR)(I + I / 512);
    }
}

static void
AcceptWriteCommand(void)
{
    const ULONG Prior = Completions;
    const ULONG Started = Starts;
    const BOOLEAN Multi = Request.Command.Index == 25;

    CHECK(Issue() == STATUS_PENDING && Starts == Started);
    CHECK(Registers[TEST_COMMAND / 4] ==
          ((Multi ? 0x02003099U : 0x02002898U) |
           (Request.Command.UseAutoCmd12 ? 1U << 28 : 0)));
    Registers[TEST_RESPONSE / 4] = 0;
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY;
    Tick(1);
    CheckCompletion(Prior, STATUS_SUCCESS);
    CHECK(Slot->Dma.Phase == T70SdDmaArmed && Starts == Started);
    CHECK(Common[SD_BUFFER_OFFSET] == 0xCC);
    Request.Type = SdRequestTypeStartTransfer;
    Request.Command.Index = 0;
    Request.Command.DataBuffer = WriteBuffer + 1;
}

static void
StartAcceptedWrite(void)
{
    const ULONG Started = Starts;
    PendingWriteBytes = Request.Command.Length;
    CHECK(Issue() == STATUS_PENDING && Starts == Started + 1 && !PendingWriteBytes);
    CHECK(Slot->Dma.Phase == T70SdDmaRunning);
}

static void
WriteAndStop(void)
{
    for (USHORT Blocks = 1; Blocks <= 128; Blocks += 127) {
        const BOOLEAN Multi = Blocks > 1;
        PrepareWrite(Blocks, Multi);
        AcceptWriteCommand();
        const ULONG Started = Starts;
        Request.Command.Length--;
        CHECK(Issue() == STATUS_INVALID_PARAMETER && Starts == Started);
        Request.Command.Length++;
        StartAcceptedWrite();
        const ULONG Before = Completions;
        Registers[MSDC_PS / 4] = 0;
        Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_COMPLETE;
        Tick(1);
        CHECK(Completions == Before && Slot->Dma.Phase == T70SdDmaIdle);
        if (Multi) {
            Registers[MSDC_INT / 4] = MSDC_AUTO_STOP_OK;
            Tick(1);
            CHECK(Completions == Before);
        }
        Registers[MSDC_PS / 4] = MSDC_DAT0;
        Tick(1);
        CheckCompletion(Before, STATUS_SUCCESS);
        CHECK(Request.Command.BlockCount == 0 && Slot->DataBytes == 0);
        CHECK(Request.Command.DataBuffer == WriteBuffer + 1 + (Blocks - 1) * 512);
    }

    PrepareWrite(2, FALSE);
    AcceptWriteCommand();
    StartAcceptedWrite();
    ULONG Before = Completions;
    Registers[MSDC_PS / 4] = 0;
    Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_COMPLETE;
    Tick(1);
    CheckCompletion(Before, STATUS_SUCCESS);

    Before = Completions;
    Command(12, SdResponseTypeR1B);
    CHECK(Issue() == STATUS_PENDING);
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY;
    Tick(1);
    CHECK(Completions == Before);
    Registers[MSDC_PS / 4] = MSDC_DAT0;
    Tick(1);
    CheckCompletion(Before, STATUS_SUCCESS);
}

static void
WriteErrors(void)
{
    ULONG Before = Completions;
    const ULONG Started = Starts;

    PrepareWrite(1, FALSE);
    CHECK(Issue() == STATUS_PENDING);
    Registers[TEST_RESPONSE / 4] = SD_R1_WP_VIOLATION;
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY;
    Tick(1);
    CheckCompletion(Before, STATUS_MEDIA_WRITE_PROTECTED);
    CHECK(Starts == Started && Slot->Dma.Phase == T70SdDmaIdle);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    PrepareWrite(1, FALSE);
    AcceptWriteCommand();
    Before = Completions;
    Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_TIMEOUT;
    CHECK(Issue() == STATUS_PENDING && Starts == Started);
    Tick(0);
    CheckCompletion(Before, STATUS_IO_TIMEOUT);
    CHECK(Common[SD_BUFFER_OFFSET] == 0xCC);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    PrepareWrite(1, FALSE);
    AcceptWriteCommand();
    Card = FALSE;
    CHECK(!Api.GetCardDetectState(Slot) && Api.GetWriteProtectState(Slot));
    Card = TRUE;
    CHECK(Api.GetCardDetectState(Slot));
    CHECK(Issue() == STATUS_DEVICE_NOT_READY && Starts == Started);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS && !Api.GetWriteProtectState(Slot));

    PrepareWrite(1, FALSE);
    AcceptWriteCommand();
    StartAcceptedWrite();
    Before = Completions;
    Registers[MSDC_INT / 4] = T70_SD_DMA_IRQ_CRC | T70_SD_DMA_IRQ_COMPLETE;
    Tick(1);
    CheckCompletion(Before, STATUS_CRC_ERROR);
    CHECK(Slot->Dma.Phase == T70SdDmaIdle && Slot->Faulted && Request.Command.BlockCount == 1);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);
}

static void
Transition(PSDPORT_SAVE_CONTEXT Callback)
{
    CHECK(Callback != NULL && Irql == PASSIVE_LEVEL && !ControlHeld);
    if (Callback != NULL) {
        Irql = DISPATCH_LEVEL;
        Callback(Slot);
        CHECK(Irql == DISPATCH_LEVEL && !Locked && !ControlHeld);
        Irql = PASSIVE_LEVEL;
    }
}

static void
PowerTransitions(void)
{
    CHECK(Bus(SdSetBusWidth, SdBusWidth4Bit) == STATUS_SUCCESS);
    CHECK(Bus(SdSetClock, 25000) == STATUS_SUCCESS);
    const ULONG Config = Registers[MSDC_CFG / 4];
    const ULONG Sdc = Registers[TEST_SDC_CFG / 4];
    const ULONG Patch = Registers[TEST_PATCH0 / 4];
    const ULONG StartsBefore = Starts;
    const ULONG RailsBefore = RailCalls;
    const ULONG PadsBefore = PadWrites;
    const ULONG ClockCalls = Writes[0] + Writes[1] + Writes[2] + Writes[3];
    ULONG Before = Completions;

    Transition(Api.SaveContext);
    CHECK(Slot->Suspended && Slot->ContextValid && Source && Hclk && Slot->Power.Settled);
    CHECK((Registers[MSDC_CFG / 4] & MSDC_CARD_CLOCK) == 0);
    ULONG RegisterWrites = MmioWrites;
    Transition(Api.SaveContext);
    Command(13, SdResponseTypeR1);
    CHECK(Issue() == STATUS_DEVICE_NOT_READY && Completions == Before && MmioWrites == RegisterWrites);
    CHECK(Bus(SdSetClock, 400) == STATUS_DEVICE_NOT_READY);

    /* Model lost host registers with the board clocks, pads and rail retained. */
    Registers[MSDC_CFG / 4] = 0;
    Registers[TEST_SDC_CFG / 4] = 8U;
    Registers[TEST_PATCH0 / 4] = 0;
    Registers[TEST_DMA_ADDRESS / 4] = 0;
    Transition(Api.RestoreContext);
    CHECK(!Slot->Suspended && !Slot->ResumeFailed && !Slot->ContextValid);
    CHECK(Slot->BusWidth == 4 && Slot->CardClockHz == 13000000);
    CHECK(Registers[MSDC_CFG / 4] == Config && Registers[TEST_SDC_CFG / 4] == (Sdc | 8U));
    CHECK(Registers[TEST_PATCH0 / 4] == Patch && Registers[TEST_DMA_ADDRESS / 4] == 0);
    CHECK(RailCalls == RailsBefore && PadWrites == PadsBefore && Starts == StartsBefore);
    CHECK(Writes[0] + Writes[1] + Writes[2] + Writes[3] == ClockCalls);
    RegisterWrites = MmioWrites;
    Transition(Api.RestoreContext);
    CHECK(MmioWrites == RegisterWrites);
    CHECK(Issue() == STATUS_PENDING);
    Registers[MSDC_INT / 4] = T70_SD_COMMAND_IRQ_READY;
    Tick(1);
    CheckCompletion(Before, STATUS_SUCCESS);

    Transition(Api.SaveContext);
    Source = FALSE;
    RegisterWrites = MmioWrites;
    Transition(Api.RestoreContext);
    CHECK(Slot->ResumeFailed && MmioWrites == RegisterWrites && Issue() == STATUS_DEVICE_NOT_READY);
    MethodFault = 1;
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_IO_DEVICE_ERROR && Slot->ResumeFailed);
    MethodFault = 0;
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS && !Slot->ResumeFailed);
    CHECK(Slot->BusWidth == 4 && Slot->CardClockHz == 13000000 && Source);

    Transition(Api.SaveContext);
    Registers[TEST_PATCH0 / 4] = 0;
    DropPatchWrite = TRUE;
    Transition(Api.RestoreContext);
    DropPatchWrite = FALSE;
    CHECK(Slot->ResumeFailed && (Registers[MSDC_CFG / 4] & MSDC_CARD_CLOCK) == 0);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    Transition(Api.SaveContext);
    MmioUnavailable = TRUE;
    RegisterWrites = MmioWrites;
    Transition(Api.RestoreContext);
    CHECK(Slot->ResumeFailed && MmioWrites == RegisterWrites && Issue() == STATUS_DEVICE_NOT_READY);
    MmioUnavailable = FALSE;
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    Transition(Api.SaveContext);
    Card = FALSE;
    CHECK(!Api.GetCardDetectState(Slot));
    Card = TRUE;
    CHECK(Api.GetCardDetectState(Slot));
    RegisterWrites = MmioWrites;
    Transition(Api.RestoreContext);
    CHECK(Slot->ResumeFailed && Issue() == STATUS_DEVICE_NOT_READY && MmioWrites == RegisterWrites);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    /* A clock flag inconsistent with the programmed rate or a busy card
       must not produce a snapshot that certifies an idle bus. */
    for (ULONG Case = 0; Case < 2; ++Case) {
        if (Case == 0) {
            Registers[MSDC_CFG / 4] &= ~MSDC_CARD_CLOCK;
        } else {
            Registers[MSDC_PS / 4] = 0;
        }
        RegisterWrites = MmioWrites;
        Transition(Api.SaveContext);
        Transition(Api.RestoreContext);
        CHECK(Slot->ResumeFailed && MmioWrites == RegisterWrites && Issue() == STATUS_DEVICE_NOT_READY);
        Registers[MSDC_PS / 4] = MSDC_DAT0;
        CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS && !Slot->ResumeFailed);
    }

    SaveDuringReset = TRUE;
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS && !SaveDuringReset);
    Transition(Api.RestoreContext);
    CHECK(Slot->ResumeFailed && Issue() == STATUS_DEVICE_NOT_READY);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS && !Slot->ResumeFailed);

    Before = Completions;
    CHECK(Issue() == STATUS_PENDING);
    Transition(Api.SaveContext);
    CHECK(Slot->ResumeFailed && !Timer && Queued && Completions == Before);
    Transition(Api.RestoreContext);
    Tick(0);
    CheckCompletion(Before, STATUS_DEVICE_POWER_FAILURE);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    BeginRead(512, 1, FALSE);
    Before = Completions;
    StopStuck = TRUE;
    Transition(Api.SaveContext);
    Tick(0);
    CheckCompletion(Before, STATUS_DEVICE_POWER_FAILURE);
    CHECK(ReadBuffer[1] == 0xCC && Slot->Dma.Phase == T70SdDmaDraining && Frees == 1 && Source);
    Transition(Api.RestoreContext);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_IO_TIMEOUT && Slot->ResumeFailed && Frees == 1);
    StopStuck = FALSE;
    Registers[TEST_DMA_CONTROL / 4] &= ~2U;
    Registers[MSDC_DMA_CFG / 4] &= ~1U;
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS && !Slot->ResumeFailed);

    Transition(Api.SaveContext);
    CHECK(Bus(SdSetClock, 0) == STATUS_SUCCESS && !Source);
    Transition(Api.RestoreContext);
    CHECK(!Source && !Slot->ResumeFailed && Slot->CardClockHz == 0);
    CHECK(Bus(SdSetClock, 25000) == STATUS_SUCCESS);

    Transition(Api.SaveContext);
    CHECK(Bus(SdSetPower, FALSE) == STATUS_SUCCESS && !Slot->Power.Settled);
    Transition(Api.RestoreContext);
    CHECK((Registers[MSDC_CFG / 4] & MSDC_CARD_CLOCK) == 0 && !Slot->ResumeFailed);
    CHECK(Bus(SdSetPower, TRUE) == STATUS_SUCCESS);
    CHECK(!Api.GetWriteProtectState(Slot));
}

static void
TimeoutAndRemoval(void)
{
    ULONG Before = Completions;
    Command(13, SdResponseTypeR1);
    CHECK(Issue() == STATUS_PENDING);
    Tick(1000);
    CheckCompletion(Before, STATUS_IO_TIMEOUT);
    CHECK(Issue() == STATUS_DEVICE_NOT_READY && Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS);

    BeginRead(512, 1, FALSE);
    Before = Completions;
    StopStuck = TRUE;
    Tick(2000);
    CHECK(Completions == Before && Timer && Slot->Dma.Phase == T70SdDmaDraining);
    Tick(10);
    CheckCompletion(Before, STATUS_IO_TIMEOUT);
    CHECK(ReadBuffer[1] == 0xCC && !Timer && Frees == 1);
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_IO_TIMEOUT && Frees == 1);
    StopStuck = FALSE;
    Registers[TEST_DMA_CONTROL / 4] &= ~2U;
    Registers[MSDC_DMA_CFG / 4] &= ~1U;
    CHECK(Bus(SdResetHost, SdResetTypeCmd) == STATUS_SUCCESS && !Slot->Faulted);

    BeginRead(512, 1, FALSE);
    Before = Completions;
    ULONG FreesBeforeQuiesce = Frees;
    ULONGLONG QuiesceStarted = Now;
    StopStuck = TRUE;
    CHECK(Bus(SdSetPower, FALSE) == STATUS_IO_TIMEOUT);
    CHECK(Now - QuiesceStarted >= (ULONGLONG)SD_DMA_QUIESCE_TIMEOUT_US * 10U &&
          Slot->Dma.Phase == T70SdDmaDraining && Frees == FreesBeforeQuiesce);
    CheckCompletion(Before, STATUS_CANCELLED);
    StopStuck = FALSE;
    Registers[TEST_DMA_CONTROL / 4] &= ~2U;
    Registers[MSDC_DMA_CFG / 4] &= ~1U;
    CHECK(Bus(SdSetPower, FALSE) == STATUS_SUCCESS);
    CHECK(!Slot->Power.Settled && ReadBuffer[1] == 0xCC && Hclk && !Source);
    CHECK(Bus(SdSetPower, TRUE) == STATUS_SUCCESS);

    BeginRead(512, 1, FALSE);
    Before = Completions;
    Api.Cleanup(&Miniport);
    CheckCompletion(Before, STATUS_DEVICE_REMOVED);
    CHECK(!Slot->Power.Settled && !Hclk && !Source && Frees == 2 && AdapterPuts == 3 && References == 0);
    Api.Cleanup(&Miniport);
    CHECK(Frees == 2 && AdapterPuts == 3 && References == 0 && Completions == Before + 1);

    CHECK(Initialize(MSDC1_BASE, MSDC1_REGISTER_BYTES, FALSE) == STATUS_SUCCESS);
    CHECK(Bus(SdSetClock, 400) == STATUS_SUCCESS && Bus(SdSetPower, TRUE) == STATUS_SUCCESS);
    BeginRead(512, 1, FALSE);
    Before = Completions;
    StopStuck = TRUE;
    Transition(Api.SaveContext);
    CHECK(Queued && !Timer && Completions == Before);
    Api.Cleanup(&Miniport);
    CheckCompletion(Before, STATUS_DEVICE_REMOVED);
    CHECK(Slot->Power.Settled && Hclk && Source && References == 0);
    CHECK(Frees == 2 && AdapterPuts == 3 && ReadBuffer[1] == 0xCC && !Timer && !Queued);
}

int
TestMain(void)
{
    Setup();
    CheckResources();
    if (Failure) { return (int)Failure; }
    PowerAndReset();
    CheckCommands();
    ReadAndStop();
    ReadErrors();
    WriteAndStop();
    WriteErrors();
    PowerTransitions();
    TimeoutAndRemoval();
    return (int)Failure;
}
