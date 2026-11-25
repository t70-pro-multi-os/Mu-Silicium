#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build an offline Android/UEFI selector prototype. Never accesses a device."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import lz4.block
sys.dont_write_bytecode = True
TOOLS=Path(__file__).resolve().parent
REPO=TOOLS.parents[3]
MDE=REPO/"Mu_Basecore/MdePkg"
FDT=MDE/"Library/BaseFdtLib"
MEDIA=REPO/"Silicon/MediaTek/MediaTekPkg"
MENU_LIBRARIES=(
    MEDIA/"Library/FramebufferCoreLib/MtkDisplayCore.c",
    FDT/"FdtLib.c", FDT/"LibFdtWrapper.c",
    *(FDT/"libfdt/libfdt"/name for name in ("fdt.c","fdt_ro.c","fdt_check.c","fdt_addresses.c")),
    REPO/"Mu_Basecore/MdeModulePkg/Universal/Console/GraphicsConsoleDxe/LaffStd.c",
)
STOCK_SHA="fc68b2ea42142f6a822f89afcaab94d2f8b09d865ae7f3b86f1b6c183d6d3475"
MAGIC=0x3151455255373054
INVERSE=(~MAGIC)&((1<<64)-1)
BASE=0x40000000
REQUEST=0x43f7ffc0

def sha(data): return hashlib.sha256(data).hexdigest()
def run(*args):
    p=subprocess.run([str(a) for a in args],capture_output=True,text=True)
    if p.returncode: raise RuntimeError(p.stderr)
    return p.stdout
def image(data):
    if len(data)<64 or data[56:60]!=b"ARM\x64": raise ValueError("Not an ARM64 Image")
    offset,size,flags=struct.unpack_from("<QQQ",data,8)
    if offset or size<len(data) or size%16 or flags!=0xa:
        raise ValueError("Unsupported ARM64 Image layout")
    return size
def legacy_lz4(data):
    if data[:4]!=b"\x02\x21\x4c\x18": raise ValueError("Expected stock legacy LZ4")
    at=4;raw=bytearray()
    while at<len(data):
        if len(data)-at<4: raise ValueError("Truncated LZ4 length")
        count=struct.unpack_from("<I",data,at)[0];at+=4
        if not count or count>len(data)-at: raise ValueError("Invalid LZ4 block length")
        raw+=lz4.block.decompress(data[at:at+count],uncompressed_size=8*1024*1024);at+=count
        if len(raw)>64*1024*1024: raise ValueError("Kernel exceeds prototype bound")
    return bytes(raw)
