#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the compiled probe and original preloader code, including failure cases."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import *

BASE, ENTRY, PORT, RETURN = 0x200f10, 0x40200000, 0x2f35a8, 0x2091ca
WINDOWS = ((0x201170,12),(0x208f24,76),(0x209178,84),(0x20949c,64))


def exercise(preloader, payload, case):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0x200000, 0x100000)
    uc.mem_map(0x401ff000, 0x3000)
    uc.mem_map(0x41000000, 0x1000)
    uc.mem_write(BASE, preloader)
    uc.mem_write(ENTRY, payload)
    port, table, callback = 0x41000000, 0x41000020, 0x240001
    arg, arg_size, ret = 0x401ffff4, 12, RETURN | 1
    success = case in ('ARM_callback','Thumb_callback')
    if case == 'ARM_callback': callback &= ~1
    elif case.startswith('code_mismatch_'):
        address = WINDOWS[int(case.rsplit('_',1)[1])][0]
        old = bytes(uc.mem_read(address,1))[0]
        uc.mem_write(address,bytes([old ^ 1]))
    elif case == 'wrong_argument': arg += 4
    elif case == 'wrong_argument_size': arg_size += 4
    elif case == 'wrong_return_address': ret += 4
    elif case.startswith('port_'):
        port = {'port_null':0,'port_unaligned':0x41000001,
                'port_outside_RAM':0x10000000,'port_crosses_end':0x4ffffff8}[case]
    elif case.startswith('table_'):
        table = {'table_null':0,'table_unaligned':0x41000021,
                 'table_outside_RAM':0x10000000,'table_crosses_end':0x2ffff8}[case]
    elif case.startswith('callback_'):
        callback = {'callback_before_code':BASE-4,'callback_after_code':BASE+len(preloader),
                    'callback_unaligned_ARM':0x240002}[case]
    uc.mem_write(PORT, struct.pack('<I',port))
    uc.mem_write(0x41000008, struct.pack('<I',table))
    uc.mem_write(0x41000020, struct.pack('<I',callback))
    stack = 0x2fe000
    uc.reg_write(UC_ARM_REG_CPSR,0x13)
    uc.reg_write(UC_ARM_REG_SP,stack)
    uc.reg_write(UC_ARM_REG_R0,ENTRY)
    uc.reg_write(UC_ARM_REG_R1,arg)
    uc.reg_write(UC_ARM_REG_R2,arg_size)
    uc.reg_write(UC_ARM_REG_R4,arg)
    uc.reg_write(UC_ARM_REG_R5,arg_size)
    uc.reg_write(UC_ARM_REG_LR,ret)
    saved = {}
    for n in range(6,13):
        reg = globals()['UC_ARM_REG_R'+str(n)]
        saved[reg] = 0x70000000+n
        uc.reg_write(reg,saved[reg])
    records, writes, returned = [], [], []

    def code(cpu,address,size,_):
        if address == 0x240000:
            pointer,length = cpu.reg_read(UC_ARM_REG_R0),cpu.reg_read(UC_ARM_REG_R1)
            assert success, ('invalid state reached USB',case)
            assert length==40 and ENTRY<=pointer<=ENTRY+len(payload)-length
            records.append(bytes(cpu.mem_read(pointer,length)))
            cpu.reg_write(UC_ARM_REG_R0,length)
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
        elif address == ret & ~1:
            returned.append(True)
            cpu.emu_stop()

    def write(cpu,access,address,size,value,_):
        assert (stack-256<=address and address+size<=stack) or \
               (ENTRY<=address and address+size<=ENTRY+len(payload)), hex(address)
        writes.append((address,size))

    uc.hook_add(UC_HOOK_CODE,code)
    uc.hook_add(UC_HOOK_MEM_WRITE,write)
    assert preloader[0x201170-BASE:0x20117c-BASE] == bytes.fromhex('0140a0e10250a0e100f0a0e1')
    # Success runs the actual shipping jump tail. Failure cases enter the probe
    # directly so a damaged jump-tail instruction cannot hide its guard behavior.
    uc.emu_start(0x201170 if success else ENTRY,0,count=10000)
    assert returned and writes, case
    assert len(records)==int(success), case
    if success:
        magic,version,pc,cpsr,sctlr,mpidr,a,n,lr=struct.unpack('<8s8I',records[0])
        assert magic==b'T70RAM32' and version==2 and pc==ENTRY
        assert cpsr & 0x3f==0x13 and a==arg and n==arg_size and lr==ret
    assert uc.reg_read(UC_ARM_REG_SP)==stack
    parser_restored=success or case.startswith(('port_','table_','callback_'))
    assert uc.reg_read(UC_ARM_REG_R4)==arg
    assert uc.reg_read(UC_ARM_REG_R5)==(stack+0x10 if parser_restored else arg_size)
    assert all(uc.reg_read(reg)==value for reg,value in saved.items())
    return {'case':case,'USB_response_emitted':success,
            'returned_with_original_stack_and_caller_registers':True,
            'writes_limited_to_probe_and_existing_stack':True}


