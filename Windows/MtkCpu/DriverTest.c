// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>

static ULONG TestRead(PULONG Address);
static VOID TestWrite(PULONG Address, ULONG Value);
static PVOID TestMap(PHYSICAL_ADDRESS Address, SIZE_T Bytes, ULONG Protection);
static VOID TestUnmap(PVOID Address, SIZE_T Bytes);
static ULONG SpinLockCalls;
static VOID TestInitializeSpinLock(PKSPIN_LOCK Lock) { *Lock = 0; }
static VOID TestAcquireSpinLock(PKSPIN_LOCK Lock, PKIRQL OldIrql) {
    UNREFERENCED_PARAMETER(Lock); ++SpinLockCalls; *OldIrql = 0;
}
static VOID TestReleaseSpinLock(PKSPIN_LOCK Lock, KIRQL OldIrql) {
    UNREFERENCED_PARAMETER(Lock); UNREFERENCED_PARAMETER(OldIrql); ++SpinLockCalls;
}

#undef READ_REGISTER_ULONG
#undef WRITE_REGISTER_ULONG
#undef KeInitializeSpinLock
#undef KeAcquireSpinLock
#undef KeReleaseSpinLock
#define READ_REGISTER_ULONG TestRead
#define WRITE_REGISTER_ULONG TestWrite
#define MmMapIoSpaceEx TestMap
#define MmUnmapIoSpace TestUnmap
#define KeInitializeSpinLock TestInitializeSpinLock
#define KeAcquireSpinLock TestAcquireSpinLock
#define KeReleaseSpinLock TestReleaseSpinLock
#include "Driver.c"

#define TEST_DEVICE ((WDFDEVICE)(ULONG_PTR)0x100)
#define TEST_QUEUE ((WDFQUEUE)(ULONG_PTR)0x101)
#define TEST_REQUEST ((WDFREQUEST)(ULONG_PTR)0x102)
#define TEST_RAW ((WDFCMRESLIST)(ULONG_PTR)0x103)
#define TEST_TRANSLATED ((WDFCMRESLIST)(ULONG_PTR)0x104)
#define CHECK(Condition) do { ++MtkCpuAssertionCount; if (!(Condition) && !Failure) Failure = __LINE__; } while (0)

