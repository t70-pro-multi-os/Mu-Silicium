#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the MSDC command-transport tests with simulated registers."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="t70-sd-command-") as tmp:
    exe = Path(tmp) / "command-test"
    subprocess.run([
        os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(root / "SdCommand.c"), str(root / "SdCommandTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
