#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Prepare and verify an EDK2 boot.img for RAM-only LK fastboot boot."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

PKG = Path(__file__).resolve().parents[1]
REPO = PKG.parents[2]
ROOT = REPO.parent
TOOLS = Path(__file__).resolve().parent
TESTS = PKG / "Tests"

BUILDER_VM = "builder"
BUILDER_ROOT = Path("/home/builder/teclast-t70pro")
BUILDER_PYTHON = BUILDER_ROOT / ".venv/bin/python"
MAC_ORB_PREFIX = Path("/Users/you/OrbStack/builder/home/builder")


class PrepareError(RuntimeError):
    """Raised when build, bridge, or offline verification inputs fail validation."""


def sha256_bytes(data):
    """Return lowercase hexadecimal SHA-256 of bytes."""
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    """Return lowercase hexadecimal SHA-256 of a file."""
    return sha256_bytes(Path(path).read_bytes())


def read_json(path):
    """Parse JSON file into a Python object."""
    return json.loads(Path(path).read_text())


def write_json(path, data):
    """Atomically write formatted JSON with trailing newline."""
    target = Path(path)
    tmp = target.with_name(f".{target.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n")
    os.replace(tmp, target)


def to_builder_path(path):
    """Translate macOS host or OrbStack-mounted paths into paths inside builder."""
    resolved = Path(path).resolve()
    text = str(resolved)
    if text == str(MAC_ORB_PREFIX) or text.startswith(str(MAC_ORB_PREFIX) + "/"):
        return "/home/builder" + text[len(str(MAC_ORB_PREFIX)):]
    if text.startswith("/home/builder/") or text.startswith("/mnt/mac/"):
        return text
    return "/mnt/mac" + text


def translate_argv_for_builder(argv):
    """Translate path arguments for delegating prepare-ram-boot.py into builder."""
    out = []
    path_flags = {"--build-dir", "--image", "--report", "--preloader", "--backup"}
    expect_path = False
    for arg in argv:
        if expect_path:
            out.append(to_builder_path(arg))
            expect_path = False
        elif arg in path_flags:
            out.append(arg)
            expect_path = True
        elif any(arg.startswith(flag + "=") for flag in path_flags):
            flag, val = arg.split("=", 1)
            out.append(f"{flag}={to_builder_path(val)}")
        else:
            out.append(arg)
    return out


def delegate_to_builder():
    """Re-execute this script inside OrbStack builder when invoked on macOS."""
    script = to_builder_path(Path(__file__).resolve())
    cmd = ["orb", "-m", BUILDER_VM, str(BUILDER_PYTHON), "-B", script]
    cmd.extend(translate_argv_for_builder(sys.argv[1:]))
    result = subprocess.run(cmd)
    raise SystemExit(result.returncode)


def resolve_latest_build_dir(repo_dir=REPO):
    """Pick the newest complete RELEASE_CLANGPDB build with a trial boot image."""
    build_root = repo_dir / "Build"
    candidates = []
    for release_dir in sorted(build_root.glob("*/RELEASE_CLANGPDB")):
        boot = release_dir / "T70Pro-UEFI-Trial-boot.img"
        image = release_dir / "T70Pro-UEFI-Trial.Image"
        fd = release_dir / "FV/SILICIUM_UEFI.fd"
        secmap = release_dir / "AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map"
        if all(p.is_file() for p in (boot, image, fd, secmap)):
            candidates.append((boot.stat().st_mtime, release_dir.parent.name == "t70proPkg", release_dir))
    if not candidates:
        raise PrepareError("No built T70Pro-UEFI-Trial-boot.img found under Build/*/RELEASE_CLANGPDB; pass --build or --build-dir.")
    candidates.sort(reverse=True)
    return candidates[0][2]


def find_matching_build_dir_for_boot(boot_path, repo_dir=REPO):
    """Locate a complete Build/*/RELEASE_CLANGPDB directory matching boot_path by SHA-256."""
    target = Path(boot_path)
    if not target.is_file():
        return None
    target_size = target.stat().st_size
    build_root = repo_dir / "Build"
    candidates = []
    for release_dir in build_root.glob("*/RELEASE_CLANGPDB"):
        boot = release_dir / "T70Pro-UEFI-Trial-boot.img"
        image = release_dir / "T70Pro-UEFI-Trial.Image"
        fd = release_dir / "FV/SILICIUM_UEFI.fd"
        secmap = release_dir / "AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map"
        if all(p.is_file() for p in (boot, image, fd, secmap)) and boot.stat().st_size == target_size:
            candidates.append((boot.stat().st_mtime, release_dir))
    if not candidates:
        return None
    digest = sha256_file(target)
    candidates.sort(reverse=True)
    for _, release_dir in candidates:
        if sha256_file(release_dir / "T70Pro-UEFI-Trial-boot.img") == digest:
            return release_dir
    return None


