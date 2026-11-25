#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare bounded original MUSB blocks and extcon hub sequences with production C.

This executes only the TX/RX routing basic blocks, cold-host POWER and SESSION
blocks, completion-status W1C, the EP0 DATAEND early-return branch and
the disconnected-controller SWRST RMW block from the stock ARM64 module. MMIO
callbacks and imported delay/PHY calls are modeled; it does not emulate
enumeration, USB packets, interrupt delivery, a physical PHY, real GPIO, VBUS,
or a device. The extcon comparison replaces GPIO and delay calls with
recorders. No module or device is opened by the kernel.
"""
from pathlib import Path
import ctypes as C
import argparse
import hashlib
import itertools
import json
import random
import struct
import sys
import subprocess
import tempfile

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as R

PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
BINARY = REPO.parent / "analysis/stock-binaries/vendor_boot_a_0/musb_hdrc.ko"
CORE = REPO / "Silicon/MediaTek/MediaTekPkg/Library/MusbHostCoreLib/MusbHostCore.c"
HEADER = REPO / "Silicon/MediaTek/MediaTekPkg/Include/Library/MusbHostCore.h"
PLATFORM_CORE = CORE.with_name("MtkMusbPlatformCore.c")
PLATFORM_HEADER = HEADER.with_name("MtkMusbPlatformCore.h")
COLD_TEST = PACKAGE / "Tests/MtkMusbPlatformCoreTest.c"
COLD_CORE = PLATFORM_CORE
COLD_HEADER = PLATFORM_HEADER
NATIVE_SELECTED_REPORT = REPO.parent / "analysis/mediatek-driver-selected-tests.json"
COLD_SOURCE_PATHS = (COLD_TEST, COLD_CORE, COLD_HEADER)
EXPECTED_SHA = "e9499f7e62996b8afa78fd9c3e9c908161aa416a3d8d040520b55dabacfd0a40"
HUB_BINARY = BINARY.with_name("extcon-mtk-usb.ko")
HUB_CORE = CORE.with_name("MtkUsbHubCore.c")
HUB_HEADER = HEADER.with_name("MtkUsbHubCore.h")
HUB_SHA = "c69b1b3932e0008b847967745000e732bdf8b5dad59b484c1c0ef2a607201c99"
TEXT, DATA, MMIO = 0x100000, 0x300000, 0x500000
R_AARCH64_ADR_PREL_PG_HI21 = 275
R_AARCH64_LDST32_ABS_LO12_NC = 285
R_AARCH64_LDST64_ABS_LO12_NC = 286
R_AARCH64_CALL26 = 283
MUSB_START_POWER_BLOCK = (0x1014, 0x1048)
DO_HOST_SESSION_BLOCK = (0x1c6ac, 0x1c704)
MUSB_SWRST_BLOCK = (0x11f4, 0x1210)
EP0_DATAEND_TEST = 0x10a44
EP0_DATAEND_RETURN = 0x10cd0
EP0_IRQ_EPILOGUE = 0x11268


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stock_text():
    raw = BINARY.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == EXPECTED_SHA, "unexpected MUSB binary")
    require(raw[:6] == b"\x7fELF\x02\x01" and
            struct.unpack_from("<H", raw, 18)[0] == 183, "expected ARM64 LE ELF")
    offset = struct.unpack_from("<Q", raw, 40)[0]
    size, count = struct.unpack_from("<HH", raw, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, offset + i * size)
                for i in range(count)]
    symbols = {}
    for section in sections:
        if section[1] != 2:
            continue
        names_section = sections[section[6]]
        names = raw[names_section[4]:names_section[4] + names_section[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            name, info, _, index, value, length = struct.unpack_from("<IBBHQQ", raw, at)
            if (info & 15) == 2:
                label = names[name:names.index(b"\0", name)].decode()
                symbols.setdefault(label, []).append((index, value, length))
    expected = {"musb_start_urb": (0xba1c, 0x124c),
                "musb_readw": (0xb084, 0x1c0),
                "musb_writew": (0xccd0, 0x1b4),
                "mt_usb_interrupt": (0x8c90, 0x2c8),
                "dma_controller_irq": (0x1965c, 0x484)}
    for name, (value, length) in expected.items():
        require(any(entry[1:] == (value, length) for entry in symbols[name]),
                "unexpected function layout: " + name)
    section = sections[next(entry[0] for entry in symbols["musb_start_urb"]
                            if entry[1:] == expected["musb_start_urb"])]
    return raw[section[4]:section[4] + section[5]]


def stock_cold_source():
    raw = BINARY.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == EXPECTED_SHA,
            "unexpected MUSB binary for cold-host analysis")
    require(raw[:6] == b"\x7fELF\x02\x01" and
            struct.unpack_from("<H", raw, 18)[0] == 183,
            "expected ARM64 LE ELF for cold-host analysis")
    shoff = struct.unpack_from("<Q", raw, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", raw, 58)
    require(shentsize == 64 and shnum and shstrndx < shnum and
            shoff + shentsize * shnum <= len(raw),
            "invalid stock MUSB section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * shentsize)
                for i in range(shnum)]

    def contents(index):
        section = sections[index]
        require(section[1] != 8, "unexpected NOBITS section payload")
        begin, length = section[4], section[5]
        require(begin + length <= len(raw), "stock MUSB section exceeds module")
        return raw[begin:begin + length]

    shstr = contents(shstrndx)
    section_names = []
    for section in sections:
        name_offset = section[0]
        require(name_offset < len(shstr), "stock MUSB section name is out of range")
        end = shstr.find(b"\0", name_offset)
        require(end >= 0, "unterminated stock MUSB section name")
        section_names.append(shstr[name_offset:end].decode("ascii"))
    text_matches = [i for i, name in enumerate(section_names) if name == ".text"]
    require(len(text_matches) == 1, "missing/duplicate stock MUSB .text")
    text_index = text_matches[0]
    text = contents(text_index)

    symbol_tables = {}
    symbols = {}
    for index, section in enumerate(sections):
        if section[1] != 2:
            continue
        require(section[9] == 24 and section[5] % 24 == 0,
                "malformed stock MUSB symbol table")
        names = contents(section[6])
        entries = []
        for at in range(section[4], section[4] + section[5], 24):
            name_offset, info, _, section_index, value, size = struct.unpack_from(
                "<IBBHQQ", raw, at
            )
            require(name_offset < len(names), "stock MUSB symbol name is out of range")
            end = names.find(b"\0", name_offset)
            require(end >= 0, "unterminated stock MUSB symbol name")
            name = names[name_offset:end].decode("ascii")
            entry = (name, info, section_index, value, size)
            entries.append(entry)
            if name:
                symbols.setdefault(name, []).append(entry)
        symbol_tables[index] = entries

    def require_function(name, offset, size):
        matches = [entry for entry in symbols.get(name, [])
                   if entry[2] == text_index and entry[3:] == (offset, size)]
        require(len(matches) == 1,
                "stock MUSB function extent changed: " + name)

    for name, offset, size in (
        ("musb_start", 0x0d48, 0x3cc),
        ("musb_platform_reset", 0x1118, 0x10c),
        ("musb_g_ep0_irq", 0x109b0, 0xe24),
        ("do_host_work", 0x1c314, 0x654),
        ("mt_usb_get_vbus_status", 0x1b73c, 8),
        ("musb_writeb", 0x45c, 0x1b4),
        ("musb_readb", 0x984, 0x1c0),
        ("musb_readb", 0x1bbf8, 0x1bc),
        ("musb_writeb", 0x1ba50, 0x1a8),
    ):
        require_function(name, offset, size)

    relocations = {}
    for index, section in enumerate(sections):
        if section[1] != 4 or section[7] != text_index:
            continue
        require(section[9] == 24 and section[5] % 24 == 0,
                "malformed stock MUSB .rela.text")
        target_symbols = symbol_tables.get(section[6])
        require(target_symbols is not None,
                "stock MUSB .rela.text has no symbol table")
        for at in range(section[4], section[4] + section[5], 24):
            place, info, addend = struct.unpack_from("<QQq", raw, at)
            symbol_index = info >> 32
            require(symbol_index < len(target_symbols),
                    "stock MUSB relocation symbol index is out of range")
            entry = target_symbols[symbol_index]
            require(place not in relocations,
                    "duplicate stock MUSB text relocation")
            relocations[place] = (info & 0xffffffff, entry[0], addend)

    expected_relocations = {
        0x1014: (R_AARCH64_ADR_PREL_PG_HI21, "musb_speed", 0),
        0x101c: (R_AARCH64_LDST32_ABS_LO12_NC, "musb_speed", 0),
        0x1030: (R_AARCH64_LDST32_ABS_LO12_NC, "musb_uart_debug", 0),
        0x1c6ac: (R_AARCH64_CALL26, "msleep", 0),
        0x1c6b0: (R_AARCH64_LDST64_ABS_LO12_NC, "mtk_musb", 0),
        0x1c6bc: (R_AARCH64_LDST64_ABS_LO12_NC, "mtk_musb", 0),
        0x1c6e4: (R_AARCH64_LDST64_ABS_LO12_NC, "mtk_musb", 0),
        0x1c6f0: (R_AARCH64_LDST64_ABS_LO12_NC, "mtk_musb", 0),
        0x1c6d4: (R_AARCH64_CALL26, "set_usb_phy_mode", 0),
        0x1c6e0: (R_AARCH64_CALL26, "__const_udelay", 0),
        0x1c704: (R_AARCH64_LDST64_ABS_LO12_NC, "glue", 0),
        0x1c714: (R_AARCH64_CALL26, "phy_set_mode_ext", 0),
        0x1c71c: (R_AARCH64_CALL26, "set_usb_phy_mode", 0),
        0x1c720: (R_AARCH64_LDST64_ABS_LO12_NC, "mtk_musb", 0),
        0x1c724: (R_AARCH64_CALL26, "musb_start", 0),
    }
    for place, expected_relocation in expected_relocations.items():
        require(relocations.get(place) == expected_relocation,
                "stock MUSB cold-host relocation changed at " + hex(place))
    start_relocations = {
        place: relocation for place, relocation in relocations.items()
        if MUSB_START_POWER_BLOCK[0] <= place < MUSB_START_POWER_BLOCK[1]
    }
    require(start_relocations == {
        place: expected_relocations[place] for place in (0x1014, 0x101c, 0x1030)
    }, "unexpected relocation inside stock musb_start POWER block")
    session_relocations = {
        place: relocation for place, relocation in relocations.items()
        if DO_HOST_SESSION_BLOCK[0] <= place < DO_HOST_SESSION_BLOCK[1]
    }
    require(session_relocations == {
        place: expected_relocations[place] for place in (0x1c6ac, 0x1c6b0, 0x1c6bc, 0x1c6d4, 0x1c6e0, 0x1c6e4, 0x1c6f0)
    }, "unexpected relocation inside stock do_host_work SESSION block")

    def call_target(place):
        instruction = struct.unpack_from("<I", text, place)[0]
        require(instruction & 0xfc000000 == 0x94000000,
                "stock MUSB expected a direct BL at " + hex(place))
        immediate = instruction & 0x03ffffff
        if immediate & (1 << 25):
            immediate -= 1 << 26
        return place + (immediate << 2)

    for place, target in (
        (0x1044, 0x45c),
        (0x11fc, 0x7c4), (0x120c, 0x2a8),
        (0x1c6b8, 0x1bbf8), (0x1c6cc, 0x1ba50),
        (0x1c6ec, 0x1bbf8), (0x1c700, 0x1ba50),
    ):
        require(call_target(place) == target,
                "stock MUSB cold-host helper branch changed at " + hex(place))
    require(struct.unpack_from("<II", text, 0x1b73c) == (0x52800020, 0xd65f03c0),
            "stock VBUS status callback is no longer `mov w0,#1; ret`")
    require(struct.unpack_from("<I", text, 0x1ba8c)[0] == 0x910182a1 and
            struct.unpack_from("<I", text, 0x1bc30)[0] == 0x91018280,
            "stock byte helpers no longer address fixed DEVCTL offset 0x60")
    require(not any(MUSB_SWRST_BLOCK[0] <= place < MUSB_SWRST_BLOCK[1]
                    for place in relocations), "unexpected relocation in reset RMW block")
    require(struct.unpack_from("<I", text, EP0_DATAEND_TEST)[0] == 0x37181479,
            "stock peripheral DATAEND branch changed")
    return text, relocations


def original_device_reset(code, before):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, code)
    reg = lambda n: getattr(R, "UC_ARM64_REG_X" + str(n))
    events, writes = [], []
    stop = TEXT + MUSB_SWRST_BLOCK[1]

    def hook(machine, pc, size, _):
        offset = pc - TEXT
        if offset in (0x7c4, 0x2a8):
            require(machine.reg_read(reg(0)) == MMIO and
                    machine.reg_read(reg(1)) == 0x74,
                    "stock reset helper base/offset changed")
            if offset == 0x7c4:
                events.append(["read", 0x74, 16, before])
                machine.reg_write(reg(0), before)
            else:
                value = machine.reg_read(reg(2)) & 0xffff
                events.append(["write", 0x74, 16, value])
                writes.append([0x74, 16, value])
            machine.reg_write(R.UC_ARM64_REG_PC, machine.reg_read(R.UC_ARM64_REG_LR))
        else:
            require(MUSB_SWRST_BLOCK[0] <= offset < MUSB_SWRST_BLOCK[1],
                    "reset execution escaped bounded RMW block")

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(reg(19), MMIO)
    uc.emu_start(TEXT + MUSB_SWRST_BLOCK[0], stop, count=32)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == stop and
            writes == [[0x74, 16, before | 3]],
            "stock reset RMW did not complete with preserved bits")
    return {"initial_swrst": before, "events": events, "register_writes": writes}


def original_ep0_pending_status(code, csr0):
    # Start after the original read helper loaded CSR0 into w25. No MMIO,
    # endpoint-state machine, callbacks, or epilogue instructions are modeled.
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, code)
    stop = TEXT + (EP0_IRQ_EPILOGUE if csr0 & 8 else EP0_DATAEND_TEST + 4)

    def hook(machine, pc, size, _):
        if pc == stop:
            machine.emu_stop()
            return
        require(pc - TEXT in (EP0_DATAEND_TEST, EP0_DATAEND_RETURN, EP0_DATAEND_RETURN + 4),
                "EP0 DATAEND execution escaped the bounded branch")

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(R.UC_ARM64_REG_X25, csr0)
    uc.reg_write(R.UC_ARM64_REG_X0, 0xfeed)
    uc.emu_start(TEXT + EP0_DATAEND_TEST, TEXT + len(code), count=16)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == stop, "EP0 DATAEND branch did not stop")
    if csr0 & 8:
        require(uc.reg_read(R.UC_ARM64_REG_X0) == 1, "pending DATAEND no longer returns IRQ_HANDLED")
    return {"csr0": csr0, "pending_status_returns_handled": bool(csr0 & 8),
            "stop_text_offset": hex(stop - TEXT)}


def patch_adrp(code, place, target):
    instruction = struct.unpack_from("<I", code, place)[0]
    delta = (target >> 12) - ((TEXT + place) >> 12)
    require(-(1 << 20) <= delta < (1 << 20), "cold-host ADRP relocation is out of range")
    instruction = (instruction & ~((3 << 29) | (0x7ffff << 5))) | (
        ((delta & 3) << 29) | (((delta >> 2) & 0x7ffff) << 5)
    )
    struct.pack_into("<I", code, place, instruction)


def patch_ldst32(code, place, target):
    require(target & 3 == 0, "cold-host 32-bit relocation target is misaligned")
    instruction = struct.unpack_from("<I", code, place)[0]
    instruction = (instruction & ~(0xfff << 10)) | (((target & 0xfff) >> 2) << 10)
    struct.pack_into("<I", code, place, instruction)


def original_musb_start_power(code, speed, port_mode_bit14):
    patched = bytearray(code)
    speed_address, debug_address = DATA + 0x100, DATA + 0x104
    patch_adrp(patched, 0x1014, speed_address)
    patch_ldst32(patched, 0x101c, speed_address)
    patch_ldst32(patched, 0x1030, debug_address)

    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(patched) + 4095) & ~4095)
    uc.mem_write(TEXT, bytes(patched))
    uc.mem_map(DATA, 0x10000)
    uc.mem_map(MMIO, 0x1000)
    musb = DATA + 0x2000
    uc.mem_write(speed_address, struct.pack("<I", speed))
    uc.mem_write(debug_address, b"\0\0\0\0")
    uc.mem_write(musb + 0x1d40, struct.pack("<H", 0x4000 if port_mode_bit14 else 0))
    reg = lambda n: getattr(R, "UC_ARM64_REG_X" + str(n))
    writes = []
    stop = TEXT + MUSB_START_POWER_BLOCK[1]

    def hook(machine, pc, size, _):
        if pc == TEXT + 0x45c:
            require(machine.reg_read(reg(0)) == MMIO and machine.reg_read(reg(1)) == 1,
                    "stock musb_start POWER write helper arguments changed")
            value = machine.reg_read(reg(2)) & 0xff
            writes.append([1, 8, value])
            machine.reg_write(R.UC_ARM64_REG_PC, machine.reg_read(R.UC_ARM64_REG_LR))
        else:
            offset = pc - TEXT
            require(MUSB_START_POWER_BLOCK[0] <= offset < MUSB_START_POWER_BLOCK[1],
                    "stock musb_start POWER execution escaped bounded block")

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(reg(19), musb)
    uc.reg_write(reg(20), MMIO)
    # x22 was loaded with the UART-debug global page before this block.
    uc.reg_write(reg(22), DATA)
    uc.reg_write(R.UC_ARM64_REG_SP, DATA + 0xf000)
    uc.emu_start(TEXT + MUSB_START_POWER_BLOCK[0], stop, count=128)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == stop,
            "stock musb_start POWER block did not stop at boundary")
    value = (1 if speed == 0 else 0x21) | (0x40 if port_mode_bit14 else 0)
    require(writes == [[1, 8, value]], "stock musb_start POWER write sequence changed")
    return writes


def original_do_host_session(code, initial_devctl):
    patched = bytearray(code)
    musb_global = DATA + 0x100
    # Relocate all four original LDR x8,[x23,#:lo12:mtk_musb] operations.
    for place in (0x1c6b0, 0x1c6bc, 0x1c6e4, 0x1c6f0):
        instruction = struct.unpack_from("<I", patched, place)[0]
        instruction = (instruction & ~(0xfff << 10)) | (((musb_global & 0xfff) >> 3) << 10)
        struct.pack_into("<I", patched, place, instruction)
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, bytes(patched))
    uc.mem_map(DATA, 0x10000)
    uc.mem_map(MMIO, 0x1000)
    reg = lambda n: getattr(R, "UC_ARM64_REG_X" + str(n))
    musb_global = DATA + 0x100
    musb = DATA + 0x2000
    uc.mem_write(musb_global, struct.pack("<Q", musb))
    uc.mem_write(musb + 0x368, struct.pack("<Q", MMIO))
    registers = {0x60: initial_devctl & 0xff}
    events = []
    writes = []
    import_sites = {
        0x1c6ac: ("msleep", 100),
        0x1c6d4: ("set_usb_phy_mode", 0),
        0x1c6e0: ("__const_udelay", 0x0147aeb8),
    }
    stop = TEXT + DO_HOST_SESSION_BLOCK[1]

    def hook(machine, pc, size, _):
        offset = pc - TEXT
        if pc == stop:
            machine.emu_stop()
            return
        if offset in import_sites:
            name, expected_arg = import_sites[offset]
            require(machine.reg_read(reg(0)) == expected_arg,
                    "stock do_host_work modeled call argument changed: " + name)
            events.append([name, expected_arg])
            machine.reg_write(R.UC_ARM64_REG_PC, pc + 4)
        elif offset == 0x1bbf8:
            require(machine.reg_read(reg(0)) == MMIO,
                    "stock DEVCTL read helper base changed")
            value = registers[0x60]
            events.append(["read", 0x60, 8, value])
            machine.reg_write(reg(0), value)
            machine.reg_write(R.UC_ARM64_REG_PC, machine.reg_read(R.UC_ARM64_REG_LR))
        elif offset == 0x1ba50:
            require(machine.reg_read(reg(0)) == MMIO,
                    "stock DEVCTL write helper base changed")
            value = machine.reg_read(reg(1)) & 0xff
            events.append(["write", 0x60, 8, value])
            writes.append([0x60, 8, value])
            registers[0x60] = value
            machine.reg_write(R.UC_ARM64_REG_PC, machine.reg_read(R.UC_ARM64_REG_LR))
        else:
            require(DO_HOST_SESSION_BLOCK[0] <= offset < DO_HOST_SESSION_BLOCK[1],
                    "stock do_host_work SESSION execution escaped bounded block")

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(reg(23), DATA)
    require(struct.unpack_from("<I", code, 0x1c6a8)[0] == 0x52800c80,
            "stock msleep input no longer comes from mov w0,#100")
    uc.reg_write(reg(0), 100)
    uc.reg_write(R.UC_ARM64_REG_SP, DATA + 0xf000)
    uc.emu_start(TEXT + DO_HOST_SESSION_BLOCK[0], stop, count=128)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == stop,
            "stock do_host_work SESSION block did not stop at boundary")
    cleared = initial_devctl & ~1
    expected_writes = [[0x60, 8, cleared], [0x60, 8, cleared | 1]]
    require(writes == expected_writes, "stock do_host_work SESSION write sequence changed")
    expected_events = [
        ["msleep", 100], ["read", 0x60, 8, initial_devctl & 0xff],
        ["write", 0x60, 8, cleared], ["set_usb_phy_mode", 0],
        ["__const_udelay", 0x0147aeb8], ["read", 0x60, 8, cleared],
        ["write", 0x60, 8, cleared | 1],
    ]
    require(events == expected_events, "stock do_host_work SESSION event order changed")
    return {"initial_devctl": initial_devctl & 0xff, "events": events,
            "register_writes": writes}


def original_do_host_start_tail(code):
    """Execute original HOST PHY argument/call order after SESSION-set.
    Imported calls are argument-checked recorders. Pre-block x21/x23 point at
    synthetic glue/mtk_musb globals; musb_start's POWER block runs separately.
    """
    begin, end = 0x1c704, 0x1c728
    expected_words = (0xf94002a8, 0x52800021, 0x2a1f03e2, 0xf9402900,
                      0x94000000, 0x52800020, 0x94000000,
                      0xf94002e0, 0x94000000)
    require(struct.unpack_from("<9I", code, begin) == expected_words,
            "stock HOST PHY/start tail instructions changed")
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, code)
    uc.mem_map(DATA, 0x10000)
    glue_global, musb_global = DATA + 0x100, DATA + 0x200
    glue, phy, musb = DATA + 0x1000, DATA + 0x2000, DATA + 0x3000
    uc.mem_write(glue_global, struct.pack("<Q", glue))
    uc.mem_write(glue + 0x50, struct.pack("<Q", phy))
    uc.mem_write(musb_global, struct.pack("<Q", musb))
    uc.reg_write(R.UC_ARM64_REG_X21, glue_global)
    uc.reg_write(R.UC_ARM64_REG_X23, musb_global)
    events = []

    def hook(engine, pc, size, _):
        offset = pc - TEXT
        require(begin <= offset < end, "stock HOST start tail escaped its block")
        if offset == 0x1c714:
            require(engine.reg_read(R.UC_ARM64_REG_X0) == phy and
                    engine.reg_read(R.UC_ARM64_REG_X1) == 1 and
                    engine.reg_read(R.UC_ARM64_REG_X2) == 0,
                    "stock phy_set_mode_ext HOST arguments changed")
            events.append(["phy_set_mode_ext", 1, 0])
            engine.reg_write(R.UC_ARM64_REG_X0, 0)
        elif offset == 0x1c71c:
            require(engine.reg_read(R.UC_ARM64_REG_X0) == 1,
                    "stock set_usb_phy_mode HOST argument changed")
            events.append(["set_usb_phy_mode", 1])
        elif offset == 0x1c724:
            require(engine.reg_read(R.UC_ARM64_REG_X0) == musb,
                    "stock musb_start context changed")
            events.append(["musb_start"])
        else:
            return
        engine.reg_write(R.UC_ARM64_REG_PC, pc + 4)

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(TEXT + begin, TEXT + end, count=32)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == TEXT + end,
            "stock HOST start tail did not reach its end")
    require(events == [["phy_set_mode_ext", 1, 0],
                       ["set_usb_phy_mode", 1], ["musb_start"]],
            "stock HOST PHY/start call order changed")
    return {"block_text_offsets": [hex(begin), hex(end)], "events": events,
            "hostmode_readback_before_musb_start": False,
            "physical_phy_behavior_validated": False,
            "limitations": ["Imported HOST PHY and musb_start calls are recorders.",
                            "The surrounding role worker, resets and interrupts do not execute."]}

def compare_cold_c_api(stock_sequences):
    report_name = str(NATIVE_SELECTED_REPORT.relative_to(REPO.parent))
    if not NATIVE_SELECTED_REPORT.is_file():
        return {"status": "not_run", "reason": "native selected-test report is missing",
                "native_report": report_name}
    raw = NATIVE_SELECTED_REPORT.read_bytes()
    native = json.loads(raw)
    require(isinstance(native, dict), "native selected-test report is not an object")
    metadata = {
        "native_report": report_name,
        "native_report_sha256": hashlib.sha256(raw).hexdigest(),
        "native_runner_sha256": native.get("runner_sha256"),
    }
    results = native.get("results")
    require(isinstance(results, list), "native selected-test results are not an array")
    matches = [item for item in results
               if isinstance(item, dict) and item.get("test") == "MtkMusbPlatformCoreTest"]
    if not matches:
        return {**metadata, "status": "not_run",
                "reason": "MtkMusbPlatformCoreTest result is absent"}
    require(len(matches) == 1, "native report has duplicate MtkMusbPlatformCoreTest results")
    test_result = matches[0]
    c_trace = test_result.get("cold_host_trace")
    if c_trace is None:
        return {**metadata, "status": "not_run",
                "reason": "C cold-host trace is absent"}
    require(str(test_result.get("status", "")).lower() in ("pass", "passed"),
            "native MtkMusbPlatformCoreTest did not report pass")

    reported_sources = native.get("source_sha256")
    require(isinstance(reported_sources, dict),
            "native selected-test report has no source SHA map")
    source_hashes = {}
    for path in COLD_SOURCE_PATHS:
        relative = str(path.relative_to(REPO))
        require(relative in reported_sources,
                "native report is missing cold-host C source identity: " + relative)
        current_sha = digest(path)
        require(reported_sources[relative] == current_sha,
                "native C source changed since its recorded run: " + relative)
        source_hashes[relative] = current_sha
    require(isinstance(metadata["native_runner_sha256"], str),
            "native runner SHA is missing")

    def write_triples(value, name):
        require(isinstance(value, list), "C cold-host trace is missing " + name)
        for item in value:
            require(isinstance(item, list) and len(item) == 3 and
                    all(type(part) is int for part in item),
                    "C cold-host register writes must be integer [offset,width,value] triples")
        return value

    require(isinstance(c_trace, dict), "C cold-host trace is not an object")
    c_session = write_triples(c_trace.get("session_set_register_writes"),
                              "session_set_register_writes")
    c_power = write_triples(c_trace.get("power_register_writes"),
                            "power_register_writes")
    stock_session = stock_sequences["do_host_work_session"]["common_c_api_register_writes"]
    stock_power = stock_sequences["musb_start_power"]["high_speed_softconn0_register_writes"]
    require(c_session == stock_session,
            "stock/C common SESSION-set register write differs")
    require(c_power == stock_power,
            "stock/C high-speed SOFTCONN=0 POWER write differs")
    trial_cases = c_trace.get("trial99_cases")
    require(isinstance(trial_cases, list) and len(trial_cases) == 2,
            "C trial99 modeled POWER transition cases are absent")
    for path, case in enumerate(trial_cases, 1):
        require(case.get("host_power_path") == path and
                case.get("initial_devctl") == 0x98 and
                case.get("initial_power") == 0x21 and
                case.get("post_phy_devctl") == 0x99 and
                case.get("post_phy_power") == 0x63 and
                case.get("host_phy_after_register_writes") == 2 and
                case.get("readiness_model") == "withheld_until_POWER_initialization",
                "C trial99 fixture or HOST PHY order changed")
        writes = write_triples(case.get("register_writes"), "trial99 register_writes")
        require(writes == [[0x60, 8, 0x98], [0x60, 8, 0x99]] + stock_power,
                "C trial99 POWER initialization differs from stock musb_start")
    return {
        **metadata,
        "status": "passed",
        "source_sha256": source_hashes,
        "trial99_modeled_cases": trial_cases,
        "stock_host_phy_start_order": stock_sequences["host_phy_start_tail"],
        "compared_register_writes": {
            "session_set": {"stock": stock_session, "c_api": c_session,
                             "match": True},
            "high_speed_softconn0_power": {"stock": stock_power, "c_api": c_power,
                                            "match": True},
        },
        "limitations": [
            "The canonical final SESSION-set write and modeled trial99 SESSION-clear/set writes are compared. Full PHY, delay and reset order is checked by the separate startup verifier.",
            "The original POWER block always writes computed 0x21 for high-speed/SOFTCONN=0. The modeled trial99 C cases write that same byte after HOST PHY selection, including when enable bits already match. C preserves unrelated ISOUPDATE/HSMODE bits, so arbitrary prior-byte equivalence is not claimed.",
            "No physical VBUS, PHY mode, or delay behavior is validated by the C trace comparison.",
        ],
    }


def original(code, receive, endpoint, address, speed, hub, port, multi, old):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, code)
    uc.mem_map(DATA, 0x10000)
    reg = lambda n: getattr(R, "UC_ARM64_REG_X" + str(n))
    def put(at, value):
        uc.mem_write(at, struct.pack("<Q", value))
    context, qh, ep, frame = DATA, DATA + 0x1000, DATA + 0x2000, DATA + 0x4000
    put(context + 0x368, MMIO)
    put(frame - 0x10, MMIO)
    uc.mem_write(ep + 0x18, bytes([endpoint]))
    uc.mem_write(qh + 0x36, bytes([address, hub | (0x80 if multi else 0), port]))
    device = DATA + 0x5000
    uc.mem_write(device, struct.pack("<I", 2 if speed == 0 else 1))
    for number, value in {0: (MMIO if receive else (0x80 if speed == 0 else 0xc0)),
                          9: device, 19: MMIO, 20: 0x10, 21: context, 22: address,
                          23: ep, 24: 0, 25: qh, 26: endpoint, 29: frame}.items():
        uc.reg_write(reg(number), value)
    uc.reg_write(R.UC_ARM64_REG_SP, frame + 0x1000)
    start, stop = ((0xc044, 0xc0c4) if receive else (0xbf8c, 0xc540))
    hub_offset = (0x486 if receive else 0x482) + endpoint * 8
    registers, trace = {hub_offset: old}, []

    def hook(machine, pc, size, _):
        offset = pc - TEXT
        if offset in (0xb084, 0xccd0):
            require(machine.reg_read(reg(0)) == MMIO, "stock MMIO base")
            at = machine.reg_read(reg(1))
            require(0x480 <= at < 0x500 and at % 2 == 0, "stock register offset")
            if offset == 0xb084:
                value = registers.get(at, 0)
                trace.append((1, at, value))
                machine.reg_write(reg(0), value)
            else:
                value = machine.reg_read(reg(2)) & 0xffff
                trace.append((2, at, value))
                registers[at] = value
            machine.reg_write(R.UC_ARM64_REG_PC, machine.reg_read(R.UC_ARM64_REG_LR))
        else:
            require(start <= offset < stop, "unexpected stock branch")
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(TEXT + start, TEXT + stop, count=1000)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == TEXT + stop, "stock block did not complete")
    return trace


def original_ack(code, tx, rx, dma):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, code)
    uc.mem_map(DATA, 0x10000)
    reg = lambda n: getattr(R, "UC_ARM64_REG_X" + str(n))
    uc.mem_write(DATA+0x368, struct.pack("<Q", MMIO))
    uc.mem_write(DATA+0x372, struct.pack("<HH", rx, tx))
    uc.reg_write(reg(20), DATA)
    trace = []

    def hook(machine, pc, size, _):
        at = pc-TEXT
        if at in (0x2a8, 0x45c, 0x19ca0):
            require(machine.reg_read(reg(0)) == MMIO, "stock ack base")
            offset = machine.reg_read(reg(1))
            width = 2 if at == 0x2a8 else 1
            value = machine.reg_read(reg(2)) & ((1 << (8*width))-1)
            require((offset, width) in ((4, 2), (2, 2), (10, 1), (0x200, 1)),
                    "stock ack register/width")
            if offset != 10 and value:
                trace.append((width, offset, value))
            machine.reg_write(R.UC_ARM64_REG_PC, machine.reg_read(R.UC_ARM64_REG_LR))
        else:
            require(0x8db0 <= at < 0x8de4 or 0x196a8 <= at < 0x196c4,
                    "unexpected stock ack branch")
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(TEXT+0x8db0, TEXT+0x8de4, count=100)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == TEXT+0x8de4, "stock TX/RX ack stopped early")
    uc.reg_write(reg(19), DATA)
    uc.reg_write(reg(0), dma)
    uc.emu_start(TEXT+0x196a8, TEXT+0x196c4, count=100)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == TEXT+0x196c4, "stock DMA ack stopped early")
    return trace


def stock_hub_text():
    raw = HUB_BINARY.read_bytes()
    require(hashlib.sha256(raw).hexdigest() == HUB_SHA, "unexpected extcon binary")
    require(raw[:6] == b"\x7fELF\x02\x01" and
            struct.unpack_from("<H", raw, 18)[0] == 183, "expected ARM64 LE ELF")
    offset = struct.unpack_from("<Q", raw, 40)[0]
    size, count = struct.unpack_from("<HH", raw, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, offset + i * size)
                for i in range(count)]
    tables = {}
    functions = {}
    for index, section in enumerate(sections):
        if section[1] != 2:
            continue
        names_section = sections[section[6]]
        names = raw[names_section[4]:names_section[4] + names_section[5]]
        symbols = []
        for at in range(section[4], section[4] + section[5], section[9]):
            name, info, _, si, value, length = struct.unpack_from("<IBBHQQ", raw, at)
            label = names[name:names.index(b"\0", name)].decode()
            symbols.append(label)
            if (info & 15) == 2:
                functions[label] = (si, value, length)
        tables[index] = symbols
    si, start, length = functions["mtk_usb_extcon_set_hub"]
    require((start, length) == (0x1208, 0x120), "unexpected stock hub function")
    require(functions["mtk_usb_extcon_detect_cable"] == (si, 0x10e8, 0xe4),
            "unexpected stock cable policy")
    relocations = {}
    for section in sections:
        if section[1] != 4 or section[7] != si:
            continue
        for at in range(section[4], section[4] + section[5], section[9]):
            place, info, addend = struct.unpack_from("<QQq", raw, at)
            if (info & 0xffffffff) == 0x11b:
                relocations[place] = (tables[section[6]][info >> 32], addend)
    for place, label in {
        0x1118: "gpiod_get_value_cansleep", 0x1254: "_dev_info",
        0x127c: "gpiod_direction_output", 0x12a0: "gpiod_direction_output",
        0x12dc: "gpiod_direction_output", 0x12f4: "gpiod_direction_output",
        0x130c: "gpiod_direction_output",
        0x1288: "__const_udelay", 0x12ac: "__const_udelay",
    }.items():
        require(relocations.get(place) == (label, 0),
                "unexpected extcon imported call at " + hex(place))
    section = sections[si]
    return raw[section[4]:section[4] + section[5]]


def original_hub(code, requests):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, code)
    uc.mem_map(DATA, 0x10000)
    reg = lambda n: getattr(R, "UC_ARM64_REG_X" + str(n))
    stop, context = DATA + 0xff00, DATA
    descriptors = {DATA + 0x8000 + i * 0x100: i for i in (1, 2, 3)}
    for offset, gpio in ((0x118, 1), (0x108, 2), (0x110, 3)):
        descriptor = next(d for d, i in descriptors.items() if i == gpio)
        uc.mem_write(context + offset, struct.pack("<Q", descriptor))
    trace = []

    # ET_REL imported BL sites still branch to themselves; emulate the call
    # result at that instruction and advance, rather than using the entry LR.
    def hook(machine, pc, size, _):
        at = pc - TEXT
        if at == 0x1254:
            machine.reg_write(reg(0), 0)
            machine.reg_write(R.UC_ARM64_REG_PC, pc + 4)
        elif at in (0x127c, 0x12a0, 0x12dc, 0x12f4, 0x130c):
            descriptor = machine.reg_read(reg(0))
            require(descriptor in descriptors, "unexpected stock GPIO")
            value = machine.reg_read(reg(1))
            require(value in (0, 1), "unexpected stock GPIO value")
            trace.append((2, descriptors[descriptor], value))
            machine.reg_write(reg(0), 0)
            machine.reg_write(R.UC_ARM64_REG_PC, pc + 4)
        elif at in (0x1288, 0x12ac):
            require(machine.reg_read(reg(0)) == 1000 * 0x10c7,
                    "unexpected stock constant delay")
            trace.append((3, 0, 1000))
            machine.reg_write(R.UC_ARM64_REG_PC, pc + 4)
        else:
            require(0x1208 <= at < 0x1328, "unexpected hub branch")

    uc.hook_add(UC_HOOK_CODE, hook)
    for request in requests:
        uc.reg_write(R.UC_ARM64_REG_SP, DATA + 0xf000)
        uc.reg_write(R.UC_ARM64_REG_LR, stop)
        uc.reg_write(reg(0), context)
        uc.reg_write(reg(1), request)
        uc.emu_start(TEXT + 0x1208, stop, count=1000)
        require(uc.reg_read(R.UC_ARM64_REG_PC) == stop, "stock hub did not return")
    return trace


def original_id_policy(code, logical_id, vbus, dr_state):
    # Start immediately after the read-only GPIO import; substitute only the
    # internal VBUS predicate. Stop before logging or role/GPIO side effects.
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(TEXT, (len(code) + 4095) & ~4095)
    uc.mem_write(TEXT, code)
    uc.mem_map(DATA, 0x1000)
    reg = lambda n: getattr(R, "UC_ARM64_REG_X" + str(n))
    uc.reg_write(reg(0), logical_id)
    uc.reg_write(reg(19), DATA)
    uc.reg_write(reg(20), DATA + 0x138)
    uc.mem_write(DATA + 0x18, struct.pack("<I", dr_state))

    def hook(machine, pc, size, _):
        at = pc - TEXT
        if at == 0xdf0:
            machine.reg_write(reg(0), vbus)
            machine.reg_write(R.UC_ARM64_REG_PC, machine.reg_read(R.UC_ARM64_REG_LR))
        else:
            require(0x111c <= at < 0x1160, "unexpected cable policy branch")

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(TEXT + 0x111c, TEXT + 0x1160, count=100)
    require(uc.reg_read(R.UC_ARM64_REG_PC) == TEXT + 0x1160, "stock policy did not stop")
    return uc.reg_read(reg(22)), uc.reg_read(reg(23))


BRIDGE = r"""
#include <stdint.h>
#include <string.h>
#include "MusbHostCore.c"
typedef struct {uint16_t Hub; uint32_t Offset, Count; uint32_t Trace[24];} FIXTURE;
static int R8(void *p, uint32_t a, uint8_t *v) {
  (void)p; if (a!=1U) return 0; *v=0x10U; return 1;
}
static void Log(FIXTURE *p, uint32_t k, uint32_t a, uint32_t v) {
  if(p->Count<8U) {uint32_t i=3U*p->Count++; p->Trace[i]=k;p->Trace[i+1]=a;p->Trace[i+2]=v;}
}
static int R16(void *p, uint32_t a, uint16_t *v) {
  FIXTURE *f=p; if(a!=f->Offset)return 0;*v=f->Hub;Log(f,1,a,*v);return 1;
}
static int W16(void *p, uint32_t a, uint16_t v) {
  FIXTURE *f=p;Log(f,2,a,v);if(a==f->Offset)f->Hub=v;return 1;
}
int target(uint8_t rx,uint8_t ep,uint8_t addr,uint8_t speed,uint8_t hub,
           uint8_t port,uint8_t multi,uint16_t old,uint32_t *out) {
  FIXTURE f={0};MUSB_HOST host={0};MUSB_HOST_ROUTE route={hub,port,multi,0};
  f.Hub=old;f.Offset=(rx?0x486U:0x482U)+8U*ep;
  host.Io.Context=&f;host.Io.ControllerLength=0x500U;
  host.Io.Read8=R8;host.Io.Read16=R16;host.Io.Write16=W16;
  if(SetTargetAddress(&host,ep,addr,speed,speed==0U?0x80U:0xc0U,rx,&route)!=MusbOk)
    return -1;
  memcpy(out,f.Trace,sizeof(f.Trace));return (int)f.Count;
}

