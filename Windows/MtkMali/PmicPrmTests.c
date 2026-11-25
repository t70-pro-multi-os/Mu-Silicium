/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if defined(MTK_PMIC_PRM_HOST_TEST)
#include "PmicPrmHostShim.h"
#include <stdio.h>
#else
#include <ntddk.h>
#include <prminterface.h>
#endif
#include <Library/MtkPmicBroker.h>
#include "PmicPrm.h"

#if defined(MTK_PMIC_PRM_HOST_TEST)
#define TEST_TRACE(...) fprintf(stderr,__VA_ARGS__)
#else
#define TEST_TRACE(...) ((void)0)
#endif

volatile ULONG MtkMaliPmicPrmAssertionCount;
static ULONG Failure;
#define CHECK(X) do {++MtkMaliPmicPrmAssertionCount;if(!(X)&&!Failure)Failure=__LINE__;} while(0)

static NTSTATUS TestGetPrmInterface(ULONG Version,PPRM_INTERFACE InterfaceOut);
static KIRQL TestCurrentIrql(void);
static ULONGLONG TestQueryInterruptTime(void);
static VOID TestStall(ULONG Microseconds);
static NTSTATUS TestDelayExecutionThread(KPROCESSOR_MODE WaitMode,BOOLEAN Alertable,PLARGE_INTEGER Interval);
static LONG TestInterlockedCompareExchange(volatile LONG *Target,LONG Exchange,LONG Comparand);
static LONG TestInterlockedExchange(volatile LONG *Target,LONG Exchange);
#define ExGetPrmInterface TestGetPrmInterface
#define KeGetCurrentIrql TestCurrentIrql
#ifdef KeQueryInterruptTime
#undef KeQueryInterruptTime
#endif
#define KeQueryInterruptTime TestQueryInterruptTime
#define KeStallExecutionProcessor TestStall
#define KeDelayExecutionThread TestDelayExecutionThread
#ifdef InterlockedCompareExchange
#undef InterlockedCompareExchange
#endif
#define InterlockedCompareExchange TestInterlockedCompareExchange
#ifdef InterlockedExchange
#undef InterlockedExchange
#endif
#define InterlockedExchange TestInterlockedExchange
#include "PmicPrm.c"
#include "../../Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsPrm/MtkPmicBroker.c"
#include "../../Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkGaugeCore.c"

static MTK_PMIC_BROKER Core;
static MTK_PMIC_BROKER_IO BrokerIo;
static PRM_INTERFACE MockInterface;
static uint16_t PmicRegisters[32768];
static ULONG Fsm,WriteData,Response,OtherOwned,CurrentIrql,ReadAddressCount,WriteCommandCount;
static ULONG AdcAutoReady,AdcRequestWrites,LastAdcRequest;
static ULONG OtpStuckBusy,OtpTriggerCount,OtpRowCount,FailWriteCommandValue;
static uint16_t OtpFuseWords[4],OtpRows[4];
static uint32_t ReadAddresses[64],WriteCommandAddresses[64],WriteCommandValues[64];
static MTK_PMIC_BROKER_REQUEST LastInput[32],LastResponse[32];
static ULONG OperationCalls[32];
static ULONGLONG Time100ns;
static uint64_t TimeUs;
static ULONG GetCalls,LockCalls,UnlockCalls,QueryCalls,InvokeCalls,Stalls;
static ULONG LockHeld,Found,ForceBusy[32],OtherOwnedClearAfterStalls,ArmOtherOwnedAfterBegin;
static ULONG CorruptOperation,CorruptOnce,CorruptBusyOperation,CorruptBusyOnce;
static ULONG CorruptField,CorruptSkip;
static ULONG NonzeroEfiOperation,NonzeroEfiOnce;
static ULONG ReenterOnStall;
static ULONG RejectLegacyKeyOpcode;
static ULONG RejectV2QueryOnce;
static ULONG FailWriteCommandAddress,RewriteAcceptedResultInvalid;
static ULONG ReenterOnUnlock,PublicationCheck;
static uint64_t *PublicationLeaseOutput;
static MTK_PMIC_KEY_IRQ_SNAPSHOT *PublicationSnapshotOutput;
static MALI_RESULT ReenterResult;
static ULONG InvokeStatusOperation,InvokeStatusOnce;
static NTSTATUS GetStatus,LockStatus,QueryStatus,UnlockStatus,InvokeStatus;
static uint64_t InvokeEfiStatus;
static const GUID ExpectedHandlerGuid=MTK_PMIC_PRM_HANDLER_GUID;
enum {CorruptSize=1,CorruptVersion,CorruptOperationField,CorruptFlags,CorruptState,
      CorruptGeneration,CorruptResult,CorruptAddress,CorruptValue,CorruptReserved0,CorruptReserved};
static BOOLEAN TestGuidMatch(LPGUID Guid) {
    if(!Guid || Guid->Data1!=ExpectedHandlerGuid.Data1 || Guid->Data2!=ExpectedHandlerGuid.Data2 ||
       Guid->Data3!=ExpectedHandlerGuid.Data3)return FALSE;
    for(ULONG I=0;I<8;I++)if(Guid->Data4[I]!=ExpectedHandlerGuid.Data4[I])return FALSE;
    return TRUE;
}

static MTK_PMIC_BROKER_IO_STATUS TestPmicRead32(void *Context,uint32_t Offset,uint32_t *Value) {
    CHECK(Context==PmicRegisters && Value);
    if(!Value)return MtkPmicBrokerIoFailed;
    if(Offset==0) {*Value=1;return MtkPmicBrokerIoOk;}
    if(Offset==0x8a8) {*Value=Fsm<<1;return MtkPmicBrokerIoOk;}
    if(Offset==0x894) {*Value=Response;return MtkPmicBrokerIoOk;}
    return MtkPmicBrokerIoFailed;
}
static MTK_PMIC_BROKER_IO_STATUS TestPmicWrite32(void *Context,uint32_t Offset,uint32_t Value) {
    CHECK(Context==PmicRegisters);
    CHECK(MtkPmicBrokerWacsWriteAllowed(&Core,Offset,Value));
    if(Offset==0x884) {WriteData=Value;return MtkPmicBrokerIoOk;}
    if(Offset==0x8a4) {
        if(Value!=1 || Fsm!=6)return MtkPmicBrokerIoFailed;
        Fsm=0;return MtkPmicBrokerIoOk;
    }
    if(Offset!=0x880)return MtkPmicBrokerIoFailed;
    uint32_t Address=Value&0xffffU;
    if(Value&0x20000000U) {
        if(WriteCommandCount<sizeof(WriteCommandAddresses)/sizeof(WriteCommandAddresses[0])) {
            WriteCommandAddresses[WriteCommandCount]=Address;
            WriteCommandValues[WriteCommandCount]=WriteData;
        }
        ++WriteCommandCount;
        if(Address==0x91cU)
            PmicRegisters[Address/2]&=(uint16_t)~WriteData;
        else if(Address==0x1108U) {
            LastAdcRequest=WriteData;++AdcRequestWrites;
            uint32_t Output=WriteData==1U?0x1088U:(WriteData==8U?0x108EU:(WriteData==16U?0x1090U:0x109EU));
            uint16_t Raw=WriteData==1U?0x1234U:(WriteData==8U?0x05ABU:(WriteData==16U?0x09CDU:0x0B00U));
            PmicRegisters[Output/2]=(uint16_t)(Raw|(AdcAutoReady?0x8000U:0U));
            PmicRegisters[Address/2]=(uint16_t)WriteData;
        }
        else if(Address==0x012cU)
            PmicRegisters[0x012aU/2]|=(uint16_t)(WriteData&0x0004U);
        else if(Address==0x012eU)
            PmicRegisters[0x012aU/2]&=(uint16_t)~(WriteData&0x0004U);
        else if(Address==0x010eU)
            PmicRegisters[0x010cU/2]|=(uint16_t)(WriteData&0x0010U);
        else if(Address==0x0110U)
            PmicRegisters[0x010cU/2]&=(uint16_t)~(WriteData&0x0010U);
        else if(Address==0x038aU) {
            PmicRegisters[Address/2]=(uint16_t)WriteData;
            if(OtpRowCount<RTL_NUMBER_OF(OtpRows))OtpRows[OtpRowCount]=(uint16_t)WriteData;
            ++OtpRowCount;
        }
        else if(Address==0x039aU) {
            PmicRegisters[Address/2]=(uint16_t)WriteData;
            ++OtpTriggerCount;
            PmicRegisters[0x03a4U/2]=(uint16_t)(OtpStuckBusy?1U:0U);
            if(!OtpStuckBusy) {
                uint16_t Row=PmicRegisters[0x038aU/2];
                uint32_t Index=Row>=0x50U?(Row-0x50U)/2U:RTL_NUMBER_OF(OtpFuseWords);
                PmicRegisters[0x03a2U/2]=Index<RTL_NUMBER_OF(OtpFuseWords)?OtpFuseWords[Index]:0U;
            }
        }
        else if(Address==0x0d0aU && WriteData==1U)
            PmicRegisters[Address/2]=0x8001U;
        else if(Address==0x0d0aU && WriteData==0x8008U)
            PmicRegisters[Address/2]=8U;
        else
            PmicRegisters[Address/2]=(uint16_t)WriteData;
        if(Address==0x1508)PmicRegisters[0x1520/2]=(uint16_t)((PmicRegisters[0x1520/2]&~1U)|(WriteData&1U));
        if(Address==0x1bc8)PmicRegisters[0x1be4/2]=(uint16_t)((PmicRegisters[0x1be4/2]&~1U)|(WriteData&1U));
        if(Address==0x1bf4)PmicRegisters[0x1be2/2]=(uint16_t)((PmicRegisters[0x1be2/2]&~0x7f00U)|((WriteData&0x7fU)<<8));
        Fsm=0;
        if(FailWriteCommandAddress==Address &&
           (!FailWriteCommandValue || FailWriteCommandValue==WriteData)) {
            FailWriteCommandAddress=FailWriteCommandValue=0;
            return MtkPmicBrokerIoFailed;
        }
    } else {
        if(ReadAddressCount<sizeof(ReadAddresses)/sizeof(ReadAddresses[0]))
            ReadAddresses[ReadAddressCount]=Address;
        ++ReadAddressCount;
        Response=PmicRegisters[Address/2];Fsm=6;
    }
    return MtkPmicBrokerIoOk;
}
static uint64_t TestNowUs(void *Context) {CHECK(Context==PmicRegisters);return TimeUs;}

