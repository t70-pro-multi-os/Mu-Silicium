#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute built EFI payload checks and the Linux lid DT transform on ARM64."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,re,struct
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X30
PKG=Path(__file__).resolve().parent;REPO=PKG.parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir',type=Path,default=REPO/'Build/t70proPkg/RELEASE_CLANGPDB')
parser.add_argument('--output',type=Path,default=REPO.parent/'analysis/linux-boot-tests.json')
args=parser.parse_args()
BUILD=args.build_dir.resolve()/'AARCH64'
IMAGE=BUILD/'T70LinuxBoot.efi'
MAP=BUILD/'t70proPkg/Applications/LinuxBoot/LinuxBoot/DEBUG/T70LinuxBoot.map'
application_source=PKG/'Applications/LinuxBoot/LinuxBoot.c'
raw=IMAGE.read_bytes();pe=struct.unpack_from('<I',raw,0x3c)[0]
assert raw[:2]==b'MZ' and raw[pe:pe+4]==b'PE\0\0'
machine,nsections=struct.unpack_from('<HH',raw,pe+4);assert machine==0xaa64
opt=pe+24;optsize=struct.unpack_from('<H',raw,pe+20)[0]
assert struct.unpack_from('<H',raw,opt)[0]==0x20b
base=struct.unpack_from('<Q',raw,opt+24)[0];image_size,headers=struct.unpack_from('<II',raw,opt+56)
assert struct.unpack_from('<II',raw,opt+120)==(0,0),'Unexpected PE imports'
symbols={m.group(2):int(m.group(1),16) for m in re.finditer(r'^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$',MAP.read_text(),re.M)}
assert all(name in symbols for name in ('ReadPayload','sha256_block_data_order','gBS','T70PrepareLinuxLidTree','FdtGetProp','FdtPathOffset'))
BS=0x1000000;ROOT=BS+0x1000;FILE=BS+0x2000;PATH=BS+0x3000;HASH=BS+0x4000;OUT=BS+0x5000
HEAP=0x2000000;STACK=0x70000000;TRAPS=0x60000000;SENTINEL=TRAPS+0xf000
ERROR=1<<63
cases=[]

def machine():
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,(image_size+4095)&~4095);u.mem_write(base,raw[:headers])
    for i in range(nsections):
        off=opt+optsize+i*40;vsize,rva,rsize,roff=struct.unpack_from('<IIII',raw,off+8)
        assert rva+max(vsize,rsize)<=image_size and roff+rsize<=len(raw)
        if rsize:u.mem_write(base+rva,raw[roff:roff+rsize])
    for address,size in ((BS,0x10000),(HEAP,0x100000),(STACK,0x200000),(TRAPS,0x10000)):u.mem_map(address,size)
    return u

