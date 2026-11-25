#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Unpack build dependencies and create x86 tool launchers inside this project."""
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import urllib.request

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("prepare_gki", HERE / "prepare.py")
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)
ROOT = prep.ROOT
HOST = ROOT / "host"
RUNTIME = HOST / "root"
BINDGEN_COMMIT = "2e77dab4d331387ed8036a3b2ae9fa48ca2f74a4"
BINDGEN_BLOB = "a7075a0f72a3f75733b01ed4716926bf20138f43"


def run(args, **kw):
    subprocess.run([str(a) for a in args], check=True, **kw)


def main():
    if subprocess.check_output(["lsb_release", "-cs"], text=True).strip() != "resolute":
        raise ValueError("This host profile was prepared for Ubuntu resolute only")
    HOST.mkdir(parents=True, exist_ok=True)
    cache = HOST / "packages"
    cache.mkdir(exist_ok=True)
    lists = HOST / "apt-lists"
    (lists / "partial").mkdir(parents=True, exist_ok=True)
    sources = HOST / "amd64.sources.list"
    sources.write_text("deb [arch=amd64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] https://archive.ubuntu.com/ubuntu resolute main\n")
    cache_opts = ["-o", "Dir::Cache::pkgcache=" + str(HOST / "pkgcache.bin"),
                  "-o", "Dir::Cache::srcpkgcache=" + str(HOST / "srcpkgcache.bin")]
    opts = [*cache_opts, "-o", "Dir::Etc::main=/dev/null", "-o", "Dir::Etc::parts=-",
            "-o", "Dir::Etc::sourcelist=" + str(sources), "-o", "Dir::Etc::sourceparts=-",
            "-o", "Dir::State::lists=" + str(lists), "-o", "APT::Architecture=amd64",
            "-o", "APT::Architectures=amd64", "-o", "Debug::NoLocking=true",
            "-o", "APT::Sandbox::User=builder"]
    run(["apt-get", *opts, "update"])
    run(["apt-get", *opts, "download", "libc6:amd64", "libgcc-s1:amd64", "libstdc++6:amd64", "zlib1g:amd64", "libc6-dev:amd64", "libgcc-15-dev:amd64"], cwd=cache)
    run(["apt-get", *cache_opts, "download", "bc", "libdw-dev", "libdw1t64", "libelf1t64", "patchelf"], cwd=cache)
    RUNTIME.mkdir(exist_ok=True)
    receipts = []
    for deb in sorted(cache.glob("*.deb")):
        run(["dpkg-deb", "-x", deb, RUNTIME])
        receipts.append({"file": deb.name, "sha256": prep.sha(deb.read_bytes())})
    # Ubuntu's libc linker script assumes the usr-merge directory aliases.
    # Recreate them only in this private sysroot.
    for folder in ("lib", "lib64"):
        alias = RUNTIME / folder
        if not alias.exists():
            alias.symlink_to("usr/" + folder, target_is_directory=True)
    binary = HOST / "bindgen"
    if not binary.exists():
        url = "https://android.googlesource.com/platform/prebuilts/clang-tools/+/" + BINDGEN_COMMIT + "/linux-x86/bin/bindgen?format=TEXT"
        with urllib.request.urlopen(url, timeout=120) as response:
            packed = response.read(90 * 1024 * 1024)
        data = base64.b64decode(packed, validate=True)
        git_hash = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
        if git_hash != BINDGEN_BLOB:
            raise ValueError("bindgen does not match its pinned public Git blob")
        binary.write_bytes(data)
        binary.chmod(0o755)
    else:
        data = binary.read_bytes()
        if hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest() != BINDGEN_BLOB:
            raise ValueError("Existing bindgen differs; preserved")
    loaders = list(RUNTIME.glob("**/ld-linux-x86-64.so.2"))
    if not loaders:
        raise ValueError("x86 glibc loader not extracted")
    loader = loaders[0].resolve()
    clang = ROOT / "toolchains/clang/clang-r536225"
    rust = ROOT / "toolchains/rust/linux-x86/1.82.0"
    tools = ROOT / "toolchains/kernel-tools/linux-x86"
    libs = [RUNTIME / "usr/lib/x86_64-linux-gnu", clang / "lib", rust / "lib", rust / "lib64", tools / "lib64"]
    libpath = ":".join(str(d) for d in libs if d.is_dir())
    launchers = HOST / "bin"
    launchers.mkdir(exist_ok=True)
    # Do not replace VM libraries, /lib64, or any downloaded source executable.
    # Clang starts its own assembler as a subprocess. Give a private copy the
    # project-local ELF interpreter so that those subprocesses work as well.
    libexec = HOST / "libexec"
    libexec.mkdir(exist_ok=True)
    patched_clang = libexec / "clang-19"
    shutil.copy2(clang / "bin/clang-19", patched_clang)
    run([RUNTIME / "usr/bin/patchelf", "--set-interpreter", loader,
         "--force-rpath", "--set-rpath", libpath, patched_clang])
    rust_overlay = HOST / "rust"
    (rust_overlay / "bin").mkdir(parents=True, exist_ok=True)
    for folder in ("lib", "lib64"):
        link = rust_overlay / folder
        if not link.exists():
            link.symlink_to(rust / folder, target_is_directory=True)
    for name in ("rustc", "rustdoc", "rustfmt"):
        target = rust_overlay / "bin" / name
        shutil.copy2(rust / "bin" / name, target)
        run([RUNTIME / "usr/bin/patchelf", "--set-interpreter", loader,
             "--force-rpath", "--set-rpath", libpath, target])
    programs = {"clang": patched_clang, "clang++": patched_clang,
                "rustc": rust_overlay / "bin/rustc", "rustdoc": rust_overlay / "bin/rustdoc",
                "rustfmt": rust_overlay / "bin/rustfmt",
                "bindgen": binary, "pahole": tools / "bin/pahole", "lz4": tools / "bin/lz4"}
    for path in (clang / "bin").iterdir():
        if path.name.startswith("llvm-") or path.name in ("ld.lld", "lld"):
            programs[path.name] = path
    for name, target in programs.items():
        if not target.exists():
            continue
        argv = [target] if name in ("clang", "clang++", "rustc", "rustdoc", "rustfmt") else [loader, "--library-path", libpath, target]
        command = "exec " + " ".join(shlex.quote(str(x)) for x in argv)
        if name in ("clang", "clang++"):
            command += " -resource-dir " + shlex.quote(str(clang / "lib/clang/19"))
        if name == "clang++":
            command += " --driver-mode=g++"
        wrapper = launchers / name
        wrapper.write_text("#!/bin/sh\n" + command + ' "$@"\n')
        wrapper.chmod(0o755)
    bc = launchers / "bc"
    if not bc.exists():
        bc.symlink_to(RUNTIME / "usr/bin/bc")
    receipt = {"schema": 1, "system_package_install": False, "runtime_packages": receipts,
               "bindgen_commit": BINDGEN_COMMIT, "bindgen_blob": BINDGEN_BLOB,
               "bindgen_sha256": prep.sha(data), "loader": str(loader), "launchers": str(launchers),
               "clang_original_sha256": prep.sha((clang / "bin/clang-19").read_bytes()),
               "clang_execution_copy_sha256": prep.sha(patched_clang.read_bytes()),
               "rust_execution_copy_sha256": prep.sha((rust_overlay / "bin/rustc").read_bytes())}
    (HOST / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"host_runtime": str(HOST), "packages": len(receipts), "system_package_install": False}))


if __name__ == "__main__":
    main()