#include "MtkUsbHubCore.c"
typedef struct { uint32_t Count; uint32_t Trace[192]; } HUB_FIXTURE;
static MTK_USB_HUB_STATUS HubRead(void *p, MTK_USB_HUB_GPIO g, uint8_t *v) {
  (void)p;if(g!=MtkUsbHubGpioId)return MtkUsbHubInvalid;*v=0;return MtkUsbHubSuccess;
}
static MTK_USB_HUB_STATUS HubRole(void *p) { (void)p;return MtkUsbHubSuccess; }
static MTK_USB_HUB_STATUS HubLog(HUB_FIXTURE *f,uint32_t k,uint32_t g,uint32_t v) {
  if(f->Count>=64U)return MtkUsbHubInvalid;
  uint32_t i=3U*f->Count++;f->Trace[i]=k;f->Trace[i+1]=g;f->Trace[i+2]=v;
  return MtkUsbHubSuccess;
}
static MTK_USB_HUB_STATUS HubWrite(void *p,MTK_USB_HUB_GPIO g,uint8_t v) {
  return HubLog(p,2U,(uint32_t)g,v);
}
static MTK_USB_HUB_STATUS HubDelay(void *p,uint32_t us) {return HubLog(p,3U,0U,us);}
int hub_target(const uint8_t *requests,uint32_t count,uint32_t *out) {
  HUB_FIXTURE f={0};MTK_USB_HUB_CORE core={0};
  MTK_USB_HUB_IO io={&f,HubRead,HubRole,HubWrite,HubDelay};
  if(MtkUsbHubCoreInitialize(&core,&io)!=MtkUsbHubSuccess ||
     MtkUsbHubCorePrepareHost(&core)!=MtkUsbHubSuccess)return -1;
  for(uint32_t i=0;i<count;i++)
    if(MtkUsbHubCoreSetHub(&core,requests[i])!=MtkUsbHubSuccess)return -1;
  memcpy(out,f.Trace,sizeof(f.Trace));return (int)f.Count;
}

