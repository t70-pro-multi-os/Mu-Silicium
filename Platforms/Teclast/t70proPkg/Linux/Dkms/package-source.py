#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build a deterministic source-only DKMS tarball; touch neither DKMS nor /usr/src."""
import argparse
import gzip
import io
from pathlib import Path
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
PACKAGE = "t70-hx83132-0.1.3"
SILICON = "Silicon/MediaTek/MediaTekPkg"
CORE_REVISION = "c7848662f625d0fdb6df018a4a3b0b2158334740"
INPUTS = {
    "dkms.conf": HERE / "Himax/dkms.conf",
    "Makefile": HERE / "Himax/Makefile",
    "hx83132_linux.c": HERE / "Himax/hx83132_linux.c",
    "himax_release.h": HERE / "Himax/himax_release.h",
    "compat/stddef.h": HERE / "Himax/compat/stddef.h",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    files = {name: path.read_bytes() for name, path in INPUTS.items()}
    files["LICENSE.shared-core"] = subprocess.check_output(
        ["git", "-C", str(ROOT), "show", f"{CORE_REVISION}:LICENSE"])
    for name, path in (
            ("HimaxCore.c", "Library/HimaxCoreLib/HimaxCore.c"),
            ("include/Library/HimaxCore.h", "Include/Library/HimaxCore.h")):
        files[name] = subprocess.check_output(
            ["git", "-C", str(ROOT), "show", f"{CORE_REVISION}:{SILICON}/{path}"])
    with args.output.open("xb") as output, gzip.GzipFile(
            fileobj=output, filename="", mode="wb", mtime=0) as gz, tarfile.open(
                fileobj=gz, mode="w|") as archive:
        for name, data in sorted(files.items()):
            entry = tarfile.TarInfo(f"{PACKAGE}/{name}")
            entry.mode = 0o644
            entry.size = len(data)
            archive.addfile(entry, io.BytesIO(data))


if __name__ == "__main__":
    main()
