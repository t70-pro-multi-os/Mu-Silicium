#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Prove that the ksysfs declaration rules restore stock types, not runtime code."""
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("gki_prepare_kabi", HERE / "prepare.py")
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)


def allocated_sections(path):
    """Include relocated symbol identities as well as loaded section bytes."""
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", data, 16) != (1, 183):
        raise ValueError("Expected an AArch64 ELF64 object")
    off = struct.unpack_from("<Q", data, 40)[0]
    size, count, names_index = struct.unpack_from("<HHH", data, 58)
    if size != 64 or not count or off + size * count > len(data) or names_index >= count:
        raise ValueError("Invalid section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, off + i * size) for i in range(count)]

    def contents(section):
        pos, length = section[4:6]
        if pos + length > len(data):
            raise ValueError("Section exceeds object")
        return data[pos:pos + length]

    def string(table, pos):
        if pos >= len(table):
            raise ValueError("String exceeds table")
        return table[pos:].split(b"\0", 1)[0].decode("ascii")

    strings = contents(sections[names_index])
    names = [string(strings, sec[0]) for sec in sections]
    selected = {i for i, sec in enumerate(sections) if sec[2] & 2 and not names[i].startswith(".discard.")}
    result = {names[i]: {"type": sections[i][1], "flags": sections[i][2],
                        "alignment": sections[i][8], "size": sections[i][5],
                        "bytes": contents(sections[i]) if sections[i][1] != 8 else b"",
                        "relocations": []} for i in selected}
    for sec in sections:
        if sec[1] != 4 or sec[7] not in selected:
            continue
        symbols = sections[sec[6]]
        symbol_strings = contents(sections[symbols[6]])
        symbol_bytes = contents(symbols)
        if sec[9] != 24 or symbols[9] != 24:
            raise ValueError("Unexpected relocation/symbol size")
        relocations = contents(sec)
        for pos in range(0, len(relocations), 24):
            place, info, addend = struct.unpack_from("<QQq", relocations, pos)
            index, kind = info >> 32, info & 0xffffffff
            symbol_pos = index * 24
            if symbol_pos + 24 > len(symbol_bytes):
                raise ValueError("Relocation symbol exceeds table")
            name, sym_info, other, shndx, value, length = struct.unpack_from("<IBBHQQ", symbol_bytes, symbol_pos)
            symbol_name = string(symbol_strings, name)
            symbol_section = names[shndx] if shndx < count else str(shndx)
            identity = (symbol_name, sym_info, other, symbol_section, value, length)
            result[names[sec[7]]]["relocations"].append((place, kind, identity, addend))
    return result


def main():
    out = prep.ROOT / "out/kexec"
    directory = prep.REPORT / "abi-diagnosis"
    directory.mkdir(parents=True, exist_ok=True)
    saved = (out / "kernel/.ksysfs.o.cmd").read_text().splitlines()[0].split(" := ", 1)[1]
    command = shlex.split(saved)
    # Reconstruct the compiler invocation without changing the build's .d file.
    command = [x for x in command if not x.startswith("-Wp,-MMD,")]
    header = str(HERE / "ksysfs-kabi.h")
    while header in command:
        pos = command.index(header)
        if command[pos - 1] != "-include":
            raise ValueError("Unexpected kABI header use in saved command")
        del command[pos - 1:pos + 1]
    stock_header = directory / "stock-config-for-ksysfs.h"
    options = ("KEXEC", "KEXEC_CORE", "CRASH_DUMP", "CRASH_RESERVE", "VMCORE_INFO", "PROC_VMCORE",
               "PROC_VMCORE_DEVICE_DUMP", "ARCH_HAS_GENERIC_CRASHKERNEL_RESERVATION")
    stock_header.write_text("".join("#undef CONFIG_" + option + "\n" for option in options))
    report = {"device_access": False, "success": False}
    try:
        versions = {}
        objects = {}
        type_maps = {}
        for name, extra in (("candidate", []), ("stock-config", ["-include", str(stock_header)]),
                            ("normalized", ["-include", header])):
            obj = directory / ("ksysfs-" + name + ".o")
            args = command.copy()
            args[args.index("-o") + 1] = str(obj)
            subprocess.run(args + extra, cwd=out, env=prep.tool_environment(), check=True)
            types = directory / (name + ".symtypes")
            version = subprocess.check_output(
                [str(out / "scripts/gendwarfksyms/gendwarfksyms"), "--stable", "-T", str(types), str(obj)],
                input="kernel_kobj\n", text=True, env=prep.tool_environment()).strip()
            versions[name] = version
            objects[name] = obj
            type_maps[name] = dict(line.split(" ", 1) for line in types.read_text().splitlines() if line)
        report["versions"] = versions
        report["type_definitions"] = {k: len(v) for k, v in type_maps.items()}
        report["normalized_types_equal_stock"] = type_maps["normalized"] == type_maps["stock-config"]
        raw, normalized = allocated_sections(objects["candidate"]), allocated_sections(objects["normalized"])
        report["runtime_sections_equal"] = raw == normalized
        report["runtime_section_count"] = len(raw)
        report["runtime_differences"] = sorted(k for k in raw.keys() | normalized.keys() if raw.get(k) != normalized.get(k))
        report["success"] = (report["normalized_types_equal_stock"] and report["runtime_sections_equal"]
                             and versions["normalized"] == versions["stock-config"] == "#SYMVER kernel_kobj 0x45e1f39a")
        report["files"] = {p.name: prep.sha(p.read_bytes()) for p in
                           [HERE / "ksysfs-kabi.h", *objects.values(), stock_header]}
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
    (directory / "kabi-validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
