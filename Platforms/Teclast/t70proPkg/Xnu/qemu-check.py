#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the unchanged built T70PRO kernel on QEMU's actual virtual resources."""
import argparse
import bisect
import datetime
import hashlib
import json
import os
import plistlib
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import uuid
def parse_root_matching_log(kernel_log):
    waiting = list(re.finditer(r"Waiting on\s*(<dict\b[^>]*>)", kernel_log))
    if not waiting:
        return None
    start = waiting[-1].start(1)
    depth = 0
    for tag in re.finditer(r"<dict\b[^>]*>|</dict\s*>", kernel_log[start:]):
        depth += -1 if tag.group().startswith("</") else 1
        if depth == 0:
            fragment = kernel_log[start:start + tag.end()]
            try:
                value = plistlib.loads(('<plist version="1.0">' + fragment + '</plist>').encode())
                return value if isinstance(value, dict) else None
            except Exception:
                return None
    return None

def _check_root_matching_log():
    raw = ('Waiting on <dict ID="0"><key>IOProviderClass</key><string ID="1">IOMedia</string>'
           '<key>IOPropertyMatch</key><dict ID="2"><key>UUID</key><string ID="3">TEST-UUID</string></dict></dict>')
    assert parse_root_matching_log(raw) == {"IOProviderClass": "IOMedia", "IOPropertyMatch": {"UUID": "TEST-UUID"}}
    assert parse_root_matching_log(raw[:-7]) is None
    assert parse_root_matching_log("No matching dictionary") is None
    fallback = "Waiting on <dict ID=\"0\"><key>Content</key><string>Apple_HFS</string></dict>"
    assert parse_root_matching_log(fallback) == {"Content": "Apple_HFS"}

def mount_root_evidence(kernel_log, expected_uuid, root_name, root_dev_t):
    matching = parse_root_matching_log(kernel_log)
    properties = matching.get("IOPropertyMatch") if isinstance(matching, dict) else None
    query_uuid = properties.get("UUID") if isinstance(properties, dict) else None
    lines = list(re.finditer(r"BSD root:\s*(\S+),\s*major\s+(\d+),\s*minor\s+(\d+)", kernel_log))
    bsd = lines[-1] if lines else None
    valid_device = isinstance(root_dev_t, int) and 0 <= root_dev_t < 0xffffffff
    agrees = bool(valid_device and bsd and bsd.group(1) == root_name and
                  int(bsd.group(2)) == (root_dev_t >> 24) & 0xff and
                  int(bsd.group(3)) == root_dev_t & 0xffffff)
    return {"provider_is_iomedia": isinstance(matching, dict) and matching.get("IOProviderClass") == "IOMedia",
            "query_uuid": query_uuid,
            "query_uuid_matches": isinstance(query_uuid, str) and query_uuid.casefold() == expected_uuid.casefold(),
            "bsd_log_matches_mount": agrees}

def _check_mount_root_evidence():
    log = ('Waiting on <dict ID="0"><key>IOProviderClass</key><string>IOMedia</string>'
           '<key>IOPropertyMatch</key><dict><key>UUID</key><string>TEST-UUID</string></dict></dict>'
           '\nBSD root: disk0s1, major 1, minor 1\n')
    def accepted(text=log, name="disk0s1", number=0x1000001, wanted="TEST-UUID"):
        result = mount_root_evidence(text, wanted, name, number)
        return all(result[key] for key in ("provider_is_iomedia", "query_uuid_matches", "bsd_log_matches_mount"))
    assert accepted()
    assert not accepted(log.replace("IOMedia", "IOService"))
    assert not accepted(wanted="OTHER-UUID")
    assert not accepted(name="disk0s2")
    assert not accepted(number=0x1000002)
    assert not accepted(log.split("\nBSD root:")[0])
    assert not accepted(number=0xffffffff)
    assert not accepted("No root-selection log")


def root_vnode_evidence(observed):
    layout = observed.get("root_vnode_layout", {})
    root = observed.get("root_vnode", 0)
    mount = observed.get("mount_pointer", 0)
    proof = {
        "setup_entry_observed": observed.get("root_setup_entry_observed") is True,
        "devfs_mountpoint_is_dev": observed.get("root_setup_mountpoint") == "/dev",
        "root_pointer_present": bool(root),
        "mount_matches": bool(mount and observed.get("root_vnode_mount") == mount),
        "directory": bool(layout.get("vdir_type") and observed.get("root_vnode_type") == layout["vdir_type"]),
        "vroot": bool(observed.get("root_vnode_flags", 0) & layout.get("vroot_flag", 0)),
        "root_mount": bool(observed.get("mount_flags", 0) & layout.get("mnt_rootfs_flag", 0)),
        "device_vnode_matches": bool(observed.get("root_device_vnode") and
                                     observed["root_device_vnode"] == observed.get("mount_device_vnode")),
        "kernel_process_root_directory": bool(observed.get("kernproc") and observed.get("kernproc_pid") == 0 and
                                              root and observed.get("kernproc_cdir") == root),
        "kernel_process_not_chrooted": bool(layout.get("fd_chroot_flag") and
                                            observed.get("kernproc_rdir") == 0 and
                                            observed.get("kernproc_fd_flags") is not None and
                                            not observed["kernproc_fd_flags"] & layout["fd_chroot_flag"]),
    }
    return {**proof, "verified": all(proof.values())}

def init_process_evidence(observed):
    layout = observed.get("root_vnode_layout", {})
    root = observed.get("root_vnode", 0)
    proof = {
        "entry_observed": observed.get("init_entry_observed") is True,
        "argument_is_initproc": bool(observed.get("initproc") and
                                    observed["initproc"] == observed.get("init_proc_argument") and
                                    observed["initproc"] != observed.get("kernproc")),
        "pid_one": observed.get("initproc_pid") == 1,
        "root_directory_inherited": bool(root and observed.get("initproc_cdir") == root),
        "not_chrooted": bool(layout.get("fd_chroot_flag") and observed.get("initproc_rdir") == 0 and
                            observed.get("initproc_fd_flags") is not None and
                            not observed["initproc_fd_flags"] & layout["fd_chroot_flag"]),
    }
    return {**proof, "verified": all(proof.values()) and root_vnode_evidence(observed)["verified"]}

def _check_root_vnode_evidence():
    good = {"root_vnode_layout": {"vdir_type": 2, "vroot_flag": 1, "mnt_rootfs_flag": 0x4000, "fd_chroot_flag": 1},
            "root_setup_entry_observed": True, "root_setup_mountpoint": "/dev", "root_vnode": 0x1000,
            "mount_pointer": 0x2000, "root_vnode_mount": 0x2000, "root_vnode_type": 2, "root_vnode_flags": 1,
            "mount_flags": 0x4000, "root_device_vnode": 0x3000, "mount_device_vnode": 0x3000,
            "kernproc": 0x4000, "kernproc_pid": 0, "kernproc_cdir": 0x1000, "kernproc_rdir": 0,
            "kernproc_fd_flags": 0}
    assert root_vnode_evidence(good)["verified"]
    for key, bad in (("root_setup_entry_observed", False), ("root_setup_mountpoint", "/wrong"), ("root_vnode", 0),
                     ("root_vnode_mount", 0x3000), ("root_vnode_type", 1), ("root_vnode_flags", 0),
                     ("mount_flags", 0), ("root_device_vnode", 0), ("kernproc", 0),
                     ("kernproc_pid", 1), ("kernproc_cdir", 0), ("kernproc_rdir", 0x1000),
                     ("kernproc_fd_flags", 1), ("root_vnode_layout", {})):
        assert not root_vnode_evidence({**good, key: bad})["verified"], key
    return good

def _check_init_process_evidence():
    good = {**_check_root_vnode_evidence(), "init_entry_observed": True, "initproc": 0x5000,
            "init_proc_argument": 0x5000, "initproc_pid": 1, "initproc_cdir": 0x1000,
            "initproc_rdir": 0, "initproc_fd_flags": 0}
    assert init_process_evidence(good)["verified"]
    for key, bad in (("init_entry_observed", False), ("init_proc_argument", 0), ("initproc", 0x4000),
                     ("initproc_pid", 0), ("initproc_cdir", 0), ("initproc_rdir", 0x1000),
                     ("initproc_fd_flags", 1), ("root_vnode_flags", 0)):
        assert not init_process_evidence({**good, key: bad})["verified"], key

