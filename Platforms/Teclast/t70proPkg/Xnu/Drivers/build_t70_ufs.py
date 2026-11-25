#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the isolated T70 UFS archive and generated IOKit registration against the measured storage ABI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

here = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--project", type=Path, required=True)
parser.add_argument("--tree", type=Path, required=True)
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
project, tree = args.project.resolve(), args.tree.resolve()
mu = project / "Mu-Silicium"
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Build T70 UFS driver", "--", sys.executable,
              str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
sys.dont_write_bytecode = True
if tree != project / "tools/xnu-build-t70-ufs":
    raise SystemExit("The physical driver must use its isolated build tree")
out = tree / "build/t70-ufs"
out.mkdir(parents=True, exist_ok=True)
driver = here / "T70Ufs"
shared = mu / "Silicon/MediaTek/MediaTekPkg"
fdt = mu / "Mu_Basecore/MdePkg/Library/BaseFdtLib/libfdt/libfdt"
report_path = out / "build-report.json"
report = {"status": "running", "linked_into_kernel": False, "device_boot": "not_run",
          "physical_io_verified": False, "stages": []}
def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def run(label, command, cwd=None):
    result = subprocess.run([str(a) for a in command], cwd=cwd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (out / (label + ".log")).write_text(result.stdout)
    report["stages"].append({"name": label, "exit_code": result.returncode})
    if result.returncode:
        print(result.stdout[-6000:], end="", flush=True)
        raise RuntimeError(label + " failed")
    return result.stdout
def compile_record(label, recipe, source, output, extra=()):
    record = json.loads(recipe.read_text().strip().rstrip(","))
    arguments = record["arguments"]
    command = []
    index = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record["file"]:
            command.append(str(source))
        elif value == "-o":
            command.extend(["-o", str(output)])
            index += 1
        elif value == "-Xclang" and arguments[index + 1].startswith("-W"):
            index += 1
        elif not value.startswith("-W"):
            command.append(value)
        index += 1
    command += ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-c99-designator",
                "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
                "-Wno-unnecessary-virtual-specifier", "-Wno-sign-compare", "-Wno-missing-designated-field-initializers", *map(str, extra)]
    (out / (label + "-command.json")).write_text(json.dumps(command, indent=2) + "\n")
    run(label, command, cwd=record["directory"])
    return output

