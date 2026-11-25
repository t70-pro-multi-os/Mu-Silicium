#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Boot the raw image from a read-only emulated USB; require an interactive RAM shell."""
import argparse
from pathlib import Path
import select
import re
import subprocess
import sys
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("image", type=Path)
p.add_argument("serial_log", type=Path)
p.add_argument("--check-modules", action="store_true", help="load modules only in the disposable VM")
p.add_argument("--firmware", default="/usr/share/qemu-efi-aarch64/QEMU_EFI.fd")
x = p.parse_args()
args = [
    "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a76", "-smp", "2", "-m", "1024",
    "-accel", "tcg", "-bios", x.firmware, "-display", "none", "-serial", "stdio",
    "-monitor", "none", "-no-reboot", "-nic", "none",
    "-drive", f"if=none,id=liveusb,file={x.image.resolve(strict=True)},format=raw,readonly=on",
    "-device", "qemu-xhci,id=xhci", "-device", "usb-storage,drive=liveusb,bootindex=1",
]
qemu = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
assert qemu.stdin and qemu.stdout
data = b""
deadline = time.monotonic() + 70
next_send = 0
ready = b"T70_LIVE_READY: RAM root and eight embedded DKMS files verified"
shell = b"T70_SHELL_VERIFIED"
mods = (
    "t70_hx83132", "t70_mt6983_i2c", "t70_mt6789_ext_sd", "t70-ccmni",
    "t70_afe_i2s_slice", "t70_mt6789_aw88261", "t70-scp-accel", "t70-stp-hci",
)
module_end = b"ADMISSION_END"
module_sent = False

try:
    with x.serial_log.open("xb") as log:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if ready in data and shell not in data and now >= next_send:
                qemu.stdin.write(b"printf 'T70_SHELL_%s\\n' VERIFIED\n")
                qemu.stdin.flush()
                next_send = now + 1
            if not select.select([qemu.stdout], [], [], 0.2)[0]:
                continue
            chunk = qemu.stdout.read1(65536)
            if not chunk:
                break
            log.write(chunk)
            log.flush()
            data += chunk
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if x.check_modules and shell in data and not module_sent:
                command = (
                    "for m in " + " ".join(mods) +
                    "; do n=$(printf '%s' \"$m\" | /bin/busybox tr '-' '_'); "
                    "if modprobe \"$m\" && grep -q \"^$n \" /proc/modules; "
                    "then printf '\\nADMITTED:%s\\n' \"$m\"; "
                    "else printf '\\nREJECTED:%s\\n' \"$m\"; fi; done; "
                    "printf '\\nADMISSION_%s\\n' END\n"
                )
                qemu.stdin.write(command.encode())
                qemu.stdin.flush()
                module_sent = True
            if ((module_end if x.check_modules else shell) in data
                    or b"No bootable option or device was found" in data or b"T70_LIVE_ERROR:" in data):
                break
finally:
    if qemu.poll() is None:
        qemu.terminate()
    qemu.wait(timeout=10)
if not all(x in data for x in (b"starting Boot", b"USB HARDDRIVE", ready, shell)):
    raise SystemExit("UEFI USB boot did not verify a responsive RAM shell")
if x.check_modules:
    admitted = [m.decode() for m in re.findall(rb"(?m)^ADMITTED:([-\w]+)\r?$", data)]
    if module_end not in data or tuple(admitted) != mods:
        raise SystemExit(f"guest module admission incomplete: {admitted!r}")
    print(f"\nGuest module registration: {len(admitted)}/{len(mods)}")
