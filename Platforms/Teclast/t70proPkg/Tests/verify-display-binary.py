#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate display snapshots and compare the production DSI core with stock LK.

Only selected original ARM64 instructions execute in Unicorn. Register memory,
stack, orientation and frame geometry are fixtures; no tablet/MMIO is accessed.
The DSI C comparison receives raw get_params bytes and six pre-config MMIO words;
it does not receive the captured DSI_config context or expected register trace.
"""
from pathlib import Path
import hashlib
import itertools
import struct

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
    UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7,
    UC_ARM64_REG_X8,
    UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC,
)

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
LK = REPO.parent / "analysis/lk/lk.bin"
LK_SHA256 = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"
BASE, IO, SP, STOP = 0xFFFF000050F00000, 0xFFFF000014005000, 0x70008000, 0x70010000
ENTRY, END = 0x50D0C, 0x51134
FORMATS = (
    0x01001000, 0x0100101A, 0x01001020, 0x0100103A,
    0x01001801, 0x01001821, 0x01002002, 0x01002003,
    0x01002022, 0x01002023, 0x01101044, 0x01101045,
    0x01101064, 0x01101065,
)



def verify_lk_paths(raw):
    """Execute the original path connector, SMI setup and OVL/RDMA starts."""
    display = 0xFFFF000014000000
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(BASE, 0x200000)
    uc.mem_write(BASE, raw)
    uc.mem_map(display, 0x14000)
    uc.mem_map(0x70000000, 0x11000)
    ranges = ((0x51B40, 0x51CA4), (0x51CE0, 0x51E70),
              (0x5222C, 0x52678), (0x50BB4, 0x50C60), (0x5332C, 0x53384))
    trace = []

    def instruction(machine, pc, size, context):
        assert any(BASE + lo <= pc < BASE + hi for lo, hi in ranges), hex(pc)

    def write(machine, access, address, size, value, context):
        if SP - 0x1000 <= address and address + size <= SP:
            return
        if BASE + 0xBF000 <= address < BASE + 0xC0000 or BASE + 0x180000 <= address < BASE + 0x181000:
            return  # Original writable pointer tables, not patched instructions.
        assert display <= address and address + size <= display + 0x14000 and size == 4, (hex(address), size)
        trace.append((address - display, value))

    uc.hook_add(UC_HOOK_CODE, instruction)
    uc.hook_add(UC_HOOK_MEM_WRITE, write)

    def call(offset, argument=0):
        uc.reg_write(UC_ARM64_REG_X0, argument)
        uc.reg_write(UC_ARM64_REG_SP, SP)
        uc.reg_write(UC_ARM64_REG_LR, STOP)
        uc.emu_start(BASE + offset, STOP, count=20000, timeout=1000000)
        assert uc.reg_read(UC_ARM64_REG_PC) == STOP and uc.reg_read(UC_ARM64_REG_SP) == SP

    routes = []
    for scenario, expected in enumerate((
        {0xF18: 1, 0xF20: 1, 0xF0C: 1, 0xF28: 0, 0xF30: 1},
        {0xF18: 1, 0xF0C: 0, 0xF28: 0, 0xF30: 0},
        {0xF20: 1, 0xF0C: 1, 0xF30: 1},
        {0xF0C: 0, 0xF30: 0},
        {0xF14: 1, 0xF18: 0, 0xF0C: 0, 0xF28: 2, 0xF30: 0},
    )):
        page = bytearray(0x14000)
        ports = [0x3380 + 4 * n for n in range(5)] + [0x4380 + 4 * n for n in range(5)]
        for port in ports:
            struct.pack_into("<I", page, port, 0xA5A50001)
        uc.mem_write(display, bytes(page))
        trace.clear()
        call(0x51B40)
        assert trace == [(port, 0xA5A50000) for n in range(5) for port in (0x3380 + 4*n, 0x4380 + 4*n)]
        trace.clear()
        if scenario < 4:
            modules = list(struct.unpack_from("<20i", raw, 0xBF218 + 0x50 * scenario))
            modules = modules[:modules.index(-1)]
            call(0x5222C, scenario)
        else:
            modules = [6, 7, 18, 16]
            uc.mem_write(BASE + 0x182000, struct.pack("<20i", *(modules + [-1] + [0] * 15)))
            call(0x52274, BASE + 0x182000)
        for offset, value in expected.items():
            assert struct.unpack("<I", uc.mem_read(display + offset, 4))[0] == value, (scenario, hex(offset))
        routes.append({"scenario": scenario if scenario < 4 else "modeled_ovl2l_direct",
                       "modules": modules, "modeled_module_list": scenario == 4,
                       "registers": {hex(k): v for k, v in expected.items()},
                       "writes": [[hex(k), v] for k, v in trace]})
    starts = []
    for module in (5, 6):
        uc.mem_write(display, bytes(0x14000))
        trace.clear()
        call(0x50BB4, module)
        bank = 0x5000 + (module - 5) * 0x1000
        values = {offset: struct.unpack("<I", uc.mem_read(display + bank + offset, 4))[0]
                  for offset in (0x0C, 0x24, 0x1F4)}
        assert values == {0x0C: 0x400001, 0x24: 0x5000001, 0x1F4: 0x10000000}
        starts.append({"module": module, "registers": {hex(k): v for k, v in values.items()}})
    uc.mem_write(display, bytes(0x14000))
    trace.clear()
    call(0x5332C)
    assert struct.unpack("<I", uc.mem_read(display + 0x7010, 4))[0] == 1
    assert struct.unpack("<I", uc.mem_read(display + 0x70BC, 4))[0] == 6
    return {"scope": "Exact LK instructions, fake MMIO and stack, no tablet access",
            "calls": 13, "path_cases": routes, "ovl_starts": starts,
            "rdma_start": {"0x10": 1, "0xbc": 6},
            "instruction_sha256": {hex(lo): hashlib.sha256(raw[lo:hi]).hexdigest() for lo, hi in ranges},
            "limits": ["clock/power/DSI states are not captured from the tablet",
                       "the fifth route uses a modeled module list with the original connector",
                       "OVL start bit28 is fixed offset 0x1f4, distinct from per-layer PITCH bit28"]}



def lk_dsi_delay_timer_proof(raw):
    """Statically prove the unit used by LK's 0x6c0b4 delay leaf."""
    def word(offset):
        return struct.unpack_from("<I", raw, offset)[0]

    assert hashlib.sha256(raw).hexdigest() == LK_SHA256
    assert word(0x6C0C4) == 0x97FE93D2  # BL 0x1100c
    assert word(0x6C0D0) == 0x97FE93CF  # BL 0x1100c
    assert word(0x1100C) == 0xD503201F  # alignment NOP at the us-reader entry
    assert word(0x11010) == 0x105D6BC9  # ADR x9, 0xCBD88
    assert word(0x11014) == 0xD53BE028  # MRS x8, CNTVCT_EL0
    assert word(0x11018) == 0xF940012A  # LDR x10, [x9] (microsecond scale)
    assert word(0x1106C) == 0xD503201F  # alignment NOP at the ms-reader entry
    assert word(0x11070) == 0x105D6849  # ADR x9, 0xCBD78
    assert word(0x110E4) == 0xD2C07D08  # 1000 << 32, millisecond scale
    assert word(0x110EC) == 0x9AD40909  # udiv x9, x8, x20 (millisecond scale)
    assert word(0x110F0) == 0xD2C84808  # low 48 bits of 1,000,000 << 32
    assert word(0x110F8) == 0xF2E001E8  # high 16 bits complete 1,000,000 << 32
    assert word(0x11140) == 0x9AD4090B  # udiv x11, x8, x20
    assert word(0x11164) == 0x29002EA9  # store microsecond scale at [x21]
    assert word(0x1114C) == 0x290026EE  # store millisecond scale at [x23]
    assert word(0x6C0C0) == 0x2A0003F4  # preserve the caller's requested interval
    assert word(0x6C0D4) == 0xCB130008  # elapsed counter subtraction
    assert word(0x6C0D8) == 0xEB14011F  # compare elapsed ticks to argument
    return {
        "status": "passed",
        "lk_sha256": LK_SHA256,
        "delay_leaf": "0x6c0b4",
        "counter_reader": "function entry 0x1100c has an alignment NOP; 0x11010 selects 0xCBD88, 0x11014 reads CNTVCT_EL0, and 0x11018 loads the microsecond scale",
        "microsecond_scale": {
            "initializer": "0x110f0/0x110f8 create 1000000 << 32",
            "conversion": "0x11140 divides by CNTFRQ_EL0; 0x11164 stores the quotient at 0xCBD88",
        },
        "separate_millisecond_scale": {
            "initializer": "0x110e4 creates 1000 << 32",
            "conversion": "0x110ec divides by CNTFRQ_EL0; 0x1114c stores at 0xCBD78",
            "reader": "function entry 0x1106c has an alignment NOP; 0x11070 reads CNTVCT_EL0 and uses 0xCBD78",
        },
        "delay_call_arguments_are_microseconds": True,
        "limitations": ["CNTFRQ_EL0 accuracy depends on firmware initialization",
                        "this is static instruction evidence, not a physical timer measurement"],
    }


