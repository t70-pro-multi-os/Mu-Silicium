#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Unit and integration tests for prepare-ram-boot.py and fastboot-ram-boot.py (no USB device needed)."""
import contextlib
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace

sys.dont_write_bytecode = True

TESTS_DIR = Path(__file__).resolve().parent
PKG_DIR = TESTS_DIR.parent
TOOLS_DIR = PKG_DIR / "Tools"
CHAIN_DIR = PKG_DIR.parents[2].parent / "analysis/lk-ram-boot"


def load_mod(name, path):
    """Import module from path."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def make_valid_v4_boot_img(kernel_payload=None):
    """Build a synthetic valid kernel-only Android v4 boot image."""
    payload = kernel_payload or (b"\x02\x21\x4c\x18" + b"\xab" * 252)
    header = bytearray(4096)
    header[:8] = b"ANDROID!"
    struct.pack_into("<4I", header, 8, len(payload), 0, 0, 1584)
    struct.pack_into("<I", header, 40, 4)
    struct.pack_into("<I", header, 1580, 4096)
    kpad = bytes(-len(payload) % 4096)
    return bytes(header) + payload + kpad + bytes(4096)


def pack_stg32(entry=0x40200000):
    """Pack a valid 64-byte T70STG32 Preloader allocation record."""
    return struct.pack(
        "<8s14I",
        b"T70STG32", 3, entry,
        0x800001D3, 0x00C50838, 0x81000000,
        0x401FFFF4, 12, 0x2091CB,
        4, 0x62F00000, 0, 0x2000000, 1, 0x4857D2F0,
    )


def pack_arm32():
    """Pack a valid 40-byte T70ARM32 Preloader mutation readback record."""
    return struct.pack(
        "<8s8I",
        b"T70ARM32", 1, 7, 0, 0x94000349, 1, 1, 0, 1,
    )


class FakeFastbootDevice:
    """In-memory fastboot USB endpoint state machine."""

    def __init__(self, version_bytes, *, unlocked=b"yes", product=b"tb8781p1_64_td89",
                 slot=b"a", userspace=b"no", boot_mode="patched",
                 fail_after_data_partial=False, fail_final_boot=False,
                 on_reboot_bootloader=None):
        self.version = version_bytes
        self.unlocked = unlocked
        self.product = product
        self.slot = slot
        self.userspace = userspace
        self.boot_mode = boot_mode
        self.fail_after_data_partial = fail_after_data_partial
        self.fail_final_boot = fail_final_boot
        self.on_reboot_bootloader = on_reboot_bootloader
        self.commands = []
        self.replies = []
        self.downloaded = bytearray()
        self.expecting = 0
        self.disposed = False

    def write(self, ep, data, timeout=3000):
        raw = bytes(data)
        if self.expecting > 0:
            take = raw[:self.expecting]
            self.downloaded.extend(take)
            self.expecting -= len(take)
            if self.fail_after_data_partial and len(self.downloaded) >= 4096:
                raise OSError("usb_disconnect_mid_download")
            if self.expecting == 0:
                self.replies.append(b"OKAY")
            return len(raw)

        cmd = raw.decode("ascii")
        self.commands.append(cmd)
        if cmd == "getvar:product":
            self.replies.append(b"OKAY" + self.product)
        elif cmd == "getvar:unlocked":
            self.replies.append(b"OKAY" + self.unlocked)
        elif cmd == "getvar:current-slot":
            self.replies.append(b"OKAY" + self.slot)
        elif cmd == "getvar:is-userspace":
            self.replies.append(b"OKAY" + self.userspace)
        elif cmd == "getvar:version-bootloader":
            self.replies.append(b"OKAY" + self.version)
        elif cmd == "reboot-bootloader":
            self.replies.append(b"OKAY")
            if self.on_reboot_bootloader:
                self.on_reboot_bootloader()
        elif cmd == "reboot":
            self.replies.append(b"OKAY")
        elif cmd.startswith("download:"):
            size = int(cmd.split(":", 1)[1], 16)
            self.downloaded.clear()
            self.expecting = size
            self.replies.append(f"DATA{size:08x}".encode("ascii"))
        elif cmd == "boot":
            if self.boot_mode == "stock":
                self.replies.append(b"FAILunknown command")
            elif not self.downloaded:
                self.replies.append(b"FAILRAM boot: unlocked, valid EDK2 image required")
            elif self.fail_final_boot:
                self.replies.append(b"FAILbad kernel header")
            else:
                self.replies.append(b"OKAY")
        else:
            self.replies.append(b"FAILunknown command")
        return len(raw)

    def read(self, ep, size, timeout=3000):
        assert self.replies, "read with empty reply queue"
        return self.replies.pop(0)


class FakeUsbUtil:
    """Track USB claim and dispose calls."""

    def __init__(self):
        self.claimed = []
        self.disposed = []

    def claim_interface(self, dev, iface):
        self.claimed.append((dev, iface))

    def dispose_resources(self, dev):
        dev.disposed = True
        self.disposed.append(dev)


def make_fake_mtk(entry=0x40200000, *, init_ok=True, jump_ok=True, bad_hw=False):
    """Build a realistic Preloader Mtk + Port + Config + Daloader mock."""
    state = {"sent": None, "jumped": False, "tokens": [], "closed": False, "da_return_calls": 0}
    reads = [pack_stg32(entry), pack_arm32()]

    class Port:
        def usbread(self, size):
            return reads.pop(0) if reads else b""

        def usbwrite(self, data):
            state["tokens"].append(bytes(data))
            return len(data)

        def close(self, reset=False):
            state["closed"] = True

    class Preloader:
        def init(self, display=False, directory=None):
            return init_ok

        def send_da(self, addr, length, sig_len, payload):
            assert addr == entry and length == len(payload)
            state["sent"] = bytes(payload)
            return True

        def jump_da(self, addr):
            assert addr == entry
            state["jumped"] = True
            return jump_ok

    class Daloader:
        patch = True

        def upload_da(self, preloader=None):
            assert self.patch is False and preloader is None
            return True

        def shutdown(self, bootmode=1):
            assert bootmode == 1
            state["da_return_calls"] += 1
            return True

    cfg = SimpleNamespace(
        is_brom=False,
        hwcode=0x9999 if bad_hw else 0x1208,
        hw_sub_code=0x8A00,
        hwver=0xCA00,
    )
    mtk = SimpleNamespace(port=Port(), preloader=Preloader(), daloader=Daloader())
    return mtk, cfg, (lambda display=False: {}), state


def runtime_paths(tmp_dir, runner):
    """Build valid local loader/preloader/vendor fixtures inside tmp_dir for isolation."""
    tmp = Path(tmp_dir)
    real_pre = Path("/Users/you/.tmp/teclast-t70pro/stock-loader/preloader_tb8781p1_64_td89.bin")
    real_da = Path("/Users/you/.tmp/teclast-t70pro/stock-loader/DA_BR.bin")
    if real_pre.is_file() and real_da.is_file():
        pre, da = real_pre, real_da
        vendor = Path("/Users/you/.tmp/teclast-t70pro/device-backup/recovery-kit/vendor")
    else:
        pre, da = None, None
        vendor = None
    assert pre and hashlib.sha256(pre.read_bytes()).hexdigest() == "cc42d1aec1f3dd9a94cfd9147ab5c1f55e592d1aa17ebbe7a9d4c1a49e9d436e"
    assert da and hashlib.sha256(da.read_bytes()).hexdigest() == runner.DA_SHA
    return {"preloader": pre, "loader": da, "vendor": vendor, "adb": Path("/opt/homebrew/bin/adb"), "report_dir": tmp}


def test_v4_validation_and_commands(fb_mod, runner):
    """Test Android v4 validation, allowlisted commands, and ADB selection."""
    cases = 0
    valid = make_valid_v4_boot_img()
    meta = fb_mod.validate_v4_image(valid)
    assert meta["bytes"] == len(valid)
    cases += 1

    bad_magic = bytearray(valid); bad_magic[0] = ord("X")
    bad_ver = bytearray(valid); struct.pack_into("<I", bad_ver, 40, 3)
    bad_hdr = bytearray(valid); struct.pack_into("<I", bad_hdr, 20, 1580)
    bad_rd = bytearray(valid); struct.pack_into("<I", bad_rd, 12, 16)
    bad_kern = bytearray(valid); struct.pack_into("<I", bad_kern, 8, 32)
    trunc = valid[:-1]
    for bad in (bytes(bad_magic), bytes(bad_ver), bytes(bad_hdr), bytes(bad_rd), bytes(bad_kern), trunc, b"\0" * 512):
        try:
            fb_mod.validate_v4_image(bad)
        except fb_mod.FastbootRamBootError:
            cases += 1
        else:
            raise AssertionError("Invalid v4 image was accepted")

    # Fastboot allowlist must allow reboot-bootloader & boot, and forbid flash/erase/unlock.
    for cmd in ("flash:boot", "erase:userdata", "flashing unlock", "set_active:b", "reboot-recovery", "oem cdms"):
        writes = []
        try:
            fb_mod.send_fastboot_command(lambda d: writes.append(d) or len(d), lambda n: b"OKAY", cmd, runner)
        except fb_mod.FastbootRamBootError:
            assert writes == []
            cases += 1
        else:
            raise AssertionError(f"Disallowed command accepted: {cmd}")

    # ADB selection handles multiple devices when one T70Pro_ROW exists, and never leaks serial on ambiguity.
    fake_sub = SimpleNamespace(
        run=lambda *a, **k: SimpleNamespace(
            returncode=0,
            stdout="List of devices attached\nOTHER123\tdevice model:Pixel\nT70SECRET99\tdevice product:T70Pro model:T70Pro_ROW device:T70Pro\n",
        )
    )
    assert fb_mod.adb_select_device(Path("/test/adb"), None, runner_subprocess=fake_sub) == "T70SECRET99"
    cases += 1

    ambig_sub = SimpleNamespace(
        run=lambda *a, **k: SimpleNamespace(
            returncode=0,
            stdout="List of devices attached\nSECRET_AAA\tdevice model:Foo\nSECRET_BBB\tdevice model:Bar\n",
        )
    )
    try:
        fb_mod.adb_select_device(Path("/test/adb"), None, runner_subprocess=ambig_sub)
    except fb_mod.FastbootRamBootError as err:
        assert "SECRET_AAA" not in str(err) and "SECRET_BBB" not in str(err)
        cases += 1
    else:
        raise AssertionError("Ambiguous ADB list accepted")
    return cases


def test_execution_paths(fb_mod, runner):
    """Exercise all 4 entry paths and fail-closed safety guards via USB/ADB/Preloader mocks."""
    cases = 0
    boot_bytes = make_valid_v4_boot_img()
    target = {
        "verified_edk2": True,
        "boot_path": "/tmp/fake.boot.img",
        "proofs_path": "/tmp/fake.proofs",
        "payload": boot_bytes,
        "sha256": hashlib.sha256(boot_bytes).hexdigest(),
        "bytes": len(boot_bytes),
        "interactive": True,
        "cached": True,
    }
    stock_ver = runner.VERSION.encode("ascii")
    rb05_ver = stock_ver[:-4] + b"RB05"
    healthy_snap = {
        "boot_completed": True, "slot": "_a", "flash_locked": "0", "vbmeta_state": "unlocked",
        "user_running_unlocked": False, "user_download_directory_accessible": False,
        "SIM_states": ["ABSENT", "ABSENT"], "USB_charging": True, "AC_powered": False,
        "WiFi_enabled": False, "WiFi_connected": False,
    }

    with tempfile.TemporaryDirectory(prefix="fb-ram-boot-test-") as tmp:
        paths = runtime_paths(tmp, runner)
        clock_now = [0.0]

        def clock():
            clock_now[0] += 0.1
            return clock_now[0]

        # 1. Entry already in RB05 RAM-patched fastboot -> skips Preloader completely.
        fb_rb05 = FakeFastbootDevice(rb05_ver)
        usb_util = FakeUsbUtil()
        args = fb_mod.build_parser().parse_args(["--execute", "--report", str(Path(tmp) / "r1.json")])
        doc = fb_mod.execute_ram_boot(args, target, paths, runner, deps={
            "find_fastboot": lambda: (fb_rb05, (0, 1, 2)),
            "find_preloader": lambda: None,
            "adb_select": lambda *a, **k: None,
            "usb_util": usb_util,
            "clock": clock,
            "sleeper": lambda s: None,
            "work_parent": tmp,
        })
        assert doc["entry_mode_used"] == "fastboot_patched"
        assert not doc["ARM32_stage_executed"] and doc["RAM_patched_LK_executed"] and doc["EDK2_boot_command_acknowledged"]
        assert bytes(fb_rb05.downloaded) == boot_bytes and "reboot" not in fb_rb05.commands and fb_rb05.disposed
        cases += 1

        # 2. Entry in Stock Fastboot -> reboot-bootloader -> Preloader -> RB05 Fastboot -> boot.
        pre_present = [False]
        fb_stock = FakeFastbootDevice(stock_ver, on_reboot_bootloader=lambda: pre_present.__setitem__(0, True))
        fb_after = FakeFastbootDevice(rb05_ver)
        fb_calls = [0]

        def find_fb_stock():
            fb_calls[0] += 1
            if fb_calls[0] == 1:
                return fb_stock, (0, 1, 2)
            return (fb_after, (0, 1, 2)) if pre_present[0] else None

        mtk, cfg, sec, mtk_state = make_fake_mtk(runner.ENTRY)
        args = fb_mod.build_parser().parse_args(["--execute", "--report", str(Path(tmp) / "r2.json")])
        doc = fb_mod.execute_ram_boot(args, target, paths, runner, deps={
            "find_fastboot": find_fb_stock,
            "find_preloader": lambda: None,
            "adb_select": lambda *a, **k: None,
            "configure_preloader": lambda: (mtk, cfg, sec),
            "usb_util": FakeUsbUtil(),
            "clock": clock,
            "sleeper": lambda s: None,
            "work_parent": tmp,
        })
        assert doc["entry_mode_used"] == "fastboot_stock"
        assert "reboot-bootloader" in fb_stock.commands and fb_stock.disposed
        assert mtk_state["tokens"] == [b"T70GO32!", b"T70RUN!!"] and mtk_state["closed"]
        assert doc["ARM32_stage_executed"] and doc["EDK2_boot_command_acknowledged"]
        assert bytes(fb_after.downloaded) == boot_bytes
        cases += 1

        # 3. Direct Preloader entry -> bridge -> RB05 Fastboot -> boot.
        mtk, cfg, sec, mtk_state = make_fake_mtk(runner.ENTRY)
        fb_pre = FakeFastbootDevice(rb05_ver)
        fb_cnt = [0]
        def find_fb_pre():
            fb_cnt[0] += 1
            return None if fb_cnt[0] == 1 else (fb_pre, (0, 1, 2))
        args = fb_mod.build_parser().parse_args(["--execute", "--entry", "preloader", "--report", str(Path(tmp) / "r3.json")])
        doc = fb_mod.execute_ram_boot(args, target, paths, runner, deps={
            "find_fastboot": find_fb_pre,
            "find_preloader": lambda: object(),
            "adb_select": lambda *a, **k: None,
            "configure_preloader": lambda: (mtk, cfg, sec),
            "usb_util": FakeUsbUtil(),
            "clock": clock,
            "sleeper": lambda s: None,
            "work_parent": tmp,
        })
        assert doc["entry_mode_used"] == "preloader" and doc["EDK2_boot_command_acknowledged"]
        cases += 1

        # 4. ADB entry + wait-return polling (must observe 2 ready samples without calling reboot).
        mtk, cfg, sec, _ = make_fake_mtk(runner.ENTRY)
        fb_adb = FakeFastbootDevice(rb05_ver)
        fb_cnt2 = [0]
        adb_calls = [0]
        cmd_calls = []

        def find_fb_adb():
            fb_cnt2[0] += 1
            return None if fb_cnt2[0] == 1 else (fb_adb, (0, 1, 2))

        def adb_sel(*a, **k):
            adb_calls[0] += 1
            return "T70SECRET"

        def fake_sub(cmd, **kw):
            cmd_calls.append(cmd)
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        args = fb_mod.build_parser().parse_args(["--execute", "--wait-return", "30", "--report", str(Path(tmp) / "r4.json")])
        doc = fb_mod.execute_ram_boot(args, target, paths, runner, deps={
            "find_fastboot": find_fb_adb,
            "find_preloader": lambda: None,
            "adb_select": adb_sel,
            "android_snapshot": lambda *a: dict(healthy_snap),
            "run_cmd": fake_sub,
            "configure_preloader": lambda: (mtk, cfg, sec),
            "usb_util": FakeUsbUtil(),
            "clock": clock,
            "sleeper": lambda s: None,
            "work_parent": tmp,
        })
        assert doc["entry_mode_used"] == "adb" and doc["stock_Android_return_observed"] is True
        assert cmd_calls == [[str(paths["adb"]), "-s", "T70SECRET", "reboot", "bootloader"]]
        cases += 1

        # 5. Failure safety: DATA failure mid-stream must NOT send 'reboot' into unknown parser stream.
        fb_mid_fail = FakeFastbootDevice(rb05_ver, fail_after_data_partial=True)
        args = fb_mod.build_parser().parse_args(["--execute", "--report", str(Path(tmp) / "r5.json")])
        try:
            fb_mod.execute_ram_boot(args, target, paths, runner, deps={
                "find_fastboot": lambda: (fb_mid_fail, (0, 1, 2)),
                "find_preloader": lambda: None,
                "adb_select": lambda *a, **k: None,
                "usb_util": FakeUsbUtil(),
                "clock": clock,
                "sleeper": lambda s: None,
                "work_parent": tmp,
            })
        except runner.DownloadError:
            assert "reboot" not in fb_mid_fail.commands
            cases += 1
        else:
            raise AssertionError("Partial download failure did not raise DownloadError")

        # 6. Failure safety: final boot FAIL (command_ready=True) DOES request stock 'reboot'.
        fb_boot_fail = FakeFastbootDevice(rb05_ver, fail_final_boot=True)
        args = fb_mod.build_parser().parse_args(["--execute", "--report", str(Path(tmp) / "r6.json")])
        try:
            fb_mod.execute_ram_boot(args, target, paths, runner, deps={
                "find_fastboot": lambda: (fb_boot_fail, (0, 1, 2)),
                "find_preloader": lambda: None,
                "adb_select": lambda *a, **k: None,
                "usb_util": FakeUsbUtil(),
                "clock": clock,
                "sleeper": lambda s: None,
                "work_parent": tmp,
            })
        except fb_mod.FastbootRamBootError:
            assert "reboot" in fb_boot_fail.commands
            cases += 1
        else:
            raise AssertionError("Final boot FAIL was not rejected")

        # 7. Locked bootloader in fastboot or ADB must reject BEFORE rebooting.
        fb_locked = FakeFastbootDevice(stock_ver, unlocked=b"no")
        args = fb_mod.build_parser().parse_args(["--execute", "--report", str(Path(tmp) / "r7.json")])
        try:
            fb_mod.execute_ram_boot(args, target, paths, runner, deps={
                "find_fastboot": lambda: (fb_locked, (0, 1, 2)),
                "find_preloader": lambda: None,
                "adb_select": lambda *a, **k: None,
                "usb_util": FakeUsbUtil(),
                "clock": clock,
                "sleeper": lambda s: None,
                "work_parent": tmp,
            })
        except fb_mod.FastbootRamBootError:
            assert "reboot-bootloader" not in fb_locked.commands
            cases += 1
        else:
            raise AssertionError("Locked fastboot was not rejected")

        # 8. Preloader unexpected hardware triggers clean stock DA return (patch=False) if allowed.
        mtk_bad, cfg_bad, sec_bad, st_bad = make_fake_mtk(runner.ENTRY, bad_hw=True)
        fb_bad_cnt = [0]
        args = fb_mod.build_parser().parse_args(["--execute", "--entry", "preloader", "--report", str(Path(tmp) / "r8.json")])
        try:
            fb_mod.execute_ram_boot(args, target, paths, runner, deps={
                "find_fastboot": lambda: None,
                "find_preloader": lambda: object(),
                "adb_select": lambda *a, **k: None,
                "configure_preloader": lambda: (mtk_bad, cfg_bad, sec_bad),
                "usb_util": FakeUsbUtil(),
                "clock": clock,
                "sleeper": lambda s: None,
                "work_parent": tmp,
            })
        except fb_mod.FastbootRamBootError as err:
            assert "unexpected_preloader_hardware" in str(err)
            assert st_bad["da_return_calls"] == 1 and mtk_bad.daloader.patch is False
            cases += 1
        else:
            raise AssertionError("Wrong preloader hwcode was not rejected")

        # 9. --require-daily-services blocks ADB preflight when SIM/Wi-Fi are missing.
        try:
            fb_mod.validate_adb_preflight(healthy_snap, require_daily_services=True, runner=runner)
        except ValueError:
            cases += 1
        else:
            raise AssertionError("--require-daily-services did not reject missing SIM/Wi-Fi")

    return cases


def test_real_cache_and_bridge_resolution(fb_mod, runner):
    """Verify resolve_boot_target hits real cached prepared-images and standalone copies without device access."""
    prep_mod = load_mod("prepare_ram_boot", TOOLS_DIR / "prepare-ram-boot.py")
    chain_valid = prep_mod.bridge_chain_valid(CHAIN_DIR)
    assert chain_valid is True
    cached = sorted((CHAIN_DIR / "prepared-images").glob("*.boot.img"))
    assert cached, "Expected at least one prepared-image boot.img"
    # Test against latest build directory if present.
    latest_build = prep_mod.resolve_latest_build_dir()
    args = fb_mod.build_parser().parse_args(["--build-dir", str(latest_build)])
    target = fb_mod.resolve_boot_target(args, runner)
    assert target["verified_edk2"] is True
    target_cached = fb_mod.resolve_boot_target(args, runner)
    assert target_cached["verified_edk2"] is True and target_cached["cached"] is True
    assert target_cached["sha256"] == target["sha256"]

    # Standalone take-to-device copy (without sibling FV/SILICIUM_UEFI.fd) must auto-match build_dir/cache.
    with tempfile.TemporaryDirectory(prefix="fb-standalone-test-") as tmp:
        copy_path = Path(tmp) / "take-to-device/T70Pro-UEFI-Trial-boot.img"
        copy_path.parent.mkdir(parents=True, exist_ok=True)
        copy_path.write_bytes((latest_build / "T70Pro-UEFI-Trial-boot.img").read_bytes())
        matched = prep_mod.find_matching_build_dir_for_boot(copy_path)
        assert matched == latest_build
        args_standalone = fb_mod.build_parser().parse_args(["--image", str(copy_path)])
        target_standalone = fb_mod.resolve_boot_target(args_standalone, runner)
        assert target_standalone["verified_edk2"] is True and target_standalone["cached"] is True
        assert target_standalone["sha256"] == target["sha256"]
    return 3


def main():
    fb_mod = load_mod("fastboot_ram_boot", TOOLS_DIR / "fastboot-ram-boot.py")
    runner = fb_mod.load_runner()
    c1 = test_v4_validation_and_commands(fb_mod, runner)
    c2 = test_execution_paths(fb_mod, runner)
    c3 = test_real_cache_and_bridge_resolution(fb_mod, runner)
    print(json.dumps({
        "all_passed": True,
        "v4_and_command_cases": c1,
        "execution_and_safety_cases": c2,
        "real_cache_resolution_cases": c3,
        "total_cases": c1 + c2 + c3,
        "device_access": False,
    }))


if __name__ == "__main__":
    main()
