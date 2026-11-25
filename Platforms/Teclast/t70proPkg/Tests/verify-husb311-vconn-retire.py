#!/usr/bin/env python3
"""Execute shipped HUSB311 VCONN disable instructions without device access."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
    UC_ARM64_REG_SP, UC_ARM64_REG_SP_EL0, UC_ARM64_REG_LR, UC_ARM64_REG_PC,
)

spec = importlib.util.spec_from_file_location(
    "stock_power_binary", Path(__file__).with_name("verify-stock-power-binary.py"))
stock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stock)
raw = stock.HUSB311_BINARY.read_bytes()
digest = hashlib.sha256(raw).hexdigest()
assert digest == stock.HUSB311_SHA256, "Shipped module identity changed"
elf = stock.parse_arm64_elf(raw, "HUSB311 VCONN retirement")
function = elf["symbols"]["husb311_set_vconn"]
assert function[1:] == (14, 0x2150, 0x134), "Function extent changed"
stubs = ["tcpc_get_dev_data", "rt_regmap_block_write", "rt_regmap_block_read",
         "_dev_err", "__stack_chk_fail"]
uc, bases, call, stop, ranges, relocations = stock.make_module_emulator(
    elf, [function], stubs, 0x10000000)
entry = bases[function[1]] + function[2]
end = entry + function[3]
bus, stack, canary = 0x40000000, 0x30000000, 0x20000000
uc.mem_map(bus, 0x10000)
uc.mem_map(stack, 0x10000)
uc.mem_map(canary, 0x1000)
tcpc, data, device, chip, regmap = [bus + x for x in (0x1000, 0x6000, 0x7000, 0x8000, 0x9000)]
uc.mem_write(data, struct.pack("<Q", device))
uc.mem_write(device + 0xB8, struct.pack("<Q", chip))
uc.mem_write(chip + 0x10, struct.pack("<Q", regmap))
uc.mem_write(canary + 0x710, struct.pack("<Q", 0xA5A5C3C35A5A3C3C))
active = {}

def ret(machine, value=0):
    machine.reg_write(UC_ARM64_REG_X0, value & 0xffffffffffffffff)
    machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))

def instruction(machine, address, size, context):
    if address == stop:
        machine.emu_stop()
    elif address == call["tcpc_get_dev_data"]:
        assert machine.reg_read(UC_ARM64_REG_X0) == tcpc
        ret(machine, data)
    elif address in (call["rt_regmap_block_read"], call["rt_regmap_block_write"]):
        reg = machine.reg_read(UC_ARM64_REG_X1)
        length = machine.reg_read(UC_ARM64_REG_X2)
        pointer = machine.reg_read(UC_ARM64_REG_X3)
        assert machine.reg_read(UC_ARM64_REG_X0) == regmap and length == 1
        assert stack <= pointer < stack + 0x10000
        write = address == call["rt_regmap_block_write"]
        assert reg in ((0x1C, 0x9B) if write else (0x1C,))
        active["operations"] += 1
        failed = active["operations"] == active["failure"]
        value = machine.mem_read(pointer, 1)[0] if write else active["control"]
        active["trace"].append(dict(operation="write" if write else "read",
                                    register=hex(reg), value=hex(value), failed=failed))
        if not write and not failed:
            machine.mem_write(pointer, bytes((value,)))
        ret(machine, -5 if failed else 0)
    elif address == call["_dev_err"]:
        active["error_logs"] += 1
        ret(machine)
    elif address == call["__stack_chk_fail"]:
        raise AssertionError("Original function stack canary failed")
    else:
        assert entry <= address < end, "Unexpected instruction address"

uc.hook_add(UC_HOOK_CODE, instruction)
cases = []
for control, failure in [(value, 0) for value in range(256)] + [(3, value) for value in range(1, 4)]:
    active.clear()
    active.update(control=control, failure=failure, operations=0, trace=[], error_logs=0)
    uc.mem_write(stack, bytes(0x10000))
    uc.reg_write(UC_ARM64_REG_SP_EL0, canary)
    uc.reg_write(UC_ARM64_REG_SP, stack + 0xFF00)
    uc.reg_write(UC_ARM64_REG_X0, tcpc)
    uc.reg_write(UC_ARM64_REG_X1, 0)
    uc.reg_write(UC_ARM64_REG_LR, stop)
    uc.emu_start(entry, stop, timeout=100000, count=1500)
    assert uc.reg_read(UC_ARM64_REG_PC) == stop and uc.reg_read(UC_ARM64_REG_SP) == stack + 0xFF00
    expected = [("read", "0x1c", hex(control)),
                ("write", "0x1c", hex(control & 0xfe)), ("write", "0x9b", "0x38")]
    actual = [(v["operation"], v["register"], v["value"]) for v in active["trace"]]
    assert actual == (expected[:failure] if failure else expected)
    assert (uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff) == (0xfffffffb if failure else 0)
    assert active["error_logs"] == bool(failure)
    if failure or control in (0, 1, 3, 0x81, 0xff):
        cases.append(dict(active))
assert hashlib.sha256(stock.HUSB311_BINARY.read_bytes()).hexdigest() == digest
report = {
    "status": "passed",
    "scope": "Original shipped ARM64 instructions under Unicorn; regmap/kernel calls modeled; no device access",
    "module_sha256": digest, "function": function[0], "function_offset": hex(function[2]),
    "function_bytes": function[3], "function_instruction_patches": 0,
    "function_sha256": hashlib.sha256(elf["section_bytes"](function[1])[function[2]:function[2]+function[3]]).hexdigest(),
    "relocations_applied": relocations,
    "all_original_control_values_passed": 256, "injected_bus_failure_cases": 3,
    "disable_write_sequence": ["1c=(original&fe)", "9b=38"],
    "original_module_unchanged": True, "physical_execution": False,
    "selected_cases": cases, "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
}
out = stock.ANALYSIS_DIR / "husb311-vconn-retire-arm64.json"
out.write_text(json.dumps(report, indent=2) + "\n")
print("Original HUSB311 VCONN disable: 256 control values and 3 bus failures PASS; 0 instruction patches")
