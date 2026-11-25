#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute compiled BootShim and packaged SEC watchdog paths with MMIO fixtures."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
import capstone
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ, UC_ERR_EXCEPTION
from unicorn.arm64_const import *

BASE, FD, HANDOFF, DTB, WDT = 0x40000000, 0x44000000, 0x43F7F000, 0x47C80000, 0x10007000
RECORD, STACK, STOP = HANDOFF+0xFE0, 0x70000000, 0x7FFF0000
MAGIC, INVERSE, HANDSHAKE = 0x0000000157443754, 0xFFFFFFFEA8BBC8AB, 0x0000000148303754


def sha(data):return hashlib.sha256(data).hexdigest()


def machine(mode=0x8D, length=0xF800, fault=None):
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for start,size in ((BASE,0x8080000),(WDT,0x1000),(STACK,0x10000),(STOP,0x1000)):
        cpu.mem_map(start,size)
    cpu.reg_write(UC_ARM64_REG_PSTATE,0x3C5)
    cpu.reg_write(UC_ARM64_REG_SP,STACK+0x8000)
    cpu.reg_write(UC_ARM64_REG_LR,STOP)
    state={'mode':mode,'length':length,'reloads':0,'writes':[]}
    def read(uc,access,at,n,value,_):
        assert n==4 and at in (WDT,WDT+4),(hex(at),n)
        uc.mem_write(at,struct.pack('<I',state['mode' if at==WDT else 'length']))
    def write(uc,access,at,n,value,_):
        assert n==4 and at in (WDT,WDT+4,WDT+8),(hex(at),n)
        state['writes'].append((at,n,value))
        if at==WDT:
            assert value&0xFF000000==0x22000000
            state['mode']=value&0xFFFFFF
            if fault=='mode':state['mode']|=1
        elif at==WDT+4:
            assert value&0x1F==8
            if fault!='length':state['length']=value&0xFFE0
        else:
            assert value==0x1971
            state['reloads']+=1
    cpu.hook_add(UC_HOOK_MEM_READ,read,begin=WDT,end=WDT+0xFFF)
    cpu.hook_add(UC_HOOK_MEM_WRITE,write,begin=WDT,end=WDT+0xFFF)
    return cpu,state


