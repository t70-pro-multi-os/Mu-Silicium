#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""End-to-end automated fastboot-boot launcher for Teclast T70 Pro via RAM-patched LK."""
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

TOOLS = Path(__file__).resolve().parent
PKG = TOOLS.parent
REPO = PKG.parents[2]
ROOT = REPO.parent
CHAIN = ROOT / "analysis/lk-ram-boot"
PRELOADER_SHA = "cc42d1aec1f3dd9a94cfd9147ab5c1f55e592d1aa17ebbe7a9d4c1a49e9d436e"
DEFAULT_MAX_IMAGE_BYTES = 0x200000


class FastbootRamBootError(RuntimeError):
    """Raised when preflight, verification, Preloader bridge, or fastboot boot fails."""


def sha256_bytes(data):
    """Return lowercase hexadecimal SHA-256 of bytes."""
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    """Return lowercase hexadecimal SHA-256 of a file."""
    return sha256_bytes(Path(path).read_bytes())


def read_json(path):
    """Parse JSON file into a Python object."""
    return json.loads(Path(path).read_text())


def write_json(path, value):
    """Atomically write formatted JSON with a trailing newline."""
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(f".{target.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(value, indent=2) + "\n")
    os.replace(tmp, target)


def from_builder_path(path_str, mac_orb_prefix):
    """Translate OrbStack builder container paths back to macOS host paths."""
    text = str(path_str)
    if sys.platform != "darwin":
        return Path(text)
    if text.startswith("/mnt/mac/"):
        return Path(text[len("/mnt/mac"):])
    if text == "/home/builder" or text.startswith("/home/builder/"):
        rel = text[len("/home/builder"):].lstrip("/")
        return Path(mac_orb_prefix) / rel
    return Path(text)


def wait_for_prepared_files(boot_path, proofs_path, timeout=5.0):
    """Wait briefly for VM-to-host mount propagation of newly published artifacts."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            with contextlib.suppress(OSError):
                os.listdir(boot_path.parent)
            if boot_path.is_file() and proofs_path.is_dir() and (proofs_path / "execution-checks.json").is_file():
                return
        except OSError:
            pass
        if time.monotonic() >= deadline:
            return
        time.sleep(0.1)


def load_module(name, path):
    """Load a sibling module by path without bytecode pollution."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_runner():
    """Import run-lk-ram-boot.py without altering its verified source hash."""
    return load_module("run_lk_ram_boot", TOOLS / "run-lk-ram-boot.py")


def load_prepare():
    """Import prepare-ram-boot.py helpers."""
    return load_module("prepare_ram_boot", TOOLS / "prepare-ram-boot.py")


def validate_v4_image(data, max_bytes=DEFAULT_MAX_IMAGE_BYTES):
    """Verify the exact Android boot header v4 bounds enforced by LkRamBoot.S."""
    if not 4096 <= len(data) <= max_bytes:
        raise FastbootRamBootError(f"Boot image length {len(data)} outside allowed bounds [4096, {max_bytes}].")
    if data[:8] != b"ANDROID!":
        raise FastbootRamBootError("Boot image missing ANDROID! magic.")
    kernel_size, ramdisk_size, _os_version, header_size = struct.unpack_from("<4I", data, 8)
    header_version = struct.unpack_from("<I", data, 40)[0]
    signature_size = struct.unpack_from("<I", data, 1580)[0]
    if header_version != 4 or header_size != 1584:
        raise FastbootRamBootError(f"Unsupported Android boot header v{header_version} (size={header_size}); expected v4/1584.")
    if ramdisk_size != 0:
        raise FastbootRamBootError(f"Ramdisk size {ramdisk_size} is non-zero; LkRamBoot.S supports kernel-only v4 images.")
    if kernel_size < 64:
        raise FastbootRamBootError("Kernel size below minimum 64 bytes.")
    aligned_kernel = (kernel_size + 4095) & ~4095
    if 4096 + aligned_kernel + signature_size > len(data):
        raise FastbootRamBootError("Boot image truncated relative to v4 kernel + signature lengths.")
    return {"bytes": len(data), "kernel_size": kernel_size, "signature_size": signature_size, "sha256": sha256_bytes(data)}


def first_existing(candidates):
    """Return the first existing Path among candidates or None."""
    for item in candidates:
        if item and Path(item).exists():
            return Path(item)
    return None


def default_local_tmp_roots():
    """Return candidate macOS or OrbStack-mounted .tmp/teclast-t70pro roots."""
    return (
        Path.home() / ".tmp/teclast-t70pro",
        Path("/Users/you/.tmp/teclast-t70pro"),
        Path("/mnt/mac/Users/you/.tmp/teclast-t70pro"),
    )


def resolve_runtime_paths(args):
    """Discover preloader, DA loader, mtkclient vendor, adb, and report directory cleanly."""
    tmp_roots = default_local_tmp_roots()
    preloader = first_existing([
        args.preloader,
        os.environ.get("T70_PRELOADER"),
        *(r / "stock-loader/preloader_tb8781p1_64_td89.bin" for r in tmp_roots),
    ])
    loader = first_existing([
        args.loader,
        os.environ.get("T70_DA_LOADER"),
        *(r / "stock-loader/DA_BR.bin" for r in tmp_roots),
        *(r / "device-backup/recovery-kit/vendor/DA_BR.bin" for r in tmp_roots),
    ])
    vendor = first_existing([
        args.vendor,
        os.environ.get("T70_MTKCLIENT_VENDOR"),
        *(r / "device-backup/recovery-kit/vendor" for r in tmp_roots),
    ])
    adb = first_existing([args.adb, "/opt/homebrew/bin/adb", shutil.which("adb")])
    report_dir = None
    if os.environ.get("T70_RUN_LOG_DIR"):
        report_dir = Path(os.environ["T70_RUN_LOG_DIR"])
    else:
        for r in tmp_roots:
            if (r / "monitor").is_dir():
                report_dir = r / "monitor"
                break
        if report_dir is None:
            report_dir = CHAIN / "runs"
    return {
        "preloader": preloader,
        "loader": loader,
        "vendor": vendor,
        "adb": adb,
        "report_dir": report_dir,
    }


def latest_return_report():
    """Locate the most recent observed Android return report without requiring CLI wiring."""
    candidates = []
    for root in default_local_tmp_roots():
        mon = root / "monitor"
        if mon.is_dir():
            candidates.extend(mon.glob("*-return-observed.json"))
    if (CHAIN / "device-test-130-return-observed.json").is_file():
        candidates.append(CHAIN / "device-test-130-return-observed.json")
    candidates = [p for p in candidates if p.is_file()]
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def adb_select_device(adb_path, serial=None, *, runner_subprocess=subprocess):
    """Select one authorized T70 Pro ADB device without leaking raw serial numbers."""
    if not adb_path:
        return None
    result = runner_subprocess.run([str(adb_path), "devices", "-l"], capture_output=True, text=True, timeout=15)
    if result.returncode != 0:
        raise FastbootRamBootError("ADB_device_inventory_failed")
    devices = []
    t70_devices = []
    for line in result.stdout.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 2 and fields[1] == "device":
            devices.append(fields[0])
            if any(tok in ("model:T70Pro_ROW", "device:T70Pro") for tok in fields[2:]):
                t70_devices.append(fields[0])
    if serial is not None:
        if devices.count(serial) != 1:
            raise FastbootRamBootError("selected_ADB_device_not_ready")
        return serial
    if len(devices) == 1:
        return devices[0]
    if len(t70_devices) == 1:
        return t70_devices[0]
    if len(devices) > 1:
        raise FastbootRamBootError("multiple_ADB_devices_specify_serial")
    return None


def send_fastboot_command(write, read, command, runner):
    """Execute one allowlisted USB fastboot command and drain up to 64 INFO replies."""
    allowed = runner.FASTBOOT_COMMANDS | {"reboot-bootloader"}
    if command not in allowed:
        raise FastbootRamBootError(f"command_outside_RAM_boot_allowlist:{command}")
    data = command.encode("ascii")
    if write(data) != len(data):
        raise FastbootRamBootError("fastboot_write_failed")
    for _ in range(64):
        reply = bytes(read(64))
        if not 4 <= len(reply) <= 64:
            raise FastbootRamBootError("invalid_fastboot_reply")
        token = reply[:4]
        if token == b"INFO":
            continue
        if token not in (b"OKAY", b"FAIL"):
            raise FastbootRamBootError("unexpected_fastboot_token")
        return token, reply[4:]
    raise FastbootRamBootError("too_many_fastboot_information_replies")


def find_preloader(backend, *, usb_core_mod=None):
    """Locate a unique MediaTek Preloader (0e8d:2000) USB interface if present."""
    if usb_core_mod is None:
        import usb.core as usb_core_mod
    matches = list(usb_core_mod.find(find_all=True, idVendor=0x0E8D, idProduct=0x2000, backend=backend))
    if len(matches) > 1:
        raise FastbootRamBootError("multiple_preloader_interfaces")
    return matches[0] if matches else None


def resolve_boot_target(args, runner):
    """Resolve and verify the boot image + offline proofs (hitting SHA cache in <0.1s)."""
    prep = load_prepare()
    plan_path = CHAIN / "bootchain-ram-plan.json"
    if args.unverified_v4_image:
        if args.image is None:
            raise FastbootRamBootError("--unverified-v4-image requires --image <boot.img>.")
        img_path = Path(args.image).resolve()
        if not img_path.is_file():
            raise FastbootRamBootError(f"Boot image not found: {img_path}")
        payload = img_path.read_bytes()
        meta = validate_v4_image(payload, max_bytes=args.max_image_bytes)
        if not (CHAIN / "preloader-ram-boot.bin").is_file() or not plan_path.is_file():
            raise FastbootRamBootError("Bridge artifacts missing in analysis/lk-ram-boot; run prepare-ram-boot.py first.")
        return {
            "verified_edk2": False,
            "boot_path": str(img_path),
            "proofs_path": None,
            "payload": payload,
            "sha256": meta["sha256"],
            "bytes": meta["bytes"],
            "interactive": True,
            "cached": True,
        }

    # Fast local cache check before calling into OrbStack builder.
    if not args.build and not args.force_prepare and not args.rebuild_chain and plan_path.is_file():
        plan = read_json(plan_path)
        candidate_boot = None
        candidate_raw = None
        candidate_fd = None
        candidate_proofs = None
        if args.image is not None:
            img = Path(args.image).resolve()
            if not img.is_file():
                raise FastbootRamBootError(f"Image not found: {img}")
            candidate_boot = img
            if img.name.endswith(".boot.img") and img.with_suffix("").with_suffix(".proofs").is_dir():
                candidate_proofs = img.with_suffix("").with_suffix(".proofs")
            else:
                if args.build_dir:
                    bdir = Path(args.build_dir).resolve()
                elif (img.parent / "FV/SILICIUM_UEFI.fd").is_file():
                    bdir = img.parent
                else:
                    bdir = prep.find_matching_build_dir_for_boot(img)
                if bdir is not None:
                    candidate_raw = bdir / "T70Pro-UEFI-Trial.Image"
                    candidate_fd = bdir / "FV/SILICIUM_UEFI.fd"
        else:
            bdir = Path(args.build_dir).resolve() if args.build_dir else prep.resolve_latest_build_dir()
            candidate_boot = bdir / "T70Pro-UEFI-Trial-boot.img"
            candidate_raw = bdir / "T70Pro-UEFI-Trial.Image"
            candidate_fd = bdir / "FV/SILICIUM_UEFI.fd"
        if candidate_boot is not None and candidate_boot.is_file():
            digest = sha256_file(candidate_boot)
            cached_boot = candidate_boot if candidate_proofs is not None else (CHAIN / "prepared-images" / f"{digest}.boot.img")
            cached_proofs = candidate_proofs or (CHAIN / "prepared-images" / f"{digest}.proofs")
            raw_arg = candidate_raw if (candidate_raw and candidate_raw.is_file()) else None
            fd_arg = candidate_fd if (candidate_fd and candidate_fd.is_file()) else None
            if prep.proof_cache_valid(cached_boot, raw_arg, fd_arg, cached_proofs, plan):
                payload = cached_boot.read_bytes()
                validate_v4_image(payload, max_bytes=args.max_image_bytes)
                return {
                    "verified_edk2": True,
                    "boot_path": str(cached_boot),
                    "proofs_path": str(cached_proofs),
                    "payload": payload,
                    "sha256": digest,
                    "bytes": len(payload),
                    "interactive": True,
                    "cached": True,
                }

    # Cache miss or explicit --build / --force-prepare: run prepare-ram-boot.py (delegates to builder on macOS).
    cmd = [sys.executable, "-B", str(TOOLS / "prepare-ram-boot.py")]
    if args.build:
        cmd.append("--build")
        if args.build_output:
            cmd.extend(["--build-output", args.build_output])
    elif args.build_dir:
        cmd.extend(["--build-dir", str(Path(args.build_dir).resolve())])
    if args.image and not args.build:
        cmd.extend(["--image", str(Path(args.image).resolve())])
    if args.force_prepare:
        cmd.append("--force")
    if args.rebuild_chain:
        cmd.append("--rebuild-chain")
    if args.preloader:
        cmd.extend(["--preloader", str(args.preloader)])
    if sys.platform != "darwin":
        cmd.append("--local-no-orb")

    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise FastbootRamBootError(f"prepare-ram-boot failed:\n{res.stderr or res.stdout}")
    summary = json.loads(res.stdout)
    boot_path = from_builder_path(summary["candidate_path"], prep.MAC_ORB_PREFIX)
    proofs_path = from_builder_path(summary["proofs_path"], prep.MAC_ORB_PREFIX)
    wait_for_prepared_files(boot_path, proofs_path)
    payload = boot_path.read_bytes()
    validate_v4_image(payload, max_bytes=args.max_image_bytes)
    plan = read_json(plan_path)
    if runner.edk2_candidate(boot_path, proofs_path, plan) != payload:
        raise FastbootRamBootError("Prepared EDK2 artifact failed post-prepare verification.")
    return {
        "verified_edk2": True,
        "boot_path": str(boot_path),
        "proofs_path": str(proofs_path),
        "payload": payload,
        "sha256": sha256_bytes(payload),
        "bytes": len(payload),
        "interactive": True,
        "cached": bool(summary.get("cached")),
    }


def query_fastboot_state(fb_device, ep, runner, *, usb_util_mod=None):
    """Read-only probe of current USB fastboot interface (stock vs RB05 RAM-patched)."""
    if usb_util_mod is None:
        import usb.util as usb_util_mod
    usb_util_mod.claim_interface(fb_device, ep[0])
    try:
        write = lambda data: fb_device.write(ep[1], data, timeout=3000)
        read = lambda size: fb_device.read(ep[2], size, timeout=3000)
        info = {}
        for var in ("product", "unlocked", "current-slot", "is-userspace"):
            st, val = send_fastboot_command(write, read, f"getvar:{var}", runner)
            info[var] = (st == b"OKAY", val.decode("ascii", errors="replace"))
        ver_reply = send_fastboot_command(write, read, "getvar:version-bootloader", runner)
        trace = runner.bootloader_trace(ver_reply)
        info["version_trace"] = trace
        return info
    finally:
        usb_util_mod.dispose_resources(fb_device)


def probe_device_state(paths, args, runner):
    """Inspect connected USB Preloader / Fastboot / ADB states read-only."""
    import usb.core
    import usb.util
    import usb.backend.libusb1

    backend = usb.backend.libusb1.get_backend()
    if backend is None:
        raise FastbootRamBootError("libusb_unavailable")

    fb_found = runner.find_fastboot(backend)
    fastboot_info = None
    if fb_found is not None:
        fastboot_info = query_fastboot_state(fb_found[0], fb_found[1], runner, usb_util_mod=usb.util)

    preloader_dev = find_preloader(backend, usb_core_mod=usb.core)
    adb_dev = None
    adb_snap = None
    if paths["adb"]:
        adb_dev = adb_select_device(paths["adb"], args.serial)
        if adb_dev:
            adb_snap = runner.android_snapshot(paths["adb"], adb_dev)

    if fastboot_info is not None:
        detected = "fastboot_patched" if fastboot_info["version_trace"]["observed"] else "fastboot_stock"
    elif preloader_dev is not None:
        detected = "preloader"
    elif adb_dev is not None:
        detected = "adb"
    else:
        detected = "none"

    return {
        "detected": detected,
        "fastboot": fastboot_info,
        "preloader_present": preloader_dev is not None,
        "adb_present": adb_dev is not None,
        "adb_serial_sha256": sha256_bytes(adb_dev.encode())[:16] if adb_dev else None,
        "adb_snapshot": adb_snap,
    }


def wait_for_fastboot(find_fb_fn, seconds, *, clock=time.monotonic, sleeper=time.sleep):
    """Poll until the USB fastboot interface appears or deadline expires."""
    deadline = clock() + seconds
    while clock() < deadline:
        found = find_fb_fn()
        if found is not None:
            return found
        sleeper(0.2)
    raise FastbootRamBootError("fastboot_did_not_appear")


def validate_adb_preflight(snapshot, *, require_daily_services=False, runner=None):
    """Enforce unlocked A-slot Android invariants before requesting reboot bootloader."""
    if not snapshot.get("boot_completed"):
        raise FastbootRamBootError("Android_boot_not_completed")
    if snapshot.get("slot") != "_a" or snapshot.get("flash_locked") != "0" or snapshot.get("vbmeta_state") != "unlocked":
        raise FastbootRamBootError("Android_slot_or_lock_state_changed")
    if require_daily_services and runner is not None:
        services = dict(snapshot, user_running_unlocked=True, user_download_directory_accessible=True)
        runner.preflight_ok({"stock_return_verified": True}, services)


def verify_fastboot_vars(command_fn, *, require_rb05=True, runner=None):
    """Verify product, unlock state, slot, non-userspace, and optional RB05 trace."""
    for name, valid_values in (
        ("product", (b"tb8781p1_64_td89",)),
        ("unlocked", (b"yes",)),
        ("current-slot", (b"a", b"_a")),
        ("is-userspace", (b"no",)),
    ):
        status, value = command_fn(f"getvar:{name}")
        if status != b"OKAY" or value not in valid_values:
            raise FastbootRamBootError(f"fastboot_{name}_mismatch")
    trace = runner.bootloader_trace(command_fn("getvar:version-bootloader"))
    if require_rb05 and not trace.get("observed"):
        raise FastbootRamBootError("RAM_patched_LK_version_trace_missing")
    return trace


def execute_preloader_bridge(stage, preloader_fn, runner, event_cb, doc):
    """Run MediaTek Preloader -> BL2_EXT -> LK RAM bridge handshake in memory."""
    quiet_ctx = runpy.run_path(str(TOOLS / "preloader-ram-probe.py"))["quiet"]
    with quiet_ctx():
        mtk, cfg, strict_config = preloader_fn()
    phase = "preloader"
    try:
        signal.alarm(35)
        with quiet_ctx():
            if not mtk.preloader.init(display=False, directory=os.getcwd()):
                raise FastbootRamBootError("preloader_window_not_captured")
            if cfg.is_brom or (cfg.hwcode, cfg.hw_sub_code, cfg.hwver) != (0x1208, 0x8A00, 0xCA00):
                raise FastbootRamBootError("unexpected_preloader_hardware")
        event_cb("original_preloader_security_and_hardware_verified")
        with quiet_ctx():
            if not mtk.preloader.send_da(runner.ENTRY, len(stage), 0, stage):
                raise FastbootRamBootError("RAM_transfer_failed")
        phase = "jump_requested"
        event_cb("RAM_bridge_transferred", bytes=len(stage))
        with quiet_ctx():
            if not mtk.preloader.jump_da(runner.ENTRY):
                raise FastbootRamBootError("RAM_jump_rejected")
            signal.alarm(8)
            raw = runner.read_stage_record(mtk.port.usbread, 64)
        event_cb("RAM_stage_reply_length_observed", bytes=len(raw), expected=64)
        doc["preloader_allocation"] = runner.boot_stage_record(raw)
        doc["ARM32_stage_executed"] = True
        event_cb("shipping_BL2_allocation_observed", **doc["preloader_allocation"])
        with quiet_ctx():
            runner.resume_ram_bridge(mtk.port.usbwrite)
        doc["boot_chain_resume_requested"] = True
        phase = "normal_boot_chain"
        event_cb("host_receipt_confirmed_boot_chain_resume_requested")
        with quiet_ctx():
            raw_mut = runner.read_stage_record(mtk.port.usbread, 40)
        doc["preloader_mutation"] = runner.mutation_stage_record(raw_mut)
        event_cb("physical_RAM_patch_readback_received", **doc["preloader_mutation"])
        with quiet_ctx():
            runner.finish_ram_bridge(mtk.port.usbwrite)
        event_cb("RAM_patch_readback_receipt_sent")
        with quiet_ctx():
            mtk.port.close(reset=False)
        mtk = None
        signal.alarm(0)
        return None
    except BaseException as exc:
        signal.alarm(0)
        return mtk, phase, strict_config, exc


def require_bridge_assets(paths, runner):
    """Validate local stock preloader, DA loader, and mtkclient before restarting device."""
    if not paths["preloader"] or sha256_file(paths["preloader"]) != PRELOADER_SHA:
        raise FastbootRamBootError("Stock preloader missing or digest mismatch")
    if not paths["loader"] or sha256_file(paths["loader"]) != runner.DA_SHA:
        raise FastbootRamBootError("Stock DA_BR.bin loader missing or digest mismatch")
    if not paths["vendor"] or not (paths["vendor"] / "mtkclient").is_dir():
        raise FastbootRamBootError("mtkclient vendor directory missing")


def detect_entry_transport(entry_mode, find_fb_fn, find_pre_fn, adb_select_fn, paths, serial,
                           wait_seconds=0, *, clock=time.monotonic, sleeper=time.sleep):
    """Detect active device transport (fastboot, preloader, or ADB) with optional polling."""
    deadline = clock() + max(0, wait_seconds)
    while True:
        fb_found = find_fb_fn() if entry_mode in ("auto", "fastboot") else None
        if fb_found is not None:
            return "fastboot", fb_found
        pre_found = find_pre_fn() if entry_mode in ("auto", "preloader") else None
        if pre_found is not None:
            return "preloader", pre_found
        adb_dev = adb_select_fn(paths["adb"], serial) if entry_mode in ("auto", "adb") else None
        if adb_dev is not None:
            return "adb", adb_dev
        if clock() >= deadline:
            return None, None
        sleeper(0.25)


def execute_ram_boot(args, target, paths, runner, *, deps=None):
    """Orchestrate entry-state handling, RAM LK patch, fastboot download, and boot."""
    deps = deps or {}
    adb_select_fn = deps.get("adb_select", adb_select_device)
    snapshot_fn = deps.get("android_snapshot", runner.android_snapshot)
    run_cmd_fn = deps.get("run_cmd", subprocess.run)
    preloader_cfg_fn = deps.get("configure_preloader")
    find_fb_fn = deps.get("find_fastboot")
    find_pre_fn = deps.get("find_preloader")
    usb_util_mod = deps.get("usb_util")
    clock = deps.get("clock", time.monotonic)
    sleeper = deps.get("sleeper", time.sleep)
    work_parent = deps.get("work_parent")

    plan = read_json(CHAIN / "bootchain-ram-plan.json")
    stage = (CHAIN / "preloader-ram-boot.bin").read_bytes()
    if sha256_bytes(stage) != plan["preloader_stage_sha256"]:
        raise FastbootRamBootError("preloader-ram-boot.bin digest differs from bootchain-ram-plan.json")

    if usb_util_mod is None:
        import usb.core
        import usb.util as usb_util_mod
        import usb.backend.libusb1
        backend = usb.backend.libusb1.get_backend()
        if backend is None:
            raise FastbootRamBootError("libusb_unavailable")
        if find_fb_fn is None:
            find_fb_fn = lambda: runner.find_fastboot(backend)
        if find_pre_fn is None:
            find_pre_fn = lambda: find_preloader(backend, usb_core_mod=usb.core)

    prior = latest_return_report()
    doc = {
        "started_at": datetime.now(timezone.utc).isoformat(),
        "status": "prepared",
        "entry_mode_requested": args.entry,
        "entry_mode_used": None,
        "ARM32_stage_executed": False,
        "RAM_patched_LK_executed": False,
        "EDK2_boot_command_acknowledged": False,
        "device_storage_write_commands": [],
        "stock_Android_return_observed": False,
        "stage_sha256": sha256_bytes(stage),
        "trial_candidate_sha256": target["sha256"],
        "trial_candidate_bytes": target["bytes"],
        "verified_edk2_proofs": target["verified_edk2"],
        "prior_return_report_sha256": sha256_file(prior) if prior else None,
        "events": [],
    }
    report_path = Path(args.report) if args.report else paths["report_dir"] / f"fastboot-ram-boot-{time.time_ns()}.json"
    started = clock()

    def event(name, **fields):
        payload = {"event": name, "at": datetime.now(timezone.utc).isoformat(),
                   "elapsed_seconds": round(clock() - started, 3), **fields}
        doc["events"].append(payload)
        write_json(report_path, doc)
        print(json.dumps(payload), flush=True)

    def timeout_handler(*_):
        raise TimeoutError("bounded_RAM_boot_timeout")

    old_alarm = signal.signal(signal.SIGALRM, timeout_handler)
    mtk = None
    phase = "preflight"
    strict_config = None
    fb_device = None
    command_ready = True
    working = None
    cwd_before = os.getcwd()

    try:
        # Determine entry transport cleanly (optionally waiting for USB/ADB attachment).
        kind, found = detect_entry_transport(
            args.entry, find_fb_fn, find_pre_fn, adb_select_fn, paths, args.serial,
            wait_seconds=getattr(args, "wait_device", 0), clock=clock, sleeper=sleeper,
        )
        if kind == "fastboot":
            fb_dev, ep = found
            usb_util_mod.claim_interface(fb_dev, ep[0])
            try:
                w = lambda d: fb_dev.write(ep[1], d, timeout=3000)
                r = lambda n: fb_dev.read(ep[2], n, timeout=3000)
                cmd = lambda name: send_fastboot_command(w, r, name, runner)
                trace = verify_fastboot_vars(cmd, require_rb05=False, runner=runner)
                if trace["observed"]:
                    doc["entry_mode_used"] = "fastboot_patched"
                    doc["BL2_to_LK_trace"] = trace
                    event("existing_RAM_patched_fastboot_detected")
                    fb_device = fb_dev
                else:
                    require_bridge_assets(paths, runner)
                    doc["entry_mode_used"] = "fastboot_stock"
                    event("stock_fastboot_detected_requesting_reboot_bootloader")
                    reply = cmd("reboot-bootloader")
                    if reply[0] != b"OKAY":
                        raise FastbootRamBootError("fastboot_reboot_bootloader_rejected")
                    event("fastboot_reboot_bootloader_acknowledged")
                    fb_dev_to_close = fb_dev
                    fb_dev = None
                    usb_util_mod.dispose_resources(fb_dev_to_close)
            except BaseException:
                if fb_device is None and fb_dev is not None:
                    with contextlib.suppress(Exception):
                        usb_util_mod.dispose_resources(fb_dev)
                raise
        elif kind == "preloader":
            require_bridge_assets(paths, runner)
            doc["entry_mode_used"] = "preloader"
            event("preloader_USB_detected")
        elif kind == "adb":
            adb_dev = found
            require_bridge_assets(paths, runner)
            doc["entry_mode_used"] = "adb"
            snap = snapshot_fn(paths["adb"], adb_dev)
            doc["before"] = snap
            validate_adb_preflight(snap, require_daily_services=args.require_daily_services, runner=runner)
            event("adb_preflight_verified", wired_power=bool(snap.get("USB_charging") or snap.get("AC_powered")))
            phase = "reboot_requested"
            res = run_cmd_fn([str(paths["adb"]), "-s", adb_dev, "reboot", "bootloader"], capture_output=True, timeout=15)
            if res.returncode != 0:
                raise FastbootRamBootError("ADB_bootloader_restart_failed")
            event("adb_reboot_bootloader_requested")
        else:
            raise FastbootRamBootError("No T70 Pro device found in fastboot, preloader, or authorized ADB mode.")

        # Inject Preloader->BL2->LK RAM bridge unless RB05 fastboot is already live.
        if fb_device is None:
            parent_dir = Path(work_parent) if work_parent else report_path.parent
            parent_dir.mkdir(parents=True, exist_ok=True)
            working = tempfile.TemporaryDirectory(prefix="t70-ram-boot-", dir=parent_dir)
            work = working.name
            if str(paths["vendor"]) not in sys.path:
                sys.path.insert(0, str(paths["vendor"]))
            os.chdir(work)
            if preloader_cfg_fn is None:
                preloader_cfg_fn = lambda: runner.configure_stock_preloader(paths["loader"], work)
            failure = execute_preloader_bridge(stage, preloader_cfg_fn, runner, event, doc)
            if failure is not None:
                mtk, phase, strict_config, bridge_exc = failure
                raise bridge_exc
            fb_device, ep = wait_for_fastboot(find_fb_fn, 45, clock=clock, sleeper=sleeper)
            usb_util_mod.claim_interface(fb_device, ep[0])
            event("fastboot_USB_transport_observed")
            w = lambda d: fb_device.write(ep[1], d, timeout=3000)
            r = lambda n: fb_device.read(ep[2], n, timeout=3000)
            cmd = lambda name: send_fastboot_command(w, r, name, runner)
            doc["BL2_to_LK_trace"] = verify_fastboot_vars(cmd, require_rb05=True, runner=runner)
            event("BL2_to_LK_progress_observed", **doc["BL2_to_LK_trace"])
        else:
            w = lambda d: fb_device.write(ep[1], d, timeout=3000)
            r = lambda n: fb_device.read(ep[2], n, timeout=3000)
            cmd = lambda name: send_fastboot_command(w, r, name, runner)

        # Verify patched boot handler rejects empty boot cleanly, then download & boot image.
        try:
            boot_probe = cmd("boot")
            event(
                "LK_boot_probe_reply_observed",
                status=boot_probe[0].decode("ascii"),
                recognized=("RAM_boot_handler" if boot_probe == (b"FAIL", runner.FAILURE)
                            else "unknown_command" if boot_probe == (b"FAIL", b"unknown command")
                            else "security_control_denied" if boot_probe == (b"FAIL", b"No support by security control")
                            else "other"),
            )
            if boot_probe == (b"FAIL", b"No support by security control"):
                raise FastbootRamBootError("shipping_security_control_rejected_RAM_boot")
            if boot_probe != (b"FAIL", runner.FAILURE):
                raise FastbootRamBootError("RAM_boot_command_not_installed_or_unexpected_reply")
            doc["RAM_patched_LK_executed"] = True
            event("RAM_patched_64bit_LK_command_execution_observed")

            phase = "RAM_image_download"
            command_ready = False
            try:
                sent = runner.fastboot_download(w, r, target["payload"], clock=clock)
            except runner.DownloadError as err:
                command_ready = err.command_ready
                raise
            command_ready = True
            event("hash_verified_image_downloaded_to_RAM", bytes=sent)

            phase = "EDK2_boot_request"
            command_ready = False
            boot_reply = cmd("boot")
            command_ready = (boot_reply[0] == b"FAIL")
            if boot_reply != (b"OKAY", b""):
                raise FastbootRamBootError("RAM_boot_command_not_acknowledged")
            doc["EDK2_boot_command_acknowledged"] = True
            phase = "EDK2_RAM_requested"
            doc["status"] = "EDK2_boot_command_acknowledged"
            event("EDK2_RAM_boot_requested", interactive=target["interactive"])
        finally:
            try:
                if command_ready:
                    ack = cmd("reboot")
                    doc["stock_reboot_acknowledged"] = ack[0] == b"OKAY"
                    event("stock_Android_reboot_requested", acknowledged=doc["stock_reboot_acknowledged"])
                else:
                    event("fastboot_transport_released")
            finally:
                with contextlib.suppress(Exception):
                    usb_util_mod.dispose_resources(fb_device)
                fb_device = None

        if args.wait_return > 0:
            event("waiting_for_stock_Android_return", timeout_seconds=args.wait_return)
            consecutive = 0
            deadline = clock() + args.wait_return
            while clock() < deadline:
                try:
                    dev = adb_select_fn(paths["adb"], args.serial)
                    if dev:
                        snap = snapshot_fn(paths["adb"], dev)
                        if snap.get("boot_completed") and snap.get("slot") == "_a" and snap.get("flash_locked") == "0":
                            consecutive += 1
                            if consecutive >= 2:
                                doc["after"] = snap
                                doc["stock_Android_return_observed"] = True
                                doc["status"] = "EDK2_boot_acknowledged_and_Android_returned"
                                event("stock_Android_return_observed")
                                break
                        else:
                            consecutive = 0
                except Exception:
                    consecutive = 0
                sleeper(2)

        write_json(report_path, doc)
        print(f"Report saved: {report_path}", flush=True)
        return doc
    except BaseException as exc:
        signal.alarm(0)
        doc["status"] = "preflight_gate_closed" if phase == "preflight" else "stopped"
        doc["error"] = str(exc) if isinstance(exc, (FastbootRamBootError, ValueError, runner.DownloadError)) else type(exc).__name__
        event("stopped", phase=phase, reason=doc["error"])
        if mtk is not None and runner.preloader_return_allowed(phase, False):
            with contextlib.suppress(Exception, SystemExit):
                signal.alarm(45)
                ack, err = runner.attempt_stock_da_return(mtk, strict_config)
                doc["stock_reboot_acknowledged"] = ack
                event("stock_DA_reboot_after_failure", acknowledged=ack, error=err)
                signal.alarm(0)
        raise
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, old_alarm)
        with contextlib.suppress(Exception):
            os.chdir(cwd_before)
        if mtk is not None:
            with contextlib.suppress(Exception, SystemExit):
                mtk.port.close(reset=False)
        if fb_device is not None:
            with contextlib.suppress(Exception):
                usb_util_mod.dispose_resources(fb_device)
        if working is not None:
            with contextlib.suppress(Exception):
                working.cleanup()


