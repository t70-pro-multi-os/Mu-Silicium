#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Package a selector as a full boot_a image. Offline; no device access."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
PARTITION_SIZE = 64 * 1024 * 1024
STOCK_SHA = "fc68b2ea42142f6a822f89afcaab94d2f8b09d865ae7f3b86f1b6c183d6d3475"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def metadata(data):
    if len(data) != PARTITION_SIZE or data[-64:-60] != b"AVBf":
        raise ValueError("Expected the full 64 MiB boot partition with an AVB footer")
    _, major, minor, original, offset, count = struct.unpack_from(">4sIIQQQ", data, len(data)-64)
    if major != 1 or minor or original > offset or offset % 4096 or not 256 <= count <= 65536:
        raise ValueError("Invalid AVB footer")
    if offset + count > len(data) - 64:
        raise ValueError("VBMeta lies outside the partition")
    v = data[offset:offset+count]
    if v[:4] != b"AVB0":
        raise ValueError("Invalid VBMeta header")
    auth, aux = struct.unpack_from(">QQ", v, 12)
    doff, dsize = struct.unpack_from(">QQ", v, 96)
    if 256 + auth + aux != count or doff + dsize > aux:
        raise ValueError("Invalid VBMeta descriptor bounds")
    descriptors = []
    at, end = 256 + auth + doff, 256 + auth + doff + dsize
    while at < end:
        if at + 16 > end:
            raise ValueError("Truncated descriptor header")
        tag, size = struct.unpack_from(">QQ", v, at)
        body = at + 16
        if size % 8 or size > end - body:
            raise ValueError("Invalid descriptor extent")
        if tag == 0:
            if size < 16:
                raise ValueError("Truncated property")
            kn, vn = struct.unpack_from(">QQ", v, body)
            if 18 + kn + vn > size or v[body+16+kn] or v[body+17+kn+vn]:
                raise ValueError("Invalid property strings")
            descriptors.append({"tag": tag, "key": v[body+16:body+16+kn].decode("utf-8"),
                                "value": v[body+17+kn:body+17+kn+vn].decode("utf-8")})
        elif tag == 2:
            if size < 116:
                raise ValueError("Truncated hash descriptor")
            image_size = struct.unpack_from(">Q", v, body)[0]
            algorithm = v[body+8:body+40].rstrip(b"\0").decode("ascii")
            nn, sn, hn, flags = struct.unpack_from(">IIII", v, body+40)
            if 116 + nn + sn + hn > size:
                raise ValueError("Invalid hash descriptor strings")
            p = body + 116
            descriptors.append({"tag": tag, "image_size": image_size, "algorithm": algorithm,
                                "name": v[p:p+nn].decode("ascii"), "salt": v[p+nn:p+nn+sn].hex(),
                                "hash": v[p+nn+sn:p+nn+sn+hn].hex(), "flags": flags})
        else:
            raise ValueError("Unexpected descriptor in this boot image")
        at = body + size
    return {"original_bytes": original, "vbmeta_offset": offset, "vbmeta_bytes": count,
            "algorithm": struct.unpack_from(">I", v, 28)[0],
            "rollback_index": struct.unpack_from(">Q", v, 112)[0],
            "flags": struct.unpack_from(">I", v, 120)[0],
            "rollback_index_location": struct.unpack_from(">I", v, 124)[0],
            "descriptors": descriptors}


def verify_hash(data, meta):
    hashes = [d for d in meta["descriptors"] if d["tag"] == 2]
    if len(hashes) != 1:
        raise ValueError("Expected one boot hash descriptor")
    h = hashes[0]
    if h["name"] != "boot" or h["algorithm"] != "sha256" or h["flags"] or h["image_size"] != meta["original_bytes"]:
        raise ValueError("Unsupported boot hash descriptor")
    if digest(bytes.fromhex(h["salt"]) + data[:h["image_size"]]) != h["hash"]:
        raise ValueError("AVB payload digest differs")
    return h


