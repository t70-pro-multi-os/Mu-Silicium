#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify T70 Pro's stock sensor path without accessing its SCP or sensors."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

MODULES = {
    "sensorhub": "5bb8c309d0de02750f3d7fa32330a3f2e00dc2f22173bad5eafcc44546a6cf28",
    "scp": "3d45008ef01b7f08a411684f6bf917fc227d70c76f47e16c7af941c94337fadd",
    "hf_manager": "3f807b725eea657643564ebd7cccee47cbdeaf325e44eaa803c6d4245a85e5fb",
    "mtk_tinysys_ipi": "6cfb3cbc315dec7208c3cea0f03a4f7f4a27d3019789de15ce9ec8d49feb106d",
}
SCP = "/soc/scp@10500000"
TX = {"control": [29, 2, 16], "notify": [31, 2, 7]}
RX = {"control": [30, 2, 2, 0], "notify": [32, 2, 7, 0]}
MEM = {
    "data": [2, 0x100000, 0], "super_data": [8, 0x10000, 0],
    "list": [9, 0x1000, 0], "debug": [10, 0x2000, 0],
    "custom_write": [11, 0x100, 0], "custom_read": [12, 0x100, 0],
}


def output(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def symbol_sets(path):
    rows = [line.split() for line in output("readelf", "-W", "-s", path).splitlines()]
    rows = [row for row in rows if len(row) == 8 and row[0].endswith(":")]
    return {row[7] for row in rows if row[6] == "UND"}, {
        row[7] for row in rows if row[6] != "UND"
    }


def verify(dtb, modules):
    node = SCP

    def string(key):
        return output("fdtget", "-t", "s", dtb, node, key)

    def cells(key):
        return [int(x, 16) for x in output("fdtget", "-t", "x", dtb, node, key).split()]

    def table(key, width):
        data = cells(key)
        require(len(data) % width == 0, f"bad {key} row width")
        rows = [data[i:i + width] for i in range(0, len(data), width)]
        require(len({r[0] for r in rows}) == len(rows), f"duplicate {key} ID")
        return {r[0]: r for r in rows}

    require(string("compatible") == "mediatek,scp" and string("status") == "okay",
            "unexpected stock SCP compatible/status")
    regs = cells("reg")
    names = string("reg-names").split()
    require(len(names) * 4 == len(regs) and len(set(names)) == len(names),
            "unexpected SCP register layout")
    banks = dict(zip(names, (regs[i:i + 4] for i in range(0, len(regs), 4))))
    require(banks.get("scp_sram_base") == [0, 0x10500000, 0, 0xc0000] and
            banks.get("mbox2_base") == [0, 0x107fd000, 0, 0x100],
            "unexpected SCP SRAM or sensor mailbox 2 bank")
    irqs = cells("interrupts")
    names = string("interrupt-names").split()
    require(len(names) * 4 == len(irqs) and "mbox2" in names,
            "unexpected SCP interrupt layout")
    n = names.index("mbox2") * 4
    require(irqs[n:n + 4] == [0, 470, 4, 0], "unexpected sensor mailbox 2 IRQ")
    for prop, expected, width in (("send-table", TX, 3), ("recv-table", RX, 4),
                                   ("scp-mem-tbl", MEM, 3)):
        rows = table(prop, width)
        for name, row in expected.items():
            require(rows.get(row[0]) == row, f"unexpected {prop} {name} tuple")
    require(string("scp-mem-key") == "mediatek,reserve-memory-scp_share",
            "unexpected SCP shared-memory key")

    syms = {}
    for mod, digest in MODULES.items():
        path = modules / (mod + ".ko")
        require(hashlib.sha256(path.read_bytes()).hexdigest() == digest,
                f"not original T70 {mod}")
        syms[mod] = symbol_sets(path)
    hub = syms["sensorhub"][0]
    for provider, required in {
        "scp": {"scp_ipidev", "scp_A_register_notify", "scp_get_reserve_mem_virt",
                "scp_get_reserve_mem_phys", "scp_get_reserve_mem_size"},
        "hf_manager": {"hf_device_register", "hf_device_unregister"},
        "mtk_tinysys_ipi": {"mtk_ipi_send", "mtk_ipi_register", "mtk_ipi_unregister"},
    }.items():
        require(required <= (hub & syms[provider][1]), f"sensorhub edge to {provider} missing")
    require(not hub & {"i2c_register_driver", "__spi_register_driver", "iio_device_register",
                        "__iio_device_register", "devm_iio_device_register"},
            "unexpected directly registered sensor bus")
    return {"scp": node, "mailbox": 2, "irq_spi": 470, "tx_rows": TX, "rx_rows": RX,
            "memory_rows": MEM, "sensor_list": "runtime SCP enumeration; fitted chips not proved",
            "interface": "MediaTek tiny IPI, SCP memory and HF manager; not generic IIO"}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("dtb", type=Path)
    p.add_argument("modules", type=Path)
    a = p.parse_args()
    try:
        print(json.dumps(verify(a.dtb, a.modules), indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as e:
        p.exit(2, f"sensorhub audit refused: {e}\n")
