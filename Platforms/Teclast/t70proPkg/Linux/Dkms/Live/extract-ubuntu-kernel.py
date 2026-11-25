#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Extract the intact AArch64 Linux EFI kernel from Ubuntu's packaged wrapper."""
import argparse
from pathlib import Path

import pefile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("vmlinuz", type=Path)
p.add_argument("kernel", type=Path)
x = p.parse_args()
outer = pefile.PE(str(x.vmlinuz))
sections = [s for s in outer.sections if s.Name.rstrip(b"\0") == b".linux"]
if outer.FILE_HEADER.Machine != 0xAA64 or len(sections) != 1:
    p.error("expected one .linux section in an AArch64 EFI wrapper")
s = sections[0]
data = s.get_data(length=s.Misc_VirtualSize)
if len(data) != s.Misc_VirtualSize:
    p.error("truncated .linux section")
inner = pefile.PE(data=data)
if inner.FILE_HEADER.Machine != 0xAA64 or inner.OPTIONAL_HEADER.DATA_DIRECTORY[5].Size:
    p.error("inner kernel is not suitable for the ARM64 systemd stub")
with x.kernel.open("xb") as out:
    out.write(data)
