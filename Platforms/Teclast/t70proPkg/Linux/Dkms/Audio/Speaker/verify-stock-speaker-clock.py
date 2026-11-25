#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Verify the I2S3 MCLK request of the exact offline T70 machine object."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

MACHINE_SHA256 = "72257b21c2b559dd85a58848fe64f46cdf81ab26e6e9f70519c1c33fa5ae8e92"


def require(condition, label):
    if not condition:
        raise ValueError(label)


def verify(machine):
    require(hashlib.sha256(machine.read_bytes()).hexdigest() == MACHINE_SHA256,
            "not the pinned T70 stock machine module")
    relocations = subprocess.check_output(["readelf", "-rW", machine], text=True)
    # The three pointers occur in the same I2S3 DAI link. Its ops table
    # then references the hardware-params function at .text+0x4ac.
    sections = re.split(r"(?m)^Relocation section '([^']+)'[^\n]*\n", relocations)
    sections = dict(zip(sections[1::2], sections[2::2]))
    for section, offset, target, value in (
        (".rela.data", "27d8", ".data", "8a8"),   # i2s3_cpus
        (".rela.data", "27e8", ".data", "8c8"),   # i2s3_codecs
        (".rela.data", "2840", ".rodata", "370"), # i2s ops
        (".rela.rodata", "380", ".text", "4ac"),  # ops .hw_params
    ):
        require(re.search(rf"(?m)^0*{offset}\s+.*\bR_AARCH64_ABS64\b.*"
                          rf"{re.escape(target)} \+ {value}$", sections.get(section, "")),
                f"missing I2S3 link reference {section}+{offset}")
    callback = subprocess.check_output([
        "aarch64-linux-gnu-objdump", "-dr",
        "--disassemble=mt6789_mt6366_i2s_hw_params", machine,
    ], text=True)
    require(re.search(r"\bmov\s+w1, wzr", callback) and
            re.search(r"\blsl\s+w2, w9, #7", callback) and
            re.search(r"\bmov\s+w3, #0x1", callback) and
            "R_AARCH64_CALL26\tsnd_soc_dai_set_sysclk" in callback,
            "unexpected stock sysclk request")
    return {"stock_machine_sha256": MACHINE_SHA256, "link": "I2S3",
            "cpu_dai_sysclk_id": 0, "clock_direction": "out",
            "mclk_per_sample_rate": 128,
            "examples_hz": {"44100": 5644800, "48000": 6144000},
            "scope": "offline contract only; no resource, clock or hardware change"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock_machine", type=Path)
    opts = parser.parse_args()
    try:
        print(json.dumps(verify(opts.stock_machine), indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(2, f"T70 speaker MCLK verification refused: {error}\n")