_check_root_matching_log()
_check_mount_root_evidence()
_check_init_process_evidence()

here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--os", choices=("ios", "macos"), default="ios")
parser.add_argument("--prelink", choices=("image4", "security", "security-virtio", "security-t70-ufs", "security-t70-ufs-hfs"), help="Execute an isolated, pinned iOS module candidate")
parser.add_argument("--negative-post", action="store_true", help="Corrupt only the VM copy of the POST integrity field")
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
parser.add_argument("--checkpoint", choices=("crypto", "bsd", "root-entry", "block", "root", "mount", "root-vnode", "init"), default="crypto")
args = parser.parse_args()
ufs_candidate = args.prelink in ("security-t70-ufs", "security-t70-ufs-hfs")
hfs_candidate = args.prelink == "security-t70-ufs-hfs"
storage_candidate = args.prelink == "security-virtio" or ufs_candidate
if args.checkpoint in ("mount", "root-vnode", "init") and not hfs_candidate:
    parser.error("Mount and later checkpoints require --prelink security-t70-ufs-hfs")
if args.negative_post and args.checkpoint != "crypto":
    parser.error("The integrity refusal control stops at the crypto checkpoint")
if args.prelink and (args.os != "ios" or args.negative_post):
    parser.error("Prelinked module trials require iOS and cannot use the native POST corruption control")
if storage_candidate and args.checkpoint not in ("block", "root", "mount", "root-vnode", "init"):
    parser.error("Storage candidates require a block, root or supported later checkpoint")
if args.checkpoint in ("block", "root", "mount", "root-vnode", "init") and not storage_candidate:
    parser.error("Block and root-return checkpoints require --prelink security-virtio")
os_suffix = "-macos" if args.os == "macos" else ""
case_suffix = ("-prelink-" + args.prelink if args.prelink else "") + ("-negative-post" if args.negative_post else "-" + args.checkpoint if args.checkpoint != "crypto" else "")
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Offline QEMU XNU execution", "--",
              sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
sys.dont_write_bytecode = True
work = project / ("tools/xnu-qemu" + os_suffix + case_suffix)
esp = work / "esp"
(esp / "EFI/BOOT").mkdir(parents=True, exist_ok=True)
(esp / "EFI/Teclast").mkdir(parents=True, exist_ok=True)
(esp / "xnu").mkdir(exist_ok=True)
build = mu / "Build/t70proPkg/RELEASE_CLANGPDB/AARCH64"
stage = mu / ("Build/t70proPkg/RELEASE_CLANGPDB/XnuStage" + os_suffix)
build_record_path = project / ("analysis/xnu-kernel" + os_suffix + "-build.json")
if storage_candidate:
    build_record_path = project / ("analysis/xnu-kernel-t70-ufs-build.json" if ufs_candidate else "analysis/xnu-kernel-virtio-build.json")
build_record = json.loads(build_record_path.read_text())
if storage_candidate:
    native_kernel = Path(build_record["image"]["stage"]).resolve()
    expected_variant_kernel = (project / ("tools/xnu-prelink-ios/" + ("t70-ufs" if ufs_candidate else "virtio") + "-native/kernel.macho")).resolve()
    if native_kernel != expected_variant_kernel:
        raise RuntimeError("VirtIO build record names the wrong isolated native image stage")
else:
    native_kernel = stage / "xnu/kernel.macho"
native_digest = hashlib.sha256(native_kernel.read_bytes()).hexdigest()
expected_build_status = ("driver_linked_kernel_built_runtime_unqualified"
                         if storage_candidate else "passed")
if (build_record.get("status") != expected_build_status or build_record.get("os_abi") != args.os or
        build_record.get("image", {}).get("stage") is None or
        Path(build_record["image"]["stage"]).resolve() != native_kernel.resolve() or
        build_record["image"].get("sha256") != native_digest):
    raise RuntimeError("Selected kernel build status, image stage, or digest does not match")
kernel = native_kernel
prelink_record = None
if args.prelink:
    prelink_record = json.loads((project / ("analysis/xnu-ios-" + args.prelink + "-prelink-build.json")).read_text())
    assert prelink_record["status"].startswith("emitted") and prelink_record["native_sha256"] == native_digest
    assert prelink_record["original_sha256"] == "f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7"
    kernel = Path(prelink_record["path"]).resolve()
    assert kernel.is_relative_to((project / "tools/xnu-prelink-ios").resolve())
    assert hashlib.sha256(kernel.read_bytes()).hexdigest() == prelink_record["sha256"]
    assert hashlib.sha256(Path(prelink_record["report"]).read_bytes()).hexdigest() == prelink_record["metadata_sha256"]
    if storage_candidate:
        driver = build_record.get("virtio_mmio_block", {})
        prepared_driver = prelink_record.get("driver_metadata", {})
        driver_hash_fields = ("source_sha256", "generated_source_sha256", "generator_sha256", "bindings_sha256",
                              "storage_abi_audit_sha256", "storage_qualification_report_sha256",
                              "storage_abi_original_sha256", "storage_abi_candidate_sha256")
        if (prelink_record.get("native_build_record_sha256") != hashlib.sha256(build_record_path.read_bytes()).hexdigest() or
                any(prepared_driver.get(field) != driver.get(field) for field in driver_hash_fields) or
                not prepared_driver.get("original_import_bindings") or
                prepared_driver.get("unresolved_original_imports")):
            raise RuntimeError("VirtIO prelink provenance does not match the linked kernel build")
if ufs_candidate:
    ufs = build_record.get("t70_ufs", {})
    if (not ufs.get("linked_into_kernel") or ufs.get("linked_kernel_sha256") != native_digest or
            prelink_record.get("ufs_driver_metadata") != ufs):
        raise RuntimeError("T70 UFS prelink provenance differs from the linked kernel")
    for item in ufs["files"] + ufs["generated_fdt_files"]:
        path = Path(item["path"]).resolve()
        if not path.is_relative_to(project.resolve()) or hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
            raise RuntimeError("T70 UFS source changed after kernel build")
    for path_key, hash_key in (("archive","archive_sha256"),("bindings","bindings_sha256"),
                               ("generated_catalogue","generated_catalogue_sha256"),("catalogue_object","catalogue_object_sha256")):
        if hashlib.sha256(Path(ufs[path_key]).read_bytes()).hexdigest() != ufs[hash_key]:
            raise RuntimeError("T70 UFS archive or IOKit registration changed")
for source, destination in ((build / "T70QemuXnu.efi", esp / "EFI/BOOT/BOOTAA64.EFI"),
                            (stage / "EFI/Teclast/DarwinBoot.efi", esp / "EFI/Teclast/DarwinBoot.efi"),
                            (kernel, esp / "xnu/kernel.macho")):
    shutil.copyfile(source, destination)
original_digest = hashlib.sha256(kernel.read_bytes()).hexdigest()
if args.negative_post:
    metadata = json.loads(Path(build_record["crypto_integrity_record"]).read_text())
    assert metadata["kernel_sha256"] == original_digest
    copied_kernel = esp / "xnu/kernel.macho"
    damaged = bytearray(copied_kernel.read_bytes())
    integrity = int(metadata["hmac_file_offset"], 16)
    assert 0 <= integrity <= len(damaged)-32
    damaged[integrity] ^= 1
    copied_kernel.write_bytes(damaged)
common = project / "xnu-sources/UniXNU"
sys.path.insert(0, str(common / "scripts"))
from unixnu.devicetree import (node, empty_nvram_proxy, read_fdt, registers, physical,
                                hardware_properties, arm_io_window)