static WDFFUNC TestWdf[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions = TestWdf;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
static CPU_DEVICE Device;
static CM_PARTIAL_RESOURCE_DESCRIPTOR Descriptors[2][ARRAYSIZE(CpuResources)];
static ULONG Registers[T70_CPU_DOMAIN_COUNT][0x120 / sizeof(ULONG)];
static ULONG Version, ScalingMode;
static union { ULONG Domain; T70_CPU_READING Reading; T70_CPU_TARGET Target; } Buffer;
static ULONG Maps, Unmaps, MapFailure, Reads, Count, Writes;
static ULONG WorkerRequests;
static POHANDLE LastWorkerHandle;
static BOOLEAN ChangePerformance, ChangeVersion, LoseReady, ChangeMode, IgnoreWrite, LoseReadyOnWrite;
static NTSTATUS CompletionStatus, BufferStatus;
static ULONG_PTR Completed;
__declspec(dllexport) volatile ULONG MtkCpuAssertionCount;
static int Failure;

static ULONG TestRead(PULONG Address) {
    ++Reads;
    if (Address == &Version) {
        CHECK(Device.Region[CPU_VERSION_RESOURCE] == &Version);
        return Version;
    }
    if (Address == &ScalingMode) {
        CHECK(Device.Region[CPU_MODE_RESOURCE] == &ScalingMode);
        return ScalingMode;
    }
    for (ULONG Domain = 0; Domain < T70_CPU_DOMAIN_COUNT; ++Domain) {
        ULONG_PTR Offset = (ULONG_PTR)Address - (ULONG_PTR)Registers[Domain];
        if (Offset >= sizeof(Registers[Domain])) continue;
        CHECK(Device.Region[Domain] == Registers[Domain]);
        CHECK(Offset < T70_CPU_MAX_OPPS * sizeof(ULONG) ||
              Offset == CPU_ENABLE || Offset == CPU_PERFORMANCE || Offset == CPU_STATUS);
        ULONG Value = *Address;
        if (Offset == CPU_PERFORMANCE && ChangePerformance) ++*Address;
        if (Offset == CPU_PERFORMANCE && ChangeVersion) Version = 1;
        if (Offset == CPU_PERFORMANCE && ChangeMode) ScalingMode = 1;
        if (Offset == CPU_PERFORMANCE && LoseReady) Registers[Domain][CPU_STATUS / 4] = 0;
        return Value;
    }
    CHECK(FALSE);
    return MAXULONG;
}

static VOID TestWrite(PULONG Address, ULONG Value) {
    Writes++;
    for (ULONG Domain = 0; Domain < T70_CPU_DOMAIN_COUNT; ++Domain) {
        if (Address != &Registers[Domain][CPU_PERFORMANCE / 4]) continue;
        CHECK(!ScalingMode && Registers[Domain][CPU_ENABLE / 4] == 1);
        CHECK(Registers[Domain][CPU_STATUS / 4] == 3 && Value < 2);
        if (!IgnoreWrite) *Address = Value;
        if (LoseReadyOnWrite) Registers[Domain][CPU_STATUS / 4] = 0;
        return;
    }
    CHECK(FALSE);
}

static PVOID TestMap(PHYSICAL_ADDRESS Address, SIZE_T Bytes, ULONG Protection) {
    ULONG Index = Maps++;
    CHECK(Index < ARRAYSIZE(CpuResources));
    CHECK(Protection == (PAGE_NOCACHE | (Index < T70_CPU_DOMAIN_COUNT ? PAGE_READWRITE : PAGE_READONLY)));
    CHECK(Address.QuadPart == CpuResources[Index].Address && Bytes == CpuResources[Index].Length);
    if (Maps == MapFailure) return NULL;
    PULONG Regions[] = {Registers[0], Registers[1], &Version, &ScalingMode};
    return Regions[Index];
}

static VOID TestUnmap(PVOID Address, SIZE_T Bytes) {
    ULONG Index = Unmaps++;
    CHECK(Index < ARRAYSIZE(CpuResources) && Bytes == CpuResources[Index].Length);
    PULONG Regions[] = {Registers[0], Registers[1], &Version, &ScalingMode};
    CHECK(Address == Regions[Index]);
}

static PVOID TestContext(PWDF_DRIVER_GLOBALS Globals, WDFOBJECT Object, PCWDF_OBJECT_CONTEXT_TYPE_INFO Type) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Object == TEST_DEVICE && Type == WDF_GET_CONTEXT_TYPE_INFO(CPU_DEVICE));
    return &Device;
}
static ULONG TestCount(PWDF_DRIVER_GLOBALS Globals, WDFCMRESLIST List) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(List == TEST_RAW || List == TEST_TRANSLATED);
    return Count;
}
static PCM_PARTIAL_RESOURCE_DESCRIPTOR TestDescriptor(PWDF_DRIVER_GLOBALS Globals, WDFCMRESLIST List, ULONG Index) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK((List == TEST_RAW || List == TEST_TRANSLATED) && Index < Count);
    return &Descriptors[List == TEST_TRANSLATED][Index];
}
static WDFDEVICE TestQueueDevice(PWDF_DRIVER_GLOBALS Globals, WDFQUEUE Queue) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Queue == TEST_QUEUE);
    return TEST_DEVICE;
}
static NTSTATUS TestInput(PWDF_DRIVER_GLOBALS Globals, WDFREQUEST Request, size_t Bytes, PVOID *Address, size_t *Length) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Request == TEST_REQUEST && (Bytes == sizeof(ULONG) || Bytes == sizeof(T70_CPU_TARGET)) && !Length);
    *Address = &Buffer;
    return BufferStatus;
}
static NTSTATUS TestOutput(PWDF_DRIVER_GLOBALS Globals, WDFREQUEST Request, size_t Bytes, PVOID *Address, size_t *Length) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Request == TEST_REQUEST && Bytes == sizeof(T70_CPU_READING) && !Length);
    *Address = &Buffer; // METHOD_BUFFERED can reuse the input buffer.
    return BufferStatus;
}
static VOID TestComplete(PWDF_DRIVER_GLOBALS Globals, WDFREQUEST Request, NTSTATUS Status, ULONG_PTR Information) {
    UNREFERENCED_PARAMETER(Globals);
    CHECK(Request == TEST_REQUEST && CompletionStatus == STATUS_PENDING);
    CompletionStatus = Status;
    Completed = Information;
}
static VOID TestRequestWorker(POHANDLE Handle) {
    ++WorkerRequests;
    LastWorkerHandle = Handle;
}

