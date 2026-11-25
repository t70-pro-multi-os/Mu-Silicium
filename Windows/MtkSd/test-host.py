#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise clock transitions and reset timing using MSDC register mocks."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="t70-sd-host-") as temp:
    exe = Path(temp) / "host-test"
    subprocess.run([
        os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(root / "SdHost.c"), str(root / "SdHostTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
