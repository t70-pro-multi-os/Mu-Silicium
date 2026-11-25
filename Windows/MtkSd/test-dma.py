#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the portable MSDC DMA tests with a simulated controller."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="t70-sd-dma-") as tmp:
    exe = Path(tmp) / "dma-test"
    subprocess.run([
        os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        str(root / "SdDma.c"), str(root / "SdDmaTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
