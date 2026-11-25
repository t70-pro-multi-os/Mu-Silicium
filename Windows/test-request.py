#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run ARM64 Windows machine code with kernel/WDF mocks. No tablet or Windows OS."""
from pathlib import Path
import argparse,hashlib,json,re,struct,sys,time
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X30
parser=argparse.ArgumentParser()
parser.add_argument('image')
parser.add_argument('--report')
parser.add_argument('--production-source')
parser.add_argument('--test-source')
parser.add_argument('--test-name',default='MtkI2c')
parser.add_argument('--framework-entry-map',help='execute the real KMDF FxDriverEntry in IMAGE using this linker map')
parser.add_argument('--framework-entry-case',choices=('bind-failure','create-success','create-failure'),default='bind-failure',
                    help='select the bounded KMDF loader/client scenario; requires --framework-entry-map')
args=parser.parse_args()
assert args.framework_entry_map or args.framework_entry_case=='bind-failure', '--framework-entry-case requires --framework-entry-map'
p=Path(args.image); data=p.read_bytes(); pe=struct.unpack_from('<I',data,0x3c)[0]
assert data[:2]==b'MZ' and data[pe:pe+4]==b'PE\0\0'
machine,count=struct.unpack_from('<HH',data,pe+4); assert machine==0xaa64
optional=pe+24; optsize=struct.unpack_from('<H',data,pe+20)[0]
assert struct.unpack_from('<H',data,optional)[0]==0x20b
entry=struct.unpack_from('<I',data,optional+16)[0]; base=struct.unpack_from('<Q',data,optional+24)[0]
size,headers=struct.unpack_from('<II',data,optional+56)
imports_rva,imports_size=struct.unpack_from('<II',data,optional+112+8)
image_sections=[]
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM); u.mem_map(base,(size+4095)&~4095); u.mem_write(base,data[:headers])
def invalid_memory(uc,access,address,size,value,user_data):
    print("ARM64 invalid memory access: kind=%d address=%s size=%d value=%s pc=%s" %
          (access,hex(address),size,hex(value),hex(uc.reg_read(UC_ARM64_REG_PC))),
          file=sys.stderr)
    return False
u.hook_add(UC_HOOK_MEM_INVALID,invalid_memory)
for i in range(count):
    off=optional+optsize+i*40
    virtual_size,rva,raw_size,raw=struct.unpack_from('<IIII',data,off+8)
    characteristics=struct.unpack_from('<I',data,off+36)[0]
    assert rva+max(raw_size,virtual_size)<=size
    image_sections.append((rva,max(raw_size,virtual_size),characteristics))
    if raw_size: u.mem_write(base+rva,data[raw:raw+raw_size])

def map_symbols(map_path, wanted):
    found={name:[] for name in wanted}
    row=re.compile(r'^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}$')
    for line in Path(map_path).read_text(errors='strict').splitlines():
        fields=line.split()
        if len(fields)>=3 and row.fullmatch(fields[0]) and fields[1] in found:
            found[fields[1]].append(int(fields[2],16))
    result={}
    for name,values in found.items():
        assert len(values)==1, ('map symbol must appear exactly once',name,values)
        assert values[0]>=base, ('map symbol precedes PE image base',name,hex(values[0]))
        rva=values[0]-base
        assert rva<size, ('map symbol outside PE image',name,hex(rva),hex(size))
        result[name]=rva
    return result

def image_range(address,length,required_characteristics=0):
    if address<base or length<=0:
        return False
    rva=address-base
    return any(start<=rva and rva+length<=start+span and
               (flags & required_characteristics)==required_characteristics
               for start,span,flags in image_sections)
