#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify physical T70 storage controller/supplier contracts without probing disks."""
import argparse
import json
from pathlib import Path
import runpy
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
FDT = runpy.run_path(str(HERE.parent / "platform-modules.py"))
CELLS = FDT["cells"]
UFS = "/soc/ufshci@11270000"
EMMC = "/soc/mmc@11230000"
SD = "/soc/mmc@11240000"
TOP = "/syscon@10000000"
INFRA = "/syscon@10001000"
RESET = INFRA + "/reset-controller"
PMIC_WRAP = "/soc/pwrap@10026000"
KERNEL_SYMBOLS = (
    "SCSI_UFS_MEDIATEK", "RESET_TI_SYSCON", "MTK_PMIC_WRAP",
    "MFD_MT6397", "REGULATOR_MT6358", "COMMON_CLK_MT6789", "PINCTRL_MT6789",
    "MMC_MTK",
)


def require(ok, msg):
    if not ok:
        raise ValueError(msg)


def verify(data):
    nodes, _ = FDT["tree"](data)
    p = {key: node["properties"] for key, node in nodes.items()}

    def resolve(value):
        matches = [path for path, node in p.items() if node.get("phandle") == value]
        require(len(matches) == 1 and 0 < CELLS(value)[0] < 0xffffffff, "ambiguous provider phandle")
        return matches[0]

    def specifiers(node, prop, count_prop, width):
        data = node[prop]
        values = CELLS(data)
        require(len(values) % width == 0, f"invalid specifier width: {prop}")
        result = []
        for idx in range(0, len(values), width):
            dest = resolve(data[idx * 4:idx * 4 + 4])
            require(CELLS(p[dest][count_prop]) == [width - 1], f"bad supplier cell count: {dest}")
            result.append((dest, *values[idx + 1:idx + width]))
        return result

    def string_list(data):
        require(data.endswith(b"\0"), "DT string not terminated")
        return data[:-1].decode("ascii").split("\0")

    def clocks(node, names, contract):
        require(string_list(node["clock-names"]) == names
                and specifiers(node, "clocks", "#clock-cells", 2) == contract,
                "wrong clock name/index/order")
        return dict(zip(names, contract))

    def supply(node, key, label):
        target = resolve(node[key])
        require("/mt6366/" in target
                and p[target]["regulator-name"] == (label + "\0").encode(),
                f"unexpected {key} regulator")
        return target

    u, e, d = p[UFS], p[EMMC], p[SD]
    require(p["/"]["compatible"] == b"mediatek,MT6789\0", "wrong SoC")
    for path, name in ((TOP, b"mediatek,mt6789-topckgen"), (INFRA, b"mediatek,mt6789-infracfg_ao")):
        require(p[path]["compatible"].split(b"\0", 1)[0] == name, "wrong clock supplier")
    require(u["compatible"] == b"mediatek,mt8183-ufshci\0"
            and CELLS(u["reg"]) == [0, 0x11270000, 0, 0x2300]
            and CELLS(u["interrupts"]) == [0, 137, 4, 0]
            and "mediatek,ufs-disable-mcq" in u, "wrong UFS host resource")
    require("phys" not in u, "expected stock UFS node without phys")
    uc = clocks(u, ["ufs", "ufs_mp", "unipro_tick", "unipro_sys", "ufs_aes", "ufs_ck"],
                [(INFRA, 47), (INFRA, 43), (INFRA, 42), (INFRA, 41), (INFRA, 48), (TOP, 39)])
    require(string_list(u["reset-names"]) == ["hci_rst", "unipro_rst", "crypto_rst"]
            and specifiers(u, "resets", "#reset-cells", 2) == [(RESET, 1), (RESET, 2), (RESET, 3)]
            and p[RESET]["compatible"] == b"ti,syscon-reset\0", "wrong UFS reset supplier")
    reset_words = CELLS(p[RESET]["ti,reset-bits"])
    require(len(reset_words) == 5 * 7, "malformed syscon reset table")
    reset_entries = [reset_words[i:i+7] for i in range(0, len(reset_words), 7)]
    expected_reset = [[0x130, 15, 0x134, 15, 0, 0, 0x1c],
                      [0x140, 7, 0x144, 7, 0, 0, 0x1c],
                      [0x150, 21, 0x154, 21, 0, 0, 0x1c]]
    require(reset_entries[1:4] == expected_reset
            and b"syscon" in p[INFRA]["compatible"].split(b"\0")
            and b"simple-mfd" in p[INFRA]["compatible"].split(b"\0"),
            "wrong UFS reset map or parent")
    require(p[PMIC_WRAP]["compatible"] == b"mediatek,mt6789-pwrap\0"
            and p[PMIC_WRAP + "/mt6366"]["compatible"] == b"mediatek,mt6366\0",
            "wrong stock UFS PMIC transport")
    require(e["compatible"] == d["compatible"] == b"mediatek,mt6789-mmc\0",
            "wrong MMC variant")
    require(CELLS(e["host-index"]) == [0] and CELLS(e["bus-width"]) == [8]
            and all(flag in e for flag in ("no-sd", "non-removable"))
            and CELLS(d["host-index"]) == [1] and CELLS(d["bus-width"]) == [4]
            and "no-mmc" in d and "non-removable" not in d, "wrong MMC/SD role")
    require("supports-cqe" in e and "supports-cqe" not in d,
            "CQE must be described only on the internal MMC node")
    require(CELLS(d["reg"]) == [0, 0x11240000, 0, 0x1000, 0, 0x11ef0000, 0, 0x1000]
            and CELLS(d["interrupts"]) == [0, 135, 4, 0], "wrong external SD resource")
    dc = clocks(d, ["source", "hclk", "source_cg"], [(TOP, 21), (INFRA, 21), (INFRA, 28)])
    gpio = specifiers(d, "cd-gpios", "#gpio-cells", 3)
    require(gpio == [("/soc/pinctrl", 7, 0)] and "gpio-controller" in p[gpio[0][0]],
            "wrong stock SD card detect")
    pin_states = [resolve(d[f"pinctrl-{i}"]) for i in range(3)]
    require(string_list(d["pinctrl-names"]) == ["default", "state_uhs", "pull_down"]
            and len(set(pin_states)) == 3 and all(s.startswith("/soc/pinctrl/") for s in pin_states),
            "wrong stock SD pinctrl states")
    return {
        "ufs": {"node": UFS, "compatible": "mediatek,mt8183-ufshci",
                "clock_specs": uc, "vcc": supply(u, "vcc-supply", "vemc"),
                "phys_present": False, "legacy_irq_spi": 137,
                "reset_provider": p[RESET]["compatible"][:-1].decode(),
                "reset_ids": {name: i for i, name in enumerate(
                    ("hci_rst", "unipro_rst", "crypto_rst"), start=1)},
                "reset_status_available": False,
                "pmic_transport": p[PMIC_WRAP]["compatible"][:-1].decode()},
        "internal_mmc_description": {"node": EMMC, "non_removable": True, "supports_cqe": True,
                                     "note": "DT node is not proof a device is populated"},
        "external_sd": {"node": SD, "clock_specs": dc, "card_detect": gpio[0],
                        "pin_states": pin_states, "irq_spi": 135, "supports_cqe": False,
                        "vmmc": supply(d, "vmmc-supply", "mt6358_vmch_eint_low"),
                        "vqmmc": supply(d, "vqmmc-supply", "vmc")},
    }


