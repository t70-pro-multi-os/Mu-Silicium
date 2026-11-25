#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Prepare an OS-neutral external UEFI test ESP using regular files only."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
PACKAGE = HERE.parent
REPO = PACKAGE.parents[2]


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def check_efi(data):
    if len(data) < 64 or len(data) > 4 * 1024 * 1024 or data[:2] != b"MZ":
        raise ValueError("Expected a bounded ARM64 EFI application")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe < 64 or pe + 24 + 70 > len(data) or data[pe:pe+4] != b"PE\0\0":
        raise ValueError("Invalid PE header")
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    if machine != 0xaa64 or not 0 < sections <= 96 or optional_size < 112:
        raise ValueError("Expected AArch64 PE32+")
    if opt + optional_size + sections * 40 > len(data):
        raise ValueError("Truncated PE tables")
    if struct.unpack_from("<H", data, opt)[0] != 0x20b or struct.unpack_from("<H", data, opt+68)[0] != 10:
        raise ValueError("Expected an EFI application, not an OS kernel or driver")
    for i in range(sections):
        off = opt + optional_size + i * 40
        count, start = struct.unpack_from("<II", data, off + 16)
        if count and (start > len(data) or count > len(data) - start):
            raise ValueError("Truncated PE section")


def prepare(shell, out, esp_mib=1024):
    shell = shell.resolve(strict=True)
    data = shell.read_bytes()
    check_efi(data)
    if not 64 <= esp_mib <= 2048:
        raise ValueError("ESP filesystem size must be 64..2048 MiB")
    if out.is_symlink() or (out.exists() and (not out.is_dir() or any(out.iterdir()))):
        raise ValueError("Output must be an empty directory; existing files preserved")
    for tool in ("mformat", "mmd", "mcopy"):
        if not shutil.which(tool):
            raise ValueError("Missing existing builder tool: " + tool)
    out.mkdir(parents=True, exist_ok=True)
    esp = out / "esp"
    boot = esp / "EFI/BOOT/BOOTAA64.EFI"
    boot.parent.mkdir(parents=True)
    boot.write_bytes(data)
    note = out / "README.txt"
    note.write_text(
        "T70 Pro G100 external UEFI test files\n"
        "esp/ is the file tree for a FAT32 EFI System Partition.\n"
        "esp.fat is a partition filesystem image, NOT a whole SD-card image.\n"
        "Do not write esp.fat over an entire card or the internal UFS.\n"
        "BOOTAA64.EFI opens the existing ARM64 UEFI Shell; no OS is included.\n"
        "Android/UEFI entry, physical SD access and Android return remain unverified.\n"
        "Do not repartition internal storage during this external-media test.\n"
    )
    image = out / "esp.fat"
    with image.open("xb") as stream:
        stream.truncate(esp_mib * 1024 * 1024)
    subprocess.run(["mformat", "-i", str(image), "-F", "-v", "T70UEFI", "::"], check=True)
    subprocess.run(["mmd", "-i", str(image), "::/EFI", "::/EFI/BOOT"], check=True)
    subprocess.run(["mcopy", "-i", str(image), str(boot), "::/EFI/BOOT/BOOTAA64.EFI"], check=True)
    # Exercise the actual FAT read path and compare its bytes with the staged PE.
    readback = out / "readback.pending"
    subprocess.run(["mcopy", "-i", str(image), "::/EFI/BOOT/BOOTAA64.EFI", str(readback)], check=True)
    if readback.read_bytes() != data:
        raise ValueError("ESP filesystem readback differs from the original EFI application")
    readback.unlink()
    report = {
        "schema": 1, "device_access": False, "installable": False,
        "target_os": None, "contains_os": False, "purpose": "external UEFI Shell entry test",
        "source": {"path": str(shell), "bytes": len(data), "sha256": digest(shell)},
        "files": {"EFI/BOOT/BOOTAA64.EFI": {"bytes": len(data), "sha256": digest(boot)}},
        "filesystem": {"path": str(image), "bytes": image.stat().st_size,
                       "type": "FAT32", "kind": "ESP filesystem image, not whole disk",
                       "sha256": digest(image), "actual_write_and_readback_passed": True},
        "internal_partition_changes": [], "os_partition_format": None,
        "unverified": ["Android to UEFI boot selector", "real SD/USB boot on the tablet",
                       "selected OS storage drivers and persistent writes", "Android recovery"],
        "source_sha256": digest(Path(__file__)),
    }
    (out / "media.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shell", type=Path, default=REPO / "Build/t70proPkg/RELEASE_CLANGPDB/AARCH64/Shell.efi")
    parser.add_argument("--out", type=Path, default=REPO / "Build/t70proPkg/UefiExternalStage")
    parser.add_argument("--esp-mib", type=int, default=1024)
    args = parser.parse_args()
    report = prepare(args.shell, args.out, args.esp_mib)
    print(json.dumps({k: report[k] for k in ("target_os", "contains_os", "device_access", "installable")}))


if __name__ == "__main__":
    main()
