#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage hardware ADT using a completely UEFI-allocated initial RAM window."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
here = Path(__file__).resolve().parent
mu = here.parents[3]
project = mu.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--os", choices=("ios", "macos"), default="ios")
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
os_suffix = "-macos" if args.os == "macos" else ""
if not args.locked:
    os.execv(str(mu / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Prepare XNU runtime allocation ADT", "--",
              sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
sys.dont_write_bytecode = True
subprocess.run([sys.executable, str(here / "analyze-stock.py"), "--locked"], check=True)
sys.path.insert(0, str(project / "xnu-sources/UniXNU/scripts"))
from unixnu.devicetree import read_fdt, node, hardware_properties, arm_io_window, empty_nvram_proxy
facts = json.loads((project / "analysis/xnu-stock-platform.json").read_text())
raw = Path(facts["dtb"]).read_bytes()
assert hashlib.sha256(raw).hexdigest() == facts["dtb_sha256"]
root = read_fdt(raw)
cpus = next(entry for entry in root.children if entry.name == "cpus")
handles = {entry.scalar("phandle"): entry for entry in root.walk() if "phandle" in entry.properties}
hardware = [entry for entry in root.walk() if "unixnu,adt-name" in entry.properties and entry.enabled()]
base, size = arm_io_window(hardware)
cpu_nodes = []
for index, entry in enumerate(cpus.children):
    cpu_nodes.append(node("cpu" + str(index), [
        ("reg", struct.pack("<I", entry.scalar("reg"))), ("device_type", b"cpu\0"),
        ("compatible", entry.properties["compatible"]),
        ("state", b"running\0" if index == 0 else b"waiting\0"),
        ("timebase-frequency", struct.pack("<I", cpus.scalar("timebase-frequency"))),
        ("cluster-type", struct.pack("<I", ord(entry.strings("unixnu,cluster-type")[0])))]))
# This is an allocation request, not a declaration of physical DRAM. DarwinBoot
# must allocate every page as loader memory before XNU is allowed to use it.
window = 0x10000000
chosen = [("random-seed", os.urandom(256)), *empty_nvram_proxy(),
          ("darwinboot-memory-size", struct.pack("<Q", window))]
psci = next(entry for entry in root.children if entry.name == "psci")
children = [node("chosen", chosen, [node("memory-map")]), node("cpus", children=cpu_nodes),
            node("psci", hardware_properties(psci, handles)),
            node("arm-io", [("device_type", b"arm-io\0"), ("#address-cells", struct.pack("<I", 2)),
                            ("#size-cells", struct.pack("<I", 2)), ("ranges", struct.pack("<QQQ", 0, base, size))],
                 [node(entry.strings("unixnu,adt-name")[0], hardware_properties(entry, handles, base)) for entry in hardware]),
            node("defaults"), node("options"), node("product")]
adt = node("device-tree", [("compatible", root.properties["compatible"]),
                          ("model", root.properties["model"]), ("#address-cells", struct.pack("<I", 2)),
                          ("#size-cells", struct.pack("<I", 2))], children)
stage = mu / f"Build/t70proPkg/RELEASE_CLANGPDB/XnuStage{os_suffix}/xnu/kernel.macho.adt"
stage.parent.mkdir(parents=True, exist_ok=True)
stage.write_bytes(adt)
assert stage.read_bytes() == adt
report = {"status": "staged", "os_abi": args.os, "path": str(stage), "bytes": len(adt), "sha256": hashlib.sha256(adt).hexdigest(),
          "memory_policy": "entire_contiguous_window_allocated_by_DarwinBoot_from_live_UEFI_free_memory",
          "requested_window_bytes": window, "physical_dram_declared": False, "fixed_ram_or_reservations": False,
          "excluded_ranges_enabled": False, "framebuffer_policy": "live_GOP_validated_against_runtime_SEC_display_reservation",
          "hardware_dtb_sha256": facts["dtb_sha256"], "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          "device_boot": "not_run", "userspace_supplied": False}
(project / f"analysis/xnu-adt{os_suffix}-build.json").write_text(json.dumps(report, indent=2) + "\n")
print("PASS: staged hardware ADT with 256 MiB full-allocation request; no physical RAM/reservation guesses")
