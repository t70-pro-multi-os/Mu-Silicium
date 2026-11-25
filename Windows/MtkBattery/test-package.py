#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the ACPI PnP id and Windows battery miniport INF agree."""
from pathlib import Path
import re

battery = Path(__file__).resolve().parent
root = battery.parents[1]
inf = (battery / 'MtkBattery.inf').read_text()
asl = (root / 'Silicium-ACPI/Platforms/Teclast/t70pro/DSDT.asl').read_text()
driver = (battery / 'Driver.c').read_text()

checks = {
    'battery_setup_class': 'Class=Battery' in inf and
        'ClassGuid={72631E54-78A4-11D0-BCF7-00AA00B7B32A}' in inf,
    'acpi_hid_binding': 'ACPI\\HANA679B' in inf and
        re.search(r'Device\s*\(BAT0\).*?Name\s*\(_HID,\s*"HANA679B"\)',
                  asl, re.S) is not None,
    'device_present_method': re.search(
        r'Device\s*\(BAT0\).*?Method\s*\(_STA,\s*0,\s*NotSerialized\).*?Return\s*\(0x0F\)',
        asl, re.S) is not None,
    'battery_class_lifecycle': all(name in driver for name in (
        'BatteryClassInitializeDevice', 'BatteryClassUnload',
        'BatteryClassIoctl', 'BatteryClassSystemControl')) and
    'Info.CycleCount = 0;' in driver,
    'wdm_preprocess_paths': all(name in driver for name in (
        'BatteryPreprocessDeviceControl', 'BatteryPreprocessSystemControl')),
}
failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise SystemExit('battery package binding failed: ' + ', '.join(failed))
print('MtkBattery package bindings passed: ' + ', '.join(checks))
