#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Acquire pinned official NuGet headers/libs once; no Windows installer is run."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import tempfile
import urllib.request
import zipfile

root = Path(__file__).resolve().parent / "Toolchain"
version = "10.0.26100.6584"
packages = ("microsoft.windows.wdk.arm64", "microsoft.windows.sdk.cpp", "microsoft.windows.sdk.cpp.arm64")
root.mkdir(exist_ok=True)
report = []
for name in packages:
    dest = root / name
    stamp = dest / "package.json"
    if stamp.exists():
        item = json.loads(stamp.read_text())
        assert item["version"] == version
        report.append(item)
        print(name + ": existing pinned package", flush=True)
        continue
    url = f"https://api.nuget.org/v3-flatcontainer/{name}/{version}/{name}.{version}.nupkg"
    digest = hashlib.sha256()
    count = total = 0
    print(name + ": acquiring official headers and ARM64 libraries", flush=True)
    with tempfile.TemporaryFile(dir=root) as archive:
        with urllib.request.urlopen(url, timeout=60) as response:
            while block := response.read(1024 * 1024):
                digest.update(block)
                archive.write(block)
        archive.seek(0)
        with zipfile.ZipFile(archive) as package:
            for info in package.infolist():
                path = PurePosixPath(info.filename)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError("Unsafe NuGet archive path")
                low = info.filename.lower()
                # Keep includes and ARM64 link libraries. No tool binaries or source copies.
                wanted = (low.endswith((".h", ".inl", ".hpp", ".idl", ".nuspec")) or
                          (low.endswith(".lib") and "arm64" in low) or
                          (len(path.parts) == 1 and ("license" in low or "notice" in low)))
                if info.is_dir() or not wanted:
                    continue
                target = dest.joinpath(*path.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                data = package.read(info)
                target.write_bytes(data)
                count += 1
                total += len(data)
    item = {"package": name, "version": version, "source": url,
            "archive_sha256": digest.hexdigest(), "files": count, "extracted_bytes": total}
    stamp.write_text(json.dumps(item, indent=2) + "\n")
    report.append(item)
    print(f"{name}: {count} files, {total} bytes retained", flush=True)
(root / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")

# Only the compiler support headers needed by the WDK, pinned to a signed
# Visual Studio channel payload. Never run a Windows installer or compiler.
crt = root / "msvc-crt-headers"
crt_url = "https://download.visualstudio.microsoft.com/download/pr/c610cd8c-801b-44b8-a80a-82cc382aeb43/852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5/Microsoft.VC.14.44.17.14.CRT.Headers.base.vsix"
crt_hash = "852382a9aa73502b7849c1bcadfb603ba7175c4e8b60e6aba03c7de711d4ece5"
if not (crt / "source.json").exists():
    with tempfile.TemporaryFile(dir=root) as archive:
        digest = hashlib.sha256()
        with urllib.request.urlopen(crt_url, timeout=60) as response:
            while block := response.read(1024 * 1024):
                digest.update(block)
                archive.write(block)
        assert digest.hexdigest() == crt_hash, "MSVC headers payload hash mismatch"
        archive.seek(0)
        count = 0
        with zipfile.ZipFile(archive) as package:
            for info in package.infolist():
                path = PurePosixPath(info.filename)
                assert not path.is_absolute() and ".." not in path.parts
                if not info.is_dir() and (path.suffix.lower() in (".h", ".hpp", ".inl") or "license" in path.name.lower()):
                    target = crt.joinpath(*path.parts)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(package.read(info))
                    count += 1
        (crt / "source.json").write_text(json.dumps({"package": "Microsoft.VC.14.44.17.14.CRT.Headers.base",
            "version": "14.44.35220", "url": crt_url, "sha256": crt_hash, "files": count}, indent=2) + "\n")
else:
    assert json.loads((crt / "source.json").read_text())["sha256"] == crt_hash
print("MSVC support headers: pinned payload ready")
