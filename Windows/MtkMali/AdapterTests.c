/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual query DDIs plus VM/allocation/scheduler integration; OS and GPU are fixtures. */
#define MALI_VM_TEST_COUNTER MtkMaliAdapterAssertionCount
#include "TestVm.h"
static __declspec(align(16)) uint8_t Output[2048];
static void Fill(void *P,size_t N,uint8_t V) {uint8_t *B=P;for(size_t I=0;I<N;I++)B[I]=V;}
static void SameByte(const void *P,size_t N,uint8_t V) {const uint8_t *B=P;for(size_t I=0;I<N;I++)CHECK(B[I]==V);}
static NTSTATUS Query(DXGK_QUERYADAPTERINFOTYPE Type,const void *In,UINT InBytes,void *Out,UINT OutBytes) {
    DXGKARG_QUERYADAPTERINFO A={0};A.Type=Type;A.pInputData=(void*)In;A.InputDataSize=InBytes;A.pOutputData=Out;A.OutputDataSize=OutBytes;
    return MtkMaliDdiQueryAdapterInfo(&Adapter,&A);
}
static void Configure(void) {CHECK(MtkMaliDdiConfigureAdapter(&Adapter,64u*1024u*1024u)==STATUS_SUCCESS);}
static void ConfigurationAndCaps(void) {
    Begin();Fill(Output,sizeof(Output),0xa5);
    CHECK(Query(DXGKQAITYPE_DRIVERCAPS,0,0,Output,sizeof(Output))==STATUS_DEVICE_NOT_READY);
    SameByte(Output,sizeof(Output),0xa5);
    Adapter.Allocations.ShareBackingEnabled=0;
    CHECK(MtkMaliDdiConfigureAdapter(&Adapter,4096)==STATUS_NOT_SUPPORTED);End();
    BeginWith(MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING,8192);
    CHECK(MtkMaliDdiConfigureAdapter(&Adapter,0)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiConfigureAdapter(&Adapter,4097)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiConfigureAdapter(&Adapter,(SIZE_T)MALI_ALLOCATION_MAX_BYTES+4096)==STATUS_INVALID_PARAMETER);
    MALI_VM *Saved=Adapter.Vm;Adapter.Vm=0;
    CHECK(MtkMaliDdiConfigureAdapter(&Adapter,4096)==STATUS_NOT_SUPPORTED);Adapter.Vm=Saved;
    Configure();Configure();CHECK(MtkMaliDdiConfigureAdapter(&Adapter,4096)==STATUS_DEVICE_BUSY);
    DXGK_DRIVERCAPS C;Fill(&C,sizeof(C),0xa5);
    CHECK(Query(DXGKQAITYPE_DRIVERCAPS,0,0,&C,sizeof(C))==STATUS_SUCCESS);
    CHECK(C.HighestAcceptableAddress.QuadPart==0xffffffffffLL && C.ApertureSegmentCommitLimit==64u*1024u*1024u);
    CHECK(!C.WDDMVersion && C.GpuEngineTopology.NbAsymetricProcessingNodes==1);
    CHECK(C.SchedulingCaps.MultiEngineAware && C.SchedulingCaps.PreemptionAware && C.SchedulingCaps.NoDmaPatching && C.SchedulingCaps.No64BitAtomics);
    CHECK(C.MemoryManagementCaps.VirtualAddressingSupported && C.MemoryManagementCaps.GpuMmuSupported);
    CHECK(!C.MemoryManagementCaps.IoMmuSupported && C.PresentationCaps.NoCacheCoherentApertureMemory);
    CHECK(C.SupportPerEngineTDR && !C.SupportRuntimePowerManagement && !C.SupportNonVGA && !C.SupportSurpriseRemoval);
    CHECK(C.PreemptionCaps.GraphicsPreemptionGranularity==D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY);
    CHECK(C.PreemptionCaps.ComputePreemptionGranularity==D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY);
    CHECK(C.InternalGpuVirtualAddressRangeStart==MALI_JM_VA_LIMIT-MALI_COMMAND_SHADOW_BYTES && C.InternalGpuVirtualAddressRangeEnd==0xffffffffffffULL);
    Fill(Output,sizeof(Output),0xa5);
    for(UINT I=0;I<sizeof(C);I++)CHECK(Query(DXGKQAITYPE_DRIVERCAPS,0,0,Output,I)==STATUS_BUFFER_TOO_SMALL);
    SameByte(Output,sizeof(Output),0xa5);
    UINT In=0;CHECK(Query(DXGKQAITYPE_DRIVERCAPS,&In,sizeof(In),Output,sizeof(Output))==STATUS_INVALID_PARAMETER);
    CHECK(Query(DXGKQAITYPE_DRIVERCAPS,0,0,0,sizeof(C))==STATUS_INVALID_PARAMETER);
    CHECK(Query(DXGKQAITYPE_DRIVERCAPS,0,0,(void*)(UINTPTR_MAX-10),sizeof(C))==STATUS_INVALID_PARAMETER);
    CHECK(Query((DXGK_QUERYADAPTERINFOTYPE)0x7fffffff,0,0,Output,sizeof(Output))==STATUS_NOT_SUPPORTED);
    DXGKARG_QUERYADAPTERINFO A={0};A.Type=DXGKQAITYPE_DRIVERCAPS;A.pOutputData=Output;A.OutputDataSize=sizeof(Output);
    A.Flags.Value=1;CHECK(MtkMaliDdiQueryAdapterInfo(&Adapter,&A)==STATUS_INVALID_PARAMETER);A.Flags.Value=0;
    A.hKmdProcessHandle=(HANDLE)7;CHECK(MtkMaliDdiQueryAdapterInfo(&Adapter,&A)==STATUS_INVALID_HANDLE);
    A.hKmdProcessHandle=Proc[0];CHECK(MtkMaliDdiQueryAdapterInfo(&Adapter,&A)==STATUS_SUCCESS);
    Irql=DISPATCH_LEVEL;CHECK(MtkMaliDdiQueryAdapterInfo(&Adapter,&A)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiConfigureAdapter(&Adapter,4096)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    End();
}
static void MmuAndSegments(void) {
    BeginWith(3,8192);Configure();DXGK_QUERYGPUMMUCAPSIN I={0};DXGK_GPUMMUCAPS C;
    CHECK(Query(DXGKQAITYPE_GPUMMUCAPS,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS);
    CHECK(C.PageTableUpdateMode==DXGK_PAGETABLEUPDATE_CPU_VIRTUAL && C.PageTableLevelCount==4 && C.VirtualAddressBitCount==48);
    CHECK(C.ExplicitPageTableInvalidation && C.PageTableUpdateRequireAddressSpaceIdle && C.ReadOnlyMemorySupported && C.NoExecuteMemorySupported);
    CHECK(!C.LargePageSupported && !C.CacheCoherentMemorySupported && !C.SysMem64KBPageSupported);
    I.PhysicalAdapterIndex=1;CHECK(Query(DXGKQAITYPE_GPUMMUCAPS,&I,sizeof(I),&C,sizeof(C))==STATUS_NOT_SUPPORTED);
    I.PhysicalAdapterIndex=0;CHECK(Query(DXGKQAITYPE_GPUMMUCAPS,&I,sizeof(I)-1,&C,sizeof(C))==STATUS_INVALID_PARAMETER);
    CHECK(Query(DXGKQAITYPE_GPUMMUCAPS,0,sizeof(I),&C,sizeof(C))==STATUS_INVALID_PARAMETER);
    DXGK_QUERYPAGETABLELEVELDESCIN L={0};DXGK_PAGE_TABLE_LEVEL_DESC D;
    for(UINT N=0;N<4;N++) {
        L.LevelIndex=(WORD)N;CHECK(Query(DXGKQAITYPE_PAGETABLELEVELDESC,&L,sizeof(L),&D,sizeof(D))==STATUS_SUCCESS);
        CHECK(D.PageTableIndexBitCount==9 && !D.PageTableSegmentId && !D.PagingProcessPageTableSegmentId);
        CHECK(D.PageTableSizeInBytes==4096);
    }
    L.LevelIndex=4;CHECK(Query(DXGKQAITYPE_PAGETABLELEVELDESC,&L,sizeof(L),&D,sizeof(D))==STATUS_INVALID_PARAMETER);
    L.LevelIndex=0;L.PhysicalAdapterIndex=1;CHECK(Query(DXGKQAITYPE_PAGETABLELEVELDESC,&L,sizeof(L),&D,sizeof(D))==STATUS_NOT_SUPPORTED);
    DXGK_QUERYSEGMENTIN4 S={0};DXGK_QUERYSEGMENTOUT4 O;Fill(&O,sizeof(O),0xa5);O.NbSegment=0;
    CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_SUCCESS && O.NbSegment==1);
    /* Count-only phase must neither read the poisoned pointer nor overwrite other members. */
    SameByte((uint8_t*)&O+sizeof(O.NbSegment),sizeof(O)-sizeof(O.NbSegment),0xa5);
    Fill(Output,sizeof(Output),0x5a);RtlZeroMemory(&O,sizeof(O));O.NbSegment=1;
    O.pSegmentDescriptor=Output;O.SegmentDescriptorStride=sizeof(DXGK_SEGMENTDESCRIPTOR4)+32;
    O.PagingBufferSegmentId=O.PagingBufferSize=O.PagingBufferPrivateDataSize=123;
    CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_SUCCESS);
    DXGK_SEGMENTDESCRIPTOR4 *Seg=(void*)Output;
    CHECK(Seg->Flags.Value==1 && Seg->Size==64u*1024u*1024u && Seg->CommitLimit==Seg->Size);
    CHECK(!Seg->BaseAddress.QuadPart && !Seg->CpuTranslatedAddress.QuadPart);
    CHECK(!O.PagingBufferSegmentId && !O.PagingBufferSize && !O.PagingBufferPrivateDataSize);
    SameByte(Output+sizeof(*Seg),sizeof(Output)-sizeof(*Seg),0x5a);
    O.NbSegment=2;CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_INVALID_PARAMETER);O.NbSegment=1;
    O.SegmentDescriptorStride=sizeof(*Seg)-1;CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_INVALID_PARAMETER);
    O.SegmentDescriptorStride=sizeof(*Seg);O.pSegmentDescriptor=(void*)((uint8_t*)&O+8);
    CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_INVALID_PARAMETER);
    O.pSegmentDescriptor=(void*)(UINTPTR_MAX-16);CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_INVALID_PARAMETER);
    O.pSegmentDescriptor=(void*)(Output+1);CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_INVALID_PARAMETER);
    O.pSegmentDescriptor=0;CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_INVALID_PARAMETER);
    S.PhysicalAdapterIndex=1;CHECK(Query(DXGKQAITYPE_QUERYSEGMENT4,&S,sizeof(S),&O,sizeof(O))==STATUS_NOT_SUPPORTED);
    End();
}
static void PrivateProtocolAndMetadata(void) {
    BeginWith(3,8192);Configure();MALI_ADAPTER_QUERY I={1,sizeof(I),0,0};MALI_ADAPTER_CAPS C;
    Fill(&C,sizeof(C),0xa5);CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS);
    CHECK(C.Version==1 && C.Bytes==64 && C.GpuId==0x90930002 && C.Features==7);
    CHECK(C.VaBits==48 && C.PaBits==40 && C.PageBytes==4096 && C.AllocationVersion==1 && C.CommandVersion==2);
    CHECK(C.MaxDmaBytes==65536 && C.MaxJobs==64 && C.MaxStages==16 && C.MaxAllocationBytes==1073741824);
    CHECK(C.RequiredAllocationFlags==3 && !C.Reserved[0] && !C.Reserved[1]);
    uint32_t Features=Adapter.PowerCaps.JobFeatures[1];Adapter.PowerCaps.JobFeatures[1]=0;
    CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS && !C.Features);
    Adapter.PowerCaps.JobFeatures[1]=4;CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS && C.Features==3);
    Adapter.PowerCaps.JobFeatures[1]=16;CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS &&
        C.Features==MALI_ADAPTER_RESOURCE_COMPUTE && C.CommandVersion==2);
    Adapter.PowerCaps.JobFeatures[1]=1u<<9;
    CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS &&
        C.Features==MALI_ADAPTER_RESOURCE_FRAGMENT && C.CommandVersion==2);
    const UINT JobBits[]={7,11,12},FeatureBits[]={MALI_ADAPTER_RESOURCE_TILER,MALI_ADAPTER_RESOURCE_MALLOC_VERTEX,MALI_ADAPTER_RESOURCE_FULLSCREEN};
    for(UINT J=0;J<3;J++) {
        Adapter.PowerCaps.JobFeatures[1]=1u<<JobBits[J];
        CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS &&
            C.Features==FeatureBits[J] && C.CommandVersion==2 && C.Bytes==64);
    }
    Adapter.PowerCaps.JobSlots&=~2u;
    CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_SUCCESS && !C.Features);
    Adapter.PowerCaps.JobSlots|=2u;Adapter.PowerCaps.JobFeatures[1]=Features;
    Fill(&C,sizeof(C),0xa5);I.Version=2;CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_GRAPHICS_DRIVER_MISMATCH);
    SameByte(&C,sizeof(C),0xa5);I.Version=1;I.Bytes=0;
    CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_GRAPHICS_DRIVER_MISMATCH);I.Bytes=sizeof(I);I.Flags=1;
    CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_GRAPHICS_DRIVER_MISMATCH);I.Flags=0;I.Reserved=1;
    CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I),&C,sizeof(C))==STATUS_GRAPHICS_DRIVER_MISMATCH);I.Reserved=0;
    CHECK(Query(DXGKQAITYPE_UMDRIVERPRIVATE,&I,sizeof(I)+1,&C,sizeof(C))==STATUS_GRAPHICS_DRIVER_MISMATCH);
    UINT Power=123;CHECK(Query(DXGKQAITYPE_NUMPOWERCOMPONENTS,0,0,&Power,sizeof(Power))==STATUS_SUCCESS && !Power);
    DXGK_QUERYHISTORYBUFFERPRECISIONIN H={0};DXGKARG_HISTORYBUFFERPRECISION Precision;
    Fill(&Precision,sizeof(Precision),0xa5);CHECK(Query(DXGKQAITYPE_HISTORYBUFFERPRECISION,&H,sizeof(H),&Precision,sizeof(Precision))==STATUS_SUCCESS);
    SameByte(&Precision,sizeof(Precision),0);H.PhysicalAdapterIndex=1;
    CHECK(Query(DXGKQAITYPE_HISTORYBUFFERPRECISION,&H,sizeof(H),&Precision,sizeof(Precision))==STATUS_NOT_SUPPORTED);
    DXGKARG_GETNODEMETADATA N;Fill(&N,sizeof(N),0xa5);
    CHECK(MtkMaliDdiGetNodeMetadata(&Adapter,0,&N)==STATUS_SUCCESS);
    CHECK(N.EngineType==DXGK_ENGINE_TYPE_OTHER && N.GpuMmuSupported && !N.IoMmuSupported && !N.Flags.Value);
    CHECK(N.FriendlyName[0]=='T' && !N.FriendlyName[19] && !N.FriendlyName[31]);
    CHECK(MtkMaliDdiGetNodeMetadata(&Adapter,1,&N)==STATUS_NOT_SUPPORTED);
    CHECK(MtkMaliDdiGetNodeMetadata(&Adapter,0x10000,&N)==STATUS_NOT_SUPPORTED);
    CHECK(MtkMaliDdiGetNodeMetadata(&Adapter,0,0)==STATUS_INVALID_PARAMETER);
    End();
}
static void FeatureInterfaceLifetime(void) {
    BeginWith(3,8192);DXGKDDI_FEATURE_INTERFACE I;Fill(&I,sizeof(I),0xa5);
    QUERY_INTERFACE Q={0};Q.InterfaceType=&MaliFeatureGuid;Q.Size=sizeof(I);Q.Version=1;Q.Interface=(PINTERFACE)&I;Q.DeviceUid=DISPLAY_ADAPTER_HW_ID;
    Q.Size=sizeof(I)-1;CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_BUFFER_TOO_SMALL);SameByte(&I,sizeof(I),0xa5);Q.Size=sizeof(I);
    GUID Bad={0};Q.InterfaceType=&Bad;CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_NOT_SUPPORTED);Q.InterfaceType=&MaliFeatureGuid;
    Q.Version=2;CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_NOT_SUPPORTED);Q.Version=1;
    Q.DeviceUid=0;CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_NOT_SUPPORTED);Q.DeviceUid=DISPLAY_ADAPTER_HW_ID;
    Q.InterfaceSpecificData=&Q;CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_NOT_SUPPORTED);Q.InterfaceSpecificData=0;
    Q.Interface=0;CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_INVALID_PARAMETER);Q.Interface=(PINTERFACE)&I;
    CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_SUCCESS && Adapter.InterfaceReferences==1);
    CHECK(I.Size==sizeof(I) && I.Version==1 && I.Context==&Adapter && I.QueryFeatureSupport && !I.QueryFeatureInterface);
    Irql=DISPATCH_LEVEL;I.InterfaceReference(I.Context);CHECK(Adapter.InterfaceReferences==2);I.InterfaceDereference(I.Context);CHECK(Adapter.InterfaceReferences==1);
    CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    DXGKARG_QUERYFEATURESUPPORT F={0};F.FeatureId=DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD;
    CHECK(I.QueryFeatureSupport(I.Context,&F)==STATUS_SUCCESS && F.SupportedByDriver && F.SupportedOnCurrentConfig && F.MinSupportedVersion==1 && F.MaxSupportedVersion==1);
    Adapter.InterfaceReferences=INT64_MAX;Fill(Output,sizeof(I),0xa5);Q.Interface=(PINTERFACE)Output;
    CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_INSUFFICIENT_RESOURCES && Adapter.InterfaceReferences==INT64_MAX);
    SameByte(Output,sizeof(I),0xa5);Adapter.InterfaceReferences=1;
    /* Remove every other live object: the borrowed interface alone blocks rundown. */
    DXGKARG_CLOSEALLOCATION Close={1,&OpenHandle};CHECK(MtkMaliDdiCloseAllocation(Dev[0],&Close)==STATUS_SUCCESS);
    DXGKARG_DESTROYALLOCATION A={0};A.NumAllocations=1;A.pAllocationList=&AllocationHandle;A.hResource=ResourceHandle;A.Flags.DestroyResource=1;
    CHECK(MtkMaliDdiDestroyAllocation(&Adapter,&A)==STATUS_SUCCESS);
    for(unsigned N=0;N<2;N++){CHECK(MtkMaliDdiDestroyDevice(Dev[N])==STATUS_SUCCESS);CHECK(MtkMaliDdiDestroyProcess(&Adapter,Proc[N])==STATUS_SUCCESS);}
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_DEVICE_BUSY && CanSchedule(&Adapter));
    Irql=DISPATCH_LEVEL;I.InterfaceDereference(I.Context);CHECK(!Adapter.InterfaceReferences);I.InterfaceDereference(I.Context);CHECK(!Adapter.InterfaceReferences);
    I.InterfaceReference(I.Context);CHECK(!Adapter.InterfaceReferences);Irql=PASSIVE_LEVEL;
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_SUCCESS && MtkMaliVmShutdown(&Vm)==STATUS_SUCCESS);
    Q.Interface=(PINTERFACE)&I;CHECK(MtkMaliDdiQueryInterface(&Adapter,&Q)==STATUS_DEVICE_NOT_READY);
}
static NTSTATUS TwoSteps(void *X,HANDLE P,uint64_t Id,uint64_t Root,const DXGKARG_SUBMITCOMMANDVIRTUAL *A,MALI_DDI_PREPARED *Out) {
    NTSTATUS S=VmFakePrepare(X,P,Id,Root,A,Out);Out->Packet.StepCount=2;Out->Packet.Bytes=MALI_EXEC_PACKET_BYTES(2);
    Out->Packet.Steps[1].Chain=0x90000080;return S;
}
static void TwoStepHooks(MALI_DDI_HOOKS *H) {H->Prepare=TwoSteps;}
static UINT Responsive(void) {
    DXGKARG_QUERYENGINESTATUS E={0};E.EngineStatus.Value=0xffffffff;
    CHECK(MtkMaliDdiQueryEngineStatus(&Adapter,&E)==STATUS_SUCCESS && !(E.EngineStatus.Value&~1u));return E.EngineStatus.Responsive;
}
static void EngineProgress(void) {
    TestVmInitializeHooks=TwoStepHooks;BeginWith(3,8192);Configure();TestVmInitializeHooks=0;
    DXGKARG_QUERYDEPENDENTENGINEGROUP Group={0};Group.DependentNodeOrdinalMask=0xffff;
    CHECK(MtkMaliDdiQueryDependentEngineGroup(&Adapter,&Group)==STATUS_SUCCESS && Group.DependentNodeOrdinalMask==1);
    Group.NodeOrdinal=1;CHECK(MtkMaliDdiQueryDependentEngineGroup(&Adapter,&Group)==STATUS_NOT_SUPPORTED);
    DXGKARG_QUERYENGINESTATUS E={0};E.NodeOrdinal=1;E.EngineStatus.Value=123;
    CHECK(MtkMaliDdiQueryEngineStatus(&Adapter,&E)==STATUS_NOT_SUPPORTED && E.EngineStatus.Value==123);
    CHECK(Responsive()==1);DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(MtkMaliDdiCreateContext(Dev[0],&C)==STATUS_SUCCESS);Ctx=C.hContext;
    DXGKARG_SETROOTPAGETABLE R={0};R.hContext=Ctx;R.Address.SegmentOffset=OsPhysical[0];R.NumEntries=512;
    MtkMaliDdiSetRootPageTable(&Adapter,&R);CHECK(Adapter.LastRootStatus==STATUS_SUCCESS);
    DXGKARG_SUBMITCOMMANDVIRTUAL S={0};S.hContext=Ctx;S.DmaBufferVirtualAddress=0x80000000;S.DmaBufferSize=4096;S.SubmissionFenceId=1;
    CHECK(MtkMaliDdiSubmitCommandVirtual(&Adapter,&S)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiConfigureAdapter(&Adapter,64u*1024u*1024u)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Adapter.Queue.HardwareActive);
    CHECK(!Responsive());CHECK(!Responsive());Completion(&G,0,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && Adapter.Queue.Step==1 && Adapter.Queue.HardwareActive);
    CHECK(Responsive()==1);CHECK(!Responsive());Completion(&G,0,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && Released==1 && Responsive()==1);
    S.SubmissionFenceId=2;CHECK(MtkMaliDdiSubmitCommandVirtual(&Adapter,&S)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,4)==STATUS_SUCCESS);Completion(&G,0,0x58,1);
    CHECK(MtkMaliDdiPump(&Adapter,5)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && Adapter.Queue.Faulted && !Responsive());
    DXGKARG_RESETENGINE Reset={0};G.ResetStuck=1;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_IO_TIMEOUT && !Responsive());G.ResetStuck=0;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS && Responsive()==1);
    CHECK(MtkMaliDdiDestroyContext(Ctx)==STATUS_SUCCESS);End();
}
int TestMain(void) {
    ConfigurationAndCaps();MmuAndSegments();PrivateProtocolAndMetadata();FeatureInterfaceLifetime();EngineProgress();return (int)FirstFailure;
}
