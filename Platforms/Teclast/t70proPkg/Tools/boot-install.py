#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""T70 Pro G100 boot_a trial installer. No USB access without --execute."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import struct
import zlib

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
DEFAULT_KIT = Path("/Users/you/.tmp/teclast-t70pro/device-backup/recovery-kit")
BLOCK = 16 * 1024 * 1024
TARGET = {"name": "boot_a", "parttype": "user", "offset_bytes": 715128832,
          "size_bytes": 67108864}
GUARDS = ("lk_a", "init_boot_a", "vendor_boot_a", "dtbo_a", "vbmeta_a")
PROPERTIES = ("ro.boot.slot_suffix", "ro.boot.flash.locked", "ro.boot.vbmeta.device_state",
              "ro.boot.verifiedbootstate", "ro.build.fingerprint", "sys.boot_completed")


class InstallError(RuntimeError):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(BLOCK), b""):
            h.update(data)
    return h.hexdigest()


def load_json(path):
    return json.loads(path.read_text())


def local_files(bundle, kit):
    manifest = load_json(bundle/"manifest.json")
    if manifest.get("schema") != 1 or manifest.get("installation_class") != "hardware_trial":
        raise InstallError("지원하지 않는 설치 묶음입니다.")
    if manifest.get("target") != TARGET or manifest.get("active_slot_required") != "_a":
        raise InstallError("이 도구는 검증한 원래 주소의 boot_a만 지원합니다.")
    for name in ("restore.py", "plan.json", "source-chunks.json", "kit-files.json"):
        if file_hash(kit/name) != manifest["recovery_files"].get(name):
            raise InstallError("복구 도구의 기준 파일이 달라졌습니다: " + name)
    for name, entry in manifest["files"].items():
        path = (bundle/name).resolve()
        if not path.is_relative_to(bundle.resolve()) or not path.is_file():
            raise InstallError("설치 파일 경로가 올바르지 않습니다.")
        if path.stat().st_size != entry["bytes"] or file_hash(path) != entry["sha256"]:
            raise InstallError("설치 파일 검증에 실패했습니다: " + name)
    if manifest["payload"] != "boot.img" or manifest["files"]["boot.img"]["bytes"] != TARGET["size_bytes"]:
        raise InstallError("boot 이미지 크기 또는 이름이 다릅니다.")
    spec = importlib.util.spec_from_file_location("sealed_t70_recovery", kit/"restore.py")
    recovery = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(recovery)
    try:
        recovery.check_integrity()
    except recovery.RecoveryError as exc:
        raise InstallError(str(exc)) from None
    plan = load_json(kit/"plan.json")
    target = plan["partitions"]["boot_a"]
    if any(target.get(k) != v for k, v in TARGET.items()):
        raise InstallError("복구 계획의 boot_a 주소가 다릅니다.")
    if target["sha256"] != manifest["original_boot_sha256"]:
        raise InstallError("순정 boot 기준이 다릅니다.")
    if manifest["guard_sha256"] != {name: plan["partitions"][name]["sha256"] for name in GUARDS}:
        raise InstallError("순정 부팅 구성 요소의 기준이 다릅니다.")
    recovery.validate_regions(plan, [target])
    return manifest, recovery, plan, load_json(kit/"source-chunks.json")


