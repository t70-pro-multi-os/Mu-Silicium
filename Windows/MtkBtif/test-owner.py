#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Opt-in native callback tests only; no driver packaging, installation or MMIO."""
from pathlib import Path
import os
import subprocess
import tempfile
here = Path(__file__).resolve().parent
silicon = here.parents[1] / "Silicon/MediaTek/MediaTekPkg"
core = silicon / "Library/MtkStpCoreLib"
with tempfile.TemporaryDirectory(prefix="t70-btif-owner-") as tmp:
    out = Path(tmp) / "owner-test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-multichar", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-I" + str(here / "Tests/include"),
        "-I" + str(here.parent / "MtkConn/Tests/include"), "-I" + str(silicon / "Include"),
        "-include", str(here / "Tests/include/wdf.h"),
        str(here / "Tests/OwnerTest.c"), str(here / "HciTransport.c"),
        str(core / "MtkBtifCore.c"),
        str(core / "MtkStpCore.c"), str(core / "MtkStpFull.c"),
        str(core / "MtkWmtMode.c"), "-o", str(out)], check=True)
    subprocess.run([str(out)], check=True)
