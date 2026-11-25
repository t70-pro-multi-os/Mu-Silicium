#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Link the opt-in BTIF driver with the existing WDK."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def verify(image, link_map):
    data = image.read_bytes()
    if data[:2] != b'MZ':
        raise ValueError('Not a PE image')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != 0xaa64:
        raise ValueError('Not an ARM64 PE image')
    opt = pe + 24
    if struct.unpack_from('<H', data, opt)[0] != 0x20b or struct.unpack_from('<H', data, opt+68)[0] != 1:
        raise ValueError('Not a native PE32+ image')
    characteristics = struct.unpack_from('<H', data, opt+70)[0]
    if not characteristics & 0x2000:
        raise ValueError('WDM image flag missing')
    if characteristics & 0x8000:
        raise ValueError('Terminal Server aware flag must be clear')
    checksum = struct.unpack_from('<I', data, opt+64)[0]
    total = 0
    for pos in range(0, len(data), 2):
        if opt+64 <= pos < opt+68:
            continue
        total += int.from_bytes(data[pos:pos+2], 'little')
        total = (total & 0xffff) + (total >> 16)
    total = (total & 0xffff) + (total >> 16)
    if not checksum or checksum != (total & 0xffff) + len(data):
        raise ValueError('PE checksum mismatch')
    symbols = {name: int(address, 16) for name, address in re.findall(
        r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+', link_map.read_text(), re.M)}
    base = struct.unpack_from('<Q', data, opt+24)[0]
    entry = base + struct.unpack_from('<I', data, opt+16)[0]
    if symbols.get('FxDriverEntry') != entry or 'DriverEntry' not in symbols or symbols['DriverEntry'] == entry:
        raise ValueError('KMDF framework entry not linked correctly')
    return {'ARM64': True, 'native': True, 'WDM': True, 'checksum': checksum,
            'entry': 'FxDriverEntry', 'sha256': hashlib.sha256(data).hexdigest()}



def verify_t70_wmt_function(root):
    kernel = root.parent / "kernel-reference"
    commit = subprocess.check_output(
        ["git", "-C", str(kernel), "rev-parse", "HEAD"], text=True).strip()
    header_path = "mindone/modules/include/drivers/misc/mediatek/connectivity/common/common_main/include/wmt_exp.h"
    header = subprocess.check_output(
        ["git", "-C", str(kernel), "show", "HEAD:" + header_path], text=True)
    enum_match = re.search(r"^\s*WMTDRV_TYPE_BT\s*=\s*(\d+)\s*,", header, re.M)
    if not enum_match:
        raise ValueError("T70 WMT header does not define WMTDRV_TYPE_BT")
    target = (kernel / "mindone/modules/BUILD.bazel").read_text()
    start = target.find('name = "bt_drv_connac1x"')
    end = target.find("ddk_module(", start + 1) if start >= 0 else -1
    block = target[start:end if end >= 0 else len(target)] if start >= 0 else ""
    if "MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT" not in block:
        raise ValueError("T70 BT target does not enable the WMT enum header")
    driver = (Path(__file__).resolve().parent / "Driver.c").read_text()
    driver_match = re.search(r"#define\s+T70_WMT_FUNCTION_BT\s+(\d+)U", driver)
    if not driver_match or int(driver_match.group(1)) != int(enum_match.group(1)):
        raise ValueError("Windows BT function ID differs from the T70 WMT enum")
    return {
        "commit": commit,
        "header_path": header_path,
        "header_sha256": hashlib.sha256(header.encode()).hexdigest(),
        "enum": "WMTDRV_TYPE_BT",
        "value": int(enum_match.group(1)),
    }


def link_command(linker, objects, image, link_map, libraries):
    return [*linker, '/nologo', '/driver:wdm', '/release', '/machine:arm64',
            '/subsystem:native,10.00', '/entry:FxDriverEntry', '/nodefaultlib',
            '/dynamicbase', '/nxcompat', '/tsaware:no', '/osversion:10.0', '/Brepro',
            '/out:'+str(image), '/map:'+str(link_map), *map(str, objects), *map(str, libraries)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prototype', action='store_true', required=True,
                        help='explicitly opt in to the BTIF driver')
    parser.add_argument('--output-dir', type=Path, required=True,
                        help='new dedicated artifact directory, relative to repository root')
    parser.add_argument('--locked', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    out = args.output_dir.expanduser()
    out = (out if out.is_absolute() else root / out).resolve()
    # A new directory makes accidental replacement of existing package artifacts
    # impossible. No default Windows/Build selection or INF generation exists.
    if out.exists():
        parser.error('output directory must not already exist')
    zig = os.environ.get('T70_WINDOWS_ZIG')
    linker = [zig, 'lld-link'] if zig else [shutil.which('lld-link')]
    if not linker[0] or (zig and not shutil.which(zig)):
        parser.error('existing lld-link or T70_WINDOWS_ZIG is required; nothing was built')
    kernel_function = verify_t70_wmt_function(root)
    if not args.locked:
        os.execv(root / 'Resources/Scripts/with-build-lock', [
            str(root / 'Resources/Scripts/with-build-lock'), 'T70Pro',
            'Link BTIF driver', '--', sys.executable,
            str(Path(__file__).resolve()), '--locked', *sys.argv[1:]])
    wdk = root / 'Windows/Toolchain/microsoft.windows.wdk.arm64/c'
    km = wdk / 'Lib/10.0.26100.0/km/ARM64'
    wdf = wdk / 'Lib/wdf/kmdf/ARM64/1.33'
    libraries = [wdf/'wdfdriverentry.lib', wdf/'wdfldr.lib', km/'ntoskrnl.lib',
                 km/'hal.lib', km/'bufferoverflowfastfailk.lib']
    for library in libraries:
        if not library.is_file():
            parser.error(f'existing official WDK library missing: {library}')
    silicon = root / 'Silicon/MediaTek/MediaTekPkg'
    cores = [silicon/'Library/MtkStpCoreLib'/f'{name}.c'
             for name in ('MtkBtifCore', 'MtkStpCore', 'MtkStpFull', 'MtkWmtMode')]
    inputs = [here/'Driver.c', here/'HciTransport.c', here/'HciTransport.h',
              here/'MtkBtif.inf', here/'check-wdk.py', here.parent/'MtkConn/BtifInterface.h',
              Path(__file__).resolve(), *libraries, *cores]
    inputs += [silicon/'Include/Library'/f'{source.stem}.h' for source in cores]
    hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    with tempfile.TemporaryDirectory(prefix='t70-btif-link-') as tmp:
        stage = Path(tmp)
        obj, image, link_map = stage/'Driver.obj', stage/'MtkBtif.sys', stage/'MtkBtif.map'
        hci_obj = stage/'HciTransport.obj'
        commands = [
            [sys.executable, str(here/'check-wdk.py'), '--object', str(obj)],
            [sys.executable, str(here/'check-wdk.py'), '--hci-object', str(hci_obj)],
        ]
        objects = [obj, hci_obj]
        for source in cores:
            core_object = stage/f'{source.stem}.obj'
            commands.append(['clang', '--target=arm64-pc-windows-msvc', '-O2', '-ffreestanding',
                             '-I', str(silicon/'Include'), '-c', str(source), '-o', str(core_object)])
            objects.append(core_object)
        commands.append(link_command(linker, objects, image, link_map, libraries))
        for command in commands:
            subprocess.run(command, check=True)
        proof = verify(image, link_map)
        if kernel_function != verify_t70_wmt_function(root):
            raise ValueError('T70 WMT function source changed during build')
        inspector = ['xcrun', 'llvm-objdump', '--private-headers'] if sys.platform == 'darwin' else ['llvm-readobj', '--coff-imports']
        imports = subprocess.check_output([*inspector, str(image)], text=True)
        if 'WdfVersionBind' not in imports or 'wdfldr.sys' not in imports.lower():
            raise ValueError('KMDF loader imports missing')
        if hashes != {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}:
            raise ValueError('build inputs changed during link')
        report = {'state': 'BTIF WMT radio-enable path linked, unsigned',
                  't70_kernel_wmt_function': kernel_function,
                  'functional_Bluetooth': False, 'INF_generated': False, 'INF_included': True,
                  'source_and_library_sha256': hashes, 'PE': proof, 'commands': commands,
                  'imports': imports}
        (stage/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
        shutil.copyfile(here/"MtkBtif.inf", stage/"MtkBtif.inf")
        out.mkdir(parents=True, exist_ok=False)
        for name in ('MtkBtif.sys', 'MtkBtif.map', 'MtkBtif.inf', 'verification.json'):
            shutil.copyfile(stage/name, out/name)
    print(f'BTIF WMT radio-enable driver linked: {out}')


if __name__ == '__main__':
    main()