def shim_case(image,fd,name,*,trial=True,el=1,pointer=DTB,magic=0xD00DFEED,
              size=40,placement=BASE,sctlr=0,fault=None,accepted=False,armed=True,sec_entry=None):
    cpu,state=machine(fault=fault)
    cpu.mem_write(placement,image)
    # A previous trial's valid cookie must never survive a normal accepted boot.
    cpu.mem_write(RECORD,struct.pack('<4Q',MAGIC,INVERSE,0x1234,7))
    cpu.mem_write(HANDOFF+0x60,bytes([0xa5])*32)
    cpu.mem_write(HANDOFF+0x800,bytes([0xa5])*72)
    if DTB<=pointer<0x48080000-40:
        cpu.mem_write(pointer,struct.pack('>II',magic,size)+bytes(32))
    cpu.reg_write(UC_ARM64_REG_PSTATE,0x3C0|(el<<2)|(1 if el else 0))
    if el in (1,2):cpu.cpr_write(3,0 if el==1 else 4,1,0,0,0)
    cpu.reg_write(UC_ARM64_REG_X0,pointer)
    dis=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_LITTLE_ENDIAN)
    dis.skipdata=True
    code=list(dis.disasm(image[:4096],placement))
    park,=[i.address for i in code if i.mnemonic=='wfe']
    if el==0:
        # Unicorn leaves CurrentEL at EL1 after a PSTATE-only EL0 write.
        # Supply zero at this read to test the guard, not an EL0 privilege trap.
        pc,=[i.address for i in code if i.mnemonic=='mrs' and i.op_str=='x10, currentel']
        def current_el_read(uc,pc,n,_):
            uc.reg_write(UC_ARM64_REG_X10,0);uc.reg_write(UC_ARM64_REG_PC,pc+4)
        cpu.hook_add(UC_HOOK_CODE,current_el_read,begin=pc,end=pc)
    if sctlr:
        # Model only the MRS result; no hardware page-table activation is claimed.
        sites=[i.address for i in code if i.mnemonic=='mrs' and i.op_str.startswith('x11, sctlr_el')]
        def register_read(uc,pc,n,_):
            uc.reg_write(UC_ARM64_REG_X11,sctlr);uc.reg_write(UC_ARM64_REG_PC,pc+4)
        for pc in sites:cpu.hook_add(UC_HOOK_CODE,register_read,begin=pc,end=pc)
    writes=[]
    def write(uc,access,at,n,value,_):
        assert ((FD<=at and at+n<=FD+len(fd)) or
                (HANDOFF<=at and at+n<=HANDOFF+64) or
                (HANDOFF+0x60<=at and at+n<=HANDOFF+0x80) or
                (HANDOFF+0x80<=at and at+n<=HANDOFF+0x148) or
                (HANDOFF+0x800<=at and at+n<=HANDOFF+0x808) or
                (RECORD<=at and at+n<=RECORD+32) or
                (WDT<=at and at+n<=WDT+12) or
                (STACK<=at and at+n<=STACK+0x10000)),hex(at)
        writes.append((at,n,value))
    cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    trapped=False
    try:cpu.emu_start(placement,FD if accepted else park,count=4000000)
    except UcError as error:
        assert el==0 and error.errno==UC_ERR_EXCEPTION,(name,error)
        trapped=True
    assert trapped or cpu.reg_read(UC_ARM64_REG_PC)==(FD if accepted else park),name
    assert bool(state['writes'])==armed,(name,state)
    if armed:
        assert [w[0]-WDT for w in state['writes']]==[0]
        assert state['reloads']==0 and state['length']==0xF800
        assert state['mode']&~1==0x8C # Unrelated mode bits preserved.
        if fault is None:assert state['mode']&1==0
    assert bytes(cpu.mem_read(HANDOFF+0x60,32))==(bytes(32) if accepted else bytes([0xa5])*32)
    assert bytes(cpu.mem_read(HANDOFF+0x800,72))==((bytes(8)+bytes([0xa5])*64) if accepted else bytes([0xa5])*72)
    if accepted:
        assert bytes(cpu.mem_read(FD,len(fd)))==fd
        assert cpu.reg_read(UC_ARM64_REG_X0)==pointer
        assert struct.unpack('<Q',cpu.mem_read(HANDOFF+0x38,8))[0]==2
        record=struct.unpack('<4Q',cpu.mem_read(RECORD,32))
        assert record==((MAGIC,INVERSE,0,1) if trial else (0,0,0,0)),(name,record)
        if sec_entry is not None:
            before=list(state['writes'])
            cpu.emu_start(FD+sec_entry,STOP,count=10000)
            assert cpu.reg_read(UC_ARM64_REG_PC)==STOP
            assert state['writes']==before+[(WDT,4,0x2200008C)] and state['reloads']==0
            assert struct.unpack('<Q',cpu.mem_read(HANDOFF+0x38,8))[0]==3
            assert struct.unpack('<IIQ',cpu.mem_read(RECORD+16,16))==(0x8C,0xF800,2)
    elif not armed:assert not writes,(name,writes)
    return {'case':name,'accepted':accepted,'watchdog_disable_attempted':armed,
            'modeled_watchdog_armed':bool(state['mode']&1),'watchdog_writes':len(state['writes']),
            'watchdog_reload_count':state['reloads'],'same_CPU_BootShim_to_packaged_SEC_executed':sec_entry is not None,
            'timer_expiration_or_physical_reset_tested':False}


