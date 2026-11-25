/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define _KERNEL32_
#include "Umd.c"
/* The product component and actual WDK function types execute unchanged.
   Only KMT, Win32 services, GPU completion and the separately verified CPU compiler are fixtures. */
volatile unsigned MtkMaliUmdAssertionCount;
#define CHECK(E) do{++MtkMaliUmdAssertionCount;if(!(E))return __LINE__;}while(0)
static UINT MockError,GateDepth;
#define EXPECT(E) do{++MtkMaliUmdAssertionCount;if(!(E) && !MockError)MockError=__LINE__;}while(0)
enum {OpenAdapter=1,CreateDevice,CreateContext,CreateQueue,CreateAllocation,MapVa,LockBo,UnlockBo,
      ResidentBo,WaitGpu,SubmitGpu,SignalGpu,FreeVa,DestroyAllocation,DestroyContext,DestroyQueue,
      DestroyDevice,CloseAdapter,HeapNew,NewEvent,EventClose,WaitEvent,CacheBo,IdleGpu,CompileShader,CompilePipeline,CompilePreload,OpCount};
static UINT Calls[OpCount],FailOp,FailAt,Sticky;
static int Fail(UINT Op) {
    UINT N=++Calls[Op];
    return Op==FailOp && (Sticky?N>=FailAt:N==FailAt);
}
/* Real DXBC/Mesa compilation has its own host ASan/UBSan suite. These
   fixtures exercise the production ARM64 DDI ownership/error boundary. */
static UINT CompilerLive;
static int CompilerFailure=T70_SHADER_UNSUPPORTED;
static struct t70_shader_token_request LastShaderRequest;
static UINT LastPairVertex,LastPairPixel,LastPairInputMask;
static struct t70_shader_io_types LastPairTypes;
static unsigned char CompilerCode[2][8192];
static SIZE_T CompilerCodeBytes[2]={24,16};
static struct t70_shader CompilerMetadata[2],PreloadMetadata;
static unsigned char PreloadCode[8192];
static SIZE_T PreloadCodeBytes=24;
static struct t70_shader_fb_preload_request LastPreloadRequest;
static NTSTATUS IdleResult;
int t70_shader_compile_tokens(const struct t70_shader_token_request *R,
    struct t70_shader *Out,char *Message,size_t MessageSize) {
    (void)Message;(void)MessageSize;
    EXPECT(GateDepth>=1 && R && Out && !Out->binary && !Out->spirv);
    if(Fail(CompileShader))return CompilerFailure;
    LastShaderRequest=*R;
    EXPECT(R->abi==T70_SHADER_ABI && R->gpu_id==T70_SHADER_GPU_ID && R->tokens_size>=8);
    const UINT *Code=R->tokens;EXPECT(Code[1]*4u==R->tokens_size && Code[0]>>16==R->stage);
    Out->binary=CompilerCode[R->stage];Out->binary_size=CompilerCodeBytes[R->stage];Out->stage=R->stage;Out->arch=9;++CompilerLive;
    return T70_SHADER_OK;
}
void t70_shader_free(struct t70_shader *S) {
    if(!S)return;
    if(S->binary){EXPECT((S->binary==CompilerCode[0] || S->binary==CompilerCode[1] || S->binary==PreloadCode) && CompilerLive);--CompilerLive;}
    ZeroMemory(S,sizeof(*S));
}
int t70_shader_compile_token_pipeline(const struct t70_shader_token_request *V,
    const struct t70_shader_token_request *P,struct t70_shader_pipeline *Out,char *Message,size_t MessageSize) {
    (void)Message;(void)MessageSize;EXPECT(GateDepth>=1 && V && P && Out);
    EXPECT(V->stage==T70_SHADER_VERTEX && P->stage==T70_SHADER_PIXEL);
    if(Fail(CompilePipeline))return CompilerFailure;
    EXPECT(!memcmp(&V->types,&P->types,sizeof(V->types)));LastPairTypes=V->types;
    LastPairVertex=((const UINT *)V->tokens)[0];LastPairPixel=((const UINT *)P->tokens)[0];
    LastPairInputMask=V->input_count?V->inputs[0].mask:0;
    Out->vertex=CompilerMetadata[T70_SHADER_VERTEX];Out->pixel=CompilerMetadata[T70_SHADER_PIXEL];
    Out->vertex.binary=CompilerCode[T70_SHADER_VERTEX];Out->pixel.binary=CompilerCode[T70_SHADER_PIXEL];
    Out->vertex.binary_size=CompilerCodeBytes[T70_SHADER_VERTEX];Out->pixel.binary_size=CompilerCodeBytes[T70_SHADER_PIXEL];
    Out->varying_stride=16;CompilerLive+=2;
    return T70_SHADER_OK;
}
int t70_shader_compile_framebuffer_preload(const struct t70_shader_fb_preload_request *R,
    struct t70_shader *Out,char *Message,size_t MessageSize) {
    (void)Message;(void)MessageSize;
    EXPECT(GateDepth>=1 && R && Out && !Out->binary && !Out->spirv);
    EXPECT(R->abi==T70_SHADER_ABI && R->gpu_id==T70_SHADER_GPU_ID && R->sample_count==1 &&
        R->array_layers==1 && !R->reserved && R->format>=T70_SHADER_FB_RGBA8_UNORM && R->format<=T70_SHADER_FB_BGRX8_UNORM);
    LastPreloadRequest=*R;
    if(Fail(CompilePreload))return CompilerFailure;
    *Out=PreloadMetadata;Out->binary=PreloadCode;Out->binary_size=PreloadCodeBytes;++CompilerLive;
    return T70_SHADER_OK;
}
void t70_shader_free_pipeline(struct t70_shader_pipeline *P) {
    if(!P)return;t70_shader_free(&P->vertex);t70_shader_free(&P->pixel);ZeroMemory(P,sizeof(*P));
}
#define OS_BEGIN(OP) do{if(Fail(OP))return (NTSTATUS)0xc000009au;}while(0)
typedef struct {
    UINT Live,Locked,Mapped,Resident,Uses,GpuPattern,GpuDirty;
    uint64_t Va,Bytes,Protection;
    UINT CacheWrites;
    unsigned char *Cpu;
    MALI_ALLOCATION_DESC Desc;
} TEST_BO;
typedef struct {UINT Live,Queued,Signaled,Uses,HandleCount;D3DKMT_HANDLE Handles[1+MALI_COMMAND_MAX_RESOURCES];} TEST_EVENT;
static TEST_BO Bos[160];
static TEST_EVENT Events[64];
static unsigned char Backing[16u*1024u*1024u],Heap[256u*1024u];
static SIZE_T BackingUsed,HeapUsed;
static void *HeapPointers[512];
static SIZE_T HeapSizes[512];
static UINT HeapLive[512],HeapCount,GateInitialized,GateDeleted,ErrorCalls,ReenterError;
static UINT AdapterLive,DeviceLive,ContextLive,QueueLive,AsyncPaging,PartialResidentFailure;
static UINT64 PagingValue,PagingNext,WaitedFence;
static HRESULT ErrorCode;
static D3D10DDI_DEVICEFUNCS Functions;
static D3D10DDI_HDEVICE ErrorDevice;
static D3DKMT_HANDLE LastSubmission[1+MALI_COMMAND_MAX_RESOURCES];
static UINT LastSubmitted,LastSubmissionCount;
static UINT FlagBits(const void *P) {UINT V;CopyMemory(&V,P,sizeof(V));return V;}
static TEST_BO *TestBo(D3DKMT_HANDLE Handle) {
    UINT I=Handle-0x1000u;EXPECT(I<160 && Bos[I].Live);return I<160?&Bos[I]:&Bos[0];
}
static TEST_BO *TestVa(uint64_t Va) {
    for(UINT I=0;I<160;I++)if(Bos[I].Live && Bos[I].Va==Va)return &Bos[I];
    EXPECT(0);return &Bos[0];
}
static UINT LiveBos(void) {UINT N=0;for(UINT I=0;I<160;I++)N+=Bos[I].Live;return N;}
static UINT LiveHeap(void) {UINT N=0;for(UINT I=0;I<HeapCount;I++)N+=HeapLive[I];return N;}
static UINT LiveEvents(void) {UINT N=0;for(UINT I=0;I<64;I++)N+=Events[I].Live;return N;}
HANDLE WINAPI GetProcessHeap(void) {return (HANDLE)(uintptr_t)0x8000;}
LPVOID WINAPI HeapAlloc(HANDLE H,DWORD Flags,SIZE_T Bytes) {
    EXPECT(H==(HANDLE)(uintptr_t)0x8000 && Flags==HEAP_ZERO_MEMORY && Bytes);
    if(Fail(HeapNew))return 0;
    Bytes=(Bytes+15u)&~(SIZE_T)15u;
    EXPECT(HeapUsed+Bytes<=sizeof(Heap) && HeapCount<512);
    if(HeapUsed+Bytes>sizeof(Heap) || HeapCount==512)return 0;
    void *P=Heap+HeapUsed;HeapUsed+=Bytes;ZeroMemory(P,Bytes);
    HeapPointers[HeapCount]=P;HeapSizes[HeapCount]=Bytes;HeapLive[HeapCount++]=1;return P;
}
BOOL WINAPI HeapFree(HANDLE H,DWORD Flags,LPVOID P) {
    EXPECT(H==(HANDLE)(uintptr_t)0x8000 && !Flags && P);
    for(UINT I=0;I<HeapCount;I++)if(HeapPointers[I]==P){EXPECT(HeapLive[I]);HeapLive[I]=0;memset(P,0xdd,HeapSizes[I]);return TRUE;}
    EXPECT(0);return FALSE;
}
BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION C,DWORD Spin,DWORD Flags) {
    EXPECT(!Spin && !Flags && !GateInitialized);ZeroMemory(C,sizeof(*C));GateInitialized=1;return TRUE;
}
VOID WINAPI EnterCriticalSection(LPCRITICAL_SECTION C) {(void)C;EXPECT(GateInitialized);++GateDepth;}
VOID WINAPI LeaveCriticalSection(LPCRITICAL_SECTION C) {(void)C;EXPECT(GateDepth>0);if(GateDepth)--GateDepth;}
VOID WINAPI DeleteCriticalSection(LPCRITICAL_SECTION C) {(void)C;EXPECT(GateInitialized && !GateDepth);GateInitialized=0;++GateDeleted;}
HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES Security,BOOL Manual,BOOL Initial,LPCWSTR Name) {
    EXPECT(!Security && Manual && !Initial && !Name);if(Fail(NewEvent))return 0;
    for(UINT I=0;I<64;I++)if(!Events[I].Live){ZeroMemory(&Events[I],sizeof(Events[I]));Events[I].Live=1;return &Events[I];}
    EXPECT(0);return 0;
}
BOOL WINAPI CloseHandle(HANDLE Handle) {
    TEST_EVENT *E=Handle;EXPECT(E>=Events && E<Events+64 && E->Live && (!E->Queued || E->Signaled));
    if(Fail(EventClose))return FALSE;E->Live=0;return TRUE;
}
static void Complete(void);
DWORD WINAPI WaitForSingleObject(HANDLE Handle,DWORD Milliseconds) {
    TEST_EVENT *E=Handle;EXPECT(E>=Events && E<Events+64 && E->Live && E->Queued && (!Milliseconds || Milliseconds==INFINITE));
    if(Fail(WaitEvent))return WAIT_FAILED;
    if(Milliseconds==INFINITE)Complete();
    return E->Signaled?WAIT_OBJECT_0:WAIT_TIMEOUT;
}
NTSTATUS APIENTRY D3DKMTOpenAdapterFromLuid(const D3DKMT_OPENADAPTERFROMLUID *A) {
    OS_BEGIN(OpenAdapter);EXPECT(A->AdapterLuid.LowPart==1 && !A->AdapterLuid.HighPart && !AdapterLive);
    AdapterLive=1;((D3DKMT_OPENADAPTERFROMLUID *)A)->hAdapter=0x10;return 0;
}
NTSTATUS APIENTRY D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *A) {
    OS_BEGIN(CreateDevice);EXPECT(A->hAdapter==0x10 && !FlagBits(&A->Flags) && AdapterLive && !DeviceLive);
    DeviceLive=1;A->hDevice=0x20;return 0;
}
NTSTATUS APIENTRY D3DKMTCreateContextVirtual(D3DKMT_CREATECONTEXTVIRTUAL *A) {
    OS_BEGIN(CreateContext);EXPECT(A->hDevice==0x20 && !A->NodeOrdinal && A->EngineAffinity==1 && !A->Flags.Value &&
        !A->PrivateDriverDataSize && !A->pPrivateDriverData && A->ClientHint==D3DKMT_CLIENTHINT_DX10 && DeviceLive && !ContextLive);
    ContextLive=1;A->hContext=0x30;return 0;
}
NTSTATUS APIENTRY D3DKMTCreatePagingQueue(D3DKMT_CREATEPAGINGQUEUE *A) {
    OS_BEGIN(CreateQueue);EXPECT(A->hDevice==0x20 && A->Priority==D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL &&
        !A->PhysicalAdapterIndex && !QueueLive && DeviceLive);
    QueueLive=1;A->hPagingQueue=0x40;A->hSyncObject=0x50;A->FenceValueCPUVirtualAddress=&PagingValue;return 0;
}
NTSTATUS APIENTRY D3DKMTCreateAllocation2(D3DKMT_CREATEALLOCATION *A) {
    OS_BEGIN(CreateAllocation);EXPECT(GateDepth>=1 && A->hDevice==0x20 && A->Flags.CreateResource &&
        A->Flags.CreateShared && A->Flags.NtSecuritySharing && FlagBits(&A->Flags)==0x43 && !A->hResource &&
        !A->pPrivateRuntimeData && !A->PrivateRuntimeDataSize && !A->pPrivateDriverData && !A->PrivateDriverDataSize && A->NumAllocations==1);
    D3DDDI_ALLOCATIONINFO2 *I=A->pAllocationInfo2;EXPECT(I && !I->pSystemMem && !I->Flags.Value && I->PrivateDriverDataSize==40);
    MALI_ALLOCATION_DESC *D=I->pPrivateDriverData;EXPECT(D && D->Version==1 && D->Bytes==40 && D->Flags==3 && D->Size && !(D->Size&4095u));
    for(UINT J=0;J<160;J++)if(!Bos[J].Live) {
        TEST_BO *B=&Bos[J];ZeroMemory(B,sizeof(*B));B->Live=1;B->Bytes=D->Size;B->Desc=*D;
        EXPECT(BackingUsed+B->Bytes<=sizeof(Backing));B->Cpu=Backing+BackingUsed;BackingUsed+=B->Bytes;
        /* Only observable command/descriptor bytes need a poison fixture. */
        if(B->Bytes==4096)memset(B->Cpu,0xa7,4096);
        A->hResource=0x2000+J;I->hAllocation=0x1000+J;return 0;
    }
    EXPECT(0);return (NTSTATUS)0xc000009au;
}
NTSTATUS APIENTRY D3DKMTMapGpuVirtualAddress(D3DDDI_MAPGPUVIRTUALADDRESS *A) {
    OS_BEGIN(MapVa);TEST_BO *B=TestBo(A->hAllocation);
    EXPECT(A->hPagingQueue==0x40 && !A->BaseAddress && A->MinimumAddress==4096 &&
        A->MaximumAddress==MALI_JM_VA_LIMIT && !(A->MaximumAddress&4095u) && !A->OffsetInPages && A->SizeInPages==B->Bytes/4096 &&
        A->Protection.Value<=2 && !A->DriverProtection && !A->Reserved0 && !A->Reserved1 && !B->Mapped);
    B->Protection=A->Protection.Value;EXPECT(B->Protection!=2 || B->Desc.Kind==MALI_ALLOCATION_BUFFER);
    B->Va=UINT64_C(0x100000)+(uint64_t)(A->hAllocation-0x1000)*0x200000;B->Mapped=1;
    A->VirtualAddress=B->Va;A->PagingFenceValue=++PagingNext;return AsyncPaging?0x103:0;
}
NTSTATUS APIENTRY D3DKMTLock2(D3DKMT_LOCK2 *A) {
    OS_BEGIN(LockBo);TEST_BO *B=TestBo(A->hAllocation);EXPECT(A->hDevice==0x20 && !A->Flags.Value && !B->Locked && !B->Uses);
    B->Locked=1;A->pData=B->Cpu;return 0;
}
NTSTATUS APIENTRY D3DKMTUnlock2(const D3DKMT_UNLOCK2 *A) {
    OS_BEGIN(UnlockBo);TEST_BO *B=TestBo(A->hAllocation);EXPECT(A->hDevice==0x20 && B->Locked);
    B->Locked=0;return 0;
}
NTSTATUS APIENTRY D3DKMTInvalidateCache(const D3DKMT_INVALIDATECACHE *A) {
    OS_BEGIN(CacheBo);TEST_BO *B=TestBo(A->hAllocation);
    EXPECT(A->hDevice==0x20 && B->Locked && !B->Uses && !A->Offset && A->Length==B->Bytes);
    ++B->CacheWrites;
    if(B->GpuDirty){memset(B->Cpu,(int)B->GpuPattern,(SIZE_T)B->Bytes);B->GpuDirty=0;}
    return 0;
}
NTSTATUS APIENTRY D3DKMTWaitForIdle(const D3DKMT_WAITFORIDLE *A) {
    OS_BEGIN(IdleGpu);EXPECT(A->hDevice==0x20 && GateDepth>=1);
    if(!IdleResult)for(UINT I=0;I<160;I++)if(Bos[I].Live && Bos[I].Protection!=1) {
        UINT Held=0;D3DKMT_HANDLE H=0x1000+I;
        for(UINT E=0;E<64;E++)if(Events[E].Live && Events[E].Uses)
            for(UINT J=0;J<Events[E].HandleCount;J++)Held+=Events[E].Handles[J]==H;
        if(LastSubmitted)for(UINT J=0;J<LastSubmissionCount;J++)Held+=LastSubmission[J]==H;
        Bos[I].Uses=Held;
    }
    return IdleResult;
}
NTSTATUS APIENTRY D3DKMTMakeResident(D3DDDI_MAKERESIDENT *A) {
    TEST_BO *B=TestBo(A->AllocationList[0]);EXPECT(A->hPagingQueue==0x40 && A->NumAllocations==1 &&
        !A->PriorityList && !A->Flags.Value && !B->Resident);
    if(Fail(ResidentBo)){
        if(PartialResidentFailure){B->Resident=1;A->PagingFenceValue=++PagingNext;}else A->NumAllocations=0;
        return (NTSTATUS)0xc000009au;
    }
    if(B->Protection!=1)EXPECT(B->CacheWrites && !B->Locked);
    B->Resident=1;A->PagingFenceValue=++PagingNext;return AsyncPaging?0x103:0;
}
NTSTATUS APIENTRY D3DKMTWaitForSynchronizationObjectFromGpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *A) {
    OS_BEGIN(WaitGpu);EXPECT(A->hContext==0x30 && A->ObjectCount==1 && A->ObjectHandleArray[0]==0x50 &&
        A->MonitoredFenceValueArray && *A->MonitoredFenceValueArray==PagingNext);
    WaitedFence=*A->MonitoredFenceValueArray;return 0;
}
static MALI_COMMAND_WORKSPACE PrepareWorkspace;
static unsigned char PrepareShadow[MALI_COMMAND_SHADOW_BYTES];
static MALI_RESULT PinWrite(void *Context,uint64_t Va,uint32_t Bytes) {
    (void)Context;(void)Va;(void)Bytes;EXPECT(0);return MaliInvalid;
}
static MALI_RESULT PinResource(void *Context,uint64_t Va,uint32_t Bytes,uint32_t Access) {
    (void)Context;TEST_BO *B=TestVa(Va);
    UINT Allowed=MaliPteRead|((B->Protection&1)?MaliPteWrite:0)|((B->Protection&2)?MaliPteExecute:0);
    EXPECT(B->Live && B->Mapped && B->Resident && B->Bytes==Bytes && (Access&Allowed)==Access);
    return MaliOk;
}
NTSTATUS APIENTRY D3DKMTSubmitCommand(const D3DKMT_SUBMITCOMMAND *A) {
    OS_BEGIN(SubmitGpu);EXPECT(A->BroadcastContextCount==1 && A->BroadcastContext[0]==0x30 && !FlagBits(&A->Flags) &&
        (A->CommandLength==128 || A->CommandLength==512) && !A->NumPrimaries && !A->NumHistoryBuffers && WaitedFence==PagingNext && !LastSubmitted);
    const uint32_t *W=A->pPrivateDriverData;
    EXPECT(W && W[0]==2 && W[1]==A->PrivateDriverDataSize && W[3]<=MALI_COMMAND_MAX_RESOURCES &&
        W[1]==MALI_COMMAND_PRIVATE_BYTES(W[2],W[3]));
    UINT Raster=A->CommandLength==512;
    EXPECT(Raster?(W[2]==2 && W[3]>=4):(W[2]==1 && W[3]==3 && W[1]==80));
    TEST_BO *Cmd=TestVa(A->Commands);EXPECT(Cmd->Desc.Kind==MALI_ALLOCATION_BUFFER && Cmd->Bytes==4096);
    LastSubmission[0]=(D3DKMT_HANDLE)(0x1000+(Cmd-Bos));LastSubmissionCount=1+W[3];
    for(UINT J=0;J<W[3];J++) {
        const MALI_COMMAND_RESOURCE *Range=(const MALI_COMMAND_RESOURCE *)((const unsigned char *)W+16u+16u*W[2]+16u*J);
        TEST_BO *B=TestVa(Range->Va);EXPECT(Range->Bytes==B->Bytes);
        if(!Raster) {
            EXPECT(Range->Access==(MaliPteRead|MaliPteWrite));
            EXPECT(J==0?B->Bytes==4096:J==1?B->Desc.Kind==MALI_ALLOCATION_LINEAR_2D:B->Bytes==MALI_COMMAND_CLEAR_HEAP_BYTES);
        }
        LastSubmission[J+1]=(D3DKMT_HANDLE)(0x1000+(B-Bos));
    }
    for(UINT I=0;I<LastSubmissionCount;I++) {
        TEST_BO *B=TestBo(LastSubmission[I]);EXPECT(B->Resident && B->Mapped && !B->Locked);++B->Uses;
    }
    UMD_DEVICE *D=ErrorDevice.pDrvPrivate;
    EXPECT(MaliCommandsPrepareResources(&D->Caps,Cmd->Cpu,A->Commands,A->CommandLength,
        A->pPrivateDriverData,A->PrivateDriverDataSize,PrepareShadow,UINT64_C(0x8000000000),sizeof(PrepareShadow),
        &PrepareWorkspace,PinWrite,PinResource,0)==MaliOk);
    EXPECT(PrepareWorkspace.JobCount==(Raster?2u:1u) && PrepareWorkspace.ResourceCount==W[3] && !PrepareWorkspace.WriteCount);
    if(Raster) {
        EXPECT(PrepareWorkspace.Jobs[0].Bytes==384 && PrepareWorkspace.Jobs[1].Bytes==64);
        EXPECT(PrepareWorkspace.Packet.Steps[0].Slot==1 && PrepareWorkspace.Packet.Steps[1].Slot==0);
        for(UINT I=512;I<4096;I++)EXPECT(!Cmd->Cpu[I]);
    }
    LastSubmitted=1;return 0;
}
NTSTATUS APIENTRY D3DKMTSignalSynchronizationObject2(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 *A) {
    OS_BEGIN(SignalGpu);EXPECT(A->hContext==0x30 && !A->ObjectCount && !A->BroadcastContextCount && A->Flags.Value==2 && A->CpuEventHandle);
    TEST_EVENT *E=A->CpuEventHandle;EXPECT(E>=Events && E<Events+64 && E->Live && !E->Queued);
    E->Queued=1;E->Uses=1;E->HandleCount=LastSubmissionCount;CopyMemory(E->Handles,LastSubmission,sizeof(LastSubmission));LastSubmitted=0;return 0;
}
NTSTATUS APIENTRY D3DKMTFreeGpuVirtualAddress(const D3DKMT_FREEGPUVIRTUALADDRESS *A) {
    OS_BEGIN(FreeVa);TEST_BO *B=TestVa(A->BaseAddress);
    EXPECT(A->hAdapter==0x10 && A->Size==B->Bytes && !B->Uses && B->Mapped && (PagingValue>=PagingNext || !AsyncPaging));
    B->Mapped=0;return 0;
}
NTSTATUS APIENTRY D3DKMTDestroyAllocation2(const D3DKMT_DESTROYALLOCATION2 *A) {
    OS_BEGIN(DestroyAllocation);EXPECT(A->hDevice==0x20 && A->hResource>=0x2000 && !A->AllocationCount && !A->phAllocationList && !A->Flags.Value);
    TEST_BO *B=TestBo(A->hResource-0x1000);EXPECT(!B->Locked && !B->Uses && !B->Mapped);B->Live=0;return 0;
}
NTSTATUS APIENTRY D3DKMTDestroyContext(const D3DKMT_DESTROYCONTEXT *A) {
    OS_BEGIN(DestroyContext);EXPECT(A->hContext==0x30 && ContextLive && !LiveBos() && !LiveEvents() && !LastSubmitted);
    ContextLive=0;return 0;
}
NTSTATUS APIENTRY D3DKMTDestroyPagingQueue(D3DDDI_DESTROYPAGINGQUEUE *A) {
    OS_BEGIN(DestroyQueue);EXPECT(A->hPagingQueue==0x40 && QueueLive && !LiveBos() && !ContextLive);QueueLive=0;return 0;
}
NTSTATUS APIENTRY D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *A) {
    OS_BEGIN(DestroyDevice);EXPECT(A->hDevice==0x20 && DeviceLive && !ContextLive && !QueueLive);DeviceLive=0;return 0;
}
NTSTATUS APIENTRY D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *A) {
    OS_BEGIN(CloseAdapter);EXPECT(A->hAdapter==0x10 && AdapterLive && !DeviceLive);AdapterLive=0;return 0;
}
static VOID APIENTRY TestError(D3D10DDI_HRTCORELAYER Core,HRESULT H) {
    EXPECT(Core.handle==(HANDLE)(uintptr_t)0x6000 && !GateDepth && FAILED(H));ErrorCode=H;++ErrorCalls;
    if(ReenterError){ReenterError=0;Functions.pfnFlush(ErrorDevice);}
}
static void Complete(void) {
    PagingValue=AsyncPaging?PagingNext:UINT64_MAX;
    for(UINT I=0;I<64;I++)if(Events[I].Live && Events[I].Queued && !Events[I].Signaled) {
        TEST_EVENT *E=&Events[I];E->Signaled=1;
        if(E->Uses){for(UINT J=0;J<E->HandleCount;J++) {
            TEST_BO *B=TestBo(E->Handles[J]);if(B->Desc.Kind==MALI_ALLOCATION_LINEAR_2D && B->GpuPattern)B->GpuDirty=1;
            EXPECT(B->Uses);--B->Uses;
        }E->Uses=0;}
    }
}
static void Reset(void) {
    EXPECT(!CompilerLive && !LiveBos() && !LiveEvents() && !LiveHeap() && !AdapterLive && !DeviceLive && !ContextLive && !QueueLive && !GateDepth && !GateInitialized);
    ZeroMemory(Calls,sizeof(Calls));ZeroMemory(Bos,sizeof(Bos));ZeroMemory(Events,sizeof(Events));
    ZeroMemory(HeapLive,sizeof(HeapLive));HeapUsed=BackingUsed=HeapCount=0;FailOp=FailAt=Sticky=0;
    GateDeleted=ErrorCalls=ReenterError=AsyncPaging=PartialResidentFailure=0;PagingValue=UINT64_MAX;PagingNext=WaitedFence=0;ErrorCode=S_OK;
    LastSubmitted=LastSubmissionCount=0;ZeroMemory(LastSubmission,sizeof(LastSubmission));
    CompilerFailure=T70_SHADER_UNSUPPORTED;ZeroMemory(&LastShaderRequest,sizeof(LastShaderRequest));
    LastPairVertex=LastPairPixel=LastPairInputMask=0;IdleResult=0;
    CompilerCodeBytes[0]=24;CompilerCodeBytes[1]=16;PreloadCodeBytes=24;
    ZeroMemory(&LastPreloadRequest,sizeof(LastPreloadRequest));
    ZeroMemory(&PreloadMetadata,sizeof(PreloadMetadata));
    PreloadMetadata.stage=T70_SHADER_PIXEL;PreloadMetadata.arch=9;PreloadMetadata.work_registers=32;
    PreloadMetadata.preload=UINT64_C(0x000f000000000000);PreloadMetadata.flags=T70_SHADER_FLAG_FTZ32;
    PreloadMetadata.fau_reserved=T70_SHADER_FB_PRELOAD_FAU_WORDS;PreloadMetadata.fau_count=4;
    PreloadMetadata.fau_const_mask[0]=(1u<<2)|(1u<<3);
    PreloadMetadata.fau_words[2]=0x12345678u;PreloadMetadata.fau_words[3]=0x90abcdefu;
    for(UINT I=0;I<sizeof(PreloadCode);I++)PreloadCode[I]=(unsigned char)(I*11u+37u);
    ZeroMemory(CompilerMetadata,sizeof(CompilerMetadata));
    CompilerMetadata[0].stage=T70_SHADER_PIXEL;CompilerMetadata[1].stage=T70_SHADER_VERTEX;
    CompilerMetadata[0].arch=CompilerMetadata[1].arch=9;
    CompilerMetadata[0].work_registers=48;CompilerMetadata[1].work_registers=18;
    CompilerMetadata[0].preload=UINT64_C(0xaa55000000000000);CompilerMetadata[1].preload=UINT64_C(0x1234000000000000);
    CompilerMetadata[0].flags=T70_SHADER_FLAG_BARRIER|T70_SHADER_FLAG_FTZ32|T70_SHADER_FLAG_FTZ16;
    CompilerMetadata[1].flags=T70_SHADER_FLAG_IDVS|T70_SHADER_FLAG_SECONDARY|T70_SHADER_FLAG_FTZ32;
    CompilerMetadata[1].secondary_offset=CompilerMetadata[1].no_point_size_offset=8;
    CompilerMetadata[1].secondary_registers=40;CompilerMetadata[1].secondary_preload=UINT64_C(0xabcd000000000000);
    for(UINT Stage=0;Stage<2;Stage++)for(UINT I=0;I<sizeof(CompilerCode[Stage]);I++)
        CompilerCode[Stage][I]=(unsigned char)(I*17u+Stage*73u);
}
static MALI_CAPS Caps(void) {
    MALI_CAPS C={0};C.GpuId=0x90930000;C.Tilers=1;C.JobSlots=1;C.JobFeatures[0]=1u<<9;return C;
}
static HRESULT Init(UMD_DEVICE *D) {
    ZeroMemory(D,sizeof(*D));MALI_CAPS C=Caps();LUID L={1,0};D3D10DDI_HRTCORELAYER Core={(HANDLE)(uintptr_t)0x6000};
    ErrorDevice.pDrvPrivate=D;return MtkMaliUmdInitialize(D,L,&C,0,Core,TestError);
}
static D3D10DDIARG_CREATERESOURCE ResourceArgs(D3D10DDI_MIPINFO *M,DXGI_FORMAT Format,UINT Width,UINT Height) {
    D3D10DDIARG_CREATERESOURCE A={0};ZeroMemory(M,sizeof(*M));
    M->TexelWidth=M->PhysicalWidth=Width;M->TexelHeight=M->PhysicalHeight=Height;M->TexelDepth=M->PhysicalDepth=1;
    A.pMipInfoList=M;A.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;A.Usage=D3D10_DDI_USAGE_DEFAULT;
    A.BindFlags=D3D10_DDI_BIND_RENDER_TARGET;A.Format=Format;A.SampleDesc.Count=1;A.MipLevels=A.ArraySize=1;return A;
}
static void Create(UMD_DEVICE *D,UMD_TOKEN *R,UMD_TOKEN *V,DXGI_FORMAT Format,UINT Width,UINT Height) {
    D3D10DDI_HDEVICE Device={D};D3D10DDI_HRESOURCE Resource={R};D3D10DDI_HRTRESOURCE Runtime={(HANDLE)(uintptr_t)0x5151};
    D3D10DDI_MIPINFO M;D3D10DDIARG_CREATERESOURCE A=ResourceArgs(&M,Format,Width,Height);
    EXPECT(Functions.pfnCalcPrivateResourceSize(Device,&A)==sizeof(*R));
    Functions.pfnCreateResource(Device,&A,Resource,Runtime);
    if(!R->Id)return;
    D3D10DDIARG_CREATERENDERTARGETVIEW B={0};B.hDrvResource=Resource;B.Format=Format;
    B.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;B.Tex2D.ArraySize=1;
    D3D10DDI_HRENDERTARGETVIEW View={V};D3D10DDI_HRTRENDERTARGETVIEW RtView={0};
    EXPECT(Functions.pfnCalcPrivateRenderTargetViewSize(Device,&B)==sizeof(*V));
    Functions.pfnCreateRenderTargetView(Device,&B,View,RtView);
}
static void Clear(UMD_DEVICE *D,UMD_TOKEN *V) {
    D3D10DDI_HDEVICE Device={D};D3D10DDI_HRENDERTARGETVIEW View={V};
    /* Bitwise IEEE 0, 0.5, 1 and +Inf; no host float oracle needed here. */
    uint32_t Color[4]={0,0x3f000000,0x3f800000,0x7f800000};Functions.pfnClearRenderTargetView(Device,View,(FLOAT *)Color);
}
static void Destroy(UMD_DEVICE *D,UMD_TOKEN *R,UMD_TOKEN *V) {
    D3D10DDI_HDEVICE Device={D};D3D10DDI_HRESOURCE Resource={R};D3D10DDI_HRENDERTARGETVIEW View={V};
    if(R->Id)Functions.pfnDestroyResource(Device,Resource);
    if(V->Id)Functions.pfnDestroyRenderTargetView(Device,View);
}