def android_state(adb, manifest):
    result = {}
    for prop in PROPERTIES:
        try:
            p = subprocess.run([str(adb), "-d", "shell", "getprop", prop],
                               capture_output=True, text=True, timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            raise InstallError("ADB 연결을 확인하지 못했습니다.") from None
        if p.returncode or len(p.stdout) > 4096:
            raise InstallError("ADB 속성을 읽지 못했습니다. USB 디버깅 허용 상태를 확인하세요.")
        result[prop] = p.stdout.strip()
    if result["ro.boot.slot_suffix"] != "_a":
        raise InstallError("현재 슬롯이 A가 아닙니다. 슬롯을 바꾸지 않고 중지합니다.")
    if result["ro.boot.flash.locked"] != "0" or result["ro.boot.vbmeta.device_state"] != "unlocked":
        raise InstallError("언락 상태가 확인되지 않았습니다. 이 도구는 언락하지 않습니다.")
    if result["ro.boot.verifiedbootstate"] != "orange" or result["sys.boot_completed"] != "1":
        raise InstallError("언락된 Android의 정상 부팅 상태가 확인되지 않았습니다.")
    if result["ro.build.fingerprint"] != manifest["android_fingerprint"]:
        raise InstallError("Android 버전이 준비한 순정 이미지와 다릅니다.")
    return result


def read_region(backend, region):
    data = bytearray()
    for offset in range(0, region["size_bytes"], BLOCK):
        length = min(BLOCK, region["size_bytes"]-offset)
        block = backend.read(region["parttype"], region["offset_bytes"]+offset, length)
        if len(block) != length:
            raise InstallError("기기에서 필요한 길이를 읽지 못했습니다.")
        data.extend(block)
    return bytes(data)


def check_boot_control(message):
    if len(message) != 4096:
        raise InstallError("misc 부팅 정보를 필요한 길이만큼 읽지 못했습니다.")
    control = message[2048:2080]
    magic, = struct.unpack_from("<I", control, 4)
    crc, = struct.unpack_from("<I", control, 28)
    if (magic != 0x42414342 or control[8] != 1 or (control[9] & 7) != 2 or
            zlib.crc32(control[:28]) != crc):
        raise InstallError("A/B 슬롯 정보 형식 또는 검사값이 다릅니다.")
    priority_a, priority_b = control[12] & 15, control[14] & 15
    if (not priority_a or priority_a < priority_b or not (control[12] & 0x80) or control[13] & 1):
        raise InstallError("정상 부팅이 확인된 A 슬롯이 LK 기본 선택이 아닙니다. 슬롯은 변경하지 않습니다.")


def check_guards(backend, recovery, plan, index, *, restoring=False):
    # A persistent recovery command could otherwise keep selecting UEFI on every boot.
    # Restoring stock boot must remain available even when such a command is pending.
    if not restoring:
        misc = plan["partitions"]["misc"]
        message = backend.read("user", misc["offset_bytes"], 4096)
        if len(message) != 4096 or message[0] != 0:
            raise InstallError("misc에 남은 부팅 명령이 있습니다. 자동으로 지우지 않고 중지합니다.")
        check_boot_control(message)
    for region in plan["gpt_regions"]:
        for address, original in recovery.fragments(region, index):
            if backend.read(region["parttype"], address, len(original)) != original:
                raise InstallError("현재 파티션 표가 원래 구성과 다릅니다.")
    for name in GUARDS:
        if digest(read_region(backend, plan["partitions"][name])) != plan["partitions"][name]["sha256"]:
            raise InstallError("현재 부팅 구성 요소가 순정 기준과 다릅니다: " + name)


class BootOnly:
    """All writes cross this fixed, aligned boot_a-only boundary."""
    def __init__(self, backend, region):
        if any(region.get(k) != v for k, v in TARGET.items()):
            raise InstallError("허용되지 않은 쓰기 영역입니다.")
        self.backend, self.region, self.write_started = backend, region, False

    def check(self, parttype, address, count):
        r = self.region
        if (parttype != "user" or type(address) is not int or type(count) is not int or
                address % 4096 or count % 4096 or not 0 < count <= BLOCK or
                address < r["offset_bytes"] or address+count > r["offset_bytes"]+r["size_bytes"]):
            raise InstallError("boot_a 밖으로 나가는 쓰기를 차단했습니다.")

    def read(self, parttype, address, count):
        self.check(parttype, address, count)
        result = self.backend.read(parttype, address, count)
        if not isinstance(result, (bytes, bytearray)) or len(result) != count:
            raise InstallError("기기 읽기 길이가 다릅니다.")
        return bytes(result)

    def write(self, parttype, address, data):
        self.check(parttype, address, len(data))
        self.write_started = True
        self.backend.write(parttype, address, data)


def program(device, data, journal, phase):
    r = device.region
    if len(data) != r["size_bytes"]:
        raise InstallError("쓰기 이미지 크기가 다릅니다.")
    for offset in range(0, len(data), BLOCK):
        block = data[offset:offset+BLOCK]
        address = r["offset_bytes"]+offset
        if device.read("user", address, len(block)) == block:
            action = "already_matches"
        else:
            journal({"phase": phase, "result": "write_intent", "offset_bytes": address,
                     "bytes": len(block), "sha256": digest(block)})
            device.write("user", address, block)
            if device.read("user", address, len(block)) != block:
                raise InstallError("쓴 boot 내용을 다시 읽어 확인하는 데 실패했습니다.")
            action = "written_and_verified"
        journal({"phase": phase, "result": action, "offset_bytes": address,
                 "bytes": len(block), "sha256": digest(block)})
    if digest(read_region(device, r)) != digest(data):
        raise InstallError("boot 전체를 다시 읽은 해시가 다릅니다.")
    journal({"phase": phase, "result": "all_bytes_verified", "sha256": digest(data)})


def install_or_restore(device, original, payload, journal, restoring=False):
    """On a failed install, restore the checked original on the same connection."""
    if restoring:
        program(device, original, journal, "restore")
        return
    current = read_region(device, device.region)
    if digest(current) not in (digest(original), digest(payload)):
        raise InstallError("현재 boot_a가 순정 또는 이 설치 이미지와 다릅니다. 먼저 복구 명령을 사용하세요.")
    try:
        program(device, payload, journal, "install")
    except BaseException:
        if device.write_started:
            try:
                program(device, original, journal, "automatic_restore")
            except BaseException:
                journal({"phase": "automatic_restore", "result": "failed_reconnect_and_restore_required"})
                raise InstallError("설치와 자동 복구가 완료되지 않았습니다. 케이블을 다시 연결한 뒤 restore 명령을 실행하세요.") from None
            raise InstallError("설치를 중지하고 순정 boot_a를 다시 써서 확인했습니다. Android 부팅은 별도로 확인하세요.") from None
        raise


def main():
    os.umask(0o077)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--bundle", type=Path, default=HERE)
    p.add_argument("--kit", type=Path, default=DEFAULT_KIT)
    p.add_argument("--adb", type=Path, default=Path(shutil.which("adb") or "/opt/homebrew/bin/adb"))
    p.add_argument("command", choices=("plan", "doctor", "install", "restore"))
    p.add_argument("--execute", action="store_true")
    p.add_argument("--reboot", action="store_true")
    a = p.parse_args()
    if a.command in ("plan", "doctor") and (a.execute or a.reboot):
        raise InstallError("plan과 doctor에서는 실행 옵션을 사용할 수 없습니다.")
    bundle, kit = a.bundle.resolve(), a.kit.resolve()
    manifest, recovery, plan, index = local_files(bundle, kit)
    print(json.dumps({"installation_class": manifest["installation_class"], "target": TARGET,
                      "payload_sha256": manifest["files"]["boot.img"]["sha256"],
                      "original_sha256": manifest["original_boot_sha256"],
                      "write_requested": a.execute, "hardware_boot_verified": False,
                      "automatic_relock": False, "userdata_modified": False}, ensure_ascii=False, indent=2))
    if a.command == "doctor":
        recovery.import_mtk()
        print("복구 모듈을 불러왔습니다. USB 연결과 기기 쓰기는 실행하지 않았습니다.")
        return
    if a.command == "plan" or not a.execute:
        print("계획만 표시했습니다. USB 연결과 기기 쓰기는 실행하지 않았습니다.")
        return
    target = plan["partitions"]["boot_a"]
    # Selected chunks only; no userdata, calibration, DRM or RPMB content is read.
    original = b"".join(data for _, data in recovery.fragments(target, index))
    if len(original) != TARGET["size_bytes"] or digest(original) != manifest["original_boot_sha256"]:
        raise InstallError("순정 boot 원본의 검증에 실패했습니다.")
    payload = (bundle/"boot.img").read_bytes()
    if digest(payload) != manifest["files"]["boot.img"]["sha256"]:
        raise InstallError("설치 이미지가 검사 후 달라졌습니다.")
    state = None if a.command == "restore" else android_state(a.adb, manifest)
    runs = bundle/"runs"
    runs.mkdir(exist_ok=True)
    run = runs/("boot-"+str(time.time_ns())+".jsonl")
    with run.open("x") as log:
        def journal(entry):
            log.write(json.dumps(entry, ensure_ascii=False)+"\n")
            log.flush()
            os.fsync(log.fileno())
        journal({"phase": "preflight", "command": a.command, "target": TARGET,
                 "manifest_sha256": file_hash(bundle/"manifest.json"),
                 "original_sha256": digest(original), "payload_sha256": digest(payload),
                 "android_state": state})
        print("태블릿 전원을 끄고 USB를 연결하세요. 제조사 preloader 연결을 90초 기다립니다.", flush=True)
        print("진행 기록:", run, flush=True)
        try:
            with recovery.Device(plan) as backend:
                check_guards(backend, recovery, plan, index, restoring=a.command == "restore")
                scoped = BootOnly(backend, target)
                install_or_restore(scoped, original, payload, journal, a.command == "restore")
                journal({"phase": "complete", "result": "requested_boot_bytes_verified",
                         "android_boot_verified": False})
                if a.reboot:
                    backend.reboot()
        except recovery.RecoveryError as exc:
            raise InstallError(str(exc)) from None
        except TimeoutError:
            raise InstallError("제조사 preloader 연결 대기 시간이 지났습니다. 전원을 끄고 다시 연결하세요.") from None
    print("boot_a 쓰기와 전체 재확인을 마쳤습니다. 실제 부팅과 순정 복귀는 기기 화면에서 확인하세요.")


if __name__ == "__main__":
    try:
        main()
    except (Exception, KeyboardInterrupt) as exc:
        # Raw library errors can contain identifiers. Only our own messages are printable.
        print("중지:", str(exc) if isinstance(exc, InstallError) else type(exc).__name__, file=sys.stderr)
        sys.exit(1)