def compile_capsule(stock,uefi,out,*,lk_boot_mode=False):
    stock_size=image(stock);uefi_size=image(uefi)
    if uefi_size!=len(uefi): raise ValueError("UEFI payload must include its declared extent")
    if stock_size+len(uefi)+0x10000>=0x3f7f000: raise ValueError("Payload overlaps handoff")
    if out.is_symlink() or (out.exists() and (not out.is_dir() or any(out.iterdir()))):
        raise ValueError("Output must be empty; existing artifacts preserved")
    out.mkdir(parents=True,exist_ok=True)
    # The BSS padding is explicit; no byte from the UEFI payload enters Android.
    (out/"stock-padded.Image").write_bytes(stock+bytes(stock_size-len(stock)))
    (out/"uefi.Image").write_bytes(uefi)
    ident=int.from_bytes(hashlib.sha256(uefi).digest()[:8],"little")
    flags=["--target=aarch64-none-elf","-O2","-ffreestanding","-fno-builtin",
           "-fno-stack-protector","-fno-unwind-tables","-fno-asynchronous-unwind-tables",
           "-fno-jump-tables","-mgeneral-regs-only","-fno-pic"]
    if lk_boot_mode: flags.append("-DT70_USE_LK_BOOT_MODE")
    c_source="BootModeSelect.c" if lk_boot_mode else "BootSelect.c"
    prefix="lk-boot-select" if lk_boot_mode else "slotless-prototype"
    run("clang",*flags,"-c",TOOLS/c_source,"-o",out/"reserve.o")
    run("clang",*flags,"-c",TOOLS/"BootSelect.S",
        '-DT70_STOCK_FILE="'+str((out/"stock-padded.Image").resolve())+'"',
        '-DT70_UEFI_FILE="'+str((out/"uefi.Image").resolve())+'"',
        "-DT70_REQUEST_IMAGE_ID="+hex(ident),"-o",out/"select.o")
    elf=out/(prefix+".elf")
    objects=[out/"select.o",out/"reserve.o"]
    link=[]
    address=0
    if lk_boot_mode:
        # This capsule already refuses any other placement before entering C.
        address=BASE
        link=["--gc-sections",f"--defsym=_image_base={address:#x}","--defsym=_stack_bytes=16384"]
        includes=(MDE/"Include", MDE/"Include/AArch64", FDT, FDT/"libfdt/libfdt",
                  MEDIA/"Include", REPO/"Mu_Basecore/MdeModulePkg/Include")
        for source in (TOOLS/"BootMenu.c",TOOLS/"BootMenuLibraries.c",*MENU_LIBRARIES):
            obj=out/(source.stem+".o")
            run("clang",*flags,"-ffunction-sections","-fdata-sections","-fshort-wchar",
                *(f"-I{path}" for path in includes),"-c",source,"-o",obj)
            objects.append(obj)
    run("ld.lld",*link,"-T",TOOLS/"BootSelect.ld",*objects,"-o",elf)
    run("llvm-objcopy","-O","binary",elf,out/(prefix+".Image"))
    names={}
    for line in run("llvm-nm","--defined-only",elf).splitlines():
        fields=line.split()
        if len(fields)==3 and fields[1].lower()!="a":
            names[fields[2]]=int(fields[0],16)-address
    data=(out/(prefix+".Image")).read_bytes()
    if image(data)!=len(data) or names["_head"]!=0: raise ValueError("Capsule extent mismatch")
    if names["_copy"]<=stock_size or names["_copy"]<names["_uefi_end"]:
        raise ValueError("Copy code overlaps a source or destination")
    report={"schema_version":1,"installable":False,"device_access":False,
            "stock_kernel_sha256":sha(stock),"stock_raw_bytes":len(stock),
            "stock_memory_bytes":stock_size,"uefi_sha256":sha(uefi),
            "request_address":REQUEST,"request_page":0x43f7f000,"request_image_id":ident,
            "request_transport":"reserved RAM; reset retention and Android issuer unverified",
            "request_words":[MAGIC,INVERSE,ident,(~ident)&((1<<64)-1)],
            "symbols":names,"image_bytes":len(data),"image_sha256":sha(data),
            "source_sha256":{name:sha((TOOLS/name).read_bytes()) for name in ("BootSelect.S","BootSelect.c","BootSelect.h","BootSelect.ld","build-slotless-boot.py")},
            "remaining":["Android request writer and init_boot integration",
                         "real reset retention of the reserved request",
                         "LK allocation/decompression of the larger kernel payload",
                         "full partition AVB packaging and stock return on hardware"]}
    if lk_boot_mode:
        for key in ("request_address","request_page","request_image_id","request_transport","request_words"):
            report.pop(key)
        report.update(
            artifact_prefix=prefix, stock_kernel_preserved=True, fdt_modified=False,
            selection_transport="LK factory mode 4 starts EDK2 after volume up is released; key-selected recovery mode 2 and stock Fastboot continue mode 99 also offer EDK2",
            selection_instructions="Factory 진입이 잡힌 뒤 볼륨 +를 떼면 바로 EDK2가 열립니다. 볼륨 +를 계속 누르면 순정 Factory가 열립니다. 복구용 선택 화면에서는 볼륨 −로 EDK2를 엽니다.",
            remaining=["physical factory-menu selection, stock Recovery and ordinary Android on the tablet",
                       "LK decompression of the larger payload and unlocked AVB acceptance",
                       "real UEFI entry, original boot restoration and Android return"])
        report["source_sha256"]={name:sha((TOOLS/name).read_bytes()) for name in
            ("BootSelect.S","BootModeSelect.c","LkBootMode.h","BootSelect.h","BootSelect.ld",
             "BootMenu.c","BootMenuLibraries.c","build-slotless-boot.py")}
        report["menu_library_sha256"]={str(path.relative_to(REPO)):sha(path.read_bytes())
                                       for path in (*MENU_LIBRARIES,MEDIA/"Include/Library/MtkDisplayCore.h")}
    (out/(prefix+".json")).write_text(json.dumps(report,indent=2)+"\n")
    return data,report
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--stock-boot",type=Path,required=True)
    p.add_argument("--uefi-image",type=Path,required=True)
    p.add_argument("--out",type=Path,required=True)
    a=p.parse_args()
    boot=a.stock_boot.read_bytes()
    if sha(boot)!=STOCK_SHA or boot[:8]!=b"ANDROID!": raise ValueError("Shipping boot_a hash mismatch")
    if struct.unpack_from("<I",boot,40)[0]!=4 or struct.unpack_from("<I",boot,12)[0]!=0:
        raise ValueError("Expected stock v4 kernel-only image")
    size=struct.unpack_from("<I",boot,8)[0]
    raw=legacy_lz4(boot[4096:4096+size])
    capsule,report=compile_capsule(raw,a.uefi_image.read_bytes(),a.out)
    run("lz4","-l","-f",a.out/"slotless-prototype.Image",a.out/"slotless-prototype.Image.lz4")
    compressed=(a.out/"slotless-prototype.Image.lz4").read_bytes()
    if legacy_lz4(compressed)!=capsule: raise ValueError("Compressed capsule differs")
    header=bytearray(boot[:4096])
    struct.pack_into("<I",header,8,len(compressed));struct.pack_into("<I",header,1580,0)
    packed=bytes(header)+compressed
    packed+=bytes((-len(packed))%4096)
    (a.out/"slotless-prototype-boot.img").write_bytes(packed)
    report.update(stock_boot_sha256=sha(boot),boot_bytes=len(packed),boot_sha256=sha(packed),
                  avb_packaged=False,init_boot_modified=False)
    (a.out/"slotless-prototype.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({k:report[k] for k in ("installable","image_bytes","boot_bytes","init_boot_modified")}))
if __name__=="__main__": main()