static NTSTATUS TestGetPrmInterface(ULONG Version,PPRM_INTERFACE InterfaceOut) {
    ++GetCalls;
    if(Version!=PRM_INTERFACE_V1 || !InterfaceOut)return STATUS_INVALID_PARAMETER;
    if(GetStatus==STATUS_SUCCESS)*InterfaceOut=MockInterface;
    return GetStatus;
}
static NTSTATUS TestLockModule(LPGUID HandlerGuid) {
    CHECK(TestGuidMatch(HandlerGuid) && LockHeld==0);++LockCalls;
    if(LockStatus==STATUS_SUCCESS)LockHeld=1;
    return LockStatus;
}
static NTSTATUS TestUnlockModule(LPGUID HandlerGuid) {
    ++UnlockCalls;CHECK(TestGuidMatch(HandlerGuid) && LockHeld==1);
    if(ReenterOnUnlock) {
        ULONG Gets=GetCalls;
        ReenterOnUnlock=0;
        CHECK(PmicClientState==1);
        CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliBusy && GetCalls==Gets);
    }
    LockHeld=0;return UnlockStatus;
}
static NTSTATUS TestQueryHandler(LPGUID HandlerGuid,PBOOLEAN IsFound) {
    CHECK(TestGuidMatch(HandlerGuid) && IsFound && LockHeld==1);++QueryCalls;
    if(QueryStatus==STATUS_SUCCESS)*IsFound=(BOOLEAN)Found;
    return QueryStatus;
}
static NTSTATUS TestInvokeHandler(LPGUID HandlerGuid,PVOID ParameterBuffer,ULONG Reserved,PULONG64 EfiStatus) {
    CHECK(TestGuidMatch(HandlerGuid) && ParameterBuffer && Reserved==0 && EfiStatus && LockHeld==1);++InvokeCalls;
    if(!ParameterBuffer || !EfiStatus)return STATUS_INVALID_PARAMETER;
    MTK_PMIC_BROKER_REQUEST *Request=ParameterBuffer,Input=*Request;
    if(Input.Operation<32U) {
        LastInput[Input.Operation]=Input;
        ++OperationCalls[Input.Operation];
    }
    *EfiStatus=0;
    if(ForceBusy[Input.Operation]) {
        --ForceBusy[Input.Operation];Request->Result=MtkPmicBrokerResultBusy;
        if(CorruptBusyOnce && Input.Operation==CorruptBusyOperation) {
            CorruptBusyOnce=0;Request->Address^=2;
        }
        if(Input.Operation<32U)LastResponse[Input.Operation]=*Request;
        return STATUS_SUCCESS;
    }
    if(RejectLegacyKeyOpcode && Input.Operation==MtkPmicBrokerBeginKeySnapshot) {
        RejectLegacyKeyOpcode=0;
        /* Emulate an earlier v1 broker's generic unsupported-op response. Its
           idle metadata still reports the previous transaction cache. */
        Request->State=Core.State;Request->Generation=Core.Generation;
        Request->Result=MtkPmicBrokerResultInvalid;
        Request->Address=Core.Address;Request->Value=Core.Value;
        LastResponse[Input.Operation]=*Request;
        return STATUS_SUCCESS;
    }
    if(RejectV2QueryOnce && Input.Operation==MtkPmicBrokerQuery &&
       Input.Version==MTK_PMIC_BROKER_VERSION_KEY_IRQ) {
        RejectV2QueryOnce=0;
        Request->State=Core.State;Request->Generation=Core.Generation;
        Request->Result=MtkPmicBrokerResultInvalid;
        Request->Address=Core.Address;Request->Value=Core.Value;
        LastResponse[Input.Operation]=*Request;
        return STATUS_SUCCESS;
    }
    MTK_PMIC_BROKER_RESULT Result=MtkPmicBrokerHandle(&Core,&BrokerIo,(int)OtherOwned,Request);
    if(Result==MtkPmicBrokerResultBusy) {
        *Request=Input;Request->Result=MtkPmicBrokerResultBusy;
    }
    if(ArmOtherOwnedAfterBegin && Input.Operation==MtkPmicBrokerBeginRead &&
       Result==MtkPmicBrokerResultInProgress) {
        ArmOtherOwnedAfterBegin=0;OtherOwned=1;OtherOwnedClearAfterStalls=1;
    }
    if(RewriteAcceptedResultInvalid && Input.Operation==MtkPmicBrokerAcquireKeyLease) {
        RewriteAcceptedResultInvalid=0;
        Request->Result=MtkPmicBrokerResultInvalid;
    }
    if(CorruptOnce && Input.Operation==CorruptOperation && CorruptSkip) {
        --CorruptSkip;
    } else if(CorruptOnce && Input.Operation==CorruptOperation) {
        CorruptOnce=0;
        switch(CorruptField) {
        case CorruptSize:Request->Size++;break;
        case CorruptVersion:Request->Version++;break;
        case CorruptOperationField:Request->Operation++;break;
        case CorruptFlags:Request->Flags=1;break;
        case CorruptState:Request->State=0;break;
        case CorruptGeneration:Request->Generation^=1;break;
        case CorruptResult:Request->Result=MtkPmicBrokerResultSuccess;break;
        case CorruptAddress:Request->Address^=2;break;
        case CorruptValue:Request->Value^=1;break;
        case CorruptReserved0:Request->Reserved0=1;break;
        default:Request->Reserved[0]=1;break;
        }
    }
    if(NonzeroEfiOnce && Input.Operation==NonzeroEfiOperation) {
        NonzeroEfiOnce=0;*EfiStatus=InvokeEfiStatus?InvokeEfiStatus:1;
    }
    if(InvokeStatusOnce && Input.Operation==InvokeStatusOperation) {
        InvokeStatusOnce=0;return InvokeStatus;
    }
    if(Input.Operation<32U)LastResponse[Input.Operation]=*Request;
    return STATUS_SUCCESS;
}
static KIRQL TestCurrentIrql(void) {return (KIRQL)CurrentIrql;}
static ULONGLONG TestQueryInterruptTime(void) {return Time100ns;}
static VOID TestStall(ULONG Microseconds) {
    CHECK(Microseconds==10 && LockHeld==1);++Stalls;
    if(ReenterOnStall) {
        uint16_t Value=0xa55a;ReenterOnStall=0;
        ReenterResult=MtkMaliPmicPrmRead16(0x1520,&Value);
        CHECK(ReenterResult==MaliBusy && Value==0xa55a);
    }
    TimeUs+=Microseconds;Time100ns+=(ULONGLONG)Microseconds*10;
    if(OtherOwnedClearAfterStalls && Stalls>=OtherOwnedClearAfterStalls) {
        OtherOwned=0;OtherOwnedClearAfterStalls=0;
    }
}
static NTSTATUS TestDelayExecutionThread(KPROCESSOR_MODE WaitMode,BOOLEAN Alertable,PLARGE_INTEGER Interval) {
    CHECK(WaitMode==KernelMode && !Alertable && Interval && Interval->QuadPart<0);
    if(!Interval || Interval->QuadPart>=0)return STATUS_INVALID_PARAMETER;
    Time100ns+=(ULONGLONG)(-Interval->QuadPart);
    return STATUS_SUCCESS;
}
static LONG TestInterlockedCompareExchange(volatile LONG *Target,LONG Exchange,LONG Comparand) {
    LONG Old=*Target;
    if(Target==&PmicClientState && Old==1 && Exchange==0 && Comparand==1 &&
       PublicationCheck) {
        ULONG Mode=PublicationCheck;PublicationCheck=0;
        CHECK(!LockHeld);
        if(Mode==1) {
            CHECK(PmicKeyLeaseToken!=0 && PmicKeyLeaseToken==Core.KeyLeaseToken &&
                  PublicationLeaseOutput && *PublicationLeaseOutput==PmicKeyLeaseToken);
        } else if(Mode==2) {
            CHECK(PmicKeySnapshotToken!=0 &&
                  PmicKeySnapshotToken==Core.KeySnapshotToken &&
                  PmicKeySnapshotLeaseToken==PmicKeyLeaseToken &&
                  PublicationSnapshotOutput &&
                  PublicationSnapshotOutput->Generation==PmicKeySnapshotToken &&
                  (((uint32_t)PublicationSnapshotOutput->GroupStatus<<16)|
                    PublicationSnapshotOutput->TopStatus)==PmicKeySnapshotPacked);
        } else if(Mode==3) {
            CHECK(PmicKeySnapshotToken==0 && PmicKeySnapshotLeaseToken==0 &&
                  PmicKeySnapshotPacked==0 && PmicKeyLeaseToken!=0 &&
                  Core.KeySnapshotValid==0);
        } else {
            CHECK(PmicKeyLeaseToken==0 && PmicKeySnapshotToken==0 &&
                  PmicKeySnapshotLeaseToken==0 && PmicKeySnapshotPacked==0 &&
                  Core.KeyLeaseState==0);
        }
    }
    if(Old==Comparand)*Target=Exchange;
    return Old;
}
static LONG TestInterlockedExchange(volatile LONG *Target,LONG Exchange) {
    LONG Old=*Target;*Target=Exchange;return Old;
}

