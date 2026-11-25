#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the generic Paris DT transform without probing hardware."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
shared = runpy.run_path(str(HERE.parent / "verify-mt6789-pinctrl.py"))
inventory, cells, require, pin_path, eint_path = (
    shared[key] for key in ("inventory", "cells", "require", "PIN", "EINT"))
MATCH = b"teclast,t70pro-mt6789-pinctrl\0"


def verify(stock, overlay):
    before = inventory(stock)
    pin, eint = before[pin_path], before[eint_path]
    gic = before["/interrupt-controller"]
    require(gic["phandle"] == before["/"]["interrupt-parent"]
            and cells(gic["#interrupt-cells"]) == [4], "unexpected parent interrupt cells")
    require(cells(before["/soc"]["#address-cells"]) == [2]
            and cells(before["/soc"]["#size-cells"]) == [2]
            and "interrupt-parent" not in before["/soc"]
            and "interrupt-parent" not in pin and "interrupt-parent" not in eint,
            "unexpected inherited pinctrl address or interrupt-parent cells")
    require(pin["compatible"] == b"mediatek,mt6789-pinctrl\0"
            and pin["mediatek,eint"] == eint["phandle"]
            and cells(eint["reg"]) == [0, 0x1000b000, 0, 0x1000]
            and cells(eint["interrupts"]) == [0, 244, 4, 0]
            and len(cells(pin["reg"])) == 7 * 4 and pin["reg-names"].count(b"\0") == 7
            and "interrupts" not in pin, "unexpected stock resource graph")

    with tempfile.TemporaryDirectory(prefix="t70-paris-") as dirname:
        d = Path(dirname)
        compiled, merged = d / "source.dtbo", d / "generic.dtb"
        subprocess.run(["dtc", "-@", "-Wno-unit_address_vs_reg", "-I", "dts", "-O", "dtb",
                        "-o", compiled, overlay],
                       check=True)
        subprocess.run(["fdtoverlay", "-i", stock, "-o", merged, compiled], check=True)
        after = inventory(merged)
    expected = dict(before)
    expected[pin_path] = {**pin, "compatible": MATCH, "reg": pin["reg"] + eint["reg"],
                          "reg-names": pin["reg-names"] + b"eint\0",
                          "interrupt-parent": gic["phandle"],
                          "interrupts": eint["interrupts"]}
    expected[eint_path] = {**eint, "status": b"disabled\0"}
    require(expected == after, "candidate changed resources or other stock properties")
    require(after[pin_path]["phandle"] == pin["phandle"], "consumer phandle changed")
    print("Generic Paris candidate: 7 unchanged GPIO banks + existing EINT bank; GIC SPI244 preserved.")
    print("Legacy resource node disabled; consumer GPIO/IRQ phandles unchanged. Native code still required.")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("dtb", type=Path)
    p.add_argument("--overlay", type=Path, default=HERE / "t70pro-paris-legacy-eint.dtso")
    a = p.parse_args()
    try:
        verify(a.dtb, a.overlay)
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        p.exit(2, f"Paris DT candidate refused: {error}\n")
