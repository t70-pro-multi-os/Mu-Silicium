#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Cross-build the BTIF HCI adapter and exercise it in a self-contained ARM64 PE."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0


def execute(image):
    pe = pefile.PE(str(image), fast_load=True)
    assert pe.FILE_HEADER.Machine == 0xaa64 and pe.OPTIONAL_HEADER.Magic == 0x20b
    imports = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]]
    assert not imports.VirtualAddress and not imports.Size
    pe.parse_data_directories([pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    counter, = (symbol.address for symbol in pe.DIRECTORY_ENTRY_EXPORT.symbols
                if symbol.name == b"T70BtHciAssertions")
    base = pe.OPTIONAL_HEADER.ImageBase
    size = pe.OPTIONAL_HEADER.SizeOfImage
    runner = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    runner.mem_map(base, (size + 4095) & ~4095)
    runner.mem_write(base, pe.get_memory_mapped_image())
    stack, stop = 0x70000000, 0x60000000
    runner.mem_map(stack, 65536)
    runner.mem_map(stop, 4096)
    runner.reg_write(UC_ARM64_REG_SP, stack + 65536 - 16)
    runner.reg_write(UC_ARM64_REG_LR, stop)
    runner.emu_start(base + pe.OPTIONAL_HEADER.AddressOfEntryPoint, stop,
                     timeout=10_000_000, count=50_000_000)
    assert runner.reg_read(UC_ARM64_REG_PC) == stop, "HCI test exceeded the execution budget"
    line = runner.reg_read(UC_ARM64_REG_X0)
    assert line == 0, f"HciTransportTest.c:{line}"
    assertions, = struct.unpack("<I", runner.mem_read(base + counter, 4))
    assert assertions > 2200
    return assertions


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, help="retain verified build artifacts in a new directory")
    args = parser.parse_args()
    if args.output_dir and args.output_dir.exists():
        parser.error("output directory must not already exist")
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    silicon = root / "Silicon/MediaTek/MediaTekPkg"
    cores = [silicon / "Library/MtkStpCoreLib" / name for name in ("MtkStpCore.c", "MtkBtifCore.c", "MtkStpFull.c")]
    inputs = [here / name for name in ("check-wdk.py", "HciTransport.h", "HciTransport.c",
                                        "Tests/HciTransportTest.c", "test-hci.py")]
    inputs += cores + [silicon / "Include/Library" / name for name in ("MtkStpCore.h", "MtkBtifCore.h", "MtkStpFull.h")]
    sources = {str(path.relative_to(root)): digest(path) for path in inputs}
    with tempfile.TemporaryDirectory(prefix="t70-bt-hci-") as tmp:
        stage = Path(tmp)
        adapter, fixture = stage / "HciTransport.obj", stage / "HciTransportTest.obj"
        for option, path in (("--hci-object", adapter), ("--hci-test-object", fixture)):
            subprocess.run([sys.executable, here / "check-wdk.py", option, path], check=True)
        objects = [adapter]
        for source in cores:
            obj = stage / (source.stem + ".obj")
            subprocess.run(["clang", "--target=arm64-pc-windows-msvc", "-O2", "-ffreestanding",
                            "-I", silicon / "Include", "-c", source, "-o", obj], check=True)
            objects.append(obj)
        library = stage / "MtkBtHciTransport.lib"
        subprocess.run(["llvm-lib", "/nologo", "/machine:arm64", f"/out:{library}", *objects], check=True)
        image = stage / "HciTransportTest.exe"
        subprocess.run(["lld-link", "/nologo", "/machine:arm64", "/subsystem:native",
                        "/nodefaultlib", "/entry:TestMain", "/export:T70BtHciAssertions,DATA",
                        f"/out:{image}", fixture, library], check=True)
        assertions = execute(image)
        assert sources == {str(path.relative_to(root)): digest(path) for path in inputs}
        if args.output_dir:
            args.output_dir.mkdir(parents=True, exist_ok=False)
            for path in (library, image):
                shutil.copyfile(path, args.output_dir / path.name)
            report = {"assertions": assertions, "library_sha256": digest(library),
                      "pe_sha256": digest(image), "source_sha256": sources,
                      "scope": "WDK ARM64 production objects with mocked BTIF registers in Unicorn"}
            (args.output_dir / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"BTIF HCI adapter passed {assertions} assertions in the ARM64 test PE")


if __name__ == "__main__":
    main()
