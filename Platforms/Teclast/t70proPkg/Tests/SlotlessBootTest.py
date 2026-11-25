#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the compiled selector, FDT reservation and payload copies in Unicorn."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
sys.dont_write_bytecode=True
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE,UC_MEM_READ
from unicorn.arm64_const import *
PKG=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("slotless",PKG/"Tools/build-slotless-boot.py")
build=importlib.util.module_from_spec(spec);spec.loader.exec_module(build)
BASE,DTB,PAGE,REQUEST=0x40000000,0x47c80000,0x43f7f000,0x43f7ffc0

def fixture_image(raw=4096,memory=8192,seed=0x5a):
    data=bytearray([seed])*raw
    struct.pack_into("<IIQQQQQQ",data,0,0x14000010,0,0,memory,0xa,0,0,0)
    data[56:64]=b"ARM\x64"+bytes(4)
    return bytes(data)
def fdt(entries=(),*,total=None,fixed_reserved=None,owner_cells=(2,2)):
    reserve=b"".join(struct.pack(">QQ",*e) for e in entries)+bytes(16)
    def node(name):
        b=struct.pack(">I",1)+name.encode()+b"\0"
        return b+bytes((-len(b))%4)
    def prop(offset,value):
        b=struct.pack(">III",3,len(value),offset)+value
        return b+bytes((-len(b))%4)
    structure=node("")
    names=b"\0"
    if fixed_reserved is not None:
        names=b"#address-cells\0#size-cells\0reg\0"
        structure+=node("reserved-memory")
        if owner_cells is not None:
            structure+=prop(names.index(b"#address-cells"),struct.pack(">I",owner_cells[0]))
            structure+=prop(names.index(b"#size-cells"),struct.pack(">I",owner_cells[1]))
        structure+=node("other-owner")
        structure+=prop(names.index(b"reg"),struct.pack(">QQ",*fixed_reserved))
        structure+=struct.pack(">II",2,2)
    structure+=struct.pack(">II",2,9)
    at=40+len(reserve);strings=at+len(structure)
    size=total or strings+len(names)
    header=struct.pack(">10I",0xd00dfeed,size,at,strings,40,17,16,0,len(names),len(structure))
    return (header+reserve+structure+names).ljust(size,b"\0")
def reserved(data):
    total,at=struct.unpack_from(">I",data,4)[0],struct.unpack_from(">I",data,16)[0]
    result=[]
    while at+16<=total:
        entry=struct.unpack_from(">QQ",data,at);at+=16
        if entry==(0,0):return result
        result.append(entry)
    raise AssertionError("Missing reservation terminator")
