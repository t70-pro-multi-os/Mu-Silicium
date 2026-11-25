// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdPower.h"
#include <Library/MtkPmicBroker.h>

static ULONG Failure;
__declspec(dllexport) volatile ULONG MtkSdPowerAssertionCount;
#define CHECK(Condition) do { \
    ++MtkSdPowerAssertionCount; \
    if (!(Condition) && !Failure) { Failure = __LINE__; } \
} while (0)

static NTSTATUS TestGetInterface(ULONG Version, PPRM_INTERFACE Interface);
static KIRQL TestIrql(VOID);
static ULONGLONG TestNow(PULONG64 Counter);
static VOID TestStall(ULONG Microseconds);
static NTSTATUS TestDelay(KPROCESSOR_MODE Mode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
#define ExGetPrmInterface TestGetInterface
#define KeGetCurrentIrql TestIrql
#define KeQueryInterruptTimePrecise TestNow
#define KeStallExecutionProcessor TestStall
#define KeDelayExecutionThread TestDelay
#include "SdPower.c"

static MTK_SD_POWER Power;
static MTK_PMIC_BROKER Broker;
static ULONGLONG Time, AcquireAck;
static KIRQL Irql;
static ULONG LockCount, UnlockCount, GetCount, Writes, Delays, ReadCommands, Invocations;
static ULONG IoBusy[MtkPmicBrokerReleaseSdPower + 1];
static USHORT FailOperation, CorruptOperation;
static BOOLEAN Found, Locked, RejectV3, PermanentBusy, LoseReply, StaleStep, FailUnlock, DelayShort;
static BOOLEAN FailWrite, DropWrite, Reenter;
static NTSTATUS DelayStatus;
static ULONG Fsm, WriteData, Response;
static const uint32_t RailAddresses[] = {0x1E48U, 0x1E4CU, 0x1D70U, 0x1CD8U, 0x1CC4U};
static uint32_t Rails[RTL_NUMBER_OF(RailAddresses)];

static MTK_PMIC_BROKER_IO_STATUS
ModelRead(void *Context, uint32_t Offset, uint32_t *Value)
{
    CHECK(Context == Rails);
    switch (Offset) {
    case 0:
        *Value = 1;
        return MtkPmicBrokerIoOk;
    case 0x8A8:
        *Value = Fsm << 1;
        return MtkPmicBrokerIoOk;
    case 0x894:
        *Value = Response;
        return MtkPmicBrokerIoOk;
    default:
        CHECK(FALSE);
        return MtkPmicBrokerIoFailed;
    }
}

static MTK_PMIC_BROKER_IO_STATUS
ModelWrite(void *Context, uint32_t Offset, uint32_t Value)
{
    CHECK(Context == Rails && MtkPmicBrokerWacsWriteAllowed(&Broker, Offset, Value));
    if (Offset == 0x884) {
        WriteData = Value;
        return MtkPmicBrokerIoOk;
    }
    if (Offset == 0x8A4) {
        CHECK(Fsm == 6 && Value == 1);
        Fsm = 0;
        return MtkPmicBrokerIoOk;
    }
    CHECK(Offset == 0x880 && Fsm == 0);
    for (ULONG Index = 0; Index < RTL_NUMBER_OF(Rails); ++Index) {
        if (RailAddresses[Index] == (Value & 0xFFFFU)) {
            if (Value & 0x20000000U) {
                if (!DropWrite) {
                    Rails[Index] = WriteData;
                }
                ++Writes;
                return FailWrite ? MtkPmicBrokerIoFailed : MtkPmicBrokerIoOk;
            }
            ++ReadCommands;
            Fsm = 6;
            Response = Rails[Index];
            return MtkPmicBrokerIoOk;
        }
    }
    CHECK(FALSE);
    return MtkPmicBrokerIoFailed;
}

static uint64_t
ModelTime(void *Context)
{
    CHECK(Context == Rails);
    return Time / 10U;
}

static NTSTATUS
ModelLock(LPGUID Guid)
{
    CHECK(!Locked && RtlEqualMemory(Guid, &SdPowerHandler, sizeof(*Guid)));
    Locked = TRUE;
    ++LockCount;
    return STATUS_SUCCESS;
}

static NTSTATUS
ModelUnlock(LPGUID Guid)
{
    CHECK(Locked && RtlEqualMemory(Guid, &SdPowerHandler, sizeof(*Guid)));
    ++UnlockCount;
    if (FailUnlock) {
        return STATUS_TIMEOUT;
    }
    Locked = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS
ModelQuery(LPGUID Guid, PBOOLEAN Present)
{
    CHECK(Locked && RtlEqualMemory(Guid, &SdPowerHandler, sizeof(*Guid)));
    *Present = Found;
    return STATUS_SUCCESS;
}

static NTSTATUS
ModelInvoke(LPGUID Guid, PVOID Buffer, ULONG Reserved, PULONG64 EfiStatus)
{
    MTK_PMIC_BROKER_REQUEST *Request = Buffer;
    MTK_PMIC_BROKER_REQUEST Input = *Request;
    const MTK_PMIC_BROKER_IO Io = {Rails, ModelRead, ModelWrite, ModelTime};
    CHECK(Locked && RtlEqualMemory(Guid, &SdPowerHandler, sizeof(*Guid)) && !Reserved);
    CHECK(Request->Operation <= MtkPmicBrokerReleaseSdPower && Request->Flags == 0 &&
          Request->State == 0 && Request->Result == 0 && !Request->Address && !Request->Value);
    ++Invocations;
    *EfiStatus = 0;
    if (IoBusy[Request->Operation] || (PermanentBusy && FailOperation == Request->Operation)) {
        if (IoBusy[Request->Operation]) {
            --IoBusy[Request->Operation];
        }
        Request->Result = MtkPmicBrokerResultBusy;
        return STATUS_SUCCESS;
    }
    if (RejectV3) {
        Request->Result = MtkPmicBrokerResultInvalid;
        return STATUS_SUCCESS;
    }
    MTK_PMIC_BROKER_RESULT Result = MtkPmicBrokerHandle(&Broker, &Io, 0, Request);
    if (Result == MtkPmicBrokerResultBusy) {
        *Request = Input;
        Request->Result = MtkPmicBrokerResultBusy;
    }
    if (Input.Operation == MtkPmicBrokerAck && Result == MtkPmicBrokerResultSuccess && Broker.SdPowerToken) {
        AcquireAck = Time;
    }
    if (LoseReply && Input.Operation == FailOperation) {
        return STATUS_DEVICE_BUSY;
    }
    if (Input.Operation == CorruptOperation) {
        Request->Reserved0 = 1;
    }
    if (StaleStep && Input.Operation == MtkPmicBrokerStep) {
        ++Request->Generation;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
TestGetInterface(ULONG Version, PPRM_INTERFACE Interface)
{
    CHECK(Version == PRM_INTERFACE_V1 && Irql == PASSIVE_LEVEL);
    ++GetCount;
    *Interface = (PRM_INTERFACE){PRM_INTERFACE_V1, ModelUnlock, ModelLock, ModelInvoke, ModelQuery};
    return STATUS_SUCCESS;
}

static KIRQL
TestIrql(VOID)
{
    return Irql;
}

static ULONGLONG
TestNow(PULONG64 Counter)
{
    CHECK(Counter != NULL);
    *Counter = Time;
    return Time;
}

static VOID
TestStall(ULONG Microseconds)
{
    CHECK(Microseconds == SD_POWER_POLL_US);
    Time += Microseconds * 10ULL;
    if (Reenter) {
        Reenter = FALSE;
        CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_DEVICE_BUSY);
    }
}

static NTSTATUS
TestDelay(KPROCESSOR_MODE Mode, BOOLEAN Alertable, PLARGE_INTEGER Interval)
{
    CHECK(Mode == KernelMode && !Alertable && Interval->QuadPart < 0 && !Power.Settled &&
          Irql <= APC_LEVEL && Broker.State == MtkPmicBrokerStateIdle && Broker.SdPowerToken == Power.Lease);
    ++Delays;
    ULONGLONG Delay = (ULONGLONG)-Interval->QuadPart;
    Time += DelayShort ? Delay / 2 : Delay;
    DelayShort = FALSE;
    return DelayStatus;
}

static VOID
Reset(VOID)
{
    RtlZeroMemory(&Power, sizeof(Power));
    MtkPmicBrokerInitialize(&Broker);
    RtlZeroMemory(Rails, sizeof(Rails));
    RtlZeroMemory(IoBusy, sizeof(IoBusy));
    Time = AcquireAck = 0;
    LockCount = UnlockCount = GetCount = Writes = Delays = ReadCommands = Invocations = 0;
    Fsm = WriteData = Response = 0;
    Irql = PASSIVE_LEVEL;
    FailOperation = CorruptOperation = MAXUSHORT;
    Locked = RejectV3 = PermanentBusy = LoseReply = StaleStep = FailUnlock = DelayShort = FALSE;
    FailWrite = DropWrite = Reenter = FALSE;
    Found = TRUE;
    DelayStatus = STATUS_SUCCESS;
    Rails[0] = 0x3200U;
    Rails[1] = 0x3400U;
    Rails[2] = 0x2004U;
    Rails[3] = 0x0100U;
    Rails[4] = 0x1000U;
    CHECK(MtkSdPowerInitialize(&Power) == STATUS_SUCCESS && GetCount == 1);
}

static VOID
TestPowerCycle(VOID)
{
    Reset();
    CHECK(MtkSdPowerInitialize(&Power) == STATUS_INVALID_PARAMETER && GetCount == 1);
    Irql = DISPATCH_LEVEL;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_INVALID_PARAMETER && !Writes && !LockCount);
    Irql = APC_LEVEL;
    IoBusy[MtkPmicBrokerQuery] = 1;
    IoBusy[MtkPmicBrokerAcquireSdPower] = 2;
    IoBusy[MtkPmicBrokerStep] = 2;
    IoBusy[MtkPmicBrokerAck] = 1;
    DelayShort = Reenter = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_SUCCESS);
    CHECK(Power.Lease && Power.Lease == Broker.SdPowerToken && Broker.State == MtkPmicBrokerStateIdle &&
          Power.Settled && Power.ModuleLocked && Delays == 2 && Time - AcquireAck >= SD_POWER_SETTLE_100NS);
    CHECK(LockCount == 1 && UnlockCount == 0 && Writes == 6 && ReadCommands == 17);
    CHECK(Rails[0] == 0x3500 && Rails[1] == 0x3D00 && Rails[2] == 0x2001 &&
          Rails[3] == 0x0101 && Rails[4] == 0x1001);
    ULONG Calls = Invocations;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_SUCCESS && Invocations == Calls);
    Rails[4] |= 0x4000;
    CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_SUCCESS);
    CHECK(!Power.Lease && !Power.Settled && !Power.ModuleLocked &&
          !Broker.SdPowerToken && !Locked && LockCount == UnlockCount);
    CHECK(Writes == 11 && Rails[0] == 0x3200 && Rails[1] == 0x3400 &&
          Rails[2] == 0x2004 && Rails[3] == 0x0100 && Rails[4] == 0x5000);
    Calls = Invocations;
    CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_SUCCESS && Invocations == Calls);
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_SUCCESS && Power.Lease > 1);
    CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_SUCCESS && LockCount == 2 && UnlockCount == 2);
}