def run(payload,corrupt=False):
    u=machine()
    def qget(a):return struct.unpack('<Q',u.mem_read(a,8))[0]
    def qput(a,v):u.mem_write(a,struct.pack('<Q',v))
    regs=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4]
    state={'position':0,'next':HEAP,'allocations':set(),'open':False,'reads':0}
    u.mem_write(PATH,'\\EFI\\Teclast\\test'.encode('utf-16le')+b'\0\0')
    digest=bytearray(hashlib.sha256(payload).digest())
    if corrupt:digest[0]^=1
    u.mem_write(HASH,bytes(digest));qput(base+symbols['gBS'],BS)
    callbacks={}
    def callback(offset,fn):
        addr=TRAPS+len(callbacks)*16;callbacks[addr]=fn;u.mem_write(addr,b'\xc0\x03\x5f\xd6');qput(offset,addr)
    def allocate(a):
        pool,n,out=a[:3];assert pool==4 and 0<n<0x10000
        address=state['next'];state['next']+=(n+15)&~15;state['allocations'].add(address);qput(out,address);return 0
    def free(a):assert a[0] in state['allocations'];state['allocations'].remove(a[0]);return 0
    def opened(a):
        assert a[0]==ROOT and a[2]==PATH and a[3]==1 and a[4]==0 and not state['open']
        state['open']=True;qput(a[1],FILE);return 0
    def closed(a):assert a[0]==FILE and state['open'];state['open']=False;return 0
    def info(a):
        assert a[0]==FILE and state['open'];n=qget(a[2])
        if a[3]==0:qput(a[2],96);return ERROR|5
        assert n>=96;u.mem_write(a[3],bytes(96));qput(a[3],96);qput(a[3]+8,len(payload));return 0
    def read(a):
        assert a[0]==FILE and state['open'];n=min(qget(a[1]),31,len(payload)-state['position'])
        u.mem_write(a[2],payload[state['position']:state['position']+n]);qput(a[1],n)
        state['position']+=n;state['reads']+=1;return 0
    callback(BS+64,allocate);callback(BS+72,free)
    callback(ROOT+8,opened);callback(FILE+16,closed);callback(FILE+32,read);callback(FILE+64,info)
    def hook(uc,addr,size,unused):
        if addr not in callbacks:raise AssertionError('Unexpected service trap')
        result=callbacks[addr]([uc.reg_read(r) for r in regs]);uc.reg_write(UC_ARM64_REG_X0,result)
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
    u.hook_add(UC_HOOK_CODE,hook,begin=TRAPS,end=TRAPS+0x1000)
    for r,v in zip(regs,[ROOT,PATH,len(payload),HASH,OUT]):u.reg_write(r,v)
    u.reg_write(UC_ARM64_REG_SP,STACK+0x200000-16);u.reg_write(UC_ARM64_REG_X30,SENTINEL)
    u.emu_start(base+symbols['ReadPayload'],SENTINEL,timeout=10000000,count=50000000)
    assert u.reg_read(UC_ARM64_REG_PC)==SENTINEL,'EFI function did not return before emulation limit'
    status=u.reg_read(UC_ARM64_REG_X0);assert status==(ERROR|26 if corrupt else 0),hex(status)
    if not corrupt:
        output=qget(OUT);assert output in state['allocations'] and bytes(u.mem_read(output,len(payload)))==payload
        state['allocations'].remove(output)
    else:assert qget(OUT)==0
    assert not state['allocations'] and not state['open']
    # The hashes embedded in this exact binary must match the prepared artifacts.
    for item,symbol in zip(early_userspace['artifacts'],['mKernelHash','mInitrdHash','mModulesHash']):
        assert bytes(u.mem_read(base+symbols[symbol],32)).hex()==item['sha256'],'Stale EFI payload identity'
    return {'bytes':len(payload),'corrupted_expected_hash':corrupt,'status':hex(status),'partial_reads':state['reads'],'returned_to_sentinel':True}

early_userspace=json.loads((REPO.parent/'analysis/linux-early-userspace.json').read_text())
profile_bytes=machine().mem_read(base+symbols['mFullDeviceOptions'],512)
profile_end=next(i for i in range(0,len(profile_bytes),2) if profile_bytes[i:i+2]==b'\0\0')
profile=profile_bytes[:profile_end].decode('utf-16le')
selection=[option.removeprefix('teclast.modules=') for option in profile.split()
           if option.startswith('teclast.modules=')]
assert len(selection)==1 and len(profile)<256
features=selection[0].split(',')
assert len(features)==len(set(features))
assert set(features)==set(early_userspace['module_plans'])
assert features[:4]==['usb','input','display','storage']
for length in (1,3,55,56,63,64,65,1024,4096):
    payload=bytes((i*29+7)&255 for i in range(length));cases.append(run(payload))
cases.append(run(b'abc',True))

# This is the linked production helper/libfdt, not a Python reimplementation.
spec=importlib.util.spec_from_file_location('lid_dt_reader',PKG/'Linux/platform-modules.py')
reader=importlib.util.module_from_spec(spec);spec.loader.exec_module(reader)
stock=(REPO.parent/'analysis/stock-fdt/effective.dtb').read_bytes()
stock_sha=hashlib.sha256(stock).hexdigest()
assert stock_sha=='a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391'
original_nodes,_=reader.tree(stock)
LIDSRC=0x4000000;LIDOUT=0x5000000;LIDMAX=0x200000
regs=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4]
def call(u,name,args):
    for reg,value in zip(regs,args):u.reg_write(reg,value)
    u.reg_write(UC_ARM64_REG_SP,STACK+0x200000-16);u.reg_write(UC_ARM64_REG_X30,SENTINEL)
    u.emu_start(base+symbols[name],SENTINEL,timeout=10000000,count=250000000)
    assert u.reg_read(UC_ARM64_REG_PC)==SENTINEL,name+' did not return before emulation limit'
    return u.reg_read(UC_ARM64_REG_X0)
def reservations(data):
    offset=struct.unpack_from('>I',data,16)[0];end=offset
    while True:
        assert end+16<=len(data)
        entry=data[end:end+16];end+=16
        if entry==bytes(16):return data[offset:end]