def exercise(capsule,report,name,record=None,dt=None,placement=BASE,expect_uefi=False,expect_reserve=True,el=1,expect_park=False,*,keypad=None,expected_keypad_reads=0, setup=None):
    cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    cpu.mem_map(BASE,0x8080000)
    cpu.mem_write(placement,capsule)
    original=fdt() if dt is None else dt
    cpu.mem_write(DTB,original+bytes(16))
    request=record if record is not None else bytes(32)
    cpu.mem_write(REQUEST,request)
    cpu.reg_write(UC_ARM64_REG_PSTATE,0x3c0|(el<<2)|1)
    cpu.cpr_write(3,0 if el==1 else 4,1,0,0,0)
    cpu.cpr_write(3,3,0,0,1,0x40004)
    cpu.reg_write(UC_ARM64_REG_X0,DTB)
    keypad_reads=[]
    if keypad is not None:
        base=0x10010000
        cpu.mem_map(base,0x1000)
        cpu.mem_write(base+0x24,struct.pack("<H",keypad[0]))
        cpu.mem_write(base+0x04,struct.pack("<H",keypad[1]))
        def keypad_access(uc,access,at,n,value,_):
            assert access==UC_MEM_READ and n==2 and at in (base+0x24,base+0x04),name
            keypad_reads.append(at-base)
        cpu.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,keypad_access,begin=base,end=base+0xfff)
    inspect = setup(cpu) if setup is not None else None
    writes=[]
    def write(uc,access,at,n,value,_):
        assert (PAGE<=at and at+n<=PAGE+4096) or (DTB<=at and at+n<=DTB+len(original)+16),(name,hex(at),n)
        writes.append((at,n))
    cpu.hook_add(UC_HOOK_MEM_WRITE,write,begin=PAGE,end=PAGE+4095)
    cpu.hook_add(UC_HOOK_MEM_WRITE,write,begin=DTB,end=DTB+len(original)+15)
    seen=[];visits=[];parked=[]
    copy_pc=placement+report["symbols"]["_copy"]
    def copying(uc,pc,n,_):
        seen.append(uc.reg_read(UC_ARM64_REG_X1))
        if expect_reserve:assert bytes(uc.mem_read(REQUEST,32))==bytes(32)
    cpu.hook_add(UC_HOOK_CODE,copying,begin=copy_pc,end=copy_pc)
    def entry(uc,pc,n,_):
        visits.append(pc)
        if len(visits)==2:uc.emu_stop()
    cpu.hook_add(UC_HOOK_CODE,entry,begin=placement,end=placement)
    if expect_park:
        def parking(uc,pc,n,_):
            parked.append(pc);uc.emu_stop()
        at=placement+report["symbols"]["_park"]
        cpu.hook_add(UC_HOOK_CODE,parking,begin=at,end=at)
    cpu.emu_start(placement,0,count=50000000)
    assert keypad_reads==[0x24,0x04][:expected_keypad_reads],name
    if expect_park:
        assert len(parked)==1 and visits==[placement] and not seen,name
        end=report["symbols"]["_uefi_end"]
        assert bytes(cpu.mem_read(placement,end))==capsule[:end],name
        assert bytes(cpu.mem_read(DTB,len(original)))==original,name
        assert bytes(cpu.mem_read(REQUEST,32))==request,name
        return {"case":name,"target":"stopped-before-kernel","payload_bytes_verified":0}
    assert len(visits)==2 and len(seen)==1,(name,hex(cpu.reg_read(UC_ARM64_REG_PC)),len(seen))
    branch="_uefi" if expect_uefi else "_stock"
    end=report["symbols"][branch+"_end"];start=report["symbols"][branch]
    expected=capsule[start:end]
    assert seen==[placement+start]
    assert bytes(cpu.mem_read(placement,len(expected)))==expected,(name,"payload copy differs")
    assert cpu.reg_read(UC_ARM64_REG_X0)==DTB
    assert cpu.reg_read(UC_ARM64_REG_PSTATE)&0x3c0==0x3c0
    assert all(cpu.reg_read(r)==0 for r in (UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3))
    new=bytes(cpu.mem_read(DTB,len(original)+16))
    if expect_reserve:
        assert any(a<=PAGE and PAGE+4096<=a+n for a,n in reserved(new)),name
        before_struct,before_strings=struct.unpack_from(">II",original,8)
        after_struct,after_strings=struct.unpack_from(">II",new,8)
        ss=struct.unpack_from(">I",original,36)[0];ts=struct.unpack_from(">I",original,32)[0]
        assert new[after_struct:after_struct+ss]==original[before_struct:before_struct+ss]
        assert new[after_strings:after_strings+ts]==original[before_strings:before_strings+ts]
    else:
        assert new[:len(original)]==original
        assert bytes(cpu.mem_read(REQUEST,32))==request
    result = {"case":name,"target":"UEFI" if expect_uefi else "Android",
              "request_consumed_before_copy":expect_reserve,"payload_bytes_verified":len(expected)}
    if inspect is not None:
        result.update(inspect())
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--artifacts",type=Path)
    p.add_argument("--report",type=Path,required=True)
    a=p.parse_args();cases=[]
    with tempfile.TemporaryDirectory(prefix="t70-slotless-") as tmp:
        capsule,r=build.compile_capsule(fixture_image(),fixture_image(4096,4096,0xa5),Path(tmp))
        valid=struct.pack("<4Q",*r["request_words"])
        cases.append(exercise(capsule,r,"no-request"))
        cases.append(exercise(capsule,r,"valid-request",valid,expect_uefi=True))
        cases.append(exercise(capsule,r,"EL2-default",el=2))
        cases.append(exercise(capsule,r,"EL2-uefi",valid,expect_uefi=True,el=2))
        for word in range(4):
            invalid=bytearray(valid);invalid[word*8]^=1
            cases.append(exercise(capsule,r,"corrupt-word-"+str(word),bytes(invalid)))
        cases.append(exercise(capsule,r,"consumed-request-replayed",bytes(32)))
        cases.append(exercise(capsule,r,"existing-reservation-rejected",valid,fdt([(PAGE,4096)]),expect_reserve=False))
        cases.append(exercise(capsule,r,"wider-existing-reservation-rejected",valid,fdt([(PAGE-4096,8192)]),expect_reserve=False))
        cases.append(exercise(capsule,r,"partial-reservation",valid,fdt([(PAGE+8,4096)]),expect_reserve=False))
        cases.append(exercise(capsule,r,"other-fixed-owner-rejected",valid,fdt(fixed_reserved=(PAGE,4096)),expect_reserve=False))
        cases.append(exercise(capsule,r,"unrelated-fixed-owner",valid,fdt(fixed_reserved=(0x50000000,4096)),expect_uefi=True))
        cases.append(exercise(capsule,r,"missing-owner-cell-layout",valid,fdt(fixed_reserved=(PAGE,4096),owner_cells=None),expect_reserve=False))
        cases.append(exercise(capsule,r,"unsupported-owner-cell-layout",valid,fdt(fixed_reserved=(PAGE,4096),owner_cells=(1,2)),expect_reserve=False))
        cases.append(exercise(capsule,r,"unrelated-memreserve",valid,fdt([(0x50000000,4096)]),expect_uefi=True))
        cases.append(exercise(capsule,r,"overflowing-memreserve",valid,fdt([((1<<64)-8,32)]),expect_reserve=False))
        bad=bytearray(fdt());pos=struct.unpack_from(">I",bad,8)[0];struct.pack_into(">I",bad,pos,0xdead)
        cases.append(exercise(capsule,r,"invalid-structure-token",valid,bytes(bad),expect_reserve=False))
        bad=bytearray(fdt());struct.pack_into(">I",bad,0,0)
        cases.append(exercise(capsule,r,"invalid-dtb-magic",valid,bytes(bad),expect_reserve=False))
        bad=bytearray(fdt());struct.pack_into(">I",bad,16,41)
        cases.append(exercise(capsule,r,"unaligned-reservation-table",valid,bytes(bad),expect_reserve=False))
        bad=bytearray(fdt());bad[40:56]=struct.pack(">QQ",1,1)
        cases.append(exercise(capsule,r,"missing-reservation-terminator",valid,bytes(bad),expect_reserve=False))
        cases.append(exercise(capsule,r,"no-fdt-growth-capacity",valid,fdt(total=0x200000),expect_reserve=False))
        cases.append(exercise(capsule,r,"alternate-placement-default",valid,placement=BASE+0x200000,expect_reserve=False))
        for payload in (bytes(64),fixture_image(4096,4080)):
            try:build.image(payload)
            except ValueError:pass
            else:raise AssertionError("Invalid Image accepted")
    if a.artifacts:
        r=json.loads((a.artifacts/"slotless-prototype.json").read_text())
        capsule=(a.artifacts/"slotless-prototype.Image").read_bytes()
        assert build.sha(capsule)==r["image_sha256"]
        assert all(build.sha((PKG/"Tools"/name).read_bytes())==digest for name,digest in r["source_sha256"].items())
        cases.append(exercise(capsule,r,"real-stock-default"))
        cases.append(exercise(capsule,r,"real-uefi-request",struct.pack("<4Q",*r["request_words"]),expect_uefi=True))
    result={"status":"passed","cases":cases,"case_count":len(cases),
            "scope":"compiled selector and memory copies; target kernels not executed",
            "reset_retention_verified":False,"Android_request_writer_verified":False,
            "physical_boot_verified":False,"device_access":False,
            "test_source_sha256":build.sha(Path(__file__).read_bytes())}
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:result[k] for k in ("status","case_count","physical_boot_verified","device_access")}))
if __name__=="__main__":main()
