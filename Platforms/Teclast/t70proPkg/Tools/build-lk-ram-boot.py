#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build hash-bound RAM-only boot-chain bridges inside the existing builder."""
import argparse
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
PRELOADER_SHA = 'cc42d1aec1f3dd9a94cfd9147ab5c1f55e592d1aa17ebbe7a9d4c1a49e9d436e'
BL2_SHA = '340da9b31b680a2c7671e9d3e5c741f6f9a557a22d4a174157cd6e742596251f'


def run(*args):
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True)
    if result.returncode:
        print(result.stderr, file=sys.stderr, end='')
        result.check_returncode()
    return result.stdout


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('lk','backup','preloader','out'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args()
    tools=Path(__file__).resolve().parent
    spec=importlib.util.spec_from_file_location('lk_plan',tools/'lk-ram-boot-plan.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    lk,preloader=a.lk.read_bytes(),a.preloader.read_bytes()
    if hashlib.sha256(lk).hexdigest()!=module.LK_SHA256:
        raise ValueError('Shipping LK hash differs')
    if hashlib.sha256(preloader).hexdigest()!=PRELOADER_SHA:
        raise ValueError('Shipping preloader hash differs')
    parts=json.loads((a.backup/'partition-layout.json').read_text())['partitions']
    lk_part=next(x for x in parts if x['name']=='lk_a')
    with (a.backup/'images/ufs-lu2.img').open('rb') as f:
        f.seek(lk_part['offset_bytes']+0xc5cc0);header=f.read(512)
        if struct.unpack_from('<I',header)[0]!=0x58881688 or header[8:15]!=b'bl2_ext':
            raise ValueError('BL2_EXT partition header differs')
        bl2=f.read(struct.unpack_from('<I',header,4)[0])
    if hashlib.sha256(bl2).hexdigest()!=BL2_SHA:
        raise ValueError('Shipping BL2_EXT hash differs')
    if any(bl2[0x400:0x800]) or bl2[0x7850:0x7854]!=module.branch(0x7850,0x8574,True):
        raise ValueError('BL2_EXT code/cave differs')
    a.out.mkdir(parents=True,exist_ok=True)

    def compile(name,source,linker,macros,arch):
        elf,binary,symbols=(a.out/(name+suffix) for suffix in ('.elf','.bin','.symbols.txt'))
        command=['/usr/bin/clang','--target='+arch,'-nostdlib','-fuse-ld=lld','-Wl,--build-id=none']
        command+=['-march=armv8-a'] if arch.startswith('aarch64') else ['-march=armv7-a','-marm']
        command+=['-Wl,-T,'+str(linker)] if linker else ['-Wl,-Ttext=0x40200000','-Wl,-e,_start']
        command+=['-D'+key+'='+value for key,value in macros.items()]
        command+=[str(source),'-o',str(elf)]
        run(*command)
        run('/usr/bin/llvm-objcopy','-O','binary',elf,binary)
        symbols.write_text(run('/usr/bin/llvm-nm','--defined-only',elf))
        return binary.read_bytes(),module.symbols(symbols)

    payload,names=compile('lk-ram-boot',tools/'LkRamBoot.S',tools/'LkRamBoot.ld',{},'aarch64-none-elf')
    lk_plan=module.make_plan(lk,payload,names)
    (a.out/'ram-edit-plan.json').write_text(json.dumps(lk_plan,indent=2)+'\n')
    # Keep the original symbol artifact path for earlier consumers.
    (a.out/'symbols.txt').write_text((a.out/'lk-ram-boot.symbols.txt').read_text())
    version=b'11ee062dcfa_202607030435\0'
    if lk.count(version)!=1:raise ValueError('Shipping LK version fingerprint differs')
    version_offset=lk.index(version);version_tail_offset=version_offset+len(version)-5
    macros={'LK_VERSION_OFFSET':hex(version_offset),'LK_VERSION_TAIL_OFFSET':hex(version_tail_offset),'LK_PATCH_FILE':'"'+str((a.out/'lk-ram-boot.bin').resolve())+'"',
            'LK_REGISTER_ORIGINAL':hex(struct.unpack_from('<I',lk,0x8358)[0]),
            'LK_REGISTER_REPLACEMENT':hex(struct.unpack('<I',module.branch(0x8358,names['register_ram_boot'],True))[0]),
            'LK_SOURCE_REPLACEMENT':hex(struct.unpack('<I',module.branch(0x5b5e4,names['ram_image_source_hook']))[0])}
    macros.update({'LK_PREFIX'+str(i):hex(struct.unpack_from('<I',lk,i*4)[0]) for i in range(4)})
    bridge,bridge_names=compile('bl2-ram-boot',tools/'Bl2RamBoot.S',tools/'Bl2RamBoot.ld',macros,'aarch64-none-elf')
    if not 0<len(bridge)<=1024 or len(bridge)%4 or bridge_names['bl2_patch_lk']!=0x400:
        raise ValueError('BL2 bridge exceeds verified padding')
    bl2_branch=module.branch(0x7850,0x400,True)
    bl2_edits=[]
    for offset,data in ((0x400,bridge),(0x7850,bl2_branch)):
        bl2_edits.append({'offset':offset,'expected_hex':bl2[offset:offset+len(data)].hex(),'replacement_hex':data.hex()})
    macros={'PRELOADER_FILE':'"'+str(a.preloader.resolve())+'"',
            'BL2_PATCH_FILE':'"'+str((a.out/'bl2-ram-boot.bin').resolve())+'"',
            'BL2_HOOK_BRANCH':hex(struct.unpack('<I',bl2_branch)[0])}
    macros.update({'BL2_PREFIX'+str(i):hex(struct.unpack_from('<I',bl2,i*4)[0]) for i in range(4)})
    stage,stage_names=compile('preloader-ram-boot',tools/'PreloaderRamBoot.S',None,macros,'armv7a-none-eabi')
    if not 40<len(stage)<=4096 or len(stage)%4 or stage_names['_start']!=0x40200000:
        raise ValueError('Preloader stage size/entry differs')
    diagnostic,diagnostic_names=compile('preloader-retained-probe',tools/'PreloaderRamBoot.S',None,
                                        dict(macros,T70_RETAINED_PROBE='1'),'armv7a-none-eabi')
    if not 296<len(diagnostic)<=4096 or len(diagnostic)%4 or diagnostic_names['_start']!=0x40200000:
        raise ValueError('Retained-record probe size/entry differs')
    # Diagnostic variant of the existing probe: report guards, then return to
    # the unchanged parser. It never installs the boot-chain bridge.
    audit,audit_names=compile('preloader-guard-audit',tools/'PreloaderRamProbe.S',None,
                             {'PRELOADER_FILE':'"'+str(a.preloader.resolve())+'"',
                              'T70_GUARD_AUDIT':'1'},'armv7a-none-eabi')
    if not 80<len(audit)<=4096 or len(audit)%4 or audit_names['_start']!=0x40200000:
        raise ValueError('Preloader guard audit size/entry differs')
    result={'schema_version':1,'shipping_lk_sha256':module.LK_SHA256,
            'shipping_preloader_sha256':PRELOADER_SHA,'shipping_bl2_ext_sha256':BL2_SHA,
            'LK_plan':'ram-edit-plan.json','BL2_RAM_edits':bl2_edits,
            'BL2_bridge_sha256':hashlib.sha256(bridge).hexdigest(),'BL2_bridge_bytes':len(bridge),
            'preloader_stage_sha256':hashlib.sha256(stage).hexdigest(),'preloader_stage_bytes':len(stage),
            'preloader_stage_entry':0x40200000,'preloader_hook_entry':stage_names['bl2_entry_patch'],
            'preloader_hook_RAM_address':0x230f44,'preloader_hook_original_hex':preloader[0x230f44-0x200f10:0x230f50-0x200f10].hex(),
            'LK_payload_sha256':lk_plan['payload_sha256'],'LK_payload_bytes':len(payload),
            'LK_version_fingerprint_offset':version_offset,'LK_version_tail_offset':version_tail_offset,
            'RAM_only_version_trace':{'RB01':'LK prefix and version matched','RB02':'registration guard matched','RB03':'source guard matched','RB05':'LK RAM edits and cache synchronization finished'},
            'normal_64bit_initialization_preserved_by_design':True,
            'storage_writes':[],'applied_to_device':False,'UEFI_execution_observed':False}
    (a.out/'bootchain-ram-plan.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({key:result[key] for key in ('preloader_stage_bytes','BL2_bridge_bytes','LK_payload_bytes','storage_writes','applied_to_device')}))


if __name__=='__main__':
    main()
