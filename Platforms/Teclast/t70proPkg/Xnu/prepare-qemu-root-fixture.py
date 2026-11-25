#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build and verify a small synthetic GPT image for QEMU root selection only."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import uuid
import zlib

here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Prepare synthetic read-only root-selection fixture", "--",
              sys.executable, str(Path(__file__).resolve()), "--locked"])

SECTOR = 512
SECTORS = 8192
ENTRY_COUNT = 128
ENTRY_SIZE = 128
ARRAY_BYTES = ENTRY_COUNT * ENTRY_SIZE
ARRAY_SECTORS = ARRAY_BYTES // SECTOR
BACKUP_HEADER = SECTORS - 1
BACKUP_ENTRIES = BACKUP_HEADER - ARRAY_SECTORS
FIRST_USABLE = 2 + ARRAY_SECTORS
LAST_USABLE = BACKUP_ENTRIES - 1
PARTITION_START = 2048
PARTITION_END = LAST_USABLE - 1
DISK_GUID = uuid.UUID("70d07e70-5eab-4ae4-9d58-f96c785cda71")
PARTITION_GUID = uuid.UUID("70d07e70-5eab-4ae4-9d58-f96c785cda72")
APPLE_HFS_TYPE_GUID = uuid.UUID("48465300-0000-11aa-aa11-00306543ecac")
PARTITION_LABEL = "T70 XNU root-selection fixture"
SECTOR_MARKER = b"T70PRO-SYNTHETIC-GPT-READONLY-DMA-FIXTURE\0"


def _header(current, backup, entries_lba, entries_crc):
    raw = bytearray(struct.pack(
        "<8sIIIIQQQQ16sQIII",
        b"EFI PART", 0x00010000, 92, 0, 0, current, backup,
        FIRST_USABLE, LAST_USABLE, DISK_GUID.bytes_le, entries_lba,
        ENTRY_COUNT, ENTRY_SIZE, entries_crc))
    struct.pack_into("<I", raw, 16, zlib.crc32(raw[:92]) & 0xffffffff)
    return bytes(raw)


def _header_at(image, lba, expected_current, expected_backup, expected_entries):
    start = lba * SECTOR
    raw = image[start:start + 92]
    if len(raw) != 92:
        raise ValueError("GPT header exceeds the image")
    signature, revision, size, stored_crc, reserved, current, backup, first, last, disk, entries_lba, count, entry_size, entries_crc = struct.unpack("<8sIIIIQQQQ16sQIII", raw)
    copy = bytearray(raw)
    struct.pack_into("<I", copy, 16, 0)
    if (signature != b"EFI PART" or revision != 0x00010000 or size != 92 or reserved != 0 or
            zlib.crc32(copy) & 0xffffffff != stored_crc):
        raise ValueError("GPT header signature, version, size or CRC is invalid")
    if (current, backup, first, last, entries_lba, count, entry_size, disk) != (
            expected_current, expected_backup, FIRST_USABLE, LAST_USABLE,
            expected_entries, ENTRY_COUNT, ENTRY_SIZE, DISK_GUID.bytes_le):
        raise ValueError("GPT header geometry or disk identity does not match the fixture")
    array = image[entries_lba * SECTOR:entries_lba * SECTOR + ARRAY_BYTES]
    if len(array) != ARRAY_BYTES or zlib.crc32(array) & 0xffffffff != entries_crc:
        raise ValueError("GPT partition-array CRC is invalid")
    return array


def verify(image):
    if len(image) != SECTORS * SECTOR:
        raise ValueError("GPT fixture size is incorrect")
    mbr = image[:SECTOR]
    if mbr[510:512] != b"\x55\xaa":
        raise ValueError("Protective MBR signature is invalid")
    partitions = [mbr[446 + i * 16:462 + i * 16] for i in range(4)]
    protective = [p for p in partitions if p[4] == 0xee]
    if (len(protective) != 1 or protective[0][0] not in (0, 0x80) or
            struct.unpack_from("<II", protective[0], 8) != (1, SECTORS - 1)):
        raise ValueError("Protective MBR does not cover the disk geometry")
    primary = _header_at(image, 1, 1, BACKUP_HEADER, 2)
    backup = _header_at(image, BACKUP_HEADER, BACKUP_HEADER, 1, BACKUP_ENTRIES)
    if primary != backup:
        raise ValueError("Primary and backup GPT arrays differ")
    entry = primary[:ENTRY_SIZE]
    type_guid, unique_guid, first, last, flags, name = struct.unpack("<16s16sQQQ72s", entry)
    if (type_guid != APPLE_HFS_TYPE_GUID.bytes_le or unique_guid != PARTITION_GUID.bytes_le or
            first != PARTITION_START or last != PARTITION_END or flags != 0 or
            name.decode("utf-16le").rstrip("\0") != PARTITION_LABEL):
        raise ValueError("Synthetic partition identity or extent is invalid")
    at = PARTITION_START * SECTOR
    if image[at:at + len(SECTOR_MARKER)] != SECTOR_MARKER:
        raise ValueError("Synthetic data sector does not contain its expected marker")
    if any(primary[ENTRY_SIZE:]):
        raise ValueError("Unused GPT entries are not empty")
    return {"sector_bytes": SECTOR, "disk_guid": str(DISK_GUID).upper(),
            "partition_guid": str(PARTITION_GUID).upper(),
            "partition_type_guid": str(APPLE_HFS_TYPE_GUID).upper(),
            "partition_start_lba": PARTITION_START, "partition_end_lba": PARTITION_END,
            "primary_header_crc_valid": True, "backup_header_crc_valid": True,
            "primary_partition_array_crc_valid": True, "backup_partition_array_crc_valid": True,
            "protective_mbr_valid": True,
            "first_sector_sha256": hashlib.sha256(image[:SECTOR]).hexdigest(),
            "partition_first_sector_sha256": hashlib.sha256(image[at:at + SECTOR]).hexdigest()}


