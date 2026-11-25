/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PmicPrm.h"
#if defined(MTK_PMIC_PRM_HOST_TEST)
#include "PmicPrmHostShim.h"
#else
#include <ntddk.h>
#include <prminterface.h>
#endif
#include <Library/MtkPmicBroker.h>
#include <Library/MtkGaugeCore.h>

#define PMIC_PRM_TIMEOUT_100NS 100000ULL
#define PMIC_PRM_POLL_DELAY_US 10U
#define PMIC_PRM_MAX_POLLS 1000U
#define PMIC_KEY_STATUS_ADDRESS 0x91CU
#define PMIC_KEY_IRQ_ENABLE_ADDRESS 0x910U
#define PMIC_KEY_IRQ_ENABLE_MASK 0x5U
#define PMIC_KEY_IRQ_CAPABILITIES \
    (MTK_PMIC_BROKER_CAP_KEY_IRQ_LEASE | MTK_PMIC_BROKER_CAP_CAPTURED_KEY_ACK)
#define PMIC_AUXADC_REQUEST_ADDRESS 0x1108U
#define PMIC_AUXADC_BIF_VOLTAGE_ADDRESS 0x109EU
#define PMIC_AUXADC_BIF_CONTROL_ADDRESS 0x0FA8U
#define PMIC_AUXADC_BIF_DISABLE 0x0002U
#define PMIC_AUXADC_DIE_TEMP_TRIM 0x11CEU
#define PMIC_AUXADC_TEMP_REF_TRIM 0x11D0U
#define PMIC_AUXADC_TEMP_SLOPE_TRIM 0x11D2U
#define PMIC_OTP_ROW 0x038AU
#define PMIC_OTP_READ_TRIGGER 0x039AU
#define PMIC_OTP_READ_ENABLE 0x03A0U
#define PMIC_OTP_READ_DATA 0x03A2U
#define PMIC_OTP_READ_BUSY 0x03A4U
#define PMIC_OTP_FUSE_ROW_FIRST 0x0050U
#define PMIC_OTP_FUSE_ROW_LAST 0x0056U
#define PMIC_AUXADC_READY 0x8000U
#define PMIC_AUXADC_POLL_TIMEOUT_100NS 320000U
#define PMIC_AUXADC_POLL_DELAY_US 26U
#define PMIC_OTP_POLL_TIMEOUT_100NS 300000U
#define PMIC_OTP_POLL_DELAY_US 13U
#define T70_GAUGE_CURRENT_SENSE_INTERNAL 50U
#define T70_GAUGE_CURRENT_TUNE_INTERNAL 1010U

typedef char PmicBrokerRequestIs64Bytes[(sizeof(MTK_PMIC_BROKER_REQUEST)==64)?1:-1];

static const GUID PmicHandlerGuid=MTK_PMIC_PRM_HANDLER_GUID;
/* A failed accepted transaction intentionally pins the PRM module for boot
   lifetime. No later caller may relock or update its outstanding journal. */
static volatile LONG PmicClientState;
/* These tokens are local capability handles, not PMIC values. They are
   committed only after the matching firmware metadata Ack and exact Unlock. */
static uint64_t PmicKeyLeaseToken;
static uint64_t PmicKeySnapshotToken;
static uint64_t PmicKeySnapshotLeaseToken;
static uint32_t PmicKeySnapshotPacked;
static MTK_GAUGE_CORE PmicBatteryGaugeCore;
static volatile LONG PmicBatteryCurrentBusy;
typedef struct {
    uint64_t LeaseToken;
    MALI_RESULT LastStatus;
} PMIC_BATTERY_CURRENT_CONTEXT;
static PMIC_BATTERY_CURRENT_CONTEXT PmicBatteryCurrentContext;
static int PmicBatteryCurrentRead16(void *Context,uint16_t Address,uint16_t *Value);
static int PmicBatteryCurrentWrite16(void *Context,uint16_t Address,uint16_t Value);
static volatile LONG PmicBatteryBifRestoreFault;
static volatile LONG PmicBatteryTrimRestoreFault;
static MTK_GAUGE_VOLTAGE_TRIM PmicBatteryVoltageTrim;
static BOOLEAN PmicBatteryVoltageTrimValid;

static NTSTATUS PmicPrmExact(NTSTATUS Status) {
    return Status==STATUS_SUCCESS?STATUS_SUCCESS:(NT_SUCCESS(Status)?STATUS_UNSUCCESSFUL:Status);
}
static BOOLEAN PmicBrokerReadable(uint32_t Address) {
    return Address==0x28U || Address==0x1508U || Address==0x1520U || Address==0x1526U ||
           Address==0x1BC8U || Address==0x1BE2U || Address==0x1BE4U || Address==0x1BF4U;
}
static BOOLEAN PmicBrokerWritable(uint32_t Address) {
    return Address==0x1508U || Address==0x1526U || Address==0x1BC8U || Address==0x1BF4U;
}
static MALI_RESULT PmicPrmStatus(NTSTATUS Status) {
    if(Status==STATUS_DEVICE_BUSY || Status==STATUS_RETRY)return MaliBusy;
    if(Status==STATUS_NOT_SUPPORTED || Status==STATUS_NOT_FOUND)return MaliUnsupported;
    if(Status==STATUS_DEVICE_NOT_READY || Status==STATUS_DELETE_PENDING)return MaliOffline;
    return MaliFault;
}
static MALI_RESULT PmicBrokerResult(uint32_t Result) {
    switch(Result) {
    case MtkPmicBrokerResultSuccess:return MaliOk;
    case MtkPmicBrokerResultInProgress:return MaliBusy;
    case MtkPmicBrokerResultInvalid:return MaliInvalid;
    case MtkPmicBrokerResultBusy:return MaliBusy;
    case MtkPmicBrokerResultWrongState:return MaliFault;
    case MtkPmicBrokerResultIoError:return MaliFault;
    case MtkPmicBrokerResultTimeout:return MaliTimeout;
    case MtkPmicBrokerResultStaleGeneration:return MaliFault;
    case MtkPmicBrokerResultExhausted:return MaliFault;
    default:return MaliFault;
    }
}