static VOID ResetFixture(VOID) {
    CpuPpmDevice = NULL;
    CpuInitializePpm();
    RtlZeroMemory(&Device, sizeof(Device));
    RtlZeroMemory(Registers, sizeof(Registers));
    RtlZeroMemory(Descriptors, sizeof(Descriptors));
    Maps = Unmaps = MapFailure = Reads = Version = ScalingMode = Writes = 0;
    WorkerRequests = 0;
    LastWorkerHandle = NULL;
    ChangePerformance = ChangeVersion = LoseReady = ChangeMode = IgnoreWrite = LoseReadyOnWrite = FALSE;
    BufferStatus = STATUS_SUCCESS;
    Count = ARRAYSIZE(CpuResources);
    for (ULONG Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource = &Descriptors[0][Index];
        Resource->Type = CmResourceTypeMemory;
        Resource->Flags = Index < T70_CPU_DOMAIN_COUNT ? CM_RESOURCE_MEMORY_READ_WRITE : CM_RESOURCE_MEMORY_READ_ONLY;
        Resource->u.Memory.Start.QuadPart = CpuResources[Index].Address;
        Resource->u.Memory.Length = CpuResources[Index].Length;
        Descriptors[1][Index] = *Resource;
    }
    for (ULONG Domain = 0; Domain < T70_CPU_DOMAIN_COUNT; ++Domain) {
        Registers[Domain][CPU_ENABLE / 4] = 1;
        Registers[Domain][CPU_STATUS / 4] = 3;
        Registers[Domain][0] = 2000 + 200 * Domain;
        Registers[Domain][1] = 1500 + 200 * Domain;
        Registers[Domain][2] = Registers[Domain][1];
        Registers[Domain][CPU_PERFORMANCE / 4] = 1;
    }
}

static VOID Request(ULONG Domain, ULONG Code, size_t In, size_t Out, NTSTATUS Expected) {
    RtlFillMemory(&Buffer, sizeof(Buffer), 0x5a);
    Buffer.Domain = Domain;
    CompletionStatus = STATUS_PENDING;
    Completed = MAXULONG_PTR;
    CpuIoControl(TEST_QUEUE, TEST_REQUEST, Out, In, Code);
    CHECK(CompletionStatus == Expected);
    CHECK(Completed == (Expected == STATUS_SUCCESS ? sizeof(T70_CPU_READING) : 0));
    if (Expected != STATUS_SUCCESS)
        CHECK(Buffer.Domain == Domain && Buffer.Reading.RequestedKhz == 0x5a5a5a5a);
}
static VOID Query(ULONG Domain, NTSTATUS Expected) {
    Request(Domain, IOCTL_T70_CPU_QUERY_DOMAIN, sizeof(ULONG), sizeof(T70_CPU_READING), Expected);
}

static VOID SetOpp(ULONG Domain, ULONG Khz, NTSTATUS Expected) {
    Buffer.Target = (T70_CPU_TARGET){Domain, Khz};
    CompletionStatus = STATUS_PENDING;
    Completed = MAXULONG_PTR;
    CpuIoControl(TEST_QUEUE, TEST_REQUEST, 0, sizeof(T70_CPU_TARGET), IOCTL_T70_CPU_SET_OPP);
    CHECK(CompletionStatus == Expected && Completed == 0);
    CHECK(Buffer.Target.Domain == Domain && Buffer.Target.FrequencyKhz == Khz);
}

