#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Reject incomplete or legacy MediaTek Paris DKMS inputs before compilation."""
import argparse
from pathlib import Path
import re

EXPORTS = frozenset((
    "debounce_time_mt6878",
    "mtk_paris_pinctrl_probe",
    "mtk_paris_pinctrl_pm_ops",
    "mtk_pinconf_bias_get_combo",
    "mtk_pinconf_bias_set_combo",
    "mtk_pinconf_drive_get_raw",
    "mtk_pinconf_drive_set_raw",
))
HEADERS = ("pinctrl-paris.h", "pinctrl-mtk-common-v2.h", "mtk-eint.h")


def check(kernel, source):
    release = (kernel / "include/config/kernel.release").read_text().strip()
    if not re.fullmatch(r"7\.0\.[0-9]+(?:[-+].+)?", release):
        raise ValueError(f"unsupported target kernel release {release!r}")
    exports = {}
    for row in (kernel / "Module.symvers").read_text().splitlines():
        cols = row.split()
        if len(cols) >= 4 and cols[1] in EXPORTS:
            if cols[1] in exports:
                raise ValueError(f"ambiguous target export: {cols[1]}")
            exports[cols[1]] = cols[0]
    if EXPORTS - exports.keys():
        raise ValueError("target lacks Paris exports: " + ", ".join(sorted(EXPORTS - exports.keys())))

    header = source / HEADERS[0] if source else None
    if header is None or not header.is_file():
        raise ValueError(f"{release} exports the required functions/data, but its private Paris headers "
                         "are missing; point T70_PARIS_INCLUDE to the matching target source")
    decl = re.sub(r"/\*.*?\*/|//[^\n]*", "", header.read_text(), flags=re.S)
    if not re.search(r"\bint\s+mtk_paris_pinctrl_probe\s*\(\s*"
                     r"struct\s+platform_device\s*\*\s*\w+(?:\s*)\)\s*;", decl):
        raise ValueError("the Paris probe declaration must take one platform_device argument; "
                         "the Android vendor header is not ABI-compatible")
    missing = [name for name in HEADERS if not (source / name).is_file()
               or not (source / name).stat().st_size]
    if missing:
        raise ValueError("missing private Paris headers: " + ", ".join(missing))
    print(f"Paris preflight: {release}, {len(exports)} exports, one-argument probe and three headers; "
          "use headers from the exact target source package (preflight is not ABI proof)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel_build", type=Path)
    parser.add_argument("--paris-include", default="")
    args = parser.parse_args()
    try:
        check(args.kernel_build, Path(args.paris_include) if args.paris_include else None)
    except (OSError, ValueError) as err:
        parser.exit(2, f"Paris preflight refused: {err}\n")
