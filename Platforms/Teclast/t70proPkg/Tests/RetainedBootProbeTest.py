#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute bounded retained-record reads and the real stock frame unwind."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode=True
from unicorn import UC_HOOK_MEM_READ
from unicorn.arm_const import *
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('retained_chain',HERE/'RamBootChainTest.py')
chain=importlib.util.module_from_spec(spec);spec.loader.exec_module(chain)
TRIAL,HANDOFF=0x43f7ffe0,0x43f7f000
MAGIC,INVERSE=0x0000000157443754,0xfffffffea8bbc8ab

class Probe(chain.Preloader):
    def __init__(self,preloader,payload,case):
        super().__init__(preloader,payload)
        self.case=case;self.entry_sctlr_fixture=0x00c50838
        self.cpu.mem_map(HANDOFF,4096)
        self.cpu.mem_write(HANDOFF,bytes([0xa5])*4096)
        self.retained_reads=[];self.cookie_writes=[];self.handoff_writes=[];self.gop_writes=[];self.snapshot_writes=[]
        self.cpu.hook_add(UC_HOOK_MEM_READ,self.read)

    def read(self,cpu,access,address,size,value,_):
        if HANDOFF<=address<HANDOFF+4096:
            assert any(a<=address and address+size<=b for a,b in
                       ((TRIAL,TRIAL+32),(HANDOFF,HANDOFF+128),(HANDOFF+0x800,HANDOFF+0x848)))
            self.retained_reads.append((address,size))

    def code(self,cpu,pc,n,_):
        if pc==0x240000:
            address,size=cpu.reg_read(UC_ARM_REG_R0),cpu.reg_read(UC_ARM_REG_R1)
            assert size==296 and chain.ENTRY<=address<=chain.ENTRY+len(self.stage)-296
            self.records.append(bytes(cpu.mem_read(address,size)));self.ret(size);return
        if pc==0x240004:
            address,size=cpu.reg_read(UC_ARM_REG_R0),cpu.reg_read(UC_ARM_REG_R1)
            assert size==8 and cpu.reg_read(UC_ARM_REG_R2)==0
            assert len(self.records)==1
            cpu.mem_write(address,b'T70NOPE!' if self.case=='receipt' else b'T70RUN!!')
            self.ret(0);return
        super().code(cpu,pc,n,_)

    def write(self,cpu,access,address,size,value,_):
        if TRIAL<=address<TRIAL+16:
            assert self.expected_trial and size==4 and value==0
            self.cookie_writes.append((address,size));return
        if HANDOFF<=address<HANDOFF+8:
            assert self.expected_handoff and size==4 and value==0
            self.handoff_writes.append((address,size));return
        if HANDOFF+0x60<=address<HANDOFF+0x68:
            assert self.expected_gop and size==4 and value==0
            self.gop_writes.append((address,size));return
        if HANDOFF+0x800<=address<HANDOFF+0x808:
            assert self.expected_snapshot and size==4 and value==0
            self.snapshot_writes.append((address,size));return
        allowed=((0x2fdc00,0x2fe000),(chain.ENTRY,chain.ENTRY+len(self.stage)))
        assert any(a<=address and address+size<=b for a,b in allowed),hex(address)


