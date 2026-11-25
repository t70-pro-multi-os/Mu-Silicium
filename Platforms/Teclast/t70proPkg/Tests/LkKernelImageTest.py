#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run shipping LK's real kernel format detector and LZ4 decoder on the EDK2 image."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *

BASE,SOURCE,DEST,STACK,STOP=0xffff000050f00000,0x60000000,0x40000000,0x70000000,0x7fff0000
LK_SHA='05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4'


def exercise(lk,source,expected,name):
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for base,size in ((BASE,0x200000),(SOURCE,0x800000),(DEST,0x800000),(STACK,0x10000),(STOP,0x1000)):
        cpu.mem_map(base,size)
    cpu.mem_write(BASE,lk);cpu.mem_write(SOURCE,source)
    canary=b'untouched-after-decompression-guard'
    cpu.mem_write(DEST+len(expected)+32,canary)
    cpu.reg_write(UC_ARM64_REG_SP,STACK+0x8000);cpu.reg_write(UC_ARM64_REG_LR,STOP)
    for i,value in enumerate((SOURCE,DEST,len(source),0x800000)):
        cpu.reg_write(globals()['UC_ARM64_REG_X'+str(i)],value)
    saved={}
    for n in range(18,30):
        reg=globals()['UC_ARM64_REG_X'+str(n)];saved[reg]=0x12340000+n;cpu.reg_write(reg,saved[reg])
    returned=[];decoder=[];writes=[]
    def hook(uc,pc,n,_):
        if pc==STOP:returned.append(True);uc.emu_stop()
        elif pc in (BASE+0x6c0ec,BASE+0x6c1d8):
            raise AssertionError('Shipping LK rejected the candidate kernel')
        elif pc==BASE+0x5f228:decoder.append(True)
        else:
            uc.reg_write(UC_ARM64_REG_X0,1 if pc==BASE+0x1106c else 0)
            uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
    for address in (STOP,BASE+0x1106c,BASE+0x6f520,BASE+0x6c0ec,BASE+0x6c1d8,BASE+0x5f228):
        cpu.hook_add(UC_HOOK_CODE,hook,begin=address,end=address)
    def write(uc,access,address,size,value,_):
        assert ((STACK<=address and address+size<=STACK+0x10000) or
                (DEST<=address and address+size<=DEST+len(expected)+16)),hex(address)
        if DEST<=address<DEST+0x800000:writes.append(address+size)
    cpu.hook_add(UC_HOOK_MEM_WRITE,write)
    cpu.emu_start(BASE+0xe338,0,count=30000000)
    assert returned and cpu.reg_read(UC_ARM64_REG_W0)==0
    assert cpu.reg_read(UC_ARM64_REG_SP)==STACK+0x8000
    assert all(cpu.reg_read(reg)==value for reg,value in saved.items())
    decoded=bytes(cpu.mem_read(DEST,len(expected)))
    assert decoded==expected
    assert bytes(cpu.mem_read(DEST+len(expected)+32,len(canary)))==canary
    assert bool(decoder)==(name=='current_LZ4_boot_image')
    assert decoded[56:60]==b'ARM\x64' and struct.unpack_from('<Q',decoded,8)[0]==0
    assert struct.unpack_from('<Q',decoded,16)[0]==len(decoded)
    return {'case':name,'shipping_format_detector_executed':True,'shipping_LZ4_blocks_executed':len(decoder),
            'decoded_image_sha256':hashlib.sha256(decoded).hexdigest(),'decoded_bytes':len(decoded),
            'entry_address':'0x40000000','decoded_bytes_match_built_BootShim_and_FD':True,
            'destination_guard_preserved':True,'highest_write_end':hex(max(writes))}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('lk','boot-image','raw-image','fd','report'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();lk=a.lk.read_bytes();boot=a.boot_image.read_bytes();raw=a.raw_image.read_bytes();fd=a.fd.read_bytes()
    assert hashlib.sha256(lk).hexdigest()==LK_SHA
    assert boot[:8]==b'ANDROID!' and struct.unpack_from('<I',boot,40)[0]==4
    length=struct.unpack_from('<I',boot,8)[0];compressed=boot[4096:4096+length]
    assert compressed[:4]==bytes.fromhex('02214c18')
    assert raw[4096:]==fd and len(fd)==0x400000 and len(raw)==0x401000
    cases=[exercise(lk,source,raw,name) for source,name in ((compressed,'current_LZ4_boot_image'),(raw,'raw_Linux_Image'))]
    report={'scope':'Actual shipping LK format detection, LZ4 block decoder and memcpy on the current EDK2 candidate; timer and logging are fixtures.',
            'shipping_lk_sha256':hashlib.sha256(lk).hexdigest(),'candidate_boot_image_sha256':hashlib.sha256(boot).hexdigest(),
            'raw_BootShim_image_sha256':hashlib.sha256(raw).hexdigest(),'FD_sha256':hashlib.sha256(fd).hexdigest(),
            'cases':cases,'all_passed':True,'tablet_execution_observed':False,'UEFI_execution_observed':False}
    a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'all_passed':True,'actual_shipping_decoder_cases':len(cases),'decoded_bytes':len(raw),
                      'tablet_execution_observed':False}))


if __name__=='__main__':main()
