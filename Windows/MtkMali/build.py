#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build and execute Mali MMU/JM, memory, scheduling, WDK DDI, worker, allocation, VM/MDL residency, adapter queries, backend and miniport lifetime, MFG domains/clocks/composite power, shared PRM PMIC transactions and command preparation tests, an unsigned ARM64 WDDM miniport SYS build artifact and a separate unsigned WDM board lower-filter SYS/INF, and an unregistered D3D10 resource/transfer/RTV/clear, shader and bounded triangle/preload component; no miniport INF, registration, installation or usable UMD."""
from pathlib import Path
import configparser,hashlib,importlib.util,json,os,re,struct,subprocess,sys,tempfile
R=Path(__file__).resolve().parents[2]; P=Path(__file__).resolve().parent
if '--locked' not in sys.argv:
 os.execv(R/'Resources/Scripts/with-build-lock',[str(R/'Resources/Scripts/with-build-lock'),
  'T70Pro','Check Mali backend and build WDK ARM64 library','--',sys.executable,__file__,'--locked'])
os.chdir(R)
B=R/'Windows/Build';B.mkdir(exist_ok=True)
Darwin=sys.platform=='darwin'
V=Path(sys.executable) if Darwin else R.parent/'.venv/bin/python'
Zig=os.environ.get('T70_MALI_ZIG')
CC=['clang']
LD=[Zig,'lld-link'] if Zig else ['lld-link']
LIB=[Zig,'lib'] if Zig else ['llvm-lib']
T=R/'Windows/Toolchain'
WDK=T/'microsoft.windows.wdk.arm64/c';SDK=T/'microsoft.windows.sdk.cpp/c'
MediaTekInclude=R/'Silicon/MediaTek/MediaTekPkg/Include'
includes=[WDK/'Include/10.0.26100.0/km',WDK/'Include/10.0.26100.0/shared',
 WDK/'Include/10.0.26100.0/um',SDK/'Include/10.0.26100.0/shared',SDK/'Include/10.0.26100.0/ucrt',SDK/'Include/10.0.26100.0/um',
 next((T/'msvc-crt-headers').rglob('excpt.h')).parent,MediaTekInclude]
