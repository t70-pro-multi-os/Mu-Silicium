#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Mock sysfs only; run with python3 test-probe-touch-linux.py."""
from pathlib import Path
import runpy
import tempfile
import unittest

tool = runpy.run_path(str(Path(__file__).with_name("probe-touch-linux.py")))


class ProbeTest(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.root = Path(td.name)
        self.bus = self.make_device("platform", "11e00000.i2c", tool["BUS"], tool["I2C_DRIVER"])
        self.touch = self.make_device("i2c", "8-0048", tool["TOUCH"], tool["TOUCH_DRIVER"])
        self.provider = self.make_device("platform", "soc:pinctrl", tool["PINCTRL"], tool["PINCTRL_DRIVER"])
        dt = self.root / "firmware/devicetree/base"
        self.pin_node = dt / tool["PINCTRL"]
        self.touch_node = dt / tool["TOUCH"]
        (self.pin_node / "compatible").write_bytes(tool["PINCTRL_COMPATIBLE"])
        (self.pin_node / "phandle").write_bytes((72).to_bytes(4, "big"))
        (self.touch_node / "interrupt-parent").write_bytes((72).to_bytes(4, "big"))
        (self.touch_node / "interrupts").write_bytes(self.cells(9, 2, 9, 0))
        self.irq = self.add_irq(123)
    @staticmethod
    def cells(*values):
        return b"".join(value.to_bytes(4, "big") for value in values)


    def make_device(self, bus, name, dt, driver):
        dev = self.root / "bus" / bus / "devices" / name
        node = self.root / "firmware/devicetree/base" / dt
        drv = self.root / "bus" / bus / "drivers" / driver
        for path in (dev, node, drv):
            path.mkdir(parents=True, exist_ok=True)
        (dev / "of_node").symlink_to(node)
        (dev / "driver").symlink_to(drv)
        return dev

    def add_irq(self, number, **overrides):
        path = self.root / "kernel/irq" / str(number)
        path.mkdir(parents=True)
        data = {"actions": "8-0048", "hwirq": "9", "chip_name": "mt-eint", "type": "edge"}
        for name, value in {**data, **overrides}.items():
            (path / name).write_text(value + "\n")
        return path

    def probe(self):
        return tool["probe"](self.root)

    def test_valid_registration_does_not_claim_touch_events(self):
        result = self.probe()
        self.assertTrue(result["registration_confirmed"])
        self.assertFalse(result["touch_events_tested"])

    def test_bound_modules_without_irq_never_confirm(self):
        (self.irq / "actions").unlink()
        result = self.probe()
        self.assertTrue(result["touch_driver_bound"])
        self.assertFalse(result["registration_confirmed"])

    def test_wrong_chip_type_pin_or_action_are_rejected(self):
        for file, bad, good in (("hwirq", "10", "9"), ("chip_name", "GIC", "mt-eint"),
                                ("type", "level", "edge"), ("actions", "18-0048", "8-0048")):
            with self.subTest(attribute=file):
                (self.irq / file).write_text(bad)
                self.assertFalse(self.probe()["registration_confirmed"])
                (self.irq / file).write_text(good)

    def test_ambiguous_irqs_never_confirm(self):
        self.add_irq(124)
        self.assertFalse(self.probe()["registration_confirmed"])

    def test_wrong_parent_driver_never_confirms(self):
        (self.bus / "driver").unlink()
        d = self.root / "bus/platform/drivers/i2c-mt65xx"
        d.mkdir()
        (self.bus / "driver").symlink_to(d)
        result = self.probe()
        self.assertTrue(result["gpio9_edge_irq_registered"])
        self.assertFalse(result["registration_confirmed"])


    def test_stock_pinctrl_never_confirms_even_with_same_chip_name(self):
        (self.pin_node / "compatible").write_bytes(b"mediatek,mt6789-pinctrl\0")
        driver = self.root / "bus/platform/drivers/mt6789-pinctrl"
        driver.mkdir()
        (self.provider / "driver").unlink()
        (self.provider / "driver").symlink_to(driver)
        result = self.probe()
        self.assertTrue(result["gpio9_edge_irq_registered"])
        self.assertFalse(result["native_eint_bound"])
        self.assertFalse(result["registration_confirmed"])

    def test_missing_pinctrl_driver_never_confirms(self):
        (self.provider / "driver").unlink()
        result = self.probe()
        self.assertTrue(result["gpio9_edge_irq_registered"])
        self.assertFalse(result["native_eint_bound"])

    def test_wrong_interrupt_parent_never_confirms(self):
        (self.touch_node / "interrupt-parent").write_bytes((73).to_bytes(4, "big"))
        result = self.probe()
        self.assertTrue(result["pinctrl_driver_bound"])
        self.assertFalse(result["pinctrl_is_touch_interrupt_parent"])
        self.assertFalse(result["registration_confirmed"])

    def test_rising_or_missing_dt_trigger_never_confirms(self):
        interrupts = self.touch_node / "interrupts"
        interrupts.write_bytes(self.cells(9, 1, 9, 0))
        self.assertFalse(self.probe()["registration_confirmed"])
        interrupts.unlink()
        self.assertFalse(self.probe()["registration_confirmed"])

    def test_extended_interrupt_override_never_confirms(self):
        (self.touch_node / "interrupts-extended").write_bytes(self.cells(72, 9, 1))
        self.assertFalse(self.probe()["registration_confirmed"])

    def test_unrelated_i2c_address_never_matches(self):
        (self.touch / "of_node").unlink()
        other = self.root / "firmware/devicetree/base/soc/i2c@11e01000/other@48"
        other.mkdir(parents=True)
        (self.touch / "of_node").symlink_to(other)
        self.assertFalse(self.probe()["touch_driver_bound"])


if __name__ == "__main__":
    unittest.main()