def run_build(build_output=None):
    """Build firmware-trial inside builder via build.sh."""
    env = os.environ.copy()
    if build_output:
        env["T70_BUILD_OUTPUT"] = build_output
    cmd = ["bash", str(PKG / "build.sh"), "firmware-trial"]
    result = subprocess.run(cmd, cwd=REPO, env=env)
    if result.returncode != 0:
        raise PrepareError(f"build.sh firmware-trial failed with exit code {result.returncode}")
    leaf = build_output or "Build/t70proPkg"
    return REPO / leaf / "RELEASE_CLANGPDB"


def resolve_inputs(args):
    """Resolve build artifacts (boot.img, raw Image, FD, Sec.dll.map) cleanly."""
    if args.build:
        build_dir = run_build(args.build_output)
    elif args.image is not None:
        img = Path(args.image).resolve()
        if not img.is_file():
            raise PrepareError(f"Image not found: {img}")
        if img.name.endswith(".boot.img") and img.with_suffix("").with_suffix(".proofs").is_dir():
            return {"mode": "prepared_archive", "boot": img, "proofs": img.with_suffix("").with_suffix(".proofs")}
        if args.build_dir is not None:
            build_dir = Path(args.build_dir).resolve()
        elif (img.parent / "FV/SILICIUM_UEFI.fd").is_file():
            build_dir = img.parent
        else:
            matched = find_matching_build_dir_for_boot(img)
            if matched is not None:
                build_dir = matched
            else:
                digest = sha256_file(img)
                archived_boot = ROOT / "analysis/lk-ram-boot/prepared-images" / f"{digest}.boot.img"
                archived_proofs = ROOT / "analysis/lk-ram-boot/prepared-images" / f"{digest}.proofs"
                if archived_boot.is_file() and archived_proofs.is_dir():
                    return {"mode": "prepared_archive", "boot": archived_boot, "proofs": archived_proofs}
                raise PrepareError(
                    f"Missing sibling FV/SILICIUM_UEFI.fd next to {img} and no matching build under "
                    "Build/*/RELEASE_CLANGPDB; pass --build-dir or a prepared image."
                )
        boot = img
        if not (build_dir / "FV/SILICIUM_UEFI.fd").is_file():
            raise PrepareError(f"Missing FV/SILICIUM_UEFI.fd under {build_dir}; pass --build-dir or a prepared image.")
        artifacts = {
            "mode": "build_dir",
            "build_dir": build_dir,
            "boot": boot,
            "image": build_dir / "T70Pro-UEFI-Trial.Image",
            "fd": build_dir / "FV/SILICIUM_UEFI.fd",
            "secmap": build_dir / "AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map",
        }
        missing = [str(p) for k, p in artifacts.items() if k not in ("mode", "build_dir") and not p.is_file()]
        if missing:
            raise PrepareError("Missing firmware build outputs: " + ", ".join(missing))
        return artifacts
    elif args.build_dir is not None:
        build_dir = Path(args.build_dir).resolve()
    else:
        build_dir = resolve_latest_build_dir()

    artifacts = {
        "mode": "build_dir",
        "build_dir": build_dir,
        "boot": build_dir / "T70Pro-UEFI-Trial-boot.img",
        "image": build_dir / "T70Pro-UEFI-Trial.Image",
        "fd": build_dir / "FV/SILICIUM_UEFI.fd",
        "secmap": build_dir / "AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map",
    }
    missing = [str(p) for k, p in artifacts.items() if k not in ("mode", "build_dir") and not p.is_file()]
    if missing:
        raise PrepareError("Missing firmware build outputs: " + ", ".join(missing))
    return artifacts