/* The PRM module lock pins the persistent journal across the whole transfer. */
static NTSTATUS PmicPrmInvokeLocked(PRM_INTERFACE *Interface,
                                     MTK_PMIC_BROKER_REQUEST *Request,
                                     BOOLEAN *Invoked) {
    ULONG64 EfiStatus=MAXULONGLONG;
    NTSTATUS Status;
    *Invoked=FALSE;
    Status=PmicPrmExact(Interface->InvokeHandler((GUID *)&PmicHandlerGuid,Request,0,&EfiStatus));
    *Invoked=TRUE;
    /* A failed Invoke status is ambiguous: the handler may already have
       accepted the request. Keep it distinct from a validated wire Busy. */
    if(Status!=STATUS_SUCCESS)return STATUS_UNSUCCESSFUL;
    return EfiStatus==0?STATUS_SUCCESS:STATUS_UNSUCCESSFUL;
}
static BOOLEAN PmicWireBaseValid(const MTK_PMIC_BROKER_REQUEST *Request,
                                 uint16_t Operation,uint32_t Flags) {
    return Request->Size==sizeof(*Request) && Request->Version==MTK_PMIC_BROKER_VERSION &&
        Request->Operation==Operation && Request->Flags==Flags && Request->Reserved0==0 &&
        Request->Reserved[0]==0 && Request->Reserved[1]==0 && Request->Reserved[2]==0;
}
static BOOLEAN PmicBusyUnchanged(const MTK_PMIC_BROKER_REQUEST *Request,
                                  uint16_t Operation,uint64_t Generation,
                                  uint32_t Address,uint32_t Value) {
    return PmicWireBaseValid(Request,Operation,0) && Request->State==0 &&
        Request->Generation==Generation && Request->Result==MtkPmicBrokerResultBusy &&
        Request->Address==Address && Request->Value==Value;
}
static NTSTATUS PmicPrmCall(PRM_INTERFACE *Interface,
                            MTK_PMIC_BROKER_REQUEST *Request,
                            uint16_t Operation,uint64_t Generation,
                            uint32_t Address,uint32_t Value,
                            BOOLEAN *Invoked) {
    NTSTATUS Status=PmicPrmInvokeLocked(Interface,Request,Invoked);
    if(Status!=STATUS_SUCCESS)return Status;
    if(!PmicWireBaseValid(Request,Operation,0))return STATUS_DATA_ERROR;
    if(Request->Result==MtkPmicBrokerResultBusy)
        return PmicBusyUnchanged(Request,Operation,Generation,Address,Value)?STATUS_DEVICE_BUSY:STATUS_DATA_ERROR;
    if(Request->Reserved0 || Request->State>MtkPmicBrokerStateFault ||
       Request->Result>MtkPmicBrokerResultExhausted)return STATUS_DATA_ERROR;
    return STATUS_SUCCESS;
}
static MALI_RESULT PmicBrokerTransaction(uint32_t Operation,uint32_t Address,
                                         uint16_t WriteValue,uint16_t *ReadValue,
                                         MTK_PMIC_KEY_SNAPSHOT *KeySnapshot) {
    PRM_INTERFACE Interface;
    MTK_PMIC_BROKER_REQUEST Request;
    ULONGLONG Started;
    uint64_t Generation=0;
    uint32_t Polls;
    BOOLEAN IsRead=Operation==MtkPmicBrokerBeginRead;
    BOOLEAN IsWrite=Operation==MtkPmicBrokerBeginWrite;
    BOOLEAN IsKeySnapshot=Operation==MtkPmicBrokerBeginKeySnapshot;
    BOOLEAN Found=FALSE,Locked=FALSE,Invoked=FALSE,Done=FALSE,JournalPinned=FALSE,Quarantined=FALSE;
    NTSTATUS Status,UnlockStatus=STATUS_SUCCESS;
    MALI_RESULT Result=MaliFault;
    MALI_RESULT OperationResult=MaliFault;
    LONG ClientState;
    uint16_t BeginOperation=(uint16_t)Operation;
    uint32_t ExpectedValue=IsWrite?WriteValue:0;
    uint32_t BeginAddress=IsKeySnapshot?0:Address;
    uint32_t ResponseAddress=IsKeySnapshot?PMIC_KEY_STATUS_ADDRESS:Address;
    uint32_t FinalValue=0;

    if(KeGetCurrentIrql()!=PASSIVE_LEVEL ||
       (IsRead && (!PmicBrokerReadable(Address) || !ReadValue || WriteValue)) ||
       (IsWrite && (!PmicBrokerReadable(Address) || !PmicBrokerWritable(Address))) ||
       (IsKeySnapshot && (!KeySnapshot || Address || WriteValue)) ||
       (!IsRead && !IsWrite && !IsKeySnapshot))return MaliInvalid;
    ClientState=InterlockedCompareExchange(&PmicClientState,1,0);
    if(ClientState!=0)return ClientState==2?MaliFault:MaliBusy;
    RtlZeroMemory(&Interface,sizeof(Interface));
    Status=PmicPrmExact(ExGetPrmInterface(PRM_INTERFACE_V1,&Interface));
    if(Status!=STATUS_SUCCESS) {Result=PmicPrmStatus(Status);goto Exit;}
    if(Interface.Version!=PRM_INTERFACE_V1 || !Interface.LockModule || !Interface.UnlockModule ||
       !Interface.QueryHandler || !Interface.InvokeHandler) {Result=MaliUnsupported;goto Exit;}
    Status=PmicPrmExact(Interface.LockModule((GUID *)&PmicHandlerGuid));
    if(Status!=STATUS_SUCCESS) {Result=PmicPrmStatus(Status);goto Exit;}
    Locked=TRUE;
    Status=PmicPrmExact(Interface.QueryHandler((GUID *)&PmicHandlerGuid,&Found));
    if(Status!=STATUS_SUCCESS || !Found) {
        Result=Status==STATUS_SUCCESS?MaliUnsupported:PmicPrmStatus(Status);
        goto Exit;
    }

    Started=KeQueryInterruptTime();
    for(Polls=0;Polls<PMIC_PRM_MAX_POLLS;Polls++) {
        RtlZeroMemory(&Request,sizeof(Request));
        Request.Size=sizeof(Request);Request.Version=MTK_PMIC_BROKER_VERSION;
        Request.Operation=BeginOperation;Request.Address=BeginAddress;Request.Value=ExpectedValue;
        Status=PmicPrmCall(&Interface,&Request,BeginOperation,0,BeginAddress,ExpectedValue,&Invoked);
        if(Status==STATUS_DEVICE_BUSY) {
            if(KeQueryInterruptTime()-Started>=PMIC_PRM_TIMEOUT_100NS) {Result=MaliBusy;goto Exit;}
            KeStallExecutionProcessor(PMIC_PRM_POLL_DELAY_US);continue;
        }
        /* A v1 broker predating opcode 5 rejects the new operation without
           accepting it. Its idle cache fields may describe the last transfer,
           so recognize only a structurally valid Invalid+Idle rejection. */
        if(IsKeySnapshot && Status==STATUS_SUCCESS &&
           Request.Result==MtkPmicBrokerResultInvalid &&
           Request.State==MtkPmicBrokerStateIdle) {
            Result=MaliUnsupported;goto Exit;
        }
        if(Invoked)JournalPinned=TRUE;
        if(Status!=STATUS_SUCCESS) {Result=Invoked?MaliFault:PmicPrmStatus(Status);goto Exit;}
        if(Request.Result!=MtkPmicBrokerResultInProgress ||
           Request.State!=MtkPmicBrokerStatePending || !Request.Generation ||
           Request.Address!=ResponseAddress ||
           Request.Value!=(IsKeySnapshot?0:ExpectedValue)) {Result=MaliFault;goto Exit;}
        Generation=Request.Generation;
        Polls=0;
        break;
    }
    if(!Generation) {Result=MaliBusy;goto Exit;}

    for(;Polls<PMIC_PRM_MAX_POLLS;Polls++) {
        RtlZeroMemory(&Request,sizeof(Request));
        Request.Size=sizeof(Request);Request.Version=MTK_PMIC_BROKER_VERSION;
        Request.Operation=MtkPmicBrokerStep;Request.Generation=Generation;
        Status=PmicPrmCall(&Interface,&Request,MtkPmicBrokerStep,Generation,0,0,&Invoked);
        if(Status==STATUS_DEVICE_BUSY) {
            if(KeQueryInterruptTime()-Started>=PMIC_PRM_TIMEOUT_100NS) {Result=MaliTimeout;goto Exit;}
            KeStallExecutionProcessor(PMIC_PRM_POLL_DELAY_US);continue;
        }
        if(Status!=STATUS_SUCCESS) {Result=MaliFault;goto Exit;}
        if(Request.State==MtkPmicBrokerStateFault) {Result=MaliFault;goto Exit;}
        if(Request.Generation!=Generation || Request.Address!=ResponseAddress ||
           (IsWrite && Request.Value!=ExpectedValue)) {Result=MaliFault;goto Exit;}
        if(Request.Result==MtkPmicBrokerResultInProgress && Request.State==MtkPmicBrokerStatePending) {
            if(((IsRead || IsKeySnapshot) && Request.Value!=0) ||
               (IsWrite && Request.Value!=ExpectedValue)) {Result=MaliFault;goto Exit;}
            if(KeQueryInterruptTime()-Started>=PMIC_PRM_TIMEOUT_100NS) {Result=MaliTimeout;goto Exit;}
            KeStallExecutionProcessor(PMIC_PRM_POLL_DELAY_US);continue;
        }
        if(Request.State!=MtkPmicBrokerStateDone) {Result=MaliFault;goto Exit;}
        if(IsKeySnapshot && Request.Result!=MtkPmicBrokerResultSuccess) {Result=MaliFault;goto Exit;}
        if(Request.Result!=MtkPmicBrokerResultSuccess && Request.Result!=MtkPmicBrokerResultInvalid) {
            Result=MaliFault;goto Exit;
        }
        Result=PmicBrokerResult(Request.Result);
        if(Result==MaliOk) {
            if(IsRead) {
                if(Request.Value>UINT16_MAX) {Result=MaliFault;goto Exit;}
                FinalValue=Request.Value;
            } else if(IsWrite) {
                if(Request.Value!=ExpectedValue) {Result=MaliFault;goto Exit;}
            } else {
                FinalValue=Request.Value;
            }
        } else if((IsRead || IsKeySnapshot) && Request.Value!=0) {
            Result=MaliFault;goto Exit;
        }
        Done=TRUE;break;
    }
    if(!Done) {Result=MaliTimeout;goto Exit;}
    OperationResult=Result;

    for(Polls=0;Polls<PMIC_PRM_MAX_POLLS;Polls++) {
        RtlZeroMemory(&Request,sizeof(Request));
        Request.Size=sizeof(Request);Request.Version=MTK_PMIC_BROKER_VERSION;
        Request.Operation=MtkPmicBrokerAck;Request.Generation=Generation;
        Status=PmicPrmCall(&Interface,&Request,MtkPmicBrokerAck,Generation,0,0,&Invoked);
        if(Status==STATUS_DEVICE_BUSY) {
            if(KeQueryInterruptTime()-Started>=PMIC_PRM_TIMEOUT_100NS) {Result=MaliTimeout;goto Exit;}
            KeStallExecutionProcessor(PMIC_PRM_POLL_DELAY_US);continue;
        }
        if(Status!=STATUS_SUCCESS || Request.State!=MtkPmicBrokerStateIdle ||
           Request.Generation!=Generation || Request.Address!=ResponseAddress ||
           Request.Value!=(IsWrite?ExpectedValue:FinalValue) ||
           Request.Result!=MtkPmicBrokerResultSuccess) {Result=MaliFault;goto Exit;}
        JournalPinned=FALSE;
        Result=OperationResult==MaliInvalid?MaliFault:OperationResult;
        break;
    }
    if(Polls==PMIC_PRM_MAX_POLLS) {Result=MaliTimeout;goto Exit;}

Exit:
    if(Locked && !JournalPinned) {
        UnlockStatus=PmicPrmExact(Interface.UnlockModule((GUID *)&PmicHandlerGuid));
        if(UnlockStatus!=STATUS_SUCCESS) {Result=MaliFault;Quarantined=TRUE;}
    }
    if(JournalPinned)Quarantined=TRUE;
    if(Result==MaliOk && IsRead)*ReadValue=(uint16_t)FinalValue;
    if(Result==MaliOk && IsKeySnapshot) {
        KeySnapshot->GroupStatus=(uint16_t)(FinalValue>>16);
        KeySnapshot->TopStatus=(uint16_t)FinalValue;
    }
    InterlockedExchange(&PmicClientState,Quarantined?2:0);
    return Result;
}

MALI_RESULT MtkMaliPmicPrmRead16(uint32_t Address,uint16_t *Value) {
    if(!Value)return MaliInvalid;
    return PmicBrokerTransaction(MtkPmicBrokerBeginRead,Address,0,Value,0);
}
MALI_RESULT MtkMaliPmicPrmWrite16(uint32_t Address,uint16_t Value) {
    return PmicBrokerTransaction(MtkPmicBrokerBeginWrite,Address,Value,0,0);
}
MALI_RESULT MtkMaliPmicPrmReadKeySnapshot(MTK_PMIC_KEY_SNAPSHOT *Snapshot) {
    if(!Snapshot)return MaliInvalid;
    return PmicBrokerTransaction(MtkPmicBrokerBeginKeySnapshot,0,0,0,Snapshot);
}

