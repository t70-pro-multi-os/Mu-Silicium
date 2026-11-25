// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>

static ULONG TestRead(PULONG Address);
static VOID TestWrite(PULONG Address, ULONG Value);
static PVOID TestMap(PHYSICAL_ADDRESS Address, SIZE_T Bytes, ULONG Protection);
static VOID TestUnmap(PVOID Address, SIZE_T Bytes);
static ULONGLONG TestTime(PULONGLONG Counter);
static NTSTATUS TestDelay(KPROCESSOR_MODE Mode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
static VOID TestStall(ULONG Microseconds);

#undef READ_REGISTER_ULONG
#undef WRITE_REGISTER_ULONG
#define READ_REGISTER_ULONG TestRead
#define WRITE_REGISTER_ULONG TestWrite
#define MmMapIoSpaceEx TestMap
#define MmUnmapIoSpace TestUnmap
#define KeQueryInterruptTimePrecise TestTime
#define KeDelayExecutionThread TestDelay
#define KeStallExecutionProcessor TestStall
#include "Driver.c"

#define HANDLE_OF(Type, Number) ((Type)(ULONG_PTR)(Number))
#define TEST_DEVICE HANDLE_OF(WDFDEVICE, 0x100)
#define TEST_QUEUE HANDLE_OF(WDFQUEUE, 0x101)
#define TEST_REQUEST HANDLE_OF(WDFREQUEST, 0x102)
#define TEST_RAW HANDLE_OF(WDFCMRESLIST, 0x103)
#define TEST_TRANSLATED HANDLE_OF(WDFCMRESLIST, 0x104)
#define TEST_WAIT_QUEUE HANDLE_OF(WDFQUEUE, 0x105)
#define TEST_TIMER HANDLE_OF(WDFTIMER, 0x106)
#define TEST_LOCK HANDLE_OF(WDFSPINLOCK, 0x107)
#define TEST_IMMEDIATE HANDLE_OF(WDFREQUEST, 0x108)
#define TEST_LOWER HANDLE_OF(WDFIOTARGET, 0x109)
#define CHECK(Condition) do { if (!(Condition) && !Failure) Failure = __LINE__; } while (0)

static int Failure;
static WDFFUNC TestWdf[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions = TestWdf;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
static NTC_DEVICE Device;
static CM_PARTIAL_RESOURCE_DESCRIPTOR Descriptors[2][ResourceCount];
static ULONG Registers[0x1000 / sizeof(ULONG)], Clock, Fuse, RawCode, TriggerMask;
static ULONG SocRegisters[0x1000 / sizeof(ULONG)], SocClock, SocReads, DescriptorCount;
static ULONG SocFuse[MTK_LVTS_EFUSE_DATA1_WORDS], SocRcFuse[2];
static BOOLEAN LoseSocClockOnSample, ChangeCalibrationOnSample, RestoreSocClock;
static NTSTATUS RestoreClockStatus;
static ULONG RestoreClockCalls;
static ULONG Maps, Unmaps, MapFailure, Writes, ChannelReads, LastTrigger;
static BOOLEAN StaleReady, MissingReady, LoseClockOnWait, RejectPower;
static ULONGLONG Now;
static NTSTATUS Completed;
static ULONG_PTR Information;
static union { ULONG Channel; T70_NTC_READING Reading; } BufferedIo;
static union { THERMAL_WAIT_READ Input; ULONG Output; } ThermalIo[2];
static NTC_THERMAL_WAIT WaitContext;
static WDFREQUEST PendingThermal;
static ULONGLONG NextPoll;
static ULONG WaitQueueStarts, WaitQueuePurges, QueueRefs;
static BOOLEAN Locked, CancelOnRetrieve;

static ULONG TestRead(PULONG Address) {
    if (Address == &Clock) return Clock;
    if (Address == &Fuse) return Fuse;
    if (Address == &SocClock) return SocClock;
    if ((ULONG_PTR)Address >= (ULONG_PTR)SocFuse &&
        (ULONG_PTR)Address < (ULONG_PTR)(SocFuse + ARRAYSIZE(SocFuse)))
        return *Address;
    if (Address == SocRcFuse) return *Address;
    if ((ULONG_PTR)Address >= (ULONG_PTR)SocRegisters &&
        (ULONG_PTR)Address < (ULONG_PTR)(SocRegisters + ARRAYSIZE(SocRegisters))) {
        CHECK(!(SocClock & LVTS_GATE));
        SocReads++;
        ULONG Offset = (ULONG)((ULONG_PTR)Address - (ULONG_PTR)SocRegisters) % LVTS_BANK_BYTES;
        if (Offset >= LVTS_FIRST_SAMPLE && Offset < LVTS_FIRST_SAMPLE + 16) {
            if (LoseSocClockOnSample) SocClock |= LVTS_GATE;
            if (ChangeCalibrationOnSample)
                Address[(LVTS_FIRST_CALIBRATION - LVTS_FIRST_SAMPLE) / 4] ^= 1;
        }
        return *Address;
    }
    CHECK(!(Clock & AUXADC_GATE));
    if (Address == &Registers[AUXADC_MISC / 4]) return *Address;
    ULONG Channel = Address == &Registers[AUXADC_FIRST_SAMPLE / 4] ? 0 : 1;
    CHECK(Address == &Registers[AUXADC_FIRST_SAMPLE / 4 + Channel]);
    CHECK(Registers[AUXADC_MISC / 4] & AUXADC_POWER);
    ChannelReads++;
    return RawCode | (StaleReady || (!MissingReady && (TriggerMask & (1u << Channel))) ? AUXADC_READY : 0);
}

static VOID TestWrite(PULONG Address, ULONG Value) {
    CHECK(!(Clock & AUXADC_GATE));
    Writes++;
    if (Address == &Registers[AUXADC_MISC / 4]) {
        CHECK(((*Address ^ Value) & ~AUXADC_POWER) == 0);
        if (!RejectPower) *Address = Value;
        return;
    }
    CHECK(Value == 1 || Value == 2);
    if (Address == &Registers[AUXADC_TRIGGER_SET / 4]) {
        CHECK(!(TriggerMask & Value));
        TriggerMask |= Value;
        LastTrigger = Value;
    } else {
        CHECK(Address == &Registers[AUXADC_TRIGGER_CLEAR / 4]);
        TriggerMask &= ~Value;
    }
}

static PVOID TestMap(PHYSICAL_ADDRESS Address, SIZE_T Bytes, ULONG Protection) {
    ULONG Index = Maps++ % ResourceCount;
    CHECK(Address.QuadPart == NtcResources[Index].Address && Bytes == NtcResourceLength(Index, DescriptorCount == LvtsRcCalibration));
    CHECK(Protection == (PAGE_NOCACHE | (Index == AdcRegisters ? PAGE_READWRITE : PAGE_READONLY)));
    if (Maps == MapFailure) return NULL;
    PVOID Regions[] = {Registers, &Clock, &Fuse, SocRegisters, &SocClock, SocFuse, SocRcFuse};
    return Regions[Index];
}
static VOID TestUnmap(PVOID Address, SIZE_T Bytes) {
    CHECK(((Address == Registers || Address == SocRegisters) && Bytes == 0x1000) ||
          ((Address == &Clock || Address == &Fuse || Address == &SocClock) && Bytes == 4) ||
          (Address == SocFuse && Bytes == NtcResourceLength(LvtsCalibration, DescriptorCount == LvtsRcCalibration)) ||
          (Address == SocRcFuse && Bytes == sizeof(SocRcFuse)));
    Unmaps++;
}
static ULONGLONG TestTime(PULONGLONG Counter) {
    *Counter = Now;
    return Now;
}
static NTSTATUS TestDelay(KPROCESSOR_MODE Mode, BOOLEAN Alertable, PLARGE_INTEGER Interval) {
    CHECK(Mode == KernelMode && !Alertable && Interval->QuadPart == -10000);
    Now -= Interval->QuadPart;
    if (LoseClockOnWait) Clock |= AUXADC_GATE;
    return STATUS_SUCCESS;
}
static VOID TestStall(ULONG Microseconds) {
    CHECK(Microseconds == 25);
    Now += Microseconds * 10;
}

static PVOID TestContext(PWDF_DRIVER_GLOBALS Globals, WDFOBJECT Object, PCWDF_OBJECT_CONTEXT_TYPE_INFO Type) {
    UNREFERENCED_PARAMETER(Globals);
    if (Object == TEST_REQUEST) {
        CHECK(Type == WDF_GET_CONTEXT_TYPE_INFO(NTC_THERMAL_WAIT));
        return &WaitContext;
    }
    CHECK(Object == TEST_DEVICE && Type == WDF_GET_CONTEXT_TYPE_INFO(NTC_DEVICE));
    return &Device;
}
static ULONG TestCount(PWDF_DRIVER_GLOBALS Globals, WDFCMRESLIST List) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(List == TEST_RAW || List == TEST_TRANSLATED);
    return DescriptorCount;
}
static PCM_PARTIAL_RESOURCE_DESCRIPTOR TestDescriptor(PWDF_DRIVER_GLOBALS Globals, WDFCMRESLIST List, ULONG Index) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK((List == TEST_RAW || List == TEST_TRANSLATED) && Index < ResourceCount);
    return &Descriptors[List == TEST_TRANSLATED][Index];
}
static WDFDEVICE TestQueueDevice(PWDF_DRIVER_GLOBALS Globals, WDFQUEUE Queue) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_QUEUE);
    return TEST_DEVICE;
}
static WDFIOTARGET TestLowerTarget(PWDF_DRIVER_GLOBALS Globals, WDFDEVICE Handle) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Handle == TEST_DEVICE);
    return TEST_LOWER;
}
static NTSTATUS TestAcpi(PWDF_DRIVER_GLOBALS Globals, WDFIOTARGET Target, WDFREQUEST Request,
    ULONG Code, PWDF_MEMORY_DESCRIPTOR Input, PWDF_MEMORY_DESCRIPTOR Output,
    PWDF_REQUEST_SEND_OPTIONS Options, PULONG_PTR Bytes) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Target == TEST_LOWER && !Request && Code == IOCTL_ACPI_EVAL_METHOD && !Output && !Bytes);
    CHECK(Input->Type == WdfMemoryDescriptorTypeBuffer && Input->u.BufferType.Length == sizeof(ACPI_EVAL_INPUT_BUFFER));
    PACPI_EVAL_INPUT_BUFFER Acpi = Input->u.BufferType.Buffer;
    CHECK(Acpi->Signature == ACPI_EVAL_INPUT_BUFFER_SIGNATURE);
    CHECK(Acpi->MethodName[0] == 'L' && Acpi->MethodName[1] == 'V' &&
        Acpi->MethodName[2] == 'O' && Acpi->MethodName[3] == 'N');
    CHECK(Options->Flags == WDF_REQUEST_SEND_OPTION_TIMEOUT && Options->Timeout == WDF_REL_TIMEOUT_IN_SEC(1));
    CHECK(Device.Region[LvtsRcCalibration] && SocClock != MAXULONG && (SocClock & LVTS_GATE));
    RestoreClockCalls++;
    if (RestoreSocClock) SocClock &= ~LVTS_GATE;
    return RestoreClockStatus;
}
static NTSTATUS TestInput(PWDF_DRIVER_GLOBALS Globals, WDFREQUEST Request, size_t Bytes, PVOID *Buffer, size_t *Length) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(!Length);
    if (Bytes == sizeof(THERMAL_WAIT_READ)) {
        CHECK(Request == TEST_REQUEST || Request == TEST_IMMEDIATE);
        *Buffer = &ThermalIo[Request == TEST_IMMEDIATE].Input;
    } else {
        CHECK(Request == TEST_REQUEST && Bytes == sizeof(ULONG));
        *Buffer = &BufferedIo.Channel;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS TestOutput(PWDF_DRIVER_GLOBALS Globals, WDFREQUEST Request, size_t Bytes, PVOID *Buffer, size_t *Length) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(!Length);
    if (Bytes == sizeof(ULONG)) {
        CHECK(Request == TEST_REQUEST || Request == TEST_IMMEDIATE);
        *Buffer = &ThermalIo[Request == TEST_IMMEDIATE].Output;
    } else {
        CHECK(Request == TEST_REQUEST && Bytes == sizeof(T70_NTC_READING));
        *Buffer = &BufferedIo.Reading;
    }
    return STATUS_SUCCESS;
}
static VOID TestComplete(PWDF_DRIVER_GLOBALS Globals, WDFREQUEST Request, NTSTATUS Status, ULONG_PTR Bytes) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK((Request == TEST_REQUEST || Request == TEST_IMMEDIATE) && Information == (ULONG_PTR)-1);
    Completed = Status;
    Information = Bytes;
}

