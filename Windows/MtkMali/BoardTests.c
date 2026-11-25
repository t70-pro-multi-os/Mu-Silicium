/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Reuse the existing real-controller PMIC/SPM/clock fixtures without copying
   them. Only provider OS leases, locks and identity/cache/IRQ proofs are mocks. */
#include "Board.h"
#define MtkMaliMfgAssertionCount MtkMaliBoardAssertionCount
#define TestMain UnusedMfgTestEntry
#include "MfgTests.c"
#undef TestMain
static KIRQL BoardIrql;
#undef KeGetCurrentIrql
#define KeGetCurrentIrql() BoardIrql
static NTSTATUS PnpCall(PDEVICE_OBJECT,PIRP);
static void PnpComplete(PIRP,CCHAR);
static void PnpEventInit(PRKEVENT,EVENT_TYPE,BOOLEAN);
static LONG PnpEventSet(PRKEVENT,KPRIORITY,BOOLEAN);
static NTSTATUS PnpWait(void *,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
static NTSTATUS PnpDelay(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
#undef IoCallDriver
#define IoCallDriver PnpCall
#undef IoCompleteRequest
#define IoCompleteRequest PnpComplete
#define KeInitializeEvent PnpEventInit
#define KeSetEvent PnpEventSet
#define KeWaitForSingleObject PnpWait
#define KeDelayExecutionThread PnpDelay
#include "Board.c"
#ifndef MALI_BOARD_TEST_PROVIDER
static MALI_BOARD_PROVIDER Provider;
#else
#define Provider (*MALI_BOARD_TEST_PROVIDER)
#endif
static MALI_BOARD_CONFIG Config;
static MALI_BOARD_INTERFACE BoardBase;
static MALI_BOARD_POWER_INTERFACE BoardPower;
static unsigned ResourceLive,AccessLive,Quarantined,Acquires,Releases,Enters,Leaves,Validations;
static unsigned NoCookie,FailedAcquireNoCookie,BadResource,ReenterAcquire,FailValidationAt;
static NTSTATUS AcquireResult,ReleaseResult,EnterResult,ValidateResult;
static void CacheClean(void *X,const void *P,size_t N) {(void)P;(void)N;CHECK(X==&ResourceLive);}
static void CacheBarrier(void *X) {CHECK(X==&ResourceLive);}
static NTSTATUS ResourceAcquire(void *X,MALI_BOARD_LEASE *L) {
    CHECK(X==&Provider && Provider.Gate && L && !ResourceLive && !AccessLive && !ControllerLease);++Acquires;
    if(ReenterAcquire) {
        MALI_BOARD_LEASE Other={0};
        CHECK(BoardBase.Acquire(BoardBase.Header.Context,&Other)==STATUS_DEVICE_BUSY);
        CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DEVICE_BUSY);
    }
    RtlZeroMemory(L,sizeof(*L));
    if(NoCookie || (AcquireResult!=STATUS_SUCCESS && FailedAcquireNoCookie))return AcquireResult;
    ResourceLive=1;L->Cookie=&ResourceLive;L->AddressModel=MaliVmDirectSystemPhysical;
    L->ApertureCommitLimit=64u*1024u*1024u;L->TimeoutTicks=100;
    L->Cache.Context=&ResourceLive;L->Cache.CleanForGpu=CacheClean;L->Cache.StoreBarrier=CacheBarrier;
    switch(BadResource) {
    case 1:L->AddressModel=MaliVmAddressingUnknown;break;case 2:L->TimeoutTicks=0;break;
    case 3:L->ApertureCommitLimit=0;break;case 4:L->ApertureCommitLimit=4097;break;
    case 5:L->ApertureCommitLimit=MALI_ALLOCATION_MAX_BYTES+4096;break;
    case 6:L->Cache.CleanForGpu=0;break;case 7:L->Cache.StoreBarrier=0;break;
    }
    return AcquireResult;
}
static NTSTATUS ResourceValidate(void *X,const MALI_BOARD_LEASE *L) {
    CHECK(X==&Provider && ResourceLive && AccessLive && ControllerLease && Provider.Gate && L && L->Cookie==&ResourceLive);
    ++Validations;return FailValidationAt==Validations?STATUS_DEVICE_NOT_READY:ValidateResult;
}
static NTSTATUS ResourceEnter(void *X,void *Cookie) {
    CHECK(X==&Provider && Cookie==&ResourceLive && ResourceLive && !AccessLive && !ControllerLease);++Enters;
    if(Quarantined)return STATUS_DEVICE_NOT_READY;
    if(EnterResult==STATUS_SUCCESS){AccessLive=1;ControllerLease=1;}
    return EnterResult;
}
static void ResourceLeave(void *X,void *Cookie,BOOLEAN Quarantine) {
    CHECK(X==&Provider && Cookie==&ResourceLive && ResourceLive && AccessLive && ControllerLease);
    CHECK(!!Quarantine==!!BpPoisoned(&Controller));++Leaves;Quarantined|=!!Quarantine;AccessLive=ControllerLease=0;
}
static NTSTATUS ResourceRelease(void *X,void *Cookie) {
    CHECK(X==&Provider && Cookie==&ResourceLive && ResourceLive && !AccessLive && !ControllerLease && !Provider.AccessHeld);
    CHECK(BpClean(&Controller));++Releases;if(ReleaseResult==STATUS_SUCCESS)ResourceLive=0;return ReleaseResult;
}
static void InitProvider(uint32_t Borrowed,uint32_t Ref) {
    InitController(Borrowed,Ref);ControllerLease=0;
    RtlZeroMemory(&Provider,sizeof(Provider));RtlZeroMemory(&Config,sizeof(Config));
    RtlZeroMemory(&BoardBase,sizeof(BoardBase));RtlZeroMemory(&BoardPower,sizeof(BoardPower));
    BoardIrql=PASSIVE_LEVEL;ResourceLive=AccessLive=Quarantined=Acquires=Releases=Enters=Leaves=Validations=0;
    NoCookie=FailedAcquireNoCookie=BadResource=ReenterAcquire=FailValidationAt=0;
    AcquireResult=ReleaseResult=EnterResult=ValidateResult=STATUS_SUCCESS;
    Config.Controller=&Controller;Config.Resources.Context=&Provider;
    Config.Resources.Acquire=ResourceAcquire;Config.Resources.Validate=ResourceValidate;
    Config.Resources.Enter=ResourceEnter;Config.Resources.Leave=ResourceLeave;Config.Resources.Release=ResourceRelease;
    unsigned W=ControllerWrites(),R=ControllerReads();
    CHECK(MtkMaliBoardProviderInitialize(&Provider,&Config)==STATUS_SUCCESS);
    CHECK(!Provider.Gate && !Provider.ClientCookie && !Provider.ResourceLease.Cookie && !Provider.References[0] && !Provider.References[1]);
    CHECK(ControllerWrites()==W && ControllerReads()==R);
}
static void GetBoth(void) {
    CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(BoardBase),&BoardBase)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardGetPowerInterface(&Provider,1,sizeof(BoardPower),&BoardPower)==STATUS_SUCCESS);
    CHECK(BoardBase.Header.Version==1 && BoardBase.Header.Size==sizeof(BoardBase) && BoardBase.Header.InterfaceReference && BoardBase.Header.InterfaceDereference);
    CHECK(BoardPower.Header.Version==1 && BoardPower.Header.Size==sizeof(BoardPower) && BoardPower.Header.InterfaceReference && BoardPower.Header.InterfaceDereference);
    CHECK(BoardBase.Header.Context!=BoardPower.Header.Context && Provider.References[0]==1 && Provider.References[1]==1);
}
static void DropBoth(void) {
    CHECK(!Provider.ClientCookie && !ResourceLive && !AccessLive && !Provider.AccessHeld);
    BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);
    CHECK(!Provider.References[0] && !Provider.References[1] && MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS && !Provider.Gate);
}
static uint32_t BoardStateWrites(void) {return Writes+CWrite+HWrites+BRegisterWrites;}
static void ProviderCycles(void) {
    for(uint32_t B=0;B<4;B++)for(uint32_t Ref=0;Ref<4;Ref++) {
        InitProvider(B,Ref);GetBoth();MALI_BOARD_LEASE L={0};ReenterAcquire=1;
        CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS && L.Cookie && L.Cookie!=Provider.ResourceLease.Cookie);
        CHECK(BpLeaseValid(&L) && Provider.PowerState==PowerDeviceD0 && Rails.Acquired==3 && Hardware.Held==7 && !AccessLive && !ControllerLease);
        void *Old=L.Cookie;MALI_BOARD_LEASE Other={0};Other.TimeoutTicks=123;
        CHECK(BoardBase.Acquire(BoardBase.Header.Context,&Other)==STATUS_DEVICE_BUSY && Other.TimeoutTicks==123);
        BoardBase.Header.InterfaceReference(BoardBase.Header.Context);BoardPower.Header.InterfaceReference(BoardPower.Header.Context);
        CHECK(Provider.References[0]==2 && Provider.References[1]==2);
        BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);
        for(uint32_t Cycle=0;Cycle<3;Cycle++) {
            CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
            unsigned W=BoardStateWrites();
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionHibernate)==STATUS_SUCCESS && BoardStateWrites()==W && Controller.Power.References==1);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS);
            CHECK(Provider.PowerState==PowerDeviceD3 && BpClean(&Controller) && !AccessLive && !ControllerLease);
            unsigned R=ControllerReads();W=ControllerWrites();
            CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY && ControllerReads()==R && ControllerWrites()==W);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionNone)==STATUS_SUCCESS && ControllerReads()==R && ControllerWrites()==W);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS && Provider.PowerState==PowerDeviceD0);
        }
        CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS && !ResourceLive && !Provider.ClientCookie && BpClean(&Controller));
        CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS && L.Cookie!=Old);
        unsigned W=ControllerWrites(),R=ControllerReads();
        CHECK(BoardBase.Ready(BoardBase.Header.Context,Old)==STATUS_INVALID_PARAMETER);
        CHECK(BoardBase.Release(BoardBase.Header.Context,Old)==STATUS_INVALID_PARAMETER);
        CHECK(BoardPower.SetPower(BoardPower.Header.Context,Old,PowerDeviceD3,PowerActionNone)==STATUS_INVALID_PARAMETER);
        CHECK(ControllerWrites()==W && ControllerReads()==R && Provider.PowerState==PowerDeviceD0);
        CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DEVICE_BUSY && Provider.Closing);
        CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(BoardBase),&BoardBase)==STATUS_DELETE_PENDING);
        CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
        CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionNone)==STATUS_SUCCESS);
        W=ControllerWrites();R=ControllerReads();
        CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS && ControllerWrites()==W && ControllerReads()==R);
        CHECK(Enters==Leaves && !Quarantined);DropBoth();if(Failure)return;
    }
}
static void QueryAndLifetime(void) {
    InitProvider(0,0);struct {MALI_BOARD_INTERFACE I;uint64_t Guard;} Out={0};Out.Guard=UINT64_C(0xaabbccddeeff0123);
    CHECK(MtkMaliBoardGetInterface(&Provider,0,sizeof(Out),&Out.I)==STATUS_NOT_SUPPORTED && !Provider.References[0]);
    CHECK(MtkMaliBoardGetInterface(&Provider,1,1,&Out.I)==STATUS_BUFFER_TOO_SMALL && !Provider.References[0]);
    CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(Out),0)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(Out),(MALI_BOARD_INTERFACE*)&Provider)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(Out),(MALI_BOARD_INTERFACE*)&Rails)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliBoardGetPowerInterface(&Provider,1,sizeof(BoardPower),(MALI_BOARD_POWER_INTERFACE*)&Hardware)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliBoardGetInterface(&Provider,65535,sizeof(Out),&Out.I)==STATUS_SUCCESS && Out.I.Header.Version==1 && Out.Guard==UINT64_C(0xaabbccddeeff0123));
    Out.I.Header.InterfaceDereference(Out.I.Header.Context);
    Provider.References[0]=MAXLONG;
    CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(Out),&Out.I)==STATUS_INSUFFICIENT_RESOURCES && !Provider.ReferenceFault);Provider.References[0]=0;
    GetBoth();MALI_BOARD_LEASE L={0};unsigned W=ControllerWrites(),R=ControllerReads();
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,(MALI_BOARD_LEASE*)&Controller)==STATUS_INVALID_PARAMETER);
    BoardIrql=DISPATCH_LEVEL;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(Out),&Out.I)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_INVALID_PARAMETER);BoardIrql=PASSIVE_LEVEL;
    Provider.Gate=1;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliBoardGetPowerInterface(&Provider,1,sizeof(BoardPower),&BoardPower)==STATUS_DEVICE_BUSY);Provider.Gate=0;
    CHECK(ControllerReads()==R && ControllerWrites()==W && !Acquires);
    Provider.Generation=UINTPTR_MAX;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_INSUFFICIENT_RESOURCES && !Acquires);Provider.Generation=0;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS);
    CHECK(BoardPower.SetPower(BoardBase.Header.Context,L.Cookie,PowerDeviceD3,PowerActionNone)==STATUS_INVALID_PARAMETER);
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD1,PowerActionNone)==STATUS_NOT_SUPPORTED);
    CHECK(BoardBase.Release(BoardBase.Header.Context,0)==STATUS_INVALID_PARAMETER);
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);DropBoth();
    InitProvider(0,0);GetBoth();BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);
    BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);
    CHECK(Provider.ReferenceFault && MtkMaliBoardProviderClose(&Provider)==STATUS_DRIVER_INTERNAL_ERROR);
    InitProvider(0,0);GetBoth();Provider.References[1]=MAXLONG;BoardPower.Header.InterfaceReference(BoardPower.Header.Context);
    CHECK(Provider.ReferenceFault && MtkMaliBoardProviderClose(&Provider)==STATUS_DRIVER_INTERNAL_ERROR);
}
static void AcquisitionFailures(void) {
    for(uint32_t F=0;F<13;F++) {
        InitProvider(0,1);GetBoth();MALI_BOARD_LEASE L={0};
        if(F<7)BadResource=F+1;
        if(F==7){AcquireResult=STATUS_UNSUCCESSFUL;FailedAcquireNoCookie=1;}
        if(F==8)AcquireResult=STATUS_UNSUCCESSFUL;
        if(F==9)AcquireResult=STATUS_PENDING;
        if(F==10)NoCookie=1;
        if(F==11)EnterResult=STATUS_PENDING;
        if(F==12)ValidateResult=STATUS_DEVICE_NOT_READY;
        unsigned W=ControllerWrites(),R=ControllerReads();NTSTATUS S=BoardBase.Acquire(BoardBase.Header.Context,&L);
        CHECK(S!=STATUS_SUCCESS && !NT_SUCCESS(S) && ControllerWrites()==W && ControllerReads()==R && !AccessLive && !ControllerLease && !Provider.AccessHeld);
        if(F==7 || F==10)CHECK(!L.Cookie && !ResourceLive && !Provider.ClientCookie);
        else {
            CHECK(L.Cookie && ResourceLive && Provider.ClientCookie==L.Cookie && Provider.PowerState==PowerDeviceUnspecified);
            CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY && ControllerReads()==R);
            CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS && ControllerReads()==R && ControllerWrites()==W);
        }
        CHECK(!Quarantined);DropBoth();if(Failure)return;
    }
}
static void PowerAndReleaseFailures(void) {
    InitProvider(0,2);GetBoth();MALI_BOARD_LEASE L={0};FailValidationAt=2;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_DEVICE_NOT_READY && L.Cookie && ResourceLive && Controller.State==MaliMfgControllerOn);
    unsigned W=ControllerWrites(),R=ControllerReads();
    CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY && ControllerReads()==R && ControllerWrites()==W);
    unsigned StateWrites=BoardStateWrites();
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS && BoardStateWrites()==StateWrites);
    W=ControllerWrites();ControllerIdle=0;
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionSleep)==STATUS_DEVICE_NOT_READY && ControllerWrites()==W);
    CHECK(Provider.PowerState==PowerDeviceUnspecified && Controller.Power.References==1 && !Quarantined);ControllerIdle=1;
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    ValidateResult=STATUS_PENDING;R=ControllerReads();W=ControllerWrites();
    CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_UNSUCCESSFUL && ControllerReads()==R && ControllerWrites()==W);
    ValidateResult=STATUS_SUCCESS;
    CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY);
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    ControllerOppAllowed=0;
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS && BpClean(&Controller));
    ReleaseResult=STATUS_PENDING;W=ControllerWrites();R=ControllerReads();
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_UNSUCCESSFUL && ResourceLive && Provider.ClientCookie==L.Cookie);
    ReleaseResult=STATUS_SUCCESS;
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS && ControllerWrites()==W && ControllerReads()==R);DropBoth();
    InitProvider(0,0);GetBoth();ControllerOppAllowed=0;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_DEVICE_NOT_READY && L.Cookie && !Rails.Acquired && !Hardware.Held);
    W=ControllerWrites();R=ControllerReads();
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS && ControllerWrites()==W && ControllerReads()==R);DropBoth();
    InitProvider(0,0);GetBoth();Stuck=1;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_IO_TIMEOUT && L.Cookie && ResourceLive && Quarantined && Controller.Poisoned && Rails.Acquired==3);
    W=ControllerWrites();R=ControllerReads();
    CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY);
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_NOT_READY);
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY && ResourceLive && Provider.ClientCookie==L.Cookie);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DEVICE_BUSY && !Releases && ControllerReads()==R && ControllerWrites()==W);
}
static void BorrowedVoltageRelease(void) {
    InitProvider(3,0);GetBoth();MALI_BOARD_LEASE L={0};CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS);
    const MALI_MFG_VOLTAGE_IO Io={&Rails,AuthVoltage};VAllowed=1;VMaxGap=200000;VAuthCalls=0;
    CHECK(ResourceEnter(&Provider,&ResourceLive)==STATUS_SUCCESS);
    CHECK(MaliMfgRailsVoltage(&Rails,ControllerExpectedGpu+6250,ControllerExpectedSram+6250,&Io)==MaliOk);ResourceLeave(&Provider,&ResourceLive,FALSE);
    unsigned W=ControllerWrites();
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_BUSY && ControllerWrites()==W && ResourceLive && Rails.Acquired==3 && !Quarantined && !Releases);
    CHECK(ResourceEnter(&Provider,&ResourceLive)==STATUS_SUCCESS);
    CHECK(MaliMfgRailsVoltage(&Rails,ControllerExpectedGpu,ControllerExpectedSram,&Io)==MaliOk);ResourceLeave(&Provider,&ResourceLive,FALSE);
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS && !ResourceLive);DropBoth();
}
static void InvalidProviderConfiguration(void) {
    for(uint32_t F=0;F<18;F++) {
        InitProvider(0,0);RtlZeroMemory(&Provider,sizeof(Provider));MALI_BOARD_CONFIG C=Config;
        switch(F) {
        case 0:C.Controller=0;break;case 1:C.Resources.Acquire=0;break;case 2:C.Resources.Validate=0;break;
        case 3:C.Resources.Enter=0;break;case 4:C.Resources.Leave=0;break;case 5:C.Resources.Release=0;break;
        case 6:C.Controller=(MALI_MFG_CONTROLLER*)&Provider;break;case 7:Controller.Initialized=0;break;
        case 8:Controller.Poisoned=1;break;case 9:Rails.Acquired=1;break;
        case 10:Controller.Io.Rails=(MALI_MFG_RAILS*)&Provider;break;
        case 11:Controller.Io.Clock=(MALI_MFG_HW_CLOCK*)&C;break;
        case 12:Controller.Io.Rails->Io.Pmic=(MALI_MFG_PMIC*)&C;break;
        case 13:Controller.State=MaliMfgControllerOn;break;case 14:Controller.Power.Clocks.Initialized=0;break;
        case 15:Rails.Initialized=0;break;
        case 16:C.Lifetime.Reference=BpReference;break;case 17:C.Lifetime.Dereference=BpDereference;break;
        }
        unsigned W=ControllerWrites(),R=ControllerReads();
        CHECK(MtkMaliBoardProviderInitialize(&Provider,&C)==(F==8 || F==9 || (F>=13 && F<=15)?STATUS_DEVICE_BUSY:STATUS_INVALID_PARAMETER));
        CHECK(!Provider.Signature && ControllerWrites()==W && ControllerReads()==R);
    }
}