virtio_runtime = None
root_fixture = None
root_fixture_sha256 = None
root_matching_uuid = None
if storage_candidate:
    fixture_report_path = project / ("analysis/xnu-hfs-root-fixture.json" if hfs_candidate else "analysis/xnu-qemu-root-fixture.json")
    fixture_report = json.loads(fixture_report_path.read_text())
    root_fixture = project / ("tools/xnu-hfs-root-fixture/gpt-hfs-root.raw" if hfs_candidate else "tools/xnu-qemu-root-fixture/gpt-root.raw")
    if Path(fixture_report["path"]).resolve() != root_fixture.resolve():
        raise RuntimeError("The fixture report names an unexpected disk")
    root_fixture_sha256 = hashlib.sha256(root_fixture.read_bytes()).hexdigest()
    fixture_verification = fixture_report.get("verification", {})
    required_gpt_facts = ("primary_header_crc_valid", "backup_header_crc_valid",
        "primary_partition_array_crc_valid", "backup_partition_array_crc_valid", "protective_mbr_valid")
    expected_fixture_status = "verified_synthetic_hfsplus_gpt_fixture" if hfs_candidate else "fixture_verified_not_filesystem"
    self_checks = fixture_report.get("malformed_input_self_checks", []) if hfs_candidate else fixture_verification.get("malformed_input_self_checks", [])
    if hfs_candidate and (fixture_report.get("filesystem_type") != "HFS+" or
            fixture_report.get("read_only_fsck", {}).get("exit_code") != 0 or
            not fixture_report.get("read_only_fsck", {}).get("standard_clean") or
            fixture_report.get("builder_script_sha256") != hashlib.sha256((here / "prepare-hfs-root-fixture.py").read_bytes()).hexdigest()):
        raise RuntimeError("The HFS fixture lacks current formatting/fsck evidence")
    if (fixture_report.get("status") != expected_fixture_status or
            fixture_report.get("mounted") is not False or fixture_report.get("sha256") != root_fixture_sha256 or
            fixture_report.get("bytes") != root_fixture.stat().st_size or root_fixture.stat().st_size % 512 or
            not all(fixture_verification.get(name) is True for name in required_gpt_facts) or
            len(self_checks) != (7 if hfs_candidate else 5)):
        raise RuntimeError("The isolated root image is not the verified synthetic GPT fixture")
    root_matching_uuid = str(uuid.UUID(fixture_report["verification"]["partition_guid"])).upper()
    qemu_bus = "virtio-mmio-bus.0"
    root_device = "virtio-blk-device,drive=root,bus=" + qemu_bus
    qemu_root_drive = "if=none,id=root,format=raw,readonly=on,file=" + str(root_fixture)
    qtree_command = ["qemu-system-aarch64", "-machine", "virt,gic-version=3,acpi=off,virtualization=off",
                     "-accel", "tcg", "-cpu", "cortex-a76", "-smp", "1", "-m", "1024",
                     "-nodefaults", "-display", "none", "-global", "virtio-mmio.force-legacy=false",
                     "-drive", qemu_root_drive, "-device", root_device, "-monitor", "stdio"]
    qtree_result = subprocess.run(qtree_command, input="info qtree\nquit\n", stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, timeout=20)
    qtree_path = work / "qemu-qtree.log"
    qtree_path.write_text(qtree_result.stdout)
    if qtree_result.returncode:
        raise RuntimeError("QEMU qtree inspection failed: " + qtree_result.stdout[-1000:])
    qtree_lines = qtree_result.stdout.splitlines()
    device_indexes = [i for i, line in enumerate(qtree_lines)
                      if re.search(r"\bdev:\s+virtio-blk-device\b", line) and
                      any('drive = "root"' in item for item in qtree_lines[i:i + 12])]
    if len(device_indexes) != 1:
        raise RuntimeError("QEMU qtree did not identify exactly one root virtio-blk device")
    device_index = device_indexes[0]
    context = qtree_lines[max(0, device_index - 10):device_index + 12]
    bus_line = next((line for line in reversed(context[:10])
                     if re.search(r"\bbus:\s+virtio-mmio-bus\.\d+", line)), None)
    mmio_line = next((line for line in reversed(context[:10])
                      if re.search(r"\bmmio\s+[0-9a-fA-F]+/[0-9a-fA-F]+", line)), None)
    if not bus_line or not mmio_line:
        raise RuntimeError("QEMU qtree lacks the root transport bus or MMIO range")
    qtree_bus = re.search(r"virtio-mmio-bus\.\d+", bus_line).group(0)
    qtree_mmio = re.search(r"mmio\s+([0-9a-fA-F]+)/([0-9a-fA-F]+)", mmio_line)
    qtree_address, qtree_size = int(qtree_mmio.group(1), 16), int(qtree_mmio.group(2), 16)
    transport_start = max(i for i in range(device_index) if "dev: virtio-mmio," in qtree_lines[i])
    force_legacy_false = "force-legacy = false" in "\n".join(qtree_lines[transport_start:device_index])
    if qtree_bus != qemu_bus or not force_legacy_false:
        raise RuntimeError("QEMU did not attach the root block backend to the requested modern MMIO bus")
    dtb_path = work / "qemu-virtio-device-tree.dtb"
    dumpdtb_machine = "virt,gic-version=3,acpi=off,virtualization=off,dumpdtb=" + str(dtb_path)
    dtb_command = ["qemu-system-aarch64", "-machine", dumpdtb_machine, "-accel", "tcg",
                   "-cpu", "cortex-a76", "-smp", "1", "-m", "1024", "-nodefaults",
                   "-display", "none", "-global", "virtio-mmio.force-legacy=false",
                   "-drive", qemu_root_drive, "-device", root_device]
    dtb_result = subprocess.run(dtb_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=20)
    (work / "qemu-dumpdtb.log").write_text(dtb_result.stdout)
    if dtb_result.returncode or not dtb_path.is_file():
        raise RuntimeError("QEMU did not emit the virtual device tree: " + dtb_result.stdout[-1000:])
    fdt_root = read_fdt(dtb_path.read_bytes())
    handles = {entry.scalar("phandle"): entry for entry in fdt_root.walk() if "phandle" in entry.properties}
    gic_entries = [entry for entry in fdt_root.walk()
                   if "arm,gic-v3" in entry.strings("compatible") and entry.enabled()]
    virtio_entries = [entry for entry in fdt_root.walk()
                      if "virtio,mmio" in entry.strings("compatible") and "reg" in entry.properties and entry.enabled()]
    if len(gic_entries) != 1 or len(virtio_entries) < 1:
        raise RuntimeError("QEMU FDT lacks one enabled GICv3 or any virtio-mmio transport")
    gic_entry = gic_entries[0]
    matching_nodes = []
    for entry in virtio_entries:
        for address, size in registers(entry):
            if physical(entry.parent, address, size) == qtree_address and size == qtree_size:
                matching_nodes.append(entry)
    if len(matching_nodes) != 1:
        raise RuntimeError("QEMU qtree MMIO range does not map to exactly one FDT virtio-mmio node")
    virtio_entry = matching_nodes[0]
    if ("dma-coherent" not in virtio_entry.properties or "interrupts" not in virtio_entry.properties or
            "phandle" not in gic_entry.properties):
        raise RuntimeError("QEMU FDT storage node or GIC lacks required DMA/interrupt metadata")
    arm_io_base, arm_io_size = arm_io_window([gic_entry, virtio_entry])
    virtio_runtime = {
        "bus": qtree_bus, "mmio_physical": qtree_address, "mmio_size": qtree_size,
        "force_legacy": not force_legacy_false, "qtree_sha256": hashlib.sha256(qtree_path.read_bytes()).hexdigest(),
        "dtb_sha256": hashlib.sha256(dtb_path.read_bytes()).hexdigest(), "dtb_path": str(dtb_path),
        "fdt_node": virtio_entry.name, "fdt_virtio_mmio_node_count": len(virtio_entries),
        "interrupts": virtio_entry.cells("interrupts"), "gic_phandle": gic_entry.scalar("phandle"),
        "gic_interrupt_cells": gic_entry.scalar("#interrupt-cells"),
        "arm_io_base": arm_io_base, "arm_io_size": arm_io_size,
        "gic_adt_node": node("intc", hardware_properties(gic_entry, handles, arm_io_base) +
                             [("AAPL,phandle", struct.pack("<I", gic_entry.scalar("phandle")))]),
        "virtio_adt_node": node("virtio_mmio", hardware_properties(virtio_entry, handles, arm_io_base))}

