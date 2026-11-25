#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the audit in shipping parser frames; prohibit all live-code writes."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode=True
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *

HERE=Path(__file__).resolve().parent

def load(name,filename):
    spec=importlib.util.spec_from_file_location(name,HERE/filename)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module

chain=load('audit_chain','RamBootChainTest.py')
probe=load('audit_probe','PreloaderRamProbeTest.py')
WINDOWS=((0x201170,3),(0x209178,21),(0x208e44,5),(0x2093d0,3),
         (0x224cd4,3),(0x224e58,4),(0x224f2c,5),(0x230f44,3),(0x236904,4))

class Audit(chain.Preloader):
    def __init__(self,preloader,payload):
        self.mode_fixture=None
        super().__init__(preloader,payload)
        self.entry_sctlr_fixture=0x00c50838

    def code(self,cpu,pc,n,_):
        if pc==chain.ENTRY:
            self.entry_registers={r:cpu.reg_read(r) for r in range(UC_ARM_REG_R4,UC_ARM_REG_R12+1)}
        if chain.ENTRY<=pc<chain.ENTRY+len(self.stage):
            instruction=chain.word(bytes(cpu.mem_read(pc,4)))
            if instruction==0xe10f0000 and self.mode_fixture is not None:
                cpu.reg_write(UC_ARM_REG_R0,self.mode_fixture)
                cpu.reg_write(UC_ARM_REG_PC,pc+4)
                return
        if pc==0x240000:
            address,size=cpu.reg_read(UC_ARM_REG_R0),cpu.reg_read(UC_ARM_REG_R1)
            assert size==80 and chain.ENTRY<=address<=chain.ENTRY+len(self.stage)-80
            self.records.append(bytes(cpu.mem_read(address,size)));self.ret(size)
            return
        super().code(cpu,pc,n,_)

    def write(self,cpu,access,address,size,value,_):
        # The parser/listener prologues and our own saved registers are the only
        # stack writes. No original code, saved caller LR or MMIO may be changed.
        allowed=((0x2fdc00,0x2fe000),(chain.ENTRY,chain.ENTRY+len(self.stage)))
        assert any(a<=address and address+size<=b for a,b in allowed),hex(address)


