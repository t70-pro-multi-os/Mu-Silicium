#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Inventory the T70 Pro's pinned display DT without touching hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

GPU = "/soc/mali@13000000"
DSI = "/soc/dsi@14013000"
PHY = "/soc/mipi_tx_config@11f60000"
PANELS = {
    "panel1@0": ("cyx,cv101wu_lh1038b", 0),
    "panel2@0": ("truly,td4330,cmd", 1),
}
LINUX_V7 = {
    "panfrost": "https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/panfrost/panfrost_drv.c",
    "panthor": "https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/panthor/panthor_drv.c",
    "dsi": "https://github.com/torvalds/linux/blob/v7.0/drivers/gpu/drm/mediatek/mtk_dsi.c",
    "phy": "https://github.com/torvalds/linux/blob/v7.0/drivers/phy/mediatek/phy-mtk-mipi-dsi.c",
}


CYX_SHA256 = "28a4c73d1929f1bf53891c6a92340fe8b9904d7c3bda89f21be63c28e45056f3"


def stock_panel_module(path, symvers):
    blob = path.read_bytes()
    if hashlib.sha256(blob).hexdigest() != CYX_SHA256:
        raise ValueError("not the audited stock T70 CYX panel module")
    aliases = subprocess.check_output(["modinfo", "-F", "alias", str(path)], text=True).splitlines()
    if "of:N*T*Ccyx,cv101wu_lh1038b" not in aliases:
        raise ValueError("stock module did not advertise the CYX OF identity")
    # File offsets come from the digest-checked ELF's rodata and default_mode.
    clock, hd, hs, he, ht, _, vd, vs, ve, vt, _ = struct.unpack_from("<I10H", blob, 0x12528 + 0x1d0)
    report = {
        "sha256": CYX_SHA256,
        "of_match": "cyx,cv101wu_lh1038b",
        "default_mode": {"clock_khz": clock, "h": [hd, hs, he, ht], "v": [vd, vs, ve, vt],
                         "refresh_hz": round(clock * 1000 / (ht * vt), 3)},
        "scope": "driver shipped in stock vendor boot; not evidence of live binding",
    }
    if symvers is not None:
        target = {cols[1] for row in symvers.read_text().splitlines()
                  if len(cols := row.split()) >= 4}
        undefined = subprocess.check_output(["nm", "-u", str(path)], text=True).splitlines()
        strong = {cols[1] for row in undefined if len(cols := row.split()) == 2 and cols[0] == "U"}
        report["strong_imports_missing_from_target_exports"] = sorted(strong - target)
    return report


def check(dtb, config):
    def prop(node, key, kind="s"):
        return subprocess.check_output(
            ["fdtget", "-t", kind, str(dtb), node, key], text=True).strip().split()

    def expect(match, name):
        if not match:
            raise ValueError("not the audited T70 display resource: " + name)

    g, d, phy = (prop(path, "compatible") for path in (GPU, DSI, PHY))
    expect(g == ["mediatek,mali", "arm,mali-valhall"], "GPU compatible")
    expect(d == ["mediatek,dsi0", "mediatek,mt6789-dsi"], "DSI compatible")
    expect(phy == ["mediatek,mipi_tx_config0", "mediatek,mt6789-mipi-tx"], "PHY compatible")
    expect(prop(DSI, "phys", "x") == prop(PHY, "phandle", "x")
           and prop(PHY, "#phy-cells", "x") == ["0"], "DSI PHY phandle")
    expect(prop(DSI, "interrupts", "x") == ["0", "12d", "4", "0"], "DSI IRQ")
    irq_names = prop(GPU, "interrupt-names")
    expect(irq_names == ["GPU", "MMU", "JOB", "EVENT", "PWR"], "GPU IRQ names")
    panel_nodes = subprocess.check_output(["fdtget", "-l", str(dtb), DSI], text=True).split()
    expect(set(x for x in panel_nodes if x.startswith("panel")) == PANELS.keys(),
           "DSI candidate panels")
    for node, (compat, channel) in PANELS.items():
        expect(prop(DSI + "/" + node, "compatible") == [compat]
               and prop(DSI + "/" + node, "reg", "x") == [f"{channel:x}"], node)
    options = dict(row.split("=", 1) for row in config.splitlines()
                   if row.startswith("CONFIG_DRM_") and "=" in row)
    return {
        "kernel_config": {key: options.get(key, "unset") for key in
                          ("CONFIG_DRM_MEDIATEK", "CONFIG_DRM_PANFROST", "CONFIG_DRM_PANTHOR")},
        "stock_gpu": {"compatible": g, "irq_names": irq_names,
                      "vendor_mali_family_is_not_an_upstream_of_match": True},
        "stock_display": {"dsi": d, "dsi_irq_spi": 301, "phy": phy,
                          "panel_candidates": {node: {"compatible": compat, "reg": channel}
                                               for node, (compat, channel) in PANELS.items()},
                          "present_panel": "not established by DT"},
        "scope": "read-only stock inventory, not a candidate DT or enablement",
        "linux_v7_source": LINUX_V7,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_dtb", type=Path)
    parser.add_argument("kernel_config", type=Path)
    parser.add_argument("--stock-cyx-module", type=Path)
    parser.add_argument("--target-symvers", type=Path)
    args = parser.parse_args()
    try:
        report = check(args.stock_dtb, args.kernel_config.read_text())
        if args.target_symvers and not args.stock_cyx_module:
            raise ValueError("--target-symvers requires --stock-cyx-module")
        if args.stock_cyx_module:
            report["stock_cyx_module"] = stock_panel_module(args.stock_cyx_module, args.target_symvers)
        print(json.dumps(report, indent=2))
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(2, f"display verification refused: {error}\n")