def kernel_config(path):
    options = {}
    for line in path.read_text().splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, val = line[7:].split("=", 1)
            if key in KERNEL_SYMBOLS:
                options[key] = val
    return {name: options.get(name, "absent_or_unconfigured") for name in KERNEL_SYMBOLS}


def verify_overlay(stock):
    def inventory(path):
        nodes, _ = FDT["tree"](path.read_bytes())
        return {key: value["properties"] for key, value in nodes.items()}

    expected = inventory(stock)
    expected[SD] = {**expected[SD], "compatible": b"teclast,t70pro-mt6789-ext-sd\0",
                    "status": b"disabled\0"}
    with tempfile.TemporaryDirectory(prefix="t70-sd-staging-") as name:
        temp = Path(name)
        overlay, result = temp / "sd.dtbo", temp / "out.dtb"
        subprocess.run(["dtc", "-@", "-I", "dts", "-O", "dtb", "-o", overlay,
                        HERE / "Sd/t70pro-external-sd-staging.dtso"], check=True)
        subprocess.run(["fdtoverlay", "-i", stock, "-o", result, overlay], check=True)
        require(inventory(result) == expected, "overlay changed more than external SD binding/status")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stock_dtb", type=Path)
    ap.add_argument("--check-staging", action="store_true")
    ap.add_argument("--kernel-config", type=Path)
    args = ap.parse_args()
    try:
        report = verify(args.stock_dtb.read_bytes())
        if args.check_staging:
            verify_overlay(args.stock_dtb)
            report["staging"] = "only external SD compatible/status changed; SD remains disabled"
        if args.kernel_config:
            report["linux_config"] = kernel_config(args.kernel_config)
        print(json.dumps(report, indent=2))
    except (KeyError, OSError, ValueError, subprocess.CalledProcessError) as error:
        ap.exit(2, f"T70 storage DT refused: {error}\n")
