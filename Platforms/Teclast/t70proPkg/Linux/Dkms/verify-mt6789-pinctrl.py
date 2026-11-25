#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Validate the MT6789 pin table, I/O layout, EINT wiring and module ABI offline."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess

BASE = "mindone/modules/pinctrl_mediatek/"
REV = "a71515dbaf0d539ffb36416f14186dad17eef743"
PIN = "/soc/pinctrl"
EINT = "/soc/apirq@1000b000"
BANKS = [0x10005000, 0x11c30000, 0x11d10000, 0x11d40000,
         0x11e20000, 0x11e60000, 0x11ea0000]
DIGESTS = {
    "pinctrl-mt6789.ko": "5ba5cca4ceb724767b8484de321b4b026b6d024aa47fe7627509e6d71553a2fb",
    "pinctrl-mtk-v2.ko": "e06f26d9006bedb7114f4a634517d5080a895b1093362ddf8727f73b1f096cd5",
}
PROBE = "mtk_paris_pinctrl_probe"
PM = "mtk_paris_pinctrl_pm_ops"
shared = runpy.run_path(str(Path(__file__).with_name("verify-touch-dt.py")))
inventory, cells, require = (shared[key] for key in ("inventory", "CELLS", "require"))


def output(*args):
    return subprocess.check_output(args, text=True)


def verify(dtb, repo, stock, headers):
    nodes = inventory(dtb)
    pin, eint = nodes[PIN], nodes[EINT]
    phandle = pin["phandle"]
    require(nodes["/"]["compatible"] == b"mediatek,MT6789\0"
            and pin["compatible"] == b"mediatek,mt6789-pinctrl\0"
            and "gpio-controller" in pin and "interrupt-controller" in pin
            and cells(pin["#gpio-cells"]) == [2] and cells(pin["#interrupt-cells"]) == [2]
            and cells(pin["gpio-ranges"]) == [*cells(phandle), 0, 0, 188],
            "not the expected T70 GPIO controller")
    require(pin["reg-names"] == b"gpio\0iocfg_rb\0iocfg_bm\0iocfg_br\0iocfg_lm\0iocfg_bl\0iocfg_rt\0"
            and cells(pin["reg"]) == [v for addr in BANKS for v in (0, addr, 0, 0x1000)],
            "unexpected GPIO/pin configuration bank geometry")
    require(pin["mediatek,eint"] == eint["phandle"]
            and sum(node.get("phandle") == eint["phandle"] for node in nodes.values()) == 1
            and eint["compatible"] == b"mediatek,mt6983-eint\0"
            and eint["reg-name"] == b"eint\0"
            and cells(eint["reg"]) == [0, 0x1000B000, 0, 0x1000]
            and cells(eint["interrupts"]) == [0, 244, 4, 0]
            and cells(eint["mediatek,instance-num"]) == [1]
            and cells(eint["mediatek,total-pin-number"]) == [160]
            and "mediatek,pins" not in eint,
            "not the single-instance legacy T70 EINT mapping")

    def source(relative):
        return output("git", "-C", str(repo), "show", f"{REV}:{relative}")

    table = source(BASE + "pinctrl-mtk-mt6789.h")
    line = re.compile(r'^\s*(\d+), "GPIO(\d+)",\s*MTK_EINT_FUNCTION\((\w+), (\w+)\)', re.M)
    routes = {}
    for idx, label, mux, irq in line.findall(table):
        require(idx == label and int(idx) not in routes, "invalid pin table index")
        routes[int(idx)] = [mux, irq]
    for gpio in (0, 7, 9):
        require(routes[gpio] == ["0", str(gpio)], f"changed GPIO{gpio} interrupt mapping")
    require(routes[152] == ["NO_EINT_SUPPORT"] * 2, "GPIO152 is not the reset-only pin")
    parser = source(BASE + "mtk-eint.c")
    require(all(key in parser for key in ('"mediatek,eint"', '"mediatek,pins"', "id = i;",
                                          "inst = 0;", "idx = i;", "(i < 32) ? 1 : 0")),
            "missing stock identity-map/debounce fallback")
    vendor = source(BASE + "pinctrl-paris.h")
    baseline = source("drivers/pinctrl/mediatek/pinctrl-paris.h")
    require(re.search(r"mtk_paris_pinctrl_probe\(struct platform_device \*pdev,\s*"
                      r"const struct mtk_pin_soc \*soc\);", vendor)
            and "mtk_paris_pinctrl_probe(struct platform_device *pdev);" in baseline,
            "unexpected in-tree and vendor Paris probe signatures")

    for name, sha in DIGESTS.items():
        require(hashlib.sha256((stock / name).read_bytes()).hexdigest() == sha,
                "wrong stock module: " + name)
    stock_versions = {fields[1]: int(fields[0], 16) for text in output(
        "modprobe", "--show-modversions", str(stock / "pinctrl-mt6789.ko")).splitlines()
                      if len(fields := text.split()) == 2}
    abi = {}
    for row in (headers / "Module.symvers").read_text().splitlines():
        fields = row.split()
        if len(fields) >= 4 and fields[1] in (PROBE, PM, "mtk_build_eint", "mtk_eint_do_init"):
            require(fields[2] == "vmlinux" and fields[3] == "EXPORT_SYMBOL_GPL",
                    "unexpected Ubuntu MediaTek export owner")
            abi[fields[1]] = int(fields[0], 16)
    require(set(abi) == {PROBE, PM, "mtk_build_eint", "mtk_eint_do_init"}
            and stock_versions[PROBE] != abi[PROBE] and stock_versions[PM] != abi[PM],
            "vendor ABI mismatches are not demonstrated")
    release = (headers / "include/config/kernel.release").read_text().strip()
    return {"kernel": release, "gpio_pins": 188, "eint_lines": 160,
            "eint_parent_spi": 244, "ports": {"hall": 0, "sd_detect": 7, "touch": 9},
            "gpio152": "reset only, no EINT",
            "probe_crc": {"stock": hex(stock_versions[PROBE]), "generic": hex(abi[PROBE])},
            "pm_crc": {"stock": hex(stock_versions[PM]), "generic": hex(abi[PM])},
            "status": "verified stock resources and binary incompatibility; no generic probe or IRQ binding"}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for arg in ("dtb", "reference", "stock", "headers"):
        p.add_argument(arg, type=Path)
    a = p.parse_args()
    try:
        print(json.dumps(verify(a.dtb, a.reference, a.stock, a.headers), indent=2))
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as exc:
        p.exit(2, f"MT6789 pinctrl verification refused: {exc}\n")
