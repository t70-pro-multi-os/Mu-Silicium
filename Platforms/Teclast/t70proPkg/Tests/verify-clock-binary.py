#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute stock MT6789 PLL CCF operations in Unicorn against fake MMIO.

The ELF objects are parsed directly. Only __const_udelay is replaced by a
verified no-time stub; the four MMIO tracing imports are argument-checked
observers while original readl/writel and PLL instructions execute. No kernel,
device, clock tree, or physical address is accessed.
"""
from pathlib import Path
import hashlib
import json
import struct

from unicorn import (
    Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE,
    UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_READ, UC_MEM_WRITE,
)
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
    UC_ARM64_REG_X3, UC_ARM64_REG_X4,
    UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC,
)

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
ANALYSIS = REPO.parent / "analysis"
MTK_BINARY = ANALYSIS / "stock-binaries/vendor_boot_a_0/clk-mt6789.ko"
COMMON_BINARY = ANALYSIS / "stock-binaries/vendor_boot_a_0/clk-common.ko"
RESULT_PATH = ANALYSIS / "clock-binary-tests.json"
NATIVE_SELECTED_REPORT = ANALYSIS / "mediatek-driver-selected-tests.json"
CLOCK_SOURCE_PATHS = (
    "Platforms/Teclast/t70proPkg/Tests/ClockTest.c",
    "Silicon/MediaTek/MediaTekPkg/Drivers/ClockDxe/ClockDxe.c",
    "Silicon/MediaTek/MT6789Pkg/Library/ClockImplLib/ClockImplLib.c",
    "Silicon/MediaTek/MediaTekPkg/Include/Protocol/MtkClock.h",
    "Silicon/MediaTek/MediaTekPkg/Include/Library/ClockImplLib.h",
)
MTK_SHA256 = "162ddd1cc3a88c6f884d153d04889071b8cf9574bf04a10684e5f72b04d2c32b"
COMMON_SHA256 = "11548b41f27584f0c2e348fb533546b2ff7974bcdf82129787784a697af3c981"

SHT_NOBITS = 8
SHT_SYMTAB = 2
SHT_RELA = 4
SHN_ABS = 0xFFF1
R_AARCH64_ABS64 = 257
R_AARCH64_LDST8_ABS_LO12_NC = 278
R_AARCH64_LDST32_ABS_LO12_NC = 285
R_AARCH64_LDST64_ABS_LO12_NC = 286
R_AARCH64_ADR_PREL_PG_HI21 = 275
R_AARCH64_ADD_ABS_LO12_NC = 277
R_AARCH64_CALL26 = 283

TEXT_BASE = 0x10000000
STUB_BASE = 0x11000000
STOP = STUB_BASE + 0x1000
BSS_BASE = 0x20000000
TABLE_BASE = 0x30000000
DATA_BASE = 0x40000000
MMIO_BASE = 0x50000000
STACK_BASE = 0x60000000
PAGE = 0x1000
ENTRY_SIZE = 0xC0
HAVE_RST_BAR = 0x10000
PLL_AO = 0x20000

FUNCTIONS = {
    "mtk_pll_prepare": (0x1908, 0x124),
    "mtk_pll_unprepare": (0x1A30, 0x104),
    "mtk_pll_is_prepared": (0x1B38, 0x5C),
    "readl": (0x3428, 0x80),
    "writel": (0x34A8, 0x7C),
}
DELAY_ARGS = {0x1934: 0x10C7, 0x1950: 0x10C7, 0x19EC: 0x14F8C}
READ_CALLER_RETURNS = {
    0x1924, 0x1940, 0x1968, 0x1984, 0x19B0, 0x19D8, 0x1A04,
    0x1A58, 0x1A7C, 0x1AA4, 0x1AB8, 0x1AE8, 0x1B04, 0x1B18,
    0x1B68,
}
WRITE_CALLER_RETURNS = {
    0x1930, 0x194C, 0x197C, 0x19A0, 0x19E4, 0x1A18,
    0x1A6C, 0x1AB0, 0x1AD4, 0x1AFC, 0x1B10, 0x1B24,
}
EXPECTED_CALLS = {
    0x1934: "__const_udelay",
    0x1950: "__const_udelay",
    0x19EC: "__const_udelay",
    0x345C: "log_read_mmio",
    0x348C: "log_post_read_mmio",
    0x34E8: "log_write_mmio",
    0x350C: "log_post_write_mmio",
}
EXTERNALS = (
    "__const_udelay", "log_read_mmio", "log_post_read_mmio",
    "log_write_mmio", "log_post_write_mmio",
)

# (name, reg, power, en_reg, en_mask, en_bit, flags, reset_bar,
#  tuner_reg, tuner_en_reg, tuner_bit, pcw_reg, pcw_bits)
EXPECTED_ROWS = (
    ("armpll_ll", 0x208, 0x214, 0x208, 0, 0, PLL_AO, 1, 0, 0, 0, 0x20C, 22),
    ("armpll_bl0", 0x218, 0x224, 0x218, 0, 0, PLL_AO, 1, 0, 0, 0, 0x21C, 22),
    ("ccipll", 0x258, 0x264, 0x258, 0, 0, PLL_AO, 1, 0, 0, 0, 0x25C, 22),
    ("mpll", 0x390, 0x39C, 0x390, 0, 0, PLL_AO, 1, 0, 0, 0, 0x394, 22),
    ("mainpll", 0x340, 0x34C, 0x340, 0xFF000000, 0, HAVE_RST_BAR | PLL_AO, 0x800000, 0, 0, 0, 0x344, 22),
    ("univpll", 0x308, 0x314, 0x308, 0xFF000000, 0, HAVE_RST_BAR, 0x800000, 0, 0, 0, 0x30C, 22),
    ("msdcpll", 0x350, 0x35C, 0x350, 0, 0, 0, 1, 0, 0, 0, 0x354, 22),
    ("mmpll", 0x360, 0x36C, 0x360, 0xFF000000, 0, HAVE_RST_BAR, 0x800000, 0, 0, 0, 0x364, 22),
    ("npupll", 0x3B4, 0x3C0, 0x3B4, 0xFF000000, 0, 0, 1, 0, 0, 0, 0x3B8, 22),
    ("mfgpll", 0x268, 0x274, 0x268, 0, 0, 0x04000000, 1, 0, 0, 0, 0x26C, 22),
    ("tvdpll", 0x380, 0x38C, 0x380, 0, 0, 0, 1, 0, 0, 0, 0x384, 22),
    ("apll1", 0x318, 0x328, 0x318, 0, 0, 0, 1, 0x40, 0x0C, 0, 0x320, 32),
    ("apll2", 0x32C, 0x33C, 0x32C, 0, 0, 0, 1, 0x44, 0x0C, 5, 0x334, 32),
    ("usbpll", 0x3C4, 0x3CC, 0x3CC, 0, 2, HAVE_RST_BAR, 0x800000, 0, 0, 0, 0x3C4, 22),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def align(value):
    return (value + PAGE - 1) & ~(PAGE - 1)


def parse_elf(path, expected_sha):
    raw = path.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == expected_sha,
            "Stock module identity mismatch: " + path.name)
    require(len(raw) >= 64 and raw[:6] == b"\x7fELF\x02\x01",
            "Expected ELF64 little-endian module: " + path.name)
    require(struct.unpack_from("<HH", raw, 16) == (1, 183),
            "Expected AArch64 ET_REL module: " + path.name)
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", raw, 58)
    require(shentsize == 64 and shnum and shstrndx < shnum and
            shoff + shentsize * shnum <= len(raw),
            "Invalid ELF section table: " + path.name)
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * shentsize)
                for i in range(shnum)]

    def contents(index):
        require(0 <= index < shnum, "ELF section index out of range")
        section = sections[index]
        if section[1] == SHT_NOBITS:
            return bytes(section[5])
        start, size = section[4], section[5]
        require(start + size <= len(raw), "ELF section exceeds module")
        return raw[start:start + size]

    section_names = []
    shstr = contents(shstrndx)
    for section in sections:
        offset = section[0]
        require(offset < len(shstr), "ELF section name outside string table")
        end = shstr.find(b"\0", offset)
        require(end >= 0, "Unterminated ELF section name")
        section_names.append(shstr[offset:end].decode("ascii"))

    symbol_tables = {}
    symbols_by_name = {}
    for index, section in enumerate(sections):
        if section[1] != SHT_SYMTAB:
            continue
        require(section[9] == 24 and section[5] % 24 == 0,
                "Malformed ELF64 symbol table")
        names = contents(section[6])
        entries = []
        data = contents(index)
        for offset in range(0, len(data), 24):
            name_offset, info, other, shndx, value, size = struct.unpack_from(
                "<IBBHQQ", data, offset
            )
            require(name_offset < len(names), "ELF symbol name out of bounds")
            end = names.find(b"\0", name_offset)
            require(end >= 0, "Unterminated ELF symbol name")
            name = names[name_offset:end].decode("ascii")
            entry = (name, info, other, shndx, value, size)
            entries.append(entry)
            if name:
                symbols_by_name.setdefault(name, []).append(entry)
        symbol_tables[index] = entries

    return {
        "path": path,
        "raw": raw,
        "sha256": expected_sha,
        "sections": sections,
        "section_names": section_names,
        "contents": contents,
        "symbol_tables": symbol_tables,
        "symbols_by_name": symbols_by_name,
    }


def section_index(elf, name):
    matches = [i for i, section_name in enumerate(elf["section_names"])
               if section_name == name]
    require(len(matches) == 1, "Missing or duplicate ELF section: " + name)
    return matches[0]


def find_symbol(elf, name, symbol_type=None):
    matches = [entry for entry in elf["symbols_by_name"].get(name, [])
               if entry[3] != 0 and
               (symbol_type is None or (entry[1] & 0x0F) == symbol_type)]
    require(len(matches) == 1, "Missing/duplicate ELF symbol: " + name)
    return matches[0]


def relocations_for(elf, target_section):
    result = []
    for index, section in enumerate(elf["sections"]):
        if section[1] != SHT_RELA or section[7] != target_section:
            continue
        require(section[9] == 24 and section[5] % 24 == 0,
                "Malformed ELF64 RELA section")
        symbols = elf["symbol_tables"].get(section[6])
        require(symbols is not None, "RELA refers to missing symbol table")
        data = elf["contents"](index)
        for offset in range(0, len(data), 24):
            place, info, addend = struct.unpack_from("<QQq", data, offset)
            symbol_index = info >> 32
            require(symbol_index < len(symbols), "Relocation symbol index out of range")
            result.append((place, info & 0xFFFFFFFF, addend, symbols[symbol_index]))
    return result


def verify_stock_table(mtk):
    rodata_index = section_index(mtk, ".rodata")
    table = find_symbol(mtk, "apmixed_plls", 1)
    require(table[3] == rodata_index and table[5] == ENTRY_SIZE * len(EXPECTED_ROWS),
            "Stock apmixed_plls symbol extent changed")
    rodata = mtk["contents"](rodata_index)
    rows = []
    for index, expected in enumerate(EXPECTED_ROWS):
        offset = table[4] + index * ENTRY_SIZE
        require(offset + ENTRY_SIZE <= len(rodata), "PLL row outside stock rodata")
        row = rodata[offset:offset + ENTRY_SIZE]
        fields = (
            expected[0],
            struct.unpack_from("<I", row, 0x00)[0],
            struct.unpack_from("<I", row, 0x20)[0],
            struct.unpack_from("<I", row, 0x24)[0],
            struct.unpack_from("<I", row, 0x28)[0],
            struct.unpack_from("<I", row, 0x2C)[0],
            row[0xB8],
            struct.unpack_from("<I", row, 0x6C)[0],
            struct.unpack_from("<I", row, 0x78)[0],
            struct.unpack_from("<I", row, 0x5C)[0],
            struct.unpack_from("<I", row, 0x60)[0],
            row[0x64],
            struct.unpack_from("<I", row, 0x98)[0],
            struct.unpack_from("<I", row, 0x90)[0],
        )
        require(fields[0] == expected[0] and fields[1:] == (index,) + expected[1:],
                "Stock apmixed_plls row changed: " + expected[0])
        require(struct.unpack_from("<I", row, 0x9C)[0] == 0,
                "Unexpected stock PCW shift: " + expected[0])
        rows.append({
            "name": expected[0],
            "id": index,
            "reg": hex(fields[2]),
            "power_reg": hex(fields[3]),
            "en_reg": hex(fields[4]),
            "en_mask": hex(fields[5]),
            "en_bit": fields[6],
            "flags": hex(fields[7]),
            "reset_bar_mask": hex(fields[8]),
            "tuner_reg": hex(fields[9]),
            "tuner_en_reg": hex(fields[10]),
            "tuner_en_bit": fields[11],
            "pcw_reg": hex(fields[12]),
            "pcw_bits": fields[13],
        })
    table_file_offset = mtk["sections"][rodata_index][4] + table[4]
    return rodata_index, table, rodata, rows, table_file_offset


def verify_common_ops(common):
    text_index = section_index(common, ".text")
    rodata_index = section_index(common, ".rodata")
    funcs = {}
    for name, extent in FUNCTIONS.items():
        symbol = find_symbol(common, name, 2)
        require(symbol[3] == text_index and symbol[4:] == extent,
                "Stock function extent changed: " + name)
        require(symbol[4] + symbol[5] <= len(common["contents"](text_index)),
                "Stock function outside .text: " + name)
        funcs[name] = symbol

    ops = find_symbol(common, "mtk_pll_ops", 1)
    require(ops[3] == rodata_index and ops[4] == 0 and ops[5] == 0xD8,
            "Stock mtk_pll_ops extent changed")
    text_relocations = relocations_for(common, text_index)
    expected = EXPECTED_CALLS
    calls = {}
    ranges = [(offset, offset + size) for offset, size in FUNCTIONS.values()]
    for place, kind, addend, symbol in text_relocations:
        if kind != R_AARCH64_CALL26 or not any(lo <= place < hi for lo, hi in ranges):
            continue
        require(symbol[3] == 0, "Unexpected relocated non-external call in PLL paths")
        calls[place] = symbol[0]
    require(calls == expected, "Stock PLL external-call sites changed: " + repr(calls))

    op_relocations = relocations_for(common, rodata_index)
    callbacks = {}
    for place, kind, addend, symbol in op_relocations:
        if not ops[4] <= place < ops[4] + ops[5]:
            continue
        require(kind == R_AARCH64_ABS64 and symbol[3] == text_index,
                "Unexpected relocation in stock mtk_pll_ops")
        target = symbol[4] + addend
        callbacks[place - ops[4]] = target
    expected_callbacks = {
        0: FUNCTIONS["mtk_pll_prepare"][0],
        8: FUNCTIONS["mtk_pll_unprepare"][0],
        0x10: FUNCTIONS["mtk_pll_is_prepared"][0],
    }
    require(all(callbacks.get(slot) == target
                for slot, target in expected_callbacks.items()),
            "Stock mtk_pll_ops no longer points at the verified functions")
    return text_index, rodata_index, funcs, calls, callbacks


def apply_text_relocations(common, uc, text_index, section_bases, ranges, stub_addresses):
    text = common["contents"](text_index)
    actual_calls = {}
    applied = 0
    supported = {
        R_AARCH64_ADR_PREL_PG_HI21,
        R_AARCH64_ADD_ABS_LO12_NC,
        R_AARCH64_LDST8_ABS_LO12_NC,
        R_AARCH64_LDST32_ABS_LO12_NC,
        R_AARCH64_LDST64_ABS_LO12_NC,
        R_AARCH64_CALL26,
    }
    shifts = {
        R_AARCH64_LDST8_ABS_LO12_NC: 0,
        R_AARCH64_LDST32_ABS_LO12_NC: 2,
        R_AARCH64_LDST64_ABS_LO12_NC: 3,
    }
    for place_offset, kind, addend, symbol in relocations_for(common, text_index):
        if not any(lo <= place_offset < hi for lo, hi in ranges):
            continue
        require(kind in supported, "Unsupported relocation in selected PLL code: " + str(kind))
        if symbol[3] == 0:
            require(kind == R_AARCH64_CALL26 and symbol[0] in stub_addresses,
                    "Unrecognized external reference in selected PLL code: " + symbol[0])
            target = stub_addresses[symbol[0]] + addend
            actual_calls[place_offset] = symbol[0]
        elif symbol[3] == SHN_ABS:
            target = symbol[4] + addend
        else:
            require(symbol[3] in section_bases,
                    "Relocation refers to an unmapped section: " + symbol[0])
            target = section_bases[symbol[3]] + symbol[4] + addend

        place = TEXT_BASE + place_offset
        instruction = struct.unpack("<I", uc.mem_read(place, 4))[0]
        if kind == R_AARCH64_ADR_PREL_PG_HI21:
            delta = (target >> 12) - (place >> 12)
            require(-(1 << 20) <= delta < (1 << 20), "ADRP relocation out of range")
            instruction = (instruction & ~((3 << 29) | (0x7FFFF << 5))) | (
                ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5)
            )
        elif kind == R_AARCH64_ADD_ABS_LO12_NC:
            instruction = (instruction & ~(0xFFF << 10)) | ((target & 0xFFF) << 10)
        elif kind in shifts:
            shift = shifts[kind]
            require((target & ((1 << shift) - 1)) == 0,
                    "Misaligned AArch64 load/store relocation")
            instruction = (instruction & ~(0xFFF << 10)) | (
                ((target & 0xFFF) >> shift) << 10
            )
        else:
            delta = target - place
            require(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27),
                    "CALL26 relocation out of range")
            instruction = (instruction & ~0x3FFFFFF) | ((delta >> 2) & 0x3FFFFFF)
        uc.mem_write(place, struct.pack("<I", instruction))
        applied += 1
    require(actual_calls == EXPECTED_CALLS,
            "Applied PLL call relocations differ from the inspected imports")
    return applied


def register_value(uc, offset):
    return struct.unpack("<I", uc.mem_read(MMIO_BASE + offset, 4))[0]


def seed_registers(row):
    offsets = {row["reg"], row["power"], row["en_reg"], row["pcw_reg"]}
    if row["tuner_reg"]:
        offsets.add(row["tuner_reg"])
    if row["tuner_en_reg"]:
        offsets.add(row["tuner_en_reg"])
    state = {
        offset: (0xA5C39E71 ^ ((row["id"] + 1) * 0x31415927) ^
                 (offset * 0x01020408)) & 0xFFFFFFFF
        for offset in offsets
    }
    if row["flags"] & HAVE_RST_BAR:
        state[row["reg"]] &= ~row["reset_bar"]
    state[row["en_reg"]] &= ~(row["en_mask"] | (1 << row["en_bit"]))
    state[row["power"]] = (state[row["power"]] | 2) & ~1
    if row["tuner_en_reg"]:
        state[row["tuner_en_reg"]] &= ~(1 << row["tuner_bit"])
    elif row["tuner_reg"]:
        state[row["tuner_reg"]] &= ~(1 << 31)
    return state, offsets


def plan_read(events, state, offset):
    value = state[offset]
    events.extend((
        ("log_read_mmio", offset, 32),
        ("read", offset, value),
        ("log_post_read_mmio", offset, value),
    ))
    return value


def plan_write(events, state, offset, value):
    value &= 0xFFFFFFFF
    events.extend((
        ("log_write_mmio", offset, value),
        ("write", offset, value),
        ("log_post_write_mmio", offset, value),
    ))
    state[offset] = value


def plan_prepare(row, state):
    events = []
    value = plan_read(events, state, row["power"])
    plan_write(events, state, row["power"], value | 1)
    events.append(("delay", 0x1934, 0x10C7))

    value = plan_read(events, state, row["power"])
    plan_write(events, state, row["power"], value & ~2)
    events.append(("delay", 0x1950, 0x10C7))

    if row["en_mask"]:
        value = plan_read(events, state, row["en_reg"])
        plan_write(events, state, row["en_reg"], value | row["en_mask"])
    value = plan_read(events, state, row["en_reg"])
    plan_write(events, state, row["en_reg"], value | (1 << row["en_bit"]))

    if row["tuner_en_reg"]:
        value = plan_read(events, state, row["tuner_en_reg"])
        plan_write(events, state, row["tuner_en_reg"], value | (1 << row["tuner_bit"]))
    elif row["tuner_reg"]:
        value = plan_read(events, state, row["tuner_reg"])
        plan_write(events, state, row["tuner_reg"], value | (1 << 31))

    events.append(("delay", 0x19EC, 0x14F8C))
    if row["flags"] & HAVE_RST_BAR:
        value = plan_read(events, state, row["reg"])
        plan_write(events, state, row["reg"], value | row["reset_bar"])
    return events


def plan_unprepare(row, state):
    events = []
    if row["flags"] & HAVE_RST_BAR:
        value = plan_read(events, state, row["reg"])
        plan_write(events, state, row["reg"], value & ~row["reset_bar"])

    if row["tuner_en_reg"]:
        value = plan_read(events, state, row["tuner_en_reg"])
        plan_write(events, state, row["tuner_en_reg"], value & ~(1 << row["tuner_bit"]))
    elif row["tuner_reg"]:
        value = plan_read(events, state, row["tuner_reg"])
        plan_write(events, state, row["tuner_reg"], value & ~(1 << 31))

    # Stock code clears the gate first, then uses a second RMW for en_mask.
    value = plan_read(events, state, row["en_reg"])
    plan_write(events, state, row["en_reg"], value & ~(1 << row["en_bit"]))
    if row["en_mask"]:
        value = plan_read(events, state, row["en_reg"])
        plan_write(events, state, row["en_reg"], value & ~row["en_mask"])

    value = plan_read(events, state, row["power"])
    plan_write(events, state, row["power"], value | 2)
    value = plan_read(events, state, row["power"])
    plan_write(events, state, row["power"], value & ~1)
    return events


def plan_status(row, state, enabled):
    if not enabled:
        return [], 0
    events = []
    value = plan_read(events, state, row["en_reg"])
    return events, (value >> row["en_bit"]) & 1


def load_native_clock_traces():
    report_path = "analysis/mediatek-driver-selected-tests.json"
    if not NATIVE_SELECTED_REPORT.is_file():
        return None, {
            "status": "not_run",
            "reason": "native selected-test report is missing",
            "native_report": report_path,
        }

    raw = NATIVE_SELECTED_REPORT.read_bytes()
    native = json.loads(raw)
    require(isinstance(native, dict), "Native selected-test report is not an object")
    source_hashes = native.get("source_sha256")
    require(isinstance(source_hashes, dict),
            "Native selected-test report has no source SHA map")
    metadata = {
        "native_report": report_path,
        "native_report_sha256": hashlib.sha256(raw).hexdigest(),
        "native_runner_sha256": native.get("runner_sha256"),
        "clock_source_sha256": {
            path: source_hashes[path]
            for path in CLOCK_SOURCE_PATHS if path in source_hashes
        },
    }

    results = native.get("results")
    require(isinstance(results, list),
            "Native selected-test results are not an array")
    matches = [item for item in results
               if isinstance(item, dict) and item.get("test") == "ClockTest"]
    if not matches:
        return None, {
            **metadata,
            "status": "not_run",
            "reason": "ClockTest result is absent",
        }
    require(len(matches) == 1, "Native report has duplicate ClockTest results")

    clock_result = matches[0]
    traces = clock_result.get("pll_traces")
    if traces is None:
        return None, {
            **metadata,
            "status": "not_run",
            "reason": "ClockTest pll_traces are absent",
        }

    require(str(clock_result.get("status", "")).lower() in ("pass", "passed"),
            "Native ClockTest did not report pass")
    require(isinstance(traces, list) and len(traces) == len(EXPECTED_ROWS),
            "Native ClockTest must provide exactly 14 PLL traces")
    require(isinstance(metadata["native_runner_sha256"], str),
            "Native selected-test runner SHA is missing")
    require(all(path in source_hashes for path in CLOCK_SOURCE_PATHS),
            "Native report is missing one or more clock source identities")
    require(all(isinstance(source_hashes[path], str) and source_hashes[path]
                for path in CLOCK_SOURCE_PATHS),
            "Native clock source SHA map contains an invalid value")
    return traces, metadata


def compare_native_clock_traces(traces, metadata, original_results):
    traces_by_id = {}
    for trace in traces:
        require(isinstance(trace, dict), "Native PLL trace is not an object")
        row_id = trace.get("id")
        require(type(row_id) is int and 0 <= row_id < len(EXPECTED_ROWS),
                "Native PLL trace id is invalid")
        require(row_id not in traces_by_id, "Native PLL trace id is duplicated")
        require(trace.get("pll") == EXPECTED_ROWS[row_id][0],
                "Native PLL trace name does not match stock row id")
        normalized = {"id": row_id, "pll": trace["pll"]}
        for phase in ("prepare", "unprepare"):
            writes = trace.get(phase + "_writes")
            require(isinstance(writes, list),
                    "Native PLL trace is missing " + phase + "_writes")
            for pair in writes:
                require(isinstance(pair, list) and len(pair) == 2 and
                        type(pair[0]) is int and type(pair[1]) is int,
                        "Native PLL write trace must contain integer [offset,value] pairs")
            normalized[phase + "_writes"] = writes
        traces_by_id[row_id] = normalized

    require(set(traces_by_id) == set(range(len(EXPECTED_ROWS))),
            "Native PLL traces do not cover all 14 row ids")
    compared = []
    for row_id, original in enumerate(original_results):
        trace = traces_by_id[row_id]
        require(original["id"] == row_id and original["pll"] == trace["pll"],
                "Original/native PLL identity mismatch at row " + str(row_id))
        for phase in ("prepare", "unprepare"):
            original_writes = original[phase + "_writes"]
            native_writes = trace[phase + "_writes"]
            require(original_writes == native_writes,
                    "Original ARM64/native C " + phase + " writes differ for " + trace["pll"])
        compared.append({
            "id": row_id,
            "pll": trace["pll"],
            "prepare_write_count": len(trace["prepare_writes"]),
            "unprepare_write_count": len(trace["unprepare_writes"]),
            "writes_match_exactly": True,
        })
    return {
        **metadata,
        "status": "passed",
        "cases": len(compared),
        "compared_plls": compared,
        "comparison": "original ARM64 write events only; C initial IsEnabled reads are not compared",
    }


def main():
    mtk = parse_elf(MTK_BINARY, MTK_SHA256)
    common = parse_elf(COMMON_BINARY, COMMON_SHA256)
    table_section, table_symbol, table_data, row_reports, table_file_offset = verify_stock_table(mtk)
    text_index, common_rodata_index, funcs, call_sites, callbacks = verify_common_ops(common)
    text = common["contents"](text_index)
    text_file_offset = common["sections"][text_index][4]
    bss_index = section_index(common, ".bss")
    bss_size = common["sections"][bss_index][5]
    require(bss_size > 0x80, "Stock PLL status gate is outside .bss")

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT_BASE, align(len(text)))
    uc.mem_write(TEXT_BASE, text)
    uc.mem_map(BSS_BASE, align(bss_size))
    uc.mem_map(STUB_BASE, 0x2000)
    uc.mem_map(TABLE_BASE, align(len(table_data)))
    uc.mem_write(TABLE_BASE, table_data)
    uc.mem_map(DATA_BASE, PAGE)
    uc.mem_map(MMIO_BASE, PAGE)
    uc.mem_map(STACK_BASE, 0x10000)

    stub_addresses = {name: STUB_BASE + index * 0x20
                      for index, name in enumerate(EXTERNALS)}
    for address in stub_addresses.values():
        uc.mem_write(address, struct.pack("<I", 0xD65F03C0))  # ret; hooks intercept all entries
    ranges = [(symbol[4], symbol[4] + symbol[5]) for symbol in funcs.values()]
    section_bases = {text_index: TEXT_BASE, bss_index: BSS_BASE}
    relocation_count = apply_text_relocations(
        common, uc, text_index, section_bases, ranges, stub_addresses
    )

    events = []
    delays = []
    pending = {"value": None}
    active_offsets = set()
    object_address = DATA_BASE + 0x100
    allowed_code = [(TEXT_BASE + start, TEXT_BASE + end) for start, end in ranges]
    external_names = {address: name for name, address in stub_addresses.items()}

    def return_from_import(machine, value=0):
        machine.reg_write(UC_ARM64_REG_X0, value & 0xFFFFFFFFFFFFFFFF)
        machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))

    def hook_code(machine, address, _size, _user):
        if address == STOP:
            machine.emu_stop()
            return
        name = external_names.get(address)
        if name is not None:
            x0 = machine.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFFFFFFFFFF
            x1 = machine.reg_read(UC_ARM64_REG_X1) & 0xFFFFFFFFFFFFFFFF
            x2 = machine.reg_read(UC_ARM64_REG_X2) & 0xFFFFFFFFFFFFFFFF
            x3 = machine.reg_read(UC_ARM64_REG_X3) & 0xFFFFFFFFFFFFFFFF
            x4 = machine.reg_read(UC_ARM64_REG_X4) & 0xFFFFFFFFFFFFFFFF
            if name == "__const_udelay":
                callsite = (machine.reg_read(UC_ARM64_REG_LR) - TEXT_BASE - 4) & 0xFFFFFFFF
                require(DELAY_ARGS.get(callsite) == x0,
                        "Unexpected stock PLL delay call/argument: " + hex(callsite))
                delays.append((callsite, x0))
                events.append(("delay", callsite, x0))
                return_from_import(machine)
                return
            if name == "log_read_mmio":
                require(x0 == 32 and MMIO_BASE <= x1 < MMIO_BASE + PAGE and
                        x2 == TEXT_BASE + FUNCTIONS["readl"][0] and
                        x3 - TEXT_BASE in READ_CALLER_RETURNS,
                        "Unexpected stock readl trace arguments/context")
                offset = x1 - MMIO_BASE
                require(offset in active_offsets and pending["value"] is None,
                        "Unexpected or nested stock MMIO read")
                pending.update({"kind": "read", "address": x1, "offset": offset,
                                "value": None, "caller_return": x3})
                events.append((name, offset, 32))
            elif name == "log_post_read_mmio":
                require(x1 == 32 and MMIO_BASE <= x2 < MMIO_BASE + PAGE,
                        "Unexpected stock post-read trace arguments")
                offset = x2 - MMIO_BASE
                require(x3 == TEXT_BASE + 0x3474 and
                        x4 == pending.get("caller_return") and
                        pending.get("kind") == "read" and pending.get("address") == x2 and
                        pending.get("value") == x0 and
                        register_value(machine, offset) == x0,
                        "Stock post-read trace does not match the original readl value")
                events.append((name, offset, x0 & 0xFFFFFFFF))
                pending.clear()
                pending["value"] = None
            elif name == "log_write_mmio":
                require(x1 == 32 and MMIO_BASE <= x2 < MMIO_BASE + PAGE and
                        x3 == TEXT_BASE + 0x34C0 and
                        x4 - TEXT_BASE in WRITE_CALLER_RETURNS,
                        "Unexpected stock writel trace arguments/context")
                offset = x2 - MMIO_BASE
                require(offset in active_offsets and pending["value"] is None,
                        "Unexpected or nested stock MMIO write")
                pending.update({"kind": "write", "address": x2, "offset": offset,
                                "value": x0 & 0xFFFFFFFF, "caller_return": x4})
                events.append((name, offset, x0 & 0xFFFFFFFF))
            elif name == "log_post_write_mmio":
                require(x1 == 32 and MMIO_BASE <= x2 < MMIO_BASE + PAGE,
                        "Unexpected stock post-write trace arguments")
                offset = x2 - MMIO_BASE
                require(x3 == TEXT_BASE + 0x34F4 and
                        x4 == pending.get("caller_return") and
                        pending.get("kind") == "write" and pending.get("address") == x2 and
                        pending.get("value") == (x0 & 0xFFFFFFFF) and
                        register_value(machine, offset) == (x0 & 0xFFFFFFFF),
                        "Stock post-write trace does not match the original writel value")
                events.append((name, offset, x0 & 0xFFFFFFFF))
                pending.clear()
                pending["value"] = None
            else:
                raise ValueError("Unrecognized external call reached: " + name)
            return_from_import(machine)
            return
        require(any(start <= address < end for start, end in allowed_code),
                "Stock PLL execution escaped selected functions: " + hex(address))

    def hook_memory(machine, access, address, size, value, _user):
        require(size == 4 and MMIO_BASE <= address < MMIO_BASE + PAGE,
                "Stock PLL touched unexpected MMIO width/address")
        offset = address - MMIO_BASE
        require(offset in active_offsets, "Stock PLL touched an unlisted register: " + hex(offset))
        if access == UC_MEM_READ:
            require(pending.get("kind") == "read" and pending.get("address") == address and
                    pending.get("value") is None,
                    "Unlogged or unexpected original MMIO read")
            read_value = register_value(machine, offset)
            pending["value"] = read_value
            events.append(("read", offset, read_value))
        elif access == UC_MEM_WRITE:
            require(pending.get("kind") == "write" and pending.get("address") == address and
                    pending.get("value") == (value & 0xFFFFFFFF),
                    "Unlogged or unexpected original MMIO write")
            events.append(("write", offset, value & 0xFFFFFFFF))
        else:
            raise ValueError("Unexpected Unicorn MMIO access kind")

    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.hook_add(UC_HOOK_MEM_READ, hook_memory,
                begin=MMIO_BASE, end=MMIO_BASE + PAGE - 1)
    uc.hook_add(UC_HOOK_MEM_WRITE, hook_memory,
                begin=MMIO_BASE, end=MMIO_BASE + PAGE - 1)

    def invoke(function_name):
        events.clear()
        delays.clear()
        pending.clear()
        pending["value"] = None
        uc.reg_write(UC_ARM64_REG_X0, object_address)
        uc.reg_write(UC_ARM64_REG_SP, STACK_BASE + 0xF000)
        uc.reg_write(UC_ARM64_REG_LR, STOP)
        uc.emu_start(TEXT_BASE + FUNCTIONS[function_name][0], STOP + 4, count=8000)
        require(uc.reg_read(UC_ARM64_REG_PC) == STOP,
                "Stock PLL function did not return: " + function_name)
        require(pending.get("value") is None and "kind" not in pending,
                "Stock MMIO instrumentation call was left unmatched")
        return list(events), list(delays), uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFF

    results = []
    for row_id, expected in enumerate(EXPECTED_ROWS):
        row = {
            "id": row_id,
            "reg": expected[1], "power": expected[2], "en_reg": expected[3],
            "en_mask": expected[4], "en_bit": expected[5], "flags": expected[6],
            "reset_bar": expected[7], "tuner_reg": expected[8],
            "tuner_en_reg": expected[9], "tuner_bit": expected[10],
            "pcw_reg": expected[11], "pcw_bits": expected[12],
        }
        state, active_offsets = seed_registers(row)
        pcw_mask = (1 << row["pcw_bits"]) - 1 if row["pcw_bits"] < 32 else 0xFFFFFFFF
        initial_pcw = state[row["pcw_reg"]] & pcw_mask
        uc.mem_write(MMIO_BASE, bytes(PAGE))
        for offset, value in state.items():
            uc.mem_write(MMIO_BASE + offset, struct.pack("<I", value))
        uc.mem_write(DATA_BASE, bytes(PAGE))
        uc.mem_write(object_address + 0x18, struct.pack("<Q", MMIO_BASE + row["reg"]))
        uc.mem_write(object_address + 0x28, struct.pack("<Q", MMIO_BASE + row["power"]))
        uc.mem_write(object_address + 0x30, struct.pack(
            "<Q", MMIO_BASE + row["tuner_reg"] if row["tuner_reg"] else 0
        ))
        uc.mem_write(object_address + 0x38, struct.pack(
            "<Q", MMIO_BASE + row["tuner_en_reg"] if row["tuner_en_reg"] else 0
        ))
        uc.mem_write(object_address + 0x50, struct.pack("<Q", MMIO_BASE + row["en_reg"]))
        descriptor = TABLE_BASE + table_symbol[4] + row_id * ENTRY_SIZE
        uc.mem_write(object_address + 0x88, struct.pack("<Q", descriptor))

        # The original is_prepared path is gated by a module .bss byte.
        uc.mem_write(BSS_BASE + 0x80, b"\0")
        actual, actual_delays, output = invoke("mtk_pll_is_prepared")
        require(output == 0 and actual == [],
                "Original .bss gate 0 did not suppress is_prepared MMIO")

        expected_trace = plan_prepare(row, state)
        actual, actual_delays, output = invoke("mtk_pll_prepare")
        require(output == 0 and actual == expected_trace and
                actual_delays == [(0x1934, 0x10C7), (0x1950, 0x10C7), (0x19EC, 0x14F8C)],
                "Original PLL prepare MMIO/delay order changed: " + expected[0])
        require(state[row["pcw_reg"]] & pcw_mask == initial_pcw,
                "PLL prepare changed PCW bits: " + expected[0])
        prepare_trace = [[event[0], hex(event[1]), hex(event[2])] for event in actual]
        prepare_writes = [[event[1], event[2]] for event in actual if event[0] == "write"]
        pcw_preserved_after_prepare = True

        uc.mem_write(BSS_BASE + 0x80, b"\0")
        actual, actual_delays, output = invoke("mtk_pll_is_prepared")
        require(output == 0 and actual == [],
                "Original .bss gate 0 did not suppress status read while enabled")

        uc.mem_write(BSS_BASE + 0x80, b"\1")
        expected_trace, expected_status = plan_status(row, state, True)
        actual, actual_delays, output = invoke("mtk_pll_is_prepared")
        require(output == expected_status == 1 and actual == expected_trace and not actual_delays,
                "Original .bss gate 1 did not report enabled en_bit: " + expected[0])
        status_on_trace = [[event[0], hex(event[1]), hex(event[2])] for event in actual]

        expected_trace = plan_unprepare(row, state)
        actual, actual_delays, output = invoke("mtk_pll_unprepare")
        require(actual == expected_trace and not actual_delays,
                "Original PLL unprepare MMIO order changed: " + expected[0])
        require(state[row["pcw_reg"]] & pcw_mask == initial_pcw,
                "PLL unprepare changed PCW bits: " + expected[0])
        pcw_preserved_after_unprepare = True
        require_state = {offset: state[offset] for offset in active_offsets}
        for offset, expected_value in require_state.items():
            actual_value = register_value(uc, offset)
            require(actual_value == expected_value,
                    "PLL final register mismatch at " + hex(offset) + ": " + expected[0])
        unprepare_trace = [[event[0], hex(event[1]), hex(event[2])] for event in actual]
        unprepare_writes = [[event[1], event[2]] for event in actual if event[0] == "write"]

        expected_trace, expected_status = plan_status(row, state, True)
        actual, actual_delays, output = invoke("mtk_pll_is_prepared")
        require(output == expected_status == 0 and actual == expected_trace and not actual_delays,
                "Original .bss gate 1 did not report disabled en_bit: " + expected[0])
        status_off_trace = [[event[0], hex(event[1]), hex(event[2])] for event in actual]
        require(state[row["pcw_reg"]] & pcw_mask == initial_pcw,
                "PLL sequence changed PCW bits: " + expected[0])

        results.append({
            "id": row_id,
            "pll": expected[0],
            "prepare_mmio_trace": prepare_trace,
            "prepare_writes": prepare_writes,
            "prepare_delay_call_sites": [
                {"text_offset": hex(site), "argument": hex(argument)}
                for site, argument in ((0x1934, 0x10C7), (0x1950, 0x10C7), (0x19EC, 0x14F8C))
            ],
            "unprepare_mmio_trace": unprepare_trace,
            "unprepare_writes": unprepare_writes,
            "is_prepared_gate_zero": {"return": 0, "mmio_reads": 0},
            "is_prepared_gate_one_after_prepare": {"return": 1, "trace": status_on_trace},
            "is_prepared_gate_one_after_unprepare": {"return": 0, "trace": status_off_trace},
            "power_and_enable_sequence_matched": True,
            "reset_bar_sequence_matched": bool(row["flags"] & HAVE_RST_BAR),
            "pcw_bits_preserved": {
                "after_prepare": pcw_preserved_after_prepare,
                "after_unprepare": pcw_preserved_after_unprepare,
            },
            "final_register_values_matched": True,
        })

    require(len(results) == 14, "Not all stock MT6789 PLL rows were exercised")
    native_traces, native_metadata = load_native_clock_traces()
    if native_traces is None:
        c_trace_comparison = native_metadata
    else:
        c_trace_comparison = compare_native_clock_traces(
            native_traces, native_metadata, results
        )
    report = {
        "schema": 1,
        "status": "passed",
        "scope": "Original stock MT6789 apmixed_plls table plus clk-common prepare/unprepare/is_prepared ARM64 bodies in Unicorn; synthetic MMIO only",
        "device_accessed": False,
        "kernel_module_loaded": False,
        "modules": {
            "clk-mt6789.ko": {
                "sha256": MTK_SHA256,
                "build_id": "d3f272bb8288f27ef6e1cca5476ed1584143c8ac",
                "apmixed_plls": {
                    "symbol_section_offset": hex(table_symbol[4]),
                    "file_offset": hex(table_file_offset),
                    "bytes": table_symbol[5],
                    "entry_bytes": ENTRY_SIZE,
                    "entries": row_reports,
                    "table_sha256": hashlib.sha256(
                        mtk["raw"][table_file_offset:table_file_offset + table_symbol[5]]
                    ).hexdigest(),
                },
            },
            "clk-common.ko": {
                "sha256": COMMON_SHA256,
                "build_id": "c4c2b34ce6e2d16748dd11157d18c4ed838a583b",
                "functions": {
                    name: {
                        "text_offset": hex(symbol[4]),
                        "file_offset": hex(text_file_offset + symbol[4]),
                        "bytes": symbol[5],
                    }
                    for name, symbol in funcs.items()
                },
                "mtk_pll_ops": {
                    "rodata_offset": hex(find_symbol(common, "mtk_pll_ops", 1)[4]),
                    "callback_targets": {hex(slot): hex(target) for slot, target in callbacks.items()},
                },
            },
        },
        "external_calls": {
            "verified_delay_stub": "__const_udelay; exact call sites and arguments checked; no simulated time",
            "validated_mmio_observers": [
                "log_read_mmio", "log_post_read_mmio",
                "log_write_mmio", "log_post_write_mmio",
            ],
            "unknown_calls": "rejected",
            "applied_text_relocations": relocation_count,
            "call_sites": {hex(site): name for site, name in call_sites.items()},
        },
        "is_prepared_runtime_gate": {
            "location": ".bss+0x80",
            "zero": "returns 0 without an MMIO read, tested before and after prepare",
            "one": "reads en_reg and tests BIT(pll_en_bit), tested on and off",
        },
        "cases": len(results),
        "functions_executed_per_pll": [
            "mtk_pll_prepare", "mtk_pll_unprepare", "mtk_pll_is_prepared"
        ],
        "results": results,
        "native_c_trace_comparison": c_trace_comparison,
        "limitations": [
            "No hardware PLL lock/ready status, mux parent, regulator, or live register state was tested.",
            "The four external log_* calls are modeled as argument-checked trace observers; original local readl/writel and MMIO instructions execute against fake memory.",
            "The module .bss gate is explicitly seeded to 0 or 1; the rest of clock registration and kernel runtime is not executed.",
        ],
    }
    require(ANALYSIS.is_dir(), "Existing analysis output directory is missing")
    RESULT_PATH.write_text(json.dumps(report, indent=2) + "\n")
    print("Stock MT6789 PLL ARM64 prepare/unprepare/status: 14 rows passed; no device")


if __name__ == "__main__":
    main()
