#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Rebuild and validate ACPI offline; does not build an FV or access hardware.

ASL uses ACPICA. Fixed-layout C tables use the host compiler and the explicitly
selected EDK2 AutoGen header. This is a table/AML check, not a firmware boot test.
No source files, existing build directories, or global tool settings are changed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    package = Path(__file__).resolve().parent
    repo = package.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="new directory for generated tables and reports")
    parser.add_argument("--autogen", type=Path, required=True,
                        help="EDK2 ACPI module AutoGen.h supplying platform PCD values")
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--acpica-bin", type=Path, required=True)
    parser.add_argument("--cc", default="clang")
    args = parser.parse_args()
    output = args.output_dir.resolve()
    autogen, dtb, tools = args.autogen.resolve(), args.dtb.resolve(), args.acpica_bin.resolve()
    board = repo/"Silicium-ACPI/Platforms/Teclast/t70pro"
    manifest = board/"AcpiTables.inf"
    sources = manifest.read_text().split("[Sources]", 1)[1].split("\n[", 1)[0]
    names = [line.partition("#")[0].strip() for line in sources.splitlines()]
    asl_sources = [board/name for name in names if name.endswith(".asl")]
    assert board/"DSDT.asl" in asl_sources and len({p.stem for p in asl_sources}) == len(asl_sources)
    c_sources = [repo/f"Silicium-ACPI/Silicon/MediaTek/MT6789/{n}.aslc"
                 for n in ("MADT", "GTDT", "FADT", "PPTT")]
    inputs = asl_sources + c_sources + [manifest, autogen, dtb, Path(__file__).resolve(), package/"verify-acpi.py"]
    def hashes():
        return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    before = hashes()
    for name in ("iasl", "acpiexec"):
        if not (tools/name).is_file():
            parser.error(f"missing ACPICA executable: {tools/name}")
    if output.exists():
        parser.error("output directory already exists; choose a new directory")
    output.mkdir(parents=True)
    commands = []
    def run(command, **kwargs):
        commands.append([str(x) for x in command])
        result = subprocess.run(command, capture_output=True, **kwargs)
        if result.returncode:
            stderr = result.stderr.decode(errors="replace") if isinstance(result.stderr, bytes) else result.stderr
            raise RuntimeError(f"command failed ({result.returncode}): {command}\n{stderr}")
        return result
    try:
        asl_log = []
        for source in asl_sources:
            result = run([str(tools/"iasl"), "-p", str(output/source.stem), str(source)], text=True)
            asl_log.append(result.stdout+result.stderr)
        (output/"iasl.log").write_text("".join(asl_log))
        includes = [repo/"Mu_Basecore/MdePkg/Include", repo/"Mu_Basecore/MdePkg/Include/AArch64",
                    repo/"Mu_Basecore/MdeModulePkg/Include", repo/"Mu_Basecore/ArmPkg/Include"]
        for source in c_sources:
            # Compile the original table directly. Do not copy platform sources.
            code = "#include <stdio.h>\n#include " + json.dumps(str(source)) + "\n"
            code += ("int main(void){unsigned char *p=ReferenceAcpiTable;"
                     "unsigned n=((EFI_ACPI_DESCRIPTION_HEADER*)p)->Length;"
                     "unsigned sum=0;p[9]=0;for(unsigned i=0;i<n;i++)sum+=p[i];"
                     "p[9]=(unsigned char)(0-sum);return fwrite(p,1,n,stdout)!=n;}\n")
            with tempfile.TemporaryDirectory(prefix="t70-acpi-host-") as temp:
                exe = str(Path(temp)/"table")
                run([args.cc, "-std=c11", "-fshort-wchar", "-DMDEPKG_NDEBUG",
                     "-include", str(autogen), *["-I"+str(p) for p in includes],
                     "-x", "c", "-", "-o", exe], input=code, text=True)
                data = run([exe]).stdout
            (output/(source.stem+".acpi")).write_bytes(data)
        env = os.environ.copy()
        env["PATH"] = str(tools)+os.pathsep+env.get("PATH", "")
        import sys
        result = run([sys.executable, "-B", str(package/"verify-acpi.py"),
                      "--dtb", str(dtb), "--build-dir", str(output), "--python-dtb",
                      "--acpica-bin", str(tools)],
                     text=True, env=env)
        verification = json.loads(result.stdout)
        (output/"verification.json").write_text(json.dumps(verification, indent=2)+"\n")
        if hashes() != before:
            raise RuntimeError("an input changed during verification; results are not a coherent snapshot")
        report = {"status": "passed", "scope": "host table generation and virtual AML; no firmware image or hardware test",
                  "input_sha256": before, "commands": commands, "verification": "verification.json"}
        (output/"build.json").write_text(json.dumps(report, indent=2)+"\n")
        print(output/"build.json")
    except Exception as error:
        (output/"failure.json").write_text(json.dumps({"status": "failed", "error": str(error),
            "input_sha256": before, "commands": commands}, indent=2)+"\n")
        raise


if __name__ == "__main__":
    main()