static NTSTATUS TestForward(PWDF_DRIVER_GLOBALS Globals, WDFREQUEST Request, WDFQUEUE Queue) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_WAIT_QUEUE && Request == TEST_REQUEST && !PendingThermal && Device.Online);
    PendingThermal = Request;
    return STATUS_SUCCESS;
}
static WDF_IO_QUEUE_STATE TestWaitState(PWDF_DRIVER_GLOBALS Globals, WDFQUEUE Queue, PULONG Queued, PULONG Owned) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_WAIT_QUEUE && Queued && !Owned);
    *Queued = PendingThermal ? 1 : 0;
    return WdfIoQueueAcceptRequests;
}
static NTSTATUS TestFind(PWDF_DRIVER_GLOBALS Globals, WDFQUEUE Queue, WDFREQUEST Found,
                         WDFFILEOBJECT File, PWDF_REQUEST_PARAMETERS Parameters, WDFREQUEST *Out) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_WAIT_QUEUE && !Found && !File && !Parameters && !Locked);
    if (!PendingThermal) return STATUS_NO_MORE_ENTRIES;
    *Out = PendingThermal;
    QueueRefs++;
    return STATUS_SUCCESS;
}
static NTSTATUS TestRetrieve(PWDF_DRIVER_GLOBALS Globals, WDFQUEUE Queue, WDFREQUEST Found, WDFREQUEST *Out) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_WAIT_QUEUE && Found == PendingThermal && QueueRefs == 1);
    PendingThermal = NULL;
    if (CancelOnRetrieve) return STATUS_NOT_FOUND;
    *Out = Found;
    return STATUS_SUCCESS;
}
static VOID TestDeref(PWDF_DRIVER_GLOBALS Globals, WDFOBJECT Object, PVOID Tag, LONG Line, PCCH Path) {
    UNREFERENCED_PARAMETER(Globals);
    UNREFERENCED_PARAMETER(Tag);
    UNREFERENCED_PARAMETER(Line);
    UNREFERENCED_PARAMETER(Path);
    CHECK(Object == TEST_REQUEST && QueueRefs == 1);
    QueueRefs--;
}
static VOID TestQueueStart(PWDF_DRIVER_GLOBALS Globals, WDFQUEUE Queue) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_WAIT_QUEUE && Device.Online);
    WaitQueueStarts++;
}
static VOID TestQueuePurge(PWDF_DRIVER_GLOBALS Globals, WDFQUEUE Queue) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_WAIT_QUEUE && Device.ThermalStopping && !NextPoll && !Locked);
    PendingThermal = NULL;
    WaitQueuePurges++;
}
static WDFOBJECT TestTimerParent(PWDF_DRIVER_GLOBALS Globals, WDFTIMER Timer) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Timer == TEST_TIMER);
    return TEST_DEVICE;
}
static BOOLEAN TestTimerStart(PWDF_DRIVER_GLOBALS Globals, WDFTIMER Timer, LONGLONG Due) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Timer == TEST_TIMER && Locked && !Device.ThermalStopping && Due < 0);
    BOOLEAN AlreadyStarted = NextPoll != 0;
    NextPoll = Now - Due;
    return AlreadyStarted;
}
static BOOLEAN TestTimerStop(PWDF_DRIVER_GLOBALS Globals, WDFTIMER Timer, BOOLEAN Wait) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Timer == TEST_TIMER && Wait && Device.ThermalStopping && !Locked);
    BOOLEAN WasStarted = NextPoll != 0;
    NextPoll = 0;
    return WasStarted;
}
static VOID TestAcquire(PWDF_DRIVER_GLOBALS Globals, WDFSPINLOCK Lock) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Lock == TEST_LOCK && !Locked);
    Locked = TRUE;
}
static VOID TestRelease(PWDF_DRIVER_GLOBALS Globals, WDFSPINLOCK Lock) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Lock == TEST_LOCK && Locked);
    Locked = FALSE;
}