def original_lk_dsi_configuration(raw, seed):
    """Run stock DSI_config with stock get_params and bounded RAM/MMIO.

    DSI/PHY code is executed unchanged. libc, print and timer leaves are
    intercepted. This obtains register contents, not a working physical link.
    """
    from unicorn import UC_HOOK_MEM_INVALID
    base, ram, stack, stop = BASE, 0x71000000, 0x71010000, 0x72000000
    display, phy = 0xFFFF000014000000, 0xFFFF000011F60000
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for address, size in ((base, 0x200000), (ram, 0x20000),
                          (stop, 0x1000), (display, 0x20000), (phy, 0x1000)):
        machine.mem_map(address, size)
    machine.mem_write(base, raw)
    for address, size in ((display, 0x20000), (phy, 0x1000)):
        machine.mem_write(address, struct.pack("<I", seed) * (size // 4))
    writes, leaves, visited = [], [], set()

    def mapped_ram(address, size):
        return any(low <= address and address + size <= high
                   for low, high in ((base, base + 0x200000),
                                     (ram, ram + 0x20000)))

    def instruction(uc, pc, size, context):
        offset = pc - base
        visited.add(offset)
        if offset == 0x6DCEC:  # memset leaf
            target = uc.reg_read(UC_ARM64_REG_X0)
            value = uc.reg_read(UC_ARM64_REG_X1)
            length = uc.reg_read(UC_ARM64_REG_X2)
            assert 0 < length <= 0x1000 and mapped_ram(target, length)
            uc.mem_write(target, bytes([value & 255]) * length)
        elif offset == 0x6D9CC:  # memcpy leaf
            target = uc.reg_read(UC_ARM64_REG_X0)
            source = uc.reg_read(UC_ARM64_REG_X1)
            length = uc.reg_read(UC_ARM64_REG_X2)
            assert 0 < length <= 0x1000
            assert mapped_ram(target, length) and mapped_ram(source, length)
            uc.mem_write(target, bytes(uc.mem_read(source, length)))
        elif offset == 0x6F520:  # printk leaf
            leaves.append({"kind": "printk", "format_pointer": hex(uc.reg_read(UC_ARM64_REG_X0))})
            uc.reg_write(UC_ARM64_REG_X0, 0)
        elif offset == 0x6C0B4:  # wait on the 0x1100c microsecond counter conversion
            leaves.append({"kind": "delay_microseconds", "value": uc.reg_read(UC_ARM64_REG_X0)})
        else:
            assert (0x46500 <= offset < 0x4C400 or 0x5BC30 <= offset < 0x5BD00), hex(offset)
            return
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def write(uc, access, address, size, value, context):
        if mapped_ram(address, size):
            return
        assert size == 4 and (
            display <= address and address + size <= display + 0x20000 or
            phy <= address and address + size <= phy + 0x1000), (hex(address), size)
        writes.append({"address": hex(address & 0xFFFFFFFF), "value": value,
                       "original_pc_offset": hex(uc.reg_read(UC_ARM64_REG_PC) - base)})

    def unmapped(uc, access, address, size, value, context):
        raise AssertionError(("unmapped DSI access", access, hex(address),
                              hex(uc.reg_read(UC_ARM64_REG_PC) - base)))

    machine.hook_add(UC_HOOK_CODE, instruction)
    machine.hook_add(UC_HOOK_MEM_WRITE, write)
    machine.hook_add(UC_HOOK_MEM_INVALID, unmapped)

    def call(offset, *arguments):
        for reg, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2), arguments):
            machine.reg_write(reg, value)
        machine.reg_write(UC_ARM64_REG_SP, stack)
        machine.reg_write(UC_ARM64_REG_LR, stop)
        machine.emu_start(base + offset, stop, timeout=2000000, count=200000)
        assert machine.reg_read(UC_ARM64_REG_PC) == stop
        assert machine.reg_read(UC_ARM64_REG_SP) == stack

    call(0x5BC30, ram)
    parameters = bytes(machine.mem_read(ram, 0x758))
    assert struct.unpack_from("<II", parameters, 0x18) == (2240, 1400)
    configuration = bytearray(0x1000)
    struct.pack_into("<I", configuration, 0xC, 1)
    configuration[0x2C8:0x2C8 + 0x5B0] = parameters[0x170:0x170 + 0x5B0]
    struct.pack_into("<II", configuration, 0x910, 2240, 1400)
    machine.mem_write(ram + 0x1000, bytes(configuration))
    call(0x4AB34, 16, ram + 0x1000, 0)
    assert machine.reg_read(UC_ARM64_REG_X0) == 0
    snapshot = bytes(machine.mem_read(display + 0x13000, 0x1000))
    word = lambda offset: struct.unpack_from("<I", snapshot, offset)[0]
    assert word(0x18) & 0x3C == 0x3C
    assert word(0x1C) & 0xF0000 == 0x30000 and word(0x1C) & 0x7FFF == 2240 * 3
    assert word(0x38) == 1400 << 16 | 2240
    assert [word(offset) for offset in (0x20, 0x24, 0x28, 0x2C)] == [8, 32, 230, 1400]
    assert [word(offset) for offset in (0x50, 0x54, 0x58, 0x5C)] == [16, 88, 108, 0]
    assert len(writes) == 46
    assert [leaf["value"] for leaf in leaves if leaf["kind"] == "delay_microseconds"] == [1000] * 4
    executed = b"".join(struct.pack("<I", offset) + raw[offset:offset + 4]
                        for offset in sorted(visited))
    return snapshot, {
        "status": "passed", "mmio_seed": hex(seed), "writes": writes, "intercepted_leaves": leaves,
        "entry_offsets": ["0x5bc30", "0x4ab34"], "module_argument": 16,
        "delay_timer_proof": lk_dsi_delay_timer_proof(raw),
        "parameter_fragment": "original get_params+0x170, 0x5b0 bytes",
        "configuration_model": "dirty=1 at +0xc, copied DSI parameters at +0x2c8, width/height at +0x910/+0x914; other RAM fields zero",
        "parameter_sha256": hashlib.sha256(parameters).hexdigest(),
        "configuration_sha256": hashlib.sha256(configuration).hexdigest(),
        "snapshot_sha256": hashlib.sha256(snapshot).hexdigest(),
        "executed_unique_instruction_count": len(visited),
        "executed_offset_and_instruction_sha256": hashlib.sha256(executed).hexdigest(),
        "limits": ["MMIO is writable RAM; no electrical response or link-ready acknowledgment is modeled",
                   "timer leaf arguments are microseconds by the 0xCBD88 counter-scale evidence; no physical timing is measured",
                   "configuration does not start the link or enable external clocks/panel power",
                   "the C handoff test models inherited start/video-mode plus SPM/clock/OVL/RDMA state separately"],
    }


