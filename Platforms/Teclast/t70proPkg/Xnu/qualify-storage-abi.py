#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Qualify the real Darwin 23 storage ABI against a live T70PRO VM candidate."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
from types import SimpleNamespace

here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Qualify original storage class against actual VM memory", "--",
              sys.executable, str(Path(__file__).resolve()), "--locked"])
sys.dont_write_bytecode = True
common = project / "xnu-sources/UniXNU"
base = project / "tools/xnu-build"
work = project / "tools/xnu-storage-abi-live"
work.mkdir(parents=True, exist_ok=True)
report_path = project / "analysis/xnu-storage-abi-qualification.json"
report = {"status": "running", "device_boot": "not_run", "disk_io_tested": False,
          "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def git(path, *arguments):
    return subprocess.check_output(["git", "-C", str(path), *arguments], text=True, timeout=60).strip()

def checked(name, command, env=None):
    result = subprocess.run([str(x) for x in command], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (work / (name + ".log")).write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(name + ": " + result.stdout[-3500:])
    print(name + ": passed", flush=True)
    return result.stdout

try:
    require(git(common, "rev-parse", "HEAD") == "dff276535db3ac394f628cec58a718cd46c20ac2" and
            not git(common, "status", "--porcelain"), "Pinned common source changed")
    checked("loader-source-pins", [sys.executable, here / "prepare-sources.py", "--check"])
    public = project / "xnu-sources/IOStorageFamily"
    public_pin = "e45fbe30a7879e0b16e26dddb78538f1b00e0bff"
    public_url = "https://github.com/apple-oss-distributions/IOStorageFamily.git"
    if not public.exists():
        checked("public-storage-fetch", ["git", "clone", "--no-checkout", "--filter=blob:none", public_url, public])
        checked("public-storage-checkout", ["git", "-C", public, "checkout", "--detach", public_pin])
    require(git(public, "rev-parse", "HEAD") == public_pin and not git(public, "status", "--porcelain"),
            "Public storage source pin changed; existing source preserved")
    kit = base / "kits/XnuPrelink"
    kit.parent.mkdir(parents=True, exist_ok=True)
    if not kit.exists():
        kit.symlink_to(project / "xnu-sources/XnuPrelink")
    require(kit.resolve() == (project / "xnu-sources/XnuPrelink").resolve(), "Unexpected prelink source link")
    inputs = json.loads((project / "analysis/xnu-ios-security-prelink-build.json").read_text())
    candidate, plan_path = Path(inputs["path"]), Path(inputs["report"])
    require(sha(candidate) == inputs["sha256"] and sha(plan_path) == inputs["metadata_sha256"],
            "Security candidate or metadata changed")
    plan = json.loads(plan_path.read_text())
    require(plan["slide"] == -0x1000000, "Unexpected original module slide")
    prior = json.loads((project / "analysis/xnu-qemu-prelink-security-root-entry-execution.json").read_text())
    require(prior["status"] == "passed" and prior["kernel_sha256"] == inputs["sha256"],
            "A verified security candidate root-entry run is required")
    esp = project / "tools/xnu-qemu-prelink-security-root-entry/esp"
    require(sha(esp / "xnu/kernel.macho") == inputs["sha256"], "Previous VM image changed")
    for name, relative in [("vm_launcher", "EFI/BOOT/BOOTAA64.EFI"),
                           ("darwinboot", "EFI/Teclast/DarwinBoot.efi")]:
        require(sha(esp / relative) == prior["efi_sha256"][name], "Previous VM loader changed")
    os.environ["UNIXNU_TOOLCHAIN"] = str(project / "tools/unixnu-linux")
    sys.path.insert(0, str(common / "scripts"))
    from unixnu.kernel import linux_env
    env = {k: v for k, v in os.environ.items() if not k.startswith(("XNU_", "UNIXNU_"))}
    env.update(linux_env(SimpleNamespace(tree=base)))
    env.update({"UNIXNU_TOOLCHAIN": str(project / "tools/unixnu-linux"),
                "PYTHONDONTWRITEBYTECODE": "1"})
    helper_source = common / "scripts/audit-storage-interface.py"
    helper = helper_source.read_text()
    substitutions = [
        ("['xcrun', 'dwarfdump',", "['llvm-dwarfdump',"),
        ("ROOT = Path(__file__).resolve().parents[1]", "ROOT = Path(" + repr(str(base)) + ")"),
        ("source = ROOT / 'refer/IOStorageFamily'", "source = Path(" + repr(str(public)) + ")"),
        ("DEVELOPMENT_ARM64_BCM2837", "DEVELOPMENT_ARM64_T70PRO"),
        ("kernel_path = ROOT / 'build/xnu-prelink/native-apfs-load-positive.macho'",
         "kernel_path = Path(" + repr(str(candidate)) + ")"),
        ("d = json.loads((ROOT / 'build/darwinboot-storage-native-metadata/gdb-stop.json').read_text())",
         "Path(" + repr(str(work / "requested-addresses.json")) +
         ").write_text(json.dumps(dict(meta=meta,service_meta=service_meta,candidate_sha256=plan['candidate_sha256'])))\n"
         "snapshot_path = Path(" + repr(str(work / "gdb-stop.json")) + ")\n"
         "if not snapshot_path.is_file():\n    raise SystemExit(0)\n"
         "d = json.loads(snapshot_path.read_text())"),
        ("layout = json.loads((ROOT / 'build/image4-ignition/aes-interface-audit.json').read_text())['compiled_metaclass_offsets']",
         "layout = json.loads(Path(" + repr(str(work / "compiled-layout.json")) + ").read_text())['compiled_metaclass_offsets']")
    ]
    for old, new in substitutions:
        require(helper.count(old) == 1, "Pinned audit input block changed: " + old[:70])
        helper = helper.replace(old, new)
    generated = work / "audit-storage-interface.py"
    generated.write_text(helper)
    (work / "gdb-stop.json").unlink(missing_ok=True)
    checked("storage-static-addresses", [sys.executable, generated], env)
    requested = json.loads((work / "requested-addresses.json").read_text())
    require(requested["candidate_sha256"] == inputs["sha256"], "Static audit identity changed")
    layout_object = base / "build/xnu/ios-obj/DEVELOPMENT_ARM64_T70PRO/libkern/DEVELOPMENT/OSMetaClass.cpo"
    dwarf = checked("metaclass-dwarf", ["llvm-dwarfdump", "--name=OSMetaClass", "--show-children", layout_object], env)
    layout = {}
    for member in dwarf.split("DW_TAG_member")[1:]:
        name = re.search(r'DW_AT_name\s+\("([^"]+)"\)', member)
        position = re.search(r'DW_AT_data_member_location\s+\((0x[0-9a-f]+)\)', member)
        if name and position and name[1] in ("superClassLink", "className", "classSize"):
            layout[name[1]] = int(position[1], 16)
    require(layout == dict(superClassLink=16, className=24, classSize=32),
            "Actual compiled OSMetaClass layout changed")
    (work / "compiled-layout.json").write_text(json.dumps({
        "compiled_metaclass_offsets": layout, "object_sha256": sha(layout_object)}) + "\n")
    nm = project / "tools/unixnu-linux/CommandLineTools/usr/bin/nm"
    symbols = {}
    for line in subprocess.check_output([str(nm), "-n", str(candidate)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3 and re.fullmatch("[0-9a-fA-F]+", fields[0]):
            symbols[fields[2]] = int(fields[0], 16)
    with socket.socket() as reserved:
        reserved.bind(("127.0.0.1", 0))
        port = reserved.getsockname()[1]
    output = work / "gdb-stop.json"
    script = [
        "set pagination off", "set confirm off", "set architecture aarch64",
        "target remote 127.0.0.1:" + str(port),
        "hbreak *" + hex(symbols["_panic"]), "hbreak *" + hex(symbols["_bsd_init"]),
        "continue", "python", "import gdb,json",
        "pc=int(gdb.parse_and_eval('$pc'))",
        "out={'kernel_sha256':" + repr(inputs["sha256"]) +
        ",'bounded_timeout_interrupted':False,'pc':hex(pc),'memory':{},'system_registers':{}}",
        "out['stop']='bsd_init' if pc==" + str(symbols["_bsd_init"]) + " else 'panic'",
        "for address in " + repr([requested["meta"], requested["service_meta"]]) + ":",
        "    out['memory'][f'{address:x}:50']=bytes(gdb.selected_inferior().read_memory(address,0x50)).hex()",
        "for name in ['sctlr_el1','tcr_el1','ttbr0_el1','ttbr1_el1']:",
        "    for alias in [name,name.upper()]:",
        "        try:",
        "            out['system_registers'][name.upper()]=hex(int(gdb.parse_and_eval('$'+alias)) & ((1<<64)-1)); break",
        "        except Exception: pass",
        "    if name.upper() not in out['system_registers']: raise RuntimeError('Missing live system register '+name)",
        "open(" + repr(str(output)) + ",'w').write(json.dumps(out,indent=2)+'\\n')",
        "end", "disconnect", "quit", ""]
    script_path = work / "observe.gdb"
    script_path.write_text("\n".join(script))
    command = list(prior["qemu_command"])
    command[command.index("-gdb") + 1] = "tcp:127.0.0.1:" + str(port)
    command[command.index("-serial") + 1] = "file:" + str(work / "serial.log")
    report.update(candidate_sha256=inputs["sha256"], original_sha256=inputs["original_sha256"],
                  metadata_sha256=inputs["metadata_sha256"], qemu_command=command,
                  efi_sha256=prior["efi_sha256"])
    with (work / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            checked("live-storage-metadata", ["gdb-multiarch", "-q", "-batch", "-x", script_path], env)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    snapshot = json.loads(output.read_text())
    require(snapshot["stop"] == "bsd_init" and int(snapshot["pc"], 16) == symbols["_bsd_init"],
            "Actual storage snapshot did not reach bsd_init")
    checked("storage-full-audit", [sys.executable, generated], env)
    audit_path = base / "build/storage-abi/interface-audit.json"
    audit = json.loads(audit_path.read_text())
    require(audit["candidate_sha256"] == inputs["sha256"] and audit["live_native_4k"],
            "Completed storage audit does not match actual observation")
    audit.update(qualification_script_sha256=sha(__file__), generated_audit_sha256=sha(generated),
                 upstream_audit_sha256=sha(helper_source), live_snapshot_sha256=sha(output),
                 compiled_layout_sha256=sha(work / "compiled-layout.json"),
                 metadata_sha256=inputs["metadata_sha256"], original_sha256=inputs["original_sha256"],
                 original_module_slide=plan["slide"], public_repository=public_url,
                 fixture_scope="QEMU metadata only; block I/O not exercised")
    audit_path.write_text(json.dumps(audit, indent=2) + "\n")
    report.update(status="storage_class_abi_and_live_metadata_passed_disk_io_unqualified",
                  audit_path=str(audit_path), audit_sha256=sha(audit_path),
                  method_slots=audit["method_slots"], kernel_bindings=audit["kernel_bindings"],
                  live_instance_sizes=audit["live_instance_sizes"], live_native_4k=True)
    print("PASS: actual public class/vtable and live original storage metadata; disk I/O remains unqualified", flush=True)
except Exception as error:
    report.update(status="failed", error=str(error))
    raise
finally:
    report.update(finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  qualification_script_sha256=sha(__file__))
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path, flush=True)