static VOID ResetFixture(VOID) {
    RtlZeroMemory(&Device, sizeof(Device));
    RtlZeroMemory(Registers, sizeof(Registers));
    RtlZeroMemory(Descriptors, sizeof(Descriptors));
    RtlZeroMemory(SocRegisters, sizeof(SocRegisters));
    RtlZeroMemory(SocFuse, sizeof(SocFuse));
    RtlZeroMemory(SocRcFuse, sizeof(SocRcFuse));
    Clock = Fuse = SocReads = 0;
    SocClock = LVTS_GATE;
    LoseSocClockOnSample = ChangeCalibrationOnSample = FALSE;
    RestoreSocClock = FALSE;
    RestoreClockStatus = STATUS_OBJECT_NAME_NOT_FOUND;
    RestoreClockCalls = 0;
    DescriptorCount = ResourceCount;
    MapFailure = Maps = Unmaps = Writes = ChannelReads = LastTrigger = 0;
    StaleReady = MissingReady = LoseClockOnWait = RejectPower = FALSE;
    Now = 0;
    RawCode = 2048;
    TriggerMask = 4;  // A different consumer's channel is untouched.
    Registers[AUXADC_MISC / 4] = 0x80;
    for (ULONG Index = 0; Index < ResourceCount; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Raw = &Descriptors[0][Index];
        Raw->Type = CmResourceTypeMemory;
        Raw->Flags = NtcResources[Index].Access;
        Raw->u.Memory.Start.QuadPart = NtcResources[Index].Address;
        Raw->u.Memory.Length = NtcResources[Index].Length;
        Descriptors[1][Index] = *Raw;
    }
}
static VOID Request(ULONG Channel, size_t InputBytes, size_t OutputBytes, ULONG Ioctl, NTSTATUS Expected) {
    BufferedIo.Reading.RawCode = Channel;
    BufferedIo.Reading.MilliCelsius = -777;
    BufferedIo.Reading.DeciKelvin = 777;
    Information = (ULONG_PTR)-1;
    NtcIoControl(TEST_QUEUE, TEST_REQUEST, OutputBytes, InputBytes, Ioctl);
    CHECK(Completed == Expected);
    CHECK(Information == (Expected == STATUS_SUCCESS ? sizeof(T70_NTC_READING) : 0));
    if (Expected != STATUS_SUCCESS) {
        CHECK(BufferedIo.Reading.RawCode == Channel && BufferedIo.Reading.MilliCelsius == -777);
        CHECK(BufferedIo.Reading.DeciKelvin == 777);
    }
}
static VOID Read(ULONG Channel, NTSTATUS Expected) {
    Request(Channel, sizeof(ULONG), sizeof(T70_NTC_READING), IOCTL_T70_READ_NTC, Expected);
}

