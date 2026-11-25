#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Fetch pinned public loader sources, or check existing checkouts without changing them."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

package = Path(__file__).resolve().parents[1]
repo = package.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--check", action="store_true", help="verify only; no fetch or checkout")
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
if not args.check and not args.locked:
    os.execv(str(repo / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Prepare pinned XNU loader sources", "--",
              sys.executable, str(Path(__file__).resolve()), "--locked"])
sources = json.loads((package / "Xnu/sources.json").read_text())
root = repo.parent / "xnu-sources"
for name, spec in sources.items():
    path = root / name
    if not path.exists():
        if args.check:
            sys.exit(f"Missing {name}; run Xnu/prepare-sources.py first")
        path.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(path)], check=True)
        subprocess.run(["git", "-C", str(path), "remote", "add", "origin", spec["url"]], check=True)
        subprocess.run(["git", "-C", str(path), "fetch", "--depth=1", "origin", spec["commit"]], check=True)
        if spec.get("sparse"):
            subprocess.run(["git", "-C", str(path), "sparse-checkout", "set", *spec["sparse"]], check=True)
        subprocess.run(["git", "-C", str(path), "checkout", "--detach", "FETCH_HEAD"], check=True)
    actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    dirty = subprocess.check_output(["git", "-C", str(path), "status", "--porcelain", "--untracked-files=normal"], text=True)
    if actual != spec["commit"] or dirty.strip() or not (path / spec["input"]).is_file():
        sys.exit(f"{name}: source pin/input mismatch or modified checkout; preserved without checkout/reset")
    print(f"{name}: {actual}")
