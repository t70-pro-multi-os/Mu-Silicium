#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute shipping LK's final kernel handoff and RAM-patch address aliases."""
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
import capstone
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *

BASE, PHYS, STACK, TABLE = 0xFFFF000050F00000, 0x50F00000, 0x70000000, 0xFFFF000068000000
WDT, GPIO = 0xFFFF000010007000, 0xFFFF000010005000
LK_SHA='05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4'


def sha(b):return hashlib.sha256(b).hexdigest()


def handoff(lk,*,cache=True,translation=True):
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for address,size in ((BASE,0x200000),(PHYS,0x200000),(STACK,0x10000),(TABLE,0x2000),(WDT,0x1000),(GPIO,0x1000)):
        cpu.mem_map(address,size)
    cpu.mem_write(BASE,lk);cpu.mem_write(PHYS,lk)
    def q(at,value):cpu.mem_write(at,struct.pack('<Q',value))
    # Fixtures for the original boot mapping list and kernel address-space root.
    cpu.mem_write(BASE+0xB9000,struct.pack('<4Q',0x40000000,0xFFFF000040000000,0x40000000,0))
    cpu.mem_write(BASE+0xB9028,bytes(40))
    space=BASE+0x181378+0x58
    q(space+8,TABLE);q(space+0x10,1);q(space+0x18,BASE);q(space+0x20,0x200000)
    q(TABLE,0x68001003);q(TABLE+0x1000+8,0x40000001 if translation else 0)
    cpu.mem_write(GPIO+0x6F0,struct.pack('<I',1<<10)) # skip panel-specific shutdown
    cpu.mem_write(WDT+0x24,struct.pack('<I',0xA5A5000F))
    cpu.mem_write(BASE+0xBDEAC,b'\x0f')
    cpu.reg_write(UC_ARM64_REG_SP,STACK+0x8000)
    cpu.reg_write(UC_ARM64_REG_X19,0x40000000)
    cpu.reg_write(UC_ARM64_REG_X24,0x47C80000)
    state={'sctlr':0x30D5183D if cache else 0x30D50839,'cache_ops':[],
           'watchdog_writes':[],'fixtures':[],'SMC':None,'rejected':False,'translated_callback':None}
    cs=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_LITTLE_ENDIAN)
    cs.skipdata=True # Literal data occurs between the real trampoline procedures.
    instructions={i.address:(i.mnemonic,i.op_str) for start,end in
        ((0x19B20,0x19D54),(0x10374,0x10380)) for i in cs.disasm(lk[start:end],BASE+start)}
    def ret(value=0):
        cpu.reg_write(UC_ARM64_REG_X0,value);cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
    def code(uc,pc,n,_):
        if pc in (BASE+0x191E8,BASE+0x10FFC,BASE+0x33090,BASE+0x1CD4C):
            state['fixtures'].append(hex(pc-BASE));ret();return
        if pc in (BASE+0x6C0EC,BASE+0x6C1D8):
            state['rejected']=True;uc.emu_stop();return
        if pc==BASE+0x6F520:ret();return
        if pc==BASE+0x19DCC:state['translated_callback']=uc.reg_read(UC_ARM64_REG_X0)
        if pc==PHYS+0x10378:
            state['SMC']=[uc.reg_read(globals()['UC_ARM64_REG_X'+str(i)]) for i in range(5)]
            uc.emu_stop();return
        op= instructions.get(pc)
        if not op:return
        mnemonic,operands=op
        if mnemonic=='mrs':
            reg,system=operands.split(', ')
            values={'sctlr_el1':state['sctlr'],'clidr_el1':0x01000003,
                    'ccsidr_el1':(3<<13)|(3<<3)|2,'id_aa64mmfr2_el1':0}
            if system in values:
                uc.reg_write(globals()['UC_ARM64_REG_'+reg.upper()],values[system]);uc.reg_write(UC_ARM64_REG_PC,pc+4)
        elif mnemonic=='msr' and operands.startswith('sctlr_el1, '):
            reg=operands.split(', ')[1]
            state['sctlr']=uc.reg_read(globals()['UC_ARM64_REG_'+reg.upper()])
            uc.reg_write(UC_ARM64_REG_PC,pc+4)
        if mnemonic in ('dc','ic','tlbi'):state['cache_ops'].append((mnemonic,operands))
    def write(uc,access,at,n,value,_):
        assert ((STACK<=at and at+n<=STACK+0x10000) or
                (WDT<=at and at+n<=WDT+0x100) or
                (at==BASE+0xBDEAC and n==1)),hex(at)
        if WDT<=at<WDT+0x100:state['watchdog_writes'].append((at-WDT,n,value))
    cpu.hook_add(UC_HOOK_CODE,code);cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    # Start at the original argument setup in boot_linux_fdt; execute its real
    # arch_chain_load, platform watchdog calls, page-table walker and trampoline.
    cpu.emu_start(BASE+0xFE14,0,count=100000)
    assert state['rejected']==(not translation)
    assert state['watchdog_writes']==[(0x24,4,0xA5A50000),(4,4,0xF008),(8,4,0x1971)]
    if translation:
        assert state['translated_callback']==PHYS+0x10374
        assert state['SMC']==[0xC2000115,0x40000000,0x47C80000,0,1],state
        assert state['sctlr']&0x1005==0,hex(state['sctlr'])
        cache_ops=state['cache_ops']
        assert sum(m=='dc' and o.startswith(('cisw,' if cache else 'isw,')) for m,o in cache_ops)==16
        assert ('ic','iallu') in cache_ops and ('tlbi','vmalle1') in cache_ops
        assert len(state['fixtures'])==4
    else:assert state['SMC'] is None
    return {'case':('enabled' if cache else 'disabled')+'_entry_cache_'+('mapped' if translation else 'missing')+'_callback',
        'shipping_virtual_to_physical_page_table_walker_executed':True,
        'callback_physical_address':hex(state['translated_callback']) if state['translated_callback'] else None,
        'shipping_platform_watchdog_length_value':'0xf008','shipping_platform_watchdog_reload':'0x1971',
        'original_watchdog_mode_not_changed':True,
        'SMC_arguments':[hex(v) for v in state['SMC']] if state['SMC'] else None,
        'SCTLR_final_fixture':hex(state['sctlr']),'cache_maintenance_instruction_count':len(state['cache_ops']),
        'invalid_address_translation_rejected':state['rejected'],'EL3_monitor_executed':False}


