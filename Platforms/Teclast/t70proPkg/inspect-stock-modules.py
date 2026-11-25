#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Read stock super/module metadata from the original backup; never mount or write it.

Logical partition images exist only in anonymous VM memory for read-only filesystem tools.
Format: AOSP system/core fs_mgr/liblp/include/liblp/metadata_format.h.
"""
import argparse
import hashlib
import importlib.util
import gzip
import json
import os
from pathlib import Path
import re
import struct
import subprocess


# Reuse the bounded ELF/export parser; neither module has import-time device I/O.
_elf_spec = importlib.util.spec_from_file_location(
    "t70_stock_elf", Path(__file__).parent / "Linux/prepare-stock-modules.py")
_elf_parser = importlib.util.module_from_spec(_elf_spec)
_elf_spec.loader.exec_module(_elf_parser)


def read_at(stream, offset, length):
    stream.seek(offset)
    data = stream.read(length)
    if len(data) != length:
        raise ValueError("Truncated backup range")
    return data


def logical_partitions(stream, super_part):
    base, size = super_part["offset_bytes"], super_part["size_bytes"]
    geometry = read_at(stream, base + 4096, 4096)
    magic, length = struct.unpack_from("<II", geometry)
    if magic != 0x616C4467 or length != 52:
        raise ValueError("Unsupported LP geometry")
    checked = bytearray(geometry[:length]); checked[8:40] = bytes(32)
    if hashlib.sha256(checked).digest() != geometry[8:40]:
        raise ValueError("LP geometry checksum mismatch")
    maximum, slots, block = struct.unpack_from("<III", geometry, 40)
    if not (128 <= maximum <= 4 * 1024 * 1024 and 1 <= slots <= 4 and block == 4096):
        raise ValueError("Unexpected LP geometry dimensions")
    if 12288 + maximum * slots * 2 > size:
        raise ValueError("LP metadata exceeds super")
    metadata = read_at(stream, base + 12288, maximum)
    magic, major, minor, header_size = struct.unpack_from("<IHHI", metadata)
    if magic != 0x414C5030 or major != 10 or minor > 2 or header_size not in (128, 256):
        raise ValueError("Unsupported LP metadata header")
    checked = bytearray(metadata[:header_size]); checked[12:44] = bytes(32)
    if hashlib.sha256(checked).digest() != metadata[12:44]:
        raise ValueError("LP header checksum mismatch")
    tables_size = struct.unpack_from("<I", metadata, 44)[0]
    if header_size + tables_size > maximum:
        raise ValueError("LP table bounds mismatch")
    tables = metadata[header_size:header_size + tables_size]
    if hashlib.sha256(tables).digest() != metadata[48:80]:
        raise ValueError("LP table checksum mismatch")

    def entries(offset, expected):
        start, count, width = struct.unpack_from("<III", metadata, offset)
        if width != expected or start + count * width > len(tables):
            raise ValueError("LP descriptor bounds mismatch")
        return [tables[start + i * width:start + (i + 1) * width] for i in range(count)]

    extents = [struct.unpack("<QIQI", x) for x in entries(92, 24)]
    devices = entries(116, 64)
    if len(devices) != 1:
        raise ValueError("Multiple physical super devices require an explicit mapping")
    first_sector, _, _, device_size = struct.unpack_from("<QIIQ", devices[0])
    if device_size != size:
        raise ValueError("Physical super size mismatch")
    result = {}
    for entry in entries(80, 52):
        name = entry[:36].split(b"\0", 1)[0].decode("ascii")
        if not re.fullmatch(r"[A-Za-z0-9_]+", name) or name in result:
            raise ValueError("Invalid or duplicate LP partition name")
        attributes, first, count, group = struct.unpack_from("<4I", entry, 36)
        if first + count > len(extents):
            raise ValueError("LP partition extent bounds mismatch")
        mapped = []
        for sectors, kind, target, source in extents[first:first + count]:
            if source != 0 or kind not in (0, 1):
                raise ValueError("Unsupported LP extent target")
            if kind == 0 and (target < first_sector or (target + sectors) * 512 > size):
                raise ValueError("LP physical extent exceeds super")
            mapped.append({"bytes": sectors * 512, "type": kind, "super_offset": target * 512})
        result[name] = {"bytes": sum(e["bytes"] for e in mapped), "attributes": attributes, "extents": mapped}
    flags = struct.unpack_from("<I", metadata, 128)[0] if header_size >= 256 else 0
    return result, {"slot_read": 0, "version": [major, minor], "geometry_header_tables_sha256_valid": True,
                    "header_flags": flags, "virtual_ab_flag": bool(flags & 1), "metadata_slots": slots}


class ReadOnlyErofs:
    """Bounded, seek-free view for this backup's single-device EROFS files.

    No image copy, mount, device node or writable file handle is used. Read is
    a caller-owned (offset,length)->bytes view of one authorized partition.
    Supported: 4KiB flat/inline and non-big-cluster LZ4 full/compact indexes.
    Unsupported features fail closed. Format reference (dual licensed):
    https://github.com/torvalds/linux/blob/v6.6/fs/erofs/erofs_fs.h
    """
    MAX_FILE = 16 * 1024 * 1024

    def __init__(self, read, size):
        self._read = read
        self.size = size
        h = self.read(1024, 128)
        magic, _, compat = struct.unpack_from("<III", h)
        self.block = 1 << h[12]
        self.root = struct.unpack_from("<H", h, 14)[0]
        blocks, self.meta = struct.unpack_from("<II", h, 36)
        self.incompat = struct.unpack_from("<I", h, 80)[0]
        if magic != 0xe0f5e1e2 or self.block != 4096 or h[13] or self.incompat & ~1 or blocks * self.block > size or self.meta * self.block >= size:
            raise ValueError("Unsupported EROFS superblock/features")
        if compat & 1:
            checked = bytearray(self.read(1024, self.block - 1024))
            expected = struct.unpack_from("<I", checked, 4)[0]
            checked[4:8] = bytes(4)
            if self.crc32c(checked) != expected:
                raise ValueError("EROFS superblock CRC32C mismatch")

    @staticmethod
    def crc32c(data):
        value = 0xffffffff
        for byte in data:
            value ^= byte
            for _ in range(8):
                value = (value >> 1) ^ (0x82f63b78 if value & 1 else 0)
        return value

    def read(self, offset, length):
        if not isinstance(offset, int) or not isinstance(length, int) or offset < 0 or length < 0 or offset + length > self.size:
            raise ValueError("EROFS range outside authorized partition")
        data = self._read(offset, length)
        if len(data) != length:
            raise ValueError("Truncated EROFS read")
        return data

    def inode(self, nid):
        if not isinstance(nid, int) or nid < 0:
            raise ValueError("Invalid EROFS inode")
        at = self.meta * self.block + nid * 32
        h = self.read(at, 32)
        fmt, xattrs, mode = struct.unpack_from("<HHH", h)
        if fmt & ~15 or (fmt >> 1) not in (0, 1, 2, 3):
            raise ValueError("Unsupported EROFS inode format")
        head = 64 if fmt & 1 else 32
        if head == 64:
            self.read(at, head)
        length = struct.unpack_from("<Q" if fmt & 1 else "<I", h, 8)[0]
        if length > self.MAX_FILE:
            raise ValueError("EROFS file exceeds bounded extraction size")
        tail = at + head + (12 + (xattrs - 1) * 4 if xattrs else 0)
        self.read(tail, 0)
        return {"nid": nid, "offset": at, "layout": (fmt >> 1) & 7,
                "size": length, "mode": mode, "raw": struct.unpack_from("<I", h, 16)[0], "inline": tail}

    def contents(self, inode):
        size, block = inode["size"], self.block
        if inode["mode"] & 0xf000 not in (0x8000, 0x4000, 0xa000):
            raise ValueError("Not an EROFS regular file, directory or link")
        if inode["layout"] in (0, 2):
            full = size // block if inode["layout"] == 2 else (size + block - 1) // block
            n = min(size, full * block)
            result = self.read(inode["raw"] * block, n) if n else b""
            if n < size:
                if inode["inline"] // block != (inode["inline"] + size - n - 1) // block:
                    raise ValueError("EROFS inline tail crosses a block")
                result += self.read(inode["inline"], size - n)
            return result
        if not size:
            return b""
        import lz4.block
        start = (inode["inline"] + 7) & ~7
        reserved, advise, algorithms, bits = struct.unpack("<IHBB", self.read(start, 8))
        if reserved or advise & ~1 or algorithms or bits:
            raise ValueError("Unsupported EROFS compression configuration")
        count = (size + block - 1) // block
        entries = []
        if inode["layout"] == 1:
            for i in range(count):
                kind, offset, physical = struct.unpack("<HHI", self.read(start + 16 + i * 8, 8))
                if kind & ~3:
                    raise ValueError("Unsupported EROFS full-index flags")
                entries.append((kind, offset, physical))
        else:
            base = start + 8
            initial = ((32 - base % 32) // 4) % 8
            middle = ((count - initial) // 16) * 16 if advise & 1 and count > initial else 0
            # Physical addresses are a pack base plus its number of preceding
            # heads. Decode each whole pack once rather than kernel lookback.
            for i in range(count):
                shift = 2 if i < initial or i >= initial + middle else 1
                position = base + (i * 4 if i < initial else initial * 4 +
                           ((i - initial) * 2 if i < initial + middle else middle * 2 + (i - initial - middle) * 4))
                width, number = (8, 2) if shift == 2 else (32, 16)
                packed = self.read(position // width * width, width)
                encbits = (width - 4) * 8 // number
                slot = (position % width) >> shift
                values = [(int.from_bytes(packed[:-4], "little") >> (j * encbits)) & ((1 << encbits) - 1) for j in range(number)]
                value = values[slot];kind = (value >> 12) & 3;offset = value & 4095
                physical = struct.unpack_from("<I", packed, width - 4)[0] + 1 + sum(((x >> 12) & 3) != 2 for x in values[:slot])
                entries.append((kind, offset, physical))
        heads = []
        for i, (kind, offset, physical) in enumerate(entries):
            if kind == 2:
                if not heads:
                    raise ValueError("EROFS compressed file starts with a non-head")
                continue
            if offset >= block or kind not in (0, 1, 3):
                raise ValueError("Invalid EROFS cluster index")
            logical = i * block + offset
            # Compact indexes can finish with a plain EOF marker inside the
            # last logical block; it has no physical data to consume.
            if logical == size and kind == 0 and i + 1 == count:
                continue
            if logical >= size or (not heads and logical != 0) or (heads and logical <= heads[-1][0]):
                raise ValueError("EROFS compressed extent is out of order")
            heads.append((logical, physical, kind))
        result = bytearray()
        for i, (logical, physical, kind) in enumerate(heads):
            length = (heads[i + 1][0] if i + 1 < len(heads) else size) - logical
            encoded = self.read(physical * block, block)
            if kind == 0:
                if length > block:
                    raise ValueError("EROFS plain cluster exceeds a block")
                decoded = encoded[:length]
            else:
                if self.incompat & 1:
                    encoded = encoded.lstrip(b"\0")
                if not encoded or length > 1024 * 1024:
                    raise ValueError("EROFS compressed extent exceeds bounds")
                decoded = lz4.block.decompress(encoded, uncompressed_size=1024 * 1024)
                if len(decoded) != length:
                    raise ValueError("EROFS decompressed extent length mismatch")
            if logical != len(result):
                raise ValueError("EROFS compressed extent leaves a gap")
            result += decoded
        if len(result) != size:
            raise ValueError("EROFS file length mismatch")
        return bytes(result)

    def directory(self, inode):
        if inode["mode"] & 0xf000 != 0x4000:
            raise ValueError("Not an EROFS directory")
        data = self.contents(inode); result = {}
        for at in range(0, len(data), self.block):
            block = data[at:at + self.block]
            if len(block) < 12:
                raise ValueError("Truncated EROFS directory block")
            end = struct.unpack_from("<H", block, 8)[0]
            if not end or end % 12 or end > len(block):
                raise ValueError("Invalid EROFS directory entries")
            rows = [struct.unpack_from("<QHBB", block, k) for k in range(0, end, 12)]
            for i, (nid, offset, kind, reserved) in enumerate(rows):
                last = rows[i + 1][1] if i + 1 < len(rows) else len(block)
                if reserved or not end <= offset < last <= len(block) or kind > 7:
                    raise ValueError("Invalid EROFS directory bounds")
                name = block[offset:last].split(b"\0", 1)[0].decode("utf-8", errors="strict")
                if not name or len(name.encode()) > 255 or "/" in name or name in result:
                    raise ValueError("Invalid EROFS directory name")
                result[name] = (nid, kind)
        return result

    def lookup(self, path):
        if not path.startswith("/") or "\0" in path:
            raise ValueError("EROFS path must be absolute")
        inode = self.inode(self.root)
        for component in path.split("/")[1:]:
            if not component or component in (".", ".."):
                raise ValueError("Unsafe EROFS path component")
            inode = self.inode(self.directory(inode)[component][0])
        return inode


def modinfo(data):
    if data[:6] != b"\x7fELF\x02\x01" or len(data) < 64:
        raise ValueError("Expected little endian ELF64 module")
    if struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError("Expected ARM64 module")
    offset = struct.unpack_from("<Q", data, 40)[0]
    width, count, strings_index = struct.unpack_from("<HHH", data, 58)
    if width != 64 or strings_index >= count or offset + width * count > len(data):
        raise ValueError("ELF section table out of range")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, offset + i * width) for i in range(count)]

    def contents(section):
        start, length = section[4:6]
        if start + length > len(data):
            raise ValueError("ELF section contents out of range")
        return data[start:start + length]

    names = contents(sections[strings_index]); fields = {}
    for section in sections:
        if section[0] >= len(names):
            raise ValueError("ELF section name out of range")
        name = names[section[0]:].split(b"\0", 1)[0]
        if name == b".modinfo":
            for item in contents(section).split(b"\0"):
                if b"=" in item:
                    key, value = item.split(b"=", 1)
                    if key in (b"vermagic", b"depends", b"firmware", b"alias", b"license", b"name", b"srcversion"):
                        fields.setdefault(key.decode(), []).append(value.decode("utf-8", errors="replace"))
    exports = _elf_parser.elf_module(data)
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "modinfo": fields,
            "module_signature_trailer_present": data.endswith(b"~Module signature appended~\n"),
            "exported_symbols": exports["exported_symbols"],
            "exported_symbol_info": exports["exported_symbol_info"],
            "export_integrity_errors": exports["export_integrity_errors"]}



class ModuleExtraction:
    """Extract explicit source:name selections; never replace different files."""
    def __init__(self, directory, selections):
        self.directory = directory.resolve()
        self.pending = set(selections)
        self.entries = {}
        manifest = self.directory / "manifest.json"
        if manifest.exists():
            previous = json.loads(manifest.read_text())
            if previous.get("schema") != 1:
                raise ValueError("Unsupported extraction manifest schema")
            for entry in previous["modules"]:
                target = Path(entry["path"])
                if self.directory not in target.resolve().parents or target.is_symlink():
                    raise ValueError("Existing extraction path escaped its directory")
                if hashlib.sha256(target.read_bytes()).hexdigest() != entry["sha256"]:
                    raise ValueError("Existing extracted module was modified")
                self.entries[entry["selection"]] = entry
        for selection in self.pending:
            if not re.fullmatch(r"(?:vendor_dlkm_a|system_dlkm_a|odm_dlkm_a|vendor_boot_a:[0-3]):[A-Za-z0-9_]+", selection):
                raise ValueError("Invalid explicit module selection: " + selection)
        self.directory.mkdir(parents=True, exist_ok=True)

    def save(self, source, name, filename, data, info):
        selection = source + ":" + name
        if selection not in self.pending:
            return
        if not re.fullmatch(r"[A-Za-z0-9_.-]+\.ko", filename):
            raise ValueError("Invalid extracted module filename")
        target = self.directory / source.replace(":", "_") / filename
        target.parent.mkdir(exist_ok=True)
        if target.is_symlink() or target.parent.is_symlink():
            raise ValueError("Refusing symlink extraction destination")
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() != info["sha256"]:
                raise ValueError("Existing extracted module differs: " + str(target))
        else:
            with target.open("xb") as output:
                output.write(data)
        self.entries[selection] = {"selection": selection, "name": name, "source": source,
            "member": "lib/modules/" + filename, "path": str(target),
            "bytes": info["bytes"], "sha256": info["sha256"],
            "loaded_on_stock_android": info["loaded_on_stock_android"]}
        self.pending.remove(selection)

    def finish(self):
        if self.pending:
            raise ValueError("Requested modules not found: " + ", ".join(sorted(self.pending)))
        manifest = {"schema": 1, "scope": "Exact shipped ARM64 Linux modules; static analysis only",
                    "device_access": False, "original_open_mode": "rb",
                    "modules": [self.entries[k] for k in sorted(self.entries)]}
        path = self.directory / "manifest.json"
        path.write_text(json.dumps(manifest, indent=2) + "\n")
        return str(path)


def inspect_vendor_boot(stream, partition, hardware, extraction=None):
    base, size = partition["offset_bytes"], partition["size_bytes"]
    header = read_at(stream, base, 4096)
    if header[:8] != b"VNDRBOOT":
        raise ValueError("Invalid vendor_boot header")
    version, page = struct.unpack_from("<II", header, 8)
    ramdisk_size = struct.unpack_from("<I", header, 24)[0]
    header_size, dtb_size = struct.unpack_from("<II", header, 2096)
    if version != 4 or page != 4096 or header_size != 2128:
        raise ValueError("Unsupported vendor_boot layout")
    table_size, count, width, bootconfig_size = struct.unpack_from("<4I", header, 2112)
    align = lambda n: (n + page - 1) // page * page
    table_offset = page + align(ramdisk_size) + align(dtb_size)
    if width != 108 or count * width != table_size or table_offset + align(table_size) + bootconfig_size > size:
        raise ValueError("Vendor ramdisk table bounds mismatch")
    table = read_at(stream, base + table_offset, table_size)
    selected = {n for group in hardware["groups"].values() for n in group}
    fragments = []
    for i in range(count):
        entry = table[i * width:(i + 1) * width]
        length, offset, kind = struct.unpack_from("<III", entry)
        name = entry[12:44].split(b"\0", 1)[0].decode("ascii")
        if offset + length > ramdisk_size:
            raise ValueError("Vendor ramdisk fragment exceeds payload")
        info = {"name": name, "type": kind, "compressed_bytes": length}
        # Recovery is not the normal Android driver set.
        if kind == 2:
            info["status"] = "recovery-only fragment, not inspected"
            fragments.append(info); continue
        packed = read_at(stream, base + page + offset, length)
        if packed[:2] == b"\x1f\x8b":
            unpacked = gzip.decompress(packed)
        elif packed[:4] in (b"\x02\x21\x4c\x18", b"\x04\x22\x4d\x18"):
            unpacked = subprocess.run(["lz4", "-dc"], input=packed, capture_output=True, check=True).stdout
        elif packed[:6] in (b"070701", b"070702"):
            unpacked = packed
        else:
            raise ValueError("Unsupported vendor ramdisk compression")
        modules, metadata = {}, {}
        cursor = 0
        while cursor < len(unpacked):
            h = unpacked[cursor:cursor + 110]
            if h[:6] not in (b"070701", b"070702") or len(h) != 110:
                raise ValueError("Invalid CPIO newc header")
            fields = [int(h[6 + j * 8:14 + j * 8], 16) for j in range(13)]
            length, name_size = fields[6], fields[11]
            name_start = cursor + 110
            data_start = (name_start + name_size + 3) & ~3
            if name_size < 1 or data_start + length > len(unpacked):
                raise ValueError("CPIO entry exceeds ramdisk")
            raw_name = unpacked[name_start:name_start + name_size]
            if raw_name[-1] != 0:
                raise ValueError("Unterminated CPIO filename")
            filename = raw_name[:-1].decode("utf-8")
            if filename == "TRAILER!!!":
                break
            if (fields[1] & 0o170000) == 0o100000 and re.fullmatch(r"(?:\./)?lib/modules/[A-Za-z0-9_.-]+", filename):
                data = unpacked[data_start:data_start + length]
                if filename.endswith(".ko"):
                    module_info = modinfo(data)
                    module = module_info["modinfo"].get("name", [Path(filename).stem])[0].replace("-", "_")
                    if module in modules:
                        raise ValueError("Duplicate module in vendor ramdisk fragment")
                    module_info["filename"] = Path(filename).name
                    module_info["loaded_on_stock_android"] = module in hardware["modules"]
                    module_info["feature_selected"] = module in selected
                    modules[module] = module_info
                    if extraction:
                        extraction.save(f"vendor_boot_a:{i}", module, Path(filename).name, data, module_info)
                elif Path(filename).name in ("modules.dep", "modules.load", "modules.load.recovery", "modules.softdep"):
                    metadata[Path(filename).name] = data.decode().splitlines()
            cursor = (data_start + length + 3) & ~3
        else:
            raise ValueError("CPIO trailer missing")
        info.update({"status": "normal-boot fragment inspected in memory", "module_count": len(modules),
                     "uncompressed_bytes": len(unpacked), "modules": modules, "module_metadata": metadata})
        fragments.append(info)
    return {"status": "inspected read-only in VM memory", "header_version": version, "fragments": fragments}


def dependency_analysis(result, hardware):
    catalogs = [(k, v.get("modules", {})) for k, v in result["partitions"].items()]
    catalogs += [(f"vendor_boot_a:{i}", v.get("modules", {})) for i, v in enumerate(result["vendor_boot_a"]["fragments"])]
    catalog, aliases = {}, {}
    for source, entries in catalogs:
        for name, info in entries.items():
            catalog.setdefault(name, []).append((source, info))
            aliases[Path(info["filename"]).stem.replace("-", "_")] = name
    for name in catalog:
        aliases[name] = name
    graph, unresolved = {}, {}
    for name, variants in catalog.items():
        deps = sorted({d.replace("-", "_") for _, v in variants for text in v["modinfo"].get("depends", [])
                       for d in text.split(",") if d})
        graph[name] = sorted({aliases[d] for d in deps if d in aliases})
        unknown = [d for d in deps if d not in aliases]
        if unknown:
            unresolved[name] = unknown
    features = {}
    for group, roots in hardware["groups"].items():
        visited, todo = set(), list(roots)
        while todo:
            name = todo.pop()
            if name not in visited:
                visited.add(name); todo.extend(graph.get(name, []))
        features[group] = {"roots": roots, "declared_dependency_closure": sorted(visited),
            "unresolved_names": sorted({d for n in visited for d in unresolved.get(n, [])})}
    variants = {n: [{"source": source, "sha256": v["sha256"], "filename": v["filename"]} for source, v in entries]
                for n, entries in catalog.items() if len({v["sha256"] for _, v in entries}) > 1}
    return {"scope": "Union of declared dependencies; not a runtime load-order or hardware test",
            "unique_module_names": len(catalog), "different_binary_variants_requiring_selection": variants,
            "unresolved_note": "May be built-in kernel exports or metadata aliases; absence is not established",
            "unresolved": unresolved, "features": features, "graph": graph}


def inspect_dlkm(stream, super_part, partition, name, hardware, extraction=None):
    """Inspect one bounded logical module filesystem in anonymous memory only."""
    selected = set(n for group in hardware["groups"].values() for n in group)
    if partition["bytes"] > 256 * 1024 * 1024:
        raise ValueError("Logical image exceeds bounded in-memory inspection limit")
    fd = os.memfd_create("t70pro-stock-dlkm")
    try:
        os.ftruncate(fd, partition["bytes"])
        cursor = 0
        for extent in partition["extents"]:
            for chunk in range(0, extent["bytes"], 4 * 1024 * 1024):
                length = min(4 * 1024 * 1024, extent["bytes"] - chunk)
                if extent["type"] == 0:
                    data = read_at(stream, super_part["offset_bytes"] + extent["super_offset"] + chunk, length)
                    if os.pwrite(fd, data, cursor + chunk) != length:
                        raise ValueError("Short anonymous-memory write")
            cursor += extent["bytes"]

        erofs = os.pread(fd, 4, 1024) == bytes.fromhex("e2e1f5e0")
        ext4 = os.pread(fd, 2, 1080) == bytes.fromhex("53ef")
        if not (erofs or ext4):
            raise ValueError("Unsupported module filesystem")

        def cat(path):
            if not re.fullmatch(r"/lib/modules/[A-Za-z0-9_.-]+", path):
                raise ValueError("Invalid read-only module path")
            command = (["dump.erofs", "--cat", "--path=" + path, f"/proc/self/fd/{fd}"] if erofs
                       else ["debugfs", "-R", "cat " + path, f"/proc/self/fd/{fd}"])
            process = subprocess.run(command, pass_fds=(fd,), capture_output=True, check=True)
            return process.stdout

        dep_text = cat("/lib/modules/modules.dep").decode()
        dependencies = {}
        filenames = {}
        for line in dep_text.splitlines():
            if ":" in line:
                module, deps = line.split(":", 1)
                key = Path(module).stem.replace("-", "_")
                filenames[key] = Path(module).name
                dependencies[key] = [Path(n).stem.replace("-", "_") for n in deps.split()]
        if not dependencies and erofs:
            listing_result = subprocess.run(["dump.erofs", "--ls", "--path=/lib/modules", f"/proc/self/fd/{fd}"],
                                            pass_fds=(fd,), capture_output=True, check=True)
            listing = (listing_result.stdout + listing_result.stderr).decode()
            for line in listing.splitlines():
                filename = line.split()[-1] if line.split() else ""
                if re.fullmatch(r"[A-Za-z0-9_.-]+\.ko", filename):
                    filenames[Path(filename).stem.replace("-", "_")] = filename
        if not filenames:
            root = subprocess.run(["dump.erofs", "--ls", "--path=/", f"/proc/self/fd/{fd}"],
                                  pass_fds=(fd,), capture_output=True, check=True)
            root_listing = (root.stdout + root.stderr).decode()
            if "NID:" not in root_listing or "directory" not in root_listing:
                raise ValueError("Cannot inspect filesystem root")
            return {"status": "Filesystem read; no module files at /lib/modules",
                    "bytes": partition["bytes"], "module_count": 0}
        modules = {}
        for key, filename in sorted(filenames.items()):
            data = cat("/lib/modules/" + filename)
            info = modinfo(data)
            module = info["modinfo"].get("name", [key])[0].replace("-", "_")
            if module in modules:
                raise ValueError("Duplicate module name in stock filesystem")
            info["filename"] = filename
            info["loaded_on_stock_android"] = module in hardware["modules"]
            info["feature_selected"] = module in selected
            modules[module] = info
            if extraction:
                extraction.save(name, module, filename, data, info)
        return {"status": "inspected read-only in anonymous VM memory",
            "bytes": partition["bytes"], "filesystem": "erofs" if erofs else "ext4",
            "module_count": len(modules),
            "modules_dep_present": bool(dependencies),
            "dependencies": dependencies,
            "load_order": cat("/lib/modules/modules.load").decode().splitlines(),
            "modules": modules}
    finally:
        os.close(fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--backup", type=Path, required=True)
    ap.add_argument("--hardware", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--extract-dir", type=Path, help="Keep only explicitly selected modules in this directory")
    ap.add_argument("--extract-module", action="append", default=[], metavar="SOURCE:NAME")
    args = ap.parse_args()
    if bool(args.extract_dir) != bool(args.extract_module):
        ap.error("--extract-dir and at least one --extract-module are required together")
    if args.extract_dir and (args.extract_dir.resolve() == args.backup.resolve() or args.backup.resolve() in args.extract_dir.resolve().parents):
        ap.error("Extraction must not write into the original backup")
    extraction = ModuleExtraction(args.extract_dir, args.extract_module) if args.extract_dir else None
    layout = json.loads((args.backup / "partition-layout.json").read_text())
    if not layout["primary_header_crc_valid"] or not layout["partition_entries_crc_valid"]:
        raise ValueError("Original GPT verification missing")
    super_part = next(x for x in layout["partitions"] if x["name"] == "super")
    hardware = json.loads(args.hardware.read_text())
    selected = set(n for group in hardware["groups"].values() for n in group)
    result = {"source": str(args.backup / "images/ufs-lu2.img"), "original_open_mode": "rb",
              "logical_images_saved_to_disk": False, "device_access": False,
              "scope": "Offline shipped module metadata; no module loading or hardware test", "partitions": {}}
    with (args.backup / "images/ufs-lu2.img").open("rb") as stream:
        logical, geometry = logical_partitions(stream, super_part)
        result["lp_metadata"] = geometry
        result["logical_partitions"] = logical
        vendor_boot = next(x for x in layout["partitions"] if x["name"] == "vendor_boot_a")
        result["vendor_boot_a"] = inspect_vendor_boot(stream, vendor_boot, hardware, extraction)
        for name in ("vendor_dlkm_a", "system_dlkm_a", "odm_dlkm_a"):
            if name not in logical or logical[name]["bytes"] == 0:
                result["partitions"][name] = {"status": "not present in metadata slot 0"}; continue
            partition = logical[name]
            result["partitions"][name] = inspect_dlkm(stream, super_part, partition, name, hardware, extraction)
    catalogs = [p.get("modules", {}) for p in result["partitions"].values()]
    catalogs += [p.get("modules", {}) for p in result["vendor_boot_a"]["fragments"]]
    inventory = {name for catalog in catalogs for name in catalog}
    result["loaded_module_coverage"] = {"loaded_count": len(hardware["modules"]),
        "found_in_inspected_backup_count": len(inventory.intersection(hardware["modules"])),
        "not_found": sorted(set(hardware["modules"]) - inventory)}
    result["dependency_analysis"] = dependency_analysis(result, hardware)
    if extraction:
        result["extraction_manifest"] = extraction.finish()
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"lp_metadata": result["lp_metadata"], "loaded_module_coverage": result["loaded_module_coverage"],
        "vendor_boot_fragments": [{k: p.get(k) for k in ("name", "type", "status", "module_count")} for p in result["vendor_boot_a"]["fragments"]], "partitions": {
        k: {f: v.get(f) for f in ("status", "bytes", "module_count")} for k, v in result["partitions"].items()}}))


if __name__ == "__main__":
    main()
