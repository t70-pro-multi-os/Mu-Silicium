#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a QEMU-only VirtIO MMIO block archive against measured Darwin23 IOStorage bindings."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

PIN = "4e1013eb8c45077b57f8d38d61b389fe899ea1c3"
PIN_SHA = "bfd6eac739590583f3154c248a153f02e84b0ce888185a0bfe73a697e4a842c5"
IOSTORAGE_REV = "e45fbe30a7879e0b16e26dddb78538f1b00e0bff"
CACHE_SHA = "f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7"
SYMBOLS = [
    "_native_virtio_read_checkpoint", "_native_virtio_ready",
    "_native_virtio_read_requests", "_native_virtio_read_failures",
    "_native_virtio_sectors", "_native_virtio_mmio_physical",
    "_native_virtio_first_sector_sha256", "_native_virtio_bytes_read",
    "_native_virtio_write_requests", "_native_virtio_write_refusals",
    "_native_virtio_magic", "_native_virtio_version",
    "_native_virtio_device_id", "_native_virtio_vendor_id",
]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def replace_once(source, old, new, label):
    if source.count(old) != 1:
        raise SystemExit("Pinned VirtIO source changed at " + label)
    return source.replace(old, new, 1)


def instrument(source):
    if source.count('if (!compatible || compatible->getLength() != 12 ||') != 1:
        raise SystemExit("VirtIO provider match changed")
    if source.count('memcmp(compatible->getBytesNoCopy(), "virtio,mmio", 12)') != 1:
        raise SystemExit("VirtIO compatible changed")
    if source.count("*request = {0, 0, sector};") != 1 or "*request = {1, 0, sector};" in source:
        raise SystemExit("VirtIO request is no longer read-only")
    write_guard = "  if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;"
    if source.count(write_guard) != 1:
        raise SystemExit("VirtIO write refusal guard changed")
    source = replace_once(
        source,
        "volatile uint64_t native_virtio_mmio_physical = 0;\n",
        "volatile uint64_t native_virtio_mmio_physical = 0;\n"
        "volatile uint64_t native_virtio_bytes_read = 0;\n"
        "volatile uint32_t native_virtio_write_requests = 0;\n"
        "volatile uint32_t native_virtio_write_refusals = 0;\n"
        "volatile uint32_t native_virtio_magic = 0;\n"
        "volatile uint32_t native_virtio_version = 0;\n"
        "volatile uint32_t native_virtio_device_id = 0;\n"
        "volatile uint32_t native_virtio_vendor_id = 0;\n",
        "observer globals",
    )
    source = replace_once(
        source,
        "  if (read32(0x00) != 0x74726976 || read32(0x04) != 2 || read32(0x08) != 2) return false;",
        "  const uint32_t magic = read32(0x00);\n"
        "  const uint32_t version = read32(0x04);\n"
        "  const uint32_t deviceID = read32(0x08);\n"
        "  native_virtio_magic = magic;\n"
        "  native_virtio_version = version;\n"
        "  native_virtio_device_id = deviceID;\n"
        "  native_virtio_vendor_id = read32(0x0c);\n"
        "  if (magic != 0x74726976 || version != 2 || deviceID != 2) return false;",
        "MMIO observer",
    )
    source = replace_once(
        source,
        "  __atomic_fetch_add(&native_virtio_read_requests, 1, __ATOMIC_RELAXED);\n  return kIOReturnSuccess;",
        "  __atomic_fetch_add(&native_virtio_read_requests, 1, __ATOMIC_RELAXED);\n"
        "  __atomic_fetch_add(&native_virtio_bytes_read, bytes, __ATOMIC_RELAXED);\n"
        "  return kIOReturnSuccess;",
        "successful DMA byte counter",
    )
    source = replace_once(
        source,
        write_guard,
        "  if (buffer->getDirection() != kIODirectionIn) {\n"
        "    __atomic_fetch_add(&native_virtio_write_requests, 1, __ATOMIC_RELAXED);\n"
        "    __atomic_fetch_add(&native_virtio_write_refusals, 1, __ATOMIC_RELAXED);\n"
        "    return kIOReturnNotWritable;\n"
        "  }",
        "write refusal counters",
    )
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--tree", type=Path, required=True)
    parser.add_argument("--source-repository", type=Path, required=True)
    parser.add_argument("--storage-audit", type=Path, required=True)
    parser.add_argument("--qualification", type=Path, required=True)
    parser.add_argument("--compile-record", type=Path)
    parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    project, tree, repository = args.project.resolve(), args.tree.resolve(), args.source_repository.resolve()
    if not args.locked:
        os.execv(
            str(project / "Mu-Silicium/Resources/Scripts/with-build-lock"),
            ["with-build-lock", "T70Pro", "Build isolated T70 VirtIO driver", "--",
             sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"],
        )

    revision = subprocess.check_output(["git", "-C", str(repository), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(
        ["git", "-C", str(repository), "status", "--porcelain", "--untracked-files=all"], text=True
    )
    source = repository / "NativeVirtioBlock.cpp"
    if revision != PIN or dirty or digest(source) != PIN_SHA:
        raise SystemExit("VirtioMmioBlock pin/hash mismatch or dirty checkout; preserving source")

    base = json.loads((project / "analysis/xnu-ios-security-prelink-build.json").read_text())
    plan = json.loads(Path(base["report"]).read_text())
    audit_path, qualification_path = args.storage_audit.resolve(), args.qualification.resolve()
    audit, qualification = json.loads(audit_path.read_text()), json.loads(qualification_path.read_text())
    if not (
        base.get("status", "").startswith("emitted")
        and base.get("original_sha256") == CACHE_SHA
        and digest(base["path"]) == base["sha256"]
        and plan.get("candidate_sha256") == base["sha256"]
        and plan.get("original_sha256") == CACHE_SHA
        and plan.get("slide") == -0x1000000
        and audit.get("candidate_sha256") == base["sha256"]
        and audit.get("public_source_revision") == IOSTORAGE_REV
        and audit.get("original_executable_code_unchanged") is True
        and audit.get("method_slots") == 190
        and audit.get("kernel_bindings") == 176
        and audit.get("pure_virtual_slots") == 14
        and audit.get("live_native_4k") is True
        and audit.get("abstract_complete_destructors_trap") is True
        and audit.get("live_instance_sizes") == {"IOBlockStorageDevice": 144, "IOService": 136}
        and qualification.get("audit_sha256") == digest(audit_path)
        and qualification.get("candidate_sha256") == base["sha256"]
        and qualification.get("original_sha256") == CACHE_SHA
        and qualification.get("metadata_sha256") == base.get("metadata_sha256")
        and qualification.get("disk_io_tested") is False
    ):
        raise SystemExit("Measured Darwin23 IOStorage audit does not match the iOS security candidate")
    extents = [e for e in plan.get("extents", []) if e.get("module") == "com.apple.iokit.IOStorageFamily"]
    if not any(e.get("segment") == "__TEXT_EXEC" for e in extents) or not any(e.get("segment") == "__DATA_CONST" for e in extents):
        raise SystemExit("Candidate lacks IOStorageFamily executable/class extents")

    source_copy = tree / "drivers/VirtioMmioBlock/NativeVirtioBlock.cpp"
    if source_copy.parent.is_symlink():
        raise SystemExit("Refusing source-copy directory linked to pinned upstream source")
    source_copy.parent.mkdir(parents=True, exist_ok=True)
    generated = instrument(source.read_text())
    if source_copy.exists():
        if source_copy.read_text() != generated:
            raise SystemExit("Existing generated driver source differs; preserving it")
    else:
        source_copy.write_text(generated)

    out = tree / "build/virtio-mmio-block"
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [sys.executable, tree / "scripts/prepare-storage-headers.py",
         "--source", tree / "refer/IOStorageFamily", "--output", tree / "build/storage-abi"],
        cwd=tree, check=True,
    )
    variant_audit = tree / "build/storage-abi/interface-audit.json"
    if variant_audit.exists() and digest(variant_audit) != digest(audit_path):
        previous_report = out / "t70pro-build-report.json"
        previous = json.loads(previous_report.read_text()) if previous_report.is_file() else {}
        if digest(variant_audit) != previous.get("storage_abi_audit_sha256"):
            raise SystemExit("Unrecognized variant ABI audit differs; preserving it")
    shutil.copyfile(audit_path, variant_audit)

    record = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_T70PRO/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    record_origin = "variant iOS compile record"
    if not record.is_file():
        if not args.compile_record or not args.compile_record.is_file():
            raise SystemExit("Missing variant iOS compile record")
        record.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.compile_record, record)
        record_origin = "base iOS compile record used for driver-only partial build"
    if json.loads(record.read_text().strip().rstrip(",")).get("directory") is None:
        raise SystemExit("Compile record is missing its working directory")

    subprocess.run([sys.executable, tree / "scripts/build-arm-driver.py", "VirtioMmioBlock"], cwd=tree, check=True)
    subprocess.run(
        [sys.executable, tree / "scripts/build-virtio-mmio-bindings.py",
         "--component", "VirtioMmioBlock", "--directory", out],
        cwd=tree, check=True,
    )
    object_path, archive_path = out / "NativeVirtioBlock.o", out / "libVirtioMmioBlock.a"
    binding_path, compile_path = out / "bindings.json", out / "compile-command.json"
    binding = json.loads(binding_path.read_text())
    if not (
        binding.get("original_candidate_sha256") == base["sha256"]
        and binding.get("compiled_object_sha256") == digest(object_path)
        and binding.get("virtual_methods_checked") == 190
        and binding.get("unresolved_original_imports") == 0
        and binding.get("original_code_modified") is False
        and binding.get("actual_io_qualified") is False
    ):
        raise SystemExit("Driver archive failed measured IOStorage binding checks")
    expected = {
        "__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass": audit["original_protected_constructor"],
        "__ZN20IOBlockStorageDeviceD2Ev": audit["original_base_destructor"],
        "__ZN20IOBlockStorageDevice10gMetaClassE": audit["original_metaclass"],
        "__ZTV20IOBlockStorageDevice": audit["vtable"],
    }
    for name, value in expected.items():
        if binding.get("bindings", {}).get(name) != value:
            raise SystemExit("Fixed storage binding differs from the already-slid audit")
    nm = subprocess.check_output(["xcrun", "nm", "-g", archive_path], text=True)
    symbols = set()
    for line in nm.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2] not in ("U", "u"):
            symbols.add(fields[-1])
    missing = sorted(set(SYMBOLS) - symbols)
    if missing:
        raise SystemExit("Archive lacks runner symbols: " + ", ".join(missing))

    report = {
        "status": "archive_built_runtime_unqualified",
        "scope": "isolated iOS QEMU VirtIO MMIO candidate",
        "source_revision": revision,
        "source_sha256": digest(source),
        "generated_source": str(source_copy),
        "generated_source_sha256": digest(source_copy),
        "generator_sha256": digest(__file__),
        "storage_abi_audit": str(audit_path),
        "storage_abi_audit_sha256": digest(audit_path),
        "storage_qualification_report": str(qualification_path),
        "storage_qualification_report_sha256": digest(qualification_path),
        "storage_abi_candidate_sha256": base["sha256"],
        "storage_abi_original_sha256": CACHE_SHA,
        "storage_abi_slide": plan["slide"],
        "binding_addresses_already_include_slide": True,
        "storage_abi_method_slots": binding["virtual_methods_checked"],
        "storage_abi_kernel_bindings": audit["kernel_bindings"],
        "storage_abi_unresolved_original_imports": binding["unresolved_original_imports"],
        "compile_record": str(record),
        "compile_record_sha256": digest(record),
        "compile_record_origin": record_origin,
        "compile_command": str(compile_path),
        "compile_command_sha256": digest(compile_path),
        "object": str(object_path),
        "object_sha256": digest(object_path),
        "archive": str(archive_path),
        "archive_sha256": digest(archive_path),
        "bindings": str(binding_path),
        "bindings_sha256": digest(binding_path),
        "service_class": "NativeVirtioBlock",
        "provider_class": "IOPlatformDevice",
        "provider_match": 'compatible="virtio,mmio" (12 bytes)',
        "required_provider_properties": ["reg", "dma-coherent"],
        "transport": "VirtIO MMIO v2 split queue with bounded polling",
        "gdb_symbols": {
            "_native_virtio_magic": {"type": "volatile uint32_t", "bytes": 4, "register": "0x00", "expected": "0x74726976"},
            "_native_virtio_version": {"type": "volatile uint32_t", "bytes": 4, "register": "0x04", "expected": 2},
            "_native_virtio_device_id": {"type": "volatile uint32_t", "bytes": 4, "register": "0x08", "expected": 2},
            "_native_virtio_vendor_id": {"type": "volatile uint32_t", "bytes": 4, "register": "0x0c", "driver_checks": False},
            "_native_virtio_mmio_physical": {"type": "volatile uint64_t", "bytes": 8},
            "_native_virtio_ready": {"type": "volatile uint32_t", "bytes": 4},
            "_native_virtio_read_requests": {"type": "volatile uint32_t", "bytes": 4, "includes_startup_lba0": True},
            "_native_virtio_read_failures": {"type": "volatile uint32_t", "bytes": 4},
            "_native_virtio_sectors": {"type": "volatile uint64_t", "bytes": 8},
            "_native_virtio_bytes_read": {"type": "volatile uint64_t", "bytes": 8, "meaning": "successful validated DMA payload bytes, including startup LBA0"},
            "_native_virtio_first_sector_sha256": {"type": "unsigned char[32]", "bytes": 32},
            "_native_virtio_write_requests": {"type": "volatile uint32_t", "bytes": 4},
            "_native_virtio_write_refusals": {"type": "volatile uint32_t", "bytes": 4},
            "_native_virtio_read_checkpoint": {"type": "noinline function"},
        },
        "read_only_contract": {
            "virtio_block_request_opcode": 0,
            "write_operation_emitted": False,
            "outbound_descriptor": "returns kIOReturnNotWritable",
            "media_reports_write_protected": True,
            "format_returns_not_writable": True,
            "runtime_write_attempts_observed": False,
        },
        "required_symbols": SYMBOLS,
        "linked_into_kernel": False,
        "actual_block_io_observed": False,
        "filesystem_mounted": False,
        "actual_io_qualified": False,
    }
    report_path = out / "t70pro-build-report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("PASS: pinned VirtioMmioBlock object and Darwin23 IOStorage bindings built")
    print("Report:", report_path)


if __name__ == "__main__":
    main()
