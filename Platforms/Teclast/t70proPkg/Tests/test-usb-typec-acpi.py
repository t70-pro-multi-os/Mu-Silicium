#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise T70 Pro USB ACPI methods with ACPICA and simulated I2C/GPIO."""

import argparse
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory


def run(*args):
    proc = subprocess.run(args, check=True, capture_output=True, text=True)
    return proc.stdout + proc.stderr


def compile_asl(source):
    run("iasl", "-p", str(source.with_suffix("")), str(source))
    return source.with_suffix(".aml")


def evaluate(aml, methods, *tables):
    output = run("acpiexec", "-b", "; ".join(methods), str(aml), *map(str, tables))
    assert not re.search(r"\bAE_[A-Z_]+\b", output), output
    return output


def integer_result(output):
    values = re.findall(r"\[Integer\] = ([0-9A-F]+)", output)
    assert len(values) == 1, output
    return int(values[0], 16)


def buffer_result(output):
    match = re.search(r"\[Buffer\] Length ([0-9A-F]+) =", output)
    assert match, output
    rows = re.findall(r"^ *[0-9A-F]{4}: ([0-9A-F ]+) +//", output, re.MULTILINE)
    data = bytes.fromhex(" ".join(rows))
    assert len(data) == int(match[1], 16), output
    return data


def mock_dsdt(source, registers, changed_cc):
    start = source.index("            Field (TCIO,")
    end = source.index("            Method (_REG", start)
    fields = []
    for name, data in registers.items():
        contents = ", ".join(f"0x{byte:02X}" for byte in data)
        fields.append(f"            Name ({name}, Buffer ({len(data)}) {{ {contents} }})\n")
    if changed_cc is not None:
        fields.extend([
            "            Name (SAMP, Zero)\n",
            "            Method (TCCT, 0, NotSerialized)\n",
            "            {\n",
            f"                If (SAMP) {{ Return (Buffer (3) {{ 0, 0, {changed_cc} }}) }}\n",
            "                Increment (SAMP)\n",
            "                Return (TCCS)\n",
            "            }\n",
        ])
        source = source.replace("TCRD (TCCS)", "TCRD (TCCT ())", 1)
    return source[:start] + "".join(fields) + source[end:]


FIXTURE = r"""
DefinitionBlock ("", "SSDT", 2, "TECLST", "TCTEST", 1)
{
    External (\_SB.IRAD, FieldUnitObj)
    External (\_SB.IREV, FieldUnitObj)
    External (\_SB.ICL6, FieldUnitObj)
    External (\_SB.MREV, FieldUnitObj)
    External (\_SB.MHPW, FieldUnitObj)
    Method (TSTI, 1, Serialized)
    {
        Store (Arg0, \_SB.IRAD)
        Store (One, \_SB.IREV)
        Store (26000000, \_SB.ICL6)
    }
    Method (TSTP, 2, Serialized)
    {
        Store (Arg0, \_SB.MREV)
        Store (Arg1, \_SB.MHPW)
    }
}
"""

USB = r"evaluate \_SB.USB0."
I2C_READY = r"evaluate \TSTI 2"
ATTACH_REGION = USB + "_REG 9 1"
READ = USB + "MTCV"