# Resources are QEMU virt's GICv3, one Cortex-A76 and its actual architectural
# timer frequency. This fixture is not a tablet DT or tablet execution evidence.
chosen = [("random-seed", os.urandom(256)), *empty_nvram_proxy(),
          ("darwinboot-memory-size", struct.pack("<Q", 0x10000000))]
if root_matching_uuid:
    xml = ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
           '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' + root_matching_uuid +
           '</string></dict></dict>\0')
    chosen.append(("root-matching", xml.encode()))
if args.prelink:
    # Digest format and restrictive platform-code policy of the pinned OS.
    # These are boot inputs, not successful authentication results.
    chosen.extend([("crypto-hash-method", b"sha2-384\0"),
                   ("amfi-only-platform-code", struct.pack("<I", 1))])
cpus = node("cpus", children=[node("cpu0", [("reg", struct.pack("<I", 0)),
    ("device_type", b"cpu\0"), ("compatible", b"arm,cortex-a76\0"), ("state", b"running\0"),
    ("timebase-frequency", struct.pack("<I", 62500000)), ("cluster-type", struct.pack("<I", ord("P")))])])
if virtio_runtime:
    arm_io = node("arm-io", [("device_type", b"arm-io\0"), ("#address-cells", struct.pack("<I", 2)),
                              ("#size-cells", struct.pack("<I", 2)),
                              ("ranges", struct.pack("<QQQ", 0, virtio_runtime["arm_io_base"], virtio_runtime["arm_io_size"]))],
                 [virtio_runtime["gic_adt_node"], virtio_runtime["virtio_adt_node"]])
else:
    intc = node("intc", [("compatible", b"arm,gic-v3\0"), ("reg", struct.pack("<4Q", 0, 0x10000, 0xa0000, 0x20000)),
                         ("interrupt-controller", b""), ("#interrupt-cells", struct.pack("<I", 3))])
    arm_io = node("arm-io", [("device_type", b"arm-io\0"), ("ranges", struct.pack("<3Q", 0, 0x08000000, 0x01000000))], [intc])
adt = node("device-tree", [("model", b"QEMU T70PRO kernel execution fixture\0"), ("compatible", b"linux,dummy-virt\0")],
           [node("chosen", chosen, [node("memory-map"), node("manifest-properties")]), cpus, arm_io,
            node("defaults", [("vmm-present", struct.pack("<I", 1))]), node("options"), node("product")])
(esp / "xnu/kernel.macho.adt").write_bytes(adt)
nm = project / "tools/unixnu-linux/CommandLineTools/usr/bin/nm"
symbols = {}
text_symbols = []
for line in subprocess.check_output([str(nm), "-n", str(kernel)], text=True).splitlines():
    values = line.split()
    if len(values) == 3 and re.fullmatch("[0-9a-fA-F]+", values[0]):
        symbols[values[2]] = int(values[0], 16)
        text_symbols.append((int(values[0], 16), values[2]))
required_symbols = ("_panic", "_kmem_crypto_init", "_g_crypto_funcs", "_prng_ready", "_gDramBase", "_gDramSize")
if storage_candidate:
    required_symbols += ("_native_virtio_read_checkpoint", "_native_virtio_ready", "_native_virtio_read_requests",
        "_native_virtio_read_failures", "_native_virtio_sectors", "_native_virtio_mmio_physical",
        "_native_virtio_first_sector_sha256", "_native_virtio_bytes_read", "_native_virtio_write_requests",
        "_native_virtio_write_refusals", "_native_virtio_magic", "_native_virtio_version",
        "_native_virtio_device_id", "_native_virtio_vendor_id", "_IOFindBSDRoot")
if ufs_candidate:
    required_symbols += tuple("_t70_ufs_" + name for name in
        ("ready", "probe_attempts", "probe_refusals", "start_attempts", "mmio_accesses", "dma_quarantines"))
if args.checkpoint in ("mount", "root-vnode", "init"):
    required_symbols += ("_vfs_mountroot", "_mountlist", "_rootdevice", "_rootdev",
        "_t70_ufs_mount_vtable_offset", "_t70_ufs_vfs_name_offset", "_t70_ufs_mount_flags_offset",
        "_t70_ufs_mnt_rdonly_flag", "_t70_ufs_fs_name_bytes")
root_layout_names = ("mount_devvp_offset", "mnt_rootfs_flag", "proc_pid_offset", "proc_pid_bytes",
    "proc_cdir_offset", "proc_rdir_offset", "proc_fd_flags_offset", "proc_fd_flags_bytes", "fd_chroot_flag",
    "vnode_mount_offset", "vnode_type_offset", "vnode_type_bytes", "vnode_flags_offset", "vnode_flags_bytes",
    "vdir_type", "vroot_flag")
if args.checkpoint in ("root-vnode", "init"):
    required_symbols += ("_devfs_kernel_mount", "_rootvnode", "_rootvp", "_kernproc", "_initproc", "_load_init_program")
    required_symbols += tuple("_t70_ufs_" + name for name in root_layout_names)
assert all(name in symbols for name in required_symbols), sorted(set(required_symbols) - symbols.keys())
text_symbols.sort()
with socket.socket() as reserved:
    reserved.bind(("127.0.0.1", 0))
    port = reserved.getsockname()[1]
observation = work / "gdb-observation.json"
observation.unlink(missing_ok=True)
checkpoint_symbol = {"crypto": "kmem_crypto_init", "bsd": "bsd_init", "root-entry": "IOFindBSDRoot",
                     "block": "native_virtio_read_checkpoint", "root": "IOFindBSDRoot", "mount": "vfs_mountroot",
                     "root-vnode": "vfs_mountroot", "init": "vfs_mountroot"}[args.checkpoint]
checkpoint = {"mount": "vfs_mountroot_return", "root-vnode": "devfs_kernel_mount", "init": "load_init_program",
              "root": "IOFindBSDRoot_return"}.get(args.checkpoint, checkpoint_symbol)