static void Reset(void) {
    RtlZeroMemory(&Core,sizeof(Core));MtkPmicBrokerInitialize(&Core);
    RtlZeroMemory(PmicRegisters,sizeof(PmicRegisters));Fsm=WriteData=Response=OtherOwned=0;
    Time100ns=0;TimeUs=0;GetCalls=LockCalls=UnlockCalls=QueryCalls=InvokeCalls=Stalls=0;
    ReadAddressCount=WriteCommandCount=RejectLegacyKeyOpcode=RejectV2QueryOnce=0;
    AdcAutoReady=1;AdcRequestWrites=LastAdcRequest=0;PmicBatteryBifRestoreFault=0;
    PmicBatteryTrimRestoreFault=0;PmicBatteryVoltageTrimValid=FALSE;
    RtlZeroMemory(&PmicBatteryVoltageTrim,sizeof(PmicBatteryVoltageTrim));
    OtpStuckBusy=OtpTriggerCount=OtpRowCount=FailWriteCommandValue=0;
    FailWriteCommandAddress=0;
    OtpFuseWords[0]=0x0404U;OtpFuseWords[1]=0x0200U;
    OtpFuseWords[2]=0x0100U;OtpFuseWords[3]=0x1000U;
    RtlZeroMemory(OtpRows,sizeof(OtpRows));
    RtlZeroMemory(&PmicBatteryGaugeCore,sizeof(PmicBatteryGaugeCore));
    RtlZeroMemory(&PmicBatteryCurrentContext,sizeof(PmicBatteryCurrentContext));
    PmicBatteryCurrentBusy=0;
    RtlZeroMemory(ReadAddresses,sizeof(ReadAddresses));
    RtlZeroMemory(WriteCommandAddresses,sizeof(WriteCommandAddresses));
    RtlZeroMemory(WriteCommandValues,sizeof(WriteCommandValues));
    RtlZeroMemory(LastInput,sizeof(LastInput));
    RtlZeroMemory(LastResponse,sizeof(LastResponse));
    RtlZeroMemory(OperationCalls,sizeof(OperationCalls));
    LockHeld=0;Found=1;RtlZeroMemory(ForceBusy,sizeof(ForceBusy));
    OtherOwnedClearAfterStalls=ArmOtherOwnedAfterBegin=0;
    CorruptOperation=CorruptOnce=CorruptBusyOperation=CorruptBusyOnce=CorruptField=CorruptSkip=0;
    NonzeroEfiOperation=NonzeroEfiOnce=0;ReenterOnStall=0;ReenterResult=MaliFault;PmicClientState=0;
    PmicKeyLeaseToken=PmicKeySnapshotToken=PmicKeySnapshotLeaseToken=0;
    PmicKeySnapshotPacked=0;
    InvokeStatusOperation=InvokeStatusOnce=0;InvokeEfiStatus=0;
    FailWriteCommandAddress=RewriteAcceptedResultInvalid=0;
    ReenterOnUnlock=PublicationCheck=0;
    PublicationLeaseOutput=0;PublicationSnapshotOutput=0;
    GetStatus=LockStatus=QueryStatus=UnlockStatus=InvokeStatus=STATUS_SUCCESS;
    CurrentIrql=PASSIVE_LEVEL;
    BrokerIo.Context=PmicRegisters;BrokerIo.Read32=TestPmicRead32;BrokerIo.Write32=TestPmicWrite32;BrokerIo.NowUs=TestNowUs;
    MockInterface.Version=PRM_INTERFACE_V1;MockInterface.UnlockModule=TestUnlockModule;
    MockInterface.LockModule=TestLockModule;MockInterface.InvokeHandler=TestInvokeHandler;
    MockInterface.QueryHandler=TestQueryHandler;
}
static void SuccessfulTransfers(void) {
    Reset();PmicRegisters[0x1520/2]=1;ForceBusy[MtkPmicBrokerBeginRead]=1;
    ForceBusy[MtkPmicBrokerStep]=1;ForceBusy[MtkPmicBrokerAck]=1;
    uint16_t Value=0xa55a;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliOk && Value==1);
    CHECK(GetCalls==1 && LockCalls==1 && QueryCalls==1 && UnlockCalls==1 && !LockHeld);
    CHECK(InvokeCalls>=7 && Stalls>=3 && !ForceBusy[MtkPmicBrokerBeginRead] &&
          !ForceBusy[MtkPmicBrokerStep] && !ForceBusy[MtkPmicBrokerAck] &&
          Core.State==MtkPmicBrokerStateIdle && Core.Generation==1 && PmicClientState==0);
    Reset();PmicRegisters[0x1520/2]=2;ForceBusy[MtkPmicBrokerBeginRead]=1;ReenterOnStall=1;Value=0xa55a;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliOk && Value==2 && ReenterResult==MaliBusy);
    CHECK(GetCalls==1 && LockCalls==1 && UnlockCalls==1 && !LockHeld);
    Reset();PmicRegisters[0x1526/2]=0x9b81;
    CHECK(MtkMaliPmicPrmWrite16(0x1526,0x9b85)==MaliOk && PmicRegisters[0x1526/2]==0x9b85);
    CHECK(LockCalls==1 && QueryCalls==1 && UnlockCalls==1 && Core.State==MtkPmicBrokerStateIdle && PmicClientState==0);
}
static void PowerButtonReadOnly(void) {
    for(unsigned Raw=0;Raw<4;Raw++) {
        Reset();PmicRegisters[0x28/2]=(uint16_t)Raw;
        ForceBusy[MtkPmicBrokerBeginRead]=1;
        uint16_t Value=0xbeef;
        CHECK(MtkMaliPmicPrmRead16(0x28,&Value)==MaliOk && Value==Raw);
        CHECK(Core.Address==0x28 && Core.State==MtkPmicBrokerStateIdle && !LockHeld);
        ULONG Gets=GetCalls,Invokes=InvokeCalls;
        CHECK(MtkMaliPmicPrmWrite16(0x28,0)==MaliInvalid &&
              MtkMaliPmicPrmWrite16(0x28,2)==MaliInvalid);
        CHECK(GetCalls==Gets && InvokeCalls==Invokes && PmicRegisters[0x28/2]==Raw);
        CHECK(MtkMaliPmicPrmRead16(0x26,&Value)==MaliInvalid &&
              MtkMaliPmicPrmRead16(0x2a,&Value)==MaliInvalid);
        CHECK(GetCalls==Gets && InvokeCalls==Invokes);
    }
}
static void KeySnapshotTransactions(void) {
    MTK_PMIC_KEY_SNAPSHOT Snapshot;
    ULONG Invokes,Reads;
    Reset();
    PmicRegisters[0x19e/2]=1U<<2;
    PmicRegisters[0x91c/2]=0xa1b2;
    PmicRegisters[0x28/2]=0xc3d4;
    ForceBusy[MtkPmicBrokerBeginKeySnapshot]=1;
    ForceBusy[MtkPmicBrokerStep]=2;
    ForceBusy[MtkPmicBrokerAck]=1;
    Snapshot.GroupStatus=0xdead;Snapshot.TopStatus=0xbeef;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliOk &&
          Snapshot.GroupStatus==0xa1b2 && Snapshot.TopStatus==0xc3d4);
    CHECK(ReadAddressCount==3 && ReadAddresses[0]==0x19e &&
          ReadAddresses[1]==0x91c && ReadAddresses[2]==0x28);
    CHECK(WriteCommandCount==0 && PmicRegisters[0x91c/2]==0xa1b2 &&
          PmicRegisters[0x28/2]==0xc3d4);
    CHECK(Core.State==MtkPmicBrokerStateIdle && Core.Address==0x91c &&
          Core.Value==0xa1b2c3d4U && Core.Generation==1 && !LockHeld &&
          GetCalls==1 && LockCalls==1 && QueryCalls==1 && UnlockCalls==1 &&
          PmicClientState==0 && Stalls>=4);

    /* Without summary bit 2, the group status is zero and its PMIC register
       is never read; other summary bits do not accidentally select group 2. */
    Reset();
    PmicRegisters[0x19e/2]=1U<<3;
    PmicRegisters[0x91c/2]=0x7654;
    PmicRegisters[0x28/2]=0x0123;
    Snapshot.GroupStatus=0xaaaa;Snapshot.TopStatus=0xbbbb;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliOk &&
          Snapshot.GroupStatus==0 && Snapshot.TopStatus==0x0123);
    CHECK(ReadAddressCount==2 && ReadAddresses[0]==0x19e && ReadAddresses[1]==0x28 &&
          WriteCommandCount==0 && PmicRegisters[0x91c/2]==0x7654);

    /* A previous idle transaction leaves cached metadata. A legacy v1 broker
       that rejects opcode 5 must not be mistaken for an accepted snapshot. */
    Reset();
    PmicRegisters[0x1520/2]=0x4321;
    uint16_t Previous=0;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Previous)==MaliOk && Previous==0x4321);
    Invokes=InvokeCalls;Reads=ReadAddressCount;
    RejectLegacyKeyOpcode=1;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliUnsupported &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468);
    CHECK(InvokeCalls==Invokes+1 && ReadAddressCount==Reads && LockCalls==2 &&
          UnlockCalls==2 && !LockHeld && PmicClientState==0 &&
          Core.State==MtkPmicBrokerStateIdle && Core.Generation==1 &&
          Core.Address==0x1520 && Core.Value==0x4321);

    Reset();Found=0;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliUnsupported &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          UnlockCalls==1 && !LockHeld && PmicClientState==0);

    Reset();PmicRegisters[0x19e/2]=1U<<2;PmicRegisters[0x91c/2]=0x0102;
    PmicRegisters[0x28/2]=0x0304;
    CorruptOperation=MtkPmicBrokerBeginKeySnapshot;CorruptOnce=1;CorruptField=CorruptAddress;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliFault &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStatePending && !UnlockCalls && LockHeld &&
          PmicClientState==2);

    /* Fail after the group register has been read but before TOPSTATUS
       completes. The caller must not observe a half-filled snapshot. */
    Reset();PmicRegisters[0x19e/2]=1U<<2;PmicRegisters[0x91c/2]=0x5566;
    PmicRegisters[0x28/2]=0x7788;
    CorruptOperation=MtkPmicBrokerStep;CorruptOnce=1;CorruptSkip=8;
    CorruptField=CorruptAddress;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliFault &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStatePending && Core.SnapshotGroupStatus==0x5566 &&
          !UnlockCalls && LockHeld && PmicClientState==2);

    Reset();PmicRegisters[0x19e/2]=1U<<2;PmicRegisters[0x91c/2]=0x0102;
    PmicRegisters[0x28/2]=0x0304;
    CorruptOperation=MtkPmicBrokerStep;CorruptOnce=1;CorruptField=CorruptValue;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliFault &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStatePending && !UnlockCalls && LockHeld &&
          PmicClientState==2);

    Reset();PmicRegisters[0x19e/2]=1U<<2;PmicRegisters[0x91c/2]=0x0102;
    PmicRegisters[0x28/2]=0x0304;
    CorruptOperation=MtkPmicBrokerAck;CorruptOnce=1;CorruptField=CorruptAddress;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliFault &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStateIdle && !UnlockCalls && LockHeld &&
          PmicClientState==2);

    Reset();
    ForceBusy[MtkPmicBrokerBeginKeySnapshot]=PMIC_PRM_MAX_POLLS+1;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliBusy &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStateIdle && !LockHeld && UnlockCalls==1 &&
          PmicClientState==0 && Core.Generation==0);

    Reset();PmicRegisters[0x19e/2]=1U<<2;PmicRegisters[0x91c/2]=0x0102;
    PmicRegisters[0x28/2]=0x0304;
    ForceBusy[MtkPmicBrokerStep]=PMIC_PRM_MAX_POLLS+1;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliTimeout &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStatePending && !UnlockCalls && LockHeld &&
          PmicClientState==2);

    Reset();PmicRegisters[0x19e/2]=1U<<2;PmicRegisters[0x91c/2]=0x0102;
    PmicRegisters[0x28/2]=0x0304;
    ForceBusy[MtkPmicBrokerAck]=PMIC_PRM_MAX_POLLS+1;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliTimeout &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStateDone && !UnlockCalls && LockHeld &&
          PmicClientState==2);

    Reset();PmicRegisters[0x19e/2]=1U<<2;PmicRegisters[0x91c/2]=0x0102;
    PmicRegisters[0x28/2]=0x0304;UnlockStatus=STATUS_TIMEOUT;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliFault &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          Core.State==MtkPmicBrokerStateIdle && UnlockCalls==1 &&
          PmicClientState==2);
    Reset();CurrentIrql=DISPATCH_LEVEL;
    Snapshot.GroupStatus=0x1357;Snapshot.TopStatus=0x2468;
    CHECK(MtkMaliPmicPrmReadKeySnapshot(&Snapshot)==MaliInvalid &&
          Snapshot.GroupStatus==0x1357 && Snapshot.TopStatus==0x2468 &&
          !GetCalls && !LockCalls);
    CHECK(MtkMaliPmicPrmReadKeySnapshot(NULL)==MaliInvalid);
}
static void SharedBusyAndAddressPolicy(void) {
    Reset();PmicRegisters[0x1520/2]=0x34;OtherOwned=1;OtherOwnedClearAfterStalls=1;
    uint16_t Value=0xbeef;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliOk && Value==0x34 && !OtherOwned);
    CHECK(Stalls>=1 && Core.State==MtkPmicBrokerStateIdle && UnlockCalls==1);
    Reset();PmicRegisters[0x1520/2]=0x56;ArmOtherOwnedAfterBegin=1;Value=0xbeef;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliOk && Value==0x56 && !OtherOwned);
    CHECK(Stalls>=1 && Core.State==MtkPmicBrokerStateIdle && UnlockCalls==1);
    Reset();Value=0xbeef;ULONG Calls=GetCalls;
    CHECK(MtkMaliPmicPrmRead16(0x1540,&Value)==MaliInvalid && GetCalls==Calls && Value==0xbeef);
    CHECK(MtkMaliPmicPrmWrite16(0x1520,1)==MaliInvalid && GetCalls==Calls);
    CurrentIrql=DISPATCH_LEVEL;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliInvalid && GetCalls==Calls);
}
static void InterfaceFailures(void) {
    uint16_t Value;
    Reset();Value=0xa55a;GetStatus=STATUS_TIMEOUT;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a && !LockCalls);
    Reset();LockStatus=STATUS_TIMEOUT;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && LockCalls==1 && !UnlockCalls);
    Reset();QueryStatus=STATUS_TIMEOUT;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && QueryCalls==1 && UnlockCalls==1 && !LockHeld);
    Reset();Found=0;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliUnsupported && UnlockCalls==1 && !LockHeld);
    Reset();Value=0xa55a;MockInterface.Version=0;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliUnsupported && Value==0xa55a && !LockCalls);
    Reset();Value=0xa55a;MockInterface.QueryHandler=0;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliUnsupported && Value==0xa55a && !LockCalls);
    Reset();UnlockStatus=STATUS_TIMEOUT;PmicRegisters[0x1520/2]=7;Value=0xa55a;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a && UnlockCalls==1);
    Reset();NonzeroEfiOperation=MtkPmicBrokerBeginRead;NonzeroEfiOnce=1;Value=0xa55a;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a);
    CHECK(Core.State==MtkPmicBrokerStatePending && InvokeCalls==1 && !UnlockCalls);
    CHECK(PmicClientState==2 && LockHeld==1);
    ULONG Gets=GetCalls,Locks=LockCalls,Invokes=InvokeCalls;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && GetCalls==Gets && LockCalls==Locks && InvokeCalls==Invokes);
    static const NTSTATUS AmbiguousInvokeStatus[]={STATUS_DEVICE_BUSY,STATUS_RETRY,STATUS_TIMEOUT};
    for(ULONG I=0;I<sizeof(AmbiguousInvokeStatus)/sizeof(AmbiguousInvokeStatus[0]);I++) {
        Reset();InvokeStatusOperation=MtkPmicBrokerBeginRead;InvokeStatusOnce=1;
        InvokeStatus=AmbiguousInvokeStatus[I];Value=0xa55a;
        CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a);
        CHECK(Core.State==MtkPmicBrokerStatePending && InvokeCalls==1 && !UnlockCalls &&
              LockHeld==1 && PmicClientState==2);
        ULONG Gets=GetCalls,Locks=LockCalls,Unlocks=UnlockCalls,Invokes=InvokeCalls;
        uint16_t RetryValue=0x5a5a;
        CHECK(MtkMaliPmicPrmRead16(0x1520,&RetryValue)==MaliFault && RetryValue==0x5a5a);
        CHECK(GetCalls==Gets && LockCalls==Locks && UnlockCalls==Unlocks && InvokeCalls==Invokes);
    }
}
static void MalformedAndTimeout(void) {
    uint16_t Value;
    for(ULONG Field=CorruptSize;Field<=CorruptReserved;Field++) {
        Reset();CorruptOperation=MtkPmicBrokerBeginRead;CorruptOnce=1;CorruptField=Field;Value=0xa55a;
        CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a);
        CHECK(Core.State==MtkPmicBrokerStatePending && InvokeCalls==1 && !UnlockCalls &&
              LockHeld==1 && PmicClientState==2);
    }
    Reset();ForceBusy[MtkPmicBrokerBeginRead]=1;CorruptBusyOperation=MtkPmicBrokerBeginRead;
    CorruptBusyOnce=1;Value=0xa55a;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a);
    CHECK(Core.State==MtkPmicBrokerStateIdle && InvokeCalls==1 && !UnlockCalls && LockHeld==1 && PmicClientState==2);
    ULONG Gets=GetCalls;CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && GetCalls==Gets);
    Reset();CorruptOperation=MtkPmicBrokerStep;CorruptOnce=1;PmicRegisters[0x1520/2]=0x1234;Value=0xa55a;
    CorruptField=CorruptReserved;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a);
    CHECK(Core.State==MtkPmicBrokerStatePending && InvokeCalls==2 && !UnlockCalls && LockHeld==1);
    Reset();CorruptOperation=MtkPmicBrokerAck;CorruptOnce=1;CorruptField=CorruptAddress;
    PmicRegisters[0x1520/2]=0x87;Value=0xa55a;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliFault && Value==0xa55a);
    CHECK(Core.State==MtkPmicBrokerStateIdle && !UnlockCalls && LockHeld==1 && PmicClientState==2);
    Reset();Fsm=2; /* A stuck foreign WACS state is never cleared or replayed. */
    Value=0xa55a;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Value)==MaliTimeout && Value==0xa55a);
    CHECK(Core.Generation==1 && Core.State==MtkPmicBrokerStatePending && !UnlockCalls);
    CHECK(InvokeCalls<=PMIC_PRM_MAX_POLLS+2 && Stalls<=PMIC_PRM_MAX_POLLS);
    CHECK(PmicClientState==2 && LockHeld==1 && UnlockCalls==0);
}