def test_route(directory, source, aml):
    pogo = {
        "UDR0": 0x80000010, "UDR2": 0x400000, "UDR3": 0x108,
        "UDI0": 0x80000010, "UDI2": 0, "UDI3": 0x108,
        "UMD0": 0, "UMD1": 0, "UMA0": 0, "UMC0": 0, "UMD3": 0,
    }
    gpio = tuple(pogo)
    fixture = directory / "gpio.asl"
    externs = "\n".join(fr"    External (\_SB.USB0.{name}, FieldUnitObj)" for name in gpio)
    setters = "\n".join(
        fr"        If (LEqual (Arg0, {i})) {{ Store (Arg1, \_SB.USB0.{name}) }}"
        for i, name in enumerate(gpio)
    )
    fixture.write_text(f"""DefinitionBlock ("", "SSDT", 2, "TECLST", "USBGPIO", 1)
{{
{externs}
    Method (GSET, 2, Serialized)
    {{
{setters}
    }}
}}
""")
    ssdt = compile_asl(fixture)

    def check(table, path, expected, overrides):
        values = pogo | overrides
        setup = [fr"evaluate \GSET {i} {values[name]:#x}" for i, name in enumerate(gpio) if values[name]]
        output = evaluate(table, [*setup, USB + f"MROU {path}"], ssdt)
        assert integer_result(output) == expected, output

    cases = [
        (2, 1, {}),
        (0, 1, {"UDI2": 0x400000}),
        (1, 1, {"UDI2": 0x400000}),
        (1, 1, {"UDI0": 0x410, "UDI2": 0x400000}),
        (1, 0, {"UDI0": 0x400, "UDI2": 0x400000}),
        (3, 1, {"UDI0": 0, "UDI2": 0x400000, "UDI3": 0}),
        (3, 1, {"UDI0": 0x400, "UDI2": 0x400000, "UDI3": 0}),
        (3, 1, {"UDI0": 0x400, "UDI2": 0x400000, "UDI3": 0x100}),
        (3, 0, {"UDI0": 0x400, "UDI2": 0x400000, "UDI3": 8}),
        (3, 0, {"UDI0": 0x410, "UDI2": 0x400000, "UDI3": 0}),
        (4, 0, {}),
        (1, 0, {}),
        (2, 1, {"UDI2": 0x400000}),
        (2, 0, {"UDI0": 0x410}),
        (2, 0, {"UDI0": 0}),
        (2, 0, {"UDI3": 8}),
        (2, 0, {"UDI3": 0x100}),
        (2, 0, {"UDR0": 0x410}),
        (2, 0, {"UDR0": 0}),
        (2, 0, {"UDR2": 0}),
        (2, 0, {"UDR3": 8}),
        (2, 0, {"UDR3": 0x100}),
    ]
    for field, bit in [("UMD0", 16), ("UMD1", 8), ("UMA0", 24), ("UMC0", 12), ("UMD3", 0)]:
        cases.append((2, 0, {field: 1 << bit}))
    for path, expected, registers in cases:
        check(aml, path, expected, registers)

    # Toggle ID between observations without changing the shared hub pins.
    changed = directory / "changed.asl"
    changed.write_text(source.replace("            Method (MRGP",
        """            Name (UREA, Zero)
            Method (UUID, 0, NotSerialized)
            {
                If (UREA) { Return (Xor (UDI0, 0x400)) }
                Increment (UREA)
                Return (UDI0)
            }
            Method (MRGP""", 1).replace("And (UDI0, 0x410)", "And (UUID (), 0x410)", 1))
    changed_aml = compile_asl(changed)
    check(changed_aml, 2, 2, {})
    check(changed_aml, 2, 2, {"UDI0": 0x410})
    check(changed_aml, 1, 1, {"UDI2": 0x400000})
    return len(cases) + 3


