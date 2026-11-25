# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the WMT mode switch against the shipped MT6789 ARM64 driver."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
    UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X8,
)


def stock_sequence(stock, sha, module, isolated_module):
    name = "_wmt_soc_mode_ctrl"
    raw, sections, symbols, ranges, bases, uc, stub, external, address, _ = (
        isolated_module(stock, (name,)))
    assert hashlib.sha256(raw).hexdigest() == sha
    _, _, _, tables, section_data = module(stock)
    text_index, low, high = ranges[0]
    start, end = bases[text_index] + low, bases[text_index] + high
    scripts = {
        address(symbols["wmt_core_init_script"]): "script",
        address(symbols["wmt_core_ctrl"]): "control",
        address(symbols["osal_sleep_ms"]): "sleep",
        address(symbols["osal_warn_print"]): "log",
        address(symbols["osal_err_print"]): "log",
    }
    scripts.update({pointer: key for key, pointer in external.items()})
    stop, stack = stub + 0xf000, 0x40000000
    uc.mem_map(stack, 0x10000)
    tape = []
    # Script descriptors use absolute command and response pointers in .data.
    # Apply only the six relocations required by this selected function.
    data_index = symbols["wmt_ic_ops_soc"][1]
    index, = (i for i, s in enumerate(sections) if s[1] == 4 and s[7] == data_index)
    offsets = {0x15a0, 0x15b0, 0x15d8, 0x15e8, 0x1610, 0x1620}
    for off, info, add in struct.iter_unpack("<QQq", section_data(index)):
        if off in offsets:
            assert (info & 0xffffffff) == 257
            target = address(tables[sections[index][6]][info >> 32]) + add
            uc.mem_write(bases[data_index] + off, struct.pack("<Q", target))
            offsets.remove(off)
    assert not offsets

    def hook(u, pc, _size, _context):
        service = scripts.get(pc)
        if service is None:
            assert start <= pc < end, hex(pc)
            if struct.unpack("<I", u.mem_read(pc, 4))[0] == 0xd5384108:
                u.reg_write(UC_ARM64_REG_X8, stack)
                u.reg_write(UC_ARM64_REG_PC, pc+4)
            return
        a, b, c = (u.reg_read(reg) for reg in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        if service == "script":
            assert b == 1
            command, tx_bytes, response, rx_bytes = struct.unpack("<4Q", u.mem_read(a, 32))
            assert 4 <= tx_bytes <= 10 and 4 <= rx_bytes <= 10
            tape.append(("wmt", u.mem_read(command, tx_bytes).hex(), u.mem_read(response, rx_bytes).hex()))
        elif service == "control":
            assert a == 5
            parameter = struct.unpack("<Q", u.mem_read(b, 8))[0]
            value = struct.unpack("<Q", u.mem_read(c, 8))[0]
            tape.append(("stp", parameter, value))
        elif service == "sleep":
            tape.append(("sleep_ms", a))
        else:
            assert service == "log", service
        u.reg_write(UC_ARM64_REG_X0, 0)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(UC_ARM64_REG_SP, stack+0xfff0)
    uc.reg_write(UC_ARM64_REG_LR, stop)
    uc.emu_start(start, stop, count=3000)
    assert uc.reg_read(UC_ARM64_REG_PC) == stop and uc.reg_read(UC_ARM64_REG_X0) == 0
    assert tape == [
        ("wmt", "0104010004", "02040600000411000000"),
        ("wmt", "0104050003db0e6801", "020402000003"),
        ("stp", 2, 4), ("stp", 0, 1), ("sleep_ms", 10),
        ("wmt", "0104010004", "020406000004db0e6801"),
    ], tape
    return {"stock_sha256": sha, "function_offset": low, "sequence": tape}


def verify(root, out, stock, sha, module, isolate, arm64_test):
    assert not out.exists(), "Use a new output directory"
    silicon = root / "Silicon/MediaTek/MediaTekPkg"
    include = silicon / "Include"
    sources = [silicon / "Library/MtkStpCoreLib" / (name+".c")
               for name in ("MtkStpCore", "MtkStpFull", "MtkWmtMode")]
    fixture = Path(__file__).with_name("MtkWmtModeTest.c")
    inputs = [*sources, fixture, Path(__file__), Path(__file__).with_name("verify-stp-binary.py")]
    inputs += [include / "Library" / (source.stem+".h") for source in sources]

    def hashes():
        return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}

    before = hashes()
    evidence = stock_sequence(stock, sha, module, isolate)
    flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", include]
    with tempfile.TemporaryDirectory(prefix="t70-wmt-mode-") as directory:
        stage = Path(directory)
        native, image = stage / "native", stage / "WmtModeTest.exe"
        subprocess.run([os.environ.get("CC", "cc"), *flags, "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-DSTP_TEST_NATIVE", *sources, fixture, "-o", native], check=True)
        subprocess.run([native], check=True)
        objects = []
        for source in [*sources, fixture]:
            obj = stage / (source.stem+".obj")
            subprocess.run(["clang", *flags, "--target=arm64-pc-windows-msvc", "-O2",
                            "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                            "-fstack-usage", "-c", source, "-o", obj], check=True)
            objects.append(obj)
        for entry in (stage/"MtkWmtMode.su").read_text().splitlines():
            _, stack, storage = entry.rsplit("\t", 2)
            assert storage == "static" and int(stack) <= 256, entry
        subprocess.run(["lld-link", "/nologo", "/machine:arm64", "/entry:TestMain",
                        "/subsystem:native", "/nodefaultlib", "/noimplib",
                        "/export:T70WmtModeAssertions,DATA", f"/out:{image}", *objects], check=True)
        arm = arm64_test(image, "T70WmtModeAssertions", 1400)
        assert hashes() == before
        out.mkdir(parents=True, exist_ok=False)
        shutil.copyfile(image, out/image.name)
        (out/"verification.json").write_text(json.dumps(
            {"original": evidence, "arm64": arm, "inputs": before,
             "pe_sha256": hashlib.sha256(image.read_bytes()).hexdigest()}, indent=2)+"\n")
        print(f"WMT mode switch matched the ARM64 stock scripts and passed {arm['assertions']} ARM64 checks: {out}")
