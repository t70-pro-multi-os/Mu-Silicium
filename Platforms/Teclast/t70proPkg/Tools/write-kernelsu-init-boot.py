#!/usr/bin/env python3
"""Write only the explicitly requested KernelSU init_boot_a; preserve every other partition."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import traceback

sys.dont_write_bytecode = True
KIT = Path("/Users/you/.tmp/teclast-t70pro/device-backup/recovery-kit")
IMAGE = Path("/Users/you/.tmp/kernelsu_patched_20261116_044027.img")
IMAGE_SHA = "7f5283aa30ff6e4078bfe7acd70438858ed53bca7ca876d73487a98000a1b9a3"
ORIGINAL_SHA = "4139e97412cf29aaaeb064444f6eebbf00d359b7892c4741a47c1ba697245632"
ADDRESS, SIZE = 849346560, 8388608
ADB = "/opt/homebrew/bin/adb"
FINGERPRINT = "Teclast/T70Pro_ROW/T70Pro:16/BP2A.250605.031.A3/20260731:user/release-keys"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def original_region(region):
    path = (KIT / region["source"]).resolve()
    require(path.is_relative_to(KIT / "images"), "invalid_original_path")
    with path.open("rb") as stream:
        stream.seek(region["source_offset_bytes"])
        data = stream.read(region["size_bytes"])
    require(len(data) == region["size_bytes"] and sha(data) == region["sha256"],
            "original_region_hash_differs")
    return data


def android():
    expected = {"ro.boot.slot_suffix": "_a", "ro.boot.flash.locked": "0",
                "ro.boot.vbmeta.device_state": "unlocked", "ro.boot.verifiedbootstate": "orange",
                "ro.build.fingerprint": FINGERPRINT, "sys.boot_completed": "1"}
    actual = {}
    for key in expected:
        result = subprocess.run([ADB, "-d", "shell", "getprop", key],
                                capture_output=True, text=True, timeout=8)
        require(result.returncode == 0, "ADB_property_failed")
        actual[key] = result.stdout.strip()
    require(actual == expected, "Android_slot_lock_or_firmware_differs")
    return actual


def write_exact(backend, payload, progress=None):
    require(len(payload) == SIZE, "init_boot_payload_length_differs")
    backend.write("user", ADDRESS, payload)
    if progress:
        progress("manufacturer_write_acknowledged")
    require(backend.read("user", ADDRESS, SIZE) == payload, "init_boot_full_readback_differs")


def main():
    os.umask(0o077)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--execute", action="store_true")
    p.add_argument("--resume-preloader", action="store_true")
    p.add_argument("--report", type=Path, required=True)
    args = p.parse_args()
    require(not args.report.exists(), "existing_report_preserved")
    recovery = module("sealed_init_boot_recovery", KIT / "restore.py")
    recovery.check_integrity()
    plan = json.loads((KIT / "plan.json").read_text())
    target = plan["partitions"]["init_boot_a"]
    require((target["parttype"], target["offset_bytes"], target["size_bytes"], target["sha256"]) ==
            ("user", ADDRESS, SIZE, ORIGINAL_SHA), "unexpected_init_boot_partition")
    recovery.validate_regions(plan, [target])
    payload = IMAGE.read_bytes()
    require(len(payload) == SIZE and sha(payload) == IMAGE_SHA, "requested_image_changed")
    require(payload[:8] == b"ANDROID!" and struct.unpack_from("<I", payload, 8)[0] == 0 and
            struct.unpack_from("<I", payload, 40)[0] == 4, "not_a_v4_init_boot")
    ramdisk_size = struct.unpack_from("<I", payload, 12)[0]
    require(0 < ramdisk_size <= SIZE - 4096 and payload[-64:-60] == b"AVBf",
            "invalid_ramdisk_bounds")
    stock = original_region(target)
    require(all(payload[i] == stock[i] for i in range(4096) if i not in range(12, 16)),
            "unexpected_boot_header_change")
    gpt_originals = [(region, original_region(region)) for region in plan["gpt_regions"]]
    if args.resume_preloader:
        previous = Path(__file__).parents[5] / "analysis/installation-preparation/kernelsu-init-boot-20261116-044027.jsonl"
        events = [json.loads(line) for line in previous.read_text().splitlines()]
        require(events[0]["event"] == "preflight_verified" and events[0]["sha256"] == IMAGE_SHA,
                "original_live_preflight_missing")
        require(events[-1]["event"] == "original_manufacturer_DA_connected_same_device_and_geometry_verified" and
                not any(e["event"] == "write_intent" for e in events), "session_not_safe_to_resume")
        state = events[0]["android"]
    else:
        state = android()
    recovery.import_mtk()
    installer = module("boot_control_guard", Path(__file__).with_name("boot-install.py"))
    if not args.execute:
        print(json.dumps({"preflight_passed": True, "target": "init_boot_a", "bytes": SIZE,
                          "sha256": IMAGE_SHA, "write_requested": False, "USB_access": False}))
        return
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("x") as journal:
        def event(name, **fields):
            item = {"event": name, "time_ns": time.time_ns(), **fields}
            journal.write(json.dumps(item, ensure_ascii=False) + "\n")
            journal.flush()
            os.fsync(journal.fileno())
            print(json.dumps(item, ensure_ascii=False), flush=True)
        event("preflight_verified", partition="init_boot_a", parttype="user",
              offset_bytes=ADDRESS, bytes=SIZE, image=str(IMAGE), sha256=IMAGE_SHA,
              original_sha256=ORIGINAL_SHA, android=state)
        if args.resume_preloader:
            event("existing_manufacturer_preloader_connection_requested")
        else:
            event("ADB_bootloader_restart_requested_for_preloader_capture")
            reboot = subprocess.run([ADB, "-d", "reboot", "bootloader"],
                                    capture_output=True, timeout=15)
            require(reboot.returncode == 0, "ADB_restart_failed")
        with recovery.Device(plan) as device:
            event("original_manufacturer_DA_connected_same_device_and_geometry_verified")
            for region, data in gpt_originals:
                require(device.read(region["parttype"], region["offset_bytes"], len(data)) == data,
                        "current_GPT_differs")
            misc = device.read("user", plan["partitions"]["misc"]["offset_bytes"], 4096)
            installer.check_boot_control(misc)
            command = misc[:32].split(b"\0", 1)[0]
            known = command in (b"", b"bootonce-bootloader")
            event("boot_request_observed", command=command.decode("ascii") if known else "unrecognized",
                  command_bytes_sha256=sha(misc[:32]))
            require(known, "unexpected_persistent_boot_command_present")
            before = device.read("user", ADDRESS, SIZE)
            before_sha = sha(before)
            if before_sha != ORIGINAL_SHA:
                saved = args.report.with_suffix(".before.img")
                with saved.open("xb") as stream:
                    stream.write(before)
                    stream.flush()
                    os.fsync(stream.fileno())
                event("current_init_boot_saved", bytes=SIZE, sha256=before_sha, path=str(saved))
            else:
                event("current_init_boot_matches_verified_stock", sha256=before_sha)
            if before_sha not in (ORIGINAL_SHA, IMAGE_SHA):
                event("damaged_current_image_restore_intent", sha256=ORIGINAL_SHA)
                write_exact(device, stock, event)
                event("verified_stock_restored_after_interrupted_session", sha256=ORIGINAL_SHA)
                device.reboot()
                raise ValueError("stock_restored_verify_Android_before_retry")
            if before_sha != IMAGE_SHA:
                event("write_intent", partition="init_boot_a", offset_bytes=ADDRESS,
                      bytes=SIZE, sha256=IMAGE_SHA)
                try:
                    write_exact(device, payload, event)
                except BaseException as exc:
                    event("write_or_readback_failed", error_type=type(exc).__name__,
                          code_locations=[{"file":Path(f.filename).name,"line":f.lineno,"function":f.name}
                                          for f in traceback.extract_tb(exc.__traceback__)])
                    try:
                        write_exact(device, stock, event)
                        event("original_bytes_restored_and_verified", sha256=ORIGINAL_SHA)
                        device.reboot()
                    except BaseException as restore_exc:
                        event("automatic_restore_failed", error_type=type(restore_exc).__name__)
                    raise
            else:
                event("requested_image_already_present_full_readback_verified",
                      bytes=SIZE, sha256=IMAGE_SHA)
            event("requested_image_written_and_full_readback_verified",
                  partition="init_boot_a", bytes=SIZE, sha256=IMAGE_SHA)
            device.reboot()
            event("normal_Android_restart_requested", android_boot_verified=False)
    print("init_boot_a 기록과 전체 바이트 검증 완료. Android 재연결 확인이 필요합니다.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        safe = isinstance(exc, ValueError)
        print("중지: " + (str(exc) if safe else type(exc).__name__), file=sys.stderr)
        sys.exit(1)
