#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the exact original-LK legacy LZ4 decoder on stock and selector payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import unicorn
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *
LK_SHA = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"
STOCK_SHA = "fc68b2ea42142f6a822f89afcaab94d2f8b09d865ae7f3b86f1b6c183d6d3475"
CODE, SRC, DST, STACK, STOP = 0x10000000, 0x50000000, 0x40000000, 0x60000000, 0x70000000
MB_KERNEL_CAPACITY = 0x7c80000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def exercise(lk, compressed, expected, name):
    assert compressed[:4] == b"\x02\x21\x4c\x18"
    assert DST + len(expected) + 16 < 0x43f7f000
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(CODE,0x100000,UC_PROT_READ|UC_PROT_EXEC)
    cpu.mem_write(CODE,lk)
    cpu.mem_map(SRC,(len(compressed)+16+4095)//4096*4096,UC_PROT_READ)
    cpu.mem_write(SRC,compressed)
    extent = (len(expected)+16+4095)//4096*4096
    cpu.mem_map(DST,extent,UC_PROT_READ|UC_PROT_WRITE)
    cpu.mem_write(DST+len(expected),b"\xcc"*(extent-len(expected)))
    cpu.mem_map(STACK,0x10000)
    cpu.mem_map(STOP,4096,UC_PROT_READ|UC_PROT_EXEC)
    cpu.reg_write(UC_ARM64_REG_SP,STACK+0x10000-16)
    for reg,value in [(UC_ARM64_REG_X0,SRC),(UC_ARM64_REG_X1,DST),
                      (UC_ARM64_REG_X2,len(compressed)),(UC_ARM64_REG_X3,MB_KERNEL_CAPACITY),
                      (UC_ARM64_REG_X30,STOP)]:
        cpu.reg_write(reg,value)
    stubs, tail_writes = [], []
    def stub(uc,address,size,user):
        stubs.append("clock" if address==CODE+0x1106c else "printf")
        uc.reg_write(UC_ARM64_REG_X0,0)
        uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
    for offset in (0x1106c,0x6f520):
        cpu.hook_add(UC_HOOK_CODE,stub,begin=CODE+offset,end=CODE+offset)
    def tail(uc,access,address,size,value,user):
        tail_writes.append((address,size))
    cpu.hook_add(UC_HOOK_MEM_WRITE,tail,begin=DST+len(expected)-64,end=DST+extent-1)
    cpu.emu_start(CODE+0xe338,STOP,count=500000000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == STOP
    assert cpu.reg_read(UC_ARM64_REG_X0) == 0
    assert bytes(cpu.mem_read(DST,len(expected))) == expected
    maximum = max((address+size for address,size in tail_writes),default=DST+len(expected))
    assert maximum <= DST+len(expected)+16
    after = max(maximum,DST+len(expected))
    assert bytes(cpu.mem_read(after,DST+extent-after)) == b"\xcc"*(DST+extent-after)
    return {"case":name,"compressed_bytes":len(compressed),"decoded_bytes":len(expected),
            "decoded_sha256":sha(expected),"return_status":0,"entire_output_matches":True,
            "maximum_tail_store_end_relative_to_output":maximum-DST,
            "write_slack_bytes":max(0,maximum-DST-len(expected)),
            "beyond_tail_slack_unchanged":True,"stubbed_non_decoder_functions":stubs}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("lk","stock-boot","artifacts","report"):
        p.add_argument("--"+name,type=Path,required=True)
    a=p.parse_args()
    lk,stock=a.lk.read_bytes(),a.stock_boot.read_bytes()
    assert sha(lk)==LK_SHA and sha(stock)==STOCK_SHA
    report=json.loads((a.artifacts/"lk-boot-select.json").read_text())
    padded=(a.artifacts/"stock-padded.Image").read_bytes()
    kernel=padded[:report["stock_raw_bytes"]]
    assert sha(kernel)==report["stock_kernel_sha256"]
    n=struct.unpack_from("<I",stock,8)[0]
    capsule=(a.artifacts/"lk-boot-select.Image").read_bytes()
    assert sha(capsule)==report["image_sha256"]
    cases=[exercise(lk,stock[4096:4096+n],kernel,"original_shipping_kernel"),
           exercise(lk,(a.artifacts/"lk-boot-select.Image.lz4").read_bytes(),capsule,"final_selector_capsule")]
    result={"schema":1,"all_passed":True,"device_access":False,"count":len(cases),"cases":cases,
            "lk_sha256":LK_SHA,"tested_artifact_sha256":sha(capsule),"unicorn_version":unicorn.__version__,
            "original_decoder_file_offset":"0xe338","original_block_decoder_file_offset":"0x5f228",
            "actual_lk_memory_copy_executed":True,"timer_and_print_only_stubbed":True,
            "kernel_allocator_runtime_state_verified":False,"physical_LK_execution_verified":False,
            "legacy_lz4_total_output_capacity_check_present":False,
            "test_source_sha256":sha(Path(__file__).read_bytes())}
    a.report.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2))


if __name__=="__main__":
    main()
