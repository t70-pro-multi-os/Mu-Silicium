#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the compiled RAM patch and the actual shipping LK consumers on an ARM64 CPU."""
import argparse
import hashlib
import importlib.util
import json
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *

sys.dont_write_bytecode = True
BASE = 0xffff000050f00000
DOWNLOAD, HEAP, FIXTURE, STACK, STOP = 0x60000000, 0x61000000, 0x62000000, 0x70000000, 0x7fff0000


def u32(cpu, address):
    return struct.unpack('<I', cpu.mem_read(address, 4))[0]


def u64(cpu, address):
    return struct.unpack('<Q', cpu.mem_read(address, 8))[0]


def put32(cpu, address, value):
    cpu.mem_write(address, struct.pack('<I', value))


class Machine:
    def __init__(self, lk, plan, locked=3, security_status=0, allocation_fail=False):
        self.cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for address, size in ((BASE, 0x200000), (DOWNLOAD, 0x200000), (HEAP, 0x200000),
                              (FIXTURE, 0x20000), (STACK, 0x10000), (STOP, 0x1000)):
            self.cpu.mem_map(address, size)
        self.cpu.mem_write(BASE, lk)
        for edit in plan['edits']:
            where = BASE + edit['offset']
            old, new = bytes.fromhex(edit['expected_hex']), bytes.fromhex(edit['replacement_hex'])
            assert bytes(self.cpu.mem_read(where, len(old))) == old
            self.cpu.mem_write(where, new)
        self.names = plan['symbols']
        self.locked, self.security_status = locked, security_status
        self.allocation_fail = allocation_fail
        self.allocations, self.responses, self.events = [], [], []
        self.next_heap = HEAP
        self.returns, self.write_count, self.instruction_count = 0, 0, 0
        self.initial_regs = {}
        for n in range(18, 30):
            reg = globals()['UC_ARM64_REG_X'+str(n)]
            self.initial_regs[reg] = 0x12340000+n
            self.cpu.reg_write(reg, 0x12340000+n)
        self.cpu.reg_write(UC_ARM64_REG_SP, STACK+0x8000)
        self.cpu.hook_add(UC_HOOK_CODE, self.code)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE, self.write)

    def string(self, address):
        data = bytearray()
        for i in range(128):
            c = bytes(self.cpu.mem_read(address+i, 1))
            if c == b'\0':
                return data.decode()
            data.extend(c)
        raise AssertionError('Unterminated fixture string')

    def ret(self, value=0):
        self.cpu.reg_write(UC_ARM64_REG_X0, value)
        self.cpu.reg_write(UC_ARM64_REG_PC, self.cpu.reg_read(UC_ARM64_REG_LR))

    def code(self, cpu, pc, size, _):
        self.instruction_count += 1
        if pc == STOP or pc == getattr(self, 'stop_at', None):
            self.returns += 1
            cpu.emu_stop()
        elif pc == BASE+0x6c68c:
            size = cpu.reg_read(UC_ARM64_REG_X0)
            self.allocations.append(size)
            if self.allocation_fail:
                self.ret(0)
            else:
                address = self.next_heap
                self.next_heap += (size+15) & ~15
                assert self.next_heap <= HEAP+0x200000
                self.ret(address)
        elif pc == BASE+0x83d9c:
            # Only the seccfg I/O is a fixture; the real getter's error/output code runs.
            put32(cpu, BASE+0x183d4c, self.locked)
            self.ret(self.security_status)
        elif pc == BASE+0x7964:
            self.responses.append((self.string(cpu.reg_read(UC_ARM64_REG_X0)),
                                   self.string(cpu.reg_read(UC_ARM64_REG_X1))))
            self.ret(0)
        elif pc == BASE+0x11cb0:
            assert cpu.reg_read(UC_ARM64_REG_X0) == BASE+0xbbc38
            self.events.append('original_boot_event')
            self.ret(0)
        elif pc in (BASE+0x6f520, BASE+0x13594, BASE+0x8facc):
            self.ret(0)
        elif pc == BASE+0x6c1d8:
            raise AssertionError('Original LK assertion reached at '+hex(cpu.reg_read(UC_ARM64_REG_LR)))
        elif pc == BASE+0x8e184:
            self.ret(0)  # AVB result was already obtained by the unmodified loader.
        elif pc == BASE+0x1a810:
            self.ret(FIXTURE+0x18000)
        elif pc == BASE+0x8d9ac:
            cpu.mem_write(cpu.reg_read(UC_ARM64_REG_X2), b'\0')
            self.ret(1)

    def write(self, cpu, access, address, size, value, _):
        permitted = ((BASE+0xc8000, BASE+0x200000), (HEAP, HEAP+0x200000),
                     (FIXTURE, FIXTURE+0x20000), (STACK, STACK+0x10000))
        assert any(start <= address and address+size <= end for start, end in permitted), hex(address)
        self.write_count += 1

    def run(self, offset, args=(), stop=None, check_registers=True):
        for i, value in enumerate(args):
            self.cpu.reg_write(globals()['UC_ARM64_REG_X'+str(i)], value)
        self.cpu.reg_write(UC_ARM64_REG_LR, STOP)
        self.stop_at = BASE+stop if stop is not None else None
        previous = self.returns
        self.cpu.emu_start(BASE+offset, 0, count=4000000)
        assert self.returns == previous+1, ('CPU did not return', hex(self.cpu.reg_read(UC_ARM64_REG_PC)))
        assert self.cpu.reg_read(UC_ARM64_REG_SP) == STACK+0x8000
        if check_registers:
            assert all(self.cpu.reg_read(reg) == value for reg, value in self.initial_regs.items())
        return self.cpu.reg_read(UC_ARM64_REG_X0)


