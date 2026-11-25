#!/usr/bin/env python3
"""Execute the shipped HUSB311 sensing-wake function, without chip access."""
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

PKG = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("stock_power_binary", Path(__file__).with_name("verify-stock-power-binary.py"))
stock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stock)
raw = stock.HUSB311_BINARY.read_bytes()
digest = hashlib.sha256(raw).hexdigest()
assert digest == stock.HUSB311_SHA256, "Shipped module identity changed"
elf = stock.parse_arm64_elf(raw, "HUSB311 sensing wake")
function = elf["symbols"]["husb311_set_low_power_mode"]
assert function[1:] == (14, 0x24C0, 0x1D8), "Function extent changed"
stubs = ["tcpc_get_dev_data", "rt_regmap_block_write", "rt_regmap_block_read", "_dev_err", "_printk", "__stack_chk_fail"]
uc, bases, call, stop, ranges, relocations = stock.make_module_emulator(elf, [function], stubs, 0x10000000)
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
uc.mem_write(data + 0x32, b"\x01")
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
    elif address in (call["rt_regmap_block_write"], call["rt_regmap_block_read"]):
        reg = machine.reg_read(UC_ARM64_REG_X1)
        length = machine.reg_read(UC_ARM64_REG_X2)
        pointer = machine.reg_read(UC_ARM64_REG_X3)
        assert machine.reg_read(UC_ARM64_REG_X0) == regmap and length == 1
        assert stack <= pointer < stack + 0x10000
        write = address == call["rt_regmap_block_write"]
        assert reg in ((0x9B, 0x99, 0x90) if write else (0x99,))
        active["operations"] += 1
        failed = active["operations"] == active["failure"]
        value = machine.mem_read(pointer, 1)[0] if write else active["mask"]
        active["trace"].append({"operation": "write" if write else "read", "register": hex(reg), "value": hex(value), "failed": failed})
        if not write and not failed:
            machine.mem_write(pointer, bytes((value,)))
        ret(machine, -5 if failed else 0)
    elif address == call["_dev_err"]:
        active["error_logs"] += 1
        ret(machine)
    elif address == call["_printk"]:
        active["print_logs"] += 1
        ret(machine)
    elif address == call["__stack_chk_fail"]:
        raise AssertionError("Original function stack canary failed")
    else:
        assert entry <= address < end, "Unexpected instruction address"

uc.hook_add(UC_HOOK_CODE, instruction)
cases = []
for mask, failure in [(value, 0) for value in range(256)] + [(0x81, value) for value in range(1, 5)]:
    active.clear()
    active.update(mask=mask, failure=failure, trace=[], operations=0, error_logs=0, print_logs=0)
    uc.mem_write(stack, bytes(0x10000))
    # Keep original MRS SP_EL0 instructions and all function instructions.
    uc.reg_write(UC_ARM64_REG_SP_EL0, canary)
    uc.reg_write(UC_ARM64_REG_SP, stack + 0xFF00)
    uc.reg_write(UC_ARM64_REG_X0, tcpc)
    uc.reg_write(UC_ARM64_REG_X1, 0)
    uc.reg_write(UC_ARM64_REG_X2, 0)
    uc.reg_write(UC_ARM64_REG_LR, stop)
    uc.emu_start(entry, stop, timeout=100000, count=1000)
    assert uc.reg_read(UC_ARM64_REG_PC) == stop, "Function did not return"
    assert uc.reg_read(UC_ARM64_REG_SP) == stack + 0xFF00
    trace = active["trace"]
    expected = [("write", "0x9b", "0x38"), ("read", "0x99", hex(mask)),
                ("write", "0x99", hex(mask | 2)), ("write", "0x90", "0x7")]
    if not failure:
        assert [(v["operation"], v["register"], v["value"]) for v in trace] == expected
        assert uc.reg_read(UC_ARM64_REG_X0) == 0 and active["error_logs"] == 0
    else:
        expected_indices = {1: [0], 2: [0, 1, 3], 3: [0, 1, 2, 3], 4: [0, 1, 2, 3]}[failure]
        assert [(v["operation"], v["register"], v["value"]) for v in trace] == [expected[i] for i in expected_indices]
        assert sum(v["failed"] for v in trace) == 1 and active["error_logs"] == 1
    if mask in (0, 0x81, 255) or failure:
        cases.append(dict(active, return_w0=hex(uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff)))
assert hashlib.sha256(stock.HUSB311_BINARY.read_bytes()).hexdigest() == digest
report = {
    "status": "passed", "scope": "original shipped ARM64 function under Unicorn; modeled regmap and kernel calls; no tablet access",
    "module_sha256": digest, "function": function[0], "function_offset": hex(function[2]),
    "function_bytes": function[3], "function_instruction_patches": 0,
    "function_sha256": hashlib.sha256(elf["section_bytes"](function[1])[function[2]:function[2]+function[3]]).hexdigest(),
    "relocations_applied": relocations, "symbol_relocations": stock.function_relocations(elf, function),
    "all_alert_mask_values_passed": 256, "injected_bus_failure_cases": 4,
    "wake_write_sequence": ["9b=38", "99=(original|02)", "90=07"],
    "original_module_unchanged": True, "physical_execution": False,
    "stock_error_limit": "Original function continues to BMC write after a failed 99 read/write; the new UEFI implementation stops and restores instead.",
    "selected_cases": cases,
    "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
}
out = stock.ANALYSIS_DIR / "husb311-sense-wake-arm64.json"
out.write_text(json.dumps(report, indent=2) + "\n")
print("Original HUSB311 sensing wake: 256 alert masks and 4 bus failures PASS; 0 instruction patches")
