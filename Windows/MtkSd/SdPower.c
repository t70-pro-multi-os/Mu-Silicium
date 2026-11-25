// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdPower.h"
#include <Library/MtkPmicBroker.h>

#define SD_POWER_POLL_US 10U
#define SD_POWER_MAX_CALLS 4096U
#define SD_POWER_JOB_100NS 400000ULL
#define SD_POWER_SETTLE_100NS 200000ULL
#define SD_VMCH_VOLTAGE 0x1E48U

C_ASSERT(sizeof(MTK_PMIC_BROKER_REQUEST) == 64);

typedef struct {
    ULONGLONG Start;
    ULONG Calls;
} SD_POWER_POLL;

static const GUID SdPowerHandler = MTK_PMIC_PRM_HANDLER_GUID;

static ULONGLONG
SdNow(VOID)
{
    ULONGLONG PerformanceCounter;
    return KeQueryInterruptTimePrecise(&PerformanceCounter);
}

static NTSTATUS
SdExactStatus(NTSTATUS Status)
{
    return NT_SUCCESS(Status) && Status != STATUS_SUCCESS ? STATUS_UNSUCCESSFUL : Status;
}

static BOOLEAN
SdKnownRail(ULONG Address)
{
    return Address == SD_VMCH_VOLTAGE || Address == 0x1E4CU || Address == 0x1D70U ||
           Address == 0x1CD8U || Address == 0x1CC4U;
}

/* Only a validated wire Busy proves that the input had no side effects. */
static NTSTATUS
SdCall(MTK_SD_POWER *Power, SD_POWER_POLL *Poll, USHORT Operation,
       ULONGLONG Generation, MTK_PMIC_BROKER_REQUEST *Request)
{
    while (Poll->Calls < SD_POWER_MAX_CALLS && SdNow() - Poll->Start < SD_POWER_JOB_100NS) {
        GUID Guid = SdPowerHandler;
        ULONGLONG EfiStatus = MAXULONGLONG;
        RtlZeroMemory(Request, sizeof(*Request));
        Request->Size = sizeof(*Request);
        Request->Version = MTK_PMIC_BROKER_VERSION_SD_POWER;
        Request->Operation = Operation;
        Request->Generation = Generation;
        ++Poll->Calls;
        NTSTATUS Status = Power->Interface.InvokeHandler(&Guid, Request, 0, &EfiStatus);
        if (Status != STATUS_SUCCESS || EfiStatus) {
            return STATUS_IO_DEVICE_ERROR;
        }
        if (Request->Size != sizeof(*Request) || Request->Version != MTK_PMIC_BROKER_VERSION_SD_POWER ||
            Request->Operation != Operation || Request->Reserved0 || Request->Reserved[0] ||
            Request->Reserved[1] || Request->Reserved[2] || Request->State > MtkPmicBrokerStateFault ||
            Request->Result > MtkPmicBrokerResultExhausted ||
            (Request->Flags && (Operation != MtkPmicBrokerQuery ||
             Request->Result != MtkPmicBrokerResultSuccess))) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        if (Request->Result != MtkPmicBrokerResultBusy) {
            return STATUS_SUCCESS;
        }
        if (Request->State != MtkPmicBrokerStateIdle || Request->Generation != Generation ||
            Request->Address || Request->Value) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        KeStallExecutionProcessor(SD_POWER_POLL_US);
    }
    return STATUS_DEVICE_BUSY;
}

