#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile only the read-only diagnostic helper with host HII/RAM mocks."""
from pathlib import Path
import subprocess, tempfile
here=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='t70-hii-diagnostic-') as d:
    exe=Path(d)/'test'
    subprocess.run(['clang','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        str(here/'T70HiiDiagnosticTest.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
# Confirm integration in the actual form update, before existing menu entries,
# and lifetime reset before package removal. Do not compile or run the UI.
source=(here.parents[3]/'Mu_Basecore/MdeModulePkg/Application/UiApp/FrontPage.c').read_text()
update=source[source.index('\nUpdateFrontPageForm ('):source.index('\nInitializeFrontPage (')]
assert update.index('AppendT70BootDiagnostic') < update.index('UiCustomizeFrontPage') < update.index('HiiUpdateForm')
assert source.index('ResetT70DiagnosticStrings ();') < source.index('HiiRemovePackages (gFrontPagePrivate.HiiHandle);')
print('FrontPage wiring: guarded text insertion before menu entries and package-lifetime reset confirmed')