static VOID CheckTargets(VOID) {
    ResetFixture();
    SetOpp(0, 1500000, STATUS_DEVICE_NOT_READY);
    CHECK(!Writes && !Reads);
    Descriptors[0][CPU_VERSION_RESOURCE].u.Memory.Start.QuadPart = 0x0011c7fc;
    CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(!Maps);
    Descriptors[0][CPU_VERSION_RESOURCE].u.Memory.Start.QuadPart = 0x00114ffc;
    CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    Request(0, IOCTL_T70_CPU_SET_OPP, sizeof(ULONG), 0, STATUS_INVALID_PARAMETER);
    Request(0, IOCTL_T70_CPU_SET_OPP, sizeof(T70_CPU_TARGET), 4, STATUS_INVALID_PARAMETER);
    SetOpp(2, 1500000, STATUS_INVALID_PARAMETER);
    SetOpp(0, 0, STATUS_INVALID_PARAMETER);
    SetOpp(0, 1700000, STATUS_INVALID_PARAMETER);
    SetOpp(0, 1500000, STATUS_SUCCESS);
    CHECK(!Writes);

    SetOpp(0, 2000000, STATUS_SUCCESS);
    CHECK(Writes == 1 && Registers[0][CPU_PERFORMANCE / 4] == 0);
    CHECK(Registers[1][CPU_PERFORMANCE / 4] == 1);
    SetOpp(1, 2200000, STATUS_SUCCESS);
    CHECK(Writes == 2 && Registers[1][CPU_PERFORMANCE / 4] == 0);
    Query(1, STATUS_SUCCESS);
    CHECK(Buffer.Reading.RequestedKhz == 2200000);

    ScalingMode = 1;
    SetOpp(1, 1700000, STATUS_NOT_SUPPORTED);
    ScalingMode = 0;
    ChangeMode = TRUE;
    SetOpp(1, 1700000, STATUS_RETRY);
    ChangeMode = FALSE;
    ScalingMode = 0;
    Registers[0][CPU_PERFORMANCE / 4] = 1800000;
    SetOpp(0, 1500000, STATUS_NOT_SUPPORTED);
    Registers[0][CPU_PERFORMANCE / 4] = 0;
    Version = 3;
    SetOpp(0, 1500000, STATUS_DEVICE_NOT_READY);
    Version = 2;
    ChangePerformance = TRUE;
    SetOpp(0, 1500000, STATUS_RETRY);
    CHECK(Writes == 2);
    ChangePerformance = FALSE;
    Registers[0][CPU_PERFORMANCE / 4] = 0;

    IgnoreWrite = TRUE;
    SetOpp(0, 1500000, STATUS_IO_DEVICE_ERROR);
    CHECK(Writes == 3 && Registers[0][CPU_PERFORMANCE / 4] == 0);
    IgnoreWrite = FALSE;
    LoseReadyOnWrite = TRUE;
    SetOpp(0, 1500000, STATUS_DEVICE_POWER_FAILURE);
    SetOpp(0, 2000000, STATUS_DEVICE_NOT_READY);
    CHECK(Writes == 4);
    LoseReadyOnWrite = FALSE;
    Registers[0][CPU_STATUS / 4] = 3;
    Query(0, STATUS_SUCCESS);
    CHECK(Buffer.Reading.ControllerValue == 1 && Buffer.Reading.RequestedKhz == 1500000);
    CHECK(CpuRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == Count);
    SetOpp(0, 1500000, STATUS_DEVICE_NOT_READY);
    CHECK(Writes == 4);
}

