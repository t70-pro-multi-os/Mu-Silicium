#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute original preloader frames, the compiled ARM32 bridge and ARM64 BL2 bridge."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import *
from unicorn.arm64_const import *

PRE, ENTRY, BL2, LK = 0x200f10, 0x40200000, 0x62f00000, 0xffff000050f00000
BL2_VA, STACK, STOP, RVBAR = 0xffff000062f00000, 0x70000000, 0x7fff0000, 0x0c53c900


def digest(b):
    return hashlib.sha256(b).hexdigest()


def word(b, offset=0):
    return struct.unpack_from('<I',b,offset)[0]


class Preloader:
    def __init__(self, preloader, stage):
        self.cpu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
        for base,size in ((0x200000,0x100000),(0x401ff000,0x5000),(0x41000000,0x1000),
                          (BL2,0x100000),(STOP,0x1000),(RVBAR&~4095,4096)):
            self.cpu.mem_map(base,size)
        self.cpu.mem_write(PRE,preloader);self.cpu.mem_write(ENTRY,stage)
        self.stage=stage
        self.cpu.mem_write(0x2f35a8,struct.pack('<I',0x41000000))
        self.cpu.mem_write(0x41000008,struct.pack('<I',0x41000020))
        self.cpu.mem_write(0x41000020,struct.pack('<II',0x240001,0x240005))
        self.original_regs={}
        for n in range(4,13):
            reg=globals()['UC_ARM_REG_R'+str(n)]
            self.original_regs[reg]=0x12340000+n
            self.cpu.reg_write(reg,0x12340000+n)
        self.cpu.reg_write(UC_ARM_REG_CPSR,0x13)
        self.cpu.reg_write(UC_ARM_REG_SP,0x2fe000)
        self.cpu.reg_write(UC_ARM_REG_LR,STOP|1)
        self.records,self.shutdowns,self.returns,self.rvbar_writes=[],0,0,[]
        self.stop_at=None
        self.resume_case='valid'
        self.entry_sctlr_fixture=None
        self.data_cleans,self.instruction_invalidates=0,0
        self.cpu.hook_add(UC_HOOK_CODE,self.code)
        self.cpu.hook_add(UC_HOOK_MEM_WRITE,self.write)

    def ret(self,value=0):
        self.cpu.reg_write(UC_ARM_REG_R0,value)
        self.cpu.reg_write(UC_ARM_REG_PC,self.cpu.reg_read(UC_ARM_REG_LR))

    def code(self,cpu,pc,n,_):
        if ENTRY<=pc<ENTRY+len(self.stage):
            instruction=word(bytes(cpu.mem_read(pc,4)))
            if instruction==0xee110f10 and self.entry_sctlr_fixture is not None:
                cpu.reg_write(UC_ARM_REG_R0,self.entry_sctlr_fixture)
                cpu.reg_write(UC_ARM_REG_PC,pc+4)
            if instruction&0xffff0fff==0xee070f3a:self.data_cleans+=1
            if instruction&0xffff0fff==0xee070f15:self.instruction_invalidates+=1
        if pc==STOP or pc==self.stop_at:
            self.returns+=1;cpu.emu_stop()
        elif pc in (0x23041c,0x230420,0x233274):
            self.ret(0)
        elif pc==0x23b51c:
            self.shutdowns+=1;self.ret(0)
        elif pc==0x240000:
            address,size=cpu.reg_read(UC_ARM_REG_R0),cpu.reg_read(UC_ARM_REG_R1)
            assert size in (40,64) and ENTRY<=address<=ENTRY+len(self.stage)-size
            self.records.append(bytes(cpu.mem_read(address,size)))
            self.ret(size)

        elif pc==0x240004:
            address,size=cpu.reg_read(UC_ARM_REG_R0),cpu.reg_read(UC_ARM_REG_R1)
            assert size==8 and ENTRY<=address<=ENTRY+len(self.stage)-8
            assert cpu.reg_read(UC_ARM_REG_R2)==0 and len(self.records) in (1,2)
            if len(self.records)==2:
                assert self.records[-1][:8]==b'T70ARM32'
                cpu.mem_write(address,b'T70RUN!!');self.ret(8);return
            assert bytes(cpu.mem_read(0x230f44,12))==self.original_rvbar
            assert word(cpu.mem_read(self.parser_sp+0x174,4))==0x224e5d
            cpu.mem_write(address,b'T70NOPE!' if self.resume_case=='bad_resume' else
                          b'T70\0\0\0\0\0' if self.resume_case=='read_error' else b'T70GO32!')
            self.ret(1 if self.resume_case=='read_error' else 8 if self.resume_case=='read_count8' else 0)

    def write(self,cpu,access,address,size,value,_):
        target=getattr(self,'bl2_base',BL2)
        allowed=((0x2f0000,0x300000),(ENTRY,ENTRY+len(self.stage)),
                 (0x230f44,0x230f50),(target+0x400,target+0x800),(target+0x7850,target+0x7854),
                 (RVBAR,RVBAR+4))
        assert any(a<=address and address+size<=b for a,b in allowed),hex(address)
        if address==RVBAR:self.rvbar_writes.append(value)

    def enter_from_stock_frames(self,case='valid'):
        # The original listener and parser prologues create both real frames.
        self.cpu.emu_start(0x224cd5,0,count=4)
        assert self.cpu.reg_read(UC_ARM_REG_SP)==0x2fe000-0x58
        self.cpu.emu_start(0x224e59,0,count=5)
        parser_sp=self.cpu.reg_read(UC_ARM_REG_SP)
        self.parser_sp=parser_sp
        self.resume_case=case
        assert parser_sp==0x2fe000-0x58-0x178
        assert word(self.cpu.mem_read(parser_sp+0x174,4))==0x224e5d
        self.cpu.reg_write(UC_ARM_REG_R0,ENTRY)
        self.cpu.reg_write(UC_ARM_REG_R1,0x401ffff4)
        self.cpu.reg_write(UC_ARM_REG_R2,12)
        self.cpu.reg_write(UC_ARM_REG_LR,0x2091cb)
        if case=='entry_cache_enabled':self.entry_sctlr_fixture=0x1004
        elif case=='entry_MMU_enabled':self.entry_sctlr_fixture=1
        elif case=='argument':self.cpu.reg_write(UC_ARM_REG_R1,0x401ffff0)
        elif case=='argument_size':self.cpu.reg_write(UC_ARM_REG_R2,16)
        elif case=='loop_caller':self.cpu.mem_write(parser_sp+0x174,struct.pack('<I',0x224e61))
        elif case=='fingerprint':
            b=bytes(self.cpu.mem_read(0x224f2c,1));self.cpu.mem_write(0x224f2c,bytes([b[0]^1]))
        elif case=='port':self.cpu.mem_write(0x2f35a8,struct.pack('<I',0))
        elif case=='table':self.cpu.mem_write(0x41000008,struct.pack('<I',0x10000000))
        elif case=='callback':self.cpu.mem_write(0x41000020,struct.pack('<I',0x10000001))
        elif case=='read_callback':self.cpu.mem_write(0x41000024,struct.pack('<I',0x10000001))
        before=bytes(self.cpu.mem_read(0x230f44,12))
        self.original_rvbar=before
        self.stop_at=None if case in ('valid','read_count8') else 0x2091ca
        self.cpu.emu_start(0x227991,0,count=100000)
        assert self.returns==1
        if case in ('valid','read_count8'):
            assert self.shutdowns==1 and len(self.records)==2
            mutation=struct.unpack('<8s8I',self.records[1])
            assert mutation[:2]==(b'T70ARM32',1) and mutation[6]==1 and mutation[8]==1
            record=struct.unpack('<8s8I',self.records[0][:40])
            assert record[:3]==(b'T70STG32',3,ENTRY)
            assert record[6:]==(0x401ffff4,12,0x2091cb)
            assert self.cpu.reg_read(UC_ARM_REG_SP)==0x2fe000
            assert self.cpu.reg_read(UC_ARM_REG_R0)==0
            assert all(self.cpu.reg_read(reg)==value for reg,value in self.original_regs.items()
                       if reg!=UC_ARM_REG_R12)
            stub=bytes(self.cpu.mem_read(0x230f44,12))
            assert stub[:8]==bytes.fromhex('014b184700bf00bf')
        else:
            assert len(self.records)==int(case in ('bad_resume','read_error')) and not self.shutdowns
            assert bytes(self.cpu.mem_read(0x230f44,12))==before
            assert self.cpu.reg_read(UC_ARM_REG_SP)==parser_sp
            assert self.cpu.reg_read(UC_ARM_REG_R5)==parser_sp+0x10
        return {'case':case,'original_frame_unwind_and_USB_shutdown':case in ('valid','read_count8'),
                'guard_failure_keeps_original_command_loop':case not in ('valid','read_count8'),
                'original_code_and_saved_LR_unchanged_before_host_resume':True}

    def loaded_bl2(self,preloader,bl2,case,bl2_base=BL2):
        self.bl2_base=bl2_base
        if bl2_base!=BL2:self.cpu.mem_map(bl2_base,0x100000)
        self.cpu.mem_write(bl2_base,bl2)
        arg=bl2_base
        if case=='base_below_DRAM':arg=0x30000000
        elif case=='base_above_DRAM':arg=0xffff0000
        elif case=='cache_enabled':self.entry_sctlr_fixture=0x1004
        elif case=='MMU_enabled':self.entry_sctlr_fixture=1
        elif case=='entry':arg+=4
        elif case=='prefix':self.cpu.mem_write(bl2_base,b'bad!')
        elif case=='padding':self.cpu.mem_write(bl2_base+0x500,b'used')
        elif case=='load_site':self.cpu.mem_write(bl2_base+0x7850,b'bad!')
        before=bytes(self.cpu.mem_read(bl2_base,len(bl2)))
        self.stop_at=None
        self.returns=0
        self.cpu.reg_write(UC_ARM_REG_CPSR,0x13)
        self.cpu.reg_write(UC_ARM_REG_SP,0x2fe000)
        self.cpu.reg_write(UC_ARM_REG_R0,arg)
        self.cpu.reg_write(UC_ARM_REG_LR,STOP|1)
        self.cpu.emu_start(0x230f45,0,count=100000)
        assert self.returns==1 and self.rvbar_writes==[arg]
        assert self.cpu.reg_read(UC_ARM_REG_SP)==0x2fe000
        restored=bytes(self.cpu.mem_read(0x230f44,12))==preloader[0x230f44-PRE:0x230f50-PRE]
        assert restored==(case!='MMU_enabled')
        assert bool(self.data_cleans)==(case!='MMU_enabled')
        assert self.instruction_invalidates==(0 if case=='MMU_enabled' else 1)
        result=bytes(self.cpu.mem_read(bl2_base,len(bl2)))
        accepted=case in ('valid','cache_enabled')
        if not accepted:assert result==before
        return result,{'case':case,'loaded_BL2_physical_base':hex(bl2_base),'original_preloader_code_restored':restored,
                       'original_RVBAR_value_preserved':True,'BL2_patched':accepted,
                       'data_cache_clean_executed':bool(self.data_cleans),
                       'instruction_cache_invalidated':bool(self.instruction_invalidates)}


