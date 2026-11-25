#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the real kexec entry instructions in EL1/EL2, without a device."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
    UC_ARM64_REG_X3, UC_ARM64_REG_SP, UC_ARM64_REG_PSTATE, UC_ARM64_REG_PC)

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("t70_handoff_builder", HERE / "build-handoff.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def execute(stub, entry, el, incoming):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    cpu.mem_map(build.IMAGE_BASE, 0x402000)
    cpu.mem_write(entry, stub)
    for reg in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3):
        cpu.reg_write(reg, incoming)
    state = 0x3c0 | (el << 2) | 1
    cpu.reg_write(UC_ARM64_REG_PSTATE, state)
    cpu.reg_write(UC_ARM64_REG_SP, 0xdead0000)  # No stack is mapped.
    visited, writes = [], []
    def code(uc, pc, size, _):
        visited.append(pc)
        if pc == build.IMAGE_BASE:
            uc.emu_stop()  # Never execute the target OS/UEFI.
    def write(uc, access, address, size, value, _):
        writes.append(address)
    cpu.hook_add(UC_HOOK_CODE, code)
    cpu.hook_add(UC_HOOK_MEM_WRITE, write)
    cpu.emu_start(entry, build.IMAGE_BASE + 4, count=16)
    assert cpu.reg_read(UC_ARM64_REG_PC) == build.IMAGE_BASE
    assert cpu.reg_read(UC_ARM64_REG_X0) == build.FDT_BASE
    assert all(cpu.reg_read(r) == 0 for r in (UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3))
    assert cpu.reg_read(UC_ARM64_REG_SP) == 0xdead0000
    assert cpu.reg_read(UC_ARM64_REG_PSTATE) == state
    assert len(visited) == 7 and not writes
    return {"el": el, "incoming_argument": hex(incoming), "entry": hex(entry),
            "fdt": hex(build.FDT_BASE), "no_memory_writes": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    meta = json.loads((args.artifacts / "handoff.json").read_text())
    image = (args.artifacts / "uefi.Image").read_bytes()
    stub = (args.artifacts / "entry.bin").read_bytes()
    assert build.layout(image) == meta["entry_address"]
    assert all(build.sha((args.artifacts / s["name"]).read_bytes()) == s["sha256"]
               for s in meta["segments"][:2])
    assert all(build.sha((HERE / n).read_bytes()) == digest
               for n, digest in meta["source_sha256"].items())
    cases = [execute(stub, meta["entry_address"], el, incoming)
             for el in (1, 2) for incoming in (0, 0x1122334455667788)]
    rejected = []
    malformed = [b"", b"ANDROID!" + bytes(64), image[:64], image[:-4096]]
    for field, value in ((8, 4096), (16, len(image) - 4096), (24, 0)):
        bad = bytearray(image)
        struct.pack_into("<Q", bad, field, value)
        malformed.append(bytes(bad))
    for i, bad in enumerate(malformed):
        try:
            build.layout(bad)
        except ValueError:
            rejected.append(i)
        else:
            raise AssertionError("Malformed input accepted")
    with tempfile.TemporaryDirectory(prefix="t70-handoff-existing-") as tmp:
        output = Path(tmp)
        (output / "keep").write_bytes(b"existing")
        try:
            build.build(image, output)
        except ValueError:
            pass
        else:
            raise AssertionError("Nonempty output accepted")
        assert (output / "keep").read_bytes() == b"existing"
    report = {"status": "passed", "cases": cases, "invalid_inputs_rejected": len(rejected),
              "existing_output_preserved": True, "target_os_fixed": False,
              "device_access": False, "hardware_handoff_verified": False,
              "scope": "compiled entry register/branch behavior; no kernel syscall or target firmware execution",
              "test_source_sha256": build.sha(Path(__file__).read_bytes())}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in
                      ("status", "invalid_inputs_rejected", "hardware_handoff_verified")}))


if __name__ == "__main__":
    main()
