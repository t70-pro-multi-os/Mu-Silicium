#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the T70PRO fork with pinned UniXNU Linux recipes in one builder workspace."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib

here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--os", choices=("ios", "macos"), default="ios")
parser.add_argument("--virtio", action="store_true", help="Build isolated iOS QEMU VirtIO storage")
parser.add_argument("--t70-ufs", action="store_true", help="Build isolated T70 UFS provider with VirtIO for VM checks")
parser.add_argument("--driver-only", action="store_true", help="Build only the isolated VirtIO driver archive")
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
if args.t70_ufs:
    args.virtio = True
    if args.driver_only:
        parser.error("--t70-ufs requires the complete kernel build")
if args.virtio and args.os != "ios":
    parser.error("--virtio is an iOS-only QEMU candidate")
if args.driver_only and not args.virtio:
    parser.error("--driver-only requires --virtio")
os_suffix = "-macos" if args.os == "macos" else ""
variant_suffix = "-t70-ufs" if args.t70_ufs else "-virtio" if args.virtio else os_suffix
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Build T70PRO XNU kernel", "--",
              sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
sys.dont_write_bytecode = True
common = project / "xnu-sources/UniXNU"
kernel = project / "xnu-sources/xnu-mtk"
prefix = project / "tools/unixnu-linux"
tree = project / ("tools/xnu-build" + variant_suffix)
analysis = project / "analysis"
report_path = analysis / ("xnu-kernel" + variant_suffix + "-build.json")
report = {"started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "target": "T70PRO", "architecture": "arm64", "stages": [], "os_abi": args.os,
          "compiler_platform": "macOS 14.0" if args.os == "macos" else "iOS 17.0",
          "native_user_va_limit_bytes": 0xfc0000000, "hardware_page_bytes": 4096,
          "device_boot": "not_run", "status": "running"}
if args.virtio:
    report["candidate_kind"] = "ios-security-t70-ufs-native-build" if args.t70_ufs else "ios-security-virtio-native-build"
    report["runtime_qualified"] = False

if args.os == "macos":
    target_profile = here / "targets/macbook-neo.toml"
    selected_target = tomllib.loads(target_profile.read_text())
    report["product_target"] = selected_target["product"]
    report["target_os_version"] = selected_target["ipsw"]["version"]
    report["target_os_build"] = selected_target["ipsw"]["build"]
    report["target_profile_sha256"] = hashlib.sha256(target_profile.read_bytes()).hexdigest()
    report["target_os_runtime_qualified"] = False


def run(label, command, *, cwd=None, env=None, tolerate=False):
    log = analysis / ("xnu" + variant_suffix + "-" + label + ".log")
    print(label + ": " + str(log), flush=True)
    with log.open("w") as output:
        result = subprocess.run([str(p) for p in command], cwd=cwd, env=env,
                                stdout=output, stderr=subprocess.STDOUT)
    report["stages"].append({"name": label, "exit_code": result.returncode, "log": str(log)})
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    if result.returncode and not tolerate:
        print("\n".join(log.read_text(errors="replace").splitlines()[-45:]), flush=True)
        raise RuntimeError(label + " failed (" + str(result.returncode) + ")")
    return result.returncode

def git(directory, *args):
    return subprocess.check_output(["git", "-C", str(directory), *args], text=True).strip()

def fetch(name, url, revision):
    destination = project / "xnu-sources" / name
    if not destination.exists():
        destination.mkdir()
        subprocess.run(["git", "init", "-q", str(destination)], check=True)
        subprocess.run(["git", "-C", str(destination), "remote", "add", "origin", url], check=True)
        subprocess.run(["git", "-C", str(destination), "fetch", "--depth=1", "origin", revision], check=True)
        subprocess.run(["git", "-C", str(destination), "checkout", "--detach", "FETCH_HEAD"], check=True)
    if git(destination, "rev-parse", "HEAD") != revision or git(destination, "status", "--porcelain"):
        raise RuntimeError(name + ": pin mismatch/modified source; preserved without reset")
    return destination

