#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check tracked Windows storage and WinPE boot service settings."""
from pathlib import Path

root = Path(__file__).resolve().parent
ufs = (root / "MtkUfs/MtkUfs.inf").read_text()
power = (root / "MtkUfsPower/MtkUfsPower.inf").read_text()
sd = (root / "MtkSd/MtkSd.inf").read_text()
usb = (root / "MtkUsb/MtkUsb.inf").read_text()
i2c = (root / "MtkI2c/MtkI2c.inf").read_text()
gpio = (root / "MtkGpio/MtkGpio.inf").read_text()

def section(text, name):
    lines = text.splitlines()
    marker = "[" + name + "]"
    try:
        start = next(i for i, line in enumerate(lines) if line.strip() == marker) + 1
    except StopIteration:
        raise AssertionError("missing INF section: " + name)
    values = {}
    for line in lines[start:]:
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and line.endswith("]"):
            break
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip().lower()] = value.strip()
    return values

def has_id(text, hardware_id):
    return "%Description%=Install,ACPI" + chr(92) + hardware_id in text

for label, text, version in (("MtkUfs", ufs, "11/24/2026,0.2.2.0"),
                             ("MtkUfsPower", power, "11/24/2026,0.2.1.0")):
    assert has_id(text, "HANA678B"), label
    service = section(text, "Service")
    assert service.get("servicetype") == "1", label
    assert service.get("starttype") == "0", label
    assert service.get("errorcontrol") == "1", label
    assert service.get("servicebinary") == "%13%" + chr(92) + label + ".sys", label
    assert section(text, "Version").get("driverver") == version, label

assert section(ufs, "Service").get("loadordergroup") == "SCSI Miniport"
assert section(power, "Install.NT.Filters").get("addfilter") == "MtkUfsPower,,UfsPowerFilter"
assert section(power, "UfsPowerFilter").get("filterposition") == "Upper"

assert has_id(sd, "HANA6792")
assert section(sd, "Version").get("driverver") == "11/22/2026,0.2.1.0"
sd_service = section(sd, "Service")
assert sd_service.get("servicetype") == "1"
assert sd_service.get("starttype") == "3"
assert sd_service.get("bootflags") == "0x88"
assert sd_service.get("loadordergroup") == "System Bus Extender"

assert has_id(gpio, "HANA678A")
gpio_service = section(gpio, "Service")
assert gpio_service.get("servicetype") == "1"
assert gpio_service.get("starttype") == "3"
sd_bootflags = int(sd_service["bootflags"], 0)
assert (int(gpio_service["bootflags"], 0) & sd_bootflags) == sd_bootflags

assert has_id(usb, "HANA678D")
assert section(usb, "Version").get("driverver") == "11/25/2026,0.2.9.0"
usb_service = section(usb, "Service")
assert usb_service.get("servicetype") == "1"
assert usb_service.get("starttype") == "3"
assert usb_service.get("bootflags") == "0x80"
assert usb_service.get("dependencies") == "ucx01000"

assert has_id(i2c, "HANA6789")
assert section(i2c, "Version").get("driverver") == "11/23/2026,0.1.2.0"
i2c_service = section(i2c, "Service")
assert i2c_service.get("servicetype") == "1"
assert i2c_service.get("starttype") == "3"
assert i2c_service.get("bootflags") == "0x80"

print("Windows storage/WinPE INF boot settings passed")
