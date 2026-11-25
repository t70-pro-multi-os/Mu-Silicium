/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Umd.h"
#include "Commands.h"
#include "AllocationAbi.h"
#include <limits.h>
#define UMD_SIGNATURE 0x55303754u
#define UMD_LIMIT 4096u
#define UMD_PENDING_LIMIT 32u
#define UMD_PROGRAM_LIMIT 32u
#define UMD_TARGET_LIMIT (512u*1024u*1024u-MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES-MALI_COMMAND_CLEAR_HEAP_BYTES)
#define UMD_NT_SUCCESS(S) ((NTSTATUS)(S)>=0)
typedef struct {
    uint64_t Id;
    const void *Owner;
} UMD_TOKEN;
typedef struct {
    D3DKMT_HANDLE Resource,Allocation;
    uint64_t Va,Bytes,PagingFence;
    void *Cpu;
    UINT Locked,Resident;
} UMD_BO;
typedef struct UMD_OLD_BO {
    struct UMD_OLD_BO *Next;
    UMD_BO Bo;
} UMD_OLD_BO;
typedef struct UMD_RESOURCE {
    struct UMD_RESOURCE *Next;
    uint64_t Id;
    UINT Live,Views,Pending,Initializing;
    UINT Usage,BindFlags,MapFlags,LogicalBytes,MapType,OldCount;
    UMD_OLD_BO *OldBos;
    DXGI_FORMAT Format;
    MALI_ALLOCATION_DESC Desc;
    UMD_BO Bo;
} UMD_RESOURCE;
typedef struct UMD_VIEW {
    struct UMD_VIEW *Next;
    uint64_t Id;
    UMD_RESOURCE *Resource;
} UMD_VIEW;
typedef struct UMD_STATE {
    uint32_t Signature,Kind;
    void *Owner;
    union {
        D3D10_DDI_RASTERIZER_DESC Rasterizer;
        D3D10_DDI_BLEND_DESC Blend;
        D3D10_DDI_DEPTH_STENCIL_DESC DepthStencil;
    } Desc;
} UMD_STATE;
typedef struct UMD_SHADER {
    struct UMD_SHADER *Next;
    uint64_t Id;
    struct t70_shader_token_request Source;
} UMD_SHADER;
typedef struct UMD_PROGRAM {
    struct UMD_PROGRAM *Next;
    uint64_t Id;
    UINT Live,NeedsIdle,Pending;
    struct t70_shader_pipeline Compiled;
    struct t70_shader Preload; /* optional internal framebuffer-load fragment program */
    UMD_BO Bo[4]; /* VS, PS, immutable program descriptors, optional preload RX code */
} UMD_PROGRAM;
typedef struct UMD_PENDING {
    struct UMD_PENDING *Next;
    UMD_PROGRAM *Program;
    UMD_BO Bo[3]; /* command, descriptors, private tiler heap */
    HANDLE Event;
    UINT Submitted,SignalQueued,ResourceCount;
    unsigned char Private[MALI_COMMAND_MAX_PRIVATE];
    UMD_RESOURCE *Resources[];
} UMD_PENDING;
typedef struct {
    UINT Signature,Ready,ResourceCount,ViewCount,PendingCount,Slot,ShaderCount,ProgramCount;
    uint64_t NextId;
    CRITICAL_SECTION Gate;
    MALI_CAPS Caps;
    D3D10DDI_HRTCORELAYER Core;
    PFND3D10DDI_SETERROR_CB SetError;
    HRESULT Error,LastFailure;
    D3DKMT_HANDLE Adapter,Device,Context,PagingQueue,PagingSync;
    volatile UINT64 *PagingFence;
    UMD_RESOURCE *Resources;
    UMD_VIEW *Views;
    UMD_PENDING *Pending;
    UMD_SHADER *Shaders,*VertexShader,*PixelShader;
    UMD_PROGRAM *Programs;
    MALI_UMD_PROGRAM DrawProgram;
    uint64_t DrawVertex,DrawPixel;
    DXGI_FORMAT DrawFormat;
    UMD_VIEW *RenderTarget;
    UINT Topology,TopologySet,VertexBufferMask,IndexBufferBound,InputLayoutBound;
    UINT DrawInvalidMask;
    UINT ViewportSet,ScissorSet,BlendValid,DepthValid,RasterizerValid,SampleMask;
    D3D10_DDI_VIEWPORT Viewport;
    D3D10_DDI_RECT Scissor;
    D3D10_DDI_RASTERIZER_DESC Rasterizer;
    D3D10_DDI_BLEND_DESC Blend;
    D3D10_DDI_DEPTH_STENCIL_DESC DepthStencil;
} UMD_DEVICE;
#define UMD_STATE_SIGNATURE 0x53553754u
#define UMD_STATE_RASTERIZER 1u
#define UMD_STATE_BLEND 2u
#define UMD_STATE_DEPTH 3u
#define UMD_DRAW_INVALID_TOPOLOGY (1u<<0)
#define UMD_DRAW_INVALID_INPUT (1u<<1)
#define UMD_DRAW_INVALID_TARGET (1u<<2)
#define UMD_DRAW_INVALID_VIEWPORT (1u<<3)
#define UMD_DRAW_INVALID_SCISSOR (1u<<4)
#define UMD_DRAW_INVALID_RASTERIZER (1u<<5)
#define UMD_DRAW_INVALID_BLEND (1u<<6)
#define UMD_DRAW_INVALID_DEPTH (1u<<7)
#define UMD_DRAW_INVALID_VERTEX_SHADER (1u<<8)
#define UMD_DRAW_INVALID_PIXEL_SHADER (1u<<9)
#define UMD_DRAW_DESCRIPTOR_OFFSET MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES
#define UMD_DRAW_HEAP_OFFSET (UMD_DRAW_DESCRIPTOR_OFFSET+MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES)
#define UMD_DRAW_TLS_OFFSET (UMD_DRAW_HEAP_OFFSET+MALI_COMMAND_CLEAR_HEAP_BYTES)
#define UMD_DRAW_VERTEX_FAU_OFFSET (UMD_DRAW_TLS_OFFSET+64u)
#define UMD_DRAW_PIXEL_FAU_OFFSET (UMD_DRAW_VERTEX_FAU_OFFSET+2048u)
#define UMD_DRAW_PRELOAD_FAU_OFFSET (UMD_DRAW_PIXEL_FAU_OFFSET+2048u)
#define UMD_DRAW_SCRATCH_BYTES ((UMD_DRAW_PRELOAD_FAU_OFFSET+2048u+4095u)&~4095u)
_Static_assert(sizeof(MALI_ALLOCATION_DESC)==40,"shared allocation ABI");
_Static_assert(sizeof(MALI_UMD_PROGRAM)==104,"uploaded program view ABI");
_Static_assert(sizeof(struct t70_shader_fb_preload_sysvals)==8,"preload inclusive bbox ABI");
_Static_assert(sizeof(UMD_TOKEN)==16,"runtime private resource/view/shader storage");
_Static_assert(D3D10_SB_OPCODE_DCL_INPUT==0x5f && D3D10_SB_OPCODE_DCL_OUTPUT_SIV==0x67 &&
    D3D10_SB_OPCODE_CUSTOMDATA==0x35,"DDI declaration opcodes used by CPU compiler");
_Static_assert(D3D10_SB_OPERAND_TYPE_INPUT==1 && D3D10_SB_OPERAND_TYPE_OUTPUT==2 &&
    D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH==12 && D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK==15,
    "DDI declaration register types used by CPU compiler");
static UMD_DEVICE *UmdDevice(D3D10DDI_HDEVICE H) {
    UMD_DEVICE *D=H.pDrvPrivate;
    return D && D->Signature==UMD_SIGNATURE ? D:0;
}
static void *UmdNew(SIZE_T N) {return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,N);}
static void UmdFree(void *P) {if(P)HeapFree(GetProcessHeap(),0,P);}
static HRESULT UmdStatus(NTSTATUS S) {
    return UMD_NT_SUCCESS(S)?S_OK:(HRESULT)((ULONG)S|FACILITY_NT_BIT);
}
static void UmdError(UMD_DEVICE *D,HRESULT H) {
    if(FAILED(H) && D && D->SetError)D->SetError(D->Core,H);
}
static HRESULT UmdActive(UMD_DEVICE *D) {
    return !D->Ready?DXGI_ERROR_DEVICE_REMOVED:D->Error;
}
static void UmdFatal(UMD_DEVICE *D,HRESULT H) {
    if(SUCCEEDED(D->LastFailure))D->LastFailure=H;
    D->Error=DXGI_ERROR_DEVICE_REMOVED;
}
static HRESULT UmdInvalidate(UMD_DEVICE *D,UMD_BO *B) {
    D3DKMT_INVALIDATECACHE A={0};A.hDevice=D->Device;A.hAllocation=B->Allocation;A.Length=B->Bytes;
    return UmdStatus(D3DKMTInvalidateCache(&A));
}
static void UmdFence(UMD_BO *B,UINT64 Fence) {if(Fence>B->PagingFence)B->PagingFence=Fence;}
static int UmdPagingDone(UMD_DEVICE *D,UMD_BO *B) {
    if(!B->PagingFence)return 1;
    /* OS-owned read-only, naturally aligned fence; never interlocked-write it. */
    UINT64 V=*D->PagingFence;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return V>=B->PagingFence;
}
static HRESULT UmdUnlock(UMD_DEVICE *D,UMD_BO *B) {
    if(!B->Locked)return S_OK;
    D3DKMT_UNLOCK2 A={0};A.hDevice=D->Device;A.hAllocation=B->Allocation;
    NTSTATUS S=D3DKMTUnlock2(&A);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    B->Locked=0;B->Cpu=0;return S_OK;
}
static HRESULT UmdDisposeBo(UMD_DEVICE *D,UMD_BO *B) {
    HRESULT H=UmdUnlock(D,B);if(FAILED(H))return H;
    if(!UmdPagingDone(D,B))return HRESULT_FROM_WIN32(ERROR_BUSY);
    if(B->Va) {
        D3DKMT_FREEGPUVIRTUALADDRESS A={0};A.hAdapter=D->Adapter;A.BaseAddress=B->Va;A.Size=B->Bytes;
        NTSTATUS S=D3DKMTFreeGpuVirtualAddress(&A);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
        B->Va=0;
    }
    if(B->Resource || B->Allocation) {
        D3DKMT_DESTROYALLOCATION2 A={0};A.hDevice=D->Device;A.hResource=B->Resource;
        if(!B->Resource){A.AllocationCount=1;A.phAllocationList=&B->Allocation;}
        NTSTATUS S=D3DKMTDestroyAllocation2(&A);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
        B->Resource=B->Allocation=B->Resident=0;
    }
    return S_OK;
}
static HRESULT UmdCreateBo(UMD_DEVICE *D,UMD_BO *B,const MALI_ALLOCATION_DESC *Desc,HANDLE Trace,UINT Access) {
    D3DDDI_ALLOCATIONINFO2 Info={0};Info.pPrivateDriverData=(void *)Desc;
    Info.PrivateDriverDataSize=sizeof(*Desc);
    D3DKMT_CREATEALLOCATION A={0};A.hDevice=D->Device;A.NumAllocations=1;A.pAllocationInfo2=&Info;
    A.Flags.CreateResource=1;A.Flags.CreateShared=1;A.Flags.NtSecuritySharing=1;
    A.hPrivateRuntimeResourceHandle=Trace;
    NTSTATUS S=D3DKMTCreateAllocation2(&A);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    B->Resource=A.hResource;B->Allocation=Info.hAllocation;B->Bytes=Desc->Size;
    if(!B->Resource || !B->Allocation || A.hGlobalShare)return E_UNEXPECTED;
    D3DDDI_MAPGPUVIRTUALADDRESS M={0};M.hPagingQueue=D->PagingQueue;M.hAllocation=B->Allocation;
    M.MinimumAddress=4096;M.MaximumAddress=MALI_JM_VA_LIMIT;M.SizeInPages=B->Bytes/4096u;
    M.Protection.Write=!!(Access&MaliPteWrite);M.Protection.Execute=!!(Access&MaliPteExecute); /* Read is implicit. */
    S=D3DKMTMapGpuVirtualAddress(&M);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    B->Va=M.VirtualAddress;UmdFence(B,M.PagingFenceValue);
    if(!B->Va || (B->Va&4095u) || B->Va>=MALI_JM_VA_LIMIT ||
       B->Bytes>MALI_JM_VA_LIMIT-B->Va)return E_UNEXPECTED;
    return S_OK;
}
static HRESULT UmdLock(UMD_DEVICE *D,UMD_BO *B) {
    D3DKMT_LOCK2 A={0};A.hDevice=D->Device;A.hAllocation=B->Allocation;
    NTSTATUS S=D3DKMTLock2(&A);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    B->Locked=1;B->Cpu=A.pData;return B->Cpu?S_OK:E_UNEXPECTED;
}
static HRESULT UmdResident(UMD_DEVICE *D,UMD_BO *B) {
    if(B->Resident)return S_OK;
    D3DDDI_MAKERESIDENT A={0};A.hPagingQueue=D->PagingQueue;A.NumAllocations=1;
    A.AllocationList=&B->Allocation;
    NTSTATUS S=D3DKMTMakeResident(&A);
    /* NumAllocations is an in/out committed prefix, including on budget errors. */
    if(A.NumAllocations==1) {B->Resident=1;UmdFence(B,A.PagingFenceValue);}
    if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    return A.NumAllocations==1?S_OK:E_UNEXPECTED;
}
static HRESULT UmdWaitPaging(UMD_DEVICE *D,UINT64 Fence) {
    if(!Fence)return S_OK;
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU A={0};A.hContext=D->Context;
    A.ObjectCount=1;A.ObjectHandleArray=&D->PagingSync;A.MonitoredFenceValueArray=&Fence;
    return UmdStatus(D3DKMTWaitForSynchronizationObjectFromGpu(&A));
}
static HRESULT UmdSignal(UMD_DEVICE *D,UMD_PENDING *P) {
    if(!P->Submitted || P->SignalQueued)return S_OK;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 A={0};A.hContext=D->Context;
    A.Flags.EnqueueCpuEvent=1;A.CpuEventHandle=P->Event;
    /* SignalAtSubmission stays FALSE: event follows actual context processing. */
    NTSTATUS S=D3DKMTSignalSynchronizationObject2(&A);
    if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    P->SignalQueued=1;return S_OK;
}
/* Gate held. Failed submission fences, paging and OS disposal keep the entire
   owner record for retry. No arbitrary timeout can prove GPU work is finished. */
