// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "ConnPmic.h"
#include <Library/MtkPmicBroker.h>

#define CONN_PMIC_POLL_US 10U
#define CONN_PMIC_JOB_100NS 400000ULL
#define CONN_PMIC_MAX_CALLS 4096U

C_ASSERT(sizeof(MTK_PMIC_BROKER_REQUEST) == 64U);

static const GUID PmicHandler = MTK_PMIC_PRM_HANDLER_GUID;

typedef struct {
    ULONGLONG Start;
    ULONG Calls;
} CONN_PMIC_POLL;

static ULONGLONG PmicNow(VOID)
{
    ULONGLONG Counter;
    return KeQueryInterruptTimePrecise(&Counter);
}

static NTSTATUS PmicCall(T70_CONN_PMIC *Pmic, CONN_PMIC_POLL *Poll,
                        USHORT Operation, ULONGLONG Generation, ULONG Address,
                        ULONG Value, MTK_PMIC_BROKER_REQUEST *Reply)
{
    while (Poll->Calls < CONN_PMIC_MAX_CALLS && PmicNow() - Poll->Start < CONN_PMIC_JOB_100NS) {
        GUID Guid = PmicHandler;
        ULONGLONG EfiStatus = MAXULONGLONG;
        *Reply = (MTK_PMIC_BROKER_REQUEST){0};
        Reply->Size = sizeof(*Reply);
        Reply->Version = MTK_PMIC_BROKER_VERSION_CONN_RAILS;
        Reply->Operation = Operation;
        Reply->Generation = Generation;
        Reply->Address = Address;
        Reply->Value = Value;
        ++Poll->Calls;
        if (Pmic->Interface.InvokeHandler(&Guid, Reply, 0, &EfiStatus) != STATUS_SUCCESS || EfiStatus) {
            return STATUS_IO_DEVICE_ERROR;
        }
        if (Reply->Size != sizeof(*Reply) || Reply->Version != MTK_PMIC_BROKER_VERSION_CONN_RAILS ||
            Reply->Operation != Operation || Reply->Reserved0 || Reply->Reserved[0] ||
            Reply->Reserved[1] || Reply->Reserved[2] || Reply->State > MtkPmicBrokerStateFault ||
            Reply->Result > MtkPmicBrokerResultExhausted ||
            (Reply->Flags && (Operation != MtkPmicBrokerQuery || Reply->Result != MtkPmicBrokerResultSuccess))) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        if (Reply->Result != MtkPmicBrokerResultBusy) {
            return STATUS_SUCCESS;
        }
        if (Reply->State != MtkPmicBrokerStateIdle || Reply->Generation != Generation ||
            Reply->Address != Address || Reply->Value != Value) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        KeStallExecutionProcessor(CONN_PMIC_POLL_US);
    }
    return STATUS_DEVICE_BUSY;
}

static NTSTATUS PmicResult(ULONG Result)
{
    switch (Result) {
    case MtkPmicBrokerResultInvalid: return STATUS_INVALID_PARAMETER;
    case MtkPmicBrokerResultWrongState:
    case MtkPmicBrokerResultStaleGeneration: return STATUS_INVALID_DEVICE_STATE;
    case MtkPmicBrokerResultExhausted: return STATUS_INTEGER_OVERFLOW;
    case MtkPmicBrokerResultIoError: return STATUS_IO_DEVICE_ERROR;
    case MtkPmicBrokerResultTimeout: return STATUS_IO_TIMEOUT;
    default: return STATUS_DEVICE_DATA_ERROR;
    }
}

static BOOLEAN BeginRejected(const MTK_PMIC_BROKER_REQUEST *Reply)
{
    return Reply->State == MtkPmicBrokerStateIdle &&
           (Reply->Result == MtkPmicBrokerResultInvalid ||
            Reply->Result == MtkPmicBrokerResultWrongState ||
            Reply->Result == MtkPmicBrokerResultStaleGeneration ||
            Reply->Result == MtkPmicBrokerResultExhausted);
}

