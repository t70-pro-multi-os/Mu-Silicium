// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "bdd.hxx"
extern "C" NTSTATUS DriverEntry(DRIVER_OBJECT*,UNICODE_STRING*);
#pragma function(memcpy,memset,memcmp)
extern "C" void *memcpy(void *a,const void *b,size_t n) { volatile UCHAR *d=(volatile UCHAR*)a; const volatile UCHAR *s=(const volatile UCHAR*)b; while(n--) *d++=*s++; return a; }
extern "C" void *memset(void *a,int v,size_t n) { volatile UCHAR *d=(volatile UCHAR*)a; while(n--) *d++=(UCHAR)v; return a; }
extern "C" int memcmp(const void *a,const void *b,size_t n) { const volatile UCHAR *d=(const volatile UCHAR*)a,*s=(const volatile UCHAR*)b; while(n--) { if(*d!=*s) return *d-*s; d++;s++; } return 0; }
static ULONG Failure,Checks,MutexDepth;
#define CHECK(x) do { ++Checks; if (!(x) && !Failure) Failure=__LINE__; } while(0)
static KMDDOD_INITIALIZATION_DATA Registered;
static __declspec(align(16)) UCHAR Pool[512*1024];
static SIZE_T PoolNext;
static ULONG Allocations,Frees,Maps,Unmaps,Locks,Unlocks,Mdls,MdlFrees;
static BOOLEAN PoolFail,MapFail,MdlFail,MdlMapFail;
static ULONG AcpiBad,AcpiCalls,AcquireFail,AcquireMismatch,AcquireCalls,ResumeDiagnostics;
static SIZE_T MappedLength;
static MTK_FB_HANDOFF H;
static UCHAR Fb[4096],Source[4096],Before[4096];
static MDL Mdl;
static void Reset() {
    CHECK(MutexDepth==0);
    PoolNext=0;Allocations=Frees=Maps=Unmaps=Locks=Unlocks=Mdls=MdlFrees=0;
    PoolFail=MapFail=MdlFail=MdlMapFail=FALSE;
    AcpiBad=AcpiCalls=AcquireFail=AcquireMismatch=AcquireCalls=ResumeDiagnostics=0;MappedLength=0;
    H={1,1,0x80000000,4096,16,8,80,1,0};
    memset(Fb,0xa5,sizeof(Fb));memset(Source,0x37,sizeof(Source));
}
extern "C" {
ULONG FakeDebugPrintEx(ULONG component,ULONG level,PCSTR format,...) {
    CHECK(component==DPFLTR_IHVVIDEO_ID && level==DPFLTR_ERROR_LEVEL && format);
    ResumeDiagnostics++;return 0;
}
decltype(&DbgPrintEx) __imp_DbgPrintEx=FakeDebugPrintEx;
VOID KeInitializeMutex(PRKMUTEX mutex,ULONG level) { CHECK(mutex && !level);memset(mutex,0,sizeof(*mutex)); }
NTSTATUS KeWaitForSingleObject(PVOID object,KWAIT_REASON reason,KPROCESSOR_MODE mode,BOOLEAN alert,PLARGE_INTEGER timeout) {
    CHECK(object && reason==Executive && mode==KernelMode && !alert && !timeout);MutexDepth++;return STATUS_SUCCESS;
}
LONG KeReleaseMutex(PRKMUTEX mutex,BOOLEAN wait) { CHECK(mutex && !wait && MutexDepth>0);MutexDepth--;return 0; }
const ULONG64 MmUserProbeAddress=0x7fffffffffffULL;
PVOID ExAllocatePool2(POOL_FLAGS flags,SIZE_T bytes,ULONG tag) {
    CHECK((flags==POOL_FLAG_PAGED || flags==POOL_FLAG_NON_PAGED) && tag==BDDTAG);
    if (PoolFail || bytes>sizeof(Pool)-PoolNext) return NULL;
    void *p=Pool+PoolNext; PoolNext=(PoolNext+bytes+15)&~15ULL; Allocations++;memset(p,0,bytes);return p;
}
VOID ExFreePoolWithTag(PVOID p,ULONG tag) { CHECK(p>=Pool && p<Pool+sizeof(Pool));UNREFERENCED_PARAMETER(tag);Frees++; }
PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS a,SIZE_T n,ULONG protect) {
    CHECK(a.QuadPart==H.Base && n==H.Pitch*H.Height && protect==(PAGE_READWRITE|PAGE_NOCACHE));
    Maps++;if(MapFail)return NULL;MappedLength=n;return Fb;
}
VOID MmUnmapIoSpace(PVOID p,SIZE_T n) { CHECK(p==Fb && n==MappedLength);Unmaps++;MappedLength=0; }
PMDL IoAllocateMdl(PVOID p,ULONG n,BOOLEAN secondary,BOOLEAN quota,PIRP irp) {
    CHECK(p==Source && n>=16*4*8 && !secondary && !quota && !irp);
    if (MdlFail) return NULL;
    Mdls++;memset(&Mdl,0,sizeof(Mdl));Mdl.MappedSystemVa=p;Mdl.MdlFlags=MdlMapFail?0:MDL_MAPPED_TO_SYSTEM_VA;
    return &Mdl;
}
VOID IoFreeMdl(PMDL m) { CHECK(m==&Mdl);MdlFrees++; }
VOID MmProbeAndLockPages(PMDL m,KPROCESSOR_MODE mode,LOCK_OPERATION access) {
    CHECK(m==&Mdl && mode==UserMode && access==IoReadAccess);Locks++;
}
VOID MmUnlockPages(PMDL m) { CHECK(m==&Mdl);Unlocks++; }
#undef MmMapLockedPagesSpecifyCache
PVOID MmMapLockedPagesSpecifyCache(PMDL m,KPROCESSOR_MODE mode,MEMORY_CACHING_TYPE cache,
    PVOID requested,ULONG bugcheck,ULONG priority) {
    CHECK(m==&Mdl && mode==KernelMode && cache==MmCached && !requested && !bugcheck);
    CHECK(priority & MdlMappingNoExecute); return NULL;
}
NTSTATUS DxgkInitializeDisplayOnlyDriver(PDRIVER_OBJECT o,PUNICODE_STRING path,PKMDDOD_INITIALIZATION_DATA data) {
    CHECK(o && path && data->Version==DXGKDDI_INTERFACE_VERSION_WIN8);
    Registered=*data;return STATUS_SUCCESS;
}
}
static NTSTATUS APIENTRY DeviceInfo(HANDLE handle,PDXGK_DEVICE_INFO info) {
    CHECK(handle==(HANDLE)0x1234);memset(info,0,sizeof(*info));return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Acpi(HANDLE handle,ULONG uid,PACPI_EVAL_INPUT_BUFFER_COMPLEX input,
    ULONG inSize,PACPI_EVAL_OUTPUT_BUFFER output,ULONG outSize) {
    CHECK(handle==(HANDLE)0x1234 && uid==DISPLAY_ADAPTER_HW_ID && inSize==sizeof(*input));
    CHECK(input->Signature==ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE && !input->ArgumentCount);
    AcpiCalls++;
    if (AcpiBad==1) return STATUS_IO_DEVICE_ERROR;
    memset(output,0,outSize);output->Signature=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE;output->Count=1;
    if (input->MethodNameAsUlong==ACPI_METHOD_HARDWARE_ID) {
        output->Argument[0].Type=ACPI_METHOD_ARGUMENT_STRING;output->Argument[0].DataLength=9;
        memcpy(output->Argument[0].Data,"HANA678C",9);
        if (AcpiBad==2) output->Argument[0].Data[7]='B';
    } else {
        CHECK(input->MethodNameAsUlong=='IBFD');
        output->Argument[0].Type=ACPI_METHOD_ARGUMENT_BUFFER;output->Argument[0].DataLength=sizeof(H);
        memcpy(output->Argument[0].Data,&H,sizeof(H));
        if(AcpiBad==3) output->Signature=0;
        if(AcpiBad==4) output->Count=2;
        if(AcpiBad==5) output->Argument[0].Type=ACPI_METHOD_ARGUMENT_INTEGER;
        if(AcpiBad==6) output->Argument[0].DataLength=47;
        if(AcpiBad==8 && AcpiCalls>2) output->Argument[0].Data[24]++;
    }
    output->Length=FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+output->Argument[0].DataLength;
    if(AcpiBad==7) output->Length++;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Acquire(HANDLE handle,PDXGK_DISPLAY_INFORMATION d) {
    CHECK(handle==(HANDLE)0x1234);AcquireCalls++;
    memset(d,0,sizeof(*d));d->PhysicAddress.QuadPart=H.Base;d->Width=H.Width;d->Height=H.Height;
    d->Pitch=H.Pitch;d->ColorFormat=D3DDDIFMT_X8R8G8B8;d->TargetId=0;
    if (AcquireMismatch==1)d->Width++;
    if (AcquireMismatch==2)d->PhysicAddress.QuadPart+=4096;
    if (AcquireMismatch==3)d->ColorFormat=D3DDDIFMT_R5G6B5;
    if (AcquireMismatch==4)d->TargetId=1;
    if (AcquireMismatch==5)d->ColorFormat=D3DDDIFMT_A8R8G8B8;
    if (AcquireMismatch==6)d->Width=0;
    return AcquireFail?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
}
static NTSTATUS APIENTRY QueryVidpn(D3DKMDT_HVIDPN,DXGK_VIDPN_INTERFACE_VERSION,const DXGK_VIDPN_INTERFACE**);
static PVOID Add() {
    PVOID ctx=NULL;CHECK(Registered.DxgkDdiAddDevice((PDEVICE_OBJECT)0x123,&ctx)==STATUS_SUCCESS && ctx);return ctx;
}
static NTSTATUS Start(PVOID ctx) {
    DXGKRNL_INTERFACE i={};DXGK_START_INFO start={};ULONG views=99,children=99;
    i.DxgkCbQueryVidPnInterface=QueryVidpn;
    i.DeviceHandle=(HANDLE)0x1234;i.DxgkCbGetDeviceInformation=DeviceInfo;
    i.DxgkCbEvalAcpiMethod=Acpi;i.DxgkCbAcquirePostDisplayOwnership=Acquire;
    NTSTATUS status=Registered.DxgkDdiStartDevice(ctx,&start,&i,&views,&children);
    if (NT_SUCCESS(status)) CHECK(views==1 && children==1);
    else CHECK(views==0 && children==0);
    return status;
}
static D3DKMDT_VIDPN_SOURCE_MODE Mode(unsigned w,unsigned h) {
    D3DKMDT_VIDPN_SOURCE_MODE s={};s.Type=D3DKMDT_RMT_GRAPHICS;
    s.Format.Graphics.PrimSurfSize.cx=w;s.Format.Graphics.PrimSurfSize.cy=h;
    s.Format.Graphics.VisibleRegionSize=s.Format.Graphics.PrimSurfSize;
    s.Format.Graphics.Stride=w*4;s.Format.Graphics.PixelFormat=D3DDDIFMT_A8R8G8B8;
    s.Format.Graphics.ColorBasis=D3DKMDT_CB_SCRGB;s.Format.Graphics.PixelValueAccessMode=D3DKMDT_PVAM_DIRECT;return s;
}
static D3DKMDT_VIDPN_SOURCE_MODE Pinned;
static D3DKMDT_VIDPN_PRESENT_PATH Path;
static DXGK_VIDPN_INTERFACE Vidpn;
static DXGK_VIDPNTOPOLOGY_INTERFACE Topology;
static DXGK_VIDPNSOURCEMODESET_INTERFACE SourceSet;
static ULONG SourceRefs,ModeRefs,PathRefs,TargetRefs,TargetModeRefs;
static D3DKMDT_VIDPN_TARGET_MODE Target;
static DXGK_VIDPNTARGETMODESET_INTERFACE TargetSet;
static BOOLEAN Unpinned;
static ULONG SourcesCreated,SourcesAdded,TargetsCreated,TargetsAdded,SupportUpdates;
static D3DKMDT_VIDPN_SOURCE_MODE NewSources[2];
static D3DKMDT_VIDPN_TARGET_MODE NewTarget;
static NTSTATUS APIENTRY QueryVidpn(D3DKMDT_HVIDPN h,DXGK_VIDPN_INTERFACE_VERSION version,const DXGK_VIDPN_INTERFACE **out) {
    CHECK(h==(D3DKMDT_HVIDPN)0x1111 && version==DXGK_VIDPN_INTERFACE_VERSION_V1);
    *out=&Vidpn;return STATUS_SUCCESS;
}
static void SetupVidpn(unsigned r) {
    Pinned=Mode((r&1)?8:16,(r&1)?16:8);memset(&Path,0,sizeof(Path));
    SourceRefs=ModeRefs=PathRefs=TargetRefs=TargetModeRefs=0;
    SourcesCreated=SourcesAdded=TargetsCreated=TargetsAdded=SupportUpdates=0;Unpinned=FALSE;
    memset(&Target,0,sizeof(Target));
    Target.VideoSignalInfo.ActiveSize.cx=Target.VideoSignalInfo.TotalSize.cx=16;
    Target.VideoSignalInfo.ActiveSize.cy=Target.VideoSignalInfo.TotalSize.cy=8;
    Target.VideoSignalInfo.ScanLineOrdering=D3DDDI_VSSLO_PROGRESSIVE;
    Target.VideoSignalInfo.PixelRate=D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Target.VideoSignalInfo.HSyncFreq.Numerator=Target.VideoSignalInfo.HSyncFreq.Denominator=D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Target.VideoSignalInfo.VSyncFreq.Numerator=Target.VideoSignalInfo.VSyncFreq.Denominator=D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Path.ContentTransformation.Scaling=D3DKMDT_VPPS_IDENTITY;
    Path.ContentTransformation.Rotation=(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION)(D3DKMDT_VPPR_IDENTITY+r);
    Path.GammaRamp.Type=D3DDDI_GAMMARAMP_DEFAULT;Path.VidPnTargetColorBasis=D3DKMDT_CB_SCRGB;
    Vidpn.pfnGetTopology=[](D3DKMDT_HVIDPN h,D3DKMDT_HVIDPNTOPOLOGY *t,const DXGK_VIDPNTOPOLOGY_INTERFACE **out)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPN)0x1111);*t=(D3DKMDT_HVIDPNTOPOLOGY)0x2222;*out=&Topology;return STATUS_SUCCESS;
    };
    Topology.pfnGetNumPaths=[](D3DKMDT_HVIDPNTOPOLOGY h,SIZE_T *n)->NTSTATUS {CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222);*n=1;return STATUS_SUCCESS;};
    Topology.pfnGetNumPathsFromSource=[](D3DKMDT_HVIDPNTOPOLOGY h,UINT id,SIZE_T *n)->NTSTATUS {CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222 && !id);*n=1;return STATUS_SUCCESS;};
    Topology.pfnEnumPathTargetsFromSource=[](D3DKMDT_HVIDPNTOPOLOGY h,UINT id,SIZE_T index,UINT *target)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222 && !id && !index);*target=0;return STATUS_SUCCESS;
    };
    Topology.pfnAcquirePathInfo=[](D3DKMDT_HVIDPNTOPOLOGY h,UINT source,UINT target,const D3DKMDT_VIDPN_PRESENT_PATH **path)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222 && !source && !target);*path=&Path;PathRefs++;return STATUS_SUCCESS;
    };
    Topology.pfnAcquireFirstPathInfo=[](D3DKMDT_HVIDPNTOPOLOGY h,const D3DKMDT_VIDPN_PRESENT_PATH **path)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222);*path=&Path;PathRefs++;return STATUS_SUCCESS;
    };
    Topology.pfnReleasePathInfo=[](D3DKMDT_HVIDPNTOPOLOGY h,const D3DKMDT_VIDPN_PRESENT_PATH *path)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222 && path==&Path && PathRefs==1);PathRefs--;return STATUS_SUCCESS;
    };
    Vidpn.pfnAcquireSourceModeSet=[](D3DKMDT_HVIDPN h,UINT source,D3DKMDT_HVIDPNSOURCEMODESET *set,const DXGK_VIDPNSOURCEMODESET_INTERFACE **out)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPN)0x1111 && !source);*set=(D3DKMDT_HVIDPNSOURCEMODESET)0x3333;*out=&SourceSet;SourceRefs++;return STATUS_SUCCESS;
    };
    Vidpn.pfnReleaseSourceModeSet=[](D3DKMDT_HVIDPN h,D3DKMDT_HVIDPNSOURCEMODESET set)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPN)0x1111 && set==(D3DKMDT_HVIDPNSOURCEMODESET)0x3333 && SourceRefs==1);SourceRefs--;return STATUS_SUCCESS;
    };
    SourceSet.pfnAcquirePinnedModeInfo=[](D3DKMDT_HVIDPNSOURCEMODESET set,const D3DKMDT_VIDPN_SOURCE_MODE **mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNSOURCEMODESET)0x3333);*mode=Unpinned?NULL:&Pinned;if(!Unpinned) ModeRefs++;return STATUS_SUCCESS;
    };
    SourceSet.pfnReleaseModeInfo=[](D3DKMDT_HVIDPNSOURCEMODESET set,const D3DKMDT_VIDPN_SOURCE_MODE *mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNSOURCEMODESET)0x3333 && mode==&Pinned && ModeRefs==1);ModeRefs--;return STATUS_SUCCESS;
    };
    Vidpn.pfnAcquireTargetModeSet=[](D3DKMDT_HVIDPN h,UINT target,D3DKMDT_HVIDPNTARGETMODESET *set,const DXGK_VIDPNTARGETMODESET_INTERFACE **out)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPN)0x1111 && !target);*set=(D3DKMDT_HVIDPNTARGETMODESET)0x4444;*out=&TargetSet;TargetRefs++;return STATUS_SUCCESS;
    };
    Vidpn.pfnReleaseTargetModeSet=[](D3DKMDT_HVIDPN h,D3DKMDT_HVIDPNTARGETMODESET set)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPN)0x1111 && set==(D3DKMDT_HVIDPNTARGETMODESET)0x4444 && TargetRefs==1);TargetRefs--;return STATUS_SUCCESS;
    };
    TargetSet.pfnAcquirePinnedModeInfo=[](D3DKMDT_HVIDPNTARGETMODESET set,const D3DKMDT_VIDPN_TARGET_MODE **mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNTARGETMODESET)0x4444);*mode=Unpinned?NULL:&Target;if(!Unpinned) TargetModeRefs++;return STATUS_SUCCESS;
    };
    TargetSet.pfnReleaseModeInfo=[](D3DKMDT_HVIDPNTARGETMODESET set,const D3DKMDT_VIDPN_TARGET_MODE *mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNTARGETMODESET)0x4444 && mode==&Target && TargetModeRefs==1);TargetModeRefs--;return STATUS_SUCCESS;
    };
    Vidpn.pfnCreateNewSourceModeSet=Vidpn.pfnAcquireSourceModeSet;
    Vidpn.pfnCreateNewTargetModeSet=Vidpn.pfnAcquireTargetModeSet;
    Vidpn.pfnAssignSourceModeSet=[](D3DKMDT_HVIDPN h,UINT id,D3DKMDT_HVIDPNSOURCEMODESET set)->NTSTATUS {
        CHECK(!id);return Vidpn.pfnReleaseSourceModeSet(h,set);
    };
    Vidpn.pfnAssignTargetModeSet=[](D3DKMDT_HVIDPN h,UINT id,D3DKMDT_HVIDPNTARGETMODESET set)->NTSTATUS {
        CHECK(!id);return Vidpn.pfnReleaseTargetModeSet(h,set);
    };
    SourceSet.pfnCreateNewModeInfo=[](D3DKMDT_HVIDPNSOURCEMODESET set,D3DKMDT_VIDPN_SOURCE_MODE **mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNSOURCEMODESET)0x3333 && SourcesCreated<2);
        if(SourcesCreated>=2)return STATUS_UNSUCCESSFUL;
        *mode=&NewSources[SourcesCreated++];memset(*mode,0,sizeof(**mode));ModeRefs++;return STATUS_SUCCESS;
    };
    SourceSet.pfnAddMode=[](D3DKMDT_HVIDPNSOURCEMODESET set,D3DKMDT_VIDPN_SOURCE_MODE *mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNSOURCEMODESET)0x3333 && mode>=NewSources && mode<NewSources+2 && ModeRefs==1);
        CHECK(mode->Type==D3DKMDT_RMT_GRAPHICS && mode->Format.Graphics.PixelFormat==D3DDDIFMT_A8R8G8B8);
        CHECK(mode->Format.Graphics.Stride==4*mode->Format.Graphics.PrimSurfSize.cx);
        ModeRefs--;SourcesAdded++;return STATUS_SUCCESS;
    };
    TargetSet.pfnCreateNewModeInfo=[](D3DKMDT_HVIDPNTARGETMODESET set,D3DKMDT_VIDPN_TARGET_MODE **mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNTARGETMODESET)0x4444 && !TargetsCreated);
        *mode=&NewTarget;memset(*mode,0,sizeof(**mode));TargetModeRefs++;TargetsCreated++;return STATUS_SUCCESS;
    };
    TargetSet.pfnAddMode=[](D3DKMDT_HVIDPNTARGETMODESET set,D3DKMDT_VIDPN_TARGET_MODE *mode)->NTSTATUS {
        CHECK(set==(D3DKMDT_HVIDPNTARGETMODESET)0x4444 && mode==&NewTarget && TargetModeRefs==1);
        CHECK(mode->VideoSignalInfo.ActiveSize.cx==16 && mode->VideoSignalInfo.ActiveSize.cy==8 &&
            mode->VideoSignalInfo.PixelRate==D3DKMDT_FREQUENCY_NOTSPECIFIED);
        TargetsAdded++;TargetModeRefs--;return STATUS_SUCCESS;
    };
    Topology.pfnAcquireNextPathInfo=[](D3DKMDT_HVIDPNTOPOLOGY h,const D3DKMDT_VIDPN_PRESENT_PATH *path,const D3DKMDT_VIDPN_PRESENT_PATH **next)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222 && path==&Path);*next=NULL;return STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET;
    };
    Topology.pfnUpdatePathSupportInfo=[](D3DKMDT_HVIDPNTOPOLOGY h,const D3DKMDT_VIDPN_PRESENT_PATH *path)->NTSTATUS {
        CHECK(h==(D3DKMDT_HVIDPNTOPOLOGY)0x2222 && path->ContentTransformation.ScalingSupport.Identity &&
            !path->ContentTransformation.ScalingSupport.Centered && path->ContentTransformation.RotationSupport.Identity &&
            path->ContentTransformation.RotationSupport.Rotate90 && path->ContentTransformation.RotationSupport.Rotate180 &&
            path->ContentTransformation.RotationSupport.Rotate270);SupportUpdates++;return STATUS_SUCCESS;
    };
}
static NTSTATUS Commit(PVOID d) {
    DXGKARG_COMMITVIDPN commit={};commit.hFunctionalVidPn=(D3DKMDT_HVIDPN)0x1111;
    return Registered.DxgkDdiCommitVidPn(d,&commit);
}
static void Enable(PVOID d,unsigned r=0) {
    SetupVidpn(r);CHECK(Commit(d)==STATUS_SUCCESS);
    CHECK(!SourceRefs && !ModeRefs && !PathRefs);
    DXGKARG_SETVIDPNSOURCEVISIBILITY visibility={};visibility.Visible=TRUE;
    CHECK(Registered.DxgkDdiSetVidPnSourceVisibility(d,&visibility)==STATUS_SUCCESS);
    DXGKARG_ISSUPPORTEDVIDPN supported={};supported.hDesiredVidPn=(D3DKMDT_HVIDPN)0x1111;
    CHECK(Registered.DxgkDdiIsSupportedVidPn(d,&supported)==STATUS_SUCCESS && supported.IsVidPnSupported);
    CHECK(!SourceRefs && !ModeRefs && !PathRefs && !TargetRefs && !TargetModeRefs);
    Target.VideoSignalInfo.PixelRate=1;
    CHECK(Registered.DxgkDdiIsSupportedVidPn(d,&supported)==STATUS_SUCCESS && !supported.IsVidPnSupported);
    Target.VideoSignalInfo.PixelRate=D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Pinned.Format.Graphics.PrimSurfSize.cx++;
    CHECK(!NT_SUCCESS(Commit(d)));
    CHECK(Registered.DxgkDdiIsSupportedVidPn(d,&supported)==STATUS_SUCCESS && !supported.IsVidPnSupported);
    Pinned.Format.Graphics.PrimSurfSize.cx--;
    CHECK(!SourceRefs && !ModeRefs && !PathRefs && !TargetRefs && !TargetModeRefs);
}
static void TestPowerLifecycle() {
    for(unsigned state=PowerDeviceD1;state<=PowerDeviceD3;state++) {
        Reset();auto ctx=Add();CHECK(Start(ctx)==STATUS_SUCCESS);Enable(ctx);
        auto d=(BASIC_DISPLAY_DRIVER*)ctx;
        DXGKARG_PRESENT_DISPLAYONLY p={};p.pSource=Source;p.BytesPerPixel=4;p.Pitch=80;
        RECT rect={0,0,16,8};p.NumDirtyRects=1;p.pDirtyRect=&rect;
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&p)==STATUS_SUCCESS);
        CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,(DEVICE_POWER_STATE)state,PowerActionSleep)==STATUS_SUCCESS);
        CHECK(AcquireCalls==1 && Maps==1 && Unmaps==1 && !d->IsScanoutReady());
        CHECK(Fb[0]==0 && Fb[639]==0 && Fb[640]==0xa5);
        memcpy(Before,Fb,sizeof(Fb));
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&p)==STATUS_DEVICE_NOT_READY);
        DXGKARG_SETVIDPNSOURCEVISIBILITY visibility={};visibility.Visible=TRUE;
        CHECK(Registered.DxgkDdiSetVidPnSourceVisibility(ctx,&visibility)==STATUS_DEVICE_NOT_READY);
        CHECK(Commit(ctx)==STATUS_DEVICE_NOT_READY);
        UINT w,h;D3DDDIFORMAT format;DXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags={};
        CHECK(Registered.DxgkDdiSystemDisplayEnable(ctx,0,&flags,&w,&h,&format)==STATUS_DEVICE_NOT_READY);
        Registered.DxgkDdiSystemDisplayWrite(ctx,Source,3,2,16,0,0);
        CHECK(!memcmp(Before,Fb,sizeof(Fb)));
        CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
        CHECK(AcquireCalls==2 && Maps==2 && Unmaps==1 && d->IsScanoutReady() && NT_SUCCESS(d->GetLastResumeStatus()));
        CHECK(!d->GetCurrentMode(0)->Flags.SourceNotVisible);
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&p)==STATUS_SUCCESS && Fb[0]==0x37);
        visibility.Visible=FALSE;
        CHECK(Registered.DxgkDdiSetVidPnSourceVisibility(ctx,&visibility)==STATUS_SUCCESS);
        CHECK(Registered.DxgkDdiSetPowerState(ctx,0,(DEVICE_POWER_STATE)state,PowerActionSleep)==STATUS_SUCCESS);
        CHECK(Registered.DxgkDdiSetPowerState(ctx,0,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
        CHECK(d->IsScanoutReady() && d->GetCurrentMode(0)->Flags.SourceNotVisible);
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&p)==STATUS_SUCCESS && Fb[0]==0);
        Registered.DxgkDdiRemoveDevice(ctx);CHECK(Unmaps==3 && Allocations==Frees && ResumeDiagnostics==0);
    }
    for(unsigned bad=0;bad<11;bad++) {
        Reset();auto ctx=Add();CHECK(Start(ctx)==STATUS_SUCCESS);Enable(ctx);
        auto d=(BASIC_DISPLAY_DRIVER*)ctx;
        CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD3,PowerActionHibernate)==STATUS_SUCCESS);
        MTK_FB_HANDOFF original=H;memcpy(Before,Fb,sizeof(Fb));
        if(bad==0)AcpiBad=1;
        else if(bad<=4)AcquireMismatch=bad;
        else if(bad==5)AcquireMismatch=6;
        else if(bad==6)MapFail=TRUE;
        else if(bad==7)H.Ready=0;
        else if(bad==8)H.Width++;
        else if(bad==9)AcquireFail=1;
        else AcpiBad=8;
        CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
        CHECK(!NT_SUCCESS(d->GetLastResumeStatus()) && !d->IsScanoutReady() && ResumeDiagnostics==1);
        CHECK(!d->GetCurrentMode(0)->FrameBuffer.Ptr && !d->GetCurrentMode(0)->Flags.FrameBufferIsActive);
        CHECK(!memcmp(Before,Fb,sizeof(Fb)) && Unmaps==1);
        UINT w,h;D3DDDIFORMAT format;DXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags={};
        CHECK(Registered.DxgkDdiSystemDisplayEnable(ctx,0,&flags,&w,&h,&format)==STATUS_DEVICE_NOT_READY);
        Registered.DxgkDdiSystemDisplayWrite(ctx,Source,3,2,16,0,0);CHECK(!memcmp(Before,Fb,sizeof(Fb)));
        DXGK_DISPLAY_INFORMATION returned;memset(&returned,0xa5,sizeof(returned));
        CHECK(Registered.DxgkDdiStopDeviceAndReleasePostDisplayOwnership(ctx,0,&returned)==STATUS_DEVICE_NOT_READY);
        CHECK(!returned.Width&&!returned.PhysicAddress.QuadPart&&d->IsDriverActive());
        H=original;AcpiBad=AcquireMismatch=AcquireFail=0;MapFail=FALSE;
        CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
        CHECK(d->IsScanoutReady());Registered.DxgkDdiRemoveDevice(ctx);
        CHECK(Unmaps==2 && Allocations==Frees);
    }
    Reset();auto ctx=Add();CHECK(Start(ctx)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiSetPowerState(ctx,0,PowerDeviceD3,PowerActionNone)==STATUS_SUCCESS);
    CHECK(Unmaps==1);
    CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    CHECK(AcquireCalls==2 && Maps==1 && !((BASIC_DISPLAY_DRIVER*)ctx)->IsScanoutReady());
    CHECK(Registered.DxgkDdiSetPowerState(ctx,0,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    CHECK(AcquireCalls==3 && Maps==2 && ((BASIC_DISPLAY_DRIVER*)ctx)->IsScanoutReady());
    Registered.DxgkDdiRemoveDevice(ctx);CHECK(Unmaps==2);

    Reset();ctx=Add();CHECK(Start(ctx)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD3,PowerActionHibernate)==STATUS_SUCCESS);
    H.Base+=8192;H.Pitch=96;
    CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
    auto d=(BASIC_DISPLAY_DRIVER*)ctx;
    CHECK(d->IsScanoutReady() && d->GetCurrentMode(0)->DispInfo.PhysicAddress.QuadPart==H.Base && d->GetCurrentMode(0)->DispInfo.Pitch==96);
    CHECK(Fb[767]==0&&Fb[768]==0xa5);Registered.DxgkDdiRemoveDevice(ctx);CHECK(Unmaps==2);
}
extern "C" ULONG TestMain() {
    DRIVER_OBJECT o={};UNICODE_STRING path={};
    CHECK(DriverEntry(&o,&path)==STATUS_SUCCESS);
    CHECK(Registered.DxgkDdiPresentDisplayOnly && Registered.DxgkDdiCommitVidPn &&
        !Registered.DxgkDdiControlInterrupt && !Registered.DxgkDdiGetScanLine &&
        !Registered.DxgkDdiInterruptRoutine && !Registered.DxgkDdiDpcRoutine);
    for(unsigned bad=0;bad<=8;bad++) {
        Reset();auto ctx=Add();AcpiBad=bad;CHECK(NT_SUCCESS(Start(ctx))==(bad==0));
        Registered.DxgkDdiRemoveDevice(ctx);CHECK(Allocations==Frees && Maps==(bad?0:1) && Maps==Unmaps);
    }
    for(unsigned bad=1;bad<=4;bad++) {
        Reset();auto ctx=Add();AcquireMismatch=bad;CHECK(!NT_SUCCESS(Start(ctx)));
        Registered.DxgkDdiRemoveDevice(ctx);CHECK(Allocations==Frees && !Maps);
    }
    Reset();auto ctx=Add();AcquireFail=1;CHECK(!NT_SUCCESS(Start(ctx)));Registered.DxgkDdiRemoveDevice(ctx);
    Reset();ctx=Add();AcquireMismatch=5;CHECK(Start(ctx)==STATUS_SUCCESS);Registered.DxgkDdiRemoveDevice(ctx);
    Reset();ctx=Add();MapFail=TRUE;
    CHECK(Start(ctx)==STATUS_NO_MEMORY && Maps==1 && !Unmaps && Fb[0]==0xa5);
    Registered.DxgkDdiRemoveDevice(ctx);CHECK(Allocations==Frees && !Unmaps);
    Reset();ctx=Add();H.Ready=0;CHECK(Start(ctx)==STATUS_DEVICE_NOT_READY);
    Registered.DxgkDdiRemoveDevice(ctx);
    Reset();ctx=Add();CHECK(Start(ctx)==STATUS_SUCCESS);SetupVidpn(0);Unpinned=TRUE;
    Path.ContentTransformation.Rotation=D3DKMDT_VPPR_UNPINNED;
    Path.ContentTransformation.Scaling=D3DKMDT_VPPS_UNPINNED;
    DXGKARG_ENUMVIDPNCOFUNCMODALITY enumeration={};enumeration.hConstrainingVidPn=(D3DKMDT_HVIDPN)0x1111;
    CHECK(Registered.DxgkDdiEnumVidPnCofuncModality(ctx,&enumeration)==STATUS_SUCCESS);
    CHECK(SourcesAdded==2 && TargetsAdded==1 && SupportUpdates==1 && !SourceRefs && !TargetRefs &&
        !ModeRefs && !PathRefs && !TargetModeRefs);
    Registered.DxgkDdiRemoveDevice(ctx);
    for(unsigned r=0;r<4;r++) {
        Reset();ctx=Add();CHECK(Start(ctx)==STATUS_SUCCESS);
        auto d=(BASIC_DISPLAY_DRIVER*)ctx;Enable(d,r);CHECK(Maps==1 && Fb[0]==0 && Fb[639]==0 && Fb[640]==0xa5);
        DXGKARG_PRESENT_DISPLAYONLY present={};present.pSource=Source;present.BytesPerPixel=4;
        present.Pitch=(r&1)?48:80;present.Flags.Rotate=TRUE;
        RECT rect={0,0,(r&1)?8:16,(r&1)?16:8};present.NumDirtyRects=1;present.pDirtyRect=&rect;
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&present)==STATUS_SUCCESS && Fb[0]==0x37 && Fb[640]==0xa5);
        CHECK(Mdls==1 && MdlFrees==1 && Locks==1 && Unlocks==1 && Allocations==Frees+1);
        memcpy(Before,Fb,sizeof(Fb));rect.right++;
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&present)==STATUS_INVALID_PARAMETER && !memcmp(Before,Fb,sizeof(Fb)));
        rect.right--;present.BytesPerPixel=8;
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&present)==STATUS_INVALID_PARAMETER);
        present.BytesPerPixel=4;MdlFail=TRUE;
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&present)==STATUS_INSUFFICIENT_RESOURCES);
        MdlFail=FALSE;MdlMapFail=TRUE;
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&present)==STATUS_INSUFFICIENT_RESOURCES);
        MdlMapFail=FALSE;PoolFail=TRUE;
        CHECK(Registered.DxgkDdiPresentDisplayOnly(ctx,&present)==STATUS_INSUFFICIENT_RESOURCES);
        PoolFail=FALSE;
        CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS);
        CHECK(Registered.DxgkDdiSetPowerState(ctx,DISPLAY_ADAPTER_HW_ID,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
        DXGK_CHILD_DESCRIPTOR children[2];
        CHECK(Registered.DxgkDdiQueryChildRelations(ctx,children,sizeof(children[0]))==STATUS_BUFFER_TOO_SMALL);
        CHECK(Registered.DxgkDdiQueryChildRelations(ctx,children,sizeof(children))==STATUS_SUCCESS && !children[1].ChildUid);
        UINT w=0,h=0;D3DDDIFORMAT fmt;DXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags={};
        CHECK(Registered.DxgkDdiSystemDisplayEnable(ctx,0,&flags,&w,&h,&fmt)==STATUS_SUCCESS && w==16 && h==8);
        memset(Source,0x4a,sizeof(Source));
        Registered.DxgkDdiSystemDisplayWrite(ctx,Source,3,2,16,13,6);
        CHECK(Fb[6*80+13*4]==0x4a && Fb[7*80+15*4]==0x4a && Fb[640]==0xa5);
        memcpy(Before,Fb,sizeof(Fb));Registered.DxgkDdiSystemDisplayWrite(ctx,Source,3,2,16,14,6);
        CHECK(!memcmp(Before,Fb,sizeof(Fb)));
        DXGK_DISPLAY_INFORMATION returned={};
        CHECK(Registered.DxgkDdiStopDeviceAndReleasePostDisplayOwnership(ctx,0,&returned)==STATUS_SUCCESS);
        CHECK(Unmaps==2 && returned.Width==16 && Fb[0]==0);
        CHECK(Registered.DxgkDdiStopDevice(ctx)==STATUS_SUCCESS && Unmaps==2);
        Registered.DxgkDdiRemoveDevice(ctx);
        CHECK(Allocations==Frees && Mdls==MdlFrees && Locks==Unlocks);
    }
    TestPowerLifecycle();
    return Failure;
}