static HRESULT UmdReap(UMD_DEVICE *D) {
    HRESULT Result=S_OK;
    UMD_PENDING **Link=&D->Pending;
    while(*Link) {
        UMD_PENDING *P=*Link;HRESULT H=UmdSignal(D,P);
        if(FAILED(H)){Result=H;Link=&P->Next;continue;}
        if(P->Submitted) {
            DWORD W=WaitForSingleObject(P->Event,0);
            if(W!=WAIT_OBJECT_0) {
                Result=W==WAIT_TIMEOUT?HRESULT_FROM_WIN32(ERROR_BUSY):E_FAIL;
                Link=&P->Next;continue;
            }
            P->Submitted=0;
        }
        for(UINT I=0;I<3;I++) {
            H=UmdDisposeBo(D,&P->Bo[I]);if(FAILED(H)){Result=H;break;}
        }
        if(FAILED(H)){Link=&P->Next;continue;}
        if(P->Event && !CloseHandle(P->Event)){Result=E_FAIL;Link=&P->Next;continue;}
        for(UINT I=0;I<P->ResourceCount;I++)--P->Resources[I]->Pending;
        if(P->Program)--P->Program->Pending;
        *Link=P->Next;--D->PendingCount;UmdFree(P);
    }
    UMD_RESOURCE **RLink=&D->Resources;
    while(*RLink) {
        UMD_RESOURCE *R=*RLink;
        if(!R->Pending) {
            UMD_OLD_BO **L=&R->OldBos;
            while(*L) {
                UMD_OLD_BO *Old=*L;HRESULT H=UmdDisposeBo(D,&Old->Bo);
                if(FAILED(H)){Result=H;L=&Old->Next;continue;}
                *L=Old->Next;--R->OldCount;UmdFree(Old);
            }
        }
        if(R->Live || R->Views || R->Pending || R->Initializing || R->OldBos){RLink=&R->Next;continue;}
        HRESULT H=UmdDisposeBo(D,&R->Bo);
        if(FAILED(H)){Result=H;RLink=&R->Next;continue;}
        *RLink=R->Next;--D->ResourceCount;UmdFree(R);
    }
    UMD_PROGRAM **ProgramLink=&D->Programs;
    while(*ProgramLink) {
        UMD_PROGRAM *P=*ProgramLink;
        if(P->Live || P->Pending){ProgramLink=&P->Next;continue;}
        if(P->NeedsIdle) {
            D3DKMT_WAITFORIDLE A={0};A.hDevice=D->Device;
            NTSTATUS S=D3DKMTWaitForIdle(&A);
            /* Pending/informational success cannot prove execution stopped. */
            if(S!=0){Result=UMD_NT_SUCCESS(S)?E_UNEXPECTED:UmdStatus(S);ProgramLink=&P->Next;continue;}
            P->NeedsIdle=0;
        }
        HRESULT H=S_OK;
        for(UINT I=0;I<4;I++){H=UmdDisposeBo(D,&P->Bo[I]);if(FAILED(H))break;}
        if(FAILED(H)){Result=H;ProgramLink=&P->Next;continue;}
        t70_shader_free(&P->Preload);
        t70_shader_free_pipeline(&P->Compiled);
        *ProgramLink=P->Next;--D->ProgramCount;UmdFree(P);
    }
    return Result;
}
SIZE_T MtkMaliUmdDeviceBytes(void) {return sizeof(UMD_DEVICE);}
HRESULT MtkMaliUmdFailureReason(D3D10DDI_HDEVICE Handle) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return E_INVALIDARG;
    EnterCriticalSection(&D->Gate);HRESULT H=D->LastFailure;LeaveCriticalSection(&D->Gate);return H;
}
HRESULT MtkMaliUmdInitialize(void *Storage,LUID Luid,const MALI_CAPS *Caps,UINT Slot,
    D3D10DDI_HRTCORELAYER Core,PFND3D10DDI_SETERROR_CB SetError) {
    UMD_DEVICE *D=Storage;
    if(!D || D->Signature || !Caps || !SetError || (!Luid.LowPart && !Luid.HighPart) ||
       (Caps->GpuId>>16)!=0x9093 || !Caps->Tilers || Slot>=MALI_JM_MAX_SLOTS ||
       !(Caps->JobSlots&(1u<<Slot)) || !(Caps->JobFeatures[Slot]&(1u<<9)))return E_INVALIDARG;
    ZeroMemory(D,sizeof(*D));
    if(!InitializeCriticalSectionEx(&D->Gate,0,0))return E_OUTOFMEMORY;
    D->Signature=UMD_SIGNATURE;D->Caps=*Caps;D->Slot=Slot;D->Core=Core;D->SetError=SetError;
    D->SampleMask=UINT_MAX;
    D3DKMT_OPENADAPTERFROMLUID A={0};A.AdapterLuid=Luid;
    NTSTATUS S=D3DKMTOpenAdapterFromLuid(&A);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);D->Adapter=A.hAdapter;
    if(!D->Adapter)return E_UNEXPECTED;
    D3DKMT_CREATEDEVICE C={0};C.hAdapter=D->Adapter;
    S=D3DKMTCreateDevice(&C);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);D->Device=C.hDevice;
    if(!D->Device)return E_UNEXPECTED;
    D3DKMT_CREATECONTEXTVIRTUAL V={0};V.hDevice=D->Device;V.EngineAffinity=1;V.ClientHint=D3DKMT_CLIENTHINT_DX10;
    S=D3DKMTCreateContextVirtual(&V);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);D->Context=V.hContext;
    if(!D->Context)return E_UNEXPECTED;
    D3DKMT_CREATEPAGINGQUEUE Q={0};Q.hDevice=D->Device;Q.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    S=D3DKMTCreatePagingQueue(&Q);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    D->PagingQueue=Q.hPagingQueue;D->PagingSync=Q.hSyncObject;D->PagingFence=Q.FenceValueCPUVirtualAddress;
    if(!D->Adapter || !D->Device || !D->Context || !D->PagingQueue || !D->PagingSync ||
       !D->PagingFence || ((uintptr_t)D->PagingFence&7u))return E_UNEXPECTED;
    D->Ready=1;return S_OK;
}
HRESULT MtkMaliUmdClose(D3D10DDI_HDEVICE Handle) {
    UMD_DEVICE *D=Handle.pDrvPrivate;
    if(!D)return E_INVALIDARG;
    if(!D->Signature)return S_OK;
    if(D->Signature!=UMD_SIGNATURE)return E_INVALIDARG;
    EnterCriticalSection(&D->Gate);
    if(D->Views || D->Shaders){LeaveCriticalSection(&D->Gate);return HRESULT_FROM_WIN32(ERROR_BUSY);}
    for(UMD_PROGRAM *P=D->Programs;P;P=P->Next)if(P->Live) {
        LeaveCriticalSection(&D->Gate);return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    for(UMD_RESOURCE *R=D->Resources;R;R=R->Next)if(R->Live) {
        LeaveCriticalSection(&D->Gate);return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    D->Ready=0;
    HRESULT H=UmdReap(D);
    if(D->Pending || D->Resources || D->Programs){LeaveCriticalSection(&D->Gate);return FAILED(H)?H:HRESULT_FROM_WIN32(ERROR_BUSY);}
    if(D->Context) {
        D3DKMT_DESTROYCONTEXT A={0};A.hContext=D->Context;H=UmdStatus(D3DKMTDestroyContext(&A));
        if(FAILED(H))goto Done;D->Context=0;
    }
    if(D->PagingQueue) {
        D3DDDI_DESTROYPAGINGQUEUE A={0};A.hPagingQueue=D->PagingQueue;H=UmdStatus(D3DKMTDestroyPagingQueue(&A));
        if(FAILED(H))goto Done;D->PagingQueue=D->PagingSync=0;D->PagingFence=0;
    }
    if(D->Device) {
        D3DKMT_DESTROYDEVICE A={0};A.hDevice=D->Device;H=UmdStatus(D3DKMTDestroyDevice(&A));
        if(FAILED(H))goto Done;D->Device=0;
    }
    if(D->Adapter) {
        D3DKMT_CLOSEADAPTER A={0};A.hAdapter=D->Adapter;H=UmdStatus(D3DKMTCloseAdapter(&A));
        if(FAILED(H))goto Done;D->Adapter=0;
    }
    H=S_OK;
Done:
    LeaveCriticalSection(&D->Gate);
    if(SUCCEEDED(H)){DeleteCriticalSection(&D->Gate);ZeroMemory(D,sizeof(*D));}
    return H;
}
static HRESULT UmdResourceDesc(const D3D10DDIARG_CREATERESOURCE *A,MALI_ALLOCATION_DESC *Out) {
    if(!A || !A->pMipInfoList || A->MiscFlags || A->pPrimaryDesc || A->MipLevels!=1 ||
       A->ArraySize!=1 || A->SampleDesc.Count!=1 || A->SampleDesc.Quality ||
       (A->MapFlags&~D3D10_DDI_CPU_ACCESS_MASK))return E_INVALIDARG;
    const D3D10DDI_MIPINFO *M=A->pMipInfoList;
    if(!M->TexelWidth || M->TexelDepth!=1 || M->PhysicalWidth!=M->TexelWidth ||
       M->PhysicalHeight!=M->TexelHeight || M->PhysicalDepth!=1)return E_INVALIDARG;
    MALI_ALLOCATION_DESC D={MALI_ALLOCATION_VERSION,sizeof(D),0,
        MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING,0,0,0,0,0};
    UINT Allowed;
    if(A->ResourceDimension==D3D10DDIRESOURCE_BUFFER) {
        if(M->TexelHeight!=1 || A->Format!=DXGI_FORMAT_UNKNOWN)return E_INVALIDARG;
        D.Kind=MALI_ALLOCATION_BUFFER;D.Size=((uint64_t)M->TexelWidth+4095u)&~UINT64_C(4095);
        Allowed=D3D10_DDI_BIND_VERTEX_BUFFER|D3D10_DDI_BIND_INDEX_BUFFER|
            D3D10_DDI_BIND_CONSTANT_BUFFER|D3D10_DDI_BIND_SHADER_RESOURCE;
        if(A->BindFlags&D3D10_DDI_BIND_CONSTANT_BUFFER)
            if(A->BindFlags!=D3D10_DDI_BIND_CONSTANT_BUFFER || (M->TexelWidth&15u) || M->TexelWidth>65536)
                return E_INVALIDARG;
    } else if(A->ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D) {
        if(!M->TexelHeight || M->TexelWidth>16384 || M->TexelHeight>16384)return E_INVALIDARG;
        D.Kind=MALI_ALLOCATION_LINEAR_2D;D.Width=M->TexelWidth;D.Height=M->TexelHeight;
        D.Pitch=(D.Width*4u+63u)&~63u;D.Size=((uint64_t)D.Pitch*D.Height+4095u)&~UINT64_C(4095);
        Allowed=D3D10_DDI_BIND_RENDER_TARGET|D3D10_DDI_BIND_SHADER_RESOURCE;
        switch(A->Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:D.Format=MaliClearRgba8;break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:D.Format=MaliClearBgra8;break;
            case DXGI_FORMAT_B8G8R8X8_UNORM:D.Format=MaliClearBgrx8;break;
            default:return E_INVALIDARG;
        }
    } else return E_INVALIDARG;
    if(D.Size>UMD_TARGET_LIMIT)return E_OUTOFMEMORY;
    if(A->BindFlags&~Allowed)return E_INVALIDARG;
    switch(A->Usage) {
        case D3D10_DDI_USAGE_DEFAULT:if(A->MapFlags)return E_INVALIDARG;break;
        case D3D10_DDI_USAGE_IMMUTABLE:
            if(A->MapFlags || !A->pInitialDataUP || (A->BindFlags&D3D10_DDI_BIND_RENDER_TARGET))return E_INVALIDARG;
            break;
        case D3D10_DDI_USAGE_DYNAMIC:
            if(A->MapFlags!=D3D10_DDI_CPU_ACCESS_WRITE || !A->BindFlags ||
               (A->BindFlags&D3D10_DDI_BIND_RENDER_TARGET))return E_INVALIDARG;
            break;
        case D3D10_DDI_USAGE_STAGING:
            if(A->BindFlags || !A->MapFlags)return E_INVALIDARG;
            break;
        default:return E_INVALIDARG;
    }
    if(A->pInitialDataUP) {
        const D3D10_DDIARG_SUBRESOURCE_UP *I=A->pInitialDataUP;
        uint64_t Bytes=M->TexelWidth;
        if(D.Kind==MALI_ALLOCATION_LINEAR_2D) {
            if(I->SysMemPitch<D.Width*4u)return E_INVALIDARG;
            Bytes=(uint64_t)(D.Height-1u)*I->SysMemPitch+(uint64_t)D.Width*4u;
        }
        if(!I->pSysMem || Bytes>UINTPTR_MAX-(uintptr_t)I->pSysMem)return E_INVALIDARG;
    }
    *Out=D;return S_OK;
}
static UMD_RESOURCE *UmdFindResource(UMD_DEVICE *D,const void *Private) {
    if(!Private || ((const UMD_TOKEN *)Private)->Owner!=D)return 0;
    uint64_t Id=((const UMD_TOKEN *)Private)->Id;
    if(Id)for(UMD_RESOURCE *R=D->Resources;R;R=R->Next)if(R->Id==Id && R->Live)return R;
    return 0;
}
static UMD_VIEW *UmdFindView(UMD_DEVICE *D,const void *Private) {
    if(!Private || ((const UMD_TOKEN *)Private)->Owner!=D)return 0;
    uint64_t Id=((const UMD_TOKEN *)Private)->Id;
    if(Id)for(UMD_VIEW *V=D->Views;V;V=V->Next)if(V->Id==Id)return V;
    return 0;
}
static SIZE_T APIENTRY UmdCalcResource(D3D10DDI_HDEVICE Handle,const D3D10DDIARG_CREATERESOURCE *A) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return 0;
    MALI_ALLOCATION_DESC Desc;HRESULT H=UmdResourceDesc(A,&Desc);UmdError(D,H);
    return SUCCEEDED(H)?sizeof(UMD_TOKEN):0;
}
typedef struct {UINT X,Y,Width,Height,PixelBytes;} UMD_REGION;
static HRESULT UmdWriteUp(UMD_DEVICE *,UMD_RESOURCE *,const UMD_REGION *,const void *,UINT);
static VOID APIENTRY UmdCreateResource(D3D10DDI_HDEVICE Handle,const D3D10DDIARG_CREATERESOURCE *A,
    D3D10DDI_HRESOURCE Resource,D3D10DDI_HRTRESOURCE Runtime) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    HRESULT H;MALI_ALLOCATION_DESC Desc;
    if(!Resource.pDrvPrivate){UmdError(D,E_INVALIDARG);return;}
    ((UMD_TOKEN *)Resource.pDrvPrivate)->Id=0;
    ((UMD_TOKEN *)Resource.pDrvPrivate)->Owner=D;
    H=UmdResourceDesc(A,&Desc);if(FAILED(H)){UmdError(D,H);return;}
    EnterCriticalSection(&D->Gate);H=UmdActive(D);if(FAILED(H))goto Done;
    (void)UmdReap(D);
    if(D->ResourceCount==UMD_LIMIT || D->NextId==UINT64_MAX){H=E_OUTOFMEMORY;goto Done;}
    UMD_RESOURCE *R=UmdNew(sizeof(*R));if(!R){H=E_OUTOFMEMORY;goto Done;}
    R->Id=++D->NextId;R->Desc=Desc;R->Format=A->Format;R->Initializing=1;
    R->Usage=A->Usage;R->BindFlags=A->BindFlags;R->MapFlags=A->MapFlags;

    R->LogicalBytes=Desc.Kind==MALI_ALLOCATION_BUFFER?A->pMipInfoList->TexelWidth:Desc.Pitch*Desc.Height;
    /* Insert before the first OS call: cleanup failures cannot lose ownership. */
    R->Next=D->Resources;D->Resources=R;++D->ResourceCount;
    H=UmdCreateBo(D,&R->Bo,&Desc,Runtime.handle,MaliPteRead|MaliPteWrite);
    if(SUCCEEDED(H) && A->pInitialDataUP) {
        UMD_REGION V={0,0,Desc.Kind==MALI_ALLOCATION_BUFFER?R->LogicalBytes:Desc.Width,
            Desc.Kind==MALI_ALLOCATION_BUFFER?1u:Desc.Height,Desc.Kind==MALI_ALLOCATION_BUFFER?1u:4u};
        H=UmdWriteUp(D,R,&V,A->pInitialDataUP->pSysMem,A->pInitialDataUP->SysMemPitch);
    }
    R->Initializing=0;
    if(SUCCEEDED(H)){R->Live=1;((UMD_TOKEN *)Resource.pDrvPrivate)->Id=R->Id;}
    else (void)UmdReap(D);
Done:
    LeaveCriticalSection(&D->Gate);UmdError(D,H);
}
static VOID APIENTRY UmdDestroyResource(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Resource) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);
    UMD_RESOURCE *R=UmdFindResource(D,Resource.pDrvPrivate);HRESULT H=R?S_OK:E_INVALIDARG;
    if(R){R->Live=0;((UMD_TOKEN *)Resource.pDrvPrivate)->Id=0;(void)UmdReap(D);}
    LeaveCriticalSection(&D->Gate);UmdError(D,H);
}
static HRESULT UmdViewResource(UMD_DEVICE *D,const D3D10DDIARG_CREATERENDERTARGETVIEW *A,UMD_RESOURCE **R) {
    if(!A || A->ResourceDimension!=D3D10DDIRESOURCE_TEXTURE2D || A->Tex2D.MipSlice ||
       A->Tex2D.FirstArraySlice || A->Tex2D.ArraySize!=1)return E_INVALIDARG;
    *R=UmdFindResource(D,A->hDrvResource.pDrvPrivate);
    return *R && (*R)->Desc.Kind==MALI_ALLOCATION_LINEAR_2D &&
        ((*R)->BindFlags&D3D10_DDI_BIND_RENDER_TARGET) && (*R)->Format==A->Format?S_OK:E_INVALIDARG;
}
static SIZE_T APIENTRY UmdCalcView(D3D10DDI_HDEVICE Handle,const D3D10DDIARG_CREATERENDERTARGETVIEW *A) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return 0;
    EnterCriticalSection(&D->Gate);UMD_RESOURCE *R;HRESULT H=UmdViewResource(D,A,&R);
    LeaveCriticalSection(&D->Gate);UmdError(D,H);return SUCCEEDED(H)?sizeof(UMD_TOKEN):0;
}
static VOID APIENTRY UmdCreateView(D3D10DDI_HDEVICE Handle,const D3D10DDIARG_CREATERENDERTARGETVIEW *A,
    D3D10DDI_HRENDERTARGETVIEW View,D3D10DDI_HRTRENDERTARGETVIEW Runtime) {
    (void)Runtime;UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    if(!View.pDrvPrivate){UmdError(D,E_INVALIDARG);return;}
    ((UMD_TOKEN *)View.pDrvPrivate)->Id=0;
    ((UMD_TOKEN *)View.pDrvPrivate)->Owner=D;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);if(FAILED(H))goto Done;
    UMD_RESOURCE *R;H=UmdViewResource(D,A,&R);if(FAILED(H))goto Done;
    if(D->ViewCount==UMD_LIMIT || D->NextId==UINT64_MAX){H=E_OUTOFMEMORY;goto Done;}
    UMD_VIEW *V=UmdNew(sizeof(*V));if(!V){H=E_OUTOFMEMORY;goto Done;}
    V->Id=++D->NextId;V->Resource=R;++R->Views;V->Next=D->Views;D->Views=V;++D->ViewCount;
    ((UMD_TOKEN *)View.pDrvPrivate)->Id=V->Id;
