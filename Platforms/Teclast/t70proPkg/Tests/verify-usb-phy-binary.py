#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the stock T70 U2 TPHY ARM64 paths in Unicorn and compare production C.

The module is never loaded. MMIO, clock, NVMEM, DT properties and time are fake.
No USB device, kernel, tablet or physical bus is accessed. The only NVMEM value
used is a fixed synthetic fixture value and is never included in output.
"""
from pathlib import Path
import ctypes as C
import hashlib
import json
import random
import struct
import subprocess
import tempfile

from unicorn import (
    Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE,
    UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_MEM_READ, UC_MEM_WRITE,
)
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
    UC_ARM64_REG_X4, UC_ARM64_REG_X8, UC_ARM64_REG_SP, UC_ARM64_REG_LR,
    UC_ARM64_REG_PC,
)

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
BINARY = REPO.parent / "analysis/stock-binaries/vendor_boot_a_0/phy-mtk-tphy.ko"
DTB = REPO.parent / "analysis/stock-fdt/effective.dtb"
CORE_C = REPO / "Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MtkUsbPhyCore.c"
CORE_INCLUDE = REPO / "Silicon/MediaTek/MediaTekPkg/Include"
EXPECTED_BINARY_SHA256 = "81a4b0c1d029e33389f1ef296c101f7c2510dff51fe7d5e7fc0f08ca376a5a6b"
EXPECTED_DTB_SHA256 = "a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391"

MMIO_BASE = 0x50000000
MMIO_SIZE = 0x1000
DATA_BASE = 0x30000000
STACK_BASE = 0x40000000
STUB_BASE = 0x11000000
STUB_SIZE = 0x10000
STOP = STUB_BASE + STUB_SIZE - 4
TLS = DATA_BASE + 0x10000
CANARY = 0x1234567812345678
SYNTHETIC_NVMEM = 0x1B

PHY_PATH = "/soc/usb-phy@11f40000/usb-phy@11f40000"
PHY_PARENT_PATH = "/soc/usb-phy@11f40000"
EXPECTED_FUNCTIONS = {
    "mtk_phy_xlate": (0x04F8, 0x0588),
    "mtk_phy_init": (0x0A84, 0x092C),
    "mtk_phy_exit": (0x13B4, 0x00B4),
    "mtk_phy_power_on": (0x146C, 0x0494),
    "mtk_phy_power_off": (0x1904, 0x0278),
    "mtk_phy_set_mode": (0x1b80, 0x02cc),
    "u2_phy_props_set": (0x1EA4, 0x02C0),
    "mtk_phy_update_bits": (0x2164, 0x004C),
    "phy_efuse_set_v1": (0x21B0, 0x0174),
    "readl": (0x2324, 0x0080),
    "writel": (0x23A4, 0x007C),
}
ALLOC_BASE = 0x10000000
PAGE = 0x1000

# __const_udelay inputs are stock instructions, keyed by the CALL instruction
# offset from .text. Values are Linux delay-calibration arguments from the ELF.
STOCK_DELAY_CALLS = {
    0x14F8: (0x0001F752, 30),
    0x1608: (0x00346DE0, 800),
    0x179C: (0x000010C7, 1),
    0x19FC: (0x008312B0, 2000),
    0x1A4C: (0x00346DE0, 800),
    0x1A68: (0x000010C7, 1),
}
PROPERTY_READS = {
    "mediatek,eye-vrt": 4,
    "mediatek,eye-term": 4,
    "mediatek,discth": 7,
    "mediatek,rx-sqth": 5,
    "mediatek,rev4": 1,
    "mediatek,disc-host": 7,
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def align(value, boundary=PAGE):
    return (value + boundary - 1) & ~(boundary - 1)


def fdt_properties(raw):
    """Read the flattened tree structure without exposing any NVMEM payload."""
    require(len(raw) >= 40, "Truncated DTB header")
    header = struct.unpack_from(">10I", raw, 0)
    magic, total, struct_off, strings_off, _reserve, _version, _last, _boot, strings_size, struct_size = header
    require(magic == 0xD00DFEED, "Invalid DTB magic")
    require(40 <= total <= len(raw), "Invalid DTB total size")
    require(struct_off + struct_size <= total and strings_off + strings_size <= total,
            "DTB block outside total size")
    strings = raw[strings_off:strings_off + strings_size]
    cursor = struct_off
    end = struct_off + struct_size
    path = []
    tree = {}
    while cursor + 4 <= end:
        token = struct.unpack_from(">I", raw, cursor)[0]
        cursor += 4
        if token == 1:  # FDT_BEGIN_NODE
            stop = raw.find(b"\0", cursor, end)
            require(stop >= 0, "Unterminated DTB node name")
            node = raw[cursor:stop].decode("ascii")
            cursor = align(stop + 1, 4)
            path.append(node)
        elif token == 2:  # FDT_END_NODE
            require(path, "Unbalanced DTB end node")
            path.pop()
        elif token == 3:  # FDT_PROP
            require(cursor + 8 <= end, "Truncated DTB property header")
            length, name_offset = struct.unpack_from(">II", raw, cursor)
            cursor += 8
            require(name_offset < len(strings), "DTB property name outside string block")
            name_end = strings.find(b"\0", name_offset)
            require(name_end >= 0, "Unterminated DTB property name")
            name = strings[name_offset:name_end].decode("ascii")
            require(cursor + length <= end, "DTB property outside structure block")
            value = raw[cursor:cursor + length]
            node_path = "/" + "/".join(part for part in path if part)
            tree.setdefault(node_path, {})[name] = value
            cursor = align(cursor + length, 4)
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            break
        else:
            raise ValueError("Unknown DTB structure token: " + hex(token))
    return tree


def dt_u32(props, name):
    value = props.get(name)
    require(value is not None and len(value) == 4, "Missing or malformed DT u32: " + name)
    return struct.unpack(">I", value)[0]


def dt_strings(props, name):
    value = props.get(name)
    require(value is not None and value.endswith(b"\0"), "Missing or malformed DT string list: " + name)
    return [part.decode("ascii") for part in value[:-1].split(b"\0")]


def verify_t70_dtb():
    raw = DTB.read_bytes()
    require(sha256(raw) == EXPECTED_DTB_SHA256, "T70 effective DTB identity mismatch")
    tree = fdt_properties(raw)
    parent = tree.get(PHY_PARENT_PATH, {})
    phy = tree.get(PHY_PATH, {})
    require(b"mediatek,generic-tphy-v2\0" in parent.get("compatible", b""),
            "T70 TPHY parent is not generic-tphy-v2")
    require(dt_strings(phy, "clock-names") == ["ref"],
            "T70 PHY clock list changed; refusing to infer an optional slot")
    require(dt_strings(phy, "nvmem-cell-names") == ["intr_cal"],
            "T70 NVMEM cell name changed")
    require(dt_u32(phy, "nvmem-cell-masks") == 0x1F,
            "T70 NVMEM mask changed")
    require("mediatek,set-efuse-v1" in phy,
            "T70 set-efuse-v1 property is absent")
    values = {name: dt_u32(phy, name) for name in PROPERTY_READS}
    require(values == PROPERTY_READS, "T70 U2 tuning inputs changed")
    require("mediatek,bc12" not in phy and "mediatek,usb-special-phy-settings" not in phy,
            "Unexpected T70 PHY property changes the stock path")
    require("mediatek,src-ref-clk-mhz" not in parent and "mediatek,src-coef" not in parent,
            "T70 parent overrides the probe slew-rate defaults")
    return {
        "sha256": sha256(raw),
        "parent_compatible": "mediatek,generic-tphy-v2",
        "clock_names": ["ref"],
        "optional_da_ref_absent": True,
        "nvmem_cell_name": "intr_cal",
        "nvmem_mask": "0x1f",
        "set_efuse_v1": True,
        "u2_tuning": values,
    }


def parse_elf(raw):
    require(len(raw) >= 64 and raw[:6] == b"\x7fELF\x02\x01",
            "Expected ELF64 little-endian module")
    require(struct.unpack_from("<HH", raw, 16) == (1, 183),
            "Expected AArch64 relocatable object")
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", raw, 58)
    require(shentsize == 64 and shnum > 0 and shstrndx < shnum,
            "Invalid ELF section table header")
    require(shoff + shentsize * shnum <= len(raw), "ELF section table outside module")
    sections = [
        struct.unpack_from("<IIQQQQIIQQ", raw, shoff + index * shentsize)
        for index in range(shnum)
    ]

    def contents(index):
        require(0 <= index < shnum, "ELF section index outside table")
        section = sections[index]
        if section[1] == 8:  # SHT_NOBITS
            return bytes(section[5])
        offset, size = section[4], section[5]
        require(offset + size <= len(raw), "ELF section outside file")
        return raw[offset:offset + size]

    shstr = contents(shstrndx)
    section_names = []
    for section in sections:
        name_at = section[0]
        require(name_at < len(shstr), "ELF section name outside string table")
        stop = shstr.find(b"\0", name_at)
        require(stop >= 0, "Unterminated ELF section name")
        section_names.append(shstr[name_at:stop].decode("ascii"))

    symbol_tables = {}
    symbols_by_name = {}
    for index, section in enumerate(sections):
        if section[1] != 2:  # SHT_SYMTAB
            continue
        require(section[9] == 24 and section[5] % 24 == 0,
                "Malformed ELF64 symbol table")
        strings = contents(section[6])
        table = []
        data = contents(index)
        for at in range(0, len(data), 24):
            name_at, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, at)
            require(name_at < len(strings), "ELF symbol name outside string table")
            stop = strings.find(b"\0", name_at)
            require(stop >= 0, "Unterminated ELF symbol name")
            name = strings[name_at:stop].decode("ascii")
            symbol = (name, info, other, shndx, value, size)
            table.append(symbol)
            if name:
                symbols_by_name.setdefault(name, []).append(symbol)
        symbol_tables[index] = table

    symbols = {}
    for name, expected in EXPECTED_FUNCTIONS.items():
        matches = [item for item in symbols_by_name.get(name, [])
                   if item[3] != 0 and (item[1] & 0x0F) == 2]
        require(len(matches) == 1, "Missing/duplicate stock function: " + name)
        item = matches[0]
        require((item[4], item[5]) == expected,
                "Stock function fingerprint mismatch: " + name)
        symbols[name] = item
    for name, expected in (("tphy_v2_pdata", (0x648, 8)), ("efuse_name", (0x7D0, 0x20))):
        matches = [item for item in symbols_by_name.get(name, [])
                   if item[3] != 0]
        require(len(matches) == 1, "Missing/duplicate stock data: " + name)
        item = matches[0]
        require((item[4], item[5]) == expected,
                "Stock data fingerprint mismatch: " + name)
        symbols[name] = item

    return sections, section_names, contents, symbol_tables, symbols, symbols_by_name


def load_stock_elf(raw, sections, section_names, contents, symbol_tables, symbols, symbols_by_name):
    alloc_bases = {}
    cursor = ALLOC_BASE
    for index, section in enumerate(sections):
        if (section[2] & 0x2) == 0 or section[5] == 0:  # SHF_ALLOC
            continue
        cursor = align(cursor)
        alloc_bases[index] = cursor
        cursor += align(section[5])
    require(cursor < STUB_BASE, "Mapped stock sections overlap external stubs")

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for index, base in alloc_bases.items():
        size = align(sections[index][5])
        uc.mem_map(base, size)
        if sections[index][1] != 8:
            data = contents(index)
            if data:
                uc.mem_write(base, data)
    uc.mem_map(STUB_BASE, STUB_SIZE)
    uc.mem_map(DATA_BASE, 0x30000)
    uc.mem_map(STACK_BASE, 0x10000)
    uc.mem_map(MMIO_BASE, MMIO_SIZE)

    text_index = symbols["mtk_phy_init"][3]
    require(section_names[text_index] == ".text", "Stock TPHY functions left .text")
    ranges = []
    for name in EXPECTED_FUNCTIONS:
        item = symbols[name]
        require(item[3] == text_index, "Stock function is outside .text: " + name)
        ranges.append((item[3], item[4], item[4] + item[5], name))
    target_data = symbols["efuse_name"]
    ranges.append((target_data[3], target_data[4], target_data[4] + target_data[5], "efuse_name"))

    external_addresses = {}

    def symbol_address(symbol, addend=0):
        name, _info, _other, section_index, value, _size = symbol
        if section_index == 0:
            # Selected functions contain references on branches this exact DT
            # never executes. Resolve those references to trap stubs; execution
            # is still rejected by code_hook unless an explicit adapter exists.
            if name not in external_addresses:
                require(len(external_addresses) < (STUB_SIZE // 16) - 1,
                        "Too many stock external trap addresses")
                external_addresses[name] = STUB_BASE + len(external_addresses) * 16
            return external_addresses[name] + addend
        if section_index == 0xFFF1:  # SHN_ABS
            return value + addend
        require(section_index in alloc_bases,
                "Relocation references unmapped symbol: " + name)
        return alloc_bases[section_index] + value + addend

    relocations_applied = 0
    for index, section in enumerate(sections):
        if section[1] != 4:  # SHT_RELA
            continue
        require(section[9] == 24 and section[5] % 24 == 0,
                "Malformed ELF64 RELA section")
        target_index = section[7]
        if target_index not in alloc_bases:
            continue
        entries = symbol_tables.get(section[6])
        require(entries is not None, "RELA section has no valid symbol table")
        for at in range(0, section[5], 24):
            place_offset, info, addend = struct.unpack_from("<QQq", contents(index), at)
            selected = next((rng for rng in ranges
                             if rng[0] == target_index and rng[1] <= place_offset < rng[2]), None)
            if selected is None:
                continue
            symbol_index = info >> 32
            require(symbol_index < len(entries), "RELA symbol index outside table")
            target = symbol_address(entries[symbol_index], addend)
            place = alloc_bases[target_index] + place_offset
            kind = info & 0xFFFFFFFF
            if kind == 257:  # R_AARCH64_ABS64
                uc.mem_write(place, struct.pack("<Q", target & 0xFFFFFFFFFFFFFFFF))
            else:
                ins = struct.unpack("<I", uc.mem_read(place, 4))[0]
                if kind == 275:  # ADR_PREL_PG_HI21
                    delta = (target >> 12) - (place >> 12)
                    require(-(1 << 20) <= delta < (1 << 20), "AArch64 ADRP relocation overflow")
                    ins = (ins & ~((3 << 29) | (0x7FFFF << 5))) | \
                          ((delta & 3) << 29) | (((delta >> 2) & 0x7FFFF) << 5)
                elif kind in (277, 278, 284, 285, 286):
                    shift = {277: 0, 278: 0, 284: 1, 285: 2, 286: 3}[kind]
                    require((target & ((1 << shift) - 1)) == 0,
                            "Misaligned AArch64 LO12 relocation")
                    ins = (ins & ~(0xFFF << 10)) | (((target & 0xFFF) >> shift) << 10)
                elif kind in (282, 283):  # JUMP26 / CALL26
                    delta = target - place
                    require(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27),
                            "AArch64 branch relocation overflow")
                    ins = (ins & ~0x03FFFFFF) | ((delta >> 2) & 0x03FFFFFF)
                else:
                    raise ValueError("Unsupported selected AArch64 relocation: " + str(kind))
                uc.mem_write(place, struct.pack("<I", ins))
            relocations_applied += 1

    data = DATA_BASE
    provider = data + 0x0000
    root = data + 0x1000
    child = data + 0x2000
    phy = data + 0x3000
    dev = data + 0x4000
    node = data + 0x5000
    children = data + 0x6000
    args = data + 0x7000
    clock_ref = data + 0x8000

    def addr(name):
        item = symbols[name]
        return alloc_bases[item[3]] + item[4]

    pdata = addr("tphy_v2_pdata")
    require(bytes(uc.mem_read(pdata, 8)) == bytes((0, 0, 1, 0, 2, 0, 0, 0)),
            "generic-tphy-v2 match data changed")
    string_section = next((i for i, name in enumerate(section_names)
                           if name == ".rodata.str1.1"), None)
    require(string_section in alloc_bases, "Missing stock strings")
    strings_base = alloc_bases[string_section]
    ref_name = strings_base + 0x88
    da_ref_name = strings_base + 0x179
    require(_read_cstr(uc, ref_name) == "ref" and _read_cstr(uc, da_ref_name) == "da_ref",
            "Stock optional clock IDs changed")

    # The structs mirror values established by the real probe/xlate path. xlate
    # itself is executed below; only Linux-owned allocations are fixture memory.
    uc.mem_write(provider + 0x98, struct.pack("<Q", root))
    uc.mem_write(root + 0x00, struct.pack("<Q", dev))
    uc.mem_write(root + 0x10, struct.pack("<Q", pdata))
    uc.mem_write(root + 0x18, struct.pack("<Q", children))
    uc.mem_write(root + 0x20, struct.pack("<I", 1))
    # Original probe .text+0x138/0x148/0x150 writes {26 MHz, 28} at
    # offsets 0x24/0x28; power_on loads the pair at .text+0x1890.
    text = contents(text_index)
    require(struct.unpack_from("<I", text, 0x138)[0] == 0xd2800348 and
            struct.unpack_from("<I", text, 0x148)[0] == 0xf2c00388 and
            struct.unpack_from("<I", text, 0x150)[0] == 0xf8024ec8,
            "Stock slew-rate probe defaults changed")
    uc.mem_write(root + 0x24, struct.pack("<II", 26, 28))
    uc.mem_write(root + 0x30, struct.pack("<Q", 0))  # no syscon provider in fixture
    uc.mem_write(children, struct.pack("<Q", child))
    uc.mem_write(child + 0x00, struct.pack("<Q", phy))
    uc.mem_write(child + 0x08, struct.pack("<Q", MMIO_BASE))
    uc.mem_write(child + 0x38, struct.pack("<QQQQ", ref_name, clock_ref, da_ref_name, 0))
    uc.mem_write(child + 0x58, struct.pack("<I", 0))
    uc.mem_write(child + 0x60, struct.pack("<Q", 0))  # no optional regmap
    uc.mem_write(phy + 0x60, struct.pack("<Q", dev))
    uc.mem_write(phy + 0x98, struct.pack("<Q", child))
    uc.mem_write(phy + 0x2E8, struct.pack("<Q", node))
    uc.mem_write(dev + 0x98, struct.pack("<Q", root))
    uc.mem_write(dev + 0x2E8, struct.pack("<Q", node))
    uc.mem_write(args + 0x00, struct.pack("<Q", node))
    uc.mem_write(args + 0x08, struct.pack("<I", 1))
    uc.mem_write(args + 0x0C, struct.pack("<I", 3))

    regs = {0x110: 1, 0x10C: 128}
    for offset, value in regs.items():
        uc.mem_write(MMIO_BASE + offset, struct.pack("<I", value))

    return {
        "uc": uc, "stop": STOP, "text_index": text_index,
        "text_base": alloc_bases[text_index], "bases": alloc_bases,
        "symbols": symbols, "external_addresses": external_addresses,
        "ranges": ranges, "relocations": relocations_applied,
        "provider": provider, "root": root, "child": child, "phy": phy,
        "dev": dev, "node": node, "args": args, "mmio": MMIO_BASE,
        "regs": regs, "time_ns": 0, "mmio_trace": [], "clock_trace": [],
        "delay_trace": [], "events": [], "property_calls": [], "nvmem_calls": 0,
        "callback_errors": [], "tls": TLS, "canary": CANARY,
        "stop_pc": STOP, "addr": addr,
    }


def _read_cstr(uc, address, limit=256):
    data = bytes(uc.mem_read(address, limit))
    stop = data.find(b"\0")
    require(stop >= 0, "Unterminated stock C string")
    try:
        return data[:stop].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ValueError("Non-ASCII stock property/string") from exc


def invoke_stock(machine, function, *args):
    uc = machine["uc"]
    address = machine["addr"](function)
    uc.reg_write(UC_ARM64_REG_SP, STACK_BASE + 0xF000)
    uc.reg_write(UC_ARM64_REG_LR, machine["stop_pc"])
    for index, value in enumerate(args):
        uc.reg_write((UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                      UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                      UC_ARM64_REG_X4)[index], value)
    uc.emu_start(address, machine["stop_pc"], timeout=10_000_000, count=10_000_000)
    require(uc.reg_read(UC_ARM64_REG_PC) == machine["stop_pc"],
            "Stock function failed to return: " + function)
    return uc.reg_read(UC_ARM64_REG_X0) & 0xFFFFFFFFFFFFFFFF


def make_stock_runner(machine, dt):
    from unicorn.arm64_const import UC_ARM64_REG_X8

    uc = machine["uc"]
    stop = machine["stop_pc"]
    external_by_address = machine["external_addresses"]
    stub_names = {address: name for name, address in external_by_address.items()}
    ranges = machine["ranges"]
    text_base = machine["text_base"]
    known_props = dt["u2_tuning"]
    present = set(known_props) | {
        "mediatek,set-efuse-v1", "nvmem-cells", "nvmem-cell-names",
        "nvmem-cell-masks",
    }
    mmio_lo = machine["mmio"]
    mmio_hi = mmio_lo + 0x700

    def ret(value=0):
        uc.reg_write(UC_ARM64_REG_X0, value & 0xFFFFFFFFFFFFFFFF)
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    def handle_external(name, address):
        x0 = uc.reg_read(UC_ARM64_REG_X0)
        x1 = uc.reg_read(UC_ARM64_REG_X1)
        x2 = uc.reg_read(UC_ARM64_REG_X2)
        x3 = uc.reg_read(UC_ARM64_REG_X3)
        if name in ("_dev_info", "_dev_err", "_dev_warn", "proc_remove"):
            if name == "proc_remove":
                require(x0 == 0, "Unexpected procfs resource in generic-v2 fixture")
            ret(0)
            return
        if name == "__stack_chk_fail":
            raise RuntimeError("Stock TPHY stack cookie mismatch")
        if name in ("log_read_mmio", "log_post_read_mmio",
                    "log_write_mmio", "log_post_write_mmio"):
            width = x0 if name == "log_read_mmio" else x1
            ptr = x1 if name == "log_read_mmio" else x2
            require(width == 32, "Unexpected stock MMIO width")
            require(mmio_lo <= ptr < mmio_hi, "Stock MMIO outside fake PHY range")
            ret(0)
            return
        if name == "device_property_present":
            require(x0 in (machine["phy"], machine["dev"]),
                    "Unexpected stock property device")
            prop = _read_cstr(uc, x1)
            machine["property_calls"].append(("present", prop))
            ret(1 if prop in present else 0)
            return
        if name == "device_property_read_u32_array":
            require(x0 in (machine["phy"], machine["dev"]),
                    "Unexpected stock property device")
            prop = _read_cstr(uc, x1)
            require(x3 == 1, "Unexpected stock property array length")
            machine["property_calls"].append(("read_u32", prop))
            if prop in known_props:
                uc.mem_write(x2, struct.pack("<I", known_props[prop]))
                ret(0)
            else:
                ret(0xFFFFFFFFFFFFFFEA)  # -EINVAL; stock xlate ignores absent optional values
            return
        if name == "of_property_match_string":
            prop, cell = _read_cstr(uc, x1), _read_cstr(uc, x2)
            require(prop == "nvmem-cell-names" and cell == "intr_cal",
                    "Unexpected stock NVMEM-cell lookup")
            ret(0)
            return
        if name == "of_property_read_u32_index":
            prop = _read_cstr(uc, x1)
            require(prop == "nvmem-cell-masks" and x2 == 0,
                    "Unexpected stock NVMEM mask lookup")
            uc.mem_write(x3, struct.pack("<I", 0x1F))
            ret(0)
            return
        if name == "nvmem_cell_read_u32":
            cell = _read_cstr(uc, x1)
            require(cell == "intr_cal", "Unexpected stock NVMEM cell")
            machine["nvmem_calls"] += 1
            uc.mem_write(x2, struct.pack("<I", SYNTHETIC_NVMEM))
            ret(0)
            return
        if name == "nvmem_cell_read_variable_le_u32":
            raise RuntimeError("Unexpected variable NVMEM read in the T70 set-efuse-v1 path")
        if name == "of_device_is_compatible":
            compat = _read_cstr(uc, x1)
            require(compat in (
                "mediatek,mt6781-tphy", "mediatek,mt6833-tphy", "mediatek,mt6853-tphy",
                "mediatek,mt8195-tphy",
            ), "Unexpected stock compatible test: " + compat)
            ret(0)
            return
        if name == "regmap_update_bits_base":
            raise RuntimeError("Unexpected regmap access in the T70 U2 fixture")
        if name.startswith("clk_bulk_"):
            count = x0
            ptr = x1
            require(count == 2, "Stock PHY did not use two optional clock slots")
            action = name.removeprefix("clk_bulk_")
            for slot in range(count):
                item = ptr + slot * 16
                id_ptr, clock_ptr = struct.unpack("<QQ", uc.mem_read(item, 16))
                clock_name = _read_cstr(uc, id_ptr)
                require(clock_name == ("ref" if slot == 0 else "da_ref"),
                        "Unexpected stock clock ID")
                if clock_ptr:
                    machine["clock_trace"].append((action, slot))
                    machine["events"].append(("clock", action, slot))
            ret(0)
            return
        if name == "__const_udelay":
            call = (uc.reg_read(UC_ARM64_REG_LR) - text_base - 4) & 0xFFFFFFFF
            expected = STOCK_DELAY_CALLS.get(call)
            require(expected is not None, "Unexpected stock delay callsite: " + hex(call))
            require(x0 == expected[0], "Stock delay argument changed at " + hex(call))
            machine["delay_trace"].append(expected[1])
            machine["events"].append(("delay", expected[1]))
            machine["time_ns"] += expected[1] * 1000
            ret(0)
            return
        if name == "ktime_get":
            ret(machine["time_ns"])
            return
        if name == "usleep_range_state":
            require((x0, x1, x2) == (3, 10, 2),
                    "Unexpected stock calibration sleep request")
            machine["delay_trace"].append(x0)
            machine["events"].append(("delay", x0))
            machine["time_ns"] += x0 * 1000
            ret(0)
            return
        raise RuntimeError("Unhandled stock external: " + name)

    def code_hook(engine, address, size, _user):
        if address == stop:
            engine.emu_stop()
            return
        name = stub_names.get(address)
        if name is not None:
            # handle_external rejects every unknown executed symbol.
            handle_external(name, address)
            return
        in_selected = any(machine["bases"][section] + begin <= address <
                          machine["bases"][section] + end
                          for section, begin, end, _name in ranges
                          if section == machine["text_index"])
        require(in_selected, "Stock execution escaped selected functions: " + hex(address))
        word = struct.unpack("<I", engine.mem_read(address, 4))[0]
        if word == 0xD5384108:  # mrs x8, SP_EL0; kernel TLS fixture only
            engine.reg_write(UC_ARM64_REG_X8, machine["tls"])
            engine.mem_write(machine["tls"] + 0x710, struct.pack("<Q", machine["canary"]))
            engine.reg_write(UC_ARM64_REG_PC, address + 4)

    def memory_hook(engine, access, address, size, value, _user):
        if not (mmio_lo <= address < mmio_hi):
            return
        require(size == 4 and address % 4 == 0,
                "Unexpected stock MMIO access width/alignment")
        offset = address - mmio_lo
        if access == UC_MEM_READ:
            read_value = struct.unpack("<I", engine.mem_read(address, 4))[0]
            machine["mmio_trace"].append(("r", offset, read_value))
            machine["events"].append(("r", offset, read_value))
        elif access == UC_MEM_WRITE:
            machine["mmio_trace"].append(("w", offset, value & 0xFFFFFFFF))
            machine["events"].append(("w", offset, value & 0xFFFFFFFF))

    uc.hook_add(UC_HOOK_CODE, code_hook)
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, memory_hook)
    # Initial register values are fake. No physical MMIO address is mapped.
    return machine


def stock_setup(machine, dt):
    provider, args, phy = machine["provider"], machine["args"], machine["phy"]
    ret = invoke_stock(machine, "mtk_phy_xlate", provider, args)
    require(ret == phy, "Stock mtk_phy_xlate did not return the expected fixture PHY")
    return ret


def stock_phase(machine, function, *args):
    machine["mmio_trace"].clear()
    machine["clock_trace"].clear()
    machine["delay_trace"].clear()
    machine["events"].clear()
    result = invoke_stock(machine, function, *args)
    return {
        "status": result,
        "mmio": list(machine["mmio_trace"]),
        "clock": list(machine["clock_trace"]),
        "delays": list(machine["delay_trace"]),
        "events": list(machine["events"]),
    }


def trace_hash(trace):
    return sha256(json.dumps(trace, separators=(",", ":")).encode("ascii"))


def first_mismatch(left, right):
    limit = min(len(left), len(right))
    for index in range(limit):
        if left[index] != right[index]:
            return index
    return limit


def compare_exact(label, stock, port):
    if stock != port:
        mismatch = first_mismatch(stock, port)
        left = max(0, mismatch - 3)
        right = mismatch + 4
        raise AssertionError(
            label + " trace mismatch at " + str(mismatch) +
            "; stock=" + repr(stock[left:right]) +
            "; port=" + repr(port[left:right]) +
            "; lengths=" + repr((len(stock), len(port)))
        )


MODE_STEPS = (
    ("mode_host", 1), ("mode_device", 6), ("mode_otg", 11),
    ("mode_host_again", 1), ("mode_device_again", 6), ("mode_otg_again", 11),
)
NORMAL_PHASES = ("init", "power_on") + tuple(name for name, _ in MODE_STEPS) + ("power_off", "exit")


class HostTuning(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in (
        "SlewControl", "EyeVrt", "EyeTerm", "Rev6", "DisconnectThreshold",
    )]


class NativeBus:
    def __init__(self, valid=True):
        self.regs = {0x110: 1 if valid else 0, 0x10C: 128}
        self.mmio = []
        self.clock = []
        self.delays = []
        self.events = []
        self.time_ns = 0
        self.nvmem_calls = 0
        self.callback_errors = []


def build_native_library():
    temp = tempfile.TemporaryDirectory(prefix="usb-phy-binary-")
    output = Path(temp.name) / "libmtk_usb_phy_core.so"
    command = [
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-shared", "-fPIC", "-I" + str(CORE_INCLUDE), str(CORE_C),
        "-x", "c", "-", "-o", str(output),
    ]
    layout_source = """#include <stddef.h>
