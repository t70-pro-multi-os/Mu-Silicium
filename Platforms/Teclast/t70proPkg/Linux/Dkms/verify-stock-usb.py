#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the stock T70 USB device-tree dependencies without enabling them."""
import argparse
import json
from pathlib import Path
import runpy

MUSB = "/soc/usb0@11200000"
EXTCON = "/soc/extcon_usb"
PHY = "/soc/usb-phy@11f40000"
TCPC = "/soc/i2c@1101a000/husb311@4e"
CHARGER = "/soc/i2c@1101a000/sc8989x@6A"
HERE = Path(__file__).resolve().parent
USB_MUSB = "CONFIG_USB_MUSB_MEDIATEK"
USB_PHY = "CONFIG_PHY_MTK_TPHY"
TYPEC_TCPCI = "CONFIG_TYPEC_TCPCI"


def fail_unless(ok, message):
    if not ok:
        raise ValueError(message)


def verify(nodes, cells, config):
    def props(path):
        return nodes[path]["properties"]

    def enabled(node):
        return node.get("status", b"okay\0") in (b"okay\0", b"ok\0")

    def reference(handle, label):
        matches = [path for path, node in nodes.items()
                   if node["properties"].get("phandle") == handle.to_bytes(4, "big")]
        fail_unless(0 < handle < 0xFFFFFFFF and len(matches) == 1,
                    f"{label}: invalid or ambiguous phandle")
        return matches[0]

    musb, extcon, tcpc = (props(path) for path in (MUSB, EXTCON, TCPC))
    fail_unless(all(map(enabled, (musb, extcon, tcpc))), "stock USB device disabled")
    fail_unless(musb["compatible"] == b"mediatek,mt6789-usb20\0"
                and cells(musb["reg"]) == [0, 0x11200000, 0, 0x10000, 0, 0x11F40000, 0, 0x10000]
                and cells(musb["interrupts"]) == [0, 128, 4, 0]
                and musb["interrupt-names"] == b"mc\0"
                and musb["dr_mode"] == b"otg\0" and "usb-role-switch" in musb,
                "unexpected T70 controller resources")
    clock_names = musb["clock-names"].rstrip(b"\0").decode("ascii").split("\0")
    clock_spec = cells(musb["clocks"])
    fail_unless(clock_names == ["sys_clk", "ref_clk", "src_clk"]
                and len(clock_spec) == 6, "unexpected T70 MUSB clocks")
    clocks = {}
    for name, handle, index in zip(clock_names, clock_spec[::2], clock_spec[1::2]):
        path = reference(handle, name)
        fail_unless(cells(props(path)["#clock-cells"]) == [1], name + ": clock cell size")
        clocks[name] = {"provider": path, "index": index}
    fail_unless(list(clocks.values()) == [
        {"provider": "/syscon@10001000", "index": 34},
        {"provider": "/syscon@10000000", "index": 29},
        {"provider": "/syscon@1000C000", "index": 13},
    ], "unexpected T70 MUSB clock supplier route")

    phy_handle, phy_selector = cells(musb["phys"])
    child = reference(phy_handle, "USB PHY")
    fail_unless(child == PHY + "/usb-phy@11f40000"
                and props(PHY)["compatible"] == b"mediatek,generic-tphy-v2\0"
                and cells(props(child)["reg"]) == [0, 0x11F40000, 0, 0x700]
                and cells(props(child)["#phy-cells"]) == [1] and phy_selector == 3
                and enabled(props(PHY)) and enabled(props(child)), "unexpected T70 USB2 PHY")
    efuse, = cells(props(child)["nvmem-cells"])
    fail_unless(props(child)["nvmem-cell-names"] == b"intr_cal\0" and
                reference(efuse, "USB PHY calibration").endswith("/u2_phy_data"),
                "unexpected USB PHY calibration cell")

    endpoints = [path + "/port/endpoint@0" for path in (MUSB, EXTCON)]
    for src, dst in (endpoints, endpoints[::-1]):
        to, = cells(props(src)["remote-endpoint"])
        fail_unless(reference(to, "USB role graph") == dst, "USB role link is not reciprocal")

    fail_unless(extcon["compatible"] == b"mediatek,extcon-usb\0"
                and extcon["tcpc"] == tcpc["tcpc,name"] == b"type_c_port0\0"
                and tcpc["compatible"] == b"hynetek,husb311\0"
                and cells(tcpc["reg"]) == [0x4E], "unexpected stock USB role producer")
    gpio_handles = set()
    signals = {}
    for node, name, expected in (
            (tcpc, "husb311pd,intr_gpio", 13), (extcon, "id-gpios", 10),
            (extcon, "usb_msel-gpios", 99), (extcon, "hub_reset-gpios", 104),
            (extcon, "usb_5v_en-gpios", 4)):
        handle, gpio, flags = cells(node[name])
        fail_unless(gpio == expected and flags == 0, name + ": unexpected GPIO")
        gpio_handles.add(handle)
        signals[name] = gpio
    fail_unless(len(gpio_handles) == 1, "stock USB GPIO providers disagree")
    gpio_path = reference(next(iter(gpio_handles)), "USB GPIOs")
    fail_unless("gpio-controller" in props(gpio_path), "USB signal supplier is not GPIO")
    vbus, = cells(extcon["vbus-supply"])
    charger, = cells(extcon["charger"])
    fail_unless(reference(vbus, "USB VBUS") == CHARGER + "/usb-otg-vbus" and
                reference(charger, "USB charger") == CHARGER, "unexpected T70 VBUS supplier")
    return {
        "controller": {"node": MUSB, "compatible": "mediatek,mt6789-usb20", "spi": 128,
                       "clocks": clocks, "linux_v7_musb_of_match": "mediatek,mtk-musb",
                       "matches_linux_v7_musb": False},
        "phy": {"node": child, "selector": phy_selector,
                "parent_compatible": "mediatek,generic-tphy-v2"},
        "role": {"graph": endpoints, "tcpc": TCPC, "transport": "vendor extcon + TCPC name",
                 "gpio_controller": gpio_path, "gpios": signals,
                 "vbus": CHARGER + "/usb-otg-vbus"},
        "target_config": {name: config.get(name, "n")
                          for name in (USB_MUSB, USB_PHY, TYPEC_TCPCI, "CONFIG_USB_ROLE_SWITCH")},
        "scope": "offline topology; no driver binding, role changes, or VBUS assumed",
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dtb", type=Path)
    ap.add_argument("kernel_config", type=Path)
    args = ap.parse_args()
    try:
        read = runpy.run_path(str(HERE.parent / "platform-modules.py"))
        nodes, _ = read["tree"](args.dtb.read_bytes())
        config = dict(line.split("=", 1) for line in args.kernel_config.read_text().splitlines()
                      if line.startswith("CONFIG_") and "=" in line)
        print(json.dumps(verify(nodes, read["cells"], config), indent=2))
    except (KeyError, OSError, ValueError) as exc:
        ap.exit(2, f"USB topology check refused: {exc}\n")


if __name__ == "__main__":
    main()