static VOID ReadSoc(ULONG Sensor, NTSTATUS Expected) {
    Request(Sensor, sizeof(ULONG), sizeof(T70_NTC_READING), IOCTL_T70_READ_SOC_TEMP, Expected);
}

static VOID PrimeLvts(BOOLEAN BlankFuses) {
    for (ULONG Index = 0; Index < T70_SOC_SENSOR_COUNT; Index++) {
        ULONG *Bank = SocRegisters + (Index / 4) * (LVTS_BANK_BYTES / 4);
        Bank[0] |= LVTS_MON_ENABLE | (1u << (Index % 4));
        Bank[LVTS_CLOCK_ENABLE / 4] = 1;
        Bank[LVTS_SENSOR_SELECT / 4] = 0x13121110;
        Bank[LVTS_CALIBRATION_SCALE / 4] = 0x300;
        ULONG Trim = BlankFuses ? 0x88b8 : 0xabc + Index;
        Bank[LVTS_FIRST_CALIBRATION / 4 + Index % 4] = Trim;
        if (!BlankFuses) SocFuse[1 + Index / 2] |= Trim << (16 * (Index % 2));
        Bank[LVTS_FIRST_SAMPLE / 4 + Index % 4] = LVTS_SAMPLE_VALID + 16384 - Index;
    }
}

