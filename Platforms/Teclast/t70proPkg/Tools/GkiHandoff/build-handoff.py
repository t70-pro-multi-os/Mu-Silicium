#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the OS-neutral ARM64 kexec entry adapter; never load or reboot."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent
IMAGE_BASE, FDT_BASE, FDT_LIMIT = 0x40000000, 0x47c80000, 0x48080000
PAGE = 4096


def sha(data):
    return hashlib.sha256(data).hexdigest()


def layout(image):
    if len(image) < 64 or image[56:60] != b"ARM\x64":
        raise ValueError("Expected raw ARM64 UEFI Image, not boot.img or a PE file")
    offset, extent, flags = struct.unpack_from("<QQQ", image, 8)
    if offset != 0 or extent != len(image) or extent % PAGE or flags != 0xa:
        raise ValueError("Unsupported UEFI Image extent/alignment")
    # FD size and wrapper size are properties of this board's BootShim.
    if extent != 0x401000:
        raise ValueError("Expected T70Pro BootShim plus its 4 MiB FD")
    entry = IMAGE_BASE + extent
    if entry + PAGE > 0x43f7f000:
        raise ValueError("Adapter would overlap board handoff page")
    return entry


def build(image, out):
    entry = layout(image)
    if out.exists() and any(out.iterdir()):
        raise ValueError("Output must be empty; existing artifacts preserved")
    out.mkdir(parents=True, exist_ok=True)
    obj, elf, binary = [out / n for n in ("entry.o", "entry.elf", "entry.bin")]
    subprocess.run(["clang", "--target=aarch64-none-elf", "-c", str(HERE / "handoff-entry.S"),
                    "-o", str(obj)], check=True)
    subprocess.run(["ld.lld", "-Ttext=" + hex(entry), "--entry=t70_kexec_entry",
                    str(obj), "-o", str(elf)], check=True)
    subprocess.run(["llvm-objcopy", "-O", "binary", str(elf), str(binary)], check=True)
    stub = binary.read_bytes()
    if len(stub) != 40 or struct.unpack_from("<QQ", stub, 24) != (FDT_BASE, IMAGE_BASE):
        raise ValueError("Unexpected compiled entry layout")
    (out / "uefi.Image").write_bytes(image)
    report = {
        "schema": 1, "device_access": False, "installable": False,
        "target_os": "unspecified ARM64 OS behind UEFI",
        "entry_address": entry, "kexec_flags": 183 << 16,
        "segments": [
            {"name": "uefi.Image", "physical_address": IMAGE_BASE,
             "bytes": len(image), "memory_bytes": len(image), "sha256": sha(image)},
            {"name": "entry.bin", "physical_address": entry,
             "bytes": len(stub), "memory_bytes": PAGE, "sha256": sha(stub)},
            {"name": "live runtime FDT (not included)", "physical_address": FDT_BASE,
             "maximum_bytes": 0x200000,
             "source": "/sys/firmware/fdt from this boot; static stock DTB is insufficient"},
        ],
        "source_sha256": {n: sha((HERE / n).read_bytes())
                          for n in ("handoff-entry.S", "build-handoff.py")},
        "runtime_requirements": [
            "validated current runtime FDT and complete firmware/modem reservations",
            "destinations including the whole UEFI bootstrap window are available",
            "kvm-arm.mode=none applied before pKVM initialization",
            "standard kexec_load succeeds; never bypass a kernel CPU/pKVM rejection",
            "only then LINUX_REBOOT_CMD_KEXEC, followed by actual device recovery checks",
        ],
        "not_implemented": ["runtime FDT/memory ownership validation and kexec syscall loader",
                            "early /init selection and init_boot integration"],
        "unverified": ["real cache/CPU/EL transition", "device DMA shutdown and reinitialization",
                       "Android return and real OS hardware operation"],
    }
    (out / "handoff.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uefi-image", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    report = build(args.uefi_image.read_bytes(), args.out)
    print(json.dumps({k: report[k] for k in ("installable", "device_access", "entry_address")}))


if __name__ == "__main__":
    main()