def registration(lk, plan):
    m = Machine(lk, plan)
    m.run(0x8358, (BASE+0xa5212, BASE+0x8d58, 0, 0), stop=0x835c)
    current = u64(m.cpu, BASE+0xc8120)
    nodes = []
    while current:
        nodes.append((m.string(u64(m.cpu, current+8)), u32(m.cpu, current+16),
                      u32(m.cpu, current+20), u32(m.cpu, current+24), u64(m.cpu, current+32)))
        current = u64(m.cpu, current)
    assert nodes == [('boot', 4, 1, 1, BASE+plan['symbols']['ram_boot']),
                     ('continue', 8, 0, 0, BASE+0x8d58)]
    return {'shipping_registration_creates_boot_and_preserves_continue': True,
            'original_registration_allocates_both_nodes': m.allocations == [40, 40]}


def dispatcher_permissions(lk,plan):
    cases=[]
    for secure,locked,old_flags in ((0,3,False),(1,3,False),(0,2,False),(1,2,False),
                                    (1,3,True),(1,2,True),(0,3,True)):
        m=Machine(lk,plan,locked=locked)
        m.run(0x8358,(BASE+0xa5212,BASE+0x8d58,0,0),stop=0x835c)
        node=u64(m.cpu,BASE+0xc8120)
        assert m.string(u64(m.cpu,node+8))=='boot'
        if old_flags:
            put32(m.cpu,node+20,0);put32(m.cpu,node+24,0)
        sp=STACK+0x8000
        m.cpu.reg_write(UC_ARM64_REG_X29,sp+0x40)
        m.cpu.reg_write(UC_ARM64_REG_X28,node)
        m.cpu.reg_write(UC_ARM64_REG_X25,FIXTURE)
        m.cpu.reg_write(UC_ARM64_REG_X20,BASE+0xc8000)
        m.cpu.mem_write(FIXTURE,b'boot\0')
        put32(m.cpu,sp+0x3c,secure);put32(m.cpu,sp+8,locked)
        put32(m.cpu,BASE+0xc8118,1)
        # Execute the actual shipping dispatch permission branches and handler
        # call. Only the already-obtained security-control and lock values are
        # fixtures. The handler still executes its original raw lock getter.
        m.run(0x7e58,stop=0x7e9c,check_registers=False)
        invoked=(not secure if old_flags else locked==3)
        assert bool(m.responses)==bool(invoked)
        if invoked:assert m.responses==[('FAIL','RAM boot: unlocked, valid EDK2 image required')]
        assert not m.events
        cases.append({'security_control':secure,'raw_lock_state':locked,
                      'old_registration_flags':old_flags,'RAM_handler_invoked':bool(invoked),
                      'original_dispatcher_instructions_executed':True,'boot_event_absent':True})
    return cases