static void KeyIrqLeaseTransactions(void) {
    static const uint16_t Borrowed[]={0,1,4,5};
    MTK_PMIC_KEY_IRQ_SNAPSHOT Snapshot,Old;
    uint64_t Lease;
    ULONG Writes,Steps,Invokes;
    Reset();
    CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliOk && Core.Generation==0 &&
          ReadAddressCount==0 && WriteCommandCount==0 && !LockHeld &&
          PmicClientState==0 && LastResponse[MtkPmicBrokerQuery].Flags==3);
    ForceBusy[MtkPmicBrokerQuery]=1;
    CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliBusy && !LockHeld &&
          PmicClientState==0 && Core.Generation==0);
    CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliOk);
    for(ULONG I=0;I<RTL_NUMBER_OF(Borrowed);++I) {
        Reset();PmicRegisters[0x910/2]=(uint16_t)(0x42U|Borrowed[I]);
        Lease=0xa55a;PublicationLeaseOutput=&Lease;PublicationCheck=1;
        ReenterOnUnlock=1;
        ForceBusy[MtkPmicBrokerAcquireKeyLease]=1;
        CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliOk &&
              Lease!=0 && Lease!=0xa55a && PublicationCheck==0 &&
              PmicRegisters[0x910/2]==0x47 && Core.KeyLeaseToken==Lease &&
              PmicKeyLeaseToken==Lease && !LockHeld && PmicClientState==0);
        Invokes=InvokeCalls;
        uint64_t Other=0xbeef;
        CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Other)==MaliBusy &&
              Other==0xbeef && InvokeCalls==Invokes);
        PmicRegisters[0x19e/2]=4;PmicRegisters[0x91c/2]=1;
        PmicRegisters[0x28/2]=2;
        PublicationSnapshotOutput=&Snapshot;PublicationCheck=2;ReenterOnUnlock=1;
        CHECK(MtkMaliPmicPrmReadKeyIrqSnapshot(Lease,&Snapshot)==MaliOk &&
              Snapshot.Generation>Lease && Snapshot.GroupStatus==1 &&
              Snapshot.TopStatus==2 && Core.KeySnapshotValid &&
              PmicKeySnapshotToken==Snapshot.Generation && PublicationCheck==0);
        Old=Snapshot;Invokes=InvokeCalls;Writes=WriteCommandCount;
        CHECK(MtkMaliPmicPrmReadKeyIrqSnapshot(Lease,&Snapshot)==MaliBusy &&
              Snapshot.Generation==Old.Generation && InvokeCalls==Invokes);
        CHECK(MtkMaliPmicPrmReleaseKeyIrqLease(Lease)==MaliInvalid &&
              PmicKeyLeaseToken==Lease && Core.KeySnapshotValid &&
              WriteCommandCount==Writes && !LockHeld && PmicClientState==0);
        /* A new release cause after capture must survive this press-only W1C. */
        PmicRegisters[0x91c/2]=5;
        PublicationCheck=3;ReenterOnUnlock=1;
        CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Snapshot)==MaliOk &&
              PublicationCheck==0 && Core.KeySnapshotValid==0 &&
              PmicRegisters[0x91c/2]==4 && WriteCommandCount==Writes+1 &&
              WriteCommandAddresses[Writes]==0x91c && WriteCommandValues[Writes]==1);
        CHECK(LastInput[MtkPmicBrokerAckKeySnapshot].Generation==Old.Generation &&
              LastInput[MtkPmicBrokerAckKeySnapshot].Address==0 &&
              LastInput[MtkPmicBrokerAckKeySnapshot].Value==0);
        Invokes=InvokeCalls;
        CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Old)==MaliInvalid &&
              InvokeCalls==Invokes);
        CHECK(MtkMaliPmicPrmReadKeyIrqSnapshot(Lease,&Snapshot)==MaliOk &&
              Snapshot.Generation>Old.Generation && Snapshot.GroupStatus==4);
        CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Snapshot)==MaliOk &&
              PmicRegisters[0x91c/2]==0);
        PmicRegisters[0x910/2]|=0x80;Writes=WriteCommandCount;
        Steps=OperationCalls[MtkPmicBrokerStep];
        PublicationCheck=4;ReenterOnUnlock=1;
        CHECK(MtkMaliPmicPrmReleaseKeyIrqLease(Lease)==MaliOk &&
              PublicationCheck==0 && PmicKeyLeaseToken==0 &&
              PmicRegisters[0x910/2]==(uint16_t)(0xc2U|Borrowed[I]) &&
              !LockHeld && PmicClientState==0);
        CHECK(LastInput[MtkPmicBrokerReleaseKeyLease].Generation==Lease &&
              LastInput[MtkPmicBrokerReleaseKeyLease].Address==0 &&
              LastInput[MtkPmicBrokerReleaseKeyLease].Value==0);
        if(Borrowed[I]==5) {
            CHECK(WriteCommandCount==Writes &&
                  OperationCalls[MtkPmicBrokerStep]==Steps &&
                  LastResponse[MtkPmicBrokerReleaseKeyLease].State==
                    MtkPmicBrokerStateDone);
        } else CHECK(WriteCommandCount==Writes+1);
    }
}
static void KeyIrqSiblingAndLocalTokens(void) {
    uint64_t Lease=0;
    MTK_PMIC_KEY_IRQ_SNAPSHOT Snapshot,Bad;
    ULONG Invokes,Writes;
    Reset();
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliOk);
    PmicRegisters[0x19e/2]=4;PmicRegisters[0x91c/2]=5;PmicRegisters[0x28/2]=0x22;
    CHECK(MtkMaliPmicPrmReadKeyIrqSnapshot(Lease,&Snapshot)==MaliOk);
    Writes=WriteCommandCount;Invokes=InvokeCalls;
    Bad=Snapshot;Bad.TopStatus^=1;
    CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Bad)==MaliInvalid && InvokeCalls==Invokes);
    Bad=Snapshot;Bad.Generation++;
    CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Bad)==MaliInvalid && InvokeCalls==Invokes);
    CHECK(MtkMaliPmicPrmReadKeyIrqSnapshot(Lease+1,&Bad)==MaliInvalid &&
          MtkMaliPmicPrmReleaseKeyIrqLease(Lease+1)==MaliInvalid && InvokeCalls==Invokes);
    PmicRegisters[0x91c/2]=0x8007;
    CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Snapshot)==MaliBusy &&
          Core.KeySnapshotValid && PmicKeySnapshotToken==Snapshot.Generation &&
          Core.State==MtkPmicBrokerStateIdle && !LockHeld && PmicClientState==0 &&
          WriteCommandCount==Writes && PmicRegisters[0x91c/2]==0x8007 &&
          LastResponse[MtkPmicBrokerStep].Result==MtkPmicBrokerResultSiblingPending);
    CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliOk && Core.KeySnapshotValid);
    PmicRegisters[0x91c/2]=5;
    CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Snapshot)==MaliOk &&
          WriteCommandCount==Writes+1 && PmicRegisters[0x91c/2]==0 &&
          !Core.KeySnapshotValid && PmicKeySnapshotToken==0);
    CHECK(MtkMaliPmicPrmReleaseKeyIrqLease(Lease)==MaliOk);
}
static void KeyIrqFailuresAndPublication(void) {
    uint64_t Lease;
    MTK_PMIC_KEY_IRQ_SNAPSHOT Snapshot;
    ULONG Invokes,Writes;
    Reset();uint16_t Previous=0;
    PmicRegisters[0x1520/2]=0x4321;
    CHECK(MtkMaliPmicPrmRead16(0x1520,&Previous)==MaliOk);
    RejectV2QueryOnce=1;
    CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliUnsupported &&
          Core.Generation==1 && !LockHeld && PmicClientState==0);
    Reset();Lease=0xbeef;ForceBusy[MtkPmicBrokerAcquireKeyLease]=PMIC_PRM_MAX_POLLS+1;
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliBusy &&
          Lease==0xbeef && Core.Generation==0 && ReadAddressCount==0 &&
          WriteCommandCount==0 && !LockHeld && PmicClientState==0);
    Reset();Lease=0xbeef;ForceBusy[MtkPmicBrokerStep]=PMIC_PRM_MAX_POLLS+1;
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliTimeout &&
          Lease==0xbeef && PmicKeyLeaseToken==0 &&
          Core.State==MtkPmicBrokerStatePending && !UnlockCalls &&
          LockHeld && PmicClientState==2);
    Invokes=InvokeCalls;
    CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliFault && InvokeCalls==Invokes);
    Reset();Lease=0xbeef;UnlockStatus=STATUS_TIMEOUT;
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliFault &&
          Lease==0xbeef && PmicKeyLeaseToken==0 && Core.KeyLeaseToken!=0 &&
          Core.State==MtkPmicBrokerStateIdle && UnlockCalls==1 && PmicClientState==2);
    Reset();Lease=0xbeef;RewriteAcceptedResultInvalid=1;
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliFault &&
          Lease==0xbeef && Core.State==MtkPmicBrokerStatePending &&
          LockHeld && !UnlockCalls && PmicClientState==2);
    for(ULONG Field=CorruptSize;Field<=CorruptReserved;Field++) {
        Reset();Lease=0xbeef;CorruptOperation=MtkPmicBrokerAcquireKeyLease;
        CorruptOnce=1;CorruptField=Field;
        CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliFault &&
              Lease==0xbeef && !PmicKeyLeaseToken && LockHeld &&
              !UnlockCalls && PmicClientState==2);
    }
    Reset();Lease=0;
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliOk);
    PmicRegisters[0x19e/2]=4;PmicRegisters[0x91c/2]=1;PmicRegisters[0x28/2]=2;
    Snapshot.Generation=0xdead;Snapshot.GroupStatus=0xbeef;Snapshot.TopStatus=0xa55a;
    CorruptOperation=MtkPmicBrokerAck;CorruptOnce=1;CorruptField=CorruptAddress;
    CHECK(MtkMaliPmicPrmReadKeyIrqSnapshot(Lease,&Snapshot)==MaliFault &&
          Snapshot.Generation==0xdead && Snapshot.GroupStatus==0xbeef &&
          Snapshot.TopStatus==0xa55a && Core.KeySnapshotValid &&
          PmicKeySnapshotToken==0 && LockHeld && PmicClientState==2);
    Reset();Lease=0;
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliOk);
    PmicRegisters[0x19e/2]=4;PmicRegisters[0x91c/2]=5;PmicRegisters[0x28/2]=2;
    CHECK(MtkMaliPmicPrmReadKeyIrqSnapshot(Lease,&Snapshot)==MaliOk);
    FailWriteCommandAddress=0x91c;Writes=WriteCommandCount;
    CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Snapshot)==MaliFault &&
          PmicRegisters[0x91c/2]==0 && WriteCommandCount==Writes+1 &&
          Core.State==MtkPmicBrokerStateFault && Core.KeySnapshotValid &&
          PmicKeySnapshotToken==Snapshot.Generation && PmicClientState==2 && LockHeld);
    Invokes=InvokeCalls;
    CHECK(MtkMaliPmicPrmAckKeyIrqSnapshot(&Snapshot)==MaliFault &&
          MtkMaliPmicPrmReleaseKeyIrqLease(Lease)==MaliFault &&
          InvokeCalls==Invokes && WriteCommandCount==Writes+1);
    Reset();Lease=0;
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliOk);
    Writes=WriteCommandCount;FailWriteCommandAddress=0x910;
    CHECK(MtkMaliPmicPrmReleaseKeyIrqLease(Lease)==MaliFault &&
          PmicRegisters[0x910/2]==0 && WriteCommandCount==Writes+1 &&
          PmicKeyLeaseToken==Lease && Core.KeyLeaseToken==Lease &&
          Core.State==MtkPmicBrokerStateFault && PmicClientState==2 && LockHeld);
    Reset();CurrentIrql=DISPATCH_LEVEL;Lease=0xbeef;
    CHECK(MtkMaliPmicPrmQueryKeyIrq()==MaliInvalid &&
          MtkMaliPmicPrmAcquireKeyIrqLease(&Lease)==MaliInvalid &&
          Lease==0xbeef && !GetCalls);
    CHECK(MtkMaliPmicPrmAcquireKeyIrqLease(NULL)==MaliInvalid &&
          MtkMaliPmicPrmReadKeyIrqSnapshot(1,NULL)==MaliInvalid &&
          MtkMaliPmicPrmAckKeyIrqSnapshot(NULL)==MaliInvalid);
}