static MALI_RESULT PmicKeyClientEnter(void) {
    LONG ClientState;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return MaliInvalid;
    ClientState=InterlockedCompareExchange(&PmicClientState,1,0);
    if(ClientState!=0)return ClientState==2?MaliFault:MaliBusy;
    return MaliOk;
}
static void PmicKeyClientLeave(void) {
    /* Never erase a quarantine while releasing a successful caller lock. */
    (void)InterlockedCompareExchange(&PmicClientState,0,1);
}
static MALI_RESULT PmicKeyClientQuarantine(void) {
    InterlockedExchange(&PmicClientState,2);
    return MaliFault;
}

/* Open and pin the PRM handler while the caller owns PmicClientState. */
static MALI_RESULT PmicKeyOpen(PRM_INTERFACE *Interface,BOOLEAN *Locked) {
    BOOLEAN Found=FALSE;
    NTSTATUS Status;
    *Locked=FALSE;
    RtlZeroMemory(Interface,sizeof(*Interface));
    Status=PmicPrmExact(ExGetPrmInterface(PRM_INTERFACE_V1,Interface));
    if(Status!=STATUS_SUCCESS) {
        PmicKeyClientLeave();
        return PmicPrmStatus(Status);
    }
    if(Interface->Version!=PRM_INTERFACE_V1 || !Interface->LockModule ||
       !Interface->UnlockModule || !Interface->QueryHandler || !Interface->InvokeHandler) {
        PmicKeyClientLeave();
        return MaliUnsupported;
    }
    Status=PmicPrmExact(Interface->LockModule((GUID *)&PmicHandlerGuid));
    if(Status!=STATUS_SUCCESS) {
        PmicKeyClientLeave();
        return PmicPrmStatus(Status);
    }
    *Locked=TRUE;
    Status=PmicPrmExact(Interface->QueryHandler((GUID *)&PmicHandlerGuid,&Found));
    if(Status!=STATUS_SUCCESS || !Found) {
        MALI_RESULT Result=Status==STATUS_SUCCESS?MaliUnsupported:PmicPrmStatus(Status);
        NTSTATUS Unlock=PmicPrmExact(Interface->UnlockModule((GUID *)&PmicHandlerGuid));
        if(Unlock!=STATUS_SUCCESS)return PmicKeyClientQuarantine();
        *Locked=FALSE;
        PmicKeyClientLeave();
        return Result;
    }
    return MaliOk;
}

/* Unlock only after every accepted firmware job has been metadata-acked.
   Any uncertain accepted job or failed unlock permanently quarantines the
   module lease, matching the v1 transaction policy. */
static MALI_RESULT PmicKeyClose(PRM_INTERFACE *Interface,BOOLEAN *Locked,
                                BOOLEAN Pinned,MALI_RESULT Result) {
    if(*Locked && !Pinned) {
        NTSTATUS Status=PmicPrmExact(Interface->UnlockModule((GUID *)&PmicHandlerGuid));
        if(Status!=STATUS_SUCCESS)Pinned=TRUE;
        else *Locked=FALSE;
    }
    if(Pinned) {
        (void)PmicKeyClientQuarantine();
        /* Preserve the bounded accepted-job timeout for the caller, as v1
           does, while keeping the client and firmware journal quarantined. */
        return Result==MaliTimeout?MaliTimeout:MaliFault;
    }
    /* The caller publishes its retained tokens before dropping client CAS. */
    return Result;
}

static BOOLEAN PmicV2HeaderValid(const MTK_PMIC_BROKER_REQUEST *Request,
                                 uint16_t Version,uint16_t Operation,uint32_t Flags) {
    return Request->Size==sizeof(*Request) &&
        Request->Version==Version &&
        Request->Operation==Operation && Request->Flags==Flags &&
        Request->Reserved0==0 && Request->Reserved[0]==0 &&
        Request->Reserved[1]==0 && Request->Reserved[2]==0;
}
static BOOLEAN PmicV2BusyUnchanged(const MTK_PMIC_BROKER_REQUEST *Request,
                                    uint16_t Version,uint16_t Operation,uint64_t Generation,
                                    uint32_t Address,uint32_t Value) {
    return PmicV2HeaderValid(Request,Version,Operation,0) &&
        Request->State==MtkPmicBrokerStateIdle &&
        Request->Generation==Generation &&
        Request->Result==MtkPmicBrokerResultBusy &&
        Request->Address==Address && Request->Value==Value;
}
static NTSTATUS PmicV2Call(PRM_INTERFACE *Interface,
                           MTK_PMIC_BROKER_REQUEST *Request,
                           uint16_t Version,uint16_t Operation,uint64_t Generation,
                           uint32_t Address,uint32_t Value,
                           BOOLEAN *Invoked) {
    NTSTATUS Status=PmicPrmInvokeLocked(Interface,Request,Invoked);
    if(Status!=STATUS_SUCCESS)return STATUS_UNSUCCESSFUL;
    if(!PmicV2HeaderValid(Request,Version,Operation,0) ||
       Request->State>MtkPmicBrokerStateFault ||
       Request->Result>MtkPmicBrokerResultSiblingPending)
        return STATUS_DATA_ERROR;
    if(Request->Result==MtkPmicBrokerResultBusy)
        return PmicV2BusyUnchanged(Request,Version,Operation,Generation,Address,Value)?
            STATUS_DEVICE_BUSY:STATUS_DATA_ERROR;
    return STATUS_SUCCESS;
}
static MALI_RESULT PmicV2Rejected(uint32_t Result) {
    switch(Result) {
    case MtkPmicBrokerResultInvalid:
    case MtkPmicBrokerResultWrongState:
    case MtkPmicBrokerResultStaleGeneration:
        return MaliInvalid;
    case MtkPmicBrokerResultExhausted:
        return MaliFault;
    default:
        return MaliFault;
    }
}
static void PmicKeyStall(void) {
    KeStallExecutionProcessor(PMIC_PRM_POLL_DELAY_US);
}
static BOOLEAN PmicKeyTimedOut(ULONGLONG Started,uint32_t Polls) {
    ULONGLONG Now=KeQueryInterruptTime();
    return Polls>=PMIC_PRM_MAX_POLLS || Now<Started ||
        Now-Started>=PMIC_PRM_TIMEOUT_100NS;
}

/* v2 Query is deliberately no-MMIO. A well-formed Invalid+Idle response is
   the only accepted compatibility signal from a pre-v2 firmware broker; its
   cached address/value/generation are not treated as query results. */
static MALI_RESULT PmicV2QueryLocked(PRM_INTERFACE *Interface,uint16_t Version,
                                      uint32_t ExpectedFlags,BOOLEAN *Pinned) {
    MTK_PMIC_BROKER_REQUEST Request;
    BOOLEAN Invoked=FALSE;
    NTSTATUS Status;
    RtlZeroMemory(&Request,sizeof(Request));
    Request.Size=sizeof(Request);
    Request.Version=Version;
    Request.Operation=MtkPmicBrokerQuery;
    Status=PmicPrmInvokeLocked(Interface,&Request,&Invoked);
    if(Status!=STATUS_SUCCESS) {
        *Pinned=TRUE;
        return MaliFault;
    }
    if(Request.Result==MtkPmicBrokerResultInvalid &&
       PmicV2HeaderValid(&Request,Version,MtkPmicBrokerQuery,0) &&
       Request.State==MtkPmicBrokerStateIdle)
        return MaliUnsupported;
    if(Request.Result==MtkPmicBrokerResultBusy) {
        if(PmicV2BusyUnchanged(&Request,Version,MtkPmicBrokerQuery,0,0,0))
            return MaliBusy;
        *Pinned=TRUE;
        return MaliFault;
    }
    if(!PmicV2HeaderValid(&Request,Version,MtkPmicBrokerQuery,ExpectedFlags) ||
       Request.State!=MtkPmicBrokerStateIdle ||
       Request.Result!=MtkPmicBrokerResultSuccess || Request.Generation ||
       Request.Address || Request.Value) {
        *Pinned=TRUE;
        return MaliFault;
    }
    return MaliOk;
}

/* Execute one accepted v2 job. The request's caller token is used only on the
   Begin operation; Step and metadata Ack use the fresh job generation. */