def test(path):
    source = path.read_text()
    with TemporaryDirectory(prefix="t70-typec-acpi-") as directory:
        directory = Path(directory)
        dsdt = directory / "DSDT.asl"
        dsdt.write_text(source)
        aml = compile_asl(dsdt)
        fixture = directory / "fixture.asl"
        fixture.write_text(FIXTURE)
        ssdt = compile_asl(fixture)

        controller = bytes.fromhex(
            "86 09 00 01 00 00 20 11 00 00 01 00 "
            "86 09 00 00 00 F1 F7 43 40 00 00 00 "
            "86 09 00 01 00 00 F4 11 00 07 00 00 "
            "89 06 00 01 01 A0 00 00 00"
        )
        tcpc = bytes.fromhex("8E 19 00 02 00 01 06 00 00 01 06 00 80 1A 06 00 4E 00")
        tcpc += b"\\_SB.I2C6\x00"

        for revision, path, i2c_ready, dep_expected, crs_tcpc in [
            (1, 0, False, 1, 1),
            (2, 1, False, 1, 1),
            (2, 2, False, 0, 0),
            (2, 2, True, 0, 1),
            (2, 3, False, 1, 1),
        ]:
            setup = [rf"evaluate \TSTP {revision} {path}"]
            if i2c_ready:
                setup.append(I2C_READY)
            output = evaluate(aml, [*setup, USB + "_DEP", USB + "_CRS"], ssdt)
            assert f"[Package] Contains {dep_expected} Elements:" in output, output
            assert ("Name I2C6 Device" in output) == bool(dep_expected), output
            # Decode the actual AML resource bytes. The original MMIO and IRQ
            # descriptors stay in place. Type-C paths and pogo boots with a
            # ready I2C6 receive I2C6 0x4E (400 kHz, 7-bit, shared), while
            # pogo keeps _DEP empty so USB0 never blocks on I2C6 startup.
            assert buffer_result(output) == controller + tcpc * crs_tcpc + b"\x79\x00", output

        # The unmodified table must return unavailable with I2C6 stopped.
        assert integer_result(evaluate(aml, [READ], ssdt)) == 0
        route_cases = test_route(directory, source, aml)

        good = {name: (0, 0, value) for name, value in {
            "TVL0": 0x99, "TVH0": 0x2E, "TDL0": 0x11, "TDH0": 3,
            "TROL": 5, "TCCS": 2, "TPWR": 0x0C, "TVS0": 0x80,
        }.items()}
        cases = [
            ("live CC1", {}, None, [I2C_READY, ATTACH_REGION], 0x0105020C),
            ("live CC2", {"TCCS": (0, 0, 8)}, None, [I2C_READY, ATTACH_REGION], 0x0105080C),
            ("I2C6 stopped", {}, None, [ATTACH_REGION], 0),
            ("region disconnected", {}, None, [I2C_READY, ATTACH_REGION, USB + "_REG 9 0"], 0),
            ("wrong vendor", {"TVH0": (0, 0, 0)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("wrong product", {"TDH0": (0, 0, 2)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("bus error", {"TVL0": (1, 0, 0x99)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("short read", {"TROL": (0, 5)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("CC read failed", {"TCCS": (1, 0, 2)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("CC open with VBUS", {"TCCS": (0, 0, 0)}, None, [I2C_READY, ATTACH_REGION], 0x0105000C),
            ("no VBUS", {"TPWR": (0, 0, 8)}, None, [I2C_READY, ATTACH_REGION], 0x01050208),
            ("VBUS at safe0V", {"TPWR": (0, 0, 8), "TVS0": (0, 0, 2)}, None,
             [I2C_READY, ATTACH_REGION], 0x01050208),
            ("detector off", {"TPWR": (0, 0, 4)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("initializing", {"TPWR": (0, 0, 0x4C)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("contradictory VBUS", {"TVS0": (0, 0, 2)}, None, [I2C_READY, ATTACH_REGION], 0),
            ("CC changed", {}, 8, [I2C_READY, ATTACH_REGION], 0),
        ]
        for name, override, changed_cc, setup, expected in cases:
            dsdt.write_text(mock_dsdt(source, good | override, changed_cc))
            output = evaluate(compile_asl(dsdt), [*setup, READ], ssdt)
            assert integer_result(output) == expected, (name, output)
        print(f"T70 Pro USB ACPI checks passed: {5 + len(cases)} Type-C and {route_cases} board route cases")


if __name__ == "__main__":
    repo = Path(__file__).resolve().parents[4]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsdt", type=Path, default=repo / "Silicium-ACPI/Platforms/Teclast/t70pro/DSDT.asl")
    test(parser.parse_args().dsdt)