static int GaugeAcquire(void *Context) {
    uint64_t *Lease=(uint64_t *)Context;
    MALI_RESULT Result=MtkMaliPmicPrmAcquireGaugeLease(Lease);
    if(Result!=MaliOk)
        TEST_TRACE("GaugeAcquire=%u lease=%llu brokerResult=%u state=%u gen=%llu get=%u lock=%u query=%u invoke=%u unlock=%u client=%d\\n",
            (unsigned)Result,(unsigned long long)*Lease,Core.FinalResult,Core.State,
            (unsigned long long)Core.Generation,GetCalls,LockCalls,QueryCalls,InvokeCalls,UnlockCalls,(int)PmicClientState);
    return Result==MaliOk;
}
static int GaugeRelease(void *Context) {
    uint64_t *Lease=(uint64_t *)Context;
    if(MtkMaliPmicPrmReleaseGaugeLease(*Lease)!=MaliOk)return 0;
    *Lease=0;
    return 1;
}
static int GaugeRead(void *Context,uint16_t Address,uint16_t *Value) {
    MALI_RESULT Result=MtkMaliPmicPrmGaugeRead16(*(uint64_t *)Context,Address,Value);
    if(Result!=MaliOk)TEST_TRACE("GaugeRead addr=%04x result=%u lease=%llu state=%u op=%u final=%u input=%u/%u/%u/%llu response=%u/%u/%u/%llu/%x/%x\n",
        Address,(unsigned)Result,(unsigned long long)*(uint64_t *)Context,Core.State,
        Core.Operation,Core.FinalResult,
        LastInput[MtkPmicBrokerBeginGaugeRead].Version,LastInput[MtkPmicBrokerBeginGaugeRead].Operation,
        LastInput[MtkPmicBrokerBeginGaugeRead].Address,(unsigned long long)LastInput[MtkPmicBrokerBeginGaugeRead].Generation,
        LastResponse[MtkPmicBrokerBeginGaugeRead].Version,LastResponse[MtkPmicBrokerBeginGaugeRead].Operation,
        LastResponse[MtkPmicBrokerBeginGaugeRead].State,(unsigned long long)LastResponse[MtkPmicBrokerBeginGaugeRead].Generation,
        LastResponse[MtkPmicBrokerBeginGaugeRead].Address,LastResponse[MtkPmicBrokerBeginGaugeRead].Value);
    return Result==MaliOk;
}
static int GaugeWrite(void *Context,uint16_t Address,uint16_t Value) {
    MALI_RESULT Result=MtkMaliPmicPrmGaugeWrite16(*(uint64_t *)Context,Address,Value);
    if(Result!=MaliOk)TEST_TRACE("GaugeWrite addr=%04x value=%04x result=%u lease=%llu state=%u op=%u final=%u\n",
        Address,Value,(unsigned)Result,(unsigned long long)*(uint64_t *)Context,Core.State,
        Core.Operation,Core.FinalResult);
    return Result==MaliOk;
}
static void GaugeCurrentSnapshot(void) {
    MTK_GAUGE_CORE Gauge={0};
    MTK_GAUGE_IO Io={0};
    uint64_t Lease=0;
    uint16_t Raw=0xA55AU;
    Reset();
    PmicRegisters[0x0D0AU/2U]=0U;
    PmicRegisters[0x0D8AU/2U]=0x3456U;
    Io.Context=&Lease;
    Io.Acquire=GaugeAcquire;
    Io.Release=GaugeRelease;
    Io.Read16=GaugeRead;
    Io.Write16=GaugeWrite;
    MTK_GAUGE_RESULT GaugeStatus=MtkGaugeReadCurrent(&Gauge,&Io,&Raw);
    if(GaugeStatus!=MtkGaugeOk)
        TEST_TRACE("GaugeStatus=%u raw=%04x owned=%u quarantine=%u lease=%llu ctrl=%04x writes=%u\n",
            (unsigned)GaugeStatus,Raw,Gauge.Owned,Gauge.Quarantined,
            (unsigned long long)Lease,PmicRegisters[0x0D0AU/2U],WriteCommandCount);
    CHECK(GaugeStatus==MtkGaugeOk);
    CHECK(Raw==0x3456U && !Gauge.Owned && !Gauge.Quarantined && !Lease);
    CHECK(!Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core));
    CHECK(PmicRegisters[0x0D0AU/2U]==0U && WriteCommandCount==3U);
}

