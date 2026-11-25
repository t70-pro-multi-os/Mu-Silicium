#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check actual FAT contents and reject wrong EFI architecture/type, offline."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("t70_media", HERE / "prepare-uefi-media.py")
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    doc = json.loads((args.artifacts / "media.json").read_text())
    image = Path(doc["filesystem"]["path"])
    assert doc["source_sha256"] == media.digest(HERE / "prepare-uefi-media.py")
    assert image.stat().st_size == doc["filesystem"]["bytes"]
    assert media.digest(image) == doc["filesystem"]["sha256"]
    original = (args.artifacts / "esp/EFI/BOOT/BOOTAA64.EFI").read_bytes()
    media.check_efi(original)
    with tempfile.TemporaryDirectory(prefix="t70-esp-test-") as directory:
        tmp = Path(directory)
        readback = tmp / "BOOTAA64.EFI"
        subprocess.run(["mcopy", "-i", str(image), "::/EFI/BOOT/BOOTAA64.EFI", str(readback)], check=True)
        assert readback.read_bytes() == original
        assert media.digest(readback) == doc["source"]["sha256"]
        existing = tmp / "keep"
        existing.mkdir()
        (existing / "data").write_bytes(b"preserve")
        try:
            media.prepare(readback, existing, 64)
        except ValueError:
            pass
        else:
            raise AssertionError("Existing output was accepted")
        assert (existing / "data").read_bytes() == b"preserve"
    pe = struct.unpack_from("<I", original, 60)[0]
    wrong_arch, wrong_type = bytearray(original), bytearray(original)
    struct.pack_into("<H", wrong_arch, pe + 4, 0x8664)
    struct.pack_into("<H", wrong_type, pe + 24 + 68, 11)
    rejected = 0
    for bad in (b"ANDROID!" + bytes(64), original[:100], bytes(wrong_arch), bytes(wrong_type)):
        try:
            media.check_efi(bad)
        except ValueError:
            rejected += 1
        else:
            raise AssertionError("Invalid EFI application was accepted")
    report = {"status": "passed", "fat_actual_file_readback": True,
              "invalid_efi_inputs_rejected": rejected, "existing_output_preserved": True,
              "internal_partition_changes": [], "device_access": False,
              "real_tablet_media_boot_verified": False,
              "test_source_sha256": media.digest(Path(__file__))}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
