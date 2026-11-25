#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Prepare a separate real iOS Image4/AMFI candidate; never replace native stages."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tomllib
from types import SimpleNamespace

here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--kind", choices=("image4", "security", "security-virtio", "security-t70-ufs", "security-t70-ufs-hfs"), default="security")
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Prepare real iOS security modules", "--",
              sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
sys.dont_write_bytecode = True
report_path = project / ("analysis/xnu-ios-" + args.kind + "-prelink-build.json")
report = {"status": "running", "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "os_abi": "ios", "device_boot": "not_run", "native_stage_replaced": False,
          "runtime_qualified": False}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def git(path, *arguments):
    return subprocess.check_output(["git", "-C", str(path), *arguments], text=True).strip()

try:
    common = project / "xnu-sources/UniXNU"
    subprocess.run([sys.executable, str(here / "prepare-sources.py"), "--check"], check=True)
    ufs_candidate = args.kind in ("security-t70-ufs", "security-t70-ufs-hfs")
    virtio_candidate = args.kind == "security-virtio" or ufs_candidate
    variant = "t70-ufs" if ufs_candidate else "virtio"
    build_record_path = project / ("analysis/xnu-kernel-" + variant + "-build.json" if virtio_candidate else
                                   "analysis/xnu-kernel-build.json")
    build = json.loads(build_record_path.read_text())
    image_record = build.get("image", {})
    native = Path(image_record["stage"]).resolve()
    native_digest = digest(native)
    expected_status = "driver_linked_kernel_built_runtime_unqualified" if virtio_candidate else "passed"
    expected_stage = (project / ("tools/xnu-prelink-ios/" + variant + "-native/kernel.macho")).resolve()
    if (build.get("status") != expected_status or build.get("os_abi") != "ios" or
            native_digest != image_record.get("sha256") or
            (virtio_candidate and native != expected_stage)):
        raise RuntimeError("A matching complete iOS kernel candidate is required")
    driver_report = None
    if virtio_candidate:
        driver = build.get("virtio_mmio_block")
        source = build.get("virtio_mmio_block_sources")
        if not isinstance(driver, dict) or not isinstance(source, dict):
            raise RuntimeError("The VirtIO driver is not linked into the isolated kernel candidate")
        defined = subprocess.check_output(
            [str(project / "tools/unixnu-linux/CommandLineTools/usr/bin/nm"), "-g", str(native)], text=True)
        names = {fields[-1] for line in defined.splitlines()
                 if len(fields := line.split()) >= 3 and fields[-2] not in ("U", "u")}
        required = set(driver["required_symbols"])
        if not required or required - names:
            raise RuntimeError("Full VirtIO kernel lacks driver symbols: " + str(sorted(required - names)))
        report["linked_driver_symbols_checked"] = sorted(required)
        source_root = Path(source["repository"]).resolve()
        if (not source_root.is_relative_to(project.resolve()) or
                git(source_root, "rev-parse", "HEAD") != source.get("revision") or
                source.get("revision") != driver.get("source_revision") or
                digest(source_root / "NativeVirtioBlock.cpp") != driver.get("source_sha256")):
            raise RuntimeError("The pinned VirtIO driver source changed after the kernel build")
        def project_file(value):
            path = Path(value).resolve()
            if not path.is_relative_to(project.resolve()):
                raise RuntimeError("VirtIO build metadata points outside the project")
            return path
        generated_source = project_file(driver["generated_source"])
        generator = here / "Drivers/build_virtio_mmio.py"
        bindings_path = project_file(driver["bindings"])
        audit_path = project_file(driver["storage_abi_audit"])
        qualification_path = project_file(driver["storage_qualification_report"])
        binding_record = json.loads(bindings_path.read_text())
        audit_record = json.loads(audit_path.read_text())
        if (digest(generated_source) != driver.get("generated_source_sha256") or
                digest(generator) != driver.get("generator_sha256") or
                digest(bindings_path) != driver.get("bindings_sha256") or
                digest(audit_path) != driver.get("storage_abi_audit_sha256") or
                digest(qualification_path) != driver.get("storage_qualification_report_sha256") or
                audit_record.get("method_slots") != driver.get("storage_abi_method_slots") or
                audit_record.get("original_sha256") != driver.get("storage_abi_original_sha256") or
                audit_record.get("candidate_sha256") != driver.get("storage_abi_candidate_sha256") or
                driver.get("storage_abi_unresolved_original_imports") != 0 or
                binding_record.get("unresolved_original_imports") or
                not driver.get("binding_addresses_already_include_slide")):
            raise RuntimeError("VirtIO source, original imports, or storage ABI provenance does not match")
        driver_report = {key: driver[key] for key in (
            "source_revision", "source_sha256", "generated_source_sha256", "generator_sha256",
            "bindings_sha256", "storage_abi_audit_sha256", "storage_qualification_report_sha256",
            "storage_abi_original_sha256", "storage_abi_candidate_sha256", "storage_abi_slide",
            "binding_addresses_already_include_slide", "storage_abi_method_slots",
            "storage_abi_kernel_bindings", "storage_abi_unresolved_original_imports")}
        driver_report["original_import_bindings"] = binding_record["bindings"]
        driver_report["linked_into_kernel"] = True
        driver_report["linked_kernel_sha256"] = native_digest
        driver_report["unresolved_original_imports"] = binding_record.get("unresolved_original_imports", [])
    if ufs_candidate:
        ufs = build.get("t70_ufs", {})
        if (not ufs.get("linked_into_kernel") or ufs.get("linked_kernel_sha256") != native_digest or
                not set(ufs.get("required_symbols", [])) or set(ufs["required_symbols"]) - names or
                ufs.get("storage_abi_audit_sha256") != driver["storage_abi_audit_sha256"] or
                ufs.get("virtual_methods_checked") != 190 or
                digest(project_file(ufs["archive"])) != ufs.get("archive_sha256") or
                digest(project_file(ufs["bindings"])) != ufs.get("bindings_sha256") or
                digest(project_file(ufs["generated_catalogue"])) != ufs.get("generated_catalogue_sha256") or
                digest(project_file(ufs["catalogue_object"])) != ufs.get("catalogue_object_sha256")):
            raise RuntimeError("T70 UFS kernel archive, registration, symbols or storage ABI changed")
        for item in ufs["files"] + ufs["generated_fdt_files"]:
            if digest(project_file(item["path"])) != item["sha256"]:
                raise RuntimeError("T70 UFS source changed after the complete kernel build: " + item["path"])
        report["ufs_driver_metadata"] = ufs
    kernel = project / "xnu-sources/xnu-mtk"
    for name, expected in build["kernel_source_sha256"].items():
        if digest(kernel / name) != expected:
            raise RuntimeError("Kernel source changed after build: " + name)
    spec = next(c for c in tomllib.loads((common / "components.toml").read_text())["component"]
                if c["id"] == "XnuPrelink")
    kit = project / "xnu-sources/XnuPrelink"
    if not kit.exists():
        subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout", spec["repository"], str(kit)], check=True)
        subprocess.run(["git", "-C", str(kit), "checkout", "--detach", spec["revision"]], check=True)
    if git(kit, "rev-parse", "HEAD") != spec["revision"] or git(kit, "status", "--porcelain"):
        raise RuntimeError("Prelink source pin/cleanliness mismatch; existing files preserved")
    profile_path = common / "profiles/os/ipados-17.7.11-21H461/profile.toml"
    profile = tomllib.loads(profile_path.read_text())
    if virtio_candidate and driver_report["storage_abi_slide"] != int(profile["kernel"]["prelink"]["slide"], 0):
        raise RuntimeError("VirtIO original-import bindings use a different iOS kernel slide")
    report.update({"native_sha256": native_digest, "native": str(native),
                   "native_build_record": str(build_record_path),
                   "native_build_record_sha256": digest(build_record_path),
                   "driver_metadata": driver_report, "profile_sha256": digest(profile_path),
                   "common_commit": git(common, "rev-parse", "HEAD"), "prelink_commit": spec["revision"],
                   "prelink_repository": spec["repository"], "os_version": profile["ipsw"]["version"],
                   "os_build": profile["ipsw"]["build"]})
    sys.path.insert(0, str(common / "scripts"))
    from unixnu.ipsw import Ipsw, kernelcache
    os.environ["PATH"] = str(project / "tools/unixnu-linux/bin") + os.pathsep + os.environ["PATH"]
    ctx = SimpleNamespace(board=profile, out_root=project / "tools/xnu-os-ios", option_text=lambda *args: None)
    source = Ipsw(ctx)
    report["apple_source"] = source.spec
    report["manifest_identity"] = {"version": source.manifest["ProductVersion"],
        "build": source.manifest["ProductBuildVersion"], "device_class": source.identity["Info"]["DeviceClass"],
        "kernelcache_member": source.member("KernelCache")}
    original = kernelcache(ctx, source)
    original_digest = digest(original)
    if original_digest != profile["kernel"]["prelink"]["kernelcache_sha256"]:
        raise RuntimeError("Original iOS kernelcache differs from the pinned identity")
    report.update(original=str(original), original_sha256=original_digest)
    checked = subprocess.run([sys.executable, "-B", "-m", "unittest", "discover", "-p", "test_*.py"],
                             cwd=kit, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    report["component_checks"] = {"exit_code": checked.returncode, "output": checked.stdout}
    checked.check_returncode()
    sys.path.insert(0, str(kit))
    from emit_prelink import emit
    roots = ["com.apple.security.AppleImage4"]
    initializers = {}
    if args.kind != "image4":
        roots.append("com.apple.driver.AppleMobileFileIntegrity")
        identifier, address = profile["kernel"]["prelink"]["early_security_init"].split("=", 1)
        if identifier != "com.apple.driver.AppleMobileFileIntegrity":
            raise RuntimeError("Unexpected OS security initializer profile")
        initializers[identifier] = int(address, 0)
    if args.kind == "security-t70-ufs-hfs":
        roots.append("com.apple.filesystems.hfs.kext")
        report["filesystem_module"] = "com.apple.filesystems.hfs.kext"
    image, plan = emit(native.read_bytes(), original.read_bytes(), roots,
                       int(profile["kernel"]["prelink"]["slide"], 0), initializers)
    output = project / ("tools/xnu-prelink-ios/" + args.kind + "-" + native_digest[:12] + "/kernel.macho")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists() and output.read_bytes() != image:
        raise RuntimeError("A different existing prelink candidate was preserved")
    if not output.exists():
        output.write_bytes(image)
    metadata = output.with_suffix(".json")
    metadata.write_text(json.dumps(plan, indent=2) + "\n")
    checked = subprocess.run([sys.executable, str(project / "xnu-sources/DarwinBoot/Test/test-kernel-parser.py"),
                              str(output)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    report["darwinboot_parser"] = {"exit_code": checked.returncode, "output": checked.stdout}
    print(checked.stdout, end="")
    checked.check_returncode()
    if digest(native) != native_digest:
        raise RuntimeError("Native stage changed during separate module preparation")
    report.update(status="emitted_parser_checked_runtime_unqualified", path=str(output), report=str(metadata),
                  roots=roots, components=plan["components"], pointer_counts=plan["pointer_counts"],
                  bytes=len(image), sha256=digest(output), metadata_sha256=digest(metadata), native_stage_unchanged=True)
    print("PREPARED:", output, len(image), "bytes; real module imports bound, runtime unqualified")
except Exception as error:
    report.update(status="failed", error=str(error))
    raise
finally:
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report["builder_script_sha256"] = digest(Path(__file__))
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path)