static MALI_RESULT PmicV2RunJobLocked(
    PRM_INTERFACE *Interface,uint16_t Version,uint16_t Operation,uint64_t InputToken,
    uint32_t ResponseAddress,uint32_t AcceptedValue,uint32_t FixedFinalValue,
    BOOLEAN FinalValueIsInput,BOOLEAN *Pinned,uint64_t *JobGeneration,
    uint32_t *FinalValue,uint32_t *FinalResult) {
    MTK_PMIC_BROKER_REQUEST Request;
    ULONGLONG Started=KeQueryInterruptTime();
    BOOLEAN Invoked=FALSE,Accepted=FALSE,Done=FALSE;
    uint64_t Generation=0;
    uint32_t ResultValue=0,TerminalResult=MtkPmicBrokerResultSuccess;
    NTSTATUS Status;
    uint32_t Polls;

    for(Polls=0;Polls<PMIC_PRM_MAX_POLLS;Polls++) {
        RtlZeroMemory(&Request,sizeof(Request));
        Request.Size=sizeof(Request);
        Request.Version=Version;
        Request.Operation=Operation;
        Request.Generation=InputToken;
        if(Version==MTK_PMIC_BROKER_VERSION_GAUGE &&
           (Operation==MtkPmicBrokerBeginGaugeRead ||
            Operation==MtkPmicBrokerBeginGaugeWrite)) {
            Request.Address=ResponseAddress;
            Request.Value=AcceptedValue;
        }
        Status=PmicV2Call(Interface,&Request,Version,Operation,InputToken,
            Request.Address,Request.Value,&Invoked);
        if(Status==STATUS_DEVICE_BUSY) {
            if(PmicKeyTimedOut(Started,Polls))return MaliBusy;
            PmicKeyStall();
            continue;
        }
        if(Status!=STATUS_SUCCESS) {
            *Pinned=Invoked;
            return MaliFault;
        }
        if(Request.Result==MtkPmicBrokerResultInvalid ||
           Request.Result==MtkPmicBrokerResultWrongState ||
           Request.Result==MtkPmicBrokerResultStaleGeneration ||
           Request.Result==MtkPmicBrokerResultExhausted) {
            if(Request.State!=MtkPmicBrokerStateIdle) {
                *Pinned=TRUE;
                return MaliFault;
            }
            return PmicV2Rejected(Request.Result);
        }
        /* A fully borrowed enable mask can release without PMIC I/O. It
           still owns a fresh terminal job until its metadata Ack succeeds. */
        if((Operation==MtkPmicBrokerReleaseKeyLease ||
            Operation==MtkPmicBrokerAcquireGaugeLease) &&
           Request.Result==MtkPmicBrokerResultSuccess &&
           Request.State==MtkPmicBrokerStateDone && Request.Generation>InputToken &&
           Request.Address==ResponseAddress && Request.Value==FixedFinalValue) {
            Generation=Request.Generation;
            Accepted=TRUE;Done=TRUE;*Pinned=TRUE;
            ResultValue=Request.Value;
            TerminalResult=Request.Result;
            break;
        }
        if(Request.Result!=MtkPmicBrokerResultInProgress ||
           Request.State!=MtkPmicBrokerStatePending ||
           !Request.Generation || Request.Generation<=InputToken ||
           Request.Address!=ResponseAddress || Request.Value!=AcceptedValue) {
            *Pinned=TRUE;
            return MaliFault;
        }
        Generation=Request.Generation;
        Accepted=TRUE;
        *Pinned=TRUE;
        break;
    }
    if(!Accepted)return MaliBusy;

    for(Polls=0;!Done && Polls<PMIC_PRM_MAX_POLLS;Polls++) {
        RtlZeroMemory(&Request,sizeof(Request));
        Request.Size=sizeof(Request);
        Request.Version=Version;
        Request.Operation=MtkPmicBrokerStep;
        Request.Generation=Generation;
        Status=PmicV2Call(Interface,&Request,Version,MtkPmicBrokerStep,Generation,0,0,&Invoked);
        if(Status==STATUS_DEVICE_BUSY) {
            if(PmicKeyTimedOut(Started,Polls))return MaliTimeout;
            PmicKeyStall();
            continue;
        }
        if(Status!=STATUS_SUCCESS) {
            *Pinned=TRUE;
            return MaliFault;
        }
        if(Request.State==MtkPmicBrokerStateFault) {
            *Pinned=TRUE;
            return PmicBrokerResult(Request.Result);
        }
        if(Request.State==MtkPmicBrokerStatePending &&
           Request.Result==MtkPmicBrokerResultInProgress) {
            if(Request.Generation!=Generation ||
               Request.Address!=ResponseAddress ||
               Request.Value!=AcceptedValue) {
                *Pinned=TRUE;
                return MaliFault;
            }
            if(PmicKeyTimedOut(Started,Polls))return MaliTimeout;
            PmicKeyStall();
            continue;
        }
        if(Request.State!=MtkPmicBrokerStateDone ||
           (Request.Result!=MtkPmicBrokerResultSuccess &&
            !(Operation==MtkPmicBrokerAckKeySnapshot &&
              Request.Result==MtkPmicBrokerResultSiblingPending)) ||
           Request.Generation!=Generation || Request.Address!=ResponseAddress ||
           (FinalValueIsInput && Request.Value!=FixedFinalValue) ||
           ((Operation==MtkPmicBrokerAcquireKeyLease ||
             Operation==MtkPmicBrokerReleaseKeyLease) &&
            Request.Value!=PMIC_KEY_IRQ_ENABLE_MASK)) {
            *Pinned=TRUE;
            return MaliFault;
        }
        ResultValue=Request.Value;
        TerminalResult=Request.Result;
        Done=TRUE;
        break;
    }
    if(!Done)return MaliTimeout;

    for(Polls=0;Polls<PMIC_PRM_MAX_POLLS;Polls++) {
        RtlZeroMemory(&Request,sizeof(Request));
        Request.Size=sizeof(Request);
        Request.Version=Version;
        Request.Operation=MtkPmicBrokerAck;
        Request.Generation=Generation;
        Status=PmicV2Call(Interface,&Request,Version,MtkPmicBrokerAck,Generation,0,0,&Invoked);
        if(Status==STATUS_DEVICE_BUSY) {
            if(PmicKeyTimedOut(Started,Polls))return MaliTimeout;
            PmicKeyStall();
            continue;
        }
        if(Status!=STATUS_SUCCESS || Request.State!=MtkPmicBrokerStateIdle ||
           Request.Generation!=Generation || Request.Address!=ResponseAddress ||
           Request.Value!=ResultValue ||
           Request.Result!=MtkPmicBrokerResultSuccess) {
            *Pinned=TRUE;
            return MaliFault;
        }
        *Pinned=FALSE;
        *JobGeneration=Generation;
        *FinalValue=ResultValue;
        *FinalResult=TerminalResult;
        return MaliOk;
    }
    return MaliTimeout;
}

MALI_RESULT MtkMaliPmicPrmQueryKeyIrq(void) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    MALI_RESULT Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,PMIC_KEY_IRQ_CAPABILITIES,&Pinned);
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    PmicKeyClientLeave();
    return Result;
}

MALI_RESULT MtkMaliPmicPrmAcquireKeyIrqLease(uint64_t *LeaseToken) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    uint64_t JobGeneration=0;
    uint32_t FinalValue=0,FinalResult=0;
    MALI_RESULT Result;
    if(!LeaseToken)return MaliInvalid;
    Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    if(PmicKeyLeaseToken) {
        PmicKeyClientLeave();
        return MaliBusy;
    }
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,PMIC_KEY_IRQ_CAPABILITIES,&Pinned);
    if(Result!=MaliOk)goto Exit;
    Result=PmicV2RunJobLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,MtkPmicBrokerAcquireKeyLease,0,
            PMIC_KEY_IRQ_ENABLE_ADDRESS,PMIC_KEY_IRQ_ENABLE_MASK,
            PMIC_KEY_IRQ_ENABLE_MASK,TRUE,&Pinned,&JobGeneration,
            &FinalValue,&FinalResult);
    if(Result!=MaliOk)goto Exit;
    if(FinalResult!=MtkPmicBrokerResultSuccess ||
       FinalValue!=PMIC_KEY_IRQ_ENABLE_MASK) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    if(Result==MaliOk) {
        PmicKeyLeaseToken=JobGeneration;
        PmicKeySnapshotToken=0;
        PmicKeySnapshotLeaseToken=0;
        PmicKeySnapshotPacked=0;
        *LeaseToken=JobGeneration;
    }
    PmicKeyClientLeave();
    return Result;
}

MALI_RESULT MtkMaliPmicPrmReleaseKeyIrqLease(uint64_t LeaseToken) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    uint64_t JobGeneration=0;
    uint32_t FinalValue=0,FinalResult=0;
    MALI_RESULT Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    if(!LeaseToken || LeaseToken!=PmicKeyLeaseToken) {
        PmicKeyClientLeave();
        return MaliInvalid;
    }
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,PMIC_KEY_IRQ_CAPABILITIES,&Pinned);
    if(Result!=MaliOk)goto Exit;
    Result=PmicV2RunJobLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,MtkPmicBrokerReleaseKeyLease,
            LeaseToken,PMIC_KEY_IRQ_ENABLE_ADDRESS,PMIC_KEY_IRQ_ENABLE_MASK,
            PMIC_KEY_IRQ_ENABLE_MASK,TRUE,&Pinned,&JobGeneration,
            &FinalValue,&FinalResult);
    if(Result!=MaliOk)goto Exit;
    if(FinalResult!=MtkPmicBrokerResultSuccess ||
       FinalValue!=PMIC_KEY_IRQ_ENABLE_MASK) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    if(Result==MaliOk) {
        PmicKeyLeaseToken=0;
        PmicKeySnapshotToken=0;
        PmicKeySnapshotLeaseToken=0;
        PmicKeySnapshotPacked=0;
    }
    PmicKeyClientLeave();
    return Result;
}

MALI_RESULT MtkMaliPmicPrmReadKeyIrqSnapshot(
    uint64_t LeaseToken,MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    uint64_t JobGeneration=0;
    uint32_t FinalValue=0,FinalResult=0;
    MALI_RESULT Result;
    if(!Snapshot || !LeaseToken)return MaliInvalid;
    Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    if(LeaseToken!=PmicKeyLeaseToken) {
        PmicKeyClientLeave();
        return MaliInvalid;
    }
    if(PmicKeySnapshotToken) {
        PmicKeyClientLeave();
        return MaliBusy;
    }
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,PMIC_KEY_IRQ_CAPABILITIES,&Pinned);
    if(Result!=MaliOk)goto Exit;
    Result=PmicV2RunJobLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,MtkPmicBrokerBeginKeySnapshot,
            LeaseToken,PMIC_KEY_STATUS_ADDRESS,0,0,FALSE,
            &Pinned,&JobGeneration,&FinalValue,&FinalResult);
    if(Result!=MaliOk)goto Exit;
    if(FinalResult!=MtkPmicBrokerResultSuccess) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    if(Result==MaliOk) {
        MTK_PMIC_KEY_IRQ_SNAPSHOT Output;
        Output.Generation=JobGeneration;
        Output.GroupStatus=(uint16_t)(FinalValue>>16);
        Output.TopStatus=(uint16_t)FinalValue;
        PmicKeySnapshotToken=JobGeneration;
        PmicKeySnapshotLeaseToken=LeaseToken;
        PmicKeySnapshotPacked=FinalValue;
        *Snapshot=Output;
    }
    PmicKeyClientLeave();
    return Result;
}

