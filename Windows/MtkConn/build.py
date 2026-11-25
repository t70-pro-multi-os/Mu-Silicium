#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test WCN0 against stock T70 Pro firmware and link its unsigned ARM64 driver."""
import argparse
import configparser
import hashlib
import json
import re
import shutil
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parent
ROOT = PORT.parents[1]
WINDOWS = ROOT / "Windows"
SILICON = ROOT / "Silicon/MediaTek/MediaTekPkg"
CORE = SILICON / "Library/MtkStpCoreLib"
TOOLCHAIN = WINDOWS / "Toolchain"
WDK = TOOLCHAIN / "microsoft.windows.wdk.arm64/c"
SDK = TOOLCHAIN / "microsoft.windows.sdk.cpp/c"
VERSION = "10.0.26100.0"
KM = WDK / f"Lib/{VERSION}/km/ARM64"
KMDF_INCLUDE = WDK / "Include/wdf/kmdf/1.33"
KMDF_LIB = WDK / "Lib/wdf/kmdf/ARM64/1.33"
DRIVER = PORT / "Driver.c"
INF = PORT / "MtkConn.inf"
BOOTSTRAP = PORT / "Bootstrap.c"
FIRMWARE = PORT / "Firmware.c"
PMIC = WINDOWS / "MtkBtif/ConnPmic.c"
CONN_CORE = [CORE / f"MtkConn{name}.c" for name in ("Boot", "Power", "Rails", "Rom")]


def run(*command):
    subprocess.run([str(arg) for arg in command], check=True)


def firmware_names():
    names = set(re.findall(r'L"(soc2_2_ram_[^"]+\.bin)"', FIRMWARE.read_text()))
    inf = configparser.ConfigParser(allow_no_value=True, interpolation=None)
    inf.optionxform = str
    inf.read(INF)
    files = names | {"MtkConn.sys"}
    if (len(names) != 6 or set(inf["SourceDisksFiles"]) != files or set(inf["ConnFiles"]) != files
            or inf["Install.NT"]["CopyFiles"] != "ConnFiles"
            or inf["DestinationDirs"]["ConnFiles"] != "13"
            or inf["Models.NTarm64.10.0...26100"]["%Description%"] != r"Install,ACPI\HANA6796"
            or inf["Install.NT.Wdf"]["KmdfService"] != "MtkConn,Wdf"
            or inf["Wdf"]["KmdfLibraryVersion"] != "1.33"):
        raise ValueError("CONN firmware names or Windows driver INF do not match the package")
    return sorted(names)


