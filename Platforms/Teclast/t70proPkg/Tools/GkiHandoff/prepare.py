#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Prepare pinned public GKI inputs without device commands or firmware packaging."""
import argparse
import ast
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PACKAGE = HERE.parents[1]
PROJECT = PACKAGE.parents[3]
ROOT = PROJECT / "gki-handoff"
REPORT = PROJECT / "analysis/slotless-boot/gki-preparation"
LOCK = json.loads((HERE / "source-lock.json").read_text())


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(args, **kwargs):
    return subprocess.run([str(x) for x in args], check=True, **kwargs)


def output(args, **kwargs):
    return subprocess.check_output([str(x) for x in args], text=True, **kwargs).strip()


def checkout(name, spec, fetch):
    path = ROOT / name
    if not path.exists():
        if not fetch:
            raise ValueError("Missing source: " + name + "; run with --fetch")
        path.mkdir(parents=True)
        run(["git", "init", "-q", path])
        run(["git", "-C", path, "remote", "add", "origin", spec["url"]])
    try:
        actual = output(["git", "-C", path, "rev-parse", "--verify", "HEAD"], stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        if not fetch:
            raise ValueError("Incomplete source: " + name)
        run(["git", "-C", path, "fetch", "--depth=1", "--filter=blob:none",
             "origin", spec["commit"]])
        if spec["sparse"]:
            run(["git", "-C", path, "sparse-checkout", "init", "--cone"])
            run(["git", "-C", path, "sparse-checkout", "set", *spec["sparse"]])
        run(["git", "-C", path, "checkout", "--detach", spec["commit"]])
        actual = output(["git", "-C", path, "rev-parse", "HEAD"])
    if actual != spec["commit"]:
        raise ValueError(name + ": revision differs; existing checkout preserved")
    dirty = output(["git", "-C", path, "status", "--porcelain", "--untracked-files=no"])
    if dirty:
        raise ValueError(name + ": tracked changes present; existing checkout preserved")
    return {"commit": actual, "tracked_clean": True}


def tool_environment():
    env = os.environ.copy()
    folders = [ROOT / "host/bin", ROOT / "toolchains/clang/clang-r536225/bin",
               ROOT / "toolchains/rust/linux-x86/1.82.0/bin",
               ROOT / "toolchains/kernel-tools/linux-x86/bin"]
    env["PATH"] = os.pathsep.join(str(x) for x in folders) + os.pathsep + env["PATH"]
    env.update(LOCK["build_environment"])
    return env


def tool_probes(env):
    probes = {}
    for name, command, expected in [
        ("clang", ["clang", "--version"], "r536225"),
        ("rustc", ["rustc", "--version"], "1.82.0"),
        ("bindgen", ["bindgen", "--version"], "0.69.5"),
        ("pahole", ["pahole", "--version"], None),
        ("bc", ["bc", "--version"], None),
    ]:
        try:
            p = subprocess.run(command, env=env, capture_output=True, text=True, timeout=20)
            text = (p.stdout + p.stderr).strip()
            probes[name] = {"ready": p.returncode == 0 and (expected is None or expected in text),
                            "exit_code": p.returncode, "version": text.splitlines()[0] if text else ""}
        except (OSError, subprocess.TimeoutExpired) as e:
            probes[name] = {"ready": False, "error": type(e).__name__ + ": " + str(e)}
    for name, language, source in [
        ("clang_arm64_c", "c", "int t70_probe(void) { return 0; }\n"),
        ("clang_arm64_asm", "assembler", ".text\n.arch armv8.5-a+memtag\nstg x0, [x0]\ncas x0, x1, [x2]\n"),
    ]:
        try:
            p = subprocess.run(["clang", "--target=aarch64-linux-gnu", "-fintegrated-as",
                                "-Werror=unused-command-line-argument", "-Wa,--fatal-warnings", "-c", "-x",
                                "assembler-with-cpp" if language == "assembler" else language,
                                "-o", "/dev/null", "-"], input=source,
                               env=env, text=True, capture_output=True, timeout=30)
            probes[name] = {"ready": p.returncode == 0, "exit_code": p.returncode,
                            "diagnostic": p.stderr[:1000]}
        except (OSError, subprocess.TimeoutExpired) as error:
            probes[name] = {"ready": False, "error": str(error)}
    return probes


def module_catalog():
    spec = importlib.util.spec_from_file_location("stock_gki_elf", PACKAGE / "Linux/prepare-stock-modules.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    document = json.loads((PROJECT / "analysis/stock-modules.json").read_text())
    return module, module.module_catalog(document)


def extract_modules(backup):
    _, catalog = module_catalog()
    manifest = PROJECT / "analysis/stock-binaries/manifest.json"
    existing = json.loads(manifest.read_text()) if manifest.exists() else {"modules": []}
    pinned = {(name, source): info for name, variants in catalog.items() for source, info in variants.items()}
    present = {(row["name"], row["source"]): row for row in existing["modules"]}
    if any(k not in pinned or row["sha256"] != pinned[k]["sha256"] for k, row in present.items()):
        raise ValueError("Existing module manifest differs from stock metadata")
    if all(k in present and Path(present[k]["path"]).is_file() for k in pinned):
        for row in present.values():
            if sha(Path(row["path"]).read_bytes()) != row["sha256"]:
                raise ValueError("Existing extracted module was modified")
        return {"binary_records": len(pinned), "manifest": str(manifest), "all_present": True}
    REPORT.mkdir(parents=True, exist_ok=True)
    hardware = REPORT / "module-selection.json"
    loaded = sorted({name for name, variants in catalog.items()
                     if any(x.get("loaded_on_stock_android") for x in variants.values())})
    hardware.write_text(json.dumps({"modules": loaded, "groups": {}}, indent=2) + "\n")
    cmd = [sys.executable, PACKAGE / "inspect-stock-modules.py", "--backup", backup,
           "--hardware", hardware, "--output", REPORT / "stock-modules-all.json",
           "--extract-dir", manifest.parent]
    for name, source in sorted(pinned):
        cmd.extend(["--extract-module", source + ":" + name])
    run(cmd)
    result = json.loads(manifest.read_text())
    if len(result["modules"]) != len(pinned):
        raise ValueError("Incomplete module extraction")
    return {"binary_records": len(pinned), "manifest": str(manifest), "all_present": True}


def configuration(backup):
    exports = json.loads((PROJECT / "analysis/stock-kernel-exports.json").read_text())
    if exports["image_sha256"] != LOCK["stock_image_sha256"]:
        raise ValueError("Stock export identity mismatch")
    certs = exports["trusted_public_certificates"]
    if not certs["public_certificates_only"] or certs["private_key_access"]:
        raise ValueError("Public certificate evidence required")
    with (backup / "images/boot_a.img").open("rb") as stream:
        header = stream.read(4096)
        if header[:8] != b"ANDROID!" or int.from_bytes(header[40:44], "little") != 4:
            raise ValueError("Expected Android boot v4")
        length = int.from_bytes(header[8:12], "little")
        if not 0 < length <= 32 * 1024 * 1024:
            raise ValueError("Unexpected kernel size")
        packed = stream.read(length)
    raw = subprocess.check_output(["lz4", "-dc"], input=packed, timeout=30)
    if sha(raw) != LOCK["stock_image_sha256"]:
        raise ValueError("Stock kernel identity mismatch")
    pem = ""
    for item in certs["certificates"]:
        offset = int(item["image_offset"], 16)
        der = raw[offset:offset + item["bytes"]]
        if len(der) != item["bytes"] or sha(der) != item["sha256"]:
            raise ValueError("Public certificate slice mismatch")
        encoded = base64.b64encode(der).decode("ascii")
        pem += "-----BEGIN CERTIFICATE-----\n"
        pem += "\n".join(encoded[i:i + 64] for i in range(0, len(encoded), 64))
        pem += "\n-----END CERTIFICATE-----\n"
    inputs = ROOT / "inputs"
    inputs.mkdir(exist_ok=True)
    cert = inputs / "stock-gki-public-certificates.pem"
    cert.write_text(pem)
    # Read literal Starlark lists as data; never execute the downloaded .bzl.
    definitions = {}
    for node in ast.parse((ROOT / "common/modules.bzl").read_text()).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name) and isinstance(node.value, ast.List):
            value = ast.literal_eval(node.value)
            if not all(isinstance(x, str) for x in value):
                raise ValueError("Invalid module list")
            definitions[node.targets[0].id] = value
    protected = [x.removesuffix(".ko") for x in
                 definitions["_COMMON_GKI_MODULES_LIST"] + definitions["_ARM64_GKI_MODULES_LIST"]
                 + definitions["_KUNIT_FRAMEWORK_MODULES"] + definitions["_KUNIT_COMMON_MODULES_LIST"]
                 + definitions["_KUNIT_CLK_MODULES_LIST"]
                 if x not in definitions["_COMMON_UNPROTECTED_MODULES_LIST"]]
    protected_file = inputs / "protected_module_names_list"
    protected_file.write_text("\n".join(sorted(protected)) + "\n")
    whitelist = inputs / "stock-vmlinux-symbols"
    whitelist.write_text("\n".join(sorted(exports["exports"])) + "\n")
    # Retain public signer trust without creating or using a private signing key.
    updates = {"KEXEC": "y", "KEXEC_FILE": "n", "MODULE_SIG_ALL": "n",
               "MODULE_SIG_KEY": '""', "SYSTEM_TRUSTED_KEYS": json.dumps(str(cert)),
               "MODULE_SIG_PROTECT_LIST": json.dumps(str(protected_file)),
               "UNUSED_KSYMS_WHITELIST": json.dumps(str(whitelist)),
               "LOCALVERSION": '"-android16-5-4k-t70-kexec-prep"'}
    original = (PROJECT / "analysis/stock-kernel.config").read_text()
    lines = []
    for line in original.splitlines():
        match = re.match(r"(?:# )?CONFIG_([A-Za-z0-9_]+)(?:=| is not set)", line)
        if not match or match[1] not in updates:
            lines.append(line)
    for key, value in updates.items():
        lines.append("# CONFIG_" + key + " is not set" if value == "n" else "CONFIG_" + key + "=" + value)
    out = ROOT / "out/kexec"
    out.mkdir(parents=True, exist_ok=True)
    config = out / ".config"
    if config.exists():
        raise ValueError("Candidate .config already exists; preserved (inspect it instead of overwriting)")
    config.write_text("\n".join(lines) + "\n")
    return {"path": str(config), "validated_by_kconfig": False,
            "updates": updates, "public_certificate_sha256": [x["sha256"] for x in certs["certificates"]]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--check", action="store_true", help="read-only verification; never fetch or write")
    parser.add_argument("--extract-all-modules", action="store_true")
    parser.add_argument("--configure", action="store_true", help="stage .config for later Kconfig validation")
    parser.add_argument("--backup", type=Path, default=Path("/mnt/mac/Users/you/.tmp/teclast-t70pro/device-backup"))
    args = parser.parse_args()
    if args.check and (args.fetch or args.extract_all_modules or args.configure):
        parser.error("--check cannot be combined with mutations")
    report = {"schema": 1, "device_access": False, "installable": False,
              "kernel_image_built": False, "module_load_verified": False, "sources": {}}
    try:
        for name, spec in LOCK["sources"].items():
            report["sources"][name] = checkout(name, spec, args.fetch)
        common = ROOT / "common"
        constants = (common / "build.config.constants").read_text()
        makefile = (common / "Makefile").read_text()
        if "KMI_GENERATION=5" not in constants or "SUBLEVEL = 38" not in makefile:
            raise ValueError("Wrong source version or KMI generation")
        report["source_ready"] = True
        report["tools"] = tool_probes(tool_environment())
        report["toolchain_ready"] = all(x["ready"] for x in report["tools"].values())
        if args.extract_all_modules:
            report["stock_modules"] = extract_modules(args.backup)
        if args.configure:
            report["configuration"] = configuration(args.backup)
        if not args.check:
            REPORT.mkdir(parents=True, exist_ok=True)
            (REPORT / "preparation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
        return 0 if report["toolchain_ready"] else 1
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        if not args.check:
            REPORT.mkdir(parents=True, exist_ok=True)
            (REPORT / "preparation.json").write_text(json.dumps(report, indent=2) + "\n")
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