static VOID CheckLvts(VOID) {
    ResetFixture();
    DescriptorCount = NTC_RESOURCE_COUNT;
    CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    Read(0, STATUS_SUCCESS);
    ReadSoc(1, STATUS_NOT_SUPPORTED);
    CHECK(!SocReads && Maps == NTC_RESOURCE_COUNT);
    CHECK(NtcRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == NTC_RESOURCE_COUNT);

    ResetFixture();
    DescriptorCount = LvtsRcCalibration;
    Descriptors[0][LvtsCalibration].u.Memory.Length = sizeof(ULONG);
    Descriptors[1][LvtsCalibration] = Descriptors[0][LvtsCalibration];
    CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    SocClock = 0;
    PrimeLvts(FALSE);
    ReadSoc(1, STATUS_DEVICE_DATA_ERROR); // The old AML contains only the golden byte.
    CHECK(!SocReads);
    SocFuse[0] = 62u << 24;
    ReadSoc(1, STATUS_SUCCESS);
    CHECK(BufferedIo.Reading.MilliCelsius == 31000);
    CHECK(NtcRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == LvtsRcCalibration);

    ResetFixture();
    CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    ULONG BaselineWrites = Writes;
    ReadSoc(T70_SOC_SENSOR_COUNT + 1, STATUS_INVALID_PARAMETER);
    ReadSoc(1, STATUS_DEVICE_NOT_READY); // Gated before any bank MMIO.
    CHECK(!SocReads);
    SocClock = 0;
    ReadSoc(1, STATUS_DEVICE_NOT_READY); // Calibration alone does not start the sensor.
    PrimeLvts(TRUE);
    SocFuse[0] = 0x1234;        // An unused part of the golden word.
    SocFuse[7] = 0x43210000;    // There is no 14th sensor.
    SocRcFuse[0] = 0x80000000;  // An unused byte of the RC word.
    ReadSoc(1, STATUS_SUCCESS);
    CHECK(BufferedIo.Reading.MilliCelsius == 30000); // Stock blank-eFuse default.
    SocRcFuse[0] |= 1;
    ReadSoc(1, STATUS_DEVICE_DATA_ERROR); // Partly programmed fuses cannot use the defaults.
    SocRcFuse[0] = MAXULONG;
    ReadSoc(1, STATUS_DEVICE_DATA_ERROR);
    SocRcFuse[0] = 0;
    PrimeLvts(FALSE);
    ReadSoc(1, STATUS_SUCCESS);
    CHECK(BufferedIo.Reading.MilliCelsius == 0); // Programmed R keeps golden 0.
    SocFuse[0] = 62u << 24;
    for (ULONG Id = 1; Id <= T70_SOC_SENSOR_COUNT; Id++) {
        ReadSoc(Id, STATUS_SUCCESS);
        CHECK(BufferedIo.Reading.RawCode == 16385 - Id);
        CHECK(BufferedIo.Reading.MilliCelsius == 31000 + (250460 * (Id - 1)) / 16384);
    }
    ReadSoc(0, STATUS_SUCCESS);
    CHECK(BufferedIo.Reading.RawCode == 16372 && BufferedIo.Reading.MilliCelsius == 31183);
    Read(1, STATUS_SUCCESS);
    CHECK(Writes == BaselineWrites + 3);
    BaselineWrites = Writes;

    SocRegisters[LVTS_FIRST_SAMPLE / 4 + 1] &= ~LVTS_SAMPLE_VALID;
    ReadSoc(0, STATUS_DEVICE_NOT_READY); // No partial maximum.
    SocRegisters[LVTS_FIRST_SAMPLE / 4 + 1] = LVTS_SAMPLE_VALID;
    ReadSoc(2, STATUS_DEVICE_DATA_ERROR);
    SocRegisters[LVTS_FIRST_CALIBRATION / 4] = 0;
    ReadSoc(1, STATUS_DEVICE_DATA_ERROR);
    SocRegisters[LVTS_FIRST_CALIBRATION / 4] = 0xabd; // Valid trim for a different slot.
    ReadSoc(1, STATUS_DEVICE_DATA_ERROR);
    SocRegisters[LVTS_FIRST_CALIBRATION / 4] = 0xabc;
    ChangeCalibrationOnSample = TRUE;
    ReadSoc(1, STATUS_DEVICE_NOT_READY);
    ChangeCalibrationOnSample = FALSE;
    SocRegisters[LVTS_FIRST_CALIBRATION / 4] = 0xabc;
    LoseSocClockOnSample = TRUE;
    ReadSoc(1, STATUS_DEVICE_NOT_READY);
    CHECK(SocClock & LVTS_GATE);
    CHECK(Writes == BaselineWrites);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    ReadSoc(1, STATUS_DEVICE_NOT_READY);
    CHECK(NtcRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == ResourceCount);
}