static NTSTATUS
SdOpen(MTK_SD_POWER *Power)
{
    GUID Guid = SdPowerHandler;
    BOOLEAN Found = FALSE;
    NTSTATUS Status = SdExactStatus(Power->Interface.LockModule(&Guid));
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    Power->ModuleLocked = TRUE;
    Status = SdExactStatus(Power->Interface.QueryHandler(&Guid, &Found));
    if (Status != STATUS_SUCCESS || Found != TRUE) {
        return Status == STATUS_SUCCESS ? STATUS_NOT_SUPPORTED : Status;
    }

    SD_POWER_POLL Poll = {SdNow(), 0};
    MTK_PMIC_BROKER_REQUEST Request;
    Status = SdCall(Power, &Poll, MtkPmicBrokerQuery, 0, &Request);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    if (Request.Result == MtkPmicBrokerResultInvalid ||
        (Request.Flags & MTK_PMIC_BROKER_CAP_SD_POWER_LEASE) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    if (Request.State != MtkPmicBrokerStateIdle || Request.Result != MtkPmicBrokerResultSuccess ||
        Request.Generation || Request.Address || Request.Value) {
        return STATUS_DEVICE_DATA_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
SdClose(MTK_SD_POWER *Power)
{
    GUID Guid = SdPowerHandler;
    NTSTATUS Status = SdExactStatus(Power->Interface.UnlockModule(&Guid));
    if (Status == STATUS_SUCCESS) {
        Power->ModuleLocked = FALSE;
    } else {
        Power->Faulted = TRUE;
    }
    return Status;
}

static NTSTATUS
SdRunJob(MTK_SD_POWER *Power, BOOLEAN Enable)
{
    SD_POWER_POLL Poll = {SdNow(), 0};
    MTK_PMIC_BROKER_REQUEST Request;
    USHORT Operation = Enable ? MtkPmicBrokerAcquireSdPower : MtkPmicBrokerReleaseSdPower;
    NTSTATUS Status = SdCall(Power, &Poll, Operation, Power->Lease, &Request);
    if (Status == STATUS_DEVICE_BUSY) {
        return Status;
    }
    if (Status != STATUS_SUCCESS) {
        goto Uncertain;
    }
    if (Request.State == MtkPmicBrokerStateIdle &&
        (Request.Result == MtkPmicBrokerResultInvalid || Request.Result == MtkPmicBrokerResultWrongState ||
         Request.Result == MtkPmicBrokerResultStaleGeneration || Request.Result == MtkPmicBrokerResultExhausted)) {
        Power->Faulted = !Enable;
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!Enable && Request.State == MtkPmicBrokerStateIdle &&
        Request.Result == MtkPmicBrokerResultSuccess && Request.Generation > Power->Lease &&
        !Request.Address && !Request.Value) {
        /* A validated replay of an already acknowledged release. */
        Power->Lease = 0;
        Power->Settled = FALSE;
        return STATUS_SUCCESS;
    }
    if (Request.State != MtkPmicBrokerStatePending || Request.Result != MtkPmicBrokerResultInProgress ||
        Request.Generation <= Power->Lease || Request.Address != SD_VMCH_VOLTAGE || Request.Value) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Uncertain;
    }

    ULONGLONG Job = Request.Generation;
    for (;;) {
        KeStallExecutionProcessor(SD_POWER_POLL_US);
        Status = SdCall(Power, &Poll, MtkPmicBrokerStep, Job, &Request);
        if (Status != STATUS_SUCCESS) {
            goto Uncertain;
        }
        if (Request.Generation != Job) {
            Status = STATUS_DEVICE_DATA_ERROR;
            goto Uncertain;
        }
        if (Request.State != MtkPmicBrokerStatePending ||
            Request.Result != MtkPmicBrokerResultInProgress || Request.Value || !SdKnownRail(Request.Address)) {
            break;
        }
    }
    if (Request.State == MtkPmicBrokerStateFault) {
        Status = Request.Result == MtkPmicBrokerResultTimeout ? STATUS_IO_TIMEOUT : STATUS_IO_DEVICE_ERROR;
        goto Uncertain;
    }
    BOOLEAN Completed = Request.Result == MtkPmicBrokerResultSuccess &&
                        !Request.Address && Request.Value == (Enable ? 1U : 0U);
    BOOLEAN Rejected = (Request.Result == MtkPmicBrokerResultWrongState ||
                        Request.Result == MtkPmicBrokerResultIoError) &&
                       !Request.Value && SdKnownRail(Request.Address);
    if (Request.State != MtkPmicBrokerStateDone || (!Completed && !Rejected)) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Uncertain;
    }
    ULONG Address = Request.Address;
    ULONG Value = Request.Value;
    ULONG Result = Request.Result;
    Status = SdCall(Power, &Poll, MtkPmicBrokerAck, Job, &Request);
    if (Status != STATUS_SUCCESS) {
        goto Uncertain;
    }
    if (Request.State != MtkPmicBrokerStateIdle || Request.Result != MtkPmicBrokerResultSuccess ||
        Request.Generation != Job || Request.Address != Address || Request.Value != Value) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Uncertain;
    }
    if (!Completed) {
        Power->Faulted = !Enable || Result == MtkPmicBrokerResultIoError;
        return Result == MtkPmicBrokerResultWrongState ? STATUS_INVALID_DEVICE_STATE : STATUS_IO_DEVICE_ERROR;
    }
    Power->Lease = Enable ? Job : 0;
    Power->Settled = FALSE;
    return STATUS_SUCCESS;

Uncertain:
    /* An accepted job may still own WACS. Leave its module pinned and avoid
       replaying any operation after losing its response or deadline. */
    Power->Faulted = TRUE;
    return Status == STATUS_DEVICE_BUSY ? STATUS_IO_TIMEOUT : Status;
}