root_return_probe = storage_candidate and args.checkpoint == "root"
mount_return_probe = args.checkpoint in ("mount", "root-vnode", "init")
root_vnode_probe = args.checkpoint in ("root-vnode", "init")
gdb_script = work / "observe.gdb"
gdb_script.write_text("\n".join([
    "set pagination off", "set confirm off", "set architecture aarch64",
    "target remote 127.0.0.1:" + str(port),
    "hbreak *" + hex(symbols["_panic"]), "hbreak *" + hex(symbols["_" + checkpoint_symbol]),
    "continue", "python", "import gdb,json,struct", "out = {}",
    "out['entry_pc'] = int(gdb.parse_and_eval('$pc'))",
    "out['root_entry_registers'] = {n:int(gdb.parse_and_eval('$'+n)) & ((1<<64)-1) for n in ['x0','x1','x2','x3','x30']} if " + repr(root_return_probe) + " and out['entry_pc'] == " + str(symbols.get("_IOFindBSDRoot", 0)) + " else {}",
    "if " + repr(root_return_probe) + " and out['root_entry_registers']:",
    "    out['root_entry_pc'] = out['entry_pc']",
    "    out['root_return_address'] = out['root_entry_registers']['x30']",
    "    gdb.execute('hbreak *' + hex(out['root_return_address']))",
    "    gdb.execute('continue')",
    "if " + repr(mount_return_probe) + " and out['entry_pc'] == " + str(symbols.get("_vfs_mountroot", 0)) + ":",
    "    out['mount_entry_pc'] = out['entry_pc']",
    "    out['mount_return_address'] = int(gdb.parse_and_eval('$x30')) & ((1<<64)-1)",
    "    gdb.execute('hbreak *' + hex(out['mount_return_address']))",
    "    gdb.execute('continue')",
    "    out['mount_return_pc'] = int(gdb.parse_and_eval('$pc'))",
    "    out['mount_return_x0'] = int(gdb.parse_and_eval('$x0')) & ((1<<64)-1)",
    "out['root_setup_entry_observed'] = False",
    "out['init_entry_observed'] = False",
    "if " + repr(root_vnode_probe) + " and out.get('mount_return_pc') == out.get('mount_return_address') and out.get('mount_return_x0') == 0:",
    "    gdb.execute('disable 2 3')",
    "    _vnode_break = gdb.Breakpoint('*' + hex(" + str(symbols.get("_devfs_kernel_mount", 0)) + "), type=gdb.BP_HARDWARE_BREAKPOINT)",
    "    gdb.execute('continue')",
    "    if int(gdb.parse_and_eval('$pc')) == " + str(symbols.get("_devfs_kernel_mount", 0)) + ":",
    "        out['root_setup_entry_observed'] = True",
    "        _mountpoint = int(gdb.parse_and_eval('$x0')) & ((1<<64)-1)",
    "        out['root_setup_mountpoint'] = bytes(gdb.selected_inferior().read_memory(_mountpoint,32)).split(bytes(1),1)[0].decode('ascii','replace')",
    "        if " + repr(args.checkpoint == "init") + ":",
    "            _vnode_break.enabled = False",
    "            gdb.Breakpoint('*' + hex(" + str(symbols.get("_load_init_program", 0)) + "), type=gdb.BP_HARDWARE_BREAKPOINT)",
    "            gdb.execute('continue')",
    "            out['init_entry_observed'] = int(gdb.parse_and_eval('$pc')) == " + str(symbols.get("_load_init_program", 0)),
    "            if out['init_entry_observed']: out['init_proc_argument'] = int(gdb.parse_and_eval('$x0')) & ((1<<64)-1)",
    "out['pc'] = int(gdb.parse_and_eval('$pc'))",
    "out['stop'] = 'panic' if out['pc'] == " + str(symbols["_panic"]) + " else ('load_init_program' if out.get('init_entry_observed') else ('devfs_kernel_mount' if out.get('root_setup_entry_observed') and out['pc'] == " + str(symbols.get("_devfs_kernel_mount", 0)) + " else ('vfs_mountroot_return' if " + repr(mount_return_probe) + " and out.get('mount_return_address') == out['pc'] else ('IOFindBSDRoot_return' if " + repr(root_return_probe) + " and out.get('root_return_address') == out['pc'] else (" + repr(checkpoint_symbol) + " if out['pc'] == " + str(symbols["_" + checkpoint_symbol]) + " else 'unexpected_stop')))))",
    "out['registers'] = {n:int(gdb.parse_and_eval('$'+n)) & ((1<<64)-1) for n in ['x0','x1','x2','x3','x4','x5','x6','x7','x29','x30','sp','cpsr']}",
    "out['root_return_observed'] = False",
    "if out.get('root_entry_registers') and out['stop'] == 'IOFindBSDRoot_return':",
    "    try:",
    "        _args = out['root_entry_registers']",
    "        _name = bytes(gdb.selected_inferior().read_memory(_args['x0'], min(_args['x1'], 256)))",
    "        out['root_name'] = _name.split(bytes(1), 1)[0].decode('ascii', 'replace')",
    "        out['root_dev_t'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(_args['x2'], 4)), 'little')",
    "        out['root_flags'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(_args['x3'], 4)), 'little')",
    "        _ret = out['registers']['x0'] & 0xffffffff",
    "        out['root_kern_return'] = _ret if _ret < 0x80000000 else _ret - 0x100000000",
    "        out['root_return_observed'] = True",
    "    except Exception as error: out['root_return_read_error'] = str(error)",
    "out['mount_return_observed'] = False",
    "if " + repr(mount_return_probe) + " and out.get('mount_return_pc') == out.get('mount_return_address'):",
    "    out['mount_return_observed'] = True",
    "    _ret = out['mount_return_x0'] & 0xffffffff",
    "    out['mount_return_code'] = _ret if _ret < 0x80000000 else _ret - 0x100000000",
    "    if _ret == 0:",
    "        try:",
    "            _read = lambda a,n: bytes(gdb.selected_inferior().read_memory(a,n))",
    "            _u64 = lambda a: int.from_bytes(_read(a,8),'little')",
    "            _layout = {k:_u64(a) for k,a in " + repr({name: symbols.get("_t70_ufs_" + name, 0) for name in
                    ("mount_vtable_offset", "vfs_name_offset", "mount_flags_offset", "mnt_rdonly_flag", "fs_name_bytes")}) + ".items()}",
    "            out['mount_layout'] = _layout",
    "            _mount = _u64(" + str(symbols.get("_mountlist", 0)) + ")",
    "            out['mount_pointer'] = _mount",
    "            _vtable = _u64(_mount + _layout['mount_vtable_offset'])",
    "            out['mounted_filesystem'] = _read(_vtable + _layout['vfs_name_offset'], min(_layout['fs_name_bytes'],32)).split(bytes(1),1)[0].decode('ascii','replace')",
    "            out['mount_flags'] = int.from_bytes(_read(_mount + _layout['mount_flags_offset'],4),'little')",
    "            out['mount_read_only'] = bool(out['mount_flags'] & _layout['mnt_rdonly_flag'])",
    "            out['mount_root_device'] = _read(" + str(symbols.get("_rootdevice", 0)) + ",32).split(bytes(1),1)[0].decode('ascii','replace')",
    "            out['mount_root_dev_t'] = int.from_bytes(_read(" + str(symbols.get("_rootdev", 0)) + ",4),'little')",
    "        except Exception as error: out['mount_read_error'] = str(error)",
    "if " + repr(root_vnode_probe) + " and out.get('root_setup_entry_observed'):",
    "    try:",
    "        _layout = {k:_u64(a) for k,a in " + repr({name: symbols.get("_t70_ufs_" + name, 0) for name in root_layout_names}) + ".items()}",
    "        out['root_vnode_layout'] = _layout",
    "        out['root_vnode'] = _u64(" + str(symbols.get("_rootvnode", 0)) + ")",
    "        out['root_device_vnode'] = _u64(" + str(symbols.get("_rootvp", 0)) + ")",
    "        out['mount_device_vnode'] = _u64(out['mount_pointer'] + _layout['mount_devvp_offset'])",
    "        _vnode = out['root_vnode']",
    "        out['root_vnode_mount'] = _u64(_vnode + _layout['vnode_mount_offset'])",
    "        out['root_vnode_type'] = int.from_bytes(_read(_vnode + _layout['vnode_type_offset'],_layout['vnode_type_bytes']),'little')",
    "        out['root_vnode_flags'] = int.from_bytes(_read(_vnode + _layout['vnode_flags_offset'],_layout['vnode_flags_bytes']),'little')",
    "        for _name,_address in " + repr({name: symbols.get("_" + name, 0) for name in ("kernproc", "initproc")}) + ".items():",
    "            _proc = _u64(_address)",
    "            out[_name] = _proc",
    "            if _proc:",
    "                out[_name+'_pid'] = int.from_bytes(_read(_proc+_layout['proc_pid_offset'],_layout['proc_pid_bytes']),'little')",
    "                out[_name+'_cdir'] = _u64(_proc+_layout['proc_cdir_offset'])",
    "                out[_name+'_rdir'] = _u64(_proc+_layout['proc_rdir_offset'])",
    "                out[_name+'_fd_flags'] = int.from_bytes(_read(_proc+_layout['proc_fd_flags_offset'],_layout['proc_fd_flags_bytes']),'little')",
    "    except Exception as error: out['root_vnode_read_error'] = str(error)",
    "if out['stop'] == 'panic':",
    "    try: out['panic_format'] = bytes(gdb.selected_inferior().read_memory(out['registers']['x0'],256)).split(b'\\0')[0].decode('utf-8','replace')",
    "    except Exception as error: out['panic_read_error'] = str(error)",
    "out['crypto_pointer'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_g_crypto_funcs"]) + ",8)),'little')",
    "out['source_crypto_status'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_source_corecrypto14_status"]) + ",4)),'little')",
    "out['source_prelinked_provider_ready'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_source_corecrypto14_prelinked_ready"]) + ",4)),'little')",
    "out['image4_interface'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_img4if"]) + ",8)),'little')",
    "out['amfi_interface'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_amfi"]) + ",8)),'little')",
    *(["out['native_virtio_" + name + "'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols[sym]) + "," + str(size) + ")),'little')"
       for name, sym, size in (("ready", "_native_virtio_ready", 4), ("read_requests", "_native_virtio_read_requests", 4),
         ("read_failures", "_native_virtio_read_failures", 4), ("sectors", "_native_virtio_sectors", 8),
         ("mmio_physical", "_native_virtio_mmio_physical", 8), ("bytes_read", "_native_virtio_bytes_read", 8),
         ("write_requests", "_native_virtio_write_requests", 4), ("write_refusals", "_native_virtio_write_refusals", 4),
         ("magic", "_native_virtio_magic", 4), ("version", "_native_virtio_version", 4),
         ("device_id", "_native_virtio_device_id", 4), ("vendor_id", "_native_virtio_vendor_id", 4))]
      + ["out['native_virtio_first_sector_sha256'] = bytes(gdb.selected_inferior().read_memory(" + str(symbols["_native_virtio_first_sector_sha256"]) + ",32)).hex()"] if storage_candidate else []),
    *(["out['t70_ufs_" + name + "'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_t70_ufs_" + name]) + ",4)),'little')"
       for name in ("ready", "probe_attempts", "probe_refusals", "start_attempts", "mmio_accesses", "dma_quarantines")] if ufs_candidate else []),
    "out['prng_ready'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_prng_ready"]) + ",1)),'little')",
    "out['owned_dram_base'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_gDramBase"]) + ",8)),'little')",
    "out['owned_dram_size'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_gDramSize"]) + ",8)),'little')",
    "out['fixed_zone_budget'] = list(struct.unpack('<H',bytes(gdb.selected_inferior().read_memory(" + str(symbols["_kt_fixed_zones"]) + ",2))))",
    "out['signature_counts'] = list(struct.unpack('<29H',bytes(gdb.selected_inferior().read_memory(" + str(symbols["_kt_freq_list"]) + ",58))))",
    "out['type_counts'] = list(struct.unpack('<29H',bytes(gdb.selected_inferior().read_memory(" + str(symbols["_kt_freq_list_total"]) + ",58))))",
    "try:",
    "    log_pointer = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_msgbufp"]) + ",8)),'little')",
    "    magic,size,position,reader,buffer = struct.unpack('<4IQ',bytes(gdb.selected_inferior().read_memory(log_pointer,24)))",
    "    if magic == 0x063061 and 0 < size <= 1048576 and position < size:",
    "        raw = bytes(gdb.selected_inferior().read_memory(buffer,size))",
    "        out['kernel_log_tail'] = (raw[position:]+raw[:position]).replace(bytes(1),b'')[-16000:].decode('utf-8','replace')",
    "except Exception as error: out['kernel_log_read_error'] = str(error)",
    "out['frames'] = []", "fp = out['registers']['x29']",
    "for index in range(12):",
    "    try:",
    "        previous,ret = struct.unpack('<2Q',bytes(gdb.selected_inferior().read_memory(fp,16)))",
    "        out['frames'].append(ret)",
    "        if previous <= fp or previous-fp > 0x10000: break",
    "        fp=previous",
    "    except Exception: break",
    "out['panic_stack_words'] = list(struct.unpack('<8Q',bytes(gdb.selected_inferior().read_memory(out['registers']['sp'],64))))",
    "out['panic_stack_strings'] = []",
    "if out['stop'] == 'panic':",
    "    for pointer in struct.unpack('<8Q',bytes(gdb.selected_inferior().read_memory(out['registers']['sp'],64))):",
    "        try:",
    "            value = bytes(gdb.selected_inferior().read_memory(pointer,256)).split(b'\\0')[0].decode('ascii')",
    "            if value and value.isprintable(): out['panic_stack_strings'].append(value)",
    "        except Exception: pass",
    "out['armcpu_registered'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_armcpu_registered"]) + ",4)),'little')",
    "out['armcpu_gic_ready'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_armcpu_gic_ready"]) + ",4)),'little')",
    "out['armcpu_timer_interrupts'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_armcpu_timer_interrupts"]) + ",8)),'little')",
    "out['armdt_platform_configured'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_armdt_platform_configured"]) + ",4)),'little')",
    "out['armdt_platform_cpu_count'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_armdt_platform_cpu_count"]) + ",4)),'little')",
    "out['armdt_platform_nvram_proxy'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_armdt_platform_nvram_proxy"]) + ",4)),'little')",
    "out['armdt_platform_device_count'] = int.from_bytes(bytes(gdb.selected_inferior().read_memory(" + str(symbols["_armdt_platform_device_count"]) + ",4)),'little')",
    "out['sysregs'] = {}",
    "for n in ['sctlr_el1','tcr_el1','ttbr0_el1','ttbr1_el1']:",
    "    for candidate in [n,n.upper()]:",
    "        try: out['sysregs'][n] = int(gdb.parse_and_eval('$'+candidate)); break",
    "        except Exception: pass",
    "open(" + repr(str(observation)) + ",'w').write(json.dumps(out,indent=2)+'\\n')", "end", "disconnect", "quit", ""]))
