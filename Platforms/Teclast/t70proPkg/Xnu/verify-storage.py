#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify current storage sources, UFS lifecycle checks and actual VM root-selection/HFS mount evidence."""
import argparse
import ast
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import runpy
import struct
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
             ["with-build-lock", "T70Pro", "Verify XNU storage integration", "--",
              sys.executable, str(Path(__file__).resolve()), "--locked"])
sys.dont_write_bytecode = True
report_path = project / "analysis/xnu-storage-verification.json"
report = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "status": "running", "device_boot": "not_run", "filesystem_mount": "not_run",
          "whole_os_runtime_qualified": False, "checks": []}

def digest(path):
    target = Path(path)
    return hashlib.sha256((target if target.is_absolute() else project / target).read_bytes()).hexdigest()

def read(name):
    return json.loads((project / "analysis" / name).read_text())

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def passed(label):
    report["checks"].append(label)
    print("PASS:", label, flush=True)

try:
    fixture = read("xnu-qemu-root-fixture.json")
    data = Path(fixture["path"]).read_bytes()
    require(hashlib.sha256(data).hexdigest() == fixture["sha256"], "GPT fixture changed")
    tree = ast.parse((here / "prepare-qemu-root-fixture.py").read_text())
    nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) or
             (isinstance(n, ast.Assign) and all(isinstance(t, ast.Name) and t.id.isupper() for t in n.targets))]
    fixture_ns = {"struct": struct, "hashlib": hashlib, "uuid": uuid, "zlib": zlib}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "actual-fixture-functions", "exec"), fixture_ns)
    facts = fixture_ns["verify"](data)
    malformed = fixture_ns["self_check"](data)
    require(len(malformed) == 5 and facts["partition_guid"] == fixture["verification"]["partition_guid"],
            "Actual GPT verification/self-check results differ")
    passed("actual GPT CRC/geometry/payload verification and five malformed-input refusals")

    runner_path = here / "qemu-check.py"
    runner_tree = ast.parse(runner_path.read_text())
    functions = [n for n in runner_tree.body if isinstance(n, ast.FunctionDef) and
                 n.name in ("parse_root_matching_log", "_check_root_matching_log")]
    require(len(functions) == 2, "Actual root-log parser/self-check is missing")
    runner_ns = {"re": re, "plistlib": plistlib}
    exec(compile(ast.Module(body=functions, type_ignores=[]), "actual-root-log-functions", "exec"), runner_ns)
    runner_ns["_check_root_matching_log"]()
    passed("actual root-log parser: nested ID attributes, truncation, absence and HFS fallback")

    build = read("xnu-kernel-virtio-build.json")
    prelink = read("xnu-ios-security-virtio-prelink-build.json")
    require(build["status"] == "driver_linked_kernel_built_runtime_unqualified" and
            not build["undefined_symbols"], "Full isolated driver kernel did not link")
    require(digest(build["image"]["stage"]) == build["image"]["sha256"] == prelink["native_sha256"],
            "Native VirtIO kernel identity mismatch")
    require(digest(prelink["path"]) == prelink["sha256"] and
            digest(prelink["report"]) == prelink["metadata_sha256"] and
            digest(project / "analysis/xnu-kernel-virtio-build.json") == prelink["native_build_record_sha256"],
            "Candidate/prelink build identities changed")
    require(digest(here / "build-kernel.py") == build["builder_script_sha256"] and
            digest(here / "prelink-ios.py") == prelink["builder_script_sha256"], "Build scripts changed")
    driver = build["virtio_mmio_block"]
    require(driver["linked_into_kernel"] and len(driver["linked_symbols_checked"]) == 14,
            "Driver symbols were not verified in the full kernel")
    for path_key, hash_key in [("generated_source", "generated_source_sha256"), ("archive", "archive_sha256"),
                               ("object", "object_sha256"), ("bindings", "bindings_sha256"),
                               ("storage_abi_audit", "storage_abi_audit_sha256"),
                               ("storage_qualification_report", "storage_qualification_report_sha256")]:
        require(digest(driver[path_key]) == driver[hash_key], "Driver input changed: " + path_key)
    qualification = read("xnu-storage-abi-qualification.json")
    require(qualification["audit_sha256"] == driver["storage_abi_audit_sha256"] and
            qualification["qualification_script_sha256"] == digest(here / "qualify-storage-abi.py") and
            qualification["method_slots"] == 190 and qualification["kernel_bindings"] == 176 and
            qualification["live_native_4k"], "Measured original storage ABI evidence changed")
    passed("full driver kernel, prelink candidate and measured 190-slot storage ABI hashes")

    driver_api = runpy.run_path(str(here / "Drivers/build_virtio_mmio.py"))
    original = project / "xnu-sources/VirtioMmioBlock/NativeVirtioBlock.cpp"
    original_text = original.read_text()
    require(digest(original) == driver_api["PIN_SHA"] == driver["source_sha256"],
            "Pinned original driver changed")
    require(driver_api["instrument"](original_text) == Path(driver["generated_source"]).read_text(),
            "Generated driver source differs from actual instrumentation")
    for old, new in [("*request = {0, 0, sector};", "*request = {1, 0, sector};"),
                     ('memcmp(compatible->getBytesNoCopy(), "virtio,mmio", 12)',
                      'memcmp(compatible->getBytesNoCopy(), "invalid,mmio", 12)'),
                     ("if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;",
                      "if (false) return kIOReturnNotWritable;")]:
        require(old in original_text, "Pinned negative-control anchor changed")
        try:
            driver_api["instrument"](original_text.replace(old, new, 1))
        except SystemExit:
            continue
        raise RuntimeError("Driver preparation accepted a changed read-only/provider rule")
    passed("actual driver instrumentation and three changed-write/provider-rule refusals")

    efi = {"vm_launcher": mu / "Build/t70proPkg/RELEASE_CLANGPDB/AARCH64/T70QemuXnu.efi",
           "darwinboot": mu / "Build/t70proPkg/RELEASE_CLANGPDB/XnuStage/EFI/Teclast/DarwinBoot.efi"}
    efi_hashes = {name: digest(path) for name, path in efi.items()}
    runtime = {}
    for checkpoint in ("block", "root"):
        record = read("xnu-qemu-prelink-security-virtio-" + checkpoint + "-execution.json")
        require(record["status"] == "passed" and record["runner_sha256"] == digest(runner_path) and
                record["kernel_sha256"] == record["executed_kernel_sha256"] == prelink["sha256"] and
                record["efi_sha256"] == efi_hashes and record["staged_kernel_unchanged"],
                "Storage VM evidence inputs changed: " + checkpoint)
        io = record["virtio_block_io"]
        require(io["ready"] == 1 and io["read_failures"] == io["write_requests"] == 0 and
                io["sectors"] == len(data) // 512 and io["bytes_read"] >= 512 and
                io["first_sector_sha256"] == hashlib.sha256(data[:512]).hexdigest() and
                io["transport_registers_verified"] and io["active_fdt_node_verified"] and
                record["synthetic_root"]["unchanged"] and record["native_4k_mmu_observed"],
                "Actual block I/O identity/data checks failed")
        runtime[checkpoint] = {"status": record["status"], "io": io}
        if checkpoint == "root":
            root = record["root_selection"]
            require(root["return_observed"] and root["kern_return"] == 0 and root["device_number_valid"] and
                    root["root_match_uuid_verified"] and root["bsd_root_log_matches_return"] and
                    record["root_match_log_evidence"]["dictionary_uuid"] == facts["partition_guid"] and
                    not root["filesystem_mount_observed"], "Actual root selection return is unqualified")
            runtime[checkpoint]["selection"] = root
    report["virtual_storage"] = runtime
    passed("actual DMA reads and IOFindBSDRoot return selecting the requested GPT partition")

    ufs = read("xnu-t70-ufs-adapter.json")
    for item in ufs["files"]:
        require(digest(item["path"]) == item["sha256"], "UFS source changed: " + item["path"])
    require(ufs["check_exit_code"] == ufs["post_submit_fault_checks"]["exit_code"] == 0 and
            ufs["post_submit_fault_checks"]["host_sanitizer"]["run_exit_code"] == 0,
            "UFS DMA-fault tests failed")
    link = ufs["cxx_interop"]["native_darwin_link"]
    compile_results = link["compile_results"]
    required_sources = {"T70UfsReadOnly.c", "MtkUfsTransport.c", "MtkUfsCore.c", "C++ caller"}
    require(link["link_exit_code"] == link["undefined_symbol_count"] == 0 and link["api_symbol_found"] and
            not link["dynamic_lookup_used"] and
            all(target in compile_results and required_sources.issubset(compile_results[target]) and
                all(result["exit_code"] == 0 for result in compile_results[target].values())
                for target in ("arm64-apple-ios17.0", "arm64-apple-macos14.0")),
            "UFS C/C++ cross-target compilation or Darwin link failed")
    hardware = ufs["hardware_source"]["full_effective_dtb"]
    require(digest(hardware["path"]) == hardware["sha256"] and not ufs["physical_ufs_verified"] and
            ufs["driver_attached_to_iokit"] == "not_integrated", "UFS resource/runtime scope changed")
    report["physical_ufs"] = {"status": ufs["status"], "hardware_io_verified": False,
                              "iokit_provider": "not_integrated"}
    passed("UFS source hashes, sanitized DMA fault tests, ARM64 compilation and C++ Darwin link")

    host = read("xnu-t70-ufs-host-verification.json")
    require(host["status"] == "passed" and
            host["asan_ubsan"]["compile_exit_code"] == host["asan_ubsan"]["run_exit_code"] == 0 and
            host["cpp_abi_smoke"]["result"] == "pass", "Retained UFS host tests failed")
    for path, expected in host["source_hashes"].items():
        require(digest(path) == expected, "Retained UFS host source changed")
    for name in ("device-tree", "memory", "block-method"):
        checked = read("xnu-t70-ufs-" + name + "-verification.json")
        require(checked["status"] == "passed" and
                checked["compile_exit_code"] == checked["run_exit_code"] == 0,
                "UFS source execution failed: " + name)
        for item in checked["files"] + checked.get("ownership_evidence", []):
            require(digest(item["path"]) == item["sha256"], "UFS tested input changed: " + item["path"])
        if name == "block-method":
            require(checked["source_unchanged_during_run"] and len(checked["failure_cases"]) >= 5 and
                    len(checked.get("success_cases", [])) >= 2 and
                    all(case["observed"] for case in checked["failure_cases"] + checked["success_cases"]) and
                    any(case.get("expected_completion") == "IOError/131072 bytes" for case in checked["failure_cases"]),
                    "Actual IOBlockStorageDevice normal, split and partial-completion execution is incomplete")
    passed("actual UFS host, stock FDT, RAM ownership guard and concrete block-method fault execution")

    physical_build_path = project / "analysis/xnu-kernel-t70-ufs-build.json"
    physical_build = read(physical_build_path.name)
    linked = physical_build["t70_ufs"]
    require(physical_build["status"] == "driver_linked_kernel_built_runtime_unqualified" and
            physical_build["builder_script_sha256"] == digest(here / "build-kernel.py") and
            not physical_build["undefined_symbols"] and linked["linked_into_kernel"] and
            linked["linked_kernel_sha256"] == physical_build["image"]["sha256"] ==
                digest(physical_build["image"]["stage"]) and len(linked["linked_symbols_checked"]) == 13,
            "Concrete UFS provider/block class did not link into the full current kernel")
    for path_key, hash_key in (("archive", "archive_sha256"), ("bindings", "bindings_sha256"),
            ("shared_original_bindings", "shared_original_bindings_sha256"),
            ("generated_catalogue", "generated_catalogue_sha256"), ("catalogue_object", "catalogue_object_sha256")):
        require(digest(linked[path_key]) == linked[hash_key], "UFS build artifact changed: " + path_key)
    for item in linked["files"] + linked["generated_fdt_files"]:
        require(digest(item["path"]) == item["sha256"], "Linked UFS input changed: " + item["path"])
    own_bindings = json.loads(Path(linked["bindings"]).read_text())["bindings"]
    shared_bindings = json.loads(Path(linked["shared_original_bindings"]).read_text())["bindings"]
    require(all(shared_bindings.get(name) == address for name, address in own_bindings.items()) and
            linked["generator_sha256"] == digest(here / "Drivers/build_t70_ufs.py") and
            linked["virtual_methods_checked"] == 190, "UFS original-storage bindings changed")
    passed("full UFS provider/block kernel, generated IOKit catalogue and shared original storage ABI")

    hfs = read("xnu-hfs-root-fixture.json")
    require(hfs["status"] == "verified_synthetic_hfsplus_gpt_fixture" and
            hfs["builder_script_sha256"] == digest(here / "prepare-hfs-root-fixture.py") and
            hfs["read_only_fsck"]["exit_code"] == 0 and hfs["read_only_fsck"]["standard_clean"],
            "HFS+ fixture provenance or filesystem check failed")
    hfs_data = Path(hfs["path"]).read_bytes()
    require(hashlib.sha256(hfs_data).hexdigest() == hfs["sha256"], "HFS+ fixture changed")
    hfs_tree = ast.parse((here / "prepare-hfs-root-fixture.py").read_text())
    nodes = [n for n in hfs_tree.body if isinstance(n, ast.FunctionDef) or
             (isinstance(n, ast.Assign) and all(isinstance(t, ast.Name) and t.id.isupper() for t in n.targets))]
    hfs_ns = {"Path": Path, "project": project, "hashlib": hashlib, "struct": struct, "uuid": uuid, "zlib": zlib}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), "actual-hfs-fixture-functions", "exec"), hfs_ns)
    hfs_facts = hfs_ns["verify_disk"](hfs_data)
    require(len(hfs_ns["malformed_input_checks"](hfs_data)) == 7 and
            hfs_facts["partition_guid"] == hfs["verification"]["partition_guid"],
            "Actual HFS/GPT parser or seven malformed-input controls failed")
    functions = [n for n in runner_tree.body if isinstance(n, ast.FunctionDef) and
                 n.name in ("mount_root_evidence", "_check_mount_root_evidence")]
    require(len(functions) == 2, "Actual mount root proof/self-check is missing")
    exec(compile(ast.Module(body=functions, type_ignores=[]), "actual-mount-root-proof", "exec"), runner_ns)
    runner_ns["_check_mount_root_evidence"]()
    passed("actual HFS+ and GPT validation, seven malformed disks and seven false mount-identity controls")
    functions = [n for n in runner_tree.body if isinstance(n, ast.FunctionDef) and
                 n.name in ("root_vnode_evidence", "init_process_evidence",
                            "_check_root_vnode_evidence", "_check_init_process_evidence")]
    require(len(functions) == 4, "Actual root vnode and PID1 proof/self-check is missing")
    exec(compile(ast.Module(body=functions, type_ignores=[]), "actual-root-continuation-proof", "exec"), runner_ns)
    runner_ns["_check_init_process_evidence"]()
    passed("fourteen false root-vnode states and eight false PID1 states refused by the actual observers")

    physical_runtime = {}
    for kind, checkpoint in (("security-t70-ufs", "block"), ("security-t70-ufs", "root"),
                            ("security-t70-ufs-hfs", "root"), ("security-t70-ufs-hfs", "mount"),
                            ("security-t70-ufs-hfs", "root-vnode"), ("security-t70-ufs-hfs", "init")):
        prepared = read("xnu-ios-" + kind + "-prelink-build.json")
        require(prepared["status"].startswith("emitted") and
                digest(prepared["path"]) == prepared["sha256"] and
                digest(prepared["report"]) == prepared["metadata_sha256"] and
                prepared["native_build_record_sha256"] == digest(physical_build_path) and
                prepared["builder_script_sha256"] == digest(here / "prelink-ios.py") and
                prepared["native_sha256"] == physical_build["image"]["sha256"] and
                prepared["ufs_driver_metadata"] == linked, "UFS module candidate provenance changed")
        record = read("xnu-qemu-prelink-" + kind + "-" + checkpoint + "-execution.json")
        selected_fixture = hfs if kind.endswith("-hfs") else fixture
        selected_data = hfs_data if kind.endswith("-hfs") else data
        require(record["status"] == "passed" and record["runner_sha256"] == digest(runner_path) and
                record["kernel_sha256"] == record["executed_kernel_sha256"] == prepared["sha256"] and
                record["native_kernel_sha256"] == physical_build["image"]["sha256"] and
                record["efi_sha256"] == efi_hashes and record["staged_kernel_unchanged"] and
                record["native_4k_mmu_observed"] and record["synthetic_root"]["unchanged"] and
                record["synthetic_root"]["sha256_before"] == record["synthetic_root"]["sha256_after"] ==
                    selected_fixture["sha256"], "UFS VM runtime identities changed")
        io = record["virtio_block_io"]
        require(io["ready"] == 1 and io["read_failures"] == io["write_requests"] == 0 and
                io["sectors"] == len(selected_data) // 512 and io["bytes_read"] >= 512 and
                io["first_sector_sha256"] == hashlib.sha256(selected_data[:512]).hexdigest() and
                io["transport_registers_verified"] and io["active_fdt_node_verified"],
                "UFS candidate disturbed actual virtual disk I/O")
        refusal = record["t70_ufs_provider_refusal"]
        require(refusal["verified"] and
                refusal["ready"] == refusal["start_attempts"] == refusal["mmio_accesses"] == refusal["dma_quarantines"] == 0 and
                refusal["probe_attempts"] == refusal["probe_refusals"] and
                (checkpoint == "block" or refusal["probe_attempts"] > 0),
                "Actual UFS IOKit refusal before hardware access is unproven")
        if checkpoint == "root":
            selected = record["root_selection"]
            require(selected["return_observed"] and selected["kern_return"] == 0 and
                    selected["root_match_uuid_verified"] and selected["bsd_root_log_matches_return"] and
                    selected["device_number_valid"], "UFS candidate root selection failed")
        if checkpoint in ("mount", "root-vnode", "init"):
            mounted = record["filesystem_mount"]
            proof = mounted["root_match_evidence"]
            require(mounted["return_observed"] and mounted["return_code"] == 0 and
                    mounted["mount_pointer"] and mounted["filesystem"] == "hfs" and mounted["read_only"] and
                    mounted["mount_observed"] and record["synthetic_root"]["mount_observed"] and
                    all(proof[name] for name in ("provider_is_iomedia", "query_uuid_matches", "bsd_log_matches_mount")) and
                    proof["query_uuid"].casefold() == hfs_facts["partition_guid"].casefold() and
                    mounted["volume_authentication"] == mounted["userspace"] == "not_run" and
                    mounted["root_vnode_setup"] == ("not_run" if checkpoint == "mount" else "observed"),
                    "Actual read-only HFS root mount/identity proof failed")
            report["filesystem_mount"] = mounted
        if checkpoint in ("root-vnode", "init"):
            vnode_proof = runner_ns["root_vnode_evidence"](record["observation"])
            require(vnode_proof["verified"] and record["root_vnode_setup"]["verified"] and
                    vnode_proof == record["root_vnode_setup"]["proof"],
                    "Actual root vnode/current directory proof failed")
            report["root_vnode_setup"] = record["root_vnode_setup"]
        if checkpoint == "init":
            init_proof = runner_ns["init_process_evidence"](record["observation"])
            require(init_proof["verified"] and record["init_process"]["entry_observed"] and
                    init_proof == record["init_process"]["proof"] and
                    record["init_process"]["pid"] == 1 and
                    record["init_process"]["userspace_execution"] == "not_run" and
                    record["init_process"]["root_authentication"] ==
                        "not_requested_by_inherited_ios_build_policy",
                    "Actual PID1 launchd-loader entry proof or scope failed")
            report["init_process"] = record["init_process"]
        physical_runtime[kind + "-" + checkpoint] = {"status": record["status"], "io": io, "ufs_refusal": refusal}
    report["physical_ufs"] = {"status": "provider_and_block_class_linked_vm_refusal_verified",
        "iokit_provider": "compiled_linked_and_probe_observed", "hardware_io_verified": False,
        "device_boot": "not_run", "runtime": physical_runtime,
        "native_kernel_sha256": physical_build["image"]["sha256"]}
    current = read("xnu-root-continuation-integration.json")
    require(current["status"] == "root_vnode_and_pid1_entry_passed_physical_os_incomplete" and
            current["canonical_inputs_before"] == current["canonical_inputs_after"] and
            all(digest(path) == expected for path, expected in current["canonical_inputs_after"].items()),
            "Canonical kernels/ADTs changed during UFS/HFS integration")
    passed("six current VM trials: UFS refusal, reads, UUID root, HFS mount, root vnode and PID1 loader entry")

    integration = read("xnu-parallel-integration.json")
    require(integration["status"] == "block_read_and_root_selection_passed_filesystem_unqualified" and
            all(digest(path) == expected for path, expected in integration["canonical_inputs_before"].items()),
            "Canonical stages changed during isolated storage work")
    baseline = read("xnu-parallel-baseline-verification.json")
    require(baseline["status"] == "native_and_prelinked_initialization_checks_passed_whole_os_incomplete" and
            len(baseline["cases"]) == 13, "Baseline initialization verification incomplete")
    for suffix in ("", "-macos"):
        checked = read("xnu-kernel" + suffix + "-verification.json")
        require(checked["status"] == "passed" and not checked["ignored_qemu_evidence"] and
                len(checked["qemu_evidence"]) == 3, "Canonical runtime evidence changed")
    passed("canonical stages preserved and eleven baseline VM cases plus two kernel checks passed")
    report["status"] = "virtual_hfs_root_and_pid1_entry_verified_physical_os_boot_incomplete"
except Exception as error:
    report.update(status="failed", error=str(error))
    raise
finally:
    report["verification_script_sha256"] = digest(__file__)
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path, flush=True)