def bl2_consumer(bl2,lk,plan,lk_plan,case,lk_base=LK):
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for base,size in ((BL2_VA,0x100000),(lk_base,0x200000),(STACK,0x10000),(STOP,0x1000)):
        uc.mem_map(base,size)
    uc.mem_write(BL2_VA,bl2);uc.mem_write(lk_base,lk)
    sp=STACK+0x8000
    uc.reg_write(UC_ARM64_REG_SP,sp);uc.reg_write(UC_ARM64_REG_LR,STOP)
    pointer=lk_base+4096 if case=='address' else lk_base
    uc.mem_write(sp+8,struct.pack('<Q',pointer))
    if case=='below_kernel_mapping':pointer=0xfffefffffff00000
    elif case=='above_kernel_mapping':pointer=0xfffffffffff10000
    elif case=='unaligned':pointer=lk_base+4
    uc.mem_write(sp+8,struct.pack('<Q',pointer))
    if case=='prefix':uc.mem_write(lk_base,b'bad!')
    elif case=='padding':uc.mem_write(lk_base+0x500,b'used')
    elif case=='registration':uc.mem_write(lk_base+0x8358,b'bad!')
    elif case=='source':uc.mem_write(lk_base+0x5b5e4,b'bad!')
    elif case=='version':uc.mem_write(lk_base+plan['LK_version_fingerprint_offset'],b'bad!')
    before=bytes(uc.mem_read(lk_base,len(lk)))
    saved={}
    for n in range(18,30):
        reg=globals()['UC_ARM64_REG_X'+str(n)]
        saved[reg]=0x76540000+n;uc.reg_write(reg,saved[reg])
    returns=[];clock=[];cache=[]
    def code(cpu,pc,n,_):
        if pc==STOP or pc==BL2_VA+0x7854:
            returns.append(True);cpu.emu_stop()
        elif pc==BL2_VA+0x8574:
            clock.append(True);cpu.reg_write(UC_ARM64_REG_X0,123)
            cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR))
        else:
            instruction=word(bytes(cpu.mem_read(pc,4)))
            if instruction&~31 in (0xd50b7a20,0xd50b7520):cache.append(instruction&~31)
    def write(cpu,access,address,size,value,_):
        ranges=[(STACK,STACK+0x10000)]
        ranges += [(lk_base+plan['LK_version_tail_offset'],lk_base+plan['LK_version_tail_offset']+4)]
        ranges += [(lk_base+e['offset'],lk_base+e['offset']+len(bytes.fromhex(e['replacement_hex']))) for e in lk_plan['edits']]
        assert any(a<=address and address+size<=b for a,b in ranges),hex(address)
    uc.hook_add(UC_HOOK_CODE,code);uc.hook_add(UC_HOOK_MEM_WRITE,write)
    uc.emu_start(BL2_VA+0x7850,0,count=100000)
    assert returns and clock==[True]
    assert uc.reg_read(UC_ARM64_REG_X0)==123 and uc.reg_read(UC_ARM64_REG_SP)==sp
    assert all(uc.reg_read(reg)==value for reg,value in saved.items())
    after=bytes(uc.mem_read(lk_base,len(lk)))
    trace_stage=5 if case=='valid' else {'registration':1,'source':2,'padding':3}.get(case,0)
    expected=bytearray(before)
    if trace_stage:
        tail=plan['LK_version_tail_offset'];expected[tail:tail+4]=('RB0'+str(trace_stage)).encode()
    if case=='valid':
        for e in lk_plan['edits']:
            offset,data=e['offset'],bytes.fromhex(e['replacement_hex'])
            expected[offset:offset+len(data)]=data
        assert after==bytes(expected)
        assert 0xd50b7a20 in cache and 0xd50b7520 in cache
    else:
        assert after==bytes(expected)
        assert bool(cache)==bool(trace_stage)
    return {'case':case,'loaded_LK_virtual_base':hex(lk_base),'LK_patched':case=='valid','stock_clock_result_and_callee_registers_preserved':True,
            'instruction_and_data_cache_sync_executed':bool(trace_stage),
            'RAM_only_progress_version_tag':trace_stage,
            'only_diagnostic_version_changes_on_guard_mismatch':case!='valid'}


