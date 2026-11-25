#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the actual LK-mode selector and complete payload copies in Unicorn."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("selector_tests", HERE/"SlotlessBootTest.py")
old = importlib.util.module_from_spec(spec)
spec.loader.exec_module(old)
build = old.build


def node(name):
    b = struct.pack(">I",1)+name.encode()+b"\0"
    return b+bytes(-len(b)%4)


def property_(value, offset=0):
    b = struct.pack(">III",3,len(value),offset)+value
    return b+bytes(-len(b)%4)


def fdt(mode=0, *, tag=None, duplicates=False, chosen="chosen", no_tag=False,
        duplicate_chosen=False, invalid_tail=False, display=False):
    if tag is None:
        tag = struct.pack("<4I",16,0x41000802,mode,2)

    strings = bytearray(b"atag,boot\0")

    def prop(name, value):
        offset = len(strings)
        strings.extend(name.encode()+b"\0")
        return property_(value, offset)

    def cell(name, value):
        return prop(name, struct.pack(">I", value))

    child = node(chosen)
    if not no_tag:
        child += property_(tag)
        if duplicates:
            child += property_(tag)

    if display:
        for suffix,value in (("fb_base_h",0),("fb_base_l",FRAMEBUFFER),("vramSize",0x1C00000),
                             ("islcmfound",1),("islcm_inited",0)):
            child += cell("atag,videolfb-"+suffix,value)
    child += struct.pack(">I",2)
    structure = node("")
    if display:
        structure+=cell("#address-cells",2)+cell("#size-cells",2)
    structure+=child
    if display:
        structure += node("memory@80000000")+prop("device_type",b"memory\0")
        structure += prop("reg",struct.pack(">QQ",0x80000000,0x80000000))+struct.pack(">I",2)
        for base,compatible in DISPLAY_RESOURCES:
            structure += node(f"test@{base:x}")+prop("compatible",compatible.encode()+b"\0")
            structure += prop("reg",struct.pack(">QQ",base,0x1000))+struct.pack(">I",2)
    if duplicate_chosen:
        structure += child
    structure += struct.pack(">II",2,0xdead if invalid_tail else 9)
    start = 56
    at = start+len(structure)
    total = at+len(strings)
    return (struct.pack(">10I",0xd00dfeed,total,start,at,40,17,16,0,len(strings),len(structure))
            +bytes(16)+structure+strings)



FRAMEBUFFER = 0xfe400000
WIDTH, HEIGHT = 2240, 1400
PITCH = WIDTH * 4
DISPLAY_RESOURCES = (
    (0x10006000,"mediatek,mt6789-scpsys"),
    (0x10001000,"mediatek,mt6789-infracfg_ao"),
    (0x14000000,"mediatek,mt6789-mmsys"),
    (0x14003000,"mediatek,smi_larb0"),
    (0x14004000,"mediatek,smi_larb1"),
    (0x14005000,"mediatek,disp_ovl0"),
    (0x14006000,"mediatek,disp_ovl0_2l"),
    (0x14007000,"mediatek,disp_rdma0"),
    (0x14013000,"mediatek,dsi0"),
)