MALI_RESULT MtkMaliPmicPrmAckKeyIrqSnapshot(
    const MTK_PMIC_KEY_IRQ_SNAPSHOT *Snapshot) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    uint64_t JobGeneration=0;
    uint32_t Packed,FinalValue=0,FinalResult=0;
    MALI_RESULT Result;
    if(!Snapshot || !Snapshot->Generation)return MaliInvalid;
    Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    Packed=((uint32_t)Snapshot->GroupStatus<<16)|Snapshot->TopStatus;
    if(!PmicKeyLeaseToken || !PmicKeySnapshotToken ||
       Snapshot->Generation!=PmicKeySnapshotToken ||
       PmicKeySnapshotLeaseToken!=PmicKeyLeaseToken ||
       Packed!=PmicKeySnapshotPacked) {
        PmicKeyClientLeave();
        return MaliInvalid;
    }
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,PMIC_KEY_IRQ_CAPABILITIES,&Pinned);
    if(Result!=MaliOk)goto Exit;
    Result=PmicV2RunJobLocked(&Interface,MTK_PMIC_BROKER_VERSION_KEY_IRQ,MtkPmicBrokerAckKeySnapshot,
            Snapshot->Generation,PMIC_KEY_STATUS_ADDRESS,Packed,Packed,TRUE,
            &Pinned,&JobGeneration,&FinalValue,&FinalResult);
    if(Result!=MaliOk)goto Exit;
    if(FinalValue!=Packed ||
       (FinalResult!=MtkPmicBrokerResultSuccess &&
        FinalResult!=MtkPmicBrokerResultSiblingPending)) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=FinalResult==MtkPmicBrokerResultSiblingPending?MaliBusy:MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    if(Result==MaliOk && FinalResult==MtkPmicBrokerResultSuccess) {
        PmicKeySnapshotToken=0;
        PmicKeySnapshotLeaseToken=0;
        PmicKeySnapshotPacked=0;
    }
    PmicKeyClientLeave();
    return Result;
}

static BOOLEAN PmicGaugeReadAddressAllowed(uint32_t Address) {
    return Address==0x0D0AU || Address==0x0D8AU || Address==0x1108U ||
           Address==0x1088U || Address==0x108EU || Address==0x1090U ||
           Address==PMIC_AUXADC_BIF_VOLTAGE_ADDRESS ||
           Address==PMIC_AUXADC_BIF_CONTROL_ADDRESS ||
           Address==0x012AU || Address==0x010CU ||
           Address==PMIC_AUXADC_DIE_TEMP_TRIM || Address==PMIC_AUXADC_TEMP_REF_TRIM ||
           Address==PMIC_AUXADC_TEMP_SLOPE_TRIM || Address==PMIC_OTP_ROW ||
           Address==PMIC_OTP_READ_TRIGGER || Address==PMIC_OTP_READ_ENABLE ||
           Address==PMIC_OTP_READ_DATA || Address==PMIC_OTP_READ_BUSY;
}
static uint16_t PmicGaugeWriteMask(uint32_t Address,uint16_t Value) {
    if(Address==0x0D0AU && (Value==0U || Value==1U || Value==8U || Value==0x8008U))
        return 0x8009U;
    if(Address==PMIC_AUXADC_REQUEST_ADDRESS && Value!=0U &&
       (Value&~0x0819U)==0U && (Value&(Value-1U))==0U)
        return 0xFFFFU;
    if(Address==PMIC_AUXADC_BIF_CONTROL_ADDRESS)return PMIC_AUXADC_BIF_DISABLE;
    if(Address==PMIC_OTP_ROW && Value>=PMIC_OTP_FUSE_ROW_FIRST &&
       Value<=PMIC_OTP_FUSE_ROW_LAST && ((Value-PMIC_OTP_FUSE_ROW_FIRST)&1U)==0U)
        return 0xFFFFU;
    if((Address==PMIC_OTP_READ_TRIGGER || Address==PMIC_OTP_READ_ENABLE) && Value<=1U)
        return 0xFFFFU;
    if((Address==0x012CU || Address==0x012EU) && Value==0x0004U)
        return 0x0004U;
    if((Address==0x010EU || Address==0x0110U) && Value==0x0010U)
        return 0x0010U;
    return 0U;
}
static BOOLEAN PmicGaugeWriteAllowed(uint32_t Address,uint16_t Value) {
    return PmicGaugeWriteMask(Address,Value)!=0U;
}

MALI_RESULT MtkMaliPmicPrmAcquireGaugeLease(uint64_t *LeaseToken) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    uint64_t JobGeneration=0;
    uint32_t FinalValue=0,FinalResult=0;
    MALI_RESULT Result;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || LeaseToken==NULL)return MaliInvalid;
    Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_GAUGE,
        MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE|MTK_PMIC_BROKER_CAP_GAUGE_LEASE,&Pinned);
    if(Result!=MaliOk)goto Exit;
    Result=PmicV2RunJobLocked(&Interface,MTK_PMIC_BROKER_VERSION_GAUGE,
        MtkPmicBrokerAcquireGaugeLease,0,0,0,0,FALSE,&Pinned,&JobGeneration,
        &FinalValue,&FinalResult);
    if(Result!=MaliOk)goto Exit;
    if(!JobGeneration || FinalValue!=0U || FinalResult!=MtkPmicBrokerResultSuccess) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    if(Result==MaliOk)*LeaseToken=JobGeneration;
    PmicKeyClientLeave();
    return Result;
}

MALI_RESULT MtkMaliPmicPrmReleaseGaugeLease(uint64_t LeaseToken) {
    PRM_INTERFACE Interface;
    MTK_PMIC_BROKER_REQUEST Request;
    BOOLEAN Locked=FALSE,Pinned=FALSE,Invoked=FALSE;
    NTSTATUS Status;
    MALI_RESULT Result;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !LeaseToken)return MaliInvalid;
    Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_GAUGE,
        MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE|MTK_PMIC_BROKER_CAP_GAUGE_LEASE,&Pinned);
    if(Result!=MaliOk)goto Exit;
    RtlZeroMemory(&Request,sizeof(Request));
    Request.Size=sizeof(Request);
    Request.Version=MTK_PMIC_BROKER_VERSION_GAUGE;
    Request.Operation=MtkPmicBrokerReleaseGaugeLease;
    Request.Generation=LeaseToken;
    Status=PmicV2Call(&Interface,&Request,MTK_PMIC_BROKER_VERSION_GAUGE,
        MtkPmicBrokerReleaseGaugeLease,LeaseToken,0,0,&Invoked);
    if(Status==STATUS_DEVICE_BUSY) {
        Result=MaliBusy;
        goto Exit;
    }
    if(Status!=STATUS_SUCCESS) {
        Pinned=Invoked;
        Result=MaliFault;
        goto Exit;
    }
    if(Request.State!=MtkPmicBrokerStateIdle ||
       Request.Result!=MtkPmicBrokerResultSuccess ||
       Request.Generation<LeaseToken || Request.Address || Request.Value) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    PmicKeyClientLeave();
    return Result;
}

MALI_RESULT MtkMaliPmicPrmGaugeRead16(
    uint64_t LeaseToken,uint32_t Address,uint16_t *Value) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    uint64_t JobGeneration=0;
    uint32_t FinalValue=0,FinalResult=0;
    MALI_RESULT Result;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !LeaseToken || !Value ||
       !PmicGaugeReadAddressAllowed(Address))return MaliInvalid;
    Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_GAUGE,
        MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE|MTK_PMIC_BROKER_CAP_GAUGE_LEASE,&Pinned);
    if(Result!=MaliOk)goto Exit;
    Result=PmicV2RunJobLocked(&Interface,MTK_PMIC_BROKER_VERSION_GAUGE,
        MtkPmicBrokerBeginGaugeRead,LeaseToken,Address,0,0,FALSE,&Pinned,
        &JobGeneration,&FinalValue,&FinalResult);
    if(Result!=MaliOk)goto Exit;
    if(FinalResult!=MtkPmicBrokerResultSuccess || FinalValue>0xFFFFU) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    if(Result==MaliOk)*Value=(uint16_t)FinalValue;
    PmicKeyClientLeave();
    return Result;
}

MALI_RESULT MtkMaliPmicPrmGaugeWrite16(
    uint64_t LeaseToken,uint32_t Address,uint16_t Value) {
    PRM_INTERFACE Interface;
    BOOLEAN Locked=FALSE,Pinned=FALSE;
    uint64_t JobGeneration=0;
    uint32_t FinalValue=0,FinalResult=0;
    MALI_RESULT Result;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !LeaseToken ||
       !PmicGaugeWriteAllowed(Address,Value))return MaliInvalid;
    Result=PmicKeyClientEnter();
    if(Result!=MaliOk)return Result;
    Result=PmicKeyOpen(&Interface,&Locked);
    if(Result!=MaliOk)return Result;
    Result=PmicV2QueryLocked(&Interface,MTK_PMIC_BROKER_VERSION_GAUGE,
        MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE|MTK_PMIC_BROKER_CAP_GAUGE_LEASE,&Pinned);
    if(Result!=MaliOk)goto Exit;
    Result=PmicV2RunJobLocked(&Interface,MTK_PMIC_BROKER_VERSION_GAUGE,
        MtkPmicBrokerBeginGaugeWrite,LeaseToken,Address,Value,Value,TRUE,&Pinned,
        &JobGeneration,&FinalValue,&FinalResult);
    if(Result!=MaliOk)goto Exit;
    if(FinalResult!=MtkPmicBrokerResultSuccess || FinalValue!=Value) {
        Pinned=TRUE;
        Result=MaliFault;
        goto Exit;
    }
    Result=MaliOk;
Exit:
    Result=PmicKeyClose(&Interface,&Locked,Pinned,Result);
    PmicKeyClientLeave();
    return Result;
}


