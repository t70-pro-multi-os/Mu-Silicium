#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the WARP D3D11/12 ARM64 app using the existing SDK; no install/download."""
from pathlib import Path
import hashlib,json,os,struct,subprocess,sys,tempfile
root=Path(__file__).resolve().parents[2]
if '--locked' not in sys.argv:
    os.execv(root/'Resources/Scripts/with-build-lock',[
        str(root/'Resources/Scripts/with-build-lock'),'T70Pro','Build Direct3D11/12 WARP app and validate readback bounds','--',
        sys.executable,__file__,'--locked',*sys.argv[1:]])
if set(sys.argv[1:])!={'--locked'}:raise SystemExit('No extra arguments accepted')
os.chdir(root)
Python=Path(sys.executable) if sys.platform=='darwin' else root.parent/'.venv/bin/python'
w=root/'Windows';t=w/'Toolchain';b=w/'Build';b.mkdir(exist_ok=True)
source=w/'MtkDisplay/DirectX.cxx';test=w/'MtkDisplay/DirectXTests.cxx';emulator=w/'MtkDisplay/test-directx.py'
sdk=t/'microsoft.windows.sdk.cpp/c';libs=t/'microsoft.windows.sdk.cpp.arm64/c/um/arm64'
crt=next((t/'msvc-crt-headers').rglob('excpt.h')).parent
includes=[sdk/'Include/10.0.26100.0/um',sdk/'Include/10.0.26100.0/shared',sdk/'Include/10.0.26100.0/ucrt',crt]
commands=[]
def run(args):
    args=list(map(str,args));commands.append(args)
    p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    if p.returncode:print(p.stdout);raise SystemExit(p.returncode)
    return p.stdout
with tempfile.TemporaryDirectory(prefix='directx-',dir=b) as td:
    tmp=Path(td);overlay=tmp/'sdk-overlay.json'
    overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':[
        {'type':'directory','name':str(d),'contents':[
            {'type':'file','name':str(p.relative_to(d)),'external-contents':str(p)}
            for p in d.rglob('*') if p.is_file()]} for d in includes]}))
    imports={
        'kernel32':['GetStdHandle','WriteFile','CloseHandle','CreateEventW','WaitForSingleObject',
                    'GetLastError','ExitProcess','GetModuleHandleW','GetTickCount64','GetCommandLineW'],
        'user32':['RegisterClassW','CreateWindowExW','DefWindowProcW','DestroyWindow',
                  'PostQuitMessage','PeekMessageW','TranslateMessage','DispatchMessageW']}
    for dll,names in imports.items():
        definition=tmp/(dll+'.def');definition.write_text('LIBRARY '+dll+'.dll\nEXPORTS\n'+'\n'.join(names)+'\n')
        run(['llvm-dlltool','-m','arm64','-d',definition,'-l',tmp/(dll+'.lib')])
    obj=tmp/'DirectX.obj';exe=b/'T70DirectX.exe'
    flags=['clang-cl','--target=arm64-pc-windows-msvc','/nologo','/c','/O2','/GS-','/Zl','/W3',
        '/std:c++17','/GR-','/EHs-c-','/D_ARM64_','/DWIN64','/DWINVER=0x0A00','/D_WIN32_WINNT=0x0A00',
        '/clang:-Wno-nonportable-include-path','/clang:-Wno-ignored-attributes',
        '/clang:-ivfsoverlay','/clang:'+str(overlay),*['/imsvc'+str(d) for d in includes]]
    warnings=run([*flags,source,'/Fo'+str(obj)])
    run(['lld-link','/nologo','/machine:arm64','/subsystem:console,10.00','/entry:Entry','/nodefaultlib',
        '/dynamicbase','/nxcompat','/highentropyva','/Brepro','/out:'+str(exe),obj,
        *[libs/n for n in ('dxgi.lib','d3d11.lib','d3d12.lib','d3dcompiler.lib')],
        tmp/'kernel32.lib',tmp/'user32.lib'])
    native=tmp/'DirectXTests'
    run(['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-fno-sanitize-recover=all',test,'-o',native])
    validation=run([native]).strip()
    emulated=run([Python,emulator]).strip()
    info=run(['llvm-readobj','--file-headers','--coff-imports',exe])
    assert 'IMAGE_FILE_MACHINE_ARM64' in info and 'IMAGE_SUBSYSTEM_WINDOWS_CUI' in info
    for symbol in ['D3D11CreateDevice','CreateDXGIFactory2','D3DCompile','D3D12SerializeRootSignature']:
        assert symbol in info,symbol
    # The SDK imports D3D12CreateDevice by ordinal, unlike D3D11. Prove
    # the ARM64 archive member maps that exact symbol to the PE ordinal.
    archive=(libs/'d3d12.lib').read_bytes();assert archive[:8]==b'!<arch>\n'
    at=8;ordinals=[]
    while at<len(archive):
        head=archive[at:at+60];length=int(head[48:58]);member=archive[at+60:at+60+length]
        if len(member)>=20 and member[:4]==b'\0\0\xff\xff' and struct.unpack_from('<H',member,6)[0]==0xaa64:
            if member[20:].split(b'\0')[0]==b'D3D12CreateDevice':ordinals.append(struct.unpack_from('<H',member,16)[0])
        at+=60+length+(length&1)
    assert ordinals==[101] and 'Symbol:  (101)' in info
    assert 'ntoskrnl' not in info.lower()
    report={'state':'ARM64 Windows application built; D3D runtime and physical device execution not performed',
        'backend':'Microsoft WARP CPU renderer; does not add Mali GPU acceleration',
        'binary':{'path':str(exe),'bytes':exe.stat().st_size,'sha256':hashlib.sha256(exe.read_bytes()).hexdigest()},
        'native_validator':{'status':'passed','result':validation,'scope':'actual production pixel validator with synthetic guarded/padded RGBA; ASan/UBSan'},
        'implemented':['D3D11 WARP feature-level 11 device and HLSL shader draw, staging readback',
            'D3D12 WARP direct queue, pipeline/root signature, draw, copy, fence then readback',
            'DXGI flip-discard double-buffer window presentation on explicit --show11 or --show12',
            'D3D12 timeout/device-removed gate retains in-flight resources until process teardown'],
        'arm64_control_flow':{'result':emulated,'report':str(b/'directx-tests.json')},
        'runtime_executed':False,'mali_hardware_acceleration':False,'device_tested':False,
        'source_sha256':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [source,test,emulator,Path(__file__).resolve()]},
        'compiler':run(['clang-cl','--version']).splitlines()[0],
        'sdk_d3d12_create_device_ordinal':ordinals[0],
        'sdk_d3d12_library_sha256':hashlib.sha256(archive).hexdigest(),
        'commands':commands,'compiler_warnings':warnings,'pe_headers_and_imports':info}
    (b/'directx-verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print(validation);print(emulated);print('DirectX ARM64 app linked:',report['binary']['bytes'],'bytes; WARP runtime remains unexecuted')
