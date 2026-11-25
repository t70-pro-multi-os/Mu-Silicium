#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the actual staged T70PRO kernel with the pinned loader and link/POST records."""
import argparse
import datetime
import hashlib
import hmac
import json
import os
import re
import tempfile
from pathlib import Path
import struct
import subprocess
import sys

here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--os", choices=("ios", "macos"), default="ios")
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
os_suffix = "-macos" if args.os == "macos" else ""
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Verify built T70PRO kernel", "--",
              sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
sys.dont_write_bytecode = True
report_path = project / f"analysis/xnu-kernel{os_suffix}-verification.json"
report = {"time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "status": "running", "os_abi": args.os, "device_boot": "not_run", "runtime_crypto_selftests": "not_run"}

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

try:
    build = json.loads((project / f"analysis/xnu-kernel{os_suffix}-build.json").read_text())
    require(build.get("os_abi") == args.os, "Kernel OS ABI selection mismatch")
    require(build["status"] == "passed", "Full kernel build has not passed")
    if args.os == "macos":
        target_profile = here / "targets/macbook-neo.toml"
        expected_target = hashlib.sha256(target_profile.read_bytes()).hexdigest()
        require(expected_target == build.get("target_profile_sha256"),
                "Selected Neo target changed after the bootstrap build")
        require(Path(build["target_profile_stage"]).read_bytes() == target_profile.read_bytes(),
                "Staged Neo target profile mismatch")
        require(build.get("target_os_runtime_qualified") is False,
                "The bootstrap cannot qualify Neo OS execution")
        report["product_target"] = build["product_target"]
        report["target_profile_sha256"] = expected_target
        report["target_os_runtime_qualified"] = False
    image, stage = Path(build["image"]["path"]), Path(build["image"]["stage"])
    data = image.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    require(stage.read_bytes() == data and digest == build["image"]["sha256"], "Build/stage hash mismatch")
    require(struct.unpack_from("<4I", data) == (0xfeedfacf, 0x100000c, 0, 2), "Plain ARM64 MH_EXECUTE required")
    ncmds, sizeofcmds = struct.unpack_from("<2I", data, 16)
    end, offset, targets = 32+sizeofcmds, 32, []
    require(end <= len(data), "Load commands exceed the kernel image")
    for index in range(ncmds):
        require(offset+8 <= end, "Truncated Mach-O load command")
        command, size = struct.unpack_from("<2I", data, offset)
        require(size >= 8 and size % 8 == 0 and offset+size <= end, "Invalid Mach-O load command size")
        if command == 0x32:
            require(size >= 24, "Truncated deployment command")
            targets.append(struct.unpack_from("<3I", data, offset+8))
        offset += size
    require(offset == end, "Mach-O load command count/size mismatch")
    expected = (1, 14 << 16) if args.os == "macos" else (2, 17 << 16)
    require(len(targets) == 1 and targets[0][:2] == expected, "Wrong kernel OS/deployment target")
    report["macho_target"] = dict(zip(("platform", "minimum_version", "sdk_version"), targets[0]))
    # Decode with the forbidden extensions enabled so those opcodes cannot hide
    # as <unknown>. HINT-space PAC mnemonics remain compatible with non-PAC CPUs.
    forbidden = set("xpaci xpacd autia autib autda autdb pacia pacib pacda pacdb pacga "
                    "paciza pacizb pacdza pacdzb autiza autizb autdza autdzb "
                    "braa braaz brab brabz blraa blraaz blrab blrabz retaa retab "
                    "ldraa ldrab eretaa eretab".split())
    decoder = ["llvm-objdump", "-d", "--no-show-raw-insn",
               "--mattr=+v8.2a,+pauth,+rcpc-immo", str(image)]
    instructions, bad = 0, []
    with tempfile.TemporaryFile(mode="w+") as diagnostics:
        decoded = subprocess.Popen(decoder, stdout=subprocess.PIPE, stderr=diagnostics, text=True)
        for line in decoded.stdout:
            instruction = re.match(r"^\s*([0-9a-f]+):\s+([.a-z0-9]+)(?:\s|$)", line)
            if not instruction:
                continue
            instructions += 1
            mnemonic = instruction[2]
            if mnemonic in forbidden or re.fullmatch(r"stlur[b-h]*|ldapur[a-z]*", mnemonic):
                if len(bad) < 20:
                    bad.append(line.strip())
        result = decoded.wait()
        diagnostics.seek(0)
        warnings = diagnostics.read()
    report["instruction_check"] = {"scope": "non-HINT PAC and RCpc-immediate instructions",
        "command": decoder, "decoded_instructions": instructions, "forbidden_examples": bad,
        "decoder_exit_code": result, "decoder_diagnostics": warnings}
    require(result == 0 and not warnings and instructions > 100000, "Kernel instruction decoder failed")
    require(not bad, "Kernel contains unsupported PAC/RCpc-immediate instructions: " + repr(bad))
    kernel = project / "xnu-sources/xnu-mtk"
    for name, expected in build["kernel_source_sha256"].items():
        require(hashlib.sha256((kernel/name).read_bytes()).hexdigest() == expected,
                "Kernel source changed since full build: " + name)
    nm = project / "tools/unixnu-linux/CommandLineTools/usr/bin/nm"
    undefined = subprocess.check_output([str(nm), "-uj", str(image)], text=True).splitlines()
    require(not undefined, "Unresolved kernel symbols: " + repr(undefined))
    defined = set(subprocess.check_output([str(nm), "-jU", str(image)], text=True).splitlines())
    required = {"_PE_cpu_start", "__ZN6ArmCpu8startCPUEmm", "__ZN6ArmCpu5startEP9IOService",
                "__ZN13ArmDTPlatform9configureEP9IOService", "_source_corecrypto14_bootstrap"}
    require(required <= defined, "Missing linked CPU/platform/crypto provider")
    require("_yunluo_panic_flush" not in defined, "Donor partition panic backend is present")
    metadata = json.loads(Path(build["crypto_integrity_record"]).read_text())
    require(metadata["kernel_sha256"] == digest, "POST record does not match the kernel")
    offset, size = int(metadata["code_file_offset"], 16), metadata["code_bytes"]
    integrity = int(metadata["hmac_file_offset"], 16)
    require(0 <= offset < offset+size <= len(data) and 0 <= integrity <= len(data)-32,
            "POST record has invalid ranges")
    mac = hmac.digest(b"\0", data[offset:offset+size], "sha256")
    require(hmac.compare_digest(data[integrity:integrity+32], mac) and mac.hex() == metadata["hmac_sha256"],
            "Source crypto POST integrity field differs from the final instructions")
    command = [sys.executable, str(project / "xnu-sources/DarwinBoot/Test/test-kernel-parser.py"), str(stage)]
    parsed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report["darwinboot_parser"] = {"exit_code": parsed.returncode, "output": parsed.stdout}
    print(parsed.stdout, end="")
    parsed.check_returncode()
    adt_record = json.loads((project / f"analysis/xnu-adt{os_suffix}-build.json").read_text())
    adt = Path(adt_record["path"])
    require(adt_record == build["adt"] and hashlib.sha256(adt.read_bytes()).hexdigest() == adt_record["sha256"],
            "ADT differs from the full build record")
    env = os.environ.copy()
    env["LD_PRELOAD"] = subprocess.check_output(["gcc", "-print-file-name=libubsan.so"], text=True).strip()
    adt_parsed = subprocess.run([sys.executable, str(here / "run-upstream-check.py"), "test-adt-window.py", str(adt)],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    report["darwinboot_adt_parser"] = {"exit_code": adt_parsed.returncode, "output": adt_parsed.stdout}
    print(adt_parsed.stdout, end="")
    adt_parsed.check_returncode()
    runner_digest = hashlib.sha256((here/"qemu-check.py").read_bytes()).hexdigest()
    observer_files = {
        "vm_launcher": mu / "Build/t70proPkg/RELEASE_CLANGPDB/AARCH64/T70QemuXnu.efi",
        "darwinboot": stage.parents[1] / "EFI/Teclast/DarwinBoot.efi",
    }
    observer_hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest()
                       for name, path in observer_files.items() if path.is_file()}
    report["current_qemu_efi_sha256"] = observer_hashes
    report["qemu_evidence"] = {}
    report["ignored_qemu_evidence"] = []
    for case in ("", "-negative-post", "-bsd"):
        path = project / f"analysis/xnu-qemu{os_suffix}{case}-execution.json"
        if not path.is_file():
            continue
        evidence = json.loads(path.read_text())
        if (evidence.get("kernel_sha256") != digest or evidence.get("os_abi") != args.os or
                evidence.get("runner_sha256") != runner_digest or
                len(observer_hashes) != len(observer_files) or
                evidence.get("efi_sha256") != observer_hashes):
            report["ignored_qemu_evidence"].append({
                "path": str(path), "reason": "kernel, OS, runner or EFI inputs changed"})
            continue
        report["qemu_evidence"][case or "crypto"] = {"path": str(path), "status": evidence["status"],
            "observation": evidence.get("observation"), "error": evidence.get("error"),
            "native_4k_mmu_observed": evidence.get("native_4k_mmu_observed")}
        if evidence["status"] == "passed" and not case:
            observed = evidence["observation"]
            require(observed["stop"] == "kmem_crypto_init" and observed["source_crypto_status"] == 0 and
                    observed["crypto_pointer"] and observed["prng_ready"] and not observed["source_prelinked_provider_ready"] and evidence["native_4k_mmu_observed"] and
                    evidence["staged_kernel_unchanged"], "Invalid successful runtime crypto evidence")
            report["runtime_crypto_selftests"] = "passed_in_QEMU"
        elif evidence["status"] == "passed" and case == "-negative-post":
            observed = evidence["observation"]
            require(evidence["negative_post"] and evidence["executed_kernel_sha256"] != digest and
                    observed["stop"] == "panic" and observed["source_crypto_status"] != 0 and
                    not observed["crypto_pointer"] and not observed["prng_ready"] and
                    "FIPS Kernel POST Failed" in observed.get("panic_format", "") and
                    evidence["staged_kernel_unchanged"], "Invalid damaged-copy refusal evidence")
            report["runtime_crypto_negative_control"] = "passed_in_QEMU"
    report["adt"] = adt_record
    report.update({"status": "passed", "image": build["image"], "undefined_symbols": undefined,
                   "required_linked_symbols": sorted(required), "post_hmac_matches": True,
                   "kernel_source_hashes_match": True, "bootable_adt": "staged_with_live_allocation_required"})
    print("PASS: full kernel/stage hashes, source hashes, provider linkage, forbidden-instruction scan, source POST HMAC and real kernel/ADT parsers")
except Exception as error:
    report.update({"status": "failed", "error": str(error)})
    raise
finally:
    report["verification_script_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path)
