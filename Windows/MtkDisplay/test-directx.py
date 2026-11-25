#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the actual ARM64 app with strict Win32/COM mocks, never a D3D runtime."""
from pathlib import Path
import hashlib,json,re,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
import unicorn.arm64_const as ar
root=Path(__file__).resolve().parents[2];w=root/'Windows';exe=w/'Build/T70DirectX.exe'
um=w/'Toolchain/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0/um'
headers={p.name.lower():p for d in [um,um.parent/'shared'] for p in d.iterdir() if p.is_file()}
def methods(interface,file):
    s=headers[file.lower()].read_text().split('typedef struct '+interface+'Vtbl',1)[1].split('END_INTERFACE',1)[0]
    # Windows C++ ABI has one slot for each method; headers contain two
    # platform branches for methods returning a structure.
    return list(dict.fromkeys(re.findall(r'\(\s*STDMETHODCALLTYPE\s*\*(\w+)\s*\)',s)))
vtables={n:methods(n,h) for n,h in [
    ('ID3D11Device','d3d11.h'),('ID3D11DeviceContext','d3d11.h'),
    ('ID3D12Device','d3d12.h'),('ID3D12GraphicsCommandList','d3d12.h'),
    ('ID3D12CommandQueue','d3d12.h'),('ID3D12CommandAllocator','d3d12.h'),
    ('ID3D12DescriptorHeap','d3d12.h'),('ID3D12Fence','d3d12.h'),('ID3D12Resource','d3d12.h'),
    ('IDXGIFactory4','dxgi1_4.h'),('IDXGISwapChain3','dxgi1_4.h')]}
