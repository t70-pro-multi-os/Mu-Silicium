// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../ConnPmic.h"
#include <Library/MtkConnRails.h>
#include <Library/MtkPmicBroker.h>

static ULONG Failure;
__declspec(dllexport) volatile ULONG MtkConnPmicAssertionCount;
#define CHECK(X) do { \
    ++MtkConnPmicAssertionCount; \
    if (!(X) && !Failure) Failure = __LINE__; \
} while (0)

static NTSTATUS ModelGetInterface(ULONG Version, PPRM_INTERFACE Interface);
static KIRQL ModelIrql(VOID);
static ULONGLONG ModelTime(PULONG64 Counter);
static VOID ModelStall(ULONG Us);
#define ExGetPrmInterface ModelGetInterface
#define KeGetCurrentIrql ModelIrql
#define KeQueryInterruptTimePrecise ModelTime
#define KeStallExecutionProcessor ModelStall
#include "../ConnPmic.c"

static MTK_PMIC_BROKER Broker;
static MTK_CONN_RAILS Rails;
static T70_CONN_PMIC Pmic;
static unsigned Registers[0x8000], Pins[0x400 / 4];
static unsigned Channel, Data, Response, Writes, Reads, Invocations, Locks, Unlocks;
static unsigned BusyCalls, IgnoreWrite, LoseOperation, FailUnlock;
static BOOLEAN Legacy, LoseWrite;
static ULONGLONG Time;

static MTK_PMIC_BROKER_IO_STATUS ModelRead(void *Context, uint32_t Offset, uint32_t *Value)
{
    CHECK(Context == &Broker);
    switch (Offset) {
    case 0: *Value = 1; break;
    case 0x8A8: *Value = Channel << 1; break;
    case 0x894: *Value = Response; break;
    default: CHECK(FALSE); return MtkPmicBrokerIoFailed;
    }
    return MtkPmicBrokerIoOk;
}

static MTK_PMIC_BROKER_IO_STATUS ModelWrite(void *Context, uint32_t Offset, uint32_t Value)
{
    CHECK(Context == &Broker && MtkPmicBrokerWacsWriteAllowed(&Broker, Offset, Value));
    if (Offset == 0x884) {
        CHECK(!Channel && Value <= MAXUSHORT);
        Data = Value;
    } else if (Offset == 0x8A4) {
        CHECK(Channel == 6 && Value == 1);
        Channel = 0;
    } else {
        CHECK(Offset == 0x880 && !Channel);
        unsigned Address = Value & 0xFFFF;
        if (Address >= RTL_NUMBER_OF(Registers)) return MtkPmicBrokerIoFailed;
        if (Value & 0x20000000U) {
            ++Writes;
            if (Address == 0x1726) {
                Registers[0x1724] |= Data;
            } else if (Address == 0x1728) {
                Registers[0x1724] &= ~Data;
            } else if (Address != IgnoreWrite) {
                CHECK(Address != 0x1D2A);
                Registers[Address] = Data;
            }
        } else {
            ++Reads;
            Channel = 6;
            Response = Registers[Address];
            if (Address == 0x1C66 && (Registers[0x1C58] & 1)) Response |= 0x8000;
            if (Address == 0x1D2C && ((Registers[0x1D1C] | Registers[0x1D2A]) & 1)) Response |= 0x8000;
        }
    }
    return MtkPmicBrokerIoOk;
}

static uint64_t ModelUs(void *Context)
{
    CHECK(Context == &Broker);
    return Time / 10U;
}

static NTSTATUS ModelLock(LPGUID Guid)
{
    CHECK(RtlEqualMemory(Guid, &PmicHandler, sizeof(*Guid)) && Locks == Unlocks);
    ++Locks;
    return STATUS_SUCCESS;
}

static NTSTATUS ModelUnlock(LPGUID Guid)
{
    CHECK(RtlEqualMemory(Guid, &PmicHandler, sizeof(*Guid)) && Locks == Unlocks + 1);
    if (FailUnlock) {
        --FailUnlock;
        return STATUS_IO_DEVICE_ERROR;
    }
    ++Unlocks;
    return STATUS_SUCCESS;
}

