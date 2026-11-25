#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the real ARM64 UMDF2 GNSS consumer; never sign/install/open a device."""
import argparse
from pathlib import Path
import contextlib,hashlib,json,os,shutil,subprocess,sys,tempfile,time
R=Path(__file__).resolve().parents[2];W=R/'Windows';T=W/'Toolchain';P=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--locked',action='store_true',help=argparse.SUPPRESS)
parser.add_argument('--compile-only',action='store_true')
parser.add_argument('--output-dir',type=Path,
                    help='select a new artifact directory instead of Windows/Build')
args=parser.parse_args()
if args.output_dir and args.output_dir.exists():
 parser.error('output directory must not already exist')
if not args.locked:
 os.execv(R/'Resources/Scripts/with-build-lock',[str(R/'Resources/Scripts/with-build-lock'),'T70Pro','Build and verify Windows ARM64 UMDF2 GNSS consumer','--',sys.executable,__file__,'--locked',*sys.argv[1:]])
os.chdir(R)
B=((args.output_dir if args.output_dir.is_absolute() else R/args.output_dir)
   if args.output_dir else W/'Build')
B.mkdir(parents=True,exist_ok=args.output_dir is None)
Darwin=sys.platform=='darwin'
Python=Path(sys.executable) if Darwin else R.parent/'.venv/bin/python'
Zig=os.environ.get('T70_WINDOWS_ZIG')
Linker=[Zig,'lld-link'] if Zig else ['lld-link']
wdk=T/'microsoft.windows.wdk.arm64/c';sdk=T/'microsoft.windows.sdk.cpp/c';sdk_lib=T/'microsoft.windows.sdk.cpp.arm64/c/um/arm64'
crt=next((T/'msvc-crt-headers').rglob('excpt.h')).parent
includes=[sdk/'Include/10.0.26100.0/shared',sdk/'Include/10.0.26100.0/um',sdk/'Include/10.0.26100.0/ucrt',crt,wdk/'Include/wdf/umdf/2.33']
commands=[]
def run(args):
 args=[str(x) for x in args];commands.append(args)
 p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 if p.returncode:print(p.stdout);raise SystemExit(p.returncode)
 if p.stdout:print(p.stdout[-2500:],flush=True)
 return p.stdout
@contextlib.contextmanager
def workdir():
 folder=Path(tempfile.mkdtemp(prefix='gnss-umdf-',dir=B))
 try:yield folder
 finally:
  for attempt in range(8):
   try:shutil.rmtree(folder);break
   except FileNotFoundError:break
   except OSError as e:
    if e.errno not in (39,66) or attempt==7:raise
    time.sleep(.5)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
with workdir() as d:
 overlay=d/'includes.json';overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':[
  {'type':'directory','name':str(folder),'contents':[{'type':'file','name':str(p.relative_to(folder)),'external-contents':str(p)} for p in folder.rglob('*') if p.is_file()]} for folder in includes]}))
 flags=['clang','--driver-mode=cl','--target=arm64-pc-windows-msvc','/nologo','/c','/O2','/GS','/Zl','/W3','/Gy',
  '/D_ARM64_','/DWIN64','/DWINVER=0x0A00','/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0A00000C',
  '/DUMDF_USING_NTSTATUS','/DUMDF_VERSION_MAJOR=2','/DUMDF_VERSION_MINOR=33','/DWDF_MAJOR_VERSION=2','/DWDF_MINOR_VERSION=33',
  '/clang:-Wno-invalid-token-paste','/clang:-Wno-nonportable-include-path','/clang:-Wno-ignored-attributes',
  '/clang:-Werror=incompatible-pointer-types','/clang:-ivfsoverlay','/clang:'+str(overlay),
  '/I'+str(R/'Silicon/MediaTek/MediaTekPkg/Include'),*['/imsvc'+str(p) for p in includes]]
 obj=d/'Driver.obj';run([*flags,'/Tc'+str(P/'Driver.c'),'/Fo'+str(obj)])
 if args.compile_only:
  print('GNSS UMDF2 production source compiled with the actual ARM64 WDK headers')
  raise SystemExit(0)
 runtime=d/'Runtime.obj';run([*flags,'/Tc'+str(P/'Runtime.c'),'/Fo'+str(runtime)])
 dll=B/'MtkGnss.dll'
 run([*Linker,'/nologo','/threads:6','/dll','/machine:arm64','/subsystem:windows,10.00','/entry:GnDllMain','/nodefaultlib','/opt:ref','/Brepro','/dynamicbase','/nxcompat','/noimplib','/out:'+str(dll),obj,runtime,
  wdk/'Lib/wdf/umdf/ARM64/2.33/WdfDriverStubUm.lib',sdk_lib/'kernel32.Lib',sdk_lib/'ntdll.lib',sdk_lib/'BufferOverflowU.lib',T/'microsoft.windows.sdk.cpp.arm64/c/ucrt/arm64/ucrt.lib'])
 info=run(['xcrun','llvm-objdump','--file-headers','--private-headers',dll] if Darwin else ['llvm-readobj','--file-headers','--coff-imports','--coff-exports',dll])
 assert ('file format coff-arm64' in info if Darwin else 'IMAGE_FILE_MACHINE_ARM64' in info) and 'DLL' in info and 'FxDriverEntryUm' in info
 assert 'ntoskrnl' not in info.lower() and 'wdfldr.sys' not in info.lower()
 test=B/'MtkGnssDriverTests.exe';testobj=d/'DriverTest.obj'
 run([*flags,'/GS-','/Tc'+str(P/'DriverTest.c'),'/Fo'+str(testobj)])
 coreobj=d/'GnssCore.obj'
 run([*flags,'/GS-','/clang:-mgeneral-regs-only','/Tc'+str(R/'Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkGnssCore.c'),'/Fo'+str(coreobj)])
 run([*Linker,'/nologo','/threads:6','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib','/opt:ref','/export:MtkGnssAssertionCount,DATA','/noimplib','/out:'+str(test),testobj,coreobj])
 run([Python,W/'test-request.py',test,'--test-name','MtkGnss','--report',B/'MtkGnss-driver-tests.json','--production-source',P/'Driver.c','--test-source',P/'DriverTest.c'])
 (B/'MtkGnss.inf').write_bytes((P/'MtkGnss.inf').read_bytes())
 sources=[P/n for n in ('Driver.c','Receiver.h','DriverTest.c','Runtime.c','MtkGnss.inf','build.py')]+[W/'test-request.py',R/'Silicon/MediaTek/MediaTekPkg/Include/Library/MtkGnssCore.h',R/'Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkGnssCore.c']
 report={'status':'passed','state':'ARM64 UMDF2 DLL and actual production callback harness built; unsigned and not installed',
  'source_sha256':{str(p.relative_to(R)):sha(p) for p in sources},
  'artifacts':{p.name:{'path':str(p),'bytes':p.stat().st_size,'sha256':sha(p)} for p in (dll,test,B/'MtkGnss.inf')},
  'commands':commands,'production_PE_headers_and_imports':info,
  'scope':'Actual UMDF2 location IOCTL, single-shot session/modify, pending fix cancellation, provider lease/generation, fix mapping and D0/removal callbacks under WDF/provider/clock mocks; no Windows load or hardware/RF/location collection',
  'missing_physical_receiver_provider':True,'receiver_protocol_is_firmware_ABI':False,'OS_runtime_executed':False,'device_access':False}
 (B/'mtkgnss-verification.json').write_text(json.dumps(report,indent=2)+'\n')
 print('GNSS UMDF2 DLL and ARM64 production callback tests passed; no installation or receiver opened')