try:
    virtio = json.loads((tree / "build/virtio-mmio-block/t70pro-build-report.json").read_text())
    audit_path = Path(virtio["storage_abi_audit"])
    audit = json.loads(audit_path.read_text())
    base = json.loads((project / "analysis/xnu-ios-security-prelink-build.json").read_text())
    if (digest(audit_path) != virtio["storage_abi_audit_sha256"] or audit["method_slots"] != 190 or
            audit["candidate_sha256"] != base["sha256"] or digest(base["path"]) != base["sha256"] or
            not audit["live_native_4k"] or not audit["original_executable_code_unchanged"]):
        raise RuntimeError("The measured original storage ABI is required")
    objects = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_T70PRO"
    cpp_record = objects / "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    c_record = objects / "bsd/DEVELOPMENT/vfs_init.o.json"
    includes = ["-I", tree / "build/storage-abi/include", "-I", shared / "Include",
                "-I", driver, "-I", tree / ".cache/xnu-work/osfmk", "-mgeneral-regs-only"]
    compiled = []
    for name in ("NativeT70UfsBlock", "T70UfsControllerProvider"):
        compiled.append(compile_record(name, cpp_record, driver / (name + ".cpp"), out / (name + ".o"), includes))
    # Only the generated binding helper is extended; the pinned common source remains untouched.
    helper = (tree / "scripts/build-virtio-mmio-bindings.py").read_text()
    old = "choices=['VirtioMmioBlock', 'RamBlock', 'NativeVirtioBlock', 'NativeRamBlock']"
    assert helper.count(old) == 1
    helper = helper.replace(old, "choices=['VirtioMmioBlock', 'RamBlock', 'NativeVirtioBlock', 'NativeRamBlock', 'T70Ufs']", 1)
    old = "{'VirtioMmioBlock': 'NativeVirtioBlock', 'RamBlock': 'NativeRamBlock'}[canonical]"
    assert helper.count(old) == 1
    helper = helper.replace(old, "{'VirtioMmioBlock': 'NativeVirtioBlock', 'RamBlock': 'NativeRamBlock', 'T70Ufs': 'NativeT70UfsBlock'}[canonical]", 1)
    helper_path = tree / "scripts/build-t70-storage-bindings.py"
    helper_path.write_text(helper)
    run("original-storage-bindings", [sys.executable, helper_path, "--component", "T70Ufs", "--directory", out], tree)
    bindings = json.loads((out / "bindings.json").read_text())
    if (bindings["compiled_object_sha256"] != digest(out / "NativeT70UfsBlock.o") or
            bindings["virtual_methods_checked"] != 190 or bindings["unresolved_original_imports"] != 0):
        raise RuntimeError("The concrete UFS class failed original-storage dispatch checks")
    generated_fdt = out / "libfdt"
    generated_fdt.mkdir(exist_ok=True)
    fdt_sources = ("fdt.c", "fdt_ro.c", "fdt_check.c", "fdt_addresses.c")
    fdt_inputs = [fdt / name for name in (*fdt_sources, "fdt.h", "libfdt.h", "libfdt_env.h", "libfdt_internal.h")]
    for path in fdt_inputs:
        text = path.read_text()
        if path.name == "libfdt_env.h":
            text = text.replace("#include <stdlib.h>", "").replace("#include <string.h>", "#include <libkern/libkern.h>")
            assert text.count("#ifdef __APPLE__") == 1
            text = text.split("#ifdef __APPLE__", 1)[0] + "\n#endif\n"
        if path.name == "fdt_addresses.c":
            # Keep the two original read-only cell queries without the unrelated writer.
            writer = "\n/* This function assumes that [address|size]_cells is 1 or 2 */\n"
            assert text.count(writer) == 1
            text = text.split(writer, 1)[0] + "\n"
        (generated_fdt / path.name).write_text(text)
    c_sources = [driver / name for name in ("T70UfsReadOnly.c", "T70UfsHost.c", "T70UfsDeviceTree.c", "T70UfsMountLayout.c")]
    c_sources += [shared / "Library/UfsCoreLib" / name for name in ("MtkUfsTransport.c", "MtkUfsCore.c")]
    c_sources += [generated_fdt / name for name in fdt_sources]
    for path in c_sources:
        compiled.append(compile_record(path.stem, c_record, path, out / (path.stem + ".o"),
                                       [*includes, "-I", generated_fdt, "-DBSD_KERNEL_PRIVATE=1"]))
    shared_bindings_path = tree / "build/virtio-mmio-block/bindings.json"
    shared_bindings = json.loads(shared_bindings_path.read_text())
    if any(shared_bindings["bindings"].get(name) != value for name,value in bindings["bindings"].items()):
        raise RuntimeError("UFS imports do not match the already linked original storage bindings")
    archive = out / "libNativeT70UfsBlock.a"
    if archive.is_symlink():
        raise RuntimeError("Unexpected generated UFS archive symlink")
    archive.unlink(missing_ok=True)
    # Both drivers use the same original IOStorageFamily imports; emit those aliases once.
    run("archive", ["xcrun", "ar", "rcs", archive, *compiled])
    config_record = objects / "iokit/DEVELOPMENT/KernelConfigTables.cpo.json"
    original_config = project / "xnu-sources/xnu-mtk/iokit/KernelConfigTables.cpp"
    config = original_config.read_text()
    ending = '    ")";\n'
    if config.count(ending) != 1:
        raise RuntimeError("The IOKit catalogue anchor changed")
    personalities = '''    "   ,{"
    "     'IOClass'         = T70UfsControllerProvider;"
    "     'IOProviderClass' = IOResources;"
    "     'IOResourceMatch' = IOBSD;"
    "     'IOProbeScore'    = 1000:32;"
    "   },{"
    "     'IOClass'         = NativeT70UfsBlock;"
    "     'IOProviderClass' = T70UfsControllerProvider;"
    "     'IOProbeScore'    = 1000:32;"
    "   }"
'''
    generated_config = out / "KernelConfigTables.cpp"
    generated_config.write_text(config.replace(ending, personalities + ending, 1))
    compiled_config = objects / "iokit/DEVELOPMENT/KernelConfigTables.cpo"
    compile_record("iokit-registration", config_record, generated_config, compiled_config)
    nm = run("symbols", ["xcrun", "nm", "-g", archive])
    names = {words[-1] for line in nm.splitlines() if len(words := line.split()) >= 3 and words[-2] not in ("U", "u")}
    required = ["_T70UfsHostStart", "_T70UfsHostStop", "_T70UfsHostReadBlocks", "_T70UfsReadBlocks",
                "_T70UfsValidateDeviceTree", "_t70_ufs_ready", "_t70_ufs_probe_attempts", "_t70_ufs_probe_refusals",
                "_t70_ufs_start_attempts", "_t70_ufs_mmio_accesses", "_t70_ufs_dma_quarantines",
                "__ZTV17NativeT70UfsBlock", "__ZTV24T70UfsControllerProvider"]
    missing = sorted(set(required) - names)
    if missing:
        raise RuntimeError("Archive lacks required UFS symbols: " + str(missing))
    inputs = sorted(path for path in driver.iterdir() if path.suffix in (".h", ".c", ".cpp"))
    inputs += [shared / "Library/UfsCoreLib" / name for name in ("MtkUfsTransport.c", "MtkUfsCore.c")]
    inputs += [shared / "Include/Library" / name for name in ("MtkUfsTransport.h", "MtkUfsCore.h")]
    inputs += fdt_inputs + [here / "build_t70_ufs.py", original_config]
    inputs += [project / "xnu-sources/xnu-mtk/bsd/sys" / name for name in
               ("mount_internal.h", "proc_internal.h", "filedesc.h", "vnode_internal.h", "vnode.h")]
    report.update(status="archive_and_iokit_registration_built_runtime_unqualified",
        archive=str(archive), archive_sha256=digest(archive),
        bindings=str(out / "bindings.json"), bindings_sha256=digest(out / "bindings.json"),
        shared_original_bindings=str(shared_bindings_path), shared_original_bindings_sha256=digest(shared_bindings_path),
        storage_abi_audit=str(audit_path), storage_abi_audit_sha256=digest(audit_path),
        virtual_methods_checked=bindings["virtual_methods_checked"],
        storage_abi_candidate_sha256=base["sha256"], binding_addresses_already_include_slide=True,
        required_symbols=required, generator_sha256=digest(__file__),
        files=[{"path": str(path), "sha256": digest(path)} for path in inputs],
        generated_catalogue=str(generated_config), generated_catalogue_sha256=digest(generated_config),
        catalogue_object=str(compiled_config), catalogue_object_sha256=digest(compiled_config),
        generated_fdt_files=[{"path":str(path),"sha256":digest(path)} for path in sorted(generated_fdt.iterdir())])
    print("PASS: concrete UFS storage class, retained-link provider and generated IOKit registration compiled", flush=True)
except Exception as error:
    report.update(status="failed", error=str(error))
    raise
finally:
    report_path.write_text(json.dumps(report, indent=2) + "\n")