static VOID CheckLvtsClockResume(VOID) {
    ResetFixture();
    SocClock = 1u << 9;
    SocFuse[0] = 62u << 24;
    PrimeLvts(FALSE);
    CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && !RestoreClockCalls);
    ReadSoc(0, STATUS_SUCCESS);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);

    SocClock |= LVTS_GATE;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && RestoreClockCalls == 1);
    ULONG Baseline = SocReads;
    ReadSoc(0, STATUS_DEVICE_NOT_READY); // Older AML has no LVON.
    CHECK(SocReads == Baseline);
    Read(0, STATUS_SUCCESS);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);

    RestoreClockStatus = STATUS_SUCCESS;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && RestoreClockCalls == 2);
    ReadSoc(0, STATUS_DEVICE_NOT_READY); // Method succeeded but the gate stayed closed.
    CHECK(SocReads == Baseline);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);

    RestoreSocClock = TRUE;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && RestoreClockCalls == 3);
    CHECK(SocClock == (1u << 9));
    ReadSoc(0, STATUS_SUCCESS);
    SocRegisters[0] = 0;
    ReadSoc(0, STATUS_DEVICE_NOT_READY); // Controller state did not survive.
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);

    SocClock = MAXULONG;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && RestoreClockCalls == 3);
    ReadSoc(0, STATUS_DEVICE_NOT_READY);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    CHECK(NtcRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == ResourceCount);
}

static VOID ThermalRequest(WDFREQUEST Request, ULONG Timeout, ULONG Low, ULONG High) {
    ThermalIo[Request == TEST_IMMEDIATE].Input = (THERMAL_WAIT_READ){Timeout, Low, High};
    Information = (ULONG_PTR)-1;
    NtcIoControl(TEST_QUEUE, Request, sizeof(ULONG), sizeof(THERMAL_WAIT_READ), IOCTL_THERMAL_READ_TEMPERATURE);
}
static VOID FireThermalPoll(VOID) {
    CHECK(NextPoll && Now <= NextPoll);
    Now = NextPoll;
    NextPoll = 0;
    NtcThermalPoll(TEST_TIMER);
    CHECK(!QueueRefs && !Locked);
}