static void BatteryCurrentPrmApi(void) {
    uint16_t Raw=0xA55AU;
    Reset();OtherOwned=1;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentRaw(&Raw)==MaliBusy && Raw==0xA55AU &&
          !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core) &&
          PmicBatteryGaugeCore.Owned==0 && PmicClientState==0);
    OtherOwned=0;PmicRegisters[0x0D0AU/2U]=0U;PmicRegisters[0x0D8AU/2U]=0x4567U;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentRaw(&Raw)==MaliOk && Raw==0x4567U &&
          PmicRegisters[0x0D0AU/2U]==0U && WriteCommandCount==3U &&
          !PmicBatteryCurrentContext.LeaseToken && PmicBatteryGaugeCore.Owned==0 &&
          !PmicBatteryGaugeCore.Quarantined && !Core.GaugeLeaseToken &&
          !MtkPmicBrokerBlocksUfs(&Core) && PmicClientState==0);
    Reset();PmicRegisters[0x0D0AU/2U]=1U;Raw=0xA55AU;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentRaw(&Raw)==MaliBusy && Raw==0xA55AU &&
          PmicRegisters[0x0D0AU/2U]==1U && WriteCommandCount==0U &&
          !PmicBatteryCurrentContext.LeaseToken && PmicBatteryGaugeCore.Owned==0 &&
          !PmicBatteryGaugeCore.Quarantined && !Core.GaugeLeaseToken);
    CHECK(MtkMaliPmicPrmReadBatteryCurrentRaw(NULL)==MaliInvalid);
    Reset();CurrentIrql=DISPATCH_LEVEL;Raw=0xA55AU;ULONG Gets=GetCalls;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentRaw(&Raw)==MaliInvalid &&
          Raw==0xA55AU && GetCalls==Gets);
}

static int32_t ExpectedStockCurrent(uint16_t Raw) {
    int64_t Value=(int64_t)(Raw<0x8000U?Raw:(Raw^0xffffU))*381470/100000;
    Value=Value*100/50;
    if(Raw>=0x8000U)Value=-Value;
    Value=Value*1010/1000;
    return (int32_t)Value;
}
static void BatteryCurrentStockUnitsApi(void) {
    int32_t Stock=0x12345678;
    Reset();OtherOwned=1;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentStockUnits(&Stock)==MaliBusy &&
          Stock==0x12345678 && !Core.GaugeLeaseToken);
    OtherOwned=0;PmicRegisters[0x0D0AU/2U]=0U;PmicRegisters[0x0D8AU/2U]=0x4567U;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentStockUnits(&Stock)==MaliOk &&
          Stock==ExpectedStockCurrent(0x4567U) && Stock>0);
    CHECK(PmicRegisters[0x0D0AU/2U]==0U && WriteCommandCount==3U &&
          !PmicBatteryCurrentContext.LeaseToken && PmicBatteryGaugeCore.Owned==0 &&
          !PmicBatteryGaugeCore.Quarantined && !Core.GaugeLeaseToken &&
          !MtkPmicBrokerBlocksUfs(&Core));
    Reset();PmicRegisters[0x0D0AU/2U]=0U;PmicRegisters[0x0D8AU/2U]=0xC123U;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentStockUnits(&Stock)==MaliOk &&
          Stock==ExpectedStockCurrent(0xC123U) && Stock<0);
    Reset();PmicRegisters[0x0D0AU/2U]=1U;Stock=0x12345678;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentStockUnits(&Stock)==MaliBusy &&
          Stock==0x12345678 && WriteCommandCount==0U &&
          !PmicBatteryCurrentContext.LeaseToken && !PmicBatteryGaugeCore.Owned);
    CHECK(MtkMaliPmicPrmReadBatteryCurrentStockUnits(NULL)==MaliInvalid);
    Reset();CurrentIrql=DISPATCH_LEVEL;Stock=0x12345678;ULONG Gets=GetCalls;
    CHECK(MtkMaliPmicPrmReadBatteryCurrentStockUnits(&Stock)==MaliInvalid &&
          Stock==0x12345678 && GetCalls==Gets);
}

