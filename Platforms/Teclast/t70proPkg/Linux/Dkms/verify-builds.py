#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the shipped Linux module files without installing or loading them."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

RELEASE = "7.0.0-43-generic"
VERMAGIC = RELEASE + " SMP preempt mod_unload modversions aarch64"
MODULES = {
    "t70_hx83132.ko": "t70pro-hx83132-retained",
    "t70_mt6983_i2c.ko": "t70pro-mt6983-i2c",
    "t70_mt6789_ext_sd.ko": "t70pro-mt6789-ext-sd",
    "t70-ccmni.ko": None,
    "t70_afe_i2s_slice.ko": "t70pro-mt6789-afe",
    "t70_mt6789_aw88261.ko": "t70pro-aw88261-speaker",
    "t70-scp-accel.ko": "t70pro-scp-accelerometer",
    "t70-stp-hci.ko": "t70pro-mt6789-stp-hci",
}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def inventory(manifest):
    section = manifest.partition("\n## SHA-256\n")[2]
    hashes = {}
    for line in section.splitlines():
        if not line.strip():
            continue
        m = re.fullmatch(r"    ([a-f0-9]{64})  ([A-Za-z0-9_.-]+/[\w.-]+\.ko)", line)
        require(m is not None, "malformed build checksum row")
        digest, rel = m.groups()
        path = Path(rel)
        require(path.parts[0] not in (".", "..") and path.name not in hashes,
                "unsafe or duplicate build checksum row")
        hashes[path.name] = (path, digest)
    require(hashes.keys() == MODULES.keys(), "build inventory differs from the expected modules")
    require(set(re.findall(r"\| ([A-Za-z0-9_.-]+/[\w.-]+\.ko) \|", manifest))
            == {str(path) for path, _ in hashes.values()}, "build table and checksums disagree")
    return hashes


def target_crcs(symvers):
    exports = {}
    for line in symvers.splitlines():
        crc, symbol, *_ = line.split()
        require(re.fullmatch(r"0x[0-9a-fA-F]{8}", crc), "bad target export CRC")
        exports.setdefault(symbol, set()).add(crc.lower())
    require("module_layout" in exports, "target symbol table has no module_layout")
    return exports


def version_rows(text, exports):
    imports = {}
    for line in text.splitlines():
        crc, symbol = line.split()
        crc = crc.lower()
        require(symbol not in imports and exports.get(symbol) == {crc},
                f"{symbol}: missing, ambiguous or mismatched CRC ({crc})")
        imports[symbol] = crc
    require("module_layout" in imports, "module has no module_layout CRC")
    return len(imports)


def inspect(base, inventory_rows, exports):
    report = {}
    for name, (rel, digest) in inventory_rows.items():
        path = (base / rel).resolve(strict=True)
        require(path.is_relative_to(base), f"{rel}: module path escapes result directory")
        data = path.read_bytes()
        require(hashlib.sha256(data).hexdigest() == digest, f"{rel}: SHA-256 mismatch")
        require(len(data) > 64 and data[:6] == b"\x7fELF\x02\x01" and
                struct.unpack_from("<HH", data, 16) == (1, 183),
                f"{rel}: not AArch64 ELF64 relocatable")

        def modinfo(field):
            return subprocess.check_output(["modinfo", "-F", field, path], text=True).strip()

        require(modinfo("vermagic") == VERMAGIC, f"{rel}: unexpected ABI")
        require(not modinfo("signer"), f"{rel}: expected unsigned local build")
        compatible = MODULES[name]
        expected = ({f"of:N*T*Cteclast,{compatible}{suffix}" for suffix in ("", "C*")}
                    if compatible else set())
        require(set(modinfo("alias").splitlines()) == expected, f"{rel}: unexpected module aliases")
        # This mode extracts CRC metadata and exits; it does not load the module.
        versions = subprocess.check_output(["modprobe", "--show-modversions", path], text=True)
        report[name] = {
            "verified_imports": version_rows(versions, exports),
            "module_dependencies": list(filter(None, modinfo("depends").split(","))),
            "of_compatible": f"teclast,{compatible}" if compatible else None,
        }
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("result_dir", type=Path)
    ap.add_argument("module_symvers", type=Path)
    ap.add_argument("--manifest", type=Path,
                    default=Path(__file__).with_name("BUILDS.md"))
    x = ap.parse_args()
    try:
        print(json.dumps({
            "release": RELEASE,
            "modules": inspect(x.result_dir.resolve(strict=True), inventory(x.manifest.read_text()),
                               target_crcs(x.module_symvers.read_text())),
            "scope": "artifact ABI only; no installation, binding, or runtime supplier validation",
        }, indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        ap.exit(2, f"module artifact check refused: {exc}\n")


if __name__ == "__main__":
    main()
