#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Offline saved-GPT inventory checker. JSON only; never opens a device/image."""
import argparse
import json
import os
from pathlib import Path
import stat


def integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f"invalid {name}")
    return value


def audit(data):
    if not isinstance(data, dict):
        raise ValueError("inventory must be an object")
    sector = integer(data.get("logical_sector_bytes"), "sector", 512)
    if sector & (sector - 1):
        raise ValueError("sector must be a power of two")
    disk = integer(data.get("physical_main_LUN_bytes"), "disk bytes", sector)
    first = integer(data.get("first_usable_lba"), "first usable LBA", 2)
    last = integer(data.get("last_usable_lba"), "last usable LBA", first)
    backup = integer(data.get("backup_header_lba"), "backup header LBA")
    if disk % sector or backup != disk // sector - 1 or last >= backup:
        raise ValueError("inconsistent disk/header/usable bounds")
    for key in ("primary_header_crc_valid", "partition_entries_crc_valid",
                "backup_header_crc_valid", "backup_entries_match_primary"):
        if data.get(key) is not True:
            raise ValueError(f"missing successful recorded evidence: {key}")
    rows = data.get("partitions")
    if not isinstance(rows, list) or integer(data.get("partition_count"), "partition count") != len(rows):
        raise ValueError("partition count mismatch")
    names, intervals = set(), []
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("invalid partition record")
        name = row.get("name")
        if not isinstance(name, str) or not name.strip() or name in names:
            raise ValueError("missing or duplicate partition name")
        names.add(name)
        start = integer(row.get("start_lba"), "partition start", first)
        end = integer(row.get("end_lba"), "partition end", start)
        offset = integer(row.get("offset_bytes"), "partition offset")
        size = integer(row.get("size_bytes"), "partition size", 1)
        if end > last or offset != start * sector or size != (end - start + 1) * sector:
            raise ValueError(f"inconsistent bounds/size: {name}")
        intervals.append((start, end, name))
    cursor, occupied, gaps = first, 0, []
    for start, end, name in sorted(intervals):
        if start < cursor:
            raise ValueError(f"overlapping partition: {name}")
        if start > cursor:
            gaps.append({"start_lba": cursor, "end_lba": start - 1,
                         "bytes": (start - cursor) * sector})
        occupied += (end - start + 1) * sector
        cursor = end + 1
    if cursor <= last:
        gaps.append({"start_lba": cursor, "end_lba": last,
                     "bytes": (last - cursor + 1) * sector})
    usable = (last - first + 1) * sector
    return {"scope": "saved JSON inventory only; CRC claims not independently recomputed",
            "occupied_bytes": occupied, "free_bytes": usable - occupied,
            "usable_bytes": usable, "largest_gap_bytes": max((g["bytes"] for g in gaps), default=0),
            "gaps": gaps,
            "recommendation": "preserve_existing_layout; assess_existing_gap_capacity" if gaps else
                              "preserve_existing_layout; additional_internal_OS_requires_reclaim",
            "limits": ["No filesystem free-space or shrinkability inference",
                       "No A/B slot, super, OTA or encrypted-data recovery guarantee",
                       "No partition modification or boot configuration performed"]}


def read_inventory(path):
    info = path.lstat()
    if path.suffix.lower() != ".json" or not stat.S_ISREG(info.st_mode) or info.st_size > 1024 * 1024:
        raise ValueError("input must be a nonsymlink regular .json file of at most 1 MiB")
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        opened = os.fstat(stream.fileno())
        if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
            raise ValueError("input changed while opening")
        raw = stream.read(1024 * 1024 + 1)
        if len(raw) > 1024 * 1024:
            raise ValueError("metadata exceeds 1 MiB")
    return json.loads(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inventory", type=Path)
    args = parser.parse_args()
    try:
        result = audit(read_inventory(args.inventory))
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
