#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the retained touch binding offline against the extracted stock DT."""
import argparse
from pathlib import Path
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
BUS = "/soc/i2c@11e00000"
TOUCH = BUS + "/himax_ts@48"
PINCTRL = "/soc/pinctrl"
OLD = b"himax,hxcommon\0"
NEW = b"teclast,t70pro-hx83132-retained\0"
NEW_BUS = b"teclast,t70pro-mt6983-i2c\0"
NEW_PINCTRL = b"teclast,t70pro-mt6789-pinctrl\0"
EINT = "/soc/apirq@1000b000"
READER = runpy.run_path(str(HERE.parent / "platform-modules.py"))
CELLS = READER["cells"]


def require(ok, message):
    if not ok:
        raise ValueError(message)


def inventory(path):
    nodes, _ = READER["tree"](path.read_bytes())
    return {name: node["properties"] for name, node in nodes.items()}


def verify(stock, include_parent=False, include_pinctrl=False):
    before = inventory(stock)
    child = before[TOUCH]
    parent = before[BUS]
    pinctrl = before[PINCTRL]
    require(before["/"]["compatible"] == b"mediatek,MT6789\0", "unexpected stock SoC")
    require(child["compatible"] == OLD and CELLS(child["reg"]) == [0x48],
            "unexpected stock touchscreen")
    require(parent["compatible"] == b"mediatek,mt6983-i2c\0"
            and CELLS(parent["reg"]) == [0, 0x11E00000, 0, 0x1000, 0, 0x10217080, 0, 0x80],
            "unexpected stock I2C resources")
    for path in ("/soc", BUS, TOUCH, PINCTRL, "/soc/apirq@1000b000",
                 "/syscon@11E02000", "/syscon@10001000"):
        require(before[path].get("status", b"okay\0") in (b"okay\0", b"ok\0"),
                f"stock touch path unavailable: {path}")
    main = before["/syscon@11E02000"]
    dma = before["/syscon@10001000"]
    main_phandle, = CELLS(main["phandle"])
    dma_phandle, = CELLS(dma["phandle"])
    for provider, phandle in ((main, main_phandle), (dma, dma_phandle)):
        require(0 < phandle < 0xffffffff
                and sum(n.get("phandle") == provider["phandle"] for n in before.values()) == 1
                and CELLS(provider["#clock-cells"]) == [1],
                "invalid stock touch clock provider")
    require(main["compatible"].split(b"\0")[0] == b"mediatek,mt6789-imp_iic_wrap_w"
            and dma["compatible"].split(b"\0")[0] == b"mediatek,mt6789-infracfg_ao"
            and CELLS(parent["clocks"]) == [main_phandle, 0, dma_phandle, 0x3D]
            and parent["clock-names"] == b"main\0dma\0"
            and CELLS(parent["clock-frequency"]) == [400000]
            and CELLS(parent["clock-div"]) == [1]
            and CELLS(parent["interrupts"]) == [0, 144, 4, 0],
            "unexpected stock touch bus clock or IRQ")
    require(pinctrl["compatible"] == b"mediatek,mt6789-pinctrl\0"
            and CELLS(pinctrl["#interrupt-cells"]) == [2]
            and child["interrupt-parent"] == pinctrl["phandle"]
            and "interrupt-controller" in pinctrl
            and CELLS(child["interrupts"]) == [9, 2, 9, 0],
            "unexpected stock touch interrupt provider or route")
    require(CELLS(child["himax,panel-coords"]) == [0, 1400, 0, 2240],
            "unexpected stock touch coordinate bounds")

    eint = before[EINT]
    require(pinctrl["mediatek,eint"] == eint["phandle"]
            and eint["compatible"] == b"mediatek,mt6983-eint\0"
            and CELLS(eint["reg"]) == [0, 0x1000B000, 0, 0x1000]
            and CELLS(eint["interrupts"]) == [0, 244, 4, 0]
            and CELLS(eint["mediatek,total-pin-number"]) == [160],
            "unexpected stock EINT resource or upstream interrupt")

    with tempfile.TemporaryDirectory(prefix="t70-touch-dt-") as tempdir:
        temp = Path(tempdir)
        result = temp / "candidate.dtb"
        sources = ["Himax/t70pro-hx83132-retained.dtso"]
        if include_parent:
            sources.insert(0, "I2c/t70pro-touch-parent.dtso")
        if include_pinctrl:
            sources.insert(0, "Pinctrl/t70pro-paris-legacy-eint.dtso")
        overlays = []
        for source in sources:
            overlay = temp / Path(source).with_suffix(".dtbo").name
            # Overlay pseudo-nodes do not inherit the target's unit-address name.
            spec = ["-Wno-unit_address_vs_reg"] if source.startswith("Pinctrl/") else []
            subprocess.run(["dtc", "-@", *spec, "-I", "dts", "-O", "dtb", "-o", overlay,
                            HERE / source], check=True)
            overlays.append(overlay)
        subprocess.run(["fdtoverlay", "-i", stock, "-o", result, *overlays], check=True)
        after = inventory(result)
    expected = dict(before)
    expected[TOUCH] = {**child, "compatible": NEW}
    if include_parent:
        expected[BUS] = {**parent, "compatible": NEW_BUS}
    if include_pinctrl:
        gic = before["/interrupt-controller"]
        require(gic["phandle"] == before["/"]["interrupt-parent"]
                and CELLS(gic["#interrupt-cells"]) == [4],
                "unexpected native EINT interrupt parent")
        expected[PINCTRL] = {**pinctrl, "compatible": NEW_PINCTRL,
                             "reg": pinctrl["reg"] + eint["reg"],
                             "reg-names": pinctrl["reg-names"] + b"eint\0",
                             "interrupt-parent": gic["phandle"],
                             "interrupts": eint["interrupts"]}
        expected[EINT] = {**eint, "status": b"disabled\0"}
    require(after == expected, "overlay changed properties outside the selected device contracts")
    require(after[TOUCH]["interrupt-parent"] == after[PINCTRL]["phandle"]
            and CELLS(after[TOUCH]["interrupts"]) == [9, 2, 9, 0],
            "candidate lost the GPIO9 route")
    scope = "parent + 0x48 child" if include_parent else "0x48 child"
    if include_pinctrl:
        print(f"PASS: {scope} + Paris; GPIO9 → unchanged pinctrl phandle → GIC SPI244")
    else:
        print(f"PASS: {scope} bindings only; GPIO9, EINT and all provider resources unchanged")
    print("This validates the DT transform, not I2C/pinctrl availability or device operation.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_dtb", type=Path)
    parser.add_argument("--include-parent", action="store_true")
    parser.add_argument("--include-pinctrl", action="store_true")
    args = parser.parse_args()
    try:
        verify(args.stock_dtb, args.include_parent, args.include_pinctrl)
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"Touch DT verification refused: {error}\n")