#include <Library/MtkMusbPlatformCore.h>
typedef struct {uint16_t Tx,Rx;uint8_t Dma,Index;uint32_t Count,Trace[9];int Lock;} ACK;
static int AckR8(void*p,uint32_t a,uint8_t*v){ACK*f=p;*v=a==0xe?f->Index:a==0x200?f->Dma:0;return 1;}
static int AckR16(void*p,uint32_t a,uint16_t*v){ACK*f=p;*v=a==2?f->Tx:a==4?f->Rx:0;return 1;}
static int AckR32(void*p,uint32_t a,uint32_t*v){(void)p;(void)a;*v=0;return 1;}
static void AckLog(ACK*f,uint32_t w,uint32_t a,uint32_t v){if(f->Count<3){uint32_t i=3*f->Count++;f->Trace[i]=w;f->Trace[i+1]=a;f->Trace[i+2]=v;}}
static int AckW8(void*p,uint32_t a,uint8_t v){ACK*f=p;if(a==0xe)f->Index=v;else if(a==0x200){AckLog(f,1,a,v);f->Dma&=(uint8_t)~v;}else return 0;return 1;}
static int AckW16(void*p,uint32_t a,uint16_t v){ACK*f=p;AckLog(f,2,a,v);if(a==2)f->Tx&=(uint16_t)~v;else if(a==4)f->Rx&=(uint16_t)~v;else return 0;return 1;}
static int AckLock(void*p){ACK*f=p;if(f->Lock)return 0;f->Lock=1;return 1;}
static void AckUnlock(void*p){((ACK*)p)->Lock=0;}
int ack_target(uint16_t tx,uint16_t rx,uint8_t dma,uint32_t*out){
  ACK f={0};MTK_MUSB_PLATFORM_IO io={0};f.Tx=tx;f.Rx=rx;f.Dma=dma;f.Index=3;
  io.Context=&f;io.ControllerLength=0x10000;io.Read8=AckR8;io.Read16=AckR16;io.Read32=AckR32;
  io.Write8=AckW8;io.Write16=AckW16;io.TryLock=AckLock;io.Unlock=AckUnlock;
  if(MtkMusbPlatformAcknowledgeIdleInterrupts(&io)!=MtkMusbPlatformSuccess||f.Tx||f.Rx||f.Dma||f.Lock||f.Index!=3)return -1;
  memcpy(out,f.Trace,sizeof(f.Trace));return (int)f.Count;
}