static void CreateOnly(UMD_DEVICE *D,UMD_TOKEN *R,const D3D10DDIARG_CREATERESOURCE *A) {
    D3D10DDI_HDEVICE Device={D};D3D10DDI_HRESOURCE Resource={R};D3D10DDI_HRTRESOURCE Runtime={0};
    ZeroMemory(R,sizeof(*R));
    EXPECT(Functions.pfnCalcPrivateResourceSize(Device,A)==sizeof(*R));
    Functions.pfnCreateResource(Device,A,Resource,Runtime);
}
static void DestroyOnly(UMD_DEVICE *D,UMD_TOKEN *R) {
    D3D10DDI_HDEVICE Device={D};D3D10DDI_HRESOURCE Resource={R};
    if(R->Id)Functions.pfnDestroyResource(Device,Resource);
}
static D3D10DDIARG_CREATERESOURCE BufferArgs(D3D10DDI_MIPINFO *M,UINT Bytes,UINT Usage,UINT Bind,UINT Map) {
    D3D10DDIARG_CREATERESOURCE A=ResourceArgs(M,DXGI_FORMAT_UNKNOWN,Bytes,1);
    A.ResourceDimension=D3D10DDIRESOURCE_BUFFER;A.Usage=Usage;A.BindFlags=Bind;A.MapFlags=Map;return A;
}
static int TestTransfers(void) {
    UMD_DEVICE D;D3D10DDI_HDEVICE Device={&D};
    D3D10DDI_HRESOURCE Source,Dest;UMD_TOKEN R={0},S={0},V={0};
    D3D10DDI_MIPINFO M;D3D10DDIARG_CREATERESOURCE A;
    unsigned char Data[512];for(UINT I=0;I<sizeof(Data);I++)Data[I]=(unsigned char)(I*37u+11u);
    D3D10_DDIARG_SUBRESOURCE_UP Initial={Data,80,0};
    /* Actual initial upload: row padding and page tail must stay unchanged. */
    const DXGI_FORMAT Formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM};
    for(UINT F=0;F<3;F++) {
        Reset();CHECK(Init(&D)==S_OK);A=ResourceArgs(&M,Formats[F],17,3);A.pInitialDataUP=&Initial;
        CreateOnly(&D,&R,&A);CHECK(R.Id && D.ResourceCount==1 && D.Resources->Live && !D.Resources->Initializing);
        TEST_BO *B=TestBo(D.Resources->Bo.Allocation);
        for(UINT I=0;I<4096;I++)CHECK(B->Cpu[I]==(I<384 && I%128<68?Data[I/128*80+I%128]:0xa7));
        CHECK(Calls[LockBo]==1 && Calls[CacheBo]==2 && Calls[UnlockBo]==1 && !MockError);
        A.pInitialDataUP=0;A.Usage=D3D10_DDI_USAGE_STAGING;A.BindFlags=0;A.MapFlags=D3D10_DDI_CPU_ACCESS_MASK;
        CreateOnly(&D,&S,&A);CHECK(S.Id);
        Source.pDrvPrivate=&R;Dest.pDrvPrivate=&S;Functions.pfnResourceCopy(Device,Dest,Source);
        D3D10DDI_MAPPED_SUBRESOURCE Map={0};
        Functions.pfnStagingResourceMap(Device,Dest,0,D3D10_DDI_MAP_READ,D3D10_DDI_MAP_FLAG_DONOTWAIT,&Map);
        CHECK(Map.pData && Map.RowPitch==128 && Map.DepthPitch==384 && !ErrorCalls);
        for(UINT I=0;I<4096;I++)CHECK(((unsigned char *)Map.pData)[I]==(I<384 && I%128<68?Data[I/128*80+I%128]:0xa7));
        Functions.pfnStagingResourceUnmap(Device,Dest,0);
        /* Partial update preserves pixels around the rectangle and row padding. */
        D3D10_DDI_BOX Box={2,1,0,7,3,1};
        Source.pDrvPrivate=&R;Functions.pfnResourceUpdateSubresourceUP(Device,Source,0,&Box,Data+300,24,0);
        for(UINT Y=0;Y<3;Y++)for(UINT X=0;X<128;X++) {
            unsigned char Expected=X>=68?0xa7:Data[Y*80+X];
            if(Y>=1 && X>=8 && X<28)Expected=Data[300+(Y-1)*24+X-8];
            CHECK(B->Cpu[Y*128+X]==Expected);
        }
        /* Empty source boxes and empty uploads do not lock, read or write. */
        UINT Locks=Calls[LockBo];Box.right=Box.left;
        Functions.pfnResourceCopyRegion(Device,Dest,0,0,0,0,Source,0,&Box);
        Functions.pfnResourceUpdateSubresourceUP(Device,Source,0,&Box,0,0,0);
        CHECK(Calls[LockBo]==Locks && !ErrorCalls && !MockError);
        DestroyOnly(&D,&R);DestroyOnly(&D,&S);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Buffer coordinates are bytes, while constant buffers require whole updates. */
    Reset();CHECK(Init(&D)==S_OK);
    A=BufferArgs(&M,33,D3D10_DDI_USAGE_IMMUTABLE,D3D10_DDI_BIND_VERTEX_BUFFER,0);
    Initial.SysMemPitch=0;A.pInitialDataUP=&Initial;CreateOnly(&D,&R,&A);
    CHECK(R.Id && D.Resources->Desc.Kind==MALI_ALLOCATION_BUFFER && D.Resources->LogicalBytes==33 && LiveBos()==1);
    TEST_BO *Original=TestBo(D.Resources->Bo.Allocation);
    for(UINT I=0;I<4096;I++)CHECK(Original->Cpu[I]==(I<33?Data[I]:0xa7));
    A=BufferArgs(&M,65,D3D10_DDI_USAGE_STAGING,0,D3D10_DDI_CPU_ACCESS_MASK);CreateOnly(&D,&S,&A);
    Source.pDrvPrivate=&R;Dest.pDrvPrivate=&S;D3D10_DDI_BOX Box={3,0,0,19,1,1};
    Functions.pfnResourceCopyRegion(Device,Dest,0,7,0,0,Source,0,&Box);
    D3D10DDI_MAPPED_SUBRESOURCE Map={0};
    Functions.pfnStagingResourceMap(Device,Dest,0,D3D10_DDI_MAP_READWRITE,0,&Map);
    CHECK(Map.pData && Map.RowPitch==65 && Map.DepthPitch==65);
    for(UINT I=0;I<4096;I++)CHECK(((unsigned char *)Map.pData)[I]==(I>=7 && I<23?Data[I-4]:0xa7));
    memset(Map.pData,0x71,65);UINT Cache=Calls[CacheBo];
    Functions.pfnStagingResourceUnmap(Device,Dest,0);CHECK(Calls[CacheBo]==Cache+1 && !ErrorCalls);
    DestroyOnly(&D,&R);DestroyOnly(&D,&S);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    Reset();CHECK(Init(&D)==S_OK);
    A=BufferArgs(&M,32,D3D10_DDI_USAGE_DEFAULT,D3D10_DDI_BIND_CONSTANT_BUFFER,0);CreateOnly(&D,&R,&A);
    Source.pDrvPrivate=&R;Functions.pfnDefaultConstantBufferUpdateSubresourceUP(Device,Source,0,0,Data,0,0);
    TEST_BO *B=TestBo(D.Resources->Bo.Allocation);
    for(UINT I=0;I<4096;I++)CHECK(B->Cpu[I]==(I<32?Data[I]:0xa7));
    DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Dynamic discard gets fresh backing; no-overwrite preserves other bytes. */
    const UINT Bindings[]={D3D10_DDI_BIND_VERTEX_BUFFER,D3D10_DDI_BIND_INDEX_BUFFER,
        D3D10_DDI_BIND_VERTEX_BUFFER|D3D10_DDI_BIND_INDEX_BUFFER,D3D10_DDI_BIND_CONSTANT_BUFFER,D3D10_DDI_BIND_SHADER_RESOURCE};
    for(UINT Bind=0;Bind<5;Bind++) {
        Reset();CHECK(Init(&D)==S_OK);
        A=BufferArgs(&M,32,D3D10_DDI_USAGE_DYNAMIC,Bindings[Bind],D3D10_DDI_CPU_ACCESS_WRITE);
        A.pInitialDataUP=&Initial;CreateOnly(&D,&R,&A);CHECK(R.Id);
        Original=TestBo(D.Resources->Bo.Allocation);Source.pDrvPrivate=&R;
        PFND3D10DDI_RESOURCEMAP MapFn=Bind<3?Functions.pfnDynamicIABufferMapDiscard:
            Bind==3?Functions.pfnDynamicConstantBufferMapDiscard:Functions.pfnDynamicResourceMapDiscard;
        PFND3D10DDI_RESOURCEUNMAP UnmapFn=Bind<3?Functions.pfnDynamicIABufferUnmap:
            Bind==3?Functions.pfnDynamicConstantBufferUnmap:Functions.pfnDynamicResourceUnmap;
        MapFn(Device,Source,0,D3D10_DDI_MAP_WRITE_DISCARD,0,&Map);
        CHECK(Map.pData && Map.pData!=Original->Cpu && !Original->Live && !D.Resources->OldCount && LiveBos()==1);
        memset(Map.pData,0x42,32);UnmapFn(Device,Source,0);
        if(Bind<3) {
            void *Previous=Map.pData;
            Functions.pfnDynamicIABufferMapNoOverwrite(Device,Source,0,D3D10_DDI_MAP_WRITE_NOOVERWRITE,0,&Map);
            CHECK(Map.pData==Previous);memset((unsigned char *)Map.pData+5,0x19,3);
            for(UINT I=0;I<32;I++)CHECK(((unsigned char *)Map.pData)[I]==(I>=5 && I<8?0x19:0x42));
            UnmapFn(Device,Source,0);
        }
        CHECK(!D.Resources->MapType && !D.Resources->Bo.Locked && !ErrorCalls && !MockError);
        DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Dynamic texture pitch differs from source row pitch and changes on discard. */
    Reset();CHECK(Init(&D)==S_OK);A=ResourceArgs(&M,Formats[0],17,3);
    A.Usage=D3D10_DDI_USAGE_DYNAMIC;A.BindFlags=D3D10_DDI_BIND_SHADER_RESOURCE;A.MapFlags=D3D10_DDI_CPU_ACCESS_WRITE;
    CreateOnly(&D,&R,&A);Source.pDrvPrivate=&R;
    Functions.pfnDynamicResourceMapDiscard(Device,Source,0,D3D10_DDI_MAP_WRITE_DISCARD,0,&Map);
    CHECK(Map.pData && Map.RowPitch==128 && Map.DepthPitch==384);
    memset(Map.pData,0x12,384);Functions.pfnDynamicResourceUnmap(Device,Source,0);
    DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Owner-retention unit fixture only; shader use of dynamic BOs is not implemented. */
    Reset();CHECK(Init(&D)==S_OK);
    A=BufferArgs(&M,32,D3D10_DDI_USAGE_DYNAMIC,D3D10_DDI_BIND_VERTEX_BUFFER,D3D10_DDI_CPU_ACCESS_WRITE);
    CreateOnly(&D,&R,&A);Source.pDrvPrivate=&R;
    UMD_RESOURCE *Owned=D.Resources;Original=TestBo(Owned->Bo.Allocation);Owned->Pending=1;Original->Uses=1;
    Functions.pfnDynamicIABufferMapDiscard(Device,Source,0,D3D10_DDI_MAP_WRITE_DISCARD,0,&Map);
    CHECK(Map.pData && Original->Live && Owned->OldCount==1 && LiveBos()==2);
    Functions.pfnDynamicIABufferUnmap(Device,Source,0);DestroyOnly(&D,&R);
    CHECK(Original->Live && LiveBos()==2 && MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY));
    Owned->Pending=0;Original->Uses=0;Functions.pfnFlush(Device);
    CHECK(!LiveBos() && MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Submitted clear -> blocking CPU copy -> staging map. GPU bytes are fixtures. */
    Reset();CHECK(Init(&D)==S_OK);Create(&D,&R,&V,Formats[0],17,3);
    Original=TestBo(D.Resources->Bo.Allocation);Original->GpuPattern=0x39;
    Clear(&D,&V);CHECK(D.PendingCount==1);
    EnterCriticalSection(&D.Gate);HRESULT H=UmdCpuIdle(&D,D.Resources,D3D10_DDI_MAP_FLAG_DONOTWAIT);LeaveCriticalSection(&D.Gate);
    CHECK(H==DXGI_DDI_ERR_WASSTILLDRAWING && !Calls[IdleGpu] && D.PendingCount==1 && Original->Uses);
    A=ResourceArgs(&M,Formats[0],17,3);A.Usage=D3D10_DDI_USAGE_STAGING;A.BindFlags=0;A.MapFlags=D3D10_DDI_CPU_ACCESS_READ;
    CreateOnly(&D,&S,&A);Source.pDrvPrivate=&R;Dest.pDrvPrivate=&S;
    Functions.pfnResourceCopy(Device,Dest,Source);
    CHECK(Calls[IdleGpu]==1 && !D.PendingCount && !Original->GpuDirty && !Original->Uses && !ErrorCalls);
    CHECK(!Functions.pfnResourceIsStagingBusy(Device,Dest));
    Functions.pfnStagingResourceMap(Device,Dest,0,D3D10_DDI_MAP_READ,0,&Map);
    for(UINT Y=0;Y<3;Y++)for(UINT X=0;X<128;X++)CHECK(((unsigned char *)Map.pData)[Y*128+X]==(X<68?0x39:0xa7));
    CHECK(Functions.pfnResourceIsStagingBusy(Device,Dest));Functions.pfnStagingResourceUnmap(Device,Dest,0);
    CHECK(!Functions.pfnResourceIsStagingBusy(Device,Dest));
    Destroy(&D,&R,&V);DestroyOnly(&D,&S);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Every initial-upload failure keeps unpublished allocation ownership. */
    const UINT UploadOps[]={LockBo,CacheBo,CacheBo,UnlockBo};
    const UINT UploadNth[]={1,1,2,1};
    for(UINT I=0;I<4;I++) {
        Reset();CHECK(Init(&D)==S_OK);A=BufferArgs(&M,32,D3D10_DDI_USAGE_IMMUTABLE,D3D10_DDI_BIND_VERTEX_BUFFER,0);
        A.pInitialDataUP=&Initial;FailOp=UploadOps[I];FailAt=UploadNth[I];Sticky=1;CreateOnly(&D,&R,&A);
        CHECK(!R.Id && ErrorCalls && !MockError);
        if(UploadOps[I]==UnlockBo)CHECK(D.Resources && D.Resources->Bo.Locked && FAILED(MtkMaliUmdClose(Device)));
        FailOp=0;CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !MockError);
    }
    /* Failure during a rename cannot publish the fresh mapping or lose either BO. */
    const UINT RenameOps[]={HeapNew,CreateAllocation,MapVa,LockBo,CacheBo};
    for(UINT I=0;I<5;I++) {
        Reset();CHECK(Init(&D)==S_OK);
        A=BufferArgs(&M,32,D3D10_DDI_USAGE_DYNAMIC,D3D10_DDI_BIND_VERTEX_BUFFER,D3D10_DDI_CPU_ACCESS_WRITE);
        CreateOnly(&D,&R,&A);Source.pDrvPrivate=&R;D3DKMT_HANDLE OldHandle=D.Resources->Bo.Allocation;
        FailOp=RenameOps[I];FailAt=Calls[FailOp]+1;Map.pData=(void *)1;
        Functions.pfnDynamicIABufferMapDiscard(Device,Source,0,D3D10_DDI_MAP_WRITE_DISCARD,0,&Map);
        CHECK(!Map.pData && !Map.RowPitch && !Map.DepthPitch && D.Resources->Bo.Allocation==OldHandle &&
            !D.Resources->MapType && D.Error==DXGI_ERROR_DEVICE_REMOVED && !MockError);
        FailOp=0;DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !MockError);
    }
    /* Both read and write maps leave cleanup ownership after OS/cache failures. */
    const UINT MapOps[]={LockBo,CacheBo,CacheBo,UnlockBo};
    for(UINT I=0;I<4;I++) {
        Reset();CHECK(Init(&D)==S_OK);A=BufferArgs(&M,33,D3D10_DDI_USAGE_STAGING,0,D3D10_DDI_CPU_ACCESS_MASK);
        CreateOnly(&D,&R,&A);Source.pDrvPrivate=&R;
        if(I<2){FailOp=MapOps[I];FailAt=Calls[FailOp]+1;}
        Functions.pfnStagingResourceMap(Device,Source,0,D3D10_DDI_MAP_READWRITE,0,&Map);
        if(I>=2) {
            CHECK(Map.pData);FailOp=MapOps[I];FailAt=Calls[FailOp]+1;Sticky=1;
            Functions.pfnStagingResourceUnmap(Device,Source,0);CHECK(D.Resources->MapType && D.Resources->Bo.Locked);
        } else CHECK(!Map.pData);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && !MockError);
        FailOp=0;DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !MockError);
    }
    /* Two-resource copies keep every lock tracked even when cleanup also fails. */
    const UINT CopyOps[]={LockBo,LockBo,CacheBo,CacheBo,CacheBo,UnlockBo,UnlockBo};
    const UINT CopyNth[]={1,2,1,2,3,1,2};
    for(UINT I=0;I<7;I++) {
        Reset();CHECK(Init(&D)==S_OK);
        A=BufferArgs(&M,33,D3D10_DDI_USAGE_DEFAULT,D3D10_DDI_BIND_VERTEX_BUFFER,0);A.pInitialDataUP=&Initial;
        CreateOnly(&D,&R,&A);A.pInitialDataUP=0;CreateOnly(&D,&S,&A);
        Source.pDrvPrivate=&R;Dest.pDrvPrivate=&S;FailOp=CopyOps[I];FailAt=Calls[FailOp]+CopyNth[I];Sticky=1;
        Functions.pfnResourceCopy(Device,Dest,Source);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && FAILED(D.LastFailure) && !MockError);
        FailOp=0;DestroyOnly(&D,&R);DestroyOnly(&D,&S);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Wait failures never make submitted commands or the target disappear. */
    for(UINT I=0;I<2;I++) {
        Reset();CHECK(Init(&D)==S_OK);Create(&D,&R,&V,Formats[0],17,3);Clear(&D,&V);
        A=ResourceArgs(&M,Formats[0],17,3);A.Usage=D3D10_DDI_USAGE_STAGING;A.BindFlags=0;A.MapFlags=D3D10_DDI_CPU_ACCESS_READ;
        CreateOnly(&D,&S,&A);Source.pDrvPrivate=&R;Dest.pDrvPrivate=&S;
        FailOp=I?WaitEvent:IdleGpu;FailAt=Calls[FailOp]+1;Sticky=1;UINT Locks=Calls[LockBo];
        Functions.pfnResourceCopy(Device,Dest,Source);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && D.PendingCount==1 && LiveBos()==5 && Calls[LockBo]==Locks && !MockError);
        FailOp=0;Complete();Destroy(&D,&R,&V);DestroyOnly(&D,&S);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Invalid descriptors are rejected before any allocation or source read. */
    for(UINT I=0;I<14;I++) {
        Reset();CHECK(Init(&D)==S_OK);A=BufferArgs(&M,32,D3D10_DDI_USAGE_DEFAULT,D3D10_DDI_BIND_VERTEX_BUFFER,0);
        D3D10_DDIARG_SUBRESOURCE_UP Bad=Initial;
        switch(I) {
            case 0:A.Usage=D3D10_DDI_USAGE_IMMUTABLE;break;
            case 1:A.Usage=D3D10_DDI_USAGE_DYNAMIC;break;
            case 2:A.Usage=D3D10_DDI_USAGE_DYNAMIC;A.MapFlags=D3D10_DDI_CPU_ACCESS_READ;break;
            case 3:A.BindFlags=D3D10_DDI_BIND_CONSTANT_BUFFER;M.TexelWidth=M.PhysicalWidth=31;break;
            case 4:A.BindFlags|=D3D10_DDI_BIND_CONSTANT_BUFFER;break;
            case 5:A.BindFlags=D3D10_DDI_BIND_RENDER_TARGET;break;
            case 6:A.Usage=D3D10_DDI_USAGE_STAGING;A.BindFlags=0;break;
            case 7:A.Format=Formats[0];break;
            case 8:M.TexelWidth=M.PhysicalWidth=UINT32_MAX;break;
            case 9:Bad.pSysMem=0;A.pInitialDataUP=&Bad;break;
            case 10:Bad.pSysMem=(void *)(UINTPTR_MAX-16);A.pInitialDataUP=&Bad;break;
            case 11:A=ResourceArgs(&M,Formats[0],17,3);Bad.SysMemPitch=67;A.pInitialDataUP=&Bad;break;
            case 12:A.Usage=D3D10_DDI_USAGE_IMMUTABLE;A.MapFlags=1;A.pInitialDataUP=&Bad;break;
            case 13:A.BindFlags=D3D10_DDI_BIND_CONSTANT_BUFFER;M.TexelWidth=M.PhysicalWidth=65552;break;
        }
        D3D10DDI_HRESOURCE Resource={&R};D3D10DDI_HRTRESOURCE Runtime={0};R.Id=1;
        CHECK(!Functions.pfnCalcPrivateResourceSize(Device,&A));Functions.pfnCreateResource(Device,&A,Resource,Runtime);
        CHECK(!R.Id && !Calls[CreateAllocation] && !D.Resources && !MockError);
        CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Signed/overflow/out-of-range boxes never reach CPU locking or memory copies. */
    for(UINT I=0;I<11;I++) {
        Reset();CHECK(Init(&D)==S_OK);A=ResourceArgs(&M,Formats[0],17,3);
        CreateOnly(&D,&R,&A);CreateOnly(&D,&S,&A);Source.pDrvPrivate=&R;Dest.pDrvPrivate=&S;
        Box=(D3D10_DDI_BOX){0,0,0,17,3,1};UINT X=0,Y=0,Z=0;
        switch(I) {
            case 0:Box.left=-1;break;case 1:Box.top=-1;break;case 2:Box.right=18;break;
            case 3:Box.bottom=4;break;case 4:Box.front=-1;break;case 5:Box.back=2;break;
            case 6:X=UINT32_MAX;break;case 7:Y=UINT32_MAX;break;case 8:Z=1;break;
            case 9:X=1;break;case 10:Y=1;break;
        }
        Functions.pfnResourceCopyRegion(Device,Dest,0,X,Y,Z,Source,0,&Box);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && D.LastFailure==E_INVALIDARG && !Calls[LockBo] && !MockError);
        DestroyOnly(&D,&R);DestroyOnly(&D,&S);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Invalid maps are reported only as device removal and clear the output. */
    for(UINT I=0;I<9;I++) {
        Reset();CHECK(Init(&D)==S_OK);
        A=BufferArgs(&M,32,D3D10_DDI_USAGE_STAGING,0,D3D10_DDI_CPU_ACCESS_READ);CreateOnly(&D,&R,&A);
        Source.pDrvPrivate=&R;D3D10_DDI_MAP Type=D3D10_DDI_MAP_READ;UINT Sub=0,Flags=0;
        if(I==0)Type=D3D10_DDI_MAP_WRITE;if(I==1)Type=D3D10_DDI_MAP_READWRITE;
        if(I==2)Type=D3D10_DDI_MAP_WRITE_DISCARD;if(I==3)Type=D3D10_DDI_MAP_WRITE_NOOVERWRITE;
        if(I==4)Sub=1;if(I==5)Flags=1;
        if(I==6){D.Resources->Usage=D3D10_DDI_USAGE_DYNAMIC;D.Resources->BindFlags=D3D10_DDI_BIND_CONSTANT_BUFFER;Type=D3D10_DDI_MAP_WRITE_NOOVERWRITE;}
        if(I==7){D.Resources->Usage=D3D10_DDI_USAGE_DEFAULT;}
        if(I==8)Source.pDrvPrivate=0;
        memset(&Map,0xff,sizeof(Map));Functions.pfnStagingResourceMap(Device,Source,Sub,Type,Flags,&Map);
        CHECK(!Map.pData && !Map.RowPitch && !Map.DepthPitch && D.Error==DXGI_ERROR_DEVICE_REMOVED &&
            ErrorCode==DXGI_ERROR_DEVICE_REMOVED && !Calls[LockBo] && !MockError);
        DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }

    /* Upload failures leave no untracked CPU lock; invalid updates cannot read UP. */
    const UINT UpdateOps[]={LockBo,CacheBo,CacheBo,UnlockBo};
    const UINT UpdateNth[]={1,1,2,1};
    for(UINT I=0;I<4;I++) {
        Reset();CHECK(Init(&D)==S_OK);A=BufferArgs(&M,32,D3D10_DDI_USAGE_DEFAULT,D3D10_DDI_BIND_CONSTANT_BUFFER,0);
        CreateOnly(&D,&R,&A);Source.pDrvPrivate=&R;FailOp=UpdateOps[I];FailAt=Calls[FailOp]+UpdateNth[I];Sticky=1;
        Functions.pfnDefaultConstantBufferUpdateSubresourceUP(Device,Source,0,0,Data,0,0);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && FAILED(D.LastFailure) && !MockError);
        FailOp=0;DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    for(UINT I=0;I<8;I++) {
        Reset();CHECK(Init(&D)==S_OK);A=ResourceArgs(&M,Formats[0],17,3);
        CreateOnly(&D,&R,&A);Source.pDrvPrivate=&R;Box=(D3D10_DDI_BOX){0,0,0,17,3,1};
        const void *Ptr=Data;UINT Pitch=80,Sub=0;
        if(I==0)Ptr=0;if(I==1)Pitch=67;if(I==2)Sub=1;
        if(I==3)Box.left=-1;if(I==4)Box.right=18;
        if(I==5)Ptr=(const void *)(UINTPTR_MAX-10);
        if(I==6)D.Resources->Usage=D3D10_DDI_USAGE_IMMUTABLE;
        if(I==7)D.Resources->MapType=D3D10_DDI_MAP_READ;
        Functions.pfnResourceUpdateSubresourceUP(Device,Source,Sub,&Box,Ptr,Pitch,0);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && !Calls[LockBo] && !MockError);
        D.Resources->MapType=0;DestroyOnly(&D,&R);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Mapped, immutable, mismatched, stale and identical copies are rejected. */
    for(UINT I=0;I<8;I++) {
        Reset();CHECK(Init(&D)==S_OK);A=BufferArgs(&M,32,D3D10_DDI_USAGE_DEFAULT,D3D10_DDI_BIND_VERTEX_BUFFER,0);
        CreateOnly(&D,&R,&A);CreateOnly(&D,&S,&A);Source.pDrvPrivate=&R;Dest.pDrvPrivate=&S;
        UMD_RESOURCE *Dst=D.Resources,*Src=Dst->Next;
        if(I==0)Dst->Usage=D3D10_DDI_USAGE_IMMUTABLE;if(I==1)Src->MapType=D3D10_DDI_MAP_READ;
        if(I==2)Dst->MapType=D3D10_DDI_MAP_WRITE;if(I==3)Dst->Format=Formats[0];
        if(I==4)Dst->LogicalBytes=31;if(I==5)Dest=Source;if(I==6)Source.pDrvPrivate=0;
        if(I==7)Dst->Desc.Kind=MALI_ALLOCATION_LINEAR_2D;
        Functions.pfnResourceCopy(Device,Dest,Source);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && !Calls[LockBo] && !MockError);
        Dst->MapType=Src->MapType=0;DestroyOnly(&D,&R);DestroyOnly(&D,&S);
        CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    return MockError;
}
static const UINT VertexTokens[]={0x10040,15,
    0x04000067,0x001020f2,0,1,
    0x08000036,0x001020f2,0,0x00004002,0,0,0,0x3f800000,0x0100003e};