def allocation_diagnostic(preloader,stage,bl2,case,plan):
    m=Preloader(preloader,stage);cpu=m.cpu
    bootarg=0x42000000;cpu.mem_map(bootarg,0x100000)
    cpu.mem_write(0x2f35ac,struct.pack('<I',0x1000 if case=='invalid_table_pointer' else bootarg))
    descriptor=bootarg+0x7cd00+(127*0x98 if case=='last_record' else 0)
    expected_descriptor=0 if case in ('invalid_table_pointer','absent','name_prefix_only') else descriptor
    if case not in ('invalid_table_pointer','absent'):
        high=1 if case=='above_32bit' else 0
        size=16 if case=='short_allocation' else 0x100000
        cpu.mem_write(descriptor,struct.pack('<QQI',BL2|(high<<32),size,0))
        name=b'system_bl2-ext-extra' if case=='name_prefix_only' else b'system_bl2-ext'
        cpu.mem_write(descriptor+0x14,name+b'\0')
    cpu.mem_write(BL2,bl2)
    if case=='prefix_differs':cpu.mem_write(BL2,b'bad!')
    elif case=='loaded_load_site_mismatch':cpu.mem_write(BL2+0x7850,b'bad!')
    elif case=='loaded_cave_occupied':cpu.mem_write(BL2+0x500,b'used')
    table_before=bytes(cpu.mem_read(bootarg+0x7cd00,128*0x98))
    image_before=bytes(cpu.mem_read(BL2,len(bl2)))
    m.enter_from_stock_frames()
    state,low,high,size,matched,actual_descriptor=struct.unpack_from('<6I',m.records[0],40)
    expected_state={'invalid_table_pointer':0,'absent':1,'name_prefix_only':1,
                    'above_32bit':2,'short_allocation':2,'prefix_differs':3,
                    'loaded':4,'last_record':4,'loaded_load_site_mismatch':4,'loaded_cave_occupied':4}[case]
    assert state==expected_state and matched==int(state==4) and actual_descriptor==expected_descriptor
    image_expected=bytearray(image_before)
    patched=state==4 and case not in ('loaded_load_site_mismatch','loaded_cave_occupied')
    mutation=struct.unpack('<8s8I',m.records[1])
    expected_patch_state=7 if patched else 4 if case=='loaded_load_site_mismatch' else 5 if case=='loaded_cave_occupied' else 0
    assert mutation[2]==expected_patch_state and mutation[5]==int(patched)
    assert mutation[3]==(0x500 if case=='loaded_cave_occupied' else 0)
    if patched:
        for edit in plan['BL2_RAM_edits']:
            offset,data=edit['offset'],bytes.fromhex(edit['replacement_hex'])
            image_expected[offset:offset+len(data)]=data
    assert bytes(cpu.mem_read(BL2,len(bl2)))==bytes(image_expected)
    # The early direct path must not program the hardware RVBAR register.
    assert not m.rvbar_writes
    if case!='invalid_table_pointer':
        # Compare our bounded match with the complete shipping allocation lookup,
        # including its actual strlen and strncmp implementations.
        name_address=ENTRY+len(stage)+64
        cpu.mem_write(name_address,b'system_bl2-ext\0')
        cpu.reg_write(UC_ARM_REG_R0,name_address);cpu.reg_write(UC_ARM_REG_R1,0)
        cpu.reg_write(UC_ARM_REG_SP,0x2fe000);cpu.reg_write(UC_ARM_REG_LR,STOP|1)
        m.returns=0
        cpu.emu_start(0x229601,0,count=100000)
        assert m.returns==1 and cpu.reg_read(UC_ARM_REG_R0)==expected_descriptor
    assert bytes(cpu.mem_read(bootarg+0x7cd00,128*0x98))==table_before
    return {'case':case,'record_state':state,'shipping_lookup_agrees':case!='invalid_table_pointer',
            'allocation_table_unchanged':True,'already_loaded_BL2_patched':patched,
            'physical_readback_record_state':mutation[2],
            'early_direct_patch_does_not_write_RVBAR_register':True,'other_RAM_contents_emitted':False}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('preloader','backup','lk','artifacts','report'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();O=a.artifacts
    a.report.write_text(json.dumps({'all_passed':False,'status':'running','tablet_execution_observed':False})+'\n')
    plan=json.loads((O/'bootchain-ram-plan.json').read_text())
    lk_plan=json.loads((O/'ram-edit-plan.json').read_text())
    stage=(O/'preloader-ram-boot.bin').read_bytes()
    preloader,lk=a.preloader.read_bytes(),a.lk.read_bytes()
    assert digest(preloader)==plan['shipping_preloader_sha256']
    assert digest(lk)==plan['shipping_lk_sha256']
    assert digest(stage)==plan['preloader_stage_sha256']
    parts=json.loads((a.backup/'partition-layout.json').read_text())['partitions']
    offset=next(x['offset_bytes'] for x in parts if x['name']=='lk_a')
    with (a.backup/'images/ufs-lu2.img').open('rb') as f:f.seek(offset+0xc5ec0);bl2=f.read(645992)
    assert digest(bl2)==plan['shipping_bl2_ext_sha256']
    frames=[]
    for case in ('valid','argument','argument_size','loop_caller','fingerprint','port','table','callback','entry_cache_enabled','entry_MMU_enabled','read_callback','bad_resume','read_error','read_count8'):
        try:frames.append(Preloader(preloader,stage).enter_from_stock_frames(case))
        except Exception as e:raise AssertionError('Preloader frame case failed: '+case) from e
    patches=[];successful=None
    for case in ('valid','entry','prefix','padding','load_site','cache_enabled','MMU_enabled','base_below_DRAM','base_above_DRAM'):
        m=Preloader(preloader,stage);m.enter_from_stock_frames()
        patched,result=m.loaded_bl2(preloader,bl2,case);patches.append(result)
        if case=='valid':successful=patched
    expected=bytearray(bl2)
    for e in plan['BL2_RAM_edits']:
        offset,data=e['offset'],bytes.fromhex(e['replacement_hex'])
        assert bytes(expected[offset:offset+len(data)])==bytes.fromhex(e['expected_hex'])
        expected[offset:offset+len(data)]=data
    assert successful==bytes(expected)
    consumers=[bl2_consumer(successful,lk,plan,lk_plan,case)
               for case in ('valid','address','prefix','padding','registration','source','below_kernel_mapping','above_kernel_mapping','unaligned','version')]
    allocation_cases=[]
    for base in (0x40000000,0x50000000,0x80000000,0xf0000000):
        m=Preloader(preloader,stage);m.enter_from_stock_frames()
        patched,result=m.loaded_bl2(preloader,bl2,'valid',bl2_base=base)
        assert patched==bytes(expected)
        allocation_cases.append(result)
    mapped_cases=[bl2_consumer(successful,lk,plan,lk_plan,'valid',lk_base=base)
                  for base in (0xffff000048000000,0xffff000062000000,0xffff000080000000,0xffff0000f0000000,
                               0xffff000000100000,0xffff000100000000,0xffff800000100000,0xffffffff80000000)]
    diagnostic_cases=[allocation_diagnostic(preloader,stage,bl2,case,plan) for case in
                      ('invalid_table_pointer','absent','name_prefix_only','above_32bit','short_allocation','prefix_differs','loaded','last_record','loaded_load_site_mismatch','loaded_cave_occupied')]
    report={'scope':'Compiled ARM32/ARM64 bridges, original preloader listener/parser frame setup, original DA wrapper and jump tail, original USB shutdown epilogue and original BL2 post-load instruction. USB, console, timers, memory mappings and non-default SCTLR reads are fixtures; no tablet execution.',
            'preloader_stage_sha256':digest(stage),'BL2_bridge_sha256':plan['BL2_bridge_sha256'],
            'LK_payload_sha256':plan['LK_payload_sha256'],
            'original_preloader_frame_cases':frames,'loaded_BL2_cases':patches,'loaded_LK_cases':consumers,
            'shipping_allocation_BL2_base_cases':allocation_cases,'shipping_allocation_LK_base_cases':mapped_cases,
            'preloader_allocation_diagnostic_cases':diagnostic_cases,
            'all_passed':True,'test_source_sha256':digest(Path(__file__).read_bytes()),'storage_writes':[],
            'hardware_MMU_cache_and_bootchain_retention_verified':False,
            'tablet_execution_observed':False,'UEFI_execution_observed':False}
    a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'all_passed':True,'original_frame_cases':len(frames),'BL2_cases':len(patches),
                      'LK_cases':len(consumers),'additional_allocation_base_cases':len(allocation_cases)+len(mapped_cases),'allocation_diagnostic_cases':len(diagnostic_cases),'tablet_execution_observed':False}))


if __name__=='__main__':
    main()