typedef struct {uint16_t Reset;uint8_t Index;int Lock;uint32_t Writes,Trace[3];} RESET;
static int ResetR8(void*p,uint32_t a,uint8_t*v){RESET*f=p;*v=a==0xe?f->Index:a==0x60?0x98:0;return 1;}
static int ResetR16(void*p,uint32_t a,uint16_t*v){RESET*f=p;*v=a==0x74?f->Reset:a==0x12&&f->Index==0?8:0;return 1;}
static int ResetR32(void*p,uint32_t a,uint32_t*v){(void)p;(void)a;*v=0;return 1;}
static int ResetW8(void*p,uint32_t a,uint8_t v){RESET*f=p;if(a!=0xe)return 0;f->Index=v;return 1;}
static int ResetW16(void*p,uint32_t a,uint16_t v){RESET*f=p;if(a!=0x74||f->Writes)return 0;
  f->Trace[0]=a;f->Trace[1]=16;f->Trace[2]=v;f->Writes++;f->Reset=v;return 1;}
static int ResetLock(void*p){RESET*f=p;if(f->Lock)return 0;f->Lock=1;return 1;}
static void ResetUnlock(void*p){((RESET*)p)->Lock=0;}
int reset_target(uint16_t before,uint32_t*out){
  RESET f={0};MTK_MUSB_PLATFORM_IO io={0};f.Reset=before;f.Index=7;
  io.Context=&f;io.ControllerLength=0x10000;io.Read8=ResetR8;io.Read16=ResetR16;io.Read32=ResetR32;
  io.Write8=ResetW8;io.Write16=ResetW16;io.TryLock=ResetLock;io.Unlock=ResetUnlock;
  if(MtkMusbPlatformRequestDisconnectedDeviceReset(&io,0)!=MtkMusbPlatformSuccess||
     f.Lock||f.Index!=7||f.Writes!=1)return -1;
  memcpy(out,f.Trace,sizeof(f.Trace));return 1;
}