def package(stock_path, kernel_path, raw_path, avbtool, output):
    stock = stock_path.read_bytes()
    if digest(stock) != STOCK_SHA or stock[:8] != b"ANDROID!":
        raise ValueError("Stock boot_a differs from the verified original")
    if struct.unpack_from("<I", stock, 40)[0] != 4 or struct.unpack_from("<I", stock, 12)[0]:
        raise ValueError("Expected a v4 kernel-only stock image")
    old = metadata(stock)
    old_hash = verify_hash(stock, old)
    if old["flags"]:
        raise ValueError("Unexpected original AVB flags")
    spec = importlib.util.spec_from_file_location("slotless_image", HERE/"build-slotless-boot.py")
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    kernel, raw = kernel_path.read_bytes(), raw_path.read_bytes()
    if build.legacy_lz4(kernel) != raw or build.image(raw) != len(raw):
        raise ValueError("Compressed selector does not match its ARM64 Image")
    if len(raw) >= 0x3f7f000:
        raise ValueError("Selector exceeds the verified bootstrap window")
    header = bytearray(stock[:4096])
    struct.pack_into("<I", header, 8, len(kernel))
    struct.pack_into("<I", header, 1580, 0)
    body = bytes(header) + kernel
    body += bytes(-len(body) % 4096)
    if output.exists() or output.is_symlink():
        raise ValueError("Output exists; original files were preserved")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="boot-package-", dir=output.parent) as temp:
        pending = Path(temp)/"boot.img"
        pending.write_bytes(body)
        argv = [sys.executable, str(avbtool), "add_hash_footer", "--image", str(pending),
                "--partition_name", "boot", "--partition_size", str(PARTITION_SIZE),
                "--algorithm", "NONE", "--hash_algorithm", "sha256", "--salt", old_hash["salt"],
                "--rollback_index", str(old["rollback_index"]),
                "--rollback_index_location", str(old["rollback_index_location"])]
        for prop in old["descriptors"]:
            if prop["tag"] == 0:
                argv.extend(["--prop", prop["key"] + ":" + prop["value"]])
        subprocess.run(argv, check=True, capture_output=True, text=True)
        subprocess.run([sys.executable, str(avbtool), "verify_image", "--image", str(pending)],
                       check=True, capture_output=True, text=True)
        data = pending.read_bytes()
        new = metadata(data)
        verify_hash(data, new)
        if data[:len(body)] != body or new["algorithm"] != 0:
            raise ValueError("Packaged payload or unsigned metadata differs")
        for key in ("rollback_index", "rollback_index_location", "flags"):
            if new[key] != old[key]:
                raise ValueError("Original rollback metadata was changed")
        if [p for p in old["descriptors"] if p["tag"] == 0] != [p for p in new["descriptors"] if p["tag"] == 0]:
            raise ValueError("Original public build properties were changed")
        # Atomic publication never overwrites a previously prepared image.
        with output.open("xb") as stream:
            stream.write(data)
    report = {"schema": 1, "device_access": False, "partition": "boot_a",
              "bytes": len(data), "sha256": digest(data), "stock_boot_sha256": digest(stock),
              "kernel_image_sha256": digest(raw), "compressed_kernel_sha256": digest(kernel),
              "avb_footer_verified": True, "oem_signed": False, "requires_unlocked_bootloader": True,
              "root_vbmeta_modified": False, "init_boot_modified": False,
              "public_build_properties_preserved": True, "rollback_metadata_preserved": True,
              "hardware_boot_verified": False, "avbtool_sha256": digest(avbtool.read_bytes()),
              "metadata": new, "source_sha256": digest(Path(__file__).read_bytes())}
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--stock-boot", type=Path, required=True)
    p.add_argument("--kernel", type=Path, required=True)
    p.add_argument("--raw-image", type=Path, required=True)
    p.add_argument("--avbtool", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    report = package(a.stock_boot, a.kernel, a.raw_image, a.avbtool, a.out)
    a.out.with_suffix(".json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps({k: report[k] for k in ("bytes", "sha256", "avb_footer_verified", "oem_signed")}))


if __name__ == "__main__":
    main()
