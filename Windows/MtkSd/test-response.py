#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile only the portable response decoder, never firmware or a Windows driver."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='t70-sd-response-') as tmp:
    exe=Path(tmp)/'response-test'
    subprocess.run([os.environ.get('CC', 'clang'),'-std=c11','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    str(root/'SdResponse.c'),str(root/'ResponseTest.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