static void BatteryVoltageTransaction(void) {
    MTK_GAUGE_VOLTAGE_TRIM ExpectedTrim={0};
    uint16_t Voltage=0xA55AU,Expected=0;
    Reset();
    PmicRegisters[PMIC_AUXADC_DIE_TEMP_TRIM/2U]=0x0104U;
    PmicRegisters[PMIC_AUXADC_TEMP_REF_TRIM/2U]=0x0640U;
    PmicRegisters[PMIC_AUXADC_TEMP_SLOPE_TRIM/2U]=0x0002U;
    PmicRegisters[0x012aU/2U]=0x0014U;
    PmicRegisters[0x010cU/2U]=0x00b0U;
    PmicRegisters[PMIC_OTP_READ_TRIGGER/2U]=1U;
    ExpectedTrim.DieTemperature=0x0104U;
    ExpectedTrim.TemperatureReference=0x0640U;
    ExpectedTrim.TemperatureSlope=0x0002U;
    for(ULONG Index=0;Index<RTL_NUMBER_OF(ExpectedTrim.Fuse);Index++)
        ExpectedTrim.Fuse[Index]=OtpFuseWords[Index];
    CHECK(MtkGaugeCalibrateVoltage(0x1234U,0x09CDU,&ExpectedTrim,&Expected)==MtkGaugeOk);
    CHECK(MtkMaliPmicPrmReadBatteryVoltageMv(&Voltage)==MaliOk && Voltage==Expected);
    CHECK(ReadAddressCount>=3U &&
          ReadAddresses[0]==PMIC_AUXADC_DIE_TEMP_TRIM &&
          ReadAddresses[1]==PMIC_AUXADC_TEMP_REF_TRIM &&
          ReadAddresses[2]==PMIC_AUXADC_TEMP_SLOPE_TRIM);
    CHECK(OtpTriggerCount==4U && OtpRowCount==4U &&
          OtpRows[0]==0x50U && OtpRows[1]==0x52U &&
          OtpRows[2]==0x54U && OtpRows[3]==0x56U);
    CHECK(PmicRegisters[PMIC_OTP_READ_ENABLE/2U]==0U &&
          PmicRegisters[0x012aU/2U]==0x0014U && PmicRegisters[0x010cU/2U]==0x00b0U &&
          !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core) &&
          OperationCalls[MtkPmicBrokerAcquireGaugeLease]==1U &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==1U &&
          PmicBatteryVoltageTrimValid && !PmicBatteryTrimRestoreFault);
    OtpStuckBusy=1U;
    CHECK(MtkMaliPmicPrmReadBatteryVoltageMv(&Voltage)==MaliOk && Voltage==Expected &&
          OtpTriggerCount==4U && OtpRowCount==4U);
    Reset();Voltage=0xA55AU;OtpStuckBusy=1U;
    PmicRegisters[0x012aU/2U]=0x0014U;PmicRegisters[0x010cU/2U]=0x00b0U;
    PmicRegisters[PMIC_OTP_READ_TRIGGER/2U]=0U;
    CHECK(MtkMaliPmicPrmReadBatteryVoltageMv(&Voltage)==MaliTimeout && Voltage==0xA55AU &&
          PmicRegisters[PMIC_OTP_READ_ENABLE/2U]==0U &&
          PmicRegisters[0x012aU/2U]==0x0014U && PmicRegisters[0x010cU/2U]==0x00b0U &&
          !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core) &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==1U &&
          !PmicBatteryTrimRestoreFault && !PmicBatteryVoltageTrimValid);
    Reset();Voltage=0xA55AU;OtpStuckBusy=1U;
    PmicRegisters[0x012aU/2U]=0x0004U;
    FailWriteCommandAddress=PMIC_OTP_READ_ENABLE;FailWriteCommandValue=0U;
    CHECK(MtkMaliPmicPrmReadBatteryVoltageMv(&Voltage)==MaliFault && Voltage==0xA55AU &&
          PmicBatteryTrimRestoreFault && PmicBatteryGaugeCore.Quarantined &&
          PmicBatteryGaugeCore.Owned && Core.GaugeLeaseToken!=0U &&
          MtkPmicBrokerBlocksUfs(&Core) &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==0U);
    ULONG Invokes=InvokeCalls;
    CHECK(MtkMaliPmicPrmReadBatteryVoltageMv(&Voltage)==MaliFault &&
          InvokeCalls==Invokes && Voltage==0xA55AU);
    Reset();CurrentIrql=DISPATCH_LEVEL;Voltage=0xA55AU;ULONG Gets=GetCalls;
    CHECK(MtkMaliPmicPrmReadBatteryVoltageMv(&Voltage)==MaliInvalid &&
          Voltage==0xA55AU && GetCalls==Gets);
}

static void BatterySnapshotTransaction(void) {
    MTK_PMIC_BATTERY_SNAPSHOT Snapshot,Expected={0};
    MTK_GAUGE_VOLTAGE_TRIM Trim={0};
    Reset();RtlZeroMemory(&Snapshot,sizeof(Snapshot));
    Snapshot.BatteryVoltageMv=0xA55AU;Snapshot.BatteryCurrentStockUnits=0x12345678;
    PmicRegisters[0x0D0AU/2U]=0U;PmicRegisters[0x0D8AU/2U]=0x3456U;
    PmicRegisters[0x0FA8U/2U]=0x0002U;
    PmicRegisters[PMIC_AUXADC_DIE_TEMP_TRIM/2U]=0x0104U;
    PmicRegisters[PMIC_AUXADC_TEMP_REF_TRIM/2U]=0x0640U;
    PmicRegisters[PMIC_AUXADC_TEMP_SLOPE_TRIM/2U]=0x0002U;
    PmicRegisters[0x012aU/2U]=0x0014U;PmicRegisters[0x010cU/2U]=0x00b0U;
    PmicRegisters[PMIC_OTP_READ_TRIGGER/2U]=1U;
    Trim.DieTemperature=0x0104U;Trim.TemperatureReference=0x0640U;
    Trim.TemperatureSlope=0x0002U;
    for(ULONG Index=0;Index<RTL_NUMBER_OF(Trim.Fuse);Index++)Trim.Fuse[Index]=OtpFuseWords[Index];
    CHECK(MtkGaugeCalibrateVoltage(0x1234U,0x09CDU,&Trim,&Expected.BatteryVoltageMv)==MtkGaugeOk);
    CHECK(MtkGaugeBatteryTemperature(0x05ABU,0x0B00U,0x3456U,
          &Expected.BatteryTemperatureDeciC)==MtkGaugeOk);
    CHECK(MtkGaugeConvertCurrent(0x3456U,T70_GAUGE_CURRENT_SENSE_INTERNAL,
          T70_GAUGE_CURRENT_TUNE_INTERNAL,&Expected.BatteryCurrentStockUnits)==MtkGaugeOk);
    CHECK(MtkMaliPmicPrmReadBatterySnapshot(&Snapshot)==MaliOk &&
          Snapshot.BatteryVoltageMv==Expected.BatteryVoltageMv &&
          Snapshot.BatteryTemperatureDeciC==Expected.BatteryTemperatureDeciC &&
          Snapshot.BatteryCurrentStockUnits==Expected.BatteryCurrentStockUnits &&
          Snapshot.BatteryAdcRaw==0x1234U && Snapshot.ChipTemperatureAdcRaw==0x09CDU &&
          Snapshot.ThermistorAdcRaw==0x05ABU && Snapshot.BifAdcRaw==0x0B00U &&
          Snapshot.CurrentRaw==0x3456U);
    CHECK(OperationCalls[MtkPmicBrokerAcquireGaugeLease]==1U &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==1U &&
          AdcRequestWrites==4U && OtpTriggerCount==4U &&
          PmicRegisters[0x0FA8U/2U]==0x0002U &&
          PmicRegisters[0x012aU/2U]==0x0014U && PmicRegisters[0x010cU/2U]==0x00b0U &&
          !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core) &&
          !PmicBatteryCurrentContext.LeaseToken && !PmicBatteryGaugeCore.Owned);
    OtpStuckBusy=1U;
    CHECK(MtkMaliPmicPrmReadBatterySnapshot(&Snapshot)==MaliOk &&
          OtpTriggerCount==4U && OtpRowCount==4U &&
          OperationCalls[MtkPmicBrokerAcquireGaugeLease]==2U &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==2U);
    Reset();AdcAutoReady=0;Snapshot.BatteryVoltageMv=0xA55AU;
    Snapshot.BatteryCurrentStockUnits=0x12345678;
    CHECK(MtkMaliPmicPrmReadBatterySnapshot(&Snapshot)==MaliTimeout &&
          Snapshot.BatteryVoltageMv==0xA55AU &&
          Snapshot.BatteryCurrentStockUnits==0x12345678 &&
          !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core) &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==1U);
    Reset();
    memset(&Snapshot,0xA5,sizeof(Snapshot));
    MTK_PMIC_BATTERY_SNAPSHOT BeforeFailure=Snapshot;
    PmicRegisters[0x0D0AU/2U]=0U;PmicRegisters[0x0D8AU/2U]=0x3456U;
    PmicRegisters[0x0FA8U/2U]=0x0002U;
    FailWriteCommandAddress=PMIC_AUXADC_BIF_CONTROL_ADDRESS;
    CHECK(MtkMaliPmicPrmReadBatterySnapshot(&Snapshot)==MaliFault &&
          memcmp(&Snapshot,&BeforeFailure,sizeof(Snapshot))==0 &&
          PmicBatteryBifRestoreFault && PmicBatteryGaugeCore.Quarantined &&
          Core.GaugeLeaseToken!=0U && MtkPmicBrokerBlocksUfs(&Core) &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==0U);
    Reset();CurrentIrql=DISPATCH_LEVEL;Snapshot.BatteryVoltageMv=0xA55AU;
    ULONG Gets=GetCalls;
    CHECK(MtkMaliPmicPrmReadBatterySnapshot(&Snapshot)==MaliInvalid &&
          Snapshot.BatteryVoltageMv==0xA55AU && GetCalls==Gets);
    CHECK(MtkMaliPmicPrmReadBatterySnapshot(NULL)==MaliInvalid);
}

