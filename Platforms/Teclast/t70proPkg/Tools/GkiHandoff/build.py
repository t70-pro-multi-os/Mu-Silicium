#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Validate the candidate config and compile kexec objects, without packaging or device access."""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import shlex
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("prepare_gki_build", HERE / "prepare.py")
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", action="store_true", help="also build Image and modules; still no boot.img packaging")
    args = parser.parse_args()
    out = prep.ROOT / "out/kexec"
    config = out / ".config"
    report = {"schema": 1, "device_access": False, "installable": False,
              "configuration_validated": False, "kexec_objects_compiled": False,
              "kernel_image_built": False, "modules_built": False, "abi_probe_built": False, "module_load_verified": False}
    prep.REPORT.mkdir(parents=True, exist_ok=True)
    try:
        report["source"] = prep.checkout("common", prep.LOCK["sources"]["common"], False)
        env = prep.tool_environment()
        probes = prep.tool_probes(env)
        report["tools"] = probes
        if not all(p["ready"] for p in probes.values()):
            raise ValueError("Run prepare-host.py and check the exact tools before compiling")
        if not config.is_file():
            raise ValueError("Run prepare.py --configure first")
        initial = out / "config.before-olddefconfig"
        if not initial.exists():
            initial.write_bytes(config.read_bytes())
        env["LIBCLANG_PATH"] = str(prep.ROOT / "toolchains/clang/clang-r536225/lib")
        env["RUST_LIB_SRC"] = str(prep.ROOT / "toolchains/rust/linux-x86/1.82.0/lib/rustlib/src/rust/library")
        env["KBUILD_BUILD_USER"] = "t70-gki-preparation"
        env["KBUILD_BUILD_HOST"] = "builder"
        env["KBUILD_GENDWARFKSYMS_STABLE"] = "1"
        runtime = prep.ROOT / "host/root/usr"
        # Rust proc macros execute inside the x86 Rust compiler, while C host
        # generators run as ARM64. Link the proc macros for x86 explicitly.
        hostroot = prep.ROOT / "host/root"
        gcc_dirs = sorted((hostroot / "usr/lib/gcc/x86_64-linux-gnu").glob("*"))
        if not gcc_dirs:
            raise ValueError("Private x86 GCC runtime is missing; run prepare-host.py")
        proc_flags = " ".join(["--target=x86_64-linux-gnu", "--sysroot=" + str(hostroot),
                                "-B" + str(gcc_dirs[-1]), "--ld-path=/usr/bin/ld.lld"])
        # Kconfig treats RUSTC as one executable path. A private wrapper both
        # satisfies that contract and makes the identity seed part of savedcmd.
        rustc = prep.ROOT / "host/bin/rustc-android"
        wrapper = ("#!/bin/sh\nexport RUSTC_FORCE_RUSTC_VERSION=" +
                   shlex.quote(env["RUSTC_FORCE_RUSTC_VERSION"]) +
                   "\nexec " + shlex.quote(str(prep.ROOT / "host/bin/rustc")) + ' "$@"\n')
        if rustc.exists() and rustc.read_text() != wrapper:
            raise ValueError("Existing Rust identity wrapper differs; preserved")
        if not rustc.exists():
            rustc.write_text(wrapper)
            rustc.chmod(0o755)
        make = ["make", "-C", str(prep.ROOT / "common"), "O=" + str(out),
                "ARCH=arm64", "LLVM=1", "LLVM_IAS=1", "HOSTCC=/usr/bin/clang", "HOSTCXX=/usr/bin/clang++",
                "HOSTCFLAGS=-O2 -I" + str(runtime / "include") + " -include " + str(HERE / "host-compat.h"),
                "HOSTLDFLAGS=-L" + str(runtime / "lib/aarch64-linux-gnu") +
                " -Wl,-rpath," + str(runtime / "lib/aarch64-linux-gnu"),
                "PROCMACROLDFLAGS=" + proc_flags,
                # Make records this prefix, so changing the Rust identity seed
                # rebuilds Rust objects instead of silently reusing old names.
                "RUSTC=" + str(rustc),
                "CFLAGS_ksysfs.o=-include " + str(HERE / "ksysfs-kabi.h"),
                "-j" + str(min(os.cpu_count() or 1, 12))]
        report["build_environment"] = prep.LOCK["build_environment"]
        report["kabi_header_sha256"] = sha(HERE / "ksysfs-kabi.h")
        subprocess.run(make + ["olddefconfig"], env=env, check=True)
        helper, _ = prep.module_catalog()
        normalized = helper.config(config)
        must_keep = ("KEXEC", "RUST", "MODULES", "MODVERSIONS", "EXTENDED_MODVERSIONS", "GENDWARFKSYMS", "MODULE_SIG_PROTECT", "ARM64_MTE", "KASAN_HW_TAGS", "DYNAMIC_SCS", "CFI_CLANG")
        if any(normalized.get(name) != "y" for name in must_keep):
            raise ValueError("Kconfig dropped required options: " + ", ".join(k for k in must_keep if normalized.get(k) != "y"))
        before = helper.config(initial)
        report["configuration_changes_from_staged"] = {k: {"before": before.get(k), "after": normalized.get(k)}
                                                        for k in sorted(before.keys() | normalized.keys())
                                                        if before.get(k) != normalized.get(k)}
        report["configuration_validated"] = True
        objects = ["kernel/kexec.o", "arch/arm64/kernel/machine_kexec.o", "arch/arm64/kernel/relocate_kernel.o"]
        subprocess.run(make + ["prepare", *objects, "kernel/ksysfs.o"], env=env, check=True)
        subprocess.run(["python3", str(HERE / "check-ksysfs-kabi.py")], env=env, check=True)
        validation = prep.REPORT / "abi-diagnosis/kabi-validation.json"
        report["kabi_declaration_rules_verified"] = json.loads(validation.read_text())["success"]
        report["kabi_validation_sha256"] = sha(validation)
        release = (out / "include/config/kernel.release").read_text().strip()
        expected_kmi = "-".join(prep.LOCK["kernel_release"].split("-")[:3])
        if not release.startswith(expected_kmi + "-"):
            raise ValueError("Candidate release lost its Android/KMI label: " + release)
        report["kernel_release"] = release
        report["kexec_objects_compiled"] = True
        report["objects"] = {name: sha(out / name) for name in objects}
        if args.image:
            subprocess.run(make + ["Image", "modules"], env=env, check=True)
            report["kernel_image_built"] = True
            report["modules_built"] = True
            probe = out / "abi-probe"
            probe.mkdir(exist_ok=True)
            for source in (HERE / "probe").iterdir():
                target = probe / source.name
                if target.exists() and target.read_bytes() != source.read_bytes():
                    raise ValueError("Existing ABI probe source differs; preserved")
                if not target.exists():
                    shutil.copy2(source, target)
            subprocess.run(make + ["M=" + str(probe), "modules"], env=env, check=True)
            report["abi_probe_built"] = True
            report["artifacts"] = {name: sha(out / name) for name in
                                    ["vmlinux", "Module.symvers", "arch/arm64/boot/Image", "include/config/kernel.release",
                                     "abi-probe/t70_gki_abi_probe.ko"]}
        report["config_sha256"] = sha(config)
        report["source_lock_sha256"] = sha(HERE / "source-lock.json")
        report["host_runtime_receipt_sha256"] = sha(prep.ROOT / "host/receipt.json")
        report["unverified"] = ["vendor module CRC comparison against a full candidate build", "signature/protected-symbol policy",
                                "pKVM override and early-init selection", "EDK2 handoff and real hardware recovery"]
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["error"] = str(error)
        print(str(error), flush=True)
    finally:
        payload = json.dumps(report, indent=2) + "\n"
        (prep.REPORT / "build.json").write_text(payload)
        if out.is_dir():
            (out / "build-receipt.json").write_text(payload)
    return 1 if "error" in report else 0


if __name__ == "__main__":
    raise SystemExit(main())