/* Real WDM IRP/CM_RESOURCE_LIST layouts, real routing and controller. Only the
   PnP manager's lists/stack and hardware/resource services are virtual fixtures. */
typedef struct {CM_RESOURCE_LIST List;CM_PARTIAL_RESOURCE_DESCRIPTOR Extra[3];} PNP_RESOURCES;
C_ASSERT(FIELD_OFFSET(PNP_RESOURCES,Extra)==FIELD_OFFSET(CM_RESOURCE_LIST,List)+
    FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR,PartialResourceList)+
    FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST,PartialDescriptors)+sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
static PNP_RESOURCES PnpRaw,PnpTranslated;
static DEVICE_OBJECT PnpPdo,PnpOther;
static IRP PnpIrp;
static IO_STACK_LOCATION PnpStacks[2];
#define PnpStack PnpStacks[1]
static MALI_BOARD_QUERY PnpProof;
static CM_PARTIAL_RESOURCE_DESCRIPTOR *PnpRawEntry(UINT I) {return &PnpRaw.List.List[0].PartialResourceList.PartialDescriptors[I];}
static CM_PARTIAL_RESOURCE_DESCRIPTOR *PnpEntry(UINT I) {return &PnpTranslated.List.List[0].PartialResourceList.PartialDescriptors[I];}
static void InitPnp(void) {
    InitProvider(0,0);RtlZeroMemory(&PnpRaw,sizeof(PnpRaw));RtlZeroMemory(&PnpTranslated,sizeof(PnpTranslated));
    PnpRaw.List.Count=PnpTranslated.List.Count=1;
    CM_PARTIAL_RESOURCE_LIST *A=&PnpRaw.List.List[0].PartialResourceList,*B=&PnpTranslated.List.List[0].PartialResourceList;
    A->Version=B->Version=A->Revision=B->Revision=1;A->Count=B->Count=4;
    CM_PARTIAL_RESOURCE_DESCRIPTOR *R=PnpRawEntry(0);R->Type=CmResourceTypeMemory;
    R->ShareDisposition=CmResourceShareDeviceExclusive;R->u.Memory.Start.QuadPart=0x13000000u;R->u.Memory.Length=0x4000u;*PnpEntry(0)=*R;
    for(UINT I=0;I<3;I++) {
        R=PnpRawEntry(I+1);R->Type=CmResourceTypeInterrupt;R->ShareDisposition=CmResourceShareDeviceExclusive;
        R->u.Interrupt.Vector=428u+I;R->u.Interrupt.Level=428u+I;R->u.Interrupt.Affinity=1;
        *PnpEntry(I+1)=*R;PnpEntry(I+1)->u.Interrupt.Level=5u+I;
        PnpEntry(I+1)->u.Interrupt.Vector=0x70u+I;PnpEntry(I+1)->u.Interrupt.Affinity=(KAFFINITY)1u<<I;
    }
}
static void InitPnpQuery(UINT Power) {
    RtlZeroMemory(&PnpIrp,sizeof(PnpIrp));RtlZeroMemory(&PnpStack,sizeof(PnpStack));RtlZeroMemory(&PnpProof,sizeof(PnpProof));
    PnpIrp.Tail.Overlay.CurrentStackLocation=&PnpStack;PnpIrp.RequestorMode=KernelMode;
    PnpIrp.IoStatus.Status=STATUS_NOT_SUPPORTED;PnpIrp.IoStatus.Information=123;
    PnpStack.MajorFunction=IRP_MJ_PNP;PnpStack.MinorFunction=IRP_MN_QUERY_INTERFACE;
    PnpStack.Parameters.QueryInterface.InterfaceType=(LPGUID)(Power?&MtkMaliBoardPowerInterfaceGuid:&MtkMaliBoardInterfaceGuid);
    PnpStack.Parameters.QueryInterface.Version=1;
    PnpStack.Parameters.QueryInterface.Size=(USHORT)(Power?sizeof(BoardPower):sizeof(BoardBase));
    PnpStack.Parameters.QueryInterface.Interface=(PINTERFACE)(Power?(void*)&BoardPower:(void*)&BoardBase);
    PnpProof.Size=sizeof(PnpProof);PnpProof.Version=1;PnpProof.Pdo=&PnpPdo;
    for(UINT I=0;I<3;I++)PnpProof.Interrupts[I]=*PnpEntry(I+1);
    PnpStack.Parameters.QueryInterface.InterfaceSpecificData=&PnpProof;
}
static void PnpBindingFailures(void) {
#if defined(NT_PROCESSOR_GROUPS)
    const UINT Cases=45;
#else
    const UINT Cases=44;
#endif
    for(UINT F=0;F<Cases;F++) {
        InitPnp();const CM_RESOURCE_LIST *Raw=&PnpRaw.List,*Translated=&PnpTranslated.List;PDEVICE_OBJECT Pdo=&PnpPdo;
        switch(F) {
        case 0:Raw=0;break;case 1:Translated=0;break;case 2:Pdo=0;break;
        case 3:PnpRaw.List.Count=0;break;case 4:PnpRaw.List.Count=2;break;case 5:PnpTranslated.List.Count=0;break;
        case 6:PnpRaw.List.List[0].PartialResourceList.Version=0;break;
        case 7:PnpTranslated.List.List[0].PartialResourceList.Revision=2;break;
        case 8:PnpRaw.List.List[0].PartialResourceList.Count=3;break;
        case 9:PnpTranslated.List.List[0].PartialResourceList.Count=5;break;
        case 10:PnpRawEntry(0)->u.Memory.Start.QuadPart++;break;
        case 11:PnpEntry(0)->u.Memory.Start.QuadPart++;break;
        case 12:PnpRawEntry(0)->u.Memory.Length++;break;case 13:PnpEntry(0)->u.Memory.Length++;break;
        case 14:PnpRawEntry(0)->Flags=CM_RESOURCE_MEMORY_READ_ONLY;break;
        case 15:PnpEntry(0)->Flags=CM_RESOURCE_MEMORY_READ_ONLY;break;
        case 16:PnpRawEntry(0)->ShareDisposition=CmResourceShareShared;break;
        case 17:PnpEntry(1)->ShareDisposition=CmResourceShareShared;break;
        case 18:PnpEntry(1)->Type=CmResourceTypeMemory;break;
        case 19:PnpRawEntry(1)->u.Interrupt.Vector=396;break;
        case 20:PnpRawEntry(1)->u.Interrupt.Vector=429;break;
        case 21:PnpRawEntry(3)->u.Interrupt.Vector=431;break;
        case 22:PnpRawEntry(1)->Flags=CM_RESOURCE_INTERRUPT_LATCHED;break;
        case 23:PnpRawEntry(1)->Flags=CM_RESOURCE_INTERRUPT_MESSAGE;break;
        case 24:PnpRawEntry(1)->Flags=0x8000;break;
        case 25:PnpEntry(1)->Flags=CM_RESOURCE_INTERRUPT_MESSAGE;break;
        case 26:PnpEntry(1)->Flags=CM_RESOURCE_INTERRUPT_LATCHED;break;
        case 27:PnpEntry(1)->Flags=0x8000;break;
        case 28:PnpEntry(1)->u.Interrupt.Vector=0;break;case 29:PnpEntry(1)->u.Interrupt.Affinity=0;break;
        case 30:PnpEntry(1)->u.Interrupt.Level=DISPATCH_LEVEL;break;case 31:PnpEntry(1)->u.Interrupt.Level=PROFILE_LEVEL;break;
        case 32:PnpEntry(2)->u.Interrupt.Vector=PnpEntry(1)->u.Interrupt.Vector;break;
        case 33:PnpRawEntry(0)->Type=CmResourceTypePort;break;case 34:PnpEntry(0)->Type=CmResourceTypePort;break;
        case 35:{CM_PARTIAL_RESOURCE_DESCRIPTOR R=*PnpRawEntry(1);*PnpRawEntry(1)=*PnpRawEntry(3);*PnpRawEntry(3)=R;break;}
        case 36:{CM_PARTIAL_RESOURCE_DESCRIPTOR R=*PnpEntry(0);*PnpEntry(0)=*PnpEntry(1);*PnpEntry(1)=R;break;}
        case 37:PnpRawEntry(3)->Type=PnpEntry(3)->Type=CmResourceTypeNull;break;
        case 38:PnpRaw.List.List[0].PartialResourceList.Revision=0;break;
        case 39:PnpTranslated.List.List[0].PartialResourceList.Version=0;break;
        case 40:PnpRawEntry(3)->u.Interrupt.Vector=432;break;
        case 41:PnpRawEntry(2)->ShareDisposition=CmResourceShareUndetermined;break;
        case 42:PnpEntry(0)->ShareDisposition=CmResourceShareShared;break;
#if defined(NT_PROCESSOR_GROUPS)
        case 43:PnpEntry(1)->u.Interrupt.Group=1;break;
        case 44:PnpRawEntry(1)->u.Interrupt.Group=1;break;
#else
        case 43:PnpEntry(1)->u.Interrupt.Level=0x10006;break;
#endif
        }
        unsigned R=ControllerReads(),W=ControllerWrites();
        CHECK(MtkMaliBoardBindResources(&Provider,Pdo,Raw,Translated)==(F<10 || F==38 || F==39?STATUS_INVALID_PARAMETER:STATUS_DEVICE_CONFIGURATION_ERROR));
        CHECK(!Provider.Pdo && !Provider.Gate && !Provider.References[0] && !Provider.References[1]);
        CHECK(ControllerReads()==R && ControllerWrites()==W);
        CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    }
    /* Memory may occupy any matching list position; IRQ order is semantic. */
    for(UINT Position=0;Position<4;Position++) {
        InitPnp();
        for(UINT I=0;I<Position;I++) {
            CM_PARTIAL_RESOURCE_DESCRIPTOR A=*PnpRawEntry(I),B=*PnpEntry(I);
            *PnpRawEntry(I)=*PnpRawEntry(I+1);*PnpEntry(I)=*PnpEntry(I+1);
            *PnpRawEntry(I+1)=A;*PnpEntry(I+1)=B;
        }
        CHECK(MtkMaliBoardBindResources(&Provider,&PnpPdo,&PnpRaw.List,&PnpTranslated.List)==STATUS_SUCCESS);
        for(UINT I=0;I<3;I++)CHECK(Provider.Interrupts[I].u.Interrupt.Vector==0x70u+I);
        CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_SUCCESS);
        CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    }
}
static void PnpQueryFailures(void) {
    InitPnp();InitPnpQuery(0);
    CHECK(MtkMaliBoardQueryInterfaceIrp(&Provider,&PnpPdo,&PnpIrp) && PnpIrp.IoStatus.Status==STATUS_NO_SUCH_DEVICE);
    CHECK(MtkMaliBoardBindResources(&Provider,&PnpPdo,&PnpRaw.List,&PnpTranslated.List)==STATUS_SUCCESS);
#if defined(NT_PROCESSOR_GROUPS)
    const UINT Cases=24;
#else
    const UINT Cases=23;
#endif
    for(UINT F=0;F<Cases;F++) {
        InitPnpQuery(0);MALI_BOARD_INTERFACE Before=BoardBase;PDEVICE_OBJECT Pdo=&PnpPdo;
        NTSTATUS Expected=STATUS_INVALID_PARAMETER;
        switch(F) {
        case 0:Pdo=&PnpOther;Expected=STATUS_NO_SUCH_DEVICE;break;
        case 1:PnpProof.Pdo=&PnpOther;break;case 2:PnpProof.Size--;break;case 3:PnpProof.Version=2;break;
        case 4:PnpStack.Parameters.QueryInterface.InterfaceSpecificData=0;break;
        case 5:PnpIrp.RequestorMode=UserMode;break;
        case 6:PnpProof.Interrupts[0].Type=CmResourceTypeMemory;break;
        case 7:PnpProof.Interrupts[0].ShareDisposition=CmResourceShareShared;break;
        case 8:PnpProof.Interrupts[0].Flags=CM_RESOURCE_INTERRUPT_LATCHED;break;
        case 9:PnpProof.Interrupts[0].u.Interrupt.Level++;break;
        case 10:PnpProof.Interrupts[0].u.Interrupt.Vector=428;break;
        case 11:PnpProof.Interrupts[0].u.Interrupt.Affinity++;break;
        case 12:PnpProof.Interrupts[1].u.Interrupt.Vector++;break;
        case 13:PnpProof.Interrupts[2].u.Interrupt.Vector++;break;
        case 14:PnpStack.Parameters.QueryInterface.Version=0;Expected=STATUS_NOT_SUPPORTED;break;
        case 15:PnpStack.Parameters.QueryInterface.Size=sizeof(INTERFACE);Expected=STATUS_BUFFER_TOO_SMALL;break;
        case 16:PnpStack.Parameters.QueryInterface.Interface=0;break;
        case 17:PnpStack.Parameters.QueryInterface.Interface=(PINTERFACE)&Provider;break;
        case 18:PnpStack.Parameters.QueryInterface.InterfaceSpecificData=(void*)(UINTPTR_MAX-7u);break;
        case 19:PnpStack.Parameters.QueryInterface.InterfaceSpecificData=(UCHAR*)&PnpProof+1;break;
        case 20:Provider.Gate=1;Expected=STATUS_DEVICE_BUSY;break;
        case 21:BoardIrql=DISPATCH_LEVEL;break;
        case 22:Pdo=0;Expected=STATUS_NO_SUCH_DEVICE;break;
#if defined(NT_PROCESSOR_GROUPS)
        case 23:PnpProof.Interrupts[0].u.Interrupt.Group=1;Expected=STATUS_DEVICE_CONFIGURATION_ERROR;break;
#endif
        }
        if(F>=6 && F<=13)Expected=STATUS_DEVICE_CONFIGURATION_ERROR;
        unsigned R=ControllerReads(),W=ControllerWrites();
        CHECK(MtkMaliBoardQueryInterfaceIrp(&Provider,Pdo,&PnpIrp));
        CHECK(PnpIrp.IoStatus.Status==Expected && !PnpIrp.IoStatus.Information && !Provider.References[0] && !Provider.References[1]);
        CHECK(RtlEqualMemory(&Before,&BoardBase,sizeof(Before)) && ControllerReads()==R && ControllerWrites()==W);
        Provider.Gate=0;BoardIrql=PASSIVE_LEVEL;
    }
    for(UINT F=0;F<5;F++) {
        InitPnpQuery(0);GUID Other=MtkMaliBoardInterfaceGuid;Other.Data1++;
        if(F==0)PnpStack.Parameters.QueryInterface.InterfaceType=&Other;
        if(F==1)PnpStack.MajorFunction=IRP_MJ_CREATE;
        if(F==2)PnpStack.MinorFunction=IRP_MN_QUERY_REMOVE_DEVICE;
        if(F==3)PnpStack.Parameters.QueryInterface.InterfaceType=0;
        if(F==4)PnpIrp.IoStatus.Status=STATUS_SUCCESS;
        IO_STATUS_BLOCK Before=PnpIrp.IoStatus;
        CHECK(!MtkMaliBoardQueryInterfaceIrp(&Provider,&PnpPdo,&PnpIrp));
        CHECK(RtlEqualMemory(&Before,&PnpIrp.IoStatus,sizeof(Before)) && !Provider.References[0] && !Provider.References[1]);
    }
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpOther)==STATUS_NO_SUCH_DEVICE);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DEVICE_BUSY && Provider.Pdo==&PnpPdo && Provider.Closing);
    InitPnpQuery(0);CHECK(MtkMaliBoardQueryInterfaceIrp(&Provider,&PnpPdo,&PnpIrp) && PnpIrp.IoStatus.Status==STATUS_DELETE_PENDING);
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardBindResources(&Provider,&PnpPdo,&PnpRaw.List,&PnpTranslated.List)==STATUS_DELETE_PENDING);
}
static void PnpControllerCycle(void) {
    InitPnp();CHECK(MtkMaliBoardBindResources(&Provider,&PnpPdo,&PnpRaw.List,&PnpTranslated.List)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardBindResources(&Provider,&PnpPdo,&PnpRaw.List,&PnpTranslated.List)==STATUS_DEVICE_BUSY);
    for(UINT I=0;I<2;I++) {
        InitPnpQuery(I);GUID ByValue=I?MtkMaliBoardPowerInterfaceGuid:MtkMaliBoardInterfaceGuid;
        PnpStack.Parameters.QueryInterface.InterfaceType=&ByValue;PnpStack.Parameters.QueryInterface.Version=3;
        CHECK(MtkMaliBoardQueryInterfaceIrp(&Provider,&PnpPdo,&PnpIrp) && PnpIrp.IoStatus.Status==STATUS_SUCCESS && !PnpIrp.IoStatus.Information);
    }
    CHECK(Provider.References[0]==1 && Provider.References[1]==1 && BoardBase.Header.Version==1 && BoardPower.Header.Version==1);
    MALI_BOARD_LEASE L={0};CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS && L.Cookie && Provider.PowerState==PowerDeviceD0);
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_DEVICE_BUSY && Provider.Pdo==&PnpPdo);
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_DEVICE_BUSY);
    CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_DEVICE_BUSY);
    BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);
    unsigned R=ControllerReads(),W=ControllerWrites();
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_SUCCESS && !Provider.Pdo);
    CHECK(ControllerReads()==R && ControllerWrites()==W);
    InitPnpQuery(0);CHECK(MtkMaliBoardQueryInterfaceIrp(&Provider,&PnpPdo,&PnpIrp) && PnpIrp.IoStatus.Status==STATUS_NO_SUCH_DEVICE);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    /* The provider owns a copy; PnP list buffers need not outlive Start. */
    InitPnp();PnpRawEntry(1)->Flags=CM_RESOURCE_INTERRUPT_POLICY_INCLUDED;
    PnpEntry(1)->Flags=CM_RESOURCE_INTERRUPT_WAKE_HINT;
    CHECK(MtkMaliBoardBindResources(&Provider,&PnpPdo,&PnpRaw.List,&PnpTranslated.List)==STATUS_SUCCESS);
    InitPnpQuery(0);PnpRawEntry(1)->u.Interrupt.Vector=0;PnpEntry(1)->u.Interrupt.Vector=0;
    CHECK(MtkMaliBoardQueryInterfaceIrp(&Provider,&PnpPdo,&PnpIrp) && PnpIrp.IoStatus.Status==STATUS_SUCCESS);
    BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    /* Surprise-removal policy must retain this poisoned instance, never unbind
       or free its live hardware resources. Actual PnP rundown remains host work. */
    InitPnp();CHECK(MtkMaliBoardBindResources(&Provider,&PnpPdo,&PnpRaw.List,&PnpTranslated.List)==STATUS_SUCCESS);
    InitPnpQuery(0);CHECK(MtkMaliBoardQueryInterfaceIrp(&Provider,&PnpPdo,&PnpIrp) && PnpIrp.IoStatus.Status==STATUS_SUCCESS);
    Stuck=1;CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_IO_TIMEOUT && L.Cookie && Quarantined);
    R=ControllerReads();W=ControllerWrites();
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_DEVICE_BUSY);
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY);
    BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);
    CHECK(MtkMaliBoardUnbindResources(&Provider,&PnpPdo)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DEVICE_BUSY && Provider.Pdo==&PnpPdo && ResourceLive);
    CHECK(ControllerReads()==R && ControllerWrites()==W);
}
/* These OS mocks execute the actual WDM stack-copy/completion/dispatch path.
   Completion can happen inline or after IoCallDriver returned STATUS_PENDING.
   No device object, hardware or Windows I/O manager is active in these tests. */