static BOOLEAN PmicBatteryAdcParameters(
    MTK_PMIC_BATTERY_ADC_CHANNEL Channel,
    uint16_t *Request,
    uint32_t *Address,
    uint16_t *Mask,
    ULONG *ConversionUs)
{
    if(!Request || !Address || !Mask || !ConversionUs)return FALSE;
    switch(Channel) {
    case MtkPmicBatteryAdcVoltage:
        *Request=1U;*Address=0x1088U;*Mask=0x7FFFU;*ConversionUs=1280U;return TRUE;
    case MtkPmicBatteryAdcTemperature:
        *Request=8U;*Address=0x108EU;*Mask=0x0FFFU;*ConversionUs=80U;return TRUE;
    case MtkPmicBatteryAdcChipTemperature:
        *Request=16U;*Address=0x1090U;*Mask=0x0FFFU;*ConversionUs=80U;return TRUE;
    case MtkPmicBatteryAdcBifVoltage:
        *Request=0x0800U;*Address=PMIC_AUXADC_BIF_VOLTAGE_ADDRESS;*Mask=0x0FFFU;*ConversionUs=80U;return TRUE;
    default:
        return FALSE;
    }
}

static BOOLEAN PmicBatteryAdcDelay(ULONG Microseconds) {
    LARGE_INTEGER Interval;
    Interval.QuadPart=-(LONGLONG)Microseconds*10;
    return KeDelayExecutionThread(KernelMode,FALSE,&Interval)==STATUS_SUCCESS;
}

static MALI_RESULT PmicBatteryAdcReadWithLease(
    uint64_t Lease,MTK_PMIC_BATTERY_ADC_CHANNEL Channel,uint16_t *Sample,
    BOOLEAN *LeaseMustBeRetained) {
    uint16_t Request,Mask,Value=0,Raw=0,BifControl=0,BifVerify=0;
    uint32_t Address;
    ULONG ConversionUs;
    ULONGLONG Started;
    BOOLEAN Ready=FALSE,BifRestoreNeeded=FALSE;
    MALI_RESULT Result,RestoreResult=MaliOk,VerifyResult=MaliOk;

    if(!Lease || !Sample || !LeaseMustBeRetained ||
       !PmicBatteryAdcParameters(Channel,&Request,&Address,&Mask,&ConversionUs))
        return MaliInvalid;
    *LeaseMustBeRetained=FALSE;
    if(Channel==MtkPmicBatteryAdcBifVoltage) {
        if(InterlockedCompareExchange(&PmicBatteryBifRestoreFault,0,0)!=0)
            return MaliFault;
        Result=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_AUXADC_BIF_CONTROL_ADDRESS,&BifControl);
        if(Result==MaliOk && (BifControl&PMIC_AUXADC_BIF_DISABLE)!=0U) {
            /* Mark ownership before the write: a timeout may still have changed the PMIC. */
            BifRestoreNeeded=TRUE;
            Result=MtkMaliPmicPrmGaugeWrite16(Lease,PMIC_AUXADC_BIF_CONTROL_ADDRESS,
                (uint16_t)(BifControl&~PMIC_AUXADC_BIF_DISABLE));
            if(Result==MaliOk) {
                Result=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_AUXADC_BIF_CONTROL_ADDRESS,&BifVerify);
                if(Result==MaliOk && (BifVerify&PMIC_AUXADC_BIF_DISABLE)!=0U)
                    Result=MaliFault;
            }
        }
    } else {
        Result=MaliOk;
    }
    if(Result==MaliOk)
        Result=MtkMaliPmicPrmGaugeWrite16(Lease,PMIC_AUXADC_REQUEST_ADDRESS,Request);
    if(Result==MaliOk && !PmicBatteryAdcDelay(ConversionUs))Result=MaliFault;
    Started=KeQueryInterruptTime();
    while(Result==MaliOk) {
        Result=MtkMaliPmicPrmGaugeRead16(Lease,Address,&Value);
        if(Result!=MaliOk)break;
        if(Value&PMIC_AUXADC_READY) {
            Raw=(uint16_t)(Value&Mask);
            Ready=TRUE;
            break;
        }
        if(KeQueryInterruptTime()-Started>=PMIC_AUXADC_POLL_TIMEOUT_100NS) {
            Result=MaliTimeout;
            break;
        }
        if(!PmicBatteryAdcDelay(PMIC_AUXADC_POLL_DELAY_US)) {
            Result=MaliFault;
            break;
        }
    }
    if(Result==MaliOk && !Ready)Result=MaliTimeout;
    if(BifRestoreNeeded) {
        RestoreResult=MtkMaliPmicPrmGaugeWrite16(Lease,PMIC_AUXADC_BIF_CONTROL_ADDRESS,BifControl);
        VerifyResult=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_AUXADC_BIF_CONTROL_ADDRESS,&BifVerify);
        if(VerifyResult!=MaliOk ||
           ((BifVerify^BifControl)&PMIC_AUXADC_BIF_DISABLE)!=0U) {
            InterlockedExchange(&PmicBatteryBifRestoreFault,1);
            PmicBatteryGaugeCore.Quarantined=1;
            PmicBatteryGaugeCore.Owned=1;
            *LeaseMustBeRetained=TRUE;
            return MaliFault;
        }
        if(Result==MaliOk && RestoreResult!=MaliOk)Result=RestoreResult;
    }
    if(Result==MaliOk)*Sample=Raw;
    return Result;
}

static void PmicBatteryQuarantineGauge(void) {
    InterlockedExchange(&PmicBatteryTrimRestoreFault,1);
    PmicBatteryGaugeCore.Quarantined=1;
    PmicBatteryGaugeCore.Owned=1;
}

static MALI_RESULT PmicBatteryReadFuseWordWithLease(
    uint64_t Lease,uint16_t Row,uint16_t *Trigger,uint16_t *FuseWord) {
    ULONGLONG Started;
    uint16_t Busy=0,Sample=0;
    MALI_RESULT Status;
    if(!Lease || !Trigger || !FuseWord || Row<PMIC_OTP_FUSE_ROW_FIRST ||
       Row>PMIC_OTP_FUSE_ROW_LAST || ((Row-PMIC_OTP_FUSE_ROW_FIRST)&1U))
        return MaliInvalid;
    Status=MtkMaliPmicPrmGaugeWrite16(Lease,PMIC_OTP_ROW,Row);
    if(Status!=MaliOk)return Status;
    if(!PmicBatteryAdcDelay(40U))return MaliFault;
    *Trigger^=1U;
    Status=MtkMaliPmicPrmGaugeWrite16(Lease,PMIC_OTP_READ_TRIGGER,*Trigger);
    if(Status!=MaliOk)return Status;
    if(!PmicBatteryAdcDelay(50U))return MaliFault;
    Started=KeQueryInterruptTime();
    for(;;) {
        Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_OTP_READ_BUSY,&Busy);
        if(Status!=MaliOk)return Status;
        if((Busy&1U)==0U) {
            if(!PmicBatteryAdcDelay(30U))return MaliFault;
            Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_OTP_READ_DATA,&Sample);
            if(Status==MaliOk)*FuseWord=Sample;
            return Status;
        }
        if(KeQueryInterruptTime()-Started>=PMIC_OTP_POLL_TIMEOUT_100NS)
            return MaliTimeout;
        if(!PmicBatteryAdcDelay(PMIC_OTP_POLL_DELAY_US))return MaliFault;
    }
}

static MALI_RESULT PmicBatteryReadVoltageTrimWithLease(
    uint64_t Lease,MTK_GAUGE_VOLTAGE_TRIM *Trim,BOOLEAN *LeaseMustBeRetained) {
    static const struct {uint16_t Base,Mask,SetAlias,ClearAlias;} ClockGates[]={
        {0x012AU,0x0004U,0x012CU,0x012EU},
        {0x010CU,0x0010U,0x010EU,0x0110U}
    };
    MTK_GAUGE_VOLTAGE_TRIM Sample={0};
    uint16_t GateBits[RTL_NUMBER_OF(ClockGates)]={0};
    uint16_t Value=0,Trigger=0;
    BOOLEAN RestoreReadEnable=FALSE,RestoreFailed=FALSE;
    MALI_RESULT Status=MaliOk,WriteStatus,VerifyStatus;
    ULONG Index;
    if(!Lease || !Trim || !LeaseMustBeRetained)return MaliInvalid;
    *LeaseMustBeRetained=FALSE;
    if(InterlockedCompareExchange(&PmicBatteryTrimRestoreFault,0,0)!=0)
        return MaliFault;

    /* The stock driver samples AUXADC temperature trims before gating clocks for OTP. */
    Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_AUXADC_DIE_TEMP_TRIM,&Sample.DieTemperature);
    if(Status!=MaliOk)goto Restore;
    if((Sample.DieTemperature&0x0100U)!=0U) {
        Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_AUXADC_TEMP_REF_TRIM,&Sample.TemperatureReference);
        if(Status==MaliOk)
            Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_AUXADC_TEMP_SLOPE_TRIM,&Sample.TemperatureSlope);
    }
    if(Status!=MaliOk)return Status;
    for(Index=0;Index<RTL_NUMBER_OF(ClockGates);Index++) {
        Status=MtkMaliPmicPrmGaugeRead16(Lease,ClockGates[Index].Base,&Value);
        if(Status!=MaliOk)goto Restore;
        GateBits[Index]=(uint16_t)(Value&ClockGates[Index].Mask);
    }
    for(Index=0;Index<RTL_NUMBER_OF(ClockGates);Index++) {
        if(GateBits[Index]==0U)continue;
        Status=MtkMaliPmicPrmGaugeWrite16(Lease,ClockGates[Index].ClearAlias,ClockGates[Index].Mask);
        if(Status!=MaliOk)goto Restore;
        Status=MtkMaliPmicPrmGaugeRead16(Lease,ClockGates[Index].Base,&Value);
        if(Status!=MaliOk || (Value&ClockGates[Index].Mask)!=0U) {
            if(Status==MaliOk)Status=MaliFault;
            goto Restore;
        }
    }
    Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_OTP_READ_ENABLE,&Value);
    if(Status!=MaliOk)goto Restore;
    if(Value!=0U) {Status=MaliBusy;goto Restore;}
    Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_OTP_READ_BUSY,&Value);
    if(Status!=MaliOk)goto Restore;
    if((Value&1U)!=0U) {Status=MaliBusy;goto Restore;}
    Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_OTP_READ_TRIGGER,&Trigger);
    if(Status!=MaliOk)goto Restore;
    if(Trigger>1U) {Status=MaliFault;goto Restore;}

    /* A timed-out write can still take effect, so own cleanup before issuing it. */
    RestoreReadEnable=TRUE;
    Status=MtkMaliPmicPrmGaugeWrite16(Lease,PMIC_OTP_READ_ENABLE,1U);
    if(Status!=MaliOk)goto Restore;
    Status=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_OTP_READ_ENABLE,&Value);
    if(Status!=MaliOk || Value!=1U) {
        if(Status==MaliOk)Status=MaliFault;
        goto Restore;
    }
    for(Index=0;Index<RTL_NUMBER_OF(Sample.Fuse);Index++) {
        Status=PmicBatteryReadFuseWordWithLease(Lease,
            (uint16_t)(PMIC_OTP_FUSE_ROW_FIRST+2U*Index),&Trigger,&Sample.Fuse[Index]);
        if(Status!=MaliOk)goto Restore;
    }