commands=[]
def run(argv):
 argv=[str(x) for x in argv];commands.append(argv)
 r=subprocess.run(argv,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 if r.returncode:print(r.stdout);raise SystemExit(r.returncode)
 return r.stdout.strip()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def map_symbol_va(map_text,name):
 addresses=set()
 for line in map_text.splitlines():
  fields=line.split()
  if len(fields)<3 or fields[1]!=name:continue
  for candidate in fields[2:]:
   if re.fullmatch(r'[0-9A-Fa-f]{8,16}',candidate):
    value=int(candidate,16)
    if value>0xffffffff:addresses.add(value);break
 if len(addresses)!=1:raise SystemExit('Link map does not define one address for '+name+': '+str(sorted(addresses)))
 return next(iter(addresses))
def pe_rva_file_range(image,pe_offset,rva,size,executable=False):
 if size<=0:raise SystemExit('PE RVA range must be nonempty')
 if rva<0 or rva+size>0x100000000:raise SystemExit('PE RVA range exceeds the 32-bit image address space')
 section_count=struct.unpack_from('<H',image,pe_offset+6)[0]
 optional_size=struct.unpack_from('<H',image,pe_offset+20)[0]
 section_table=pe_offset+24+optional_size
 for index in range(section_count):
  offset=section_table+40*index
  virtual_size,virtual_address,raw_size,raw_offset=struct.unpack_from('<IIII',image,offset+8)
  characteristics=struct.unpack_from('<I',image,offset+36)[0]
  if virtual_address<=rva and rva+size<=virtual_address+raw_size:
   delta=rva-virtual_address
   if executable and not characteristics&0x20000000:break
   file_offset=raw_offset+delta
   if file_offset+size>len(image):raise SystemExit('PE RVA range exceeds file-backed bytes')
   return file_offset
 raise SystemExit('PE RVA is not backed by file bytes: '+hex(rva))
def pe_rva_file_offset(image,pe_offset,rva):
 return pe_rva_file_range(image,pe_offset,rva,1)
def arm64_function_range(image,pe_offset,optional,rva):
 exception_rva,exception_size=struct.unpack_from('<II',image,optional+112+3*8)
 if not exception_rva or not exception_size or exception_size%8:
  raise SystemExit('ARM64 PE has no well-formed exception function table')
 exception_offset=pe_rva_file_range(image,pe_offset,exception_rva,exception_size)
 records={}
 for index in range(0,exception_size,8):
  start,unwind=struct.unpack_from('<II',image,exception_offset+index)
  if start in records and records[start]!=unwind:raise SystemExit('Conflicting ARM64 runtime-function records')
  records[start]=unwind
 if rva not in records:raise SystemExit('Link-map function is absent from the ARM64 exception table: '+hex(rva))
 unwind=records[rva];flag=unwind&3
 if flag in (1,2):
  length_words=(unwind>>2)&0x7ff
 elif flag==0:
  xdata_rva=unwind&0xfffffffc
  xdata_offset=pe_rva_file_range(image,pe_offset,xdata_rva,4)
  xdata_header=struct.unpack_from('<I',image,xdata_offset)[0]
  if (xdata_header>>18)&3:raise SystemExit('Unsupported ARM64 xdata version')
  length_words=xdata_header&0x3ffff
 else:
  raise SystemExit('ARM64 function entry is a jump-island pdata link, not a function-length record')
 length_bytes=length_words*4;end_rva=rva+length_bytes
 if not length_bytes or end_rva>0xffffffff:raise SystemExit('ARM64 runtime-function length is invalid')
 following=[start for start in records if start>rva]
 if following and min(following)<end_rva:
  raise SystemExit('ARM64 runtime-function extent overlaps the next pdata begin')
 pe_rva_file_range(image,pe_offset,rva,length_bytes,executable=True)
 return rva,end_rva
def arm64_direct_branch(image,pe_offset,optional,start_rva,end_rva,target_rva):
 image_base=struct.unpack_from('<Q',image,optional+24)[0]
 code_size=end_rva-start_rva
 code_offset=pe_rva_file_range(image,pe_offset,start_rva,code_size,executable=True)
 for delta in range(0,code_size-3,4):
  pc_rva=start_rva+delta
  instruction=struct.unpack_from('<I',image,code_offset+delta)[0]
  opcode=instruction&0xfc000000
  if opcode not in (0x14000000,0x94000000):continue
  immediate=instruction&0x03ffffff
  if immediate&0x02000000:immediate-=0x04000000
  destination=pc_rva+(immediate<<2)
  if destination==target_rva:
   return {'instruction_rva':pc_rva,'opcode':'BL' if opcode==0x94000000 else 'B',
    'target_rva':destination,'target_va':image_base+destination}
 return None
BrokerHeader=MediaTekInclude/'Library/MtkPmicBroker.h'
BrokerCore=R/'Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsPrm/MtkPmicBroker.c'
GaugeCore=R/'Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/MtkGaugeCore.c'
GaugeHeader=MediaTekInclude/'Library/MtkGaugeCore.h'
ConnPowerHeader=MediaTekInclude/'Library/MtkConnPower.h'
ConnPowerCore=R/'Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkConnPower.c'
SourceFiles=[*sorted(p for p in P.iterdir() if p.is_file() and p.suffix in ('.c','.h','.py','.inf')),
             P/'Shader/ShaderCompiler.h',BrokerHeader,BrokerCore,GaugeHeader,GaugeCore,ConnPowerHeader,ConnPowerCore]
FrozenSources={str(f.relative_to(R)):digest(f) for f in SourceFiles}
with tempfile.TemporaryDirectory(prefix='mali-check-',dir=None if Darwin else B) as tmp:
 tmp=Path(tmp)
 native=tmp/'MaliJmTests'
 run(['clang' if Darwin else 'gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-fno-sanitize-recover=all',P/'MaliJm.c',P/'CoreTests.c','-o',native])
 native_result=run([native]);print(native_result)
 execution=tmp/'MaliExecutionTests'
 run(['clang' if Darwin else 'gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-fno-sanitize-recover=all',P/'MaliJm.c',P/'Execution.c',P/'ExecutionTests.c','-o',execution])
 execution_result=run([execution]);print(execution_result)
 commands_test=tmp/'MaliCommandsTests'
 run(['clang' if Darwin else 'gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-fno-sanitize-recover=all','-DMALI_COMMAND_TEST_NATIVE',P/'Commands.c',P/'CommandsTests.c','-o',commands_test])
 commands_result=run([commands_test]);print(commands_result)
 mfg_test=tmp/'MaliMfgTests'
 run(['clang' if Darwin else 'gcc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-fno-sanitize-recover=all','-DMALI_MFG_TEST_NATIVE',P/'Mfg.c',P/'MfgTests.c','-o',mfg_test])
 mfg_result=run([mfg_test]);print(mfg_result)
 shared=tmp/('libMaliJm-test.dylib' if Darwin else 'libMaliJm-test.so')
 # Execute Mesa's original float_to_fixed helper as a separate native oracle.
 # It uses the user's existing Mesa source; no source copy survives this build.
 mesa_clear=(R.parent/'mesa/src/panfrost/lib/pan_clear.c').read_text()
 helper=mesa_clear[mesa_clear.index('static inline uint32_t\nfloat_to_fixed('):mesa_clear.index('\nstruct mali_tib_layout')]
 oracle=tmp/'mesa-clear-reference.c'
 oracle.write_text('#include <stdint.h>\n#include <stdbool.h>\n#include <string.h>\n#include <math.h>\n#define _mesa_roundevenf rintf\n'+helper+
  '\nuint32_t MaliMesaClearReference(uint32_t bits) { float f; memcpy(&f,&bits,4); if(!(f>0))return 0; if(f>=1)return 255; return float_to_fixed(f,8,0,false); }\n')
 run(['clang' if Darwin else 'gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-dynamiclib' if Darwin else '-shared','-fPIC',P/'MaliJm.c',P/'Commands.c',P/'Mfg.c',oracle,'-lm','-o',shared])
 print(run([V,P/'test-stock.py',shared]))
 print(run([V,P/'test-commands.py',shared]))
 print(run([V,P/'test-stock-power.py',shared]))
 overlay=tmp/'wdk-overlay.json'
 roots=[{'type':'directory','name':str(d),'contents':[{'type':'file','name':str(f.relative_to(d)),
  'external-contents':str(f)} for f in d.rglob('*') if f.is_file()]} for d in includes]
 overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':roots}))
 flags=[*CC,'--target=aarch64-pc-windows-msvc','-c','-Xclang','-fms-kernel','-fms-extensions',
  '-fms-compatibility','-O2','-fstack-usage','-fstack-protector-strong','-mno-red-zone',
  '-D_ARM64_','-DWIN64','-DWINVER=0x0A00','-D_WIN32_WINNT=0x0A00',
  '-Wno-invalid-token-paste','-Wno-nonportable-include-path','-Wno-ignored-attributes',
  '-Werror=incompatible-pointer-types','-ivfsoverlay',str(overlay)]
 for d in includes: flags+=['-isystem',str(d)]
 objects=[];stack_usage={}
 for name in ('MaliJm','Wddm','Memory','Execution','Ddi','Worker','Commands','Prepare','Allocation','Vm','Residency','Runtime','Mfg','Miniport','Board','Soc','Host','Interfaces','PmicPrm'):
  obj=tmp/(name+'.obj');output=run([*flags,P/(name+'.c'),'-o',str(obj)])
  if output:print(output)
  if obj.read_bytes()[:2]!=b'\x64\xaa':raise SystemExit('Non-ARM64 COFF member: '+str(obj))
  objects.append(obj)
 gauge_obj=tmp/'MtkGaugeCore.obj'
 run([*flags,GaugeCore,'-o',gauge_obj])
 objects.append(gauge_obj)
 # WDK exposes processor groups either as a Group field or the high half of
 # Level. Compile the real product DDI and execute its tests in both layouts.
 group_obj=tmp/'Ddi-groups.obj'
 run([*flags,'-DNT_PROCESSOR_GROUPS=1',P/'Ddi.c','-o',group_obj])
 group_frames={}
 for line in group_obj.with_suffix('.su').read_text().splitlines():
  function,size,kind=line.rsplit('\t',2)
  group_frames[function.rsplit(':',1)[-1]]={'bytes':int(size),'kind':kind}
 if any(v['bytes']>2048 or v['kind']!='static' for v in group_frames.values()):
  raise SystemExit('Processor-group DDI exceeds fixed 2 KiB frame limit')
 for obj in objects:
  if obj.stem=='Interfaces':
   assert not obj.with_suffix('.su').exists()  # constants only, no stack frames
   continue
  for line in obj.with_suffix('.su').read_text().splitlines():
   function,size,kind=line.rsplit('\t',2)
   stack_usage[obj.stem+'.c:'+function.rsplit(':',1)[-1]]={'bytes':int(size),'kind':kind}
 if any(v['bytes']>2048 or v['kind']!='static' for v in stack_usage.values()):
  raise SystemExit('Mali production function exceeds fixed 2 KiB kernel frame limit: '+str(stack_usage))
 library=B/'MtkMaliCore.lib'
 run([*LIB,'/nologo','/machine:arm64','/out:'+str(library),*objects])
 # Keep user-mode COFF objects out of the kernel archive and kernel flags.
 user_flags=[]
 for i,value in enumerate(flags):
  if value in ('-Xclang','-fms-kernel',str(WDK/'Include/10.0.26100.0/km')):continue
  if value=='-isystem' and flags[i+1]==str(WDK/'Include/10.0.26100.0/km'):continue
  user_flags.append(value)
 user_flags+=['-Wall','-Wextra','-Werror']
 user_objects=[];user_stack={}
 for name,filename in (('Umd','Umd.c'),('UmdCommands','Commands.c')):
  obj=tmp/(name+'.obj');run([*user_flags,P/filename,'-o',obj]);user_objects.append(obj)
  assert obj.read_bytes()[:2]==b'\x64\xaa'
  for line in obj.with_suffix('.su').read_text().splitlines():
   function,size,kind=line.rsplit('\t',2)
   user_stack[filename+':'+function.rsplit(':',1)[-1]]={'bytes':int(size),'kind':kind}
 oversized_user_frames={name:v for name,v in user_stack.items() if v['kind']!='static' or v['bytes']>2048}
 if oversized_user_frames:
  raise SystemExit('Mali user-mode function exceeds fixed 2 KiB frame limit: '+str(oversized_user_frames))
 user_library=B/'MtkMaliUmd.lib'
 run([*LIB,'/nologo','/machine:arm64','/out:'+str(user_library),*user_objects])
 # Link the actual user component to the real compiler import library. The
 # six shader callbacks execute in fixtures below; this proves the DLL boundary,
 # not Direct3D runtime registration or GPU rendering.
 sys.path.insert(0,str(P/'Shader'))
 spec=importlib.util.spec_from_file_location('t70_shader_windows',P/'Shader/build-windows.py')
 shader_windows=importlib.util.module_from_spec(spec);spec.loader.exec_module(shader_windows)
 shader_proofs={}
 for report_name in ('mali-shader-verification.json','mali-shader-windows.json'):
  report_path=R.parent/'analysis'/report_name;proof=json.loads(report_path.read_text())
  if proof['status']!='passed' or any(digest(R.parent/name)!=value for name,value in proof['source_sha256'].items()):
   raise SystemExit('Shader verification is stale: '+report_name)
  shader_proofs[report_name]={'path':str(report_path),'sha256':digest(report_path)}
 shader_dir=R/'Build/MaliShader/windows'
 shader_dll=shader_dir/'MtkMaliShader.dll'
 if digest(shader_dll)!=json.loads((R.parent/'analysis/mali-shader-windows.json').read_text())['artifacts']['compiler']['sha256']:
  raise SystemExit('Shader DLL differs from verified artifact')
 shader_import_library=shader_dir/'libMtkMaliShader.dll.a'
 qualified_import=json.loads((R.parent/'analysis/mali-shader-windows.json').read_text())['artifacts']['compiler_import_library']
 if digest(shader_import_library)!=qualified_import['sha256']:
  raise SystemExit('Shader import library differs from verified artifact')
 user_dll=B/'MtkMaliUmd.dll';user_sdk=T/'microsoft.windows.sdk.cpp.arm64/c'
 runtime_candidates=list((R/'Build/MaliShader/zig-cache').rglob('compiler_rt.lib'))
 if len(runtime_candidates)!=1:raise SystemExit('Expected the one qualified ARM64 Zig compiler runtime')
 user_runtime=runtime_candidates[0];user_map=tmp/'MtkMaliUmd.map'
 user_exports=['MtkMaliUmdDeviceBytes','MtkMaliUmdInitialize','MtkMaliUmdClose','MtkMaliUmdFailureReason',
  'MtkMaliUmdResourceFunctions','MtkMaliUmdShaderFunctions','MtkMaliUmdCompileBoundShaders',
  'MtkMaliUmdUploadBoundShaders','MtkMaliUmdReleaseProgram',
  'MtkMaliUmdResourceRange','MtkMaliUmdSubmitRaster']
 run([*LD,'/nologo','/threads:6','/dll','/entry:_DllMainCRTStartupForGS','/noimplib','/nodefaultlib','/machine:arm64',
  '/dynamicbase','/highentropyva','/nxcompat','/Brepro','/map:'+str(user_map),'/out:'+str(user_dll),
  *['/export:'+name for name in user_exports],user_library,shader_import_library,
  user_sdk/'um/arm64/kernel32.Lib',user_sdk/'um/arm64/Gdi32.Lib',
  user_sdk/'um/arm64/BufferOverflowU.lib',user_runtime])
 user_map_text=user_map.read_text()
 if not all(name in user_map_text for name in ('_DllMainCRTStartupForGS','__security_init_cookie','__security_check_cookie')):
  raise SystemExit('User DLL does not initialize/check the production MSVC stack cookie')
 user_pe=shader_windows.inspect_pe(user_dll)
 expected_imports={'t70_shader_compile_tokens','t70_shader_compile_token_pipeline','t70_shader_compile_framebuffer_preload','t70_shader_free','t70_shader_free_pipeline'}
 if set(user_pe['exports'])!=set(user_exports) or not expected_imports<=set(user_pe['imports'].get('mtkmalishader.dll',[])):
  raise SystemExit('User component does not import the actual shader ABI')
 host_entry=tmp/'HostEntry.obj'
 run([*flags,P/'HostEntry.c','-o',host_entry])
 host_sys=B/'MtkMaliBoard.sys';km=WDK/'Lib/10.0.26100.0/km/ARM64'
 host_map=tmp/'MtkMaliBoard.map'
 run([*LD,'/nologo','/threads:6','/driver:wdm','/release','/map:'+str(host_map),'/machine:arm64','/subsystem:native,10.00',
  '/entry:GsDriverEntry','/nodefaultlib','/dynamicbase','/nxcompat','/tsaware:no','/osversion:10.0','/Brepro',
  '/out:'+str(host_sys),host_entry,library,km/'ntoskrnl.lib',km/'hal.lib',km/'libcntpr.lib',km/'bufferoverflowfastfailk.lib'])
 host_pe=run(['xcrun','llvm-objdump','--file-headers','--private-headers',host_sys] if Darwin else
  ['llvm-readobj','--file-headers','--coff-imports',host_sys])
 # Both existing header commands already include the actual import table.
 # Xcode ships llvm-objdump but does not provide llvm-readobj on this host.
 host_imports=host_pe
 if Darwin:
  current_dll=None;prm_import=False
  for line in host_imports.splitlines():
   cells=line.split()
   if line.strip().startswith('DLL Name:'):current_dll=line.split(':',1)[1].strip().lower()
   if len(cells)==2 and cells[0].isdigit() and cells[1]=='ExGetPrmInterface':
    prm_import=current_dll=='ntoskrnl.exe'
 else:
  prm_import='Symbol: ExGetPrmInterface (' in host_imports
 if Darwin:assert 'file format coff-arm64' in host_pe and '(NT native)' in host_pe
 else:assert 'IMAGE_FILE_MACHINE_ARM64' in host_pe and 'IMAGE_SUBSYSTEM_NATIVE' in host_pe
 assert 'dxgkrnl' not in host_pe.lower() and 'DxgkInitialize' not in host_pe
 if not prm_import:raise SystemExit('Board SYS does not import the fixed-GUID PRM client API')
 if any(symbol not in host_imports for symbol in ('IoRegisterDeviceInterface','IoSetDeviceInterfaceState')):
  raise SystemExit('Board SYS does not publish the CONN device interface')
 map_text=host_map.read_text()
 assert 'HostEntry.obj' in map_text and 'MtkMaliBoardHostDriverEntry' in map_text and 'Host.obj' in map_text
 assert 'Soc.obj' in map_text and 'MtkMaliSocStart' in map_text
 assert 'Miniport.obj' not in map_text and 'DxgkInitialize' not in map_text
 image=host_sys.read_bytes();pe_offset=struct.unpack_from('<I',image,0x3c)[0];optional=pe_offset+24
 assert image[:2]==b'MZ' and image[pe_offset:pe_offset+4]==b'PE\0\0'
 assert struct.unpack_from('<H',image,pe_offset+4)[0]==0xaa64 and struct.unpack_from('<H',image,optional)[0]==0x20b
 checksum_offset=optional+64;checksum=struct.unpack_from('<I',image,checksum_offset)[0];total=0
 for offset in range(0,len(image),2):
  if checksum_offset<=offset<checksum_offset+4:continue
  total+=int.from_bytes(image[offset:offset+2],'little');total=(total&0xffff)+(total>>16)
 total=(total&0xffff)+(total>>16);expected_checksum=(total&0xffff)+len(image)
 assert checksum and checksum==expected_checksum
 characteristics=struct.unpack_from('<H',image,optional+70)[0]
 assert characteristics&0x2000  # WDM image flag
 assert not characteristics&0x8000  # TERMINAL_SERVER_AWARE must be clear
 assert struct.unpack_from('<II',image,optional+112+4*8)==(0,0)  # unsigned
 inf=configparser.ConfigParser(interpolation=None,allow_no_value=True,delimiters=('=',))
 inf.read(P/'MtkMaliBoard.inf')
 assert inf['Version']['Class']=='Extension' and inf['Version']['ExtensionId']=='{27316789-37e8-4370-a1b3-7562f713e012}'
 assert inf['Install.NT.Filters']['AddFilter']=='MtkMaliBoard,,BoardFilter' and inf['BoardFilter']['FilterPosition']=='Lower'
 assert inf['Install.NT.Services']['AddService']=='MtkMaliBoard,0,Service'
 assert inf['Models.NTarm64.10.0...26100']['%Description%']=='Install,ACPI\\HANA678E'
 assert inf['Service']['ServiceBinary']=='%13%\\MtkMaliBoard.sys' and inf['Service']['StartType']=='3'
 host_package={'kind':'unsigned WDM board resource lower filter, not a display miniport or UMD',
  'host_entry_and_board_member_selected':True,'display_miniport_member_linked':False,
  'entry_link_map_excerpt':[line.strip() for line in map_text.splitlines() if any(x in line for x in ('DriverEntry','MtkMaliBoardHostAddDevice','MtkMaliBoardHostDispatch'))],
  'PE_checksum':checksum,'PE_checksum_independently_verified':True,'PE_WDM_flag':True,'signature_present':False,
  'INF_is_device_specific_extension_lower_filter':True,'INF_associated_function_driver_service':False,
  'INF_requires_real_parent_resource_interface':True,'Windows_InfVerif_executed':False,
  'PE_headers_and_imports':host_pe,'PRM_interface_imports':host_imports,'ExGetPrmInterface_import_verified':True,
  'installed_or_Windows_loaded':False,'hardware_executed':False}
 (B/'MtkMaliBoard.inf').write_bytes((P/'MtkMaliBoard.inf').read_bytes())
 soc_inf=configparser.ConfigParser(interpolation=None,allow_no_value=True,delimiters=('=',));soc_inf.read(P/'MtkMaliSoc.inf')
 assert soc_inf['Version']['Class']=='System' and soc_inf['Models.NTarm64.10.0...26100']['%Description%']=='Install,ACPI\\HANA678F'
 assert soc_inf['Install.NT.Services']['AddService']=='MtkMaliBoard,2,Service'
 assert soc_inf['Service']['ServiceBinary']==inf['Service']['ServiceBinary'] and soc_inf['Service']['StartType']=='3'
 (B/'MtkMaliSoc.inf').write_bytes((P/'MtkMaliSoc.inf').read_bytes())
 host_package['same_service_root_resource_owner_INF']='MtkMaliSoc.inf'
 host_package['root_owner_HID']='ACPI\\HANA678F'
 host_package['root_and_GPU_share_the_same_SYS_service']=True
 host_package['root_owner_qualification_backend_installed']=False
 miniport_sys=B/'MtkMaliMiniport.sys';miniport_map=tmp/'MtkMaliMiniport.map'
 displib=km/'displib.lib'
 if not displib.is_file():raise SystemExit('Pinned WDK ARM64 displib.lib is missing')
 run([*LD,'/nologo','/threads:6','/driver:wdm','/release','/map:'+str(miniport_map),'/machine:arm64','/subsystem:native,10.00',
  '/entry:GsDriverEntry','/nodefaultlib','/dynamicbase','/nxcompat','/tsaware:no','/osversion:10.0','/Brepro',
  '/out:'+str(miniport_sys),library,displib,km/'ntoskrnl.lib',km/'hal.lib',km/'libcntpr.lib',km/'bufferoverflowfastfailk.lib'])
 miniport_pe=run(['xcrun','llvm-objdump','--file-headers','--private-headers',miniport_sys] if Darwin else
  ['llvm-readobj','--file-headers','--coff-imports',miniport_sys])
 if Darwin:assert 'file format coff-arm64' in miniport_pe and '(NT native)' in miniport_pe
 else:assert 'IMAGE_FILE_MACHINE_ARM64' in miniport_pe and 'IMAGE_SUBSYSTEM_NATIVE' in miniport_pe
 assert 'ntoskrnl.exe' in miniport_pe.lower() and 'DxgkInitialize' not in miniport_pe
 miniport_map_text=miniport_map.read_text()
 miniport_required=('GsDriverEntry','DriverEntry','__security_init_cookie','__security_check_cookie',
  'DxgkInitialize','displib','Miniport.obj','Ddi.obj','Runtime.obj','MtkMaliMiniportStartDevice','MtkMaliDdiQueryAdapterInfo')
 missing_miniport_map=[name for name in miniport_required if name.lower() not in miniport_map_text.lower()]
 if missing_miniport_map:raise SystemExit('Miniport link map is missing required WDK/core members: '+str(missing_miniport_map))
 assert 'HostEntry.obj' not in miniport_map_text and 'Host.obj' not in miniport_map_text and 'MtkMaliBoardHostDriverEntry' not in miniport_map_text
 miniport_image=miniport_sys.read_bytes();miniport_pe_offset=struct.unpack_from('<I',miniport_image,0x3c)[0];miniport_optional=miniport_pe_offset+24
 assert miniport_image[:2]==b'MZ' and miniport_image[miniport_pe_offset:miniport_pe_offset+4]==b'PE\0\0'
 assert struct.unpack_from('<H',miniport_image,miniport_pe_offset+4)[0]==0xaa64 and struct.unpack_from('<H',miniport_image,miniport_optional)[0]==0x20b
 miniport_entry_rva=struct.unpack_from('<I',miniport_image,miniport_optional+16)[0]
 assert miniport_entry_rva!=0
 miniport_checksum_offset=miniport_optional+64;miniport_checksum=struct.unpack_from('<I',miniport_image,miniport_checksum_offset)[0];miniport_total=0
 for offset in range(0,len(miniport_image),2):
  if miniport_checksum_offset<=offset<miniport_checksum_offset+4:continue
  miniport_total+=int.from_bytes(miniport_image[offset:offset+2],'little');miniport_total=(miniport_total&0xffff)+(miniport_total>>16)
 miniport_total=(miniport_total&0xffff)+(miniport_total>>16);miniport_expected_checksum=(miniport_total&0xffff)+len(miniport_image)
 assert miniport_checksum and miniport_checksum==miniport_expected_checksum
 miniport_characteristics=struct.unpack_from('<H',miniport_image,miniport_optional+70)[0]
 assert miniport_characteristics&0x2000  # WDM image flag
 assert not miniport_characteristics&0x8000  # TERMINAL_SERVER_AWARE must be clear
 assert struct.unpack_from('<II',miniport_image,miniport_optional+112)==(0,0)  # no PE exports
 assert struct.unpack_from('<II',miniport_image,miniport_optional+112+4*8)==(0,0)  # unsigned
 miniport_image_base=struct.unpack_from('<Q',miniport_image,miniport_optional+24)[0]
 gs_entry_va=map_symbol_va(miniport_map_text,'GsDriverEntry')
 driver_entry_va=map_symbol_va(miniport_map_text,'DriverEntry')
 dxgk_initialize_va=map_symbol_va(miniport_map_text,'DxgkInitialize')
 if gs_entry_va-miniport_image_base!=miniport_entry_rva:
  raise SystemExit('PE entry RVA does not resolve to the mapped GsDriverEntry')
 gs_range=arm64_function_range(miniport_image,miniport_pe_offset,miniport_optional,gs_entry_va-miniport_image_base)
 driver_range=arm64_function_range(miniport_image,miniport_pe_offset,miniport_optional,driver_entry_va-miniport_image_base)
 gs_to_driver=arm64_direct_branch(miniport_image,miniport_pe_offset,miniport_optional,*gs_range,
  driver_entry_va-miniport_image_base)
 driver_to_dxgk=arm64_direct_branch(miniport_image,miniport_pe_offset,miniport_optional,*driver_range,
  dxgk_initialize_va-miniport_image_base)
 if not gs_to_driver:raise SystemExit('GsDriverEntry has no direct ARM64 B/BL call to Miniport DriverEntry')
 if not driver_to_dxgk:raise SystemExit('Miniport DriverEntry has no direct ARM64 B/BL call to WDK DxgkInitialize')
 miniport_package={'kind':'unsigned ARM64 WDDM miniport SYS build artifact only; no INF, registration, installation or OS load',
  'entrypoint':'GsDriverEntry','entry_rva':miniport_entry_rva,'GS_entry_and_cookie_helpers_linked':True,
  'image_base':miniport_image_base,'mapped_symbol_addresses':{'GsDriverEntry':gs_entry_va,'DriverEntry':driver_entry_va,'DxgkInitialize':dxgk_initialize_va},
  'entry_call_path':{'PE_entry_matches_GsDriverEntry':True,'GsDriverEntry_function_bytes':gs_range[1]-gs_range[0],
   'DriverEntry_function_bytes':driver_range[1]-driver_range[0],'GsDriverEntry_to_DriverEntry':gs_to_driver,
   'DriverEntry_to_static_DxgkInitialize':driver_to_dxgk,
   'verification':'ARM64 direct B/BL targets decoded only inside executable file-backed bytes bounded by packed .pdata or full .xdata FunctionLength; the next .pdata begin is an overlap guard'},
  'core_archive_members_selected':['Miniport.obj','Ddi.obj','Runtime.obj'],
  'host_entry_and_board_members_excluded':True,
  'entry_link_map_excerpt':[line.strip() for line in miniport_map_text.splitlines() if any(x.lower() in line.lower() for x in
   ('GsDriverEntry','DriverEntry','DxgkInitialize','security_init_cookie','security_check_cookie','Miniport.obj','Ddi.obj','Runtime.obj','MtkMaliMiniportStartDevice','MtkMaliDdiQueryAdapterInfo'))],
  'DxgkInitialize_resolution':{'symbol_resolution':'WDK_static_displib_helper','literal_DxgkInitialize_import_present':False,
   'map_proves_static_helper_selected':True,'library':str(displib.relative_to(R)),'library_sha256':digest(displib)},
  'PE_headers_and_imports':miniport_pe,'ntoskrnl_import_table_present':True,
  'PE_checksum':miniport_checksum,'PE_checksum_independently_verified':True,'PE_WDM_flag':True,
  'PE_exports_present':False,'signature_present':False,'miniport_INF_created_or_installed':False,
  'registered_or_Windows_loaded':False,'hardware_executed':False}
 tests={}
 test_flags=[*flags,'-fno-stack-protector','-mstack-probe-size=65536','-D_NTOSKRNL_','-DNTKERNELAPI=']
 common=[]
 for name in ('MaliJm','Wddm','Execution','Commands','Mfg','Interfaces','TestRuntime'):
  obj=tmp/(name+'-test.obj');run([*test_flags,P/(name+'.c'),'-o',str(obj)]);common.append(obj)
 for name in ('Wddm','Memory','Execution','Ddi','Commands','Prepare','Allocation','Vm','Residency','Adapter','Runtime','Mfg','Miniport','Board','Host','Soc','PmicPrm'):
  obj=tmp/(name+'Tests.obj');run([*test_flags,P/(name+'Tests.c'),'-o',str(obj)])
  exe=B/('Mali'+name+'Tests.exe');counter='MtkMali'+name+'AssertionCount'
  run([*LD,'/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
   '/opt:ref','/export:'+counter+',DATA','/out:'+str(exe),*common,obj])
  print(run([V,P/'test-wdk.py',exe,counter]));tests[name]=json.loads(exe.with_suffix('.json').read_text())
 user_test_flags=[*user_flags,'-fno-stack-protector','-mstack-probe-size=65536']
 user_test_objects=[]
 for name in ('UmdTests','Commands','TestRuntime'):
  obj=tmp/(name+'-user-test.obj');run([*user_test_flags,P/(name+'.c'),'-o',obj]);user_test_objects.append(obj)
 # Compile separate C and C++ consumers; this catches WDK C constant
 # definitions and missing extern-C linkage that a single translation unit hides.
 for suffix,function in (('c','MtkMaliUmdCConsumer'),('cpp','MtkMaliUmdCppConsumer')):
  source=tmp/('UmdConsumer.'+suffix);obj=tmp/('UmdConsumer-'+suffix+'.obj')
  prefix='extern "C" ' if suffix=='cpp' else ''
  source.write_text('#include "Umd.h"\n'+prefix+'SIZE_T '+function+'(void) { D3D10DDI_HDEVICE D={0}; MALI_UMD_PROGRAM P; ZeroMemory(&P,sizeof(P)); if(MtkMaliUmdUploadBoundShaders(D,0,&P)!=E_INVALIDARG || MtkMaliUmdReleaseProgram(D,&P)!=E_INVALIDARG || MtkMaliUmdResourceRange(D,(D3D10DDI_HRESOURCE){0},MaliPteRead,0)!=E_INVALIDARG || MtkMaliUmdSubmitRaster(D,&P,0,0,0)!=E_INVALIDARG)return 0; return MtkMaliUmdDeviceBytes()+D3D10_16BIT_INDEX_STRIP_CUT_VALUE; }\n')
  run([*user_test_flags,*(['-std=c++17'] if suffix=='cpp' else []),'-I',P,'-c',source,'-o',obj])
  user_test_objects.append(obj)
 user_exe=B/'MaliUmdTests.exe';user_counter='MtkMaliUmdAssertionCount'
 run([*LD,'/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:console','/entry:TestMain','/nodefaultlib',
  '/opt:ref','/export:'+user_counter+',DATA','/out:'+str(user_exe),*user_test_objects])
 print(run([V,P/'test-wdk.py',user_exe,user_counter]))
 tests['Umd']=json.loads(user_exe.with_suffix('.json').read_text())
 if user_stack['Umd.c:UmdDraw']['bytes']!=800 or user_stack['Umd.c:UmdDraw']['kind']!='static':
  raise SystemExit('Bounded Draw stack frame differs from the qualified 800-byte static frame')
 user_component={'status':'implemented and ARM64-fixture executed; not registered or a usable D3D device',
  'library':str(user_library.relative_to(R)),'unregistered_DLL':str(user_dll.relative_to(R)),
  'actual_compiler_DLL_link':user_pe,'compiler_proofs':shader_proofs,
  'SDK_GS_entry_and_cookie_initialization_linked':True,'DLL_entry_or_Windows_loader_executed':False,
  'compiler_memory_runtime':{'path':str(user_runtime),'sha256':digest(user_runtime)},
'production_coff_members':[f.name for f in user_objects],
  'production_stack_frames':user_stack,'kernel_compile_mode':False,'separate_C_and_Cpp_header_consumers_linked_and_executed':True,
  'd3d10_callbacks':['CalcPrivateResourceSize','CreateResource','DestroyResource','CalcPrivateRenderTargetViewSize',
   'CreateRenderTargetView','DestroyRenderTargetView','ClearRenderTargetView','Flush',
   'ResourceCopy','ResourceCopyRegion','ResourceUpdateSubresourceUP','DefaultConstantBufferUpdateSubresourceUP',
   'StagingResourceMap','StagingResourceUnmap','DynamicIABufferMapDiscard','DynamicIABufferMapNoOverwrite',
   'DynamicIABufferUnmap','DynamicConstantBufferMapDiscard','DynamicConstantBufferUnmap',
   'DynamicResourceMapDiscard','DynamicResourceUnmap','ResourceIsStagingBusy',
   'CalcPrivateShaderSize','CreateVertexShader','CreatePixelShader','DestroyShader','VsSetShader','PsSetShader',
   'Draw','IaSetTopology','IaSetVertexBuffers','IaSetIndexBuffer','IaSetInputLayout','SetRenderTargets',
   'SetBlendState','SetDepthStencilState','SetRasterizerState','SetViewports','SetScissorRects',
   'CalcPrivateRasterizerStateSize','CreateRasterizerState','DestroyRasterizerState',
   'CalcPrivateBlendStateSize','CreateBlendState','DestroyBlendState',
   'CalcPrivateDepthStencilStateSize','CreateDepthStencilState','DestroyDepthStencilState'],
  'formats':['R8G8B8A8_UNORM','B8G8R8A8_UNORM','B8G8R8X8_UNORM'],
  'scope':'buffer (vertex/index/constant/shader-resource) and linear 32-bit 2D (render-target/shader-resource), default/immutable/dynamic/staging usage; single-mip/single-array/single-sample, initial UP upload, whole/boxed CPU copy and default UP updates, staging read/write/readwrite map, dynamic discard rename and VB/IB no-overwrite; actual cache invalidation before CPU reads and after CPU writes; nonpolling OS idle/event wait for CPU/GPU hazards and DONOTWAIT busy status; unpublished-create hold and retained old backing until outstanding use drains; secure shared KMT allocations, page-aligned exclusive 48-bit GPUVA upper bound, Lock2/Unlock2, make-resident and paging-fence GPU waits, real v2 clear submission through production preparation parser, completion-ordered CPU events, independent resource/view records and deferred destruction with restartable OS cleanup. Runtime-private tokens include owner and nonwrapping ID. Failed post-submit signaling retains GPU buffers. Own KMT device/context requires qualified actual adapter LUID/caps from a future parent; never cast runtime handles.',
  'shader_scope':'owned bounded raw SM4/5 tokens and portable full-signature copies; real CPU validation at creation; nonwrapping owner/ID handles, stage-checked VS/PS selection, null unbind, implicit unbind on destruction, complete failed-create cleanup before SetError reentrancy; bound-pair CPU compilation specialized from caller-supplied actual input-layout/attachment types produces caller-owned independent code. DDI/Win32/KMT and compiler-call boundary run under ARM64 fixtures; actual raw-token/Mesa compilation is qualified separately on host CPU. GPU code upload uses read/execute mappings, zero-padded immutable code and a separate read-only/NX arch-9 program descriptor buffer, cache/lock/residency and paging waits; unique uploads survive source deletion/rebinding and retire only after exact device-idle success, preserving ownership on failures. The bounded Draw producer consumes this pipeline and keeps one last-used program keyed by exact VS/PS IDs and render-target format. Adapter registration and runtime execution remain.',
  'shader_limit':4096,'shader_compiler_boundary_fixture':True,
  'shader_GPU_upload_implemented':True,'shader_upload_limit':32,
  'shader_program_descriptors_implemented':True,'shader_program_descriptor_count':4,
  'shader_program_descriptor_scope':'actual arch-9 VS point/non-point position, optional varying and PS descriptors; exact compiler offsets, 32/64-register class, R48-R63 preload, FTZ and helper flags; uploaded to separate read-only/NX mapping, held with code until exact idle. Descriptor bytes are checked; GPU shader execution is separate.',
  'shader_upload_retirement_scope':'Device-idle CPU wait; pending/informational results are not completion. GPU shader instructions are not executed by fixtures.',
  'raster_submission_implemented':True,'raster_resource_range_snapshots_implemented':True,
  'raster_submission_scope':'actual owned program/resource lookup; KMT command allocation, arch-9 malloc-vertex then fragment stages, immutable code/descriptor binding from compiler metadata, point/non-point entry selection, secondary stride and FAU count validation, resource residency/paging waits, private resource wire data and real command preparation; every referenced resource and program retained until OS event completion including destruction, DISCARD rename and cleanup/signal failures. Explicit descriptor/attribute/FAU/TLS/tiler/framebuffer state is supplied by the caller. The bounded D3D triangle-list Draw/state producer and framebuffer LOAD_IMAGE preload path are implemented separately; general draws, Present and runtime registration remain.',
  'd3d10_first_use_triangle_implemented':True,
  'd3d10_bounded_triangle_draw_implemented':True,
  'd3d10_first_use_triangle_scope':'Bounded non-indexed triangle-list Draw on one single-sample linear RGBA8/BGRA8/BGRX8 target, including prior clear/copy/update/initial data and repeated Draw. One-entry last-used Draw-program cache key is the exact VS object ID, PS object ID and render-target format; a hit reuses compiled/uploaded pipeline and preload program, a failed miss leaves the old entry intact, and pending submissions retain a retired program until completion. SV_VertexID-only VS to SV_Position and no-input PS to target0; no vertex/index buffers or input layout, full-size viewport, scissor, no blending/depth-stencil. The actual Mesa arch-9 LOAD_IMAGE compiler API, internal sampler/texture/resource/DCD/FAU descriptors and fourth RX code BO feed the retained KMT path. Preload ownership survives failure and pending completion. GPU pixel output, general draws, Present or runtime registration are not verified.',
  'draw_program_cache':{'entries':1,'key':['exact_vertex_shader_object_id','exact_pixel_shader_object_id','render_target_format'],
   'cache_hit_reuses_compilation_and_uploaded_program':True,'cache_miss_commits_only_after_successful_submit':True,
   'failed_miss_preserves_previous_cache_entry':True,
   'pending_old_program_retires_after_completion':True,'fixture':'UmdTests.c:TestDrawProgramCache',
   'draw_stack_frame_bytes':user_stack['Umd.c:UmdDraw']['bytes'],
   'draw_stack_frame_kind':user_stack['Umd.c:UmdDraw']['kind']},
  'framebuffer_preload_component_implemented_and_arm64_fixture_executed':True,
  'framebuffer_preload_or_prior_pixel_preservation_implemented':True,
  'hardware_framebuffer_preservation_verified':False,
  'd3d10_bounded_triangle_with_preload_implemented':True,
  'resource_view_limit':4096,'pending_clear_limit':32,'pending_raster_and_clear_limit':32,'retained_discard_generation_limit':32,
  'transfer_execution':'synchronized CPU fallback over actual KMT Lock2/cache calls; not GPU copy or shader rendering',
  'dynamic_gpu_use_executed':False,'dynamic_submission_ownership_fixture_executed':True,'discard_owner_retention_unit_fixture':True,
  'unsupported_resource_features':['mip chains','arrays','multisampling','depth/stencil','other formats','sRGB','typeless','external resource sharing'],
  'queue_pressure_policy':'clear DDI marks this component device removed on internal allocation/queue exhaustion; explicit raster submission returns a failure and preserves outstanding work for completion/retry',
  'runtime_interop_or_swap_chain_implemented':False,'OpenAdapter_or_CreateDevice_DDI_registered':False,
  'Windows_or_Direct3D_runtime_executed':False,'physical_gpu_or_pixel_execution':False,
  'tests':tests['Umd']}
 group_test_obj=tmp/'MaliDdiGroupsTests.obj'
 run([*test_flags,'-DNT_PROCESSOR_GROUPS=1',P/'DdiTests.c','-o',group_test_obj])
 group_exe=tmp/'MaliDdiGroupsTests.exe';group_counter='MtkMaliDdiAssertionCount'
 run([*LD,'/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
  '/opt:ref','/export:'+group_counter+',DATA','/out:'+str(group_exe),*common,group_test_obj])
 print(run([V,P/'test-wdk.py',group_exe,group_counter]))
 group_tests=json.loads(group_exe.with_suffix('.json').read_text())
 group_tests['temporary_artifact_removed_after_verification']=True
 # Board PnP binding also inspects raw/translated Group fields. Compile and run
 # that production path with the alternate WDK processor-group layout too.
 board_group_obj=tmp/'Board-groups.obj'
 run([*flags,'-DNT_PROCESSOR_GROUPS=1',P/'Board.c','-o',board_group_obj])
 board_group_frames={}
 for line in board_group_obj.with_suffix('.su').read_text().splitlines():
  function,size,kind=line.rsplit('\t',2)
  board_group_frames[function.rsplit(':',1)[-1]]={'bytes':int(size),'kind':kind}
 if any(v['bytes']>2048 or v['kind']!='static' for v in board_group_frames.values()):
  raise SystemExit('Processor-group Board exceeds fixed 2 KiB frame limit')
 board_group_test=tmp/'BoardGroupsTests.obj'
 run([*test_flags,'-DNT_PROCESSOR_GROUPS=1',P/'BoardTests.c','-o',board_group_test])
 board_group_exe=tmp/'MaliBoardGroupsTests.exe';board_group_counter='MtkMaliBoardAssertionCount'
 run([*LD,'/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
  '/opt:ref','/export:'+board_group_counter+',DATA','/out:'+str(board_group_exe),*common,board_group_test])
 print(run([V,P/'test-wdk.py',board_group_exe,board_group_counter]))
 board_group_tests=json.loads(board_group_exe.with_suffix('.json').read_text())
 board_group_tests['temporary_artifact_removed_after_verification']=True
 host_group_test=tmp/'HostGroupsTests.obj'
 run([*test_flags,'-DNT_PROCESSOR_GROUPS=1',P/'HostTests.c','-o',host_group_test])
 host_group_exe=tmp/'MaliHostGroupsTests.exe';host_counter='MtkMaliHostAssertionCount'
 run([*LD,'/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
  '/opt:ref','/export:'+host_counter+',DATA','/out:'+str(host_group_exe),*common,host_group_test])
 print(run([V,P/'test-wdk.py',host_group_exe,host_counter]))
 host_group_tests=json.loads(host_group_exe.with_suffix('.json').read_text())
 host_group_tests['temporary_artifact_removed_after_verification']=True
 soc_group_obj=tmp/'SocGroupsTests.obj';run([*test_flags,'-DNT_PROCESSOR_GROUPS=1',P/'SocTests.c','-o',soc_group_obj])
 soc_group_exe=tmp/'MaliSocGroupsTests.exe';soc_counter='MtkMaliSocAssertionCount'
 run([*LD,'/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib',
  '/opt:ref','/export:'+soc_counter+',DATA','/out:'+str(soc_group_exe),*common,soc_group_obj])
 print(run([V,P/'test-wdk.py',soc_group_exe,soc_counter]))
 soc_group_tests=json.loads(soc_group_exe.with_suffix('.json').read_text());soc_group_tests['temporary_artifact_removed_after_verification']=True
 report={'status':'passed','kind':'callable core, OS CPU page-table tracking, PFN-verified shared-backing MDL residency with private execution roots, ordered scheduler, bounded arch-9 command preparation and actual WDK allocation/lifecycle/submission/notification/capability DDIs and provider-backed miniport registration/start/stop/remove and non-destructive D0/D3 bridge code; typed board/power interface adapter with actual MFG controller; unsigned ARM64 WDDM miniport SYS build artifact only, without INF, registration or installation; actual WDM board lower filter with unsigned SYS/Extension INF; actual same-service shared SoC resource owner and cache/lifecycle; unregistered D3D10 resource/transfer/RTV/clear and shader component; no installed device qualification, Windows execution or usable user driver',
  'build_host':sys.platform,'compiler':run([*CC,'--version']).splitlines()[0],
  'mt6789_mfg_domain_and_clock_sequences_implemented':True,'mt6789_composite_power_sequence_implemented':True,'mt6789_pwrap_transport_implemented':True,'mt6358_gpu_rail_enable_implemented':True,'mt6789_mfg_hw_clock_provider_implemented':True,'clock_frequency_programming_implemented':True,'gpu_voltage_pair_programming_implemented':True,'adjusted_opp_springboard_planner_implemented':True,'gpu_opp_transition_executor_implemented':True,'device_adjusted_opp_table_qualified':False,'thermal_AVS_aging_governor_implemented':False,'stock_opp_calibration_implemented':True,
  'stock_opp_calibration_scope':'pure MT6789 stock 45-point unadjusted table to segment/MCL50 upper bound, three explicit AVS words, clamped anchor correction, stock integer interpolation/6.25mV rounding and one-time public per-point aging deltas; actual ARM64 probe blocks compared with synthetic words only. All three AVS read-presence bits are mandatory, including explicit zero words. Invalid or nonmonotonic outputs leave policy untouched. SoC qualification v2 optionally uses this path, retains only its adjusted immutable table, and still requires live firmware ownership, direct DMA, idle and OPP approval. No per-device fuse reading, installed qualification or thermal/battery governor.','dvfs_policy_implemented':False,'full_board_power_implemented':False,
  'mt6789_board_power_controller_implemented':True,
  'board_power_controller_scope':'production composition of actual WACS2/rail, SPM domain, reference gate, PLL/parent/BG3D and composite power cores. Before enabling rails the actual PLL rate and both selector voltages need explicit device OPP approval; before reporting ON the complete sequence/rails/clocks and OPP are rechecked. Borrowed rail original voltages are preflighted before any OFF bus/clock/domain mutation. Idempotent set owns exactly one internal reference; OFF/unknown Ready performs no MMIO. Partial child mutations retain all journals and quarantine further operations; no guessed reset/recovery or installed OPP. The typed Windows board/power adapter is compiled and executed separately with this controller; PnP dispatch, same-PDO IRQ binding and real WDM host attachment are implemented separately. Shared OS resource mapping, operation mutex and cache callbacks are implemented by Soc.c. Real firmware exclusion/direct-DMA/OPP qualification, physical IRQ/power proof and Windows execution remain.',
  'board_power_controller_mock_configurations':16,'board_power_controller_mock_on_off_cycles':48,
  'board_power_controller_partial_failure_cases':9,
  'board_power_controller_miniport_provider_binding_implemented':True,
  'board_interface_provider_adapter_implemented':True,'board_power_interface_provider_adapter_implemented':True,
  'board_provider_PnP_and_shared_resource_backend_implemented':True,
  'board_provider_PnP_resource_binding_implemented':True,'board_provider_PnP_query_routing_implemented':True,
  'board_provider_shared_resource_backend_implemented':True,
  'board_provider_PnP_dispatch_implemented':True,
  'board_provider_PnP_stop_cancel_restart_implemented':True,
  'board_provider_PnP_forced_removal_MMIO_fence_implemented':True,
  'board_provider_PnP_host_attachment_and_remove_lock_implemented':True,
  'board_provider_PnP_dispatch_restart_cycles':3,
  'board_provider_PnP_dispatch_start_failure_cases':6,
  'board_provider_PnP_dispatch_forced_removal_configurations':4,
  'board_host_filter':host_package,
  'board_host_driver_entry_and_AddDevice_implemented':True,
  'board_host_interface_device_pins_implemented':True,
  'board_host_remove_IRP_lock_rundown_implemented':True,
  'board_host_deferred_reference_cleanup_and_poison_quarantine_implemented':True,
  'board_host_parent_resource_query_implemented':True,
  'board_host_parent_resource_provider_implemented':True,
  'wdk_arm64_host_processor_groups':host_group_tests,
  'wdk_arm64_soc_processor_groups':soc_group_tests,
  'soc_resource_owner_implemented':True,'soc_parent_factory_implemented':True,'soc_MMIO_mapping_and_operation_mutex_implemented':True,
  'soc_root_device_power_lifecycle_implemented':True,'soc_noncoherent_table_cache_clean_implemented':True,
  'soc_device_qualification_backend_implemented':False,'soc_qualified_adjusted_OPP_installed':False,
  'soc_scope':'actual HANA678F root FDO in the same MtkMaliBoard service, five OS-assigned exclusive DT MMIO regions with PWRAP excluded, all-or-none mapping, fixed-GUID ExGetPrmInterface client for shared PMIC transactions, one PRM module lock across each Begin/Step/Ack journal, real parent factory and device lifetime pins, shared mutex spanning PMIC/SPM/clock operations, real controller composition, nonpaged table MDL cache clean through KeFlushIoBuffers, START/STOP/cancel/restart/removal and parent D3/D0 fences. Qualification is a mandatory kernel input with live owner/direct-DMA/idle/OPP proof; no production device qualification is installed, so hardware acquisition remains refused by default. ARM64 execution uses OS/MMIO and qualification fixtures; no Windows or device execution.',
  'soc_direct_pwrap_mapping_implemented':False,'soc_shared_prm_pmic_client_implemented':True,
  'soc_shared_pmic_prm_interface_import':'ExGetPrmInterface',
  'board_host_scope':'real WDM DriverEntry/AddDevice, exact ACPI hardware-id check, synchronous same-stack or same-service root parent-resource lookup/Open, deferred requery after lower START when AddDevice precedes owner publication, immutable config and initial-ref ownership, IoCreateDevice and IoAttachDeviceToDeviceStackSafe, raw/translated START through Board.c, independent interface ObReferenceObject pins, completion-held IO_REMOVE_LOCK for ordinary/power requests, lower REMOVE completion before lock drain/detach/IRP completion, deferred last-reference cleanup and live/poisoned/invalid-owner retention. Actual code plus real MFG controller runs in both WDK IRQ layouts with Windows objects/I/O manager/parent resources/MMIO mocked. A separate unsigned board lower-filter SYS and device-specific Extension INF are linked and entry-map/checksum/import validated. Shared SoC resource owner is implemented separately; actual device qualification and Windows/device execution remain; this is not a graphics miniport SYS or D3D UMD.',
  'board_provider_PnP_dispatch_scope':'actual WDM stack copy and completion routine, nonalertable lower-completion drain, lower-success-only START resource publication, reversible query-stop/remove admission fence, matching lower-success-only cancel, live reference/lease/journal stop veto, restart on the same provider, STOP after failed START, synchronous unhandled PnP status/information preservation, forced-removal withdrawal and hardware-gate drain before the lower bus, post-removal MMIO prohibition and live/poisoned lease quarantine. Immediate, pending, cancelled and failed lower completion and hardware-gate interleavings are executed as ARM64 PE in both WDK IRQ layouts. The owning WDM filter, remove-lock rundown, device reference pins and deferred quarantine storage are implemented and tested separately below. Actual shared-resource/cache provider is implemented by Soc.c; device address/OPP qualification and Windows I/O manager concurrency remain.',
  'board_provider_PnP_invalid_resource_cases':44,'board_provider_PnP_invalid_query_cases':23,
  'board_provider_PnP_group_invalid_resource_cases':45,'board_provider_PnP_group_invalid_query_cases':24,
  'board_provider_PnP_memory_positions_tested':4,'board_provider_PnP_controller_D3_D0_cycles':1,
  'board_provider_PnP_scope':'actual WDM IRP and CM_RESOURCE_LIST routing/copy/validation. Same-PDO raw GSIV 428/429/430 pair with OS translated GPU/MMU/JOB descriptors, the same production DDI interrupt validator, explicit miniport query proof, by-value GUIDs, independent interface references and reference/lease/journal-aware unbind/close. Unknown or already handled queries stay untouched. Real controller D3/D0 executes after successful query; OS lists/IRPs and hardware are virtual fixtures. START/STOP/cancel/lower completion and forced-removal fencing are implemented separately in the same production code. Host filter attachment and remove rundown are implemented separately below. Shared PMIC/clock mapping, operation mutex and cache callbacks are implemented by Soc.c; firmware exclusion/direct-DMA/OPP qualification and Windows execution remain.',
  'board_provider_mock_configurations':16,'board_provider_mock_D3_D0_cycles':48,
  'board_provider_malformed_configuration_cases':18,'board_provider_acquisition_failure_cases':13,
  'board_provider_adapter_scope':'actual typed board Acquire/Ready/Release and separate power-interface SetPower invoke the production MFG controller under mandatory shared-resource access locks. Independent interface references, partial-acquire cookie retention, nonwrapping client generation, exact success status, idempotent power, unknown-state Ready/MMIO rejection, borrowed-voltage and poisoned-resource retention, quarantine notification and close lifetime are executed as ARM64 PE. Resource association/cache/IRQ/OPP providers are fixtures; PnP IRQ pairing and query routing helpers are implemented and tested; actual PnP dispatch/completion/stop-cancel-restart and forced-removal fencing are implemented below; real WDM host attachment and deferred cleanup are implemented separately below; the shared resource backend is implemented by Soc.c; device qualification and Windows execution remain.',
  'inherited_gpu_rail_voltage_handoff_guard_implemented':True,
  'inherited_gpu_rail_handoff_scope':'original selector voltage captured once at acquisition; preflight every acquired borrowed rail before either release, with selector/DA/enabled-status proof. Unrestored mixed pairs remain fully held for authorized restoration. Busy is retryable, PMIC transport failure poisons and retains ownership. Does not restore frequency automatically or implement board D3/thermal policy.',
  'power_sequence_scope':'provider-backed rail0/rail1, four SPM domains, reference clock, mux/BG3D and DCM/bus-idle sequence; bounded idle wait, serialized reference count and failure ownership. Production SoC removes the PWRAP MMIO mapping and uses a fixed-GUID PRM client for shared GPU/UFS WACS2 transactions; the portable MFG PMIC core keeps raw WACS2 only as a separate, strictly exclusive transport, with no fallback between paths. VGPU/VSRAM_GPU enable/status/voltage reads retain inherited-rail preservation. Actual MFGPLL prepare/unprepare, nearest-PCW stopped-PLL programming and rate decoding, reference gate, no-update parent mux and BG3D gate are implemented. VGPU/VSRAM pair programming validates board approval of old/intermediate/final pairs and matches stock 8mV/us up, 4mV/us down settle calculation. Descending adjusted-table validation, exact 200mV springboards and bounded voltage/rate planning are implemented, including equal-voltage plateaus. The explicitly authorized idle-only executor uses real pair/PMIC and stopped-PLL cores, gates BG3D and moves through Sub/Main, commits stable index only on success, and retains ownership with poisoned state after partial failures. The production controller now composes these actual power/rail/clock cores with pre-enable OPP qualification, pre-shutdown borrowed-rail restoration checks, repeated ON/OFF and retained partial failures. Board lease/resource/OS binding, device calibration read/qualification, thermal/battery governor, whole-device suspend/resume and physical power proof remain. The pure stock segment/MCL50/AVS/aging transform is implemented separately with synthetic-original-code comparisons.',
  'native_mfg_asan_ubsan':mfg_result,'stock_mfg':json.loads((B/'mali-stock-power-tests.json').read_text()),
  'native_core_asan_ubsan':native_result,'native_execution_asan_ubsan':execution_result,'native_commands_asan_ubsan':commands_result,'stock_encoder':json.loads((B/'mali-stock-tests.json').read_text()),
  'mesa_arch9_format':json.loads((B/'mali-command-format-tests.json').read_text()),
  'wdk_arm64':tests,'production_stack_frames':stack_usage,
  'd3d10_resource_clear_component_implemented':True,'d3d10_resource_transfer_component_implemented':True,'umd_resource_clear':user_component,
  'wdk_arm64_processor_groups':group_tests,'processor_group_production_stack_frames':group_frames,
  'wdk_arm64_board_processor_groups':board_group_tests,'board_processor_group_production_stack_frames':board_group_frames,
  'production_coff_members':[p.name for p in objects],
  'implemented_ddis':['CreateProcess','DestroyProcess','CreateDevice','DestroyDevice',
   'CreateContext','DestroyContext','SetRootPageTable','GetRootPageTableSize',
   'SubmitCommandVirtual','PreemptCommand','QueryCurrentFence','ResetEngine','BuildPagingBuffer','InterruptRoutine','DpcRoutine',
   'CreateAllocation','DestroyAllocation','OpenAllocation','CloseAllocation','DescribeAllocation','SetAllocationBackingStore','QueryFeatureSupport',
   'QueryAdapterInfo','GetNodeMetadata','QueryDependentEngineGroup','QueryEngineStatus','QueryInterface'],
  'wdk_execution_test_groups':['non-destructive runtime power pause with live OS objects/VM roots/interfaces, repeated same-context submissions after full Core reset, powered-off stable queries and reset rejection; concurrent dispatch preempt publication, IRQ/thread/cache/start/identity failures and retry','actual DDI receives v2 private data larger than the old execution packet: 20 resources through real preparer, MDL/MMU, submission and completion','original borrowed-rail voltage capture, preflight the entire mixed pair before either release, approved restore, actual selector/DA/status mismatch, transport failure and authorization retention','MT6789 WACS2, GPU regulator commands/voltage reads, inherited rail preservation and transport/rail timeout retention; actual PLL/ref/mux/BG3D MMIO provider, stopped-PLL rate programming, authorized voltage pairs/stock settle timing; adjusted OPP validation/springboards/planning and real PMIC/stopped-PLL executor, both directions/equal-voltage plateaus, failed authorization/write/readback retention and stable-index commit; absent-update handling, failed release retention and combined power integration; four-domain SPM/bus protection, mux/BG3D and composite rail/reference/domain/clock/DCM power sequence, counted references, bounded retries and retained ownership on failure','managed GPU/JOB/MMU mask/ISR/DPC/worker, translated IRQ validation and common maximum-DIRQL spin lock across distinct source levels; deferred higher-level source and other-CPU disconnect interleavings; no DIRQL PowerReady/mutex/release, RAW preserved until Poll; both WDK processor-group layouts; failed connect/disconnect/rearm ownership and reset masking','FIFO and multi-stage execution','context generations/root/reference lifetime',
   'malformed packet rejection before MMIO','DMA-boundary preemption and 32-bit fence wrap',
   'dispatch preemption arriving during synchronized completion notification',
   'fault/timeout retention and failed reset','notification failure retry without duplicate delivery',
   'reset-aborted fence excludes unstarted jobs','reset success followed by failed GPU restart',
   'separate immutable prepared command range','WDK paging and wrong-IRQL rejection',
   'actual worker loop with mocked OS threads/events: idle sleep, completion, fault and stop retry',
   'arch-9 command graph bounds/format/reserved fields/cycles/dependencies and immutable rebuild',
   'fragment MFBD bounds/targets/ZS/render orders, 64-job immutable shadow, same-slot capability checks, full MDL/PFN/private-MMU lifetime through completion and retained fault/reset',
   'real preparation + DDI + scheduler: snapshot mutation, all backend failure unwinds and 33 resident leases',
   'actual completion notification/preemption/reset controls prepared-memory release',
   'adapter-owned shared allocations survive creator process exit; OS handle resolution and read-only opens',
   'atomic allocation/open batch failure cleanup and extension of existing resources',
   '4096 allocation/open/reference capacity and monotonic reference token exhaustion',
   'WDDM 3.1 and 3.2 feature gating, referenced interface release and bounded shared-backing CPU copies',
   'actual scheduler completion/notification failure/reset retention of allocation references',
   'OS table initial update, explicit invalidation, eviction, unknown entries and physical/object generation reuse',
   'actual MDL-backed preparation with PFN mismatch, read-only and command/table/private-page alias rejection',
   'private root detached before completion; detach and reset failures retain backing',
   '33 MDL leases, failed preemption notification, 64 KiB unaligned input over 17 pages and 64 write targets',
   '64 scattered top-level target regions with 196 private tables and complete release',
   'v2 arch-9 compute, explicit resource list, whole-range MDLs and exact PFN/OS permission/offset checks; RO, RX and RW mappings with no table/MMIO/unlisted/source exposure',
   'physical W^X across live leases, same-allocation aliases, second-page PFN/protection mismatch, hash growth through the 512 MiB budget, full resource unwind and reset retention',
   'bounded adapter/MMU/node/private ABI queries; invalid buffer/flags/index rejection before output mutation',
   'two-phase segment count query and caller descriptor stride/padding preservation',
   'feature interface GUID/adapter UID/version/reference lifetime and reentrant negotiation during start',
   'full software bundle start/stop, failed-start unwind and retained interface/thread-wait/close retry',
   'actual DriverEntry/DxgkInitialize table and Add/Start/Stop/Remove with OS/board boundaries mocked; early resource rejection, pending QUERY_INTERFACE, live-object stop refusal, IRQ/DPC wrapper lifetime, retained reset/thread/callback/lease/map/interface failures and retry; typed hAdapter query/process DDIs; optional separate-GUID power interface, actual SetPowerState D0/D3, live allocation/root/context preservation across three cycles and subsequent submissions; provider/query/IRQ/thread/reset/cache/callback/Ready/identity failures retain resources, unknown-power MMIO rejection, late DPC acknowledgement and D3/unknown Stop without GPU MMIO',
   'typed board/power interface provider with actual MFG controller/PMIC/SPM/clocks; resource/access lease failures, quarantine, references, stale cookie rejection, withdrawal and hardware OFF release; OS ownership/cache/IRQ inputs are fixtures',
   'actual runtime-bundle allocation, OS PTE writes, locked MDLs, private root, submit and failed notification retention'],
  'arch9_compute_resource_submission_implemented':True,
  'arch9_fragment_resource_submission_implemented':True,
  'arch9_raster_command_submission_implemented':True,
  'raster_command_scope':'actual malloc-vertex/fragment producer and owned user-component KMT submission are implemented separately above. Arch-9 type-11 malloc-vertex (384 bytes), type-7 tiler (256 bytes) and type-12 fullscreen (128 bytes) parsing with variable 128-byte-aligned shadow offsets, exact submit/seal extent and six retained private pages. Validates indexed ranges, primitive/scissor bounds, inline shader environments, allocation stride, tiler R/W, DCD/blend/depth/occlusion buffer ranges, source-extent overlap and immutable snapshots before residency callbacks. Actual preparer/MDL/private-MMU/scheduler code executes with mocked OS and GPU boundaries. Tile/scissor arrays and unknown job kinds remain rejected; shader/indirect containment is the private MMU, not instruction validation. A bounded D3D triangle-list Draw producer, its last-used program cache and framebuffer LOAD_IMAGE preload path are implemented; Windows runtime and GPU pixel execution remain.',
  'arch9_fixed_function_clear_builder_implemented':True,
  'fixed_function_clear_scope':'real arch-9 single-sample full-surface RGBA/BGRA/X8 UNORM clear command, MFBD/RT/tiler-context/256-KiB heap/sample descriptors and v2 resource wire data; integer binary32 conversion matches the original native Mesa float_to_fixed helper and independent XML packing. Explicit page-padded 2D allocation and actual MDL/private-MMU/DDI/scheduler completion/fault/reset lifetime are executed under ARM64 emulation. No registered D3D UMD, DirectX application or hardware pixel execution.',
  'fragment_resource_scope':'actual arch-9 type-9 command parser and canonical shadow payload, same-slot JS_FEATURES gating, inclusive tile bounds, five render orders and readable 128-byte MFBD plus one-to-eight 64-byte render targets and optional 64-byte ZS/CRC extension. Uses the existing full MDL/PFN/owner/permission proof, private MMU, cache and completion/fault/reset retention. Tile-enable maps are rejected. Indirect descriptors/frame shaders stay untrusted inside the private MMU; this is not a D3D UMD, pixel-output proof or hardware rasterization execution.',
  'compute_resource_limits':{'version':2,'max_resources':128,'max_bytes':536870912,'max_jobs':64,'max_stages':16},
  'compute_resource_scope':'actual command parser, full-range shared-backing MDL/PFN/ownership proof, OS R/W/X permission subset, live-lease physical W^X, isolated per-submit MMU, cache and completion/fault/reset lifetimes. No shader ISA/indirect descriptor validation; containment is the MMU and status pages remain GPU writable. Does not implement a D3D UMD or prove hardware shader execution.',
  'adapter_queries_implemented':True,
  'adapter_query_types':['DRIVERCAPS','GPUMMUCAPS','PAGETABLELEVELDESC','QUERYSEGMENT4','NUMPOWERCOMPONENTS','HISTORYBUFFERPRECISION','UMDRIVERPRIVATE'],
  'adapter_query_scope':'bounded noncoherent GPU MMU/aperture, one OTHER node, DMA-boundary preemption, private ABI queries advertise command versions 1/2 and supported JM write/cache/compute/fragment/tiler/malloc-vertex/fullscreen feature bits; no D3D feature levels',
  'software_backend_lifecycle_bundle_implemented':True,
  'managed_interrupt_isr_dpc_worker_implemented':True,'physical_gpu_irq_binding_implemented':False,
  'multi_line_common_dirql_lock_implemented':True,
  'translated_interrupt_resource_validation_implemented':True,
  'interrupt_domain_scope':'exactly three OS translated, level-triggered line descriptors in processor group 0; distinct vectors and nonzero affinities; derive highest assigned DIRQL, raise before common nonpaged spin lock, restore original IRQL after unlock; recheck connection under the lock. Resource association, OS ISR connection and final rundown remain parent responsibilities. Notifications remain in the fixed DxgkCbSynchronizeExecution domain.',
  'interrupt_scope':'explicit managed Core mode; actual translated IRQ descriptors derive a highest-DIRQL common spin lock for all GPU/JOB/MMU mask/state accesses; real ISR and DPC worker wake, serialized RAW Poll/notification, read-back mask confirmation, reset/start gating and retained connection/stages on failure. Parent must validate/associate and connect all three physical lines to the ISR, own resources/power and drain OS ISR/DPC calls. Physical binding is not established by this library.',
  'non_destructive_runtime_suspend_resume_implemented':True,
  'runtime_power_transition_scope':'idle-only admission pause preserves live processes/devices/contexts/allocations/VM roots and feature-interface references; atomic dispatch preempt race rejection, synchronized IRQ mask/disconnect, worker drain, cache flush/core stop. Power-off queries use stable cached capabilities and reset/submission are blocked. Resume validates the original full GPU identity, restarts worker/IRQs before admission and retains failed transitions for retry. Miniport D0/D3 now closes/drains MMIO callbacks around an optional separate-GUID board power interface; failed board transitions become unknown until an explicit proved D0. Actual physical power provider, OS final callback lifetime, display/wake and full system suspend remain separate.',
  'runtime_power_transition_executed_with_live_objects':True,
  'physical_board_suspend_resume_executed':False,
  'backend_lifecycle_scope':'caller-owned zeroed nonpaged bundle, first-member hAdapter, actual VM/MDL/preparer/worker initialization, mandatory validation of the three translated IRQ descriptors before managed Worker startup, IRQ arm after worker start, mask/disconnect before worker stop and retryable stage retention; parent owns Core/board/resources/physical IRQ and OS DDI/ISR/DPC rundown',
  'miniport_entrypoint_and_lifecycle_implemented':True,
  'miniport_sys_artifact_built':True,'miniport_driver_package':miniport_package,
  'miniport_INF_created_or_installed':False,'miniport_installed_or_Windows_loaded':False,
  'miniport_board_provider_interface_implemented':True,'miniport_board_provider_implemented':False,
  'miniport_power_callback_bridge_implemented':True,'miniport_power_interface_implemented':True,
  'miniport_power_provider_implemented':False,
  'miniport_power_callback_windows_failure_policy_qualified':False,
  'miniport_power_callback_failure_scope':'WDDM SetPowerState is required not to fail for valid OS power requests. The offline bridge reports unproved transitions and preserves resources instead of returning a false success. An actual qualified board provider, OS power sequencing and hardware-failure policy are required before Windows registration.',
  'miniport_power_scope':'actual registered D0/D3 callback preserves OS objects, waits for idle, stops Core/worker/IRQs and drains MMIO callbacks before the optional separately referenced board power interface. Failed provider status blocks MMIO in unknown state until an explicit proved D0. Resume revalidates capabilities before admission; late DPC acknowledges OS without hardware access. Stop from D3/unknown retains and releases resources without GPU reset/MMIO. Provider fixtures only; actual physical rails/clocks/OPP ownership and Windows execution remain.',
  'miniport_ddis':['DriverEntry','AddDevice','StartDevice','StopDevice','RemoveDevice','InterruptRoutine','DpcRoutine','QueryChildRelations','QueryChildStatus','QueryDeviceDescriptor','DispatchIoRequest','SetPowerState(D0/D3 with optional board power provider)','Unload'],
  'miniport_scope':'typed WDDM 3.1 DxgkInitialize table; first-member hAdapter; validated GPU MMIO and three translated IRQs before mapping/power; synchronous PnP QUERY_INTERFACE reference; real DxgkCbMapMemory, explicit retained board lease, Core and runtime startup; masked IRQ/software worker teardown, callback drain, Core stop/reset, board release, unmap and interface dereference in order; failures retain resources for Stop retry; optional second power-interface GUID with independent query/ref/deref; non-destructive D0/D3 bridge, unknown-power guard, late DPC acknowledgement and off-state Stop; no display children. D3 without the optional provider is rejected before MMIO. OS/resource association, actual board/power provider and Windows registration remain unexecuted.',
  'render_miniport_registered':False,'command_validation_implemented':False,
  'bounded_command_validation_implemented':True,
  'command_validation_scope':['G57 arch-9 immediate 8/16/32/64 writes','system timestamp writes','canonical clean/invalidate cache jobs','arch-9 compute payload with explicit process-owned R/W/X resource ranges under a private MMU root','arch-9 fragment payload with inclusive bounds, five orders, up to eight MFBD targets and optional ZS/CRC extent under the same private MMU','arch-9 variable-sized malloc-vertex/tiler/fullscreen payloads and mixed chains with bounded indices, tiler, shader-environment, blend/depth/occlusion and source extents'],
  'command_validation_limits':{'jobs':64,'stages':16,'dma_bytes':65536,'shadow_alignment':128,'shadow_bytes':24576,'max_job_bytes':384,'variable_job_offsets':True,'resource_version':2,'resources':128,'resource_bytes':536870912,'private_bytes':2320},
  'allocation_lifecycle_ddis_implemented':True,
  'allocation_scope':{'kinds':['buffer','linear 32-bit 2D'],'max_allocation_bytes':1073741824,'max_batch':256,
   'max_allocations':4096,'max_opens':4096,'max_references':4096,'object_references_are_gpu_residency':False},
  'allocation_gpu_residency_implemented':True,'shared_backing_store_cpu_access_implemented':True,
  'parent_vidmm_mapping_residency_backend_implemented':True,
  'os_cpu_page_table_tracking_implemented':True,'private_submission_root_implemented':True,
  'residency_scope':{'memory':'OS-shared CPU-visible backing, fresh probe-locked MDLs, PFN equality with held GPUVA mappings',
   'commands':'v1 write/timestamp/cache; v2 arch-9 compute/fragment/malloc-vertex/tiler/fullscreen plus explicit buffers','address_model':'parent-proven direct system physical required',
   'max_table_records':4096,'max_leases':33,'max_data_pages_per_lease':81,'max_private_table_pages_per_lease':1223,'max_resource_ranges_per_lease':128,'max_resource_pages_per_lease':131072,'max_resource_bytes_per_lease':536870912,
   'mdls_are_real_production_code':True,'seh_compiled':True,'windows_exception_dispatch_executed':False,
   'general_buffer_residency_implemented':True,'general_graphics_residency_implemented':False,
   'permissions':'exact held OS GPUVA and allocation offset, full-range locked MDL PFNs, R/W/X subset and physical W^X across live leases',
   'cache':'all resource MDLs cleaned before GPU access; writable outputs invalidated before release after completion/reset',
   'compute_range_test_max_pages':64,'range_boundaries_tested':[2097152,1073741824,549755813888]},
  'command_preparation_ddi_integration_executed':True,
  'passive_worker_implemented':True,'real_kernel_concurrency_executed':False,'hardware_directx_implemented':False,'windows_runtime_executed':False,
  'gpu_runtime_executed':False,'sources':FrozenSources,'inputs_unchanged_during_execution':True,
  'artifacts':{str(f.relative_to(R)):{'bytes':f.stat().st_size,'sha256':digest(f)} for f in
    [library,user_library,user_dll,host_sys,miniport_sys,B/'MtkMaliBoard.inf',B/'MtkMaliSoc.inf',*[B/('Mali'+name+'Tests.exe') for name in tests]]},
  'commands':commands,
  'remaining':['miniport device-specific INF/package, Windows registration/installation, physical IRQ association and device qualification for the implemented shared SoC owner and StartDevice/runtime bundle',
   'tiler/vertex pipeline and graphics/fragment descriptor/resource translation through a usable UMD; actual shader and indirect-resource hardware behavior remains unexecuted',
   'qualify the implemented shared SoC resource owner against secure-world/other-client ownership and real device DMA; supply actual adjusted OPP/thermal/battery policy through its mandatory kernel qualification input; no production qualification backend is installed',
   'physical board IRQ connection and OS-final ISR/DPC rundown; implement the actual board power provider for the runtime/Miniport D0/D3 bridge, qualify shared rails/clocks/OPP suspend/resume and WDDM power-callback no-failure/OS hardware-failure policy',
   'register and complete D3D user-mode device beyond the implemented offscreen resource/RTV/clear and bounded triangle-list Draw/preload path; runtime sharing/interop and swap chain/present, general draw-time resource/FAU/tiler/fragment descriptors, uploaded shader and actual framebuffer pixel execution, general draws, broader formats/caps and queries; Windows Vulkan ICD and WSI',
   'proven MT6789 board power/coherency/physical address ownership and suspend/resume',
   'real Windows/Linux GPU execution']}
 CurrentSources={str(f.relative_to(R)):digest(f) for f in SourceFiles}
 if CurrentSources!=FrozenSources:raise SystemExit('Mali verification inputs changed during build/test execution')
 (B/'mali-verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('Mali core, unsigned ARM64 miniport SYS build artifact and WDM board lower-filter SYS/INF verified; miniport not packaged, registered or installed; no usable graphics driver/UMD produced')