def link(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink():
        if destination.resolve() == source.resolve():
            return
        raise RuntimeError("Existing link points elsewhere: " + str(destination))
    if destination.exists():
        raise RuntimeError("Existing non-link preserved: " + str(destination))
    destination.symlink_to(source.resolve())

try:
    run("loader-source-pins", [sys.executable, here / "prepare-sources.py", "--check"])
    baseline = git(kernel, "rev-parse", "HEAD")
    if baseline != "53bc420e5acda41c1689f6690a85841393a347cf":
        raise RuntimeError("Review the new kernel base before changing the build pin")
    report["kernel_base_commit"] = baseline
    report["common_commit"] = git(common, "rev-parse", "HEAD")
    tree.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(common / "scripts"))
    from unixnu.tree import _copy_scripts
    _copy_scripts(common / "scripts", tree / "scripts")
    # Like UniXNU tree.assemble: scripts are generated copies, modules are links.
    # Adapt the device/OS/CPU selection; do not reapply Apple patches to this existing fork.
    adaptations = {}
    for script in (tree / "scripts").rglob("*"):
        if not script.is_file() or script.suffix not in (".py", ".sh"):
            continue
        original = script.read_text()
        updated = original.replace("DEVELOPMENT_ARM64_BCM2837", "DEVELOPMENT_ARM64_T70PRO")
        updated = updated.replace("mach.development.bcm2837", "mach.development.t70pro")
        updated = updated.replace("MACHINE_CONFIGS=BCM2837", "MACHINE_CONFIGS=T70PRO")
        if args.virtio and script.name == "build-xnu-4k-baseline.sh":
            dollar = "$"
            old_guard = "\n".join([
                "if [ \"" + dollar + "{XNU_NATIVE_VIRTIO_BLOCK:-0}\" = 1 ]; then",
                "  if [ \"" + dollar + "{XNU_NATIVE_AES:-0}\" != 1 ]; then",
                "    echo 'NativeVirtioBlock currently requires the qualified NativeAES/original-storage prelink baseline' >&2",
                "    exit 1",
                "  fi",
                "  PERSONALITY_CFLAGS=\"$PERSONALITY_CFLAGS -DXNU_NATIVE_VIRTIO_BLOCK=1\"",
                "fi",
            ])
            new_guard = "\n".join([
                "if [ \"" + dollar + "{XNU_NATIVE_VIRTIO_BLOCK:-0}\" = 1 ]; then",
                "  if [ \"" + dollar + "{XNU_T70_VIRTIO_IOSTORAGE_AUDIT:-0}\" != 1 ]; then",
                "    echo 'NativeVirtioBlock requires the measured Darwin23 original IOStorageFamily ABI audit' >&2",
                "    exit 1",
                "  fi",
                "  PERSONALITY_CFLAGS=\"$PERSONALITY_CFLAGS -DXNU_NATIVE_VIRTIO_BLOCK=1\"",
                "fi",
            ])
            if updated.count(old_guard) != 1:
                raise RuntimeError("Generated VirtIO guard anchor changed")
            updated = updated.replace(old_guard, new_guard, 1)
        if args.os == "macos":
            updated = updated.replace("mach.development.t70pro", "kernel.development.t70pro")
            if script.name == "build-xnu-4k-baseline.sh":
                updated = updated.replace("export PLATFORM=iPhoneOS HOST_CC=", "export PLATFORM=MacOSX HOST_CC=")
                updated = updated.replace('-miphoneos-version-min=$IOS_VERSION $XNU_TARGET_DEFINE',
                                          '-mmacosx-version-min=14.0 $XNU_TARGET_DEFINE')
                updated = updated.replace('-Wl,-ios_version_min,$IOS_VERSION', '-Wl,-platform_version,macos,14.0,14.0')
            elif script.name == "build-firehose.sh":
                updated = updated.replace('"$ARCH-apple-ios$IOS_VERSION"', '"$ARCH-apple-macos14.0"')
                updated = updated.replace('xcrun --sdk iphoneos', 'xcrun --sdk macosx')
            elif script.name == "prepare-xnu.py":
                updated = updated.replace("'arm64-apple-ios17.0'", "'arm64-apple-macos14.0'")
            elif script.name in ("build-corecrypto-14.py", "build-libpthread-kernel.py"):
                updated = updated.replace("'--sdk', 'iphoneos'", "'--sdk', 'macosx'")
            elif script.name == "kernel_target.py":
                updated = updated.replace("TRIPLE = f'{ARCH}-apple-ios{IOS_VERSION}'", "TRIPLE = f'{ARCH}-apple-macos14.0'")
                updated = updated.replace("DEPLOYMENT = (2, int(_major) << 16 | int(_minor) << 8)",
                                          "DEPLOYMENT = (1, 14 << 16)")
        if script.suffix == ".py":
            updated = updated.replace("'-target', TRIPLE,", "'-target', TRIPLE, '-march=armv8.2-a', '-mcpu=cortex-a55',")
            for triple in ("arm64-apple-ios17.0", "arm64-apple-macos14.0"):
                updated = updated.replace("'-target', '" + triple + "',",
                    "'-target', '" + triple + "', '-march=armv8.2-a', '-mcpu=cortex-a55',")
        elif script.name == "build-firehose.sh":
            for triple in ('"$ARCH-apple-ios$IOS_VERSION"', '"$ARCH-apple-macos14.0"'):
                updated = updated.replace(triple + ' -std=',
                    triple + ' -march=armv8.2-a -mcpu=cortex-a55 -std=')
        if updated != original:
            script.write_text(updated)
            adaptations[str(script.relative_to(tree))] = {
                "original_sha256": hashlib.sha256(original.encode()).hexdigest(),
                "generated_sha256": hashlib.sha256(updated.encode()).hexdigest()}
    report["generated_recipe_adaptations"] = adaptations
    link(common / "components.toml", tree / "components.toml")
    link(common / "patches/libpthread", tree / "patches/libpthread")
    link(kernel, tree / "refer/xnu")
    link(kernel, tree / ".cache/xnu-work")
    modules = {}
    catalog = tomllib.loads((common / "components.toml").read_text())["component"]
    for spec in catalog:
        if spec["id"] in ("ArmCpu", "ArmDTPlatform"):
            source = fetch(spec["id"], spec["repository"], spec["revision"])
            link(source, tree / spec["path"])
            modules[spec["id"]] = spec["revision"]
    references = tomllib.loads((common / "feeds.toml").read_text())["source"]
    for spec in references:
        if spec["name"] in ("libplatform", "libdispatch", "AvailabilityVersions", "corecrypto-2023", "libpthread"):
            source = fetch(spec["name"], spec["git"], spec["commit"])
            link(source, tree / "refer" / spec["name"])
            modules[spec["name"]] = spec["commit"]
    virtio_source = None
    storage_revision = "e45fbe30a7879e0b16e26dddb78538f1b00e0bff"
    if args.virtio:
        base_conditionals = project / "tools/xnu-build/.cache/fakeroot/usr/include/TargetConditionals.h"
        base_fakeroot = project / "tools/xnu-build/.cache/fakeroot"
        variant_fakeroot = tree / ".cache/fakeroot"
        if variant_fakeroot.is_symlink():
            if variant_fakeroot.resolve() != base_fakeroot.resolve():
                raise RuntimeError("Refusing an unexpected variant fakeroot link")
            variant_fakeroot.unlink()
        variant_conditionals = variant_fakeroot / "usr/include/TargetConditionals.h"
        variant_conditionals.parent.mkdir(parents=True, exist_ok=True)
        if variant_conditionals.exists():
            if variant_conditionals.read_bytes() != base_conditionals.read_bytes():
                raise RuntimeError("Existing variant TargetConditionals.h differs; preserving it")
        else:
            shutil.copyfile(base_conditionals, variant_conditionals)
        spec = next(item for item in catalog if item["id"] == "VirtioMmioBlock")
        virtio_source = fetch(spec["id"], spec["repository"], spec["revision"])
        storage_source = project / "xnu-sources/IOStorageFamily"
        if git(storage_source, "rev-parse", "HEAD") != storage_revision or git(storage_source, "status", "--porcelain", "--untracked-files=all"):
            raise RuntimeError("IOStorageFamily source pin mismatch/modified source; preserving it")
        link(storage_source, tree / "refer/IOStorageFamily")
        prelink_tools = project / "xnu-sources/XnuPrelink"
        if git(prelink_tools, "status", "--porcelain", "--untracked-files=all"):
            raise RuntimeError("XnuPrelink source is modified; preserving it")
        link(prelink_tools, tree / "kits/XnuPrelink")
        modules["XnuPrelink"] = git(prelink_tools, "rev-parse", "HEAD")
        modules["VirtioMmioBlock"] = spec["revision"]
        modules["IOStorageFamily"] = storage_revision
    report["module_revisions"] = modules
    if args.virtio:
        report["virtio_mmio_block_sources"] = {
            "repository": str(virtio_source),
            "revision": git(virtio_source, "rev-parse", "HEAD"),
            "source_sha256": hashlib.sha256((virtio_source / "NativeVirtioBlock.cpp").read_bytes()).hexdigest(),
            "storage_family_revision": storage_revision,
            "storage_family_path": str(storage_source),
        }
    os.environ["UNIXNU_TOOLCHAIN"] = str(prefix)
    from types import SimpleNamespace
    from unixnu.kernel import linux_env
    env = {key: value for key, value in os.environ.items() if not key.startswith(("XNU_", "UNIXNU_"))}
    env.update(linux_env(SimpleNamespace(tree=tree)))
    env.update({
        "UNIXNU_TOOLCHAIN": str(prefix), "PYTHONDONTWRITEBYTECODE": "1",
        "ZERO_AR_DATE": "1", "MAKEFLAGS": "--output-sync=target", "JOBS": "12", "UNIXNU_XNU_ARCH": "arm64",
        "UNIXNU_IOS_VERSION": "17.0", "UNIXNU_XNU_TARGET": "macosx" if args.os == "macos" else "ios",
        "UNIXNU_XNU_COMMIT": baseline,
        "UNIXNU_PATCH_LIST": "device-managed-t70pro-fork",
        "XNU_SOURCE_CORECRYPTO14": "1", "XNU_SOURCE_PTHREAD": "1",
        "XNU_ARM_DT_PLATFORM": "1", "XNU_ARMCPU": "1",
        "XNU_NATIVE_ARCH_TIMER": "1", "XNU_NATIVE_PAN": "1",
        "XNU_DT_PANIC_REGION": "1", "XNU_NATIVE_AES": "0",
        "XNU_SOURCE_MOCKFS": "0", "XNU_CRYPTO_BOOTSTRAP": "source"})
    if args.virtio:
        env.update({
            "XNU_NATIVE_VIRTIO_BLOCK": "1",
            "XNU_NATIVE_VIRTIO_BLOCK_ARCHIVE": str(tree / "build/virtio-mmio-block/libVirtioMmioBlock.a"),
            "XNU_T70_VIRTIO_IOSTORAGE_AUDIT": "1",
        })
    if args.t70_ufs:
        env["XNU_EXTRA_LD_ARCHIVES"] = "-Wl,-force_load," + str(tree / "build/t70-ufs/libNativeT70UfsBlock.a")
    if args.os == "macos":
        env["DARWIN_TRIPLE"] = "arm64-apple-macos14.0"
        env["XNU_SDKROOT"] = "macosx"
    report["selection"] = {key: value for key, value in env.items() if key.startswith(("XNU_", "UNIXNU_"))}
    # XNU does not track the machine-and-OS MASTER file as a make prerequisite.
    config_files = ("config/MASTER", "config/MASTER.arm64", "config/MASTER.arm64.t70pro",
                    "config/MASTER.arm64.t70pro.MacOSX", "makedefs/MakeInc.def")
    if args.virtio:
        config_files += ("iokit/KernelConfigTables.cpp",)
    configuration = {"os_abi": args.os, "generated_recipes": adaptations, "selection": report["selection"],
                     "kernel_configuration_sha256": {name: hashlib.sha256((kernel/name).read_bytes()).hexdigest()
                                                        for name in config_files}}
    if args.virtio:
        audit_path = project / "tools/xnu-build/build/storage-abi/interface-audit.json"
        qualification_path = analysis / "xnu-storage-abi-qualification.json"
        configuration["virtio_mmio_block"] = {
            "source_revision": git(virtio_source, "rev-parse", "HEAD"),
            "source_sha256": hashlib.sha256((virtio_source / "NativeVirtioBlock.cpp").read_bytes()).hexdigest(),
            "generator_sha256": hashlib.sha256((here / "Drivers/build_virtio_mmio.py").read_bytes()).hexdigest(),
            "storage_audit_sha256": hashlib.sha256(audit_path.read_bytes()).hexdigest(),
            "storage_qualification_sha256": hashlib.sha256(qualification_path.read_bytes()).hexdigest(),
            "target_conditionals_sha256": hashlib.sha256((project / "tools/xnu-build/.cache/fakeroot/usr/include/TargetConditionals.h").read_bytes()).hexdigest(),
            "xnu_prelink_revision": modules["XnuPrelink"],
            "xnu_prelink_source_sha256": hashlib.sha256((prelink_tools / "native_prelink.py").read_bytes()).hexdigest(),
        }
    if args.t70_ufs:
        driver_sources = sorted((here / "Drivers/T70Ufs").glob("*"))
        shared_root = mu / "Silicon/MediaTek/MediaTekPkg"
        driver_sources += [shared_root / "Library/UfsCoreLib" / name for name in ("MtkUfsTransport.c", "MtkUfsCore.c")]
        driver_sources += [shared_root / "Include/Library" / name for name in ("MtkUfsTransport.h", "MtkUfsCore.h")]
        driver_sources += [here / "Drivers/build_t70_ufs.py"]
        configuration["t70_ufs"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                    for path in driver_sources if path.is_file()}
    stamp = tree / "build/t70pro-configuration.json"
    objects = tree / "build/xnu"
    previous = json.loads(stamp.read_text()) if stamp.is_file() else None
    # UFS objects and its catalogue are rebuilt on every run; they only require relinking.
    changed = previous is None or (
        {key:value for key,value in previous.items() if key != "t70_ufs"} !=
        {key:value for key,value in configuration.items() if key != "t70_ufs"})
    if changed and objects.exists():
        if objects.is_symlink() or objects.resolve() != tree.resolve() / "build/xnu":
            raise RuntimeError("Refusing to clean an unexpected generated object directory")
        shutil.rmtree(objects)
    # These two archives are otherwise reused solely on file existence.
    # Invalidate only generated dependencies when the target configuration changes.
    invalidated = []
    if changed:
        library_dir = tree / ".cache/fakeroot/usr/local/lib/kernel"
        if library_dir.exists() and library_dir.resolve() != tree.resolve() / ".cache/fakeroot/usr/local/lib/kernel":
            raise RuntimeError("Refusing to clean an unexpected generated library directory")
        for name in ("libfirehose_kernel.a", "libcc_kext.a"):
            archive = library_dir / name
            if archive.is_symlink():
                raise RuntimeError("Unexpected generated library symlink preserved: " + str(archive))
            if archive.is_file():
                archive.unlink()
                invalidated.append(str(archive))
    report["invalidated_generated_libraries"] = invalidated
    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text(json.dumps(configuration, indent=2) + "\n")
    report["configuration"] = configuration
    report["configuration_rebuilt"] = changed
    if args.virtio and args.driver_only:
        base_record = project / "tools/xnu-build/build/xnu/ios-obj/DEVELOPMENT_ARM64_T70PRO/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
        helper = here / "Drivers/build_virtio_mmio.py"
        run("virtio-driver-only", [sys.executable, helper, "--project", project, "--tree", tree,
            "--source-repository", virtio_source,
            "--storage-audit", project / "tools/xnu-build/build/storage-abi/interface-audit.json",
            "--qualification", analysis / "xnu-storage-abi-qualification.json",
            "--compile-record", base_record, "--locked"], cwd=tree, env=env)
        driver_report = tree / "build/virtio-mmio-block/t70pro-build-report.json"
        report["virtio_mmio_block"] = json.loads(driver_report.read_text())
        report["status"] = "driver_archive_built_runtime_unqualified"
        print("PASS: isolated iOS VirtIO archive built; link/runtime pending", flush=True)
        raise SystemExit(0)
    record = tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_T70PRO/osfmk/DEVELOPMENT/startup.o.json"
    if not record.exists() or not (record.parents[2] / "link.filelist").is_file():
        run("kernel-bootstrap", ["bash", tree / "scripts/build-xnu-4k-baseline.sh"],
            cwd=tree, env={**env, "XNU_BOOTSTRAP": "1"}, tolerate=True)
        if not record.is_file() or not (record.parents[2] / "link.filelist").is_file():
            raise RuntimeError("Bootstrap failed before compile records and link.filelist were ready; inspect its log")
    for label, command in [
        ("corecrypto-plain", ["build-corecrypto-14.py"]),
        ("corecrypto-namespaced", ["build-corecrypto-14.py", "--namespace"]),
        ("armdt-platform", ["build-arm-driver.py", "ArmDTPlatform"]),
        ("armcpu", ["build-arm-driver.py", "ArmCpu"]),
        ("pthread", ["build-libpthread-kernel.py"]),
    ]:
        run(label, [sys.executable, tree / "scripts" / command[0], *command[1:]], cwd=tree, env=env)
    if args.virtio:
        helper = here / "Drivers/build_virtio_mmio.py"
        run("virtio-driver", [sys.executable, helper, "--project", project, "--tree", tree,
            "--source-repository", virtio_source,
            "--storage-audit", project / "tools/xnu-build/build/storage-abi/interface-audit.json",
            "--qualification", analysis / "xnu-storage-abi-qualification.json", "--locked"], cwd=tree, env=env)
        report["virtio_mmio_block"] = json.loads((tree / "build/virtio-mmio-block/t70pro-build-report.json").read_text())
    if args.t70_ufs:
        run("t70-ufs-driver", [sys.executable, here / "Drivers/build_t70_ufs.py",
            "--project", project, "--tree", tree, "--locked"], cwd=tree, env=env)
        report["t70_ufs"] = json.loads((tree / "build/t70-ufs/build-report.json").read_text())
    run("kernel-final", ["bash", tree / "scripts/build-xnu-4k-baseline.sh"], cwd=tree, env=env)
    image_name = "kernel" if args.os == "macos" else "mach"
    image = tree / f"build/xnu/ios-obj/DEVELOPMENT_ARM64_T70PRO/{image_name}.development.t70pro.unstripped"
    data = image.read_bytes()
    magic, cpu, subtype, kind = struct.unpack_from("<4I", data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 2):
        raise RuntimeError("Final image is not a plain ARM64 MH_EXECUTE kernel")
    stage = (project / ("tools/xnu-prelink-ios/" + variant_suffix[1:] + "-native/kernel.macho") if args.virtio else
             mu / f"Build/t70proPkg/RELEASE_CLANGPDB/XnuStage{os_suffix}/xnu/kernel.macho")
    stage.parent.mkdir(parents=True, exist_ok=True)
    stage.write_bytes(data)
    if stage.read_bytes() != data:
        raise RuntimeError("Staged kernel differs from the build output")
    if args.os == "macos":
        staged_profile = stage.parent.parent / "os-target.toml"
        shutil.copyfile(target_profile, staged_profile)
        report["target_profile_stage"] = str(staged_profile)
    report["image"] = {"path": str(image), "stage": str(stage), "bytes": len(data),
                       "sha256": hashlib.sha256(data).hexdigest(), "format": "ARM64 MH_EXECUTE"}
    names = subprocess.check_output(["xcrun", "nm", "-u", "-j", str(image)], env=env, text=True)
    report["undefined_symbols"] = sorted(set(names.splitlines()))
    if report["undefined_symbols"]:
        raise RuntimeError("Final kernel has unresolved symbols")
    if args.virtio:
        defined = subprocess.check_output(["xcrun", "nm", "-g", str(image)], env=env, text=True)
        symbols = {fields[-1] for line in defined.splitlines()
                   if len(fields := line.split()) >= 3 and fields[-2] not in ("U", "u")}
        required = set(report["virtio_mmio_block"]["required_symbols"])
        if not required or required - symbols:
            raise RuntimeError("Full kernel lacks required VirtIO driver symbols")
        report["virtio_mmio_block"].update(linked_into_kernel=True,
            linked_kernel_sha256=report["image"]["sha256"], linked_symbols_checked=sorted(required))
    if args.t70_ufs:
        required = set(report["t70_ufs"]["required_symbols"])
        if not required or required - symbols:
            raise RuntimeError("Full kernel lacks required T70 UFS provider/block symbols")
        report["t70_ufs"].update(linked_into_kernel=True,
            linked_kernel_sha256=report["image"]["sha256"], linked_symbols_checked=sorted(required))
    source_files = ("makedefs/MakeInc.def", "config/MASTER.arm64.t70pro",
                    "config/MASTER.arm64.t70pro.MacOSX",
                    "pexpert/pexpert/arm64/T70PRO.h", "pexpert/pexpert/arm64/board_config.h",
                    "pexpert/pexpert/arm64/Makefile", "osfmk/arm64/start.s", "osfmk/arm64/proc_reg.h",
                    "osfmk/kern/debug.c", "osfmk/kern/kalloc.c", "osfmk/kern/zalloc.c", "osfmk/arm/arm_init.c",
                    "osfmk/mach/arm/vm_param.h", "bsd/kern/bsd_init.c", "bsd/kern/kern_exec.c",
                    "osfmk/prng/prng_random.c", "bsd/kern/hvg_sysctl.c", "iokit/KernelConfigTables.cpp", "libkern/c++/OSValueObject.cpp",
                    "SETUP/kextsymboltool/kextsymboltool.c")
    report["kernel_source_sha256"] = {name: hashlib.sha256((kernel / name).read_bytes()).hexdigest()
                                       for name in source_files}
    report["builder_script_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report["toolchain_prefix"] = str(prefix)
    report["crypto_integrity_record"] = str(image.with_suffix(image.suffix + ".crypto-hmac.json"))
    if args.virtio:
        report["adt"] = json.loads((analysis / "xnu-adt-build.json").read_text())
        report["adt_reused_from_base_ios_build"] = True
    else:
        run("platform-adt", [sys.executable, here / "prepare-adt.py", "--os", args.os, "--locked"])
        report["adt"] = json.loads((analysis / f"xnu-adt{os_suffix}-build.json").read_text())
    report["bootable_adt"] = "staged_requires_successful_live_UEFI_window_allocation_and_platform_checks"
    report["compile_os_flags"] = {}
    expected = "-DXNU_TARGET_OS_OSX" if args.os == "macos" else "-DXNU_TARGET_OS_IOS"
    other = "-DXNU_TARGET_OS_IOS" if args.os == "macos" else "-DXNU_TARGET_OS_OSX"
    recipes = [tree / "build/xnu/ios-obj/DEVELOPMENT_ARM64_T70PRO" / name for name in
               ("osfmk/DEVELOPMENT/startup.o.json", "iokit/DEVELOPMENT/IOPlatformExpert.cpo.json")]
    recipes += [tree / "build" / name / "compile-command.json" for name in ("armcpu", "armdt-platform")]
    for recipe in recipes:
        record = json.loads(recipe.read_text().strip().rstrip(","))
        arguments = record["arguments"] if isinstance(record, dict) else record
        if "-mcpu=cortex-a55" not in arguments or "-march=armv8.2-a" not in arguments:
            raise RuntimeError("Native CPU baseline missing from compile recipe: " + str(recipe))
        defines = {arg[2:] for arg in arguments if arg.startswith("-D") and len(arg) > 2}
        defines.update(arguments[i+1] for i,arg in enumerate(arguments[:-1]) if arg == "-D")
        if expected[2:] not in defines or other[2:] in defines:
            raise RuntimeError("Mixed/wrong kernel OS flags: " + str(recipe))
        if args.os == "macos" and {"CONFIG_JETSAM", "CONFIG_FREEZE", "XNU_PLATFORM_iPhoneOS", "SCHED_HYGIENE_DEBUG"} & defines:
            raise RuntimeError("Unsupported phone/private-debug configuration in macOS kernel: " + str(recipe))
        report["compile_os_flags"][str(recipe)] = expected
    report["status"] = "driver_linked_kernel_built_runtime_unqualified" if args.virtio else "passed"
    if args.virtio:
        report["device_boot"] = "not_run"
        report["runtime_qualified"] = False
        print("PASS: isolated T70PRO VirtIO-linked iOS kernel built; runtime verification pending", flush=True)
    else:
        print("PASS: T70PRO full kernel and runtime allocation ADT staged", flush=True)
except Exception as error:
    report["status"] = "failed"
    report["error"] = str(error)
    raise
finally:
    report["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path, flush=True)
