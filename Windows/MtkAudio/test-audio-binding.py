#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check development ID/resource wiring; compile ONLY the audio SSDT offline.
No firmware build, installation, device creation, hardware or power operations.
Requires existing iasl and fdtget; --dtb is the caller's stock evidence file.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dtb', type=Path, required=True)
p.add_argument('--kernel-config', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
audio = root/'Windows/MtkAudio'
acpi = root/'Silicium-ACPI/Platforms/Teclast/t70pro'
source = (acpi/'Audio.asl').read_text()
inf = (audio/'MtkAudio.inf').read_text()
assert source.count('"HANA6793"') == 1
assert inf.count('ACPI\\HANA6793') == 1
assert 'HANA6793' not in (acpi/'DSDT.asl').read_text()
assert (acpi/'AcpiTables.inf').read_text().splitlines().count('  Audio.asl') == 1
for forbidden in ('PowerResource', '_PR0', '_PR3', '_PS0', '_PS3', 'OperationRegion'):
    assert forbidden not in source
assert source.count('Name (_CCA, Zero)') == 1
config = a.kernel_config.read_text().splitlines()
assert 'CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU=y' in config
assert 'CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE=y' in config
assert 'CONFIG_ARCH_DMA_DEFAULT_COHERENT=y' not in config
assert 'CONFIG_OF_DMA_DEFAULT_COHERENT=y' not in config
node = '/soc/mt6789-afe-pcm@11210000'
for parent in (node, '/soc', '/'):
    properties = subprocess.check_output(['fdtget', '-p', str(a.dtb), parent], text=True).splitlines()
    assert 'dma-coherent' not in properties

def cells(prop):
    return [int(s, 16) for s in subprocess.check_output(
        ['fdtget', '-t', 'x', str(a.dtb), node, prop], text=True).split()]
assert cells('reg') == [0, 0x11210000, 0, 0x2000]
assert cells('interrupts') == [0, 234, 4, 0]
with tempfile.TemporaryDirectory(prefix='t70-audio-ssdt-') as tmp:
    prefix = Path(tmp)/'Audio'
    subprocess.run(['iasl', '-p', str(prefix), str(acpi/'Audio.asl')], check=True)
    aml = prefix.with_suffix('.aml').read_bytes()
    assert aml[:4] == b'SSDT' and struct.unpack_from('<I', aml, 4)[0] == len(aml)
    assert sum(aml) % 256 == 0
    # Exact ACPI large resource descriptors emitted by the source above.
    memory = b'\x86\x09\x00\x01' + struct.pack('<II', 0x11210000, 0x2000)
    irq = b'\x89\x06\x00\x01\x01' + struct.pack('<I', 266)
    assert aml.count(memory) == 1 and aml.count(irq) == 1
    assert aml.count(b'\x08_CCA\x00') == 1
print('Audio binding: project ID, INF/source inclusion, stock MMIO/GSI, noncoherent policy and SSDT descriptors passed; no OS load')