static VOID TestPpm(VOID) {
    static WCHAR Cpu6Id[] = {0x0041,0x0043,0x0050,0x0049,0x005c,0x0041,0x0043,0x0050,
        0x0049,0x0030,0x0030,0x0030,0x0037,0x005c,0x0036};
    static WCHAR BadCpuId[] = {0x0041,0x0043,0x0050,0x0049,0x005c,0x0041,0x0043,0x0050,
        0x0049,0x0030,0x0030,0x0030,0x0037,0x005c,0x0038};
    UNICODE_STRING Id = {sizeof(Cpu6Id), sizeof(Cpu6Id), Cpu6Id};
    UNICODE_STRING BadId = {sizeof(BadCpuId), sizeof(BadCpuId), BadCpuId};
    PEP_PREPARE_DEVICE Prepare = {&Id, FALSE};
    PEP_PREPARE_DEVICE BadPrepare = {&BadId, FALSE};
    PEP_REGISTER_DEVICE Register = {0};
    PEP_DEVICE_STARTED Started = {0};
    PEP_DEVICE_POWER_STATE Power = {0};
    PEP_PPM_ENTER_SYSTEM_STATE Enter = {0};
    PEP_PPM_RESUME_FROM_SYSTEM_STATE Resume = {0};
    PEP_PPM_QUERY_CAPABILITIES Caps = {0};
    PEP_PPM_QUERY_PERF_CAPABILITIES Perf = {0};
    PEP_WORK Work = {0};
    PEP_POWER_CONTROL_COMPLETE WorkComplete = {0};
    PEP_PPM_PERF_SET Set = {0};
    ULONG BeforeWrites;

    ResetFixture();
    CHECK(CpuDpmNotify(PEP_DPM_PREPARE_DEVICE, &Prepare) && Prepare.DeviceAccepted);
    CHECK(CpuDpmNotify(PEP_DPM_PREPARE_DEVICE, &BadPrepare) && !BadPrepare.DeviceAccepted);
    Register.DeviceId = &Id;
    Register.KernelHandle = (POHANDLE)(ULONG_PTR)0x777;
    Register.DeviceAccepted = PepDeviceNotAccepted;
    CHECK(CpuDpmNotify(PEP_DPM_REGISTER_DEVICE, &Register));
    CHECK(Register.DeviceAccepted == PepDeviceAccepted &&
        Register.DeviceHandle == (PEPHANDLE)&CpuPpmProcessors[6]);
    /* Capability flags must be available before device MMIO preparation. */
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_QUERY_CAPABILITIES, &Caps));
    CHECK(Caps.PerformanceStatesSupported && !Caps.DiscretePerformanceStateCount);
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES, &Perf));
    CHECK(Perf.DomainId == 1 && Perf.DomainMembers == 2 &&
          Perf.HighestPerformance == CPU_PPM_EARLY_HIGH_BIG &&
          Perf.NominalPerformance == CPU_PPM_EARLY_HIGH_BIG &&
          Perf.LowestNonlinearPerformance == CPU_PPM_EARLY_LOW_BIG &&
          Perf.LowestPerformance == CPU_PPM_EARLY_LOW_BIG);
    CpuPpmKernelInformation.RequestWorker = TestRequestWorker;
    CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS);
    CHECK(WorkerRequests == 0);
    Started.DeviceHandle = Register.DeviceHandle;
    CHECK(CpuDpmNotify(PEP_DPM_DEVICE_STARTED, &Started));
    CHECK(WorkerRequests == 0);
    Power.DeviceHandle = Register.DeviceHandle;
    Power.PowerState = PowerDeviceD0;
    Power.Complete = TRUE;
    CHECK(CpuDpmNotify(PEP_DPM_DEVICE_POWER_STATE, &Power));
    CHECK(WorkerRequests == 1 && LastWorkerHandle == Register.KernelHandle);
    CHECK(CpuDpmNotify(PEP_DPM_WORK, &Work) && Work.NeedWork && Work.WorkInformation);
    CHECK(Work.WorkInformation->WorkType == PepWorkRequestPowerControl &&
          Work.WorkInformation->PowerControl.DeviceHandle == Register.KernelHandle &&
          Work.WorkInformation->PowerControl.PowerControlCode == &CpuPpmPerfConstraintChange &&
          Work.WorkInformation->PowerControl.InBuffer == NULL &&
          Work.WorkInformation->PowerControl.OutBuffer == NULL);
    WorkComplete.DeviceHandle = Register.DeviceHandle;
    WorkComplete.PowerControlCode = &CpuPpmPerfConstraintChange;
    WorkComplete.RequestContext = Work.WorkInformation->PowerControl.RequestContext;
    WorkComplete.Status = STATUS_SUCCESS;
    CHECK(CpuDpmNotify(PEP_DPM_POWER_CONTROL_COMPLETE, &WorkComplete));
    CHECK(WorkerRequests == 1);
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_QUERY_CAPABILITIES, &Caps));
    CHECK(Caps.PerformanceStatesSupported && !Caps.DiscretePerformanceStateCount);
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_QUERY_PERF_CAPABILITIES, &Perf));
    CHECK(Perf.DomainId == 1 && Perf.DomainMembers == 2);
    CHECK(Perf.HighestPerformance == 2252800 && Perf.LowestPerformance == 1740800);
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_ENTER_SYSTEM_STATE, &Enter));
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_RESUME_FROM_SYSTEM_STATE, &Resume));
    CHECK(!CpuPpmDomainCacheValid);

    Set.MinimumPerformance = Perf.LowestPerformance;
    Set.DesiredPerformance = 2200000;
    Set.MaximumPerformance = Perf.HighestPerformance;
    BeforeWrites = Writes;
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_PERF_SET, &Set));
    CHECK(Writes == BeforeWrites + 1 && Registers[1][CPU_PERFORMANCE / 4] == 0);
    Set.MinimumPerformance = Perf.HighestPerformance + 1;
    BeforeWrites = Writes;
    CHECK(!CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_PERF_SET, &Set));
    CHECK(Writes == BeforeWrites);
    Set.MinimumPerformance = Perf.LowestPerformance;
    Set.PerformanceTolerance = Perf.HighestPerformance + 1;
    BeforeWrites = Writes;
    CHECK(!CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_PERF_SET, &Set));
    CHECK(Writes == BeforeWrites &&
        Registers[1][CPU_PERFORMANCE / 4] == 0);
    Set.PerformanceTolerance = 0;
    Set.MinimumPerformance = Perf.LowestPerformance + 1;
    Set.MaximumPerformance = Perf.HighestPerformance - 1;
    CHECK(!CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_PERF_SET, &Set));
    CHECK(Writes == BeforeWrites);
    Set.MinimumPerformance = 0;
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_PERF_SET, &Set));
    CHECK(Writes == BeforeWrites + 1 && Registers[1][CPU_PERFORMANCE / 4] == 1);

    ScalingMode = 1;
    Caps = (PEP_PPM_QUERY_CAPABILITIES){0};
    CHECK(CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_QUERY_CAPABILITIES, &Caps));
    CHECK(Caps.PerformanceStatesSupported && !Caps.DiscretePerformanceStateCount);
    ScalingMode = 0;
    CHECK(CpuDpmNotify(PEP_DPM_UNREGISTER_DEVICE, &(PEP_UNREGISTER_DEVICE){Register.DeviceHandle}));
    CHECK(!CpuPpmNotify(Register.DeviceHandle, PEP_NOTIFY_PPM_QUERY_CAPABILITIES, &Caps));
    CHECK(CpuRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS);
}