def aliases(lk,plan,image,headers,consumer_path):
    spec=importlib.util.spec_from_file_location('lk_consumer',consumer_path)
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    results=[]
    for base in (PHYS,BASE):
        mod.BASE=base
        assert mod.registration(lk,plan)['shipping_registration_creates_boot_and_preserves_continue']
        for case in ('valid','arguments','locked','seccfg_error','allocation_failure'):
            mod.command(lk,plan,image,case)
        for pending in (False,True):mod.parsed_sources(lk,plan,image,headers,pending)
        results.append({'execution_base':hex(base),'actual_registration_executed':True,
                        'command_cases':5,'original_partition_and_header_consumer_cases':2,
                        'position_independent_RAM_payload_verified':True})
    return results


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('lk','payload','symbols','plan-tool','consumer-test','image','backup','report'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();doc={'all_passed':False,'status':'running'}
    def save():a.report.write_text(json.dumps(doc,indent=2)+'\n')
    save();lk=a.lk.read_bytes();assert sha(lk)==LK_SHA
    payload=a.payload.read_bytes();image=a.image.read_bytes()
    spec=importlib.util.spec_from_file_location('lk_plan',a.plan_tool)
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
    plan=mod.make_plan(lk,payload,mod.symbols(a.symbols))
    parts=json.loads((a.backup/'partition-layout.json').read_text())['partitions'];headers={}
    with (a.backup/'images/ufs-lu2.img').open('rb') as f:
        for name in ('boot','vendor_boot','init_boot'):
            part=next(x for x in parts if x['name']==name+'_a');f.seek(part['offset_bytes']);headers[name]=f.read(4096)
    cases=[handoff(lk,cache=cache,translation=translation) for cache,translation in ((True,True),(False,True),(True,False))]
    pic=aliases(lk,plan,image,headers,a.consumer_test)
    doc.update({'status':'passed','all_passed':True,'shipping_LK_sha256':sha(lk),
        'candidate_image_sha256':sha(image),'payload_sha256':sha(payload),
        'test_source_sha256':sha(Path(__file__).read_bytes()),'consumer_test_source_sha256':sha(a.consumer_test.read_bytes()),
        'cases':cases,'RAM_payload_address_alias_cases':pic,
        'scope':'Actual stock LK tail argument setup, platform watchdog length/reload functions, original VA-to-PA mapping/page-table walk, cache cleanup loop and final SMC argument assembly; no replacement kernel jump.',
        'fixtures':{'kernel_virtual_address_space_and_boot_mapping_list':'synthetic mapping and two-level tables using shipping structure layout',
            'SCTLR_and_cache_feature_registers':'read/write values modeled; actual mask and set/way loop instructions execute',
            'peripheral_and_secure_quiesce_calls':['0x191e8','0x10ffc','0x33090','0x1cd4c'],
            'watchdog_registers':'synthetic MMIO; no real ticking or reset','console':'log function returns without output'},
        'real_MMU_cache_coherency_or_EL3_behavior_tested':False,'watchdog_mode_enabled_by_this_path':False,
        'UEFI_executed_on_tablet':False,'tablet_execution_observed':False,
        'remaining':['Real boot MMU/cache and EL3 transition','Enabled watchdog mode and physical reset boot target','Actual EDK2 entry and Android return']})
    save();print(json.dumps({'all_passed':True,'original_LK_handoff_cases':len(cases),'RAM_payload_address_aliases':len(pic),'tablet_execution_observed':False}))


if __name__=='__main__':main()