stack=0x70000000; end=0x60000000
u.mem_map(stack,0x200000);u.mem_map(end,4096)
imports=[]
import_stubs={}
if imports_rva:
    # COFF retains a few uncalled production power/MMIO paths. Map their IAT
    # entries to traps and fail if any real kernel import is executed.
    trap=end+0x1000; u.mem_map(trap,0x10000)
    def read32(a): return struct.unpack('<I',u.mem_read(a,4))[0]
    def read64(a): return struct.unpack('<Q',u.mem_read(a,8))[0]
    def cstr(a):
        out=bytearray()
        while (v:=bytes(u.mem_read(a+len(out),1)))!=b'\0': out+=v
        return out.decode()
    desc=base+imports_rva
    while read32(desc+12):
        dll=cstr(base+read32(desc+12)); names=base+read32(desc); iat=base+read32(desc+16); i=0
        while (name:=read64(names+i*8)):
            assert not name>>63, 'Unexpected ordinal import'
            import_name=dll+'!'+cstr(base+name+2)
            imports.append(import_name)
            stub=trap+len(imports)*4
            import_stubs[stub]=import_name
            if args.framework_entry_map:
                u.mem_write(stub,struct.pack('<I',0xd65f03c0))  # RET X30
            u.mem_write(iat+i*8,struct.pack('<Q',stub)); i+=1
        desc+=20
    if not args.framework_entry_map:
        def forbidden(uc,address,size,unused):
            raise AssertionError('Test called real kernel import: '+imports[(address-trap)//4-1])
        u.hook_add(UC_HOOK_CODE,forbidden,begin=trap,end=trap+0xffff)
u.reg_write(UC_ARM64_REG_SP,stack+0x200000-16);u.reg_write(UC_ARM64_REG_X30,end)

if args.framework_entry_map:
    framework_targets=('MtkButtons','MtkI2c','MtkGpio','HimaxTouch','MtkUsb')
    assert args.test_name in framework_targets, ('unsupported KMDF framework-entry target',args.test_name)
    assert p.stem.lower()==args.test_name.lower(), ('image name does not match requested KMDF target',p.name,args.test_name)
    if args.framework_entry_case!='bind-failure':
        assert args.test_name=='MtkButtons', 'successful binding fixture is only verified against MtkButtons.sys'
        assert p.name.lower()=='mtkbuttons.sys', ('success path requires the actual MtkButtons production image',p.name)
    wanted_symbols=['FxDriverEntry','DriverEntry','__security_cookie','__security_cookie_complement',
                    '__security_init_cookie','WdfBindInfo','WdfFunctionCount','WdfFunctions_01033',
                    'WdfDriverGlobals','WdfDriverMiniportUnloadOverride','WdfMinimumVersionRequired',
                    'WdfStructureCount','WdfStructures','WdfClientVersionHigherThanFramework',
                    'WdfDriverStubRegistryPath','WdfDriverStubRegistryPathBuffer','WdfDriverStubDriverObject',
                    '?FxStubBindClasses@@YAJPEAU_WDF_BIND_INFO@@@Z','?FxStubInitTypes@@YAJXZ',
                    '?FxStubDriverEntryFailure@@YAXXZ','?FxStubUnbindClasses@@YAXPEAU_WDF_BIND_INFO@@@Z',
                    '?__KMDF_CLASS_BIND_START@@3U_MARKER_TYPE@@A',
                    '?__KMDF_CLASS_BIND_LAST_BOUND@@3PEAU_WDF_CLASS_BIND_INFO@@EA']
    if args.framework_entry_case!='bind-failure':
        wanted_symbols.append('ButtonsDeviceAdd')
    symbols=map_symbols(args.framework_entry_map,wanted_symbols)
    fx_entry=symbols['FxDriverEntry']; driver_entry=symbols['DriverEntry']
    buttons_device_add=symbols.get('ButtonsDeviceAdd')
    cookie_rva=symbols['__security_cookie']
    cookie_complement_rva=symbols['__security_cookie_complement']
    cookie_init_rva=symbols['__security_init_cookie']
    bind_info=base+symbols['WdfBindInfo']
    function_count_cell=base+symbols['WdfFunctionCount']
    functions_pointer_cell=base+symbols['WdfFunctions_01033']
    driver_globals_cell=base+symbols['WdfDriverGlobals']
    miniport_unload_cell=base+symbols['WdfDriverMiniportUnloadOverride']
    registry_path_image=base+symbols['WdfDriverStubRegistryPath']
    registry_buffer_image=base+symbols['WdfDriverStubRegistryPathBuffer']
    driver_object_image=base+symbols['WdfDriverStubDriverObject']
    fx_bind_classes=base+symbols['?FxStubBindClasses@@YAJPEAU_WDF_BIND_INFO@@@Z']
    fx_init_types=base+symbols['?FxStubInitTypes@@YAJXZ']
    fx_driver_entry_failure=base+symbols['?FxStubDriverEntryFailure@@YAXXZ']
    fx_unbind_classes=base+symbols['?FxStubUnbindClasses@@YAXPEAU_WDF_BIND_INFO@@@Z']
    class_bind_start=base+symbols['?__KMDF_CLASS_BIND_START@@3U_MARKER_TYPE@@A']
    class_bind_last=base+symbols['?__KMDF_CLASS_BIND_LAST_BOUND@@3PEAU_WDF_CLASS_BIND_INFO@@EA']
    assert entry==fx_entry, ('PE entry point is not map FxDriverEntry',hex(entry),hex(fx_entry))
    assert driver_entry!=fx_entry
    executable=0x20000000
    assert image_range(base+fx_entry,4,executable) and image_range(base+driver_entry,4,executable)
    assert image_range(base+cookie_init_rva,4,executable)
    writable=0x80000000
    assert image_range(base+cookie_rva,8,writable)
    assert image_range(base+cookie_complement_rva,8,writable)
    assert optsize>=112+(10+1)*8, ('optional header is too short for the PE load-config directory',optsize)
    directory_count=struct.unpack_from('<I',data,optional+108)[0]
    assert directory_count>10, ('missing PE load-config directory',directory_count,optsize)
    load_config_rva,load_config_size=struct.unpack_from('<II',data,optional+112+10*8)
    assert load_config_rva and load_config_size>=0x60 and load_config_rva+load_config_size<=size
    load_config=base+load_config_rva
    assert image_range(load_config,0x60)
    load_config_declared_size=struct.unpack('<I',u.mem_read(load_config,4))[0]
    assert load_config_declared_size>=0x60 and load_config_declared_size<=load_config_size
    security_cookie_pointer=struct.unpack('<Q',u.mem_read(load_config+0x58,8))[0]
    assert security_cookie_pointer==base+cookie_rva, ('load-config SecurityCookie pointer does not match map symbol',
                                                       hex(security_cookie_pointer),hex(base+cookie_rva))
    bind_size=struct.unpack('<I',u.mem_read(bind_info,4))[0]
    bind_major=struct.unpack('<I',u.mem_read(bind_info+0x10,4))[0]
    bind_minor=struct.unpack('<I',u.mem_read(bind_info+0x14,4))[0]
    bind_function_count=struct.unpack('<I',u.mem_read(bind_info+0x1c,4))[0]
    bind_functions_cell=struct.unpack('<Q',u.mem_read(bind_info+0x20,8))[0]
    assert bind_size==0x58 and (bind_major,bind_minor)==(1,33)
    assert bind_function_count==458 and bind_functions_cell==functions_pointer_cell
    minimum_version_cell=struct.unpack('<Q',u.mem_read(bind_info+0x30,8))[0]
    assert minimum_version_cell==base+symbols['WdfMinimumVersionRequired']
    assert struct.unpack('<I',u.mem_read(minimum_version_cell,4))[0]==bind_minor
    assert struct.unpack('<Q',u.mem_read(bind_info+0x38,8))[0]==base+symbols['WdfClientVersionHigherThanFramework']
    assert struct.unpack('<Q',u.mem_read(bind_info+0x40,8))[0]==function_count_cell
    assert struct.unpack('<Q',u.mem_read(bind_info+0x48,8))[0]==base+symbols['WdfStructureCount']
    assert struct.unpack('<Q',u.mem_read(bind_info+0x50,8))[0]==base+symbols['WdfStructures']
    assert struct.unpack('<I',u.mem_read(function_count_cell,4))[0]==bind_function_count
    assert struct.unpack('<I',u.mem_read(base+symbols['WdfStructureCount'],4))[0]==81
    assert struct.unpack('<Q',u.mem_read(class_bind_last,8))[0]==class_bind_start
    bind_name='WDFLDR.SYS!WdfVersionBind'
    copy_name='ntoskrnl.exe!RtlCopyUnicodeString'
    dbgprint_name='ntoskrnl.exe!DbgPrintEx'
    bind_stub=[address for address,name in import_stubs.items() if name==bind_name]
    copy_stub=[address for address,name in import_stubs.items() if name==copy_name]
    assert len(bind_stub)==1 and len(copy_stub)==1, ('missing/duplicate framework imports',bind_stub,copy_stub)

    bind_failure=0xc0000001  # STATUS_UNSUCCESSFUL, kept distinct for exact propagation check.
    client_failure=0xc0000001
    wdf_driver_create_index=116  # WDK 1.33 wdffuncenum.h: WdfDriverCreateTableIndex.
    wdf_table_count=458  # Also validated against this image's WDF_BIND_INFO.
    wdf_table_base=stack+0x10000
    wdf_globals=stack+0x20000
    wdf_function_stubs={trap+0x2000+i*4:i for i in range(wdf_table_count)}
    wdf_stubs_by_index={index:address for address,index in wdf_function_stubs.items()}
    for stub in wdf_function_stubs:
        u.mem_write(stub,struct.pack('<I',0xd65f03c0))
    wdf_table=b''.join(struct.pack('<Q',stub) for stub in wdf_function_stubs)
    assert len(wdf_table)==wdf_table_count*8
    u.mem_write(wdf_table_base,wdf_table)
    u.mem_write(wdf_globals,bytes(0x100))
    unbind_name='WDFLDR.SYS!WdfVersionUnbind'
    unbind_stub=[address for address,name in import_stubs.items() if name==unbind_name]
    if args.framework_entry_case=='create-failure':
        assert len(unbind_stub)==1, ('missing/duplicate WdfVersionUnbind import',unbind_stub)
    dbgprint_stub=[address for address,name in import_stubs.items() if name==dbgprint_name]
    if args.framework_entry_case=='create-failure':
        assert len(dbgprint_stub)==1, ('missing/duplicate DbgPrintEx import',dbgprint_stub)
    assert args.framework_entry_case in ('bind-failure','create-success','create-failure')
    if args.framework_entry_case=='bind-failure':
        assert struct.unpack('<Q',u.mem_read(functions_pointer_cell,8))[0]==0
        assert struct.unpack('<Q',u.mem_read(driver_globals_cell,8))[0]==0
    else:
        assert args.test_name=='MtkButtons'
        assert struct.unpack('<Q',u.mem_read(functions_pointer_cell,8))[0]==0
        assert struct.unpack('<Q',u.mem_read(driver_globals_cell,8))[0]==0
    assert struct.unpack('<Q',u.mem_read(miniport_unload_cell,8))[0]==0
    loader_cookie=0x1029384756abcdef
    default_security_cookie=0x00002b992ddfa232
    assert loader_cookie not in (0,default_security_cookie)
    initial_security_cookie=struct.unpack('<Q',u.mem_read(base+cookie_rva,8))[0]
    u.mem_write(base+cookie_rva,struct.pack('<Q',loader_cookie))
    driver_object=stack+0x100
    registry_path=stack+0x200
    registry_buffer=stack+0x300
    registry_text=('\\Registry\\Machine\\System\\CurrentControlSet\\Services\\'+args.test_name).encode('utf-16le')
    u.mem_write(driver_object,bytes(0x100))
    u.mem_write(registry_buffer,registry_text+b'\0\0')
    u.mem_write(registry_path,struct.pack('<HH4xQ',len(registry_text),len(registry_text)+2,registry_buffer))
    framework_state={'calls':[],'worker_calls':[],'bind_args':None,'bind_count':0,
                     'driver_entry_calls':0,'driver_entry_args':None,'create_calls':0,
                     'create_args':None,'create_status':None,'unbind_count':0,'unbind_args':None,
                     'dbgprint_count':0,'copy_count':0,'copy_bytes':0,
                     'cookie_init_calls':0}
    def framework_entry_hook(uc,address,instruction_size,unused):
        if address==base+cookie_init_rva:
            framework_state['cookie_init_calls']+=1
            assert framework_state['cookie_init_calls']==1
            assert struct.unpack('<Q',uc.mem_read(base+cookie_rva,8))[0]==loader_cookie
            return
        if address==base+driver_entry:
            framework_state['driver_entry_calls']+=1
            args_seen=[uc.reg_read(reg) for reg in (UC_ARM64_REG_X0,UC_ARM64_REG_X1)]
            framework_state['driver_entry_args']=args_seen
            assert framework_state['bind_count']==1 and framework_state['calls'].count(copy_name)==1
            assert args_seen==[driver_object,registry_path], ('client DriverEntry must receive the original RegistryPath pointer',list(map(hex,args_seen)))
            if args.framework_entry_case=='bind-failure':
                raise AssertionError('client DriverEntry was reached after WdfVersionBind failure')
            return
        if address==fx_bind_classes:
            framework_state['worker_calls'].append('FxStubBindClasses')
            assert framework_state['bind_count']==1 and framework_state['bind_args'] is not None
            assert uc.reg_read(UC_ARM64_REG_X0)==bind_info
            return
        if address==fx_init_types:
            framework_state['worker_calls'].append('FxStubInitTypes')
            assert framework_state['bind_count']==1
            return
        if address==fx_driver_entry_failure:
            framework_state['worker_calls'].append('FxStubDriverEntryFailure')
            assert args.framework_entry_case=='create-failure'
            assert framework_state['driver_entry_calls']==1 and framework_state['create_status']==client_failure
            return
        if address==fx_unbind_classes:
            framework_state['worker_calls'].append('FxStubUnbindClasses')
            assert args.framework_entry_case=='create-failure'
            assert uc.reg_read(UC_ARM64_REG_X0)==bind_info
            return
        name=import_stubs.get(address)
        if address in wdf_function_stubs:
            function_index=wdf_function_stubs[address]
            if function_index!=wdf_driver_create_index:
                raise AssertionError(('unexpected WDF function-table call',function_index,hex(address)))
            assert args.framework_entry_case in ('create-success','create-failure')
            assert framework_state['driver_entry_calls']==1
            assert framework_state['bind_count']==1
            actual_args=[uc.reg_read(reg) for reg in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,
                                                       UC_ARM64_REG_X2,UC_ARM64_REG_X3,
                                                       UC_ARM64_REG_X4,UC_ARM64_REG_X5)]
            framework_state['create_args']=actual_args
            assert actual_args[0]==wdf_globals, ('WdfDriverCreate DriverGlobals mismatch',hex(actual_args[0]))
            assert actual_args[1]==driver_object and actual_args[2]==registry_path
            assert actual_args[3]==0 and actual_args[5]==0
            config=actual_args[4]
            assert stack<=config and config+32<=stack+0x200000, ('WDF_DRIVER_CONFIG outside mock stack',hex(config))
            config_size=struct.unpack('<I',uc.mem_read(config,4))[0]
            config_add=struct.unpack('<Q',uc.mem_read(config+8,8))[0]
            config_unload=struct.unpack('<Q',uc.mem_read(config+16,8))[0]
            config_flags,config_pool_tag=struct.unpack('<II',uc.mem_read(config+24,8))
            assert config_size==32 and config_add==base+buttons_device_add
            assert config_unload==0 and config_flags==0 and config_pool_tag==0, ('unexpected WDF_DRIVER_CONFIG fields',
                                                                                 config_size,hex(config_add),
                                                                                 hex(config_unload),config_flags,
                                                                                 hex(config_pool_tag))
            framework_state['create_calls']+=1
            assert framework_state['create_calls']==1
            result=0 if args.framework_entry_case=='create-success' else client_failure
            framework_state['create_status']=result
            uc.reg_write(UC_ARM64_REG_X0,result)
            return
        if name is None:
            return
        framework_state['calls'].append(name)
        if name==copy_name:
            destination=uc.reg_read(UC_ARM64_REG_X0)
            source=uc.reg_read(UC_ARM64_REG_X1)
            assert image_range(destination,16,0x80000000), ('RtlCopyUnicodeString destination is not writable image data',hex(destination))
            assert stack<=source and source+16<=stack+0x200000, ('RtlCopyUnicodeString source is outside mock stack',hex(source))
            source_length,source_maximum=struct.unpack('<HH',uc.mem_read(source,4))
            destination_length,destination_maximum=struct.unpack('<HH',uc.mem_read(destination,4))
            source_buffer=struct.unpack('<Q',uc.mem_read(source+8,8))[0]
            destination_buffer=struct.unpack('<Q',uc.mem_read(destination+8,8))[0]
            assert source_length%2==0 and source_length<=source_maximum
            assert source_length<=destination_maximum and destination_length<=destination_maximum
            assert stack<=source_buffer and source_buffer+source_length<=stack+0x200000
            assert image_range(destination_buffer,source_length,0x80000000)
            copied=bytes(uc.mem_read(source_buffer,source_length))
            uc.mem_write(destination_buffer,copied)
            uc.mem_write(destination,struct.pack('<H',source_length))
            framework_state['copy_count']+=1
            framework_state['copy_bytes']=source_length
            uc.reg_write(UC_ARM64_REG_X0,0)
            return
        if name==bind_name:
            assert framework_state['calls'].count(copy_name)==1
            args_seen=[uc.reg_read(reg) for reg in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,
                                                       UC_ARM64_REG_X2,UC_ARM64_REG_X3)]
            assert args_seen[0]==driver_object
            assert args_seen[1]==registry_path_image and args_seen[2]==bind_info and args_seen[3]==driver_globals_cell
            assert all(image_range(value,8) for value in args_seen[1:]), ('WdfVersionBind received invalid image metadata pointers',list(map(hex,args_seen)))
            image_path_length,image_path_maximum=struct.unpack('<HH',uc.mem_read(registry_path_image,4))
            image_path_buffer=struct.unpack('<Q',uc.mem_read(registry_path_image+8,8))[0]
            assert image_path_length==len(registry_text) and image_path_maximum>=image_path_length+2
            assert image_path_buffer==registry_buffer_image
            assert bytes(uc.mem_read(image_path_buffer,image_path_length))==registry_text
            framework_state['bind_count']+=1
            framework_state['bind_args']=args_seen
            if args.framework_entry_case=='bind-failure':
                uc.reg_write(UC_ARM64_REG_X0,bind_failure)
            else:
                uc.mem_write(functions_pointer_cell,struct.pack('<Q',wdf_table_base))
                uc.mem_write(driver_globals_cell,struct.pack('<Q',wdf_globals))
                uc.reg_write(UC_ARM64_REG_X0,0)
            return
        if name==unbind_name:
            framework_state['unbind_count']+=1
            assert framework_state['unbind_count']==1 and args.framework_entry_case=='create-failure'
            unbind_args=[uc.reg_read(reg) for reg in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2)]
            framework_state['unbind_args']=unbind_args
            assert unbind_args==[registry_path_image,bind_info,wdf_globals], ('WdfVersionUnbind arguments mismatch',
                                                                              list(map(hex,unbind_args)))
            uc.reg_write(UC_ARM64_REG_X0,0)
            return
        if name==dbgprint_name:
            framework_state['dbgprint_count']+=1
            assert framework_state['dbgprint_count']==1 and args.framework_entry_case=='create-failure'
            dbg_args=[uc.reg_read(reg) for reg in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,
                                                    UC_ARM64_REG_X3,UC_ARM64_REG_X4)]
            assert dbg_args[0]==77 and dbg_args[1]==0 and dbg_args[3]==client_failure
            assert dbg_args[4]==registry_path_image and image_range(dbg_args[2],1)
            debug_message=cstr(dbg_args[2])
            assert 'DriverEntry failed' in debug_message, ('unexpected framework diagnostic',debug_message)
            uc.reg_write(UC_ARM64_REG_X0,0)
            return
        raise AssertionError('Framework entry called an unapproved external import: '+name)
    u.hook_add(UC_HOOK_CODE,framework_entry_hook)
    u.reg_write(UC_ARM64_REG_X0,driver_object)
    u.reg_write(UC_ARM64_REG_X1,registry_path)
    emulation_started=time.monotonic()
    timeout_us=10000000
    instruction_budget=50000000
    u.emu_start(base+fx_entry,end,timeout=timeout_us,count=instruction_budget)
    emulation_elapsed=time.monotonic()-emulation_started
    returned_to_sentinel=u.reg_read(UC_ARM64_REG_PC)==end
    returned_status=u.reg_read(UC_ARM64_REG_X0)&0xffffffff
    assert returned_to_sentinel, ('FxDriverEntry did not return to sentinel',hex(u.reg_read(UC_ARM64_REG_PC)))
    assert framework_state['calls'][:2]==[copy_name,bind_name], framework_state['calls']
    assert framework_state['copy_count']==1 and framework_state['copy_bytes']==len(registry_text)
    assert framework_state['bind_count']==1 and framework_state['bind_args'] is not None
    assert framework_state['cookie_init_calls']==1
    final_security_cookie=struct.unpack('<Q',u.mem_read(base+cookie_rva,8))[0]
    final_cookie_complement=struct.unpack('<Q',u.mem_read(base+cookie_complement_rva,8))[0]
    assert final_security_cookie==loader_cookie
    assert final_cookie_complement==(~loader_cookie)&0xffffffffffffffff
    if args.framework_entry_case=='bind-failure':
        assert framework_state['driver_entry_calls']==0
        assert framework_state['create_calls']==0 and framework_state['unbind_count']==0
        assert returned_status==bind_failure, ('FxDriverEntry did not propagate WdfVersionBind failure exactly',hex(returned_status),hex(bind_failure))
        assert framework_state['worker_calls']==[]
        assert framework_state['calls']==[copy_name,bind_name], framework_state['calls']
        assert struct.unpack('<Q',u.mem_read(functions_pointer_cell,8))[0]==0
        assert struct.unpack('<Q',u.mem_read(driver_globals_cell,8))[0]==0
        assert struct.unpack('<Q',u.mem_read(miniport_unload_cell,8))[0]==0
    elif args.framework_entry_case=='create-success':
        assert framework_state['driver_entry_calls']==1 and framework_state['create_calls']==1
        assert framework_state['create_status']==0 and framework_state['unbind_count']==0
        assert framework_state['dbgprint_count']==0
        assert framework_state['worker_calls']==['FxStubBindClasses','FxStubInitTypes']
        assert framework_state['calls']==[copy_name,bind_name], framework_state['calls']
        assert returned_status==0, ('successful WdfDriverCreate status was not propagated',hex(returned_status))
        assert struct.unpack('<Q',u.mem_read(functions_pointer_cell,8))[0]==wdf_table_base
        assert struct.unpack('<Q',u.mem_read(driver_globals_cell,8))[0]==wdf_globals
        assert struct.unpack('<Q',u.mem_read(miniport_unload_cell,8))[0]==wdf_stubs_by_index[201]
    else:
        assert framework_state['driver_entry_calls']==1 and framework_state['create_calls']==1
        assert framework_state['create_status']==client_failure
        assert framework_state['unbind_count']==1 and framework_state['dbgprint_count']==1
        assert framework_state['worker_calls']==['FxStubBindClasses','FxStubInitTypes',
                                                  'FxStubDriverEntryFailure','FxStubUnbindClasses']
        assert framework_state['calls']==[copy_name,bind_name,dbgprint_name,unbind_name], framework_state['calls']
        assert returned_status==client_failure, ('WdfDriverCreate failure did not propagate exactly',hex(returned_status))
        assert struct.unpack('<Q',u.mem_read(functions_pointer_cell,8))[0]==wdf_table_base
        assert struct.unpack('<Q',u.mem_read(driver_globals_cell,8))[0]==wdf_globals
        assert struct.unpack('<Q',u.mem_read(miniport_unload_cell,8))[0]==wdf_stubs_by_index[201]
    case_title=''.join(part.title() for part in args.framework_entry_case.split('-'))
    if args.framework_entry_case=='bind-failure':
        framework_scope=f'actual {p.name} KMDF FxDriverEntry and Worker in ARM64 Unicorn; bounded RtlCopyUnicodeString and WdfVersionBind mocks; client DriverEntry is confirmed not reached; other imports fail closed'
        case_coverage=['WdfVersionBind failure is returned exactly and the client DriverEntry is not entered',
                       'execution stops before class/type initialization, function-table publication or DriverEntry cleanup']
    elif args.framework_entry_case=='create-success':
        framework_scope=f'actual {p.name} KMDF FxDriverEntry, Worker and client DriverEntry in ARM64 Unicorn; WDF loader binding and one WdfDriverCreate response are narrowly mocked; unknown imports and WDF table calls fail closed'
        case_coverage=['successful bounded WDF bind populates the version 1.33 function table and globals before client DriverEntry',
                       'the client DriverEntry and its WdfDriverCreate call use the original RegistryPath pointer; WdfVersionBind uses the stub-owned copied path',
                       'real client DriverEntry passes the validated WDF_DRIVER_CONFIG to WdfDriverCreate and propagates success',
                       'function-table entry 201 initializes the miniport unload override; class/type lists are empty for this MtkButtons image']
    else:
        framework_scope=f'actual {p.name} KMDF FxDriverEntry, Worker and client DriverEntry in ARM64 Unicorn; WDF loader binding and one failing WdfDriverCreate response are narrowly mocked; unknown imports and WDF table calls fail closed'
        case_coverage=['real client DriverEntry passes the validated WDF_DRIVER_CONFIG to WdfDriverCreate',
                       'the client DriverEntry and its WdfDriverCreate call use the original RegistryPath pointer; WdfVersionBind uses the stub-owned copied path',
                       'WdfDriverCreate failure follows the original diagnostic and framework unbind cleanup path, then propagates the same status',
                       'function-table entry 201 initializes the miniport unload override; class/type lists are empty for this MtkButtons image']
    bound_status=bind_failure if args.framework_entry_case=='bind-failure' else 0
    function_table_final=struct.unpack('<Q',u.mem_read(functions_pointer_cell,8))[0]
    driver_globals_final=struct.unpack('<Q',u.mem_read(driver_globals_cell,8))[0]
    report={'status':'pass','test_name':args.test_name+'FrameworkEntry'+case_title,
            'scope':framework_scope,
            'coverage':['PE entry RVA equals unique FxDriverEntry RVA from the supplied map',
                        'real FxDriverEntry invokes WdfVersionBind before client DriverEntry',
                        'WDF bind metadata version 1.33, 458 function entries and image globals match the actual PE/map',
                        *case_coverage,
                        *([] if args.framework_entry_case=='bind-failure' else
                          ['WdfDriverCreate is the only mocked WDF table call; every other function-table entry traps']),
                        'PE load-config SecurityCookie points to the mapped cookie global; a fixed nondefault loader value is supplied before entry',
                        'the original __security_init_cookie code runs once and writes the matching complement',
                        'Windows loader behavior beyond the documented boundary mocks and actual mapped code is not modeled'],
            'entrypoints':{'pe_entry_rva':entry,'map_fx_driver_entry_rva':fx_entry,
                           'map_driver_entry_rva':driver_entry,'security_cookie_init_rva':cookie_init_rva},
            'loader_security_cookie':{'value':f'0x{loader_cookie:016x}',
                                      'initial_image_value':f'0x{initial_security_cookie:016x}',
                                      'load_config_directory_index':10,'security_cookie_field_offset':0x58,
                                      'load_config_pointer':f'0x{security_cookie_pointer:016x}',
                                      'cookie_global_rva':cookie_rva,
                                      'complement_global_rva':cookie_complement_rva,
                                      'pointer_matches_map_symbol':True,
                                      'original_initializer_calls':framework_state['cookie_init_calls'],
                                      'final_cookie':f'0x{final_security_cookie:016x}',
                                      'final_complement':f'0x{final_cookie_complement:016x}'},
            'wdf_version_bind':{'calls':framework_state['bind_count'],
                                'status':f'0x{bound_status:08x}',
                                'driver_object_matches':True,'metadata_pointers_in_image':True,
                                'interface_version':[bind_major,bind_minor],'function_count':bind_function_count,
                                'function_table_pointer':f'0x{function_table_final:016x}',
                                'driver_globals_pointer':f'0x{driver_globals_final:016x}'},
            'wdf_driver_create':{'calls':framework_state['create_calls'],'table_index':wdf_driver_create_index,
                                 'status':None if framework_state['create_status'] is None else f'0x{framework_state["create_status"]:08x}',
                                 'argument_registers':None if framework_state['create_args'] is None else
                                    [f'0x{value:016x}' for value in framework_state['create_args']],
                                 'driver_config_size':32,'device_add_rva':buttons_device_add,
                                 'mocked_function_table_count':wdf_table_count,
                                 'all_non_driver_create_entries_trap':True},
            'worker_internal_calls':framework_state['worker_calls'],
            'wdf_version_unbind_calls':framework_state['unbind_count'],
            'wdf_version_unbind_arguments':None if framework_state['unbind_args'] is None else
                [f'0x{value:016x}' for value in framework_state['unbind_args']],
            'dbgprint_calls':framework_state['dbgprint_count'],
            'rtl_copy_unicode_string':{'calls':framework_state['copy_count'],'copied_bytes':framework_state['copy_bytes']},
            'client_driver_entry_calls':framework_state['driver_entry_calls'],
            'called_imports':framework_state['calls'],
            'unexecuted_imports':[name for name in imports if name not in framework_state['calls']],
            'returned_to_sentinel':returned_to_sentinel,'returned_status':f'0x{returned_status:08x}',
            'emulation_limits':{'timeout_us':timeout_us,'instruction_budget':instruction_budget},
            'emulation_elapsed_seconds':emulation_elapsed,
            'binary_sha256':hashlib.sha256(data).hexdigest(),
            'map_sha256':hashlib.sha256(Path(args.framework_entry_map).read_bytes()).hexdigest(),
            'test_runner_source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    report_path=Path(args.report) if args.report else p.parent/(args.test_name+'-'+args.framework_entry_case+'-framework-entry-tests.json')
    report_path.write_text(json.dumps(report,indent=2)+'\n')
    print('Windows ARM64 '+args.test_name+' KMDF entry: '+args.framework_entry_case+' path passed')
    sys.exit(0)
# Expanded UFS restart tests scan the entire 128 KiB DMA fixture for each
# failed power-state gate. Keep other harness limits unchanged.
timeout_us=20000000 if args.test_name in ('MtkUfs','MtkUfsPower') else 10000000
instruction_budget=100000000 if args.test_name in ('MtkUfs','MtkUfsPower') else 50000000
emulation_started=time.monotonic()
try:
    u.emu_start(base+entry,end,timeout=timeout_us,count=instruction_budget)
except Exception as error:
    print("ARM64 test execution exception: "+repr(error)+
          "; PC="+hex(u.reg_read(UC_ARM64_REG_PC))+
          "; LR="+hex(u.reg_read(UC_ARM64_REG_X30))+
          "; SP="+hex(u.reg_read(UC_ARM64_REG_SP)),file=sys.stderr)
    raise
emulation_elapsed=time.monotonic()-emulation_started
returned_to_sentinel=u.reg_read(UC_ARM64_REG_PC)==end
assert returned_to_sentinel, ('ARM64 harness did not return to sentinel before emulator timeout/count limit',
                              hex(u.reg_read(UC_ARM64_REG_PC)),timeout_us,instruction_budget)
line=u.reg_read(UC_ARM64_REG_X0)
windows_source=Path(__file__).resolve().parent
test_source=Path(args.test_source) if args.test_source else windows_source/'Tests/RequestTest.c'
production_source=Path(args.production_source) if args.production_source else windows_source/'MtkI2c/Driver.c'
assert line==0, f'Windows ARM64 {args.test_name} execution failed at {test_source.name}:{line}'
if args.test_name=='MtkDisplay':
    coverage=['actual Dxgk display-only callback registration; no fake ISR/VSync or worker threads',
              'actual ACPI HID and bounded 48-byte DFBI validation before and after POST ownership',
              'actual CommitVidPn, pinned source/target validation and cleanup',
              'actual software present, four rotations, full repaint and bounds-before-write',
              'actual pool/MDL/map failure cleanup and synchronous completion',
              'actual D1-D3 display gating and D0 POST/ACPI revalidation, map failure quarantine, child/crash-display/POST release callbacks',
              'Windows SEH exception dispatch is not emulated; only nonexception cleanup paths run']
elif args.test_name=='MtkUfsPower':
    coverage=['actual WDM power filter and fixed GUID PRM client callbacks under mocks',
              'PRM status and miniport generation are checked independently',
              'Private miniport request timeout, completion racing cancellation, and cleanup after late completion or removal',
              'I/O-manager, kernel, power, storage and PRM responses are mocked; no Windows/hardware execution']
elif args.test_name=='MtkSd':
    coverage=['actual SDPORT registration and ACPI/MMIO resource validation',
              'actual ACPI request framing, integer reply checks, clock reset and DMA adapter lifetime',
              'command responses, R1b DAT0 wait, CRC precedence and write protection',
              '64 KiB MSDC DMA read with AutoCMD12 and per-block SDPORT PIO completions',
              'short reads, card response errors, removal/reinsertion, timeout and recovery',
              'power-off cancels once; surprise removal retains DMA buffers if the engine stays active']
elif args.test_name=='MtkSdPower':
    coverage=['actual ARM64 Windows SD power client and firmware broker through the PRM wire',
              'VMCH/VMC enable, verified 20 ms settling and restore of original register fields',
              'wire Busy retries, re-entry, stale or lost job replies and failed WACS writes',
              'no clock readiness until the lease is acknowledged and power has settled',
              'known readback failure is acknowledged with the WACS channel idle']
elif args.test_name=='MtkUfs':
    coverage=['actual physical Storport registration and strict ACPI/MMIO/handoff resource rejection',
              'uncached contiguous DMA validation before controller ownership',
              'full-duplex callbacks serialize through actual StorPortSynchronizeAccess call shape',
              'NOP then bounded READ CAPACITY 16 and exact LU2 capacity gate',
              'actual asynchronous read, IRQ, short transfer, CHECK CONDITION, sense and timer completion',
              'single-slot BUSY, WRITE payload snapshot, direction checks, protected LUs, MODE SENSE and REPORT LUNS',
              'actual FLUSH and SHUTDOWN send non-IMMED SYNCHRONIZE CACHE and wait for completion',
              'idle PnP stop/restart revalidates inherited link and disk',
              'valid handoff with powered-off HCI and thirteen lost/stale restart gates reject before MMIO/DMA writes',
              'active Stop aborts exactly once without copying DMA into caller memory; HCE-stuck Stop quarantines DMA',
              'fatal interrupt, timeout, bus-reset failure and unsafe DMA quarantine block reuse']
elif args.test_name=='MtkGpio':
    coverage=['actual GpioClx controller resource validation and partial-map cleanup',
              'actual bounded ACPI GRDY boot integer gate; stale boot readiness cannot authorize D3 resume',
              'unresumable SaveContext transition rejected before MMIO or rundown changes; D0 callbacks and final release remain usable',
              'resume refusal preserves mappings/state without treating static GRDY as runtime readiness',
              'actual ARM64 MgSuspendWake/MgResume on mocked powered MMIO: selected wake pins, restored masks, pending edges and dropped-write recovery; not a Windows D3 callback',
              'shared Schmitt ownership, raw drive-field preservation and PASSIVE/DIRQL transition serialization',
              'actual GPIO input/output and IRQ callbacks with fake MMIO',
              'GNSS-facing WDF query-interface registration and every DeviceAdd failure; real interface entry points called with mocked WDF',
              'GPIO25/26 opaque LNA leases: read-only acquire, all three modes, GPIO exclusion, stale tokens across reprepare and exhaustion',
              'balanced provider/lease references, static stop/remove veto, PASSIVE validation and no WDF call at bank DIRQL',
              'active-lease stop refusal and D0 restore retry; release after hardware loss never performs MMIO, unmaps all resources and revokes lease references',
              'missing/failed stop, suspended GPIO context and virtual PMIC cleanup failure retain their journal and block reprepare after detach',
              'clean stop restores before release; repeated release and retained LNA interface callbacks perform no MMIO or duplicate reference drop',
              'surprise-removal preprocess before prepare, before start and with both leases: no MMIO, lease references revoked, IRP forwarded once, repeated removal safe',
              'MH248 GPIO0 shared read-only input and interrupt: either connect/release order, no alias pad writes, remaining connection preserved',
              'all eight exact GPIO/EINT/IOCFG resource mappings, each partial-map failure unwound',
              'all four pull configurations through actual I/O and IRQ callbacks',
              'physical and interrupt pin range validation before writes',
              'mask/query/ACK/reconfigure and original pin restoration',
              'bank interrupt lock calling rules for passive and DIRQL callbacks']
elif args.test_name=='MtkButtons':
    coverage=['actual WDF PnP/D0 timer/work and ordinary KPD ISR/DPC callbacks from Driver.c and production button core',
              'exact read-only KPD resource plus optional raw GSIV138 exclusive edge IRQ; translated vector remains HAL-assigned',
              'default interrupt parent with manual callback synchronization; creation errors and NULL output unwind MMIO for retry',
              'interrupt vector/mode/polarity/share validation only in D0Entry after PrepareHardware; failures admit no timer/work/sample',
              'five ordered 16-bit matrix reads plus shared PMIC TOPSTATUS complete sample; persistent 20ms power-key polling and no PMIC writes',
              'IRQ/timer sample coalescing including IRQ arrival during PMIC read; no per-sample hardware-mask claim',
              'initial held-button suppression, press/release ordering and no repeat of uncertain inbox toggles',
              'exclusive GUID_GPIOBUTTONS_NOTIFY_INTERFACE handle and four-byte button enum writes',
              'D0ExitPreInterruptsDisabled closes admission; D0/stop drains work and retains cleanup on failures',
              'kernel/WDF/MMIO/PMIC entry and inbox interface are mocks; no Windows load, OS suspend or physical wake']
elif args.test_name=='MtkUsb':
    coverage=['actual UCX endpoint callbacks, SET_ADDRESS on address zero and configure/disable request routing',
              'control, bulk and interrupt URB validation with MDL transfer-buffer staging',
              'manual endpoint queue NAK retry, unmark-cancel race and timer requeue',
              'cancel completion through a separate DPC and endpoint abort/purge accounting',
              'root-hub control/status handling and interrupt change bitmap',
              'actual root-port suspend and resume with queued transfers, resume timing and disconnect or replacement during recovery',
              'strict raw/translated MMIO, IRQ and boot handoff validation',
              'D0/stop sequencing, DEVCTL readback, mocked idle/FIFO gate failures and UCX critical-failure notification',
              'first D0 avoids controller/PHY access if MCLK is gated, malformed or times out; legacy AML is covered',
              'retained D1/D2/D3 cycles, saved POWER mode bits and idle FIFO restoration without stale boot handoff',
              'saved USB2 PHY tuning restoration, unchanged tuning, legacy read-only mapping and combined PHY/FIFO loss',
              'refusal on live role or clock loss, active DMA or calibration, malformed resources and failed readback',
              'MGON single-integer D0 restore, live clock read-back and refusal on missing/malformed methods or host role loss',
              'work-item coalescing, reentrant callback suppression and no drain from the active worker thread',
              'the timer cannot schedule USB work during core initialization',
              'VBUS loss before or during D0 exit, separation of detach and reconnect, and transfer gating through re-enumeration',
              'MUSB bulk transfers continue across TCPC and ACPI read failures and retained D0 resume. Physical disconnect and VBUS loss still end the connection']
elif args.test_name=='MtkCpu':
    coverage=['actual KMDF resource preparation, scoped writable cluster mappings and partial-map rollback',
              'both cluster windows, LUT format versions, bounded table parsing, and both index and kHz encodings',
              'buffered read API validation and suppression of unstable or unavailable samples',
              'buffered OPP requests on both clusters; duplicate, invalid and concurrent requests, and register readback failure',
              'PEP registration callbacks, ACPI0007 CPU UID mapping, A55/A76 performance domains and member counts',
              'PEP DPM device-start and D0 return notifications; system resume invalidates cached OPP domains',
              'PEP performance query, bounds-respecting OPP selection, unsupported scaling-mode and released-device rejection']
elif args.test_name=='MtkThermal':
    coverage=['actual KMDF resource, D0, buffered read IOCTL and release code',
              'all seven map failure rollbacks, older ACPI resource layouts and resource rejection before writes',
              'bounded stale/missing ready handling and preservation of unrelated channels',
              'board and LTE samples, enabled/disabled calibration and eight D0 exit/re-entry cycles',
              'clock disappearance while sampling and no MMIO after release',
              'all 13 inherited LVTS sensors and maximum, with clock, configuration and invalid-sample rejection',
              'stock golden temperature and thirteen factory trims with both eFuse cells, blank and partially programmed trims, and MMIO failure',
              'LVTS clock gate restore via LVON with live readback, legacy AML, missing banks and independent board temperature',
              'standard thermal IOCTL immediate reads, threshold crossing, deadline and indefinite waits',
              'manual-queue cancellation, immediate reads during a wait, power-down purge and re-entry']
elif args.test_name=='MtkGnss':
    coverage=['actual UMDF2 device creation, every partial initialization failure and real GNSS interface registration',
              'single-owner file, platform handshake and honest single-shot capabilities',
              'actual buffered GNSS IOCTL session start/modify/stop and lower 500 ms timeout call shape',
              'pending fix cancellation, duplicate GET rejection, final fix and secure cached-event clearing',
              'strictly increasing receiver leases survive failed re-query; new provider generation resets lease history',
              'old-session and stale/future fix rejection; monotonic cached-fix expiry despite stalled/backward wall time',
              'actual D0, file cleanup and removal paths; uncertain stop/start quarantined until confirmed reset',
              'MSL versus ellipsoid altitude, unavailable NaNs, explicit 95 percent accuracy and constellation/band coalescing',
              'actual portable NMEA stream output mapped into Windows GNSS_EVENT_2',
              'WDF, receiver and clock mocked; no real receiver provider, Windows runtime or RF execution']
elif args.test_name=='HimaxTouch':
    coverage=['actual HmInterruptIsr combined SPB command/read and 50 ms timeout shape',
              'actual HmPrepare through ResourceHub v2 exact controller/address/speed/general flags and all cleanup failures',
              'actual HmD0Entry HMXR startup gate, fresh HMRD echo and live firmware-state read for retained resume',
              'actual HmReadWork VHF ready-token race and failed-submit recovery',
              'actual three-error all-up and interrupt-disable path',
              'actual queue-overflow all-up resynchronization',
              'actual delayed-ready FIFO preserves down/lift/new-down and avoids requeue while waiting',
              'actual release generation compare-exchange preserves a newer overflow during lift submit',
              'actual HmD0Exit lift path with and without a ready token',
              'release drains work and closes SPB, invalidates the auto-deleted interrupt; reprepare recreates it and requires fresh HMRD']
else:
    coverage=['MDL chains and length checks','every message and DMA staging validated before the first MMIO write','one through 32 message sequences and per-transfer delay validation',
              'request cancellation before/during/after transfer and single completion',
              'completed whole-message byte counts and SpbCx target bus-fault success status; unsafe DMA remains an error',
              'DMA fault retains buffers and prevents reuse',
              'exact raw/translated I2C0/I2C6 controller/APDMA/GSI rejection before mapping',
              'all map/DMA/common-buffer/interrupt creation failures unwind; post-release handles are invalidated and repeated release never locks a deleted interrupt',
              'bounded IRDY/IRCK validation, live source frequency on every entry and malformed live-clock responses rejected before initialization',
              'I2C0 and I2C6 survive eight D3/D0 cycles each under WDF/ACPI mocks; current clock is used rather than static ICLK',
              'all three buses only call IRRS after an orderly stop; old, unavailable, invalid and successful AML restoration handled before controller access',
              'safe resource reassignment creates a fresh interrupt; active release performs no stop/MMIO and unsafe DMA remains pinned',
              'actual passive ISR masks enabled pending status, leaves W1C flags for owner and signals synchronization event',
              'bounded 5 ms passive waits, cancellation wakes before completion and stale event cleared once per request',
              'exact I2cSerialBusV2 Resource Hub descriptor bounds, exclusive flags, address and speed',
              'portable MiTransfer engine is mocked here; independent ASan/UBSan core tests exercise IRQ/polling modes']
report={'status':'pass','test_name':args.test_name,
        'scope':('actual Windows ARM64 PE machine code in Unicorn; WDF/kernel/MMIO/shared PMIC entry/inbox button interface mocked; no Windows load or physical wake' if args.test_name=='MtkButtons' else 'actual Windows ARM64 PE machine code in Unicorn; UMDF/WDF/receiver/clock mocked; no Windows load or hardware/location access' if args.test_name=='MtkGnss' else 'Windows ARM64 PE machine code in Unicorn with MUSB port and bulk transfer logic. UCX, WDF, kernel services and hardware are modeled. No Windows load.' if args.test_name=='MtkUsb' else 'actual Windows ARM64 PE machine code in Unicorn; Dxgk/ACPI/MDL/kernel mocked; no Windows load' if args.test_name=='MtkDisplay' else 'actual Windows ARM64 PE machine code in Unicorn; WDM/kernel/I/O/power/storage/PRM mocked; no Windows load' if args.test_name=='MtkUfsPower' else 'actual Windows ARM64 PE machine code in Unicorn; Storport/kernel/IRQ/DMA/MMIO mocked; no Windows load' if args.test_name=='MtkUfs' else 'actual Windows ARM64 PE machine code in Unicorn; kernel/WDF/GpioClx/ACPI/MMIO mocked; no Windows load' if args.test_name=='MtkGpio' else 'actual Windows ARM64 PE machine code in Unicorn; kernel/WDF/VHF/SpbCx mocked; no Windows load' if args.test_name=='HimaxTouch' else 'actual Windows ARM64 CPU and PEP callback machine code in Unicorn; WDF, PoFx handles and MMIO mocked; no Windows load' if args.test_name=='MtkCpu' else 'actual Windows ARM64 PE machine code in Unicorn; WDF, time, ACPI and MMIO mocked; no Windows load' if args.test_name=='MtkThermal' else 'actual Windows ARM64 PE machine code in Unicorn; kernel/WDF/SpbCx/ACPI/MMIO and transfer core mocked; no Windows load'),
        'coverage':coverage,'returned_to_sentinel':returned_to_sentinel,'unexecuted_kernel_imports':imports,
        'emulation_limits':{'timeout_us':timeout_us,'instruction_budget':instruction_budget},
        'emulation_elapsed_seconds':emulation_elapsed,
        'binary_sha256':hashlib.sha256(data).hexdigest(),
        'production_source_sha256':hashlib.sha256(production_source.read_bytes()).hexdigest(),
        'test_source_sha256':hashlib.sha256(test_source.read_bytes()).hexdigest(),
        'test_runner_source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
if args.test_name in ('MtkButtons','MtkGnss','MtkUfsPower','HimaxTouch','MtkI2c','MtkCpu','MtkSdPower','MtkSd'):
    export_rva,export_size=struct.unpack_from('<II',data,optional+112)
    assert export_rva and export_size and export_rva+40<=size
    exports=bytes(u.mem_read(base+export_rva,40))
    function_count,name_count,functions,names,ordinals=struct.unpack_from('<IIIII',exports,20)
    assertion_count=None
    for i in range(name_count):
        name_rva=struct.unpack('<I',u.mem_read(base+names+4*i,4))[0]
        name=bytes(u.mem_read(base+name_rva,min(256,size-name_rva))).split(b'\0',1)[0]
        if name==(args.test_name+'AssertionCount').encode():
            ordinal=struct.unpack('<H',u.mem_read(base+ordinals+2*i,2))[0]
            assert ordinal<function_count
            value_rva=struct.unpack('<I',u.mem_read(base+functions+4*ordinal,4))[0]
            assert value_rva+4<=size and not export_rva<=value_rva<export_rva+export_size
            assertion_count=struct.unpack('<I',u.mem_read(base+value_rva,4))[0]
            break
    assert assertion_count and assertion_count>0, args.test_name+' production callback assertions did not execute'
    report['assertions']=assertion_count
if args.test_name=='MtkSdPower':
    report['scope']='actual Windows ARM64 PE and firmware broker in Unicorn, with PRM interface, time and WACS registers modeled'
if args.test_name=='MtkSd':
    report['scope']='actual Windows ARM64 SDPORT callbacks and MSDC host, command and DMA code in Unicorn, with SDPORT, ACPI, PRM client and kernel services mocked'
report_path=Path(args.report) if args.report else p.parent/'request-tests.json'
report_path.write_text(json.dumps(report,indent=2)+'\n')
print('Windows ARM64 '+args.test_name+' production code: emulated checks passed (kernel/driver-framework mocks)')
