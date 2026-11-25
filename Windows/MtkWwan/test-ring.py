#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

here=Path(__file__).resolve().parent
cc=os.environ.get('CC') or shutil.which('gcc') or shutil.which('clang')
if not cc: raise SystemExit('clang or gcc is required')
with tempfile.TemporaryDirectory(prefix='t70-ccci-ring-') as directory:
    exe=Path(directory)/'CcciRingTest'
    subprocess.run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-DT70_CCCI_HOST_TEST',str(here/'CcciRing.c'),str(here/'CcciNet.c'),str(here/'CcciRingTest.c'),
        '-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    descriptor_exe=Path(directory)/"DpmaifDescriptorTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifDescriptor.c"),str(here/"DpmaifDescriptorTest.c"),
        "-o",str(descriptor_exe)],check=True)
    subprocess.run([str(descriptor_exe)],check=True)
    ring_exe=Path(directory)/"DpmaifRingTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifRing.c"),str(here/"DpmaifRingTest.c"),
        "-o",str(ring_exe)],check=True)
    subprocess.run([str(ring_exe)],check=True)
    bat_exe=Path(directory)/"DpmaifBatCoreTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifDescriptor.c"),str(here/"DpmaifRing.c"),
        str(here/"DpmaifBatCore.c"),str(here/"DpmaifBatCoreTest.c"),
        "-o",str(bat_exe)],check=True)
    subprocess.run([str(bat_exe)],check=True)
    tx_exe=Path(directory)/"DpmaifTxPacketTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifDescriptor.c"),str(here/"DpmaifRing.c"),
        str(here/"DpmaifTxPacket.c"),str(here/"DpmaifTxPacketTest.c"),
        "-o",str(tx_exe)],check=True)
    subprocess.run([str(tx_exe)],check=True)
    sg_exe=Path(directory)/"DpmaifSgCoreTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifSgCore.c"),str(here/"DpmaifSgCoreTest.c"),
        "-o",str(sg_exe)],check=True)
    subprocess.run([str(sg_exe)],check=True)
    wdm_exe=Path(directory)/"DpmaifWdmDmaTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all","-ffunction-sections",
        "-fdata-sections","-pthread","-Wl,--gc-sections","-I",str(here/"Tests/include"),"-I",str(here),
        str(here/"DpmaifWdmDmaTest.c"),"-o",str(wdm_exe)],check=True)
    subprocess.run([str(wdm_exe)],check=True)
    resources_exe=Path(directory)/"DpmaifResourcesTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifResources.c"),str(here/"DpmaifResourcesTest.c"),
        "-o",str(resources_exe)],check=True)
    subprocess.run([str(resources_exe)],check=True)
    hw_exe=Path(directory)/"DpmaifHwCoreTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifHwCore.c"),str(here/"DpmaifHwCoreTest.c"),
        "-o",str(hw_exe)],check=True)
    subprocess.run([str(hw_exe)],check=True)
    layout_exe=Path(directory)/"DpmaifLayoutTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"DpmaifLayout.c"),str(here/"DpmaifHwCore.c"),
        str(here/"DpmaifLayoutTest.c"),"-o",str(layout_exe)],check=True)
    subprocess.run([str(layout_exe)],check=True)
    ccif_exe=Path(directory)/"CcciCcifCoreTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"CcciCcifCore.c"),str(here/"CcciCcifCoreTest.c"),
        "-o",str(ccif_exe)],check=True)
    subprocess.run([str(ccif_exe)],check=True)
    ccif_res_exe=Path(directory)/"CcciCcifResourcesTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"CcciCcifResources.c"),str(here/"CcciCcifResourcesTest.c"),
        "-o",str(ccif_res_exe)],check=True)
    subprocess.run([str(ccif_res_exe)],check=True)
    handoff_exe=Path(directory)/"CcciMemoryHandoffTest"
    handoff_include=here.parents[1]/"Silicon/MediaTek/MediaTekPkg/Include"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        "-I",str(handoff_include),str(here/"CcciMemoryHandoffTest.c"),
        "-o",str(handoff_exe)],check=True)
    subprocess.run([str(handoff_exe)],check=True)
    callback_exe=Path(directory)/"NdisCallbackCoreTest"
    subprocess.run([cc,"-std=c11","-O1","-g","-Wall","-Wextra","-Werror",
        "-fsanitize=address,undefined","-fno-sanitize-recover=all",
        str(here/"NdisCallbackCoreTest.c"),"-o",str(callback_exe)],check=True)
    subprocess.run([str(callback_exe)],check=True)
    subprocess.run([sys.executable,str(here/"test-oid.py")],check=True)