def exercise(preloader,payload,case):
    m=Probe(preloader,payload,case);cpu=m.cpu
    trial=[MAGIC,INVERSE,0x0000f00822000005,2]
    handoff=[0x0000000148303754,0x47c80000,0x1000,0x40000000,4,0x81000000,0x30d00800,4,0x42000000,0x43000000,4096,0]
    gop=[0x0000000147503754,2,1,0x800000000000000e,0]
    snapshot=[0x000000014c443754,0x14005000,1]+list(range(14))
    snapshot[6]=2
    expected_trial=expected_handoff=expected_gop=expected_snapshot=True
    if case.startswith('trial_word_'):
        index=int(case.rsplit('_',1)[1]);raw=bytearray(struct.pack('<4Q',*trial));raw[index*4]^=1
        trial=list(struct.unpack('<4Q',raw));expected_trial=False
    elif case in ('trial_phase_zero','trial_phase_high','trial_phase_large'):
        trial[3]={'trial_phase_zero':0,'trial_phase_high':0x100000001,'trial_phase_large':4}[case]
        expected_trial=False
    elif case.startswith('handoff_'):
        index,value={'handoff_magic':(0,0),'handoff_head':(3,0x40001000),
                     'handoff_phase':(7,5),'handoff_phase_zero':(7,0),
                     'handoff_phase_high':(7,0x100000003)}[case]
        handoff[index]=value;expected_handoff=False
    elif case.startswith('gop_'):
        index,value={'gop_magic':(0,0),'gop_phase_zero':(1,0),'gop_phase_large':(1,9),'gop_attempts':(2,0)}[case]
        gop[index]=value;expected_gop=False
    elif case.startswith('trace_bds7_'):
        snapshot=[0x000000074c443754,0x43f7f060,0,10,3,1,1,1,0,2,1,1,1,1,4,7,0]
        gop[1]=8;gop[3]=0
        if case=='trace_bds7_phase':snapshot[15]=9
        elif case=='trace_bds7_status':snapshot[16]=0x20000
        elif case=='trace_bds7_base':snapshot[1]+=4
        elif case=='trace_bds7_layer':snapshot[2]=1
        elif case=='trace_bds7_stage':snapshot[3]=12
        elif case=='trace_bds7_gop':gop[1]=7
        elif case=='trace_bds7_error':snapshot[15]=4;snapshot[16]=0x10001
        elif case!='trace_bds7_valid':raise AssertionError(case)
        expected_snapshot=case in ('trace_bds7_valid','trace_bds7_error')
    elif case.startswith('trace_bds_'):
        snapshot=[0x000000064c443754,0x43f7f060,0,7,3,1,1,1,0,2,1,1,1,1,2,0,0]
        gop[1]=8;gop[3]=0
        if case=='trace_bds_base':snapshot[1]+=4
        elif case=='trace_bds_layer':snapshot[2]=1
        elif case=='trace_bds_stage_zero':snapshot[3]=0
        elif case=='trace_bds_stage_high':snapshot[3]=12
        elif case=='trace_bds_tail0':snapshot[15]=1
        elif case=='trace_bds_tail1':snapshot[16]=1
        elif case=='trace_bds_gop_phase':gop[1]=7
        elif case=='trace_bds_gop_error':gop[3]=1
        elif case!='trace_bds_valid':raise AssertionError(case)
        expected_snapshot=case=='trace_bds_valid'
    elif case.startswith('trace_fault_'):
        snapshot=[0x000000054c443754,0x43f7f060,0,4,0,0x96000021,0,0x44001234,0,
                  0x47c81234,0,0x30d00800,0,0x3c5,0,0,0]
        gop[0]=0;expected_gop=False;handoff[7]=3
        if case=='trace_fault_base':snapshot[1]+=4
        elif case=='trace_fault_layer':snapshot[2]=1
        elif case=='trace_fault_EL':snapshot[3]=12
        elif case=='trace_fault_type':snapshot[4]=4
        elif case=='trace_fault_tail':snapshot[16]=1
        elif case=='trace_fault_pre_C':handoff[7]=2
        elif case=='trace_fault_EL2':snapshot[3]=8
        elif case!='trace_fault_valid':raise AssertionError(case)
        expected_snapshot=case in ('trace_fault_valid','trace_fault_EL2')
    elif case.startswith('trace_sec_'):
        snapshot=[0x000000044c443754,0x43f7f060,0,8,0,6,0x80000000,14,0x80000000,
                  0x14005044,0x10002300,3]+[0]*5
        gop[0]=0;expected_gop=False;handoff[7]=3
        if case=='trace_sec_base':snapshot[1]+=4
        elif case=='trace_sec_layer':snapshot[2]=1
        elif case=='trace_sec_phase':snapshot[3]=12
        elif case=='trace_sec_count':snapshot[4]=128
        elif case=='trace_sec_operation':snapshot[11]=5
        elif case=='trace_sec_tail':snapshot[12]=1
        elif case=='trace_sec_pre_C':handoff[7]=2
        elif case!='trace_sec_valid':raise AssertionError(case)
        expected_snapshot=case=='trace_sec_valid'
    elif case.startswith('trace_overlap_'):
        snapshot=[0x000000034c443754,0x43f7f080,0]+list(range(10))+[0]*4
        if case=='trace_overlap_base':snapshot[1]+=8
        elif case=='trace_overlap_layer':snapshot[2]=1
        elif case=='trace_overlap_tail':snapshot[13]=1
        elif case!='trace_overlap_valid':raise AssertionError(case)
        expected_snapshot=case=='trace_overlap_valid'
    elif case.startswith('trace_dsi_'):
        snapshot=[0x000000024c443754,0x14013000,0]+list(range(12))+[0,0]
        if case=='trace_dsi_base':snapshot[1]=0x14013004
        elif case=='trace_dsi_layer':snapshot[2]=1
        elif case=='trace_dsi_tail0':snapshot[15]=1
        elif case=='trace_dsi_tail1':snapshot[16]=1
        elif case=='trace_dsi_version':snapshot[0]=0x000000044c443754
        elif case!='trace_dsi_valid':raise AssertionError(case)
        expected_snapshot=case=='trace_dsi_valid'
    elif case.startswith('trace_'):
        if case=='trace_magic':snapshot[0]=0
        elif case=='trace_base':snapshot[1]=0x14004000
        elif case=='trace_layer':snapshot[2]=4
        elif case=='trace_2l_layer':snapshot[1:3]=[0x14006000,2]
        elif case=='trace_source':snapshot[6]=8
        elif case=='trace_2l_valid':snapshot[1]=0x14006000
        else:raise AssertionError(case)
        expected_snapshot=case=='trace_2l_valid'
    elif case=='pre_DXE':handoff[7]=3
    elif case not in ('valid','receipt','MMU','cache','caller','fingerprint','port','read_callback'):
        raise AssertionError(case)
    expected_snapshot = expected_snapshot and expected_handoff and (expected_gop or case.startswith(('trace_sec_','trace_fault_')))
    m.expected_trial=expected_trial;m.expected_handoff=expected_handoff;m.expected_gop=expected_handoff and expected_gop
    m.expected_snapshot=expected_snapshot
    cpu.mem_write(TRIAL,struct.pack('<4Q',*trial));cpu.mem_write(HANDOFF,struct.pack('<12Q',*handoff))
    cpu.mem_write(HANDOFF+0x60,struct.pack('<QIIQQ',*gop))
    cpu.mem_write(HANDOFF+0x800,struct.pack('<Q16I',*snapshot))
    before_retained=bytes(cpu.mem_read(HANDOFF,4096))
    cpu.emu_start(0x224cd5,0,count=4);cpu.emu_start(0x224e59,0,count=5)
    sp=cpu.reg_read(UC_ARM_REG_SP);m.parser_sp=sp
    for reg,value in ((UC_ARM_REG_R0,chain.ENTRY),(UC_ARM_REG_R1,0x401ffff4),
                      (UC_ARM_REG_R2,12),(UC_ARM_REG_LR,0x2091cb)):
        cpu.reg_write(reg,value)
    rejected=case in ('MMU','cache','caller','fingerprint','port','read_callback')
    if case=='MMU':m.entry_sctlr_fixture|=1
    if case=='cache':m.entry_sctlr_fixture|=4
    if case=='caller':cpu.mem_write(sp+0x174,struct.pack('<I',0x224e61))
    if case=='fingerprint':cpu.mem_write(0x224f2c,b'bad!')
    if case=='port':cpu.mem_write(0x2f35a8,struct.pack('<I',0))
    if case=='read_callback':cpu.mem_write(0x41000024,struct.pack('<I',0x10000001))
    before_code=bytes(cpu.mem_read(chain.PRE,len(preloader)))
    before_stack=bytes(cpu.mem_read(sp,0x2fe000-sp))
    m.stop_at=0x2091ca if rejected or case=='receipt' else None
    cpu.emu_start(0x227991,0,count=100000)
    assert m.returns==1 and len(m.records)==int(not rejected)
    assert bytes(cpu.mem_read(chain.PRE,len(preloader)))==before_code
    assert not m.rvbar_writes and not m.data_cleans and not m.instruction_invalidates
    after_retained=bytearray(cpu.mem_read(HANDOFF,4096))
    if not rejected and expected_trial:
        assert m.cookie_writes==[(TRIAL+i,4) for i in range(0,16,4)]
        assert after_retained[0xfe0:0xff0]==bytes(16)
        after_retained[0xfe0:0xff0]=before_retained[0xfe0:0xff0]
    else:assert not m.cookie_writes
    if not rejected and expected_handoff:
        assert m.handoff_writes==[(HANDOFF,4),(HANDOFF+4,4)]
        assert after_retained[:8]==bytes(8)
        after_retained[:8]=before_retained[:8]
    else:assert not m.handoff_writes
    if not rejected and expected_handoff and expected_gop:
        assert m.gop_writes==[(HANDOFF+0x60,4),(HANDOFF+0x64,4)]
        assert after_retained[0x60:0x68]==bytes(8)
        after_retained[0x60:0x68]=before_retained[0x60:0x68]
    else:assert not m.gop_writes
    if not rejected and expected_snapshot:
        assert m.snapshot_writes==[(HANDOFF+0x800,4),(HANDOFF+0x804,4)]
        assert after_retained[0x800:0x808]==bytes(8)
        after_retained[0x800:0x808]=before_retained[0x800:0x808]
    else:assert not m.snapshot_writes
    assert bytes(after_retained)==before_retained
    if rejected:
        assert not m.retained_reads and not m.shutdowns
    else:
        record=m.records[0]
        assert struct.unpack_from('<8sII',record)==(b'T70DIA32',3,chain.ENTRY)
        assert struct.unpack_from('<II',record,40)==(int(expected_trial),int(expected_handoff))
        assert record[48:80]==(struct.pack('<4Q',*trial) if expected_trial else bytes(32))
        assert record[80:176]==((struct.pack('<12Q',*handoff) if handoff[7]==4 else struct.pack('<8Q',*handoff[:8])+bytes(32)) if expected_handoff else bytes(96))
        assert record[176:208]==(struct.pack('<QIIQQ',*gop) if expected_handoff and expected_gop else bytes(32))
        assert struct.unpack_from('<4I',record,208)==(int(expected_trial),int(expected_handoff),int(expected_handoff and expected_gop),int(expected_snapshot))
        assert record[224:296]==(struct.pack('<Q16I',*snapshot) if expected_snapshot else bytes(72))
        assert m.shutdowns==int(case!='receipt')
    if rejected or case=='receipt':
        assert bytes(cpu.mem_read(sp,0x2fe000-sp))==before_stack
        assert cpu.reg_read(UC_ARM_REG_SP)==sp and cpu.reg_read(UC_ARM_REG_R5)==sp+0x10
    else:
        assert cpu.reg_read(UC_ARM_REG_SP)==0x2fe000
        assert all(cpu.reg_read(r)==v for r,v in m.original_regs.items() if r!=UC_ARM_REG_R12)
    return {'case':case,'record_emitted':not rejected,'trial_cookie_exported':not rejected and expected_trial,
            'handoff_exported':not rejected and expected_handoff,'original_code_and_MMIO_unchanged':True,
            'stock_frame_unwind':not rejected and case!='receipt',
            'retained_writes':'only recognized trial magic/inverse cleared' if m.cookie_writes else 'none'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('preloader','payload','report'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();a.report.write_text(json.dumps({'all_passed':False,'status':'running'})+'\n')
    preloader,payload=a.preloader.read_bytes(),a.payload.read_bytes()
    cases=['valid','pre_DXE','receipt','MMU','cache','caller','fingerprint','port','read_callback']
    cases+=['trace_magic','trace_base','trace_layer','trace_2l_layer','trace_source','trace_2l_valid']
    cases+=['trace_bds7_valid','trace_bds7_error','trace_bds7_phase','trace_bds7_status',
            'trace_bds7_base','trace_bds7_layer','trace_bds7_stage','trace_bds7_gop']
    cases+=['trace_bds_valid','trace_bds_base','trace_bds_layer','trace_bds_stage_zero',
            'trace_bds_stage_high','trace_bds_tail0','trace_bds_tail1','trace_bds_gop_phase','trace_bds_gop_error']
    cases+=['trace_overlap_valid','trace_overlap_base','trace_overlap_layer','trace_overlap_tail']
    cases+=['trace_fault_valid','trace_fault_EL2','trace_fault_base','trace_fault_layer',
            'trace_fault_EL','trace_fault_type','trace_fault_tail','trace_fault_pre_C']
    cases+=['trace_sec_valid','trace_sec_base','trace_sec_layer','trace_sec_phase',
            'trace_sec_count','trace_sec_operation','trace_sec_tail','trace_sec_pre_C']
    cases+=['trace_dsi_valid','trace_dsi_base','trace_dsi_layer','trace_dsi_tail0','trace_dsi_tail1','trace_dsi_version']
    cases+=['gop_magic','gop_phase_zero','gop_phase_large','gop_attempts']
    cases+=['trial_word_'+str(i) for i in range(4)]
    cases+=['trial_phase_zero','trial_phase_high','trial_phase_large','handoff_magic','handoff_head',
            'handoff_phase','handoff_phase_zero','handoff_phase_high']
    results=[exercise(preloader,payload,c) for c in cases]
    package=HERE.parent
    doc={'all_passed':True,'payload_sha256':chain.digest(payload),'payload_bytes':len(payload),
         'preloader_sha256':chain.digest(preloader),'record_bytes':296,'cases':results,
         'source_sha256':{n:chain.digest((package/n).read_bytes()) for n in
                          ('Tools/PreloaderRamBoot.S','Tests/RetainedBootProbeTest.py','Tests/RamBootChainTest.py')},
         'scope':'Compiled ARM32 probe and actual original parser/listener frames; USB callbacks and retained records are fixtures.',
         'storage_writes':[],'tablet_execution_observed':False}
    a.report.write_text(json.dumps(doc,indent=2)+'\n')
    print(json.dumps({'all_passed':True,'cases':len(results),'payload_bytes':len(payload)}))
if __name__=='__main__':main()