Done:
    LeaveCriticalSection(&D->Gate);UmdError(D,H);
}
static VOID APIENTRY UmdDestroyView(D3D10DDI_HDEVICE Handle,D3D10DDI_HRENDERTARGETVIEW View) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);UMD_VIEW *V=UmdFindView(D,View.pDrvPrivate);HRESULT H=V?S_OK:E_INVALIDARG;
    if(V) {
        if(D->RenderTarget==V)D->RenderTarget=0;
        UMD_VIEW **L=&D->Views;while(*L!=V)L=&(*L)->Next;*L=V->Next;--D->ViewCount;
        --V->Resource->Views;((UMD_TOKEN *)View.pDrvPrivate)->Id=0;UmdFree(V);(void)UmdReap(D);
    }
    LeaveCriticalSection(&D->Gate);UmdError(D,H);
}
static HRESULT UmdRasterizerDesc(const D3D10_DDI_RASTERIZER_DESC *R) {
    if(!R || (R->CullMode!=D3D10_DDI_CULL_NONE && R->CullMode!=D3D10_DDI_CULL_FRONT &&
        R->CullMode!=D3D10_DDI_CULL_BACK) || (R->FrontCounterClockwise!=FALSE && R->FrontCounterClockwise!=TRUE) ||
        (R->DepthClipEnable!=FALSE && R->DepthClipEnable!=TRUE) ||
        (R->ScissorEnable!=FALSE && R->ScissorEnable!=TRUE) ||
        (R->MultisampleEnable!=FALSE && R->MultisampleEnable!=TRUE) ||
        (R->AntialiasedLineEnable!=FALSE && R->AntialiasedLineEnable!=TRUE))return E_INVALIDARG;
    if(R->FillMode!=D3D10_DDI_FILL_SOLID || R->DepthBias || R->DepthBiasClamp!=0.0f ||
       R->SlopeScaledDepthBias!=0.0f || !R->DepthClipEnable || R->MultisampleEnable ||
       R->AntialiasedLineEnable)return DXGI_ERROR_UNSUPPORTED;
    return S_OK;
}
static HRESULT UmdBlendDesc(const D3D10_DDI_BLEND_DESC *B) {
    if(!B || (B->AlphaToCoverageEnable!=FALSE && B->AlphaToCoverageEnable!=TRUE))return E_INVALIDARG;
    if(B->AlphaToCoverageEnable)return DXGI_ERROR_UNSUPPORTED;
    for(UINT I=0;I<D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT;I++)
        if(B->BlendEnable[I] || (I==0?B->RenderTargetWriteMask[I]!=D3D10_DDI_COLOR_WRITE_ENABLE_ALL:
            (B->RenderTargetWriteMask[I]!=0 && B->RenderTargetWriteMask[I]!=D3D10_DDI_COLOR_WRITE_ENABLE_ALL)))
            return DXGI_ERROR_UNSUPPORTED;
    return S_OK;
}
static HRESULT UmdDepthDesc(const D3D10_DDI_DEPTH_STENCIL_DESC *S) {
    if(!S)return E_INVALIDARG;
    if(S->DepthEnable || S->StencilEnable)return DXGI_ERROR_UNSUPPORTED;
    return S_OK;
}
static UMD_STATE *UmdFindState(UMD_DEVICE *D,const void *Private,UINT Kind) {
    UMD_STATE *S=(UMD_STATE *)Private;
    return S && S->Signature==UMD_STATE_SIGNATURE && S->Kind==Kind && S->Owner==D?S:0;
}
static SIZE_T UmdCalcState(D3D10DDI_HDEVICE Handle,const void *Desc,UINT Kind) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D || !Desc)return 0;
    HRESULT H=Kind==UMD_STATE_RASTERIZER?UmdRasterizerDesc(Desc):
        Kind==UMD_STATE_BLEND?UmdBlendDesc(Desc):UmdDepthDesc(Desc);
    UmdError(D,H);return SUCCEEDED(H)?sizeof(UMD_STATE):0;
}
static VOID UmdCreateState(D3D10DDI_HDEVICE Handle,const void *Desc,void *Private,UINT Kind) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    HRESULT H=!Private?E_INVALIDARG:Kind==UMD_STATE_RASTERIZER?UmdRasterizerDesc(Desc):
        Kind==UMD_STATE_BLEND?UmdBlendDesc(Desc):UmdDepthDesc(Desc);
    if(SUCCEEDED(H)) {
        UMD_STATE *S=Private;ZeroMemory(S,sizeof(*S));S->Signature=UMD_STATE_SIGNATURE;
        S->Kind=Kind;S->Owner=D;
        if(Kind==UMD_STATE_RASTERIZER)CopyMemory(&S->Desc.Rasterizer,Desc,sizeof(S->Desc.Rasterizer));
        else if(Kind==UMD_STATE_BLEND)CopyMemory(&S->Desc.Blend,Desc,sizeof(S->Desc.Blend));
        else CopyMemory(&S->Desc.DepthStencil,Desc,sizeof(S->Desc.DepthStencil));
    }
    UmdError(D,H);
}
static SIZE_T APIENTRY UmdCalcRasterizerState(D3D10DDI_HDEVICE H,const D3D10_DDI_RASTERIZER_DESC *D) {
    return UmdCalcState(H,D,UMD_STATE_RASTERIZER);
}
static VOID APIENTRY UmdCreateRasterizerState(D3D10DDI_HDEVICE H,const D3D10_DDI_RASTERIZER_DESC *D,
    D3D10DDI_HRASTERIZERSTATE S,D3D10DDI_HRTRASTERIZERSTATE Runtime) {
    (void)Runtime;UmdCreateState(H,D,S.pDrvPrivate,UMD_STATE_RASTERIZER);
}
static VOID APIENTRY UmdDestroyRasterizerState(D3D10DDI_HDEVICE H,D3D10DDI_HRASTERIZERSTATE S) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;
    UMD_STATE *P=UmdFindState(D,S.pDrvPrivate,UMD_STATE_RASTERIZER);HRESULT R=P?S_OK:E_INVALIDARG;
    if(P)ZeroMemory(P,sizeof(*P));UmdError(D,R);
}
static SIZE_T APIENTRY UmdCalcBlendState(D3D10DDI_HDEVICE H,const D3D10_DDI_BLEND_DESC *D) {
    return UmdCalcState(H,D,UMD_STATE_BLEND);
}
static VOID APIENTRY UmdCreateBlendState(D3D10DDI_HDEVICE H,const D3D10_DDI_BLEND_DESC *D,
    D3D10DDI_HBLENDSTATE S,D3D10DDI_HRTBLENDSTATE Runtime) {
    (void)Runtime;UmdCreateState(H,D,S.pDrvPrivate,UMD_STATE_BLEND);
}
static VOID APIENTRY UmdDestroyBlendState(D3D10DDI_HDEVICE H,D3D10DDI_HBLENDSTATE S) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;
    UMD_STATE *P=UmdFindState(D,S.pDrvPrivate,UMD_STATE_BLEND);HRESULT R=P?S_OK:E_INVALIDARG;
    if(P)ZeroMemory(P,sizeof(*P));UmdError(D,R);
}
static SIZE_T APIENTRY UmdCalcDepthState(D3D10DDI_HDEVICE H,const D3D10_DDI_DEPTH_STENCIL_DESC *D) {
    return UmdCalcState(H,D,UMD_STATE_DEPTH);
}
static VOID APIENTRY UmdCreateDepthState(D3D10DDI_HDEVICE H,const D3D10_DDI_DEPTH_STENCIL_DESC *D,
    D3D10DDI_HDEPTHSTENCILSTATE S,D3D10DDI_HRTDEPTHSTENCILSTATE Runtime) {
    (void)Runtime;UmdCreateState(H,D,S.pDrvPrivate,UMD_STATE_DEPTH);
}
static VOID APIENTRY UmdDestroyDepthState(D3D10DDI_HDEVICE H,D3D10DDI_HDEPTHSTENCILSTATE S) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;
    UMD_STATE *P=UmdFindState(D,S.pDrvPrivate,UMD_STATE_DEPTH);HRESULT R=P?S_OK:E_INVALIDARG;
    if(P)ZeroMemory(P,sizeof(*P));UmdError(D,R);
}
static VOID APIENTRY UmdSetRasterizerState(D3D10DDI_HDEVICE H,D3D10DDI_HRASTERIZERSTATE State) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    UMD_STATE *S=State.pDrvPrivate?UmdFindState(D,State.pDrvPrivate,UMD_STATE_RASTERIZER):0;
    if(SUCCEEDED(R) && State.pDrvPrivate && !S)R=E_INVALIDARG;
    if(SUCCEEDED(R)) {D->RasterizerValid=S!=0;if(S)D->Rasterizer=S->Desc.Rasterizer;D->DrawInvalidMask&=~UMD_DRAW_INVALID_RASTERIZER;}
    else D->DrawInvalidMask|=UMD_DRAW_INVALID_RASTERIZER;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetBlendState(D3D10DDI_HDEVICE H,D3D10DDI_HBLENDSTATE State,
    const FLOAT Factor[4],UINT SampleMask) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    UMD_STATE *S=State.pDrvPrivate?UmdFindState(D,State.pDrvPrivate,UMD_STATE_BLEND):0;
    if(SUCCEEDED(R) && State.pDrvPrivate && !S)R=E_INVALIDARG;
    if(SUCCEEDED(R) && (!Factor || SampleMask!=UINT_MAX))R=DXGI_ERROR_UNSUPPORTED;
    if(SUCCEEDED(R)) {D->BlendValid=S!=0;D->SampleMask=SampleMask;if(S)D->Blend=S->Desc.Blend;D->DrawInvalidMask&=~UMD_DRAW_INVALID_BLEND;}
    else D->DrawInvalidMask|=UMD_DRAW_INVALID_BLEND;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetDepthState(D3D10DDI_HDEVICE H,D3D10DDI_HDEPTHSTENCILSTATE State,UINT StencilRef) {
    (void)StencilRef;UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    UMD_STATE *S=State.pDrvPrivate?UmdFindState(D,State.pDrvPrivate,UMD_STATE_DEPTH):0;
    if(SUCCEEDED(R) && State.pDrvPrivate && !S)R=E_INVALIDARG;
    if(SUCCEEDED(R)) {D->DepthValid=S!=0;if(S)D->DepthStencil=S->Desc.DepthStencil;D->DrawInvalidMask&=~UMD_DRAW_INVALID_DEPTH;}
    else D->DrawInvalidMask|=UMD_DRAW_INVALID_DEPTH;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetTopology(D3D10DDI_HDEVICE H,D3D10_DDI_PRIMITIVE_TOPOLOGY Topology) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    if(SUCCEEDED(R) && Topology!=D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST)R=DXGI_ERROR_UNSUPPORTED;
    if(SUCCEEDED(R)){D->Topology=Topology;D->TopologySet=1;D->DrawInvalidMask&=~UMD_DRAW_INVALID_TOPOLOGY;}
    else D->DrawInvalidMask|=UMD_DRAW_INVALID_TOPOLOGY;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetVertexBuffers(D3D10DDI_HDEVICE H,UINT Start,UINT Count,
    const D3D10DDI_HRESOURCE *Resources,const UINT *Strides,const UINT *Offsets) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    if(SUCCEEDED(R) && (Start>D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ||
       Count>D3D10_1_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT-Start || (Count && (!Resources || !Strides || !Offsets))))R=E_INVALIDARG;
    if(SUCCEEDED(R)) {
        for(UINT I=0;I<Count;I++) {
            UINT Bit=1u<<(Start+I);
            if(Resources[I].pDrvPrivate || Strides[I] || Offsets[I])D->VertexBufferMask|=Bit;
            else D->VertexBufferMask&=~Bit;
        }
        D->DrawInvalidMask&=~UMD_DRAW_INVALID_INPUT;
        if(D->VertexBufferMask)R=DXGI_ERROR_UNSUPPORTED;
    } else D->DrawInvalidMask|=UMD_DRAW_INVALID_INPUT;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetIndexBuffer(D3D10DDI_HDEVICE H,D3D10DDI_HRESOURCE Resource,DXGI_FORMAT Format,UINT Offset) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    if(SUCCEEDED(R)) {
        D->IndexBufferBound=Resource.pDrvPrivate || Format!=DXGI_FORMAT_UNKNOWN || Offset;
        D->DrawInvalidMask&=~UMD_DRAW_INVALID_INPUT;
        if(D->IndexBufferBound)R=DXGI_ERROR_UNSUPPORTED;
    } else D->DrawInvalidMask|=UMD_DRAW_INVALID_INPUT;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetInputLayout(D3D10DDI_HDEVICE H,D3D10DDI_HELEMENTLAYOUT Layout) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    if(SUCCEEDED(R)) {
        D->InputLayoutBound=Layout.pDrvPrivate!=0;
        D->DrawInvalidMask&=~UMD_DRAW_INVALID_INPUT;
        if(D->InputLayoutBound)R=DXGI_ERROR_UNSUPPORTED;
    } else D->DrawInvalidMask|=UMD_DRAW_INVALID_INPUT;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetViewports(D3D10DDI_HDEVICE H,UINT Count,UINT ClearCount,const D3D10_DDI_VIEWPORT *Viewports) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    if(SUCCEEDED(R) && (Count>1 || ClearCount>D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE-Count || (Count && !Viewports)))R=DXGI_ERROR_UNSUPPORTED;
    if(SUCCEEDED(R) && Count) {
        const D3D10_DDI_VIEWPORT *V=&Viewports[0];
        if(V->TopLeftX!=0.0f || V->TopLeftY!=0.0f || V->Width<=0.0f || V->Height<=0.0f ||
           V->MinDepth!=0.0f || V->MaxDepth!=1.0f)R=DXGI_ERROR_UNSUPPORTED;
    }
    if(SUCCEEDED(R)){D->ViewportSet=Count!=0;if(Count)D->Viewport=Viewports[0];D->DrawInvalidMask&=~UMD_DRAW_INVALID_VIEWPORT;}
    else D->DrawInvalidMask|=UMD_DRAW_INVALID_VIEWPORT;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetScissors(D3D10DDI_HDEVICE H,UINT Count,UINT ClearCount,const D3D10_DDI_RECT *Rects) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    if(SUCCEEDED(R) && (Count>1 || ClearCount>D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE-Count || (Count && !Rects)))R=DXGI_ERROR_UNSUPPORTED;
    if(SUCCEEDED(R) && Count && (Rects[0].left<0 || Rects[0].top<0 || Rects[0].right<=Rects[0].left || Rects[0].bottom<=Rects[0].top))R=E_INVALIDARG;
    if(SUCCEEDED(R)){D->ScissorSet=Count!=0;if(Count)D->Scissor=Rects[0];D->DrawInvalidMask&=~UMD_DRAW_INVALID_SCISSOR;}
    else D->DrawInvalidMask|=UMD_DRAW_INVALID_SCISSOR;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static VOID APIENTRY UmdSetRenderTargets(D3D10DDI_HDEVICE H,const D3D10DDI_HRENDERTARGETVIEW *Targets,
    UINT Count,UINT ClearSlots,D3D10DDI_HDEPTHSTENCILVIEW Depth) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);UMD_VIEW *V=0;
    if(SUCCEEDED(R) && (Count>1 || ClearSlots>D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT-Count || (Count && !Targets)))R=DXGI_ERROR_UNSUPPORTED;
    if(SUCCEEDED(R) && Depth.pDrvPrivate)R=DXGI_ERROR_UNSUPPORTED;
    if(SUCCEEDED(R) && Count){V=UmdFindView(D,Targets[0].pDrvPrivate);if(!V)R=E_INVALIDARG;}
    if(SUCCEEDED(R)){D->RenderTarget=V;D->DrawInvalidMask&=~UMD_DRAW_INVALID_TARGET;}
    else D->DrawInvalidMask|=UMD_DRAW_INVALID_TARGET;
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static int UmdShaderIo(const UMD_SHADER *V,const UMD_SHADER *P) {
    if(!V || !P || V->Source.stage!=T70_SHADER_VERTEX || P->Source.stage!=T70_SHADER_PIXEL ||
       V->Source.input_count!=1 || V->Source.output_count!=1 || P->Source.input_count!=0 || P->Source.output_count!=1 ||
       !V->Source.inputs || !V->Source.outputs || !P->Source.outputs)return 0;
    const struct t70_shader_signature_entry *VI=V->Source.inputs,*VO=V->Source.outputs,*PO=P->Source.outputs;
    return VI[0].system_value==D3D10_SB_NAME_VERTEX_ID && VI[0].register_index==0 && VI[0].mask==1 &&
       VO[0].system_value==D3D10_SB_NAME_POSITION && VO[0].register_index==0 && VO[0].mask==15 &&
       PO[0].system_value==D3D10_SB_NAME_UNDEFINED && PO[0].register_index==0 && PO[0].mask==15;
}
static int UmdFauConst(const struct t70_shader *S,UINT Index) {
    return (S->fau_const_mask[Index>>5]>>(Index&31u))&1u;
}
static HRESULT UmdShaderResult(int Status);
static HRESULT UmdUploadSelected(UMD_DEVICE *D,
    const struct t70_shader_io_types *Types,struct t70_shader *Preload,MALI_UMD_PROGRAM *Out);
