#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the actual DTB DXE C code with native boot-service substitutes in builder."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dtb", type=Path, required=True)
    ap.add_argument("--expect-t70-sd", action="store_true",
                    help="assert the known stock T70 root and SD properties at EFI publication")
    ap.add_argument("--expect-t70-ufs", action="store_true",
                    help="assert the known stock T70 UFS resources at EFI publication")
    args = ap.parse_args()
    assert platform.system() == "Linux" and platform.machine() == "aarch64"
    package = Path(__file__).resolve().parent
    repo = package.parents[2]
    mde = repo / "Mu_Basecore/MdePkg"
    fdt = mde / "Library/BaseFdtLib"
    raw = args.dtb.resolve().read_bytes()
    with tempfile.TemporaryDirectory(prefix="dtb-test-", dir=repo / "Build") as temp:
        temp = Path(temp)
        exe = temp / "BootStatusDxeTest"
        cmd = ["clang", "-std=gnu11", "-O1", "-g", "-fshort-wchar",
               "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
               "-include", "Base.h",
               "-I" + str(mde / "Include"), "-I" + str(mde / "Include/AArch64"),
               "-idirafter", str(fdt), "-I" + str(fdt / "libfdt/libfdt"),
               str(package / "Tests/BootStatusDxeTest.c"),
               str(package / "Drivers/BootStatusDxe/BootStatusDxe.c"), str(fdt / "FdtLib.c")]
        cmd += [str(fdt / "libfdt/libfdt" / name) for name in ("fdt.c", "fdt_ro.c", "fdt_check.c")]
        subprocess.run(cmd + ["-o", str(exe)], check=True, capture_output=True)
        stock_command = [str(exe), str(args.dtb.resolve())]
        if args.expect_t70_sd:
            stock_command.append("--expect-t70-sd")
        if args.expect_t70_ufs:
            stock_command.append("--expect-t70-ufs")
        normal = subprocess.run(stock_command, check=True, capture_output=True, text=True)
        # Simulate runtime additions beyond the static 1 GiB memory map, with
        # modem reservations and vendor chosen properties that must stay intact.
        dts = b"""/dts-v1/;
/memreserve/ 0x148000000 0x200000;
/ { #address-cells = <2>; #size-cells = <2>;
  memory@40000000 { device_type = "memory"; reg = <0 0x40000000 2 0>; };
  reserved-memory { #address-cells = <2>; #size-cells = <2>; ranges;
    modem@148000000 { reg = <1 0x48000000 0 0x200000>; no-map; };
  };
  chosen { bootargs = "synthetic-test-only"; vendor,modem-data = [01 ab 02 cd]; };
};
"""
        runtime = temp / "synthetic-runtime.dtb"
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(runtime)], input=dts, check=True, capture_output=True)
        synthetic = subprocess.run([str(exe), str(runtime)], check=True, capture_output=True, text=True)
        def check_board(flag, path, missing, changes):
            mismatch = subprocess.run([str(exe), str(runtime), flag],
                                      capture_output=True, text=True)
            if mismatch.returncode != 2 or missing not in mismatch.stderr:
                raise SystemExit("T70 property check accepted well-formed non-T70 DTB: " + flag)
            # These mutated inputs must pass generic publication and only fail
            # at the more specific check for this board resource.
            for prop, cells, error in changes:
                wrong = temp / "changed-board-resource.dtb"
                wrong.write_bytes(raw)
                subprocess.run(["fdtput", "-t", "x", str(wrong), path, prop, *cells.split()],
                               check=True, capture_output=True)
                subprocess.run([str(exe), str(wrong)], check=True, capture_output=True)
                mismatch = subprocess.run([str(exe), str(wrong), flag],
                                          capture_output=True, text=True)
                if mismatch.returncode != 2 or error not in mismatch.stderr:
                    raise SystemExit("T70 property check accepted wrong resource: " + flag + " " + error)

        if args.expect_t70_sd:
            check_board("--expect-t70-sd", "/soc/mmc@11240000", "T70 DTB SD node missing", (
                ("reg", "0 11250000 0 1000 0 11ef0000 0 1000", "reg cell 1 mismatch"),
                ("cd-gpios", "48 8 0", "cd-gpios cell 1 mismatch"),
                ("cd-gpios", "e 7 0", "card detect provider mismatch"),
                ("interrupts", "0 88 4 0", "interrupts cell 1 mismatch"),
            ))
        if args.expect_t70_ufs:
            check_board("--expect-t70-ufs", "/soc/ufshci@11270000", "T70 DTB UFS node missing", (
                ("reg", "0 11280000 0 2300", "reg cell 1 mismatch"),
                ("reg", "0 11270000 0 2000", "reg cell 3 mismatch"),
                ("interrupts", "0 8a 4 0", "interrupts cell 1 mismatch"),
            ))
    print(json.dumps({"status": "pass", "stock_dtb_sha256": hashlib.sha256(raw).hexdigest(),
        "scope": "Actual BootStatusDxe and BaseFdtLib C; native VM substitutes for UEFI services",
        "stock_cases": normal.stdout.strip(), "synthetic_runtime_cases": synthetic.stdout.strip(),
        "t70_sd_properties_checked": args.expect_t70_sd,
        "non_t70_dtb_rejected_by_property_check": args.expect_t70_sd or args.expect_t70_ufs,
        "incorrect_sd_base_rejected_by_property_check": args.expect_t70_sd,
        "incorrect_sd_card_detect_and_irq_rejected": args.expect_t70_sd,
        "t70_ufs_properties_checked": args.expect_t70_ufs,
        "incorrect_ufs_base_size_irq_rejected": args.expect_t70_ufs,
        "runtime_input_is_synthetic": True, "device_execution_tested": False,
        "real_cache_or_uefi_services_tested": False}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        if error.stderr:
            sys.stderr.write(error.stderr.decode() if isinstance(error.stderr, bytes) else error.stderr)
        raise SystemExit(f"DTB check failed (exit {error.returncode})")