def menu_fixture(report, *, choices, mode=4, power=True, ready=True, draws=True):
    """Replay the linear LK plane and advancing timer with a physical key sequence."""
    def setup(cpu):
        frequency = 13000000
        cpu.cpr_write(3,3,14,0,0,frequency)
        counter = 0
        timer_pc = old.BASE + report["symbols"]["MenuCounter"]
        samples = iter(choices)
        last = choices[-1]
        polls = 0
        kp = 0x10010000

        def tick(uc, address, size, _):
            nonlocal counter
            counter += frequency // 100
            uc.reg_write(old.UC_ARM64_REG_X0,counter)
            uc.reg_write(old.UC_ARM64_REG_PC,uc.reg_read(old.UC_ARM64_REG_X30))

        def keys(uc, access, address, size, value, _):
            nonlocal last, polls
            assert access==old.UC_MEM_READ and size==2 and address-kp in (4,8,0x24)
            if address==kp+4:
                polls += 1
                last = next(samples,last)
                uc.mem_write(kp+4,struct.pack("<H",last))

        cpu.mem_map(kp,0x1000)
        cpu.mem_write(kp+0x24,struct.pack("<H",int(ready)))
        cpu.mem_write(kp+0x08,b"\xff\xff")
        cpu.hook_add(old.UC_HOOK_CODE,tick,begin=timer_pc,end=timer_pc)
        cpu.hook_add(old.UC_HOOK_MEM_READ|old.UC_HOOK_MEM_WRITE,keys,begin=kp,end=kp+0xfff)
        cpu.mem_map(0x10001000,0x1000)
        cpu.mem_map(0x10006000,0x1000)
        cpu.mem_map(0x14000000,0x14000)
        cpu.mem_map(FRAMEBUFFER,(PITCH*HEIGHT+4095)&~4095)
        roi=(HEIGHT<<16)|WIDTH
        registers={
            0x1000616c:0x200000 if power else 0, 0x10006170:0x200000, 0x10006354:0xd,
            0x14000f18:1,0x14007010:1,0x14007014:WIDTH,0x14007018:HEIGHT,
            0x1400500c:0x400001,0x14005020:roi,0x1400502c:2,
            0x14005050:0x21ff,0x14005058:roi,0x14005060:0x100000,0x14005064:PITCH,
            0x140050e0:0x5a0001,0x14005f60:FRAMEBUFFER,
            0x14013000:1,0x14013014:1,0x14013018:0x3c,0x1401301c:0x30000|WIDTH*3,
            0x1401302c:HEIGHT,0x14013038:roi,
        }
        for address,value in registers.items():
            cpu.mem_write(address,struct.pack("<I",value))

        def no_mmio_write(uc, access, address, size, value, _):
            raise AssertionError(f"menu wrote MMIO {address:#x}")

        for begin,end in ((0x10001000,0x10006fff),(0x14000000,0x14013fff)):
            cpu.hook_add(old.UC_HOOK_MEM_WRITE,no_mmio_write,begin=begin,end=end)

        def check():
            frame = cpu.mem_read(FRAMEBUFFER,PITCH*HEIGHT)
            white = frame.count(b"\xff\xff\xff\xff")
            top = ((HEIGHT-312)//2*WIDTH+(WIDTH-960)//2)*4
            assert bool(white)==draws, (mode,draws,white,counter,polls)
            if draws:
                assert frame[top:top+4]==struct.pack("<I",0xff182230)
                assert not any(frame[:top])
                assert not any(frame[top+311*PITCH+960*4:])
            else:
                assert not any(frame)
            return {"menu_drawn":bool(white),"key_samples":polls,
                    "elapsed_ms":counter*1000//frequency,"hardware_registers_written":False}

        return check
    return setup

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--artifacts", type=Path)
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args()
    cases = []
    with tempfile.TemporaryDirectory(prefix="t70-lk-mode-") as temp:
        capsule, report = build.compile_capsule(old.fixture_image(), old.fixture_image(4096,4096,0xa5),
                                                Path(temp), lk_boot_mode=True)
        before = {p.name:build.sha(p.read_bytes()) for p in Path(temp).iterdir() if p.is_file()}
        try:
            build.compile_capsule(old.fixture_image(),old.fixture_image(4096,4096,0xa5),
                                  Path(temp),lk_boot_mode=True)
        except ValueError:
            pass
        else:
            raise AssertionError("Existing selector output was overwritten")
        assert before == {p.name:build.sha(p.read_bytes()) for p in Path(temp).iterdir() if p.is_file()}
        def exercise(name, dt=None, expected=False, parked=False, **kwargs):
            kwargs.setdefault("keypad",(1,0xffff))
            cases.append(old.exercise(capsule,report,name,dt=dt,expect_uefi=expected,
                                      expect_reserve=False,expect_park=parked,**kwargs))
        exercise("normal-is-exact-stock-Android", fdt(0), expected_keypad_reads=2)
        exercise("normal-both-volume-keys-keep-Android", fdt(0),
                 keypad=(1,0xfffc), expected_keypad_reads=2)
        exercise("normal-volume-down-keeps-Android", fdt(0),
                 keypad=(1,0xfffd), expected_keypad_reads=2)
        exercise("normal-disabled-keypad-keeps-Android", fdt(0),
                 keypad=(0,0xfffe), expected_keypad_reads=1)
        exercise("normal-volume-up-at-handoff-opens-EDK2", fdt(0), True,
                 keypad=(1,0xfffe), expected_keypad_reads=2)
        exercise("fastboot-continue-is-UEFI", fdt(99), True, keypad=(0,0))
        exercise("EL2-fastboot-continue-is-UEFI", fdt(99), True, el=2, keypad=(0,0))
        exercise("EL2-normal-is-Android", fdt(0), el=2, expected_keypad_reads=2)
        for mode in (3,7,8,9):
            exercise("Android-reboot-alarm-charging-mode-"+str(mode), fdt(mode))
        for mode in (1,5,6,255):
            exercise("diagnostic-or-unknown-mode-stops-"+str(mode), fdt(mode),parked=True)
        def menu(name, mode, keys, expected=False, **kwargs):
            exercise(name, fdt(mode,display=True), expected, keypad=None,
                     setup=menu_fixture(report,choices=keys,mode=mode,**kwargs))
        exercise("factory-volume-up-released-opens-EDK2-without-display", fdt(4), True,
                 expected_keypad_reads=2)
        exercise("factory-volume-down-opens-EDK2", fdt(4), True,
                 keypad=(1,0xfffd), expected_keypad_reads=2)
        exercise("factory-held-volume-up-keeps-stock", fdt(4),
                 keypad=(1,0xfffe), expected_keypad_reads=2)
        exercise("factory-both-volume-keys-keep-stock", fdt(4),
                 keypad=(1,0xfffc), expected_keypad_reads=2)
        exercise("factory-invalid-keypad-status-keeps-stock", fdt(4),
                 keypad=(1,0x7fff), expected_keypad_reads=2)
        exercise("factory-disabled-keypad-keeps-stock", fdt(4),
                 keypad=(0,0xffff), expected_keypad_reads=1)
        menu("recovery-no-key-starts-stock-immediately",2,[0xffff],draws=False)
        menu("physical-recovery-release-and-minus-opens-EDK2",2,
             [0xfffc]*5+[0xffff]*5+[0xfffd],True)
        exercise("no-LK-boot-tag", fdt(no_tag=True),parked=True)
        exercise("tag-outside-chosen", fdt(2,chosen="other"),parked=True)
        exercise("duplicate-boot-tag", fdt(2,duplicates=True),parked=True)
        exercise("duplicate-chosen-node", fdt(2,duplicate_chosen=True),parked=True)
        exercise("invalid-tail-after-valid-recovery", fdt(2,invalid_tail=True),parked=True)
        for i,value in ((0,12),(1,0),(2,256),(3,0)):
            words=[16,0x41000802,2,2]
            words[i]=value
            exercise("invalid-tag-word-"+str(i), fdt(tag=struct.pack("<4I",*words)),parked=True)
        exercise("big-endian-tag-rejected", fdt(tag=struct.pack(">4I",16,0x41000802,2,2)),parked=True)
        exercise("short-tag-rejected", fdt(tag=struct.pack("<3I",16,0x41000802,2)),parked=True)
        exercise("large-tag-rejected", fdt(tag=struct.pack("<5I",16,0x41000802,2,2,0)),parked=True)
        for offset,value,name in [(0,0,"bad-magic"),(4,0x200001,"oversized-total"),
                                   (8,57,"unaligned-structure"),(12,0,"bad-strings"),
                                   (20,18,"unknown-version"),(36,0xffffffff,"oversized-structure")]:
            bad=bytearray(fdt(2))
            struct.pack_into(">I",bad,offset,value)
            exercise(name,bytes(bad),parked=True)
        exercise("alternate-placement-stops-before-Android", fdt(2), placement=old.BASE+0x200000,parked=True)
    if a.artifacts:
        report = json.loads((a.artifacts/"lk-boot-select.json").read_text())
        capsule = (a.artifacts/"lk-boot-select.Image").read_bytes()
        assert build.sha(capsule) == report["image_sha256"]
        symbols = report["symbols"]
        embedded_stock = capsule[symbols["_stock"]:symbols["_stock_end"]]
        assert build.sha(embedded_stock[:report["stock_raw_bytes"]]) == report["stock_kernel_sha256"]
        assert not any(embedded_stock[report["stock_raw_bytes"]:])
        assert build.sha(capsule[symbols["_uefi"]:symbols["_uefi_end"]]) == report["uefi_sha256"]
        for name,digest in report["source_sha256"].items():
            assert build.sha((old.PKG/"Tools"/name).read_bytes()) == digest, name
        cases.append(old.exercise(capsule,report,"full-stock-Android-copy",dt=fdt(0),
                                  expect_reserve=False,keypad=(1,0xffff),expected_keypad_reads=2))
        cases.append(old.exercise(capsule,report,"full-EDK2-copy-with-held-volume-up",dt=fdt(0),
                                  expect_reserve=False,expect_uefi=True,
                                  keypad=(1,0xfffe),expected_keypad_reads=2))
        cases.append(old.exercise(capsule,report,"full-stock-Recovery-copy",dt=fdt(2,display=True),
            expect_reserve=False,setup=menu_fixture(report,choices=[0xffff],mode=2,draws=False)))
        cases.append(old.exercise(capsule,report,"full-stock-factory-kernel-copy",
            dt=fdt(4),expect_reserve=False,keypad=(1,0xfffe),expected_keypad_reads=2))
        cases.append(old.exercise(capsule,report,"full-EDK2-copy-from-factory-mode",
            dt=fdt(4),expect_reserve=False,expect_uefi=True,
            keypad=(1,0xffff),expected_keypad_reads=2))
        cases.append(old.exercise(capsule,report,"full-UEFI-copy-after-fastboot-continue",
                                  dt=fdt(99),expect_uefi=True,expect_reserve=False,keypad=(0,0)))
    result={"schema":1,"all_passed":True,"compiled_selector_executed":True,
            "device_access":False,"physical_boot_verified":False,
            "target_kernels_executed":False,"fdt_or_retained_request_modified":False,
            "tested_artifact_sha256":build.sha(capsule) if a.artifacts else None,
            "stock_kernel_copy_matches_original_sha256":bool(a.artifacts),
            "keypad_mmio_reads_emulated":True,"existing_output_preserved":True,"count":len(cases),"cases":cases,
            "test_source_sha256":build.sha(Path(__file__).read_bytes())}
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:result[k] for k in ("all_passed","count","physical_boot_verified","device_access")}))


if __name__ == "__main__":
    main()
