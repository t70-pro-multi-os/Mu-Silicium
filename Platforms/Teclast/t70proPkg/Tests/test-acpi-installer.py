#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile the production ACPI installer with host-only service mocks."""
from pathlib import Path
import subprocess, sys, tempfile
here = Path(__file__).resolve().parent
root = here.parents[3]
source = root/'Mu_Basecore/MdeModulePkg/Universal/Acpi/AcpiPlatformDxe/AcpiPlatform.c'
# Pin the pre-fix source: HEAD advances when the repair is committed.
BASELINE_REVISION = 'e79d1d2d059d315b78bcb13dc051915a4e69e477'
headers = ('PiDxe.h', 'Protocol/AcpiTable.h', 'Protocol/FirmwareVolume2.h',
           'Library/BaseLib.h', 'Library/UefiBootServicesTableLib.h',
           'Library/DebugLib.h', 'Library/PcdLib.h', 'IndustryStandard/Acpi.h')
with tempfile.TemporaryDirectory(prefix='t70-acpi-installer-') as tmp:
    tmp = Path(tmp)
    if '--baseline' in sys.argv[1:]:
        original = subprocess.check_output([
            'git', '-C', str(root/'Mu_Basecore'), 'show',
            BASELINE_REVISION + ':MdeModulePkg/Universal/Acpi/AcpiPlatformDxe/AcpiPlatform.c'])
        source = tmp/'AcpiPlatform.c'
        source.write_bytes(original)
    for name in headers:
        p = tmp/name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('/* Types supplied by the test service shim. */\n')
    exe = tmp/'test'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I'+str(tmp),
                    '-DACPI_PLATFORM_SOURCE="'+str(source)+'"',
                    str(here/'AcpiPlatformInstallTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe), *sys.argv[1:]], check=True)