static VOID CheckThermalWait(VOID) {
    ResetFixture();
    Device.ThermalWaitQueue = TEST_WAIT_QUEUE;
    Device.ThermalTimer = TEST_TIMER;
    Device.ThermalTimerLock = TEST_LOCK;
    Device.ThermalStopping = TRUE;
    CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && WaitQueueStarts == 1);
    SocClock = 0;
    SocFuse[0] = 62u << 24;
    PrimeLvts(FALSE);

    ThermalRequest(TEST_IMMEDIATE, 0, 0, MAXULONG);
    CHECK(Completed == STATUS_SUCCESS && Information == sizeof(ULONG) && !NextPoll);
    ULONG Temperature = ThermalIo[1].Output;
    CHECK(Temperature == 3043);
    ThermalRequest(TEST_IMMEDIATE, 0, 2, 1);
    CHECK(Completed == STATUS_INVALID_PARAMETER && Information == 0 && !NextPoll);
    Information = (ULONG_PTR)-1;
    NtcIoControl(TEST_QUEUE, TEST_IMMEDIATE, 3, sizeof(THERMAL_WAIT_READ), IOCTL_THERMAL_READ_TEMPERATURE);
    CHECK(Completed == STATUS_BUFFER_TOO_SMALL && Information == 0);

    ThermalRequest(TEST_REQUEST, 350, Temperature, Temperature + 5);
    CHECK(PendingThermal == TEST_REQUEST && Information == (ULONG_PTR)-1 && NextPoll == Now + THERMAL_POLL_100NS);
    ThermalRequest(TEST_IMMEDIATE, 0, 0, MAXULONG);
    CHECK(Completed == STATUS_SUCCESS && ThermalIo[1].Output == Temperature && PendingThermal);
    ThermalRequest(TEST_IMMEDIATE, 100, 0, MAXULONG);
    CHECK(Completed == STATUS_DEVICE_BUSY && Information == 0 && PendingThermal);
    Information = (ULONG_PTR)-1;
    FireThermalPoll();
    CHECK(Information == (ULONG_PTR)-1 && PendingThermal && NextPoll == Now + THERMAL_POLL_100NS);
    SocRegisters[LVTS_FIRST_SAMPLE / 4] = LVTS_SAMPLE_VALID + 15872;
    FireThermalPoll();
    CHECK(Completed == STATUS_SUCCESS && Information == sizeof(ULONG) && !PendingThermal && !NextPoll);
    CHECK(ThermalIo[0].Output > Temperature + 5);

    ThermalRequest(TEST_REQUEST, 13, 0, MAXULONG);
    CHECK(PendingThermal && NextPoll == Now + 130000);
    FireThermalPoll();
    CHECK(Completed == STATUS_SUCCESS && Information == sizeof(ULONG) && !PendingThermal && !NextPoll);
    ThermalRequest(TEST_IMMEDIATE, THERMAL_WAIT_READ_TIMEOUT_NONE, Temperature + 200, MAXULONG);
    CHECK(Completed == STATUS_SUCCESS && Information == sizeof(ULONG) && !PendingThermal && !NextPoll);

    ThermalRequest(TEST_REQUEST, THERMAL_WAIT_READ_TIMEOUT_NONE, 0, MAXULONG);
    SocClock = LVTS_GATE;
    CancelOnRetrieve = TRUE;
    FireThermalPoll();
    CHECK(Information == (ULONG_PTR)-1 && !NextPoll && !PendingThermal);
    CancelOnRetrieve = FALSE;
    SocClock = 0;

    ThermalRequest(TEST_REQUEST, THERMAL_WAIT_READ_TIMEOUT_NONE, 0, MAXULONG);
    SocClock = LVTS_GATE;
    FireThermalPoll();
    CHECK(Completed == STATUS_DEVICE_NOT_READY && Information == 0 && !NextPoll && !PendingThermal);
    SocClock = 0;

    ThermalRequest(TEST_REQUEST, THERMAL_WAIT_READ_TIMEOUT_NONE, 0, MAXULONG);
    CHECK(WaitContext.Deadline == MAXULONGLONG && PendingThermal && NextPoll);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && !Device.Online);
    CHECK(WaitQueuePurges == 1 && !PendingThermal && !NextPoll && Information == (ULONG_PTR)-1);
    ULONG ReadsBefore = SocReads;
    NtcThermalPoll(TEST_TIMER);
    CHECK(SocReads == ReadsBefore && !NextPoll);
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && WaitQueueStarts == 2);
    ThermalRequest(TEST_IMMEDIATE, 0, 0, MAXULONG);
    CHECK(Completed == STATUS_SUCCESS && Information == sizeof(ULONG) && !NextPoll);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS && WaitQueuePurges == 2);
    CHECK(NtcRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == ResourceCount);
}

