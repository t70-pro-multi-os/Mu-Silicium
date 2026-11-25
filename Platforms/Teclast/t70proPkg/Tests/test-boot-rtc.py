#!/usr/bin/env python3
"""Only native fake-transport RTC tests; never run a tablet or full firmware build."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
import sys

p = argparse.ArgumentParser()
p.add_argument("--dtb", type=Path, required=True, help="Existing stock effective.dtb")
a = p.parse_args()
root = Path(__file__).resolve().parents[4]
tests = Path(__file__).resolve().parent
fdt = root / "Mu_Basecore/MdePkg/Library/BaseFdtLib"
libfdt = fdt / "libfdt/libfdt"
includes = ["Mu_Basecore/MdePkg/Include", "Mu_Basecore/MdePkg/Include/AArch64",
    "Mu_Basecore/ArmPkg/Include", "Mu_Basecore/MdeModulePkg/Include",
    "Mu_Basecore/EmbeddedPkg/Include", "Mu_Basecore/PrmPkg/Include", "Silicon/Silicium/SiliciumPkg/Include",
    "Silicon/MediaTek/MediaTekPkg/Include"]
with tempfile.TemporaryDirectory(prefix="t70-rtc-test-") as tmp:
    for name in ("BootRtcTest", "SetRtcTest", "UnavailableRtcTest", "RtcWrapperMetadataTest", "RuntimeRtcTest"):
        out = Path(tmp) / name
        cmd = [os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g", "-fshort-wchar", "-DMDEPKG_NDEBUG",
            "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-include", "Base.h"]
        cmd += ["-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
        cmd += ["-I" + str(root / x) for x in includes]
        cmd += ["-I" + str(tests), "-idirafter", str(fdt), "-I" + str(libfdt)]
        cmd += [str(fdt / "FdtLib.c")]
        cmd += [str(libfdt / x) for x in ("fdt.c", "fdt_ro.c", "fdt_check.c",
            "fdt_addresses.c", "fdt_empty_tree.c", "fdt_rw.c", "fdt_sw.c")]
        cmd += [str(tests / (name + ".c")), "-o", str(out)]
        if name == "RuntimeRtcTest":
            silicon = root / "Silicon/MediaTek/MediaTekPkg"
            cmd += [str(silicon / "Drivers/MtkUfsPrm/MtkPmicBroker.c"),
                    str(silicon / "Library/UfsPowerLib/MtkUfsPowerCore.c")]
        subprocess.run(cmd, check=True)
        subprocess.run([str(out), str(a.dtb.resolve())], check=True)