static NTSTATUS
SdWaitSettled(MTK_SD_POWER *Power)
{
    ULONGLONG Started = SdNow();
    for (ULONG Pass = 0; ; ++Pass) {
        ULONGLONG Now = SdNow();
        if (Now < Started) {
            return STATUS_IO_DEVICE_ERROR;
        }
        ULONGLONG Elapsed = Now - Started;
        if (Elapsed >= SD_POWER_SETTLE_100NS) {
            Power->Settled = TRUE;
            return STATUS_SUCCESS;
        }
        if (Pass == 8) {
            return STATUS_IO_TIMEOUT;
        }
        LARGE_INTEGER Delay;
        Delay.QuadPart = -(LONGLONG)(SD_POWER_SETTLE_100NS - Elapsed);
        NTSTATUS Status = SdExactStatus(KeDelayExecutionThread(KernelMode, FALSE, &Delay));
        if (Status != STATUS_SUCCESS) {
            return Status;
        }
    }
}

NTSTATUS
MtkSdPowerInitialize(MTK_SD_POWER *Power)
{
    if (!Power || KeGetCurrentIrql() != PASSIVE_LEVEL || Power->Interface.Version) {
        return STATUS_INVALID_PARAMETER;
    }
    PRM_INTERFACE Interface = {0};
    NTSTATUS Status = SdExactStatus(ExGetPrmInterface(PRM_INTERFACE_V1, &Interface));
    if (Status != STATUS_SUCCESS) {
        return Status;
    }
    if (Interface.Version != PRM_INTERFACE_V1 || !Interface.LockModule || !Interface.UnlockModule ||
        !Interface.QueryHandler || !Interface.InvokeHandler) {
        return STATUS_NOT_SUPPORTED;
    }
    Power->Interface = Interface;
    return STATUS_SUCCESS;
}

NTSTATUS
MtkSdPowerSet(MTK_SD_POWER *Power, BOOLEAN Enable)
{
    if (!Power || KeGetCurrentIrql() > APC_LEVEL || Power->Interface.Version != PRM_INTERFACE_V1) {
        return STATUS_INVALID_PARAMETER;
    }
    if (InterlockedCompareExchange(&Power->Busy, 1, 0)) {
        return STATUS_DEVICE_BUSY;
    }
    NTSTATUS Status = STATUS_SUCCESS;
    if (Power->Faulted) {
        Status = STATUS_DEVICE_NOT_READY;
    } else if (Enable && !Power->Lease) {
        Status = SdOpen(Power);
        if (Status == STATUS_SUCCESS) {
            Status = SdRunJob(Power, TRUE);
        }
    } else if (!Enable && Power->Lease) {
        Power->Settled = FALSE;
        Status = SdRunJob(Power, FALSE);
    }
    if (!Power->Lease && Power->ModuleLocked && !Power->Faulted) {
        NTSTATUS CloseStatus = SdClose(Power);
        if (CloseStatus != STATUS_SUCCESS) {
            Status = CloseStatus;
        }
    }
    if (Status == STATUS_SUCCESS && Enable && !Power->Settled) {
        Status = SdWaitSettled(Power);
    }
    InterlockedExchange(&Power->Busy, 0);
    return Status;
}
