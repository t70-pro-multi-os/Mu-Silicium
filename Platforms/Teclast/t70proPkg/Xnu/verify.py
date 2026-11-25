#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the built ARM64 loader pair and exercise source-level launch failures."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

package = Path(__file__).resolve().parents[1]
repo = package.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--os", choices=("ios", "macos"), default="ios")
parser.add_argument("--locked", action="store_true", help=argparse.SUPPRESS)
args = parser.parse_args()
os_suffix = "-macos" if args.os == "macos" else ""
if not args.locked:
    os.execv(str(repo / "Resources/Scripts/with-build-lock"),
             ["with-build-lock", "T70Pro", "Verify XNU loader integration", "--",
              sys.executable, str(Path(__file__).resolve()), *sys.argv[1:], "--locked"])
output = repo / "Build/t70proPkg/RELEASE_CLANGPDB"
sources = repo.parent / "xnu-sources"
report = {"time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
          "device": "Teclast T70 Pro / MT6789", "scope": "loader-only", "os_abi": args.os,
          "source_revisions": json.loads((package / "Xnu/sources.json").read_text()),
          "kernel_build_report": str(repo.parent / f"analysis/xnu-kernel{os_suffix}-build.json"), "device_boot": "not_run", "checks": []}
report_path = repo.parent / f"analysis/xnu-loader{os_suffix}-verification.json"

def run(name, command, **kwargs):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    report["checks"].append({"name": name, "command": [str(x) for x in command],
                             "exit_code": result.returncode, "output": result.stdout})
    print(result.stdout, end="")
    result.check_returncode()

try:
    run("pinned sources", [sys.executable, str(package / "Xnu/prepare-sources.py"), "--check"])
    includes = ["Mu_Basecore/MdePkg/Include", "Mu_Basecore/MdePkg/Include/AArch64",
                "Mu_Basecore/ArmPkg/Include", "Mu_Basecore/MdeModulePkg/Include",
                "Mu_Basecore/EmbeddedPkg/Include", "Silicon/Silicium/SiliciumPkg/Include"]
    with tempfile.TemporaryDirectory(prefix="xnu-test-", dir=repo / "Build") as temporary:
        exe = Path(temporary) / "XnuBootTest"
        command = ["gcc", "-std=gnu11", "-O1", "-g", "-fshort-wchar", "-DMDEPKG_NDEBUG",
                   "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                   "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-include", "Base.h"]
        command += ["-I" + str(repo / inc) for inc in includes]
        command += ["-I" + str(sources / "UniXNU/package/UniXNUPkg/Include"),
                    str(package / "Tests/XnuBootTest.c"), "-o", str(exe)]
        run("compile production launcher tests with sanitizers", command)
        run("31 launcher scenarios", [str(exe)])
        platform_exe = Path(temporary) / "XnuPlatformTest"
        fdt = repo / "Mu_Basecore/MdePkg/Library/BaseFdtLib"
        libfdt = fdt / "libfdt/libfdt"
        platform_command = command[:command.index(str(package / "Tests/XnuBootTest.c"))]
        platform_command += ["-I" + str(sources / "DarwinBoot"), "-idirafter", str(fdt), "-I" + str(libfdt),
                             str(package / "Tests/XnuPlatformTest.c"), str(fdt / "FdtLib.c")]
        platform_command += [str(libfdt / name) for name in
                             ("fdt.c", "fdt_ro.c", "fdt_check.c", "fdt_addresses.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c")]
        platform_command += ["-o", str(platform_exe)]
        run("compile production runtime platform/libfdt checks with sanitizers", platform_command)
        run("runtime CPU/GIC/timer/GOP reservation checks", [str(platform_exe), str(repo.parent / "analysis/stock-fdt/effective.dtb")])
        env = os.environ.copy()
        # Upstream uses ctypes to load UBSan-instrumented shared libraries.
        # Linux Python needs the sanitizer runtime loaded first.
        runtime = subprocess.check_output(["gcc", "-print-file-name=libubsan.so"], text=True).strip()
        if not Path(runtime).is_file():
            raise RuntimeError("Missing GCC UBSan runtime for upstream ctypes tests")
        env["LD_PRELOAD"] = runtime
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        for name in ("test-kernel-parser.py", "test-adt-window.py", "test-memory-window.py",
                     "test-memory-outside.py", "test-loader-inputs.py", "test-packed-ramdisk.py"):
            run(name, [sys.executable, str(package / "Xnu/run-upstream-check.py"), name], env=env)
        run("compile window-size check", ["clang", "-Wall", "-Wextra", "-Werror",
            str(sources / "DarwinBoot/Test/window-size.c"), "-o", str(Path(temporary) / "window-size")])
        run("window-size check", [str(Path(temporary) / "window-size")])
    images = {}
    for name, subsystem in (("T70XnuBoot.efi", 10), ("DarwinBoot.efi", 11)):
        path = output / "AARCH64" / name
        data = path.read_bytes()
        assert data[:2] == b"MZ", name
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        assert data[pe:pe+4] == b"PE\0\0", name
        assert struct.unpack_from("<H", data, pe+4)[0] == 0xaa64, name
        assert struct.unpack_from("<H", data, pe+24)[0] == 0x20b, name
        assert struct.unpack_from("<H", data, pe+24+68)[0] == subsystem, name
        stage = output / f"XnuStage{os_suffix}/EFI/Teclast" / name
        assert stage.read_bytes() == data, name
        images[name] = {"path": str(stage), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(), "machine": "AARCH64",
                        "subsystem": subsystem}
    assert (output / f"XnuStage{os_suffix}/sources.json").read_bytes() == (package / "Xnu/sources.json").read_bytes()
    report["images"] = images
    report["source_sha256"] = {
        str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [package / "t70pro.dsc", package / "build.sh", package / "Tests/XnuBootTest.c", package / "Tests/XnuPlatformTest.c",
                  package / "Applications/XnuBoot/XnuBoot.c", package / "Applications/XnuBoot/XnuPlatform.c",
                  package / "Applications/XnuBoot/XnuPlatform.h", package / "Applications/XnuBoot/XnuBoot.inf",
                  package / "Xnu/sources.json", package / "Xnu/prepare-sources.py",
                  package / "Xnu/run-upstream-check.py", Path(__file__).resolve()]
    }
    report["status"] = "passed"
    print("PASS: AARCH64 EFI application/driver headers and staged file hashes")
except Exception as error:
    report["status"] = "failed"
    report["error"] = str(error)
    raise
finally:
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("Report:", report_path)
