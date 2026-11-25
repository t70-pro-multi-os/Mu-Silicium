#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the WDK CONN power adapter and the firmware broker together on ARM64."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    here = Path(__file__).resolve().parent
    silicon = here.parents[1] / "Silicon/MediaTek/MediaTekPkg"
    with tempfile.TemporaryDirectory(prefix="t70-conn-pmic-") as tmp:
        stage = Path(tmp)
        adapter = stage / "ConnPmic.obj"
        fixture = stage / "ConnPmicTest.obj"
        for option, output in (("--pmic-object", adapter), ("--pmic-test-object", fixture)):
            subprocess.run([sys.executable, here / "check-wdk.py", option, output], check=True)

        # The fixture compiles the adapter with its kernel entry points mocked.
        objects = [fixture]
        for source in (silicon / "Drivers/MtkUfsPrm/MtkPmicBroker.c",
                       silicon / "Library/MtkStpCoreLib/MtkConnRails.c"):
            output = stage / (source.stem + ".obj")
            subprocess.run(["clang", "--target=arm64-pc-windows-msvc", "-O2", "-ffreestanding",
                            "-I", silicon / "Include", "-c", source, "-o", output], check=True)
            objects.append(output)

        image = stage / "ConnPmicTest.exe"
        kernel = here.parent / "Toolchain/microsoft.windows.wdk.arm64/c/Lib/10.0.26100.0/km/ARM64/ntoskrnl.lib"
        subprocess.run(["lld-link", "/nologo", "/machine:arm64", "/subsystem:native", "/nodefaultlib",
                        "/entry:TestMain", f"/out:{image}", *objects, kernel], check=True)
        subprocess.run([sys.executable, here.parent / "test-request.py", image, "--test-name", "MtkConnPmic",
                        "--production-source", here / "ConnPmic.c",
                        "--test-source", here / "Tests/ConnPmicTest.c"], check=True)


if __name__ == "__main__":
    main()