static void BatteryTemperatureTransaction(void) {
    int32_t Temperature=0x12345678,Expected=0;
    Reset();PmicRegisters[0x0D8AU/2U]=0x3456U;PmicRegisters[0x0FA8U/2U]=2U;
    CHECK(MtkGaugeBatteryTemperature(0x05ABU,0x0B00U,0x3456U,&Expected)==MtkGaugeOk);
    CHECK(MtkMaliPmicPrmReadBatteryTemperatureDeciC(&Temperature)==MaliOk &&
          Temperature==Expected);
    CHECK(PmicRegisters[0x0FA8U/2U]==2U && !Core.GaugeLeaseToken &&
          !MtkPmicBrokerBlocksUfs(&Core) && !PmicBatteryCurrentContext.LeaseToken &&
          PmicBatteryGaugeCore.Owned==0 && !PmicBatteryGaugeCore.Quarantined &&
          OperationCalls[MtkPmicBrokerAcquireGaugeLease]==1U &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==1U);
    Reset();AdcAutoReady=0;PmicRegisters[0x0D8AU/2U]=0x3456U;Temperature=0x12345678;
    CHECK(MtkMaliPmicPrmReadBatteryTemperatureDeciC(&Temperature)==MaliTimeout &&
          Temperature==0x12345678 && !Core.GaugeLeaseToken &&
          !MtkPmicBrokerBlocksUfs(&Core) && !PmicBatteryCurrentContext.LeaseToken);
    Reset();PmicBatteryCurrentBusy=1;PmicBatteryGaugeCore.Quarantined=1;Temperature=0x12345678;
    CHECK(MtkMaliPmicPrmReadBatteryTemperatureDeciC(&Temperature)==MaliBusy &&
          Temperature==0x12345678 && PmicBatteryCurrentBusy==1);
}

static void BatteryAdcSnapshots(void) {
    static const struct {MTK_PMIC_BATTERY_ADC_CHANNEL Channel;uint16_t Request,Expected;} Cases[]={
        {MtkPmicBatteryAdcVoltage,1U,0x1234U},
        {MtkPmicBatteryAdcTemperature,8U,0x05ABU},
        {MtkPmicBatteryAdcChipTemperature,16U,0x09CDU},
        {MtkPmicBatteryAdcBifVoltage,0x0800U,0x0B00U}
    };
    for(ULONG Index=0;Index<RTL_NUMBER_OF(Cases);++Index) {
        Reset();uint16_t Sample=0xA55AU;
        if(Cases[Index].Channel==MtkPmicBatteryAdcBifVoltage)
            PmicRegisters[0x0FA8U/2U]=0x0002U;
        CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(Cases[Index].Channel,&Sample)==MaliOk &&
              Sample==Cases[Index].Expected);
        CHECK(AdcRequestWrites==1U && LastAdcRequest==Cases[Index].Request &&
              !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core) &&
              Core.State==MtkPmicBrokerStateIdle && !LockHeld && PmicClientState==0);
        if(Cases[Index].Channel==MtkPmicBatteryAdcBifVoltage) {
            CHECK(WriteCommandCount==3U && WriteCommandAddresses[0]==0x0FA8U &&
                  WriteCommandValues[0]==0U && WriteCommandAddresses[1]==0x1108U &&
                  WriteCommandValues[1]==0x0800U && WriteCommandAddresses[2]==0x0FA8U &&
                  WriteCommandValues[2]==0x0002U && PmicRegisters[0x0FA8U/2U]==0x0002U);
        } else {
            CHECK(WriteCommandCount>=1U && WriteCommandAddresses[0]==0x1108U &&
                  WriteCommandValues[0]==Cases[Index].Request);
        }
    }
    Reset();uint16_t Sample=0xA55AU;ULONG Gets=GetCalls,Invokes=InvokeCalls;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw((MTK_PMIC_BATTERY_ADC_CHANNEL)99,&Sample)==MaliInvalid &&
          Sample==0xA55AU && GetCalls==Gets && InvokeCalls==Invokes);
    Reset();AdcAutoReady=0;Sample=0xA55AU;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcVoltage,&Sample)==MaliTimeout &&
          Sample==0xA55AU && !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core) &&
          Core.State==MtkPmicBrokerStateIdle && !LockHeld && PmicClientState==0);
    CHECK(AdcRequestWrites==1U && LastAdcRequest==1U &&
          OperationCalls[MtkPmicBrokerReleaseGaugeLease]==1U);
    Reset();PmicRegisters[0x0FA8U/2U]=0x0002U;AdcAutoReady=0;Sample=0xA55AU;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcBifVoltage,&Sample)==MaliTimeout &&
          Sample==0xA55AU && PmicRegisters[0x0FA8U/2U]==0x0002U &&
          !Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core));
    Reset();PmicRegisters[0x0FA8U/2U]=0x0002U;Sample=0xA55AU;
    FailWriteCommandAddress=0x0FA8U;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcBifVoltage,&Sample)==MaliFault &&
          Sample==0xA55AU && PmicBatteryBifRestoreFault!=0 &&
          Core.GaugeLeaseToken!=0U && MtkPmicBrokerBlocksUfs(&Core));
    ULONG GetsAfterBifFault=GetCalls, WritesAfterBifFault=WriteCommandCount;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcBifVoltage,&Sample)==MaliFault &&
          GetCalls==GetsAfterBifFault && WriteCommandCount==WritesAfterBifFault);
    Reset();PmicBatteryCurrentBusy=1;Sample=0xA55AU;Gets=GetCalls;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcVoltage,&Sample)==MaliBusy &&
          Sample==0xA55AU && GetCalls==Gets && PmicBatteryCurrentBusy==1);
    PmicBatteryCurrentBusy=0;
    Reset();InvokeStatusOperation=MtkPmicBrokerReleaseGaugeLease;InvokeStatusOnce=1;InvokeStatus=STATUS_TIMEOUT;Sample=0xA55AU;
    MALI_RESULT ReleaseFailure=MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcVoltage,&Sample);
    CHECK(ReleaseFailure==MaliFault && Sample==0xA55AU && PmicBatteryGaugeCore.Quarantined &&
          PmicBatteryGaugeCore.Owned && PmicBatteryTrimRestoreFault && !PmicBatteryCurrentBusy);
    Gets=GetCalls;WriteCommandCount=0;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcVoltage,&Sample)==MaliFault &&
          GetCalls==Gets && WriteCommandCount==0);
    Reset();CurrentIrql=DISPATCH_LEVEL;Sample=0xA55AU;Gets=GetCalls;
    CHECK(MtkMaliPmicPrmReadBatteryAdcRaw(MtkPmicBatteryAdcVoltage,&Sample)==MaliInvalid &&
          Sample==0xA55AU && GetCalls==Gets);
}

int TestMain(void) {
    BatteryCurrentPrmApi();if(Failure)return (int)Failure;
    BatteryCurrentStockUnitsApi();if(Failure)return (int)Failure;
    BatteryTemperatureTransaction();if(Failure)return (int)Failure;
    BatteryVoltageTransaction();if(Failure)return (int)Failure;
    BatterySnapshotTransaction();if(Failure)return (int)Failure;
    BatteryAdcSnapshots();if(Failure)return (int)Failure;
    GaugeCurrentSnapshot();if(Failure)return (int)Failure;
    SuccessfulTransfers();if(Failure)return (int)Failure;
    PowerButtonReadOnly();if(Failure)return (int)Failure;
    KeySnapshotTransactions();if(Failure)return (int)Failure;
    SharedBusyAndAddressPolicy();if(Failure)return (int)Failure;
    InterfaceFailures();if(Failure)return (int)Failure;
    MalformedAndTimeout();if(Failure)return (int)Failure;
    KeyIrqLeaseTransactions();if(Failure)return (int)Failure;
    KeyIrqSiblingAndLocalTokens();if(Failure)return (int)Failure;
    KeyIrqFailuresAndPublication();return (int)Failure;
}

#if defined(MTK_PMIC_PRM_HOST_TEST)
int main(void) {
    int Status=TestMain();
    if(Status!=0) {
        printf("MtkMaliPmicPrmTest failed at line %d",Status);
        return 1;
    }
    printf("MtkMaliPmicPrmTest passed, checks=%u",(unsigned)MtkMaliPmicPrmAssertionCount);
    return 0;
}
#endif
