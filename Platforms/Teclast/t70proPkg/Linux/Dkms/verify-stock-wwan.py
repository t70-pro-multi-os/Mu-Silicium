#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the T70's genuine LTE control/data paths without running the modem."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess

common = runpy.run_path(str(Path(__file__).with_name("verify-stock-sensorhub.py")))
output, require, symbol_sets = (common[name] for name in ("output", "require", "symbol_sets"))
MODULES = {
    "ccci_dpmaif": "48439cbc6214ca6d0f8aabee7073aa5342f8e2c115bf65d5f69dfc47a8295677",
    "ccci_ccif": "c5b6e12b116e5e3ff52e5d8ef94e7426dab1201a1e452cf796a8dea9197a36c6",
    "ccci_md_all": "c7a0435c27c27d1efdd5076ee3d656b383141c6252417e1975ed6d0550e0762b",
    "ccmni": "a70691e767aaea87043e584bb534bf59141c0db3dc0e45b00ed69a5b329752c3",
}

DPMA = "/soc/dpmaif@10014000"
CCIF = "/soc/ccifdriver@10209000"


def verify(dtb, modules):
    def get(kind, node, prop):
        return output("fdtget", "-t", kind, dtb, node, prop)

    def cells(node, prop):
        return [int(value, 16) for value in get("x", node, prop).split()]

    infra = cells("/syscon@10001000", "phandle")
    require(len(infra) == 1, "invalid infracfg phandle")
    ph = infra[0]
    require(get("s", DPMA, "compatible") == "mediatek,dpmaif"
            and cells(DPMA, "mediatek,dpmaif-ver") == [1]
            and cells(DPMA, "mediatek,dpmaif-cap") == [0x42]
            and cells(DPMA, "mediatek,plat-info") == [6789]
            and cells(DPMA, "reg") == [0, 0x10014000, 0, 0x1000,
                                      0, 0x1022d000, 0, 0x1000,
                                      0, 0x1022c000, 0, 0x1000,
                                      0, 0x1022e000, 0, 0x1000]
            and cells(DPMA, "interrupts") == [0, 241, 4, 0]
            and cells(DPMA, "clocks") == [ph, 55]
            and get("s", DPMA, "clock-names") == "infra-dpmaif-clk"
            and cells(DPMA, "dpmaif-infracfg") == infra, "unexpected MT6789 DPMAIF v1 resources")

    ccif_indices = [30, 33, 25, 26, 57, 51]
    require(get("s", CCIF, "compatible") == "mediatek,ccci_ccif"
            and cells(CCIF, "reg") == [0, 0x10209000, 0, 0x1000, 0, 0x1020a000, 0, 0x1000]
            and cells(CCIF, "interrupts") == [0, 226, 4, 0, 0, 227, 4, 0]
            and cells(CCIF, "mediatek,sram-size") == [0x200]
            and cells(CCIF, "clocks") == [v for idx in ccif_indices for v in (ph, idx)]
            and get("s", CCIF, "clock-names").split() == [
                "infra-ccif-ap", "infra-ccif-md", "infra-ccif1-ap",
                "infra-ccif1-md", "infra-ccif4-md", "infra-ccif5-md"],
            "unexpected CCIF control resources")

    reservations = {}
    for name, size in (("cache", 0x50000), ("nocache", 0x20000)):
        path = f"/reserved-memory/ccci-dpmaif-{name}-memory"
        require(get("s", path, "compatible") == f"mediatek,dpmaif-resv-{name}-mem"
                and cells(path, "size") == [0, size]
                and cells(path, "alignment") == [0, 0x10000]
                and cells(path, "alloc-ranges") == [0, 0x40000000, 0, 0xf0000000]
                and "reg" not in output("fdtget", "-p", dtb, path).splitlines(),
                f"unexpected DPMAIF {name} reservation")
        reservations[name] = size

    symbols = {}
    for mod, digest in MODULES.items():
        file = modules / (mod + ".ko")
        require(hashlib.sha256(file.read_bytes()).hexdigest() == digest, f"not the stock {mod}")
        symbols[mod] = symbol_sets(file)
    data, data_def = symbols["ccci_dpmaif"]
    ctrl, ctrl_def = symbols["ccci_ccif"]
    core, core_def = symbols["ccci_md_all"]
    net, net_def = symbols["ccmni"]
    require({"__platform_driver_register", "ccmni_ops", "ccci_hif_register"} <= data
            and {"dpmaif_probe", "ccci_dpmaif_drv1_init", "dpmaif_txq_set_budget_v1_mt6789"} <= data_def
            and "__pci_register_driver" not in data, "DPMAIF platform/CCMNI linkage missing")
    require({"__platform_driver_register", "ccci_hif_register", "ccci_port_recv_skb"} <= ctrl
            and "ccci_hif_ccif_probe" in ctrl_def
            and {"ccci_hif_register", "ccci_port_recv_skb"} <= core_def, "CCIF/CCCI linkage missing")
    require("ccmni_ops" in core and {"register_netdev", "napi_gro_receive"} <= net
            and {"ccmni_ops", "ccmni_start_xmit"} <= net_def, "CCMNI netdev linkage missing")
    return {"lte": {"platform": "MT6789", "data": DPMA, "dpmaif_version": 1,
                    "ccif_control": CCIF, "network_interface": "CCMNI",
                    "dpmaif_irq_spi": 241, "ccif_irq_spis": [226, 227],
                    "dpmaif_dynamic_reservation_sizes": reservations},
            "scope": "stock DT and module contracts; no live modem state or binding"}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("dtb", type=Path)
    p.add_argument("modules", type=Path)
    a = p.parse_args()
    try:
        print(json.dumps(verify(a.dtb, a.modules), indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        p.exit(2, f"WWAN audit refused: {e}\n")
