#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Create and verify a separate GPT disk with a blank 32 MiB HFS+ volume."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
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
             ["with-build-lock", "T70Pro", "Generate isolated 32 MiB HFS+ GPT root fixture", "--",
              sys.executable, str(Path(__file__).resolve()), "--locked"])
sys.dont_write_bytecode = True

OUT = project / "tools/xnu-hfs-root-fixture"
ANALYSIS = project / "analysis/xnu-hfs-root-fixture.json"
VOLUME = OUT / "hfs-volume.raw"
DISK = OUT / "gpt-hfs-root.raw"
UUID_FILE = OUT / "root-matching-uuid.txt"
VOLUME_PENDING = OUT / "hfs-volume.pending.raw"
DISK_PENDING = OUT / "gpt-hfs-root.pending.raw"
UUID_PENDING = OUT / "root-matching-uuid.pending.txt"
PACKAGE = OUT / "packages/hfsprogs_540.1.linux3-6build1_arm64.deb"
TOOL_ROOT = OUT / "hfsprogs-root"
MKFS = TOOL_ROOT / "usr/sbin/mkfs.hfsplus"
FSCK = TOOL_ROOT / "usr/sbin/fsck.hfsplus"
PACKAGE_VERSION = "540.1.linux3-6build1"
PACKAGE_SHA256 = "d1076311cb019ad46cf8af15cf72c7ec389b2ae78db7e1e9c4d3c5736991f209"
FORMATTER_SHA256 = "b411fefc0cad289321fe7d46601b8a6e0d6dde954756b2f13fb89659c8f9eb4e"
APT_SOURCE = "http://ports.ubuntu.com/ubuntu-ports"
SECTOR = 512
VOLUME_BYTES = 32 * 1024 * 1024
VOLUME_SECTORS = VOLUME_BYTES // SECTOR
PARTITION_START = 2048
PARTITION_END = PARTITION_START + VOLUME_SECTORS - 1
DISK_SECTORS = PARTITION_END + 34
BACKUP_HEADER = DISK_SECTORS - 1
ENTRY_COUNT = 128
ENTRY_SIZE = 128
ARRAY_BYTES = ENTRY_COUNT * ENTRY_SIZE
ARRAY_SECTORS = ARRAY_BYTES // SECTOR
BACKUP_ENTRIES = BACKUP_HEADER - ARRAY_SECTORS
FIRST_USABLE = 2 + ARRAY_SECTORS
LAST_USABLE = BACKUP_ENTRIES - 1
APPLE_HFS_TYPE_GUID = uuid.UUID("48465300-0000-11aa-aa11-00306543ecac")
DISK_GUID = uuid.UUID("8a6ea231-e962-5c48-9ec3-8521312b84f1")
PARTITION_GUID = uuid.UUID("915c4ad8-28eb-5825-a65e-631dab379d8a")
PARTITION_LABEL = "T70 HFS+ root fixture"
VOLUME_LABEL = "T70-HFS-ROOT"