def sec_case(fd,entry,name,*,cookie=True,inverse=INVERSE,handshake=HANDSHAKE,
             stage=2,mode=0x8C,length=0xF000,preserve=True,status=2,fault=None):
    cpu,state=machine(mode,length,fault)
    cpu.mem_write(FD,fd)
    cpu.mem_write(HANDOFF,struct.pack('<Q',handshake))
    cpu.mem_write(HANDOFF+0x38,struct.pack('<Q',stage))
    cpu.mem_write(RECORD,struct.pack('<4Q',MAGIC if cookie else 0,inverse,0,1))
    writes=[]
    def write(uc,access,at,n,value,_):
        assert ((STACK<=at and at+n<STACK+0x10000) or (at==WDT and n==4) or
                (at==HANDOFF+0x38 and n==8) or (RECORD+16<=at and at+n<=RECORD+32)),hex(at)
        writes.append((at,n,value))
    cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    cpu.emu_start(FD+entry,STOP,count=10000)
    assert cpu.reg_read(UC_ARM64_REG_PC)==STOP
    assert cpu.reg_read(UC_ARM64_REG_SP)==STACK+0x8000
    assert struct.unpack('<Q',cpu.mem_read(HANDOFF+0x38,8))[0]==3
    assert state['reloads']==0
    if preserve:
        assert state['writes']==[(WDT,4,0x22000000|(mode&0xFFFFFE))] and state['length']==length
        assert state['mode']==((mode&~1) if fault is None else (mode|1))
        observed=struct.unpack('<IIQ',cpu.mem_read(RECORD+16,16))
        assert observed==(state['mode'],length,status),(name,observed)
    else:
        assert state['writes']==[(WDT,4,0x22000000|(mode&0xFFFFFFFE))]
    return {'case':name,'packaged_SEC_PlatformInitialize_executed':True,
            'trial_watchdog_preserved':preserve,'watchdog_writes':len(state['writes']),
            'watchdog_reload_count':state['reloads']}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('normal-image','trial-image','fd','sec-map','report'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    doc={'all_passed':False,'status':'running','test_source_sha256':sha(Path(__file__).read_bytes())}
    def save():a.report.write_text(json.dumps(doc,indent=2)+'\n')
    save()
    normal,trial,fd=(x.read_bytes() for x in (a.normal_image,a.trial_image,a.fd))
    for image in (normal,trial):
        assert len(image)==0x401000 and image[4096:]==fd and image[56:60]==b'ARM\x64'
    symbols={n:int(at,16) for at,n in re.findall(r'^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$',a.sec_map.read_text(),re.M)}
    cases=[]
    def run(name,**kw):cases.append(shim_case(trial,fd,name,**kw))
    run('trial_EL1_valid',accepted=True,sec_entry=symbols['PlatformInitialize'])
    run('trial_EL2_valid',el=2,accepted=True,sec_entry=symbols['PlatformInitialize'])
    cases.append(shim_case(normal,fd,'normal_clears_retained_trial_cookie',trial=False,armed=False,accepted=True))
    run('trial_invalid_FDT_watchdog_disabled',magic=0)
    run('trial_unaligned_FDT_watchdog_disabled',pointer=DTB+1)
    run('trial_oversized_FDT_watchdog_disabled',size=0x200001)
    run('trial_FDT_below_reservation_watchdog_disabled',pointer=DTB-8)
    run('trial_mode_write_not_accepted_no_handoff',fault='mode')
    run('trial_wrong_placement_rejects_before_MMIO',placement=BASE+0x80000,armed=False)
    run('trial_EL0_rejects_before_MMIO',el=0,armed=False)
    run('trial_MMU_on_rejects_before_MMIO',sctlr=1,armed=False)
    run('trial_Dcache_on_rejects_before_MMIO',sctlr=4,armed=False)
    cases.append(shim_case(normal,fd,'normal_invalid_FDT_still_writes_nothing',trial=False,magic=0,armed=False))
    for name,kw in (
        ('SEC_valid_interactive_trial_keeps_watchdog_disabled',{}),
        ('SEC_no_cookie_existing_production_policy',{'cookie':False,'preserve':False}),
        ('SEC_cookie_inverse_mismatch_existing_policy',{'inverse':0,'preserve':False}),
        ('SEC_handoff_magic_mismatch_existing_policy',{'handshake':0,'preserve':False}),
        ('SEC_incomplete_shim_existing_policy',{'stage':1,'preserve':False}),
        ('SEC_trial_unexpected_enabled_timer_disabled',{'mode':0x85}),
        ('SEC_trial_unexpected_IRQ_watchdog_disabled',{'mode':0x8D}),
        ('SEC_trial_unrelated_length_preserved',{'length':0xF800}),
        ('SEC_watchdog_disable_rejected_diagnostic',{'mode':0x85,'fault':'mode','status':3})):
        cases.append(sec_case(fd,symbols['PlatformInitialize'],name,**kw))
    package=Path(__file__).resolve().parents[1]
    doc.update({'status':'passed','all_passed':True,'scope':'Actual normal/trial BootShim and packaged SEC ARM64 instructions; watchdog MMIO register/key acceptance is a fixture.',
        'normal_image_sha256':sha(normal),'trial_image_sha256':sha(trial),'FD_sha256':sha(fd),
        'sec_map_sha256':sha(a.sec_map.read_bytes()),'cases':cases,'trial_timeout_seconds':None,'interactive_watchdog_disabled':True,
        'source_sha256':{str(x.relative_to(package)):sha(x.read_bytes()) for x in
            (package/'BootShim.S',package/'Library/PlatformSecLib/PlatformSec.c',package/'Library/PlatformSecLib/RamBootWatchdog.h')},
        'no_timer_reload_in_SEC':True,'physical_timer_clock_or_reset_behavior_tested':False,
        'stock_Android_return_verified':False,'tablet_execution_observed':False,
        'CPU_register_read_fixtures':['EL0 CurrentEL guard input','Nonzero SCTLR guard inputs'],
        'unprotected_pre_entry_states':['Before BootShim entry','Unexpected image placement','EL0 entry','MMU or D-cache enabled entry'],
        'remaining':['Physical interactive setup stability beyond the former timeout','User-requested manual reset and Android return']})
    save()
    print(json.dumps({'all_passed':True,'compiled_code_cases':len(cases),'physical_reset_or_stock_return_tested':False}))


if __name__=='__main__':main()
