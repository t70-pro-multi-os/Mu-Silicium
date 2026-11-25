#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Confirm that the exact T70 MMC module does not use its MT6779 profile."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import re
import subprocess

DIGEST = "a83f74452557b06ea105a82ec52d9d31951145cccb5c57a225108c6fb34c909c"
OF_TABLE = "__mod_of__msdc_of_ids_device_table"
VARIANTS = ("mt6779", "mt6789")


def require(ok, message):
    if not ok:
        raise ValueError(message)


def verify(module):
    data = module.read_bytes()
    require(sha256(data).hexdigest() == DIGEST, "not the exact T70 stock MMC module")

    def elf(opt):
        return subprocess.check_output(["readelf", opt, module], text=True)

    require("AArch64" in elf("-h"), "not ARM64")
    sec = re.search(r"(?m)^\s*\[\s*(\d+)\]\s+\.rodata\s+PROGBITS\s+"
                    r"[0-9a-f]+\s+([0-9a-f]+)\s+([0-9a-f]+)\s", elf("-SW"))
    require(sec is not None, "missing .rodata")
    section_index = int(sec[1])
    start, length = (int(sec[i], 16) for i in (2, 3))
    require(start + length <= len(data), "truncated .rodata")
    section = data[start:start + length]
    syms = {}
    for line in elf("-Ws").splitlines():
        cols = line.split()
        if len(cols) == 8 and cols[3] == "OBJECT" and cols[-2] == str(section_index):
            name = cols[-1]
            if name in (OF_TABLE, *(v + "_compat" for v in VARIANTS)):
                require(name not in syms, "duplicate ELF object")
                syms[name] = int(cols[1], 16), int(cols[2])
    table_at, table_size = syms[OF_TABLE]
    # arm64 struct of_device_id: name[32], type[32], compatible[128], data pointer.
    require(table_size % 200 == 0 and table_at + table_size <= length, "bad OF table")
    reloc = {}
    active = False
    for line in elf("-rW").splitlines():
        if line.startswith("Relocation section"):
            active = line.startswith("Relocation section '.rela.rodata'")
        if active:
            m = re.match(r"^([0-9a-f]+)\s+[0-9a-f]+\s+R_AARCH64_ABS64\s+[0-9a-f]+"
                         r"\s+\.rodata \+ ([0-9a-f]+)$", line)
            if m:
                reloc[int(m[1], 16)] = int(m[2], 16)

    profiles, report = {}, {}
    for variant in VARIANTS:
        label = ("mediatek," + variant + "-mmc").encode()
        entries = [off for off in range(table_at, table_at + table_size, 200)
                   if section[off + 64:off + 192].split(b"\0", 1)[0] == label]
        require(len(entries) == 1, "missing/duplicate OF match: " + variant)
        offset, size = syms[variant + "_compat"]
        require(size == 40 and offset + size <= length, "unexpected stock profile size")
        data_reloc = entries[0] + 192
        require(reloc.get(data_reloc) == offset, "OF relocation does not select the named profile")
        profiles[variant] = section[offset:offset + size]
        report[variant] = {"of_row_offset": hex(entries[0]), "data_relocation": hex(data_reloc),
                           "profile_offset": hex(offset), "profile_size": size}
    differences = [{"byte_offset": i, **dict(zip(VARIANTS, pair))}
                   for i, pair in enumerate(zip(*(profiles[v] for v in VARIANTS)))
                   if pair[0] != pair[1]]
    require(differences == [{"byte_offset": 39, "mt6779": 0, "mt6789": 1}],
            "unexpected MT6789/MT6779 profile delta")
    asm = subprocess.check_output(["aarch64-linux-gnu-objdump", "-dr", module], text=True)
    loads = re.findall(r"(?m)^\s*([0-9a-f]+):\s+[0-9a-f]+\s+ldrb\s+w8,\s*\[x8,\s*#39\]", asm)
    require(loads == ["745c"], "offset-39 read is not the expected stock CQE check")
    a = asm.index("<msdc_cqe_enable>:")
    b = asm.index("\n\n", a)
    cqe = asm[a:b]
    skip = re.search(r"(?m)^\s*7464:\s+[0-9a-f]+\s+b\.ne\s+([0-9a-f]+)", cqe)
    smc = re.search(r"(?m)^\s*([0-9a-f]+):\s+R_AARCH64_CALL26\s+__arm_smccc_smc", cqe)
    require(skip and smc and 0x7464 < int(smc[1], 16) < int(skip[1], 16)
            and re.search(r"ldrb\s+w8,\s*\[x8,\s*#39\].*?cmp\s+w8,\s*#0x1",
                          cqe, re.S), "stock CQE profile-controlled call not confirmed")
    a = asm.index("<msdc_drv_probe>:")
    b = asm.index("\n\n", a)
    probe = asm[a:b]
    require(re.search(r"mov\s+w2,\s*#0x1\b[^\n]*\n[^\n]*platform_get_resource"
                      r"[^\n]*\n[^\n]*R_AARCH64_CALL26\s+platform_get_resource",
                      probe) and "R_AARCH64_CALL26\tdevm_ioremap_resource" in probe,
            "stock probe top-register resource not confirmed")
    return {"module_sha256": DIGEST, "of_matches": report, "profile_delta": differences,
            "profile_delta_consumer": "msdc_cqe_enable gates a secure firmware call",
            "probe_maps_secondary_mmio": True,
            "scope": "offline stock artifact; no firmware call was invoked"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_mmc_ko", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.stock_mmc_ko), indent=2))
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"T70 stock MSDC verification refused: {error}\n")
