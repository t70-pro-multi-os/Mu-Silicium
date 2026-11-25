/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual WDK allocation + lifecycle/scheduler DDIs. Fixtures are OS pool,
   handle resolution/feature queries/synchronization, command preparation and MMIO. */
#include "Ddi.h"
#include "Commands.h"
static unsigned FirstFailure;
unsigned MtkMaliAllocationAssertionCount;
#define CHECK(X) do {++MtkMaliAllocationAssertionCount;if(!(X) && !FirstFailure)FirstFailure=__LINE__;} while(0)
#include "TestGpu.h"
static KIRQL Irql;
static unsigned GateHeld;
static NTSTATUS AllocationWait(PVOID O,KWAIT_REASON W,KPROCESSOR_MODE M,BOOLEAN A,PLARGE_INTEGER T) {
    CHECK(O && W==Executive && M==KernelMode && !A && !T && !GateHeld && Irql==PASSIVE_LEVEL);
    GateHeld=1;return STATUS_SUCCESS;
}
static LONG AllocationUnlock(PKMUTEX M,BOOLEAN W) {CHECK(M && !W && GateHeld);GateHeld=0;return 0;}
static void AllocationMutex(PKMUTEX M,ULONG L) {CHECK(M && !L);}
#define KeGetCurrentIrql() Irql
#define KeWaitForSingleObject AllocationWait
#define KeReleaseMutex AllocationUnlock
#define KeInitializeMutex AllocationMutex
#include "TestPool.h"
#include "Allocation.c"
#include "Vm.c"
#include "Ddi.c"
static GPU G;
static MALI_JM Core;
static MALI_DDI_ADAPTER Adapter;
static DXGKRNL_INTERFACE K;
static HANDLE ProcessHandles[2],DeviceHandles[2],ContextHandle;
static unsigned FeatureEnable,FeatureBad,FeatureQueries,FeatureReleases,HandleQueries,SyncFail;
static NTSTATUS FeatureStatus;
static struct {UINT Id;HANDLE Allocation,Open;} OsHandles[8];
static unsigned HandleCount;
static struct {uint64_t Token;unsigned Live;} ExecutionLease[MALI_DDI_RESIDENTS];
static UINT ExecutionHandle;
static HANDLE ExecutionDevice,ExecutionProcess;
static unsigned Releases,Reasons[5];
static uint8_t __declspec(align(4096)) BackingData[16384];
static uint8_t CopyData[8192];
static MALI_ALLOCATION_DESC Specs[MALI_ALLOCATION_BATCH];
static DXGK_ALLOCATIONINFO Infos[MALI_ALLOCATION_BATCH],SavedInfos[MALI_ALLOCATION_BATCH];
static DXGK_OPENALLOCATIONINFO OpenInfos[MALI_ALLOCATION_BATCH],SavedOpenInfos[MALI_ALLOCATION_BATCH];
static HANDLE Handles[MALI_ALLOCATION_LIMIT],OpenHandles[MALI_ALLOCATION_LIMIT];
static uint64_t RefTokens[MALI_ALLOCATION_LIMIT];
static int Equal(const void *P,const void *Q,size_t N) {
    const volatile uint8_t *A=P,*B=Q;unsigned Different=0;
    while(N--)Different|=*A++^*B++;
    return !Different;
}
static VOID *APIENTRY HandleData(const DXGKARGCB_GETHANDLEDATA *H) {
    CHECK(GateHeld && Irql==PASSIVE_LEVEL && H && H->Type==DXGK_HANDLE_ALLOCATION && !(H->Flags.Value&~1u));
    ++HandleQueries;
    for(unsigned I=0;I<HandleCount;I++)if(OsHandles[I].Id==H->hObject)
        return H->Flags.DeviceSpecific?OsHandles[I].Open:OsHandles[I].Allocation;
    return 0;
}
static unsigned Expose(UINT Id,HANDLE AllocationHandle) {
    CHECK(HandleCount<8);unsigned I=HandleCount++;
    OsHandles[I].Id=Id;OsHandles[I].Allocation=AllocationHandle;OsHandles[I].Open=0;return I;
}
static void FeatureNegotiation(void) {
    DXGKARG_QUERYFEATURESUPPORT A={0};A.FeatureId=DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD;
    CHECK(MtkMaliDdiQueryFeatureSupport(&Adapter,&A)==STATUS_SUCCESS);
    CHECK(A.MinSupportedVersion==1 && A.MaxSupportedVersion==1 && A.SupportedByDriver && A.SupportedOnCurrentConfig);
}
static NTSTATUS APIENTRY LegacyFeature(DXGKARGCB_QUERYFEATURESUPPORT *A) {
    CHECK(!GateHeld && Irql==PASSIVE_LEVEL && A->DeviceHandle==(HANDLE)0x777 &&
        A->FeatureId==DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD && A->DriverSupportState==DXGK_FEATURE_SUPPORT_ALWAYS_ON);
    ++FeatureQueries;FeatureNegotiation();A->Enabled=(BOOLEAN)FeatureEnable;return FeatureStatus;
}
static NTSTATUS APIENTRY OldFeature(DXGKARGCB_ISFEATUREENABLED *A) {
    CHECK(!GateHeld && Irql==PASSIVE_LEVEL && A->DeviceHandle==(HANDLE)0x777 &&
        A->FeatureId==DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD);
    ++FeatureQueries;A->Enabled=(BOOLEAN)FeatureEnable;return FeatureStatus;
}
static void FeatureDeref(PVOID X) {CHECK(X==&Adapter && !GateHeld && Irql==PASSIVE_LEVEL);++FeatureReleases;}
static NTSTATUS APIENTRY Feature2(HANDLE H,DXGKARGCB_ISFEATUREENABLED2 *A) {
    CHECK(H==(HANDLE)0x777 && !GateHeld && Irql==PASSIVE_LEVEL &&
        A->FeatureId==DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD && !A->Flags.Value);
    ++FeatureQueries;FeatureNegotiation();A->Result.Version=FeatureBad==4?2:1;
    A->Result.Enabled=(UINT16)FeatureEnable;A->Result.KnownFeature=FeatureBad!=5;
    A->Result.SupportedByDriver=FeatureBad!=6;A->Result.SupportedOnCurrentConfig=FeatureBad!=7;
    return FeatureStatus;
}
static NTSTATUS APIENTRY Services(HANDLE H,DXGK_SERVICES Type,PINTERFACE Interface) {
    CHECK(!GateHeld && H==(HANDLE)0x777 && Type==DxgkServicesFeature && Interface->Size==sizeof(DXGK_FEATURE_INTERFACE) &&
        Interface->Version==DXGK_FEATURE_INTERFACE_VERSION_1);
    if(FeatureBad==1)return STATUS_NOT_SUPPORTED;
    DXGK_FEATURE_INTERFACE *F=(DXGK_FEATURE_INTERFACE*)Interface;
    F->Context=&Adapter;F->InterfaceDereference=FeatureDeref;F->IsFeatureEnabled=Feature2;
    if(FeatureBad==2)F->Size=sizeof(INTERFACE);
    if(FeatureBad==3)F->Version=2;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY AllocationSync(HANDLE H,PKSYNCHRONIZE_ROUTINE R,PVOID X,ULONG Msg,PBOOLEAN Return) {
    CHECK(H==(HANDLE)0x777 && !Msg && GateHeld && Irql==PASSIVE_LEVEL);
    if(SyncFail){*Return=FALSE;return STATUS_UNSUCCESSFUL;}
    Irql=5;*Return=R(X);Irql=PASSIVE_LEVEL;return STATUS_SUCCESS;
}
static VOID APIENTRY AllocationNotify(HANDLE H,DXGKARGCB_NOTIFY_INTERRUPT_DATA *A) {
    CHECK(H==(HANDLE)0x777 && A && GateHeld && Irql==5);
}
static BOOLEAN APIENTRY AllocationDpc(HANDLE H) {CHECK(H==(HANDLE)0x777 && Irql==5);return TRUE;}
static VOID APIENTRY AllocationNotifyDpc(HANDLE H) {CHECK(H==(HANDLE)0x777 && Irql==DISPATCH_LEVEL);}
static void AllocationWake(void *X) {CHECK(X==&Adapter && Irql<=DISPATCH_LEVEL);}
static void AllocationClean(void *X,const void *P,size_t N) {CHECK(X==&Adapter && P && N);}
static void AllocationBarrier(void *X) {CHECK(X==&Adapter);}
static NTSTATUS AllocationPrepare(void *X,HANDLE P,uint64_t Id,uint64_t Root,
    const DXGKARG_SUBMITCOMMANDVIRTUAL *A,MALI_DDI_PREPARED *Out) {
    CHECK(X==&Adapter && P==ExecutionProcess && Id && Root==0x40001000 && A && Out && GateHeld);
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(!ExecutionLease[I].Live) {
        uint64_t Token=0;
        NTSTATUS S=MtkMaliAllocationAcquire(&Adapter.Allocations,ExecutionDevice,P,ExecutionHandle,0,8,TRUE,&Token);
        if(S!=STATUS_SUCCESS)return S;
        ExecutionLease[I].Token=Token;ExecutionLease[I].Live=1;
        Out->Packet.Version=1;Out->Packet.Bytes=MALI_EXEC_PACKET_BYTES(1);Out->Packet.StepCount=1;
        Out->Packet.Steps[0].Chain=0x90000000;Out->ExecutionRoot=Root;Out->DmaVa=0x90000000;Out->DmaBytes=4096;
        Out->Cookie=&ExecutionLease[I];return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}
static void AllocationRelease(void *X,void *Cookie,MALI_DDI_RELEASE_REASON Why) {
    CHECK(X==&Adapter && GateHeld && Irql==PASSIVE_LEVEL && Why>=MaliDdiRejected && Why<=MaliDdiReset);
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(Cookie==&ExecutionLease[I]) {
        CHECK(ExecutionLease[I].Live);
        CHECK(MtkMaliAllocationRelease(&Adapter.Allocations,ExecutionLease[I].Token)==STATUS_SUCCESS);
        ExecutionLease[I].Live=0;++Releases;++Reasons[Why];return;
    }
    CHECK(0);
}
static void Configure(UINT Version) {
    RtlZeroMemory(&K,sizeof(K));K.Size=sizeof(K);K.Version=Version;K.DeviceHandle=(HANDLE)0x777;
    K.DxgkCbSynchronizeExecution=AllocationSync;K.DxgkCbNotifyInterrupt=AllocationNotify;
    K.DxgkCbQueueDpc=AllocationDpc;K.DxgkCbNotifyDpc=AllocationNotifyDpc;
    K.DxgkCbGetHandleData=HandleData;K.DxgkCbQueryFeatureSupport=LegacyFeature;
    K.DxgkCbIsFeatureEnabled=OldFeature;K.DxgkCbQueryServices=Services;
}
static void Initialize(void) {
    MALI_DDI_HOOKS H={&Adapter,AllocationPrepare,AllocationRelease,AllocationWake};
    MALI_WDDM_CACHE_OPS C={&Adapter,AllocationClean,AllocationBarrier};
    CHECK(MtkMaliDdiInitialize(&Adapter,&Core,100,&K,0,&H,&C)==MaliOk);
}
static void Begin(UINT Version) {
    CHECK(!TestPoolLive && !GateHeld);Irql=PASSIVE_LEVEL;HandleCount=0;
    TestPoolCalls=TestPoolFrees=TestPoolFail=TestPoolCursor=0;TestPoolMutation=0;
    FeatureEnable=1;FeatureBad=0;FeatureQueries=FeatureReleases=HandleQueries=0;FeatureStatus=STATUS_SUCCESS;
    SyncFail=Releases=0;RtlZeroMemory(Reasons,sizeof(Reasons));RtlZeroMemory(ExecutionLease,sizeof(ExecutionLease));
    Init(&G,&Core);CHECK(MaliJmStart(&Core)==MaliOk);Configure(Version);Initialize();
    for(unsigned I=0;I<2;I++) {
        DXGKARG_CREATEPROCESS P={0};P.hDxgkProcess=(HANDLE)(uintptr_t)(0x1234+I);
        CHECK(MtkMaliDdiCreateProcess(&Adapter,&P)==STATUS_SUCCESS);ProcessHandles[I]=P.hKmdProcess;
        DXGKARG_CREATEDEVICE V={0};V.hKmdProcess=P.hKmdProcess;
        CHECK(MtkMaliDdiCreateDevice(&Adapter,&V)==STATUS_SUCCESS);DeviceHandles[I]=V.hDevice;
    }
}
static void End(void) {
    CHECK(!MtkMaliAllocationHasObjects(&Adapter.Allocations) && !TestPoolLive && !GateHeld);
    for(unsigned I=0;I<2;I++) {
        if(DeviceHandles[I])CHECK(MtkMaliDdiDestroyDevice(DeviceHandles[I])==STATUS_SUCCESS);
        if(ProcessHandles[I])CHECK(MtkMaliDdiDestroyProcess(&Adapter,ProcessHandles[I])==STATUS_SUCCESS);
    }
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_SUCCESS && !GateHeld && TestPoolCalls>=TestPoolFrees);
}
static void Spec(unsigned I,uint64_t Bytes,unsigned Flags) {
    RtlZeroMemory(&Specs[I],sizeof(Specs[I]));Specs[I].Version=1;Specs[I].Bytes=sizeof(Specs[I]);
    Specs[I].Kind=MALI_ALLOCATION_BUFFER;Specs[I].Size=Bytes;Specs[I].Flags=Flags;
    RtlZeroMemory(&Infos[I],sizeof(Infos[I]));Infos[I].pPrivateDriverData=&Specs[I];Infos[I].PrivateDriverDataSize=sizeof(Specs[I]);
}
static DXGKARG_CREATEALLOCATION Creation(UINT Count,HANDLE Resource) {
    DXGKARG_CREATEALLOCATION A={0};A.NumAllocations=Count;A.pAllocationInfo=Infos;A.Flags.Resource=1;A.hResource=Resource;return A;
}
static NTSTATUS Destroy(UINT Count,const HANDLE *List,HANDLE Resource,UINT DestroyResource) {
    DXGKARG_DESTROYALLOCATION A={0};A.NumAllocations=Count;A.pAllocationList=List;A.hResource=Resource;
    A.Flags.DestroyResource=DestroyResource;return MtkMaliDdiDestroyAllocation(&Adapter,&A);
}
static HANDLE OpenOne(unsigned DeviceIndex,unsigned OsIndex,UINT ReadOnly) {
    DXGKARG_OPENALLOCATION A={0};A.NumAllocations=1;A.pOpenAllocation=OpenInfos;A.Flags.ReadOnly=ReadOnly;
    RtlZeroMemory(OpenInfos,sizeof(OpenInfos[0]));OpenInfos[0].hAllocation=OsHandles[OsIndex].Id;
    CHECK(MtkMaliDdiOpenAllocation(DeviceHandles[DeviceIndex],&A)==STATUS_SUCCESS);
    OsHandles[OsIndex].Open=OpenInfos[0].hDeviceSpecificAllocation;return OsHandles[OsIndex].Open;
}
static NTSTATUS Close(unsigned DeviceIndex,UINT Count,const HANDLE *List) {
    DXGKARG_CLOSEALLOCATION A={Count,List};return MtkMaliDdiCloseAllocation(DeviceHandles[DeviceIndex],&A);
}
static NTSTATUS Acquire(unsigned DeviceIndex,UINT Id,uint64_t Offset,uint64_t Bytes,BOOLEAN Write,uint64_t *Token) {
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    NTSTATUS S=MtkMaliAllocationAcquire(&Adapter.Allocations,DeviceHandles[DeviceIndex],
        (HANDLE)(uintptr_t)(0x1234+DeviceIndex),Id,Offset,Bytes,Write,Token);Leave(&Adapter);return S;
}
static NTSTATUS Drop(uint64_t Token) {
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);NTSTATUS S=MtkMaliAllocationRelease(&Adapter.Allocations,Token);Leave(&Adapter);return S;
}
static void SharingAndBacking(void) {
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);CHECK(FeatureQueries==1 && FeatureReleases==1 && Adapter.Allocations.ShareBackingEnabled);
    Spec(0,5000,3);Spec(1,1024,1);Specs[1].Kind=MALI_ALLOCATION_LINEAR_2D;
    Specs[1].Width=128;Specs[1].Height=2;Specs[1].Pitch=512;Specs[1].Format=D3DDDIFMT_A8R8G8B8;
    DXGKARG_CREATEALLOCATION A=Creation(2,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS && A.hResource);
    HANDLE H[2]={Infos[0].hAllocation,Infos[1].hAllocation};CHECK(H[0] && H[1] && H[0]!=H[1]);
    CHECK(Infos[0].Size==8192 && Infos[0].Alignment==4096 && Infos[1].Size==4096);
    CHECK(Infos[0].MmuSet==1 && Infos[0].PreferredSegment.SegmentId0==1 && Infos[0].SupportedWriteSegmentSet==1);
    CHECK(Infos[0].Flags2.ShareBackingStoreWithKmd && Infos[0].FlagsWddm2.CpuVisible && Infos[0].FlagsWddm2.Cached);
    CHECK(!Infos[0].FlagsWddm2.AccessedPhysically && !Infos[0].FlagsWddm2.ExistingSysMem && Infos[0].AllocationPriority);
    CHECK(Infos[0].pPrivateDriverData==&Specs[0] && Infos[0].PrivateDriverDataSize==40 && !Infos[1].Flags2.Value);
    DXGKARG_DESCRIBEALLOCATION D={0};D.hAllocation=H[1];CHECK(MtkMaliDdiDescribeAllocation(&Adapter,&D)==STATUS_SUCCESS);
    CHECK(D.Width==128 && D.Height==2 && D.Format==D3DDDIFMT_A8R8G8B8 && D.MultisampleMethod.NumSamples==1 && !D.MultisampleMethod.NumQualityLevels);
    CHECK(D.Rotation==D3DDDI_ROTATION_IDENTITY && !D.RefreshRate.Denominator && !D.RefreshRate.Numerator);
    D.Flags.CheckDisplayMode=1;CHECK(MtkMaliDdiDescribeAllocation(&Adapter,&D)==STATUS_NOT_SUPPORTED);
    D.Flags.Value=0;D.hAllocation=H[0];CHECK(MtkMaliDdiDescribeAllocation(&Adapter,&D)==STATUS_NOT_SUPPORTED);
    unsigned I=Expose(10,H[0]),J=Expose(11,H[0]);HANDLE O[2]={OpenOne(0,I,0),OpenOne(1,J,1)};
    CHECK(MtkMaliDdiDestroyDevice(DeviceHandles[0])==STATUS_DEVICE_BUSY);
    CHECK(Destroy(2,H,A.hResource,1)==STATUS_DEVICE_BUSY);
    uint64_t T=0,R=0;CHECK(Acquire(0,10,5,128,TRUE,&T)==STATUS_SUCCESS && T);
    CHECK(Acquire(1,11,0,8,TRUE,&R)==STATUS_ACCESS_DENIED && !R);
    CHECK(Acquire(0,11,0,8,FALSE,&R)==STATUS_ACCESS_DENIED && !R);
    CHECK(Acquire(0,10,5000,1,FALSE,&R)==STATUS_INVALID_PARAMETER);
    CHECK(Acquire(0,10,4999,2,FALSE,&R)==STATUS_INVALID_PARAMETER);
    CHECK(Acquire(0,10,UINT64_MAX,UINT64_MAX,FALSE,&R)==STATUS_INVALID_PARAMETER);
    CHECK(Acquire(0,10,0,0,FALSE,&R)==STATUS_INVALID_PARAMETER && !R);
    CHECK(Acquire(1,11,0,128,FALSE,&R)==STATUS_SUCCESS && R!=T);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);MALI_ALLOCATION_VIEW View={0};
    CHECK(MtkMaliAllocationReferenceView(&Adapter.Allocations,R,&View)==STATUS_SUCCESS);
    CHECK(View.Allocation==H[0] && View.Device==DeviceHandles[1] && View.Process==(HANDLE)0x1235 && View.ReadOnly && !View.Write);
    CHECK(View.AllocationBytes==5000 && View.Bytes==128 && !View.Offset && View.CpuVisible);
    CHECK(MtkMaliAllocationCopyBacking(&Adapter.Allocations,T,0,CopyData,128)==STATUS_DEVICE_NOT_READY);Leave(&Adapter);
    DXGKARG_SETALLOCATIONBACKINGSTORE B={H[0],BackingData};
    CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&B)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&B)==STATUS_SUCCESS);
    B.pBackingStore=BackingData+4096;CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&B)==STATUS_DEVICE_BUSY);
    B.pBackingStore=BackingData+1;CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&B)==STATUS_INVALID_PARAMETER);
    B.pBackingStore=0;CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&B)==STATUS_INVALID_PARAMETER);
    B.pBackingStore=BackingData;B.hDriverAllocation=H[1];CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&B)==STATUS_NOT_SUPPORTED);
    for(unsigned N=0;N<5000;N++)BackingData[N]=(uint8_t)(N*17+3);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliAllocationCopyBacking(&Adapter.Allocations,T,7,CopyData,121)==STATUS_SUCCESS && Equal(CopyData,BackingData+12,121));
    CHECK(MtkMaliAllocationCopyBacking(&Adapter.Allocations,T,7,CopyData,122)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliAllocationCopyBacking(&Adapter.Allocations,T,128,CopyData,1)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliAllocationCopyBacking(&Adapter.Allocations,T,0,BackingData+4096,8)==STATUS_ACCESS_DENIED);
    CHECK(MtkMaliAllocationCopyBacking(&Adapter.Allocations,T,0,(void*)(UINTPTR_MAX-1),8)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliAllocationAcquire(&Adapter.Allocations,DeviceHandles[0],(HANDLE)0x9999,10,0,8,FALSE,&T)==STATUS_ACCESS_DENIED);
    Leave(&Adapter);
    CHECK(Close(0,1,O)==STATUS_DEVICE_BUSY && Close(1,1,O+1)==STATUS_DEVICE_BUSY);
    CHECK(Drop(T)==STATUS_SUCCESS && Drop(T)==STATUS_INVALID_HANDLE);
    CHECK(Close(1,1,O)==STATUS_ACCESS_DENIED);HANDLE Duplicates[2]={O[0],O[0]};
    CHECK(Close(0,2,Duplicates)==STATUS_INVALID_PARAMETER && Adapter.Allocations.OpenCount==2);
    CHECK(Close(0,1,O)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyDevice(DeviceHandles[0])==STATUS_SUCCESS);DeviceHandles[0]=0;
    CHECK(MtkMaliDdiDestroyProcess(&Adapter,ProcessHandles[0])==STATUS_SUCCESS);ProcessHandles[0]=0;
    /* The adapter-owned shared allocation survives the creator's exit. */
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliAllocationCopyBacking(&Adapter.Allocations,R,0,CopyData,128)==STATUS_SUCCESS && Equal(CopyData,BackingData,128));Leave(&Adapter);
    CHECK(Drop(R)==STATUS_SUCCESS && Close(1,1,O+1)==STATUS_SUCCESS);
    CHECK(Destroy(1,H,A.hResource,1)==STATUS_INVALID_PARAMETER);
    HANDLE Dup[2]={H[0],H[0]};CHECK(Destroy(2,Dup,A.hResource,1)==STATUS_INVALID_PARAMETER);
    CHECK(Destroy(1,H,0,0)==STATUS_SUCCESS && Adapter.Allocations.ResourceCount==1);
    CHECK(Destroy(1,H+1,A.hResource,1)==STATUS_SUCCESS && !TestPoolLive);
    CHECK(MtkMaliDdiDescribeAllocation(&Adapter,&D)==STATUS_INVALID_HANDLE);End();
}
static void FeatureCases(void) {
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_1);CHECK(FeatureQueries==1 && !FeatureReleases && Adapter.Allocations.ShareBackingEnabled);End();
    for(unsigned Mode=0;Mode<11;Mode++) {
        Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);FeatureQueries=FeatureReleases=0;
        FeatureEnable=Mode!=0;FeatureBad=Mode>=1 && Mode<=7?Mode:0;
        FeatureStatus=Mode==8?STATUS_UNSUCCESSFUL:Mode==9?STATUS_TIMEOUT:STATUS_SUCCESS;
        if(Mode==10)K.DxgkCbQueryServices=0;
        MtkMaliAllocationInitialize(&Adapter.Allocations,&K);CHECK(!Adapter.Allocations.ShareBackingEnabled);
        CHECK(FeatureReleases==(Mode!=1 && Mode!=10));
        Spec(0,4096,3);DXGKARG_CREATEALLOCATION A=Creation(1,0);SavedInfos[0]=Infos[0];
        CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_NOT_SUPPORTED && !A.hResource);
        CHECK(Equal(Infos,SavedInfos,sizeof(Infos[0])) && !TestPoolLive);End();
    }
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_1);K.DxgkCbQueryFeatureSupport=0;FeatureQueries=0;
    MtkMaliAllocationInitialize(&Adapter.Allocations,&K);CHECK(FeatureQueries==1 && Adapter.Allocations.ShareBackingEnabled);End();
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_1);K.Size=FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbIsFeatureEnabled);
    FeatureQueries=0;MtkMaliAllocationInitialize(&Adapter.Allocations,&K);CHECK(!FeatureQueries && !Adapter.Allocations.ShareBackingEnabled);End();
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM2_0);CHECK(!FeatureQueries && !Adapter.Allocations.ShareBackingEnabled);End();
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);K.DxgkCbGetHandleData=0;MtkMaliAllocationInitialize(&Adapter.Allocations,&K);
    Spec(0,4096,0);DXGKARG_CREATEALLOCATION A=Creation(1,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_NOT_SUPPORTED);End();
}
static void AtomicFailures(void) {
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);
    for(unsigned FailAt=1;FailAt<=5;FailAt++) {
        for(unsigned I=0;I<4;I++)Spec(I,4096,0);
        RtlCopyMemory(SavedInfos,Infos,4*sizeof(*Infos));DXGKARG_CREATEALLOCATION A=Creation(4,0);
        TestPoolFail=TestPoolCalls+FailAt;
        CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_NO_MEMORY && !A.hResource);
        CHECK(Equal(Infos,SavedInfos,4*sizeof(*Infos)) && !MtkMaliAllocationHasObjects(&Adapter.Allocations) && !TestPoolLive);
    }
    TestPoolFail=0;
    for(unsigned Bad=0;Bad<20;Bad++) {
        for(unsigned I=0;I<4;I++)Spec(I,4096,0);
        MALI_ALLOCATION_DESC *D=&Specs[2];
        switch(Bad) {
            case 0:D->Version=2;break;case 1:D->Bytes=0;break;case 2:D->Size=0;break;
            case 3:D->Size=MALI_ALLOCATION_MAX_BYTES+1;break;case 4:D->Flags=4;break;
            case 5:D->Flags=2;break;case 6:D->Kind=3;break;case 7:D->Width=1;break;
            case 8:Infos[2].PrivateDriverDataSize=39;break;case 9:Infos[2].pPrivateDriverData=0;break;
            case 10:Infos[2].FlagsWddm2.CreateInVpr=1;break;
            default:D->Kind=2;D->Width=128;D->Height=2;D->Pitch=512;D->Size=1024;D->Format=D3DDDIFMT_A8R8G8B8;
                if(Bad==11)D->Width=0;if(Bad==12)D->Width=16385;if(Bad==13)D->Height=0;
                if(Bad==14)D->Height=16385;if(Bad==15)D->Pitch=256;if(Bad==16)D->Pitch=513;
                if(Bad==17)D->Size=1025;if(Bad==18)D->Format=D3DDDIFMT_DXT1;if(Bad==19)D->Size=UINT64_MAX;
        }
        DXGKARG_CREATEALLOCATION A=Creation(4,0);RtlCopyMemory(SavedInfos,Infos,4*sizeof(*Infos));
        CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)!=STATUS_SUCCESS && !A.hResource);
        CHECK(Equal(Infos,SavedInfos,4*sizeof(*Infos)) && !TestPoolLive);
    }
    Spec(0,4096,0);DXGKARG_CREATEALLOCATION A=Creation(1,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS);
    HANDLE H=Infos[0].hAllocation;unsigned I=Expose(10,H);
    for(unsigned FailAt=1;FailAt<=4;FailAt++) {
        for(unsigned N=0;N<4;N++){RtlZeroMemory(&OpenInfos[N],sizeof(OpenInfos[N]));OpenInfos[N].hAllocation=10;}
        RtlCopyMemory(SavedOpenInfos,OpenInfos,4*sizeof(*OpenInfos));DXGKARG_OPENALLOCATION O={0};O.NumAllocations=4;O.pOpenAllocation=OpenInfos;
        TestPoolFail=TestPoolCalls+FailAt;CHECK(MtkMaliDdiOpenAllocation(DeviceHandles[0],&O)==STATUS_NO_MEMORY);
        CHECK(Equal(OpenInfos,SavedOpenInfos,4*sizeof(*OpenInfos)) && !Adapter.Allocations.OpenCount && TestPoolLive==2);
    }
    TestPoolFail=0;
    DXGKARG_OPENALLOCATION O={0};O.NumAllocations=4;O.pOpenAllocation=OpenInfos;
    OpenInfos[2].hAllocation=99;CHECK(MtkMaliDdiOpenAllocation(DeviceHandles[0],&O)==STATUS_INVALID_HANDLE && !Adapter.Allocations.OpenCount && TestPoolLive==2);
    OsHandles[I].Allocation=(HANDLE)1;O.NumAllocations=1;CHECK(MtkMaliDdiOpenAllocation(DeviceHandles[0],&O)==STATUS_INVALID_HANDLE);OsHandles[I].Allocation=H;
    HANDLE OH=OpenOne(0,I,0);uint64_t T=777;
    TestPoolFail=TestPoolCalls+1;CHECK(Acquire(0,10,0,8,TRUE,&T)==STATUS_NO_MEMORY && T==777 && !Adapter.Allocations.ReferenceCount);
    TestPoolFail=0;Adapter.Allocations.NextReference=UINT64_MAX;
    CHECK(Acquire(0,10,0,8,TRUE,&T)==STATUS_INSUFFICIENT_RESOURCES && T==777);Adapter.Allocations.NextReference=0;
    CHECK(Close(0,1,&OH)==STATUS_SUCCESS && Destroy(1,&H,A.hResource,1)==STATUS_SUCCESS);End();
}
static void SnapshotMutation(void) {
    TestPoolMutation=0;Specs[0].Size=123;
}
static void ExtensionAndValidation(void) {
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);Spec(0,4097,0);TestPoolMutation=SnapshotMutation;
    DXGKARG_CREATEALLOCATION A=Creation(1,0);A.Flags.Resource=0;
    CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS && !A.hResource && Infos[0].Size==8192 && Specs[0].Size==123);
    HANDLE H=Infos[0].hAllocation;
    CHECK(((ALLOCATION*)H)->Desc.Size==4097 && Destroy(1,&H,0,0)==STATUS_SUCCESS);
    Spec(0,4096,0);A=Creation(1,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS);H=Infos[0].hAllocation;
    HANDLE Resource=A.hResource;Spec(0,8192,0);A=Creation(1,Resource);TestPoolFail=TestPoolCalls+1;
    CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_NO_MEMORY && A.hResource==Resource && Adapter.Allocations.AllocationCount==1);
    CHECK(((ALLOCATION_RESOURCE*)Resource)->Allocations==1);TestPoolFail=0;
    CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS && A.hResource==Resource);
    HANDLE H2=Infos[0].hAllocation;
    CHECK(Destroy(1,&H,Resource,1)==STATUS_INVALID_PARAMETER && Adapter.Allocations.AllocationCount==2);
    CHECK(Destroy(1,&H,0,0)==STATUS_SUCCESS && Destroy(1,&H2,0,0)==STATUS_SUCCESS && Adapter.Allocations.ResourceCount==1);
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_DEVICE_BUSY);
    CHECK(Destroy(0,0,Resource,1)==STATUS_SUCCESS && !TestPoolLive);
    CHECK(Destroy(0,0,0,1)==STATUS_INVALID_HANDLE && Destroy(0,0,0,0)==STATUS_INVALID_PARAMETER);
    Spec(0,4096,3);A=Creation(1,0);A.Flags.Resource=0;CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INVALID_PARAMETER);
    Spec(0,4096,0);A=Creation(1,(HANDLE)1);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INVALID_HANDLE);
    A=Creation(1,0);A.PrivateDriverDataSize=1;CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_NOT_SUPPORTED);
    A=Creation(0,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INVALID_PARAMETER);
    A=Creation(257,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INVALID_PARAMETER);
    A=Creation(1,0);A.Flags.Value=2;CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INVALID_PARAMETER);
    A=Creation(1,0);Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INVALID_PARAMETER && !GateHeld);
    CHECK(MtkMaliDdiDestroyAllocation(&Adapter,0)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiOpenAllocation(DeviceHandles[0],0)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiCloseAllocation(DeviceHandles[0],0)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiDescribeAllocation(&Adapter,0)==STATUS_INVALID_PARAMETER);
    CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,0)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;End();
}
static void Capacity(void) {
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);HANDLE Resource=0;
    for(unsigned Batch=0;Batch<MALI_ALLOCATION_LIMIT/MALI_ALLOCATION_BATCH;Batch++) {
        for(unsigned I=0;I<MALI_ALLOCATION_BATCH;I++)Spec(I,1,0);
        DXGKARG_CREATEALLOCATION A=Creation(MALI_ALLOCATION_BATCH,Resource);
        CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS);Resource=A.hResource;
        for(unsigned I=0;I<MALI_ALLOCATION_BATCH;I++)Handles[Batch*MALI_ALLOCATION_BATCH+I]=Infos[I].hAllocation;
    }
    CHECK(Adapter.Allocations.AllocationCount==MALI_ALLOCATION_LIMIT && TestPoolLive==MALI_ALLOCATION_LIMIT+1);
    Spec(0,1,0);DXGKARG_CREATEALLOCATION A=Creation(1,Resource);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INSUFFICIENT_RESOURCES);
    for(unsigned Batch=MALI_ALLOCATION_LIMIT/MALI_ALLOCATION_BATCH;Batch;Batch--)
        CHECK(Destroy(MALI_ALLOCATION_BATCH,Handles+(Batch-1)*MALI_ALLOCATION_BATCH,Resource,Batch==1)==STATUS_SUCCESS);
    Spec(0,4096,0);A=Creation(1,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS);
    HANDLE H=Infos[0].hAllocation;unsigned Os=Expose(10,H);
    for(unsigned Batch=0;Batch<MALI_ALLOCATION_LIMIT/MALI_ALLOCATION_BATCH;Batch++) {
        for(unsigned I=0;I<MALI_ALLOCATION_BATCH;I++){RtlZeroMemory(&OpenInfos[I],sizeof(OpenInfos[I]));OpenInfos[I].hAllocation=10;}
        DXGKARG_OPENALLOCATION O={0};O.NumAllocations=MALI_ALLOCATION_BATCH;O.pOpenAllocation=OpenInfos;
        CHECK(MtkMaliDdiOpenAllocation(DeviceHandles[0],&O)==STATUS_SUCCESS);
        for(unsigned I=0;I<MALI_ALLOCATION_BATCH;I++)OpenHandles[Batch*MALI_ALLOCATION_BATCH+I]=OpenInfos[I].hDeviceSpecificAllocation;
    }
    CHECK(Adapter.Allocations.OpenCount==MALI_ALLOCATION_LIMIT);
    DXGKARG_OPENALLOCATION O={0};O.NumAllocations=1;O.pOpenAllocation=OpenInfos;
    CHECK(MtkMaliDdiOpenAllocation(DeviceHandles[0],&O)==STATUS_INSUFFICIENT_RESOURCES);
    for(unsigned Batch=MALI_ALLOCATION_LIMIT/MALI_ALLOCATION_BATCH;Batch;Batch--)
        CHECK(Close(0,MALI_ALLOCATION_BATCH,OpenHandles+(Batch-1)*MALI_ALLOCATION_BATCH)==STATUS_SUCCESS);
    HANDLE OH=OpenOne(0,Os,0);
    for(unsigned I=0;I<MALI_ALLOCATION_LIMIT;I++)CHECK(Acquire(0,10,0,8,FALSE,&RefTokens[I])==STATUS_SUCCESS);
    uint64_t T=0;CHECK(Acquire(0,10,0,8,FALSE,&T)==STATUS_INSUFFICIENT_RESOURCES && !T);
    CHECK(Close(0,1,&OH)==STATUS_DEVICE_BUSY);
    for(unsigned I=MALI_ALLOCATION_LIMIT;I;I--)CHECK(Drop(RefTokens[I-1])==STATUS_SUCCESS);
    CHECK(Close(0,1,&OH)==STATUS_SUCCESS && Destroy(1,&H,A.hResource,1)==STATUS_SUCCESS);End();
}
static NTSTATUS SubmitAllocation(UINT Fence) {
    DXGKARG_SUBMITCOMMANDVIRTUAL A={0};A.hContext=ContextHandle;A.DmaBufferVirtualAddress=0x80000000;
    A.DmaBufferSize=4096;A.SubmissionFenceId=Fence;return MtkMaliDdiSubmitCommandVirtual(&Adapter,&A);
}
static void ScheduledReferences(void) {
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);Spec(0,4096,0);
    DXGKARG_CREATEALLOCATION A=Creation(1,0);CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS);
    HANDLE H=Infos[0].hAllocation;unsigned Os=Expose(10,H);HANDLE OH=OpenOne(0,Os,0);
    ExecutionHandle=10;ExecutionDevice=DeviceHandles[0];ExecutionProcess=(HANDLE)0x1234;
    DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(MtkMaliDdiCreateContext(ExecutionDevice,&C)==STATUS_SUCCESS);ContextHandle=C.hContext;
    DXGKARG_SETROOTPAGETABLE Root={0};Root.hContext=ContextHandle;Root.Address.SegmentOffset=0x40001000;Root.NumEntries=512;
    MtkMaliDdiSetRootPageTable(&Adapter,&Root);CHECK(Adapter.LastRootStatus==STATUS_SUCCESS);
    CHECK(SubmitAllocation(1)==STATUS_SUCCESS && Adapter.Allocations.ReferenceCount==1);
    CHECK(SubmitAllocation(1)==STATUS_INVALID_PARAMETER && Releases==1 && Reasons[MaliDdiRejected]==1 && Adapter.Allocations.ReferenceCount==1);
    CHECK(Close(0,1,&OH)==STATUS_DEVICE_BUSY && Destroy(1,&H,A.hResource,1)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,0,1,0);SyncFail=1;
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_UNSUCCESSFUL && Adapter.Allocations.ReferenceCount==1);
    CHECK(Close(0,1,&OH)==STATUS_DEVICE_BUSY);SyncFail=0;
    CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && !Adapter.Allocations.ReferenceCount && Reasons[MaliDdiCompleted]==1);
    CHECK(SubmitAllocation(2)==STATUS_SUCCESS && SubmitAllocation(3)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,4)==STATUS_SUCCESS);Completion(&G,0,0x42,1);
    CHECK(MtkMaliDdiPump(&Adapter,5)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && Adapter.Allocations.ReferenceCount==2);
    DXGKARG_RESETENGINE Reset={0};G.ResetStuck=1;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_IO_TIMEOUT && Adapter.Allocations.ReferenceCount==2);
    CHECK(Close(0,1,&OH)==STATUS_DEVICE_BUSY);G.ResetStuck=0;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS && !Adapter.Allocations.ReferenceCount && Reasons[MaliDdiReset]==2);
    CHECK(MtkMaliDdiDestroyContext(ContextHandle)==STATUS_SUCCESS);
    CHECK(Close(0,1,&OH)==STATUS_SUCCESS && Destroy(1,&H,A.hResource,1)==STATUS_SUCCESS);End();
}
static void PaddedLinearTargets(void) {
    _Static_assert(MaliClearBgra8==D3DDDIFMT_A8R8G8B8 && MaliClearBgrx8==D3DDDIFMT_X8R8G8B8 &&
        MaliClearRgba8==D3DDDIFMT_A8B8G8R8 && MaliClearRgbx8==D3DDDIFMT_X8B8G8R8,"clear format ABI");
    const UINT Formats[]={MaliClearBgra8,MaliClearBgrx8,MaliClearRgba8,MaliClearRgbx8};
    Begin(DXGKDDI_INTERFACE_VERSION_WDDM3_2);
    for(UINT F=0;F<4;F++)for(UINT Padded=0;Padded<2;Padded++) {
        Spec(0,Padded?4096:2432,3);Specs[0].Kind=MALI_ALLOCATION_LINEAR_2D;
        Specs[0].Width=17;Specs[0].Height=19;Specs[0].Pitch=128;Specs[0].Format=Formats[F];
        DXGKARG_CREATEALLOCATION A=Creation(1,0);
        CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_SUCCESS && Infos[0].Size==4096);
        HANDLE H=Infos[0].hAllocation;UINT Id=10+F*2+Padded;unsigned Os=Expose(Id,H);HANDLE O=OpenOne(0,Os,0);
        DXGKARG_DESCRIBEALLOCATION D={0};D.hAllocation=H;
        CHECK(MtkMaliDdiDescribeAllocation(&Adapter,&D)==STATUS_SUCCESS && D.Width==17 && D.Height==19 && D.Format==Formats[F]);
        uint64_t Ref=0;NTSTATUS S=Acquire(0,Id,0,4096,TRUE,&Ref);
        CHECK(S==(Padded?STATUS_SUCCESS:STATUS_INVALID_PARAMETER));
        if(Padded)CHECK(Ref && Drop(Ref)==STATUS_SUCCESS);else CHECK(!Ref);
        CHECK(Close(0,1,&O)==STATUS_SUCCESS && Destroy(1,&H,A.hResource,1)==STATUS_SUCCESS);
    }
    const uint64_t Bad[]={2431,2433,4095,4097,8192};
    for(UINT I=0;I<sizeof(Bad)/sizeof(Bad[0]);I++) {
        Spec(0,Bad[I],3);Specs[0].Kind=MALI_ALLOCATION_LINEAR_2D;
        Specs[0].Width=17;Specs[0].Height=19;Specs[0].Pitch=128;Specs[0].Format=MaliClearBgra8;
        DXGKARG_CREATEALLOCATION A=Creation(1,0);DXGK_ALLOCATIONINFO Before=Infos[0];
        CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INVALID_PARAMETER && !A.hResource);
        CHECK(Equal(&Before,&Infos[0],sizeof(Before)) && !Adapter.Allocations.AllocationCount && !TestPoolLive);
    }
    End();
}
int TestMain(void) {
    SharingAndBacking();FeatureCases();AtomicFailures();ExtensionAndValidation();Capacity();ScheduledReferences();PaddedLinearTargets();
    CHECK(!TestPoolLive && !GateHeld);return (int)FirstFailure;
}
