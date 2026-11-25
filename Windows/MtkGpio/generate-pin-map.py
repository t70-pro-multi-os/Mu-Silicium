#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate a factual register map from the already extracted stock module."""
import hashlib
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT.parent
MODULE = PROJECT / "analysis/stock-binaries/vendor_boot_a_0/pinctrl-mt6789.ko"
EXPECTED = "5ba5cca4ceb724767b8484de321b4b026b6d024aa47fe7627509e6d71553a2fb"
FIELDS = ["pu", "pd", "pupd", "r0", "r1", "ies", "smt", "drv", "rsel", "eh"]
EXPECTED_PINS = {
    "pu": 160, "pd": 160, "pupd": 28, "r0": 28, "r1": 28,
    "ies": 188, "smt": 188, "drv": 187, "rsel": 20, "eh": 34,
}
EXPECTED_UNIQUE = {
    "pu": 160, "pd": 160, "pupd": 28, "r0": 28, "r1": 28,
    "ies": 188, "smt": 108, "drv": 145, "rsel": 20, "eh": 34,
}


def symbols(data):
    assert data[:6] == b"\x7fELF\x02\x01"
    assert struct.unpack_from("<H", data, 18)[0] == 183
    offset = struct.unpack_from("<Q", data, 40)[0]
    size, count, _ = struct.unpack_from("<HHH", data, 58)
    assert size == 64 and offset + size * count <= len(data)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * size)
                for i in range(count)]
    result = {}
    wanted = {f"mt6789_pin_{field}_range" for field in FIELDS}
    wanted.add("mt6789_pull_type")
    for section in sections:
        if section[1] != 2:
            continue
        assert section[9] == 24 and section[5] % 24 == 0
        strings_section = sections[section[6]]
        strings = data[strings_section[4]:strings_section[4] + strings_section[5]]
        for pos in range(section[4], section[4] + section[5], 24):
            name_offset, _, _, index, value, length = struct.unpack_from(
                "<IBBHQQ", data, pos)
            name = strings[name_offset:strings.index(0, name_offset)].decode()
            if name not in wanted:
                continue
            target_section = sections[index]
            assert value + length <= target_section[5]
            start = target_section[4] + value
            assert start + length <= len(data) and name not in result
            result[name] = (start, data[start:start + length])
    assert len(result) == len(FIELDS) + 1
    return result


def parse_ranges(raw, field, body):
    assert len(raw) % 20 == 0
    parsed = {}
    reference_fields = {}
    for pos in range(0, len(raw), 20):
        first, last, base, address, step, bit, width, regbits, fixed =             struct.unpack_from("<HHB3xIBBBBB3x", raw, pos)
        assert 0 <= first <= last < 188 and 1 <= base <= 6
        assert step == 16 and regbits == 32 and not fixed
        if field in ("pu", "pd", "pupd", "r0", "r1", "ies", "smt"):
            assert width == 1
        elif field == "drv":
            assert width in (2, 3)
        elif field == "rsel":
            assert width == 3
        elif field == "eh":
            assert width in (3, 5)
        for pin in range(first, last + 1):
            shift = bit + (pin - first) * width
            assert shift % 32 + width <= 32
            value = (base + 1, address + (shift // 32) * step,
                     shift % 32, width)
            assert pin not in parsed and value[1] + 8 < 4096
            parsed[pin] = value

    for match in re.finditer(r"(PINS?_FIELD_BASE)\(([^)]+)\)", body):
        args = [int(x.strip(), 0) for x in match[2].split(",")]
        first, last, base, address, step, bit, width = args
        assert step == 16
        for pin in range(first, last + 1):
            shift = bit + (0 if match[1] == "PINS_FIELD_BASE" else pin - first) * width
            assert shift % 32 + width <= 32
            reference_fields[pin] = (
                base + 1, address + (shift // 32) * step, shift % 32, width)
    assert parsed == reference_fields, field
    assert len(parsed) == EXPECTED_PINS[field], field
    assert len(set(parsed.values())) == EXPECTED_UNIQUE[field], field
    return parsed


def main():
    data = MODULE.read_bytes()
    assert hashlib.sha256(data).hexdigest() == EXPECTED
    reference = PROJECT / "kernel-reference/mindone/modules/pinctrl_mediatek/pinctrl-mt6789.c"
    text = reference.read_text()
    syms = symbols(data)
    maps = {}
    for field in FIELDS:
        name = f"mt6789_pin_{field}_range"
        _, raw = syms[name]
        body = re.search(r"\b" + name + r"\[\] = \{(.*?)\};",
                         text, re.S).group(1)
        maps[field] = parse_ranges(raw, field, body)

    types = struct.unpack("<188I", syms["mt6789_pull_type"][1])
    assert set(types) == {1, 4, 9}
    body = re.search(r"mt6789_pull_type\[\] = \{(.*?)\};", text, re.S).group(1)
    reference_types = tuple(
        int(x) for x in re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        .replace("\n", "").split(",") if x.strip())
    assert types == reference_types

    def field(value):
        return "{0x%03X,%u,%u,%u}" % (value[1], value[0], value[2], value[3])

    zero = "{0,0,0,0}"
    header = [
        "/* SPDX-License-Identifier: BSD-2-Clause-Patent */",
        "/* Generated factual register/bit locations from the exact stock pinctrl-mt6789.ko.",
        " * Original module SHA-256: " + EXPECTED,
        " * Cross-checked with the pinned MediaTek reference. No Linux implementation code.",
        " * Shared SMT and drive descriptors are represented as shared fields.",
        " * Regenerate with generate-pin-map.py. */",
        "#ifndef MTK_GPIO_PIN_MAP_H",
        "#define MTK_GPIO_PIN_MAP_H",
        "typedef struct { uint16_t Offset; uint8_t Space,Bit,Width; } MG_FIELD;",
        "typedef struct { MG_FIELD First,Second,Third,Input,Schmitt,Drive,Rsel,Eh; uint8_t Type; } MG_PAD;",
        "static const MG_PAD MgPads[MG_PINS]={",
    ]
    for pin, pull_type in enumerate(types):
        pull_fields = ["pu", "pd"] if pull_type in (1, 9) else ["pupd", "r0", "r1"]
        for name in pull_fields + ["ies", "smt"]:
            assert pin in maps[name], (pin, name)
        values = [field(maps[name][pin]) if pin in maps[name] else zero
                  for name in pull_fields]
        if len(values) == 2:
            values.append(zero)
        values.extend([
            field(maps["ies"][pin]),
            field(maps["smt"][pin]),
            field(maps["drv"][pin]) if pin in maps["drv"] else zero,
            field(maps["rsel"][pin]) if pin in maps["rsel"] else zero,
            field(maps["eh"][pin]) if pin in maps["eh"] else zero,
        ])
        header.append("  {" + ",".join(values) + ",%u}, /* %u */"
                      % (pull_type, pin))
    header += ["};", "#endif", ""]
    out = Path(__file__).with_name("PinMap.h")
    out.write_text("\n".join(header))
    print("Stock GPIO map: 188 pins; verified PU/PD, PUPD, IES, SMT, drive, RSEL, EH descriptors; no device access")


if __name__ == "__main__":
    main()