static unsigned PnpCalls,PnpCompletions,PnpWaits,PnpSignals,PnpDelays,PnpPending,PnpCancelled;
static unsigned PnpCheckFence,PnpDrainGate,PnpDrainActive,PnpPostGate;
static NTSTATUS PnpLowerResult;
static ULONG_PTR PnpLowerInformation;
static PIRP PnpOutstanding;
static PKEVENT PnpEvent;
static void PnpLowerComplete(PIRP Irp) {
    PIO_STACK_LOCATION Lower=IoGetCurrentIrpStackLocation(Irp);
    CHECK(Lower==&PnpStacks[0] && Lower->CompletionRoutine &&
        (Lower->Control&(SL_INVOKE_ON_SUCCESS|SL_INVOKE_ON_ERROR|SL_INVOKE_ON_CANCEL))==
        (SL_INVOKE_ON_SUCCESS|SL_INVOKE_ON_ERROR|SL_INVOKE_ON_CANCEL));
    PIO_COMPLETION_ROUTINE Completion=Lower->CompletionRoutine;void *Context=Lower->Context;
    Irp->IoStatus.Status=PnpLowerResult;Irp->IoStatus.Information=PnpLowerInformation;
    Irp->Cancel=(BOOLEAN)PnpCancelled;
    if(PnpPostGate){Provider.Gate=1;PnpDrainGate=1;}
    IoSkipCurrentIrpStackLocation(Irp);
    CHECK(Completion(&PnpOther,Irp,Context)==STATUS_MORE_PROCESSING_REQUIRED);
}
static NTSTATUS PnpCall(PDEVICE_OBJECT Lower,PIRP Irp) {
    CHECK(Lower==&PnpOther && Irp==&PnpIrp && !PnpOutstanding && Irp->CurrentLocation==2);
    if(PnpStack.MinorFunction==IRP_MN_QUERY_DEVICE_RELATIONS || PnpStack.MinorFunction==IRP_MN_QUERY_INTERFACE)
        CHECK(Irp->IoStatus.Status==STATUS_NOT_SUPPORTED && Irp->IoStatus.Information==123);
    else CHECK(Irp->IoStatus.Status==STATUS_SUCCESS && !Irp->IoStatus.Information);
    ++PnpCalls;IoSetNextIrpStackLocation(Irp);
    CHECK(IoGetCurrentIrpStackLocation(Irp)->MinorFunction==PnpStack.MinorFunction);
    if(PnpCheckFence) {
        MALI_BOARD_INTERFACE I;RtlFillMemory(&I,sizeof(I),0xa5);MALI_BOARD_INTERFACE Before=I;
        CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(I),&I)==
            (Provider.Closing?STATUS_DELETE_PENDING:STATUS_DEVICE_NOT_READY));
        CHECK(RtlEqualMemory(&Before,&I,sizeof(I)));
    }
    if(Provider.Removed && !Provider.Pdo && !Provider.References[0] && !Provider.References[1] && !Provider.ResourceLease.Cookie)
        CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DEVICE_BUSY); /* Dispatch still pins storage. */
    if(PnpPending){PnpOutstanding=Irp;return STATUS_PENDING;}
    PnpLowerComplete(Irp);return PnpLowerResult;
}
static void PnpComplete(PIRP Irp,CCHAR Boost) {
    CHECK(Irp==&PnpIrp && !Boost && !PnpOutstanding);
    ++PnpCompletions;
}
static void PnpEventInit(PRKEVENT Event,EVENT_TYPE Type,BOOLEAN State) {
    CHECK(Type==NotificationEvent && !State && !PnpOutstanding);
    PnpEvent=Event;Event->Header.SignalState=0;
}
static LONG PnpEventSet(PRKEVENT Event,KPRIORITY Increment,BOOLEAN Wait) {
    CHECK(Event==PnpEvent && !Increment && !Wait);++PnpSignals;
    LONG Previous=Event->Header.SignalState;Event->Header.SignalState=1;return Previous;
}
static NTSTATUS PnpWait(void *Event,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    CHECK(Event==PnpEvent && Reason==Executive && Mode==KernelMode && !Alert && !Timeout);++PnpWaits;
    if(PnpOutstanding){PIRP Irp=PnpOutstanding;PnpOutstanding=0;PnpLowerComplete(Irp);}
    CHECK(PnpEvent->Header.SignalState==1);return STATUS_SUCCESS;
}
static NTSTATUS PnpDelay(KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    CHECK(Mode==KernelMode && !Alert && Timeout && Timeout->QuadPart==-10000);++PnpDelays;
    if(PnpDrainActive){PnpDrainActive=0;InterlockedExchange(&Provider.PnpActive,0);}
    else if(PnpDrainGate){PnpDrainGate=0;InterlockedExchange(&Provider.Gate,0);}
    else CHECK(0); /* A stuck gate must never turn into a silent passing test. */
    return STATUS_SUCCESS;
}
static void InitPnpDispatch(UCHAR Minor) {
    RtlZeroMemory(&PnpIrp,sizeof(PnpIrp));RtlZeroMemory(PnpStacks,sizeof(PnpStacks));
    PnpIrp.Tail.Overlay.CurrentStackLocation=&PnpStack;PnpIrp.StackCount=2;PnpIrp.CurrentLocation=2;
    PnpIrp.RequestorMode=KernelMode;PnpIrp.IoStatus.Status=STATUS_NOT_SUPPORTED;
    PnpIrp.IoStatus.Information=123;PnpStack.MajorFunction=IRP_MJ_PNP;PnpStack.MinorFunction=Minor;
    PnpStack.Parameters.StartDevice.AllocatedResources=&PnpRaw.List;
    PnpStack.Parameters.StartDevice.AllocatedResourcesTranslated=&PnpTranslated.List;
    PnpCalls=PnpCompletions=PnpWaits=PnpSignals=PnpDelays=PnpPending=PnpCancelled=0;
    PnpCheckFence=PnpDrainGate=PnpDrainActive=PnpPostGate=0;PnpOutstanding=0;PnpEvent=0;
    PnpLowerResult=STATUS_SUCCESS;PnpLowerInformation=0;
}
static NTSTATUS DispatchPnp(UCHAR Minor) {
    InitPnpDispatch(Minor);return MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp);
}
static void PnpDispatchCycles(void) {
    InitPnp();
    for(UINT Cycle=0;Cycle<3;Cycle++) {
        InitPnpDispatch(IRP_MN_START_DEVICE);PnpPending=Cycle&1u;PnpCheckFence=1;PnpPostGate=1;
        CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_SUCCESS);
        CHECK(PnpDelays==1 && PnpCalls==1 && PnpCompletions==1 && PnpWaits==1 && PnpSignals==1 && Provider.Pdo==&PnpPdo && !Provider.Stopping);
        CHECK(DispatchPnp(IRP_MN_START_DEVICE)==STATUS_DEVICE_BUSY && !PnpCalls && Provider.Pdo==&PnpPdo);
        for(UINT Power=0;Power<2;Power++) {
            InitPnpQuery(Power);PnpIrp.CurrentLocation=2;PnpIrp.StackCount=2;
            PnpCalls=PnpCompletions=PnpWaits=PnpSignals=0;
            CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_SUCCESS);
            CHECK(PnpCompletions==1 && !PnpCalls && !PnpWaits && !PnpSignals);
        }
        MALI_BOARD_LEASE L={0};CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS && L.Cookie);
        CHECK(DispatchPnp(IRP_MN_QUERY_STOP_DEVICE)==STATUS_DEVICE_BUSY && !PnpCalls && !Provider.Stopping);
        CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS);
        CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
        CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
        CHECK(DispatchPnp(IRP_MN_QUERY_REMOVE_DEVICE)==STATUS_DEVICE_BUSY && !PnpCalls);
        BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);
        for(UINT Remove=0;Remove<2;Remove++) {
            UCHAR Query=Remove?IRP_MN_QUERY_REMOVE_DEVICE:IRP_MN_QUERY_STOP_DEVICE;
            UCHAR Cancel=Remove?IRP_MN_CANCEL_REMOVE_DEVICE:IRP_MN_CANCEL_STOP_DEVICE;
            InitPnpDispatch(Query);PnpPending=1;PnpCheckFence=1;
            CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_SUCCESS && Provider.Stopping==Query);
            MALI_BOARD_INTERFACE I;
            CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(I),&I)==STATUS_DEVICE_NOT_READY);
            CHECK(DispatchPnp(Remove?IRP_MN_CANCEL_STOP_DEVICE:IRP_MN_CANCEL_REMOVE_DEVICE)==STATUS_SUCCESS && Provider.Stopping==Query);
            InitPnpDispatch(Cancel);PnpLowerResult=STATUS_UNSUCCESSFUL;PnpPending=1;PnpCancelled=1;
            CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_UNSUCCESSFUL && Provider.Stopping==Query);
            CHECK(PnpCompletions==1 && PnpSignals==1 && PnpWaits==1);
            InitPnpDispatch(Cancel);Provider.Gate=1;PnpDrainGate=1;PnpPostGate=1;
            CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_SUCCESS && !Provider.Stopping && PnpDelays==2);
            CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(I),&I)==STATUS_SUCCESS);I.Header.InterfaceDereference(I.Header.Context);
        }
        CHECK(DispatchPnp(IRP_MN_QUERY_STOP_DEVICE)==STATUS_SUCCESS);
        InitPnpDispatch(IRP_MN_STOP_DEVICE);PnpPending=1;PnpCheckFence=1;PnpPostGate=1;Provider.Gate=1;PnpDrainGate=1;
        CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_SUCCESS && !Provider.Pdo && Provider.Stopping==IRP_MN_STOP_DEVICE);
        CHECK(!Provider.Closing && !ResourceLive && !Controller.Power.References);
    }
    CHECK(DispatchPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS);
    CHECK(DispatchPnp(IRP_MN_QUERY_REMOVE_DEVICE)==STATUS_SUCCESS);
    CHECK(DispatchPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS && !Provider.Pdo && Provider.Closing && Provider.Removed);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    CHECK(DispatchPnp(IRP_MN_START_DEVICE)==STATUS_DELETE_PENDING && !PnpCalls);
}
static void PnpDispatchFailures(void) {
    for(UINT Kind=0;Kind<3;Kind++)for(UINT Pending=0;Pending<2;Pending++) {
        InitPnp();InitPnpDispatch(IRP_MN_START_DEVICE);PnpPending=Pending;
        if(Kind==0)PnpLowerResult=STATUS_DEVICE_NOT_READY;
        if(Kind==1)PnpLowerResult=STATUS_TIMEOUT; /* success-class nonzero is not START success */
        if(Kind==2)PnpRawEntry(1)->u.Interrupt.Vector=396;
        unsigned R=ControllerReads(),W=ControllerWrites();
        CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==
            (Kind==0?STATUS_DEVICE_NOT_READY:Kind==1?STATUS_UNSUCCESSFUL:STATUS_DEVICE_CONFIGURATION_ERROR));
        CHECK(!Provider.Pdo && Provider.Stopping==IRP_MN_STOP_DEVICE && PnpCompletions==1 && !Provider.PnpActive);
        CHECK(ControllerReads()==R && ControllerWrites()==W && !ResourceLive);
        CHECK(DispatchPnp(IRP_MN_STOP_DEVICE)==STATUS_SUCCESS && !Provider.Pdo);
        PnpRawEntry(1)->u.Interrupt.Vector=428;
        CHECK(DispatchPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS);
        CHECK(DispatchPnp(IRP_MN_QUERY_STOP_DEVICE)==STATUS_SUCCESS);
        InitPnpDispatch(IRP_MN_STOP_DEVICE);PnpLowerResult=STATUS_UNSUCCESSFUL;
        CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_UNSUCCESSFUL && Provider.Pdo==&PnpPdo);
        CHECK(Provider.Stopping==IRP_MN_STOP_DEVICE);
        CHECK(DispatchPnp(IRP_MN_CANCEL_STOP_DEVICE)==STATUS_SUCCESS && Provider.Stopping==IRP_MN_STOP_DEVICE);
        CHECK(DispatchPnp(IRP_MN_STOP_DEVICE)==STATUS_SUCCESS && !Provider.Pdo);
        CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    }
    InitPnp();CHECK(DispatchPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS);
    for(UINT Kind=0;Kind<10;Kind++) {
        InitPnpDispatch(IRP_MN_QUERY_STOP_DEVICE);PDEVICE_OBJECT Pdo=&PnpPdo,Lower=&PnpOther;
        switch(Kind) {
        case 0:Pdo=&PnpOther;break;case 1:Lower=0;break;case 2:BoardIrql=DISPATCH_LEVEL;break;
        case 3:PnpIrp.RequestorMode=UserMode;break;case 4:PnpStack.MajorFunction=IRP_MJ_CREATE;break;
        case 5:PnpIrp.CurrentLocation=1;break;case 6:Provider.Gate=1;break;
        case 7:Provider.PnpActive=1;break;case 8:Provider.ReferenceFault=1;break;
        case 9:PnpIrp.StackCount=1;break;
        }
        CHECK(MtkMaliBoardDispatchPnp(&Provider,Pdo,Lower,&PnpIrp)==
            (Kind==0?STATUS_NO_SUCH_DEVICE:Kind==6 || Kind==7?STATUS_DEVICE_BUSY:Kind==8?STATUS_DRIVER_INTERNAL_ERROR:STATUS_INVALID_PARAMETER));
        CHECK(!PnpCalls && PnpCompletions==1 && !Provider.Stopping && Provider.Pdo==&PnpPdo);
        Provider.Gate=Provider.PnpActive=Provider.ReferenceFault=0;BoardIrql=PASSIVE_LEVEL;
    }
    /* A failed lower query remains fenced until PnP supplies its cancel. */
    InitPnpDispatch(IRP_MN_QUERY_REMOVE_DEVICE);PnpLowerResult=STATUS_UNSUCCESSFUL;PnpPending=1;
    CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_UNSUCCESSFUL && Provider.Stopping==IRP_MN_QUERY_REMOVE_DEVICE);
    CHECK(DispatchPnp(IRP_MN_CANCEL_REMOVE_DEVICE)==STATUS_SUCCESS && !Provider.Stopping);
    /* Unhandled PnP data and incoming status survive the lower round trip. */
    InitPnpDispatch(IRP_MN_QUERY_DEVICE_RELATIONS);PnpLowerInformation=0x1234;PnpPending=1;
    CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_SUCCESS && PnpIrp.IoStatus.Information==0x1234);
    CHECK(PnpCalls==1 && PnpCompletions==1 && PnpSignals==1 && PnpWaits==1);
    InitPnpDispatch(IRP_MN_QUERY_DEVICE_RELATIONS);PnpLowerResult=STATUS_TIMEOUT;PnpLowerInformation=0x5678;
    CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_TIMEOUT && PnpIrp.IoStatus.Information==0x5678);
    InitPnpQuery(0);GUID Other=MtkMaliBoardInterfaceGuid;Other.Data1++;
    PnpStack.Parameters.QueryInterface.InterfaceType=&Other;PnpIrp.CurrentLocation=2;PnpIrp.StackCount=2;
    PnpCalls=PnpCompletions=PnpWaits=PnpSignals=0;PnpLowerResult=STATUS_NOT_SUPPORTED;
    CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_NOT_SUPPORTED && PnpCalls==1 && PnpCompletions==1);
    for(UINT Forced=0;Forced<2;Forced++) {
        InitPnpDispatch(Forced?IRP_MN_REMOVE_DEVICE:IRP_MN_SURPRISE_REMOVAL);
        CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpOther,&PnpOther,&PnpIrp)==STATUS_NO_SUCH_DEVICE && !PnpCalls && !Provider.Removed && !Provider.Closing);
    }
    CHECK(DispatchPnp(IRP_MN_STOP_DEVICE)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
}
static void PnpForcedRemoval(void) {
    for(UINT Lease=0;Lease<3;Lease++) {
        InitPnp();CHECK(DispatchPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS);GetBoth();MALI_BOARD_LEASE L={0};
        if(Lease) {
            if(Lease==2)Stuck=1;
            CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==(Lease==2?STATUS_IO_TIMEOUT:STATUS_SUCCESS));
            CHECK(L.Cookie);
        }
        unsigned R=ControllerReads(),W=ControllerWrites(),E=Enters,A=Acquires;
        InitPnpDispatch(IRP_MN_SURPRISE_REMOVAL);PnpPending=1;PnpCheckFence=1;
        Provider.Gate=1;PnpDrainGate=1;Provider.PnpActive=1;PnpDrainActive=1;
        CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,&PnpIrp)==STATUS_SUCCESS);
        CHECK(PnpDelays==2 && Provider.Removed && Provider.Closing && Provider.Pdo==&PnpPdo);
        MALI_BOARD_LEASE Other={0};CHECK(BoardBase.Acquire(BoardBase.Header.Context,&Other)==STATUS_DEVICE_REMOVED && !Other.Cookie);
        if(Lease) {
            CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_REMOVED);
            CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_REMOVED);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_DEVICE_REMOVED);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionNone)==STATUS_DEVICE_REMOVED);
            CHECK(Provider.ClientCookie==L.Cookie && ResourceLive && Provider.ResourceLease.Cookie);
        }
        BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);
        CHECK(DispatchPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS && Provider.Removed && Provider.Closing);
        CHECK(MtkMaliBoardProviderClose(&Provider)==(Lease?STATUS_DEVICE_BUSY:STATUS_SUCCESS));
        CHECK(!!Provider.Pdo==!!Lease && !!ResourceLive==!!Lease);
        CHECK(ControllerReads()==R && ControllerWrites()==W && Enters==E && Acquires==A && !Provider.Gate && !Provider.PnpActive);
    }
    InitPnp();CHECK(DispatchPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS);
    unsigned R=ControllerReads(),W=ControllerWrites();Provider.ReferenceFault=1;
    CHECK(DispatchPnp(IRP_MN_SURPRISE_REMOVAL)==STATUS_SUCCESS && Provider.Removed);
    CHECK(DispatchPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS && Provider.Pdo==&PnpPdo);
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DRIVER_INTERNAL_ERROR);
    CHECK(ControllerReads()==R && ControllerWrites()==W && !Provider.Gate && !Provider.PnpActive);
}
static void PnpStorageLifetime(void) {
    for(UINT Dirty=0;Dirty<4;Dirty++) {
        InitProvider(0,0);RtlZeroMemory(&Provider,sizeof(Provider));
        if(Dirty==0)Provider.Closing=1;
        if(Dirty==1)Provider.Removed=1;
        if(Dirty==2)Provider.Stopping=IRP_MN_QUERY_STOP_DEVICE;
        if(Dirty==3)Provider.PnpActive=1;
        unsigned R=ControllerReads(),W=ControllerWrites();
        CHECK(MtkMaliBoardProviderInitialize(&Provider,&Config)==STATUS_DEVICE_BUSY && !Provider.Signature);
        CHECK(ControllerReads()==R && ControllerWrites()==W);
    }
    InitProvider(0,0);Provider.PnpActive=1;
    CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_DEVICE_BUSY && Provider.Closing);
    MALI_BOARD_INTERFACE I;
    CHECK(MtkMaliBoardGetInterface(&Provider,1,sizeof(I),&I)==STATUS_DELETE_PENDING);
    Provider.PnpActive=0;CHECK(MtkMaliBoardProviderClose(&Provider)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardDispatchPnp(&Provider,&PnpPdo,&PnpOther,0)==STATUS_INVALID_PARAMETER);
}
#ifndef MALI_BOARD_TEST_ENTRY
#define MALI_BOARD_TEST_ENTRY TestMain
#endif
int MALI_BOARD_TEST_ENTRY(void) {
    PnpStorageLifetime();if(Failure)return (int)Failure;
    PnpDispatchCycles();if(Failure)return (int)Failure;
    PnpDispatchFailures();if(Failure)return (int)Failure;
    PnpForcedRemoval();if(Failure)return (int)Failure;
    PnpBindingFailures();if(Failure)return (int)Failure;
    PnpQueryFailures();if(Failure)return (int)Failure;
    PnpControllerCycle();if(Failure)return (int)Failure;
    InvalidProviderConfiguration();if(Failure)return (int)Failure;
    QueryAndLifetime();if(Failure)return (int)Failure;
    AcquisitionFailures();if(Failure)return (int)Failure;
    ProviderCycles();if(Failure)return (int)Failure;
    BorrowedVoltageRelease();if(Failure)return (int)Failure;
    PowerAndReleaseFailures();return (int)Failure;
}