static NTSTATUS ModelQuery(LPGUID Guid, PBOOLEAN Present)
{
    CHECK(RtlEqualMemory(Guid, &PmicHandler, sizeof(*Guid)) && Locks == Unlocks + 1);
    *Present = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS ModelInvoke(LPGUID Guid, PVOID Buffer, ULONG Reserved, PULONG64 EfiStatus)
{
    CHECK(RtlEqualMemory(Guid, &PmicHandler, sizeof(*Guid)) && !Reserved && Locks == Unlocks + 1);
    MTK_PMIC_BROKER_REQUEST *Request = Buffer;
    MTK_PMIC_BROKER_REQUEST Input = *Request;
    MTK_PMIC_BROKER_IO Io = {&Broker, ModelRead, ModelWrite, ModelUs};
    unsigned Before = Writes;
    ++Invocations;
    *EfiStatus = 0;
    if (Legacy) {
        Request->Result = MtkPmicBrokerResultInvalid;
    } else if (BusyCalls && Request->Operation == MtkPmicBrokerBeginConnWrite) {
        --BusyCalls;
        Request->Result = MtkPmicBrokerResultBusy;
    } else if (MtkPmicBrokerHandle(&Broker, &Io, 0, Request) == MtkPmicBrokerResultBusy) {
        *Request = Input;
        Request->Result = MtkPmicBrokerResultBusy;
    }
    if ((LoseOperation && LoseOperation == Input.Operation) || (LoseWrite && Writes != Before)) {
        LoseOperation = 0;
        LoseWrite = FALSE;
        return STATUS_IO_DEVICE_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ModelGetInterface(ULONG Version, PPRM_INTERFACE Interface)
{
    CHECK(Version == PRM_INTERFACE_V1);
    *Interface = (PRM_INTERFACE){Version, ModelUnlock, ModelLock, ModelInvoke, ModelQuery};
    return STATUS_SUCCESS;
}

static KIRQL ModelIrql(VOID)
{
    return PASSIVE_LEVEL;
}

static ULONGLONG ModelTime(PULONG64 Counter)
{
    *Counter = Time;
    return Time;
}

static VOID ModelStall(ULONG Us)
{
    CHECK(Us > 0 && Us <= 300U);
    Time += (ULONGLONG)Us * 10U;
}

static int ReadPin(void *Context, unsigned Offset, unsigned *Value)
{
    CHECK(Context == &Pmic && Pmic.Lease && Offset < sizeof(Pins) && !(Offset & 3U));
    *Value = Pins[Offset / 4U];
    return 0;
}

static int WritePin(void *Context, unsigned Offset, unsigned Value)
{
    CHECK(Context == &Pmic && Pmic.Lease && Offset < sizeof(Pins) && !(Offset & 3U));
    Pins[Offset / 4U] = Value;
    return 0;
}

static int WaitUs(void *Context, unsigned Us)
{
    CHECK(Context == &Pmic && Pmic.Lease);
    ModelStall(Us);
    return 0;
}

static int Authorized(void *Context, MTK_CONN_RAIL Rail)
{
    CHECK(Context == &Pmic && Rail <= MtkConnRuntimeVcn33);
    return Pmic.Lease && !Pmic.Uncertain;
}

static VOID Reset(VOID)
{
    MtkPmicBrokerInitialize(&Broker);
    RtlZeroMemory(&Rails, sizeof(Rails));
    RtlZeroMemory(&Pmic, sizeof(Pmic));
    RtlZeroMemory(Registers, sizeof(Registers));
    RtlZeroMemory(Pins, sizeof(Pins));
    Channel = Data = Response = Writes = Reads = Invocations = Locks = Unlocks = 0;
    BusyCalls = IgnoreWrite = LoseOperation = FailUnlock = 0;
    Legacy = LoseWrite = FALSE;
    Time = 0;
}

static VOID TestRails(unsigned Adie)
{
    Reset();
    Registers[0x1C58] = 0x100;
    Registers[0x1E8C] = 0xB352;
    Registers[0x1E28] = 0x1840;
    Registers[0x1720] = 1;
    Registers[0x1724] = 0x20;
    Registers[0x172A] = 0x30;
    Registers[0x172E] = 0x60;
    Registers[0x170A] = 0x40;
    Pins[0x380 / 4] = 0x30000;
    CHECK(T70ConnPmicAcquire(&Pmic) == STATUS_SUCCESS && Pmic.Lease == Broker.ConnLeaseToken);
    CHECK(!Reads && !Writes);
    const MTK_CONN_RAIL_IO Io = {&Pmic, T70ConnPmicRead, T70ConnPmicWrite, ReadPin, WritePin,
                                WaitUs, Authorized};
    CHECK(MtkConnRailsInit(&Rails, &Io) == MtkConnPowerOk);
    BusyCalls = 3;
    CHECK(MtkConnRailsAcquireRuntime(&Rails, Adie, &Rails) == MtkConnPowerOk);
    CHECK(!BusyCalls && Reads && Writes && Broker.ConnDirty);
    CHECK(MtkConnRailsCheckRuntime(&Rails, &Rails) == MtkConnPowerOk);
    CHECK(Registers[0x1C58] == 0x101 && Registers[0x1E8C] == 0xBC50);
    if (Adie == 0x6631) {
        CHECK(Registers[0x1E28] == 0x1940 && Registers[0x1D1C] == 1U);
    } else {
        CHECK(Registers[0x1724] == 0x30 && (Pins[0x120 / 4] & 0x10));
    }
    CHECK(T70ConnPmicRelease(&Pmic) == STATUS_INVALID_DEVICE_STATE && Pmic.Lease && Pmic.ModuleLocked);
    IgnoreWrite = 0x1C58;
    CHECK(MtkConnRailsReleaseRuntime(&Rails, &Rails) == MtkConnPowerIoError && Broker.ConnDirty);
    CHECK(Pmic.Lease && !Pmic.Uncertain && !MtkPmicBrokerBlocksUfs(&Broker));
    IgnoreWrite = 0;
    CHECK(MtkConnRailsReleaseRuntime(&Rails, &Rails) == MtkConnPowerOk);
    CHECK(Registers[0x1C58] == 0x100 && Registers[0x1E8C] == 0xB352 && Registers[0x1E28] == 0x1840);
    CHECK(Registers[0x1724] == 0x20 && Registers[0x172A] == 0x30 && Registers[0x172E] == 0x60 &&
          Registers[0x170A] == 0x40 && Pins[0x380 / 4] == 0x30000 && !Pins[0x120 / 4]);
    CHECK(T70ConnPmicRelease(&Pmic) == STATUS_SUCCESS && !Broker.ConnLeaseToken);
    CHECK(!Pmic.Lease && !Pmic.ModuleLocked && Unlocks == 1 && Locks == 1);
}

static VOID TestFailures(VOID)
{
    Reset();
    Legacy = TRUE;
    CHECK(T70ConnPmicAcquire(&Pmic) == STATUS_NOT_SUPPORTED && Locks == Unlocks && !Reads && !Writes);
    Legacy = FALSE;
    CHECK(T70ConnPmicAcquire(&Pmic) == STATUS_SUCCESS && Pmic.Lease);
    unsigned Value = 27;
    CHECK(T70ConnPmicRead(&Pmic, 0x1CD8, &Value) == STATUS_INVALID_PARAMETER && Value == 27);
    CHECK(T70ConnPmicWrite(&Pmic, 0x1D2A, 1) == STATUS_INVALID_PARAMETER && !Reads && !Writes);
    BusyCalls = 5000;
    CHECK(T70ConnPmicWrite(&Pmic, 0x1C58, 1) == STATUS_DEVICE_BUSY && !Pmic.Uncertain);
    CHECK(BusyCalls && !Reads && !Writes);
    LoseOperation = MtkPmicBrokerReleaseConnRails;
    CHECK(T70ConnPmicRelease(&Pmic) == STATUS_IO_DEVICE_ERROR);
    CHECK(!Broker.ConnLeaseToken && Pmic.Lease && Pmic.ModuleLocked && Locks == Unlocks + 1);
    FailUnlock = 1;
    CHECK(T70ConnPmicRelease(&Pmic) == STATUS_IO_DEVICE_ERROR && !Pmic.Lease && Pmic.ModuleLocked);
    CHECK(T70ConnPmicRelease(&Pmic) == STATUS_SUCCESS && Locks == Unlocks);

    Reset();
    CHECK(T70ConnPmicAcquire(&Pmic) == STATUS_SUCCESS && Pmic.Lease);
    LoseWrite = TRUE;
    CHECK(T70ConnPmicWrite(&Pmic, 0x1C58, 1) == STATUS_IO_DEVICE_ERROR);
    CHECK(Pmic.Uncertain && Pmic.Lease && Pmic.ModuleLocked && Registers[0x1C58] == 1U && Writes == 1U);
    unsigned Before = Invocations;
    CHECK(T70ConnPmicRelease(&Pmic) == STATUS_DEVICE_NOT_READY && Locks == Unlocks + 1);
    CHECK(T70ConnPmicWrite(&Pmic, 0x1C58, 0) == STATUS_DEVICE_NOT_READY && Invocations == Before);
}

ULONG TestMain(VOID)
{
    TestRails(0x6631);
    TestRails(0x6635);
    TestFailures();
    return Failure;
}
