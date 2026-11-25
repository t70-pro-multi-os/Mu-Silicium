#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Read Linux T70 I2C and GPIO9 IRQ registration; never open an input device."""
import argparse
import json
from pathlib import Path
import re

BUS = "soc/i2c@11e00000"
TOUCH = BUS + "/himax_ts@48"
PINCTRL = "soc/pinctrl"
I2C_DRIVER = "t70-mt6983-i2c"
TOUCH_DRIVER = "t70_hx83132"
PINCTRL_DRIVER = "t70-mt6789-pinctrl"
PINCTRL_COMPATIBLE = b"teclast,t70pro-mt6789-pinctrl\0"


def linked(path):
    try:
        return path.resolve(strict=True) if path.is_symlink() else None
    except (OSError, RuntimeError):
        return None


def devices(root, bus, node, pattern):
    return sorted(p for p in (root / "bus" / bus / "devices").glob("*")
                  if re.fullmatch(pattern, p.name) and linked(p / "of_node") == node)


def bound(root, bus, device, driver):
    if device is None:
        return False
    target = root / "bus" / bus / "drivers" / driver
    return linked(device / "driver") == target


def read_attr(path):
    if path.is_symlink() or not path.is_file():
        return None
    try:
        with path.open("r", encoding="ascii") as file:
            s = file.read(4097)
            return s.strip() if len(s) <= 4096 else None
    except (OSError, UnicodeError):
        return None


def read_of(path, limit=128):
    if path.is_symlink() or not path.is_file():
        return None
    try:
        with path.open("rb") as file:
            data = file.read(limit + 1)
            return data if len(data) <= limit else None
    except OSError:
        return None



def probe(root):
    root = root.resolve(strict=True)
    dt = root / "firmware/devicetree/base"
    buses = devices(root, "platform", dt / BUS, r"[A-Za-z0-9_.:@-]+")
    controllers = devices(root, "platform", dt / PINCTRL, r"[A-Za-z0-9_.:@-]+")
    clients = devices(root, "i2c", dt / TOUCH, r"[0-9]+-0048")
    bus = buses[0] if len(buses) == 1 else None
    client = clients[0] if len(clients) == 1 else None
    controller = controllers[0] if len(controllers) == 1 else None
    provider_bound = bound(root, "platform", controller, PINCTRL_DRIVER)
    provider_dt_matches = read_of(dt / PINCTRL / "compatible") == PINCTRL_COMPATIBLE
    phandle = read_of(dt / PINCTRL / "phandle")
    irq_parent_matches = (phandle is not None and len(phandle) == 4 and
                          0 < int.from_bytes(phandle, "big") < 0xffffffff and
                          read_of(dt / TOUCH / "interrupt-parent") == phandle)
    native_eint_bound = provider_bound and provider_dt_matches and irq_parent_matches
    touch_node = dt / TOUCH
    # IRQ sysfs reports "edge" for both polarities; check the audited DT tuple.
    # An interrupts-extended property can override interrupts/interrupt-parent.
    extended = touch_node / "interrupts-extended"
    irq_dt_matches = (not extended.exists() and not extended.is_symlink() and
                      read_of(touch_node / "interrupts") ==
                      bytes.fromhex("00000009 00000002 00000009 00000000"))
    bus_bound = bound(root, "platform", bus, I2C_DRIVER)
    touch_bound = bound(root, "i2c", client, TOUCH_DRIVER)
    linked_irqs = []
    if touch_bound:
        for path in (root / "kernel/irq").glob("*"):
            if not re.fullmatch(r"[0-9]+", path.name):
                continue
            actions = read_attr(path / "actions")
            if not actions or client.name not in actions.split(","):
                continue
            linked_irqs.append({
                "irq": int(path.name),
                "hwirq": read_attr(path / "hwirq"),
                "chip": read_attr(path / "chip_name"),
                "type": read_attr(path / "type"),
            })
    linked_irqs.sort(key=lambda entry: entry["irq"])
    irq_registered = len(linked_irqs) == 1 and all(
        linked_irqs[0][key] == value
        for key, value in (("hwirq", "9"), ("chip", "mt-eint"), ("type", "edge")))
    return {
        "platform_matches": [p.name for p in buses],
        "i2c_matches": [p.name for p in clients],
        "pinctrl_matches": [p.name for p in controllers],
        "pinctrl_driver_bound": provider_bound,
        "pinctrl_generic_binding": provider_dt_matches,
        "pinctrl_is_touch_interrupt_parent": irq_parent_matches,
        "native_eint_bound": native_eint_bound,
        "i2c_driver_bound": bus_bound,
        "touch_driver_bound": touch_bound,
        "touch_irq_attributes_available": (root / "kernel/irq").is_dir(),
        "touch_irq_candidates": linked_irqs,
        "gpio9_edge_irq_registered": irq_registered,
        "gpio9_falling_edge_dt_matches": irq_dt_matches,
        "registration_confirmed": (bus_bound and touch_bound and native_eint_bound
                                   and irq_registered and irq_dt_matches),
        "touch_events_tested": False,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sysfs", type=Path, default=Path("/sys"))
    args = parser.parse_args()
    try:
        print(json.dumps(probe(args.sysfs), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(2, f"Touch registration probe failed: {error}\n")