static int UmdDrawPreload(const struct t70_shader *P) {
    if(!P || !P->binary || P->stage!=T70_SHADER_PIXEL || P->arch!=9 ||
       P->tls_size || P->wls_size || P->attributes_read || P->attribute_count ||
       P->varying_count || P->varying_stride || P->cb_mask ||
       (P->flags&~(T70_SHADER_FLAG_FTZ32|T70_SHADER_FLAG_FTZ16)) ||
       P->fau_reserved!=T70_SHADER_FB_PRELOAD_FAU_WORDS ||
       P->fau_count<T70_SHADER_FB_PRELOAD_FAU_WORDS || P->fau_count>128)return 0;
    for(UINT I=T70_SHADER_FB_PRELOAD_FAU_WORDS;I<P->fau_count;I++)if(!UmdFauConst(P,I))return 0;
    return 1;
}
static int UmdDrawShader(const MALI_UMD_PROGRAM *P) {
    if(!P || !P->Compiled)return 0;
    const struct t70_shader *V=&P->Compiled->vertex,*F=&P->Compiled->pixel;
    if(V->tls_size || V->wls_size || F->tls_size || F->wls_size || V->attributes_read || V->attribute_count ||
       V->varying_count || V->varying_stride || F->varying_count || F->varying_stride || V->cb_mask || F->cb_mask ||
       V->sampler_mask || F->sampler_mask || V->texture_mask[0] || V->texture_mask[1] || V->texture_mask[2] || V->texture_mask[3] ||
       F->texture_mask[0] || F->texture_mask[1] || F->texture_mask[2] || F->texture_mask[3] ||
       (V->flags&~(T70_SHADER_FLAG_IDVS|T70_SHADER_FLAG_FTZ32|T70_SHADER_FLAG_FTZ16)) ||
       (F->flags&~(T70_SHADER_FLAG_FTZ32|T70_SHADER_FLAG_FTZ16)) || !(V->flags&T70_SHADER_FLAG_IDVS) ||
       V->fau_reserved!=12 || V->fau_count<12 || V->fau_count>128 || V->fau_count<V->fau_reserved ||
       F->fau_reserved || F->fau_count>128)return 0;
    for(UINT I=12;I<V->fau_count;I++)if(!UmdFauConst(V,I))return 0;
    for(UINT I=0;I<F->fau_count;I++)if(!UmdFauConst(F,I))return 0;
    return 1;
}
/* Gate held. Pending submissions keep the retired program alive in Programs. */
static HRESULT UmdDropDrawProgram(UMD_DEVICE *D) {
    if(!D->DrawProgram.Id)return S_OK;
    HRESULT H=MtkMaliUmdReleaseProgram((D3D10DDI_HDEVICE){D},&D->DrawProgram);
    D->DrawVertex=D->DrawPixel=0;D->DrawFormat=DXGI_FORMAT_UNKNOWN;
    if(FAILED(H) && H!=HRESULT_FROM_WIN32(ERROR_BUSY)){UmdFatal(D,H);return H;}
    return S_OK;
}
static UINT UmdFloatBits(FLOAT Value) {UINT Bits;CopyMemory(&Bits,&Value,sizeof(Bits));return Bits;}
static VOID APIENTRY UmdDraw(D3D10DDI_HDEVICE H,UINT VertexCount,UINT StartVertex) {
    UMD_DEVICE *D=UmdDevice(H);if(!D)return;EnterCriticalSection(&D->Gate);HRESULT R=UmdActive(D);
    MALI_UMD_PROGRAM Program={0};UMD_RESOURCE *Scratch=0;
    struct t70_shader *Preload=0;UMD_PROGRAM *Uploaded=0;UINT Borrowed=0;
    if(FAILED(R))goto Done;
    if(!VertexCount)goto Done;
    if(VertexCount<3 || VertexCount%3 || StartVertex>INT32_MAX || StartVertex>UINT_MAX-VertexCount || D->DrawInvalidMask ||
       !D->TopologySet || D->Topology!=D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST || !D->RenderTarget ||
       D->VertexBufferMask || D->IndexBufferBound || D->InputLayoutBound || D->SampleMask!=UINT_MAX) {R=DXGI_ERROR_UNSUPPORTED;goto Done;}
    UMD_VIEW *Bound=0;for(UMD_VIEW *I=D->Views;I;I=I->Next)if(I==D->RenderTarget){Bound=I;break;}
    UMD_RESOURCE *Target=Bound?Bound->Resource:0;
    if(!Target || !Target->Live || Target->MapType || Target->Bo.Locked || Target->Desc.Kind!=MALI_ALLOCATION_LINEAR_2D ||
       !(Target->BindFlags&D3D10_DDI_BIND_RENDER_TARGET) ||
       (Target->Format!=DXGI_FORMAT_R8G8B8A8_UNORM && Target->Format!=DXGI_FORMAT_B8G8R8A8_UNORM && Target->Format!=DXGI_FORMAT_B8G8R8X8_UNORM) ||
       !D->ViewportSet || D->Viewport.TopLeftX!=0.0f || D->Viewport.TopLeftY!=0.0f ||
       D->Viewport.Width!=(FLOAT)Target->Desc.Width || D->Viewport.Height!=(FLOAT)Target->Desc.Height ||
       D->Viewport.MinDepth!=0.0f || D->Viewport.MaxDepth!=1.0f) {R=DXGI_ERROR_UNSUPPORTED;goto Done;}
    D3D10_DDI_RASTERIZER_DESC Raster={0};Raster.FillMode=D3D10_DDI_FILL_SOLID;Raster.CullMode=D3D10_DDI_CULL_BACK;Raster.DepthClipEnable=TRUE;
    if(D->RasterizerValid)Raster=D->Rasterizer;
    R=UmdRasterizerDesc(&Raster);if(FAILED(R))goto Done;
    if(D->BlendValid && FAILED(UmdBlendDesc(&D->Blend))){R=DXGI_ERROR_UNSUPPORTED;goto Done;}
    if(D->DepthValid && FAILED(UmdDepthDesc(&D->DepthStencil))){R=DXGI_ERROR_UNSUPPORTED;goto Done;}
    D3D10_DDI_RECT Scissor={0,0,(LONG)Target->Desc.Width,(LONG)Target->Desc.Height};
    if(Raster.ScissorEnable) {
        if(!D->ScissorSet || D->Scissor.right>(LONG)Target->Desc.Width || D->Scissor.bottom>(LONG)Target->Desc.Height) {R=DXGI_ERROR_UNSUPPORTED;goto Done;}
        Scissor=D->Scissor;
    }
    if(!UmdShaderIo(D->VertexShader,D->PixelShader)){R=DXGI_ERROR_UNSUPPORTED;goto Done;}
    if(D->DrawProgram.Id && D->DrawVertex==D->VertexShader->Id &&
       D->DrawPixel==D->PixelShader->Id && D->DrawFormat==Target->Format) {
        Program=D->DrawProgram;Borrowed=1;
    } else {
        struct t70_shader_io_types Types={0};
        struct t70_shader_fb_preload_request PreloadRequest={T70_SHADER_ABI,T70_SHADER_GPU_ID,
            Target->Format==DXGI_FORMAT_R8G8B8A8_UNORM?T70_SHADER_FB_RGBA8_UNORM:
            Target->Format==DXGI_FORMAT_B8G8R8A8_UNORM?T70_SHADER_FB_BGRA8_UNORM:T70_SHADER_FB_BGRX8_UNORM,1,1,0};
        /* Shader metadata includes fixed FAU/varying arrays. Keep it off the
           callback stack; the program consumes its output after linking. */
        Preload=UmdNew(sizeof(*Preload));if(!Preload){R=E_OUTOFMEMORY;goto Done;}
        R=UmdShaderResult(t70_shader_compile_framebuffer_preload(&PreloadRequest,Preload,0,0));
        if(FAILED(R))goto Done;
        if(!UmdDrawPreload(Preload)){R=DXGI_ERROR_UNSUPPORTED;goto Done;}
        R=UmdUploadSelected(D,&Types,Preload,&Program);if(FAILED(R))goto Done;
    }
    if(!UmdDrawShader(&Program)){R=DXGI_ERROR_UNSUPPORTED;goto Done;}
    for(Uploaded=D->Programs;Uploaded && Uploaded->Id!=Program.Id;Uploaded=Uploaded->Next){}
    if(!Uploaded || !Uploaded->Live || !Uploaded->Preload.binary){R=E_UNEXPECTED;goto Done;}
    if(D->ResourceCount==UMD_LIMIT || D->NextId==UINT64_MAX){R=E_OUTOFMEMORY;goto Done;}
    Scratch=UmdNew(sizeof(*Scratch));if(!Scratch){R=E_OUTOFMEMORY;goto Done;}
    Scratch->Id=++D->NextId;Scratch->Desc=(MALI_ALLOCATION_DESC){MALI_ALLOCATION_VERSION,sizeof(MALI_ALLOCATION_DESC),
        MALI_ALLOCATION_BUFFER,MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING,UMD_DRAW_SCRATCH_BYTES,0,0,0,0};
    Scratch->Initializing=1;Scratch->Next=D->Resources;D->Resources=Scratch;++D->ResourceCount;
    MALI_ALLOCATION_DESC ScratchDesc=Scratch->Desc;
    R=UmdCreateBo(D,&Scratch->Bo,&ScratchDesc,0,MaliPteRead|MaliPteWrite);
    if(FAILED(R)){Scratch->Initializing=0;Scratch->Live=0;(void)UmdReap(D);goto Done;}
    R=UmdLock(D,&Scratch->Bo);if(FAILED(R)){Scratch->Initializing=0;Scratch->Live=0;(void)UmdReap(D);goto Done;}
    ZeroMemory(Scratch->Bo.Cpu,(SIZE_T)Scratch->Bo.Bytes);
    UMD_TOKEN TargetToken={Target->Id,D},ScratchToken={Scratch->Id,D};
    MALI_COMMAND_PRELOAD Load={0};Load.Bytes=sizeof(Load);Load.Width=Target->Desc.Width;Load.Height=Target->Desc.Height;
    Load.Pitch=Target->Desc.Pitch;Load.Format=Target->Desc.Format;Load.TargetVa=Target->Bo.Va;
    Load.TargetBytes=(UINT)Target->Bo.Bytes;Load.HeapVa=Scratch->Bo.Va+UMD_DRAW_HEAP_OFFSET;
    Load.ShaderProgramVa=Uploaded->Bo[2].Va+128u;
    Load.FauVa=Scratch->Bo.Va+UMD_DRAW_PRELOAD_FAU_OFFSET;
    Load.FauCount=(Uploaded->Preload.fau_count+1u)/2u;
    Load.ThreadStorageVa=Scratch->Bo.Va+UMD_DRAW_TLS_OFFSET;Load.ThreadStorageBytes=64;
    if(MaliCommandsBuildPreload(&D->Caps,&Load,
       (unsigned char *)Scratch->Bo.Cpu+UMD_DRAW_DESCRIPTOR_OFFSET,Scratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET,
       MALI_COMMAND_PRELOAD_DESCRIPTOR_BYTES)!=MaliOk){R=E_UNEXPECTED;goto ScratchFailed;}
    UINT VertexSlot=UINT_MAX;
    for(UINT I=0;I<MALI_JM_MAX_SLOTS;I++)if(I!=D->Slot && (D->Caps.JobSlots&(1u<<I)) &&
        (D->Caps.JobFeatures[I]&(1u<<11))){VertexSlot=I;break;}
    if(VertexSlot==UINT_MAX){R=DXGI_ERROR_UNSUPPORTED;goto ScratchFailed;}
    unsigned char *Bytes=Scratch->Bo.Cpu;UINT *Vfau=(void *)(Bytes+UMD_DRAW_VERTEX_FAU_OFFSET);
    UINT *Ffau=(void *)(Bytes+UMD_DRAW_PIXEL_FAU_OFFSET);const struct t70_shader *Vs=&Program.Compiled->vertex,*Ps=&Program.Compiled->pixel;
    CopyMemory(Vfau,Vs->fau_words,Vs->fau_count*sizeof(UINT));CopyMemory(Ffau,Ps->fau_words,Ps->fau_count*sizeof(UINT));
    UINT *Lfau=(void *)(Bytes+UMD_DRAW_PRELOAD_FAU_OFFSET);
    CopyMemory(Lfau,Uploaded->Preload.fau_words,Uploaded->Preload.fau_count*sizeof(UINT));
    struct t70_shader_fb_preload_sysvals Bounds={0,0,(uint16_t)(Target->Desc.Width-1u),(uint16_t)(Target->Desc.Height-1u)};
    CopyMemory(Lfau,&Bounds,sizeof(Bounds));
    FLOAT XScale=D->Viewport.Width*0.5f,YScale=-D->Viewport.Height*0.5f,ZScale=D->Viewport.MaxDepth-D->Viewport.MinDepth;
    Vfau[0]=UmdFloatBits(XScale);Vfau[1]=UmdFloatBits(YScale);Vfau[2]=UmdFloatBits(ZScale);
    Vfau[3]=0;Vfau[4]=UmdFloatBits(D->Viewport.TopLeftX+D->Viewport.Width*0.5f);
    Vfau[5]=UmdFloatBits(D->Viewport.TopLeftY+D->Viewport.Height*0.5f);Vfau[6]=UmdFloatBits(D->Viewport.MinDepth);
    Vfau[7]=0;Vfau[8]=StartVertex;Vfau[9]=0;Vfau[10]=0;Vfau[11]=0;
    /* Mesa's empty local-storage descriptor encodes 2^31 WLS instances in
       the low descriptor field; 31 is the hardware log2-minus-one value. */
    ((UINT *)(Bytes+UMD_DRAW_TLS_OFFSET))[1]=31u;
    MALI_UMD_RASTER_RESOURCE Resources[2]={{{&TargetToken},MaliPteRead|MaliPteWrite},{{&ScratchToken},MaliPteRead|MaliPteWrite}};
    MALI_COMMAND_RASTER Draw={0};Draw.Bytes=sizeof(Draw);Draw.VertexSlot=VertexSlot;Draw.FragmentSlot=D->Slot;
    Draw.Primitive=8u|(1u<<15)|(Raster.DepthClipEnable?((1u<<16)|(1u<<17)):0);
    Draw.BaseVertex=StartVertex;Draw.IndexCount=VertexCount;Draw.InstanceCount=1;
    Draw.Scissor[0]=(UINT)Scissor.left|((UINT)Scissor.top<<16);
    Draw.Scissor[1]=(UINT)(Scissor.right-1)|((UINT)(Scissor.bottom-1)<<16);
    Draw.PrimitiveSize=UmdFloatBits(1.0f);Draw.Flags[0]=(1u<<0)|(1u<<1)|(1u<<23)|
        (Raster.FrontCounterClockwise?(1u<<16):0)|
        (Raster.CullMode==D3D10_DDI_CULL_FRONT?(1u<<17):Raster.CullMode==D3D10_DDI_CULL_BACK?(1u<<18):0);
    Draw.Flags[1]=(D->SampleMask&0xffffu)|(1u<<16);Draw.MinZ=UmdFloatBits(0.0f);Draw.MaxZ=UmdFloatBits(1.0f);
    Draw.Tiles[1]=((Target->Desc.Height-1u)>>4)<<16|((Target->Desc.Width-1u)>>4);
    Draw.Tiler=Scratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET+192u;Draw.Framebuffer=(Scratch->Bo.Va+UMD_DRAW_DESCRIPTOR_OFFSET)|1u;
    Draw.Position.ThreadStorage=Draw.Fragment.ThreadStorage=Scratch->Bo.Va+UMD_DRAW_TLS_OFFSET;
    Draw.Position.Fau=Scratch->Bo.Va+UMD_DRAW_VERTEX_FAU_OFFSET;Draw.Position.FauCount=(Vs->fau_count+1u)/2u;
    Draw.Fragment.Fau=Ps->fau_count?Scratch->Bo.Va+UMD_DRAW_PIXEL_FAU_OFFSET:0;Draw.Fragment.FauCount=(Ps->fau_count+1u)/2u;
    R=UmdInvalidate(D,&Scratch->Bo);if(FAILED(R))goto ScratchFailed;
    R=UmdUnlock(D,&Scratch->Bo);if(FAILED(R))goto ScratchFailed;
    Scratch->Initializing=0;Scratch->Live=1;
    R=MtkMaliUmdSubmitRaster(H,&Program,&Draw,Resources,2);
    Scratch->Live=0;(void)UmdReap(D);
    if(SUCCEEDED(R) && !Borrowed) {
        HRESULT Release=UmdDropDrawProgram(D);
        D->DrawProgram=Program;D->DrawVertex=D->VertexShader->Id;
        D->DrawPixel=D->PixelShader->Id;D->DrawFormat=Target->Format;
        ZeroMemory(&Program,sizeof(Program));
        if(FAILED(Release))R=Release;
    }
    goto Done;
ScratchFailed:
    Scratch->Initializing=0;Scratch->Live=0;(void)UmdReap(D);
Done:
    if(Preload){t70_shader_free(Preload);UmdFree(Preload);}
    if(Program.Id && !Borrowed) {
        HRESULT Release=MtkMaliUmdReleaseProgram(H,&Program);
        if(FAILED(Release) && Release!=HRESULT_FROM_WIN32(ERROR_BUSY)) {
            UmdFatal(D,Release);if(SUCCEEDED(R))R=Release;
        }
    }
    LeaveCriticalSection(&D->Gate);UmdError(D,R);
}
static HRESULT UmdClear(UMD_DEVICE *D,UMD_RESOURCE *R,const FLOAT Color[4]) {
    if(!Color || R->MapType || R->Bo.Locked)return E_INVALIDARG;
    (void)UmdReap(D);if(D->PendingCount==UMD_PENDING_LIMIT)return E_OUTOFMEMORY;
    UMD_PENDING *P=UmdNew(sizeof(*P)+sizeof(P->Resources[0]));if(!P)return E_OUTOFMEMORY;
    P->ResourceCount=1;P->Resources[0]=R;++R->Pending;
    P->Next=D->Pending;D->Pending=P;++D->PendingCount;
    P->Event=CreateEventW(0,TRUE,FALSE,0);if(!P->Event)return E_OUTOFMEMORY;
    MALI_ALLOCATION_DESC Desc={MALI_ALLOCATION_VERSION,sizeof(Desc),MALI_ALLOCATION_BUFFER,
        MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING,4096,0,0,0,0};
    HRESULT H=S_OK;
    for(UINT I=0;I<3;I++) {
        Desc.Size=I==2?MALI_COMMAND_CLEAR_HEAP_BYTES:4096;
        H=UmdCreateBo(D,&P->Bo[I],&Desc,0,MaliPteRead|MaliPteWrite);if(FAILED(H))return H;
    }
    for(UINT I=0;I<2;I++){H=UmdLock(D,&P->Bo[I]);if(FAILED(H))return H;}
    MALI_COMMAND_CLEAR Clear={0};Clear.Bytes=sizeof(Clear);Clear.Width=R->Desc.Width;
    Clear.Height=R->Desc.Height;Clear.Pitch=R->Desc.Pitch;Clear.Format=R->Desc.Format;Clear.Slot=D->Slot;
    CopyMemory(Clear.Color,Color,sizeof(Clear.Color));Clear.TargetVa=R->Bo.Va;
    Clear.TargetBytes=(UINT)R->Bo.Bytes;Clear.HeapVa=P->Bo[2].Va;
    ZeroMemory(P->Bo[0].Cpu,4096);
    if(MaliCommandsBuildClear(&D->Caps,&Clear,P->Bo[0].Cpu,P->Bo[0].Va,MALI_COMMAND_CLEAR_JOB_BYTES,
       P->Bo[1].Cpu,P->Bo[1].Va,MALI_COMMAND_CLEAR_DESCRIPTOR_BYTES,P->Private,MALI_COMMAND_CLEAR_PRIVATE_BYTES)!=MaliOk)return E_INVALIDARG;
    for(UINT I=0;I<2;I++){H=UmdInvalidate(D,&P->Bo[I]);if(FAILED(H))return H;}
    /* Production KMD residency cleans shared backing to PoC before execution. */
    for(UINT I=0;I<2;I++){H=UmdUnlock(D,&P->Bo[I]);if(FAILED(H))return H;}
    H=UmdResident(D,&R->Bo);if(FAILED(H))return H;
    UINT64 Fence=R->Bo.PagingFence;
    for(UINT I=0;I<3;I++) {
        H=UmdResident(D,&P->Bo[I]);if(FAILED(H))return H;
        if(P->Bo[I].PagingFence>Fence)Fence=P->Bo[I].PagingFence;
    }
    H=UmdWaitPaging(D,Fence);if(FAILED(H))return H;
    D3DKMT_SUBMITCOMMAND Submit={0};Submit.Commands=P->Bo[0].Va;Submit.CommandLength=MALI_COMMAND_CLEAR_JOB_BYTES;
    Submit.BroadcastContextCount=1;Submit.BroadcastContext[0]=D->Context;
    Submit.pPrivateDriverData=P->Private;Submit.PrivateDriverDataSize=MALI_COMMAND_CLEAR_PRIVATE_BYTES;
    NTSTATUS S=D3DKMTSubmitCommand(&Submit);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    P->Submitted=1;return UmdSignal(D,P);
}
static VOID APIENTRY UmdClearView(D3D10DDI_HDEVICE Handle,D3D10DDI_HRENDERTARGETVIEW View,FLOAT Color[4]) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);
    if(SUCCEEDED(H)) {
        UMD_VIEW *V=UmdFindView(D,View.pDrvPrivate);
        H=V?UmdClear(D,V->Resource,Color):E_INVALIDARG;
        if(FAILED(H)){D->LastFailure=H;D->Error=DXGI_ERROR_DEVICE_REMOVED;(void)UmdReap(D);}
    }
    LeaveCriticalSection(&D->Gate);
    /* Clear has no failure result; only device removal is legal for this DDI. */
    UmdError(D,FAILED(H)?DXGI_ERROR_DEVICE_REMOVED:S_OK);
}
static VOID APIENTRY UmdFlush(D3D10DDI_HDEVICE Handle) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdReap(D);
    if(FAILED(H) && H!=HRESULT_FROM_WIN32(ERROR_BUSY)) {
        if(SUCCEEDED(D->LastFailure))D->LastFailure=H;D->Error=DXGI_ERROR_DEVICE_REMOVED;
    }
    HRESULT Error=D->Error;LeaveCriticalSection(&D->Gate);UmdError(D,Error);
}
/* Gate held. CPU access waits on OS completion rather than polling the GPU.
   GPU-idle success does not release our event or allocation ownership early. */