def command(lk, plan, image, case):
    m = Machine(lk, plan, locked=2 if case=='locked' else 3,
                security_status=1 if case=='seccfg_error' else 0,
                allocation_fail=case=='allocation_failure')
    data = bytearray(image)
    size, maximum, pointer, arguments = len(data), 0x100000, DOWNLOAD, b'\0'
    if case=='arguments': arguments=b'x\0'
    elif case=='null_buffer': pointer=0
    elif case=='pending': size |= 0x80000000
    elif case=='short': size=4095
    elif case=='over_maximum': maximum=len(data)-1
    elif case=='magic': data[0] ^= 1
    elif case=='version': struct.pack_into('<I',data,40,3)
    elif case=='header_size': struct.pack_into('<I',data,20,1580)
    elif case=='ramdisk': struct.pack_into('<I',data,12,1)
    elif case=='zero_kernel': struct.pack_into('<I',data,8,0)
    elif case=='huge_kernel': struct.pack_into('<I',data,8,0xffffffff)
    elif case=='truncated_kernel': size=4096
    elif case=='huge_signature': struct.pack_into('<I',data,1580,0xffffffff)
    elif case=='truncated_signature': size-=1
    m.cpu.mem_write(DOWNLOAD, bytes(data))
    m.cpu.mem_write(FIXTURE, arguments)
    m.cpu.mem_write(FIXTURE+0x18000, b'fixture-commandline\0')
    m.cpu.mem_write(BASE+0xc8138, struct.pack('<Q',pointer))
    put32(m.cpu, BASE+0xc8140, maximum)
    put32(m.cpu, BASE+0xc8178, size)
    before = bytes(m.cpu.mem_read(BASE+0xc8138,68))
    m.run(plan['symbols']['ram_boot'], (FIXTURE, pointer, size))
    if case=='valid':
        assert m.responses == [('OKAY','')] and m.events == ['original_boot_event']
        assert u32(m.cpu,BASE+0xc8118)==4
        assert m.allocations==[len(data)]
        assert u64(m.cpu,BASE+0xc8138)==HEAP
        assert u32(m.cpu,BASE+0xc8140)==len(data)
        assert u32(m.cpu,BASE+0xc8178)==len(data)|0x80000000
        assert bytes(m.cpu.mem_read(HEAP,len(data)))==bytes(data)
        m.cpu.mem_write(DOWNLOAD,b'overwritten original fastboot transport buffer')
        assert bytes(m.cpu.mem_read(HEAP,len(data)))==bytes(data)
    else:
        assert len(m.responses)==1 and m.responses[0][0]=='FAIL' and not m.events
        assert bytes(m.cpu.mem_read(BASE+0xc8138,68))==before
        assert m.allocations==([len(data)] if case=='allocation_failure' else [])
    return m, {'case':case,'accepted':case=='valid','original_callee_registers_and_stack_preserved':True,
               'no_MMIO_or_storage_writes':True}


def source_guards(lk, plan):
    cases=[]
    for name, pending, pointer in (('init_boot',True,HEAP),('vendor_boot',True,HEAP),
                                    ('boot_a',True,HEAP),('boot',False,HEAP),('boot',True,0),
                                    ('boot',True,HEAP)):
        m=Machine(lk,plan)
        m.cpu.mem_write(FIXTURE,name.encode()+b'\0')
        entry=FIXTURE+0x100
        m.cpu.mem_write(entry,struct.pack('<QQQQ',FIXTURE,DOWNLOAD,12345,0x11223344))
        m.cpu.mem_write(BASE+0xc8138,struct.pack('<Q',pointer))
        put32(m.cpu,BASE+0xc8178,8192|(0x80000000 if pending else 0))
        m.cpu.reg_write(UC_ARM64_REG_X8,entry)
        expected=name=='boot' and pending and pointer!=0
        m.run(0x5b5e4,(0,FIXTURE),stop=0x5b5e8,check_registers=False)
        assert u64(m.cpu,entry+8)==(HEAP if expected else DOWNLOAD)
        assert u64(m.cpu,entry+16)==(8192 if expected else 12345)
        assert u64(m.cpu,entry+24)==0x11223344
        assert m.cpu.reg_read(UC_ARM64_REG_X27)==(HEAP if expected else DOWNLOAD)
        assert bool(u32(m.cpu,BASE+0xc8178)&0x80000000)==(pending and not expected)
        cases.append({'partition':name,'pending':pending,'pointer_nonzero':bool(pointer),'replaced':expected})
    return cases