def load_runner_module():
    """Import run-lk-ram-boot.py helpers without modifying the runner."""
    spec = importlib.util.spec_from_file_location("run_lk_ram_boot", TOOLS / "run-lk-ram-boot.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def default_preloader_path():
    """Locate original MT6789 preloader image if needed to rebuild the bridge chain."""
    candidates = (
        Path("/mnt/mac/Users/you/.tmp/teclast-t70pro/stock-loader/preloader_tb8781p1_64_td89.bin"),
        Path("/Users/you/.tmp/teclast-t70pro/stock-loader/preloader_tb8781p1_64_td89.bin"),
    )
    for c in candidates:
        if c.is_file():
            return c
    return None


def default_backup_path():
    """Locate original device backup if needed to rebuild the bridge chain."""
    candidates = (
        Path("/mnt/mac/Users/you/.tmp/teclast-t70pro/device-backup"),
        Path("/Users/you/.tmp/teclast-t70pro/device-backup"),
    )
    for c in candidates:
        if (c / "partition-layout.json").is_file() and (c / "images/ufs-lu2.img").is_file():
            return c
    return None


def bridge_chain_valid(chain_dir):
    """Verify existing Preloader/BL2/LK bridge artifacts match current sources."""
    required = (
        "bootchain-ram-plan.json",
        "ram-edit-plan.json",
        "lk-ram-boot.bin",
        "lk-ram-boot.symbols.txt",
        "preloader-ram-boot.bin",
        "preloader-retained-probe.bin",
        "retained-probe-verification.json",
        "bootchain-verification.json",
        "verification.json",
    )
    if not all((chain_dir / name).is_file() for name in required):
        return False
    try:
        plan = read_json(chain_dir / "bootchain-ram-plan.json")
        lk_plan = read_json(chain_dir / "ram-edit-plan.json")
        chain_ver = read_json(chain_dir / "bootchain-verification.json")
        probe_ver = read_json(chain_dir / "retained-probe-verification.json")
        lk_ver = read_json(chain_dir / "verification.json")
    except Exception:
        return False

    if not (chain_ver.get("all_passed") and probe_ver.get("all_passed") and lk_ver.get("all_passed")):
        return False
    if sha256_file(chain_dir / "lk-ram-boot.bin") != plan.get("LK_payload_sha256"):
        return False
    if lk_plan.get("payload_sha256") != plan.get("LK_payload_sha256"):
        return False
    if lk_ver.get("payload_sha256") != plan.get("LK_payload_sha256"):
        return False
    if sha256_file(chain_dir / "preloader-ram-boot.bin") != plan.get("preloader_stage_sha256"):
        return False
    if chain_ver.get("preloader_stage_sha256") != plan.get("preloader_stage_sha256"):
        return False
    if chain_ver.get("BL2_bridge_sha256") != plan.get("BL2_bridge_sha256"):
        return False
    if chain_ver.get("test_source_sha256") != sha256_file(TESTS / "RamBootChainTest.py"):
        return False
    probe_src = probe_ver.get("source_sha256", {})
    if set(probe_src) != {"Tools/PreloaderRamBoot.S", "Tests/RetainedBootProbeTest.py", "Tests/RamBootChainTest.py"}:
        return False
    if any(sha256_file(PKG / rel) != digest for rel, digest in probe_src.items()):
        return False
    if sha256_file(chain_dir / "preloader-retained-probe.bin") != probe_ver.get("payload_sha256"):
        return False
    return True


def run_step(name, cmd, log_dir, json_stdout=False):
    """Run one preparation or verification subprocess and capture its log."""
    start = time.monotonic()
    str_cmd = [str(x) for x in cmd]
    with (log_dir / f"{name}.log").open("w") as log:
        result = subprocess.run(
            str_cmd,
            stdout=subprocess.PIPE if json_stdout else log,
            stderr=log,
            text=True,
            cwd=REPO,
        )
    if result.returncode != 0:
        tail = (log_dir / f"{name}.log").read_text(errors="replace")[-4000:]
        raise PrepareError(f"Step '{name}' failed (exit {result.returncode}):\n{tail}")
    payload = None
    if json_stdout:
        payload = json.loads(result.stdout)
        if payload.get("status") != "pass" or payload.get("continuous_boot", {}).get("status") != "passed":
            raise PrepareError(f"Step '{name}' verification status did not pass.")
    return {
        "name": name,
        "command": str_cmd,
        "exit_code": 0,
        "elapsed_seconds": round(time.monotonic() - start, 3),
    }, payload


def ensure_bridge_chain(chain_dir, preloader_path, backup_path, force_chain=False):
    """Ensure verified Preloader/BL2/LK RAM-boot bridge artifacts exist and match sources."""
    if not force_chain and bridge_chain_valid(chain_dir):
        return False
    preloader = Path(preloader_path) if preloader_path else default_preloader_path()
    backup = Path(backup_path) if backup_path else default_backup_path()
    if not preloader or not preloader.is_file() or not backup or not backup.is_dir():
        raise PrepareError("Bridge chain needs rebuild but stock preloader/backup was not found.")
    lk = ROOT / "analysis/lk/lk.bin"
    if not lk.is_file():
        raise PrepareError(f"Missing stock LK binary: {lk}")

    chain_dir.mkdir(parents=True, exist_ok=True)
    log_dir = chain_dir / ".rebuild-logs"
    log_dir.mkdir(exist_ok=True)
    run_step("build-lk-ram-boot", [sys.executable, "-B", TOOLS / "build-lk-ram-boot.py",
                                   "--lk", lk, "--backup", backup, "--preloader", preloader, "--out", chain_dir], log_dir)
    trial_boot = next(iter(sorted((chain_dir / "prepared-images").glob("*.boot.img"))), None)
    if trial_boot is None:
        trial_boot = resolve_latest_build_dir() / "T70Pro-UEFI-Trial-boot.img"
    run_step("lk-ram-boot-test", [
        sys.executable, "-B", TESTS / "LkRamBootTest.py",
        "--lk", lk, "--payload", chain_dir / "lk-ram-boot.bin",
        "--symbols", chain_dir / "lk-ram-boot.symbols.txt",
        "--plan-tool", TOOLS / "lk-ram-boot-plan.py",
        "--image", trial_boot,
        "--backup", backup, "--report", chain_dir / "verification.json",
    ], log_dir)
    run_step("ram-boot-chain-test", [
        sys.executable, "-B", TESTS / "RamBootChainTest.py",
        "--preloader", preloader, "--backup", backup, "--lk", lk,
        "--artifacts", chain_dir, "--report", chain_dir / "bootchain-verification.json",
    ], log_dir)
    run_step("retained-boot-probe-test", [
        sys.executable, "-B", TESTS / "RetainedBootProbeTest.py",
        "--preloader", preloader, "--payload", chain_dir / "preloader-retained-probe.bin",
        "--report", chain_dir / "retained-probe-verification.json",
    ], log_dir)
    if not bridge_chain_valid(chain_dir):
        raise PrepareError("Rebuilt bridge chain still failed validation.")
    return True


def proof_cache_valid(boot_path, raw_image_path, fd_path, proofs_dir, plan):
    """Return True if cached <digest>.proofs already passes all source and image checks."""
    if not (boot_path.is_file() and proofs_dir.is_dir()):
        return False
    required_files = (
        "execution-checks.json",
        "transport-verification.json",
        "watchdog-verification.json",
        "trial-kernel-image-verification.json",
        "trial-LK-verification.json",
        "kernel-handoff-verification.json",
        "rebuilt-firmware-verification.json",
        "bootchain-ram-plan.json",
        "preloader-ram-boot.bin",
        "preloader-retained-probe.bin",
        "retained-probe-verification.json",
        "bootchain-verification.json",
        "verification.json",
        "Sec.map",
    )
    if not all((proofs_dir / name).is_file() for name in required_files):
        return False
    try:
        runner = load_runner_module()
        if runner.edk2_candidate(boot_path, proofs_dir, plan) != boot_path.read_bytes():
            return False
        decode = read_json(proofs_dir / "trial-kernel-image-verification.json")
        if raw_image_path is not None and decode.get("raw_BootShim_image_sha256") != sha256_file(raw_image_path):
            return False
        if fd_path is not None and decode.get("FD_sha256") != sha256_file(fd_path):
            return False
        return True
    except Exception:
        return False


def generate_proofs(artifacts, chain_dir, digest, force=False):
    """Run offline CPU/Emulation verification suite and publish cached prepared-images entry."""
    archive = chain_dir / "prepared-images"
    archive.mkdir(parents=True, exist_ok=True)
    target_boot = archive / f"{digest}.boot.img"
    target_proofs = archive / f"{digest}.proofs"
    plan = read_json(chain_dir / "bootchain-ram-plan.json")

    if not force and proof_cache_valid(target_boot, artifacts.get("image"), artifacts.get("fd"), target_proofs, plan):
        return target_boot, target_proofs, True

    if artifacts["mode"] == "prepared_archive":
        raise PrepareError(f"Prepared archive failed verification and no build inputs were supplied: {target_boot}")

    fixture_root = ROOT / "analysis/ram-physical-boot-20261117"
    header_fixture = fixture_root / "stock-boot-header-inputs"
    preloader_fixture = fixture_root / "preloader-GFH-offset-fixture.bin"
    dtb = ROOT / "analysis/stock-fdt/effective.dtb"
    lk = ROOT / "analysis/lk/lk.bin"
    for req in (header_fixture / "partition-layout.json", header_fixture / "images/ufs-lu2.img", preloader_fixture, dtb, lk):
        if not req.is_file():
            raise PrepareError(f"Required offline verification fixture missing: {req}")

    fd = artifacts["fd"]
    image = artifacts["image"]
    boot = artifacts["boot"]
    secmap = artifacts["secmap"]
    payload = chain_dir / "lk-ram-boot.bin"
    symbols = chain_dir / "lk-ram-boot.symbols.txt"

    inputs = [fd, image, boot, PKG / "BootShim.S", PKG / "verify-boot.py", TOOLS / "run-lk-ram-boot.py",
              TESTS / "RamBootWatchdogTest.py", TESTS / "LkRamBootTest.py", TESTS / "LkKernelHandoffTest.py"]
    before = {str(p): sha256_file(p) for p in inputs}

    with tempfile.TemporaryDirectory(prefix=f".staging-{digest[:12]}-", dir=archive) as tmp_str:
        out = Path(tmp_str)
        results = []
        results.append(run_step("transport", [sys.executable, "-B", TESTS / "LkRamBootTransportTest.py",
                                              "--report", out / "transport-verification.json"], out)[0])

        normal_obj = out / "BootShim-normal.o"
        normal_img = out / "T70Pro-Normal-Shim-Fixture.Image"
        results.append(run_step("normal-shim-compile", [
            "clang", "--target=aarch64-none-elf", "-c", PKG / "BootShim.S",
            "-DT70_RAM_TRIAL=0", f'-DFD_PATH="{fd}"', "-o", normal_obj,
        ], out)[0])
        results.append(run_step("normal-shim-link", [
            "ld.lld", "--image-base=0", "-Ttext=0", "--entry=_Head", "--oformat=binary", normal_obj, "-o", normal_img,
        ], out)[0])

        common = ["--lk", lk, "--payload", payload, "--symbols", symbols,
                  "--plan-tool", TOOLS / "lk-ram-boot-plan.py", "--image", boot, "--backup", header_fixture]
        results.append(run_step("watchdog", [
            sys.executable, "-B", TESTS / "RamBootWatchdogTest.py",
            "--normal-image", normal_img, "--trial-image", image,
            "--fd", fd, "--sec-map", secmap, "--report", out / "watchdog-verification.json",
        ], out)[0])
        results.append(run_step("lk-decoder", [
            sys.executable, "-B", TESTS / "LkKernelImageTest.py",
            "--lk", lk, "--boot-image", boot, "--raw-image", image,
            "--fd", fd, "--report", out / "trial-kernel-image-verification.json",
        ], out)[0])
        results.append(run_step("lk-consumer", [
            sys.executable, "-B", TESTS / "LkRamBootTest.py", *common,
            "--report", out / "trial-LK-verification.json",
        ], out)[0])
        results.append(run_step("lk-handoff", [
            sys.executable, "-B", TESTS / "LkKernelHandoffTest.py", *common,
            "--consumer-test", TESTS / "LkRamBootTest.py",
            "--report", out / "kernel-handoff-verification.json",
        ], out)[0])
        step_fw, fw_json = run_step("trial-firmware", [
            sys.executable, "-B", PKG / "verify-boot.py",
            "--image", image, "--fd", fd, "--boot", boot, "--ram-trial",
            "--python-dtb", "--dtb", dtb, "--preloader", preloader_fixture,
        ], out, json_stdout=True)
        results.append(step_fw)
        write_json(out / "rebuilt-firmware-verification.json", fw_json)

        if before != {str(p): sha256_file(p) for p in inputs}:
            raise PrepareError("Firmware or test inputs changed while verification was running.")

        for name in ("bootchain-ram-plan.json", "preloader-ram-boot.bin", "preloader-retained-probe.bin",
                     "retained-probe-verification.json", "bootchain-verification.json", "verification.json"):
            shutil.copy2(chain_dir / name, out / name)
        shutil.copy2(secmap, out / "Sec.map")

        runner = load_runner_module()
        if runner.edk2_candidate(boot, out, plan) != boot.read_bytes():
            raise PrepareError("Prepared proofs did not satisfy edk2_candidate() validation.")

        exec_report = {
            "status": "passed",
            "results": results,
            "inputs_sha256": before,
            "inputs_unchanged": True,
            "candidate_gate": "passed",
            "trial_boot_sha256": digest,
            "trial_boot_bytes": boot.stat().st_size,
            "raw_image_sha256": sha256_file(image),
            "FD_sha256": sha256_file(fd),
            "candidate_path": str(target_boot),
            "proofs_path": str(target_proofs),
            "device_access": False,
            "storage_write_commands": [],
            "stock_inputs": "Bounded 4 KiB stock headers + preloader GFH fixture; no full UFS backup opened.",
        }
        write_json(out / "execution-checks.json", exec_report)
        write_json(out / "steps.json", results)

        boot_tmp = archive / f".tmp-{digest[:12]}.boot.img"
        shutil.copy2(boot, boot_tmp)
        os.replace(boot_tmp, target_boot)

        proofs_staging = archive / f".tmp-{digest[:12]}.proofs"
        if proofs_staging.exists():
            shutil.rmtree(proofs_staging)
        shutil.copytree(out, proofs_staging)
        os.chmod(proofs_staging, 0o755)
        if target_proofs.exists():
            stale = archive / f".stale-{digest[:12]}-{os.getpid()}.proofs"
            os.replace(target_proofs, stale)
            shutil.rmtree(stale, ignore_errors=True)
        os.replace(proofs_staging, target_proofs)
        try:
            dfd = os.open(archive, os.O_RDONLY)
            try:
                os.fsync(dfd)
            finally:
                os.close(dfd)
        except OSError:
            pass

    return target_boot, target_proofs, False


def prepare(args):
    """Run bridge checks, optional build, and EDK2 proof preparation."""
    if sys.platform == "darwin" and not args.local_no_orb:
        delegate_to_builder()

    chain_dir = ROOT / "analysis/lk-ram-boot"
    bridge_rebuilt = ensure_bridge_chain(chain_dir, args.preloader, args.backup, force_chain=args.rebuild_chain)
    artifacts = resolve_inputs(args)
    boot = artifacts["boot"]
    digest = sha256_file(boot)

    if artifacts["mode"] == "prepared_archive":
        plan = read_json(chain_dir / "bootchain-ram-plan.json")
        if not proof_cache_valid(boot, None, None, artifacts["proofs"], plan):
            raise PrepareError(f"Prepared image proofs are stale or invalid: {artifacts['proofs']}")
        target_boot, target_proofs, cached = boot, artifacts["proofs"], True
    else:
        target_boot, target_proofs, cached = generate_proofs(artifacts, chain_dir, digest, force=args.force)

    summary = {
        "status": "passed",
        "cached": cached,
        "bridge_rebuilt": bridge_rebuilt,
        "build_dir": str(artifacts.get("build_dir")) if "build_dir" in artifacts else None,
        "trial_boot_sha256": digest,
        "trial_boot_bytes": target_boot.stat().st_size,
        "candidate_path": str(target_boot),
        "proofs_path": str(target_proofs),
        "interactive_watchdog_disabled": True,
        "device_access": False,
        "storage_writes": [],
    }
    if args.report:
        write_json(args.report, summary)
    print(json.dumps(summary, indent=2), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Build firmware-trial via build.sh before preparing.")
    parser.add_argument("--build-output", help="Custom Build/<name> directory when --build is used.")
    parser.add_argument("--build-dir", type=Path, help="Existing RELEASE_CLANGPDB build output directory.")
    parser.add_argument("--image", type=Path, help="Specific T70Pro-UEFI-Trial-boot.img (or archived prepared image).")
    parser.add_argument("--force", action="store_true", help="Re-run offline verification even if cached proofs match.")
    parser.add_argument("--rebuild-chain", action="store_true", help="Rebuild and re-verify Preloader/BL2/LK RAM bridges.")
    parser.add_argument("--preloader", type=Path, help="Override stock preloader path when rebuilding bridges.")
    parser.add_argument("--backup", type=Path, help="Override backup path when rebuilding bridges.")
    parser.add_argument("--report", type=Path, help="Optional JSON output report path.")
    parser.add_argument("--local-no-orb", action="store_true", help="Internal flag: do not auto-delegate to OrbStack.")
    args = parser.parse_args()
    try:
        prepare(args)
        return 0
    except PrepareError as error:
        print(f"prepare-ram-boot error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
