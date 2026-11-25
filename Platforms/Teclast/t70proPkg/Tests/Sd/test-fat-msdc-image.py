#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Native actual FAT -> SdDxe -> MSDC command/FIFO regression, private FAT image.
EFI services, byte DiskIo RMW and register/card behavior are test adapters.
No physical card, boot, product policy change, or firmware build. Native only.
"""
from pathlib import Path
import subprocess,tempfile,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--screenshot',action='store_true',help='deterministic 1280x800 BMP fixture, not GOP capture')
args=parser.parse_args()
here=Path(__file__).resolve().parent;repo=here.parents[4];base=repo/'Mu_Basecore'
incs=['Mu_Basecore/MdePkg/Include','Mu_Basecore/MdePkg/Include/AArch64','Mu_Basecore/MdeModulePkg/Include',
      'Mu_Basecore/ArmPkg/Include','Silicon/Silicium/SiliciumPkg/Include','Silicon/MediaTek/MediaTekPkg/Include']
with tempfile.TemporaryDirectory(prefix='fat-msdc-abi-') as td:
 t=Path(td);lib=t/'msdc.so';exports=t/'exports.map'
 exports.write_text('{ global: T70MsdcImageInit; T70MsdcImagePacket; T70MsdcImageStats; local: *; };')
 cmd=['gcc','-shared','-fPIC','-fshort-wchar','-DMDEPKG_NDEBUG','-ffunction-sections','-fdata-sections',
      '-Wl,--gc-sections','-Wl,-Bsymbolic','-Wl,--version-script='+str(exports),'-include','Base.h']
 if args.screenshot:cmd+=['-DMSDC_TEST_TX_CAPACITY=4194304'] # Fixture capture limit only; production timeout unchanged.
 cmd+=['-I'+str(repo/x) for x in incs]+['-I'+str(here.parent)]
 subprocess.run(cmd+[str(here/'MsdcImageBackend.c'),'-o',str(lib)],check=True)
 subprocess.run(['python3',str(base/'FatPkg/EnhancedFatDxe/test-regular-image.py'),
                 '--sd-bridge','--msdc-backend',str(lib)]+(['--screenshot'] if args.screenshot else []),check=True)