def build_parser():
    """Create CLI argument parser for fastboot-ram-boot."""
    parser = argparse.ArgumentParser(
        description="Automated fastboot boot launcher for Teclast T70 Pro via RAM-patched LK.",
    )
    parser.add_argument("--build", action="store_true", help="Build firmware-trial first inside builder.")
    parser.add_argument("--build-output", help="Custom Build/<name> directory when --build is used.")
    parser.add_argument("--build-dir", type=Path, help="Specific RELEASE_CLANGPDB directory containing boot image.")
    parser.add_argument("--image", type=Path, help="Specific boot.img (default: newest built T70Pro-UEFI-Trial-boot.img).")
    parser.add_argument("--unverified-v4-image", action="store_true",
                        help="Allow booting a standalone Android v4 kernel-only boot.img without full EDK2 proofs.")
    parser.add_argument("--max-image-bytes", type=int, default=DEFAULT_MAX_IMAGE_BYTES,
                        help="Maximum RAM download size check (default 2 MiB).")
    parser.add_argument("--force-prepare", action="store_true", help="Re-run offline verification even on cache hit.")
    parser.add_argument("--rebuild-chain", action="store_true", help="Rebuild and re-verify Preloader/BL2/LK RAM bridges.")
    parser.add_argument("--entry", choices=("auto", "adb", "fastboot", "preloader"), default="auto",
                        help="How to reach RAM-patched fastboot (default: auto-detect).")
    parser.add_argument("--serial", help="Explicit ADB device serial when multiple devices are connected.")
    parser.add_argument("--preloader", type=Path, help="Path to stock preloader_tb8781p1_64_td89.bin.")
    parser.add_argument("--loader", type=Path, help="Path to stock DA_BR.bin.")
    parser.add_argument("--vendor", type=Path, help="Path to mtkclient vendor directory.")
    parser.add_argument("--adb", type=Path, help="Path to adb binary.")
    parser.add_argument("--require-daily-services", action="store_true",
                        help="Require active SIM, Wi-Fi, and wired power in ADB preflight.")
    parser.add_argument("--wait-device", type=int, default=0,
                        help="Optional seconds to wait for ADB/Fastboot/Preloader connection before starting.")
    parser.add_argument("--wait-return", type=int, default=0,
                        help="Optional seconds to poll read-only ADB for Android return after boot.")
    parser.add_argument("--report", type=Path, help="Explicit JSON report output path.")
    parser.add_argument("--status", action="store_true", help="Read-only check of image cache and USB/ADB status.")
    parser.add_argument("--execute", action="store_true", help="Perform the RAM-only fastboot boot on the tablet.")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    runner = load_runner()
    paths = resolve_runtime_paths(args)
    try:
        if args.status:
            prep = load_prepare()
            default_bdir = str(args.build_dir or (args.image.parent if args.image else prep.resolve_latest_build_dir()))
            usb_state = probe_device_state(paths, args, runner)
            print(json.dumps({
                "status": "ok",
                "device_access": "read_only",
                "default_build_dir": default_bdir,
                "usb_and_adb": usb_state,
                "paths": {k: str(v) if v else None for k, v in paths.items()},
            }, indent=2))
            return 0

        target = resolve_boot_target(args, runner)
        if not args.execute:
            print(json.dumps({
                "prepared": True,
                "device_access": False,
                "storage_writes": [],
                "boot_path": target["boot_path"],
                "proofs_path": target["proofs_path"],
                "sha256": target["sha256"],
                "bytes": target["bytes"],
                "cached": target["cached"],
                "verified_edk2_proofs": target["verified_edk2"],
                "entry": args.entry,
                "next_step": "Re-run with --execute to perform RAM-only fastboot boot.",
            }, indent=2))
            return 0

        execute_ram_boot(args, target, paths, runner)
        return 0
    except (FastbootRamBootError, ValueError, OSError, runner.DownloadError) as error:
        print(f"fastboot-ram-boot error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