vtables['Blob']=['QueryInterface','AddRef','Release','GetBufferPointer','GetBufferSize']
vtables['Plain']=['QueryInterface','AddRef','Release']
class Run:
    def __init__(self,case='',option=''):
        self.case=case;self.calls={};self.objects={};self.callbacks={};self.output='';self.exit=None
        self.next=0x20000000;self.trap=0x60000000;self.ticks=0;self.completed=0;self.signalled=0
        self.executed=False;self.waited=False;self.event_open=False;self.releases_after_submit=0
        self.resource_states={};self.rtv={};self.current_target=None;self.drawn=set();self.last11=None
        self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.u.mem_map(self.next,0x2000000)
        self.u.mem_map(self.trap,0x100000);self.u.mem_map(0x70000000,0x200000)
        data=exe.read_bytes();pe=struct.unpack_from('<I',data,0x3c)[0];op=pe+24
        count=struct.unpack_from('<H',data,pe+6)[0];optsize=struct.unpack_from('<H',data,pe+20)[0]
        self.base=struct.unpack_from('<Q',data,op+24)[0];size,head=struct.unpack_from('<II',data,op+56)
        self.u.mem_map(self.base,(size+4095)&~4095);self.u.mem_write(self.base,data[:head])
        for i in range(count):
            off=op+optsize+i*40;vs,rva,rawlen,raw=struct.unpack_from('<IIII',data,off+8)
            assert rva+max(vs,rawlen)<=size
            if rawlen:self.u.mem_write(self.base+rva,data[raw:raw+rawlen])
        desc=self.base+struct.unpack_from('<I',data,op+120)[0]
        while self.i(desc+12):
            dll=self.string(self.base+self.i(desc+12)).lower();names=self.base+self.i(desc);iat=self.base+self.i(desc+16);i=0
            while (name:=self.q(names+i*8)):
                symbol='D3D12CreateDevice' if name>>63 and dll=='d3d12.dll' and (name&65535)==101 else self.string(self.base+name+2) if not name>>63 else 'forbidden-ordinal'
                self.putq(iat+i*8,self.hook(lambda symbol=symbol:self.api(symbol)));i+=1
            desc+=20
        self.command=self.alloc(('"T70DirectX.exe" '+option).encode('utf-16-le')+b'\0\0')
        self.u.hook_add(UC_HOOK_CODE,self.dispatch,begin=self.trap,end=self.trap+0xfffff)
        self.u.reg_write(ar.UC_ARM64_REG_SP,0x701ffff0);self.u.reg_write(ar.UC_ARM64_REG_X30,self.trap+0xffff0)
        entry=self.base+struct.unpack_from('<I',data,op+16)[0]
        self.u.emu_start(entry,self.trap+0xffff0,timeout=15000000,count=20000000)
        assert self.exit is not None,(case,'entry did not ExitProcess')
        failed=bool(case and case!='event-success') or bool(option and option not in ['--show11','--show12'])
        assert self.exit==(64 if option=='--bad' else 1 if failed else 0),(case,self.exit,self.output)
        if option!='--bad':
            assert ('D3D11 shader/texture/readback/DXGI: ' in self.output and 'D3D12 shader/queue/fence/readback/DXGI: ' in self.output),self.output
        keep=case in ['signal','event-fail','timeout','wait-fail','wait-zero','incomplete','removed']
        if keep:
            assert self.executed and not self.waited and self.releases_after_submit==0
            assert self.event_open
        else:
            assert all(o['refs']==0 for o in self.objects.values()),[(o['kind'],o['refs']) for o in self.objects.values() if o['refs']]
            assert not self.event_open
        if not failed:
            assert self.calls.get('ID3D11DeviceContext.Draw',0)>=1 and self.calls.get('ID3D12GraphicsCommandList.DrawInstanced',0)>=1
            assert self.calls.get('ID3D12Resource.Map',0)==1 and self.waited
        if not failed and option in ['--show11','--show12']:
            assert self.calls.get('IDXGISwapChain3.Present',0)>=1
        if keep or case=='bad-footprint':assert not self.calls.get('ID3D12Resource.Map',0)
    def alloc(self,data):
        a=self.next;self.next+=(len(data)+15)&~15;assert self.next<0x22000000
        self.u.mem_write(a,bytes(data));return a
    def i(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
    def q(self,a):return struct.unpack('<Q',self.u.mem_read(a,8))[0]
    def putq(self,a,v):self.u.mem_write(a,struct.pack('<Q',v))
    def string(self,a):
        b=bytearray()
        while (c:=bytes(self.u.mem_read(a+len(b),1)))!=b'\0':b+=c
        return b.decode()
    def arg(self,n):return self.u.reg_read(getattr(ar,'UC_ARM64_REG_X'+str(n))) if n<8 else self.q(self.u.reg_read(ar.UC_ARM64_REG_SP)+(n-8)*8)
    def hook(self,f):
        p=self.trap+4+len(self.callbacks)*4;self.callbacks[p]=f;return p
    def dispatch(self,u,address,size,unused):
        assert address in self.callbacks,hex(address)
        result=self.callbacks[address]()
        if self.exit is None:
            u.reg_write(ar.UC_ARM64_REG_X0,0 if result is None else result&0xffffffffffffffff)
            u.reg_write(ar.UC_ARM64_REG_PC,u.reg_read(ar.UC_ARM64_REG_X30))
    def called(self,name):
        self.calls[name]=self.calls.get(name,0)+1
        return self.case==name+'#'+str(self.calls[name])
    def obj(self,kind,**fields):
        o={'kind':kind,'refs':1,**fields};a=self.alloc(bytes(16));self.objects[a]=o
        table=self.alloc(b''.join(struct.pack('<Q',self.hook(lambda a=a,n=n:self.com(a,n))) for n in vtables[kind]))
        self.putq(a,table);return a
    def out(self,arg,kind,**fields):self.putq(self.arg(arg),self.obj(kind,**fields));return 0
    def blob(self,out):return self.out(out,'Blob',data=self.alloc(b'mocked-bytecode'),length=15)
    def api(self,n):
        fail=self.called(n)
        if n=='ExitProcess':self.exit=self.arg(0);self.u.emu_stop();return
        if n=='GetCommandLineW':return self.command
        if n=='GetStdHandle':return 1
        if n=='WriteFile':
            self.output+=bytes(self.u.mem_read(self.arg(1),self.arg(2))).decode();self.u.mem_write(self.arg(3),struct.pack('<I',self.arg(2)));return 1
        if n=='GetLastError':return 0 if self.case in ['window-zero','event-zero','wait-zero'] else 5
        if n=='GetModuleHandleW':return 0x100
        if n=='GetTickCount64':self.ticks+=4000;return self.ticks
        if n=='RegisterClassW':return 1
        if n=='CreateWindowExW':return 0 if fail or self.case=='window-zero' else 0x123
        if n=='PeekMessageW':return 0
        if n in ['DefWindowProcW','PostQuitMessage','TranslateMessage','DispatchMessageW']:return 0
        if n=='DestroyWindow':return 1
        if n=='CreateEventW':
            if fail or self.case=='event-zero':return 0
            self.event_open=True;return 0x456
        if n=='CloseHandle':assert self.event_open;self.event_open=False;return 1
        if n=='WaitForSingleObject':
            assert self.executed and self.arg(1)==10000
            if self.case=='timeout':return 258
            if self.case in ['wait-fail','wait-zero']:return 0xffffffff
            if self.case!='incomplete':self.completed=self.signalled
            return 0
        if fail:return 0x80004005
        if n=='D3D11CreateDevice':
            assert self.arg(0)==0 and self.arg(1)==5 and self.arg(3)==32 and self.arg(5)==2 and self.arg(6)==7
            self.putq(self.arg(7),self.obj('ID3D11Device'));self.u.mem_write(self.arg(8),struct.pack('<I',0xb000));self.putq(self.arg(9),self.obj('ID3D11DeviceContext'));return 0
        if n=='CreateDXGIFactory2':return self.out(2,'IDXGIFactory4')
        if n=='D3D12CreateDevice':assert self.arg(0) in self.objects and self.arg(1)==0xb000;return self.out(3,'ID3D12Device')
        if n=='D3DCompile':
            code=bytes(self.u.mem_read(self.arg(0),self.arg(1)));target=self.string(self.arg(6));assert self.string(self.arg(5))=='main'
            assert (target=='vs_5_0' and b'SV_VertexID' in code) or (target=='ps_5_0' and b'SV_Target' in code)
            return self.blob(9)
        if n=='D3D12SerializeRootSignature':return self.blob(2)
        raise AssertionError('Unexpected Win32 import '+n)
    def com(self,a,n):
        o=self.objects[a];kind=o['kind'];name=kind+'.'+n;fail=self.called(name);assert self.arg(0)==a and o['refs']>0,(name,hex(self.arg(0)),hex(a))
        if n=='Release':
            if self.executed and not self.waited:self.releases_after_submit+=1
            o['refs']-=1;return o['refs']
        if n=='QueryInterface':
            if fail:return 0x80004005
            o['refs']+=1;self.putq(self.arg(2),a);return 0
        if n=='GetBufferPointer':return o['data']
        if n=='GetBufferSize':return o['length']
        if fail:return 0x80004005
        if kind=='IDXGIFactory4':
            if n=='EnumWarpAdapter':return self.out(2,'Plain')
            if n=='MakeWindowAssociation':assert self.arg(2)==2;return 0
            if n=='CreateSwapChainForHwnd':
                d=self.arg(3);assert self.i(d)==640 and self.i(d+4)==480 and self.i(d+8)==28 and self.i(d+28)==2 and self.i(d+36)==4
                return self.out(6,'IDXGISwapChain3')
        if kind=='IDXGISwapChain3':
            if n=='GetBuffer':
                is12=self.calls.get('ID3D12Device.CreateCommandQueue',0)>0
                return self.out(3,'ID3D12Resource' if is12 else 'Plain',state=0)
            if n=='GetCurrentBackBufferIndex':return self.calls.get(name,1)%2
            if n=='Present':assert self.arg(1)==1;return 0
        if kind=='ID3D11Device':
            if n=='CreateTexture2D':
                d=self.arg(1);assert self.i(d)==64 and self.i(d+4)==64 and self.i(d+16)==28
                return self.out(3,'Plain',staging=self.i(d+28)==3)
            if n in ['CreateVertexShader','CreatePixelShader']:return self.out(4,'Plain')
            if n=='CreateRenderTargetView':return self.out(3,'Plain',target=self.arg(1))
            if n=='CreateRasterizerState':assert self.i(self.arg(1)+4)==1;return self.out(2,'Plain')
        if kind=='ID3D11DeviceContext':
            if n=='OMSetRenderTargets':
                if self.arg(1):self.current_target=self.objects[self.q(self.arg(2))]['target']
                return 0
            if n=='Draw':assert self.arg(1)==3;self.drawn.add(self.current_target);return 0
            if n=='CopyResource':assert self.arg(2) in self.drawn;self.last11=self.arg(1);return 0
            if n=='Map':
                assert self.arg(1)==self.last11 and self.arg(3)==1
                data=bytearray(bytes([64,128,191,255])*64*64)
                if self.case=='bad-pixel11':data[-1]=0
                self.u.mem_write(self.arg(5),struct.pack('<QII',self.alloc(data),256,len(data)));return 0
            if n in ['Unmap','RSSetState','RSSetViewports','IASetPrimitiveTopology','VSSetShader','PSSetShader','ClearRenderTargetView','ClearState','Flush']:return 0
        if kind=='ID3D12Device':
            if n=='CreateCommandQueue':assert self.i(self.arg(1))==0;return self.out(3,'ID3D12CommandQueue')
            if n=='CreateCommandAllocator':return self.out(3,'ID3D12CommandAllocator')
            if n=='CreateFence':return self.out(4,'ID3D12Fence')
            if n=='CreateDescriptorHeap':assert self.i(self.arg(1))==2 and self.i(self.arg(1)+4)==2;return self.out(3,'ID3D12DescriptorHeap')
            if n=='CreateRootSignature':return self.out(5,'Plain')
            if n=='CreateGraphicsPipelineState':assert self.q(self.arg(1)) in self.objects;return self.out(3,'Plain')
            if n=='CreateCommandList':return self.out(6,'ID3D12GraphicsCommandList',closed=False)
            if n=='CreateCommittedResource':
                d=self.arg(3);dim=self.i(d);assert dim in [1,3]
                return self.out(7,'ID3D12Resource',state=self.arg(4),readback=dim==1)
            if n=='CreateRenderTargetView':self.rtv[self.arg(3)]=self.arg(1);return 0
            if n=='GetDescriptorHandleIncrementSize':return 32
            if n=='GetCopyableFootprints':
                assert self.arg(2)==0 and self.arg(3)==1 and self.arg(4)==0
                pitch=128 if self.case=='bad-footprint' else 256
                self.u.mem_write(self.arg(5),struct.pack('<QIIIII',0,28,64,64,1,pitch));self.u.mem_write(self.arg(6),struct.pack('<I',64))
                self.putq(self.arg(7),256);self.putq(self.arg(8),16384);return 0
        if kind=='ID3D12DescriptorHeap' and n=='GetCPUDescriptorHandleForHeapStart':
            self.putq(self.arg(1),0x51000000);return self.arg(1)
        if kind=='ID3D12CommandAllocator' and n=='Reset':assert self.waited;return 0
        if kind=='ID3D12GraphicsCommandList':
            if n=='Close':assert not o['closed'];o['closed']=True;return 0
            if n=='Reset':assert o['closed'] and self.waited;o['closed']=False;return 0
            assert not o['closed']
            if n=='OMSetRenderTargets':assert self.arg(1)==1;self.current_target=self.rtv[self.q(self.arg(2))];return 0
            if n=='DrawInstanced':assert self.arg(1)==3 and self.arg(2)==1;self.drawn.add(self.current_target);return 0
            if n=='ResourceBarrier':
                b=self.arg(2);assert self.arg(1)==1 and self.i(b)==0
                r=self.objects[self.q(b+8)];before,after=struct.unpack('<II',self.u.mem_read(b+20,8));assert r['state']==before,(r,before,after);r['state']=after;return 0
            if n=='CopyTextureRegion':assert self.q(self.arg(5)) in self.drawn;return 0
            if n in ['SetGraphicsRootSignature','RSSetViewports','RSSetScissorRects','ClearRenderTargetView','IASetPrimitiveTopology']:return 0
        if kind=='ID3D12CommandQueue':
            if n=='ExecuteCommandLists':
                assert self.arg(1)==1 and self.objects[self.q(self.arg(2))]['closed'];self.executed=True;self.waited=False;return 0
            if n=='Signal':
                if self.case=='signal':return 0x80004005
                self.signalled=self.arg(2)
                if self.case not in ['event-fail','timeout','wait-fail','wait-zero','incomplete','removed','event-success']:self.completed=self.signalled
                return 0
        if kind=='ID3D12Fence':
            if n=='GetCompletedValue':
                if self.case=='removed':return 0xffffffffffffffff
                if self.completed>=self.signalled and self.signalled:self.waited=True
                return self.completed
            if n=='SetEventOnCompletion':
                assert self.event_open and self.arg(1)==self.signalled
                return 0x80004005 if self.case=='event-fail' else 0
        if kind=='ID3D12Resource':
            if n=='Map':
                assert self.executed and self.waited and o.get('readback')
                pixels=bytearray(bytes([64,128,191,255])*64*64)
                if self.case=='bad-pixel12':pixels[-1]=0
                self.putq(self.arg(3),0 if self.case=='null-map12' else self.alloc(pixels));return 0
            if n=='Unmap':assert self.waited and self.q(self.arg(2))==0 and self.q(self.arg(2)+8)==0;return 0
        raise AssertionError('Unexpected COM call '+name)
cases=[('', ''),('', '--show11'),('', '--show12'),('', '--bad'),
       ('D3D11CreateDevice#1',''),('CreateDXGIFactory2#1',''),('D3DCompile#1',''),('D3DCompile#2',''),
       ('ID3D11Device.CreateVertexShader#1',''),('ID3D11Device.CreatePixelShader#1',''),('ID3D11Device.CreateRasterizerState#1',''),
       ('ID3D11Device.CreateTexture2D#1',''),('ID3D11Device.CreateRenderTargetView#1',''),('ID3D11Device.CreateTexture2D#2',''),
       ('ID3D11DeviceContext.Map#1',''),('bad-pixel11',''),('CreateDXGIFactory2#2',''),('IDXGIFactory4.EnumWarpAdapter#1',''),
       ('D3D12CreateDevice#1',''),('ID3D12Device.CreateCommandQueue#1',''),('ID3D12Device.CreateCommandAllocator#1',''),
       ('ID3D12Device.CreateFence#1',''),('ID3D12Device.CreateDescriptorHeap#1',''),('D3D12SerializeRootSignature#1',''),
       ('ID3D12Device.CreateRootSignature#1',''),('D3DCompile#3',''),('D3DCompile#4',''),('ID3D12Device.CreateGraphicsPipelineState#1',''),
       ('ID3D12Device.CreateCommandList#1',''),('ID3D12Device.CreateCommittedResource#1',''),('bad-footprint',''),
       ('ID3D12Device.CreateCommittedResource#2',''),('ID3D12GraphicsCommandList.Close#1',''),
       ('event-success',''),('signal',''),('event-fail',''),('timeout',''),('wait-fail',''),('incomplete',''),('removed',''),
       ('ID3D12Resource.Map#1',''),('bad-pixel12',''),('null-map12',''),
       ('CreateEventW#1',''),('event-zero',''),('wait-zero',''),('CreateWindowExW#1','--show11'),('window-zero','--show12'),
       ('IDXGIFactory4.CreateSwapChainForHwnd#1','--show11'),('IDXGISwapChain3.QueryInterface#1','--show12'),
       ('IDXGIFactory4.MakeWindowAssociation#1','--show12'),('IDXGISwapChain3.GetBuffer#1','--show11'),
       ('IDXGISwapChain3.GetBuffer#2','--show12'),('IDXGISwapChain3.Present#1','--show11'),
       ('IDXGISwapChain3.Present#1','--show12'),('ID3D12CommandAllocator.Reset#1','--show12'),
       ('ID3D12GraphicsCommandList.Reset#1','--show12')]
results=[]
for case,option in cases:
    r=Run(case,option);results.append({'case':case or option or 'success','exit_code':r.exit,'status':'passed'})
report={'status':'passed','cases':len(results),'results':results,
        'scope':'actual ARM64 T70DirectX.exe Entry and D3D11/12 control flow; Win32, DXGI, D3D compiler/runtime, COM resources and pixel output mocked; no Windows or GPU execution',
        'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
        'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [Path(__file__).resolve(),w/'MtkDisplay/DirectX.cxx']},
        'runtime_executed':False,'device_tested':False}
(w/'Build/directx-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print('Actual Windows ARM64 DirectX app:',len(results),'Win32/COM mock scenarios passed; no D3D runtime executed')