static VOID
TestKnownFailures(VOID)
{
    Reset();
    Found = FALSE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_NOT_SUPPORTED && !Locked && !Invocations);
    Found = TRUE;
    RejectV3 = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_NOT_SUPPORTED && !Locked && !ReadCommands);
    RejectV3 = FALSE;
    FailOperation = MtkPmicBrokerAcquireSdPower;
    PermanentBusy = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_DEVICE_BUSY && !Locked && !ReadCommands && !Power.Faulted);
    PermanentBusy = FALSE;
    Rails[4] |= 1;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_INVALID_DEVICE_STATE && !Locked && !Power.Faulted && !Writes);
    CHECK(Broker.State == MtkPmicBrokerStateIdle && !Broker.SdPowerToken);
    Rails[4] &= ~1U;
    DelayStatus = STATUS_ALERTED;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_UNSUCCESSFUL && Power.Lease && !Power.Settled && Locked);
    ULONG SavedWrites = Writes;
    DelayStatus = STATUS_SUCCESS;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_SUCCESS && Writes == SavedWrites && Power.Settled);
    FailOperation = MtkPmicBrokerReleaseSdPower;
    PermanentBusy = TRUE;
    CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_DEVICE_BUSY && Power.Lease && !Power.Settled && Locked);
    PermanentBusy = FALSE;
    CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_SUCCESS && !Power.Lease && !Locked);
}