def parsed_sources(lk,plan,image,stock_headers,pending):
    m,_=command(lk,plan,image,'valid')
    if not pending: put32(m.cpu,BASE+0xc8178,len(image))
    # Execute the original complete AVB partition-selection consumer, including
    # real header parsing, header validation and body-source publication.
    names=('boot','vendor_boot','init_boot')
    original_sources={}
    table=FIXTURE+0x1000
    for i,name in enumerate(names):
        name_addr=FIXTURE+0x200+i*64
        src=FIXTURE+0x4000+i*0x4000
        original_sources[name]=src
        m.cpu.mem_write(name_addr,name.encode()+b'\0')
        m.cpu.mem_write(src,stock_headers[name])
        m.cpu.mem_write(table+32*i,struct.pack('<QQQQ',name_addr,src,len(stock_headers[name]),0))
    slot=FIXTURE+0x2000
    m.cpu.mem_write(slot+0x18,struct.pack('<QQQ',table,3,FIXTURE+0x18000))
    m.run(0x5b534,(0,0,slot))
    expected_boot=HEAP if pending else original_sources['boot']
    assert u64(m.cpu,BASE+0xc9900)==expected_boot
    assert u64(m.cpu,BASE+0xc9908)==original_sources['vendor_boot']
    assert u64(m.cpu,BASE+0xc9910)==original_sources['init_boot']
    assert bytes(m.cpu.mem_read(BASE+0xc90a0,0x850))==stock_headers['vendor_boot'][:0x850]
    assert bytes(m.cpu.mem_read(BASE+0xc83f0,0x630))==stock_headers['init_boot'][:0x630]
    expected_kernel=struct.unpack_from('<I',image if pending else stock_headers['boot'],8)[0]
    assert u32(m.cpu,BASE+0xc8a28)==expected_kernel
    assert m.run(0xc51c)==4
    assert m.run(0xc5ec)==0x40000000
    assert m.run(0xc6e4)==expected_boot+4096
    assert m.run(0xc768)==(expected_kernel+4095)&~4095
    assert u64(m.cpu,table+8)==expected_boot
    assert u64(m.cpu,table+16)==(len(image) if pending else len(stock_headers['boot']))
    assert not u32(m.cpu,BASE+0xc8178)&0x80000000
    return {'pending_RAM_request':pending,'boot_header_and_body_pointer_match_selected_image':True,
            'shipping_header_getters_verified':True,'kernel_load_address':'0x40000000',
            'vendor_boot_header_and_source_preserved':True,'init_boot_header_and_source_preserved':True}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('lk','payload','symbols','plan_tool','image','backup','report'):
        p.add_argument('--'+name.replace('_','-'),type=Path,required=True)
    a=p.parse_args()
    spec=importlib.util.spec_from_file_location('lk_plan',a.plan_tool)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    lk,payload,image=a.lk.read_bytes(),a.payload.read_bytes(),a.image.read_bytes()
    plan=module.make_plan(lk,payload,module.symbols(a.symbols))
    assert image[:8]==b'ANDROID!' and struct.unpack_from('<I',image,40)[0]==4
    parts=json.loads((a.backup/'partition-layout.json').read_text())['partitions']
    headers={}
    with (a.backup/'images/ufs-lu2.img').open('rb') as f:
        for name in ('boot','vendor_boot','init_boot'):
            part=next(x for x in parts if x['name']==name+'_a')
            f.seek(part['offset_bytes']);headers[name]=f.read(4096)
    # Make the baseline boot size distinguishable while retaining real stock headers.
    stock_kernel=struct.unpack_from('<I',headers['boot'],8)[0]
    assert stock_kernel!=struct.unpack_from('<I',image,8)[0]
    registered=registration(lk,plan)
    permissions=dispatcher_permissions(lk,plan)
    cases=['valid','arguments','locked','seccfg_error','null_buffer','pending','short','over_maximum',
           'magic','version','header_size','ramdisk','zero_kernel','huge_kernel','truncated_kernel',
           'huge_signature','truncated_signature','allocation_failure']
    results=[command(lk,plan,image,case)[1] for case in cases]
    sources=source_guards(lk,plan)
    parsed=[parsed_sources(lk,plan,image,headers,pending) for pending in (False,True)]
    result={'scope':'Compiled RAM patch plus original G3D1 LK command registration, raw lock-state getter, memcpy, continue/event, partition selection, header parser and getters. Heap allocation, seccfg I/O, USB, event scheduler and AVB-result checks are fixtures.',
            'shipping_lk_sha256':hashlib.sha256(lk).hexdigest(),
            'payload_sha256':hashlib.sha256(payload).hexdigest(),'payload_bytes':len(payload),
            'candidate_image_sha256':hashlib.sha256(image).hexdigest(),
            'registration':registered,'original_dispatcher_permission_cases':permissions,'command_cases':results,'source_guards':sources,
            'original_header_and_body_consumers':parsed,'all_passed':True,
            'tablet_execution_observed':False,'UEFI_execution_observed':False,'storage_writes':[]}
    a.report.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'all_passed':True,'command_cases':len(results),'dispatcher_permission_cases':len(permissions),'source_guards':len(sources),
                      'original_consumer_cases':len(parsed),'payload_bytes':len(payload),
                      'tablet_execution_observed':False}))


if __name__=='__main__':
    main()