def region_check(preloader,address,size):
    uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    uc.mem_map(0x200000,0x100000)
    uc.mem_map(0x5000000,0x1000)
    uc.mem_write(BASE,preloader)
    uc.reg_write(UC_ARM_REG_CPSR,0x13)
    uc.reg_write(UC_ARM_REG_SP,0x2fe000)
    uc.reg_write(UC_ARM_REG_LR,0x5000001)
    uc.reg_write(UC_ARM_REG_R0,address)
    uc.reg_write(UC_ARM_REG_R1,size)
    uc.reg_write(UC_ARM_REG_R2,1)
    returned=[]
    def code(cpu,pc,n,_):
        if pc==0x233274: # log only; no hardware or USB in this range checker
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
        elif pc==0x5000000:
            returned.append(True)
            cpu.emu_stop()
    uc.hook_add(UC_HOOK_CODE,code)
    uc.emu_start(0x235a19,0,count=10000)
    assert returned
    return uc.reg_read(UC_ARM_REG_R0)


def following_send_da(preloader,payload,record_size=40):
    """Run the real next command after the probe, not just its return instruction."""
    uc=Uc(UC_ARCH_ARM,UC_MODE_ARM)
    uc.mem_map(0x200000,0x100000)
    uc.mem_map(0x401ff000,0x3000)
    uc.mem_map(0x41000000,0x1000)
    uc.mem_write(BASE,preloader)
    uc.mem_write(ENTRY,payload)
    uc.mem_write(PORT,struct.pack('<I',0x41000000))
    uc.mem_write(0x41000008,struct.pack('<I',0x41000020))
    uc.mem_write(0x41000020,struct.pack('<II',0x240001,0x240005))
    stack=0x2fe000
    uc.mem_write(stack+0x174,struct.pack('<I',0x224e5d))
    for reg,value in ((UC_ARM_REG_CPSR,0x13),(UC_ARM_REG_SP,stack),
                      (UC_ARM_REG_R0,ENTRY),(UC_ARM_REG_R1,0x401ffff4),
                      (UC_ARM_REG_R2,12),(UC_ARM_REG_LR,RETURN|1),
                      (UC_ARM_REG_R7,stack+0x170),(UC_ARM_REG_R8,PORT),
                      (UC_ARM_REG_R9,stack+0x50),(UC_ARM_REG_R10,stack+7),
                      (UC_ARM_REG_R11,0)):
        uc.reg_write(reg,value)
    seen=[]
    def code(cpu,pc,n,_):
        if pc==0x240000:
            size=cpu.reg_read(UC_ARM_REG_R1)
            assert size in (1,record_size)
            cpu.reg_write(UC_ARM_REG_R0,size)
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
        elif pc==0x240004:
            dest=cpu.reg_read(UC_ARM_REG_R0)
            assert dest==stack+7 and cpu.reg_read(UC_ARM_REG_R1)==1
            cpu.mem_write(dest,b'\xd7')
            cpu.reg_write(UC_ARM_REG_R0,0)
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
        elif pc==0x23041c:
            cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))
        elif pc==0x209634:
            dest=cpu.reg_read(UC_ARM_REG_R0)
            assert dest==stack+0x10, ('next SEND_DA header uses wrong address',hex(dest))
            seen.append(True)
            cpu.emu_stop()
    uc.hook_add(UC_HOOK_CODE,code)
    uc.emu_start(0x201170,0,count=10000)
    assert seen
    return {'actual_next_SEND_DA_handler_reached':True,'header_destination_is_original_stack_buffer':True}


def main():
    p=argparse.ArgumentParser()
    for name in ('preloader','payload','report'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    preloader,payload=a.preloader.read_bytes(),a.payload.read_bytes()
    assert struct.unpack_from('<I',preloader,0x1c)[0]==BASE
    assert 40<len(payload)<=4096 and len(payload)%4==0
    cases=['ARM_callback','Thumb_callback','wrong_argument','wrong_argument_size','wrong_return_address']
    cases += ['code_mismatch_'+str(i) for i in range(len(WINDOWS))]
    cases += ['port_null','port_unaligned','port_outside_RAM','port_crosses_end',
              'table_null','table_unaligned','table_outside_RAM','table_crosses_end',
              'callback_before_code','callback_after_code','callback_unaligned_ARM']
    continuation=following_send_da(preloader,payload)
    results=[exercise(preloader,payload,case) for case in cases]
    regions=[]
    for address,size,allowed in ((0x201170,12,False),(ENTRY,len(payload),False),
                                 (0x10007000,4,True),(0x10008000,4,False)):
        status=region_check(preloader,address,size)
        assert (status==0)==allowed, (hex(address),hex(status))
        regions.append({'address':hex(address),'allowed':allowed,'status':hex(status)})
    result={'scope':'Actual compiled probe, original jump tail and original USB read-region checker; USB callback is a fixture',
            'probe_version':2,'preloader_sha256':hashlib.sha256(preloader).hexdigest(),
            'payload_sha256':hashlib.sha256(payload).hexdigest(),'payload_bytes':len(payload),
            'entry_address':ENTRY,'cases':results,'original_read_region_checker':regions,
            'live_code_verification_before_USB_callback':True,'following_shipping_SEND_DA_header_verified':continuation,'all_passed':True,
            'tablet_execution_observed':False,'UEFI_execution_observed':False}
    a.report.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'all_passed':True,'guard_cases':len(results),'original_region_check_cases':len(regions),
                      'payload_bytes':len(payload),'tablet_execution_observed':False}))

if __name__=='__main__':
    main()