def verify_driver(image, link_map):
    info = subprocess.check_output(["llvm-readobj", "--file-headers", "--coff-imports", image], text=True)
    required = ("IMAGE_FILE_MACHINE_ARM64", "IMAGE_SUBSYSTEM_NATIVE", "IMAGE_DLL_CHARACTERISTICS_WDM_DRIVER",
                "WdfVersionBind", "BCryptHash", "ExGetPrmInterface", "IoGetDriverDirectory", "ZwReadFile")
    if not all(field in info for field in required):
        raise ValueError("Unexpected ARM64 KMDF driver imports or PE flags")
    if "TERMINAL_SERVER_AWARE" in info:
        raise ValueError("Terminal Server aware flag must be clear")

    def hex_field(name):
        return int(re.search(r"^\s*" + name + r": (0x[0-9a-fA-F]+)", info, re.M).group(1), 16)

    symbols = {name: int(address, 16) for name, address in re.findall(
        r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+",
        link_map.read_text(), re.M)}
    if (symbols["FxDriverEntry"] != hex_field("ImageBase") + hex_field("AddressOfEntryPoint")
            or symbols["DriverEntry"] == symbols["FxDriverEntry"]
            or "ConnScoInterrupt" not in symbols):
        raise ValueError("Invalid KMDF framework entry")
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-firmware", type=Path,
                        default=ROOT.parent / "analysis/stock-binaries/vendor_a_firmware")
    parser.add_argument("--output-dir", type=Path, help="new directory for the unsigned driver and stock ROM files")
    args = parser.parse_args()
    if args.output_dir and args.output_dir.exists():
        parser.error("output directory must not exist")
    includes = [WDK / f"Include/{VERSION}/km", WDK / f"Include/{VERSION}/shared",
                SDK / f"Include/{VERSION}/shared", SDK / f"Include/{VERSION}/ucrt",
                SDK / f"Include/{VERSION}/um",
                next((TOOLCHAIN / "msvc-crt-headers").rglob("excpt.h")).parent, KMDF_INCLUDE]
    with tempfile.TemporaryDirectory(prefix="t70-conn-") as temporary:
        work = Path(temporary)
        package = work / "package"
        package.mkdir()
        for name in firmware_names():
            shutil.copyfile(args.stock_firmware / name, package / name)
        shutil.copyfile(INF, package / INF.name)
        test_flags = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
                      "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Wno-multichar",
                      "-I", PORT / "Tests/include", "-I", WINDOWS / "MtkBtif/Tests/include",
                      "-I", SILICON / "Include"]
        bootstrap_test = work / "bootstrap-test"
        run(*test_flags, BOOTSTRAP, FIRMWARE, PORT / "Tests/BootstrapTest.c",
            *CONN_CORE, "-lcrypto", "-o", bootstrap_test)
        run(bootstrap_test, package)
        driver_test = work / "driver-test"
        run(*test_flags, PORT / "Tests/DriverTest.c", "-o", driver_test)
        run(driver_test)
        sco_packet_test = work / "sco-packet-test"
        run(*test_flags, PORT / "ScoPacket.c", PORT / "Tests/ScoPacketTest.c",
            "-o", sco_packet_test)
        run(sco_packet_test)

        overlay = work / "wdk-overlay.json"
        overlay.write_text(json.dumps({"version": 0, "case-sensitive": False, "roots": [
            {"type": "directory", "name": str(folder), "contents": [
                {"type": "file", "name": str(path.relative_to(folder)), "external-contents": str(path)}
                for path in folder.rglob("*") if path.is_file()]} for folder in includes]}))
        flags = ["clang", "--driver-mode=cl", "--target=arm64-pc-windows-msvc",
                 "/nologo", "/c", "/kernel", "/O2", "/GS", "/Zl", "/W4", "/WX", "/Gy",
                 "/D_ARM64_", "/DWIN64", "/DWINVER=0x0A00", "/D_WIN32_WINNT=0x0A00",
                 "/DNTDDI_VERSION=0x0A000010", "/clang:-Wno-invalid-token-paste",
                 "/clang:-Wno-nonportable-include-path", "/clang:-Wno-ignored-attributes",
                 "/clang:-ivfsoverlay", "/clang:" + str(overlay), "/I" + str(SILICON / "Include"),
                 *["/imsvc" + str(folder) for folder in includes]]
        sco_packet_object = work / "ScoPacket.obj"
        run(*flags, "/Tc" + str(PORT / "ScoPacket.c"), "/Fo" + str(sco_packet_object))
        objects = []
        for source in (DRIVER, BOOTSTRAP, FIRMWARE, PMIC, *CONN_CORE):
            obj = work / (source.stem + ".obj")
            run(*flags, "/Tc" + str(source), "/Fo" + str(obj))
            objects.append(obj)
        image = package / "MtkConn.sys"
        link_map = work / "MtkConn.map"
        run("lld-link", "/nologo", "/driver:wdm", "/release", "/machine:arm64", "/entry:FxDriverEntry",
            "/nodefaultlib", "/subsystem:native,10.00", "/opt:ref", "/dynamicbase", "/nxcompat", "/tsaware:no", "/Brepro",
            "/out:" + str(image), "/map:" + str(link_map), *objects,
            KMDF_LIB / "wdfdriverentry.lib", KMDF_LIB / "wdfldr.lib",
            KM / "ntoskrnl.lib", KM / "hal.lib", KM / "cng.lib", KM / "libcntpr.lib",
            KM / "bufferoverflowfastfailk.lib")
        imports = verify_driver(image, link_map)
        if args.output_dir:
            report = {
                "hardware_id": r"ACPI\HANA6796",
                "status": "ARM64 KMDF linked, unsigned",
                "files_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted(package.iterdir())},
                "sco_packet_core": {
                    "arm64_coff_bytes": sco_packet_object.stat().st_size,
                    "sha256": hashlib.sha256((PORT / "ScoPacket.c").read_bytes()).hexdigest(),
                    "runtime_linked": False,
                    "scope": "SCO packet snapshot/SRAM batch-copy object; WCN binds GSIV 439 and ACKs status only while masked, but packet transfer and audio endpoint are not connected",
                },
                "sco_irq": {
                    "gsiv": 439,
                    "wdf_isr_linked": True,
                    "handler": "ConnScoInterrupt",
                    "acknowledges_status_only": True,
                    "packet_transfer_linked": False,
                    "hfp_audio_endpoint": False,
                },
                "imports": imports,
            }
            (package / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
            shutil.copytree(package, args.output_dir)
        print("MtkConn ARM64 KMDF driver linked with the WDK and stock ROMs")


if __name__ == "__main__":
    main()