lid_cases=[]
for name,expected in [('native-lid',0),('wrong-model',ERROR|3),('wrong-polarity',ERROR|3),
                      ('malformed-tree',ERROR|33),('short-capacity',ERROR|2),
                      ('unaligned-source',ERROR|2),('unaligned-output',ERROR|2),('overlapping-buffers',ERROR|2)]:
    u=machine();u.mem_map(LIDSRC,LIDMAX);u.mem_map(LIDOUT,LIDMAX)
    u.mem_write(LIDOUT,b'\xa5'*LIDMAX)
    source=LIDSRC+(name=='unaligned-source');destination=LIDOUT+(name=='unaligned-output')
    capacity=len(stock)+1024-(name=='short-capacity');data=stock
    if name=='wrong-model':
        assert data.count(b'MH248\0')==1;data=data.replace(b'MH248\0',b'MH249\0')
    if name=='malformed-tree':data=bytes(4)+data[4:]
    u.mem_write(source,data)
    if name=='wrong-polarity':
        u.mem_write(PATH,b'/soc/odm/hall_para\0');node=call(u,'FdtPathOffset',[source,PATH])
        assert node<0x80000000
        u.mem_write(PATH,b'hall_int_port\0');address=call(u,'FdtGetProp',[source,node,PATH,OUT])
        assert struct.unpack('<I',u.mem_read(OUT,4))[0]==12
        u.mem_write(address+8,struct.pack('>I',0))
    if name=='overlapping-buffers':destination=source+0x100
    before=bytes(u.mem_read(LIDSRC,LIDMAX))
    status=call(u,'T70PrepareLinuxLidTree',[source,len(stock),destination,capacity])
    assert status==expected,(name,hex(status))
    assert bytes(u.mem_read(LIDSRC,LIDMAX))==before,'Source tree was changed'
    if status==0:
        length=struct.unpack('>I',u.mem_read(destination+4,4))[0];assert length<=capacity
        output=bytes(u.mem_read(destination,length));nodes,_=reader.tree(output)
        assert set(nodes)-set(original_nodes)=={'/teclast-cover-lid','/teclast-cover-lid/lid-switch'}
        for path,node in original_nodes.items():
            properties=dict(node['properties'])
            if path=='/soc/odm/hall_para':properties['status']=b'disabled\0'
            assert nodes[path]['properties']==properties,'Unrelated property delta: '+path
        assert reservations(output)==reservations(stock)
        assert output[28:32]==stock[28:32],'Boot CPU changed'
        assert nodes['/teclast-cover-lid']['compatible']==['gpio-keys']
        props=nodes['/teclast-cover-lid/lid-switch']['properties']
        handle=reader.cells(original_nodes['/soc/odm/hall_para']['properties']['hall_int_port'])[0]
        assert reader.cells(props['gpios'])==[handle,0,0]
        for prop,value in [('linux,input-type',5),('linux,code',0),('debounce-interval',10),('wakeup-event-action',2)]:
            assert reader.cells(props[prop])==[value],prop
        assert props['wakeup-source']==b''
        assert bytes(u.mem_read(destination+capacity,16))==b'\xa5'*16,'Output buffer guard changed'
    else:
        assert bytes(u.mem_read(LIDOUT,LIDMAX))==b'\xa5'*LIDMAX,'Rejected input changed output'
    lid_cases.append({'case':name,'status':hex(status),'returned_to_sentinel':True,'original_tree_unchanged':True})
report={'status':'passed','scope':'Actual built ARM64 EFI ReadPayload/OpenSSL and T70PrepareLinuxLidTree/libfdt executed in Unicorn; file/heap services mocked; lid helper has no service stubs; no OS/tablet boot',
        'binary':str(IMAGE),'binary_sha256':hashlib.sha256(raw).hexdigest(),'cases':cases,'all_device_profile':features,
        'native_lid':{'cases':lid_cases,'stock_dtb_sha256':stock_sha,'original_node_count':len(original_nodes),'unrelated_properties_and_reservations_preserved':True},
        'source_sha256':hashlib.sha256(application_source.read_bytes()).hexdigest(),
        'support_source_sha256':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [PKG/'Applications/LinuxBoot/LinuxLid.c',PKG/'Applications/LinuxBoot/LinuxLid.h',PKG/'Applications/LinuxBoot/BootPayload.h',PKG/'Linux/platform-modules.py']},'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'limitations':['Firmware filesystem, actual bootloader runtime DTB acquisition, configuration-table lifecycle, boot services exit and kernel launch are not exercised here']}
args.output.write_text(json.dumps(report,indent=2)+'\n')
print('Linux EFI ARM64: 10 file/SHA cases and 8 production lid/libfdt cases passed; pinned payload identities and all device profiles match; no kernel launch')