Restore:
    if(RestoreReadEnable) {
        WriteStatus=MtkMaliPmicPrmGaugeWrite16(Lease,PMIC_OTP_READ_ENABLE,0U);
        VerifyStatus=MtkMaliPmicPrmGaugeRead16(Lease,PMIC_OTP_READ_ENABLE,&Value);
        if(VerifyStatus!=MaliOk || Value!=0U)RestoreFailed=TRUE;
        else if(Status==MaliOk && WriteStatus!=MaliOk)Status=WriteStatus;
    }
    for(Index=0;Index<RTL_NUMBER_OF(ClockGates);Index++) {
        if(GateBits[Index]==0U)continue;
        WriteStatus=MtkMaliPmicPrmGaugeWrite16(Lease,ClockGates[Index].SetAlias,ClockGates[Index].Mask);
        VerifyStatus=MtkMaliPmicPrmGaugeRead16(Lease,ClockGates[Index].Base,&Value);
        if(VerifyStatus!=MaliOk ||
           (Value&ClockGates[Index].Mask)!=GateBits[Index])RestoreFailed=TRUE;
        else if(Status==MaliOk && WriteStatus!=MaliOk)Status=WriteStatus;
    }
    if(RestoreFailed) {
        PmicBatteryQuarantineGauge();
        *LeaseMustBeRetained=TRUE;
        return MaliFault;
    }
    if(Status==MaliOk)*Trim=Sample;
    return Status;
}

MALI_RESULT MtkMaliPmicPrmReadBatteryVoltageMv(uint16_t *Millivolts) {
    uint64_t Lease=0;
    MTK_GAUGE_VOLTAGE_TRIM Trim;
    uint16_t BatteryAdc=0,ChipAdc=0,Sample=0;
    BOOLEAN Retain=FALSE,TrimLoaded=FALSE;
    MALI_RESULT Status,ReleaseStatus;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Millivolts)return MaliInvalid;
    if(InterlockedCompareExchange(&PmicBatteryCurrentBusy,1,0)!=0)return MaliBusy;
    if(InterlockedCompareExchange(&PmicBatteryTrimRestoreFault,0,0)!=0 ||
       PmicBatteryGaugeCore.Quarantined || PmicBatteryGaugeCore.Owned) {
        Status=MaliFault;
        goto Exit;
    }
    Status=MtkMaliPmicPrmAcquireGaugeLease(&Lease);
    if(Status!=MaliOk)goto Exit;
    if(PmicBatteryVoltageTrimValid) {
        Trim=PmicBatteryVoltageTrim;
    } else {
        Status=PmicBatteryReadVoltageTrimWithLease(Lease,&Trim,&Retain);
        if(Status!=MaliOk)goto Release;
        TrimLoaded=TRUE;
    }
    Status=PmicBatteryAdcReadWithLease(Lease,MtkPmicBatteryAdcVoltage,&BatteryAdc,&Retain);
    if(Status!=MaliOk)goto Release;
    Status=PmicBatteryAdcReadWithLease(Lease,MtkPmicBatteryAdcChipTemperature,&ChipAdc,&Retain);
    if(Status!=MaliOk)goto Release;
    if(MtkGaugeCalibrateVoltage(BatteryAdc,ChipAdc,&Trim,&Sample)!=MtkGaugeOk) {
        Status=MaliInvalid;
        goto Release;
    }
Release:
    if(Retain) {
        InterlockedExchange(&PmicBatteryCurrentBusy,0);
        return Status;
    }
    ReleaseStatus=MtkMaliPmicPrmReleaseGaugeLease(Lease);
    if(ReleaseStatus!=MaliOk) {
        PmicBatteryQuarantineGauge();
        InterlockedExchange(&PmicBatteryCurrentBusy,0);
        return MaliFault;
    }
    if(TrimLoaded) {
        PmicBatteryVoltageTrim=Trim;
        PmicBatteryVoltageTrimValid=TRUE;
    }
    if(Status==MaliOk)*Millivolts=Sample;
Exit:
    InterlockedExchange(&PmicBatteryCurrentBusy,0);
    return Status;
}

MALI_RESULT MtkMaliPmicPrmReadBatterySnapshot(
    MTK_PMIC_BATTERY_SNAPSHOT *Snapshot) {
    static const MTK_GAUGE_IO Io={
        &PmicBatteryCurrentContext,NULL,NULL,PmicBatteryCurrentRead16,PmicBatteryCurrentWrite16
    };
    MTK_PMIC_BATTERY_SNAPSHOT Sample={0};
    MTK_GAUGE_VOLTAGE_TRIM Trim;
    uint64_t Lease=0;
    BOOLEAN Retain=FALSE,TrimLoaded=FALSE;
    MTK_GAUGE_RESULT GaugeStatus;
    MALI_RESULT Status,ReleaseStatus;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Snapshot)return MaliInvalid;
    if(InterlockedCompareExchange(&PmicBatteryCurrentBusy,1,0)!=0)return MaliBusy;
    if(InterlockedCompareExchange(&PmicBatteryBifRestoreFault,0,0)!=0 ||
       InterlockedCompareExchange(&PmicBatteryTrimRestoreFault,0,0)!=0 ||
       PmicBatteryGaugeCore.Quarantined || PmicBatteryGaugeCore.Owned) {
        Status=MaliFault;
        goto Exit;
    }
    Status=MtkMaliPmicPrmAcquireGaugeLease(&Lease);
    if(Status!=MaliOk)goto Exit;
    PmicBatteryCurrentContext.LeaseToken=Lease;
    PmicBatteryCurrentContext.LastStatus=MaliOk;
    if(PmicBatteryVoltageTrimValid) {
        Trim=PmicBatteryVoltageTrim;
    } else {
        Status=PmicBatteryReadVoltageTrimWithLease(Lease,&Trim,&Retain);
        if(Status!=MaliOk)goto Release;
        TrimLoaded=TRUE;
    }
    Status=PmicBatteryAdcReadWithLease(
        Lease,MtkPmicBatteryAdcVoltage,&Sample.BatteryAdcRaw,&Retain);
    if(Status!=MaliOk)goto Release;
    Status=PmicBatteryAdcReadWithLease(
        Lease,MtkPmicBatteryAdcChipTemperature,&Sample.ChipTemperatureAdcRaw,&Retain);
    if(Status!=MaliOk)goto Release;
    Status=PmicBatteryAdcReadWithLease(
        Lease,MtkPmicBatteryAdcTemperature,&Sample.ThermistorAdcRaw,&Retain);
    if(Status!=MaliOk)goto Release;

    GaugeStatus=MtkGaugeReadCurrentInLease(
        &PmicBatteryGaugeCore,&Io,&Sample.CurrentRaw);
    if(GaugeStatus==MtkGaugeQuarantined) {
        Retain=TRUE;
        Status=MaliFault;
    } else if(GaugeStatus==MtkGaugeBusy) {
        Status=PmicBatteryCurrentContext.LastStatus==MaliOk?MaliBusy:PmicBatteryCurrentContext.LastStatus;
    } else if(GaugeStatus==MtkGaugeTimeout) {
        Status=MaliTimeout;
    } else if(GaugeStatus==MtkGaugeInvalid) {
        Status=MaliInvalid;
    } else if(GaugeStatus!=MtkGaugeOk) {
        Status=PmicBatteryCurrentContext.LastStatus==MaliOk?MaliFault:PmicBatteryCurrentContext.LastStatus;
    } else {
        Status=MaliOk;
    }
    if(Status!=MaliOk)goto Release;

    Status=PmicBatteryAdcReadWithLease(
        Lease,MtkPmicBatteryAdcBifVoltage,&Sample.BifAdcRaw,&Retain);
    if(Status!=MaliOk)goto Release;
    if(MtkGaugeCalibrateVoltage(
        Sample.BatteryAdcRaw,Sample.ChipTemperatureAdcRaw,&Trim,
        &Sample.BatteryVoltageMv)!=MtkGaugeOk ||
       MtkGaugeBatteryTemperature(
        Sample.ThermistorAdcRaw,Sample.BifAdcRaw,Sample.CurrentRaw,
        &Sample.BatteryTemperatureDeciC)!=MtkGaugeOk ||
       MtkGaugeConvertCurrent(
        Sample.CurrentRaw,T70_GAUGE_CURRENT_SENSE_INTERNAL,
        T70_GAUGE_CURRENT_TUNE_INTERNAL,&Sample.BatteryCurrentStockUnits)!=MtkGaugeOk) {
        Status=MaliInvalid;
        goto Release;
    }
Release:
    if(Retain) {
        InterlockedExchange(&PmicBatteryCurrentBusy,0);
        return Status;
    }
    ReleaseStatus=MtkMaliPmicPrmReleaseGaugeLease(Lease);
    if(ReleaseStatus!=MaliOk) {
        PmicBatteryQuarantineGauge();
        InterlockedExchange(&PmicBatteryCurrentBusy,0);
        return MaliFault;
    }
    PmicBatteryCurrentContext.LeaseToken=0;
    if(TrimLoaded) {
        PmicBatteryVoltageTrim=Trim;
        PmicBatteryVoltageTrimValid=TRUE;
    }
    if(Status==MaliOk)*Snapshot=Sample;