gdb_python = gdb_script.read_text().split("\npython\n", 1)[1].rsplit("\nend\n", 1)[0]
compile(gdb_python, str(gdb_script) + ":python", "exec")
firmware = Path("/usr/share/qemu-efi-aarch64/QEMU_EFI.fd")
command = ["qemu-system-aarch64", "-machine", "virt,gic-version=3,acpi=off,virtualization=off", "-accel", "tcg"]
if virtio_runtime:
    command.extend(["-global", "virtio-mmio.force-legacy=false"])
command.extend(["-cpu", "cortex-a76", "-smp", "1", "-m", "1024", "-bios", str(firmware), "-display", "none",
                "-serial", "file:" + str(work / "serial.log"), "-monitor", "none", "-nic", "none", "-device", "ramfb",
                "-drive", "if=none,id=esp,format=raw,readonly=on,file=fat:ro:" + str(esp),
                "-device", "virtio-blk-pci,drive=esp"])
if virtio_runtime:
    command.extend(["-drive", "if=none,id=root,format=raw,readonly=on,file=" + str(root_fixture),
                    "-device", "virtio-blk-device,drive=root,bus=" + virtio_runtime["bus"]])
command.extend(["-gdb", "tcp:127.0.0.1:" + str(port), "-S"])
report = {"status": "running", "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "scope": "selected T70PRO iOS kernel candidate and DarwinBoot in QEMU virt; virtual CPU/GIC fixture",
           "os_abi": args.os, "kernel_sha256": original_digest, "negative_post": args.negative_post,
           "native_kernel_sha256": native_digest, "native_build_record": str(build_record_path), "prelink_kind": args.prelink, "prelink_record": prelink_record,
           "executed_kernel_sha256": hashlib.sha256((esp / "xnu/kernel.macho").read_bytes()).hexdigest(), "adt_sha256": hashlib.sha256(adt).hexdigest(),
           "device_boot": "not_run", "vm_present_declared": True, "checkpoint": checkpoint, "qemu_command": command, "firmware_sha256": hashlib.sha256(firmware.read_bytes()).hexdigest(),
           "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
           "efi_sha256": {name: hashlib.sha256((esp/path).read_bytes()).hexdigest() for name,path in
                          (("vm_launcher", "EFI/BOOT/BOOTAA64.EFI"), ("darwinboot", "EFI/Teclast/DarwinBoot.efi"))},
           "boot_args": "-v keepsyms=1 debug=0x14e"}
if virtio_runtime:
    report["qemu_virtio_transport"] = {key: value for key, value in virtio_runtime.items()
                                       if key not in ("gic_adt_node", "virtio_adt_node")}
    report["synthetic_root"] = {"path": str(root_fixture), "sha256_before": root_fixture_sha256,
        "bytes": root_fixture.stat().st_size, "backend_read_only": True, "read_only_qemu_drive": True,
        "partition_uuid_from_verified_gpt": root_matching_uuid, "gpt_verification_report": str(fixture_report_path),
        "filesystem_present": hfs_candidate, "mount_observed": False}