def _repair_header_crc(image, lba):
    start = lba * SECTOR
    header = bytearray(image[start:start + 92])
    struct.pack_into("<I", header, 16, 0)
    struct.pack_into("<I", header, 16, zlib.crc32(header) & 0xffffffff)
    image[start:start + 92] = header


def self_check(data):
    cases = []

    def rejects(label, damaged):
        try:
            verify(bytes(damaged))
        except ValueError:
            cases.append(label)
            return
        raise AssertionError("Fixture verifier accepted " + label)

    damaged = bytearray(data)
    damaged[SECTOR + 16] ^= 1
    rejects("primary header CRC damage", damaged)

    damaged = bytearray(data)
    damaged[BACKUP_HEADER * SECTOR + 16] ^= 1
    rejects("backup header CRC damage", damaged)

    damaged = bytearray(data)
    damaged[2 * SECTOR + 8] ^= 1
    rejects("partition-array damage", damaged)

    damaged = bytearray(data)
    struct.pack_into("<Q", damaged, SECTOR + 24, 2)
    _repair_header_crc(damaged, 1)
    rejects("invalid geometry with recomputed header CRC", damaged)

    damaged = bytearray(data)
    damaged[PARTITION_START * SECTOR] ^= 1
    rejects("payload marker damage", damaged)
    return cases


def build():
    image = bytearray(SECTORS * SECTOR)
    image[510:512] = b"\x55\xaa"
    image[446:462] = struct.pack("<B3sB3sII", 0, b"\0" * 3, 0xee, b"\xff" * 3, 1, SECTORS - 1)
    entries = bytearray(ARRAY_BYTES)
    name = PARTITION_LABEL.encode("utf-16le")[:72].ljust(72, b"\0")
    entries[:ENTRY_SIZE] = struct.pack("<16s16sQQQ72s", APPLE_HFS_TYPE_GUID.bytes_le,
                                       PARTITION_GUID.bytes_le, PARTITION_START,
                                       PARTITION_END, 0, name)
    array_crc = zlib.crc32(entries) & 0xffffffff
    primary = _header(1, BACKUP_HEADER, 2, array_crc)
    backup = _header(BACKUP_HEADER, 1, BACKUP_ENTRIES, array_crc)
    image[SECTOR:SECTOR + len(primary)] = primary
    image[2 * SECTOR:2 * SECTOR + ARRAY_BYTES] = entries
    backup_at = BACKUP_ENTRIES * SECTOR
    image[backup_at:backup_at + ARRAY_BYTES] = entries
    end = BACKUP_HEADER * SECTOR
    image[end:end + len(backup)] = backup
    at = PARTITION_START * SECTOR
    image[at:at + len(SECTOR_MARKER)] = SECTOR_MARKER
    image[at + 0x100:at + 0x110] = bytes(range(16))
    return bytes(image)


output = project / "tools/xnu-qemu-root-fixture/gpt-root.raw"
report_path = project / "analysis/xnu-qemu-root-fixture.json"
data = build()
facts = verify(data)
facts["malformed_input_self_checks"] = self_check(data)
digest = hashlib.sha256(data).hexdigest()
output.parent.mkdir(parents=True, exist_ok=True)
if output.exists():
    if output.read_bytes() != data:
        raise SystemExit("An existing different root fixture was preserved")
else:
    with tempfile.NamedTemporaryFile(prefix="t70-gpt-", dir=output.parent, delete=False) as stream:
        temp = Path(stream.name)
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, output)
if hashlib.sha256(output.read_bytes()).hexdigest() != digest:
    raise SystemExit("Written synthetic GPT image failed SHA-256 read-back")
report = {"status": "fixture_verified_not_filesystem", "format": "raw GPT; APFS/HFS filesystem absent",
          "bytes": len(data), "sha256": digest, "path": str(output), "device_boot": "not_run",
          "mounted": False, "read_only_attachment_required": True,
          "verification": facts, "finished_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
report_path.write_text(json.dumps(report, indent=2) + "\n")
print("PASS: protective MBR; primary/backup GPT headers and partition arrays; expected partition UUID and payload sector")
print("PASS: rejected", len(facts["malformed_input_self_checks"]), "damaged GPT/fixture inputs")
print("Synthetic partition:", PARTITION_GUID, "LBA", PARTITION_START, "to", PARTITION_END)
print("No filesystem is formatted in this image; QEMU must attach it read-only.")
print("Image SHA-256:", digest)
print("Report:", report_path)