def exercise(preloader,payload,case):
    a=Audit(preloader,payload);cpu=a.cpu
    cpu.emu_start(0x224cd5,0,count=4)
    cpu.emu_start(0x224e59,0,count=5)
    sp=cpu.reg_read(UC_ARM_REG_SP)
    assert sp==0x2fe000-0x58-0x178
    assert chain.word(cpu.mem_read(sp+0x174,4))==0x224e5d
    for reg,value in ((UC_ARM_REG_R0,chain.ENTRY),(UC_ARM_REG_R1,0x401ffff4),
                      (UC_ARM_REG_R2,12),(UC_ARM_REG_R4,0x401ffff4),
                      (UC_ARM_REG_R5,12),(UC_ARM_REG_LR,0x2091cb)):
        cpu.reg_write(reg,value)
    expected_failure=0;record_expected=True;bad_window=None
    if case=='MMU':a.entry_sctlr_fixture|=1;expected_failure=1
    elif case=='D_cache':a.entry_sctlr_fixture|=4;expected_failure=1
    elif case=='I_cache':a.entry_sctlr_fixture|=0x1000;expected_failure=1
    elif case=='CPU_mode':a.mode_fixture=0x1f;expected_failure=2
    elif case=='caller_LR':
        cpu.mem_write(sp+0x174,struct.pack('<I',0x224e61));expected_failure=4
    elif case.startswith('bridge_window_'):
        bad_window=int(case.rsplit('_',1)[1]);address=WINDOWS[bad_window][0]
        value=bytes(cpu.mem_read(address,1))[0];cpu.mem_write(address,bytes([value^1]))
        expected_failure=3;record_expected=bad_window not in (0,1)
    elif case.startswith('trusted_window_'):
        address=probe.WINDOWS[int(case.rsplit('_',1)[1])][0]
        value=bytes(cpu.mem_read(address,1))[0];cpu.mem_write(address,bytes([value^1]))
        record_expected=False
    elif case=='port':cpu.mem_write(0x2f35a8,struct.pack('<I',0));record_expected=False
    elif case=='table':cpu.mem_write(0x41000008,struct.pack('<I',0x10000000));record_expected=False
    elif case=='callback':cpu.mem_write(0x41000020,struct.pack('<I',0x10000001));record_expected=False
    elif case=='argument':cpu.reg_write(UC_ARM_REG_R4,0x401ffff0);record_expected=False
    elif case=='argument_size':cpu.reg_write(UC_ARM_REG_R5,16);record_expected=False
    elif case=='entry_LR':cpu.reg_write(UC_ARM_REG_LR,0x2091cf);record_expected=False
    elif case!='valid':raise AssertionError(case)
    original_code=bytes(cpu.mem_read(chain.PRE,len(preloader)))
    original_stack=bytes(cpu.mem_read(sp,0x2fe000-sp))
    a.stop_at=cpu.reg_read(UC_ARM_REG_LR)&~1
    # Positive cases enter through the real shipping DA jump wrapper. Deliberate
    # code/argument damage enters directly so it cannot corrupt the jump itself.
    cpu.emu_start(0x227991 if case=='valid' else chain.ENTRY,0,count=100000)
    assert a.returns==1 and not a.shutdowns and not a.rvbar_writes,case
    assert len(a.records)==int(record_expected),case
    assert bytes(cpu.mem_read(chain.PRE,len(preloader)))==original_code,case
    assert bytes(cpu.mem_read(sp,0x2fe000-sp))==original_stack,case
    assert cpu.reg_read(UC_ARM_REG_SP)==sp and cpu.reg_read(UC_ARM_REG_R5)==sp+0x10
    assert all(cpu.reg_read(r)==v for r,v in a.entry_registers.items() if r!=UC_ARM_REG_R5), case
    if record_expected:
        r=struct.unpack('<8s18I',a.records[0])
        assert r[:3]==(b'T70RAM32',3,chain.ENTRY) and r[6:9]==(0x401ffff4,12,0x2091cb)
        assert r[9]==expected_failure and r[13]==sp and r[17:]==(0,0), (case,r)
        assert r[14]==(0x224e61 if case=='caller_LR' else 0x224e5d)
        assert r[15]==(0x1ff & ~(1<<bad_window) if bad_window is not None else 0x1ff)
        assert r[16]==sum(n for _,n in WINDOWS) and r[17]==0
        if expected_failure==3:
            address=WINDOWS[bad_window][0]
            assert r[10]==address and r[11]==chain.word(preloader,address-chain.PRE)
            assert r[12]==r[11]^1
        if expected_failure==4:assert r[10:13]==(sp+0x174,0x224e5d,0x224e61)
    return {'case':case,'diagnostic_record_emitted':record_expected,
            'first_failed_guard':expected_failure if record_expected else None,
            'shipping_code_and_saved_caller_frame_unchanged':True,
            'parser_stack_buffer_restored':True}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('preloader','payload','report'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.report.write_text(json.dumps({'all_passed':False,'status':'running'})+'\n')
    preloader,payload=a.preloader.read_bytes(),a.payload.read_bytes()
    assert hashlib.sha256(preloader).hexdigest()=='cc42d1aec1f3dd9a94cfd9147ab5c1f55e592d1aa17ebbe7a9d4c1a49e9d436e'
    assert 80<len(payload)<=4096 and len(payload)%4==0
    cases=['valid','MMU','D_cache','I_cache','CPU_mode','caller_LR']
    cases+=['bridge_window_'+str(i) for i in range(len(WINDOWS))]
    cases+=['trusted_window_'+str(i) for i in range(len(probe.WINDOWS))]
    cases+=['port','table','callback','argument','argument_size','entry_LR']
    results=[exercise(preloader,payload,c) for c in cases]
    continuation=probe.following_send_da(preloader,payload,record_size=80)
    package=HERE.parent
    sources=['Tools/PreloaderRamProbe.S','Tests/PreloaderGuardAuditTest.py',
             'Tests/PreloaderRamProbeTest.py','Tests/RamBootChainTest.py']
    doc={'all_passed':True,'probe_version':3,'record_bytes':80,'guard_cases':results,
         'payload_bytes':len(payload),'payload_sha256':hashlib.sha256(payload).hexdigest(),
         'preloader_sha256':hashlib.sha256(preloader).hexdigest(),
         'source_sha256':{n:hashlib.sha256((package/n).read_bytes()).hexdigest() for n in sources},
         'shipping_code_and_saved_caller_frame_unchanged':True,
         'following_shipping_SEND_DA_header_verified':continuation,
         'scope':'Actual compiled audit and shipping parser/listener frames. USB callbacks and CPU system registers are fixtures.',
         'tablet_execution_observed':False,'UEFI_execution_observed':False}
    a.report.write_text(json.dumps(doc,indent=2)+'\n')
    print(json.dumps({'all_passed':True,'guard_cases':len(results),'payload_bytes':len(payload)}))

if __name__=='__main__':main()