static NTSTATUS PmicJob(T70_CONN_PMIC *Pmic, USHORT Operation, ULONG Address,
                       ULONG Value, PULONG ReadValue)
{
    BOOLEAN Acquire = Operation == MtkPmicBrokerAcquireConnRails;
    CONN_PMIC_POLL Poll = {PmicNow(), 0};
    MTK_PMIC_BROKER_REQUEST Reply;
    NTSTATUS Status = PmicCall(Pmic, &Poll, Operation, Pmic->Lease, Address, Value, &Reply);
    if (Status == STATUS_DEVICE_BUSY) {
        return Status;
    }
    if (Status != STATUS_SUCCESS) {
        goto Uncertain;
    }
    if (BeginRejected(&Reply)) {
        return PmicResult(Reply.Result);
    }
    ULONGLONG Job = Reply.Generation;
    if (Job <= Pmic->Lease || Reply.Address != Address || Reply.Value != Value ||
        (Acquire ? (Reply.State != MtkPmicBrokerStateDone || Reply.Result != MtkPmicBrokerResultSuccess)
                 : (Reply.State != MtkPmicBrokerStatePending || Reply.Result != MtkPmicBrokerResultInProgress))) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Uncertain;
    }
    while (Reply.State == MtkPmicBrokerStatePending) {
        KeStallExecutionProcessor(CONN_PMIC_POLL_US);
        Status = PmicCall(Pmic, &Poll, MtkPmicBrokerStep, Job, 0, 0, &Reply);
        if (Status != STATUS_SUCCESS) {
            goto Uncertain;
        }
        if (Reply.Generation != Job || Reply.Address != Address ||
            (Reply.State == MtkPmicBrokerStatePending &&
             (Reply.Result != MtkPmicBrokerResultInProgress || Reply.Value != Value))) {
            Status = STATUS_DEVICE_DATA_ERROR;
            goto Uncertain;
        }
    }
    if (Reply.State == MtkPmicBrokerStateFault) {
        Status = PmicResult(Reply.Result);
        goto Uncertain;
    }
    if (Reply.State != MtkPmicBrokerStateDone ||
        (Reply.Result != MtkPmicBrokerResultSuccess &&
         (Operation != MtkPmicBrokerBeginConnWrite ||
          (Reply.Result != MtkPmicBrokerResultInvalid && Reply.Result != MtkPmicBrokerResultIoError))) ||
        (ReadValue ? Reply.Value > MAXUSHORT : Reply.Value != Value)) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Uncertain;
    }
    ULONG Result = Reply.Result;
    ULONG CompleteValue = Reply.Value;
    Status = PmicCall(Pmic, &Poll, MtkPmicBrokerAck, Job, 0, 0, &Reply);
    if (Status != STATUS_SUCCESS) {
        goto Uncertain;
    }
    if (Reply.State != MtkPmicBrokerStateIdle || Reply.Result != MtkPmicBrokerResultSuccess ||
        Reply.Generation != Job || Reply.Address != Address || Reply.Value != CompleteValue) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto Uncertain;
    }
    if (Result != MtkPmicBrokerResultSuccess) {
        return PmicResult(Result);
    }
    if (Acquire) {
        Pmic->Lease = Job;
    }
    if (ReadValue) {
        *ReadValue = CompleteValue;
    }
    return STATUS_SUCCESS;

Uncertain:
    /* The call may own a WACS response or have changed a rail. Retain the
       lease and module after losing the accepted job or its completion. */
    Pmic->Uncertain = TRUE;
    return Status == STATUS_DEVICE_BUSY ? STATUS_IO_TIMEOUT : Status;
}

static NTSTATUS PmicUnlock(T70_CONN_PMIC *Pmic)
{
    GUID Guid = PmicHandler;
    NTSTATUS Status = Pmic->Interface.UnlockModule(&Guid);
    if (Status == STATUS_SUCCESS) {
        Pmic->ModuleLocked = FALSE;
    }
    return NT_SUCCESS(Status) && Status != STATUS_SUCCESS ? STATUS_UNSUCCESSFUL : Status;
}