static int UmdPendingUses(const UMD_PENDING *P,const UMD_RESOURCE *R) {
    for(UINT I=0;I<P->ResourceCount;I++)if(P->Resources[I]==R)return 1;
    return 0;
}
static int UmdGpuBusy(UMD_DEVICE *D,UMD_RESOURCE *R) {
    for(UMD_PENDING *P=D->Pending;P;P=P->Next)if(UmdPendingUses(P,R) && P->Submitted)return 1;
    return 0;
}
static HRESULT UmdCpuIdle(UMD_DEVICE *D,UMD_RESOURCE *R,UINT DoNotWait) {
    (void)UmdReap(D);
    if(!UmdGpuBusy(D,R))return S_OK;
    if(DoNotWait)return DXGI_DDI_ERR_WASSTILLDRAWING;
    D3DKMT_WAITFORIDLE A={0};A.hDevice=D->Device;
    NTSTATUS S=D3DKMTWaitForIdle(&A);if(!UMD_NT_SUCCESS(S))return UmdStatus(S);
    for(UMD_PENDING *P=D->Pending;P;P=P->Next)if(UmdPendingUses(P,R) && P->Submitted) {
        HRESULT H=UmdSignal(D,P);if(FAILED(H))return H;
        if(WaitForSingleObject(P->Event,INFINITE)!=WAIT_OBJECT_0)return E_FAIL;
    }
    (void)UmdReap(D);return UmdGpuBusy(D,R)?E_UNEXPECTED:S_OK;
}
static HRESULT UmdMapDiscard(UMD_DEVICE *D,UMD_RESOURCE *R) {
    (void)UmdReap(D);
    if(R->OldCount==UMD_PENDING_LIMIT)return E_OUTOFMEMORY;
    UMD_OLD_BO *Old=UmdNew(sizeof(*Old));if(!Old)return E_OUTOFMEMORY;
    /* Track before OS allocation, so failed map/lock/cleanup cannot lose it. */
    Old->Next=R->OldBos;R->OldBos=Old;++R->OldCount;
    HRESULT H=UmdCreateBo(D,&Old->Bo,&R->Desc,0,MaliPteRead|MaliPteWrite);if(FAILED(H))return H;
    H=UmdLock(D,&Old->Bo);if(FAILED(H))return H;
    H=UmdInvalidate(D,&Old->Bo);if(FAILED(H))return H;
    UMD_BO Saved=R->Bo;R->Bo=Old->Bo;Old->Bo=Saved;return S_OK;
}
static VOID APIENTRY UmdMapResource(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Resource,
    UINT Subresource,D3D10_DDI_MAP Type,UINT Flags,D3D10DDI_MAPPED_SUBRESOURCE *Out) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    if(Out)ZeroMemory(Out,sizeof(*Out));
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);if(FAILED(H))goto Done;
    UMD_RESOURCE *R=UmdFindResource(D,Resource.pDrvPrivate);
    if(!Out || !R || Subresource || R->MapType || R->Bo.Locked ||
       (Flags&~D3D10_DDI_MAP_FLAG_DONOTWAIT)){H=E_INVALIDARG;goto Done;}
    if(R->Usage==D3D10_DDI_USAGE_STAGING) {
        UINT Access=Type==D3D10_DDI_MAP_READ?D3D10_DDI_CPU_ACCESS_READ:
            Type==D3D10_DDI_MAP_WRITE?D3D10_DDI_CPU_ACCESS_WRITE:
            Type==D3D10_DDI_MAP_READWRITE?D3D10_DDI_CPU_ACCESS_MASK:0;
        if(!Access || (R->MapFlags&Access)!=Access){H=E_INVALIDARG;goto Done;}
        H=UmdCpuIdle(D,R,Flags&D3D10_DDI_MAP_FLAG_DONOTWAIT);if(FAILED(H))goto Done;
        H=UmdLock(D,&R->Bo);if(SUCCEEDED(H))H=UmdInvalidate(D,&R->Bo);
    } else if(R->Usage==D3D10_DDI_USAGE_DYNAMIC && !Flags) {
        if(Type==D3D10_DDI_MAP_WRITE_DISCARD)H=UmdMapDiscard(D,R);
        else if(Type==D3D10_DDI_MAP_WRITE_NOOVERWRITE && R->Desc.Kind==MALI_ALLOCATION_BUFFER &&
                (R->BindFlags&(D3D10_DDI_BIND_VERTEX_BUFFER|D3D10_DDI_BIND_INDEX_BUFFER)) &&
                !(R->BindFlags&~(D3D10_DDI_BIND_VERTEX_BUFFER|D3D10_DDI_BIND_INDEX_BUFFER))) {
            /* Runtime/application guarantees no overlap with GPU-read bytes. */
            H=UmdLock(D,&R->Bo);if(SUCCEEDED(H))H=UmdInvalidate(D,&R->Bo);
        } else H=E_INVALIDARG;
    } else H=E_INVALIDARG;
    if(SUCCEEDED(H)) {
        R->MapType=Type;Out->pData=R->Bo.Cpu;
        Out->RowPitch=R->Desc.Kind==MALI_ALLOCATION_BUFFER?R->LogicalBytes:R->Desc.Pitch;
        Out->DepthPitch=R->LogicalBytes;(void)UmdReap(D);
    }
