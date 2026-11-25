#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Embed the distribution dependencies of the already staged DKMS modules."""
import argparse
from pathlib import Path
import shutil
import subprocess

import zstandard

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("distro_modules", type=Path)
p.add_argument("initramfs_root", type=Path)
x = p.parse_args()
src, root = x.distro_modules.resolve(strict=True), x.initramfs_root.resolve(strict=True)
dst = root / "lib/modules" / src.name


def key(name):
    return name.split(".ko", 1)[0].replace("-", "_")


shipped = {}
for file in (src / "kernel").rglob("*.ko*"):
    if file.name.endswith((".ko", ".ko.zst")):
        shipped.setdefault(key(file.name), []).append(file)
builtins = {key(Path(line).name) for line in (src / "modules.builtin").read_text().splitlines()}
visited = set()
copied = []


def include(path):
    name = key(path.name)
    if name in visited:
        return
    visited.add(name)
    raw = subprocess.check_output(["modinfo", "-F", "depends", path], text=True).strip()
    for token in filter(None, raw.split(",")):
        needed = key(token)
        if needed in builtins:
            continue
        candidates = shipped.get(needed, [])
        if len(candidates) != 1:
            raise SystemExit(f"{name}: unresolved or ambiguous dependency {needed}")
        include(candidates[0])
    if path.is_relative_to(src):
        target = dst / str(path.relative_to(src)).removesuffix(".zst")
        target.parent.mkdir(parents=True, exist_ok=True)
        if path.name.endswith(".zst"):
            with path.open("rb") as compressed, target.open("xb") as out:
                zstandard.ZstdDecompressor().copy_stream(compressed, out)
        else:
            shutil.copyfile(path, target)
        copied.append(name)


modules = list((dst / "updates/dkms").glob("*.ko"))
if len(modules) != 8:
    raise SystemExit("expected eight DKMS modules")
for module in sorted(modules):
    include(module)
for optional in ("modules.builtin", "modules.builtin.modinfo", "modules.order"):
    input_file = src / optional
    if input_file.is_file():
        shutil.copyfile(input_file, dst / optional)
print("Embedded distribution module dependencies:", ", ".join(copied))