def sha256_file(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def parse_deb_fields(text):
    result = {}
    for line in text.splitlines():
        if line and not line[0].isspace() and ":" in line:
            key, value = line.split(":", 1)
            result[key] = value.strip()
    return result


def gpt_header(current, backup, entries_lba, entries_crc):
    raw = bytearray(struct.pack(
        "<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0,
        current, backup, FIRST_USABLE, LAST_USABLE, DISK_GUID.bytes_le,
        entries_lba, ENTRY_COUNT, ENTRY_SIZE, entries_crc))
    struct.pack_into("<I", raw, 16, zlib.crc32(raw[:92]) & 0xffffffff)
    return bytes(raw)


def rewrite_header_crc(image, offset):
    struct.pack_into("<I", image, offset + 16, 0)
    crc = zlib.crc32(image[offset:offset + 92]) & 0xffffffff
    struct.pack_into("<I", image, offset + 16, crc)


def make_gpt(volume):
    if len(volume) != VOLUME_BYTES:
        raise ValueError("HFS+ volume is not exactly 32 MiB")
    entries = bytearray(ARRAY_BYTES)
    entry = bytearray(ENTRY_SIZE)
    entry[0:16] = APPLE_HFS_TYPE_GUID.bytes_le
    entry[16:32] = PARTITION_GUID.bytes_le
    struct.pack_into("<QQQ", entry, 32, PARTITION_START, PARTITION_END, 0)
    entry[56:128] = PARTITION_LABEL.encode("utf-16le").ljust(72, b"\0")[:72]
    entries[:ENTRY_SIZE] = entry
    entries_crc = zlib.crc32(entries) & 0xffffffff
    image = bytearray(DISK_SECTORS * SECTOR)
    struct.pack_into("<B3sB3sII", image, 446, 0, b"\0\x02\0", 0xee,
                     b"\xff\xff\xff", 1, min(DISK_SECTORS - 1, 0xffffffff))
    image[510:512] = b"\x55\xaa"
    image[SECTOR:SECTOR + 92] = gpt_header(1, BACKUP_HEADER, 2, entries_crc)
    image[2 * SECTOR:2 * SECTOR + ARRAY_BYTES] = entries
    image[BACKUP_ENTRIES * SECTOR:BACKUP_ENTRIES * SECTOR + ARRAY_BYTES] = entries
    image[BACKUP_HEADER * SECTOR:BACKUP_HEADER * SECTOR + 92] = gpt_header(
        BACKUP_HEADER, 1, BACKUP_ENTRIES, entries_crc)
    image[PARTITION_START * SECTOR:(PARTITION_END + 1) * SECTOR] = volume
    return bytes(image)


def _header_at(image, lba, expected_current, expected_backup, expected_entries):
    offset = lba * SECTOR
    raw = image[offset:offset + 92]
    if len(raw) != 92:
        raise ValueError("GPT header exceeds the disk image")
    (signature, revision, size, stored_crc, reserved, current, backup, first,
     last, disk_guid, entries_lba, count, entry_size, entries_crc) = struct.unpack(
        "<8sIIIIQQQQ16sQIII", raw)
    copy = bytearray(raw)
    struct.pack_into("<I", copy, 16, 0)
    if (signature != b"EFI PART" or revision != 0x00010000 or size != 92 or reserved != 0 or
            zlib.crc32(copy) & 0xffffffff != stored_crc):
        raise ValueError("GPT header signature, version, size or CRC is invalid")
    if (current, backup, first, last, entries_lba, count, entry_size, disk_guid) != (
            expected_current, expected_backup, FIRST_USABLE, LAST_USABLE,
            expected_entries, ENTRY_COUNT, ENTRY_SIZE, DISK_GUID.bytes_le):
        raise ValueError("GPT header geometry or disk identity does not match")
    array = image[entries_lba * SECTOR:entries_lba * SECTOR + ARRAY_BYTES]
    if len(array) != ARRAY_BYTES or zlib.crc32(array) & 0xffffffff != entries_crc:
        raise ValueError("GPT partition-array CRC is invalid")
    return array


def _hfs_header(volume, offset):
    raw = volume[offset:offset + 512]
    if len(raw) != 512:
        raise ValueError("HFS+ volume header exceeds the partition")
    signature, version = raw[:2].decode("ascii", "replace"), struct.unpack_from(">H", raw, 2)[0]
    attributes = struct.unpack_from(">I", raw, 4)[0]
    files, folders, block_size, total_blocks, free_blocks = struct.unpack_from(">IIIII", raw, 32)
    if (signature != "H+" or version != 4 or block_size < SECTOR or block_size % SECTOR or
            total_blocks == 0 or block_size * total_blocks != VOLUME_BYTES or free_blocks > total_blocks):
        raise ValueError("HFS+ volume header signature or geometry is invalid")
    return {"signature": signature, "version": version, "attributes": attributes,
            "file_count": files, "folder_count": folders, "block_size": block_size,
            "total_blocks": total_blocks, "free_blocks": free_blocks}


def verify_disk(image):
    if len(image) != DISK_SECTORS * SECTOR or len(image) % SECTOR:
        raise ValueError("GPT disk image size is incorrect")
    if image[510:512] != b"\x55\xaa":
        raise ValueError("Protective MBR signature is invalid")
    _, _, mbr_type, _, mbr_start, mbr_sectors = struct.unpack_from("<B3sB3sII", image, 446)
    if (mbr_type, mbr_start, mbr_sectors) != (0xee, 1, DISK_SECTORS - 1):
        raise ValueError("Protective MBR entry is invalid")
    primary = _header_at(image, 1, 1, BACKUP_HEADER, 2)
    backup = _header_at(image, BACKUP_HEADER, BACKUP_HEADER, 1, BACKUP_ENTRIES)
    if primary != backup:
        raise ValueError("Primary and backup GPT arrays differ")
    entry = primary[:ENTRY_SIZE]
    (start, end, attributes) = struct.unpack_from("<QQQ", entry, 32)
    if (entry[:16] != APPLE_HFS_TYPE_GUID.bytes_le or entry[16:32] != PARTITION_GUID.bytes_le or
            start != PARTITION_START or end != PARTITION_END or attributes != 0 or
            any(primary[ENTRY_SIZE:])):
        raise ValueError("Apple HFS partition entry or geometry is invalid")
    name = entry[56:128].decode("utf-16le").split("\0", 1)[0]
    if name != PARTITION_LABEL:
        raise ValueError("GPT partition label does not match the fixture")
    volume = image[PARTITION_START * SECTOR:(PARTITION_END + 1) * SECTOR]
    if len(volume) != VOLUME_BYTES:
        raise ValueError("HFS+ partition length is incorrect")
    primary_hfs = _hfs_header(volume, 1024)
    alternate_hfs = _hfs_header(volume, VOLUME_BYTES - 1024)
    for key in ("signature", "version", "block_size", "total_blocks"):
        if primary_hfs[key] != alternate_hfs[key]:
            raise ValueError("Primary and alternate HFS+ volume headers disagree")
    return {"sector_bytes": SECTOR, "disk_bytes": len(image), "disk_sectors": DISK_SECTORS,
            "disk_guid": str(DISK_GUID).upper(), "partition_guid": str(PARTITION_GUID).upper(),
            "partition_type_guid": str(APPLE_HFS_TYPE_GUID).upper(),
            "partition_label": name, "partition_start_lba": start, "partition_end_lba": end,
            "partition_bytes": len(volume), "primary_header_crc_valid": True,
            "backup_header_crc_valid": True, "primary_partition_array_crc_valid": True,
            "backup_partition_array_crc_valid": True, "protective_mbr_valid": True,
            "primary_hfs_volume_header": primary_hfs, "alternate_hfs_volume_header": alternate_hfs,
            "volume_sha256": sha256_bytes(volume)}


def malformed_input_checks(image):
    checks = []
    def reject(name, mutate):
        candidate = bytearray(image)
        mutate(candidate)
        try:
            verify_disk(candidate)
        except (ValueError, struct.error) as error:
            checks.append({"name": name, "rejected": True, "reason": str(error)})
        else:
            raise RuntimeError("Malformed fixture self-check was accepted: " + name)
    reject("primary GPT header CRC damage", lambda b: b.__setitem__(SECTOR + 16, b[SECTOR + 16] ^ 1))
    reject("backup GPT header CRC damage", lambda b: b.__setitem__(BACKUP_HEADER * SECTOR + 16,
                                                                    b[BACKUP_HEADER * SECTOR + 16] ^ 1))
    reject("partition-array damage", lambda b: b.__setitem__(2 * SECTOR + 60, b[2 * SECTOR + 60] ^ 1))
    def bad_geometry(b):
        struct.pack_into("<Q", b, SECTOR + 24, 2)
        rewrite_header_crc(b, SECTOR)
    reject("invalid geometry with recomputed header CRC", bad_geometry)
    reject("primary HFS+ header signature damage",
           lambda b: b.__setitem__(PARTITION_START * SECTOR + 1024, ord("X")))
    reject("alternate HFS+ header signature damage",
           lambda b: b.__setitem__((PARTITION_END + 1) * SECTOR - 1024, ord("X")))
    reject("HFS+ allocation geometry damage",
           lambda b: struct.pack_into(">I", b, PARTITION_START * SECTOR + 1024 + 40, 3))
    return checks


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    previous_attempt = None
    if ANALYSIS.exists():
        previous_attempt = json.loads(ANALYSIS.read_text())
        if (previous_attempt.get("status") != "failed" or
                previous_attempt.get("formatter", {}).get("format_exit_code") != 0 or
                not VOLUME_PENDING.exists() or not DISK_PENDING.exists()):
            raise RuntimeError("Refuse to replace an unrelated existing fixture report or partial image")
    elif VOLUME_PENDING.exists() or DISK_PENDING.exists():
        raise RuntimeError("Refuse to reuse partial images without their matching failure report")
    for path in (VOLUME, DISK, UUID_FILE, UUID_PENDING):
        if path.exists():
            raise RuntimeError("Refuse to overwrite existing fixture output: " + str(path))
    old_fixture = project / "tools/xnu-qemu-root-fixture/gpt-root.raw"
    old_report = project / "analysis/xnu-qemu-root-fixture.json"
    old_raw_sha = sha256_file(old_fixture)
    old_report_sha = sha256_file(old_report)
    old_report_data = json.loads(old_report.read_text())
    old_uuid = old_report_data["verification"]["partition_guid"]
    if old_raw_sha != old_report_data["sha256"]:
        raise RuntimeError("Existing 4 MiB GPT fixture no longer matches its report")

    apt_policy = subprocess.check_output(["apt-cache", "policy", "hfsprogs"], text=True)
    apt_metadata = parse_deb_fields(subprocess.check_output(["apt-cache", "show", "hfsprogs"], text=True))
    if (APT_SOURCE not in apt_policy or "resolute/universe arm64 Packages" not in apt_policy or
            apt_metadata.get("Version") != PACKAGE_VERSION or apt_metadata.get("Architecture") != "arm64" or
            apt_metadata.get("SHA256") != PACKAGE_SHA256 or
            apt_metadata.get("Filename") != "pool/universe/h/hfsprogs/" + PACKAGE.name):
        raise RuntimeError("hfsprogs APT candidate differs from the verified official metadata")
    if sha256_file(PACKAGE) != PACKAGE_SHA256 or not MKFS.is_file() or not FSCK.is_file():
        raise RuntimeError("Project-local hfsprogs package or extracted tools are missing or changed")
    formatter_sha = sha256_file(MKFS)
    fsck_sha = sha256_file(FSCK)
    if formatter_sha != FORMATTER_SHA256:
        raise RuntimeError("Extracted formatter binary hash differs from the inspected package")
    dependencies = subprocess.run(["ldd", str(MKFS)], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, check=True).stdout
    if "not found" in dependencies:
        raise RuntimeError("Project-local formatter has an unresolved shared library")

    report = {"status": "running", "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "format": "GPT Apple_HFS partition containing a blank HFS+ volume",
              "filesystem_type": "HFS+", "mounted": False, "read_only_attachment_required": True,
              "device_boot": "not_run", "filesystem_mounted": False,
              "tool_acquisition": {"method": "apt-get download; dpkg-deb -x",
                  "extracted_to": str(TOOL_ROOT), "project_local_only": True,
                  "global_package_installation_performed": False},
              "legacy_fixture_preserved": {"path": str(old_fixture), "sha256_before": old_raw_sha,
                  "report_path": str(old_report), "report_sha256_before": old_report_sha,
                  "partition_guid": old_uuid}}
    if previous_attempt:
        report["recovered_previous_attempt"] = {"status": previous_attempt.get("status"),
            "error": previous_attempt.get("error"),
            "formatter_output_sha256": previous_attempt.get("formatter", {}).get("formatted_volume_sha256")}
    try:
        if previous_attempt:
            report["formatter"] = previous_attempt["formatter"]
            volume_sha = sha256_file(VOLUME_PENDING)
            if (volume_sha != report["formatter"].get("formatted_volume_sha256") or
                    VOLUME_PENDING.stat().st_size != VOLUME_BYTES):
                raise RuntimeError("Pending formatted volume does not match its failed-stage report")
        else:
            preflight_command = [str(MKFS), "-N", "32m", "-v", VOLUME_LABEL]
            preflight = subprocess.run(preflight_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, check=True)
            with VOLUME_PENDING.open("xb") as stream:
                stream.truncate(VOLUME_BYTES)
            blank_sha = sha256_file(VOLUME_PENDING)
            format_command = [str(MKFS), "-v", VOLUME_LABEL, str(VOLUME_PENDING)]
            formatted = subprocess.run(format_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True)
            report["formatter"] = {"package": "hfsprogs", "package_version": PACKAGE_VERSION,
                "apt_origin": APT_SOURCE, "apt_suite_component_arch": "resolute/universe/arm64",
                "apt_filename": apt_metadata["Filename"], "apt_package_sha256": PACKAGE_SHA256,
                "formatter_path": str(MKFS), "formatter_sha256": formatter_sha,
                "fsck_path": str(FSCK), "fsck_sha256": fsck_sha, "ldd": dependencies.strip(),
                "preflight_command": preflight_command, "preflight_exit_code": preflight.returncode,
                "preflight_output": preflight.stdout,
                "format_command": format_command, "format_input_bytes": VOLUME_BYTES,
                "format_input_sha256_all_zero": blank_sha, "format_exit_code": formatted.returncode,
                "format_output": formatted.stdout}
            if formatted.returncode:
                raise RuntimeError("mkfs.hfsplus failed: " + formatted.stdout[-1500:])
            if VOLUME_PENDING.stat().st_size != VOLUME_BYTES:
                raise RuntimeError("Formatter changed the requested 32 MiB volume size")
            volume_sha = sha256_file(VOLUME_PENDING)
            report["formatter"]["formatted_volume_sha256"] = volume_sha

        if VOLUME_PENDING.stat().st_size != VOLUME_BYTES:
            raise RuntimeError("Formatted HFS+ volume is not exactly 32 MiB")
        checked = subprocess.run([str(FSCK), "-n", str(VOLUME_PENDING)],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        fsck_hash_after = sha256_file(VOLUME_PENDING)
        report["read_only_fsck"] = {"command": [str(FSCK), "-n", str(VOLUME_PENDING)],
            "exit_code": checked.returncode, "output": checked.stdout,
            "volume_sha256_before": volume_sha, "volume_sha256_after": fsck_hash_after,
            "read_only_no_repair": "(NO WRITE)" in checked.stdout,
            "non_journaled": "Checking non-journaled HFS Plus Volume." in checked.stdout,
            "catalog_hierarchy_checked": "Checking catalog hierarchy." in checked.stdout,
            "standard_clean": "appears to be OK." in checked.stdout}
        if (checked.returncode or fsck_hash_after != volume_sha or
                not report["read_only_fsck"]["read_only_no_repair"] or
                not report["read_only_fsck"]["non_journaled"] or
                not report["read_only_fsck"]["catalog_hierarchy_checked"] or
                not report["read_only_fsck"]["standard_clean"]):
            raise RuntimeError("Read-only fsck did not confirm a clean, unjournaled, valid HFS+ root")

        volume = VOLUME_PENDING.read_bytes()
        image = make_gpt(volume)
        if DISK_PENDING.exists():
            disk_bytes = DISK_PENDING.read_bytes()
            if sha256_bytes(disk_bytes) != sha256_bytes(image):
                raise RuntimeError("Pending GPT disk differs from the formatted HFS+ volume")
        else:
            with DISK_PENDING.open("xb") as stream:
                stream.write(image)
            disk_bytes = DISK_PENDING.read_bytes()
        verification = verify_disk(disk_bytes)
        verification["filesystem_type"] = "HFS+"
        verification["empty_volume"] = (verification["primary_hfs_volume_header"]["file_count"] == 0 and
                                         verification["primary_hfs_volume_header"]["folder_count"] == 0)
        verification["standard_clean_unjournaled_root"] = (
            report["read_only_fsck"]["standard_clean"] and report["read_only_fsck"]["non_journaled"] and
            report["read_only_fsck"]["catalog_hierarchy_checked"])
        self_checks = malformed_input_checks(disk_bytes)
        if len(self_checks) != 7 or not all(item["rejected"] for item in self_checks):
            raise RuntimeError("HFS+ GPT malformed-input self-checks are incomplete")
        if not verification["empty_volume"] or not verification["standard_clean_unjournaled_root"]:
            raise RuntimeError("Formatted HFS+ volume is not an empty, clean, unjournaled root volume")
        if (sha256_file(old_fixture) != old_raw_sha or sha256_file(old_report) != old_report_sha or
                json.loads(old_report.read_text())["verification"]["partition_guid"] != old_uuid):
            raise RuntimeError("The existing 4 MiB fixture or its UUID report changed")

        os.replace(VOLUME_PENDING, VOLUME)
        os.replace(DISK_PENDING, DISK)
        UUID_PENDING.write_text(str(PARTITION_GUID).upper() + "\n")
        os.replace(UUID_PENDING, UUID_FILE)
        final_disk_sha = sha256_file(DISK)
        final_volume_sha = sha256_file(VOLUME)
        final_verification = verify_disk(DISK.read_bytes())
        fsck = report["read_only_fsck"]
        final_verification["filesystem_type"] = "HFS+"
        final_verification["empty_volume"] = (
            final_verification["primary_hfs_volume_header"]["file_count"] == 0 and
            final_verification["primary_hfs_volume_header"]["folder_count"] == 0)
        final_verification["standard_clean"] = fsck["standard_clean"]
        final_verification["non_journaled"] = fsck["non_journaled"]
        final_verification["root_catalog_valid"] = (
            fsck["read_only_no_repair"] and fsck["catalog_hierarchy_checked"] and fsck["standard_clean"])
        if (final_verification["volume_sha256"] != final_volume_sha or
                not final_verification["empty_volume"] or not final_verification["standard_clean"] or
                not final_verification["non_journaled"] or not final_verification["root_catalog_valid"]):
            raise RuntimeError("Final disk partition is not an empty, clean, unjournaled valid HFS+ root volume")
        report.update(status="verified_synthetic_hfsplus_gpt_fixture", path=str(DISK),
            volume_path=str(VOLUME), root_matching_uuid_path=str(UUID_FILE),
            bytes=DISK.stat().st_size, disk_bytes=DISK.stat().st_size, volume_bytes=VOLUME.stat().st_size,
            sha256=final_disk_sha, disk_sha256=final_disk_sha, volume_sha256=final_volume_sha,
            root_matching_uuid=str(PARTITION_GUID).upper(),
            root_matching_uuid_file_sha256=sha256_file(UUID_FILE),
            verification=final_verification, malformed_input_self_checks=self_checks,
            legacy_fixture_preserved={"path": str(old_fixture), "sha256_before": old_raw_sha,
                "sha256_after": sha256_file(old_fixture), "report_path": str(old_report),
                "report_sha256_before": old_report_sha, "report_sha256_after": sha256_file(old_report),
                "partition_guid": old_uuid,
                "unchanged": sha256_file(old_fixture) == old_raw_sha and sha256_file(old_report) == old_report_sha})
        if not report["legacy_fixture_preserved"]["unchanged"]:
            raise RuntimeError("The existing GPT fixture or UUID report changed")
        print("PREPARED:", DISK, DISK.stat().st_size, "bytes; HFS+", final_volume_sha)
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        report["builder_script_sha256"] = sha256_file(Path(__file__))
        ANALYSIS.parent.mkdir(parents=True, exist_ok=True)
        ANALYSIS.write_text(json.dumps(report, indent=2) + "\n")
        print("Report:", ANALYSIS)

if __name__ == "__main__":
    main()
