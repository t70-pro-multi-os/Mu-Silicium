#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare stock SC8989x and HUSB311 ARM64 behavior with production data.

The charge and boost getters are compared with the portable production C. The
stock OTG enable/disable function runs with its regmap/GPIO calls intercepted.
The HUSB311 status getter runs with regmap reads supplied by a fixture, then
the original TCPM refresh/safe0V functions run against its context bytes. No
kernel, device, physical I2C bus, or tablet code runs.
"""
from pathlib import Path
import argparse
import ctypes as C
import _ctypes
import errno
import hashlib
import json
import shutil
import struct
import subprocess
import tempfile
import time
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_X0,
    UC_ARM64_REG_X1,
    UC_ARM64_REG_X2,
    UC_ARM64_REG_X3,
    UC_ARM64_REG_X8,
    UC_ARM64_REG_SP,
    UC_ARM64_REG_LR,
    UC_ARM64_REG_PC,
)

PKG = Path(__file__).resolve().parents[1]
REPO = PKG.parents[2]
BINARY = REPO.parent / "analysis/stock-binaries/vendor_boot_a_0/sc8989x_charger.ko"
SHA256 = "105dae80cf41ecc1fbd46eb400417989d1fe38aa91c486986452fcc80afbd6bd"
HUSB311_BINARY = REPO.parent / "analysis/stock-binaries/vendor_boot_a_0/tcpc_husb311.ko"
HUSB311_SHA256 = "1224c8fa0fcec8ef8979f9ad81157d2fdd27124e6d17838b5fb141e5b9a2f0df"
TCPC_CLASS_BINARY = REPO.parent / "analysis/stock-binaries/vendor_boot_a_0/tcpc_class.ko"
TCPC_CLASS_SHA256 = "7f8ecb8c32ab4049287249fd7ec677392c0af35c9a8327c931a58b97aceb42ff"
FUNCTION = "sc8989x_get_charge_stat"
BOOST_FUNCTION = "sc8989x_boost_is_enabled"
OTG_FUNCTION = "sc8989x_set_otg"
ANALYSIS_DIR = REPO.parent / "analysis"
RESULT_PATH = ANALYSIS_DIR / "stock-power-binary-tests.json"
ANALYSIS_PATH = ANALYSIS_DIR / "stock-power-binary.json"
LK_SHA256 = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"
LK_SIZE = 805888
LK_PROVIDER_INIT = 0x50C8
LK_READ_FIELD = 0x50D0
LK_GET_KEY_STATE = 0x31450
LK_PMIC_READ_LEAF = 0x5A0A8
LK_POWER_KEY_CALLER = 0x20530
LK_KEY_INPUT_TABLE = 0xBC530
LK_POWER_KEY_INPUT = 2
LK_POWER_KEY_INPUT_VALUE = 0x8
LK_POWER_FIELD_TABLE = 0xAFFD4
LK_PROVIDER_OBJECT = 0xB9448
LK_PROVIDER_GLOBAL = 0xF47A8
LK_ERROR_LOG = 0x6C1D8
LK_LOG = 0x6F520
EFFECTIVE_DTB_SHA256 = "a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391"
PMIC_KEYS_MODULE_SHA256 = "0d56fbfe6f06319cc22775c55d7b07e555bbd703574555cddd6be3bcece70279"

SHF_ALLOC = 2
SHT_NOBITS = 8
SHT_SYMTAB = 2
SHT_RELA = 4
R_AARCH64_LDST8_ABS_LO12_NC = 278
R_AARCH64_LDST16_ABS_LO12_NC = 284
R_AARCH64_LDST32_ABS_LO12_NC = 285
R_AARCH64_LDST64_ABS_LO12_NC = 286
R_AARCH64_ADR_PREL_PG_HI21 = 275
R_AARCH64_ADD_ABS_LO12_NC = 277
R_AARCH64_JUMP26 = 282
R_AARCH64_CALL26 = 283

class Sc8989xStatus(C.Structure):
    _fields_ = [
        ("PartCode", C.c_ubyte),
        ("StatusB", C.c_ubyte),
        ("StatusC", C.c_ubyte),
        ("ChargeStateCode", C.c_ubyte),
        ("VbusStateCode", C.c_ubyte),
        ("PowerGood", C.c_ubyte),
        ("Reg0CBit3", C.c_ubyte),
        ("ChargeStatus", C.c_int),
    ]

class Sc8989xBoost(C.Structure):
    _fields_ = [("PartCode", C.c_ubyte), ("BoostEnabled", C.c_ubyte)]

READ_REGS = C.CFUNCTYPE(
    C.c_int, C.c_void_p, C.c_ubyte, C.c_ubyte, C.POINTER(C.c_ubyte)
)

def align_up(value, alignment=0x1000):
    return (value + alignment - 1) & ~(alignment - 1)


def parse_arm64_elf(raw, description):
    if raw[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", raw, 18)[0] != 183:
        raise ValueError("Expected an ARM64 ELF64 little-endian " + description)
    section_table = struct.unpack_from("<Q", raw, 40)[0]
    section_width, section_count, shstr_index = struct.unpack_from("<HHH", raw, 58)
    if section_width != 64 or shstr_index >= section_count or section_table + section_width * section_count > len(raw):
        raise ValueError("Invalid ELF section table in " + description)
    sections = [
        struct.unpack_from("<IIQQQQIIQQ", raw, section_table + index * section_width)
        for index in range(section_count)
    ]

    def section_bytes(index):
        section = sections[index]
        if section[4] + section[5] > len(raw) and section[1] != SHT_NOBITS:
            raise ValueError("ELF section exceeds source bytes in " + description)
        if section[1] == SHT_NOBITS:
            return bytes(section[5])
        return raw[section[4]:section[4] + section[5]]

    name_bytes = section_bytes(shstr_index)

    def string_at(strings, offset):
        if offset >= len(strings):
            raise ValueError("Invalid ELF string offset in " + description)
        return strings[offset:].split(b"\0", 1)[0].decode()

    section_names = {index: string_at(name_bytes, section[0]) for index, section in enumerate(sections)}
    symbols = {}
    symbol_tables = {}
    for index, section in enumerate(sections):
        if section[1] != SHT_SYMTAB:
            continue
        strings = section_bytes(section[6])
        if section[5] % 24:
            raise ValueError("Invalid ELF symbol-table size in " + description)
        entries = []
        for offset in range(0, section[5], 24):
            name_offset, info, other, shndx, value, size = struct.unpack_from(
                "<IBBHQQ", section_bytes(index), offset
            )
            name = string_at(strings, name_offset)
            entry = (name, shndx, value, size)
            entries.append(entry)
            if name:
                symbols[name] = entry
        symbol_tables[index] = entries
    return {
        "raw": raw,
        "sections": sections,
        "section_names": section_names,
        "section_bytes": section_bytes,
        "symbols": symbols,
        "symbol_tables": symbol_tables,
    }


def function_relocations(elf, function):
    _, function_section, function_offset, function_size = function
    found = []
    for index, section in enumerate(elf["sections"]):
        if section[1] != SHT_RELA or section[7] != function_section:
            continue
        if section[5] % 24:
            raise ValueError("Invalid ELF relocation-table size")
        symbol_entries = elf["symbol_tables"].get(section[6])
        if symbol_entries is None:
            raise ValueError("Missing ELF relocation symbol table")
        for offset in range(0, section[5], 24):
            place, info, addend = struct.unpack_from("<QQq", elf["section_bytes"](index), offset)
            if not function_offset <= place < function_offset + function_size:
                continue
            symbol = symbol_entries[info >> 32]
            name = symbol[0] or elf["section_names"].get(symbol[1], "")
            found.append((place, info & 0xffffffff, name, addend))
    return sorted(found)


def make_module_emulator(elf, functions, stub_names, base_address):
    section_bases = {}
    next_address = base_address
    for index, section in enumerate(elf["sections"]):
        if (section[2] & SHF_ALLOC) and section[5]:
            section_bases[index] = next_address
            next_address += align_up(section[5])
    if next_address == base_address:
        raise ValueError("Selected module has no allocated ELF sections")
    stub_base = align_up(next_address + 0x10000)
    stub_addresses = {name: stub_base + index * 0x10 for index, name in enumerate(stub_names)}
    stop_address = stub_base + 0x800
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(base_address, next_address - base_address)
    for index, address in section_bases.items():
        if elf["sections"][index][1] != SHT_NOBITS:
            uc.mem_write(address, elf["section_bytes"](index))
    uc.mem_map(stub_base, 0x1000)
    return_stub = struct.pack("<II", 0x52800000, 0xD65F03C0)
    for address in stub_addresses.values():
        uc.mem_write(address, return_stub)
    ranges = [
        (function[1], function[2], function[2] + function[3])
        for function in functions
    ]

    def resolve_symbol(symbol, relocation_kind):
        name, shndx, value, _ = symbol
        if shndx:
            if shndx not in section_bases:
                raise ValueError("Relocation target is outside mapped sections: " + name)
            return section_bases[shndx] + value
        if relocation_kind in (R_AARCH64_CALL26, R_AARCH64_JUMP26):
            if name not in stub_addresses:
                raise ValueError("Unmodeled external ARM64 call: " + name)
            return stub_addresses[name]
        raise ValueError("Unmodeled external ARM64 data symbol: " + name)

    relocations_applied = 0
    for index, section in enumerate(elf["sections"]):
        if section[1] != SHT_RELA:
            continue
        target_section = section[7]
        if target_section not in section_bases:
            continue
        symbol_entries = elf["symbol_tables"].get(section[6])
        if symbol_entries is None:
            raise ValueError("Missing ELF relocation symbol table")
        for offset in range(0, section[5], 24):
            place_offset, info, addend = struct.unpack_from(
                "<QQq", elf["section_bytes"](index), offset
            )
            if not any(target_section == sec and begin <= place_offset < end for sec, begin, end in ranges):
                continue
            kind = info & 0xffffffff
            symbol = symbol_entries[info >> 32]
            target = resolve_symbol(symbol, kind) + addend
            place = section_bases[target_section] + place_offset
            instruction = struct.unpack("<I", uc.mem_read(place, 4))[0]
            if kind == R_AARCH64_ADR_PREL_PG_HI21:
                delta = (target >> 12) - (place >> 12)
                if not -(1 << 20) <= delta < (1 << 20):
                    raise ValueError("ADRP relocation out of range")
                instruction = (instruction & ~((3 << 29) | (0x7FFFF << 5))) | (
                    ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5)
                )
            elif kind == R_AARCH64_ADD_ABS_LO12_NC:
                instruction = (instruction & ~(0xFFF << 10)) | ((target & 0xFFF) << 10)
            elif kind in (
                R_AARCH64_LDST8_ABS_LO12_NC,
                R_AARCH64_LDST16_ABS_LO12_NC,
                R_AARCH64_LDST32_ABS_LO12_NC,
                R_AARCH64_LDST64_ABS_LO12_NC,
            ):
                shift = {
                    R_AARCH64_LDST8_ABS_LO12_NC: 0,
                    R_AARCH64_LDST16_ABS_LO12_NC: 1,
                    R_AARCH64_LDST32_ABS_LO12_NC: 2,
                    R_AARCH64_LDST64_ABS_LO12_NC: 3,
                }[kind]
                instruction = (instruction & ~(0xFFF << 10)) | (((target & 0xFFF) >> shift) << 10)
            elif kind in (R_AARCH64_JUMP26, R_AARCH64_CALL26):
                delta = target - place
                if (delta % 4) or not -(1 << 27) <= delta < (1 << 27):
                    raise ValueError("Branch relocation out of range")
                instruction = (instruction & ~0x3FFFFFF) | ((delta >> 2) & 0x3FFFFFF)
            else:
                raise ValueError("Unsupported selected-function relocation: " + str(kind))
            uc.mem_write(place, struct.pack("<I", instruction))
            relocations_applied += 1
    return uc, section_bases, stub_addresses, stop_address, ranges, relocations_applied


def verify_stock_tcpc_safe0v(core):
    husb_raw = HUSB311_BINARY.read_bytes()
    tcpc_raw = TCPC_CLASS_BINARY.read_bytes()
    husb_hash = hashlib.sha256(husb_raw).hexdigest()
    tcpc_hash = hashlib.sha256(tcpc_raw).hexdigest()
    if husb_hash != HUSB311_SHA256:
        raise ValueError("Stock HUSB311 module identity mismatch")
    if tcpc_hash != TCPC_CLASS_SHA256:
        raise ValueError("Stock TCPM class module identity mismatch")

    husb_elf = parse_arm64_elf(husb_raw, "HUSB311 module")
    tcpc_elf = parse_arm64_elf(tcpc_raw, "TCPM class module")
    husb_function = husb_elf["symbols"].get("husb311_get_power_status")
    tcpc_refresh = tcpc_elf["symbols"].get("tcpci_vbus_level_refresh")
    tcpc_check = tcpc_elf["symbols"].get("tcpci_check_vsafe0v")
    if husb_function is None or husb_function[1:] != (14, 0x1A3C, 0xEC):
        raise ValueError("HUSB311 power-status function symbol/extent changed")
    if tcpc_refresh is None or tcpc_refresh[1:] != (20, 0x6754, 0x8C):
        raise ValueError("TCPM VBUS refresh symbol/extent changed")
    if tcpc_check is None or tcpc_check[1:] != (20, 0x423C, 0x14):
        raise ValueError("TCPM safe0V check symbol/extent changed")
    husb_text = next((index for index, name in husb_elf["section_names"].items() if name == ".text"), None)
    tcpc_text = next((index for index, name in tcpc_elf["section_names"].items() if name == ".text"), None)
    if husb_text != husb_function[1] or tcpc_text != tcpc_refresh[1] or tcpc_text != tcpc_check[1]:
        raise ValueError("Selected status functions are not in the expected .text section")

    expected_husb_relocations = [
        (0x1A60, R_AARCH64_CALL26, "tcpc_get_dev_data", 0),
        (0x1A80, R_AARCH64_CALL26, "rt_regmap_block_read", 0),
        (0x1A9C, R_AARCH64_CALL26, "tcpc_get_dev_data", 0),
        (0x1ABC, R_AARCH64_CALL26, "rt_regmap_block_read", 0),
        (0x1B08, R_AARCH64_ADR_PREL_PG_HI21, ".rodata.str1.1", 0x5FA),
        (0x1B0C, R_AARCH64_ADD_ABS_LO12_NC, ".rodata.str1.1", 0x5FA),
        (0x1B18, R_AARCH64_CALL26, "_dev_err", 0),
        (0x1B24, R_AARCH64_CALL26, "__stack_chk_fail", 0),
    ]
    expected_refresh_relocations = [
        (0x6778, R_AARCH64_CALL26, "mutex_lock", 0),
        (0x67AC, R_AARCH64_ADR_PREL_PG_HI21, ".rodata.str1.1", 0x27EF),
        (0x67B0, R_AARCH64_ADD_ABS_LO12_NC, ".rodata.str1.1", 0x27EF),
        (0x67B8, R_AARCH64_CALL26, "pd_dbg_info", 0),
        (0x67C8, R_AARCH64_CALL26, "mutex_unlock", 0),
    ]
    husb_relocation_facts = function_relocations(husb_elf, husb_function)
    refresh_relocation_facts = function_relocations(tcpc_elf, tcpc_refresh)
    check_relocation_facts = function_relocations(tcpc_elf, tcpc_check)
    if husb_relocation_facts != expected_husb_relocations:
        raise ValueError("HUSB311 status-function relocations differ from the reviewed call graph")
    if refresh_relocation_facts != expected_refresh_relocations:
        raise ValueError("TCPM refresh relocations differ from the reviewed call graph")
    if check_relocation_facts:
        raise ValueError("TCPM safe0V check unexpectedly has relocations")

    husb_stubs = ["tcpc_get_dev_data", "rt_regmap_block_read", "_dev_err", "__stack_chk_fail"]
    husb_uc, husb_bases, husb_stub, husb_stop, husb_ranges, husb_relocations = make_module_emulator(
        husb_elf, [husb_function], husb_stubs, 0x10000000
    )
    husb_entry = husb_bases[husb_function[1]] + husb_function[2]
    husb_end = husb_entry + husb_function[3]
    canary_replacements = 0
    for begin, end in ((husb_function[2], husb_function[2] + husb_function[3]),):
        for address in range(husb_bases[husb_function[1]] + begin,
                             husb_bases[husb_function[1]] + end, 4):
            if struct.unpack("<I", husb_uc.mem_read(address, 4))[0] == 0xD5384108:
                husb_uc.mem_write(address, struct.pack("<I", 0xD2820008))
                canary_replacements += 1
    if canary_replacements != 2:
        raise ValueError("HUSB311 stack-canary access sites changed")

    bus_base = 0x40000000
    tcpc_device = bus_base + 0x1000
    # Keep the device's 0x4000-byte context disjoint from the data chain:
    # every case clears that context before executing the original getter.
    driver_data = bus_base + 0x6000
    device_data = bus_base + 0x7000
    chip_data = bus_base + 0x8000
    regmap = bus_base + 0x9000
    husb_stack = 0x30000000
    canary_base = 0x1000
    husb_uc.mem_map(bus_base, 0x10000)
    husb_uc.mem_map(husb_stack, 0x10000)
    husb_uc.mem_map(canary_base, 0x1000)
    husb_uc.mem_write(driver_data, struct.pack("<Q", device_data))
    husb_uc.mem_write(device_data + 0xB8, struct.pack("<Q", chip_data))
    husb_uc.mem_write(chip_data + 0x10, struct.pack("<Q", regmap))
    guard = 0xA5A5C3C35A5A3C3C
    husb_uc.mem_write(canary_base + 0x710, struct.pack("<Q", guard))

    cases = []
    active_husb = {}

    def fail_husb(message):
        active_husb["failures"].append(message)
        husb_uc.emu_stop()

    def on_husb_instruction(machine, address, _size, _):
        if address == husb_stop:
            machine.emu_stop()
        elif address == husb_stub["tcpc_get_dev_data"]:
            if machine.reg_read(UC_ARM64_REG_X0) != tcpc_device:
                fail_husb("HUSB311 status getter requested unexpected tcpc data")
                return
            active_husb["data_call_count"] += 1
            active_husb["call_trace"].append("tcpc_get_dev_data")
            machine.reg_write(UC_ARM64_REG_X0, driver_data)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
        elif address == husb_stub["rt_regmap_block_read"]:
            register = machine.reg_read(UC_ARM64_REG_X1) & 0xFFFFFFFF
            length = machine.reg_read(UC_ARM64_REG_X2) & 0xFFFFFFFF
            output = machine.reg_read(UC_ARM64_REG_X3)
            if (machine.reg_read(UC_ARM64_REG_X0) != regmap or length != 1 or
                    register not in active_husb["current_registers"] or
                    register in active_husb["read_trace"]):
                fail_husb("HUSB311 status getter requested an unmodeled regmap read")
                return
            expected_output = husb_stack + 0xFED0 + (4 if register == 0x97 else 0)
            if output != expected_output:
                fail_husb("HUSB311 regmap output pointer differs from its reviewed stack slot")
                return
            active_husb["read_trace"].append(register)
            active_husb["call_trace"].append("rt_regmap_block_read:" + hex(register))
            machine.mem_write(output, bytes((active_husb["current_registers"][register],)))
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
        elif address in (husb_stub["_dev_err"], husb_stub["__stack_chk_fail"]):
            fail_husb("HUSB311 error/canary path reached in success fixture")
        elif not husb_entry <= address < husb_end:
            fail_husb("HUSB311 execution reached an unmodeled address: " + hex(address))

    husb_uc.hook_add(UC_HOOK_CODE, on_husb_instruction)
    for other_bits_set in (False, True):
        for power_present in (0, 1):
            for ht_status_bit1 in (0, 1):
                raw_power_status = ((0xFB if other_bits_set else 0x00) | (power_present << 2))
                raw_ht_status = ((0xFD if other_bits_set else 0x00) | (ht_status_bit1 << 1))
                active_husb.update({
                    "current_registers": {0x1E: raw_power_status, 0x97: raw_ht_status},
                    "read_trace": [],
                    "failures": [],
                    "data_call_count": 0,
                    "call_trace": [],
                })
                husb_uc.mem_write(tcpc_device, bytes(0x4000))
                husb_uc.mem_write(husb_stack, bytes(0x10000))
                husb_uc.mem_write(canary_base + 0x710, struct.pack("<Q", guard))
                husb_uc.reg_write(UC_ARM64_REG_X0, tcpc_device)
                husb_uc.reg_write(UC_ARM64_REG_SP, husb_stack + 0xFF00)
                husb_uc.reg_write(UC_ARM64_REG_LR, husb_stop)
                husb_uc.emu_start(husb_entry, husb_stop, timeout=100000, count=1000)
                if active_husb["failures"]:
                    raise RuntimeError(active_husb["failures"][-1])
                if husb_uc.reg_read(UC_ARM64_REG_PC) != husb_stop:
                    raise RuntimeError("HUSB311 status getter did not return")
                expected_husb_trace = [
                    "tcpc_get_dev_data", "rt_regmap_block_read:0x1e",
                    "tcpc_get_dev_data", "rt_regmap_block_read:0x97",
                ]
                if (active_husb["read_trace"] != [0x1E, 0x97] or
                        active_husb["data_call_count"] != 2 or
                        active_husb["call_trace"] != expected_husb_trace):
                    raise RuntimeError("HUSB311 raw status read order/call count changed")
                raw_power_bool = husb_uc.mem_read(tcpc_device + 0x3343, 1)[0]
                raw_ht_bool = husb_uc.mem_read(tcpc_device + 0x3342, 1)[0]
                if raw_power_bool != ((raw_power_status >> 2) & 1):
                    raise AssertionError("HUSB311 POWER_STATUS bit 2 context mapping changed")
                if raw_ht_bool != ((raw_ht_status >> 1) & 1):
                    raise AssertionError("HUSB311 HT_STATUS bit 1 context mapping changed")
                cases.append({
                    "raw_power_status": hex(raw_power_status),
                    "raw_ht_status_reg97": hex(raw_ht_status),
                    "power_status_bit2": raw_power_bool,
                    "ht_status_bit1": raw_ht_bool,
                    "other_bits_set": other_bits_set,
                    "husb311_regmap_read_order": [hex(value) for value in active_husb["read_trace"]],
                    "husb311_call_trace": active_husb["call_trace"],
                })

    tcpm_stubs = ["mutex_lock", "pd_dbg_info", "mutex_unlock"]
    tcpm_uc, tcpm_bases, tcpm_stub, tcpm_stop, tcpm_ranges, tcpm_relocations = make_module_emulator(
        tcpc_elf, [tcpc_refresh, tcpc_check], tcpm_stubs, 0x20000000
    )
    tcpm_entry = tcpm_bases[tcpc_refresh[1]] + tcpc_refresh[2]
    tcpm_end = tcpm_entry + tcpc_refresh[3]
    check_entry = tcpm_bases[tcpc_check[1]] + tcpc_check[2]
    check_end = check_entry + tcpc_check[3]
    log_section = next((index for index, name in tcpc_elf["section_names"].items()
                        if name == ".rodata.str1.1"), None)
    if log_section is None or 0x27EF >= len(tcpc_elf["section_bytes"](log_section)):
        raise ValueError("TCPM diagnostic string relocation is outside its section")
    log_string = tcpm_bases[log_section] + 0x27EF
    tcpm_data = 0x50000000
    tcpc_context = tcpm_data + 0x1000
    tcpm_stack = 0x60000000
    tcpm_uc.mem_map(tcpm_data, 0x10000)
    tcpm_uc.mem_map(tcpm_stack, 0x10000)
    safe_cases = []
    active_tcpm = {}

    def fail_tcpm(message):
        active_tcpm["failures"].append(message)
        tcpm_uc.emu_stop()

    def on_tcpm_instruction(machine, address, _size, _):
        if address == tcpm_stop:
            machine.emu_stop()
        elif address == tcpm_stub["mutex_lock"]:
            if active_tcpm["lock_held"] or machine.reg_read(UC_ARM64_REG_X0) != tcpc_context + 0x2098:
                fail_tcpm("TCPM refresh mutex model saw unexpected ownership/pointer")
                return
            active_tcpm["lock_held"] = True
            active_tcpm["call_trace"].append("mutex_lock")
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
        elif address == tcpm_stub["pd_dbg_info"]:
            if (not active_tcpm["lock_held"] or machine.reg_read(UC_ARM64_REG_X0) != log_string or
                    (machine.reg_read(UC_ARM64_REG_X1) & 0xFFFFFFFF) != 2):
                fail_tcpm("TCPM diagnostic call arguments differ from reviewed code")
                return
            active_tcpm["call_trace"].append("pd_dbg_info")
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
        elif address == tcpm_stub["mutex_unlock"]:
            if not active_tcpm["lock_held"] or machine.reg_read(UC_ARM64_REG_X0) != tcpc_context + 0x2098:
                fail_tcpm("TCPM refresh mutex unlock did not match its lock")
                return
            active_tcpm["lock_held"] = False
            active_tcpm["call_trace"].append("mutex_unlock")
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
        elif not (tcpm_entry <= address < tcpm_end or check_entry <= address < check_end):
            fail_tcpm("TCPM execution reached an unmodeled address: " + hex(address))

    tcpm_uc.hook_add(UC_HOOK_CODE, on_tcpm_instruction)
    core.SpHusb311VbusSafe0V.argtypes = [C.c_ubyte, C.c_ubyte]
    core.SpHusb311VbusSafe0V.restype = C.c_int
    for item in cases:
        raw_power_status = int(item["raw_power_status"], 16)
        raw_ht_status = int(item["raw_ht_status_reg97"], 16)
        power_bool = item["power_status_bit2"]
        ht_bool = item["ht_status_bit1"]
        tcpm_uc.mem_write(tcpc_context, bytes(0x4000))
        tcpm_uc.mem_write(tcpc_context + 0x3340, b"\xA5")
        tcpm_uc.mem_write(tcpc_context + 0x3343, bytes((power_bool,)))
        tcpm_uc.mem_write(tcpc_context + 0x3342, bytes((ht_bool,)))
        active_tcpm.update({"lock_held": False, "call_trace": [], "failures": []})
        tcpm_uc.reg_write(UC_ARM64_REG_X0, tcpc_context)
        tcpm_uc.reg_write(UC_ARM64_REG_SP, tcpm_stack + 0xFF00)
        tcpm_uc.reg_write(UC_ARM64_REG_LR, tcpm_stop)
        tcpm_uc.emu_start(tcpm_entry, tcpm_stop, timeout=100000, count=1000)
        if active_tcpm["failures"]:
            raise RuntimeError(active_tcpm["failures"][-1])
        if tcpm_uc.reg_read(UC_ARM64_REG_PC) != tcpm_stop or active_tcpm["lock_held"]:
            raise RuntimeError("TCPM VBUS refresh did not return with mutex released")
        expected_trace = ["mutex_lock"]
        if power_bool and ht_bool:
            expected_trace.append("pd_dbg_info")
        expected_trace.append("mutex_unlock")
        if active_tcpm["call_trace"] != expected_trace:
            raise AssertionError("TCPM refresh call sequence changed: " + repr(active_tcpm["call_trace"]))
        cached = tcpm_uc.mem_read(tcpc_context + 0x3340, 1)[0]
        if power_bool:
            expected_low_bits = 2
        elif ht_bool:
            expected_low_bits = 0
        else:
            expected_low_bits = 1
        expected_cached = (0xA5 & 0xFC) | expected_low_bits
        if cached != expected_cached:
            raise AssertionError(
                f"TCPM refresh cache mismatch: raw={(power_bool, ht_bool)}, "
                f"got={hex(cached)}, expected={hex(expected_cached)}"
            )

        tcpm_uc.reg_write(UC_ARM64_REG_X0, tcpc_context)
        tcpm_uc.reg_write(UC_ARM64_REG_SP, tcpm_stack + 0xFF00)
        tcpm_uc.reg_write(UC_ARM64_REG_LR, tcpm_stop)
        tcpm_uc.emu_start(check_entry, tcpm_stop, timeout=100000, count=100)
        if tcpm_uc.reg_read(UC_ARM64_REG_PC) != tcpm_stop:
            raise RuntimeError("TCPM safe0V predicate did not return")
        stock_safe = tcpm_uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF
        portable_safe = core.SpHusb311VbusSafe0V(raw_power_status, raw_ht_status)
        if stock_safe not in (0, 1) or portable_safe not in (0, 1):
            raise AssertionError("Stock or portable safe0V result is not boolean")
        if stock_safe != portable_safe:
            raise AssertionError(
                "Portable safe0V predicate differs from original TCPM: "
                f"raw={(hex(raw_power_status), hex(raw_ht_status))}, "
                f"stock={stock_safe}, portable={portable_safe}"
            )
        safe_cases.append({
            **item,
            "cached_status_0x3340": hex(cached),
            "refresh_call_trace": active_tcpm["call_trace"],
            "stock_tcpci_check_vsafe0v": stock_safe,
            "portable_SpHusb311VbusSafe0V": portable_safe,
            "matches": True,
        })

    return {
        "status": "passed",
        "husb311_binary": str(HUSB311_BINARY.relative_to(REPO.parent)),
        "husb311_sha256": HUSB311_SHA256,
        "husb311_function": {"name": "husb311_get_power_status", "text_offset": "0x1a3c", "file_offset": hex(husb_elf["sections"][husb_text][4] + 0x1A3C), "bytes": 0xEC},
        "husb311_mapping": {
            "raw_register_order": ["POWER_STATUS(0x1e)", "HT_STATUS(0x97)"],
            "ctx_offsets": {"power_status_bit2": "0x3343", "ht_status_bit1": "0x3342"},
            "raw_bits_executed": True,
            "successful_cases": len(cases),
            "relocations_applied": husb_relocations,
            "function_relocations": [
                {"text_offset": hex(place), "type": kind, "target": target, "addend": hex(addend)}
                for place, kind, target, addend in husb_relocation_facts
            ],
            "canary_fixture_replacements": canary_replacements,
        },
        "tcpm_class_binary": str(TCPC_CLASS_BINARY.relative_to(REPO.parent)),
        "tcpm_class_sha256": TCPC_CLASS_SHA256,
        "tcpm_functions": {
            "tcpci_vbus_level_refresh": {"text_offset": "0x6754", "file_offset": hex(tcpc_elf["sections"][tcpc_text][4] + 0x6754), "bytes": 0x8C},
            "tcpci_check_vsafe0v": {"text_offset": "0x423c", "file_offset": hex(tcpc_elf["sections"][tcpc_text][4] + 0x423C), "bytes": 0x14},
        },
        "tcpm_relocations_applied": tcpm_relocations,
        "tcpm_function_relocations": {
            "tcpci_vbus_level_refresh": [
                {"text_offset": hex(place), "type": kind, "target": target, "addend": hex(addend)}
                for place, kind, target, addend in refresh_relocation_facts
            ],
            "tcpci_check_vsafe0v": [
                {"text_offset": hex(place), "type": kind, "target": target, "addend": hex(addend)}
                for place, kind, target, addend in check_relocation_facts
            ],
        },
        "cases": safe_cases,
        "matches": len(safe_cases),
        "external_call_models": {
            "husb311_get_power_status": [
                "tcpc_get_dev_data returns the fixed fixture driver-data chain",
                "rt_regmap_block_read returns only the supplied one-byte 0x1e/0x97 register values",
                "_dev_err and __stack_chk_fail reject the run if reached",
            ],
            "tcpci_vbus_level_refresh": [
                "mutex_lock/unlock model an uncontended, balanced lock on ctx+0x2098",
                "pd_dbg_info is an argument-checked no-op log sink",
                "tcpci_check_vsafe0v has no external calls or relocations",
            ],
            "not_executed": ["physical I2C/HUSB transport", "other kernel functions"],
        },
    }


def verify_lk_power_key(lk_path):
    lk_path = Path(lk_path).expanduser().resolve()
    raw = lk_path.read_bytes()
    lk_hash = hashlib.sha256(raw).hexdigest()
    if lk_hash != LK_SHA256 or len(raw) != LK_SIZE:
        raise ValueError("Stock LK identity or size mismatch")

    expected_bytes = {
        0x50C8: bytes.fromhex("001c5a1012b10014"),
        0x3147C: bytes.fromhex("090940f9ff0b0079e1130091080140f923094079220540792001407900013fd6"),
        0x50E0: bytes.fromhex("f503032af403022af30301aae103002aa3730091e0031f2ae2031f2aeb530194"),
        0x5104: bytes.fromhex("a83b4079a90e4092f50b40f90825c91a0801140a68020079"),
        0x20544: bytes.fromhex("69ff4d10335960b87f1e0171c9000054"),
        0x20568: bytes.fromhex("e803002a1f200071e0031f2a28ffff54290080526a2080522921c81a3f010a6ac10100540a0980523f010a6a20050054"),
        0x20640: bytes.fromhex("e0031f2a834300941f040071e0031f2a41f8ff54"),
        0x20668: bytes.fromhex("ae3b019420008052baffff17"),
    }
    for offset, expected in expected_bytes.items():
        if raw[offset:offset + len(expected)] != expected:
            raise ValueError("Stock LK power-key code changed at " + hex(offset))

    provider_first, provider_second, provider_table = struct.unpack_from(
        "<QQQ", raw, LK_PROVIDER_OBJECT
    )
    if (provider_first, provider_second, provider_table) != (
        0xFFFF000050F050D0, 0xFFFF000050F05140, 0xFFFF000050FAFFD4
    ):
        raise ValueError("Stock LK PMIC reader object pointers changed")
    field_tuple = struct.unpack_from("<HHH", raw, LK_POWER_FIELD_TABLE)
    if field_tuple != (0x28, 1, 1):
        raise ValueError("Stock LK power-key field tuple changed")
    if struct.unpack_from("<I", raw, LK_KEY_INPUT_TABLE + LK_POWER_KEY_INPUT * 4)[0] != LK_POWER_KEY_INPUT_VALUE:
        raise ValueError("Stock LK key index 2 mapping changed")

    image_base = 0x10000000
    image_size = align_up(max(len(raw), LK_PROVIDER_GLOBAL + 8))
    stop_address = image_base + image_size + 0x1000
    stack_top = image_base + image_size - 0x1000
    output_address = image_base + 0xE0000
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(image_base, image_size)
    uc.mem_write(image_base, raw)
    uc.mem_map(stop_address, 0x1000)
    uc.mem_write(stop_address, struct.pack("<I", 0xD65F03C0))

    # The original image contains absolute pointers for its fixed LK load
    # address. Relocate only these data pointers for the in-memory fixture.
    uc.mem_write(image_base + LK_PROVIDER_OBJECT,
                 struct.pack("<Q", image_base + LK_READ_FIELD))
    uc.mem_write(image_base + LK_PROVIDER_OBJECT + 0x10,
                 struct.pack("<Q", image_base + LK_POWER_FIELD_TABLE))

    state = {
        "raw_value": 0,
        "read_status": 0,
        "allowed": [],
        "leaf_calls": [],
        "logs": [],
        "error_log_seen": False,
        "expect_error_log": False,
        "unmodeled": None,
    }

    def reject(message, machine):
        state["unmodeled"] = message
        machine.emu_stop()

    def on_instruction(machine, address, _size, _user_data):
        if address == image_base + LK_PMIC_READ_LEAF:
            mode = machine.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF
            register = machine.reg_read(UC_ARM64_REG_X1) & 0xFFFFFFFF
            value_arg = machine.reg_read(UC_ARM64_REG_X2) & 0xFFFFFFFF
            output = machine.reg_read(UC_ARM64_REG_X3)
            if mode != 0 or register != 0x28 or value_arg != 0:
                reject("LK PMIC read leaf received an unexpected call", machine)
                return
            if output < image_base or output + 2 > image_base + image_size:
                reject("LK PMIC read leaf output is outside mapped memory", machine)
                return
            state["leaf_calls"].append({
                "register": hex(register),
                "raw_u16": hex(state["raw_value"]),
                "status": state["read_status"],
            })
            if state["read_status"] == 0:
                machine.mem_write(output, struct.pack("<H", state["raw_value"]))
            machine.reg_write(UC_ARM64_REG_X0, state["read_status"] & 0xFFFFFFFF)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return

        if address == image_base + LK_LOG:
            format_address = machine.reg_read(UC_ARM64_REG_X0)
            state["logs"].append({
                "format_offset": hex(format_address - image_base)
                if image_base <= format_address < image_base + len(raw)
                else hex(format_address),
                "line": machine.reg_read(UC_ARM64_REG_X2) & 0xFFFFFFFF,
            })
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return

        if address == image_base + LK_ERROR_LOG:
            if state["expect_error_log"]:
                state["error_log_seen"] = True
                machine.emu_stop()
            else:
                reject("Unexpected LK PMIC error-log path", machine)
            return

        offset = address - image_base
        if not any(begin <= offset < end for begin, end in state["allowed"]):
            reject("LK execution reached an unmodeled address: " + hex(offset), machine)

    uc.hook_add(UC_HOOK_CODE, on_instruction)

    def run_lk(entry, registers, raw_value=0, read_status=0,
               allowed=(), expect_error_log=False):
        state["raw_value"] = raw_value
        state["read_status"] = read_status
        state["allowed"] = list(allowed)
        state["leaf_calls"] = []
        state["logs"] = []
        state["error_log_seen"] = False
        state["expect_error_log"] = expect_error_log
        state["unmodeled"] = None
        uc.reg_write(UC_ARM64_REG_SP, stack_top)
        uc.reg_write(UC_ARM64_REG_LR, stop_address)
        uc.reg_write(UC_ARM64_REG_X0, 0)
        uc.reg_write(UC_ARM64_REG_X1, 0)
        uc.reg_write(UC_ARM64_REG_X2, 0)
        uc.reg_write(UC_ARM64_REG_X3, 0)
        for register, value in registers.items():
            uc.reg_write(register, value & 0xFFFFFFFFFFFFFFFF)
        uc.emu_start(image_base + entry, stop_address, timeout=100000, count=5000)
        if state["unmodeled"] is not None:
            raise RuntimeError(state["unmodeled"])
        pc = uc.reg_read(UC_ARM64_REG_PC)
        if state["error_log_seen"]:
            if not expect_error_log or pc != image_base + LK_ERROR_LOG:
                raise RuntimeError("LK read-error fixture stopped at an unexpected point")
        elif pc != stop_address:
            raise RuntimeError("Stock LK function did not return to the fixture sentinel")
        return {
            "return_u32": uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF,
            "leaf_calls": list(state["leaf_calls"]),
            "logs": list(state["logs"]),
            "error_log_seen": state["error_log_seen"],
            "pc": hex(pc - image_base) if image_base <= pc < image_base + image_size else hex(pc),
        }

    initializer = run_lk(
        LK_PROVIDER_INIT, {},
        allowed=((0x50C8, 0x50D0), (0x31514, 0x31544)),
    )
    provider_value = struct.unpack_from(
        "<Q", uc.mem_read(image_base + LK_PROVIDER_GLOBAL, 8)
    )[0]
    if initializer["return_u32"] != 0 or provider_value != image_base + LK_PROVIDER_OBJECT:
        raise RuntimeError("Stock LK PMIC reader registration did not complete")

    callback_ranges = ((0x50D0, 0x5140),)
    getter_ranges = ((0x31450, 0x31514),) + callback_ranges
    caller_ranges = (
        (0x20530, 0x20568), (0x20568, 0x20598), (0x20638, 0x20674),
        (0x31450, 0x31514), (0x50D0, 0x5140),
    )

    def run_field(raw_value):
        uc.mem_write(output_address, b"\xa5\x5a")
        result = run_lk(
            LK_READ_FIELD,
            {UC_ARM64_REG_X0: 0x28, UC_ARM64_REG_X1: output_address,
             UC_ARM64_REG_X2: 1, UC_ARM64_REG_X3: 1},
            raw_value=raw_value, allowed=callback_ranges,
        )
        field_value = struct.unpack("<H", uc.mem_read(output_address, 2))[0]
        if result["return_u32"] != 0 or result["leaf_calls"] != [{
            "register": "0x28", "raw_u16": hex(raw_value), "status": 0,
        }]:
            raise RuntimeError("Stock LK field callback returned an unexpected result")
        return field_value

    def run_getter(raw_value):
        result = run_lk(
            LK_GET_KEY_STATE,
            {UC_ARM64_REG_X0: 0, UC_ARM64_REG_X1: output_address},
            raw_value=raw_value, allowed=getter_ranges,
        )
        if len(result["leaf_calls"]) != 1:
            raise RuntimeError("Stock LK key getter did not perform exactly one PMIC read")
        return result

    def run_power_caller(raw_value):
        result = run_lk(
            LK_POWER_KEY_CALLER, {UC_ARM64_REG_X0: LK_POWER_KEY_INPUT},
            raw_value=raw_value, allowed=caller_ranges,
        )
        expected_field = (raw_value >> 1) & 1
        expected_pressed = int(expected_field == 0)
        if result["return_u32"] != expected_pressed or len(result["leaf_calls"]) != 1:
            raise RuntimeError("Stock LK power-key caller inversion differs from the field")
        if len(result["logs"]) != expected_pressed:
            raise RuntimeError("Stock LK power-key press log differs from the caller result")
        if expected_pressed and result["logs"][0]["format_offset"] != "0xa2a7f":
            raise RuntimeError("Stock LK power-key caller reached a different log string")
        return result

    basic_cases = []
    for raw_value in range(4):
        field_value = run_field(raw_value)
        getter = run_getter(raw_value)
        caller = run_power_caller(raw_value)
        expected_field = (raw_value >> 1) & 1
        expected_pressed = int(expected_field == 0)
        if field_value != expected_field or getter["return_u32"] != expected_pressed:
            raise AssertionError("Stock LK bit1 extraction differs for raw sample " + hex(raw_value))
        basic_cases.append({
            "raw_u16": hex(raw_value),
            "field_value": field_value,
            "getter_pressed": bool(getter["return_u32"]),
            "caller_pressed": bool(caller["return_u32"]),
        })

    bit_sweep = []
    for bit in range(16):
        for raw_value, label in ((1 << bit, "one_hot"), (0xFFFF ^ (1 << bit), "all_ones_except_bit")):
            field_value = run_field(raw_value)
            expected_field = (raw_value >> 1) & 1
            if field_value != expected_field:
                raise AssertionError("Stock LK independent bit sweep failed at bit " + str(bit))
            bit_sweep.append({"pattern": label, "bit": bit, "raw_u16": hex(raw_value),
                              "field_value": field_value})

    invalid_getter = run_lk(
        LK_GET_KEY_STATE, {UC_ARM64_REG_X0: 3, UC_ARM64_REG_X1: output_address},
        allowed=getter_ranges,
    )
    if invalid_getter["return_u32"] != 0xFFFFFFF8 or invalid_getter["leaf_calls"]:
        raise RuntimeError("Stock LK invalid key selector error return changed")

    saved_provider = bytes(uc.mem_read(image_base + LK_PROVIDER_GLOBAL, 8))
    uc.mem_write(image_base + LK_PROVIDER_GLOBAL, bytes(8))
    unregistered_getter = run_lk(
        LK_GET_KEY_STATE, {UC_ARM64_REG_X0: 0, UC_ARM64_REG_X1: output_address},
        allowed=getter_ranges,
    )
    uc.mem_write(image_base + LK_PROVIDER_GLOBAL, saved_provider)
    if unregistered_getter["return_u32"] != 0xFFFFFFDA or unregistered_getter["leaf_calls"]:
        raise RuntimeError("Stock LK unregistered-reader error return changed")

    uc.mem_write(output_address, b"\x34\x12")
    read_failure = run_lk(
        LK_READ_FIELD,
        {UC_ARM64_REG_X0: 0x28, UC_ARM64_REG_X1: output_address,
         UC_ARM64_REG_X2: 1, UC_ARM64_REG_X3: 1},
        raw_value=0, read_status=1, allowed=callback_ranges,
        expect_error_log=True,
    )
    if (not read_failure["error_log_seen"] or
            uc.mem_read(output_address, 2) != b"\x34\x12" or
            len(read_failure["leaf_calls"]) != 1):
        raise RuntimeError("Stock LK PMIC read-failure path changed")

    def relative_path(path):
        try:
            return str(path.relative_to(REPO.parent))
        except ValueError:
            return str(path)

    return {
        "schema": 1,
        "status": "passed",
        "mode": "buttons_lk",
        "scope": "Original LK power-key field reader and caller with only the PWRAP read leaf replaced by supplied raw U16 fixtures",
        "binary": relative_path(lk_path),
        "binary_sha256": lk_hash,
        "static_evidence": {
            "provider_initializer": {"text_offset": hex(LK_PROVIDER_INIT), "branches_to": "0x31514",
                                     "provider_object_offset": hex(LK_PROVIDER_OBJECT),
                                     "registered_global_offset": hex(LK_PROVIDER_GLOBAL)},
            "field_tuple": {"file_offset": hex(LK_POWER_FIELD_TABLE),
                            "raw_bytes": raw[LK_POWER_FIELD_TABLE:LK_POWER_FIELD_TABLE + 6].hex(" "),
                            "register": "0x28", "mask": "0x1", "shift": 1,
                            "extraction": "(raw_u16 >> 1) & 1"},
            "getter": {"text_offset": hex(LK_GET_KEY_STATE), "key_index": 0,
                       "table_offsets_loaded": ["+0x0", "+0x2", "+0x4"],
                       "returns_pressed_when_field_is_zero": True},
            "read_field_callback": {"text_offset": hex(LK_READ_FIELD),
                                    "pmic_read_leaf_text_offset": hex(LK_PMIC_READ_LEAF),
                                    "press_log_string_file_offset": "0xa2a7f",
                                    "caller_returns_pressed_when_getter_returns_one": True},
            "power_key_caller": {"text_offset": hex(LK_POWER_KEY_CALLER),
                                 "input_table_offset": hex(LK_KEY_INPUT_TABLE),
                                 "input_index": LK_POWER_KEY_INPUT,
                                 "mapped_input_value": hex(LK_POWER_KEY_INPUT_VALUE),
                                 "dispatch_path": "input 2 passes the bit tests and reaches getter 0x31450 at 0x20640",
                                 "caller_result_and_press_log_executed": True},
            "module_release_irq_fallback": {
                "module_sha256": PMIC_KEYS_MODULE_SHA256,
                "mt6366_match": {"compatible_rodata_offset": "0x360",
                                  "table_relocation_offset": "0x3e0",
                                  "table_rodata_offset": "0x828",
                                  "first_record": "28 00 00 00 01 00 00 00 10 09 00 00 00 00 00 00"},
                "mt6358_match": {"compatible_rodata_offset": "0x420",
                                  "table_relocation_offset": "0x4a8",
                                  "table_rodata_offset": "0x870",
                                  "first_record": "28 00 00 00 01 00 00 00 10 09 00 00 00 00 00 00"},
                "handler_text_offset": "0xaf8",
                "raw_and_text_offset": "0xb30",
                "fallback_condition": "release_irq <= 0; this path reads reg 0x28 and tests raw & mask without a shift",
                "t70_power_release_resource_index": 2,
                "t70_reachability": "not taken when the MT6366 power release IRQ resource 2 is positive; the probe requests release IRQ at offset 2 + power-child ordinal 0",
                "observation_limit": "static branch/resource evidence only; no physical IRQ was observed",
            },
        },
        "dt_irq_context": {
            "effective_dtb": "analysis/stock-fdt/effective.dtb",
            "effective_dtb_sha256": EFFECTIVE_DTB_SHA256,
            "pmic_node": "/soc/pwrap@10026000/mt6366",
            "keys_node": "/soc/pwrap@10026000/mt6366/mt6366keys",
            "children_in_order": ["power", "home"],
            "parent_interrupt": {"controller": "pio", "raw_cells": [137, 4, 137, 0],
                                  "interpretation": "EINT 137; the separate PWRAP GIC interrupt is SPI 252"},
            "eint_alias": {"physical_gpio137": "NO_EINT_SUPPORT", "virtual_gpio199": "EINT 137"},
            "source_locations": [
                "kernel-reference/mindone/dts/mt6789-pmic.dtsi:5-18,141-153",
                "kernel-reference/mindone/modules/pinctrl_mediatek/pinctrl-mtk-mt6789.h:1269-1272,1713",
            ],
        },
        "execution": {
            "initializer_executed": True,
            "lk_getter_executed": True,
            "lk_power_caller_executed": True,
            "leaf_model": "Only LK offset 0x5a0a8 is intercepted; it returns the case's raw U16 and status.",
            "logging_model": "LK log sinks are intercepted; only arguments are captured.",
            "not_executed": ["physical PWRAP MMIO/I2C", "PMIC hardware", "Linux kernel or module", "LK boot", "device key or wake event"],
        },
        "raw_0_to_3_cases": basic_cases,
        "independent_bit_sweep": {"cases": bit_sweep, "count": len(bit_sweep)},
        "error_cases": {
            "invalid_key_index": {"input": 3, "return_u32": hex(invalid_getter["return_u32"]),
                                  "signed_return": -8, "bus_reads": 0},
            "unregistered_reader": {"input": 0, "return_u32": hex(unregistered_getter["return_u32"]),
                                    "signed_return": -38, "bus_reads": 0},
            "pwrap_read_failure": {
                "leaf_status_fixture": {
                    "value": 1,
                    "meaning": "synthetic nonzero used only to enter the original error branch; not a claimed hardware status",
                },
                "error_log_seen": read_failure["error_log_seen"],
                "output_unchanged": True,
                "return_value_claimed": False,
                "note": "The fixture stops at the original error logger entry; it does not invent a callback return after that branch.",
            },
        },
        "checks": 4 + len(bit_sweep) + 2 + 1,
    }



def verify_stock_panel_lifecycle():
    """Execute the pinned shipping panel functions and compare production C.

    Only Linux leaf services (GPIO, logging, delay and work scheduling) are
    fixtures. Original function bodies, context offsets, branches and loops
    execute. Scheduler time is advanced by the observed ticks/CONFIG_HZ; no
    physical timer, interrupt, DSI host, Linux kernel or tablet runs here.
    """
    panel_path = REPO.parent / (
        "analysis/stock-binaries/vendor_boot_a_0/"
        "panel-along_cyx_cv101wu_lh1038b.ko"
    )
    panel_sha = "28a4c73d1929f1bf53891c6a92340fe8b9904d7c3bda89f21be63c28e45056f3"
    raw = panel_path.read_bytes()
    if len(raw) != 98936 or hashlib.sha256(raw).hexdigest() != panel_sha:
        raise ValueError("Shipping panel module identity mismatch")
    config_path = REPO.parent / "analysis/stock-kernel.config"
    config_lines = config_path.read_text().splitlines()
    if [line for line in config_lines if line.startswith("CONFIG_HZ=")] != ["CONFIG_HZ=250"]:
        raise ValueError("Shipping kernel CONFIG_HZ changed")
    hz = 250
    elf = parse_arm64_elf(raw, "shipping panel module")
    function_sizes = {
        "jdi_prepare": (0x608, 0xc8),
        "jdi_enable": (0x6d4, 0xa0),
        "jdi_disable": (0x778, 0xc0),
        "jdi_unprepare": (0x83c, 0x7c),
        "lcm_power_ctrl": (0x960, 0x50c),
        "jdi_power_delay_work": (0x8c, 0x68),
    }
    functions = []
    for name, bounds in function_sizes.items():
        function = elf["symbols"][name]
        if function[2:] != bounds:
            raise ValueError("Shipping panel function bounds changed: " + name)
        functions.append(function)
    text_index = functions[0][1]
    text_data = elf["section_bytes"](text_index)
    strings = elf["section_bytes"](next(
        index for index, name in elf["section_names"].items()
        if name == ".rodata.str1.1"
    ))
    # Name ADRP/ADD relocations and context stores in the actual probe bind
    # these descriptors. These are identities, not a replayed expected trace.
    probe_bindings = (
        ("bl_power", 15, 0x88, 0x31d, 0x230, 0x248),
        ("edp_en", 37, 0x90, 0x316, 0x258, 0x270),
        ("edp_rst", 43, 0x98, 0x3ee, 0x280, 0x298),
        ("vdd33", 49, 0xa0, 0xf0, 0x2a8, 0x2c0),
        ("vdd18", 155, 0xa8, 0xfc, 0x2d0, 0x2e8),
        ("vdd12", 36, 0xb0, 0xf6, 0x2f8, 0x310),
        ("avdd", 3, 0xb8, 0x3cb, 0x320, 0x338),
        ("avee", 153, 0xc0, 0x349, 0x348, 0x360),
        ("reset", 85, 0xc8, 0x188, 0x370, 0x388),
    )
    probe_relocs = function_relocations(elf, elf["symbols"]["jdi_probe"])
    for name, pin, context_offset, string_offset, name_site, store_site in probe_bindings:
        if strings[string_offset:].split(b"\0", 1)[0].decode() != name:
            raise ValueError("Panel GPIO descriptor name changed")
        for kind, site in ((R_AARCH64_ADR_PREL_PG_HI21, name_site),
                           (R_AARCH64_ADD_ABS_LO12_NC, name_site + 4)):
            if (site, kind, ".rodata.str1.1", string_offset) not in probe_relocs:
                raise ValueError("Panel probe descriptor relocation changed")
        instruction = struct.unpack_from("<I", text_data, store_site)[0]
        expected = 0xf9000000 | ((context_offset // 8) << 10) | (20 << 5)
        if instruction != expected:
            raise ValueError("Panel probe descriptor context store changed")
    # Work is embedded at panel+0xd8 = ctx+0xe0. Its ctx-relative -0x50
    # load therefore selects edp_en at ctx+0x90, not edp_rst at ctx+0x98.
    if struct.unpack_from("<I", text_data, 0xb4)[0] != 0xf85b0273:
        raise ValueError("Panel delayed-work descriptor load changed")

    U32 = C.c_uint
    P32 = C.POINTER(U32)
    VALIDATE = C.CFUNCTYPE(C.c_int, C.c_void_p, U32, C.c_int)
    READ = C.CFUNCTYPE(C.c_int, C.c_void_p, U32, P32, P32, P32)
    SET = C.CFUNCTYPE(C.c_int, C.c_void_p, U32, U32)
    DELAY = C.CFUNCTYPE(C.c_int, C.c_void_p, U32)

    class PanelIo(C.Structure):
        _fields_ = [("Context", C.c_void_p), ("ValidateGpio", VALIDATE),
                    ("ReadGpio", READ), ("SetMode", SET),
                    ("SetDirection", SET), ("SetValue", SET), ("DelayUs", DELAY)]

    class PanelCore(C.Structure):
        _fields_ = [("Io", PanelIo), ("Signature", U32), ("State", C.c_int),
                    ("PendingOperation", C.c_int), ("ActionIndex", U32),
                    ("ActionAttempted", C.c_ubyte), ("Busy", C.c_ubyte),
                    ("Reserved", C.c_ubyte * 2), ("LastStatus", C.c_int)]

    role_pins = (37, 43, 3, 155, 49, 152, 153, 36, 15)
    descriptor_pins = {0x51000000 + pin * 0x10: pin
                       for _, pin, *_ in probe_bindings}
    context = 0x50000000
    panel = context + 8
    work = panel + 0xd8
    queue = 0x52000000
    stack = 0x53000000
    failures = []
    compared_cases = []
    call_count = 0
    total_relocations = 0

    def append_event(events, kind, first, second=None):
        if kind == "delay_us" and events and events[-1][0] == kind:
            events[-1][1] += first
        else:
            events.append([kind, first] if second is None else [kind, first, second])

    def fail(message):
        failures.append(message)
        return 5

    core_source = REPO / "Silicon/MediaTek/MediaTekPkg/Library/FramebufferCoreLib/MtkPanelCore.c"
    header = REPO / "Silicon/MediaTek/MediaTekPkg/Include/Library/MtkPanelCore.h"
    with tempfile.TemporaryDirectory(
        prefix="panel-lifecycle-", dir=REPO / "Build", ignore_cleanup_errors=True
    ) as temporary:
        library = Path(temporary) / ("panel.dylib" if sys.platform == "darwin" else "panel.so")
        compiler = shutil.which("clang") or shutil.which("cc")
        if compiler is None:
            raise ValueError("No host C compiler for production panel comparison")
        command = [compiler, "-dynamiclib" if sys.platform == "darwin" else "-shared",
                   "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-I" + str(REPO / "Silicon/MediaTek/MediaTekPkg/Include"),
                   str(core_source), "-o", str(library)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        core_library = C.CDLL(str(library))
        initialize = core_library.MtkPanelCoreInitialize
        initialize.argtypes = [C.POINTER(PanelCore), C.POINTER(PanelIo)]
        initialize.restype = C.c_int
        production = {}
        for operation in ("Prepare", "Enable", "Unprepare"):
            function = getattr(core_library, "MtkPanelCoreRunKernel" + operation)
            function.argtypes = [C.POINTER(PanelCore)]
            function.restype = C.c_int
            production[operation.lower()] = function
        for seed in (0, 0x5a, 0xa5):
            uc, section_bases, stubs, stop, ranges, relocations = make_module_emulator(
                elf, functions,
                ("_printk", "gpiod_direction_output", "__const_udelay",
                 "queue_delayed_work_on", "cancel_delayed_work_sync",
                 "mutex_lock", "mutex_unlock"),
                0x10000000,
            )
            total_relocations += relocations
            uc.mem_map(context, 0x1000)
            uc.mem_map(stack, 0x10000)
            for _, pin, offset, *_ in probe_bindings:
                uc.mem_write(context + offset, struct.pack("<Q", 0x51000000 + pin * 0x10))
            uc.mem_write(panel + 0xd0, struct.pack("<Q", queue))
            binary_levels = {pin: (seed >> (pin % 8)) & 1
                             for pin in set(role_pins) | {85}}
            native_levels = dict(binary_levels)
            native_modes = {pin: seed & 7 for pin in role_pins}
            native_dirs = {pin: seed & 1 for pin in role_pins}
            binary_events = []
            native_events = []
            queued = None

            def native_validate(_context, pin, role):
                if role not in range(len(role_pins)) or role_pins[role] != pin:
                    return fail("Production C used an unexpected GPIO mapping")
                return 0

            def native_read(_context, pin, mode, direction, latch):
                if pin not in native_levels:
                    return fail("Production C read an unknown GPIO")
                mode[0], direction[0], latch[0] = (
                    native_modes[pin], native_dirs[pin], native_levels[pin])
                return 0

            def native_mode(_context, pin, value):
                if pin not in native_modes or value != 0:
                    return fail("Production C selected an unexpected GPIO mode")
                native_modes[pin] = value
                return 0

            def native_dir(_context, pin, value):
                if pin not in native_dirs or value != 1 or native_modes[pin] != 0:
                    return fail("Production C selected an unexpected GPIO direction")
                native_dirs[pin] = value
                append_event(native_events, "output", pin, native_levels[pin])
                return 0

            def native_value(_context, pin, value):
                if pin not in native_levels or value not in (0, 1):
                    return fail("Production C selected an unexpected GPIO value")
                native_levels[pin] = value
                return 0

            def native_delay(_context, usec):
                if not 0 < usec <= 510000:
                    return fail("Production C requested an unexpected delay")
                append_event(native_events, "delay_us", usec)
                return 0

            callbacks = (VALIDATE(native_validate), READ(native_read), SET(native_mode),
                         SET(native_dir), SET(native_value), DELAY(native_delay))
            io = PanelIo(None, *callbacks)
            native = PanelCore()
            if C.sizeof(native) != 80 or initialize(C.byref(native), C.byref(io)) != 0:
                raise ValueError("Production panel core initialization/ABI mismatch")

            def on_instruction(machine, address, size, _data):
                nonlocal queued
                if address == stop:
                    machine.emu_stop()
                    return
                if address == stubs["_printk"]:
                    pass
                elif address == stubs["gpiod_direction_output"]:
                    descriptor = machine.reg_read(UC_ARM64_REG_X0)
                    pin = descriptor_pins.get(descriptor)
                    value = machine.reg_read(UC_ARM64_REG_X1) & 0xffffffff
                    if pin is None or value not in (0, 1):
                        raise ValueError("Original panel emitted an unknown GPIO operation")
                    binary_levels[pin] = value
                    append_event(binary_events, "output", pin, value)
                elif address == stubs["__const_udelay"]:
                    argument = machine.reg_read(UC_ARM64_REG_X0)
                    if argument % 0x10c7:
                        raise ValueError("Original __const_udelay has an unknown scale")
                    append_event(binary_events, "delay_us", argument // 0x10c7)
                elif address == stubs["queue_delayed_work_on"]:
                    arguments = tuple(machine.reg_read(reg) for reg in (
                        UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3))
                    if queued is not None or arguments != (32, queue, work, 125):
                        raise ValueError("Original panel delayed-work arguments changed")
                    queued = arguments
                    machine.reg_write(UC_ARM64_REG_X0, 1)
                    machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
                    return
                elif address == stubs["cancel_delayed_work_sync"]:
                    if machine.reg_read(UC_ARM64_REG_X0) != work:
                        raise ValueError("Original panel canceled unknown work")
                    queued = None
                elif address in (stubs["mutex_lock"], stubs["mutex_unlock"]):
                    raise ValueError("Unexpected optional-backlight provider access")
                elif not any(section_bases[section] + begin <= address <
                             section_bases[section] + end
                             for section, begin, end in ranges):
                    raise ValueError("Panel execution escaped pinned functions: " + hex(address))
                else:
                    return
                machine.reg_write(UC_ARM64_REG_X0, 0)
                machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))

            uc.hook_add(UC_HOOK_CODE, on_instruction)

            def invoke(name, argument=panel):
                nonlocal call_count
                function = elf["symbols"][name]
                entry = section_bases[function[1]] + function[2]
                uc.reg_write(UC_ARM64_REG_X0, argument)
                uc.reg_write(UC_ARM64_REG_X1, 0)
                uc.reg_write(UC_ARM64_REG_SP, stack + 0xff00)
                uc.reg_write(UC_ARM64_REG_LR, stop)
                uc.emu_start(entry, stop + 4, count=30000)
                if uc.reg_read(UC_ARM64_REG_PC) != stop:
                    raise ValueError("Original panel function did not return")
                if name != "jdi_power_delay_work" and uc.reg_read(UC_ARM64_REG_X0) != 0:
                    raise ValueError("Original panel function returned failure")
                call_count += 1

            for cycle in range(3):
                for operation in ("prepare", "enable", "unprepare"):
                    binary_events.clear()
                    native_events.clear()
                    if operation == "prepare":
                        invoke("jdi_prepare")
                        if bytes(uc.mem_read(panel + 0xc8, 1)) != b"\x01":
                            raise ValueError("Original panel did not set prepared state")
                    elif operation == "enable":
                        invoke("jdi_enable")
                        if queued is None:
                            raise ValueError("Original panel did not queue delayed enable")
                        ticks = queued[3]
                        if (ticks * 1000000) % hz:
                            raise ValueError("Panel delay does not convert exactly to microseconds")
                        append_event(binary_events, "delay_us", ticks * 1000000 // hz)
                        queued = None
                        invoke("jdi_power_delay_work", work)
                        if bytes(uc.mem_read(panel + 0xc9, 1)) != b"\x01":
                            raise ValueError("Original panel did not set enabled state")
                    else:
                        invoke("jdi_disable")
                        if queued is not None:
                            raise ValueError("Original panel left delayed work active")
                        invoke("jdi_unprepare")
                        if bytes(uc.mem_read(panel + 0xc8, 2)) != b"\0\0":
                            raise ValueError("Original panel did not clear prepared/enabled state")
                    if production[operation](C.byref(native)) != 0 or failures:
                        raise ValueError("Production panel operation failed: " + repr(failures))
                    if binary_events != native_events or binary_levels != native_levels:
                        raise ValueError("Original/C panel lifecycle mismatch: " + repr({
                            "operation": operation, "seed": seed, "cycle": cycle,
                            "original": binary_events, "production_C": native_events,
                            "original_levels": binary_levels, "C_levels": native_levels,
                        }))
                    compared_cases.append({
                        "seed": seed, "cycle": cycle, "operation": operation,
                        "events": [event[:] for event in binary_events],
                        "exact_event_order_and_final_levels": True,
                    })
                    # Both implementations suppress an already completed operation.
                    binary_events.clear()
                    native_events.clear()
                    if operation == "unprepare":
                        invoke("jdi_disable")
                    invoke("jdi_" + operation)
                    if production[operation](C.byref(native)) != 0:
                        raise ValueError("Repeated completed C operation failed")
                    if binary_events or native_events or queued is not None:
                        raise ValueError("Completed operation repeated GPIO/delay/work activity")
        _ctypes.dlclose(core_library._handle)
        core_library._handle = 0
        library.unlink()
    return {
        "state": "passed",
        "checked_unix": int(time.time()),
        "original_module": str(panel_path),
        "original_module_sha256": panel_sha,
        "original_module_bytes": len(raw),
        "kernel_config_sha256": hashlib.sha256(config_path.read_bytes()).hexdigest(),
        "stock_CONFIG_HZ": hz,
        "enable_delay_jiffies": 125,
        "enable_delay_microseconds": 500000,
        "delayed_worker_output_pin": 37,
        "worker_pin_evidence": "panel+0xd8-0x50 = ctx+0x90; probe binds edp_en GPIO37",
        "const_udelay_argument_scale": "microseconds * 0x10c7",
        "delay_scale_primary_source": "https://raw.githubusercontent.com/torvalds/linux/v6.12/include/asm-generic/delay.h",
        "functions": {name: {"offset": hex(bounds[0]), "bytes": bounds[1]}
                      for name, bounds in function_sizes.items()},
        "descriptor_bindings": [{"name": name, "pin": pin, "context_offset": hex(offset)}
                                for name, pin, offset, *_ in probe_bindings],
        "actual_original_function_calls": call_count,
        "applied_selected_function_relocations": total_relocations,
        "production_C_sha256": hashlib.sha256(core_source.read_bytes()).hexdigest(),
        "production_header_sha256": hashlib.sha256(header.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "compared_lifecycle_cases": len(compared_cases),
        "cases": compared_cases,
        "physical_device_tested": False,
        "limits": [
            "GPIO descriptors, delays, logging and work scheduling are fixtures.",
            "The original compiled function bodies/context accesses/loops execute with normal ELF relocations; no function is bypassed or replaced.",
            "Scheduler advances the observed 125 ticks at CONFIG_HZ=250; no real timer or IRQ executes.",
            "The production C uses serialized synchronous enable; no deferred callback survives it.",
            "DT mapping/lease/shutdown guards are checked separately by the platform adapter tests.",
            "Optional backlight-provider path is absent in the effective panel DT and rejected in this fixture.",
            "DSI host, display stream, visible pixels, OS suspend and physical device execution remain separate.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--panel-lifecycle", action="store_true",
                        help="compare shipping panel lifecycle ARM64 functions with production C")
    parser.add_argument("--buttons", action="store_true",
                        help="verify the pinned stock LK power-key reader")
    parser.add_argument("--lk", type=Path,
                        help="path to the pinned stock LK image for --buttons")
    args = parser.parse_args()
    if args.panel_lifecycle:
        if args.buttons or args.lk is not None:
            parser.error("--panel-lifecycle excludes --buttons/--lk")
        print(json.dumps(verify_stock_panel_lifecycle(), indent=2))
        return
    if args.buttons:
        if args.lk is None:
            parser.error("--buttons requires --lk PATH")
        print(json.dumps(verify_lk_power_key(args.lk), indent=2))
        return
    if args.lk is not None:
        parser.error("--lk is only valid with --buttons")

    raw = BINARY.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise ValueError("Stock SC8989x module identity mismatch")
    if raw[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", raw, 18)[0] != 183:
        raise ValueError("Expected an ARM64 ELF64 little-endian module")

    section_table = struct.unpack_from("<Q", raw, 40)[0]
    section_width, section_count, shstr_index = struct.unpack_from("<HHH", raw, 58)
    if section_width != 64 or section_table + section_width * section_count > len(raw):
        raise ValueError("Invalid ELF section table")

    sections = [
        struct.unpack_from("<IIQQQQIIQQ", raw, section_table + index * section_width)
        for index in range(section_count)
    ]

    def section_bytes(index):
        section = sections[index]
        if section[1] == SHT_NOBITS:
            return bytes(section[5])
        data = raw[section[4]:section[4] + section[5]]
        if len(data) != section[5]:
            raise ValueError("ELF section exceeds source bytes")
        return data

    symbols = {}
    symbol_tables = {}
    for index, section in enumerate(sections):
        if section[1] != SHT_SYMTAB:
            continue
        names = section_bytes(section[6])
        entries = []
        for offset in range(0, section[5], 24):
            name_offset, info, other, shndx, value, size = struct.unpack_from(
                "<IBBHQQ", section_bytes(index), offset
            )
            name = names[name_offset:].split(b"\0", 1)[0].decode()
            entry = (name, shndx, value, size)
            entries.append(entry)
            if name:
                symbols[name] = entry
        symbol_tables[index] = entries

    function = symbols.get(FUNCTION)
    if function is None or function[1] == 0 or function[3] == 0:
        raise ValueError("Function symbol has no complete ELF extent")
    function_name, function_section, function_offset, function_size = function
    boost = symbols[BOOST_FUNCTION]
    assert boost[1] == function_section and boost[2:] == (0x539c, 0xd4)
    otg = symbols.get(OTG_FUNCTION)
    if otg is None or otg[1] != function_section or otg[2:] != (0x476c, 0x160):
        raise ValueError("Stock OTG function symbol does not match the inspected extent")
    set_boost_ilmt = symbols.get("sc8989x_set_boost_ilmt")
    if set_boost_ilmt is None or set_boost_ilmt[1] != function_section or set_boost_ilmt[2:] != (0x48d0, 0x154):
        raise ValueError("Stock boost-current setter symbol does not match the inspected extent")
    # Stock getter loads drvdata+0x90. Probe's pointer table starts at +0x18,
    # so field index 15 in the 20-byte descriptor array is the boost field.
    fields = symbols["sc8989x_reg_fields"]
    assert fields[3] == 70 * 20

    def read_field_descriptor(index):
        return struct.unpack_from("<IIIII", section_bytes(fields[1]), fields[2] + index * 20)

    field_descriptors = {
        index: read_field_descriptor(index)[:3]
        for index in (0, 15, 16, 43, 45)
    }
    expected_fields = {
        0: (0x00, 7, 7),
        15: (0x03, 5, 5),
        16: (0x03, 4, 4),
        43: (0x0a, 4, 7),
        45: (0x0a, 0, 2),
    }
    if field_descriptors != expected_fields:
        raise ValueError("Stock regmap-field descriptors differ from the inspected fields")
    boost_descriptor_offset = fields[2] + 15 * 20
    boost_descriptor = field_descriptors[15]
    assert struct.unpack_from("<I", section_bytes(function_section), boost[2] + 0x3c)[0] == 0xf9404a60

    default_cfg = symbols.get("sc8989x_default_cfg")
    iboost_table = symbols.get("sc8989x_iboost")
    init_device = symbols.get("sc8989x_init_device")
    if default_cfg is None or default_cfg[3] < 0x54:
        raise ValueError("Stock default charger configuration is missing")
    if iboost_table is None or iboost_table[3] != 8 * 4:
        raise ValueError("Stock boost-current selector table is missing")
    if init_device is None or init_device[1] != function_section or init_device[2:] != (0x0b68, 0x6e8):
        raise ValueError("Stock charger-init function symbol does not match the inspected extent")
    default_config_bytes = section_bytes(default_cfg[1])
    default_votg = struct.unpack_from("<I", default_config_bytes, default_cfg[2] + 0x4c)[0]
    default_iboost = struct.unpack_from("<I", default_config_bytes, default_cfg[2] + 0x50)[0]
    iboost_values = struct.unpack_from("<8I", section_bytes(iboost_table[1]), iboost_table[2])
    if (default_votg, default_iboost) != (12, 7):
        raise ValueError("Stock default VOTG/IBOOST configuration changed")
    if iboost_values != (500, 750, 1200, 1400, 1650, 1875, 2150, 2450):
        raise ValueError("Stock boost-current selector table changed")
    boost_setter_bytes = section_bytes(set_boost_ilmt[1])
    boost_setter_proof_words = {
        0x84: 0x528b9808,  # mov w8, #0x5cc0 (lower selector-3 boundary)
        0x88: 0x72a002a8,  # movk w8, #0x15, lsl #16
        0x8c: 0x6b08027f,  # cmp w19, w8
        0x90: 0x54000062,  # b.hs past selector 2
        0x9c: 0x5285aa08,  # mov w8, #0x2d50 (upper selector-3 boundary)
        0xa0: 0x72a00328,  # movk w8, #0x19, lsl #16
        0xa4: 0x6b08027f,  # cmp w19, w8
        0xa8: 0x54000062,  # b.hs to the next selector
        0xac: 0x52800062,  # mov w2, #3
        0x100: 0xf940c280,  # ldr x0, [x20, #0x180]
        0x114: 0x94000000,  # relocated call to regmap_field_update_bits_base
    }
    for offset, expected in boost_setter_proof_words.items():
        actual = struct.unpack_from("<I", boost_setter_bytes, set_boost_ilmt[2] + offset)[0]
        if actual != expected:
            raise ValueError("Stock boost-current selector-3 code differs at " + hex(offset))

    # These raw selectors were confirmed from the selected stock DTB.
    # Keep them separate from the module's compiled defaults.
    stock_dt_votg = 12
    stock_dt_iboost = 3
    if stock_dt_votg > (1 << (field_descriptors[43][2] - field_descriptors[43][1] + 1)) - 1:
        raise ValueError("Stock DT VOTG selector does not fit field 43")
    if stock_dt_iboost > (1 << (field_descriptors[45][2] - field_descriptors[45][1] + 1)) - 1:
        raise ValueError("Stock DT IBOOST selector does not fit field 45")

    ranges = [(function_offset, function_offset + function_size),
              (boost[2], boost[2] + boost[3]),
              (otg[2], otg[2] + otg[3])]

    section_bases = {}
    next_address = 0x10000000
    for index, section in enumerate(sections):
        if (section[2] & SHF_ALLOC) and section[5]:
            section_bases[index] = next_address
            next_address += align_up(section[5])

    code_end = next_address
    stub_base = align_up(code_end + 0x10000)
    stub_addresses = {
        "_printk": stub_base,
        "_dev_err": stub_base + 0x10,
        "regmap_field_read": stub_base + 0x20,
        "__stack_chk_fail": stub_base + 0x30,
        "rdev_get_drvdata": stub_base + 0x40,
        "_dev_info": stub_base + 0x50,
        "regmap_field_update_bits_base": stub_base + 0x60,
        "gpio_to_desc": stub_base + 0x70,
        "gpiod_direction_output_raw": stub_base + 0x80,
    }
    stop_address = stub_base + 0x1000
    data_base = 0x30000000
    device = data_base + 0x1000
    charge_field = data_base + 0x2000
    vbus_field = data_base + 0x2010
    pg_field = data_base + 0x2020
    boost_field = data_base + 0x2030
    otg_field_pointers = {
        0: data_base + 0x2040,
        15: data_base + 0x2050,
        16: data_base + 0x2060,
    }
    otg_field_index_by_pointer = {pointer: index for index, pointer in otg_field_pointers.items()}
    otg_device = data_base + 0x6000
    otg_context = data_base + 0x6100
    gpio_descriptors = {48: data_base + 0x7000, 86: data_base + 0x7010}
    field_values = {}
    field_trace = []
    failures = []
    otg_trace = []
    active_otg_fixture = None
    pending_gpio = None

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(0x10000000, code_end - 0x10000000)
    for index, address in section_bases.items():
        if sections[index][1] != SHT_NOBITS:
            uc.mem_write(address, section_bytes(index))

    uc.mem_map(stub_base, 0x2000)
    return_stub = struct.pack("<II", 0x52800000, 0xD65F03C0)  # mov w0, #0; ret
    for address in stub_addresses.values():
        uc.mem_write(address, return_stub)
    uc.mem_map(data_base, 0x10000)
    uc.mem_map(0x40000000, 0x10000)
    uc.mem_map(0x1000, 0x1000)

    uc.mem_write(otg_device + 0xe0, struct.pack("<Q", otg_context))
    for index, pointer in otg_field_pointers.items():
        uc.mem_write(otg_context + 0x18 + index * 8, struct.pack("<Q", pointer))
    uc.mem_write(otg_context + 0x248, struct.pack("<I", 48))
    uc.mem_write(otg_context + 0x24c, struct.pack("<I", 86))

    def resolve_symbol(symbol, relocation_kind):
        name, shndx, value, size = symbol
        if shndx:
            if shndx not in section_bases:
                raise ValueError("Relocation target is outside mapped sections: " + name)
            return section_bases[shndx] + value
        if relocation_kind in (R_AARCH64_CALL26, R_AARCH64_JUMP26):
            if name not in stub_addresses:
                raise ValueError("Unrecognized external call: " + name)
            return stub_addresses[name]
        # External data references in this function are used only as log labels.
        if name.startswith("__mod_of__"):
            return data_base + 0x8000
        raise ValueError("Unexpected undefined data symbol: " + name)

    relocations_applied = 0
    for index, section in enumerate(sections):
        if section[1] != SHT_RELA or section[7] != function_section:
            continue
        symbol_entries = symbol_tables[section[6]]
        for offset in range(0, section[5], 24):
            place_offset, info, addend = struct.unpack_from("<QQq", section_bytes(index), offset)
            if not any(begin <= place_offset < end for begin, end in ranges):
                continue
            kind = info & 0xFFFFFFFF
            target_symbol = symbol_entries[info >> 32]
            target = resolve_symbol(target_symbol, kind) + addend
            place = section_bases[function_section] + place_offset
            instruction = struct.unpack("<I", uc.mem_read(place, 4))[0]

            if kind == R_AARCH64_ADR_PREL_PG_HI21:
                delta = (target >> 12) - (place >> 12)
                if not -(1 << 20) <= delta < (1 << 20):
                    raise ValueError("ADRP relocation out of range")
                instruction = (instruction & ~((3 << 29) | (0x7FFFF << 5))) | (
                    ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5)
                )
            elif kind == R_AARCH64_ADD_ABS_LO12_NC:
                instruction = (instruction & ~(0xFFF << 10)) | ((target & 0xFFF) << 10)
            elif kind in (
                R_AARCH64_LDST8_ABS_LO12_NC,
                R_AARCH64_LDST16_ABS_LO12_NC,
                R_AARCH64_LDST32_ABS_LO12_NC,
                R_AARCH64_LDST64_ABS_LO12_NC,
            ):
                shift = {
                    R_AARCH64_LDST8_ABS_LO12_NC: 0,
                    R_AARCH64_LDST16_ABS_LO12_NC: 1,
                    R_AARCH64_LDST32_ABS_LO12_NC: 2,
                    R_AARCH64_LDST64_ABS_LO12_NC: 3,
                }[kind]
                instruction = (instruction & ~(0xFFF << 10)) | (((target & 0xFFF) >> shift) << 10)
            elif kind in (R_AARCH64_JUMP26, R_AARCH64_CALL26):
                delta = target - place
                if (delta % 4) or not -(1 << 27) <= delta < (1 << 27):
                    raise ValueError("Branch relocation out of range")
                instruction = (instruction & ~0x3FFFFFF) | ((delta >> 2) & 0x3FFFFFF)
            else:
                raise ValueError("Unsupported function relocation: " + str(kind))

            uc.mem_write(place, struct.pack("<I", instruction))
            relocations_applied += 1

    # The kernel's SP_EL0 stack canary access is replaced with a mapped fixture.
    entry = section_bases[function_section] + function_offset
    function_end = entry + function_size
    canary_word = 0xD5384108  # mrs x8, SP_EL0
    patched_mrs = 0xD2820008  # movz x8, #0x1000
    for begin, end in ranges:
        for address in range(section_bases[function_section] + begin, section_bases[function_section] + end, 4):
            if struct.unpack("<I", uc.mem_read(address, 4))[0] == canary_word:
                uc.mem_write(address, struct.pack("<I", patched_mrs))
    uc.mem_write(0x1710, struct.pack("<Q", 0xA5A5C3C35A5A3C3C))
    uc.mem_write(device + 0x190, struct.pack("<Q", charge_field))
    uc.mem_write(device + 0x188, struct.pack("<Q", vbus_field))
    uc.mem_write(device + 0x1A8, struct.pack("<Q", pg_field))
    uc.mem_write(device + 0x90, struct.pack("<Q", boost_field))

    def on_instruction(machine, address, size, _):
        nonlocal pending_gpio
        if address == stop_address:
            machine.emu_stop()
            return
        if address == stub_addresses["rdev_get_drvdata"]:
            machine.reg_write(UC_ARM64_REG_X0, device)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return
        if address == stub_addresses["regmap_field_read"]:
            field = machine.reg_read(UC_ARM64_REG_X0)
            output = machine.reg_read(UC_ARM64_REG_X1)
            if field not in field_values:
                raise RuntimeError("Unexpected regmap field pointer: " + hex(field))
            machine.mem_write(output, struct.pack("<I", field_values[field]))
            field_trace.append(field)
        elif address == stub_addresses["regmap_field_update_bits_base"]:
            if active_otg_fixture is None:
                raise RuntimeError("OTG regmap write ran outside an OTG fixture")
            field = machine.reg_read(UC_ARM64_REG_X0)
            if field not in otg_field_index_by_pointer:
                raise RuntimeError("Unexpected OTG regmap field pointer: " + hex(field))
            index = otg_field_index_by_pointer[field]
            mask = machine.reg_read(UC_ARM64_REG_X1) & 0xffffffff
            value = machine.reg_read(UC_ARM64_REG_X2) & 0xffffffff
            otg_trace.append(("field_write", index, mask, value))
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return
        elif address == stub_addresses["gpio_to_desc"]:
            if active_otg_fixture is None:
                raise RuntimeError("OTG GPIO lookup ran outside an OTG fixture")
            line = machine.reg_read(UC_ARM64_REG_X0) & 0xffffffff
            if line not in gpio_descriptors:
                raise RuntimeError("Unexpected OTG GPIO number: " + str(line))
            if pending_gpio is not None:
                raise RuntimeError("OTG GPIO lookup occurred before the prior output call")
            pending_gpio = line
            otg_trace.append(("gpio_to_desc", line))
            machine.reg_write(UC_ARM64_REG_X0, gpio_descriptors[line])
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return
        elif address == stub_addresses["gpiod_direction_output_raw"]:
            if active_otg_fixture is None:
                raise RuntimeError("OTG GPIO output ran outside an OTG fixture")
            descriptor = machine.reg_read(UC_ARM64_REG_X0)
            line = next((number for number, desc in gpio_descriptors.items() if desc == descriptor), None)
            output = machine.reg_read(UC_ARM64_REG_X1) & 0xffffffff
            if line is None or pending_gpio != line:
                raise RuntimeError("OTG GPIO output did not match the preceding lookup")
            pending_gpio = None
            otg_trace.append(("gpio_output_raw", line, output))
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))
            return
        elif address == stub_addresses["_dev_err"] and active_otg_fixture is not None:
            failures.append("stock OTG function logged an error on the success fixture")
            machine.emu_stop()
            return
        elif address == stub_addresses["__stack_chk_fail"]:
            failures.append("stack canary failure")
            machine.emu_stop()
            return
        elif not any(stub <= address < stub + 8 for stub in stub_addresses.values()) and not entry <= address < function_end:
            failures.append("execution escaped the selected function: " + hex(address))
            machine.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_instruction)

    with tempfile.TemporaryDirectory(prefix="stock-power-binary-", dir=REPO / "Build", ignore_cleanup_errors=True) as temporary:
        core_path = Path(temporary) / "libstock_power_core.so"
        subprocess.run(
            [
                "gcc", "-shared", "-fPIC", "-O2",
                "-I" + str(REPO / "Silicon/MediaTek/MediaTekPkg/Include"),
                str(REPO / "Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/StockPowerCore.c"),
                "-o", str(core_path),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        core = C.CDLL(str(core_path))
        core.SpReadSc8989xStatus.argtypes = [READ_REGS, C.c_void_p, C.POINTER(Sc8989xStatus)]
        core.SpReadSc8989xStatus.restype = C.c_int
        core.SpReadSc8989xBoostState.argtypes = [READ_REGS, C.c_void_p, C.POINTER(Sc8989xBoost)]
        core.SpReadSc8989xBoostState.restype = C.c_int
        tcpc_safe0v_proof = verify_stock_tcpc_safe0v(core)
        compared = 0

        def fake_read(_context, register, length, output):
            if length != 1:
                return 3
            values = current_registers
            if register not in values:
                return 3
            output[0] = values[register]
            return 0

        read_callback = READ_REGS(fake_read)
        for charge in range(4):
            for vbus in range(8):
                current_registers = {
                    0x14: 2 << 3,
                    0x0B: (charge << 3) | (vbus << 5),
                    0x0C: 0x08,
                }
                field_values.clear()
                field_values.update({
                    charge_field: charge,
                    vbus_field: vbus,
                    pg_field: 1,
                })
                field_trace.clear()
                uc.mem_write(0x40000000, bytes(0x10000))
                uc.reg_write(UC_ARM64_REG_X0, device)
                uc.reg_write(UC_ARM64_REG_X1, 0)
                uc.reg_write(UC_ARM64_REG_SP, 0x4000FF00)
                uc.reg_write(UC_ARM64_REG_LR, stop_address)
                uc.emu_start(entry, stop_address, timeout=100000, count=2000)
                if failures:
                    raise RuntimeError(failures[-1])
                if uc.reg_read(UC_ARM64_REG_PC) != stop_address:
                    raise RuntimeError("Stock function did not return")
                if field_trace != [charge_field, vbus_field, pg_field]:
                    raise RuntimeError("Stock function read fields out of observed order")
                stock_status = uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF

                c_status = Sc8989xStatus()
                result = core.SpReadSc8989xStatus(
                    read_callback, None, C.byref(c_status)
                )
                if result != 0:
                    raise RuntimeError("Portable C status decode failed")
                if stock_status != c_status.ChargeStatus:
                    raise AssertionError(
                        f"charge={charge}, vbus={vbus}: stock={stock_status}, C={c_status.ChargeStatus}"
                    )
                compared += 1

        boost_compared = 0
        entry = section_bases[function_section] + boost[2]
        function_end = entry + boost[3]
        for register_value in range(256):
            current_registers = {0x14: 2 << 3, 0x03: register_value}
            field_values.clear()
            field_values[boost_field] = (register_value >> boost_descriptor[1]) & 1
            field_trace.clear()
            uc.mem_write(0x40000000, bytes(0x10000))
            uc.reg_write(UC_ARM64_REG_X0, data_base + 0x4000)  # regulator fixture
            uc.reg_write(UC_ARM64_REG_SP, 0x4000FF00)
            uc.reg_write(UC_ARM64_REG_LR, stop_address)
            uc.emu_start(entry, stop_address, timeout=100000, count=2000)
            if failures or uc.reg_read(UC_ARM64_REG_PC) != stop_address:
                raise RuntimeError(failures[-1] if failures else "Boost getter did not return")
            assert field_trace == [boost_field]
            state = Sc8989xBoost()
            assert core.SpReadSc8989xBoostState(read_callback, None, C.byref(state)) == 0
            assert state.PartCode == 2
            assert (uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff) == state.BoostEnabled
            boost_compared += 1

        entry = section_bases[function_section] + otg[2]
        function_end = entry + otg[3]
        otg_fixtures = {}
        expected_otg_traces = {
            "enable": [
                ("field_write", 0, 0xffffffff, 0),
                ("gpio_to_desc", 48),
                ("gpio_output_raw", 48, 1),
                ("field_write", 16, 0xffffffff, 0),
                ("gpio_to_desc", 86),
                ("gpio_output_raw", 86, 1),
                ("field_write", 15, 0xffffffff, 1),
            ],
            "disable": [
                ("gpio_to_desc", 86),
                ("gpio_output_raw", 86, 0),
                ("field_write", 15, 0xffffffff, 0),
            ],
        }
        for fixture_name, enable in (("enable", 1), ("disable", 0)):
            active_otg_fixture = fixture_name
            otg_trace.clear()
            pending_gpio = None
            failures.clear()
            uc.mem_write(0x40000000, bytes(0x10000))
            uc.reg_write(UC_ARM64_REG_X0, otg_device)
            uc.reg_write(UC_ARM64_REG_X1, enable)
            uc.reg_write(UC_ARM64_REG_X2, 0)
            uc.reg_write(UC_ARM64_REG_SP, 0x4000FF00)
            uc.reg_write(UC_ARM64_REG_LR, stop_address)
            uc.emu_start(entry, stop_address, timeout=100000, count=2000)
            if failures:
                raise RuntimeError(failures[-1])
            if uc.reg_read(UC_ARM64_REG_PC) != stop_address:
                raise RuntimeError("Stock OTG function did not return")
            if (uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff) != 0:
                raise RuntimeError("Stock OTG success fixture returned an error")
            if pending_gpio is not None:
                raise RuntimeError("Stock OTG function returned with an unmatched GPIO lookup")
            if otg_trace != expected_otg_traces[fixture_name]:
                raise AssertionError(
                    f"OTG {fixture_name} call trace differs: {otg_trace!r}"
                )
            otg_fixtures[fixture_name] = [list(event) for event in otg_trace]
        active_otg_fixture = None
        # The shared mount retains an unlinked image while dlopen owns it.
        # No C entry point is used after this close.
        _ctypes.dlclose(core._handle)
        core._handle = 0

    # OrbStack's shared mount may report ENOTEMPTY briefly after unlink.
    # Do not publish a successful report until this generated binary is gone.
    for attempt in range(20):
        try:
            shutil.rmtree(temporary)
            break
        except FileNotFoundError:
            break
        except OSError as error:
            if error.errno != errno.ENOTEMPTY or attempt == 19:
                raise
            time.sleep(0.05)

    def text_location(offset):
        return {
            "text_offset": hex(offset),
            "file_offset": hex(sections[function_section][4] + offset),
        }

    report = {
        "schema": 1,
        "status": "passed",
        "scope": "Shipped SC8989x charge/boost getters and OTG control, plus HUSB311 raw-status mapping and TCPM safe0V ARM64 execution with modeled peripherals",
        "test_script": str(Path(__file__).resolve().relative_to(REPO.parent)),
        "result_file": str(RESULT_PATH.relative_to(REPO.parent)),
        "binary": str(BINARY.relative_to(REPO.parent)),
        "binary_sha256": SHA256,
        "function": FUNCTION,
        "function_offset": hex(function_offset),
        "function_bytes": function_size,
        "relocations_applied": relocations_applied,
        "field_read_order": ["CHRG_STAT", "VBUS_STAT", "PG_STAT"],
        "combinations_compared": compared,
        "boost_combinations_compared": boost_compared,
        "boost_function": {"name": BOOST_FUNCTION, "offset": hex(boost[2]), "bytes": boost[3],
                           "field_index": 15, "drvdata_pointer_offset": "0x90",
                           "descriptor_offset": hex(boost_descriptor_offset),
                           "register": boost_descriptor[0], "lsb": boost_descriptor[1], "msb": boost_descriptor[2]},
        "otg": {
            "function": OTG_FUNCTION,
            "text_offset": hex(otg[2]),
            "file_offset": hex(sections[function_section][4] + otg[2]),
            "bytes": otg[3],
            "successful_fixtures": len(otg_fixtures),
            "call_trace": otg_fixtures,
            "fields": {
                str(index): {
                    "section_offset": hex(fields[2] + index * 20),
                    "file_offset": hex(sections[fields[1]][4] + fields[2] + index * 20),
                    "register": hex(descriptor[0]),
                    "lsb": descriptor[1],
                    "msb": descriptor[2],
                    "mask": hex(((1 << (descriptor[2] - descriptor[1] + 1)) - 1) << descriptor[1]),
                }
                for index, descriptor in field_descriptors.items()
            },
            "call_sites": {
                "regmap_field_update_bits_base": [text_location(value) for value in (0x47a4, 0x47f0, 0x4860)],
                "gpio_to_desc": [text_location(value) for value in (0x47cc, 0x483c)],
                "gpiod_direction_output_raw": [text_location(value) for value in (0x47d4, 0x4844)],
            },
            "gpio_lines": {"enable": 48, "otg": 86},
            "side_effects": "regmap and GPIO functions are mocked and only their ARM64 call order/arguments are captured",
        },
        "stock_tcpm_safe0v": tcpc_safe0v_proof,
        "static_boost_selector_proof": {
            "binary_sha256": SHA256,
            "selected_stock_dtb_raw_properties": {
                "sc,sc8989x,votg": stock_dt_votg,
                "sc,sc8989x,iboost": stock_dt_iboost,
                "provenance": "values read from the selected stock DTB during platform analysis; this script does not parse or dump DTB chosen data",
            },
            "compiled_default_cfg": {
                "symbol": "sc8989x_default_cfg",
                "section_offset": hex(default_cfg[2]),
                "file_offset": hex(sections[default_cfg[1]][4] + default_cfg[2]),
                "votg_raw": default_votg,
                "iboost_raw": default_iboost,
                "votg_member_offset": "0x4c",
                "iboost_member_offset": "0x50",
            },
            "reg0a_votg": {
                "selector_raw": stock_dt_votg,
                "field_index": 43,
                "register": "0x0a",
                "lsb": 4,
                "msb": 7,
                "mask": "0xf0",
                "encoded_field_nibble": hex(stock_dt_votg),
                "encoded_register_bits": hex(stock_dt_votg << field_descriptors[43][1]),
                "init_function": "sc8989x_init_device",
                "selector_pair_text_offset": "0xc54",
                "regmap_update_call_text_offset": "0xd58",
                "decoded_millivolts": None,
                "unit_note": "The shipped ELF proves the raw selector/field location; no selector-to-millivolt conversion was established.",
            },
            "reg0a_iboost": {
                "selector_raw": stock_dt_iboost,
                "field_index": 45,
                "register": "0x0a",
                "lsb": 0,
                "msb": 2,
                "mask": "0x07",
                "init_function": "sc8989x_init_device",
                "selector_pair_text_offset": "0xc60",
                "regmap_update_call_text_offset": "0xd58",
                "table_symbol": "sc8989x_iboost",
                "table_section_offset": hex(iboost_table[2]),
                "table_file_offset": hex(sections[iboost_table[1]][4] + iboost_table[2]),
                "table_values_raw": list(iboost_values),
                "selected_table_value_raw": iboost_values[stock_dt_iboost],
                "set_boost_ilmt": {
                    "text_offset": hex(set_boost_ilmt[2]),
                    "file_offset": hex(sections[function_section][4] + set_boost_ilmt[2]),
                    "selector3_instruction_text_offset": "0x497c",
                    "selector3_instruction_file_offset": hex(sections[function_section][4] + 0x497c),
                    "field45_pointer_load_text_offset": "0x49d0",
                    "regmap_update_call_text_offset": "0x49e4",
                    "selector3_input_interval_raw": {
                        "lower_inclusive": 1400000,
                        "upper_exclusive": 1650000,
                    },
                    "input_to_table_scale": 1000,
                    "unit_note": "The binary proves raw input thresholds and table value 1400 (a 1000:1 scale); it does not encode an explicit uA/mA unit label. The function selects a table index rather than dividing the input.",
                },
            },
        },
        "source_sha256": {str(path.relative_to(REPO)): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in (Path(__file__).resolve(),
                             REPO/"Silicon/MediaTek/MediaTekPkg/Library/StockPowerCoreLib/StockPowerCore.c",
                             REPO/"Silicon/MediaTek/MediaTekPkg/Include/Library/StockPowerCore.h")},
        "external_behavior": "SC8989x driver-data/regmap reads are controlled; OTG regmap writes and GPIO lookups/outputs are captured mocks; HUSB311 regmap reads return only fixture bytes for 0x1e/0x97; TCPM lock/log calls are explicitly modeled",
        "limitations": [
            "No kernel, physical I2C/HUSB transport, device, or tablet execution",
            "Regmap writes and GPIO output levels are observed arguments only; hardware side effects are not modeled",
            "The selected DTB VOTG/IBOOST properties are reported as raw selectors; the VOTG selector-to-voltage map is not established",
            "TCPC raw status values do not provide an analog VBUS voltage measurement",
            "Only successful charge/VBUS, boost reads, and OTG enable/disable paths are modeled; I2C failure paths use separate native tests",
            "The HUSB311 regmap transport is a fixture; _dev_err and __stack_chk_fail are rejection paths, not success stubs",
            "TCPM mutex calls model one uncontended synchronous call; pd_dbg_info is an argument-checked no-op log sink",
        ],
    }
    result_json = json.dumps(report, indent=2) + "\n"
    RESULT_PATH.write_text(result_json)
    result_sha256 = hashlib.sha256(result_json.encode()).hexdigest()
    manifest = json.loads(ANALYSIS_PATH.read_text())
    manifest["binary_execution_test"] = {
        "status": report["status"],
        "script": report["test_script"],
        "result": report["result_file"],
        "result_sha256": result_sha256,
        "binary_sha256": SHA256,
        "function": FUNCTION,
        "combinations_compared": compared,
        "field_read_order": report["field_read_order"],
        "boost_combinations_compared": boost_compared,
        "boost_function": report["boost_function"],
        "otg_function": report["otg"],
        "static_boost_selector_proof": report["static_boost_selector_proof"],
        "stock_tcpm_safe0v": report["stock_tcpm_safe0v"],
    }
    ANALYSIS_PATH.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()