static const UINT PixelTokens[]={0x40,14,
    0x03000065,0x001020f2,0,
    0x08000036,0x001020f2,0,0x00004002,0,0,0,0x3f800000,0x0100003e};
static VOID CreateShader(UMD_DEVICE *D,UMD_TOKEN *Token,UINT Stage,const UINT *Code,
    const D3D10DDIARG_STAGE_IO_SIGNATURES *Sig) {
    D3D10DDI_HDEVICE Device={D};D3D10DDI_HSHADER Shader={Token};D3D10DDI_HRTSHADER Runtime={(HANDLE)(uintptr_t)0x7010};
    EXPECT(Functions.pfnCalcPrivateShaderSize(Device,Code,Sig)==sizeof(*Token));
    if(Stage==T70_SHADER_VERTEX)Functions.pfnCreateVertexShader(Device,Code,Shader,Runtime,Sig);
    else Functions.pfnCreatePixelShader(Device,Code,Shader,Runtime,Sig);
}
static int TestShaders(void) {
    struct t70_shader_io_types Types={.vertex_uint=1u<<3,.target_sint=1};
    UMD_DEVICE D;UMD_TOKEN V={0},P={0},Stale={0};D3D10DDI_HDEVICE Device={&D};
    D3D10DDI_HSHADER VS={&V},PS={&P},Null={0};
    struct t70_shader_pipeline *Compiled=(void *)Backing;
    Reset();CHECK(Init(&D)==S_OK);
    CHECK(MtkMaliUmdClose(Device)==S_OK);
    Reset();CHECK(Init(&D)==S_OK);ZeroMemory(Compiled,sizeof(*Compiled));
    CHECK(MtkMaliUmdCompileBoundShaders(Device,0,Compiled)==E_INVALIDARG);
    CHECK(MtkMaliUmdCompileBoundShaders(Device,&Types,Compiled)==E_INVALIDARG && !Calls[CompilePipeline]);
    UINT VCode[sizeof(VertexTokens)/4],PCode[sizeof(PixelTokens)/4];
    CopyMemory(VCode,VertexTokens,sizeof(VCode));CopyMemory(PCode,PixelTokens,sizeof(PCode));
    D3D10DDIARG_SIGNATURE_ENTRY In={D3D10_SB_NAME_UNDEFINED,3,3};
    D3D10DDIARG_SIGNATURE_ENTRY VOut={D3D10_SB_NAME_POSITION,0,15},POut={D3D10_SB_NAME_UNDEFINED,0,15};
    D3D10DDIARG_STAGE_IO_SIGNATURES Sig={&In,1,&VOut,1};
    CreateShader(&D,&V,T70_SHADER_VERTEX,VCode,&Sig);
    CHECK(V.Id && V.Owner==&D && D.ShaderCount==1 && LiveHeap()==1 && !CompilerLive);
    CHECK(LastShaderRequest.tokens!=VCode && LastShaderRequest.inputs!=(void *)&In &&
        LastShaderRequest.inputs[0].mask==3 && LastShaderRequest.outputs[0].system_value==D3D10_SB_NAME_POSITION);
    Sig.pOutputSignature=&POut;CreateShader(&D,&P,T70_SHADER_PIXEL,PCode,&Sig);
    CHECK(P.Id && P.Id!=V.Id && D.ShaderCount==2 && LiveHeap()==2 && Calls[CompileShader]==2);
    CHECK(MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY) && D.Ready);
    Functions.pfnVsSetShader(Device,VS);Functions.pfnPsSetShader(Device,PS);
    CHECK(D.VertexShader && D.PixelShader && !ErrorCalls && !Calls[CompilePipeline]);
    VCode[0]=PCode[0]=0;In.Mask=1;VOut.SystemValue=D3D10_SB_NAME_INSTANCE_ID;
    CHECK(MtkMaliUmdCompileBoundShaders(Device,&Types,Compiled)==S_OK && CompilerLive==2);
    CHECK(LastPairTypes.vertex_uint==Types.vertex_uint && LastPairTypes.target_sint==Types.target_sint);
    CHECK(LastPairVertex==VertexTokens[0] && LastPairPixel==PixelTokens[0] && LastPairInputMask==3);
    CHECK(MtkMaliUmdCompileBoundShaders(Device,&Types,Compiled)==E_INVALIDARG && Calls[CompilePipeline]==1);
    Functions.pfnPsSetShader(Device,VS);CHECK(ErrorCode==E_INVALIDARG && D.PixelShader->Id==P.Id);
    UMD_TOKEN Foreign=V;Foreign.Owner=(void *)(uintptr_t)0xdead;D3D10DDI_HSHADER Bad={&Foreign};
    Functions.pfnVsSetShader(Device,Bad);CHECK(D.VertexShader->Id==V.Id);
    Stale=V;Functions.pfnDestroyShader(Device,VS);
    CHECK(!V.Id && !V.Owner && !D.VertexShader && D.ShaderCount==1 && CompilerLive==2);
    Functions.pfnDestroyShader(Device,PS);
    CHECK(!D.PixelShader && !D.Shaders && !LiveHeap() && CompilerLive==2);
    t70_shader_free_pipeline(Compiled);CHECK(!CompilerLive);
    ReenterError=1;Bad.pDrvPrivate=&Stale;Functions.pfnVsSetShader(Device,Bad);
    CHECK(!D.VertexShader && !GateDepth && !ReenterError && !MockError);
    CreateShader(&D,&V,T70_SHADER_VERTEX,VertexTokens,0);
    CHECK(V.Id>Stale.Id && D.ShaderCount==1);
    Functions.pfnDestroyShader(Device,Bad);CHECK(D.ShaderCount==1 && V.Id);
    Functions.pfnVsSetShader(Device,VS);Functions.pfnVsSetShader(Device,Null);Functions.pfnPsSetShader(Device,Null);
    CHECK(!D.VertexShader && !D.PixelShader);
    Functions.pfnDestroyShader(Device,VS);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    for(UINT Failure=1;Failure<=3;Failure++) {
        Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&V,sizeof(V));
        FailOp=Failure<=2?HeapNew:CompileShader;FailAt=Failure<=2?Failure:1;ReenterError=1;
        CreateShader(&D,&V,T70_SHADER_VERTEX,VertexTokens,0);
        CHECK(!V.Id && !D.Shaders && !D.ShaderCount && !LiveHeap() && !CompilerLive && !ReenterError && !MockError);
        CHECK(ErrorCode==(Failure<=2?E_OUTOFMEMORY:DXGI_ERROR_UNSUPPORTED));
        CHECK(MtkMaliUmdClose(Device)==S_OK);
    }
    Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&V,sizeof(V));ZeroMemory(&P,sizeof(P));
    CreateShader(&D,&V,T70_SHADER_VERTEX,VertexTokens,0);CreateShader(&D,&P,T70_SHADER_PIXEL,PixelTokens,0);
    Functions.pfnVsSetShader(Device,VS);Functions.pfnPsSetShader(Device,PS);
    const int CompilerErrors[]={T70_SHADER_MEMORY,T70_SHADER_INVALID,T70_SHADER_UNSUPPORTED,T70_SHADER_TRANSLATION};
    const HRESULT Results[]={E_OUTOFMEMORY,E_INVALIDARG,DXGI_ERROR_UNSUPPORTED,E_FAIL};
    Compiled->varying_stride=0xbeef;
    for(UINT I=0;I<4;I++) {
        FailOp=CompilePipeline;FailAt=Calls[CompilePipeline]+1;CompilerFailure=CompilerErrors[I];
        CHECK(MtkMaliUmdCompileBoundShaders(Device,&Types,Compiled)==Results[I]);
        CHECK(Compiled->varying_stride==0xbeef && !Compiled->vertex.binary && !CompilerLive && !ErrorCalls);
    }
    Functions.pfnDestroyShader(Device,VS);Functions.pfnDestroyShader(Device,PS);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&V,sizeof(V));
    D.NextId=UINT64_MAX;CreateShader(&D,&V,T70_SHADER_VERTEX,VertexTokens,0);
    CHECK(!V.Id && !Calls[HeapNew] && !Calls[CompileShader] && ErrorCode==E_OUTOFMEMORY);
    D.NextId=0;D.ShaderCount=UMD_LIMIT;CreateShader(&D,&V,T70_SHADER_VERTEX,VertexTokens,0);
    CHECK(!V.Id && !Calls[CompileShader]);D.ShaderCount=0;
    CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    Reset();CHECK(Init(&D)==S_OK);
    CHECK(Functions.pfnCalcPrivateShaderSize(Device,0,0)==(SIZE_T)-1);
    CopyMemory(VCode,VertexTokens,sizeof(VCode));VCode[1]=T70_SHADER_MAX_INPUT/4u+1u;
    CHECK(Functions.pfnCalcPrivateShaderSize(Device,VCode,0)==(SIZE_T)-1);
    D3D10DDI_HRTSHADER Runtime={0};ZeroMemory(&V,sizeof(V));
    Functions.pfnCreateVertexShader(Device,PixelTokens,VS,Runtime,0);
    CHECK(!V.Id && !Calls[CompileShader] && ErrorCode==E_INVALIDARG);
    Functions.pfnCreateVertexShader(Device,VertexTokens,Null,Runtime,0);
    CHECK(!Calls[HeapNew] && ErrorCode==E_INVALIDARG);
    Sig.NumInputSignatureEntries=T70_SHADER_MAX_SIGNATURE+1;
    CHECK(Functions.pfnCalcPrivateShaderSize(Device,VertexTokens,&Sig)==(SIZE_T)-1);
    Sig.NumInputSignatureEntries=1;Sig.pInputSignature=0;
    CHECK(Functions.pfnCalcPrivateShaderSize(Device,VertexTokens,&Sig)==(SIZE_T)-1);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveHeap() && !CompilerLive && !MockError);
    return MockError;
}
static int EqualBytes(const void *Left,const void *Right,SIZE_T Count) {
    const volatile unsigned char *A=Left,*B=Right;
    for(SIZE_T I=0;I<Count;I++)if(A[I]!=B[I])return 0;
    return 1;
}
static void BindPair(UMD_DEVICE *D,UMD_TOKEN Tokens[2]) {
    ZeroMemory(Tokens,2*sizeof(*Tokens));
    CreateShader(D,&Tokens[0],T70_SHADER_VERTEX,VertexTokens,0);
    CreateShader(D,&Tokens[1],T70_SHADER_PIXEL,PixelTokens,0);
    D3D10DDI_HDEVICE Device={D};D3D10DDI_HSHADER V={&Tokens[0]},P={&Tokens[1]};
    Functions.pfnVsSetShader(Device,V);Functions.pfnPsSetShader(Device,P);
}
static void DeletePair(UMD_DEVICE *D,UMD_TOKEN Tokens[2]) {
    D3D10DDI_HDEVICE Device={D};
    for(UINT I=0;I<2;I++){D3D10DDI_HSHADER S={&Tokens[I]};Functions.pfnDestroyShader(Device,S);}
}
static int TestPrograms(void) {
    UMD_DEVICE D;D3D10DDI_HDEVICE Device={&D};UMD_TOKEN Tokens[2];
    struct t70_shader_io_types Types={.vertex_sint=2,.target_uint=1};
    MALI_UMD_PROGRAM Out={0},Empty={0},Stale={0};
    Reset();CHECK(Init(&D)==S_OK);
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_INVALIDARG && !Calls[CompilePipeline]);
    Out.Descriptors.Access=MaliPteRead;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_INVALIDARG && Out.Descriptors.Access==MaliPteRead);
    ZeroMemory(&Out,sizeof(Out));Out.Shader[3]=1;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_INVALIDARG && Out.Shader[3]==1 && !Calls[CompilePipeline]);
    ZeroMemory(&Out,sizeof(Out));
    BindPair(&D,Tokens);CompilerCodeBytes[T70_SHADER_PIXEL]=4104;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,0,&Out)==E_INVALIDARG);
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,0)==E_INVALIDARG);
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==S_OK);
    CHECK(Out.Id && Out.Owner==&D && Out.Compiled && D.ProgramCount==1 && CompilerLive==2);
    CHECK(Out.Code[0].Bytes==4096 && Out.Code[1].Bytes==8192 && WaitedFence==PagingNext);
    CHECK(LastPairTypes.vertex_sint==Types.vertex_sint && LastPairTypes.target_uint==Types.target_uint);
    for(UINT I=0;I<2;I++) {
        const struct t70_shader *S=I?&Out.Compiled->pixel:&Out.Compiled->vertex;
        TEST_BO *B=TestVa(Out.Code[I].Va);
        CHECK(B->Protection==2 && Out.Code[I].Access==(MaliPteRead|MaliPteExecute) && B->Resident && !B->Locked);
        CHECK(B->CacheWrites==1 && EqualBytes(B->Cpu,S->binary,S->binary_size));
        for(SIZE_T J=S->binary_size;J<B->Bytes;J++)CHECK(!B->Cpu[J]);
        B->Uses=1; /* GPU-work ownership fixture; no shader instructions run. */
    }
    TEST_BO *Descriptors=TestVa(Out.Descriptors.Va);const UINT *Words=(const UINT *)Descriptors->Cpu;
    CHECK(Out.Descriptors.Bytes==4096 && Out.Descriptors.Access==MaliPteRead && !Descriptors->Protection);
    CHECK(Descriptors->Resident && !Descriptors->Locked && Descriptors->CacheWrites==1);
    for(UINT I=0;I<4;I++)CHECK(Out.Shader[I]==Out.Descriptors.Va+32u*I);
    CHECK(Words[0]==0x80020138 && Words[1]==0x1234);
    CHECK(((uint64_t)Words[3]<<32|Words[2])==Out.Code[0].Va);
    CHECK(Words[8]==Words[0] && Words[9]==Words[1]);
    CHECK(((uint64_t)Words[11]<<32|Words[10])==Out.Code[0].Va+8);
    CHECK(Words[16]==0x00020038 && Words[17]==0xabcd);
    CHECK(((uint64_t)Words[19]<<32|Words[18])==Out.Code[0].Va+8);
    CHECK(Words[24]==0x10040028 && Words[25]==0xaa55);
    CHECK(((uint64_t)Words[27]<<32|Words[26])==Out.Code[1].Va);
    for(UINT I=0;I<4;I++)for(UINT J=16;J<32;J++)CHECK(!Descriptors->Cpu[I*32+J]);
    for(UINT I=128;I<4096;I++)CHECK(!Descriptors->Cpu[I]);
    Descriptors->Uses=1;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_INVALIDARG && Calls[CompilePipeline]==1);
    DeletePair(&D,Tokens);CHECK(!D.Shaders && CompilerLive==2);
    CHECK(MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY) && D.Ready && !Calls[IdleGpu]);
    MALI_UMD_PROGRAM Wrong=Out;Wrong.Owner=(void *)(uintptr_t)1;
    CHECK(MtkMaliUmdReleaseProgram(Device,&Wrong)==E_INVALIDARG && D.ProgramCount==1);
    Wrong=Out;++Wrong.Id;
    CHECK(MtkMaliUmdReleaseProgram(Device,&Wrong)==E_INVALIDARG && D.ProgramCount==1);
    Stale=Out;CHECK(MtkMaliUmdReleaseProgram(Device,&Out)==S_OK && EqualBytes(&Out,&Empty,sizeof(Out)));
    CHECK(!D.Programs && !D.ProgramCount && !LiveBos() && !CompilerLive && Calls[IdleGpu]==1);
    CHECK(MtkMaliUmdReleaseProgram(Device,&Stale)==E_INVALIDARG && Calls[IdleGpu]==1);
    BindPair(&D,Tokens);CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==S_OK && Out.Id>Stale.Id);
    CHECK(MtkMaliUmdReleaseProgram(Device,&Stale)==E_INVALIDARG && D.ProgramCount==1);
    DeletePair(&D,Tokens);CHECK(MtkMaliUmdReleaseProgram(Device,&Out)==S_OK);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    const UINT Ops[]={HeapNew,CompilePipeline,CreateAllocation,MapVa,LockBo,CacheBo,UnlockBo,ResidentBo,WaitGpu};
    const UINT Counts[]={1,1,3,3,3,3,3,3,1};
    for(UINT I=0;I<sizeof(Ops)/sizeof(Ops[0]);I++)for(UINT N=1;N<=Counts[I];N++) {
        Reset();CHECK(Init(&D)==S_OK);BindPair(&D,Tokens);
        FailOp=Ops[I];FailAt=Calls[FailOp]+N;
        CHECK(FAILED(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)));
        CHECK(EqualBytes(&Out,&Empty,sizeof(Out)) && !D.Programs && !D.ProgramCount && !LiveBos() && !CompilerLive);
        CHECK(!Calls[IdleGpu] && !Calls[SubmitGpu] && !ErrorCalls && LiveHeap()==2);
        DeletePair(&D,Tokens);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Unpublished upload failures retain pending maps and committed residency. */
    Reset();CHECK(Init(&D)==S_OK);BindPair(&D,Tokens);
    AsyncPaging=PartialResidentFailure=1;PagingValue=0;FailOp=ResidentBo;FailAt=1;
    CHECK(FAILED(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)) && !Out.Id);
    CHECK(D.ProgramCount==1 && LiveBos()==1 && D.Programs->Bo[0].Resident && !Calls[FreeVa] && CompilerLive==2);
    DeletePair(&D,Tokens);CHECK(MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY) && ContextLive);
    FailOp=0;PagingValue=PagingNext;CHECK(MtkMaliUmdClose(Device)==S_OK && !CompilerLive && !MockError);
    Reset();CHECK(Init(&D)==S_OK);BindPair(&D,Tokens);
    FailOp=UnlockBo;FailAt=2;Sticky=1;
    CHECK(FAILED(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)) && D.ProgramCount==1 && CompilerLive==2);
    CHECK(D.Programs->Bo[1].Locked && LiveBos()==1 && !Calls[IdleGpu]);
    DeletePair(&D,Tokens);CHECK(FAILED(MtkMaliUmdClose(Device)) && ContextLive);
    FailOp=0;CHECK(MtkMaliUmdClose(Device)==S_OK && !CompilerLive && !MockError);
    /* Published code cannot be freed on failed or merely pending idle waits. */
    for(UINT Failure=0;Failure<2;Failure++) {
        Reset();CHECK(Init(&D)==S_OK);BindPair(&D,Tokens);
        CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==S_OK);DeletePair(&D,Tokens);
        for(UINT I=0;I<2;I++)TestVa(Out.Code[I].Va)->Uses=1;
        if(Failure){IdleResult=0x103;}else{FailOp=IdleGpu;FailAt=1;Sticky=1;}
        CHECK(FAILED(MtkMaliUmdReleaseProgram(Device,&Out)) && !Out.Id && D.ProgramCount==1);
        CHECK(LiveBos()==3 && CompilerLive==2 && !Calls[FreeVa]);
        CHECK(FAILED(MtkMaliUmdClose(Device)) && ContextLive && LiveBos()==3);
        FailOp=0;IdleResult=0;CHECK(MtkMaliUmdClose(Device)==S_OK && !CompilerLive && !MockError);
    }
    /* GPU queue waits do not imply that the CPU-observed paging fence is done. */
    Reset();CHECK(Init(&D)==S_OK);BindPair(&D,Tokens);AsyncPaging=1;PagingValue=0;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==S_OK && WaitedFence==PagingNext);
    DeletePair(&D,Tokens);CHECK(MtkMaliUmdReleaseProgram(Device,&Out)==HRESULT_FROM_WIN32(ERROR_BUSY));
    CHECK(LiveBos()==3 && !Calls[FreeVa] && Calls[IdleGpu]==1 && CompilerLive==2);
    CHECK(MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY) && Calls[IdleGpu]==1);
    PagingValue=PagingNext;CHECK(MtkMaliUmdClose(Device)==S_OK && !CompilerLive && !MockError);
    const UINT CleanupOps[]={FreeVa,DestroyAllocation};
    for(UINT I=0;I<2;I++)for(UINT N=1;N<=3;N++) {
        Reset();CHECK(Init(&D)==S_OK);BindPair(&D,Tokens);
        CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==S_OK);DeletePair(&D,Tokens);
        FailOp=CleanupOps[I];FailAt=N;Sticky=1;
        CHECK(FAILED(MtkMaliUmdReleaseProgram(Device,&Out)) && !Out.Id && D.Programs && CompilerLive==2);
        CHECK(FAILED(MtkMaliUmdClose(Device)) && ContextLive && Calls[IdleGpu]==1);
        FailOp=0;CHECK(MtkMaliUmdClose(Device)==S_OK && !CompilerLive && !MockError);
    }
    Reset();CHECK(Init(&D)==S_OK);BindPair(&D,Tokens);
    const SIZE_T BadSizes[]={0,7,(SIZE_T)UMD_TARGET_LIMIT+1u};
    for(UINT I=0;I<3;I++) {
        CompilerCodeBytes[T70_SHADER_VERTEX]=BadSizes[I];
        CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_UNEXPECTED && !Out.Id && !Calls[CreateAllocation]);
        CHECK(!D.Programs && !CompilerLive);
    }
    CompilerCodeBytes[T70_SHADER_VERTEX]=16;
    struct t70_shader V=CompilerMetadata[1],P=CompilerMetadata[0];
    for(UINT Case=0;Case<26;Case++) {
        CompilerMetadata[1]=V;CompilerMetadata[0]=P;
        struct t70_shader *S=Case>=19?&CompilerMetadata[0]:&CompilerMetadata[1];
        switch(Case) {
        case 0:S->stage=0;break;case 1:S->arch=10;break;
        case 2:S->work_registers=0;break;case 3:S->work_registers=65;break;
        case 4:S->preload|=1;break;case 5:S->preload|=UINT64_C(1)<<47;break;
        case 6:S->flags&=~T70_SHADER_FLAG_IDVS;break;
        case 7:S->no_point_size_offset=1;break;case 8:S->no_point_size_offset=16;break;
        case 9:S->no_point_size_offset=UINT32_MAX-7;break;
        case 10:S->secondary_offset=1;break;case 11:S->secondary_offset=16;break;
        case 12:S->secondary_offset=UINT32_MAX-7;break;
        case 13:S->secondary_registers=0;break;case 14:S->secondary_registers=65;break;
        case 15:S->secondary_preload|=1;break;case 16:S->secondary_preload|=UINT64_C(1)<<47;break;
        case 17:S->flags|=T70_SHADER_FLAG_FTZ16;S->flags&=~T70_SHADER_FLAG_FTZ32;break;
        case 18:S->arch=0;break;case 19:S->stage=1;break;case 20:S->arch=7;break;
        case 21:S->flags|=T70_SHADER_FLAG_IDVS;break;case 22:S->flags|=T70_SHADER_FLAG_SECONDARY;break;
        case 23:S->work_registers=65;break;case 24:S->flags&=~T70_SHADER_FLAG_FTZ32;break;case 25:S->fau_count=UINT32_MAX;break;
        }
        CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_UNEXPECTED && !Out.Id && !Calls[CreateAllocation]);
        CHECK(!D.Programs && !CompilerLive);
    }
    CompilerMetadata[1]=V;CompilerMetadata[0]=P;
    /* No-varying shader leaves both the descriptor bytes and public pointer zero. */
    CompilerMetadata[1].flags&=~T70_SHADER_FLAG_SECONDARY;
    CompilerMetadata[1].flags&=~T70_SHADER_FLAG_FTZ32;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==S_OK && !Out.Shader[MaliUmdVarying]);
    Descriptors=TestVa(Out.Descriptors.Va);Words=(const UINT *)Descriptors->Cpu;
    CHECK(Words[0]==0x80000138);
    for(UINT I=64;I<96;I++)CHECK(!Descriptors->Cpu[I]);
    CHECK(MtkMaliUmdReleaseProgram(Device,&Out)==S_OK);
    CompilerMetadata[1]=V;
    UINT Before=Calls[CompilePipeline];D.ProgramCount=UMD_PROGRAM_LIMIT;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_OUTOFMEMORY && Calls[CompilePipeline]==Before);
    D.ProgramCount=0;D.NextId=UINT64_MAX;
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==E_OUTOFMEMORY && Calls[CompilePipeline]==Before);
    D.Ready=0;CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&Out)==DXGI_ERROR_DEVICE_REMOVED);
    DeletePair(&D,Tokens);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    return MockError;
}
typedef struct {
    UMD_DEVICE Device;
    UMD_TOKEN Shaders[2],Resources[4];
    MALI_UMD_PROGRAM Program;
    MALI_COMMAND_RASTER Draw;
    MALI_UMD_RASTER_RESOURCE Bindings[4];
} RASTER_CASE;
static int InitRaster(RASTER_CASE *F,UINT Secondary) {
    Reset();ZeroMemory(F,sizeof(*F));CHECK(Init(&F->Device)==S_OK);
    UMD_DEVICE *D=&F->Device;D3D10DDI_HDEVICE Device={D};
    D->Caps.JobSlots|=2;D->Caps.JobFeatures[1]=1u<<11;
    if(!Secondary)CompilerMetadata[1].flags&=~T70_SHADER_FLAG_SECONDARY;
    CompilerMetadata[1].fau_count=2;CompilerMetadata[0].fau_count=4;
    BindPair(D,F->Shaders);struct t70_shader_io_types Types={0};
    CHECK(MtkMaliUmdUploadBoundShaders(Device,&Types,&F->Program)==S_OK);
    D3D10DDI_MIPINFO M;D3D10DDIARG_CREATERESOURCE A;
    MALI_COMMAND_RESOURCE Ranges[4];
    for(UINT I=0;I<4;I++) {
        if(I==1)A=ResourceArgs(&M,DXGI_FORMAT_R8G8B8A8_UNORM,17,19);
        else A=BufferArgs(&M,I==3?MALI_COMMAND_CLEAR_HEAP_BYTES:4096,
            I==2?D3D10_DDI_USAGE_DYNAMIC:D3D10_DDI_USAGE_DEFAULT,
            I==2?D3D10_DDI_BIND_VERTEX_BUFFER|D3D10_DDI_BIND_INDEX_BUFFER:D3D10_DDI_BIND_SHADER_RESOURCE,
            I==2?D3D10_DDI_CPU_ACCESS_WRITE:0);
        CreateOnly(D,&F->Resources[I],&A);CHECK(F->Resources[I].Id);
        F->Bindings[I].Resource.pDrvPrivate=&F->Resources[I];F->Bindings[I].Access=I==2?MaliPteRead:MaliPteRead|MaliPteWrite;
        CHECK(MtkMaliUmdResourceRange(Device,F->Bindings[I].Resource,F->Bindings[I].Access,&Ranges[I])==S_OK);
    }
    /* Existing clear descriptors provide a structurally valid framebuffer and
       tiler fixture. No shaders or pixels execute in this OS/GPU test boundary. */
    MALI_COMMAND_CLEAR C={0};C.Bytes=sizeof(C);C.Width=17;C.Height=19;C.Pitch=128;
    C.Format=MaliClearRgba8;C.TargetVa=Ranges[1].Va;C.TargetBytes=Ranges[1].Bytes;C.HeapVa=Ranges[3].Va;
    unsigned char Job[128],Private[80];TEST_BO *Descriptors=TestVa(Ranges[0].Va);
    CHECK(MaliCommandsBuildClear(&D->Caps,&C,Job,UINT64_C(0x70000000000),128,
        Descriptors->Cpu,Ranges[0].Va,4096,Private,80)==MaliOk);
    F->Draw.Bytes=sizeof(F->Draw);F->Draw.VertexSlot=1;F->Draw.FragmentSlot=0;
    F->Draw.Primitive=8u|(2u<<8);F->Draw.IndexCount=3;F->Draw.InstanceCount=1;
    F->Draw.PrimitiveSize=0x3f800000;F->Draw.MaxZ=0x3f800000;
    F->Draw.Scissor[1]=16u|(18u<<16);F->Draw.Flags[1]=0x1ffff;
    F->Draw.Tiler=Ranges[0].Va+192;F->Draw.Framebuffer=Ranges[0].Va|1u;
    F->Draw.Tiles[1]=1u|(1u<<16);F->Draw.Indices=Ranges[2].Va;
    F->Draw.Position.ThreadStorage=F->Draw.Fragment.ThreadStorage=Ranges[0].Va+512;
    F->Draw.Position.Fau=Ranges[0].Va+576;F->Draw.Position.FauCount=1;
    F->Draw.Fragment.Fau=Ranges[0].Va+608;F->Draw.Fragment.FauCount=2;
    return MockError;
}
static HRESULT SubmitRaster(RASTER_CASE *F) {
    D3D10DDI_HDEVICE Device={&F->Device};return MtkMaliUmdSubmitRaster(Device,&F->Program,&F->Draw,F->Bindings,4);
}
static int FinishRaster(RASTER_CASE *F) {
    UMD_DEVICE *D=&F->Device;D3D10DDI_HDEVICE Device={D};
    DeletePair(D,F->Shaders);
    if(F->Program.Id){HRESULT H=MtkMaliUmdReleaseProgram(Device,&F->Program);CHECK(SUCCEEDED(H) || H==HRESULT_FROM_WIN32(ERROR_BUSY));}
    for(UINT I=0;I<4;I++)DestroyOnly(D,&F->Resources[I]);
    Complete();Functions.pfnFlush(Device);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveEvents() && !LiveHeap() && !CompilerLive && !MockError);
    return MockError;
}
static int TestRaster(void) {
    RASTER_CASE F;D3D10DDI_HDEVICE Device={&F.Device};
    for(UINT Secondary=0;Secondary<2;Secondary++)for(UINT Point=0;Point<2;Point++) {
        int R=InitRaster(&F,Secondary);if(R)return R;
        if(Point)F.Draw.Primitive=(F.Draw.Primitive&~255u)|1u;
        UMD_RESOURCE *RenderTarget=UmdFindResource(&F.Device,&F.Resources[1]);
        CHECK(RenderTarget);
        /* Public views cannot inject a different code/descriptor pointer. */
        F.Program.Shader[0]=1;F.Program.Code[0].Va=1;F.Program.Compiled=(void *)(uintptr_t)1;
        CHECK(SubmitRaster(&F)==S_OK && F.Device.PendingCount==1 && F.Device.Programs->Pending==1);
        CHECK(RenderTarget->Pending==1);
        CHECK(Calls[SubmitGpu]==1 && !LastSubmitted && PrepareWorkspace.ResourceCount==7);
        const UINT *W=(const UINT *)TestBo(LastSubmission[0])->Cpu;
        CHECK((W[8]&(1u<<18))==(Secondary?1u<<18:0) && W[13]==(Secondary?0x100020u:16u));
        CHECK(((uint64_t)W[75]<<32|W[74])==F.Device.Programs->Bo[2].Va+(Point?0u:32u));
        CHECK(((uint64_t)W[59]<<32|W[58])==F.Device.Programs->Bo[2].Va+96);
        for(UINT I=0;I<4;I++) {
            UMD_RESOURCE *Resource=UmdFindResource(&F.Device,&F.Resources[I]);CHECK(Resource && Resource->Pending==1);
            EnterCriticalSection(&F.Device.Gate);HRESULT H=UmdCpuIdle(&F.Device,Resource,1);LeaveCriticalSection(&F.Device.Gate);
            CHECK(H==DXGI_DDI_ERR_WASSTILLDRAWING && !Calls[IdleGpu]);
        }
        CHECK(MtkMaliUmdReleaseProgram(Device,&F.Program)==HRESULT_FROM_WIN32(ERROR_BUSY) && !F.Program.Id);
        CHECK(F.Device.ProgramCount==1 && F.Device.Programs->Pending==1 && !Calls[IdleGpu] && CompilerLive==2);
        for(UINT I=0;I<4;I++)DestroyOnly(&F.Device,&F.Resources[I]);
        CHECK(F.Device.ResourceCount==4 && LiveBos()==8 && MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY));
        R=FinishRaster(&F);if(R)return R;
    }
    /* A submitted input's old backing remains held across DISCARD rename;
       stale explicit addresses fail, then the new snapshot can be submitted. */
    int R=InitRaster(&F,1);if(R)return R;CHECK(SubmitRaster(&F)==S_OK);
    UMD_RESOURCE *Vertex=UmdFindResource(&F.Device,&F.Resources[2]);uint64_t OldVa=Vertex->Bo.Va;
    D3D10DDI_MAPPED_SUBRESOURCE Map={0};
    Functions.pfnDynamicIABufferMapDiscard(Device,F.Bindings[2].Resource,0,D3D10_DDI_MAP_WRITE_DISCARD,0,&Map);
    CHECK(Map.pData && Vertex->Bo.Va!=OldVa && Vertex->OldCount==1 && TestVa(OldVa)->Uses==1 && !MockError);
    MALI_COMMAND_RESOURCE Range={1,2,3},Before=Range;
    CHECK(MtkMaliUmdResourceRange(Device,F.Bindings[2].Resource,MaliPteRead,&Range)==E_INVALIDARG && EqualBytes(&Range,&Before,sizeof(Range)));
    Functions.pfnDynamicIABufferUnmap(Device,F.Bindings[2].Resource,0);
    CHECK(SubmitRaster(&F)==E_INVALIDARG && F.Device.PendingCount==1 && Calls[SubmitGpu]==1);
    CHECK(MtkMaliUmdResourceRange(Device,F.Bindings[2].Resource,MaliPteRead,&Range)==S_OK && Range.Va==Vertex->Bo.Va);
    F.Draw.Indices=Range.Va;CHECK(SubmitRaster(&F)==S_OK && F.Device.PendingCount==2 && Vertex->Pending==2 && Vertex->OldCount==1);
    R=FinishRaster(&F);if(R)return R;
    const UINT Ops[]={HeapNew,NewEvent,CreateAllocation,MapVa,LockBo,ResidentBo,CacheBo,UnlockBo,WaitGpu,SubmitGpu,SignalGpu};
    const UINT Counts[]={1,1,1,1,1,5,1,1,1,1,1};
    for(UINT I=0;I<sizeof(Ops)/sizeof(Ops[0]);I++)for(UINT N=1;N<=Counts[I];N++) {
        R=InitRaster(&F,1);if(R)return R;FailOp=Ops[I];FailAt=Calls[FailOp]+N;Sticky=Ops[I]==SignalGpu;
        CHECK(FAILED(SubmitRaster(&F)) && !MockError);
        if(Ops[I]==SignalGpu) {
            CHECK(F.Device.PendingCount==1 && F.Device.Programs->Pending==1 && LastSubmitted &&
                F.Device.Error==DXGI_ERROR_DEVICE_REMOVED && LiveEvents()==1);
            FailOp=0;Functions.pfnFlush(Device);CHECK(!LastSubmitted && F.Device.PendingCount==1);
        } else CHECK(!F.Device.PendingCount && !F.Device.Programs->Pending && !LastSubmitted && LiveBos()==7);
        FailOp=0;R=FinishRaster(&F);if(R)return R;
    }
    /* Committed residency and pending CPU paging fences retain ownership even
       when no command reached the GPU. Cleanup is resumed after fence advance. */
    R=InitRaster(&F,1);if(R)return R;AsyncPaging=PartialResidentFailure=1;PagingValue=0;
    FailOp=ResidentBo;FailAt=Calls[FailOp]+1;
    CHECK(FAILED(SubmitRaster(&F)) && F.Device.PendingCount==1 && F.Device.Programs->Pending==1 && !Calls[SubmitGpu]);
    CHECK(F.Device.Pending->Resources[0]->Bo.Resident && !Calls[FreeVa] && !MockError);
    FailOp=0;CHECK(MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY));PagingValue=PagingNext;
    R=FinishRaster(&F);if(R)return R;
    R=InitRaster(&F,1);if(R)return R;FailOp=UnlockBo;FailAt=Calls[FailOp]+1;Sticky=1;
    CHECK(FAILED(SubmitRaster(&F)) && F.Device.PendingCount==1 && F.Device.Pending->Bo[0].Locked);
    CHECK(F.Device.Programs->Pending==1 && !Calls[SubmitGpu] && !MockError);
    FailOp=0;R=FinishRaster(&F);if(R)return R;
    /* Maximal public resource list reaches the real wire/parser and event
       ownership tables, rather than only checking the count guard. */
    R=InitRaster(&F,1);if(R)return R;UMD_TOKEN Extra[121];MALI_UMD_RASTER_RESOURCE Many[125];
    CopyMemory(Many,F.Bindings,sizeof(F.Bindings));
    for(UINT I=0;I<121;I++) {
        D3D10DDI_MIPINFO M;D3D10DDIARG_CREATERESOURCE A=BufferArgs(&M,4096,D3D10_DDI_USAGE_DEFAULT,D3D10_DDI_BIND_SHADER_RESOURCE,0);
        CreateOnly(&F.Device,&Extra[I],&A);CHECK(Extra[I].Id);
        Many[I+4].Resource.pDrvPrivate=&Extra[I];Many[I+4].Access=MaliPteRead;
    }
    CHECK(MtkMaliUmdSubmitRaster(Device,&F.Program,&F.Draw,Many,125)==S_OK && !MockError);
    CHECK(PrepareWorkspace.ResourceCount==128 && LastSubmissionCount==129 && F.Device.ResourceCount==125);
    for(UINT I=0;I<121;I++)DestroyOnly(&F.Device,&Extra[I]);
    CHECK(F.Device.ResourceCount==125);R=FinishRaster(&F);if(R)return R;
    R=InitRaster(&F,1);if(R)return R;
    for(UINT I=0;I<UMD_PENDING_LIMIT;I++)CHECK(SubmitRaster(&F)==S_OK);
    CHECK(SubmitRaster(&F)==E_OUTOFMEMORY && F.Device.PendingCount==UMD_PENDING_LIMIT && Calls[SubmitGpu]==UMD_PENDING_LIMIT);
    CHECK(F.Device.Programs->Pending==UMD_PENDING_LIMIT && !MockError);Complete();Functions.pfnFlush(Device);
    CHECK(!F.Device.PendingCount && !F.Device.Programs->Pending && SubmitRaster(&F)==S_OK);
    R=FinishRaster(&F);if(R)return R;
    const UINT CleanupOps[]={FreeVa,DestroyAllocation,EventClose,WaitEvent};
    for(UINT I=0;I<4;I++) {
        R=InitRaster(&F,1);if(R)return R;CHECK(SubmitRaster(&F)==S_OK);Complete();
        FailOp=CleanupOps[I];FailAt=Calls[FailOp]+1;Sticky=1;Functions.pfnFlush(Device);
        CHECK(F.Device.PendingCount==1 && F.Device.Programs->Pending==1 && F.Device.ResourceCount==4 && !MockError);
        FailOp=0;R=FinishRaster(&F);if(R)return R;
    }
    for(UINT Case=0;Case<17;Case++) {
        R=InitRaster(&F,1);if(R)return R;
        switch(Case) {
        case 0:F.Program.Owner=0;break;case 1:F.Program.Id++;break;
        case 2:F.Draw.Bytes--;break;case 3:F.Draw.Position.FauCount++;break;
        case 4:F.Draw.Fragment.FauCount--;break;case 5:F.Draw.Indices++;break;
        case 6:F.Bindings[1]=F.Bindings[0];break;case 7:F.Bindings[0].Access=MaliPteWrite;break;
        case 8:F.Bindings[0].Access=MaliPteRead|MaliPteExecute;break;
        case 9:F.Resources[0].Owner=0;break;case 10:F.Device.Caps.JobFeatures[1]=0;break;
        case 11:F.Draw.Tiler=F.Program.Descriptors.Va;break;
        case 12:F.Draw.Framebuffer=F.Draw.Indices|1u;F.Draw.Indices=0;break;
        case 13:F.Draw.Position.ThreadStorage=0;break;
        case 14:F.Draw.Flags[0]=2u<<14;break;case 15:F.Device.PendingCount=UMD_PENDING_LIMIT;break;
        case 16:F.Device.Ready=0;break;
        }
        CHECK(FAILED(SubmitRaster(&F)) && !Calls[SubmitGpu] && !MockError);
        F.Device.PendingCount=0;F.Device.Ready=1;F.Device.Caps.JobFeatures[1]=1u<<11;
        F.Resources[0].Owner=&F.Device;F.Program.Owner=&F.Device;F.Program.Id=F.Device.Programs->Id;
        R=FinishRaster(&F);if(R)return R;
    }
    R=InitRaster(&F,1);if(R)return R;MALI_COMMAND_RESOURCE RangeOut={7,8,9};
    CHECK(MtkMaliUmdResourceRange(Device,F.Bindings[0].Resource,MaliPteWrite,&RangeOut)==E_INVALIDARG && RangeOut.Va==7);
    CHECK(MtkMaliUmdSubmitRaster(Device,&F.Program,&F.Draw,F.Bindings,126)==E_INVALIDARG);
    CHECK(MtkMaliUmdSubmitRaster(Device,&F.Program,&F.Draw,0,4)==E_INVALIDARG);
    CHECK(MtkMaliUmdSubmitRaster(Device,&F.Program,0,F.Bindings,4)==E_INVALIDARG);
    CHECK(MtkMaliUmdSubmitRaster(Device,0,&F.Draw,F.Bindings,4)==E_INVALIDARG);
    R=FinishRaster(&F);return R?R:MockError;
}
typedef struct {
    UMD_DEVICE Device;
    UMD_TOKEN Shaders[2],Target,View;
    UMD_STATE Rasterizer,Blend,Depth;
} DRAW_CASE;
static int InitDrawFormat(DRAW_CASE *F,DXGI_FORMAT Format) {
    Reset();ZeroMemory(F,sizeof(*F));CHECK(Init(&F->Device)==S_OK);
    UMD_DEVICE *D=&F->Device;D3D10DDI_HDEVICE Device={D};
    D->Caps.JobSlots|=2;D->Caps.JobFeatures[1]=1u<<11;
    CompilerMetadata[T70_SHADER_VERTEX].flags=T70_SHADER_FLAG_IDVS|T70_SHADER_FLAG_FTZ32;
    CompilerMetadata[T70_SHADER_VERTEX].no_point_size_offset=8;
    CompilerMetadata[T70_SHADER_VERTEX].fau_reserved=12;
    CompilerMetadata[T70_SHADER_VERTEX].fau_count=12;
    CompilerMetadata[T70_SHADER_PIXEL].flags=T70_SHADER_FLAG_FTZ32;
    CompilerMetadata[T70_SHADER_PIXEL].fau_reserved=0;
    CompilerMetadata[T70_SHADER_PIXEL].fau_count=0;
    D3D10DDIARG_SIGNATURE_ENTRY VIn={D3D10_SB_NAME_VERTEX_ID,0,1};
    D3D10DDIARG_SIGNATURE_ENTRY VOut={D3D10_SB_NAME_POSITION,0,15};
    D3D10DDIARG_SIGNATURE_ENTRY POut={D3D10_SB_NAME_UNDEFINED,0,15};
    D3D10DDIARG_STAGE_IO_SIGNATURES VSig={&VIn,1,&VOut,1};
    D3D10DDIARG_STAGE_IO_SIGNATURES PSig={0,0,&POut,1};
    CreateShader(D,&F->Shaders[0],T70_SHADER_VERTEX,VertexTokens,&VSig);
    CreateShader(D,&F->Shaders[1],T70_SHADER_PIXEL,PixelTokens,&PSig);
    CHECK(F->Shaders[0].Id && F->Shaders[1].Id);
    D3D10DDI_HSHADER VS={&F->Shaders[0]},PS={&F->Shaders[1]};
    Functions.pfnVsSetShader(Device,VS);Functions.pfnPsSetShader(Device,PS);
    D3D10DDI_MIPINFO M;D3D10DDIARG_CREATERESOURCE A=ResourceArgs(&M,Format,17,19);
    D3D10DDI_HRESOURCE Resource={&F->Target};D3D10DDI_HRTRESOURCE RuntimeResource={0};
    CHECK(Functions.pfnCalcPrivateResourceSize(Device,&A)==sizeof(UMD_TOKEN));
    Functions.pfnCreateResource(Device,&A,Resource,RuntimeResource);CHECK(F->Target.Id && D->Resources->Live);
    D3D10DDIARG_CREATERENDERTARGETVIEW ViewDesc={0};ViewDesc.hDrvResource=Resource;
    ViewDesc.Format=Format;ViewDesc.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;
    ViewDesc.Tex2D.ArraySize=1;
    D3D10DDI_HRENDERTARGETVIEW View={&F->View};D3D10DDI_HRTRENDERTARGETVIEW RuntimeView={0};
    CHECK(Functions.pfnCalcPrivateRenderTargetViewSize(Device,&ViewDesc)==sizeof(UMD_TOKEN));
    Functions.pfnCreateRenderTargetView(Device,&ViewDesc,View,RuntimeView);CHECK(F->View.Id);
    D3D10DDI_HRENDERTARGETVIEW Targets[1]={View};D3D10DDI_HDEPTHSTENCILVIEW NoDepth={0};
    Functions.pfnSetRenderTargets(Device,Targets,1,0,NoDepth);
    Functions.pfnIaSetTopology(Device,D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D10_DDI_VIEWPORT Port={0,0,17,19,0,1};Functions.pfnSetViewports(Device,1,0,&Port);
    D3D10_DDI_RASTERIZER_DESC RD={0};RD.FillMode=D3D10_DDI_FILL_SOLID;
    RD.CullMode=D3D10_DDI_CULL_BACK;RD.DepthClipEnable=TRUE;
    CHECK(Functions.pfnCalcPrivateRasterizerStateSize(Device,&RD)==sizeof(UMD_STATE));
    D3D10DDI_HRTRASTERIZERSTATE RtRaster={0};D3D10DDI_HRASTERIZERSTATE Raster={&F->Rasterizer};
    Functions.pfnCreateRasterizerState(Device,&RD,Raster,RtRaster);CHECK(F->Rasterizer.Signature==UMD_STATE_SIGNATURE);
    Functions.pfnSetRasterizerState(Device,Raster);
    D3D10_DDI_BLEND_DESC BD={0};BD.RenderTargetWriteMask[0]=D3D10_DDI_COLOR_WRITE_ENABLE_ALL;
    CHECK(Functions.pfnCalcPrivateBlendStateSize(Device,&BD)==sizeof(UMD_STATE));
    D3D10DDI_HRTBLENDSTATE RtBlend={0};D3D10DDI_HBLENDSTATE Blend={&F->Blend};
    Functions.pfnCreateBlendState(Device,&BD,Blend,RtBlend);CHECK(F->Blend.Signature==UMD_STATE_SIGNATURE);
    FLOAT Factor[4]={0,0,0,0};Functions.pfnSetBlendState(Device,Blend,Factor,UINT_MAX);
    D3D10_DDI_DEPTH_STENCIL_DESC DD={0};
    CHECK(Functions.pfnCalcPrivateDepthStencilStateSize(Device,&DD)==sizeof(UMD_STATE));
    D3D10DDI_HRTDEPTHSTENCILSTATE RtDepth={0};D3D10DDI_HDEPTHSTENCILSTATE Depth={&F->Depth};
    Functions.pfnCreateDepthStencilState(Device,&DD,Depth,RtDepth);
    CHECK(F->Depth.Signature==UMD_STATE_SIGNATURE);Functions.pfnSetDepthStencilState(Device,Depth,0);
    CHECK(!D->DrawInvalidMask && D->SampleMask==UINT_MAX && !D->VertexBufferMask &&
        !D->IndexBufferBound && !D->InputLayoutBound && !ErrorCalls && !MockError);
    return 0;
}
static int InitDraw(DRAW_CASE *F) {return InitDrawFormat(F,DXGI_FORMAT_R8G8B8A8_UNORM);}
static void DestroyDraw(DRAW_CASE *F) {
    UMD_DEVICE *D=&F->Device;D3D10DDI_HDEVICE Device={D};
    Functions.pfnDestroyRasterizerState(Device,(D3D10DDI_HRASTERIZERSTATE){&F->Rasterizer});
    Functions.pfnDestroyBlendState(Device,(D3D10DDI_HBLENDSTATE){&F->Blend});
    Functions.pfnDestroyDepthStencilState(Device,(D3D10DDI_HDEPTHSTENCILSTATE){&F->Depth});
    DeletePair(D,F->Shaders);
    if(F->Target.Id)Functions.pfnDestroyResource(Device,(D3D10DDI_HRESOURCE){&F->Target});
    if(F->View.Id)Functions.pfnDestroyRenderTargetView(Device,(D3D10DDI_HRENDERTARGETVIEW){&F->View});
}
static int TestD3dDraw(void) {
    DRAW_CASE F;int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
    UINT SubmitBefore=Calls[SubmitGpu],Errors=ErrorCalls;
    Functions.pfnDraw(Device,0,0);CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors);
    Functions.pfnIaSetTopology(Device,(D3D10_DDI_PRIMITIVE_TOPOLOGY)3);
    Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+2);Errors=ErrorCalls;
    Functions.pfnIaSetTopology(Device,D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D10DDI_HRESOURCE Bound[1]={{(void *)(uintptr_t)1}};UINT Stride[1]={16},Offset[1]={0};
    Functions.pfnIaSetVertexBuffers(Device,0,1,Bound,Stride,Offset);Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+2);Errors=ErrorCalls;
    D3D10DDI_HRESOURCE NullBuffer[1]={{0}};UINT Zero[1]={0};
    Functions.pfnIaSetVertexBuffers(Device,0,1,NullBuffer,Zero,Zero);
    Functions.pfnIaSetIndexBuffer(Device,(D3D10DDI_HRESOURCE){(void *)(uintptr_t)1},DXGI_FORMAT_R16_UINT,0);
    Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+2);Errors=ErrorCalls;
    Functions.pfnIaSetIndexBuffer(Device,(D3D10DDI_HRESOURCE){0},DXGI_FORMAT_UNKNOWN,0);
    Functions.pfnIaSetInputLayout(Device,(D3D10DDI_HELEMENTLAYOUT){(void *)(uintptr_t)1});
    Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+2);Errors=ErrorCalls;
    Functions.pfnIaSetInputLayout(Device,(D3D10DDI_HELEMENTLAYOUT){0});
    D3D10_DDI_VIEWPORT Narrow={0,0,16,19,0,1};Functions.pfnSetViewports(Device,1,0,&Narrow);
    Functions.pfnDraw(Device,3,0);CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+1);Errors=ErrorCalls;
    D3D10_DDI_VIEWPORT Full={0,0,17,19,0,1};Functions.pfnSetViewports(Device,1,0,&Full);
    D3D10_DDI_RASTERIZER_DESC ScissorDesc={0};ScissorDesc.FillMode=D3D10_DDI_FILL_SOLID;
    ScissorDesc.CullMode=D3D10_DDI_CULL_BACK;ScissorDesc.DepthClipEnable=TRUE;ScissorDesc.ScissorEnable=TRUE;
    UMD_STATE ScissorState={0};D3D10DDI_HRASTERIZERSTATE ScissorHandle={&ScissorState};
    D3D10DDI_HRTRASTERIZERSTATE ScissorRuntime={0};
    CHECK(Functions.pfnCalcPrivateRasterizerStateSize(Device,&ScissorDesc)==sizeof(UMD_STATE));
    Functions.pfnCreateRasterizerState(Device,&ScissorDesc,ScissorHandle,ScissorRuntime);
    Functions.pfnSetRasterizerState(Device,ScissorHandle);Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+1);Errors=ErrorCalls;
    Functions.pfnSetRasterizerState(Device,(D3D10DDI_HRASTERIZERSTATE){&F.Rasterizer});
    const struct t70_shader_signature_entry *OriginalInputs=F.Device.VertexShader->Source.inputs;
    struct t70_shader_signature_entry InstanceInput=OriginalInputs[0];
    InstanceInput.system_value=D3D10_SB_NAME_INSTANCE_ID;
    F.Device.VertexShader->Source.inputs=&InstanceInput;
    Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+1);Errors=ErrorCalls;
    F.Device.VertexShader->Source.inputs=OriginalInputs;
    Functions.pfnDraw(Device,4,0);CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+1);Errors=ErrorCalls;
    D3D10DDI_HRENDERTARGETVIEW TwoTargets[2]={{&F.View},{&F.View}};
    Functions.pfnSetRenderTargets(Device,TwoTargets,2,0,(D3D10DDI_HDEPTHSTENCILVIEW){0});
    Functions.pfnDraw(Device,3,0);CHECK(Calls[SubmitGpu]==SubmitBefore && ErrorCalls==Errors+2);Errors=ErrorCalls;
    Functions.pfnSetRenderTargets(Device,TwoTargets,1,0,(D3D10DDI_HDEPTHSTENCILVIEW){0});
    CHECK(!F.Device.DrawInvalidMask && !MockError);
    Functions.pfnDraw(Device,3,5);
    CHECK(Calls[SubmitGpu]==SubmitBefore+1 && ErrorCalls==Errors && !MockError);
    CHECK(F.Device.PendingCount==1 && F.Device.ResourceCount==2 && F.Device.ProgramCount==1);
    CHECK(LastSubmissionCount==7 && !LastSubmitted);
    TEST_BO *Cmd=TestBo(LastSubmission[0]);const UINT *W=(const UINT *)Cmd->Cpu;
    CHECK(Cmd->Bytes==4096 && PrepareWorkspace.JobCount==2 && PrepareWorkspace.ResourceCount==6);
    CHECK(W[8]==(8u|(1u<<15)|(1u<<16)|(1u<<17)) && W[9]==5 && W[11]==3 && W[12]==1);
    CHECK(W[14]==(UINT)(F.Device.Resources->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET+192u) &&
        W[26]==0 && W[27]==((18u<<16)|16u) && W[28]==0x3f800000);
    CHECK(W[32]==((1u<<0)|(1u<<1)|(1u<<18)|(1u<<23)) && W[33]==0x1ffff);
    CHECK(W[38]==0 && W[39]==0x3f800000 && W[44]==0 && W[45]==0);
    CHECK(W[58]==(UINT)(F.Device.Programs->Bo[2].Va+96u) && W[59]==(UINT)((F.Device.Programs->Bo[2].Va+96u)>>32));
    CHECK(W[65]==6 && W[72]==0 && W[73]==0 && W[74]==(UINT)(F.Device.Programs->Bo[2].Va+32u) &&
        W[75]==(UINT)((F.Device.Programs->Bo[2].Va+32u)>>32));
    CHECK(W[76]==(UINT)(F.Device.Resources->Bo.Va+UMD_DRAW_TLS_OFFSET) &&
        W[78]==(UINT)(F.Device.Resources->Bo.Va+UMD_DRAW_VERTEX_FAU_OFFSET));
    UMD_RESOURCE *Scratch=F.Device.Resources;while(Scratch && Scratch->Desc.Size!=UMD_DRAW_SCRATCH_BYTES)Scratch=Scratch->Next;
    CHECK(Scratch && Scratch->Pending==1);
    TEST_BO *ScratchBo=TestVa(Scratch->Bo.Va);UINT32 *Fb=(UINT32 *)(ScratchBo->Cpu+UMD_DRAW_DESCRIPTOR_OFFSET);
    UMD_RESOURCE *Target=UmdFindResource(&F.Device,&F.Target);CHECK(Target && Target->Bo.Va);
    CHECK(W[104]==0 && W[105]==0x00010001 && W[106]==(UINT)(Scratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET|1u));
    CHECK(((UINT64 *)Fb)[20]==Target->Bo.Va && Fb[42]==128 && Fb[43]==128*19 &&
        Fb[44]==0 && Fb[45]==0 && Fb[46]==0 && Fb[47]==0 && Fb[128]==0x00800080 && Fb[160]==0x00800080);
    CHECK(((UINT32 *)(ScratchBo->Cpu+UMD_DRAW_TLS_OFFSET))[1]==31);
    UINT64 FrameVa=Scratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET;
    unsigned char *FrameBytes=ScratchBo->Cpu+UMD_DRAW_DESCRIPTOR_OFFSET;
    CHECK(Fb[0]==1u && ((UINT64 *)FrameBytes)[3]==FrameVa+MALI_COMMAND_PRELOAD_DCD0_OFFSET);
    const UINT64 *Dcd=(const UINT64 *)(FrameBytes+MALI_COMMAND_PRELOAD_DCD0_OFFSET);
    CHECK(Dcd[12]==((FrameVa+MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET)|4u) &&
        Dcd[13]==F.Device.Programs->Bo[2].Va+128u &&
        Dcd[14]==Scratch->Bo.Va+UMD_DRAW_TLS_OFFSET && Dcd[15]==Scratch->Bo.Va+UMD_DRAW_PRELOAD_FAU_OFFSET);
    CHECK(((const UINT *)Dcd)[17]==(F.Device.Programs->Preload.fau_count+1u)/2u);
    const UINT64 *Plane=(const UINT64 *)(FrameBytes+MALI_COMMAND_PRELOAD_PLANE_OFFSET);
    CHECK(Plane[1]==Target->Bo.Va && ((const UINT *)Plane)[4]==Target->Desc.Pitch);
    for(UINT I=MALI_COMMAND_PRELOAD_DCD1_OFFSET;I<MALI_COMMAND_PRELOAD_DCD2_OFFSET+MALI_COMMAND_PRELOAD_DCD_BYTES;I++)
        CHECK(!FrameBytes[I]);
    UINT *Vfau=(UINT *)(ScratchBo->Cpu+UMD_DRAW_VERTEX_FAU_OFFSET);
    CHECK(Vfau[0]==0x41080000 && Vfau[1]==0xc1180000 && Vfau[2]==0x3f800000 &&
        Vfau[4]==0x41080000 && Vfau[5]==0x41180000 && Vfau[6]==0 && Vfau[8]==5 && Vfau[10]==0);
    UINT *Lfau=(UINT *)(ScratchBo->Cpu+UMD_DRAW_PRELOAD_FAU_OFFSET);
    CHECK(Lfau[0]==0 && Lfau[1]==((18u<<16)|16u) && Lfau[2]==0x12345678u && Lfau[3]==0x90abcdefu);
    CHECK(Calls[CompilePreload]==1 && LastPreloadRequest.format==T70_SHADER_FB_RGBA8_UNORM &&
        F.Device.Programs->Preload.binary && F.Device.Programs->Bo[3].Resident);
    TEST_BO *LoadCode=TestBo(F.Device.Programs->Bo[3].Allocation);
    /* GPU-VA protection: implicit read, Execute set and Write clear. */
    CHECK(LoadCode && LoadCode->Protection==2u && EqualBytes(LoadCode->Cpu,PreloadCode,PreloadCodeBytes));
    Errors=ErrorCalls;SubmitBefore=Calls[SubmitGpu];UINT64 Cached=F.Device.DrawProgram.Id;
    UINT Linked=Calls[CompilePipeline],Preloaded=Calls[CompilePreload];
    Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==SubmitBefore+1 && ErrorCalls==Errors && F.Device.PendingCount==2 && !MockError);
    CHECK(Cached && F.Device.DrawProgram.Id==Cached && F.Device.ProgramCount==1 &&
        Calls[CompilePipeline]==Linked && Calls[CompilePreload]==Preloaded && F.Device.Programs->Live);
    DestroyDraw(&F);Complete();Functions.pfnFlush(Device);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveEvents() && !LiveHeap() && !CompilerLive && !MockError);
    return 0;
}
static int TestDrawExistingContents(void) {
    DRAW_CASE F;int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
    D3D10DDI_HRESOURCE Resource={&F.Target};D3D10DDIARG_CREATERENDERTARGETVIEW Desc={0};
    Desc.hDrvResource=Resource;Desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    Desc.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;Desc.Tex2D.ArraySize=1;
    UMD_TOKEN AliasToken={0};D3D10DDI_HRENDERTARGETVIEW Alias={&AliasToken};D3D10DDI_HRTRENDERTARGETVIEW Runtime={0};
    CHECK(Functions.pfnCalcPrivateRenderTargetViewSize(Device,&Desc)==sizeof(AliasToken));
    Functions.pfnCreateRenderTargetView(Device,&Desc,Alias,Runtime);CHECK(AliasToken.Id);
    UINT Before=Calls[SubmitGpu],Errors=ErrorCalls;uint32_t ColorBits[4]={0x3f800000,0x3f000000,0,0x3f800000};
    Functions.pfnClearRenderTargetView(Device,Alias,(FLOAT *)ColorBits);
    CHECK(Calls[SubmitGpu]==Before+1 && !MockError);
    Functions.pfnDraw(Device,3,0);CHECK(Calls[SubmitGpu]==Before+2 && ErrorCalls==Errors && Calls[CompilePreload]==1 && !MockError);
    Functions.pfnDestroyRenderTargetView(Device,Alias);DestroyDraw(&F);Complete();Functions.pfnFlush(Device);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveEvents() && !LiveHeap() && !CompilerLive && !MockError);

    R=InitDraw(&F);if(R)return R;Device.pDrvPrivate=&F.Device;
    UMD_TOKEN Source={0};D3D10DDI_MIPINFO M;D3D10DDIARG_CREATERESOURCE A=ResourceArgs(&M,DXGI_FORMAT_R8G8B8A8_UNORM,17,19);
    CreateOnly(&F.Device,&Source,&A);CHECK(Source.Id);
    D3D10DDI_HRESOURCE DestResource={&F.Target},SourceResource={&Source};
    D3D10_DDI_BOX CopyBox={0,0,0,1,1,1};Before=Calls[SubmitGpu];Errors=ErrorCalls;
    Functions.pfnResourceCopyRegion(Device,DestResource,0,0,0,0,SourceResource,0,&CopyBox);
    CHECK(Calls[SubmitGpu]==Before && !MockError);
    Functions.pfnDraw(Device,3,0);CHECK(Calls[SubmitGpu]==Before+1 && ErrorCalls==Errors && !MockError);
    DestroyOnly(&F.Device,&Source);DestroyDraw(&F);Complete();Functions.pfnFlush(Device);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !CompilerLive && !MockError);

    R=InitDraw(&F);if(R)return R;Device.pDrvPrivate=&F.Device;Before=Calls[SubmitGpu];Errors=ErrorCalls;
    UINT Pixel=0;D3D10_DDI_BOX UpdateBox={0,0,0,1,1,1};
    Functions.pfnResourceUpdateSubresourceUP(Device,(D3D10DDI_HRESOURCE){&F.Target},0,&UpdateBox,&Pixel,sizeof(Pixel),0);
    CHECK(Calls[SubmitGpu]==Before && !MockError);
    Functions.pfnDraw(Device,3,0);CHECK(Calls[SubmitGpu]==Before+1 && ErrorCalls==Errors && !MockError);
    DestroyDraw(&F);Complete();Functions.pfnFlush(Device);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !CompilerLive && !MockError);

    R=InitDraw(&F);if(R)return R;Device.pDrvPrivate=&F.Device;
    UINT Pixels[17u*19u]={0};D3D10_DDIARG_SUBRESOURCE_UP Initial={Pixels,17u*4u,0};
    A=ResourceArgs(&M,DXGI_FORMAT_R8G8B8A8_UNORM,17,19);
    A.pInitialDataUP=&Initial;UMD_TOKEN Defined={0},DefinedView={0};CreateOnly(&F.Device,&Defined,&A);
    CHECK(Defined.Id);
    D3D10DDI_HRESOURCE DefinedResource={&Defined};
    ZeroMemory(&Desc,sizeof(Desc));
    Desc.hDrvResource=DefinedResource;Desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    Desc.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;Desc.Tex2D.ArraySize=1;
    D3D10DDI_HRENDERTARGETVIEW DefinedHandle={&DefinedView};D3D10DDI_HRTRENDERTARGETVIEW DefinedRuntime={0};
    CHECK(Functions.pfnCalcPrivateRenderTargetViewSize(Device,&Desc)==sizeof(DefinedView));
    Functions.pfnCreateRenderTargetView(Device,&Desc,DefinedHandle,DefinedRuntime);CHECK(DefinedView.Id);
    D3D10DDI_HRENDERTARGETVIEW DefinedTargets[1]={DefinedHandle};
    Functions.pfnSetRenderTargets(Device,DefinedTargets,1,0,(D3D10DDI_HDEPTHSTENCILVIEW){0});
    Before=Calls[SubmitGpu];Errors=ErrorCalls;Functions.pfnDraw(Device,3,0);
    CHECK(Calls[SubmitGpu]==Before+1 && ErrorCalls==Errors && !MockError);
    D3D10DDI_HRENDERTARGETVIEW OriginalTargets[1]={{&F.View}};
    Functions.pfnSetRenderTargets(Device,OriginalTargets,1,0,(D3D10DDI_HDEPTHSTENCILVIEW){0});
    Functions.pfnDestroyRenderTargetView(Device,DefinedHandle);DestroyOnly(&F.Device,&Defined);
    DestroyDraw(&F);Complete();Functions.pfnFlush(Device);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !CompilerLive && !MockError);
    return 0;
}
static int TestDrawProgramCache(void) {
    DRAW_CASE F;int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
    Functions.pfnDraw(Device,3,0);
    UINT64 First=F.Device.DrawProgram.Id;UINT Linked=Calls[CompilePipeline],Preloaded=Calls[CompilePreload];
    CHECK(First && F.Device.ProgramCount==1 && F.Device.Programs->Live && !MockError);
    UINT Errors=ErrorCalls,Submits=Calls[SubmitGpu];
    FailOp=HeapNew;FailAt=Calls[HeapNew]+1;
    Functions.pfnDraw(Device,3,0);
    CHECK(ErrorCalls==Errors+1 && Calls[SubmitGpu]==Submits && F.Device.DrawProgram.Id==First &&
        F.Device.ProgramCount==1 && Calls[CompilePipeline]==Linked && Calls[CompilePreload]==Preloaded && !MockError);
    FailOp=0;Functions.pfnDraw(Device,3,3);
    CHECK(Calls[SubmitGpu]==Submits+1 && F.Device.PendingCount==2 && F.Device.DrawProgram.Id==First &&
        F.Device.ProgramCount==1 && Calls[CompilePipeline]==Linked && Calls[CompilePreload]==Preloaded && !MockError);
    Complete();Functions.pfnFlush(Device);
    CHECK(!F.Device.PendingCount && F.Device.ProgramCount==1 && F.Device.DrawProgram.Id==First &&
        F.Device.Programs->Live && LiveBos()==5 && CompilerLive==3 && !MockError);

    UMD_TOKEN Other={0};D3D10DDIARG_SIGNATURE_ENTRY POut={D3D10_SB_NAME_UNDEFINED,0,15};
    D3D10DDIARG_STAGE_IO_SIGNATURES PSig={0,0,&POut,1};
    CreateShader(&F.Device,&Other,T70_SHADER_PIXEL,PixelTokens,&PSig);CHECK(Other.Id);
    Functions.pfnPsSetShader(Device,(D3D10DDI_HSHADER){&Other});
    Errors=ErrorCalls;Submits=Calls[SubmitGpu];FailOp=CompilePipeline;FailAt=Calls[CompilePipeline]+1;
    Functions.pfnDraw(Device,3,0);
    CHECK(ErrorCalls==Errors+1 && Calls[SubmitGpu]==Submits && F.Device.DrawProgram.Id==First &&
        F.Device.ProgramCount==1 && F.Device.Programs->Live && CompilerLive==3 && !MockError);
    FailOp=0;Functions.pfnPsSetShader(Device,(D3D10DDI_HSHADER){&F.Shaders[1]});
    Linked=Calls[CompilePipeline];Preloaded=Calls[CompilePreload];Functions.pfnDraw(Device,3,0);
    CHECK(Calls[CompilePipeline]==Linked && Calls[CompilePreload]==Preloaded &&
        F.Device.DrawProgram.Id==First && F.Device.PendingCount==1 && !MockError);
    Functions.pfnPsSetShader(Device,(D3D10DDI_HSHADER){&Other});
    Functions.pfnDraw(Device,3,0);
    CHECK(F.Device.DrawProgram.Id && F.Device.DrawProgram.Id!=First && F.Device.DrawPixel==Other.Id &&
        F.Device.ProgramCount==2 && F.Device.PendingCount==2 && Calls[CompilePipeline]==Linked+1 &&
        Calls[CompilePreload]==Preloaded+1 && !MockError);
    UMD_PROGRAM *Old=F.Device.Programs;
    while(Old && Old->Id!=First)Old=Old->Next;
    CHECK(Old && !Old->Live && Old->Pending==1 && CompilerLive==6);
    Functions.pfnDestroyShader(Device,(D3D10DDI_HSHADER){&Other});
    CHECK(!Other.Id && !F.Device.DrawProgram.Id && !F.Device.DrawPixel && !F.Device.PixelShader &&
        F.Device.ProgramCount==2 && CompilerLive==6 && !MockError);
    DestroyDraw(&F);Complete();Functions.pfnFlush(Device);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveEvents() && !LiveHeap() && !CompilerLive && !MockError);

    R=InitDraw(&F);if(R)return R;Device.pDrvPrivate=&F.Device;
    Functions.pfnDraw(Device,3,0);First=F.Device.DrawProgram.Id;Linked=Calls[CompilePipeline];
    UMD_TOKEN Target={0},ViewToken={0};D3D10DDI_MIPINFO M;
    D3D10DDIARG_CREATERESOURCE A=ResourceArgs(&M,DXGI_FORMAT_B8G8R8A8_UNORM,17,19);
    CreateOnly(&F.Device,&Target,&A);CHECK(Target.Id);
    D3D10DDIARG_CREATERENDERTARGETVIEW Desc={0};Desc.hDrvResource=(D3D10DDI_HRESOURCE){&Target};
    Desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;Desc.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;Desc.Tex2D.ArraySize=1;
    D3D10DDI_HRENDERTARGETVIEW View={&ViewToken};
    Functions.pfnCreateRenderTargetView(Device,&Desc,View,(D3D10DDI_HRTRENDERTARGETVIEW){0});CHECK(ViewToken.Id);
    Functions.pfnSetRenderTargets(Device,&View,1,0,(D3D10DDI_HDEPTHSTENCILVIEW){0});
    Functions.pfnDraw(Device,3,0);
    CHECK(F.Device.DrawProgram.Id && F.Device.DrawProgram.Id!=First &&
        F.Device.DrawFormat==DXGI_FORMAT_B8G8R8A8_UNORM && F.Device.ProgramCount==2 &&
        Calls[CompilePipeline]==Linked+1 && LastPreloadRequest.format==T70_SHADER_FB_BGRA8_UNORM && !MockError);
    Functions.pfnDestroyRenderTargetView(Device,View);DestroyOnly(&F.Device,&Target);
    DestroyDraw(&F);Complete();Functions.pfnFlush(Device);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveEvents() && !LiveHeap() && !CompilerLive && !MockError);

    R=InitDraw(&F);if(R)return R;Device.pDrvPrivate=&F.Device;
    Functions.pfnDraw(Device,3,0);First=F.Device.DrawProgram.Id;
    Complete();Functions.pfnFlush(Device);Errors=ErrorCalls;
    FailOp=IdleGpu;FailAt=Calls[IdleGpu]+1;Sticky=1;
    Functions.pfnDestroyShader(Device,(D3D10DDI_HSHADER){&F.Shaders[0]});
    CHECK(!F.Shaders[0].Id && !F.Device.DrawProgram.Id && !F.Device.DrawVertex &&
        F.Device.ProgramCount==1 && !F.Device.Programs->Live && F.Device.Programs->Id==First &&
        ErrorCalls==Errors+1 && FAILED(MtkMaliUmdFailureReason(Device)) && !MockError);
    FailOp=0;Functions.pfnFlush(Device);DestroyDraw(&F);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveEvents() && !LiveHeap() && !CompilerLive && !MockError);
    return 0;
}
static int TestDrawPreloadOtherFormats(void) {
    const struct {
        DXGI_FORMAT DdiFormat;
        UINT ShaderFormat,TextureOrder,TargetFormat,WritebackSwizzle;
    } Cases[]={
        {DXGI_FORMAT_B8G8R8A8_UNORM,T70_SHADER_FB_BGRA8_UNORM,4u,MaliClearBgra8,0x60au},
        {DXGI_FORMAT_B8G8R8X8_UNORM,T70_SHADER_FB_BGRX8_UNORM,20u,MaliClearBgrx8,0x60au}
    };
    for(UINT Case=0;Case<sizeof(Cases)/sizeof(Cases[0]);Case++) {
        DRAW_CASE F;int R=InitDrawFormat(&F,Cases[Case].DdiFormat);if(R)return R;
        D3D10DDI_HDEVICE Device={&F.Device};UMD_RESOURCE *Target=UmdFindResource(&F.Device,&F.Target);
        CHECK(Target && Target->Desc.Format==Cases[Case].TargetFormat && Target->Desc.Pitch==128 &&
            Target->Bo.Va && Target->Bo.Bytes==4096);
        UINT Errors=ErrorCalls,Submits=Calls[SubmitGpu];
        Functions.pfnDraw(Device,3,0);
        CHECK(Calls[SubmitGpu]==Submits+1 && Calls[CompilePreload]==1 &&
            ErrorCalls==Errors && F.Device.PendingCount==1 && !MockError);
        UMD_RESOURCE *Scratch=F.Device.Resources;
        while(Scratch && Scratch->Desc.Size!=UMD_DRAW_SCRATCH_BYTES)Scratch=Scratch->Next;
        CHECK(Scratch && Scratch->Pending==1);
        TEST_BO *ScratchBo=TestVa(Scratch->Bo.Va);
        unsigned char *Frame=ScratchBo->Cpu+UMD_DRAW_DESCRIPTOR_OFFSET;
        UINT *Words=(UINT *)Frame;
        UINT TextureType=Words[MALI_COMMAND_PRELOAD_TEXTURE_OFFSET/4u];
        UINT ExpectedTexture=2u|(2u<<4)|(187u<<22)|(Cases[Case].TextureOrder<<10);
        UINT64 *Texture=(UINT64 *)(Frame+MALI_COMMAND_PRELOAD_TEXTURE_OFFSET);
        UINT64 *Plane=(UINT64 *)(Frame+MALI_COMMAND_PRELOAD_PLANE_OFFSET);
        UINT64 *Dcd=(UINT64 *)(Frame+MALI_COMMAND_PRELOAD_DCD0_OFFSET);
        CHECK(LastPreloadRequest.format==Cases[Case].ShaderFormat &&
            LastPreloadRequest.sample_count==1 && LastPreloadRequest.array_layers==1 &&
            !LastPreloadRequest.reserved);
        CHECK(TextureType==ExpectedTexture && Words[MALI_COMMAND_PRELOAD_TEXTURE_OFFSET/4u+2u]==0x688u &&
            Texture[2]==Scratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET+MALI_COMMAND_PRELOAD_PLANE_OFFSET);
        CHECK(Words[MALI_COMMAND_PRELOAD_PLANE_OFFSET/4u]==0x0200021au &&
            Words[MALI_COMMAND_PRELOAD_PLANE_OFFSET/4u+1u]==128u*19u &&
            Plane[1]==Target->Bo.Va && Words[MALI_COMMAND_PRELOAD_PLANE_OFFSET/4u+4u]==Target->Desc.Pitch);
        CHECK(Words[132u/4u]==(1u|(19u<<3)|(2u<<8)|(1u<<15)|
            (Cases[Case].WritebackSwizzle<<16)|(1u<<31)));
        /* BGRX uses the BGR1 texture component order (order 20), so sampling
           the ignored X byte supplies alpha 1 to the LOAD_IMAGE shader. */
        if(Cases[Case].DdiFormat==DXGI_FORMAT_B8G8R8X8_UNORM)
            CHECK(((TextureType>>10)&31u)==20u);
        CHECK(Dcd[12]==((Scratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET+
            MALI_COMMAND_PRELOAD_RESOURCE_TABLE_OFFSET)|4u) &&
            Dcd[13]==F.Device.Programs->Bo[2].Va+128u &&
            ((UINT *)Dcd)[17]==(F.Device.Programs->Preload.fau_count+1u)/2u &&
            F.Device.Programs->Bo[3].Resident);
        UMD_RESOURCE *RetainedTarget=Target;
        Submits=Calls[SubmitGpu];Functions.pfnDraw(Device,3,0);
        CHECK(Calls[SubmitGpu]==Submits+1 && Calls[CompilePreload]==1 &&
            F.Device.PendingCount==2 && RetainedTarget->Pending==2 && !MockError);
        UMD_RESOURCE *SubmittedScratch=F.Device.Resources;
        while(SubmittedScratch && SubmittedScratch->Desc.Size!=UMD_DRAW_SCRATCH_BYTES)SubmittedScratch=SubmittedScratch->Next;
        CHECK(SubmittedScratch && SubmittedScratch!=Scratch && SubmittedScratch->Pending==1);
        TEST_BO *SubmittedScratchBo=TestVa(SubmittedScratch->Bo.Va);
        unsigned char *SubmittedFrame=SubmittedScratchBo->Cpu+UMD_DRAW_DESCRIPTOR_OFFSET;
        UINT *SubmittedWords=(UINT *)SubmittedFrame;
        UINT64 *SubmittedTexture=(UINT64 *)(SubmittedFrame+MALI_COMMAND_PRELOAD_TEXTURE_OFFSET);
        UINT64 *SubmittedPlane=(UINT64 *)(SubmittedFrame+MALI_COMMAND_PRELOAD_PLANE_OFFSET);
        UINT64 *SubmittedDcd=(UINT64 *)(SubmittedFrame+MALI_COMMAND_PRELOAD_DCD0_OFFSET);
        CHECK(SubmittedWords[MALI_COMMAND_PRELOAD_TEXTURE_OFFSET/4u]==ExpectedTexture &&
            SubmittedTexture[2]==SubmittedScratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET+
                MALI_COMMAND_PRELOAD_PLANE_OFFSET && SubmittedPlane[1]==RetainedTarget->Bo.Va &&
            SubmittedWords[MALI_COMMAND_PRELOAD_PLANE_OFFSET/4u+4u]==RetainedTarget->Desc.Pitch &&
            SubmittedDcd[13]==F.Device.Programs->Bo[2].Va+128u);
        const UINT *SubmittedCommand=(const UINT *)TestBo(LastSubmission[0])->Cpu;
        CHECK(SubmittedCommand[106]==(UINT)(SubmittedScratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET|1u));
        CHECK(PrepareWorkspace.ResourceCount==6);
        UINT FoundTarget=0,FoundScratch=0,FoundProgram=0,FoundPreloadCode=0;
        UINT64 ProgramCodeVa=F.Device.Programs->Bo[3].Va;
        for(UINT I=0;I<PrepareWorkspace.ResourceCount;I++) {
            const MALI_COMMAND_RESOURCE *Resource=&PrepareWorkspace.Resources[I];
            FoundTarget|=Resource->Va==RetainedTarget->Bo.Va && Resource->Bytes==RetainedTarget->Bo.Bytes &&
                Resource->Access==(MaliPteRead|MaliPteWrite);
            FoundScratch|=Resource->Va==SubmittedScratch->Bo.Va && Resource->Bytes==SubmittedScratch->Bo.Bytes &&
                Resource->Access==(MaliPteRead|MaliPteWrite);
            FoundProgram|=Resource->Va==F.Device.Programs->Bo[2].Va && Resource->Bytes==4096 &&
                Resource->Access==MaliPteRead;
            FoundPreloadCode|=Resource->Va==ProgramCodeVa && Resource->Bytes==4096 &&
                Resource->Access==(MaliPteRead|MaliPteExecute);
        }
        CHECK(FoundTarget && FoundScratch && FoundProgram && FoundPreloadCode);
        DestroyDraw(&F);Complete();Functions.pfnFlush(Device);
        CHECK(!F.Device.PendingCount && !F.Device.Resources && !F.Device.Programs &&
            !LiveBos() && !LiveEvents() && !LiveHeap() && !CompilerLive && !MockError);
        CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    return 0;
}
static int TestDrawPreloadFailures(void) {
    DRAW_CASE F;
    /* Metadata allocation, program allocation and scratch allocation can
       fail independently without retaining unpublished shader/BO ownership. */
    for(UINT At=1;At<=3;At++) {
        int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
        UINT HeapBefore=LiveHeap(),Errors=ErrorCalls;
        FailOp=HeapNew;FailAt=Calls[HeapNew]+At;
        Functions.pfnDraw(Device,3,0);
        CHECK(Calls[HeapNew]==FailAt && ErrorCalls==Errors+1 && ErrorCode==E_OUTOFMEMORY &&
            !Calls[SubmitGpu] && !F.Device.ProgramCount && F.Device.ResourceCount==1 &&
            LiveBos()==1 && LiveHeap()==HeapBefore && !CompilerLive && !MockError);
        CHECK(Calls[CompilePreload]==(At==1?0u:1u) &&
            Calls[CompilePipeline]==(At==3?1u:0u));
        FailOp=0;DestroyDraw(&F);
        CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !MockError);
    }
    for(UINT Kind=0;Kind<4;Kind++) {
        int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
        const int Failures[]={T70_SHADER_UNSUPPORTED,T70_SHADER_MEMORY,T70_SHADER_INVALID,T70_SHADER_TRANSLATION};
        CompilerFailure=Failures[Kind];FailOp=CompilePreload;FailAt=1;
        UINT Errors=ErrorCalls;Functions.pfnDraw(Device,3,0);
        CHECK(ErrorCalls==Errors+1 && !Calls[SubmitGpu] && !F.Device.ProgramCount &&
            F.Device.ResourceCount==1 && LiveBos()==1 && !CompilerLive && !MockError);
        FailOp=0;DestroyDraw(&F);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !MockError);
    }
    for(UINT Kind=0;Kind<15;Kind++) {
        int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
        switch(Kind) {
            case 0:PreloadMetadata.fau_count=129;break;
            case 1:PreloadMetadata.fau_count=1;break;
            case 2:PreloadMetadata.fau_reserved=0;break;
            case 3:PreloadMetadata.fau_const_mask[0]=0;break;
            case 4:PreloadMetadata.tls_size=1;break;
            case 5:PreloadMetadata.wls_size=1;break;
            case 6:PreloadMetadata.arch=8;break;
            case 7:PreloadMetadata.stage=T70_SHADER_VERTEX;break;
            case 8:PreloadMetadata.attributes_read=1;break;
            case 9:PreloadMetadata.varying_count=1;break;
            case 10:PreloadMetadata.cb_mask=1;break;
            case 11:PreloadMetadata.flags|=T70_SHADER_FLAG_DISCARD;break;
            case 12:PreloadMetadata.work_registers=0;break;
            case 13:PreloadCodeBytes=0;break;
            case 14:PreloadCodeBytes=7;break;
        }
        UINT Errors=ErrorCalls;Functions.pfnDraw(Device,3,0);
        CHECK(ErrorCalls==Errors+1 && !Calls[SubmitGpu] && !F.Device.ProgramCount &&
            F.Device.ResourceCount==1 && LiveBos()==1 && !CompilerLive && !MockError);
        DestroyDraw(&F);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !MockError);
    }
    /* Both the new RX code BO (third) and its immutable descriptor BO (fourth)
       must clean their CPU/GPU ownership after each fallible upload stage. */
    const UINT Ops[]={CreateAllocation,MapVa,LockBo,CacheBo,UnlockBo,ResidentBo};
    for(UINT Op=0;Op<sizeof(Ops)/sizeof(Ops[0]);Op++)for(UINT At=3;At<=4;At++) {
        int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
        FailOp=Ops[Op];FailAt=Calls[FailOp]+At;
        UINT Errors=ErrorCalls;Functions.pfnDraw(Device,3,0);
        CHECK(Calls[FailOp]>=FailAt && ErrorCalls==Errors+1 && !Calls[SubmitGpu] &&
            !F.Device.ProgramCount && LiveBos()==1 && !CompilerLive && !MockError);
        FailOp=0;DestroyDraw(&F);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !MockError);
    }
    /* A failed teardown of the optional last BO retains its compiler output
       and program owner until a subsequent cleanup succeeds. */
    int R=InitDraw(&F);if(R)return R;D3D10DDI_HDEVICE Device={&F.Device};
    struct t70_shader_fb_preload_request Request={T70_SHADER_ABI,T70_SHADER_GPU_ID,T70_SHADER_FB_RGBA8_UNORM,1,1,0};
    struct t70_shader Preload={0};struct t70_shader_io_types Types={0};MALI_UMD_PROGRAM Program={0};
    EnterCriticalSection(&F.Device.Gate);
    CHECK(t70_shader_compile_framebuffer_preload(&Request,&Preload,0,0)==T70_SHADER_OK);
    CHECK(UmdUploadSelected(&F.Device,&Types,&Preload,&Program)==S_OK && !Preload.binary);
    LeaveCriticalSection(&F.Device.Gate);
    CHECK(Program.Id && F.Device.ProgramCount==1 && CompilerLive==3 && LiveBos()==5);
    FailOp=FreeVa;FailAt=Calls[FreeVa]+4;
    CHECK(FAILED(MtkMaliUmdReleaseProgram(Device,&Program)) && !Program.Id && F.Device.ProgramCount==1 &&
        F.Device.Programs->Bo[3].Va && CompilerLive==3 && LiveBos()==2 && !MockError);
    FailOp=0;Functions.pfnFlush(Device);
    CHECK(!F.Device.ProgramCount && !CompilerLive && LiveBos()==1 && !MockError);
    DestroyDraw(&F);CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !MockError);
    return 0;
}
SIZE_T MtkMaliUmdCConsumer(void);
SIZE_T MtkMaliUmdCppConsumer(void);
int TestMain(void) {
    CHECK(MtkMaliUmdCConsumer()==sizeof(UMD_DEVICE)+0xffffu);
    CHECK(MtkMaliUmdCppConsumer()==sizeof(UMD_DEVICE)+0xffffu);
    ZeroMemory(&Functions,sizeof(Functions));
    MtkMaliUmdResourceFunctions(&Functions);MtkMaliUmdShaderFunctions(&Functions);
    CHECK(Functions.pfnDraw && Functions.pfnFlush && Functions.pfnIaSetTopology &&
        Functions.pfnSetRenderTargets && Functions.pfnSetViewports &&
        MtkMaliUmdDeviceBytes()==sizeof(UMD_DEVICE));
    MtkMaliUmdResourceFunctions(0);MtkMaliUmdShaderFunctions(0);
    UMD_DEVICE D;D3D10DDI_HDEVICE Device={&D};
    for(UINT Op=OpenAdapter;Op<=CreateQueue;Op++) {
        Reset();FailOp=Op;FailAt=1;CHECK(FAILED(Init(&D)));CHECK(MtkMaliUmdClose(Device)==S_OK);
        CHECK(!D.Signature && !MockError && GateDeleted==1);
    }
    for(UINT Op=DestroyContext;Op<=CloseAdapter;Op++) {
        Reset();CHECK(Init(&D)==S_OK);FailOp=Op;FailAt=1;
        CHECK(FAILED(MtkMaliUmdClose(Device)) && D.Signature && !GateDeleted);
        CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError && GateDeleted==1);
    }
    const DXGI_FORMAT Formats[3]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM};
    const UINT Widths[3]={1,17,16384},Heights[3]={1,19,1};
    for(UINT F=0;F<3;F++)for(UINT Size=0;Size<3;Size++) {
        Reset();CHECK(Init(&D)==S_OK);UMD_TOKEN R={0},V={0};Create(&D,&R,&V,Formats[F],Widths[Size],Heights[Size]);
        CHECK(R.Id && V.Id && D.ResourceCount==1 && D.ViewCount==1 && LiveBos()==1 && !MockError);
        CHECK(D.Resources->Desc.Pitch==((Widths[Size]*4+63)&~63u) && D.Resources->Desc.Size==((uint64_t)D.Resources->Desc.Pitch*Heights[Size]+4095u)/4096u*4096u);
        Clear(&D,&V);CHECK(D.PendingCount==1 && Calls[SubmitGpu]==1 && !ErrorCalls && LiveBos()==4 && !MockError);
        Destroy(&D,&R,&V);R.Id=V.Id=UINT64_MAX;CHECK(LiveBos()==4 && D.ResourceCount==1 && !D.ViewCount);
        Functions.pfnFlush(Device);CHECK(D.PendingCount==1 && LiveBos()==4 && !ErrorCalls);
        CHECK(MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY) && ContextLive && !GateDeleted);
        Complete();Functions.pfnFlush(Device);CHECK(!D.PendingCount && !D.Resources && !LiveBos() && !LiveHeap() && !LiveEvents());
        CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    Reset();CHECK(Init(&D)==S_OK);UMD_TOKEN R={0},V={0};Create(&D,&R,&V,Formats[0],17,19);
    Clear(&D,&V);Clear(&D,&V);CHECK(D.PendingCount==2 && Calls[ResidentBo]==7 && LiveBos()==7 && !MockError);
    Destroy(&D,&R,&V);Complete();Functions.pfnFlush(Device);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Invalid resource requests cannot allocate, and errors run outside Gate. */
    Reset();CHECK(Init(&D)==S_OK);
    for(UINT Mutation=0;Mutation<23;Mutation++) {
        D3D10DDI_MIPINFO M;D3D10DDIARG_CREATERESOURCE A=ResourceArgs(&M,Formats[0],17,19);
        switch(Mutation) {
            case 0:A.ResourceDimension=D3D10DDIRESOURCE_BUFFER;break;
            case 1:A.Usage=D3D10_DDI_USAGE_STAGING;break;
            case 2:A.BindFlags|=D3D10_DDI_BIND_DEPTH_STENCIL;break;
            case 3:A.MapFlags=1;break;case 4:A.MiscFlags=1;break;case 5:A.MapFlags=~0u;break;
            case 6:A.pPrimaryDesc=(void *)1;break;case 7:A.MipLevels=2;break;case 8:A.ArraySize=2;break;
            case 9:A.SampleDesc.Count=4;break;case 10:A.SampleDesc.Quality=1;break;case 11:A.pMipInfoList=0;break;
            case 12:M.TexelWidth=0;break;case 13:M.TexelHeight=0;break;case 14:M.TexelWidth=16385;break;
            case 15:M.TexelHeight=16385;break;case 16:M.TexelDepth=2;break;case 17:M.PhysicalWidth++;break;
            case 18:M.PhysicalHeight++;break;case 19:M.PhysicalDepth++;break;
            case 20:A.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;break;
            case 21:A.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;break;
            case 22:M.TexelWidth=M.PhysicalWidth=M.TexelHeight=M.PhysicalHeight=16384;break;
        }
        R.Id=123;D3D10DDI_HRESOURCE Resource={&R};D3D10DDI_HRTRESOURCE Runtime={0};
        UINT Before=ErrorCalls;ReenterError=1;
        CHECK(!Functions.pfnCalcPrivateResourceSize(Device,&A));
        Functions.pfnCreateResource(Device,&A,Resource,Runtime);
        CHECK(!R.Id && !Calls[CreateAllocation] && ErrorCalls>=Before+2 && !MockError);
    }
    CHECK(MtkMaliUmdClose(Device)==S_OK);
    /* View shape, stale/cross-device tokens and create limits. */
    Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
    UMD_TOKEN Other=R;Other.Owner=(void *)1;
    D3D10DDIARG_CREATERENDERTARGETVIEW A={0};A.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;A.Tex2D.ArraySize=1;A.Format=Formats[0];
    for(UINT Mutation=0;Mutation<6;Mutation++) {
        D3D10DDIARG_CREATERENDERTARGETVIEW B=A;B.hDrvResource.pDrvPrivate=&R;
        if(Mutation==0)B.hDrvResource.pDrvPrivate=&Other;
        if(Mutation==1)B.Format=Formats[1];
        if(Mutation==2)B.ResourceDimension=D3D10DDIRESOURCE_TEXTURE1D;
        if(Mutation==3)B.Tex2D.MipSlice=1;
        if(Mutation==4)B.Tex2D.FirstArraySlice=1;
        if(Mutation==5)B.Tex2D.ArraySize=2;
        UMD_TOKEN Out={0};D3D10DDI_HRENDERTARGETVIEW View={&Out};D3D10DDI_HRTRENDERTARGETVIEW Runtime={0};
        CHECK(!Functions.pfnCalcPrivateRenderTargetViewSize(Device,&B));
        Functions.pfnCreateRenderTargetView(Device,&B,View,Runtime);CHECK(!Out.Id && D.ViewCount==1 && !MockError);
    }
    Destroy(&D,&R,&V);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Each fallible clear operation is reached, fails, and cleans only safely. */
    const UINT Ops[]={HeapNew,NewEvent,CreateAllocation,MapVa,LockBo,CacheBo,UnlockBo,ResidentBo,WaitGpu,SubmitGpu,SignalGpu};
    const UINT Counts[]={1,1,3,3,2,2,2,4,1,1,1};
    for(UINT I=0;I<sizeof(Ops)/sizeof(Ops[0]);I++)for(UINT N=1;N<=Counts[I];N++) {
        Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
        FailOp=Ops[I];FailAt=Calls[FailOp]+N;Clear(&D,&V);
        CHECK(D.Error==DXGI_ERROR_DEVICE_REMOVED && FAILED(D.LastFailure) && ErrorCode==DXGI_ERROR_DEVICE_REMOVED && !MockError);
        if(Ops[I]==SignalGpu)CHECK(D.PendingCount==1 && LiveBos()==4 && ContextLive);
        else CHECK(!D.PendingCount);
        Destroy(&D,&R,&V);Complete();Functions.pfnFlush(Device);
        CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveHeap() && !LiveEvents() && !MockError);
    }
    /* Failed completion signaling is not proof that submitted work disappeared. */
    Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
    FailOp=SignalGpu;FailAt=1;Sticky=1;Clear(&D,&V);Destroy(&D,&R,&V);
    CHECK(D.PendingCount==1 && LiveBos()==4 && LiveEvents()==1 && LastSubmitted && !Calls[FreeVa]);
    CHECK(FAILED(MtkMaliUmdClose(Device)) && ContextLive && LiveBos()==4);
    FailOp=0;Functions.pfnFlush(Device);CHECK(D.PendingCount==1 && !LastSubmitted && !Calls[FreeVa]);
    Complete();Functions.pfnFlush(Device);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Both asynchronous map and make-resident paging fences precede GPU work. */
    Reset();CHECK(Init(&D)==S_OK);AsyncPaging=1;PagingValue=0;ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));
    Create(&D,&R,&V,Formats[1],17,19);Clear(&D,&V);Destroy(&D,&R,&V);
    CHECK(D.PendingCount==1 && WaitedFence==PagingNext && LiveBos()==4 && !Calls[FreeVa]);
    CHECK(MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY));
    Complete();Functions.pfnFlush(Device);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    Reset();CHECK(Init(&D)==S_OK);AsyncPaging=1;PagingValue=0;ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));
    Create(&D,&R,&V,Formats[1],17,19);Destroy(&D,&R,&V);
    CHECK(D.ResourceCount==1 && LiveBos()==1 && !Calls[FreeVa] && MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY));
    PagingValue=PagingNext;CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Disposal is restartable at every OS stage, including event closure. */
    const UINT CleanupOps[]={FreeVa,DestroyAllocation,EventClose,WaitEvent};
    for(UINT I=0;I<sizeof(CleanupOps)/sizeof(CleanupOps[0]);I++) {
        Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[2],17,19);
        Clear(&D,&V);Destroy(&D,&R,&V);Complete();FailOp=CleanupOps[I];FailAt=Calls[FailOp]+1;Sticky=1;
        Functions.pfnFlush(Device);CHECK(D.PendingCount==1 && ContextLive && FAILED(MtkMaliUmdClose(Device)));
        CHECK(!MockError);FailOp=0;Functions.pfnFlush(Device);CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    }
    /* Failed resource creation and view allocation cannot publish a token. */
    const UINT ResourceOps[]={HeapNew,CreateAllocation,MapVa};
    for(UINT I=0;I<sizeof(ResourceOps)/sizeof(ResourceOps[0]);I++) {
        Reset();CHECK(Init(&D)==S_OK);FailOp=ResourceOps[I];FailAt=1;
        ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
        CHECK(!R.Id && !V.Id && ErrorCalls && !D.Resources && !LiveBos() && !LiveHeap() && !MockError);
        CHECK(MtkMaliUmdClose(Device)==S_OK);
    }
    Reset();CHECK(Init(&D)==S_OK);FailOp=HeapNew;FailAt=2;
    ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
    CHECK(R.Id && !V.Id && D.ResourceCount==1 && !D.ViewCount && LiveBos()==1 && ErrorCalls && !MockError);
    Destroy(&D,&R,&V);CHECK(MtkMaliUmdClose(Device)==S_OK);
    /* The OS can commit residency for a prefix even when MakeResident fails. */
    Reset();CHECK(Init(&D)==S_OK);AsyncPaging=1;PagingValue=0;PartialResidentFailure=1;
    ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
    FailOp=ResidentBo;FailAt=1;Clear(&D,&V);Destroy(&D,&R,&V);
    CHECK(D.PendingCount==1 && LiveBos()==4 && !Calls[SubmitGpu] && !Calls[FreeVa] && D.Resources->Bo.Resident);
    CHECK(FAILED(MtkMaliUmdFailureReason(Device)) && MtkMaliUmdClose(Device)==HRESULT_FROM_WIN32(ERROR_BUSY));
    FailOp=0;PagingValue=PagingNext;CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* A failed create with pending mapping also retains its allocation owner. */
    Reset();CHECK(Init(&D)==S_OK);AsyncPaging=1;PagingValue=0;FailOp=LockBo;FailAt=1;
    ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);Clear(&D,&V);Destroy(&D,&R,&V);
    CHECK(D.PendingCount==1 && !Calls[SubmitGpu] && !Calls[FreeVa] && LiveBos()==4);
    FailOp=0;PagingValue=PagingNext;CHECK(MtkMaliUmdClose(Device)==S_OK && !MockError);
    /* Shared monotonic IDs never wrap into a still-live object. */
    Reset();CHECK(Init(&D)==S_OK);D.NextId=UINT64_MAX;
    ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
    CHECK(!R.Id && !V.Id && !Calls[CreateAllocation] && ErrorCode==E_OUTOFMEMORY && !MockError);
    CHECK(MtkMaliUmdClose(Device)==S_OK);
    /* Bounded queue pressure never overwrites an in-flight command or target. */
    Reset();CHECK(Init(&D)==S_OK);ZeroMemory(&R,sizeof(R));ZeroMemory(&V,sizeof(V));Create(&D,&R,&V,Formats[0],17,19);
    for(UINT I=0;I<UMD_PENDING_LIMIT;I++)Clear(&D,&V);
    CHECK(D.PendingCount==UMD_PENDING_LIMIT && LiveBos()==1+UMD_PENDING_LIMIT*3 && !ErrorCalls && !MockError);
    Clear(&D,&V);CHECK(Calls[SubmitGpu]==UMD_PENDING_LIMIT && D.LastFailure==E_OUTOFMEMORY && ErrorCode==DXGI_ERROR_DEVICE_REMOVED);
    Destroy(&D,&R,&V);Complete();Functions.pfnFlush(Device);
    CHECK(MtkMaliUmdClose(Device)==S_OK && !LiveBos() && !LiveEvents() && !LiveHeap() && !MockError);
    CHECK(GateDeleted==1 && !GateDepth);int Result=TestTransfers();if(Result)return Result;
    Result=TestShaders();if(Result)return Result;Result=TestPrograms();if(Result)return Result;
    Result=TestRaster();if(Result)return Result;Result=TestD3dDraw();if(Result)return Result;
    Result=TestDrawExistingContents();if(Result)return Result;Result=TestDrawProgramCache();if(Result)return Result;Result=TestDrawPreloadOtherFormats();if(Result)return Result;
    Result=TestDrawPreloadFailures();return Result?Result:MockError;
}