"""


def main():
    global NATIVE_SELECTED_REPORT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-report", type=Path, default=NATIVE_SELECTED_REPORT)
    parser.add_argument("--output", type=Path, default=REPO.parent / "analysis/musb-binary-tests.json")
    parser.add_argument("--hub-output", type=Path, default=REPO.parent / "analysis/usb-hub-binary-tests.json")
    args = parser.parse_args()
    for path in (args.native_report, args.output, args.hub_output):
        require(path.resolve().parent == (REPO.parent / "analysis").resolve(),
                "reports must remain in project analysis")
    NATIVE_SELECTED_REPORT = args.native_report.resolve()
    code = stock_text()
    cold_code, cold_relocations = stock_cold_source()
    require(code == cold_code, "cold-host parser selected a different stock .text")
    hub_code = stock_hub_text()
    cold_session_cases = [
        {"initial_devctl": seed, **original_do_host_session(cold_code, seed)}
        for seed in (0x1d, 0x39)
    ]
    canonical_session = next(case for case in cold_session_cases
                             if case["initial_devctl"] == 0x1d)
    power_cases = [
        {
            "musb_speed_global": speed,
            "port_mode_bit14": port_mode_bit14,
            "register_writes": original_musb_start_power(
                cold_code, speed, port_mode_bit14
            ),
        }
        for speed in (0, 1) for port_mode_bit14 in (0, 1)
    ]
    high_speed_softconn0 = next(
        case for case in power_cases
        if case["musb_speed_global"] == 1 and not case["port_mode_bit14"]
    )
    cold_host_sequences = {
        "host_phy_start_tail": original_do_host_start_tail(cold_code),
        "do_host_work_session": {
            "function": "do_host_work",
            "block_text_offsets": [hex(DO_HOST_SESSION_BLOCK[0]), hex(DO_HOST_SESSION_BLOCK[1])],
            "canonical_seed": 0x1d,
            "canonical_seed_meaning": "SESSION|HOSTMODE(bit2)|VBUS_VALID",
            "cases": cold_session_cases,
            "common_c_api_register_writes": [canonical_session["register_writes"][-1]],
            "additional_seed_note": "0x39 is a noncanonical preservation-only fixture; it is not compared with C host readiness.",
            "c_api_sequence_note": "This canonical comparison selects the final SESSION-set write; the separate full startup verifier compares SESSION-clear, INVALID hold, PHY and reset ordering.",
        },
        "musb_start_power": {
            "function": "musb_start",
            "block_text_offsets": [hex(MUSB_START_POWER_BLOCK[0]), hex(MUSB_START_POWER_BLOCK[1])],
            "cases": power_cases,
            "high_speed_softconn0_register_writes": high_speed_softconn0["register_writes"],
            "power_register_semantics": "musb_speed==0 writes 0x01; nonzero writes 0x21; port_mode bit14 adds SOFTCONN bit6 (0x40).",
            "c_api_policy_note": "The shared nonzero-speed, bit14-clear value is 0x21. Stock writes this computed byte; the C API RMW preserves unrelated POWER bits, so this match does not claim equivalence for arbitrary prior POWER values. The bit14-set stock value intentionally includes SOFTCONN.",
        },
        "vbus_status_callback": {
            "text_offset": "0x1b73c",
            "function_bytes": ["0x52800020", "0xd65f03c0"],
            "disassembly": "mov w0, #1; ret",
            "reported_value": 1,
            "physical_vbus_measured": False,
            "limitation": "The stock callback is an unconditional constant and is not evidence that VBUS is present.",
        },
        "modeled_calls": [
            "do_host_work msleep(100) and __const_udelay(0x0147aeb8) are recorded, not waited.",
            "set_usb_phy_mode(0) is recorded without changing a PHY.",
            "musb_readb/writeb wrappers are modeled at fixed DEVCTL offset 0x60 with synthetic byte memory; wrapper lock and clock-gate paths do not execute.",
            "musb_start POWER block uses original instructions with pre-block x22 set to the musb_uart_debug global page; musb_writeb at .text+0x45c is an argument-checked recorder.",
            "The SESSION block starts after the earlier host/VBUS callback. A second bounded block executes the original HOST PHY arguments and call order through musb_start, with imported calls recorded. No physical HOSTMODE change or full role-worker equivalence is claimed.",
        ],
    }
    native_cold_comparison = compare_cold_c_api(cold_host_sequences)
    count = 0
    hub_cases = 0
    id_cases = 0
    ack_cases = 0
    reset_cases = []
    ep0_status_cases = [original_ep0_pending_status(code, csr)
                       for csr in (0, 1, 2, 4, 8, 9, 10, 0x18, 0xffff)]
    rng = random.Random(6789)
    with tempfile.TemporaryDirectory(prefix="musb-binary-", dir=None if sys.platform == "darwin" else REPO / "Build") as temporary:
        temporary = Path(temporary)
        bridge = temporary / "bridge.c"
        bridge.write_text(BRIDGE)
        library = temporary / "musb.so"
        subprocess.run(["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-I" + str(HEADER.parents[1]),
                        "-I" + str(CORE.parent), str(bridge), str(PLATFORM_CORE), "-o", str(library)],
                       check=True, capture_output=True, text=True)
        compiled = C.CDLL(str(library))
        reset_target = compiled.reset_target
        reset_target.argtypes = [C.c_uint16, C.POINTER(C.c_uint32)]
        reset_target.restype = C.c_int
        for before in (0, 2, 0x4000, 0x8000, 0xfffc):
            expected = original_device_reset(code, before)
            output = (C.c_uint32 * 3)()
            require(reset_target(before, output) == 1, "production reset request failed")
            actual = [list(output)]
            require(actual == expected["register_writes"], "stock/C reset width or RMW differs")
            reset_cases.append({**expected, "production_register_writes": actual, "match": True})
        ack_target = compiled.ack_target
        ack_target.argtypes = [C.c_uint16, C.c_uint16, C.c_uint8, C.POINTER(C.c_uint32)]
        ack_target.restype = C.c_int
        for tx, rx, dma in itertools.product((0, 1, 0x1234, 0xffff),
                                             (0, 2, 0x8000, 0xffff), (0, 1, 0xa5, 0xff)):
            expected = original_ack(code, tx, rx, dma)
            output = (C.c_uint32 * 9)()
            events = ack_target(tx, rx, dma, output)
            require(events >= 0, "production idle ack failed")
            actual = [tuple(output[i*3:i*3+3]) for i in range(events)]
            require(actual == expected, f"ack mismatch: {actual} != {expected}")
            ack_cases += 1
        hub_target = compiled.hub_target
        hub_target.argtypes = [C.POINTER(C.c_uint8), C.c_uint32, C.POINTER(C.c_uint32)]
        hub_target.restype = C.c_int
        for length in range(1, 6):
            for tail in itertools.product((0, 1), repeat=length - 1):
                requests = (1,) + tail
                expected = original_hub(hub_code, requests)
                request_array = (C.c_uint8 * length)(*requests)
                output = (C.c_uint32 * 192)()
                events = hub_target(request_array, length, output)
                require(events >= 0, "production hub sequence failed")
                actual = [tuple(output[i * 3:i * 3 + 3]) for i in range(events)]
                require(actual == expected, f"hub mismatch {requests}: {actual} != {expected}")
                hub_cases += 1
        for logical_id, vbus, dr_state in itertools.product((0, 1), (0, 1), range(3)):
            expected = (1, 1) if logical_id == 0 else (0, 2 if vbus and dr_state == 1 else 0)
            require(original_id_policy(hub_code, logical_id, vbus, dr_state) == expected,
                    "stock ID host policy mismatch")
            id_cases += 1
        target = compiled.target
        target.argtypes = [C.c_uint8] * 7 + [C.c_uint16, C.POINTER(C.c_uint32)]
        target.restype = C.c_int
        for (receive, endpoint), speed, address, hub, port, multi in itertools.product(
                [(0, 0), (0, 1), (1, 1)], [0, 1], [0, 127],
                [1, 127], [1, 127], [0, 1]):
            old = rng.randrange(65536)
            expected = original(code, receive, endpoint, address, speed, hub, port, multi, old)
            out = (C.c_uint32 * 24)()
            length = target(receive, endpoint, address, speed, hub, port, multi, old, out)
            require(length >= 0, "production C failed")
            actual = [tuple(out[i * 3:i * 3 + 3]) for i in range(length)]
            require(actual == expected, f"routing mismatch rx={receive} ep={endpoint} speed={speed}: "
                    f"stock={expected!r} production={actual!r}")
            count += 1
    result = {
        "status": "passed", "routing_cases": count, "idle_ack_cases": ack_cases,
        "scope": "original musb_start_urb TX/RX routing, idle W1C, bounded cold-host POWER/SESSION, and extcon hub blocks with production C traces",
        "transport": "fake MMIO/GPIO/delay and modeled import calls; no enumeration, packets, physical VBUS/PHY or device execution",
        "stock_binary_sha256": digest(BINARY), "runner_sha256": digest(Path(__file__)),
        "source_sha256": {str(p.relative_to(REPO)): digest(p) for p in
                           (CORE, HEADER, PLATFORM_CORE, PLATFORM_HEADER, COLD_TEST)},
        "idle_ack_blocks": {"tx_rx": ["0x8db0", "0x8de4"], "dma": ["0x196a8", "0x196c4"]},
        "idle_ack_scope": "stock observed completion W1C write widths/order vs production idle ack; active-state rejection tested separately with fake MMIO",
        "blocks": {"tx": ["0xbf8c", "0xc540"], "rx": ["0xc044", "0xc0c4"]},
        "stock_cold_host_functions": {
            "musb_start": {"text_offset": "0xd48", "bytes": 0x3cc},
            "do_host_work": {"text_offset": "0x1c314", "bytes": 0x654},
            "mt_usb_get_vbus_status": {"text_offset": "0x1b73c", "bytes": 8},
            "session_read_wrapper": {"text_offset": "0x1bbf8", "bytes": 0x1bc},
            "session_write_wrapper": {"text_offset": "0x1ba50", "bytes": 0x1a8},
        },
        "stock_cold_host_relocations": {
            hex(place): {"type": relocation[0], "symbol": relocation[1], "addend": relocation[2]}
            for place, relocation in cold_relocations.items()
            if place in (0x1014, 0x101c, 0x1030, 0x1c6ac, 0x1c6b0, 0x1c6bc, 0x1c6d4, 0x1c6e0, 0x1c6e4, 0x1c6f0)
        },
        "cold_host_sequences": cold_host_sequences,
        "native_cold_host_comparison": native_cold_comparison,
        "device_disconnect_sequences": {
            "ep0_pending_status_branch": {
                "function": "musb_g_ep0_irq",
                "test_text_offset": hex(EP0_DATAEND_TEST),
                "return_text_offset": hex(EP0_DATAEND_RETURN),
                "cases": ep0_status_cases,
            },
            "reset": {
                "function": "musb_platform_reset",
                "block_text_offsets": [hex(value) for value in MUSB_SWRST_BLOCK],
                "stock_c_api_cases": reset_cases,
                "limitations": [
                    "Only the final 16-bit SWRST RMW block executes; imported MMIO wrappers are modeled.",
                    "Stock DMA channel cancellation/burst configuration is excluded. The C API instead requires all DMA/QMU and endpoint requests already idle.",
                    "No controller reset completion, cable detach, USB packets, VBUS, PHY or physical keyboard input is established.",
                    "The EP0 test starts after CSR0 was read and stops before the epilogue; it does not execute the full IRQ handler."
                ],
            },
        },
        "multi_tt": "explicit core API only; UEFI default hub alternate setting uses single TT",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    hub_result = {
        "status": "passed", "sequence_cases": hub_cases, "logical_id_cases": id_cases,
        "scope": "original extcon set_hub ARM64 vs production GPIO/delay traces; read-only stock ID branch",
        "transport": "fake GPIO/delay, caller host gate and ID reads; no hardware or role changes",
        "stock_binary_sha256": digest(HUB_BINARY), "runner_sha256": digest(Path(__file__)),
        "source_sha256": {str(p.relative_to(REPO)): digest(p) for p in (HUB_CORE, HUB_HEADER)},
        "stock_functions": {"set_hub": "0x1208+0x120", "detect_cable": "0x10e8+0xe4"},
        "on": ["GPIO99=1", "1000us", "GPIO4=1", "1000us", "GPIO104=1"],
        "off": ["GPIO104=0", "GPIO4=0", "GPIO99=0"],
        "stock_policy": "DT flags=0; logical ID=0 requests host/hub-on",
        "additional_port_policy": "two consecutive ID samples and read-only caller role check; fail-stop quarantine",
        "limitations": "valid GPIOs, successful callbacks, sequences first enable then known states; initial unknown-off and failures tested only natively; physical pogo routing unproven",
    }
    args.hub_output.write_text(json.dumps(hub_result, indent=2) + "\n")
    print(f"MUSB original/production routing traces: {count} passed; idle completion ack: {ack_cases} passed")
    print(f"USB hub original/production sequences: {hub_cases} passed; stock ID cases: {id_cases} passed")
    print(f"Peripheral DATAEND branches: {len(ep0_status_cases)} passed; stock/production reset RMW: {len(reset_cases)} passed")


if __name__ == "__main__":
    main()