Done:
    if(FAILED(H) && H!=DXGI_DDI_ERR_WASSTILLDRAWING)UmdFatal(D,H);
    LeaveCriticalSection(&D->Gate);
    UmdError(D,H==DXGI_DDI_ERR_WASSTILLDRAWING?H:FAILED(H)?DXGI_ERROR_DEVICE_REMOVED:S_OK);
}
static VOID APIENTRY UmdUnmapResource(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Resource,UINT Subresource) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);UMD_RESOURCE *R=UmdFindResource(D,Resource.pDrvPrivate);
    HRESULT H=!R || Subresource || !R->MapType || !R->Bo.Locked?E_INVALIDARG:S_OK;
    if(SUCCEEDED(H)) {
        if(R->MapType!=D3D10_DDI_MAP_READ)H=UmdInvalidate(D,&R->Bo);
        if(SUCCEEDED(H))H=UmdUnlock(D,&R->Bo);
        if(SUCCEEDED(H))R->MapType=0;
    }
    if(FAILED(H))UmdFatal(D,H);
    LeaveCriticalSection(&D->Gate);UmdError(D,FAILED(H)?DXGI_ERROR_DEVICE_REMOVED:S_OK);
}
static HRESULT UmdRegion(UMD_RESOURCE *R,const D3D10_DDI_BOX *Box,UMD_REGION *Out) {
    UINT W=R->Desc.Kind==MALI_ALLOCATION_BUFFER?R->LogicalBytes:R->Desc.Width;
    UINT H=R->Desc.Kind==MALI_ALLOCATION_BUFFER?1:R->Desc.Height;
    UMD_REGION V={0,0,W,H,R->Desc.Kind==MALI_ALLOCATION_BUFFER?1u:4u};
    if(Box) {
        if(Box->left>=Box->right || Box->top>=Box->bottom || Box->front>=Box->back)return S_FALSE;
        if(Box->left<0 || Box->top<0 || (UINT)Box->right>W || (UINT)Box->bottom>H ||
           Box->front || Box->back!=1)return E_INVALIDARG;
        V.X=Box->left;V.Y=Box->top;V.Width=Box->right-Box->left;V.Height=Box->bottom-Box->top;
    }
    *Out=V;return S_OK;
}
static UINT UmdPitch(UMD_RESOURCE *R) {return R->Desc.Kind==MALI_ALLOCATION_BUFFER?R->LogicalBytes:R->Desc.Pitch;}
static HRESULT UmdWriteUp(UMD_DEVICE *D,UMD_RESOURCE *R,const UMD_REGION *V,const void *Source,UINT Pitch) {
    UINT Row=V->Width*V->PixelBytes;
    if(R->Desc.Kind==MALI_ALLOCATION_BUFFER)Pitch=Row;
    uint64_t Bytes=(uint64_t)(V->Height-1u)*Pitch+Row;
    if(!Source || Pitch<Row || Bytes>UINTPTR_MAX-(uintptr_t)Source)return E_INVALIDARG;
    HRESULT H=UmdCpuIdle(D,R,0);if(FAILED(H))return H;
    H=UmdLock(D,&R->Bo);if(FAILED(H))return H;
    H=UmdInvalidate(D,&R->Bo);
    if(SUCCEEDED(H)) {
        for(UINT Y=0;Y<V->Height;Y++)CopyMemory((unsigned char *)R->Bo.Cpu+(SIZE_T)(V->Y+Y)*UmdPitch(R)+V->X*V->PixelBytes,
            (const unsigned char *)Source+(SIZE_T)Y*Pitch,Row);
        H=UmdInvalidate(D,&R->Bo);
    }
    HRESULT Unlock=UmdUnlock(D,&R->Bo);return FAILED(H)?H:Unlock;
}
static VOID APIENTRY UmdUpdateResource(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Resource,UINT Subresource,
    const D3D10_DDI_BOX *Box,const void *Source,UINT Pitch,UINT DepthPitch) {
    (void)DepthPitch;UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);if(FAILED(H))goto Done;
    UMD_RESOURCE *R=UmdFindResource(D,Resource.pDrvPrivate);UMD_REGION V;
    if(!R || Subresource || R->MapType || R->Bo.Locked || R->Usage!=D3D10_DDI_USAGE_DEFAULT ||
       (Box && (R->BindFlags&D3D10_DDI_BIND_CONSTANT_BUFFER))){H=E_INVALIDARG;goto Done;}
    H=UmdRegion(R,Box,&V);
    if(H==S_OK)H=UmdWriteUp(D,R,&V,Source,Pitch);
Done:
    if(FAILED(H))UmdFatal(D,H);
    LeaveCriticalSection(&D->Gate);UmdError(D,FAILED(H)?DXGI_ERROR_DEVICE_REMOVED:S_OK);
}
static HRESULT UmdCopy(UMD_DEVICE *D,UMD_RESOURCE *Dst,UINT X,UINT Y,UINT Z,UMD_RESOURCE *Src,const D3D10_DDI_BOX *Box) {
    if(!Dst || !Src || Dst==Src || Dst->MapType || Src->MapType || Dst->Bo.Locked || Src->Bo.Locked ||
       Dst->Usage==D3D10_DDI_USAGE_IMMUTABLE || Dst->Desc.Kind!=Src->Desc.Kind || Dst->Format!=Src->Format || Z)return E_INVALIDARG;
    UMD_REGION V;HRESULT H=UmdRegion(Src,Box,&V);if(H!=S_OK)return H;
    UINT Width=Dst->Desc.Kind==MALI_ALLOCATION_BUFFER?Dst->LogicalBytes:Dst->Desc.Width;
    UINT Height=Dst->Desc.Kind==MALI_ALLOCATION_BUFFER?1:Dst->Desc.Height;
    if(X>Width || Y>Height || V.Width>Width-X || V.Height>Height-Y)return E_INVALIDARG;
    H=UmdCpuIdle(D,Src,0);if(FAILED(H))return H;
    H=UmdCpuIdle(D,Dst,0);if(FAILED(H))return H;
    H=UmdLock(D,&Src->Bo);if(FAILED(H))return H;
    H=UmdLock(D,&Dst->Bo);
    if(SUCCEEDED(H))H=UmdInvalidate(D,&Src->Bo);
    if(SUCCEEDED(H))H=UmdInvalidate(D,&Dst->Bo);
    if(SUCCEEDED(H)) {
        for(UINT Row=0;Row<V.Height;Row++)CopyMemory((unsigned char *)Dst->Bo.Cpu+(SIZE_T)(Y+Row)*UmdPitch(Dst)+X*V.PixelBytes,
            (const unsigned char *)Src->Bo.Cpu+(SIZE_T)(V.Y+Row)*UmdPitch(Src)+V.X*V.PixelBytes,V.Width*V.PixelBytes);
        H=UmdInvalidate(D,&Dst->Bo);
    }
    HRESULT U=UmdUnlock(D,&Dst->Bo);if(SUCCEEDED(H))H=U;
    U=UmdUnlock(D,&Src->Bo);return FAILED(H)?H:U;
}
static VOID APIENTRY UmdCopyRegion(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Dest,UINT DestSubresource,
    UINT X,UINT Y,UINT Z,D3D10DDI_HRESOURCE Source,UINT SourceSubresource,const D3D10_DDI_BOX *Box) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);
    if(SUCCEEDED(H))H=DestSubresource || SourceSubresource?E_INVALIDARG:
        UmdCopy(D,UmdFindResource(D,Dest.pDrvPrivate),X,Y,Z,UmdFindResource(D,Source.pDrvPrivate),Box);
    if(FAILED(H))UmdFatal(D,H);
    LeaveCriticalSection(&D->Gate);UmdError(D,FAILED(H)?DXGI_ERROR_DEVICE_REMOVED:S_OK);
}
static VOID APIENTRY UmdCopyResource(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Dest,D3D10DDI_HRESOURCE Source) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);
    if(SUCCEEDED(H)) {
        UMD_RESOURCE *Dst=UmdFindResource(D,Dest.pDrvPrivate),*Src=UmdFindResource(D,Source.pDrvPrivate);
        H=!Dst || !Src || Dst->LogicalBytes!=Src->LogicalBytes ||
            Dst->Desc.Width!=Src->Desc.Width || Dst->Desc.Height!=Src->Desc.Height?E_INVALIDARG:UmdCopy(D,Dst,0,0,0,Src,0);
    }
    if(FAILED(H))UmdFatal(D,H);
    LeaveCriticalSection(&D->Gate);UmdError(D,FAILED(H)?DXGI_ERROR_DEVICE_REMOVED:S_OK);
}
static BOOL APIENTRY UmdIsStagingBusy(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Resource) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return TRUE;
    EnterCriticalSection(&D->Gate);(void)UmdReap(D);UMD_RESOURCE *R=UmdFindResource(D,Resource.pDrvPrivate);
    BOOL Busy=!R || R->Usage!=D3D10_DDI_USAGE_STAGING || R->MapType || UmdGpuBusy(D,R);
    LeaveCriticalSection(&D->Gate);return Busy;
}
void MtkMaliUmdResourceFunctions(D3D10DDI_DEVICEFUNCS *F) {
    if(!F)return;
    F->pfnCalcPrivateResourceSize=UmdCalcResource;F->pfnCreateResource=UmdCreateResource;
    F->pfnDestroyResource=UmdDestroyResource;F->pfnCalcPrivateRenderTargetViewSize=UmdCalcView;
    F->pfnCreateRenderTargetView=UmdCreateView;F->pfnDestroyRenderTargetView=UmdDestroyView;
    F->pfnClearRenderTargetView=UmdClearView;F->pfnFlush=UmdFlush;
    F->pfnResourceCopy=UmdCopyResource;F->pfnResourceCopyRegion=UmdCopyRegion;
    F->pfnResourceUpdateSubresourceUP=UmdUpdateResource;
    F->pfnDefaultConstantBufferUpdateSubresourceUP=UmdUpdateResource;
    F->pfnStagingResourceMap=UmdMapResource;F->pfnStagingResourceUnmap=UmdUnmapResource;
    F->pfnDynamicIABufferMapDiscard=UmdMapResource;F->pfnDynamicIABufferMapNoOverwrite=UmdMapResource;
    F->pfnDynamicIABufferUnmap=UmdUnmapResource;
    F->pfnDynamicConstantBufferMapDiscard=UmdMapResource;F->pfnDynamicConstantBufferUnmap=UmdUnmapResource;
    F->pfnDynamicResourceMapDiscard=UmdMapResource;F->pfnDynamicResourceUnmap=UmdUnmapResource;
    F->pfnResourceIsStagingBusy=UmdIsStagingBusy;
    F->pfnDraw=UmdDraw;F->pfnIaSetTopology=UmdSetTopology;
    F->pfnIaSetVertexBuffers=UmdSetVertexBuffers;F->pfnIaSetIndexBuffer=UmdSetIndexBuffer;
    F->pfnIaSetInputLayout=UmdSetInputLayout;F->pfnSetRenderTargets=UmdSetRenderTargets;
    F->pfnSetBlendState=UmdSetBlendState;F->pfnSetDepthStencilState=UmdSetDepthState;
    F->pfnSetRasterizerState=UmdSetRasterizerState;F->pfnSetViewports=UmdSetViewports;
    F->pfnSetScissorRects=UmdSetScissors;
    F->pfnCalcPrivateRasterizerStateSize=UmdCalcRasterizerState;
    F->pfnCreateRasterizerState=UmdCreateRasterizerState;F->pfnDestroyRasterizerState=UmdDestroyRasterizerState;
    F->pfnCalcPrivateBlendStateSize=UmdCalcBlendState;
    F->pfnCreateBlendState=UmdCreateBlendState;F->pfnDestroyBlendState=UmdDestroyBlendState;
    F->pfnCalcPrivateDepthStencilStateSize=UmdCalcDepthState;
    F->pfnCreateDepthStencilState=UmdCreateDepthState;F->pfnDestroyDepthStencilState=UmdDestroyDepthState;
}


