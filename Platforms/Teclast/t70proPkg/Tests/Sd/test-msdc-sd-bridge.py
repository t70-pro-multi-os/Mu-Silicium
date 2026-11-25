#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Focused native actual SdDxe -> MsdcSendCmd integration, fake register/FIFO IO.
Native unsanitized test: ASan retains unused discovery templates requiring a broader link.
No tablet, source copies, or product policy changes. The callback deliberately
exercises the command implementation below the unchanged read-only PassThru gate.
--image creates one 8192-byte regular file, passes only its FD, and checks every
byte after child shutdown via a fresh open. TX persistence uses captured FIFO
bytes; RX loads the same image into the fixture FIFO. This is NOT FAT integration
or a physical SD durability result; fsync is solely for the host image.
"""
from pathlib import Path
import subprocess,tempfile,argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--image',action='store_true',help='round trip a private 16-sector regular image through TX/RX FIFO and independently verify every byte')
args=parser.parse_args()
here=Path(__file__).resolve().parent
repo=here.parents[4]
base=repo/'Mu_Basecore';sd=base/'MdeModulePkg/Bus/Sd/SdDxe'
incs=['Mu_Basecore/MdePkg/Include','Mu_Basecore/MdePkg/Include/AArch64','Mu_Basecore/MdeModulePkg/Include',
      'Mu_Basecore/ArmPkg/Include','Silicon/Silicium/SiliciumPkg/Include','Silicon/MediaTek/MediaTekPkg/Include']
with tempfile.TemporaryDirectory(prefix='sd-msdc-bridge-') as t:
 out=Path(t)/'test'
 cmd=['gcc','-fshort-wchar','-DMDEPKG_NDEBUG','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
      '-D_PCD_GET_MODE_32_PcdMaximumLinkedListLength=0U',
      '-include',str(base/'FatPkg/EnhancedFatDxe/TestSdBuild.h')]
 cmd+=['-I'+str(repo/x) for x in incs]+['-I'+str(here.parent)]
 cmd += [str(here/'MsdcSdBridgeTest.c'),str(sd/'SdBlockIo.c'),str(sd/'SdDxe.c')]
 cmd += [str(base/'MdePkg/Library/BaseLib'/n) for n in ['LinkedList.c','Math64.c','MultU64x32.c']]
 subprocess.run(cmd+['-o',str(out)],check=True)
 if args.image:
  image=Path(t)/'private-card.img'
  original=bytes((n*19+n//512*7+3)&255 for n in range(8192))
  with image.open('xb') as f:f.write(original)
  with image.open('r+b') as f:
   subprocess.run([str(out),str(f.fileno())],pass_fds=(f.fileno(),),check=True)
  # Child has closed its FD; this is a fresh independent open, not its FIFO/cache.
  expected=bytearray(original);expected[9*512:11*512]=bytes((n*37+11)&255 for n in range(1024))
  assert image.read_bytes()==expected
  print('Independent reopened image: 1024 target bytes match; all 7168 non-target bytes unchanged')
 else:
  subprocess.run([str(out)],check=True)