int TestMain(VOID) {
    TestWdf[WdfObjectGetTypedContextWorkerTableIndex] = (WDFFUNC)TestContext;
    TestWdf[WdfCmResourceListGetCountTableIndex] = (WDFFUNC)TestCount;
    TestWdf[WdfCmResourceListGetDescriptorTableIndex] = (WDFFUNC)TestDescriptor;
    TestWdf[WdfIoQueueGetDeviceTableIndex] = (WDFFUNC)TestQueueDevice;
    TestWdf[WdfDeviceGetIoTargetTableIndex] = (WDFFUNC)TestLowerTarget;
    TestWdf[WdfIoTargetSendIoctlSynchronouslyTableIndex] = (WDFFUNC)TestAcpi;
    TestWdf[WdfRequestRetrieveInputBufferTableIndex] = (WDFFUNC)TestInput;
    TestWdf[WdfRequestRetrieveOutputBufferTableIndex] = (WDFFUNC)TestOutput;
    TestWdf[WdfRequestCompleteWithInformationTableIndex] = (WDFFUNC)TestComplete;
    TestWdf[WdfRequestForwardToIoQueueTableIndex] = (WDFFUNC)TestForward;
    TestWdf[WdfIoQueueGetStateTableIndex] = (WDFFUNC)TestWaitState;
    TestWdf[WdfIoQueueFindRequestTableIndex] = (WDFFUNC)TestFind;
    TestWdf[WdfIoQueueRetrieveFoundRequestTableIndex] = (WDFFUNC)TestRetrieve;
    TestWdf[WdfObjectDereferenceActualTableIndex] = (WDFFUNC)TestDeref;
    TestWdf[WdfIoQueueStartTableIndex] = (WDFFUNC)TestQueueStart;
    TestWdf[WdfIoQueuePurgeSynchronouslyTableIndex] = (WDFFUNC)TestQueuePurge;
    TestWdf[WdfTimerGetParentObjectTableIndex] = (WDFFUNC)TestTimerParent;
    TestWdf[WdfTimerStartTableIndex] = (WDFFUNC)TestTimerStart;
    TestWdf[WdfTimerStopTableIndex] = (WDFFUNC)TestTimerStop;
    TestWdf[WdfSpinLockAcquireTableIndex] = (WDFFUNC)TestAcquire;
    TestWdf[WdfSpinLockReleaseTableIndex] = (WDFFUNC)TestRelease;

    for (ULONG Index = 0; Index < ResourceCount; Index++) {
        ResetFixture();
        Descriptors[1][Index].u.Memory.Start.QuadPart++;
        CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_DEVICE_CONFIGURATION_ERROR);
        CHECK(!Maps && !Writes);
        ResetFixture();
        MapFailure = Index + 1;
        CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_INSUFFICIENT_RESOURCES);
        CHECK(Unmaps == Index && !Writes);
    }

    ResetFixture();
    CHECK(NtcPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    Read(0, STATUS_DEVICE_NOT_READY);
    Clock = AUXADC_GATE;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_NOT_READY && !Writes);
    Clock = 0;
    Fuse = MAXULONG;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_DATA_ERROR && !Writes);
    Fuse = 0;
    RejectPower = TRUE;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_DEVICE_POWER_FAILURE && !Device.Online);
    CHECK(Registers[AUXADC_MISC / 4] == 0x80);
    RejectPower = FALSE;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    ULONG BaselineWrites = Writes;
    Request(0, 0, sizeof(T70_NTC_READING), IOCTL_T70_READ_NTC, STATUS_INVALID_PARAMETER);
    Request(0, sizeof(ULONG), 4, IOCTL_T70_READ_NTC, STATUS_BUFFER_TOO_SMALL);
    Request(0, sizeof(ULONG), sizeof(T70_NTC_READING), 0, STATUS_INVALID_DEVICE_REQUEST);
    Read(2, STATUS_INVALID_PARAMETER);
    CHECK(Writes == BaselineWrites);

    Read(0, STATUS_SUCCESS);
    CHECK(BufferedIo.Reading.RawCode == 2048 && BufferedIo.Reading.MilliCelsius == 32263);
    CHECK(BufferedIo.Reading.DeciKelvin == 3054 && LastTrigger == 1 && TriggerMask == 4);
    CHECK(Writes == BaselineWrites + 3 && ChannelReads == 2);
    StaleReady = TRUE;
    Read(1, STATUS_IO_TIMEOUT);
    CHECK(LastTrigger == 1 && TriggerMask == 4);  // No trigger until stale ready clears.
    StaleReady = FALSE;
    MissingReady = TRUE;
    Read(1, STATUS_IO_TIMEOUT);
    CHECK(LastTrigger == 2 && TriggerMask == 4);  // Timeout still drops our trigger.
    MissingReady = FALSE;
    RawCode = 0;
    Read(1, STATUS_DEVICE_DATA_ERROR);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    CHECK(Registers[AUXADC_MISC / 4] == 0x80 && !Device.Online);
    Read(1, STATUS_DEVICE_NOT_READY);

    Fuse = (1u << 20) | (462u << 10) | 540u;  // Gain -50, offset +28.
    RawCode = 2560;
    for (ULONG Cycle = 0; Cycle < 8; Cycle++) {
        CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
        Read(Cycle & 1, STATUS_SUCCESS);
        CHECK(BufferedIo.Reading.RawCode == 2560 && BufferedIo.Reading.MilliCelsius == 23227);
        CHECK(BufferedIo.Reading.DeciKelvin == 2964 && LastTrigger == (1u << (Cycle & 1)));
        CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
        CHECK(Registers[AUXADC_MISC / 4] == 0x80);
    }
    Registers[AUXADC_MISC / 4] |= AUXADC_POWER;
    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    CHECK(NtcD0Exit(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    CHECK(Registers[AUXADC_MISC / 4] == (0x80 | AUXADC_POWER)); // Inherited power stays on.

    CHECK(NtcD0Entry(TEST_DEVICE, WdfPowerDeviceD3) == STATUS_SUCCESS);
    MissingReady = LoseClockOnWait = TRUE;
    BaselineWrites = Writes;
    Read(0, STATUS_DEVICE_POWER_FAILURE);
    CHECK(Writes == BaselineWrites + 2);  // Gate loss prevents even a cleanup MMIO write.
    CHECK(NtcRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == ResourceCount);
    Read(0, STATUS_DEVICE_NOT_READY);
    CHECK(Writes == BaselineWrites + 2);

    CheckLvts();
    CheckLvtsClockResume();
    CheckThermalWait();
    return Failure;
}
