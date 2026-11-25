#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Actual FAT install/uninstall + production DeviceCheck discovery, mock metadata.
Only the existing private regular FAT image is opened; never a physical device.
"""
from pathlib import Path
import subprocess,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--export',action='store_true',help='exercise actual choice state, core and SD adapter')
args=parser.parse_args()
repo=Path(__file__).resolve().parents[5]
subprocess.run(['python3',str(repo/'Mu_Basecore/FatPkg/EnhancedFatDxe/test-regular-image.py'),
                '--devicecheck-registry',str(repo)]+(['--devicecheck-export'] if args.export else []),check=True)