static VOID
TestUncertainOutcomes(VOID)
{
    Reset();
    FailOperation = MtkPmicBrokerAcquireSdPower;
    LoseReply = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_IO_DEVICE_ERROR && Locked && Power.Faulted && !Power.Lease);
    CHECK(Broker.State == MtkPmicBrokerStatePending && !ReadCommands);
    ULONG Calls = Invocations;
    CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_DEVICE_NOT_READY && Invocations == Calls);

    Reset();
    FailOperation = MtkPmicBrokerStep;
    PermanentBusy = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_IO_TIMEOUT && Locked && Power.Faulted && !Power.Lease);
    CHECK(Time < SD_POWER_JOB_100NS + 1000);

    Reset();
    StaleStep = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_DEVICE_DATA_ERROR && Locked && Power.Faulted && !Writes);
    CHECK(Broker.State == MtkPmicBrokerStatePending);

    Reset();
    CorruptOperation = MtkPmicBrokerAck;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_DEVICE_DATA_ERROR && Locked && Power.Faulted &&
          !Power.Lease && Broker.SdPowerToken && !Delays);

    Reset();
    FailWrite = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_IO_DEVICE_ERROR && Locked && Power.Faulted && Writes == 1);
    CHECK(Broker.State == MtkPmicBrokerStateFault);

    Reset();
    DropWrite = TRUE;
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_IO_DEVICE_ERROR && Locked && Power.Faulted && Writes == 1);
    CHECK(Broker.State == MtkPmicBrokerStateIdle && Broker.SdPowerFault && !MtkPmicBrokerBlocksUfs(&Broker));

    Reset();
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_SUCCESS);
    FailUnlock = TRUE;
    CHECK(MtkSdPowerSet(&Power, FALSE) == STATUS_UNSUCCESSFUL);
    CHECK(Power.Faulted && Locked && !Power.Lease && !Broker.SdPowerToken && !Power.Settled);
    CHECK(MtkSdPowerSet(&Power, TRUE) == STATUS_DEVICE_NOT_READY && LockCount == 1 && UnlockCount == 1);
}

ULONG
TestMain(VOID)
{
    TestPowerCycle();
    TestKnownFailures();
    TestUncertainOutcomes();
    return Failure;
}
