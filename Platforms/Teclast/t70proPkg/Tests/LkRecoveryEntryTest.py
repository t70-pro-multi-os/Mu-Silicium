#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the original LK A/B boot-kernel decision without device access."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

sys.dont_write_bytecode = True
import unicorn
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_READ, UC_PROT_EXEC
from unicorn.arm64_const import *

LK_SHA = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"
CODE, STACK, STOP = 0x10000000, 0x60000000, 0x70000000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def exercise(lk, control, mode, expected, name, read_failure=False):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(CODE, 0x100000, UC_PROT_READ | UC_PROT_EXEC)
    cpu.mem_write(CODE, lk)
    cpu.mem_write(CODE + 0xc9918, bytes([mode]))
    cpu.mem_map(STACK, 0x10000)
    cpu.mem_map(STOP, 4096, UC_PROT_READ | UC_PROT_EXEC)
    cpu.reg_write(UC_ARM64_REG_SP, STACK + 0x10000 - 16)
    cpu.reg_write(UC_ARM64_REG_X30, STOP)
    reads, selected = [], []
    def hook(uc, address, size, user):
        if address == CODE + 0x1a8e8:
            assert uc.reg_read(UC_ARM64_REG_X1) == 0, "Unexpected boot-control write"
            reads.append("boot_control_read_only")
            if not read_failure:
                uc.mem_write(uc.reg_read(UC_ARM64_REG_X0), control)
            uc.reg_write(UC_ARM64_REG_X0, 0xffffffff if read_failure else 0)
        elif address == CODE + 0x5af38:
            selected.append(uc.reg_read(UC_ARM64_REG_X0))
            uc.emu_stop()
            return
        else:
            uc.reg_write(UC_ARM64_REG_X0, 0)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_X30))
    for offset in (0x1a8e8, 0x5af38, 0x1106c, 0x6f520):
        cpu.hook_add(UC_HOOK_CODE, hook, begin=CODE + offset, end=CODE + offset)
    cpu.emu_start(CODE + 0x10380, STOP, count=1000000)
    assert reads == ["boot_control_read_only"]
    assert selected == [expected], (name, selected)
    return {"case": name, "lk_boot_mode": mode, "load_vfy_boot_argument": expected,
            "image_path": "boot_with_AB_suffix" if expected == 0 else "separate_recovery",
            "boot_control_write_executed": False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("lk", "backup-user", "evidence", "report"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--misc-offset", type=int, required=True)
    a = p.parse_args()
    lk = a.lk.read_bytes()
    assert sha(lk) == LK_SHA
    # Read only this non-secret 32-byte A/B record, never adjacent device-specific data.
    with a.backup_user.open("rb") as stream:
        stream.seek(a.misc_offset + 2048)
        control = stream.read(32)
    assert len(control) == 32
    assert struct.unpack_from("<I", control, 4)[0] == 0x42414342
    assert control[8] == 1 and (control[9] & 7) == 2
    assert zlib.crc32(control[:28]) == struct.unpack_from("<I", control, 28)[0]
    assert (control[12] & 15) > 0 and (control[12] & 15) >= (control[14] & 15)
    b_first = bytearray(control)
    b_first[12] = (b_first[12] & 0xf0) | 1
    b_first[14] = (b_first[14] & 0xf0) | 15
    struct.pack_into("<I", b_first, 28, zlib.crc32(b_first[:28]))
    cases = [
        exercise(lk, control, 0, 0, "normal_valid_A_suffix_uses_boot"),
        exercise(lk, control, 99, 0, "fastboot_continue_valid_A_suffix_uses_boot"),
        exercise(lk, control, 2, 0, "recovery_valid_A_suffix_uses_boot"),
        exercise(lk, control, 4, 0, "factory_valid_A_suffix_uses_boot"),
        exercise(lk, bytes(b_first), 2, 0, "recovery_valid_B_suffix_uses_boot"),
        exercise(lk, control, 2, 1, "recovery_read_failure_empty_suffix_uses_separate_recovery",
                 read_failure=True),
    ]
    result = {"schema": 1, "all_passed": True, "device_access": False,
              "physical_execution_verified": False, "selection_mode": 2,
              "selection_mode_uses_boot_kernel": True, "requires_valid_AB_boot_control": True,
              "original_LK_decision_and_suffix_getter_executed": True,
              "actual_storage_and_kernel_loader_executed": False,
              "stubbed_functions": ["boot_control_storage_read", "load_vfy_boot_stop", "clock", "printf"],
              "lk_sha256": LK_SHA, "count": len(cases), "cases": cases,
              "unicorn_version": unicorn.__version__,
              "evidence_report_sha256": sha(a.evidence.read_bytes()),
              "test_source_sha256": sha(Path(__file__).read_bytes())}
    a.report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
