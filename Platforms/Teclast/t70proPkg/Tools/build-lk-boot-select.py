#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a stock-Android/UEFI capsule selected by the original LK boot mode."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("slotless_builder", HERE/"build-slotless-boot.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)
LK_SHA = "05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4"


def prepare(stock_boot, uefi_image, lk, out):
    if out.exists() or out.is_symlink():
        raise ValueError("Output exists; previous artifacts preserved")
    stock = stock_boot.read_bytes()
    if build.sha(stock) != build.STOCK_SHA or stock[:8] != b"ANDROID!":
        raise ValueError("Stock boot_a differs from the verified original")
    if build.sha(lk.read_bytes()) != LK_SHA:
        raise ValueError("LK differs from the analyzed G100 firmware")
    if struct.unpack_from("<I", stock, 40)[0] != 4 or struct.unpack_from("<I", stock, 12)[0]:
        raise ValueError("Expected the stock v4 kernel-only image")
    n = struct.unpack_from("<I", stock, 8)[0]
    kernel = build.legacy_lz4(stock[4096:4096+n])
    capsule, report = build.compile_capsule(kernel, uefi_image.read_bytes(), out, lk_boot_mode=True)
    prefix = report["artifact_prefix"]
    build.run("lz4", "-l", "-f", out/(prefix+".Image"), out/(prefix+".Image.lz4"))
    compressed = (out/(prefix+".Image.lz4")).read_bytes()
    if build.legacy_lz4(compressed) != capsule:
        raise ValueError("LZ4 round trip differs")
    report.update(stock_boot_sha256=build.sha(stock), original_lk_sha256=LK_SHA,
                  compressed_bytes=len(compressed), compressed_sha256=build.sha(compressed),
                  init_boot_modified=False, stock_android_kernel_modified=False,
                  hardware_boot_verified=False,
                  boot_mode_tag={"bytes":16, "little_endian_words":[16,0x41000802,"mode & 255",2],
                                 "uefi_mode":99, "factory_mode":4, "recovery_mode":2,
                                 "lk_function_file_offset":"0xe6e8"},
                  builder_sha256=build.sha(Path(__file__).read_bytes()))
    (out/(prefix+".json")).write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n")
    return report


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("stock-boot", "uefi-image", "lk", "out"):
        p.add_argument("--"+name, type=Path, required=True)
    a = p.parse_args()
    r = prepare(a.stock_boot, a.uefi_image, a.lk, a.out)
    print(json.dumps({k:r[k] for k in ("selection_transport","image_bytes","compressed_bytes","stock_android_kernel_modified")}))