NTSTATUS T70ConnPmicAcquire(T70_CONN_PMIC *Pmic)
{
    if (!Pmic || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Pmic->Uncertain) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Pmic->ModuleLocked) {
        return STATUS_DEVICE_BUSY;
    }
    PRM_INTERFACE Interface = {0};
    NTSTATUS Status = ExGetPrmInterface(PRM_INTERFACE_V1, &Interface);
    if (Status != STATUS_SUCCESS) {
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
    }
    if (Interface.Version != PRM_INTERFACE_V1 || !Interface.LockModule || !Interface.UnlockModule ||
        !Interface.QueryHandler || !Interface.InvokeHandler) {
        return STATUS_NOT_SUPPORTED;
    }
    GUID Guid = PmicHandler;
    Status = Interface.LockModule(&Guid);
    if (Status != STATUS_SUCCESS) {
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
    }
    Pmic->Interface = Interface;
    Pmic->ModuleLocked = TRUE;

    BOOLEAN Present = FALSE;
    Status = Interface.QueryHandler(&Guid, &Present);
    if (Status == STATUS_SUCCESS && Present == TRUE) {
        CONN_PMIC_POLL Poll = {PmicNow(), 0};
        MTK_PMIC_BROKER_REQUEST Reply;
        Status = PmicCall(Pmic, &Poll, MtkPmicBrokerQuery, 0, 0, 0, &Reply);
        if (Status == STATUS_SUCCESS) {
            if (Reply.Result == MtkPmicBrokerResultInvalid ||
                (Reply.Flags & MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE) == 0) {
                Status = STATUS_NOT_SUPPORTED;
            } else if (Reply.State != MtkPmicBrokerStateIdle || Reply.Result != MtkPmicBrokerResultSuccess ||
                       Reply.Flags != MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE ||
                       Reply.Generation || Reply.Address || Reply.Value) {
                Status = STATUS_DEVICE_DATA_ERROR;
            } else {
                Status = PmicJob(Pmic, MtkPmicBrokerAcquireConnRails, 0, 0, NULL);
            }
        }
    } else if (NT_SUCCESS(Status)) {
        Status = Status == STATUS_SUCCESS ? STATUS_NOT_SUPPORTED : STATUS_UNSUCCESSFUL;
    }
    if (Status != STATUS_SUCCESS && !Pmic->Uncertain) {
        NTSTATUS Unlock = PmicUnlock(Pmic);
        if (Unlock != STATUS_SUCCESS) {
            return Unlock;
        }
    }
    return Status;
}

NTSTATUS T70ConnPmicRelease(T70_CONN_PMIC *Pmic)
{
    if (!Pmic || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Pmic->Uncertain) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Pmic->Lease) {
        CONN_PMIC_POLL Poll = {PmicNow(), 0};
        MTK_PMIC_BROKER_REQUEST Reply;
        NTSTATUS Status = PmicCall(Pmic, &Poll, MtkPmicBrokerReleaseConnRails, Pmic->Lease, 0, 0, &Reply);
        if (Status != STATUS_SUCCESS) {
            return Status;
        }
        if (BeginRejected(&Reply)) {
            return PmicResult(Reply.Result);
        }
        if (Reply.State != MtkPmicBrokerStateIdle || Reply.Result != MtkPmicBrokerResultSuccess ||
            Reply.Generation != Pmic->Lease || Reply.Address || Reply.Value) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        Pmic->Lease = 0;
    }
    return Pmic->ModuleLocked ? PmicUnlock(Pmic) : STATUS_SUCCESS;
}

int T70ConnPmicRead(void *Context, unsigned Address, unsigned *Value)
{
    T70_CONN_PMIC *Pmic = Context;
    ULONG Result;
    if (!Pmic || !Value || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Pmic->Lease || Pmic->Uncertain) {
        return STATUS_DEVICE_NOT_READY;
    }
    NTSTATUS Status = PmicJob(Pmic, MtkPmicBrokerBeginConnRead, Address, 0, &Result);
    if (Status == STATUS_SUCCESS) {
        *Value = Result;
    }
    return Status;
}

int T70ConnPmicWrite(void *Context, unsigned Address, unsigned Value)
{
    T70_CONN_PMIC *Pmic = Context;
    if (!Pmic || Value > MAXUSHORT || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!Pmic->Lease || Pmic->Uncertain) {
        return STATUS_DEVICE_NOT_READY;
    }
    return PmicJob(Pmic, MtkPmicBrokerBeginConnWrite, Address, Value, NULL);
}
