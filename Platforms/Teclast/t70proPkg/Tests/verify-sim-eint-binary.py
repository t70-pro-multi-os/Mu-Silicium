#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run shipped CCCI SIM DT consumers in ARM64 Unicorn with isolated OF services.

No kernel/module initialization, modem command, private identity or device access.
This checks DT names/properties and modem RPC config results, not tray interrupts.
"""
from pathlib import Path
import argparse, hashlib, importlib.util, json, struct
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
    UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
    UC_ARM64_REG_X8, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR)

PKG = Path(__file__).resolve().parents[1]
ROOT = PKG.parents[2].parent
MODULE = ROOT / "analysis/stock-binaries/vendor_dlkm_a/ccci_md_all.ko"
SHA = "c7a0435c27c27d1efdd5076ee3d656b383141c6252417e1975ed6d0550e0762b"
DT = ROOT / "analysis/stock-fdt/effective.dtb"

def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def elf(raw):
    assert raw[:6] == b"\x7fELF\x02\x01"
    assert struct.unpack_from("<HH", raw, 16) == (1, 183)
    start = struct.unpack_from("<Q", raw, 40)[0]
    width, count = struct.unpack_from("<HH", raw, 58)
    assert width == 64 and start + count * width <= len(raw)
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, start + i * width) for i in range(count)]
    def content(i):
        s = sections[i]
        if s[1] == 8:
            return bytes(s[5])
        assert s[4] + s[5] <= len(raw)
        return raw[s[4]:s[4] + s[5]]
    symbols, tables, bases = {}, {}, {}
    for i, s in enumerate(sections):
        if s[1] != 2:
            continue
        names, table = content(s[6]), []
        assert s[9] == 24 and s[5] % 24 == 0
        for offset in range(0, s[5], 24):
            no, info, _, si, value, size = struct.unpack_from("<IBBHQQ", content(i), offset)
            assert no < len(names) and b"\0" in names[no:]
            name = names[no:].split(b"\0", 1)[0].decode("ascii")
            row = (name, si, value, size)
            table.append(row)
            if name:
                symbols[name] = row
        tables[i] = table
    cursor = 0x10000000
    for i, s in enumerate(sections):
        if s[2] & 2 and s[5]:
            bases[i] = cursor
            cursor += (s[5] + 4095) & ~4095
    return sections, content, symbols, tables, bases

def verify():
    raw = MODULE.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == SHA
    sections, content, symbols, tables, bases = elf(raw)
    names = ("get_dtsi_eint_node", "get_eint_attr_DTSVal")
    assert symbols[names[0]][2:] == (0xa080, 0x35c)
    assert symbols[names[1]][2:] == (0xa3e0, 0x194)
    helper = load(PKG / "Tests/verify-himax-binary.py", "sim_stock_machine")
    dt = load(PKG / "Linux/platform-modules.py", "sim_dt_parser")
    nodes, _ = dt.tree(DT.read_bytes())
    node_names = ("MD1-SIM1-HOT-PLUG-EINT", "MD1-SIM2-HOT-PLUG-EINT")
    props = {}
    for name in node_names:
        node = nodes["/soc/" + name]
        assert node["enabled"]
        props[name] = {k: dt.cells(v) for k, v in node["properties"].items()
                       if k in ("interrupts", "debounce", "sockettype", "dedicated", "src_pin")}
    ranges = [(symbols[n][1], symbols[n][2], symbols[n][2] + symbols[n][3]) for n in names]
    allowed = {"of_find_node_by_name", "of_property_read_u32_index",
               "ccci_dump_write", "_printk", "__stack_chk_fail", "strncmp"}
    cases = []
    def machine(values):
        u, stop, external, address, relocations = helper.make_stock_machine(
            raw, sections, content, tables, symbols, bases, ranges, allowed)
        data_names = ("md_eint_struct", "md_eint_map_struct", "md_eint_node", "eint_node_prop")
        data_ranges = [(symbols[n][1], symbols[n][2], symbols[n][2] + symbols[n][3]) for n in data_names]
        # Legacy-name vector is anonymous in this exact ELF; eint_node_prop+16 references it.
        data_ranges.append((6, 0xede0, 0xee30))
        data_relocations = 0
        for i, sec in enumerate(sections):
            if sec[1] != 4:
                continue
            for offset in range(0, sec[5], 24):
                loc, info, addend = struct.unpack_from("<QQq", content(i), offset)
                if not any(sec[7] == si and lo <= loc < hi for si, lo, hi in data_ranges):
                    continue
                assert info & 0xffffffff == 257
                u.mem_write(bases[sec[7]] + loc,
                            struct.pack("<Q", address(tables[sec[6]][info >> 32]) + addend))
                data_relocations += 1
        at = {pc: name for name, pc in external.items()}
        node_at = {0x30002000 + i * 0x100: name for i, name in enumerate(node_names)}
        lookups, reads = [], []
        def cstr(ptr):
            data = bytes(u.mem_read(ptr, 256))
            assert b"\0" in data
            return data.split(b"\0", 1)[0].decode("ascii")
        def trap(uc, pc, size, unused):
            if pc == stop:
                uc.emu_stop()
                return
            name = at.get(pc)
            if name:
                a, b, c, out = [uc.reg_read(r) for r in (
                    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3)]
                result = 0
                if name == "of_find_node_by_name":
                    assert a == 0
                    target = cstr(b)
                    lookups.append(target)
                    result = next((p for p, n in node_at.items() if n == target and n in values), 0)
                elif name == "of_property_read_u32_index":
                    assert a in node_at
                    prop = cstr(b)
                    rows = values[node_at[a]].get(prop, [])
                    reads.append((node_at[a], prop, c))
                    if c < len(rows):
                        uc.mem_write(out, struct.pack("<I", rows[c]))
                    else:
                        result = -22
                elif name == "strncmp":
                    assert c <= 64
                    aa, bb = bytes(uc.mem_read(a, c)), bytes(uc.mem_read(b, c))
                    aa, bb = aa.split(b"\0", 1)[0], bb.split(b"\0", 1)[0]
                    result = (aa > bb) - (aa < bb)
                elif name == "__stack_chk_fail":
                    raise AssertionError("stock stack cookie failed")
                else:
                    assert name in ("ccci_dump_write", "_printk")
                uc.reg_write(UC_ARM64_REG_X0, result & 0xffffffffffffffff)
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
                return
            assert any(bases[si] + lo <= pc < bases[si] + hi for si, lo, hi in ranges), hex(pc)
            # Linux current-task pointer; no stock instruction bytes are changed.
            if struct.unpack("<I", uc.mem_read(pc, 4))[0] == 0xd5384108:
                uc.reg_write(UC_ARM64_REG_X8, 0x30000000)
                uc.reg_write(UC_ARM64_REG_PC, pc + 4)
        u.hook_add(UC_HOOK_CODE, trap)
        def call(name, args):
            for reg, val in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                 UC_ARM64_REG_X3, UC_ARM64_REG_X4), args):
                u.reg_write(reg, val)
            u.reg_write(UC_ARM64_REG_SP, 0x4000fff0)
            u.reg_write(UC_ARM64_REG_LR, stop)
            u.emu_start(address(symbols[name]), stop, timeout=1000000, count=50000)
            assert u.reg_read(UC_ARM64_REG_PC) == stop
            return struct.unpack("<i", struct.pack("<I", u.reg_read(UC_ARM64_REG_X0) & 0xffffffff))[0]
        call(names[0], [0])
        def query(name, kind, null=None):
            text, out, length = 0x30004000, 0x30004100, 0x30004110
            u.mem_write(text, name.encode("ascii") + b"\0")
            u.mem_write(out, b"\xa5" * 4)
            u.mem_write(length, b"\0" * 4)
            args = [text, len(name), kind, out, length]
            if null is not None:
                args[null] = 0
            result = call(names[1], args)
            return result, struct.unpack("<i", u.mem_read(out, 4))[0], struct.unpack("<I", u.mem_read(length, 4))[0]
        return u, address, query, lookups, reads, relocations, data_relocations
    u, address, query, lookups, reads, cr, dr = machine(props)
    flags = struct.unpack("<I", u.mem_read(address(symbols["eint_node_prop"]), 4))[0]
    assert flags == 3 and lookups[:2] == list(node_names), (flags, lookups)
    for i, expected in enumerate(([0,10,0,0,0,0,1], [1,10,0,0,0,0,2])):
        for name in (node_names[i], node_names[i].replace("-", "_")):
            for kind, value in enumerate(expected):
                result, actual, length = query(name, kind)
                assert (result, actual, length) == (0, value, 4), (i, kind, result, actual, length)
                cases.append({"slot":i + 1, "name":name, "attribute":kind, "value":actual, "status":result})
    for null in (0, 3, 4):
        assert query(node_names[0], 0, null)[0] == -13
        cases.append({"null_argument_index":null, "status":-13})
    assert query(node_names[0], 7)[0] == -12
    assert query("missing-sim-slot", 0)[0] == -11
    cases.extend([{"invalid_attribute":7,"status":-12}, {"missing_slot":True,"status":-11}])
    _, _, query2, _, _, _, _ = machine({node_names[0]:props[node_names[0]]})
    assert query2(node_names[1], 0)[0] == -11
    cases.append({"removed_node":node_names[1],"status":-11})
    bad = {name:dict(v) for name,v in props.items()}
    del bad[node_names[1]]["debounce"]
    _, _, query3, _, _, _, _ = machine(bad)
    assert query3(node_names[1], 1)[0] == -11
    assert query3(node_names[1], 0) == (0, 1, 4)
    cases.append({"missing_property":"debounce","slot":2,"status":-11,"other_attribute_preserved":True})
    return {"status":"passed","cases":len(cases),"case_results":cases,
            "module":str(MODULE),"module_sha256":SHA,
            "dtb":str(DT),"dtb_sha256":hashlib.sha256(DT.read_bytes()).hexdigest(),
            "code_relocations":cr,"data_relocations":dr,
            "node_lookup_calls":lookups,"property_reads":reads,"found_slot_mask":flags,
            "device_access":False,"modem_commands":False,"imei_read":False,
            "limitations":["Actual ARM64 consumers execute; OF/property/log services are isolated substitutes.",
                "No modem firmware, RIL, SIM interrupt/physical tray event or identity acquisition is executed."]}

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify()
    if args.output:
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k:result[k] for k in ("status","cases","module_sha256","found_slot_mask","device_access","imei_read")}))
