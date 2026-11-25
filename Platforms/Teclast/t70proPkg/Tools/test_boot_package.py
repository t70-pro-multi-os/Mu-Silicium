#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Independently reject corruption of the actual packaged AVB boot image."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("boot_package", Path(__file__).with_name("package-boot.py"))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def expect_error(fn):
    try:
        fn()
    except ValueError:
        return
    raise AssertionError("Corruption was accepted")


def run(image, stock, avbtool):
    source_hash = mod.digest(stock.read_bytes())
    data = image.read_bytes()
    meta = mod.metadata(data)
    mod.verify_hash(data, meta)
    assert meta["algorithm"] == 0
    assert meta["flags"] == 0
    assert len(data) == 64*1024*1024
    passed = ["actual_full_partition_footer_and_digest"]
    bad = bytearray(data)
    bad[8192] ^= 1
    expect_error(lambda: mod.verify_hash(bytes(bad), meta))
    with tempfile.TemporaryDirectory(prefix="avb-corruption-test-") as temp:
        # avbtool resolves a hash descriptor through its partition name, boot.img.
        path = Path(temp)/"boot.img"
        path.symlink_to(image.resolve())
        subprocess.run([sys.executable, str(avbtool.resolve()), "verify_image", "--image", str(path)],
                       capture_output=True, text=True, check=True)
        passed.append("official_avbtool_accepts_generated_image")
        path.unlink()
        path.write_bytes(bad)
        r = subprocess.run([sys.executable, str(avbtool), "verify_image", "--image", str(path)],
                           capture_output=True, text=True)
        assert r.returncode != 0
    passed.append("both_verifiers_reject_payload_corruption")
    expect_error(lambda: mod.metadata(data[:-4096]))
    bad = bytearray(data)
    bad[-64:-60] = b"BAD!"
    expect_error(lambda: mod.metadata(bytes(bad)))
    passed.append("truncated_partition_and_bad_footer_rejected")
    # A descriptor extent must never escape its declared auxiliary block.
    bad = bytearray(data)
    import struct
    v = meta["vbmeta_offset"]
    auth = struct.unpack_from(">Q", data, v+12)[0]
    doff = struct.unpack_from(">Q", data, v+96)[0]
    struct.pack_into(">Q", bad, v+256+auth+doff+8, 0xfffffffffffffff8)
    expect_error(lambda: mod.metadata(bytes(bad)))
    passed.append("overflowing_descriptor_extent_rejected")
    assert mod.digest(stock.read_bytes()) == source_hash == mod.STOCK_SHA
    passed.append("verified_stock_original_remains_unchanged")
    return {"schema": 1, "device_access": False, "image_sha256": mod.digest(data),
            "oem_signature_created": False, "physical_AVB_acceptance_verified": False,
            "count": len(passed), "passed": passed}


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", type=Path, required=True)
    p.add_argument("--stock", type=Path, required=True)
    p.add_argument("--avbtool", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args()
    result = run(a.image, a.stock, a.avbtool)
    a.report.write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
