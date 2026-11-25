#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""T70 Pro 순정 fastboot로 EDK2 RAM 부팅을 시험합니다."""

import argparse
import fcntl
import hashlib
from pathlib import Path
import subprocess
import tempfile
import time

import usb.core
import usb.util


PRODUCT = b"tb8781p1_64_td89"
OKAY, FAIL, DATA = b"OKAY", b"FAIL", b"DATA"
MAX_IMAGE_BYTES = 16 * 1024 * 1024


def adb_output(*args):
    try:
        result = subprocess.run(
            ["adb", *args], capture_output=True, text=True, timeout=7,
        )
        return result.stdout.strip() if result.returncode == 0 else None
    except subprocess.TimeoutExpired:
        return None


def property_value(serial, name):
    return adb_output("-s", serial, "shell", "getprop", name)


def android_target():
    devices = []
    for line in (adb_output("devices", "-l") or "").splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] == "device" and "model:T70Pro_ROW" in fields:
            if any(field.startswith("usb:") for field in fields[2:]):
                devices.append(fields[0])
    if len(devices) != 1:
        raise RuntimeError("ADB로 연결된 T70 Pro 한 대가 필요합니다.")
    return devices[0]


def tablet_usb():
    return list(usb.core.find(find_all=True, idVendor=0x0E8D, idProduct=0x201C))


def usb_port(device):
    return device.bus, device.port_numbers


def usb_interface(device, protocol):
    return [
        interface for config in device for interface in config
        if (interface.bInterfaceClass, interface.bInterfaceSubClass,
            interface.bInterfaceProtocol) == (0xFF, 0x42, protocol)
    ]


def wait_for(check, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.25)
    return None


def stock_fastboot(port):
    for device in tablet_usb():
        if usb_port(device) == port:
            try:
                interfaces = usb_interface(device, 3)
            except usb.core.USBError:
                continue
            if len(interfaces) == 1:
                return device, interfaces[0]
    return None


