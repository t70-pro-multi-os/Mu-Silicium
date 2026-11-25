#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the T70 Pro SD clock, pad and detect methods under ACPICA."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def compile_asl(source, output):
    subprocess.run(["iasl", "-p", str(output), str(source)], check=True,
                   capture_output=True, text=True)
    return output.with_suffix(".aml")


def fixture(path, fields):
    # ACPICA stores OperationRegion values in memory. W1S/W1C aliases are
    # traced as writes, and do not modify the backing status fields.
    src = ['DefinitionBlock ("", "SSDT", 2, "TECLST", "SDTEST", 1) {']
    for name in fields:
        src.append(f" External ({fields[name]}, FieldUnitObj)")
    src.append(" Method (STRG, 2, Serialized) { Switch (ToInteger (Arg0)) {")
    for index, name in enumerate(fields):
        src.append(f" Case ({index}) {{ Store (Arg1, {fields[name]}) }}")
    src.extend([" } }", "}"])
    asl = path / "SDT.asl"
    asl.write_text("\n".join(src) + "\n")
    return compile_asl(asl, path / "SDT")


good = {
    "SCMX": 0x13000077, "SCST": 0x84080000, "IAXI": 0,
    "GRAD": 1, "GREV": 1, "GPIN": 188, "GIRQ": 126,
    "SDD0": 0, "SDIN": 0x80, "SDM0": 0, "SDDR": 0x10400 & ~0x1F00,
    "SDM1": 0x12345678, "SDM2": 0x44411111, "PIES": 0x100007C,
    "PDRV": 0x436DB600, "PUPD": 0x18000001, "PRZV": 0x2A00003E, "PROV": 0x30000001,
}

bus_open = (0x1000108c, 0x10)
bus_stop = (0x10001088, 0x10)
src_open = (0x1000108c, 0x10000)
src_stop = (0x10001088, 0x10000)
top_select = (0x10000088, 0x700)
top_commit = (0x10000004, 0x20000000)
top_open = (0x10000088, 0x8000)
electrical = [
    (0x10005028, 0x1F00), (0x11EA0084, 0x7C),
    (0x11EA0038, 0x04924800), (0x11EA0034, 0x036DB600),
    (0x11EA00B8, 0x3E), (0x11EA00B4, 1), (0x11EA00E8, 1),
    (0x11EA00E4, 0x3E), (0x11EA00F8, 0x3E), (0x11EA00F4, 1),
]
pad_mux = [(0x1000538c, 0x90000000), (0x1000539c, 0x99999)]
tune = [(0x11EF0000, 0xB000), (0x11EF0004, 0xC00)]

cases = [
    ("card present", "SDCD", {}, 1, []),
    ("card absent", "SDCD", {"SDIN": 0x1200}, 0, []),
    ("detect not available", "SDCD", {"SDIN": 0xFFFFFFFF}, 2, []),
    ("detect pin is output", "SDCD", {"SDD0": 0x80}, 2, []),
    ("detect pin mux", "SDCD", {"SDM0": 0x10000000}, 2, []),
    ("GPIO not initialized", "SDCD", {"GRAD": 0}, 2, []),
    ("AXI invalid selector", "SCOP 0", {"IAXI": 7}, 0, []),
    ("AXI PLL not ready", "SCOP 0", {"IAXI": 1}, 0, []),
    ("inaccessible status", "SCOP 0", {"SCST": 0xFFFFFFFF}, 0, []),
    ("invalid operation", "SCOP 4", {}, 0, []),
    ("HCLK on", "SCOP 0", {}, 1, []),
    ("HCLK enable did not latch", "SCOP 0", {"SCST": 0x84080010}, 0, [bus_open]),
    ("source clock did not stop", "SCOP 1", {}, 0, [src_stop]),
    ("retained 26 MHz and pads", "SCOP 1", {"SCST": 0x84090000}, 1, []),
    ("switch source from PLL", "SCOP 1", {"SCST": 0x84090000, "SCMX": 0x13000377},
     0, [top_select, top_commit]),
    ("switch source with TOP gated", "SCOP 1", {"SCST": 0x84090000, "SCMX": 0x13008377},
     0, [top_select, top_commit, top_open]),
    ("program pads", "SCOP 1", {"SCST": 0x84090000, "SDM1": 0, "SDM2": 0},
     0, electrical + pad_mux),
    ("pad failure blocks mux", "SCOP 1", {"SCST": 0x84090000, "PIES": 0},
     0, electrical),
    ("bad IOCFG blocks pad writes", "SCOP 1", {"SCST": 0x84090000, "PDRV": 0xFFFFFFFF}, 0, []),
    ("source ready", "SCOP 2", {}, 1, []),
    ("source enable did not latch", "SCOP 2", {"SCST": 0x84090000}, 0, [src_open, src_stop]),
    ("no HCLK", "SCOP 2", {"SCST": 0x84090010}, 0, []),
    ("wrong TOP mux", "SCOP 2", {"SCMX": 0x13000177}, 0, []),
    ("wrong pad drive", "SCOP 2", {"PDRV": 0x436D9600}, 0, []),
    ("stop HCLK with source isolated", "SCOP 3", {"SCST": 0x84090000}, 0, [bus_stop]),
    ("already stopped", "SCOP 3", {"SCST": 0x84090010}, 1, []),
    ("configure sampling", "SDTC", {}, 1, tune),
    ("sampling with clock stopped", "SDTC", {"SCST": 0x84090000}, 0, []),
]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--dsdt", type=Path, default=Path(__file__).resolve().parents[2] /
                    "Silicium-ACPI/Platforms/Teclast/t70pro/DSDT.asl")
args = parser.parse_args()
parent_fields = {"IAXI", "GRAD", "GREV", "GPIN", "GIRQ"}
fields = {name: ("\\_SB." if name in parent_fields else "\\_SB.SDC1.") + name for name in good}
with tempfile.TemporaryDirectory(prefix="t70-sd-acpi-") as temp:
    path = Path(temp)
    dsdt = compile_asl(args.dsdt, path / "DSDT")
    mock = fixture(path, fields)
    for case, method, updates, value, writes in cases:
        registers = good | updates
        inputs = [f"evaluate STRG {index} {registers[name]:#x}" for index, name in enumerate(fields)]
        inputs.append(f"evaluate \\_SB.SDC1.{method}")
        result = subprocess.run(["acpiexec", "-vr", "-b", ";".join(inputs), str(dsdt), str(mock)],
                                capture_output=True, text=True, check=True)
        trace = (result.stdout + result.stderr).split(r"Evaluating \_SB.SDC1.", 1)[1]
        readback = [int(v, 16) for v in re.findall(r"\[Integer\] = ([0-9A-F]+)", trace)]
        actual_writes = [(int(address, 16), int(v, 16)) for v, address in re.findall(
            r"SystemMemory Write:\s+Val ([0-9A-F]+) Addr ([0-9A-F]+) BitWidth 20", trace)]
        assert readback == [value] and actual_writes == writes, (case, readback, actual_writes, trace)
        assert not re.search(r"\bAE_[A-Z_]+\b", trace), (case, trace)
print(f"T70 Pro SD ACPI checks passed: {len(cases)}")
