/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual resource owner, Windows host, Board adapter and MFG controller.
   Only OS objects/I/O/cache/MMIO and the mandatory qualification are virtual. */
#define MALI_HOST_TEST_ENTRY UnusedHostTestEntry
#define MtkMaliHostAssertionCount MtkMaliSocAssertionCount
#include "HostTests.c"
#include "../../Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkConnPower.c"
static MALI_SOC_PROVIDER Owner;
static MALI_BOARD_PARENT_INTERFACE Factory;
static MALI_BOARD_CONFIG OwnerConfig;
static MALI_SOC_QUALIFICATION Qualification;
static MALI_MFG_OPP QualifiedEntry;
static void *OpenToken;
static ULONG LifeRefs,QualificationRefs,ProofCalls,DeniedProofs;
static NTSTATUS ProofStatus;
#define SOC_LIST_BYTES (FIELD_OFFSET(CM_RESOURCE_LIST,List)+FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR,PartialResourceList)+FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST,PartialDescriptors)+MALI_SOC_REGIONS*sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR))
#define SOC_MAP_ALL ((1u<<MALI_SOC_REGIONS)-1u)
static union {CM_RESOURCE_LIST List;UCHAR Bytes[SOC_LIST_BYTES];} OwnerRaw,OwnerTranslated;
static CM_PARTIAL_RESOURCE_DESCRIPTOR *OwnerEntry(BOOLEAN Raw,ULONG I) {
    return &(Raw?&OwnerRaw:&OwnerTranslated)->List.List[0].PartialResourceList.PartialDescriptors[I];
}
static void LifeReference(void *X) {CHECK(X==&LifeRefs && LifeRefs<MAXULONG);++LifeRefs;}
static void LifeDereference(void *X) {CHECK(X==&LifeRefs && LifeRefs);--LifeRefs;}
static void QualificationReference(void *X) {CHECK(X==&QualificationRefs && !QualificationRefs);++QualificationRefs;}
static void QualificationDereference(void *X) {CHECK(X==&QualificationRefs && QualificationRefs==1);--QualificationRefs;}
static NTSTATUS Qualify(void *X,PDEVICE_OBJECT Pdo,ULONG Proofs,uint64_t Hz,uint32_t Gpu,uint32_t Sram) {
    CHECK(X==&QualificationRefs && QualificationRefs==1 && Pdo==&PnpPdo && Proofs && !(Proofs&~15u));++ProofCalls;
    if(Proofs&MaliSocProofOpp)CHECK(Hz==ControllerExpectedHz && Gpu==ControllerExpectedGpu && Sram==ControllerExpectedSram);
    else CHECK(!Hz && !Gpu && !Sram);
    return Proofs&DeniedProofs?STATUS_DEVICE_NOT_READY:ProofStatus;
}
static void OwnerLists(ULONG Rotation,BOOLEAN Reverse) {
    RtlZeroMemory(&OwnerRaw,sizeof(OwnerRaw));RtlZeroMemory(&OwnerTranslated,sizeof(OwnerTranslated));
    OwnerRaw.List.Count=OwnerTranslated.List.Count=1;
    CM_PARTIAL_RESOURCE_LIST *A=&OwnerRaw.List.List[0].PartialResourceList,*B=&OwnerTranslated.List.List[0].PartialResourceList;
    A->Count=B->Count=MALI_SOC_REGIONS;A->Version=B->Version=A->Revision=B->Revision=1;
    for(ULONG I=0;I<MALI_SOC_REGIONS;I++) {
        ULONG Index=((Reverse?MALI_SOC_REGIONS-1-I:I)+Rotation)%MALI_SOC_REGIONS;
        CM_PARTIAL_RESOURCE_DESCRIPTOR *R=OwnerEntry(TRUE,I);R->Type=CmResourceTypeMemory;
        R->ShareDisposition=CmResourceShareDeviceExclusive;R->Flags=CM_RESOURCE_MEMORY_READ_WRITE;
        R->u.Memory.Start.QuadPart=SocBases[Index];R->u.Memory.Length=4096;*OwnerEntry(FALSE,I)=*R;
    }
}
static void InitOwner(ULONG Borrowed,ULONG Ref) {
    ResetHost();InitController(Borrowed,Ref);
    RtlZeroMemory(&Owner,sizeof(Owner));RtlZeroMemory(&Factory,sizeof(Factory));RtlZeroMemory(&OwnerConfig,sizeof(OwnerConfig));
    SocMapCalls=SocUnmapCalls=SocMapFail=SocMapMask=SocHardwareReads=SocHardwareWrites=SocStallUs=0;
    SocPmicReads=SocPmicWrites=0;
    SocMdlBuilds=SocCacheFlushes=LifeRefs=QualificationRefs=ProofCalls=DeniedProofs=0;SocMockThread=1;
    OpenToken=0;ProofStatus=STATUS_SUCCESS;OwnerLists(0,FALSE);
    MALI_BOARD_LIFETIME Life={&LifeRefs,LifeReference,LifeDereference};
    CHECK(MtkMaliSocInitialize(&Owner,&Life)==STATUS_SUCCESS && Owner.Mutex.Header.SignalState==1);
    QualifiedEntry.Hz=ControllerExpectedHz;QualifiedEntry.VgpuUv=ControllerExpectedGpu;QualifiedEntry.VsramUv=ControllerExpectedSram;
    RtlZeroMemory(&Qualification,sizeof(Qualification));Qualification.Size=sizeof(Qualification);Qualification.Version=2;
    Qualification.Context=&QualificationRefs;Qualification.Reference=QualificationReference;Qualification.Dereference=QualificationDereference;
    Qualification.Validate=Qualify;Qualification.Entries=&QualifiedEntry;Qualification.EntryCount=1;
    Qualification.AddressModel=MaliVmDirectSystemPhysical;Qualification.ApertureCommitLimit=64u*1024u*1024u;Qualification.TimeoutTicks=100;
}
static void StartOwner(BOOLEAN Qualified) {
    CHECK(MtkMaliSocStart(&Owner,&OwnerRaw.List,&OwnerTranslated.List)==STATUS_SUCCESS);
    CHECK(Owner.Started && SocMapMask==SOC_MAP_ALL && !SocHardwareReads && !SocHardwareWrites);
    if(Qualified)CHECK(MtkMaliSocSetQualification(&Owner,&Qualification)==STATUS_SUCCESS && QualificationRefs==1);
}
static void OpenOwner(void) {
    CHECK(MtkMaliSocGetFactory(&Owner,&PnpPdo,&Factory)==STATUS_SUCCESS && LifeRefs==1 && PdoObjectRefs==1);
    CHECK(Factory.Open(Factory.Header.Context,&PnpPdo,&OwnerConfig,&OpenToken)==STATUS_SUCCESS && OpenToken);
    CHECK(OwnerConfig.Controller==&Owner.Controller && !OwnerConfig.Lifetime.Reference && !SocHardwareReads && !SocHardwareWrites);
}
static void CloseOwner(void) {
    CHECK(Factory.Close(Factory.Header.Context,OpenToken)==STATUS_SUCCESS);OpenToken=0;
    Factory.Header.InterfaceDereference(Factory.Header.Context);CHECK(!LifeRefs && !PdoObjectRefs && !Owner.GpuPdo);
    CHECK(MtkMaliSocClose(&Owner)==STATUS_SUCCESS && !SocMapMask && !QualificationRefs && !Owner.Started);
}
static void ResourceMapping(void) {
    InitOwner(0,0);
    for(ULONG I=0;I<MALI_SOC_REGIONS;I++) {
        OwnerEntry(TRUE,I)->Flags|=CM_RESOURCE_MEMORY_PREFETCHABLE;
        OwnerEntry(FALSE,I)->Flags|=CM_RESOURCE_MEMORY_PREFETCHABLE;
    }
    StartOwner(FALSE);
    CHECK(MtkMaliSocStop(&Owner)==STATUS_SUCCESS && MtkMaliSocClose(&Owner)==STATUS_SUCCESS);
    for(ULONG Reverse=0;Reverse<2;Reverse++)for(ULONG Rotation=0;Rotation<MALI_SOC_REGIONS;Rotation++) {
        InitOwner(0,0);OwnerLists(Rotation,(BOOLEAN)Reverse);StartOwner(FALSE);
        CHECK(MtkMaliSocStart(&Owner,&OwnerRaw.List,&OwnerTranslated.List)==STATUS_DEVICE_BUSY && SocMapCalls==MALI_SOC_REGIONS);
        CHECK(MtkMaliSocStop(&Owner)==STATUS_SUCCESS && SocUnmapCalls==MALI_SOC_REGIONS && !SocMapMask);
        CHECK(MtkMaliSocStart(&Owner,&OwnerRaw.List,&OwnerTranslated.List)==STATUS_SUCCESS && SocMapCalls==2*MALI_SOC_REGIONS);
        CHECK(MtkMaliSocClose(&Owner)==STATUS_SUCCESS && SocUnmapCalls==2*MALI_SOC_REGIONS && !SocHardwareReads && !SocHardwareWrites);
    }
    for(ULONG Fail=1;Fail<=MALI_SOC_REGIONS;Fail++) {
        InitOwner(0,0);SocMapFail=Fail;
        CHECK(MtkMaliSocStart(&Owner,&OwnerRaw.List,&OwnerTranslated.List)==STATUS_INSUFFICIENT_RESOURCES);
        CHECK(!SocMapMask && !Owner.Started && SocUnmapCalls==Fail-1 && !SocHardwareReads && !SocHardwareWrites);
        SocMapFail=0;StartOwner(FALSE);CHECK(MtkMaliSocClose(&Owner)==STATUS_SUCCESS && !SocMapMask);
    }
    for(ULONG Kind=0;Kind<20;Kind++) {
        InitOwner(0,0);const CM_RESOURCE_LIST *Raw=&OwnerRaw.List,*Translated=&OwnerTranslated.List;
        switch(Kind) {
        case 0:Raw=0;break;case 1:Translated=0;break;case 2:OwnerRaw.List.Count=2;break;
        case 3:OwnerTranslated.List.Count=0;break;case 4:OwnerRaw.List.List[0].PartialResourceList.Version=0;break;
        case 5:OwnerTranslated.List.List[0].PartialResourceList.Revision=2;break;
        case 6:OwnerRaw.List.List[0].PartialResourceList.Count=MALI_SOC_REGIONS-1;break;
        case 7:OwnerTranslated.List.List[0].PartialResourceList.Count=MALI_SOC_REGIONS+1;break;
        case 8:OwnerEntry(TRUE,0)->Type=CmResourceTypePort;break;case 9:OwnerEntry(FALSE,0)->Type=CmResourceTypeInterrupt;break;
        case 10:OwnerEntry(TRUE,1)->ShareDisposition=CmResourceShareShared;break;case 11:OwnerEntry(FALSE,2)->ShareDisposition=CmResourceShareUndetermined;break;
        case 12:OwnerEntry(TRUE,3)->Flags=CM_RESOURCE_MEMORY_READ_ONLY;break;case 13:OwnerEntry(FALSE,4)->Flags=CM_RESOURCE_MEMORY_WRITE_ONLY;break;
        case 14:OwnerEntry(TRUE,0)->u.Memory.Length=4095;break;
        case 15:OwnerEntry(FALSE,MALI_SOC_REGIONS-1)->u.Memory.Length=8192;break;
        case 16:OwnerEntry(FALSE,2)->u.Memory.Start.QuadPart+=4096;break;
        case 17:OwnerEntry(TRUE,2)->u.Memory.Start.QuadPart=OwnerEntry(FALSE,2)->u.Memory.Start.QuadPart=0x1000d000;break;
        case 18:*OwnerEntry(TRUE,4)=*OwnerEntry(TRUE,1);*OwnerEntry(FALSE,4)=*OwnerEntry(FALSE,1);break;
        case 19:OwnerEntry(TRUE,0)->u.Memory.Start.QuadPart=OwnerEntry(FALSE,0)->u.Memory.Start.QuadPart=-4096;break;
        }
        CHECK(MtkMaliSocStart(&Owner,Raw,Translated)==STATUS_DEVICE_CONFIGURATION_ERROR);
        CHECK(!SocMapCalls && !SocHardwareReads && !SocHardwareWrites && !Owner.Started);
        CHECK(MtkMaliSocClose(&Owner)==STATUS_SUCCESS);
    }
}
static void MandatoryQualification(void) {
    InitOwner(0,0);StartOwner(FALSE);OpenOwner();MALI_BOARD_LEASE L={0};L.TimeoutTicks=42;
    CHECK(OwnerConfig.Resources.Acquire(&Owner,&L)==STATUS_DEVICE_NOT_READY && !L.Cookie && L.TimeoutTicks==42);
    CHECK(!SocHardwareReads && !SocHardwareWrites && !Owner.ResourceCookie);CloseOwner();
    for(ULONG Kind=0;Kind<16;Kind++) {
        InitOwner(0,0);StartOwner(FALSE);MALI_SOC_QUALIFICATION Q=Qualification;
        switch(Kind) {
        case 0:Q.Size--;break;case 1:Q.Version++;break;case 2:Q.Reference=0;break;case 3:Q.Dereference=0;break;
        case 4:Q.Validate=0;break;case 5:Q.Entries=0;break;case 6:Q.EntryCount=0;break;case 7:Q.EntryCount=MALI_MFG_OPP_MAX+1;break;
        case 8:Q.AddressModel=MaliVmAddressingUnknown;break;case 9:Q.TimeoutTicks=0;break;case 10:Q.ApertureCommitLimit=0;break;
        case 11:Q.ApertureCommitLimit=4097;break;case 12:Q.ApertureCommitLimit=MALI_ALLOCATION_MAX_BYTES+4096;break;
        case 13:QualifiedEntry.Hz=0;break;case 14:QualifiedEntry.VgpuUv=1;break;case 15:Q.Entries=(const MALI_MFG_OPP *)&Owner;break;
        }
        CHECK(MtkMaliSocSetQualification(&Owner,&Q)==STATUS_INVALID_PARAMETER && !Owner.Qualified && !QualificationRefs);
        CHECK(!SocHardwareReads && !SocHardwareWrites);CHECK(MtkMaliSocClose(&Owner)==STATUS_SUCCESS);
    }
    InitOwner(0,0);StartOwner(TRUE);MALI_MFG_OPP Saved=Owner.Policy.Entries[0];QualifiedEntry.Hz=0;
    CHECK(Owner.Qualification.Entries==Owner.Policy.Entries && Owner.Policy.Entries[0].Hz==Saved.Hz);
    CHECK(MtkMaliSocSetQualification(&Owner,&Qualification)==STATUS_DEVICE_BUSY && QualificationRefs==1);
    CHECK(MtkMaliSocStop(&Owner)==STATUS_SUCCESS && !QualificationRefs && !Owner.Policy.Signature);
    QualifiedEntry=Saved;StartOwner(TRUE);OpenOwner();
    for(ULONG Proof=1;Proof<=2;Proof++) {
        DeniedProofs=Proof;MALI_BOARD_LEASE L={0};CHECK(OwnerConfig.Resources.Acquire(&Owner,&L)==STATUS_DEVICE_NOT_READY && !L.Cookie);
    }
    DeniedProofs=0;ProofStatus=STATUS_TIMEOUT;MALI_BOARD_LEASE Lease={0};
    CHECK(OwnerConfig.Resources.Acquire(&Owner,&Lease)==STATUS_UNSUCCESSFUL && !Lease.Cookie);ProofStatus=STATUS_SUCCESS;
    CHECK(!SocHardwareReads && !SocHardwareWrites);CloseOwner();
}
static void CalibratedQualification(void) {
    MALI_MFG_OPP E[MALI_MFG_STOCK_OPP_COUNT];MALI_MFG_CALIBRATION C;
    InitOwner(0,0);StartOwner(FALSE);CalibrationFixture(E,&C);C.Segment=1;
    Qualification.Entries=E;Qualification.EntryCount=MALI_MFG_STOCK_OPP_COUNT;Qualification.Calibration=&C;
    C.PresentMask=3;
    CHECK(MtkMaliSocSetQualification(&Owner,&Qualification)==STATUS_INVALID_PARAMETER && !Owner.Qualified && !QualificationRefs);
    CHECK(!Owner.Policy.Signature && !Owner.Policy.Count && !SocHardwareReads && !SocHardwareWrites);
    C.PresentMask=7;
    CHECK(MtkMaliSocSetQualification(&Owner,&Qualification)==STATUS_SUCCESS && QualificationRefs==1);
    CHECK(Owner.Policy.Count==38 && Owner.Policy.Entries[0].Hz==E[7].Hz);
    CHECK(Owner.Qualification.Entries==Owner.Policy.Entries && Owner.Qualification.EntryCount==38 && !Owner.Qualification.Calibration);
    E[7].Hz=0;C.PresentMask=0;
    CHECK(Owner.Policy.Entries[0].Hz && !SocHardwareReads && !SocHardwareWrites);
    CHECK(MtkMaliSocStop(&Owner)==STATUS_SUCCESS && !Owner.Qualified && !QualificationRefs);
    for(ULONG Bad=0;Bad<3;Bad++) {
        InitOwner(0,0);StartOwner(FALSE);CalibrationFixture(E,&C);
        Qualification.Entries=E;Qualification.EntryCount=45;Qualification.Calibration=&C;
        if(Bad==0)Qualification.Calibration=(const MALI_MFG_CALIBRATION *)&Owner;
        if(Bad==1)Qualification.Calibration=(const MALI_MFG_CALIBRATION *)&Qualification;
        if(Bad==2)Qualification.Version=1;
        CHECK(MtkMaliSocSetQualification(&Owner,&Qualification)==STATUS_INVALID_PARAMETER && !Owner.Qualified && !QualificationRefs);
        CHECK(!SocHardwareReads && !SocHardwareWrites && MtkMaliSocClose(&Owner)==STATUS_SUCCESS);
    }
}
static void ControllerThroughOwner(void) {
    for(ULONG Borrowed=0;Borrowed<4;Borrowed++)for(ULONG Ref=0;Ref<4;Ref++) {
        InitOwner(Borrowed,Ref);StartOwner(TRUE);OpenOwner();RtlZeroMemory(&Provider,sizeof(Provider));
        CHECK(MtkMaliBoardProviderInitialize(&Provider,&OwnerConfig)==STATUS_SUCCESS);GetBoth();MALI_BOARD_LEASE L={0};
        CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS && L.Cookie && Owner.ResourceCookie);
        CHECK(Owner.Controller.State==MaliMfgControllerOn && Owner.Rails.Acquired==3 && Owner.Clock.Held==7);
        CHECK(!Owner.AccessThread && Owner.Mutex.Header.SignalState==1 && !Owner.Quarantined);
        for(ULONG Cycle=0;Cycle<3;Cycle++) {
            CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
            CHECK(MtkMaliSocSuspend(&Owner)==STATUS_DEVICE_BUSY && !Owner.Suspended);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS && SocClean(&Owner));
            CHECK(MtkMaliSocSuspend(&Owner)==STATUS_SUCCESS && Owner.Suspended);
            ULONG Reads=SocHardwareReads,Writes=SocHardwareWrites;
            CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY);
            CHECK(SocHardwareReads==Reads && SocHardwareWrites==Writes && !Owner.AccessThread);
            CHECK(MtkMaliSocResume(&Owner)==STATUS_SUCCESS && !Owner.Suspended);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
            CHECK(MtkMaliSocFencePower(&Owner)==STATUS_SUCCESS && Owner.Suspended);
            CHECK(MtkMaliSocResume(&Owner)==STATUS_SUCCESS && !Owner.Suspended);
        }
        void *Old=Owner.ResourceCookie;
        CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS && !Owner.ResourceCookie && SocClean(&Owner));DropBoth();
        MALI_BOARD_LEASE Direct={0};CHECK(OwnerConfig.Resources.Acquire(&Owner,&Direct)==STATUS_SUCCESS && Direct.Cookie!=Old);
        CHECK(OwnerConfig.Resources.Enter(&Owner,Old)==STATUS_INVALID_PARAMETER);
        CHECK(OwnerConfig.Resources.Enter(&Owner,Direct.Cookie)==STATUS_SUCCESS);
        CHECK(Owner.AccessThread==SocTestThread() && Owner.Mutex.OwnerThread==SocTestThread() && Owner.Mutex.Header.SignalState==0);
        CHECK(OwnerConfig.Resources.Validate(&Owner,&Direct)==STATUS_SUCCESS);
        MALI_BOARD_LEASE Bad=Direct;Bad.TimeoutTicks++;CHECK(OwnerConfig.Resources.Validate(&Owner,&Bad)==STATUS_DEVICE_NOT_READY);
        OwnerConfig.Resources.Leave(&Owner,Direct.Cookie,FALSE);CHECK(!Owner.AccessThread && Owner.Mutex.Header.SignalState==1);
        CHECK(OwnerConfig.Resources.Validate(&Owner,&Direct)==STATUS_DEVICE_NOT_READY);
        CHECK(OwnerConfig.Resources.Release(&Owner,Direct.Cookie)==STATUS_SUCCESS);
        CHECK(OwnerConfig.Resources.Release(&Owner,Direct.Cookie)==STATUS_INVALID_PARAMETER);
        CHECK(ProofCalls && SocHardwareReads && SocHardwareWrites && SocStallUs && !Owner.Quarantined);CloseOwner();
        if(Failure)return;
    }
}
static void CacheAndFaultLifetime(void) {
    InitOwner(0,0);StartOwner(TRUE);OpenOwner();
    static UCHAR Buffer[4*4096];uintptr_t Page=((uintptr_t)Buffer+4095)&~(uintptr_t)4095;
    MtkMaliSocCleanForGpu(&Owner,(void *)(Page+4093),4102);
    CHECK(SocMdlBuilds==3 && SocCacheFlushes==3 && !SocHardwareReads && !SocHardwareWrites);
    MtkMaliSocCleanForGpu(&Owner,(void *)Page,0);CHECK(SocMdlBuilds==3);CloseOwner();
    for(ULONG Fault=0;Fault<3;Fault++) {
        InitOwner(0,0);StartOwner(TRUE);OpenOwner();MALI_BOARD_LEASE L={0};
        CHECK(OwnerConfig.Resources.Acquire(&Owner,&L)==STATUS_SUCCESS);
        if(Fault==0) {
            CHECK(OwnerConfig.Resources.Enter(&Owner,L.Cookie)==STATUS_SUCCESS);SocMockThread=2;
            OwnerConfig.Resources.Leave(&Owner,L.Cookie,FALSE);CHECK(Owner.Quarantined && Owner.AccessThread && Owner.Mutex.Header.SignalState==0);
            SocMockThread=1;OwnerConfig.Resources.Leave(&Owner,L.Cookie,FALSE);
        } else if(Fault==1) {
            CHECK(OwnerConfig.Resources.Enter(&Owner,L.Cookie)==STATUS_SUCCESS);
            CHECK(SocRead(&Owner,6,0)==0 && Owner.Quarantined);OwnerConfig.Resources.Leave(&Owner,L.Cookie,FALSE);
        } else CHECK(MtkMaliSocWithdraw(&Owner)==STATUS_SUCCESS && Owner.Closing && Owner.Removed);
        ULONG Reads=SocHardwareReads,Writes=SocHardwareWrites;
        CHECK(OwnerConfig.Resources.Enter(&Owner,L.Cookie)==STATUS_DEVICE_NOT_READY);
        CHECK(OwnerConfig.Resources.Release(&Owner,L.Cookie)==STATUS_DEVICE_BUSY);
        CHECK(Factory.Close(&Owner,OpenToken)==STATUS_DEVICE_BUSY && Owner.OpenCookie && Owner.ResourceCookie && SocMapMask==SOC_MAP_ALL);
        CHECK(MtkMaliSocClose(&Owner)==STATUS_DEVICE_BUSY && SocHardwareReads==Reads && SocHardwareWrites==Writes);
    }
    InitOwner(0,0);StartOwner(TRUE);OpenOwner();Factory.Header.InterfaceReference(&Owner);
    CHECK(LifeRefs==2 && Owner.References==2);CHECK(Factory.Close(&Owner,OpenToken)==STATUS_SUCCESS);OpenToken=0;
    CHECK(MtkMaliSocWithdraw(&Owner)==STATUS_SUCCESS && MtkMaliSocClose(&Owner)==STATUS_DEVICE_BUSY);
    Factory.Header.InterfaceDereference(&Owner);CHECK(LifeRefs==1 && PdoObjectRefs==1);
    Factory.Header.InterfaceDereference(&Owner);CHECK(!LifeRefs && !PdoObjectRefs);
    CHECK(MtkMaliSocClose(&Owner)==STATUS_SUCCESS && !SocMapMask && !QualificationRefs);
}
static void OwnerAdmissionAndFailure(void) {
    InitOwner(0,0);StartOwner(TRUE);
    for(ULONG Kind=1;Kind<=6;Kind++) {
        IdKind=Kind;NTSTATUS Expected=Kind==3 || Kind==4?STATUS_DEVICE_CONFIGURATION_ERROR:
            Kind==5?STATUS_BUFFER_TOO_SMALL:Kind==2?STATUS_SUCCESS:STATUS_NO_SUCH_DEVICE;
        CHECK(MtkMaliSocGetFactory(&Owner,&PnpPdo,&Factory)==Expected);
        if(Expected==STATUS_SUCCESS)Factory.Header.InterfaceDereference(&Owner);
        CHECK(!LifeRefs && !PdoObjectRefs && !Owner.References);
    }
    IdKind=0;OpenOwner();MALI_BOARD_LEASE L={0};
    CHECK(OwnerConfig.Resources.Acquire(&Owner,&L)==STATUS_SUCCESS);
    DeniedProofs=MaliSocProofDirectDma;CHECK(OwnerConfig.Resources.Enter(&Owner,L.Cookie)==STATUS_DEVICE_NOT_READY);
    CHECK(!Owner.AccessThread && Owner.Mutex.Header.SignalState==1 && !SocHardwareReads && !SocHardwareWrites);
    DeniedProofs=0;CHECK(MtkMaliSocSuspend(&Owner)==STATUS_SUCCESS);DeniedProofs=MaliSocProofOwner;
    CHECK(MtkMaliSocResume(&Owner)==STATUS_DEVICE_NOT_READY && Owner.Suspended);
    CHECK(OwnerConfig.Resources.Enter(&Owner,L.Cookie)==STATUS_DEVICE_NOT_READY && !SocHardwareReads);
    DeniedProofs=0;CHECK(MtkMaliSocResume(&Owner)==STATUS_SUCCESS);
    CHECK(OwnerConfig.Resources.Release(&Owner,L.Cookie)==STATUS_SUCCESS);
    Owner.Generation=UINTPTR_MAX;CHECK(OwnerConfig.Resources.Acquire(&Owner,&L)==STATUS_INSUFFICIENT_RESOURCES);
    CloseOwner();
    InitOwner(0,0);StartOwner(TRUE);OpenOwner();RtlZeroMemory(&Provider,sizeof(Provider));
    CHECK(MtkMaliBoardProviderInitialize(&Provider,&OwnerConfig)==STATUS_SUCCESS);GetBoth();Stuck=1;
    CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_IO_TIMEOUT && L.Cookie && Owner.ResourceCookie);
    CHECK(Owner.Quarantined && Owner.Controller.Poisoned && Owner.Rails.Acquired==3 && SocMapMask==SOC_MAP_ALL);
    ULONG Reads=SocHardwareReads,Writes=SocHardwareWrites;
    CHECK(MtkMaliSocWithdraw(&Owner)==STATUS_SUCCESS);
    CHECK(BoardBase.Ready(BoardBase.Header.Context,L.Cookie)==STATUS_DEVICE_NOT_READY);
    CHECK(Factory.Close(&Owner,OpenToken)==STATUS_DEVICE_BUSY && MtkMaliSocClose(&Owner)==STATUS_DEVICE_BUSY);
    CHECK(SocHardwareReads==Reads && SocHardwareWrites==Writes && SocMapMask==SOC_MAP_ALL);
}
static MALI_BOARD_HOST RootHolder;
static void DeferredOwnerStart(void) {
    InitOwner(0,0);CHECK(MtkMaliBoardHostDriverEntry(&HostDriver,0)==STATUS_SUCCESS);QueryResult=STATUS_NOT_SUPPORTED;
    CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)==STATUS_SUCCESS && Attached && !HostStore.BoardInitialized);
    RtlZeroMemory(&RootHolder,sizeof(RootHolder));MALI_BOARD_LIFETIME Life={&LifeRefs,LifeReference,LifeDereference};
    CHECK(MtkMaliSocInitialize(&RootHolder.Soc,&Life)==STATUS_SUCCESS);
    CHECK(MtkMaliSocStart(&RootHolder.Soc,&OwnerRaw.List,&OwnerTranslated.List)==STATUS_SUCCESS);
    CHECK(MtkMaliSocSetQualification(&RootHolder.Soc,&Qualification)==STATUS_SUCCESS);
    CHECK(BhSocPublish(&RootHolder,TRUE)==STATUS_SUCCESS);ParentEvent=0;
    CHECK(HostPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS && HostStore.BoardInitialized && HostStore.Board.Pdo==&PnpPdo);
    CHECK(HostStore.Parent.Header.Context==&RootHolder.Soc && LifeRefs==1 && PdoObjectRefs==2);
    GetBoth();MALI_BOARD_LEASE L={0};CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS);
    CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
    BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);
    InitPnpDispatch(IRP_MN_SURPRISE_REMOVAL);HostStore.PnpActive=1;
    CHECK(BhDeferredPnp(&HostStore,&PnpIrp)==STATUS_SUCCESS && Provider.Removed && Provider.Closing && !HostStore.PnpActive);
    CHECK(HostPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS);RunHostWork();
    CHECK(Deleted && !LifeRefs && !RootHolder.Soc.References && !RootHolder.Soc.OpenCookie);
    CHECK(BhSocPublish(&RootHolder,FALSE)==STATUS_SUCCESS);
    CHECK(MtkMaliSocClose(&RootHolder.Soc)==STATUS_SUCCESS && !SocMapMask && !QualificationRefs);
}
static NTSTATUS RootPnp(UCHAR Minor,BOOLEAN Pending,NTSTATUS Status) {
    InitPnpDispatch(Minor);PnpPending=Pending;PnpLowerResult=Status;
    PnpStack.Parameters.StartDevice.AllocatedResources=&OwnerRaw.List;
    PnpStack.Parameters.StartDevice.AllocatedResourcesTranslated=&OwnerTranslated.List;
    return MtkMaliBoardHostDispatch(&HostDevice,&PnpIrp);
}
static NTSTATUS RootPower(DEVICE_POWER_STATE State,BOOLEAN Pending,NTSTATUS Status) {
    RtlZeroMemory(&PassIrp,sizeof(PassIrp));RtlZeroMemory(PassStacks,sizeof(PassStacks));
    PassIrp.Tail.Overlay.CurrentStackLocation=&PassStacks[1];PassIrp.CurrentLocation=2;PassIrp.StackCount=2;
    PassStacks[1].MajorFunction=IRP_MJ_POWER;PassStacks[1].MinorFunction=IRP_MN_SET_POWER;
    PassStacks[1].Parameters.Power.Type=DevicePowerState;PassStacks[1].Parameters.Power.State.DeviceState=State;
    PassPending=Pending;PassStatus=Status;ParentEvent=0;
    return MtkMaliBoardHostDispatch(&HostDevice,&PassIrp);
}
static NTSTATUS RootConnQuery(const GUID *Guid,USHORT Version,USHORT Size,MTK_CONN_POWER_INTERFACE *Out,BOOLEAN Pending) {
    RtlZeroMemory(&PassIrp,sizeof(PassIrp));RtlZeroMemory(PassStacks,sizeof(PassStacks));
    PassIrp.Tail.Overlay.CurrentStackLocation=&PassStacks[1];PassIrp.CurrentLocation=2;PassIrp.StackCount=2;
    PassIrp.RequestorMode=KernelMode;PassIrp.IoStatus.Status=STATUS_NOT_SUPPORTED;PassIrp.IoStatus.Information=123;
    IO_STACK_LOCATION *Stack=&PassStacks[1];
    Stack->MajorFunction=IRP_MJ_PNP;Stack->MinorFunction=IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType=Guid;Stack->Parameters.QueryInterface.Version=Version;
    Stack->Parameters.QueryInterface.Size=Size;Stack->Parameters.QueryInterface.Interface=(PINTERFACE)Out;
    PassPending=Pending;PassStatus=STATUS_NOT_SUPPORTED;
    return MtkMaliBoardHostDispatch(&HostDevice,&PassIrp);
}
typedef struct {MTK_CONN_POWER_INTERFACE Interface;ULONGLONG Token;} TEST_CONN_CLIENT;
static int TestConnRead(void *Context,MTK_CONN_REGION Region,unsigned Offset,unsigned *Value) {
    TEST_CONN_CLIENT *Client=Context;
    ULONG Live;
    if(Client->Interface.Read(Client->Interface.Header.Context,Client->Token,Region,Offset,&Live)!=STATUS_SUCCESS)return 1;
    *Value=Live;return 0;
}
static int TestConnWrite(void *Context,MTK_CONN_REGION Region,unsigned Offset,unsigned Value) {
    TEST_CONN_CLIENT *Client=Context;
    return Client->Interface.Write(Client->Interface.Header.Context,Client->Token,Region,Offset,Value)!=STATUS_SUCCESS;
}
static int TestConnWait(void *Context,unsigned Us) {
    (void)Context;SocTestStall(Us);return 0;
}
static void TestConnBarrier(void *Context) {(void)Context;__dsb(15);}
static void ConnInterfaceLifecycle(void) {
    InitOwner(0,0);IdKind=6;
    CHECK(MtkMaliBoardHostDriverEntry(&HostDriver,0)==STATUS_SUCCESS);
    ConnRegisterResult=STATUS_INSUFFICIENT_RESOURCES;
    CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)==ConnRegisterResult && Deleted && !Attached && !ConnRegistrations);

    InitOwner(0,0);IdKind=6;
    CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)==STATUS_SUCCESS && ConnRegistrations==1 && !ConnLinkLive);
    TEST_CONN_CLIENT Client={0};
    const GUID *Guid=&GUID_MTK_CONN_POWER_INTERFACE_V1;
    const USHORT Bytes=sizeof(Client.Interface);
    CHECK(RootConnQuery(Guid,1,Bytes,&Client.Interface,FALSE)==STATUS_DEVICE_NOT_READY && !Client.Interface.Header.Context);
    ConnEnableResult=STATUS_INSUFFICIENT_RESOURCES;
    CHECK(RootPnp(IRP_MN_START_DEVICE,FALSE,STATUS_SUCCESS)==ConnEnableResult && !SocMapMask && !BhSocOwner && !ConnLinkLive);
    ConnEnableResult=STATUS_OBJECT_NAME_EXISTS;
    CHECK(RootPnp(IRP_MN_START_DEVICE,TRUE,STATUS_SUCCESS)==STATUS_SUCCESS && ConnLinkLive && ConnEnables==2);
    CHECK(RootConnQuery(&BhParentGuid,1,Bytes,&Client.Interface,TRUE)==STATUS_PENDING && LockCount==2 && PassOutstanding);
    CompletePass();
    CHECK(LockCount==1 && PassIrp.IoStatus.Status==STATUS_NOT_SUPPORTED && PassIrp.IoStatus.Information==123);
    CHECK(RootConnQuery(Guid,2,Bytes,&Client.Interface,FALSE)==STATUS_NOT_SUPPORTED && !HostObjectRefs);
    CHECK(RootConnQuery(Guid,1,Bytes-1,&Client.Interface,FALSE)==STATUS_BUFFER_TOO_SMALL && !HostObjectRefs);
    CHECK(RootConnQuery(Guid,1,Bytes,0,FALSE)==STATUS_INVALID_PARAMETER && !HostObjectRefs);
    CHECK(RootConnQuery(Guid,1,Bytes,&Client.Interface,FALSE)==STATUS_SUCCESS && HostObjectRefs==1 &&
        Client.Interface.Header.Size==Bytes && Client.Interface.Header.Version==1 && !PassIrp.IoStatus.Information);
    MTK_CONN_POWER_INTERFACE *Interface=&Client.Interface;
    void *Device=Interface->Header.Context;
    Interface->Header.InterfaceReference(Device);
    CHECK(HostObjectRefs==2 && HostStore.Soc.ConnReferences==2);
    Interface->Header.InterfaceDereference(Device);
    CHECK(HostObjectRefs==1 && HostStore.Soc.ConnReferences==1);

    RtlZeroMemory(SocMappedRegisters,sizeof(SocMappedRegisters));SocConnMock=TRUE;
    ULONG *Spm=SocMappedRegisters[0],*Infra=SocMappedRegisters[1];
    Spm[0x304/4]=0x88000012;Spm[0x16c/4]=0x00200000;Spm[0x170/4]=0x00400000;
    Infra[0x90/4]=0x80200000;
    Infra[0x0c8/4]=0x11000000;
    Infra[0x220/4]=Infra[0x228/4]=0x80046000;
    Infra[0x250/4]=Infra[0x258/4]=0x40000400;
    Infra[0x380/4]=0xa4509999;Infra[0x388/4]=0x33211111;
    Infra[0x0f00/4]=0x12345678U;
    const ULONGLONG EmiBase=0x51000000;
    CHECK(Interface->Acquire(Device,0,&Client.Token)==STATUS_INVALID_PARAMETER);
    CHECK(Interface->Acquire(Device,EmiBase+4096,&Client.Token)==STATUS_INVALID_PARAMETER);
    CHECK(Interface->Acquire(Device,MTK_CONN_EMI_LIMIT,&Client.Token)==STATUS_INVALID_PARAMETER);
    CHECK(!SocHardwareReads && !SocHardwareWrites);
    Spm[0x16c/4]|=2;
    CHECK(Interface->Acquire(Device,EmiBase,&Client.Token)==STATUS_DEVICE_BUSY && !Client.Token);
    Spm[0x16c/4]=MAXULONG;
    CHECK(Interface->Acquire(Device,EmiBase,&Client.Token)==STATUS_DEVICE_HARDWARE_ERROR && !SocHardwareWrites);
    Spm[0x16c/4]=0x00200000;
    CHECK(Interface->Acquire(Device,EmiBase,&Client.Token)==STATUS_SUCCESS && Client.Token && HostObjectRefs==2 && !SocHardwareWrites);
    ULONGLONG PriorToken=Client.Token,Additional=0;
    CHECK(Interface->Acquire(Device,EmiBase,&Additional)==STATUS_DEVICE_BUSY && !Additional);
    ULONG Reads=SocHardwareReads,Value=0;
    CHECK(Interface->Read(Device,PriorToken+1,MtkConnSpm,0x304,&Value)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Read(Device,PriorToken,MtkConnSpm,0x300,&Value)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0x16c,2)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0x304,0x80)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x2a4,0x46000)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x0c4,1u<<27)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x380,0x5200)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x388,0x2000)==STATUS_ACCESS_DENIED);
    HostIrql=DISPATCH_LEVEL;
    CHECK(Interface->Read(Device,PriorToken,MtkConnSpm,0x304,&Value)==STATUS_INVALID_PARAMETER);
    HostIrql=PASSIVE_LEVEL;
    CHECK(SocHardwareReads==Reads && !SocHardwareWrites);
    CHECK(Interface->Read(Device,PriorToken,MtkConnInfra,0x0f00,&Value)==STATUS_SUCCESS && Value==0);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x0f00,0x00000400U)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x0f00,0x00000800U)==STATUS_SUCCESS &&
          Infra[0x0f00/4]==0x12345e78U);
    CHECK(Interface->Release(Device,PriorToken)==STATUS_DEVICE_BUSY);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x0f00,0U)==STATUS_SUCCESS &&
          Infra[0x0f00/4]==0x12345678U);

    Spm[0]=0x2460;Infra[0xc14/4]=0x76543210;
    ULONG ScoWrites=SocHardwareWrites;
    CHECK(Interface->Read(Device,PriorToken,MtkConnSpm,0,&Value)==STATUS_SUCCESS && Value==0x2460);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0,0x0b162460)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0,0x0b162441)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0,0x88162461)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0xc14,1)==STATUS_ACCESS_DENIED);
    CHECK(SocHardwareWrites==ScoWrites);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0,0x0b162461)==STATUS_SUCCESS && Spm[0]==0x2461);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0,0x0b162461)==STATUS_SUCCESS && SocHardwareWrites==ScoWrites+1);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0xc14,0)==STATUS_SUCCESS && !Infra[0xc14/4]);

    MTK_CONN_POWER Power;
    MTK_CONN_POWER_IO Io={&Client,TestConnRead,TestConnWrite,TestConnWait,TestConnBarrier};
    CHECK(MtkConnPowerInit(&Power,&Io)==MtkConnPowerOk);
    CHECK(MtkConnPowerStart(&Power,EmiBase,MTK_CONN_EMI_SIZE)==MtkConnPowerOk && MtkConnPowerCheck(&Power)==MtkConnPowerOk);
    CHECK(Spm[0x304/4]==0x8800000d && Spm[0x16c/4]==0x00200002 && Spm[0x170/4]==0x00400002);
    CHECK(Infra[0x0c8/4]==0x01000000 && Infra[0x220/4]==0x80000000 && Infra[0x250/4]==0x40000000);
    CHECK(Infra[0x380/4]==0xa4505100 && Infra[0x388/4]==0x33201000);
    ULONG GateWrites=SocHardwareWrites;
    CHECK(Interface->Read(Device,PriorToken,MtkConnInfra,0x90,&Value)==STATUS_SUCCESS && Value==0x80000000);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80001000)==STATUS_ACCESS_DENIED);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0xd4,0x80000000)==STATUS_ACCESS_DENIED);
    Infra[0xd8/4]=0xffffffff;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_DEVICE_HARDWARE_ERROR);
    Infra[0xd8/4]=0x80000000;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_DEVICE_NOT_READY);
    Infra[0xd8/4]=0;
    Infra[0x90/4]|=1u<<12;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_DEVICE_NOT_READY);
    Infra[0x90/4]&=~(1u<<12);
    ULONG *Top=SocMappedRegisters[3],*Pll=SocMappedRegisters[4];
    Top[0x10/4]=6;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_DEVICE_NOT_READY);
    Top[0x10/4]=3;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_DEVICE_NOT_READY &&
          SocHardwareWrites==GateWrites && Infra[0x90/4]==0x80200000);
    Pll[0x340/4]=0xff800001;Pll[0x344/4]=0x160000;Pll[0x34c/4]=1;
    SocBtifGateStuck=TRUE;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_DEVICE_HARDWARE_ERROR);
    SocBtifGateStuck=FALSE;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_SUCCESS && Infra[0x90/4]==0x00200000);
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x84,0x80000000)==STATUS_SUCCESS && SocHardwareWrites==GateWrites+2);
    CHECK(Interface->Release(Device,PriorToken)==STATUS_DEVICE_BUSY && HostObjectRefs==2);
    CHECK(RootPnp(IRP_MN_QUERY_STOP_DEVICE,FALSE,STATUS_SUCCESS)==STATUS_DEVICE_BUSY && ConnLinkLive);
    CHECK(RootPower(PowerDeviceD3,FALSE,STATUS_SUCCESS)==STATUS_DEVICE_BUSY && !HostStore.Soc.Suspended);
    CHECK(MtkConnPowerStop(&Power)==MtkConnPowerOk && Spm[0x304/4]==0x88000012 && Infra[0x0c8/4]==0x11000000);
    CHECK(Infra[0x380/4]==0xa4509999 && Infra[0x388/4]==0x33211111);
    SocBtifGateStuck=TRUE;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x80,0x80000000)==STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(Interface->Release(Device,PriorToken)==STATUS_DEVICE_BUSY && HostObjectRefs==2);
    SocBtifGateStuck=FALSE;
    CHECK(Interface->Write(Device,PriorToken,MtkConnInfra,0x80,0x80000000)==STATUS_SUCCESS &&
          Infra[0x90/4]==0x80200000 && !Infra[0xd8/4]);
    Infra[0x228/4]=MAXULONG;
    CHECK(Interface->Release(Device,PriorToken)==STATUS_DEVICE_HARDWARE_ERROR && HostObjectRefs==2);
    Infra[0x228/4]=0x80046000;
    CHECK(Interface->Release(Device,PriorToken)==STATUS_SUCCESS && HostObjectRefs==1 && Spm[0]==0x2461);
    CHECK(Interface->Release(Device,PriorToken)==STATUS_ACCESS_DENIED && HostObjectRefs==1);
    CHECK(RootPower(PowerDeviceD3,TRUE,STATUS_SUCCESS)==STATUS_SUCCESS && HostStore.Soc.Suspended);
    CHECK(Interface->Acquire(Device,EmiBase,&Additional)==STATUS_DEVICE_NOT_READY);
    CHECK(RootPower(PowerDeviceD0,TRUE,STATUS_SUCCESS)==STATUS_SUCCESS && !HostStore.Soc.Suspended);

    Infra[0x90/4]=0x00200000;
    Top[0x10/4]=0;
    CHECK(Interface->Acquire(Device,EmiBase,&Client.Token)==STATUS_SUCCESS && Client.Token!=PriorToken);
    GateWrites=SocHardwareWrites;
    CHECK(Interface->Write(Device,Client.Token,MtkConnInfra,0x84,0x80000000)==STATUS_SUCCESS &&
          SocHardwareWrites==GateWrites);
    CHECK(Interface->Write(Device,Client.Token,MtkConnInfra,0x80,0x80000000)==STATUS_ACCESS_DENIED &&
          Infra[0x90/4]==0x00200000);
    CHECK(Interface->Write(Device,PriorToken,MtkConnSpm,0x304,0x16)==STATUS_ACCESS_DENIED);
    CHECK(MtkConnPowerStart(&Power,EmiBase,MTK_CONN_EMI_SIZE)==MtkConnPowerOk);
    Reads=SocHardwareReads;
    ULONG Writes=SocHardwareWrites;
    CHECK(RootPnp(IRP_MN_SURPRISE_REMOVAL,TRUE,STATUS_SUCCESS)==STATUS_SUCCESS && !ConnLinkLive && ConnDisables==1);
    CHECK(HostObjectRefs==1 && !HostStore.Soc.ConnToken && SocMapMask==SOC_MAP_ALL);
    CHECK(Interface->Read(Device,Client.Token,MtkConnSpm,0x304,&Value)==STATUS_DEVICE_REMOVED);
    CHECK(Interface->Write(Device,Client.Token,MtkConnSpm,0x304,0x12)==STATUS_DEVICE_REMOVED);
    CHECK(Interface->Release(Device,Client.Token)==STATUS_DEVICE_REMOVED);
    CHECK(RootConnQuery(Guid,1,Bytes,&Client.Interface,FALSE)==STATUS_DEVICE_REMOVED && HostObjectRefs==1);
    CHECK(SocHardwareReads==Reads && SocHardwareWrites==Writes);
    CHECK(RootPnp(IRP_MN_REMOVE_DEVICE,TRUE,STATUS_SUCCESS)==STATUS_SUCCESS && ConnDisables==1 && WorkQueued);
    RunHostWork();CHECK(!Deleted && SocMapMask==SOC_MAP_ALL);
    Interface->Header.InterfaceDereference(Device);
    RunHostWork();CHECK(Deleted && !SocMapMask && !HostObjectRefs && ConnFrees==1);
}
static void RootHostLifecycle(void) {
    for(ULONG Pending=0;Pending<2;Pending++) {
        InitOwner(0,0);IdKind=6;
        CHECK(MtkMaliBoardHostDriverEntry(&HostDriver,0)==STATUS_SUCCESS);
        CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)==STATUS_SUCCESS && HostStore.Role && HostStore.SocInitialized);
        CHECK(!ParentQueries && !ParentRefs && Attached && (HostDevice.Flags&DO_POWER_PAGABLE));
        CHECK(RootPnp(IRP_MN_START_DEVICE,(BOOLEAN)Pending,STATUS_UNSUCCESSFUL)==STATUS_UNSUCCESSFUL && !SocMapCalls && !BhSocOwner);
        CHECK(RootPnp(IRP_MN_START_DEVICE,(BOOLEAN)Pending,STATUS_TIMEOUT)==STATUS_UNSUCCESSFUL && !SocMapCalls && !BhSocOwner);
        for(ULONG Cycle=0;Cycle<3;Cycle++) {
            CHECK(RootPnp(IRP_MN_START_DEVICE,(BOOLEAN)Pending,STATUS_SUCCESS)==STATUS_SUCCESS && BhSocOwner==&HostStore && SocMapMask==SOC_MAP_ALL);
            CHECK(RootPower(PowerDeviceD3,(BOOLEAN)Pending,STATUS_SUCCESS)==STATUS_SUCCESS && HostStore.Soc.Suspended && LockCount==1);
            CHECK(RootPower(PowerDeviceD0,(BOOLEAN)Pending,STATUS_UNSUCCESSFUL)==STATUS_UNSUCCESSFUL && HostStore.Soc.Suspended);
            CHECK(RootPower(PowerDeviceD0,(BOOLEAN)Pending,STATUS_TIMEOUT)==STATUS_UNSUCCESSFUL && HostStore.Soc.Suspended);
            CHECK(RootPower(PowerDeviceD0,(BOOLEAN)Pending,STATUS_SUCCESS)==STATUS_SUCCESS && !HostStore.Soc.Suspended);
            CHECK(RootPower(PowerDeviceD3,(BOOLEAN)Pending,STATUS_UNSUCCESSFUL)==STATUS_UNSUCCESSFUL && !HostStore.Soc.Suspended);
            CHECK(RootPower(PowerDeviceD2,FALSE,STATUS_SUCCESS)==STATUS_NOT_SUPPORTED && LockCount==1);
            CHECK(RootPnp(IRP_MN_QUERY_STOP_DEVICE,(BOOLEAN)Pending,STATUS_SUCCESS)==STATUS_SUCCESS && HostStore.Soc.Stopping==IRP_MN_QUERY_STOP_DEVICE);
            CHECK(RootPnp(IRP_MN_CANCEL_REMOVE_DEVICE,FALSE,STATUS_SUCCESS)==STATUS_SUCCESS && HostStore.Soc.Stopping);
            CHECK(RootPnp(IRP_MN_CANCEL_STOP_DEVICE,TRUE,STATUS_UNSUCCESSFUL)==STATUS_UNSUCCESSFUL && HostStore.Soc.Stopping);
            CHECK(RootPnp(IRP_MN_CANCEL_STOP_DEVICE,TRUE,STATUS_SUCCESS)==STATUS_SUCCESS && !HostStore.Soc.Stopping);
            CHECK(RootPnp(IRP_MN_STOP_DEVICE,(BOOLEAN)Pending,STATUS_SUCCESS)==STATUS_SUCCESS && !BhSocOwner && !SocMapMask);
        }
        CHECK(RootPnp(IRP_MN_START_DEVICE,FALSE,STATUS_SUCCESS)==STATUS_SUCCESS);IdKind=0;
        MALI_BOARD_HOST Client={0};Client.Pdo=&PnpPdo;CHECK(BhSocLookup(&Client)==STATUS_SUCCESS && HostObjectRefs==1 && PdoObjectRefs==2);
        IdKind=6;CHECK(RootPnp(IRP_MN_QUERY_REMOVE_DEVICE,FALSE,STATUS_SUCCESS)==STATUS_DEVICE_BUSY && !HostStore.Soc.Stopping);
        CHECK(RootPnp(IRP_MN_REMOVE_DEVICE,TRUE,STATUS_SUCCESS)==STATUS_SUCCESS && !BhSocOwner && Detached && WorkQueued);
        RunHostWork();CHECK(!Deleted && HostStore.Quarantined && SocMapMask==SOC_MAP_ALL);
        Client.Parent.Header.InterfaceDereference(Client.Parent.Header.Context);CHECK(WorkQueued && !HostObjectRefs && PdoObjectRefs==1);
        RunHostWork();CHECK(Deleted && !SocMapMask && !PdoObjectRefs && !WorkObjectRefs && WorkFreed==1);
    }
}
int TestMain(void) {
    ResourceMapping();if(Failure)return (int)Failure;
    MandatoryQualification();if(Failure)return (int)Failure;
    CalibratedQualification();if(Failure)return (int)Failure;
    ControllerThroughOwner();if(Failure)return (int)Failure;
    CacheAndFaultLifetime();if(Failure)return (int)Failure;
    OwnerAdmissionAndFailure();if(Failure)return (int)Failure;
    RootHostLifecycle();if(Failure)return (int)Failure;
    ConnInterfaceLifecycle();if(Failure)return (int)Failure;
    DeferredOwnerStart();return (int)Failure;
}