#include <Library/MtkUsbPhyCore.h>
size_t MtkUsbPhyTestAbi(unsigned int slot) {
  const size_t sizes[] = {
    sizeof(MTK_USB_PHY_IO), sizeof(MTK_USB_PHY_CONFIG), sizeof(MTK_USB_PHY_CORE),
    offsetof(MTK_USB_PHY_IO, ReadNvmemCell),
    offsetof(MTK_USB_PHY_CONFIG, SetEfuseV1),
    offsetof(MTK_USB_PHY_CORE, RegistersTouched),
    sizeof(MTK_USB_PHY_HOST_TUNING),
    offsetof(MTK_USB_PHY_HOST_TUNING, DisconnectThreshold)
  };
  return slot < sizeof(sizes) / sizeof(sizes[0]) ? sizes[slot] : 0;
}
"""
    try:
        subprocess.run(command, check=True, input=layout_source, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        library = C.CDLL(str(output))
        library.MtkUsbPhyTestAbi.argtypes = [C.c_uint]
        library.MtkUsbPhyTestAbi.restype = C.c_size_t
        *_, Io, Config, Core = native_structures()
        layout = [C.sizeof(Io), C.sizeof(Config), C.sizeof(Core),
                  Io.ReadNvmemCell.offset, Config.SetEfuseV1.offset, Core.RegistersTouched.offset,
                  C.sizeof(HostTuning), HostTuning.DisconnectThreshold.offset]
        require(all(library.MtkUsbPhyTestAbi(index) == value for index, value in enumerate(layout)),
                "ctypes ABI disagrees with the production C header")
        return temp, library
    except Exception:
        temp.cleanup()
        raise


def native_structures():
    Read32 = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32))
    Write32 = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32, C.c_uint32)
    NowNs = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(C.c_uint64))
    DelayUs = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint32)
    Resolve = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_int, C.POINTER(C.c_uint8))
    ClockAction = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_int)
    ReadNvmem = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_char_p,
                            C.c_uint32, C.POINTER(C.c_uint32))

    class Io(C.Structure):
        _fields_ = [
            ("Context", C.c_void_p), ("PhyBase", C.c_uint64), ("PhyLength", C.c_uint64),
            ("Read32", Read32), ("Write32", Write32), ("NowNs", NowNs),
            ("DelayUs", DelayUs), ("ResolveClock", Resolve),
            ("ClockPrepare", ClockAction), ("ClockEnable", ClockAction),
            ("ClockDisable", ClockAction), ("ClockUnprepare", ClockAction),
            ("ReadNvmemCell", ReadNvmem),
        ]

    class Config(C.Structure):
        _fields_ = [
            ("Revision", C.c_uint32), ("PhyType", C.c_uint32),
            ("TphyVersion", C.c_uint32), ("TuningPresent", C.c_uint32),
            ("EyeVrt", C.c_uint32), ("EyeTerm", C.c_uint32),
            ("DiscTh", C.c_uint32), ("RxSqth", C.c_uint32),
            ("Rev4", C.c_uint32), ("SetEfuseV1", C.c_uint8),
            ("SlewReferencePresent", C.c_uint8),
            ("SlewCoefficientPresent", C.c_uint8), ("Reserved", C.c_uint8),
            ("SlewReferenceMHz", C.c_uint32), ("SlewCoefficient", C.c_uint32),
        ]

    class Core(C.Structure):
        _fields_ = [
            ("Io", Io), ("Config", Config), ("Signature", C.c_uint32),
            ("State", C.c_uint8), ("ClockPresentMask", C.c_uint8),
            ("ClockPreparedMask", C.c_uint8), ("ClockEnabledMask", C.c_uint8),
            ("PowerCleanupPending", C.c_uint8), ("CalibrationActive", C.c_uint8),
            ("RegistersTouched", C.c_uint8),
        ]

    return Read32, Write32, NowNs, DelayUs, Resolve, ClockAction, ReadNvmem, Io, Config, Core


def run_native(corelib, dt, valid=True, initial=None, host_tuning=None):
    Read32, Write32, NowNs, DelayUs, Resolve, ClockAction, ReadNvmem, Io, Config, Core = native_structures()
    bus = NativeBus(valid)
    if initial is not None:
        bus.regs.update(initial)
    callbacks = []

    @Read32
    def read32(_context, offset, value):
        try:
            result = bus.regs.get(int(offset), 0)
            bus.mmio.append(("r", int(offset), result))
            bus.events.append(("r", int(offset), result))
            value[0] = result
            return 0
        except Exception as exc:
            bus.callback_errors.append(str(exc))
            return 6

    @Write32
    def write32(_context, offset, value):
        try:
            offset, value = int(offset), int(value) & 0xFFFFFFFF
            bus.mmio.append(("w", offset, value))
            bus.events.append(("w", offset, value))
            bus.regs[offset] = value
            return 0
        except Exception as exc:
            bus.callback_errors.append(str(exc))
            return 6

    @NowNs
    def now_ns(_context, value):
        try:
            value[0] = bus.time_ns
            return 0
        except Exception as exc:
            bus.callback_errors.append(str(exc))
            return 9

    @DelayUs
    def delay_us(_context, microseconds):
        try:
            value = int(microseconds)
            bus.delays.append(value)
            bus.events.append(("delay", value))
            bus.time_ns += value * 1000
            return 0
        except Exception as exc:
            bus.callback_errors.append(str(exc))
            return 10

    @Resolve
    def resolve(_context, slot, present):
        if int(slot) not in (0, 1):
            return 1
        present[0] = 1 if int(slot) == 0 else 0
        return 0

    def clock_callback(action):
        @ClockAction
        def callback(_context, slot):
            try:
                slot = int(slot)
                if slot not in (0, 1):
                    return 1
                bus.clock.append((action, slot))
                bus.events.append(("clock", action, slot))
                return 0
            except Exception as exc:
                bus.callback_errors.append(str(exc))
                return 7
        return callback

    prepare = clock_callback("prepare")
    enable = clock_callback("enable")
    disable = clock_callback("disable")
    unprepare = clock_callback("unprepare")

    @ReadNvmem
    def read_nvmem(_context, name, mask, raw):
        try:
            if name != b"intr_cal" or int(mask) != 0x1F:
                bus.callback_errors.append("unexpected C NVMEM request")
                return 8
            bus.nvmem_calls += 1
            raw[0] = SYNTHETIC_NVMEM
            return 0
        except Exception as exc:
            bus.callback_errors.append(str(exc))
            return 8

    callbacks.extend((read32, write32, now_ns, delay_us, resolve,
                      prepare, enable, disable, unprepare, read_nvmem))
    io = Io(None, 0x11F40000, 0x700, read32, write32, now_ns,
            delay_us, resolve, prepare, enable, disable, unprepare, read_nvmem)
    config = Config(
        1, 3, 2, 0x1F,
        dt["u2_tuning"]["mediatek,eye-vrt"],
        dt["u2_tuning"]["mediatek,eye-term"],
        dt["u2_tuning"]["mediatek,discth"],
        dt["u2_tuning"]["mediatek,rx-sqth"],
        dt["u2_tuning"]["mediatek,rev4"],
        1, 0, 0, 0, 0, 0,
    )
    core = Core()
    corelib.MtkUsbPhyCoreInitialize.argtypes = [
        C.POINTER(Core), C.POINTER(Io), C.POINTER(Config)
    ]
    corelib.MtkUsbPhyCoreInitialize.restype = C.c_int
    corelib.MtkUsbPhyCorePowerOn.argtypes = [C.POINTER(Core)]
    corelib.MtkUsbPhyCorePowerOn.restype = C.c_int
    corelib.MtkUsbPhyCorePowerOff.argtypes = [C.POINTER(Core)]
    corelib.MtkUsbPhyCorePowerOff.restype = C.c_int
    corelib.MtkUsbPhyCoreExit.argtypes = [C.POINTER(Core)]
    corelib.MtkUsbPhyCoreExit.restype = C.c_int

    corelib.MtkUsbPhyCoreSetUsbMode.argtypes = [
        C.POINTER(Core), C.c_int, C.POINTER(HostTuning),
    ]
    corelib.MtkUsbPhyCoreSetUsbMode.restype = C.c_int
    host = HostTuning(*(host_tuning if host_tuning is not None else
                        (0, 0, 0, 0, dt["u2_tuning"]["mediatek,disc-host"])))

    phases = {}
    init_status = corelib.MtkUsbPhyCoreInitialize(C.byref(core), C.byref(io), C.byref(config))
    require(not bus.callback_errors, "Native callback error: " + repr(bus.callback_errors))
    phases["init"] = {
        "status": int(init_status), "mmio": list(bus.mmio),
        "clock": list(bus.clock), "delays": list(bus.delays),
        "events": list(bus.events),
    }
    bus.mmio.clear()
    bus.clock.clear()
    bus.delays.clear()
    bus.events.clear()

    on_status = corelib.MtkUsbPhyCorePowerOn(C.byref(core))
    require(not bus.callback_errors, "Native callback error: " + repr(bus.callback_errors))
    phases["power_on"] = {
        "status": int(on_status), "mmio": list(bus.mmio),
        "clock": list(bus.clock), "delays": list(bus.delays),
        "events": list(bus.events),
    }
    bus.mmio.clear()
    bus.clock.clear()
    bus.delays.clear()
    bus.events.clear()

    if valid:
        for name, mode in MODE_STEPS:
            result = corelib.MtkUsbPhyCoreSetUsbMode(
                C.byref(core), mode, C.byref(host) if mode == 1 else None)
            require(not bus.callback_errors, "Native callback error: " + repr(bus.callback_errors))
            phases[name] = {
                "status": int(result), "mmio": list(bus.mmio),
                "clock": list(bus.clock), "delays": list(bus.delays),
                "events": list(bus.events),
            }
            bus.mmio.clear()
            bus.clock.clear()
            bus.delays.clear()
            bus.events.clear()

    off_status = corelib.MtkUsbPhyCorePowerOff(C.byref(core))
    require(not bus.callback_errors, "Native callback error: " + repr(bus.callback_errors))
    phases["power_off"] = {
        "status": int(off_status), "mmio": list(bus.mmio),
        "clock": list(bus.clock), "delays": list(bus.delays),
        "events": list(bus.events),
    }
    bus.mmio.clear()
    bus.clock.clear()
    bus.delays.clear()
    bus.events.clear()

    exit_status = corelib.MtkUsbPhyCoreExit(C.byref(core))
    require(not bus.callback_errors, "Native callback error: " + repr(bus.callback_errors))
    phases["exit"] = {
        "status": int(exit_status), "mmio": list(bus.mmio),
        "clock": list(bus.clock), "delays": list(bus.delays),
        "events": list(bus.events),
    }
    phases["nvmem_reads"] = bus.nvmem_calls
    phases["callbacks"] = callbacks
    return phases


def new_stock_machine(raw, parsed, valid=True, initial=None):
    sections, section_names, contents, symbol_tables, symbols, symbols_by_name = parse_elf(raw)
    machine = load_stock_elf(raw, sections, section_names, contents,
                             symbol_tables, symbols, symbols_by_name)
    if initial is not None:
        for offset, value in initial.items():
            require(0 <= offset < 0x700 and offset % 4 == 0 and 0 <= value <= 0xffffffff,
                    "Invalid synthetic PHY register fixture")
            machine["uc"].mem_write(machine["mmio"] + offset, struct.pack("<I", value))
    if not valid:
        machine["uc"].mem_write(machine["mmio"] + 0x110, struct.pack("<I", 0))
    make_stock_runner(machine, parsed)
    stock_setup(machine, parsed)
    return machine


def run_stock(corelib, raw, dt, valid=True, initial=None, host_tuning=None):
    machine = new_stock_machine(raw, dt, valid, initial=initial)
    if host_tuning is not None:
        # Synthetic host-property edge vectors, separate from the exact T70
        # xlate baseline. Layout comes from the stock set_mode LDR offsets.
        machine["uc"].mem_write(machine["child"] + 0xb8, struct.pack("<5I", *host_tuning))
    phases = {}
    phases["init"] = stock_phase(machine, "mtk_phy_init", machine["phy"])
    phases["power_on"] = stock_phase(machine, "mtk_phy_power_on", machine["phy"])
    if valid:
        for key, mode in MODE_STEPS:
            phases[key] = stock_phase(machine, "mtk_phy_set_mode", machine["phy"], mode, 0)
    phases["power_off"] = stock_phase(machine, "mtk_phy_power_off", machine["phy"])
    phases["exit"] = stock_phase(machine, "mtk_phy_exit", machine["phy"])
    phases["nvmem_reads"] = machine["nvmem_calls"]
    return phases, machine


def collapse_frequency_poll_reads(events):
    result = []
    for event in events:
        if (event[0] == "r" and event[1] == 0x110 and
                result and event == result[-1]):
            continue
        result.append(event)
    return result


def clean_status(phase):
    return {
        "status": phase["status"],
        "mmio": phase["mmio"],
        "clock": phase["clock"],
        "delays": phase["delays"],
    }


def main():
    dt = verify_t70_dtb()
    raw = BINARY.read_bytes()
    require(sha256(raw) == EXPECTED_BINARY_SHA256,
            "Stock TPHY module identity mismatch")
    sections, section_names, contents, symbol_tables, symbols, symbols_by_name = parse_elf(raw)

    temp, corelib = build_native_library()
    try:
        stock, stock_machine = run_stock(corelib, raw, dt, valid=True)
        port = run_native(corelib, dt, valid=True)
        required_success = dict.fromkeys(NORMAL_PHASES, 0)
        comparisons = {}
        for phase in NORMAL_PHASES:
            require(stock[phase]["status"] == required_success[phase],
                    "Stock phase failed: " + phase)
            require(port[phase]["status"] == required_success[phase],
                    "Production C phase failed: " + phase)
            compare_exact(phase + " MMIO", stock[phase]["mmio"], port[phase]["mmio"])
            compare_exact(phase + " delay", stock[phase]["delays"], port[phase]["delays"])
            compare_exact(phase + " interleaved events", stock[phase]["events"], port[phase]["events"])
            stock_clock = stock[phase]["clock"]
            port_clock = port[phase]["clock"]
            compare_exact(phase + " clock actions", stock_clock, port_clock)
            comparisons[phase] = {
                "status": "exact",
                "mmio_events": len(stock[phase]["mmio"]),
                "mmio_sha256": trace_hash(stock[phase]["mmio"]),
                "clock_actions": len(stock_clock),
                "delay_us": stock[phase]["delays"],
                "interleaved_event_count": len(stock[phase]["events"]),
                "interleaved_event_sha256": trace_hash(stock[phase]["events"]),
            }
        require(stock["nvmem_reads"] == 1 and port["nvmem_reads"] == 1,
                "Expected one synthetic NVMEM fixture read per path")

        # Bit preservation needs nonzero starting values; a zero-only fixture
        # would miss an incorrect clear mask. All values here are synthetic.
        rng = random.Random(0x6789)
        vectors = []
        for fill in (0, 0xffffffff, 0xaaaaaaaa, 0x55555555):
            vectors.append({offset: fill for offset in range(0, 0x700, 4)})
        for _ in range(12):
            vectors.append({offset: rng.getrandbits(32) for offset in range(0, 0x700, 4)})
        for frequency in (0, 1, 128, 1024, 0xffffffff):
            vectors.append({0x110: 1, 0x10c: frequency})
        for index, initial in enumerate(vectors):
            initial[0x110] = initial.get(0x110, 0) | 1
            initial.setdefault(0x10c, 128)
            host = ([0, 0, 0, 0, 0] if index == 0 else
                    [7, 7, 7, 3, 15] if index == 1 else
                    [rng.randrange(8), rng.randrange(8), rng.randrange(8),
                     rng.randrange(4), rng.randrange(16)])
            stock_vector, _ = run_stock(corelib, raw, dt, initial=initial, host_tuning=host)
            port_vector = run_native(corelib, dt, initial=initial, host_tuning=host)
            for phase in NORMAL_PHASES:
                require(stock_vector[phase]["status"] == 0 and port_vector[phase]["status"] == 0,
                        "Synthetic normal-path phase failed: " + str(index) + " " + phase)
                for field in ("mmio", "clock", "delays", "events"):
                    compare_exact("vector " + str(index) + " " + phase + " " + field,
                                  stock_vector[phase][field], port_vector[phase][field])

        timeout_comparisons = []
        for frequency in (0, 1, 128, 1024, 0xffffffff):
            initial = {0x10c: frequency}
            timeout_stock, _ = run_stock(corelib, raw, dt, valid=False, initial=initial)
            timeout_port = run_native(corelib, dt, valid=False, initial=initial)
            for phase in ("init", "power_on", "power_off", "exit"):
                require(timeout_stock[phase]["status"] == 0 and timeout_port[phase]["status"] == 0,
                        "Stock-compatible timeout phase failed: " + phase)
                compare_exact("timeout " + str(frequency) + " " + phase + " MMIO",
                              collapse_frequency_poll_reads(timeout_stock[phase]["mmio"]),
                              collapse_frequency_poll_reads(timeout_port[phase]["mmio"]))
                compare_exact("timeout " + str(frequency) + " " + phase + " clocks",
                              timeout_stock[phase]["clock"], timeout_port[phase]["clock"])
                compare_exact("timeout " + str(frequency) + " " + phase + " fixed delays",
                              [value for value in timeout_stock[phase]["delays"] if value != 3],
                              [value for value in timeout_port[phase]["delays"] if value != 3])
            require(0 < timeout_port["power_on"]["delays"].count(3) <= 128,
                    "Timeout polling lost its iteration bound")
            timeout_comparisons.append({
                "FMOUT": frequency, "stock_return_status": timeout_stock["power_on"]["status"],
                "port_status": timeout_port["power_on"]["status"],
                "stock_poll_delay_count": timeout_stock["power_on"]["delays"].count(3),
                "port_poll_delay_count": timeout_port["power_on"]["delays"].count(3),
                "MMIO_matches_after_collapsing_identical_poll_reads": True,
                "stock_power_on_mmio_events": len(timeout_stock["power_on"]["mmio"]),
                "port_power_on_mmio_events": len(timeout_port["power_on"]["mmio"]),
            })

        return {
            "status": "passed",
            "stock_binary_sha256": EXPECTED_BINARY_SHA256,
            "effective_dtb_sha256": dt["sha256"],
            "stock_functions": {
                name: {"text_offset": hex(symbols[name][4]), "size": hex(symbols[name][5])}
                for name in EXPECTED_FUNCTIONS
            },
            "elf_relocations_applied": stock_machine["relocations"],
            "binary_executed": [
                "mtk_phy_xlate", "mtk_phy_init", "mtk_phy_power_on",
                "mtk_phy_power_off", "mtk_phy_set_mode", "mtk_phy_exit",
            ],
            "comparisons": comparisons,
            "additional_exact_lifecycles": len(vectors),
            "normal_exact_lifecycles_total": 1 + len(vectors),
            "usb_mode_transitions_per_lifecycle": len(MODE_STEPS),
            "usb_mode_transition_total": (1 + len(vectors)) * len(MODE_STEPS),
            "mode_submode": 0,
            "synthetic_host_tuning_vectors": "zero/max plus 19 bounded deterministic random vectors",
            "type_c_role_or_vbus_proven": False,
            "bit_preservation_vectors": "zero/all-one/alternating plus 12 deterministic random synthetic register maps",
            "frequency_boundary_cases": [0, 1, 128, 1024, 0xffffffff],
            "synthetic_nvmem_fixture": {
                "used": True, "raw_value_reported": False,
                "cell": "intr_cal", "mask": "0x1f",
            },
            "timeout_fallback_comparisons": {
                "stock_dynamic_poll_ns": 200000, "port_max_polls": 128,
                "port_poll_delay_us": 3, "frequency_vectors": timeout_comparisons,
                "description": (
                    "Both paths continue from bounded polling to FMOUT-based slew control "
                    "or default4 for FMOUT0. MMIO comparisons collapse repeated identical "
                    "FMMONR1 reads; fixed delays and clock actions match. The port retains "
                    "its3-us poll delay and128-read bound if the timer is frozen."
                ),
            },
        }
    finally:
        temp.cleanup()


if __name__ == "__main__":
    report = main()
    report["scope"] = ("Original shipped ARM64 instructions in Unicorn versus production portable C; "
                       "synthetic MMIO/clocks/NVMEM/time only; no device execution")
    report["runner_sha256"] = sha256(Path(__file__).read_bytes())
    report["source_sha256"] = {
        str(path.relative_to(REPO)): sha256(path.read_bytes())
        for path in (Path(__file__), CORE_C, CORE_INCLUDE / "Library/MtkUsbPhyCore.h")
    }
    report["ctypes_abi_matches_c_header"] = True
    output = REPO.parent / "analysis/usb-phy-binary-tests.json"
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"status": report["status"],
                      "normal_exact_lifecycles_total": report["normal_exact_lifecycles_total"],
                      "comparisons": report["comparisons"],
                      "timeout_fallback_comparisons": report["timeout_fallback_comparisons"],
                      "report": str(output)}, indent=2))