def original_lk_dsi_framework_wiring(raw):
    """Run only stock module-vector initialization and DSI API registration.

    The registration consumer is an unmapped, explicitly intercepted callback:
    the hook records its arguments and returns to the original caller. Any other
    external branch, unmapped access, or MMIO write fails the case.
    """
    from unicorn import UC_HOOK_MEM_INVALID

    base, scratch, stack, stop = BASE, 0x71000000, 0x7101F000, 0x72000000
    callback = 0x73000000
    vector = base + 0x17F508
    api_address = base + 0x17F1F0
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    machine.mem_map(base, 0x200000)
    machine.mem_map(scratch, 0x20000)
    machine.mem_map(stop, 0x1000)
    machine.mem_write(base, raw)
    callback_object = scratch + 0x1000
    machine.mem_write(callback_object, bytes(0x20))
    machine.mem_write(callback_object + 8, struct.pack("<Q", callback))

    module_copy_ranges = ((0x50944, 0x5095C), (0x4F8A8, 0x4F94C))
    registration_ranges = ((0x4BE14, 0x4C09C),)
    active_ranges = module_copy_ranges
    active_case = "module-vector-init"
    visited = {active_case: set(), "dsi-api-registration": set()}
    writes = []
    registration_calls = []

    def instruction(uc, pc, size, context):
        offset = pc - base
        if active_case == "dsi-api-registration" and offset == 0x4C080:
            target = uc.reg_read(UC_ARM64_REG_X8)
            api_pointer = uc.reg_read(UC_ARM64_REG_X0)
            registration_context = uc.reg_read(UC_ARM64_REG_X1)
            assert target == callback, ("unexpected registration callback", hex(target))
            assert registration_context == callback_object
            registration_calls.append(api_pointer)
            # Observe the boundary; do not execute or assume the external consumer.
            uc.reg_write(UC_ARM64_REG_LR, pc + 4)
            uc.reg_write(UC_ARM64_REG_PC, pc + 4)
            return
        assert any(low <= offset < high for low, high in active_ranges), (
            active_case, hex(offset), "unexpected original/external instruction")
        visited[active_case].add(offset)

    def write(uc, access, address, size, value, context):
        in_module_vector = vector <= address and address + size <= vector + 24 * 8
        in_api_table = api_address <= address and address + size <= api_address + 0x108
        in_scratch = scratch <= address and address + size <= scratch + 0x20000
        assert size in (1, 2, 4, 8, 16) and (in_module_vector or in_api_table or in_scratch), (
            active_case, hex(address), size, hex(uc.reg_read(UC_ARM64_REG_PC) - base))
        writes.append((active_case, address, size, value))

    def unmapped(uc, access, address, size, value, context):
        raise AssertionError((active_case, "unmapped memory", access, hex(address), size,
                              hex(uc.reg_read(UC_ARM64_REG_PC) - base)))

    machine.hook_add(UC_HOOK_CODE, instruction)
    machine.hook_add(UC_HOOK_MEM_WRITE, write)
    machine.hook_add(UC_HOOK_MEM_INVALID, unmapped)

    def call(offset, x0=0, x1=0):
        machine.reg_write(UC_ARM64_REG_X0, x0)
        machine.reg_write(UC_ARM64_REG_X1, x1)
        machine.reg_write(UC_ARM64_REG_SP, stack)
        machine.reg_write(UC_ARM64_REG_LR, stop)
        machine.emu_start(base + offset, stop, timeout=1000000, count=50000)
        assert machine.reg_read(UC_ARM64_REG_PC) == stop, (active_case, hex(offset))
        assert machine.reg_read(UC_ARM64_REG_SP) == stack, (active_case, hex(offset))

    original_image = bytes(machine.mem_read(base, len(raw)))
    call(0x50944)
    assert machine.reg_read(UC_ARM64_REG_X0) == 0

    source_table = 0xBEB38
    source_ops = tuple(struct.unpack_from("<Q", raw, source_table + index * 0x20)[0]
                       for index in range(24))
    copied_ops = struct.unpack("<24Q", machine.mem_read(vector, 24 * 8))
    assert copied_ops == source_ops
    assert len(copied_ops) == 24 and copied_ops[23] == 0

    records = [struct.unpack_from("<QQQQ", raw, source_table + index * 0x20)
               for index in range(23)]
    dsi_index = next(index for index, record in enumerate(records) if record[1] == 0x10)
    dsi_descriptor = records[dsi_index][0]
    assert dsi_descriptor == base + 0xBE3E0
    assert copied_ops[dsi_index] == dsi_descriptor
    descriptor_offset = dsi_descriptor - base
    optional_lifecycle_callback = struct.unpack_from("<Q", raw, descriptor_offset + 0x80)[0]
    operation_count = struct.unpack_from("<Q", raw, descriptor_offset + 0xA0)[0]
    operation_table_offset = descriptor_offset + 0xA8
    dsi_config_pointer = struct.unpack_from("<Q", raw, operation_table_offset + 2 * 8)[0]
    dsi_start_pointer = struct.unpack_from("<Q", raw, operation_table_offset + 4 * 8)[0]
    dsi_quiesce_pointer = struct.unpack_from("<Q", raw, operation_table_offset + 8 * 8)[0]
    dsi_status_pointer = struct.unpack_from("<Q", raw, operation_table_offset + 9 * 8)[0]
    dsi_register_pointer = struct.unpack_from("<Q", raw, operation_table_offset + 14 * 8)[0]
    assert optional_lifecycle_callback == 0
    assert operation_count == 16
    assert dsi_config_pointer == base + 0x4AB34
    assert dsi_start_pointer == base + 0x4B910
    assert dsi_quiesce_pointer == base + 0x4B324
    assert dsi_status_pointer == base + 0x4B6BC
    assert dsi_register_pointer == base + 0x4BE14

    api_offset = 0x17F1F0
    machine.mem_write(api_address, bytes(0x108))
    active_case = "dsi-api-registration"
    active_ranges = registration_ranges
    call(0x4BE14, 0x10, callback_object)
    assert machine.reg_read(UC_ARM64_REG_X0) == 0
    assert registration_calls == [api_address]

    api_functions = {
        0x00: 0x4C09C,
        0x20: 0x4C0F8,
        0x28: 0x4BCA0,
        0x50: 0x4BAF8,
        0x58: 0x4BA34,
        0x60: 0x4BB34,
        0x90: 0x4BB70,
        0xB8: 0x4BD10,
        0xC0: 0x4BBE8,
        0xC8: 0x4BCAC,
        0xD8: 0x4BA64,
        0xE0: 0x4BAA4,
        0xF8: 0x4BAD4,
        0x100: 0x4BBAC,
    }
    api = bytes(machine.mem_read(api_address, 0x108))
    api_words = struct.unpack("<33Q", api)
    for offset in range(0, len(api), 8):
        expected = base + api_functions[offset] if offset in api_functions else 0
        assert api_words[offset // 8] == expected, (
            "DSI API table", hex(offset), hex(api_words[offset // 8]), hex(expected))
    api_snapshot = bytes(machine.mem_read(api_address, 0x108))
    registrations_before_invalid_id = len(registration_calls)
    call(0x4BE14, 0x12, callback_object)
    assert machine.reg_read(UC_ARM64_REG_X0) == 0xFFFFFFFF
    assert len(registration_calls) == registrations_before_invalid_id
    assert bytes(machine.mem_read(api_address, 0x108)) == api_snapshot
    assert bytes(machine.mem_read(base, len(raw))) == original_image

    return {
        "status": "passed",
        "scope": "Original LK ARM64 only: module operation-vector copy and DSI API registration table construction",
        "lk_sha256": LK_SHA256,
        "module_vector": {
            "entry_offset": "0x50944", "copy_helper_offset": "0x4f8a8",
            "source_record_offset": hex(source_table), "source_record_stride": 0x20,
            "source_record_count": len(records), "destination_bss_offset": hex(0x17F508),
            "copied_qword_count": len(copied_ops),
            "trailing_null_slot": {"index": 23,
                                   "source_offset": hex(source_table + 23 * 0x20),
                                   "value": hex(copied_ops[23])},
            "dsi_record_index": dsi_index, "dsi_module_id": "0x10",
            "dsi_descriptor_offset": hex(descriptor_offset),
            "optional_lifecycle_callback_offset": "0x80",
            "optional_lifecycle_callback": hex(optional_lifecycle_callback),
            "operation_count_offset": "0xa0", "operation_count": operation_count,
            "operation_table_offset": hex(operation_table_offset),
            "dsi_config_operation_pointer": hex(dsi_config_pointer - base),
            "dsi_op4_start_pointer": hex(dsi_start_pointer - base),
            "dsi_op8_pointer": hex(dsi_quiesce_pointer - base),
            "dsi_op9_pointer": hex(dsi_status_pointer - base),
            "dsi_api_registration_operation_pointer": hex(dsi_register_pointer - base),
            "runtime_sha256": hashlib.sha256(bytes(machine.mem_read(vector, 24 * 8))).hexdigest(),
        },
        "dsi_api_registration": {
            "entry_offset": "0x4be14", "module_id": 0x10,
            "destination_bss_offset": hex(api_offset), "table_size": len(api),
            "callback_intercepted": True, "callback_count": len(registration_calls),
            "invalid_module_id_rejected_without_callback_or_table_change": True,
            "callback_target": "unmapped modeled registration-consumer boundary",
            "function_pointers": {hex(offset): hex(pointer) for offset, pointer in api_functions.items()},
            "table_sha256": hashlib.sha256(api).hexdigest(),
        },
        "executed_offsets": {
            name: {"count": len(offsets),
                  "sha256": hashlib.sha256(b"".join(
                      struct.pack("<I", offset) + raw[offset:offset + 4]
                      for offset in sorted(offsets))).hexdigest()}
            for name, offsets in visited.items()
        },
        "modeled_writes": {
            "count": len(writes), "ranges": ["module-vector/API BSS tables", "scratch RAM/stack"],
            "mmio": "unmapped; any MMIO access fails",
        },
        "limits": [
            "registration consumer is intercepted only to capture the original API-table pointer; consumer code is not executed",
            "this does not run the 0x5039c DDP configuration dispatcher or DSI operations 0x4ae90/0x4b910",
            "no MIPI TX/PHY/clock/start sequence or physical link readiness is inferred",
        ],
    }


def original_lk_dsi_mipi_op7(raw, seed=0xA5A5A5A5, lifecycle_cycles=0, busy_reads=0):
    """Run get_params, DSI_config and the registered DSI op7 for module 0x10.

    The original ARM64 routines run against RAM-backed DSI/MIPI windows.  The
    setup stage reuses the already bounded get_params/config call pattern; the
    op7 stage has a narrower instruction allowlist and records every MMIO read,
    write and intercepted timer argument.  This demonstrates the register
    sequence only; it does not model a powered PHY or a ready display link.
    """
    from unicorn import UC_HOOK_MEM_INVALID, UC_HOOK_MEM_READ

    assert len(raw) == 805888 and hashlib.sha256(raw).hexdigest() == LK_SHA256
    base, ram, stack, stop = BASE, 0x71000000, 0x71010000, 0x72000000
    display, phy = 0xFFFF000014000000, 0xFFFF000011F60000
    display_base, mipi_base = 0x14013000, 0x11F60000
    display_pointer, mipi_pointer = 0xFFFF000014013000, 0xFFFF000011F60000
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for address, size in ((base, 0x200000), (ram, 0x20000), (stop, 0x1000),
                          (display, 0x20000), (phy, 0x1000)):
        machine.mem_map(address, size)
    machine.mem_write(base, raw)
    for address, size in ((display, 0x20000), (phy, 0x1000)):
        machine.mem_write(address, struct.pack("<I", seed) * (size // 4))

    def mapped_ram(address, size):
        return any(low <= address and address + size <= high
                   for low, high in ((base, base + 0x200000),
                                     (ram, ram + 0x20000)))

    def writable_ram(address, size):
        return (ram <= address and address + size <= ram + 0x20000 or
                base + 0x17E000 <= address and address + size <= base + 0x181000)

    assert 0 <= lifecycle_cycles <= 4 and 0 <= busy_reads <= 4
    status_reads = 0
    byte_probe_reads = 0
    setup_ranges = ((0x46500, 0x4C400), (0x5BC30, 0x5BD00))
    op7_ranges = ((0x4AE90, 0x4B324), (0x47B74, 0x4816C),
                  (0x46A30, 0x46B68), (0x4F434, 0x4F458),
                  (0x4F540, 0x4F578))
    lifecycle_ranges = op7_ranges + (
        (0x4B78C, 0x4B87C), (0x4B910, 0x4BA34), (0x4ADD4, 0x4AE38),
        (0x4B6BC, 0x4B714), (0x4B324, 0x4B668),
        (0x468EC, 0x46A30), (0x46B68, 0x46CB8), (0x474C0, 0x475EC))
    phase = "setup"
    visited = {"setup": set(), "op7": set()}
    writes, reads, leaves, events = [], [], [], []

    def instruction(uc, pc, size, context):
        nonlocal phase
        offset = pc - base
        if offset in (0x6DCEC, 0x6D9CC, 0x6F520, 0x6C0B4):
            if offset == 0x6C0B4:
                delay = uc.reg_read(UC_ARM64_REG_X0)
                callsite = uc.reg_read(UC_ARM64_REG_LR) - base - 4
                assert 0 <= callsite <= len(raw) - 4, ("invalid delay callsite", hex(callsite))
                instruction_word = struct.unpack_from("<I", raw, callsite)[0]
                assert instruction_word & 0xFC000000 == 0x94000000, (
                    "delay call is not BL", hex(callsite), hex(instruction_word))
                branch_imm26 = instruction_word & 0x03FFFFFF
                if branch_imm26 & (1 << 25):
                    branch_imm26 -= 1 << 26
                branch_target = callsite + (branch_imm26 << 2)
                assert branch_target == offset, (
                    "delay BL target mismatch", hex(callsite), hex(branch_target), hex(offset))
                leaves.append({"phase": phase, "kind": "delay_microseconds",
                               "original_pc_offset": hex(offset),
                               "original_callsite_offset": hex(callsite), "value": delay})
                events.append({"kind": "delay", "value": delay})
            elif phase == "op7":
                raise AssertionError(("unexpected op7 external call", hex(offset)))
            elif offset == 0x6DCEC:
                target = uc.reg_read(UC_ARM64_REG_X0)
                value = uc.reg_read(UC_ARM64_REG_X1)
                length = uc.reg_read(UC_ARM64_REG_X2)
                assert 0 < length <= 0x1000 and writable_ram(target, length)
                uc.mem_write(target, bytes([value & 255]) * length)
            elif offset == 0x6D9CC:
                target = uc.reg_read(UC_ARM64_REG_X0)
                source = uc.reg_read(UC_ARM64_REG_X1)
                length = uc.reg_read(UC_ARM64_REG_X2)
                assert 0 < length <= 0x1000
                assert writable_ram(target, length) and mapped_ram(source, length)
                uc.mem_write(target, bytes(uc.mem_read(source, length)))
            elif offset == 0x6F520:
                leaves.append({"phase": phase, "kind": "printk",
                               "format_pointer": hex(uc.reg_read(UC_ARM64_REG_X0))})
                uc.reg_write(UC_ARM64_REG_X0, 0)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
            return

        allowed = setup_ranges if phase == "setup" else (
            op7_ranges if phase == "op7" else lifecycle_ranges)
        assert any(low <= offset < high for low, high in allowed), (
            phase, "unexpected original/external instruction", hex(offset))
        visited[phase].add(offset)

    def read(uc, access, address, size, value, context):
        nonlocal status_reads, byte_probe_reads
        if mapped_ram(address, size):
            return
        in_display = display <= address and address + size <= display + 0x20000
        in_phy = phy <= address and address + size <= phy + 0x1000
        pc = uc.reg_read(UC_ARM64_REG_PC) - base
        byte_probe = lifecycle_cycles and size == 1 and pc == 0x4ADEC and address == display_pointer + 0x14
        assert (size == 4 or byte_probe) and address % 4 == 0 and (in_display or in_phy), (
            phase, "unexpected MMIO read", hex(address), size, hex(pc))
        if byte_probe:
            byte_probe_reads += 1
        if lifecycle_cycles and pc == 0x4B6F0 and address == display_pointer + 0xC:
            # Explicit peripheral fixture: busy for N polls, then clear.
            busy = status_reads % (busy_reads + 1) < busy_reads
            previous = struct.unpack("<I", uc.mem_read(address, 4))[0]
            uc.mem_write(address, struct.pack("<I", (previous | 0x80000000) if busy
                                             else (previous & 0x7fffffff)))
            status_reads += 1
        # Normalize the original byte-sized condition probe to the same
        # register word that the portable Read32 API reads. All writes and
        # other accesses retain the original widths and ordering.
        previous = struct.unpack("<I", uc.mem_read(address, 4))[0]
        reads.append({"phase": phase,
                      "region": "dsi" if in_display else "mipi_tx",
                      "offset": hex(address - (display if in_display else phy)),
                      "address": hex(address), "value": previous,
                      "original_pc_offset": hex(uc.reg_read(UC_ARM64_REG_PC) - base)})
        events.append({"kind": "read", "region": reads[-1]["region"],
                       "offset": reads[-1]["offset"], "value": previous})

    def write(uc, access, address, size, value, context):
        if writable_ram(address, size):
            return
        in_display = display <= address and address + size <= display + 0x20000
        in_phy = phy <= address and address + size <= phy + 0x1000
        assert size == 4 and address % 4 == 0 and (in_display or in_phy), (
            phase, "unexpected MMIO write", hex(address), size,
            hex(uc.reg_read(UC_ARM64_REG_PC) - base))
        previous = struct.unpack("<I", uc.mem_read(address, 4))[0]
        writes.append({"phase": phase,
                       "region": "dsi" if in_display else "mipi_tx",
                       "offset": hex(address - (display if in_display else phy)),
                       "address": hex(address), "before": previous,
                       "value": value & 0xFFFFFFFF,
                       "changed_mask": (previous ^ value) & 0xFFFFFFFF,
                       "original_pc_offset": hex(uc.reg_read(UC_ARM64_REG_PC) - base)})
        events.append({"kind": "write", "region": writes[-1]["region"],
                       "offset": writes[-1]["offset"], "value": value & 0xffffffff})

    def unmapped(uc, access, address, size, value, context):
        raise AssertionError((phase, "unmapped access", access, hex(address), size,
                              hex(uc.reg_read(UC_ARM64_REG_PC) - base)))

    machine.hook_add(UC_HOOK_CODE, instruction)
    machine.hook_add(UC_HOOK_MEM_READ, read)
    machine.hook_add(UC_HOOK_MEM_WRITE, write)
    machine.hook_add(UC_HOOK_MEM_INVALID, unmapped)

    def call(offset, *arguments):
        for reg, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2), arguments):
            machine.reg_write(reg, value)
        machine.reg_write(UC_ARM64_REG_SP, stack)
        machine.reg_write(UC_ARM64_REG_LR, stop)
        machine.emu_start(base + offset, stop, timeout=2000000, count=200000)
        assert machine.reg_read(UC_ARM64_REG_PC) == stop, (phase, hex(offset))
        assert machine.reg_read(UC_ARM64_REG_SP) == stack, (phase, hex(offset))

    parameters_address, configuration_address = ram, ram + 0x1000
    call(0x5BC30, parameters_address)
    parameters = bytes(machine.mem_read(parameters_address, 0x758))
    assert struct.unpack_from("<II", parameters, 0x18) == (2240, 1400)
    assert struct.unpack_from("<I", parameters, 0)[0] == 2
    assert struct.unpack_from("<I", parameters, 0x170)[0] == 1
    assert struct.unpack_from("<I", parameters, 0x230)[0] == 733
    assert struct.unpack_from("<I", parameters, 0x234)[0] == 0
    assert struct.unpack_from("<I", parameters, 0x240)[0] == 1
    assert struct.unpack_from("<I", parameters, 0x624)[0] == 0
    assert parameters[0x628:0x640] == bytes(0x18)

    module_table = 0xBEB38
    dsi_module_index = 15
    dsi_module_record_offset = module_table + dsi_module_index * 0x20
    dsi_module_record = struct.unpack_from("<QQQQ", raw, dsi_module_record_offset)
    assert dsi_module_record == (base + 0xBE3E0, 0x10, base + 0xAEA7E, 1)
    descriptor_pointer, module_id, module_registration, module_flags = dsi_module_record
    descriptor = descriptor_pointer - base
    descriptor_function_pointer = struct.unpack_from("<Q", raw, descriptor + 8)[0]
    assert descriptor_function_pointer == base + 0x45D80
    module_gate_delta = struct.unpack_from(
        "<i", raw, 0xB31CC + (module_id - 2) * 4)[0]
    module_gate_target = 0x4F448 + module_gate_delta
    assert module_gate_target == 0x4F540
    assert struct.unpack_from("<I", parameters, 0x6D8)[0] == 0

    configuration = bytearray(0x1000)
    struct.pack_into("<I", configuration, 0xC, 1)
    configuration[0x2C8:0x2C8 + 0x5B0] = parameters[0x170:0x170 + 0x5B0]
    struct.pack_into("<II", configuration, 0x910, 2240, 1400)
    port_initial_offsets = (0x13008, 0x13014, 0x13018, 0x1301C, 0x13064, 0x13200)
    port_initial = [struct.unpack_from("<I", machine.mem_read(display + offset, 4))[0]
                    for offset in port_initial_offsets]
    assert port_initial == [seed] * len(port_initial_offsets)
    machine.mem_write(configuration_address, bytes(configuration))
    call(0x4AB34, module_id, configuration_address, 0)
    assert machine.reg_read(UC_ARM64_REG_X0) == 0
    assert 0x47B74 in visited["setup"] and 0x475EC not in visited["setup"]
    context_address = base + 0x17EA50
    assert bytes(machine.mem_read(context_address + 0x1D8, 0x5B0)) == parameters[0x170:0x170 + 0x5B0]
    assert struct.unpack_from("<Q", raw, 0xB08D8)[0] == display_pointer
    assert struct.unpack_from("<Q", raw, 0xB08D8 + 8)[0] == 0
    assert struct.unpack_from("<Q", raw, 0xB0948)[0] == mipi_pointer
    assert struct.unpack_from("<Q", raw, 0xB0948 + 8)[0] == 0
    op_table = descriptor + 0xA8
    assert struct.unpack_from("<Q", raw, descriptor + 0xA0)[0] == 16
    assert struct.unpack_from("<Q", raw, op_table + 7 * 8)[0] == base + 0x4AE90
    assert machine.mem_read(base + 0x17F1E4, 1) == b"\0"
    assert machine.mem_read(base + 0x17F1E8, 1) == b"\0"

    # Raw source context values, captured before op7 rather than inferred from
    # its MMIO trace. Pair order follows the original context-to-port routine.
    shadow_offsets = (0x14,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x5c,
                      0x60,0x64,0x68,0x70,0x9c,0x11c,0x120,0x124,0x128,0x1ac)
    port_shadow = [struct.unpack("<I", machine.mem_read(context_address+off,4))[0]
                   for off in shadow_offsets]
    get_params_offsets = (0x00,0x18,0x1c,0x230,0x234,0x238,0x240,0x624,
                          0x628,0x62c,0x630,0x634,0x638,0x63c,0x6d8)
    get_params_words = {hex(off): struct.unpack_from("<I",parameters,off)[0]
                        for off in get_params_offsets}
    setup_writes = list(writes)
    setup_reads = list(reads)
    setup_leaves = list(leaves)
    pre_display = bytes(machine.mem_read(display, 0x20000))
    pre_phy = bytes(machine.mem_read(phy, 0x1000))
    setup_lane_words = {0x40: 0x65432100, 0x44: 0x24232987,
                        0x48: 0x76543210, 0x4C: 8}
    for offset, expected in setup_lane_words.items():
        assert struct.unpack_from("<I", pre_phy, offset)[0] == expected
        selected = [item for item in setup_writes
                    if item["region"] == "mipi_tx" and item["offset"] == hex(offset)]
        assert len(selected) == 1 and selected[0]["value"] == expected, (hex(offset), selected)
    dsi_port_offset = display_pointer - display
    dsi_reset_before = struct.unpack_from("<I", pre_display, dsi_port_offset)[0]
    writes.clear()
    reads.clear()
    leaves.clear()
    events.clear()
    phase = "op7"
    call(0x4AE90, module_id, 0, 0)
    assert machine.reg_read(UC_ARM64_REG_X0) == 0
    assert 0x4F434 in visited["op7"] and 0x4F540 in visited["op7"]
    assert 0x47B74 in visited["op7"] and 0x475EC not in visited["op7"]
    assert 0x46A30 in visited["op7"]
    post_display = bytes(machine.mem_read(display, 0x20000))
    post_phy = bytes(machine.mem_read(phy, 0x1000))

    mipi_writes = [item for item in writes if item["region"] == "mipi_tx"]
    dsi_writes = [item for item in writes if item["region"] == "dsi"]
    reset_writes = [item for item in dsi_writes if item["offset"] == hex(dsi_port_offset)]
    assert [item["original_pc_offset"] for item in reset_writes] == [
        "0x4b1d4", "0x4b1e0", "0x4b1f4"]
    assert [item["value"] for item in reset_writes] == [
        dsi_reset_before & ~4, (dsi_reset_before & ~4) | 4, dsi_reset_before & ~4]
    assert all(item["changed_mask"] & ~4 == 0 and
               item["changed_mask"] == ((item["before"] ^ item["value"]) & 4)
               for item in reset_writes)
    assert struct.unpack_from("<I", post_display, dsi_port_offset)[0] == dsi_reset_before & ~4
    assert not any(item["offset"] == "0x0" for item in mipi_writes)

    expected_lane_words = {0x40: 0x65432100, 0x44: 0x24232987,
                           0x48: 0x76543210, 0x4C: 8}
    for offset, expected in expected_lane_words.items():
        selected = [item for item in mipi_writes if item["offset"] == hex(offset)]
        assert len(selected) == 1 and selected[0]["value"] == expected, (hex(offset), selected)
    mipi_gate_writes = [item for item in mipi_writes if item["offset"] == "0x328"]
    assert len(mipi_gate_writes) == 1 and mipi_gate_writes[0]["value"] & 1
    assert mipi_gate_writes[0]["changed_mask"] & ~1 == 0
    assert not any(item["offset"] in {"0x128", "0x228", "0x428", "0x528"}
                   for item in mipi_writes)
    assert any(item["offset"] == "0x30" for item in mipi_writes)
    assert any(item["offset"] == "0x14" for item in mipi_writes)

    rmw_masks = {
        ("dsi", 0x4B1D4): 0x4, ("dsi", 0x4B1E0): 0x4,
        ("dsi", 0x4B1F4): 0x4, ("dsi", 0x4B208): 1 << 20,
        ("mipi_tx", 0x47C1C): 1, ("mipi_tx", 0x47F3C): 1 << 4,
        ("mipi_tx", 0x47FA4): 1, ("mipi_tx", 0x47FB4): 1,
        ("mipi_tx", 0x47FC4): 1, ("mipi_tx", 0x47FD4): 1,
        ("mipi_tx", 0x47FE4): 1, ("mipi_tx", 0x47FF0): 1,
        ("mipi_tx", 0x48000): 2, ("mipi_tx", 0x48070): 0x700,
        ("mipi_tx", 0x48114): 2,
        ("dsi", 0x4F558): 1 << 19, ("dsi", 0x4F570): 1,
        ("dsi", 0x4B22C): 1, ("dsi", 0x4B250): 1,
        ("dsi", 0x4B25C): 1,
    }
    observed_rmw = {}
    for item in writes:
        key = (item["region"], int(item["original_pc_offset"], 16))
        if key in rmw_masks:
            assert item["changed_mask"] & ~rmw_masks[key] == 0, item
            observed_rmw[key] = observed_rmw.get(key, 0) + 1
    assert set(observed_rmw) == set(rmw_masks), observed_rmw
    assert all(count == 1 for count in observed_rmw.values()), observed_rmw
    conditional_rmw_masks = {
        ("mipi_tx", 0x48090): 1, ("mipi_tx", 0x4809C): 0xFFFF0000,
        ("mipi_tx", 0x480E4): 0xFFFF0000, ("mipi_tx", 0x480F4): 0xFFFF,
    }
    assert struct.unpack_from("<I", parameters, 0x240)[0] == 1
    assert 0x4807C in visited["op7"]
    assert not any(0x48080 <= pc < 0x480FC for pc in visited["op7"])
    assert not any((item["region"], int(item["original_pc_offset"], 16)) in conditional_rmw_masks
                   for item in writes)
    op7_delay_calls = [leaf for leaf in leaves if leaf["kind"] == "delay_microseconds"]
    op7_delays = [leaf["value"] for leaf in op7_delay_calls]
    assert op7_delays == [1000] * 5, op7_delays
    assert [leaf["original_callsite_offset"] for leaf in op7_delay_calls] == [
        "0x47f80", "0x47ff4", "0x47f2c", "0x47f40", "0x4b1e4"]
    assert machine.mem_read(base + 0x17F1E4, 1) == b"\1"
    assert machine.mem_read(base + 0x17F1E8, 1) == b"\1"

    if lifecycle_cycles:
        cycles = []
        for cycle in range(lifecycle_cycles):
            phase = "cycle-" + str(cycle)
            visited[phase] = set()
            if cycle:
                call(0x4AE90, module_id, 0, 0)
                assert machine.reg_read(UC_ARM64_REG_X0) == 0
            call(0x474C0, module_id, 0, base + 0x17EA50 + 0x1D8)
            call(0x4B78C, module_id, 0, 0)
            call(0x4B910, module_id, 0, 0)
            call(0x4ADD4, module_id, 0, 0)
            polls = 0
            while True:
                call(0x4B6BC, module_id, 0, 0)
                polls += 1
                if machine.reg_read(UC_ARM64_REG_X0) == 1:
                    break
                assert polls <= busy_reads
                # The portable caller adds a bounded wait around original
                # status operation 9. Its delay policy is modeled here.
                events.append({"kind": "delay", "value": 1000})
            call(0x4B324, module_id, 0, 0)
            assert machine.reg_read(UC_ARM64_REG_X0) == 0
            cycles.append({"cycle": cycle, "idle_polls": polls,
                           "executed_instruction_count": len(visited[phase])})
        assert byte_probe_reads == lifecycle_cycles
        return {
            "status": "passed", "lk_sha256": LK_SHA256,
            "supported_input": {"get_params_raw_hex": parameters.hex(),
                                "port_initial_registers": port_initial},
            "setup_mmio_trace": {"writes": setup_writes},
            "pre_op7_mmio_sha256": {
                "dsi": hashlib.sha256(pre_display).hexdigest(),
                "mipi_tx": hashlib.sha256(pre_phy).hexdigest()},
            "mmio_trace": {"events": events},
            "checks": {"post_op7_mmio_sha256": {
                "dsi": hashlib.sha256(machine.mem_read(display, 0x20000)).hexdigest(),
                "mipi_tx": hashlib.sha256(machine.mem_read(phy, 0x1000)).hexdigest()}},
            "cycles": cycles, "byte_condition_probes_normalized_to_Read32": byte_probe_reads,
            "operation_offsets": {"configure_phy": "0x4ae90", "configure_txrx": "0x474c0", "enable": "0x4b78c",
                                  "trigger": "0x4b910", "stop_video": "0x4add4",
                                  "idle_status": "0x4b6bc", "suspend_phy": "0x4b324"},
            "instruction_ranges": [[hex(lo),hex(hi)] for lo,hi in lifecycle_ranges],
            "executed_instruction_sha256": {
                name: hashlib.sha256(b"".join(struct.pack("<I", off)+raw[off:off+4]
                                              for off in sorted(offsets))).hexdigest()
                for name,offsets in visited.items()},
            "limitations": ["MMIO, busy-bit transition and timer are fixtures; no physical link response",
                            "the outer bounded status polling policy is implemented by the C caller",
                            "one original byte-sized mode probe per cycle is normalized to Read32",
                            "no panel/domain/clock ownership is provided by this core"]}

    executed = b"".join(struct.pack("<I", offset) + raw[offset:offset + 4]
                        for offset in sorted(visited["op7"]))
    return {
        "status": "passed",
        "scope": "Original LK ARM64 get_params -> DSI_config -> DSI operation 7, RAM-backed MMIO",
        "lk_sha256": LK_SHA256,
        "supported_input": {
            "module_id": module_id, "module_record_index": dsi_module_index,
            "module_record_offset": hex(dsi_module_record_offset),
            "module_registration_pointer": hex(module_registration),
            "module_record_flags": module_flags,
            "descriptor_offset": hex(descriptor),
            "descriptor_function_pointer": hex(descriptor_function_pointer),
            "operation_index": 7, "operation_pointer_offset": "0x4ae90",
            "module_gate_switch_target": hex(module_gate_target),
            "dsi_virtual_pointer": hex(display_pointer), "dsi_physical_base": hex(display_base),
            "mipi_tx_virtual_pointer": hex(mipi_pointer), "mipi_tx_physical_base": hex(mipi_base),
            "get_params_size": len(parameters), "panel_size": [2240, 1400],
            "get_params_raw_hex": parameters.hex(),
            "port_initial_register_offsets": [hex(offset) for offset in port_initial_offsets],
            "port_initial_registers": port_initial,
            "port_initial_snapshot_stage": "after original get_params, immediately before DSI_config; all six values are the modeled MMIO seed",
            "dsi_rate_field": 733, "derived_rate_operand": 1466,
            "conditional_timing_field": {"get_params_offset": "0x240", "value": 1,
                                          "branch_pc": "0x4807c"},
            "lane_configuration_words": [0] * 6,
            "get_params_words": get_params_words,
            "port_shadow_context_offsets": [hex(off) for off in shadow_offsets],
            "port_shadow_before_op7": port_shadow,
            "mode": "source get_params/config fixture; op7 has no separate mode argument",
        },
        "execution": {
            "entry_offsets": ["0x5bc30", "0x4ab34", "0x4ae90"],
            "module_vector_and_register_consumer": "not required; original operation pointer verified in descriptor",
            "valid_input_callee_graph": [
                "0x4ae90 -> 0x4f434 -> 0x4f540 (module 0x10 DSI gate bits)",
                "0x4ae90 -> 0x47b74 -> 0x6c0b4 (MIPI sequence and delay leaf)",
                "0x4ae90 -> 0x46a30 (context-to-port configuration copy)",
                "0x4ae90 path reaches 0x4b1d4/0x4b1e0/0x4b1f4 (DSI port reset bit2 pulse)",
            ],
            "alternate_helper_0x475ec_not_taken": True,
            "op7_instruction_count": len(visited["op7"]),
            "op7_instruction_and_offset_sha256": hashlib.sha256(executed).hexdigest(),
            "instruction_ranges": [[hex(lo), hex(hi)] for lo, hi in op7_ranges],
        },
        "pre_op7_mmio_sha256": {
            "dsi": hashlib.sha256(pre_display).hexdigest(),
            "mipi_tx": hashlib.sha256(pre_phy).hexdigest(),
        },
        "setup_mmio_trace": {
            "scope": "get_params plus DSI_config before direct op7 call",
            "get_params_0x6d8_selects_helper": "0x47b74",
            "mipi_lane_words_after_config": {hex(offset): hex(value)
                                              for offset, value in setup_lane_words.items()},
            "reads": setup_reads, "writes": setup_writes,
            "delay_calls": [leaf for leaf in setup_leaves
                            if leaf["kind"] == "delay_microseconds"],
            "dsi_sha256_after_setup": hashlib.sha256(pre_display).hexdigest(),
            "mipi_tx_sha256_after_setup": hashlib.sha256(pre_phy).hexdigest(),
        },
        "mmio_trace": {"events": events, "reads": reads, "writes": writes,
                       "delay_calls": op7_delay_calls,
                       "delay_microseconds": op7_delays},
        "checks": {
            "dsi_port_reset_bit2_pulse_and_final_clear": [hex(item["value"]) for item in reset_writes],
            "dsi_port_reset_address": hex(display_pointer),
            "mipi_tx_offset_0_reset_written": False,
            "op7_bss_flags_before": {"0x17f1e4": 0, "0x17f1e8": 0},
            "op7_bss_flags_after": {"0x17f1e4": 1, "0x17f1e8": 1},
            "op7_bss_branch_effect": {
                "0x17f1e8_zero": "runs module gate dispatch before setting the flag",
                "0x17f1e4_zero": "takes first-use setup then reaches the DSI reset loop",
            },
            "fixed_lane_register_words": {hex(offset): hex(value)
                                           for offset, value in expected_lane_words.items()},
            "mipi_gate_offset_0x328_bit0_set": True,
            "other_lane_control_offsets_not_written": ["0x128", "0x228", "0x428", "0x528"],
            "rmw_changed_masks_checked": {f"{region}@{pc:#x}": hex(mask)
                                           for (region, pc), mask in rmw_masks.items()},
            "conditional_rmw_writes_skipped": {
                "branch": "MIPI helper loads x2+0xd0 (get_params+0x240); value 1 branches at 0x4807c to 0x47f24",
                "unexecuted_pc_range": ["0x48080", "0x480fc"],
                "masks": {f"{region}@{pc:#x}": hex(mask)
                          for (region, pc), mask in conditional_rmw_masks.items()},
            },
            "dsi_mmio_write_count": len(dsi_writes),
            "mipi_tx_mmio_write_count": len(mipi_writes),
            "post_op7_mmio_sha256": {
                "dsi": hashlib.sha256(post_display).hexdigest(),
                "mipi_tx": hashlib.sha256(post_phy).hexdigest(),
            },
        },
        "delay_timer_proof": lk_dsi_delay_timer_proof(raw),
        "limits": [
            "the function returns zero without proving PLL lock, powered rails, or a ready MIPI link",
            "timer calls are intercepted; the 0xCBD88 CNTVCT scale proves the arguments are microseconds, but no physical elapsed time is measured",
            "MMIO reads and writes use seeded RAM; no electrical or panel response is modeled",
            "only the source-generated module 0x10 input is run; unsupported IDs are not treated as an original rejection path",
            "this is a direct op7 call; the generic display dispatcher/framework does not call this operation",
        ],
    }


def verify_c_dsi_op7(lifecycle_cycles=0, busy_reads=0):
    """Compare production C with independently executed LK, including ordering."""
    import ctypes
    import subprocess
    import tempfile
    import sys

    source = REPO / "Silicon/MediaTek/MediaTekPkg/Library/FramebufferCoreLib/MtkDsiCore.c"
    header = REPO / "Silicon/MediaTek/MediaTekPkg/Include/Library/MtkDsiCore.h"
    identities = {str(p.relative_to(REPO)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (source, header, Path(__file__).resolve())}
    # This bridge only passes the public input/callback API. It contains no
    # register operations, expected trace, or copy of the implementation.
    bridge = r"""
#include <Library/MtkDsiCore.h>
typedef char InitialWireSize[(sizeof(MTK_DSI_PORT_INITIAL)==6U*4U)?1:-1];
typedef char ProfileWireSize[(sizeof(MTK_DSI_OP7_PROFILE)==56U)?1:-1];
int T70CompareOp7(const MTK_DSI_CORE_IO *Io, const unsigned char *Raw,
                  const unsigned *Initial, unsigned *Stats, unsigned Cycles) {
  MTK_DSI_CORE Core={0};
  MTK_DSI_OP7_PROFILE Profile={0};
  MTK_DSI_STATUS Result;
  Profile.GetParams=Raw;
  Profile.GetParamsSize=MTK_DSI_GET_PARAMS_SIZE;
  Profile.ModuleId=MTK_DSI_LK_MODULE_ID;
  Profile.Mode=0;
  Profile.ConfigWidth=MTK_DSI_LK_WIDTH;
  Profile.ConfigHeight=MTK_DSI_LK_HEIGHT;
  Profile.Initial.Reg08=Initial[0];
  Profile.Initial.Reg14=Initial[1];
  Profile.Initial.Reg18=Initial[2];
  Profile.Initial.Reg1C=Initial[3];
  Profile.Initial.Reg64=Initial[4];
  Profile.Initial.Reg200=Initial[5];
  Result=MtkDsiCoreInitialize(&Core,Io);
  if (Result==MtkDsiSuccess) Result=MtkDsiCoreRunAlongCyxOp7(&Core,&Profile);
  for (unsigned Cycle=0; Cycle<Cycles && Result==MtkDsiSuccess; ++Cycle) {
    if (Cycle) Result=MtkDsiCoreResume(&Core);
    if (Result==MtkDsiSuccess) Result=MtkDsiCoreStartVideo(&Core);
    if (Result==MtkDsiSuccess) Result=MtkDsiCoreSuspend(&Core,1000);
  }
  Stats[0]=Core.State; Stats[1]=Core.ReadCalls; Stats[2]=Core.WriteAttempts;
  Stats[3]=Core.WritesAccepted; Stats[4]=Core.DelayAttempts; Stats[5]=Core.Busy;
  return Result;
}
"""
    Read = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                            ctypes.c_uint, ctypes.POINTER(ctypes.c_uint))
    Write = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                             ctypes.c_uint, ctypes.c_uint)
    Delay = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint)

    class Io(ctypes.Structure):
        _fields_ = [("Context", ctypes.c_void_p), ("Read32", Read),
                    ("Write32", Write), ("DelayMicroseconds", Delay)]

    cases = []
    raw = LK.read_bytes()
    with tempfile.TemporaryDirectory(prefix="t70-dsi-compare-") as temporary:
        temporary = Path(temporary)
        wrapper = temporary / "bridge.c"
        wrapper.write_text(bridge)
        library = temporary / ("core.dylib" if sys.platform=="darwin" else "core.so")
        command = ["clang" if sys.platform=="darwin" else "gcc", "-std=c11",
                   "-O1", "-Wall", "-Wextra", "-Werror",
                   "-dynamiclib" if sys.platform=="darwin" else "-shared",
                   "-fPIC", "-I"+str(header.parents[1]), str(source),
                   str(wrapper), "-o", str(library)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        product = ctypes.CDLL(str(library))
        run = product.T70CompareOp7
        run.argtypes = [ctypes.POINTER(Io), ctypes.POINTER(ctypes.c_ubyte),
                        ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint), ctypes.c_uint]
        run.restype = ctypes.c_int
        product_hash = hashlib.sha256(library.read_bytes()).hexdigest()
        for seed in (0, 0xa5a5a5a5):
            original = original_lk_dsi_mipi_op7(raw, seed, lifecycle_cycles, busy_reads)
            banks = {"dsi":bytearray(struct.pack("<I",seed)*0x8000),
                     "mipi_tx":bytearray(struct.pack("<I",seed)*0x400)}
            for item in original["setup_mmio_trace"]["writes"]:
                struct.pack_into("<I", banks[item["region"]],
                                 int(item["offset"],16), item["value"])
            assert {name:hashlib.sha256(data).hexdigest() for name,data in banks.items()
                    } == original["pre_op7_mmio_sha256"], "setup reconstruction mismatch"
            events, errors = [], []
            status_reads = 0
            def address(bank, offset):
                assert bank in (0,1) and offset % 4 == 0
                name = ("dsi","mipi_tx")[bank]
                assert 0 <= offset <= len(banks[name])-4
                return name, banks[name]
            @Read
            def read(context, bank, offset, result):
                nonlocal status_reads
                try:
                    name, data = address(bank,offset)
                    if lifecycle_cycles and bank == 0 and offset == 0x1300C:
                        busy = status_reads % (busy_reads + 1) < busy_reads
                        value = struct.unpack_from("<I",data,offset)[0]
                        struct.pack_into("<I",data,offset,
                                         (value | 0x80000000) if busy else (value & 0x7fffffff))
                        status_reads += 1
                    value = struct.unpack_from("<I",data,offset)[0]
                    result[0] = value
                    events.append({"kind":"read","region":name,"offset":hex(offset),"value":value})
                    return 0
                except Exception as error:
                    errors.append(repr(error)); return 4
            @Write
            def write(context, bank, offset, value):
                try:
                    name, data = address(bank,offset)
                    struct.pack_into("<I",data,offset,value)
                    events.append({"kind":"write","region":name,"offset":hex(offset),"value":value})
                    return 0
                except Exception as error:
                    errors.append(repr(error)); return 4
            @Delay
            def delay(context, value):
                events.append({"kind":"delay","value":value})
                return 0
            parameters = bytes.fromhex(original["supported_input"]["get_params_raw_hex"])
            initial_values = original["supported_input"]["port_initial_registers"]
            assert len(parameters) == 0x758 and len(initial_values) == 6
            raw_wire = (ctypes.c_ubyte * len(parameters)).from_buffer_copy(parameters)
            initial_wire = (ctypes.c_uint * 6)(*initial_values)
            stats = (ctypes.c_uint*6)()
            io = Io(None,read,write,delay)
            result = run(ctypes.byref(io),raw_wire,initial_wire,stats,lifecycle_cycles)
            assert not errors and result==0, (seed,errors,result)
            expected = original["mmio_trace"]["events"]
            if events != expected:
                first = next((i for i,(a,b) in enumerate(zip(events,expected)) if a!=b),
                             min(len(events),len(expected)))
                raise AssertionError(("C/original op7 order mismatch",hex(seed),first,
                                      events[first:first+1],expected[first:first+1],
                                      len(events),len(expected)))
            if not lifecycle_cycles:
                assert list(stats)==[3,20,48,48,5,0], list(stats)
            else:
                assert stats[0] == 8 and stats[2] == stats[3] and stats[5] == 0, list(stats)
            final = {name:hashlib.sha256(data).hexdigest() for name,data in banks.items()}
            assert final==original["checks"]["post_op7_mmio_sha256"]
            cases.append({"seed":hex(seed),"status":"passed",
                          "all_interleaved_events_match":True,"event_count":len(events),
                          "lifecycle_cycles":lifecycle_cycles,"busy_reads_per_suspend":busy_reads,
                          "reads":stats[1],"writes":stats[2],"delay_calls":stats[4],
                          "whole_mmio_after_sha256":final,
                          "input_source":"Original 0x758-byte get_params result plus six initial DSI MMIO words read before DSI_config; captured DSI_config context and expected trace are not C inputs",
                          "get_params_sha256":hashlib.sha256(parameters).hexdigest(),
                          "initial_register_values":initial_values,
                          "original":original})
    assert identities == {str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in (source,header,Path(__file__).resolve())}
    return {"status":"passed","scope":"Native production C callbacks against exact original LK ARM64; seeded RAM-backed MMIO",
            "source_sha256":identities,"source_inputs_unchanged":True,
            "temporary_compiled_C_sha256":product_hash,"cases":cases,
            "lifecycle_cycles":lifecycle_cycles,"busy_reads_per_suspend":busy_reads,
            "byte_probe_normalization":bool(lifecycle_cycles),
            "tablet_executed":False,"phy_or_panel_ready_proved":False}


def write_lk_ovl_fixtures(output):
    raw = LK.read_bytes()
    assert len(raw) == 805888 and hashlib.sha256(raw).hexdigest() == LK_SHA256
    assert struct.unpack_from("<Q", raw, 0xBEE28)[0] == IO
    assert struct.unpack_from("<Q", raw, 0xC4AC0)[0] == BASE + 0xBEE28
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(BASE, 0x200000)
    uc.mem_write(BASE, raw)
    uc.mem_map(IO, 0x2000)
    uc.mem_map(0x70000000, 0x11000)
    trace = []

    def instruction(machine, pc, size, context):
        assert BASE + ENTRY <= pc < BASE + END, ("unselected LK instruction", hex(pc))

    def write(machine, access, address, size, value, context):
        if SP - 0x20 <= address and address + size <= SP:
            return
        assert IO <= address < IO + 0x2000 and size == 4, (hex(address), size)
        trace.append((address - IO, value))

    uc.hook_add(UC_HOOK_CODE, instruction)
    uc.hook_add(UC_HOOK_MEM_WRITE, write)
    records = []
    names = []
    for module, layers in ((5, range(4)), (6, range(2))):
        for layer, fmt, rotation, source in itertools.product(layers, FORMATS, (0, 1), (0, 1)):
            trace.clear()
            bank = (module - 5) * 0x1000
            width, height, pitch, address, src_x, src_y = 2240, 1400, 8960, 0x90000000, 32, 3
            roi = width | height << 16
            page = bytearray(0x2000)
            for off, value in ((0x0C, 0x400001), (0x20, roi), (0x24, 0x5000001), (0x2C, 1 << layer)):
                struct.pack_into("<I", page, bank + off, value)
            uc.mem_write(IO, bytes(page))
            uc.mem_write(BASE + 0x180180, struct.pack("<I", rotation))
            uc.mem_write(SP, struct.pack("<8Q", 0, 0, width, height, 0, 0, 1, 255))
            for reg, value in zip(
                (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                 UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7),
                (module, layer, source, fmt, address, src_x, src_y, pitch),
            ):
                uc.reg_write(reg, value)
            uc.reg_write(UC_ARM64_REG_SP, SP)
            uc.reg_write(UC_ARM64_REG_LR, STOP)
            uc.emu_start(BASE + ENTRY, STOP, count=1000, timeout=1000000)
            assert uc.reg_read(UC_ARM64_REG_PC) == STOP and uc.reg_read(UC_ARM64_REG_SP) == SP
            assert uc.reg_read(UC_ARM64_REG_X0) == 0
            offsets = [bank + layer * 0x20 + off for off in (0xC0, 0x30, 0x3C, 0xF40, 0x38, 0x34, 0x44)]
            assert [offset for offset, value in trace] == offsets, trace
            snapshot = bytes(uc.mem_read(IO + bank, 0x1000))
            selected = dict(trace)
            assert selected[bank + layer * 0x20 + 0xC0] == 1
            assert selected[bank + layer * 0x20 + 0x38] == roi
            accepted = fmt == 0x01002002 and not rotation and not source
            expected = (address + pitch * src_y + 4 * src_x, width, height, pitch, layer, 0x1FF)
            if accepted:
                assert selected[bank + layer * 0x20 + 0x30] == 0x21FF
                assert selected[bank + layer * 0x20 + 0xF40] == expected[0]
                assert selected[bank + layer * 0x20 + 0x44] == pitch
            records.append(struct.pack("<8I", 0x14005000 + bank, 0 if accepted else 1,
                                       *(expected if accepted else (0,) * 6)) + snapshot)
            names.append({"module": module, "layer": layer, "format": hex(fmt),
                          "rotation": rotation, "source": source, "decoder_accepts": accepted})
    path_proof = verify_lk_paths(raw)
    framework_wiring = original_lk_dsi_framework_wiring(raw)
    dsi_records, dsi_proof = [], []
    for seed in (0, 0xA5A5A5A5):
        snapshot, proof = original_lk_dsi_configuration(raw, seed)
        dsi_records.append(struct.pack("<III", seed, 2240, 1400) + snapshot)
        dsi_proof.append(proof)
    dsi_op7_c_comparison = verify_c_dsi_op7()
    dsi_lifecycle_c_comparisons = [verify_c_dsi_op7(3, waits) for waits in (0, 3)]
    payload = (b"LKOVL002" + struct.pack("<I", len(records)) + b"".join(records) +
               b"LKDSI001" + struct.pack("<I", len(dsi_records)) + b"".join(dsi_records))
    Path(output).write_bytes(payload)
    return {
        "scope": "Exact LK layer programming function in ARM64 Unicorn, fake register pages; production decoder comparison performed by caller",
        "lk_path": str(LK.relative_to(REPO.parent)), "lk_sha256": LK_SHA256,
        "original_function_offset": hex(ENTRY), "original_function_end": hex(END),
        "original_instruction_sha256": hashlib.sha256(raw[ENTRY:END]).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "path_proof": path_proof,
        "dsi_framework_wiring_proof": framework_wiring,
        "dsi_configuration_proof": dsi_proof,
        "dsi_op7_production_c_comparison": dsi_op7_c_comparison,
        "dsi_lifecycle_production_c_comparisons": dsi_lifecycle_c_comparisons,
        "calls": len(records), "accepted": sum(case["decoder_accepts"] for case in names),
        "snapshot_sha256": hashlib.sha256(payload).hexdigest(),
        "modeled": ["OVL enable/ROI/source-mask/datapath initial values", "stack arguments", "orientation global", "MMIO storage"],
        "limits": ["no live capture or clock/power/routing validation", "DMA address is not proven CPU physical", "alpha control is preserved, not made opaque", "no GOP/PE/tablet execution"],
        "cases": names,
    }


if __name__ == "__main__":
    import argparse
    import json
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    report = write_lk_ovl_fixtures(args.fixtures)
    print(json.dumps({key: value for key, value in report.items() if key != "cases"}, indent=2))