try:
    with (work / "qemu.log").open("w") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            try:
                result = subprocess.run(["gdb-multiarch", "-q", "-batch", "-x", str(gdb_script)],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120 if root_return_probe else 50)
            except subprocess.TimeoutExpired as error:
                partial = error.stdout or ""
                if isinstance(partial, bytes):
                    partial = partial.decode("utf-8", "replace")
                (work / "gdb.log").write_text(partial)
                report["gdb_timed_out"] = True
                raise RuntimeError("Kernel checkpoint timed out; partial GDB output preserved") from error
            (work / "gdb.log").write_text(result.stdout)
            report["gdb_exit_code"] = result.returncode
            if not observation.exists():
                raise RuntimeError("No kernel checkpoint observation; inspect serial.log and gdb.log")
            report["observation"] = json.loads(observation.read_text())
            observed = report["observation"]
            tcr = observed["sysregs"].get("tcr_el1")
            sctlr = observed["sysregs"].get("sctlr_el1", 0)
            report["native_4k_mmu_observed"] = (tcr is not None and bool(sctlr & 1) and
                ((tcr >> 14) & 3) == 0 and ((tcr >> 30) & 3) == 2 and
                (tcr & 63) == 26 and ((tcr >> 16) & 63) == 26)
            report["symbolized_frames"] = []
            for address in observed["frames"]:
                index = bisect.bisect_right(text_symbols, (address, "\uffff"))-1
                if index >= 0:
                    base, name = text_symbols[index]
                    report["symbolized_frames"].append(name + "+" + hex(address-base))
            if result.returncode:
                raise RuntimeError("GDB observation did not finish successfully")
            if args.negative_post:
                if (observed["stop"] != "panic" or observed["source_crypto_status"] == 0 or
                        observed["crypto_pointer"] or observed["prng_ready"] or
                        "FIPS Kernel POST Failed" not in observed.get("panic_format", "")):
                    raise RuntimeError("The deliberately damaged VM copy was not refused by the complete POST")
            elif args.prelink:
                plan = json.loads(Path(prelink_record["report"]).read_text())
                original_provider = any(e["module"] == "com.apple.kec.corecrypto" and
                    e["va"] <= observed["crypto_pointer"] < e["va"] + e["size"] for e in plan["extents"])
                report["original_crypto_provider_observed"] = original_provider
                report["security_interfaces_observed"] = {
                    "image4": bool(observed["image4_interface"]), "amfi": bool(observed["amfi_interface"])}
                # kmem_crypto_init precedes MAC policy / Image4 loading. Do not
                # require a later interface at the earlier crypto checkpoint.
                security_ready = (args.checkpoint == "crypto" or
                    (observed["image4_interface"] and
                     (args.prelink == "image4" or observed["amfi_interface"])))
                if (observed["stop"] != checkpoint or not original_provider or not observed["prng_ready"] or
                        observed["source_prelinked_provider_ready"] != 1 or observed["source_crypto_status"] != 5 or
                        not security_ready):
                    raise RuntimeError("Prelinked kernel stopped before " + checkpoint + ": " + observed.get("panic_format", "provider/security state mismatch"))
            elif (observed["stop"] != checkpoint or observed["source_crypto_status"] != 0 or
                  observed["source_prelinked_provider_ready"] or not observed["crypto_pointer"] or not observed["prng_ready"]):
                raise RuntimeError("Kernel stopped before " + checkpoint + ": " + observed.get("panic_format", "unknown stop"))
            if virtio_runtime:
                expected_sector_sha256 = hashlib.sha256(root_fixture.read_bytes()[:512]).hexdigest()
                virtio_observed = {"ready": observed["native_virtio_ready"],
                    "read_requests": observed["native_virtio_read_requests"],
                    "read_failures": observed["native_virtio_read_failures"],
                    "sectors": observed["native_virtio_sectors"], "mmio_physical": observed["native_virtio_mmio_physical"],
                    "bytes_read": observed["native_virtio_bytes_read"],
                    "write_requests": observed["native_virtio_write_requests"],
                    "write_refusals": observed["native_virtio_write_refusals"],
                    "magic": observed["native_virtio_magic"], "version": observed["native_virtio_version"],
                    "device_id": observed["native_virtio_device_id"], "vendor_id": observed["native_virtio_vendor_id"],
                    "first_sector_sha256": observed["native_virtio_first_sector_sha256"]}
                report["virtio_block_io"] = {**virtio_observed,
                    "expected_first_sector_sha256": expected_sector_sha256,
                    "expected_mmio_physical": virtio_runtime["mmio_physical"],
                    "expected_sectors": root_fixture.stat().st_size // 512,
                    "first_sector_data_verified": virtio_observed["first_sector_sha256"] == expected_sector_sha256,
                    "transport_registers_verified": (virtio_observed["magic"] == 0x74726976 and
                        virtio_observed["version"] == 2 and virtio_observed["device_id"] == 2),
                    "active_fdt_node_verified": virtio_observed["mmio_physical"] == virtio_runtime["mmio_physical"],
                    "no_write_request_observed": virtio_observed["write_requests"] == 0}
                io = report["virtio_block_io"]
                if (io["ready"] != 1 or io["read_requests"] < 1 or io["read_failures"] != 0 or
                        io["sectors"] != io["expected_sectors"] or io["bytes_read"] < 512 or
                        not io["first_sector_data_verified"] or not io["transport_registers_verified"] or
                        not io["active_fdt_node_verified"] or io["write_requests"] != 0):
                    raise RuntimeError("Virtio did not complete a verified read from the selected read-only QEMU block device")
                if args.checkpoint == "block":
                    report["first_block_read_checkpoint_observed"] = observed["stop"] == checkpoint
                if args.checkpoint == "root":
                    root_name = observed.get("root_name", "")
                    root_dev_t = observed.get("root_dev_t", 0xffffffff)
                    root_major = (root_dev_t >> 24) & 0xff
                    root_minor = root_dev_t & 0xffffff
                    root_valid = (observed.get("stop") == checkpoint and observed.get("root_return_observed") is True and
                        observed.get("root_kern_return") == 0 and root_dev_t != 0xffffffff and
                        re.fullmatch(r"disk[0-9]+s[0-9]+", root_name) is not None)
                    kernel_log = observed.get("kernel_log_tail", "")
                    matching = parse_root_matching_log(kernel_log)
                    property_match = matching.get("IOPropertyMatch") if isinstance(matching, dict) else None
                    waiting_uuid = property_match.get("UUID") if isinstance(property_match, dict) else None
                    root_match_verified = (isinstance(matching, dict) and
                        matching.get("IOProviderClass") == "IOMedia" and isinstance(property_match, dict) and
                        isinstance(waiting_uuid, str) and waiting_uuid.casefold() == root_matching_uuid.casefold())
                    bsd_lines = list(re.finditer(r"BSD root:\s*(\S+),\s*major\s+(\d+),\s*minor\s+(\d+)", kernel_log))
                    bsd_log = bsd_lines[-1] if bsd_lines else None
                    bsd_log_matches_return = bool(bsd_log and bsd_log.group(1) == root_name and
                        int(bsd_log.group(2)) == root_major and int(bsd_log.group(3)) == root_minor)
                    report["root_match_log_evidence"] = {
                        "waiting_on_dictionary_parsed": isinstance(matching, dict),
                        "provider_class": matching.get("IOProviderClass") if isinstance(matching, dict) else None,
                        "io_property_match": property_match if isinstance(property_match, dict) else None,
                        "requested_uuid": root_matching_uuid, "dictionary_uuid": waiting_uuid,
                        "uuid_match": root_match_verified,
                        "bsd_root_log_name": bsd_log.group(1) if bsd_log else None,
                        "bsd_root_log_major": int(bsd_log.group(2)) if bsd_log else None,
                        "bsd_root_log_minor": int(bsd_log.group(3)) if bsd_log else None,
                        "bsd_log_matches_return": bsd_log_matches_return}
                    report["root_selection"] = {"return_observed": observed.get("root_return_observed", False),
                        "kern_return": observed.get("root_kern_return"), "bsd_name": root_name,
                        "dev_t": root_dev_t, "major": root_major, "minor": root_minor,
                        "flags": observed.get("root_flags"), "device_number_valid": root_valid,
                        "root_match_uuid_verified": root_match_verified,
                        "bsd_root_log_matches_return": bsd_log_matches_return,
                        "root_matching_query_uuid": root_matching_uuid, "filesystem_mount_observed": False}
                    if not root_valid or not root_match_verified or not bsd_log_matches_return:
                        raise RuntimeError("IOFindBSDRoot return did not match the parsed UUID query and BSD root log")
                if mount_return_probe:
                    root_proof = mount_root_evidence(observed.get("kernel_log_tail", ""), root_matching_uuid,
                        observed.get("mount_root_device", ""), observed.get("mount_root_dev_t"))
                    uuid_matches = root_proof["query_uuid_matches"]
                    mounted = (observed.get("mount_return_observed") is True and
                        observed.get("mount_return_code") == 0 and observed.get("mount_pointer", 0) != 0 and
                        observed.get("mounted_filesystem") == "hfs" and observed.get("mount_read_only") is True and
                        re.fullmatch(r"disk[0-9]+s[0-9]+", observed.get("mount_root_device", "")) is not None and
                        uuid_matches and root_proof["provider_is_iomedia"] and root_proof["bsd_log_matches_mount"] and io["read_requests"] > 1)
                    report["filesystem_mount"] = {
                        "return_observed": observed.get("mount_return_observed", False),
                        "return_code": observed.get("mount_return_code"),
                        "mount_pointer": observed.get("mount_pointer"),
                        "filesystem": observed.get("mounted_filesystem"),
                        "read_only": observed.get("mount_read_only"),
                        "root_device": observed.get("mount_root_device"),
                        "root_dev_t": observed.get("mount_root_dev_t"),
                        "requested_uuid": root_matching_uuid, "query_uuid_matches": uuid_matches,
                        "root_match_evidence": root_proof,
                        "compiler_measured_layout": observed.get("mount_layout"),
                        "mount_observed": mounted, "volume_authentication": "not_run",
                        "root_vnode_setup": "not_run", "userspace": "not_run"}
                    report["synthetic_root"]["mount_observed"] = mounted
                    if not mounted:
                        raise RuntimeError("vfs_mountroot did not return a verified read-only HFS mount")
                if root_vnode_probe:
                    vnode_proof = root_vnode_evidence(observed)
                    report["root_vnode_setup"] = {
                        "proof": vnode_proof, "compiler_measured_layout": observed.get("root_vnode_layout"),
                        "root_vnode": observed.get("root_vnode"), "root_mount": observed.get("root_vnode_mount"),
                        "kernel_cdir": observed.get("kernproc_cdir"), "verified": vnode_proof["verified"]}
                    report["filesystem_mount"]["root_vnode_setup"] = "observed" if vnode_proof["verified"] else "failed"
                    if not vnode_proof["verified"]:
                        raise RuntimeError("The actual HFS root vnode/current directory did not match the mounted root")
                if args.checkpoint == "init":
                    init_proof = init_process_evidence(observed)
                    report["init_process"] = {
                        "proof": init_proof, "entry": "load_init_program", "pid": observed.get("initproc_pid"),
                        "proc": observed.get("initproc"), "argument": observed.get("init_proc_argument"),
                        "current_directory": observed.get("initproc_cdir"),
                        "entry_observed": init_proof["verified"], "userspace_execution": "not_run",
                        "root_authentication": "not_requested_by_inherited_ios_build_policy",
                        "next_step": "load_init_program searches for launchd; the blank fixture contains no executable"}
                    if not init_proof["verified"]:
                        raise RuntimeError("PID 1 did not inherit the verified HFS root directory at load_init_program")
                after_hash = hashlib.sha256(root_fixture.read_bytes()).hexdigest()
                report["synthetic_root"]["sha256_after"] = after_hash
                report["synthetic_root"]["unchanged"] = after_hash == root_fixture_sha256
                if after_hash != root_fixture_sha256:
                    raise RuntimeError("QEMU changed the read-only synthetic GPT fixture")
            if ufs_candidate:
                refusal = {name: observed["t70_ufs_" + name] for name in
                    ("ready", "probe_attempts", "probe_refusals", "start_attempts", "mmio_accesses", "dma_quarantines")}
                refused = (refusal["ready"] == refusal["start_attempts"] == refusal["mmio_accesses"] ==
                    refusal["dma_quarantines"] == 0 and refusal["probe_refusals"] == refusal["probe_attempts"] and
                    (args.checkpoint == "block" or refusal["probe_attempts"] > 0))
                report["t70_ufs_provider_refusal"] = {**refusal, "verified": refused, "physical_io": "not_run",
                    "scope": "actual IOKit matching refuses the VM before handoff/controller MMIO"}
                if not refused:
                    raise RuntimeError("T70 UFS provider was not refused before physical MMIO in QEMU")
            if not report["native_4k_mmu_observed"]:
                raise RuntimeError("Native 4 KiB page-table configuration was not observed")
            report["bsd_entry_observed"] = observed["stop"] in ("bsd_init", "IOFindBSDRoot", "IOFindBSDRoot_return", "vfs_mountroot_return", "devfs_kernel_mount", "load_init_program")
            report["root_selection_entry_observed"] = (observed["stop"] == "IOFindBSDRoot" or bool(observed.get("root_entry_pc")))
            report["root_selection_return_observed"] = observed["stop"] == "IOFindBSDRoot_return"
            report["platform_driver_initialization_complete"] = all(observed[name] == 1 for name in
                ("armdt_platform_configured", "armdt_platform_cpu_count", "armdt_platform_nvram_proxy",
                 "armcpu_registered", "armcpu_gic_ready"))
            report["timer_interrupt_observed"] = observed["armcpu_timer_interrupts"] > 0
            # bsd_init entry can precede asynchronous CPU driver matching.
            # Keep that milestone distinct from the later driver requirement.
            if args.checkpoint != "crypto" and any(observed[name] != 1 for name in
                    ("armdt_platform_configured", "armdt_platform_cpu_count", "armdt_platform_nvram_proxy")):
                raise RuntimeError("BSD entry lacks real virtual platform publication")
            if args.checkpoint in ("root", "root-entry", "mount", "root-vnode", "init") and not report["platform_driver_initialization_complete"]:
                raise RuntimeError("Root selection entry lacks real virtual CPU/GIC initialization")
            assert hashlib.sha256(kernel.read_bytes()).hexdigest() == original_digest
            report["staged_kernel_unchanged"] = True
            report["status"] = "passed"
            if args.prelink:
                print("PASS: prelinked T70PRO kernel reached", checkpoint,
                      "with original crypto/PRNG; source POST not run; security interfaces:",
                      report["security_interfaces_observed"])
            else:
                print("PASS:", "complete POST refused the damaged VM copy" if args.negative_post else "actual T70PRO kernel reached " + checkpoint + " with complete POST and PRNG initialized")
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
except Exception as error:
    report.update(status="failed", error=str(error))
    raise
finally:
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    if root_fixture and "synthetic_root" in report:
        final_fixture_hash = hashlib.sha256(root_fixture.read_bytes()).hexdigest()
        report["synthetic_root"]["sha256_after"] = final_fixture_hash
        report["synthetic_root"]["unchanged"] = final_fixture_hash == root_fixture_sha256
        if final_fixture_hash != root_fixture_sha256 and report.get("status") == "passed":
            report.update(status="failed", error="QEMU changed the read-only synthetic GPT fixture")
    report["staged_kernel_unchanged"] = hashlib.sha256(kernel.read_bytes()).hexdigest() == original_digest
    report["native_kernel_unchanged"] = hashlib.sha256(native_kernel.read_bytes()).hexdigest() == native_digest
    report_path = project / ("analysis/xnu-qemu" + os_suffix + case_suffix + "-execution.json")
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path)