static HRESULT UmdShaderResult(int Status) {
    switch(Status) {
        case T70_SHADER_OK:return S_OK;
        case T70_SHADER_MEMORY:return E_OUTOFMEMORY;
        case T70_SHADER_INVALID:return E_INVALIDARG;
        case T70_SHADER_UNSUPPORTED:return DXGI_ERROR_UNSUPPORTED;
        default:return E_FAIL;
    }
}
static HRESULT UmdShaderSource(const UINT *Code,const D3D10DDIARG_STAGE_IO_SIGNATURES *Signatures,
    UINT Stage,SIZE_T *Bytes) {
    if(!Code || Stage>T70_SHADER_VERTEX || (Code[0]>>16)!=Stage || Code[1]<2 ||
        Code[1]>T70_SHADER_MAX_INPUT/4u)return E_INVALIDARG;
    UINT Major=(Code[0]>>4)&15u,Minor=Code[0]&15u;
    if(Major<4 || Major>5 || Minor>1 || (Major==5 && Minor))return DXGI_ERROR_UNSUPPORTED;
    if(Signatures && (Signatures->NumInputSignatureEntries>T70_SHADER_MAX_SIGNATURE ||
        Signatures->NumOutputSignatureEntries>T70_SHADER_MAX_SIGNATURE ||
        (Signatures->NumInputSignatureEntries && !Signatures->pInputSignature) ||
        (Signatures->NumOutputSignatureEntries && !Signatures->pOutputSignature)))return E_INVALIDARG;
    *Bytes=(SIZE_T)Code[1]*4u;return S_OK;
}
static SIZE_T APIENTRY UmdCalcShader(D3D10DDI_HDEVICE Handle,const UINT *Code,
    const D3D10DDIARG_STAGE_IO_SIGNATURES *Signatures) {
    UMD_DEVICE *D=UmdDevice(Handle);SIZE_T Bytes;
    if(!D || !Code || FAILED(UmdShaderSource(Code,Signatures,Code[0]>>16,&Bytes)))return (SIZE_T)-1;
    return sizeof(UMD_TOKEN);
}
static VOID UmdCreateShader(D3D10DDI_HDEVICE Handle,const UINT *Code,D3D10DDI_HSHADER Shader,
    const D3D10DDIARG_STAGE_IO_SIGNATURES *Signatures,UINT Stage) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    if(!Shader.pDrvPrivate){UmdError(D,E_INVALIDARG);return;}
    UMD_TOKEN *Token=Shader.pDrvPrivate;Token->Id=0;Token->Owner=D;
    SIZE_T Bytes=0;HRESULT H=UmdShaderSource(Code,Signatures,Stage,&Bytes);
    if(FAILED(H)){UmdError(D,H);return;}
    UINT Inputs=Signatures?Signatures->NumInputSignatureEntries:0;
    UINT Outputs=Signatures?Signatures->NumOutputSignatureEntries:0;
    EnterCriticalSection(&D->Gate);H=UmdActive(D);
    UMD_SHADER *S=0;struct t70_shader *Compiled=0;
    if(FAILED(H))goto Done;
    if(D->ShaderCount==UMD_LIMIT || D->NextId==UINT64_MAX){H=E_OUTOFMEMORY;goto Done;}
    S=UmdNew(sizeof(*S)+Bytes+(Inputs+Outputs)*sizeof(struct t70_shader_signature_entry));
    Compiled=UmdNew(sizeof(*Compiled));
    if(!S || !Compiled){H=E_OUTOFMEMORY;goto Done;}
    unsigned char *Tokens=(void *)(S+1);CopyMemory(Tokens,Code,Bytes);
    struct t70_shader_signature_entry *Entries=(void *)(Tokens+Bytes);
    S->Source=(struct t70_shader_token_request){.abi=T70_SHADER_ABI,.gpu_id=D->Caps.GpuId,.stage=Stage,
        .tokens=Tokens,.tokens_size=Bytes,.inputs=Inputs?Entries:0,.outputs=Outputs?Entries+Inputs:0,
        .input_count=Inputs,.output_count=Outputs};
    for(UINT I=0;I<Inputs+Outputs;I++) {
        const D3D10DDIARG_SIGNATURE_ENTRY *E=I<Inputs?&Signatures->pInputSignature[I]:
            &Signatures->pOutputSignature[I-Inputs];
        Entries[I]=(struct t70_shader_signature_entry){E->SystemValue,E->Register,E->Mask};
    }
    H=UmdShaderResult(t70_shader_compile_tokens(&S->Source,Compiled,0,0));
    if(SUCCEEDED(H)) {
        S->Id=++D->NextId;S->Next=D->Shaders;D->Shaders=S;++D->ShaderCount;
        Token->Id=S->Id;S=0;
    }
Done:
    /* Create errors do not get a runtime DestroyShader callback. */
    if(Compiled){t70_shader_free(Compiled);UmdFree(Compiled);}UmdFree(S);
    LeaveCriticalSection(&D->Gate);UmdError(D,H);
}
static VOID APIENTRY UmdCreateVertexShader(D3D10DDI_HDEVICE Handle,const UINT *Code,D3D10DDI_HSHADER Shader,
    D3D10DDI_HRTSHADER Runtime,const D3D10DDIARG_STAGE_IO_SIGNATURES *Signatures) {
    (void)Runtime;UmdCreateShader(Handle,Code,Shader,Signatures,T70_SHADER_VERTEX);
}
static VOID APIENTRY UmdCreatePixelShader(D3D10DDI_HDEVICE Handle,const UINT *Code,D3D10DDI_HSHADER Shader,
    D3D10DDI_HRTSHADER Runtime,const D3D10DDIARG_STAGE_IO_SIGNATURES *Signatures) {
    (void)Runtime;UmdCreateShader(Handle,Code,Shader,Signatures,T70_SHADER_PIXEL);
}
static UMD_SHADER *UmdFindShader(UMD_DEVICE *D,const void *Private) {
    if(!Private || ((const UMD_TOKEN *)Private)->Owner!=D)return 0;
    uint64_t Id=((const UMD_TOKEN *)Private)->Id;
    if(Id)for(UMD_SHADER *S=D->Shaders;S;S=S->Next)if(S->Id==Id)return S;
    return 0;
}
static VOID APIENTRY UmdDestroyShader(D3D10DDI_HDEVICE Handle,D3D10DDI_HSHADER Shader) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);UMD_SHADER *S=UmdFindShader(D,Shader.pDrvPrivate);
    HRESULT H=S?S_OK:E_INVALIDARG;
    if(S) {
        if(D->DrawVertex==S->Id || D->DrawPixel==S->Id)H=UmdDropDrawProgram(D);
        if(D->VertexShader==S)D->VertexShader=0;if(D->PixelShader==S)D->PixelShader=0;
        UMD_SHADER **Link=&D->Shaders;while(*Link!=S)Link=&(*Link)->Next;*Link=S->Next;
        --D->ShaderCount;UmdFree(S);ZeroMemory(Shader.pDrvPrivate,sizeof(UMD_TOKEN));
    }
    LeaveCriticalSection(&D->Gate);UmdError(D,H);
}
static VOID UmdSetShader(D3D10DDI_HDEVICE Handle,D3D10DDI_HSHADER Shader,UINT Stage) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);
    UMD_SHADER *S=UmdFindShader(D,Shader.pDrvPrivate);
    if(SUCCEEDED(H) && Shader.pDrvPrivate && (!S || S->Source.stage!=Stage))H=E_INVALIDARG;
    UINT Invalid=Stage==T70_SHADER_VERTEX?UMD_DRAW_INVALID_VERTEX_SHADER:UMD_DRAW_INVALID_PIXEL_SHADER;
    if(SUCCEEDED(H)){if(Stage==T70_SHADER_VERTEX)D->VertexShader=S;else D->PixelShader=S;D->DrawInvalidMask&=~Invalid;}
    else D->DrawInvalidMask|=Invalid;
    LeaveCriticalSection(&D->Gate);UmdError(D,H);
}
static VOID APIENTRY UmdVsSetShader(D3D10DDI_HDEVICE Handle,D3D10DDI_HSHADER Shader) {
    UmdSetShader(Handle,Shader,T70_SHADER_VERTEX);
}
static VOID APIENTRY UmdPsSetShader(D3D10DDI_HDEVICE Handle,D3D10DDI_HSHADER Shader) {
    UmdSetShader(Handle,Shader,T70_SHADER_PIXEL);
}
static HRESULT UmdCompileSelected(UMD_DEVICE *D,const struct t70_shader_io_types *Types,
    struct t70_shader_pipeline *Out) {
    HRESULT H=UmdActive(D);if(FAILED(H))return H;
    if(!D->VertexShader || !D->PixelShader)return E_INVALIDARG;
    struct t70_shader_token_request Vertex=D->VertexShader->Source,Pixel=D->PixelShader->Source;
    Vertex.types=Pixel.types=*Types;
    return UmdShaderResult(t70_shader_compile_token_pipeline(&Vertex,&Pixel,Out,0,0));
}
HRESULT MtkMaliUmdCompileBoundShaders(D3D10DDI_HDEVICE Handle,
    const struct t70_shader_io_types *Types,struct t70_shader_pipeline *Out) {
    UMD_DEVICE *D=UmdDevice(Handle);
    if(!D || !Types || !Out || Out->vertex.binary || Out->vertex.spirv || Out->pixel.binary || Out->pixel.spirv)return E_INVALIDARG;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdCompileSelected(D,Types,Out);
    LeaveCriticalSection(&D->Gate);return H;
}
/* Compiler metadata is not a GPU address. Each entry point must stay within
   the exact code, including eight-byte instruction and 48-bit VA limits. */
static int UmdShaderMetadata(const struct t70_shader *S,UINT Stage) {
    if(!S->binary || !S->binary_size || (S->binary_size&7u) || S->binary_size>UMD_TARGET_LIMIT ||
       S->stage!=Stage || S->arch!=9 || !S->work_registers || S->work_registers>64 || S->fau_count>510 ||
       (S->preload&UINT64_C(0x0000ffffffffffff)) ||
       ((S->flags&T70_SHADER_FLAG_FTZ16) && !(S->flags&T70_SHADER_FLAG_FTZ32)))return 0;
    if(Stage==T70_SHADER_PIXEL)return !(S->flags&(T70_SHADER_FLAG_IDVS|T70_SHADER_FLAG_SECONDARY));
    if(!(S->flags&T70_SHADER_FLAG_IDVS) || (S->no_point_size_offset&7u) ||
       S->no_point_size_offset>S->binary_size-8u)return 0;
    return !(S->flags&T70_SHADER_FLAG_SECONDARY) ||
        (!(S->secondary_offset&7u) && S->secondary_offset<=S->binary_size-8u &&
         S->secondary_registers && S->secondary_registers<=64 &&
         !(S->secondary_preload&UINT64_C(0x0000ffffffffffff)));
}
static HRESULT UmdPackProgram(UMD_DEVICE *D,const struct t70_shader *S,uint64_t Va,
    UINT Offset,UINT Secondary,void *Output) {
    MALI_COMMAND_SHADER Desc={sizeof(Desc),S->stage==T70_SHADER_VERTEX?3u:2u,
        Secondary?S->secondary_registers:S->work_registers,
        S->stage==T70_SHADER_VERTEX && !Secondary?1u:0u,
        S->flags&T70_SHADER_FLAG_FTZ32?(S->flags&T70_SHADER_FLAG_FTZ16?2u:1u):0u,
        S->stage==T70_SHADER_PIXEL && (S->flags&T70_SHADER_FLAG_BARRIER)?1u:0u,
        Va+Offset,Secondary?S->secondary_preload:S->preload};
    return MaliCommandsBuildShader(&D->Caps,&Desc,Output,MALI_COMMAND_SHADER_BYTES)==MaliOk?S_OK:E_UNEXPECTED;
}
/* Gate held. A supplied internal preload is consumed only after its owning
   program object is linked; failures before then leave it with the caller. */
