// SPDX-License-Identifier: BSD-2-Clause-Patent
// Direct3D 11/12 WARP execution, shader/readback validation and DXGI presentation.
// WARP executes on the CPU. This does not implement a Mali WDDM render driver.
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
static int DxPixelsValid(const void *Data,uint32_t Pitch,uint64_t Bytes,uint32_t Width,uint32_t Height) {
    if(!Data || !Width || !Height || Width>8192 || Height>8192 ||
       Pitch<(uint64_t)Width*4 || (Pitch&3) || (uint64_t)Pitch*Height>Bytes ||
       Bytes>128ULL*1024*1024) return 0;
    const uint8_t *P=(const uint8_t*)Data;
    for(uint32_t Y=0;Y<Height;Y++) for(uint32_t X=0;X<Width;X++) {
        const uint8_t *V=P+(size_t)Y*Pitch+(size_t)X*4;
        if(V[0]<63 || V[0]>65 || V[1]<127 || V[1]>129 ||
           V[2]<190 || V[2]>192 || V[3]!=255) return 0;
    }
    return 1;
}
#ifndef T70_DIRECTX_READBACK_TEST
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_4.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
extern "C" int _fltused=0;
#pragma function(memcpy,memset)
extern "C" void *memcpy(void *D,const void *S,size_t N) {
    volatile uint8_t *A=(volatile uint8_t*)D;const volatile uint8_t *B=(const volatile uint8_t*)S;
    while(N--)*A++=*B++;return D;
}
extern "C" void *memset(void *D,int V,size_t N) {
    volatile uint8_t *A=(volatile uint8_t*)D;while(N--)*A++=(uint8_t)V;return D;
}
template<class T> static void Drop(T *&P) { if(P){P->Release();P=nullptr;} }
static void Text(const char *S) {
    DWORD N=0,W=0;while(S[N])N++;
    HANDLE H=GetStdHandle(STD_OUTPUT_HANDLE);
    while(W<N) { DWORD Done=0;if(!WriteFile(H,S+W,N-W,&Done,nullptr)||!Done)break;W+=Done; }
}
static void Status(const char *Name,HRESULT R) {
    static const char Hex[]="0123456789abcdef";char Value[12]="0x00000000\n";
    for(unsigned I=0;I<8;I++)Value[9-I]=Hex[((uint32_t)R>>(I*4))&15];
    Text(Name);Text(SUCCEEDED(R)?": PASS ":": FAIL ");Text(Value);
}
static HRESULT Win32Failure(DWORD Error) { return Error?HRESULT_FROM_WIN32(Error):E_FAIL; }
static HRESULT Shaders(ID3DBlob **Vs,ID3DBlob **Ps) {
    const char V[]="float4 main(uint n:SV_VertexID):SV_Position { float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[n],0,1);}";
    const char P[]="float4 main():SV_Target {return float4(0.25,0.5,0.75,1);}";
    ID3DBlob *Error=nullptr;
    HRESULT R=D3DCompile(V,sizeof(V)-1,"T70-VS",nullptr,nullptr,"main","vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,0,Vs,&Error);Drop(Error);
    if(FAILED(R))return R;
    R=D3DCompile(P,sizeof(P)-1,"T70-PS",nullptr,nullptr,"main","ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS,0,Ps,&Error);Drop(Error);return R;
}
static bool WindowClosed;
static LRESULT CALLBACK WindowProc(HWND W,UINT M,WPARAM A,LPARAM B) {
    if(M==WM_CLOSE){DestroyWindow(W);return 0;}
    if(M==WM_DESTROY){WindowClosed=true;PostQuitMessage(0);return 0;}
    return DefWindowProcW(W,M,A,B);
}
static HWND OpenWindow() {
    WindowClosed=false;WNDCLASSW C={};C.lpfnWndProc=WindowProc;
    C.hInstance=GetModuleHandleW(nullptr);C.lpszClassName=L"T70DirectX";
    if(!RegisterClassW(&C) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return nullptr;
    return CreateWindowExW(0,C.lpszClassName,L"T70Pro DirectX - WARP CPU rendering",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,960,640,
        nullptr,nullptr,C.hInstance,nullptr);
}
static bool Pump() {
    MSG M;
    while(PeekMessageW(&M,nullptr,0,0,PM_REMOVE)){
        if(M.message==WM_QUIT){WindowClosed=true;break;}
        TranslateMessage(&M);DispatchMessageW(&M);
    }
    return !WindowClosed;
}
static HRESULT Swapchain(IDXGIFactory4 *F,IUnknown *Owner,HWND W,IDXGISwapChain3 **Out) {
    DXGI_SWAP_CHAIN_DESC1 D={};D.Width=640;D.Height=480;D.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    D.SampleDesc.Count=1;D.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    D.BufferCount=2;D.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;D.Scaling=DXGI_SCALING_STRETCH;
    IDXGISwapChain1 *S=nullptr;
    HRESULT R=F->CreateSwapChainForHwnd(Owner,W,&D,nullptr,nullptr,&S);
    if(SUCCEEDED(R))R=S->QueryInterface(IID_PPV_ARGS(Out));Drop(S);
    if(SUCCEEDED(R))R=F->MakeWindowAssociation(W,DXGI_MWA_NO_ALT_ENTER);
    return R;
}
struct DX11 {
    ID3D11Device *D=nullptr;ID3D11DeviceContext *C=nullptr;
    ID3D11VertexShader *V=nullptr;ID3D11PixelShader *P=nullptr;
    ID3D11RasterizerState *Raster=nullptr;ID3DBlob *Vs=nullptr,*Ps=nullptr;
    IDXGIFactory4 *F=nullptr;IDXGISwapChain3 *S=nullptr;
    ID3D11Texture2D *Target=nullptr,*Readback=nullptr;ID3D11RenderTargetView *View=nullptr;
    ~DX11() {
        if(C){C->ClearState();C->Flush();}
        Drop(View);Drop(Target);Drop(Readback);Drop(S);Drop(Raster);Drop(V);Drop(P);
        Drop(Vs);Drop(Ps);Drop(C);Drop(D);Drop(F);
    }
    HRESULT Create() {
        D3D_FEATURE_LEVEL Level,Levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        HRESULT R=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            Levels,2,D3D11_SDK_VERSION,&D,&Level,&C);
        if(FAILED(R))return R;
        R=CreateDXGIFactory2(0,IID_PPV_ARGS(&F));if(FAILED(R))return R;
        R=Shaders(&Vs,&Ps);if(FAILED(R))return R;
        R=D->CreateVertexShader(Vs->GetBufferPointer(),Vs->GetBufferSize(),nullptr,&V);if(FAILED(R))return R;
        R=D->CreatePixelShader(Ps->GetBufferPointer(),Ps->GetBufferSize(),nullptr,&P);if(FAILED(R))return R;
        D3D11_RASTERIZER_DESC Desc={};Desc.FillMode=D3D11_FILL_SOLID;
        Desc.CullMode=D3D11_CULL_NONE;Desc.DepthClipEnable=TRUE;
        return D->CreateRasterizerState(&Desc,&Raster);
    }
    void Draw(UINT Width,UINT Height) {
        D3D11_VIEWPORT Vp={0,0,(FLOAT)Width,(FLOAT)Height,0,1};FLOAT Black[4]={0,0,0,1};
        C->OMSetRenderTargets(1,&View,nullptr);C->ClearRenderTargetView(View,Black);
        C->RSSetState(Raster);C->RSSetViewports(1,&Vp);
        C->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        C->VSSetShader(V,nullptr,0);C->PSSetShader(P,nullptr,0);C->Draw(3,0);
    }
    HRESULT Check() {
        D3D11_TEXTURE2D_DESC T={};T.Width=T.Height=64;T.MipLevels=T.ArraySize=1;
        T.Format=DXGI_FORMAT_R8G8B8A8_UNORM;T.SampleDesc.Count=1;
        T.Usage=D3D11_USAGE_DEFAULT;T.BindFlags=D3D11_BIND_RENDER_TARGET;
        HRESULT R=D->CreateTexture2D(&T,nullptr,&Target);if(FAILED(R))return R;
        R=D->CreateRenderTargetView(Target,nullptr,&View);if(FAILED(R))return R;
        Draw(64,64);
        T.Usage=D3D11_USAGE_STAGING;T.BindFlags=0;T.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        R=D->CreateTexture2D(&T,nullptr,&Readback);if(FAILED(R))return R;
        C->CopyResource(Readback,Target);D3D11_MAPPED_SUBRESOURCE M={};
        R=C->Map(Readback,0,D3D11_MAP_READ,0,&M);if(FAILED(R))return R;
        bool Ok=DxPixelsValid(M.pData,M.RowPitch,(uint64_t)M.RowPitch*64,64,64)!=0;
        C->Unmap(Readback,0);return Ok?S_OK:E_FAIL;
    }
    HRESULT Show(HWND W) {
        C->OMSetRenderTargets(0,nullptr,nullptr);Drop(View);Drop(Target);
        HRESULT R=Swapchain(F,D,W,&S);if(FAILED(R))return R;
        R=S->GetBuffer(0,IID_PPV_ARGS(&Target));if(FAILED(R))return R;
        R=D->CreateRenderTargetView(Target,nullptr,&View);if(FAILED(R))return R;
        ULONGLONG End=GetTickCount64()+10000;
        while(GetTickCount64()<End&&Pump()) {
            Draw(640,480);R=S->Present(1,0);if(FAILED(R))break;
        }
        return R;
    }
};
struct DX12 {
    IDXGIFactory4 *F=nullptr;IDXGIAdapter *A=nullptr;IDXGISwapChain3 *S=nullptr;
    ID3D12Device *D=nullptr;ID3D12CommandQueue *Q=nullptr;
    ID3D12CommandAllocator *Allocator=nullptr;ID3D12GraphicsCommandList *List=nullptr;
    ID3D12DescriptorHeap *Heap=nullptr;ID3D12RootSignature *Root=nullptr;
    ID3D12PipelineState *Pipeline=nullptr;ID3D12Fence *Fence=nullptr;
    ID3D12Resource *Target=nullptr,*Readback=nullptr,*Buffers[2]={nullptr,nullptr};
    ID3DBlob *Vs=nullptr,*Ps=nullptr,*Serialized=nullptr;
    HANDLE Event=nullptr;UINT64 FenceValue=0;bool InFlight=false;
    ~DX12() {
        // A timeout does not prove completion. Keep all submitted resources
        // alive until Windows tears down the process/device; never reuse them.
        if(InFlight)return;
        Drop(Buffers[0]);Drop(Buffers[1]);Drop(Target);Drop(Readback);Drop(S);
        Drop(List);Drop(Allocator);Drop(Heap);Drop(Pipeline);Drop(Root);
        Drop(Vs);Drop(Ps);Drop(Serialized);Drop(Fence);Drop(Q);Drop(D);Drop(A);Drop(F);
        if(Event)CloseHandle(Event);
    }
    HRESULT Wait() {
        UINT64 Value=++FenceValue;
        HRESULT R=Q->Signal(Fence,Value);if(FAILED(R))return R;
        UINT64 Complete=Fence->GetCompletedValue();
        if(Complete==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(Complete<Value) {
            R=Fence->SetEventOnCompletion(Value,Event);if(FAILED(R))return R;
            DWORD W=WaitForSingleObject(Event,10000);
            if(W!=WAIT_OBJECT_0)return Win32Failure(W==WAIT_FAILED?GetLastError():ERROR_TIMEOUT);
            Complete=Fence->GetCompletedValue();
            if(Complete==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
            if(Complete<Value)return E_UNEXPECTED;
        }
        InFlight=false;return S_OK;
    }
    HRESULT Create() {
        HRESULT R=CreateDXGIFactory2(0,IID_PPV_ARGS(&F));if(FAILED(R))return R;
        R=F->EnumWarpAdapter(IID_PPV_ARGS(&A));if(FAILED(R))return R;
        R=D3D12CreateDevice(A,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&D));if(FAILED(R))return R;
        D3D12_COMMAND_QUEUE_DESC Qd={};Qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        R=D->CreateCommandQueue(&Qd,IID_PPV_ARGS(&Q));if(FAILED(R))return R;
        R=D->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&Allocator));if(FAILED(R))return R;
        R=D->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&Fence));if(FAILED(R))return R;
        Event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!Event)return Win32Failure(GetLastError());
        D3D12_DESCRIPTOR_HEAP_DESC Hd={};Hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;Hd.NumDescriptors=2;
        R=D->CreateDescriptorHeap(&Hd,IID_PPV_ARGS(&Heap));if(FAILED(R))return R;
        D3D12_ROOT_SIGNATURE_DESC Rd={};Rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob *Error=nullptr;
        R=D3D12SerializeRootSignature(&Rd,D3D_ROOT_SIGNATURE_VERSION_1,&Serialized,&Error);Drop(Error);
        if(FAILED(R))return R;
        R=D->CreateRootSignature(0,Serialized->GetBufferPointer(),Serialized->GetBufferSize(),IID_PPV_ARGS(&Root));if(FAILED(R))return R;
        R=Shaders(&Vs,&Ps);if(FAILED(R))return R;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC Pd={};Pd.pRootSignature=Root;
        Pd.VS={Vs->GetBufferPointer(),Vs->GetBufferSize()};Pd.PS={Ps->GetBufferPointer(),Ps->GetBufferSize()};
        Pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;Pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        Pd.RasterizerState.DepthClipEnable=TRUE;Pd.SampleMask=UINT_MAX;
        Pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        Pd.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_ONE;Pd.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_ZERO;
        Pd.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_ADD;
        Pd.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;Pd.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_ZERO;
        Pd.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;Pd.BlendState.RenderTarget[0].LogicOp=D3D12_LOGIC_OP_NOOP;
        Pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;Pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        Pd.DepthStencilState.StencilReadMask=Pd.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_READ_MASK;
        Pd.DepthStencilState.FrontFace.StencilFailOp=Pd.DepthStencilState.FrontFace.StencilDepthFailOp=
            Pd.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_KEEP;
        Pd.DepthStencilState.FrontFace.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        Pd.DepthStencilState.BackFace=Pd.DepthStencilState.FrontFace;
        Pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;Pd.NumRenderTargets=1;
        Pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;Pd.SampleDesc.Count=1;
        R=D->CreateGraphicsPipelineState(&Pd,IID_PPV_ARGS(&Pipeline));if(FAILED(R))return R;
        return D->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,Allocator,Pipeline,IID_PPV_ARGS(&List));
    }
    void Draw(ID3D12Resource *Resource,D3D12_CPU_DESCRIPTOR_HANDLE Handle,UINT W,UINT H,D3D12_RESOURCE_STATES Before) {
        D3D12_RESOURCE_BARRIER B={};B.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        B.Transition.pResource=Resource;B.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        B.Transition.StateBefore=Before;B.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
        if(Before!=D3D12_RESOURCE_STATE_RENDER_TARGET)List->ResourceBarrier(1,&B);
        D3D12_VIEWPORT Vp={0,0,(FLOAT)W,(FLOAT)H,0,1};D3D12_RECT Sc={0,0,(LONG)W,(LONG)H};
        FLOAT Black[4]={0,0,0,1};List->SetGraphicsRootSignature(Root);
        List->RSSetViewports(1,&Vp);List->RSSetScissorRects(1,&Sc);
        List->OMSetRenderTargets(1,&Handle,FALSE,nullptr);List->ClearRenderTargetView(Handle,Black,0,nullptr);
        List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);List->DrawInstanced(3,1,0,0);
    }
    HRESULT Submit() {
        HRESULT R=List->Close();if(FAILED(R))return R;
        ID3D12CommandList *Commands[]={List};InFlight=true;Q->ExecuteCommandLists(1,Commands);
        return Wait();
    }
    HRESULT Check() {
        D3D12_HEAP_PROPERTIES Hp={};Hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC T={};T.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        T.Width=T.Height=64;T.DepthOrArraySize=T.MipLevels=1;T.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        T.SampleDesc.Count=1;T.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        HRESULT R=D->CreateCommittedResource(&Hp,D3D12_HEAP_FLAG_NONE,&T,D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,IID_PPV_ARGS(&Target));if(FAILED(R))return R;
        D3D12_CPU_DESCRIPTOR_HANDLE Handle=Heap->GetCPUDescriptorHandleForHeapStart();
        D->CreateRenderTargetView(Target,nullptr,Handle);Draw(Target,Handle,64,64,D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT Foot={};UINT Rows=0;UINT64 RowBytes=0,Total=0;
        D->GetCopyableFootprints(&T,0,1,0,&Foot,&Rows,&RowBytes,&Total);
        if(Rows!=64 || RowBytes!=256 || Foot.Footprint.Width!=64 || Foot.Footprint.Height!=64 ||
           Foot.Footprint.Depth!=1 || Foot.Footprint.RowPitch<256 || Foot.Offset>Total ||
           Total>16U*1024*1024 || (UINT64)Foot.Footprint.RowPitch*64>Total-Foot.Offset)return E_UNEXPECTED;
        D3D12_RESOURCE_DESC B={};B.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;B.Width=Total;
        B.Height=B.DepthOrArraySize=B.MipLevels=1;B.SampleDesc.Count=1;B.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hp.Type=D3D12_HEAP_TYPE_READBACK;
        R=D->CreateCommittedResource(&Hp,D3D12_HEAP_FLAG_NONE,&B,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&Readback));if(FAILED(R))return R;
        D3D12_RESOURCE_BARRIER Barrier={};Barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource=Target;Barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;Barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        List->ResourceBarrier(1,&Barrier);
        D3D12_TEXTURE_COPY_LOCATION Source={},Dest={};Source.pResource=Target;Source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dest.pResource=Readback;Dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;Dest.PlacedFootprint=Foot;
        List->CopyTextureRegion(&Dest,0,0,0,&Source,nullptr);R=Submit();if(FAILED(R))return R;
        void *Data=nullptr;D3D12_RANGE Range={0,(SIZE_T)Total};
        R=Readback->Map(0,&Range,&Data);if(FAILED(R))return R;
        bool Ok=Data && DxPixelsValid((uint8_t*)Data+Foot.Offset,Foot.Footprint.RowPitch,Total-Foot.Offset,64,64)!=0;
        D3D12_RANGE Written={0,0};Readback->Unmap(0,&Written);return Ok?S_OK:E_FAIL;
    }
    HRESULT Show(HWND W) {
        HRESULT R=Swapchain(F,Q,W,&S);if(FAILED(R))return R;
        auto First=Heap->GetCPUDescriptorHandleForHeapStart();UINT Step=D->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for(UINT I=0;I<2;I++){R=S->GetBuffer(I,IID_PPV_ARGS(&Buffers[I]));if(FAILED(R))return R;
            D3D12_CPU_DESCRIPTOR_HANDLE H={First.ptr+(SIZE_T)I*Step};D->CreateRenderTargetView(Buffers[I],nullptr,H);}
        ULONGLONG End=GetTickCount64()+10000;
        while(GetTickCount64()<End&&Pump()) {
            UINT I=S->GetCurrentBackBufferIndex();if(I>=2)return E_UNEXPECTED;
            R=Allocator->Reset();if(FAILED(R))return R;R=List->Reset(Allocator,Pipeline);if(FAILED(R))return R;
            D3D12_CPU_DESCRIPTOR_HANDLE H={First.ptr+(SIZE_T)I*Step};Draw(Buffers[I],H,640,480,D3D12_RESOURCE_STATE_PRESENT);
            D3D12_RESOURCE_BARRIER B={};B.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            B.Transition.pResource=Buffers[I];B.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            B.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;B.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
            List->ResourceBarrier(1,&B);
            R=List->Close();if(FAILED(R))return R;
            ID3D12CommandList *Commands[]={List};InFlight=true;Q->ExecuteCommandLists(1,Commands);
            HRESULT Present=S->Present(1,0);R=Wait();if(FAILED(R))return R;if(FAILED(Present))return Present;
        }
        return S_OK;
    }
};
static bool Options(bool *Show11,bool *Show12) {
    const WCHAR *P=GetCommandLineW();*Show11=*Show12=false;
    if(*P==L'"'){P++;while(*P&&*P!=L'"')P++;if(*P)P++;}
    else while(*P&&*P!=L' '&&*P!=L'\t')P++;
    while(*P==L' '||*P==L'\t')P++;
    if(!*P)return true;
    const WCHAR *A=L"--show11",*B=L"--show12";const WCHAR *Q=P;
    while(*P&&*A&&*P==*A){P++;A++;}
    if(!*A&&(!*P||*P==L' '||*P==L'\t'))*Show11=true;
    else {P=Q;while(*P&&*B&&*P==*B){P++;B++;}if(!*B&&(!*P||*P==L' '||*P==L'\t'))*Show12=true;else return false;}
    while(*P==L' '||*P==L'\t')P++;return !*P;
}
extern "C" void Entry() {
    bool Show11=false,Show12=false;
    if(!Options(&Show11,&Show12)){Text("Usage: T70DirectX.exe [--show11|--show12]\n");ExitProcess(64);}
    Text("T70 DirectX: WARP CPU rendering; no Mali hardware acceleration.\n");
    HRESULT R11,R12;
    {
        DX11 G;R11=G.Create();if(SUCCEEDED(R11))R11=G.Check();
        if(SUCCEEDED(R11)&&Show11){HWND W=OpenWindow();R11=W?G.Show(W):Win32Failure(GetLastError());if(W&&!WindowClosed)DestroyWindow(W);}
    }
    Status("D3D11 shader/texture/readback/DXGI",R11);
    {
        DX12 G;R12=G.Create();if(SUCCEEDED(R12))R12=G.Check();
        if(SUCCEEDED(R12)&&Show12){HWND W=OpenWindow();R12=W?G.Show(W):Win32Failure(GetLastError());if(W&&!WindowClosed&&!G.InFlight)DestroyWindow(W);}
        if(G.InFlight)Text("D3D12 completion unconfirmed; submitted resources retained until process exit.\n");
    }
    Status("D3D12 shader/queue/fence/readback/DXGI",R12);
    ExitProcess(FAILED(R11)||FAILED(R12)?1:0);
}
#endif