class Fastboot:
    def __init__(self, interface):
        endpoints = {}
        for endpoint in interface:
            if usb.util.endpoint_type(endpoint.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK:
                endpoints[usb.util.endpoint_direction(endpoint.bEndpointAddress)] = endpoint
        if set(endpoints) != {usb.util.ENDPOINT_IN, usb.util.ENDPOINT_OUT}:
            raise RuntimeError("fastboot의 USB 송수신 경로를 확인할 수 없습니다.")
        self.input = endpoints[usb.util.ENDPOINT_IN]
        self.output = endpoints[usb.util.ENDPOINT_OUT]

    def write(self, data, timeout=1800):
        if self.output.write(data, timeout=timeout) != len(data):
            raise RuntimeError("fastboot 전송이 중간에 끊겼습니다.")

    def reply(self, expected=(OKAY, FAIL), timeout=1800):
        for _ in range(8):
            data = bytes(self.input.read(256, timeout=timeout))
            status = data[:4]
            if status in expected:
                return status, data[4:]
            if status not in (b"INFO", b"TEXT"):
                break
        raise RuntimeError("fastboot 응답을 확인할 수 없습니다.")

    def command(self, data):
        self.write(data)
        return self.reply()

    def download(self, image):
        size = f"{len(image):08x}".encode("ascii")
        self.write(b"download:" + size)
        reply = self.reply((DATA, FAIL))
        if reply[0] == FAIL:
            return False
        if reply != (DATA, size):
            raise RuntimeError("기기가 요청한 RAM 전송 크기가 이미지와 다릅니다.")
        deadline = time.monotonic() + 15
        for start in range(0, len(image), 16384):
            remaining_ms = int(1000 * (deadline - time.monotonic()))
            if remaining_ms <= 0:
                raise RuntimeError("RAM 이미지 전송 시간이 초과됐습니다.")
            self.write(image[start:start + 16384], min(1800, remaining_ms))
        return self.reply(timeout=4000)[0] == OKAY


def request_boot(transport, image, slot):
    if transport.command(b"getvar:product") != (OKAY, PRODUCT):
        raise RuntimeError("fastboot 제품명이 T70 Pro와 일치하지 않습니다.")
    expected = (b"no", b"yes", slot[1:].encode("ascii"))
    actual = tuple(
        transport.command(b"getvar:" + name)
        for name in (b"is-userspace", b"unlocked", b"current-slot")
    )
    if actual != tuple((OKAY, value) for value in expected):
        print("순정 fastboot 모드, 잠금 상태 또는 슬롯이 예상과 다릅니다.", flush=True)
    elif transport.download(image):
        print(f"RAM 이미지 전송 완료 ({len(image):,}바이트)", flush=True)
        status, message = transport.command(b"boot")
        if status == OKAY:
            print("부팅 요청이 수락됐습니다. UEFI 실행 여부는 기기 화면에서 확인합니다.", flush=True)
            return True
        if message == b"unknown command":
            print("순정 부트로더 응답은 FAIL unknown command입니다.", flush=True)
        elif message == b"No support by security control":
            print("순정 부트로더가 보안 설정에 따라 부팅 요청을 거절했습니다.", flush=True)
        else:
            print("순정 부트로더가 부팅 요청을 거절했습니다.", flush=True)
    else:
        print("순정 부트로더가 RAM 이미지 전송을 거절했습니다.", flush=True)
    if transport.command(b"reboot")[0] == OKAY:
        print("Android 재시작 요청이 수락됐습니다.", flush=True)
    return False


def trial(image):
    serial = android_target()
    slot = property_value(serial, "ro.boot.slot_suffix")
    build = property_value(serial, "ro.build.version.incremental")
    if (slot not in ("_a", "_b") or not build
            or property_value(serial, "ro.product.device") != "T70Pro"
            or property_value(serial, "ro.boot.flash.locked") != "0"
            or property_value(serial, "sys.boot_completed") != "1"):
        raise RuntimeError("기기의 Android 부팅, 슬롯, 잠금 상태를 확인할 수 없습니다.")
    devices = tablet_usb()
    if len(devices) != 1 or len(usb_interface(devices[0], 1)) != 1 or not devices[0].port_numbers:
        raise RuntimeError("macOS에서 T70 Pro USB에 접근할 수 있는지 확인하세요.")
    port = usb_port(devices[0])

    print("T70 Pro를 순정 부트로더로 재시작합니다.", flush=True)
    try:
        if adb_output("-s", serial, "reboot", "bootloader") is None:
            raise RuntimeError("ADB 재시작 응답을 확인할 수 없습니다.")
        match = wait_for(lambda: stock_fastboot(port), 30)
        if not match:
            raise RuntimeError("원래 USB 포트에서 fastboot를 찾을 수 없습니다.")
        device, interface = match
        try:
            usb.util.claim_interface(device, interface.bInterfaceNumber)
            if request_boot(Fastboot(interface), image, slot):
                return 0
        finally:
            usb.util.dispose_resources(device)
    except usb.core.USBError:
        print("fastboot USB 연결이 끊겼거나 응답 시간이 초과됐습니다.", flush=True)
    except RuntimeError as error:
        print(error, flush=True)

    def android_returned():
        return (property_value(serial, "sys.boot_completed") == "1"
                and property_value(serial, "ro.boot.slot_suffix") == slot
                and property_value(serial, "ro.build.version.incremental") == build)

    if wait_for(android_returned, 100):
        print(f"Android 복귀 확인 ({build}, 슬롯 {slot[1:]})", flush=True)
        return 1
    print("Android 복귀 미확인", flush=True)
    return 2


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="종료 코드 0은 부팅 요청 수락, 1은 부팅 실패 뒤 Android 확인, 2는 시험 상태 미확인입니다.",
    )
    parser.add_argument("image", type=Path, help="T70Pro-UEFI-Trial-boot.img")
    parser.add_argument("--sha256", required=True, help="부팅 이미지의 예상 SHA-256")
    args = parser.parse_args()
    try:
        with args.image.open("rb") as source:
            image = source.read(MAX_IMAGE_BYTES + 1)
        if (not 4096 <= len(image) <= MAX_IMAGE_BYTES or not image.startswith(b"ANDROID!")
                or hashlib.sha256(image).hexdigest() != args.sha256.lower()):
            raise RuntimeError("Android 부팅 이미지의 형식, 크기 또는 SHA-256이 다릅니다.")
        lock_path = Path(tempfile.gettempdir()) / "t70pro-stock-ram-boot.lock"
        with lock_path.open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return trial(image)
    except BlockingIOError:
        print("진행 중인 T70 Pro 시험이 있습니다.", flush=True)
    except usb.core.USBError:
        print("T70 Pro USB에 접근할 수 없습니다.", flush=True)
    except OSError:
        print("이미지 파일이나 ADB, 잠금 파일을 열 수 없습니다.", flush=True)
    except RuntimeError as error:
        print(error, flush=True)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