int TestMain(VOID) {
    TestInitializeSpinLock(&CpuPpmLock);
    CpuInitializePpm();
    TestWdf[WdfObjectGetTypedContextWorkerTableIndex] = (WDFFUNC)TestContext;
    TestWdf[WdfCmResourceListGetCountTableIndex] = (WDFFUNC)TestCount;
    TestWdf[WdfCmResourceListGetDescriptorTableIndex] = (WDFFUNC)TestDescriptor;
    TestWdf[WdfIoQueueGetDeviceTableIndex] = (WDFFUNC)TestQueueDevice;
    TestWdf[WdfRequestRetrieveInputBufferTableIndex] = (WDFFUNC)TestInput;
    TestWdf[WdfRequestRetrieveOutputBufferTableIndex] = (WDFFUNC)TestOutput;
    TestWdf[WdfRequestCompleteWithInformationTableIndex] = (WDFFUNC)TestComplete;

    for (ULONG Index = 0; Index < ARRAYSIZE(CpuResources); ++Index) {
        ResetFixture();
        Descriptors[1][Index].u.Memory.Start.QuadPart += 4;
        CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_DEVICE_CONFIGURATION_ERROR);
        CHECK(!Maps && !Reads);
        ResetFixture();
        Descriptors[0][Index].Flags ^= CM_RESOURCE_MEMORY_READ_ONLY;
        CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_DEVICE_CONFIGURATION_ERROR);
        CHECK(!Maps && !Reads);
        ResetFixture();
        MapFailure = Index + 1;
        CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_INSUFFICIENT_RESOURCES);
        CHECK(Unmaps == Index && !Reads);
        Query(0, STATUS_DEVICE_NOT_READY);
        CHECK(!Reads);
    }

    ResetFixture();
    CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_SUCCESS && !Reads);
    CHECK(CpuPrepare(TEST_DEVICE, TEST_RAW, TEST_TRANSLATED) == STATUS_DEVICE_CONFIGURATION_ERROR);
    Request(0, 0, sizeof(ULONG), sizeof(T70_CPU_READING), STATUS_INVALID_DEVICE_REQUEST);
    Request(0, IOCTL_T70_CPU_QUERY_DOMAIN, 0, sizeof(T70_CPU_READING), STATUS_INVALID_PARAMETER);
    Request(0, IOCTL_T70_CPU_QUERY_DOMAIN, sizeof(ULONG), 4, STATUS_BUFFER_TOO_SMALL);
    Query(2, STATUS_INVALID_PARAMETER);
    BufferStatus = STATUS_BUFFER_TOO_SMALL;
    Query(0, STATUS_BUFFER_TOO_SMALL);
    BufferStatus = STATUS_SUCCESS;
    CHECK(!Reads);

    for (ULONG Domain = 0; Domain < T70_CPU_DOMAIN_COUNT; ++Domain) {
        Query(Domain, STATUS_SUCCESS);
        CHECK(Buffer.Reading.ControllerValue == 1 && Buffer.Reading.FrequencyCount == 2);
        CHECK(Buffer.Reading.FrequenciesKhz[0] == 2000000 + 200000 * Domain);
        CHECK(Buffer.Reading.RequestedKhz == 1500000 + 200000 * Domain);
        CHECK(Buffer.Reading.FrequenciesKhz[2] == 0);
    }

    PULONG R = Registers[0];
    R[0] |= 0x1000;
    Query(0, STATUS_SUCCESS);
    CHECK(Buffer.Reading.FrequenciesKhz[0] == 2000000);
    Version = 2;
    Query(0, STATUS_SUCCESS);
    CHECK(Buffer.Reading.FrequenciesKhz[0] == 6096000);
    R[CPU_PERFORMANCE / 4] = 1800000;
    Query(0, STATUS_SUCCESS);
    CHECK(Buffer.Reading.RequestedKhz == 1800000 && Buffer.Reading.ControllerValue == 1800000);

    R[CPU_PERFORMANCE / 4] = 1499999;
    Query(0, STATUS_DEVICE_DATA_ERROR);
    R[CPU_PERFORMANCE / 4] = 6096001;
    Query(0, STATUS_DEVICE_DATA_ERROR);
    R[CPU_PERFORMANCE / 4] = 2;
    Query(0, STATUS_DEVICE_DATA_ERROR);
    R[CPU_PERFORMANCE / 4] = 1;
    R[1] = 0;
    Query(0, STATUS_DEVICE_DATA_ERROR);
    R[1] = MAXULONG;
    Query(0, STATUS_DEVICE_DATA_ERROR);
    R[1] = 7000;
    Query(0, STATUS_DEVICE_DATA_ERROR);
    for (ULONG Index = 0; Index < T70_CPU_MAX_OPPS; ++Index) R[Index] = 2000 - 10 * Index;
    R[CPU_PERFORMANCE / 4] = 31;
    Query(0, STATUS_SUCCESS);
    CHECK(Buffer.Reading.RequestedKhz == 1690000 && Buffer.Reading.FrequencyCount == 32);

    R[CPU_ENABLE / 4] = 0;
    ULONG Before = Reads;
    Query(0, STATUS_DEVICE_NOT_READY);
    CHECK(Reads == Before + 2);
    R[CPU_ENABLE / 4] = 1;
    R[CPU_STATUS / 4] = MAXULONG;
    Query(0, STATUS_DEVICE_NOT_READY);
    R[CPU_STATUS / 4] = 3;
    Version = 3;
    Query(0, STATUS_DEVICE_NOT_READY);
    Version = 2;
    R[CPU_PERFORMANCE / 4] = 0;
    ChangePerformance = TRUE;
    Query(0, STATUS_RETRY);
    ChangePerformance = FALSE;
    ChangeVersion = TRUE;
    Query(0, STATUS_RETRY);
    ChangeVersion = FALSE;
    LoseReady = TRUE;
    Query(0, STATUS_RETRY);

    CHECK(CpuRelease(TEST_DEVICE, TEST_TRANSLATED) == STATUS_SUCCESS && Unmaps == Count);
    Before = Reads;
    Query(0, STATUS_DEVICE_NOT_READY);
    CHECK(Reads == Before);
    CheckTargets();
    TestPpm();
    CHECK(SpinLockCalls > 0);
    return Failure;
}