static HRESULT UmdUploadSelected(UMD_DEVICE *D,
    const struct t70_shader_io_types *Types,struct t70_shader *Preload,MALI_UMD_PROGRAM *Out) {
    MALI_UMD_PROGRAM Empty={0};
    if(!D || !Types || !Out || Out->Id || Out->Owner || Out->Compiled ||
       Out->Code[0].Va || Out->Code[0].Bytes || Out->Code[0].Access ||
       Out->Code[1].Va || Out->Code[1].Bytes || Out->Code[1].Access ||
       Out->Descriptors.Va || Out->Descriptors.Bytes || Out->Descriptors.Access ||
       Out->Shader[0] || Out->Shader[1] || Out->Shader[2] || Out->Shader[3])return E_INVALIDARG;
    HRESULT H=UmdActive(D);
    if(FAILED(H))goto Done;
    if(Preload && !UmdShaderMetadata(Preload,T70_SHADER_PIXEL)){H=E_INVALIDARG;goto Done;}
    (void)UmdReap(D);
    if(!D->VertexShader || !D->PixelShader){H=E_INVALIDARG;goto Done;}
    if(D->ProgramCount==UMD_PROGRAM_LIMIT || D->NextId==UINT64_MAX){H=E_OUTOFMEMORY;goto Done;}
    UMD_PROGRAM *P=UmdNew(sizeof(*P));if(!P){H=E_OUTOFMEMORY;goto Done;}
    P->Id=++D->NextId;P->Next=D->Programs;D->Programs=P;++D->ProgramCount;
    if(Preload){P->Preload=*Preload;ZeroMemory(Preload,sizeof(*Preload));}
    H=UmdCompileSelected(D,Types,&P->Compiled);if(FAILED(H))goto Failed;
    if(!UmdShaderMetadata(&P->Compiled.vertex,T70_SHADER_VERTEX) ||
       !UmdShaderMetadata(&P->Compiled.pixel,T70_SHADER_PIXEL)){H=E_UNEXPECTED;goto Failed;}
    uint64_t Total=4096,Fence=0;
    UINT BoCount=P->Preload.binary?4u:3u;
    for(UINT N=0;N<BoCount;N++) {
        /* The optional RX code must exist before its immutable descriptor. */
        UINT I=BoCount==4u && N>=2u?(N==2u?3u:2u):N;
        const struct t70_shader *S=I==3u?&P->Preload:I?&P->Compiled.pixel:&P->Compiled.vertex;
        uint64_t Bytes=I==2?4096:(S->binary_size+4095u)&~UINT64_C(4095);
        if(I!=2)Total+=Bytes;
        if(Total>UMD_TARGET_LIMIT){H=E_OUTOFMEMORY;goto Failed;}
        MALI_ALLOCATION_DESC Desc={MALI_ALLOCATION_VERSION,sizeof(Desc),MALI_ALLOCATION_BUFFER,
            MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING,Bytes,0,0,0,0};
        H=UmdCreateBo(D,&P->Bo[I],&Desc,0,I==2?MaliPteRead:MaliPteRead|MaliPteExecute);if(FAILED(H))goto Failed;
        H=UmdLock(D,&P->Bo[I]);if(FAILED(H))goto Failed;
        ZeroMemory(P->Bo[I].Cpu,(SIZE_T)Bytes);
        if(I!=2)CopyMemory(P->Bo[I].Cpu,S->binary,S->binary_size);
        else {
            const struct t70_shader *V=&P->Compiled.vertex;uint8_t *Cpu=P->Bo[I].Cpu;
            H=UmdPackProgram(D,V,P->Bo[0].Va,0,0,Cpu);if(FAILED(H))goto Failed;
            H=UmdPackProgram(D,V,P->Bo[0].Va,V->no_point_size_offset,0,Cpu+32);if(FAILED(H))goto Failed;
            if(V->flags&T70_SHADER_FLAG_SECONDARY) {
                H=UmdPackProgram(D,V,P->Bo[0].Va,V->secondary_offset,1,Cpu+64);if(FAILED(H))goto Failed;
            }
            H=UmdPackProgram(D,&P->Compiled.pixel,P->Bo[1].Va,0,0,Cpu+96);if(FAILED(H))goto Failed;
            if(P->Preload.binary) {
                H=UmdPackProgram(D,&P->Preload,P->Bo[3].Va,0,0,Cpu+128);if(FAILED(H))goto Failed;
            }
        }
        H=UmdInvalidate(D,&P->Bo[I]);if(FAILED(H))goto Failed;
        H=UmdUnlock(D,&P->Bo[I]);if(FAILED(H))goto Failed;
        H=UmdResident(D,&P->Bo[I]);if(FAILED(H))goto Failed;
        if(P->Bo[I].PagingFence>Fence)Fence=P->Bo[I].PagingFence;
    }
    H=UmdWaitPaging(D,Fence);if(FAILED(H))goto Failed;
    P->Live=P->NeedsIdle=1;
    Empty.Id=P->Id;Empty.Owner=D;Empty.Compiled=&P->Compiled;
    for(UINT I=0;I<2;I++)Empty.Code[I]=(MALI_COMMAND_RESOURCE){P->Bo[I].Va,(uint32_t)P->Bo[I].Bytes,MaliPteRead|MaliPteExecute};
    Empty.Descriptors=(MALI_COMMAND_RESOURCE){P->Bo[2].Va,4096,MaliPteRead};
    for(UINT I=0;I<4;I++)Empty.Shader[I]=I==MaliUmdVarying && !(P->Compiled.vertex.flags&T70_SHADER_FLAG_SECONDARY)?0:P->Bo[2].Va+I*32u;
    *Out=Empty;goto Done;
Failed:
    (void)UmdReap(D);
Done:
    return H;
}
HRESULT MtkMaliUmdUploadBoundShaders(D3D10DDI_HDEVICE Handle,
    const struct t70_shader_io_types *Types,MALI_UMD_PROGRAM *Out) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D)return E_INVALIDARG;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdUploadSelected(D,Types,0,Out);
    LeaveCriticalSection(&D->Gate);return H;
}
HRESULT MtkMaliUmdReleaseProgram(D3D10DDI_HDEVICE Handle,MALI_UMD_PROGRAM *Program) {
    UMD_DEVICE *D=UmdDevice(Handle);if(!D || !Program || Program->Owner!=D || !Program->Id)return E_INVALIDARG;
    EnterCriticalSection(&D->Gate);UMD_PROGRAM *P=D->Programs;
    while(P && (!P->Live || P->Id!=Program->Id))P=P->Next;
    HRESULT H=E_INVALIDARG;
    if(P){P->Live=0;ZeroMemory(Program,sizeof(*Program));H=UmdReap(D);}
    LeaveCriticalSection(&D->Gate);return H;
}
void MtkMaliUmdShaderFunctions(D3D10DDI_DEVICEFUNCS *F) {
    if(!F)return;
    F->pfnCalcPrivateShaderSize=UmdCalcShader;F->pfnCreateVertexShader=UmdCreateVertexShader;
    F->pfnCreatePixelShader=UmdCreatePixelShader;F->pfnDestroyShader=UmdDestroyShader;
    F->pfnVsSetShader=UmdVsSetShader;F->pfnPsSetShader=UmdPsSetShader;
}

HRESULT MtkMaliUmdResourceRange(D3D10DDI_HDEVICE Handle,D3D10DDI_HRESOURCE Resource,
    UINT Access,MALI_COMMAND_RESOURCE *Out) {
    UMD_DEVICE *D=UmdDevice(Handle);
    if(!D || !Out || (Access!=MaliPteRead && Access!=(MaliPteRead|MaliPteWrite)))return E_INVALIDARG;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);
    if(SUCCEEDED(H)) {
        UMD_RESOURCE *R=UmdFindResource(D,Resource.pDrvPrivate);
        if(!R || R->Initializing || R->MapType || R->Bo.Locked)H=E_INVALIDARG;
        else *Out=(MALI_COMMAND_RESOURCE){R->Bo.Va,(uint32_t)R->Bo.Bytes,Access};
    }
    LeaveCriticalSection(&D->Gate);return H;
}
HRESULT MtkMaliUmdSubmitRaster(D3D10DDI_HDEVICE Handle,const MALI_UMD_PROGRAM *Program,
    const MALI_COMMAND_RASTER *Raster,const MALI_UMD_RASTER_RESOURCE *Resources,UINT Count) {
    UMD_DEVICE *D=UmdDevice(Handle);
    if(!D || !Program || Program->Owner!=D || !Program->Id || !Raster || !Resources ||
       !Count || Count>MALI_COMMAND_MAX_RESOURCES-3u || Raster->Bytes!=sizeof(*Raster))return E_INVALIDARG;
    EnterCriticalSection(&D->Gate);HRESULT H=UmdActive(D);UMD_PENDING *P=0;
    if(FAILED(H))goto Done;
    (void)UmdReap(D);
    UMD_PROGRAM *Code=D->Programs;
    while(Code && (!Code->Live || Code->Id!=Program->Id))Code=Code->Next;
    if(!Code){H=E_INVALIDARG;goto Done;}
    UINT CodeBos=Code->Preload.binary?4u:3u;
    if(Count>MALI_COMMAND_MAX_RESOURCES-CodeBos){H=E_INVALIDARG;goto Done;}
    if(D->PendingCount==UMD_PENDING_LIMIT){H=E_OUTOFMEMORY;goto Done;}
    P=UmdNew(sizeof(*P)+(SIZE_T)Count*sizeof(P->Resources[0]));
    if(!P){H=E_OUTOFMEMORY;goto Done;}
    P->Program=Code;++Code->Pending;P->Next=D->Pending;D->Pending=P;++D->PendingCount;
    for(UINT I=0;I<Count;I++) {
        UMD_RESOURCE *R=UmdFindResource(D,Resources[I].Resource.pDrvPrivate);
        UINT Access=Resources[I].Access;
        if(!R || R->Initializing || R->MapType || R->Bo.Locked ||
           (Access!=MaliPteRead && Access!=(MaliPteRead|MaliPteWrite))) {H=E_INVALIDARG;goto Failed;}
        for(UINT J=0;J<P->ResourceCount;J++)if(P->Resources[J]==R){H=E_INVALIDARG;goto Failed;}
        P->Resources[P->ResourceCount++]=R;++R->Pending;
    }
    P->Event=CreateEventW(0,TRUE,FALSE,0);if(!P->Event){H=E_OUTOFMEMORY;goto Failed;}
    MALI_ALLOCATION_DESC Desc={MALI_ALLOCATION_VERSION,sizeof(Desc),MALI_ALLOCATION_BUFFER,
        MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING,4096,0,0,0,0};
    H=UmdCreateBo(D,&P->Bo[0],&Desc,0,MaliPteRead|MaliPteWrite);if(FAILED(H))goto Failed;
    H=UmdLock(D,&P->Bo[0]);if(FAILED(H))goto Failed;
    ZeroMemory(P->Bo[0].Cpu,4096);
    /* Temporary input table lives beyond the 512 command bytes in the same
       CPU allocation; it is scrubbed before publication and never submitted. */
    MALI_COMMAND_RESOURCE *Ranges=(void *)((unsigned char *)P->Bo[0].Cpu+MALI_COMMAND_RASTER_JOB_BYTES);
    UINT64 Fence=0;
    for(UINT I=0;I<Count;I++) {
        UMD_BO *B=&P->Resources[I]->Bo;
        Ranges[I]=(MALI_COMMAND_RESOURCE){B->Va,(uint32_t)B->Bytes,Resources[I].Access};
        H=UmdResident(D,B);if(FAILED(H))goto Failed;
        if(B->PagingFence>Fence)Fence=B->PagingFence;
    }
    for(UINT I=0;I<CodeBos;I++) {
        UMD_BO *B=&Code->Bo[I];
        Ranges[Count+I]=(MALI_COMMAND_RESOURCE){B->Va,(uint32_t)B->Bytes,
            MaliPteRead|(I!=2?MaliPteExecute:0)};
        H=UmdResident(D,B);if(FAILED(H))goto Failed;
        if(B->PagingFence>Fence)Fence=B->PagingFence;
    }
    MALI_COMMAND_RASTER Draw=*Raster;
    Draw.Position.Shader=Code->Bo[2].Va+((Draw.Primitive&255u)==1?0:32u);
    Draw.Fragment.Shader=Code->Bo[2].Va+96u;
    Draw.Primitive&=~(1u<<18);Draw.VaryingStride=0;ZeroMemory(&Draw.Varying,sizeof(Draw.Varying));
    if(Code->Compiled.vertex.flags&T70_SHADER_FLAG_SECONDARY) {
        Draw.Primitive|=1u<<18;Draw.VaryingStride=Code->Compiled.varying_stride;
        Draw.Varying=Draw.Position;Draw.Varying.Shader=Code->Bo[2].Va+64u;
    }
    if(Draw.Position.FauCount!=(Code->Compiled.vertex.fau_count+1u)/2u ||
       Draw.Fragment.FauCount!=(Code->Compiled.pixel.fau_count+1u)/2u) {H=E_INVALIDARG;goto Failed;}
    UINT PrivateBytes=MALI_COMMAND_RASTER_PRIVATE_BYTES(Count+CodeBos);
    MALI_RESULT Result=MaliCommandsBuildRaster(&D->Caps,&Draw,Ranges,Count+CodeBos,
        P->Bo[0].Cpu,P->Bo[0].Va,MALI_COMMAND_RASTER_JOB_BYTES,P->Private,PrivateBytes);
    if(Result!=MaliOk){H=Result==MaliUnsupported?E_NOTIMPL:E_INVALIDARG;goto Failed;}
    ZeroMemory((unsigned char *)P->Bo[0].Cpu+MALI_COMMAND_RASTER_JOB_BYTES,4096-MALI_COMMAND_RASTER_JOB_BYTES);
    H=UmdInvalidate(D,&P->Bo[0]);if(FAILED(H))goto Failed;
    H=UmdUnlock(D,&P->Bo[0]);if(FAILED(H))goto Failed;
    H=UmdResident(D,&P->Bo[0]);if(FAILED(H))goto Failed;
    if(P->Bo[0].PagingFence>Fence)Fence=P->Bo[0].PagingFence;
    H=UmdWaitPaging(D,Fence);if(FAILED(H))goto Failed;
    D3DKMT_SUBMITCOMMAND Submit={0};Submit.Commands=P->Bo[0].Va;Submit.CommandLength=MALI_COMMAND_RASTER_JOB_BYTES;
    Submit.BroadcastContextCount=1;Submit.BroadcastContext[0]=D->Context;
    Submit.pPrivateDriverData=P->Private;Submit.PrivateDriverDataSize=PrivateBytes;
    NTSTATUS Status=D3DKMTSubmitCommand(&Submit);if(!UMD_NT_SUCCESS(Status)){H=UmdStatus(Status);goto Failed;}
    P->Submitted=1;H=UmdSignal(D,P);
    if(FAILED(H))UmdFatal(D,H);
    goto Done;
Failed:
    (void)UmdReap(D);
Done:
    LeaveCriticalSection(&D->Gate);return H;
}
