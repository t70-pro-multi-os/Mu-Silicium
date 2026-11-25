#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a small GPT/FAT32 image containing the AArch64 UEFI fallback file."""
import argparse
from pathlib import Path
import struct
import subprocess
import uuid
import zlib

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("efi", type=Path)
p.add_argument("image", type=Path)
x = p.parse_args()
data = x.efi.read_bytes()
if data[:2] != b"MZ" or len(data) < 64:
    p.error("input is not a PE image")
pe, = struct.unpack_from("<I", data, 60)
if len(data) < pe + 6 or data[pe:pe + 6] != b"PE\0\0\x64\xaa":
    p.error("EFI input is not AArch64")
if len(data) >= 60 * 1024 * 1024:
    p.error("input does not fit in the diagnostic image")

sector, total, start, size = 512, 147456, 2048, 131072
part_type = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
part_id, disk_id = uuid.uuid4(), uuid.uuid4()
entries = bytearray(128 * 128)
struct.pack_into("<16s16sQQQ", entries, 0, part_type.bytes_le, part_id.bytes_le,
                 start, start + size - 1, 0)
label = "T70 RAM Live".encode("utf-16le")
entries[56:56 + len(label)] = label
table_crc = zlib.crc32(entries)


def header(current, other, entry_lba):
    buf = bytearray(sector)
    struct.pack_into("<8sIIIIQQQQ16sQIII", buf, 0, b"EFI PART", 0x10000,
                     92, 0, 0, current, other, 34, total - 34, disk_id.bytes_le,
                     entry_lba, 128, 128, table_crc)
    struct.pack_into("<I", buf, 16, zlib.crc32(buf[:92]))
    return buf


pmbr = bytearray(sector)
struct.pack_into("<B3sB3sII", pmbr, 446, 0, b"\x00\x02\x00", 0xEE,
                 b"\xff\xff\xff", 1, total - 1)
pmbr[510:] = b"\x55\xaa"
with x.image.open("xb") as image:
    image.truncate(total * sector)
    image.write(pmbr)
    image.write(header(1, total - 1, 2))
    image.write(entries)
    image.seek((total - 33) * sector)
    image.write(entries)
    image.write(header(total - 1, 1, total - 33))

fat = f"{x.image.resolve()}@@{start * sector}"
subprocess.run(["mformat", "-i", fat, "-F", "-T", str(size), "-v", "T70LIVE", "::"], check=True)
subprocess.run(["mmd", "-i", fat, "::/EFI", "::/EFI/BOOT"], check=True)
subprocess.run(["mcopy", "-i", fat, str(x.efi), "::/EFI/BOOT/BOOTAA64.EFI"], check=True)
subprocess.run(["mdir", "-i", fat, "::/EFI/BOOT"], check=True)