Exit:
    InterlockedExchange(&PmicBatteryCurrentBusy,0);
    return Status;
}

MALI_RESULT MtkMaliPmicPrmReadBatteryAdcRaw(
    MTK_PMIC_BATTERY_ADC_CHANNEL Channel,uint16_t *Sample) {
    uint16_t Request,Mask,Raw=0;
    uint32_t Address;
    ULONG ConversionUs;
    uint64_t Lease=0;
    BOOLEAN Retain=FALSE;
    MALI_RESULT Result,ReleaseResult;

    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Sample ||
       !PmicBatteryAdcParameters(Channel,&Request,&Address,&Mask,&ConversionUs))
        return MaliInvalid;
    if(InterlockedCompareExchange(&PmicBatteryCurrentBusy,1,0)!=0)return MaliBusy;
    if(InterlockedCompareExchange(&PmicBatteryBifRestoreFault,0,0)!=0 ||
       InterlockedCompareExchange(&PmicBatteryTrimRestoreFault,0,0)!=0 ||
       PmicBatteryGaugeCore.Quarantined || PmicBatteryGaugeCore.Owned) {
        Result=MaliFault;goto ExitRaw;
    }
    Result=MtkMaliPmicPrmAcquireGaugeLease(&Lease);
    if(Result!=MaliOk)goto ExitRaw;
    Result=PmicBatteryAdcReadWithLease(Lease,Channel,&Raw,&Retain);
    if(Retain)goto ExitRaw;
    ReleaseResult=MtkMaliPmicPrmReleaseGaugeLease(Lease);
    if(ReleaseResult!=MaliOk) {
        PmicBatteryQuarantineGauge();
        Result=MaliFault;goto ExitRaw;
    }
    if(Result==MaliOk)*Sample=Raw;
ExitRaw:
    InterlockedExchange(&PmicBatteryCurrentBusy,0);
    return Result;
}

static int PmicBatteryCurrentAcquire(void *Context) {
    PMIC_BATTERY_CURRENT_CONTEXT *Battery=Context;
    if(!Battery || Battery->LeaseToken) {
        if(Battery)Battery->LastStatus=MaliBusy;
        return 0;
    }
    Battery->LastStatus=MtkMaliPmicPrmAcquireGaugeLease(&Battery->LeaseToken);
    return Battery->LastStatus==MaliOk;
}

static int PmicBatteryCurrentRelease(void *Context) {
    PMIC_BATTERY_CURRENT_CONTEXT *Battery=Context;
    if(!Battery || !Battery->LeaseToken) {
        if(Battery)Battery->LastStatus=MaliFault;
        return 0;
    }
    Battery->LastStatus=MtkMaliPmicPrmReleaseGaugeLease(Battery->LeaseToken);
    if(Battery->LastStatus==MaliOk)Battery->LeaseToken=0;
    return Battery->LastStatus==MaliOk;
}

static int PmicBatteryCurrentRead16(void *Context,uint16_t Address,uint16_t *Value) {
    PMIC_BATTERY_CURRENT_CONTEXT *Battery=Context;
    if(!Battery || !Battery->LeaseToken) {
        if(Battery)Battery->LastStatus=MaliFault;
        return 0;
    }
    Battery->LastStatus=MtkMaliPmicPrmGaugeRead16(Battery->LeaseToken,Address,Value);
    return Battery->LastStatus==MaliOk;
}

static int PmicBatteryCurrentWrite16(void *Context,uint16_t Address,uint16_t Value) {
    PMIC_BATTERY_CURRENT_CONTEXT *Battery=Context;
    if(!Battery || !Battery->LeaseToken) {
        if(Battery)Battery->LastStatus=MaliFault;
        return 0;
    }
    Battery->LastStatus=MtkMaliPmicPrmGaugeWrite16(Battery->LeaseToken,Address,Value);
    return Battery->LastStatus==MaliOk;
}

MALI_RESULT MtkMaliPmicPrmReadBatteryTemperatureDeciC(int32_t *DeciCelsius) {
    static const MTK_GAUGE_IO Io={
        &PmicBatteryCurrentContext,NULL,NULL,PmicBatteryCurrentRead16,PmicBatteryCurrentWrite16
    };
    uint64_t Lease=0;
    uint16_t Thermistor=0,Current=0,Bif=0;
    int32_t Sample=0;
    BOOLEAN Retain=FALSE;
    MTK_GAUGE_RESULT GaugeStatus;
    MALI_RESULT Status,ReleaseStatus;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !DeciCelsius)return MaliInvalid;
    if(InterlockedCompareExchange(&PmicBatteryCurrentBusy,1,0)!=0)return MaliBusy;
    if(InterlockedCompareExchange(&PmicBatteryBifRestoreFault,0,0)!=0 ||
       InterlockedCompareExchange(&PmicBatteryTrimRestoreFault,0,0)!=0 ||
       PmicBatteryGaugeCore.Quarantined || PmicBatteryGaugeCore.Owned) {
        Status=MaliFault;goto Exit;
    }
    Status=MtkMaliPmicPrmAcquireGaugeLease(&Lease);
    if(Status!=MaliOk)goto Exit;
    PmicBatteryCurrentContext.LeaseToken=Lease;
    PmicBatteryCurrentContext.LastStatus=MaliOk;

    Status=PmicBatteryAdcReadWithLease(Lease,MtkPmicBatteryAdcTemperature,&Thermistor,&Retain);
    if(Status!=MaliOk)goto Release;
    GaugeStatus=MtkGaugeReadCurrentInLease(&PmicBatteryGaugeCore,&Io,&Current);
    if(GaugeStatus==MtkGaugeQuarantined) {
        Retain=TRUE;
        Status=MaliFault;
        goto Release;
    }
    if(GaugeStatus==MtkGaugeBusy)Status=MaliBusy;
    else if(GaugeStatus==MtkGaugeTimeout)Status=MaliTimeout;
    else if(GaugeStatus==MtkGaugeInvalid)Status=MaliInvalid;
    else if(GaugeStatus!=MtkGaugeOk)
        Status=PmicBatteryCurrentContext.LastStatus==MaliOk?MaliFault:PmicBatteryCurrentContext.LastStatus;
    else Status=MaliOk;
    if(Status!=MaliOk)goto Release;

    Status=PmicBatteryAdcReadWithLease(Lease,MtkPmicBatteryAdcBifVoltage,&Bif,&Retain);
    if(Status!=MaliOk)goto Release;
    if(MtkGaugeBatteryTemperature(Thermistor,Bif,Current,&Sample)!=MtkGaugeOk) {
        Status=MaliInvalid;
        goto Release;
    }
Release:
    if(Retain) {
        InterlockedExchange(&PmicBatteryCurrentBusy,0);
        return Status;
    }
    ReleaseStatus=MtkMaliPmicPrmReleaseGaugeLease(Lease);
    if(ReleaseStatus!=MaliOk) {
        PmicBatteryGaugeCore.Quarantined=1;
        PmicBatteryGaugeCore.Owned=1;
        InterlockedExchange(&PmicBatteryCurrentBusy,0);
        return MaliFault;
    }
    PmicBatteryCurrentContext.LeaseToken=0;
    if(Status==MaliOk)*DeciCelsius=Sample;
Exit:
    InterlockedExchange(&PmicBatteryCurrentBusy,0);
    return Status;
}

MALI_RESULT MtkMaliPmicPrmReadBatteryCurrentRaw(uint16_t *Raw) {
    static const MTK_GAUGE_IO Io={
        &PmicBatteryCurrentContext,PmicBatteryCurrentAcquire,
        PmicBatteryCurrentRelease,PmicBatteryCurrentRead16,PmicBatteryCurrentWrite16
    };
    uint16_t Sample=0;
    MTK_GAUGE_RESULT Status;
    MALI_RESULT Result;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Raw)return MaliInvalid;
    if(InterlockedCompareExchange(&PmicBatteryCurrentBusy,1,0)!=0)return MaliBusy;
    Status=MtkGaugeReadCurrent(&PmicBatteryGaugeCore,&Io,&Sample);
    switch(Status) {
    case MtkGaugeOk: Result=MaliOk;break;
    case MtkGaugeInvalid: Result=MaliInvalid;break;
    case MtkGaugeBusy:
        Result=PmicBatteryCurrentContext.LastStatus==MaliOk?MaliBusy:PmicBatteryCurrentContext.LastStatus;
        break;
    case MtkGaugeTimeout: Result=MaliTimeout;break;
    case MtkGaugeQuarantined: Result=MaliFault;break;
    default:
        Result=PmicBatteryCurrentContext.LastStatus==MaliOk?MaliFault:PmicBatteryCurrentContext.LastStatus;
        break;
    }
    if(Result==MaliOk)*Raw=Sample;
    InterlockedExchange(&PmicBatteryCurrentBusy,0);
    return Result;
}

/* T70 DT r-fg-value/car-tune-value are expanded x10 by the stock parser. */
MALI_RESULT MtkMaliPmicPrmReadBatteryCurrentStockUnits(int32_t *StockUnits) {
    uint16_t Raw=0;
    int32_t Sample=0;
    MALI_RESULT Status;
    if(!StockUnits)return MaliInvalid;
    Status=MtkMaliPmicPrmReadBatteryCurrentRaw(&Raw);
    if(Status!=MaliOk)return Status;
    if(MtkGaugeConvertCurrent(Raw,T70_GAUGE_CURRENT_SENSE_INTERNAL,
                              T70_GAUGE_CURRENT_TUNE_INTERNAL,&Sample)!=MtkGaugeOk)
        return MaliInvalid;
    *StockUnits=Sample;
    return MaliOk;
}
