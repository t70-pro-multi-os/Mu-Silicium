#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Real regular-file write/readback and failure tests. No USB libraries loaded."""
import importlib.util
import json
from pathlib import Path
import tempfile
import struct
import zlib

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("boot_install", HERE/"boot-install.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
R = dict(mod.TARGET)
START, SIZE = R["offset_bytes"], R["size_bytes"]
ORIGINAL = b"\x35" * SIZE
PAYLOAD = b"\xa7" * SIZE


class Disk:
    def __init__(self, path, failure=None):
        self.file = path.open("w+b")
        self.file.truncate(START + SIZE + 4096)
        self.file.seek(START-4096)
        self.file.write(b"L"*4096 + ORIGINAL + b"R"*4096)
        self.file.flush()
        self.writes = []
        self.failure, self.failed = failure, False
        self.corrupt_next = False

    def read(self, parttype, address, count):
        assert parttype == "user"
        self.file.seek(address)
        data = self.file.read(count)
        if self.corrupt_next:
            self.corrupt_next = False
            data = bytes([data[0]^1]) + data[1:]
        return data

    def write(self, parttype, address, data):
        assert parttype == "user" and START <= address < address+len(data) <= START+SIZE
        self.writes.append((address, len(data)))
        self.file.seek(address)
        if self.failure == "disconnect" and not self.failed and len(self.writes) == 2:
            self.failed = True
            self.file.write(data[:4096])
            self.file.flush()
            raise OSError("simulated interrupted transfer")
        self.file.write(data)
        self.file.flush()
        if self.failure == "readback" and not self.failed:
            self.failed = True
            self.corrupt_next = True

    def check(self, expected):
        assert self.read("user", START, SIZE) == expected
        assert self.read("user", START-4096, 4096) == b"L"*4096
        assert self.read("user", START+SIZE, 4096) == b"R"*4096
        assert all(a % 4096 == 0 and n % 4096 == 0 and 0 < n <= mod.BLOCK for a,n in self.writes)

    def close(self):
        self.file.close()


def expect_error(fn):
    try:
        fn()
    except mod.InstallError:
        return
    raise AssertionError("Expected the operation to be rejected")


def run():
    passed = []
    with tempfile.TemporaryDirectory(prefix="t70-boot-installer-") as temp:
        disk = Disk(Path(temp)/"ufs-test.img")
        log = []
        scope = mod.BootOnly(disk, R)
        mod.install_or_restore(scope, ORIGINAL, PAYLOAD, log.append)
        disk.check(PAYLOAD)
        assert len(disk.writes) == 4
        assert sum(e["result"] == "write_intent" for e in log) == 4
        assert log[-1]["result"] == "all_bytes_verified"
        passed.append("actual_regular_file_install_and_full_readback")
        disk.writes.clear()
        mod.install_or_restore(mod.BootOnly(disk, R), ORIGINAL, PAYLOAD, log.append)
        assert not disk.writes
        disk.check(PAYLOAD)
        passed.append("identical_install_skips_all_writes")
        mod.install_or_restore(mod.BootOnly(disk, R), ORIGINAL, PAYLOAD, log.append, restoring=True)
        disk.check(ORIGINAL)
        passed.append("actual_regular_file_stock_restore")
        disk.close()
        for failure in ("disconnect", "readback"):
            disk = Disk(Path(temp)/"ufs-test.img", failure)
            log = []
            expect_error(lambda: mod.install_or_restore(mod.BootOnly(disk, R), ORIGINAL, PAYLOAD, log.append))
            disk.check(ORIGINAL)
            assert log[-1]["phase"] == "automatic_restore"
            assert log[-1]["result"] == "all_bytes_verified"
            passed.append(failure + "_restores_original_bytes")
            disk.close()
        disk = Disk(Path(temp)/"ufs-test.img")
        disk.file.seek(START)
        disk.file.write(b"x"*4096)
        disk.file.flush()
        expect_error(lambda: mod.install_or_restore(mod.BootOnly(disk, R), ORIGINAL, PAYLOAD, lambda _: None))
        assert not disk.writes
        passed.append("unknown_current_boot_rejected_without_write")
        # Explicit recovery still works when Android or the modified image cannot boot.
        mod.install_or_restore(mod.BootOnly(disk, R), ORIGINAL, PAYLOAD, lambda _: None, restoring=True)
        disk.check(ORIGINAL)
        passed.append("explicit_restore_accepts_damaged_boot")
        scope = mod.BootOnly(disk, R)
        for part, address, count in [("boot1",START,4096),("user",START-4096,4096),
                                     ("user",START+SIZE,4096),("user",START+1,4096),
                                     ("user",START,4095),("user",START,0),
                                     ("user",START,mod.BLOCK+4096)]:
            before = len(disk.writes)
            expect_error(lambda: scope.write(part, address, bytes(count)))
            assert len(disk.writes) == before
        passed.append("all_outside_or_unaligned_writes_blocked")
        expect_error(lambda: mod.BootOnly(disk, dict(R, name="boot_b")))
        expect_error(lambda: mod.program(scope, PAYLOAD[:-4096], lambda _: None, "install"))
        passed.append("wrong_partition_and_short_image_blocked")
        disk.close()

        # Persistent BCB recovery requests are read only and must stop installation.
        guard_path = Path(temp)/"guards.img"
        header = bytearray(4096)
        control = bytearray(32)
        control[:4] = b"_a\0\0"
        struct.pack_into("<I",control,4,0x42414342)
        control[8:10] = bytes([1,2])
        control[12],control[14] = 0x8f,0x0f
        struct.pack_into("<I",control,28,zlib.crc32(control[:28]))
        header[2048:2080] = control
        mod.check_boot_control(bytes(header))
        guard_path.write_bytes(header+bytes(len(mod.GUARDS)*4096))
        class GuardBackend:
            def read(self, parttype, address, count):
                assert parttype == "user"
                with guard_path.open("rb") as f:
                    f.seek(address)
                    return f.read(count)
        plan = {"gpt_regions": [], "partitions": {"misc": {"offset_bytes": 0}}}
        for i,name in enumerate(mod.GUARDS,1):
            plan["partitions"][name] = {"parttype": "user", "offset_bytes": i*4096,
                                        "size_bytes": 4096, "sha256": mod.digest(bytes(4096))}
        mod.check_guards(GuardBackend(), None, plan, None)
        with guard_path.open("r+b") as f:
            f.write(b"boot-recovery")
        expect_error(lambda: mod.check_guards(GuardBackend(), None, plan, None))
        mod.check_guards(GuardBackend(), None, plan, None, restoring=True)
        passed.append("persistent_recovery_command_blocks_install_but_allows_stock_restore")
        for offset,value in ((4,0),(8,2),(9,1)):
            bad = bytearray(header)
            bad[2048+offset] = value
            struct.pack_into("<I",bad,2048+28,zlib.crc32(bad[2048:2048+28]))
            expect_error(lambda: mod.check_boot_control(bytes(bad)))
        bad = bytearray(header)
        bad[2048+28] ^= 1
        expect_error(lambda: mod.check_boot_control(bytes(bad)))
        passed.append("invalid_AB_magic_version_count_and_crc_rejected")
        for offset,value in ((12,0x80),(12,0x0f),(13,1),(14,0x0f)):
            bad = bytearray(header)
            bad[2048+offset] = value
            if offset == 14:
                bad[2048+12] = 0x8e
            struct.pack_into("<I",bad,2048+28,zlib.crc32(bad[2048:2048+28]))
            expect_error(lambda: mod.check_boot_control(bytes(bad)))
        passed.append("slot_B_priority_or_unconfirmed_corrupt_A_rejected")
        bad = bytearray(header)
        bad[2048+28] ^= 1
        guard_path.write_bytes(bad+bytes(len(mod.GUARDS)*4096))
        mod.check_guards(GuardBackend(),None,plan,None,restoring=True)
        passed.append("emergency_stock_restore_remains_available_with_bad_AB_metadata")
        with guard_path.open("r+b") as f:
            f.seek(0)
            f.write(header)
            f.seek(plan["partitions"]["dtbo_a"]["offset_bytes"])
            f.write(b"x")
        expect_error(lambda: mod.check_guards(GuardBackend(), None, plan, None))
        expect_error(lambda: mod.check_guards(GuardBackend(), None, plan, None, restoring=True))
        passed.append("changed_board_firmware_blocks_boot_only_install_and_restore")

        base = {"ro.boot.slot_suffix": "_a", "ro.boot.flash.locked": "0",
                "ro.boot.vbmeta.device_state": "unlocked", "ro.boot.verifiedbootstate": "orange",
                "ro.build.fingerprint": "test/build", "sys.boot_completed": "1"}
        adb = Path(temp)/"adb-fixture"
        values = Path(temp)/"properties.json"
        adb.write_text("#!/usr/bin/env python3\nimport json,sys\nfrom pathlib import Path\n"
                       "assert sys.argv[1:4] == ['-d','shell','getprop']\n"
                       "print(json.loads(Path(__file__).with_name('properties.json').read_text())[sys.argv[4]])\n")
        adb.chmod(0o700)
        values.write_text(json.dumps(base))
        assert mod.android_state(adb, {"android_fingerprint": "test/build"}) == base
        passed.append("adb_reads_only_six_approved_properties")
        for key, value in [("ro.boot.slot_suffix","_b"),("ro.boot.flash.locked","1"),
                           ("ro.boot.vbmeta.device_state","locked"),("ro.boot.verifiedbootstate","green"),
                           ("ro.build.fingerprint","different/build"),("sys.boot_completed","0")]:
            values.write_text(json.dumps(dict(base, **{key:value})))
            expect_error(lambda: mod.android_state(adb, {"android_fingerprint": "test/build"}))
        passed.append("slot_lock_version_and_boot_state_mismatches_rejected")
    return {"schema": 1, "device_access": False, "physical_DA_write_verified": False,
            "regular_file_write_and_readback_verified": True, "passed": passed, "count": len(passed),
            "installer_source_sha256": mod.digest(Path(__file__).with_name("boot-install.py").read_bytes()),
            "test_source_sha256": mod.digest(Path(__file__).read_bytes())}


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--report", type=Path)
    a = p.parse_args()
    result = run()
    if a.report:
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
