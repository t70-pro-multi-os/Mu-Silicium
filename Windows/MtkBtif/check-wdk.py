#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Opt-in compile of BTIF code using the existing WDK."""
from pathlib import Path
import argparse
import json
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
target = parser.add_mutually_exclusive_group()
target.add_argument("--object", type=Path, help="retain the compiled BTIF owner object")
target.add_argument("--hci-object", type=Path, help="compile the HCI packet adapter")
target.add_argument("--hci-test-object", type=Path, help="compile the HCI test fixture")
target.add_argument("--pmic-object", type=Path, help="compile the CONN PMIC lease adapter")
target.add_argument("--pmic-test-object", type=Path, help="compile the CONN PMIC test fixture")
args = parser.parse_args()
here = Path(__file__).resolve().parent
toolchain = here.parent / "Toolchain"
wdk = toolchain / "microsoft.windows.wdk.arm64/c"
sdk = toolchain / "microsoft.windows.sdk.cpp/c"
crt = next((toolchain / "msvc-crt-headers").rglob("excpt.h")).parent
includes = [wdk / "Include/10.0.26100.0/km", wdk / "Include/10.0.26100.0/shared",
    sdk / "Include/10.0.26100.0/shared", sdk / "Include/10.0.26100.0/ucrt",
    sdk / "Include/10.0.26100.0/um", crt, wdk / "Include/wdf/kmdf/1.33"]
for folder in includes:
    if not folder.is_dir():
        raise SystemExit(f"Existing WDK directory unavailable: {folder}")
with tempfile.TemporaryDirectory(prefix="t70-btif-wdk-") as tmp:
    overlay = Path(tmp) / "wdk-overlay.json"
    overlay.write_text(json.dumps({"version": 0, "case-sensitive": False, "roots": [
        {"type": "directory", "name": str(folder), "contents": [
            {"type": "file", "name": str(p.relative_to(folder)), "external-contents": str(p)}
            for p in folder.rglob("*") if p.is_file()]} for folder in includes]}))
    command = ["clang", "--driver-mode=cl"] if sys.platform == "darwin" else ["clang-cl"]
    command += ["--target=arm64-pc-windows-msvc", "/nologo", "/c", "/kernel", "/O2", "/GS", "/Zl", "/W3",
        "/D_ARM64_", "/DWIN64", "/DWINVER=0x0A00", "/D_WIN32_WINNT=0x0A00",
        "/DWDF_MAJOR_VERSION=1", "/DWDF_MINOR_VERSION=33",
        "/clang:-Wno-invalid-token-paste", "/clang:-Wno-nonportable-include-path",
        "/clang:-Wno-ignored-attributes", "/clang:-Werror=incompatible-pointer-types",
        "/clang:-ivfsoverlay", "/clang:" + str(overlay)]
    command += ["/imsvc" + str(p) for p in includes]
    source, obj = here / "Driver.c", args.object or Path(tmp) / "BtifOwner.obj"
    command += ["/I" + str(here.parents[1] / "Silicon/MediaTek/MediaTekPkg/Include")]
    if args.hci_object or args.hci_test_object or args.pmic_object or args.pmic_test_object:
        mode = [(args.hci_object, "HciTransport.c"), (args.hci_test_object, "Tests/HciTransportTest.c"),
                (args.pmic_object, "ConnPmic.c"), (args.pmic_test_object, "Tests/ConnPmicTest.c")]
        obj, name = next((path, name) for path, name in mode if path is not None)
        source = here / name
        if args.hci_test_object or args.pmic_test_object:
            command += ["/GS-"]
        if args.pmic_object or args.pmic_test_object:
            command += ["/W4", "/WX", "/DNTDDI_VERSION=0x0A000010"]
    if source == here / "Driver.c":
        command += ["/W4", "/WX", "/DNTDDI_VERSION=0x0A000010"]
    command += ["/Tc" + str(source), "/Fo" + str(obj.resolve())]
    subprocess.run(command, check=True)
    print(f"{source.name}: WDK ARM64 object compile passed")
