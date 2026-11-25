#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the T70 Pro Windows install kit from a signed driver set.

    python3 make-kit.py --drivers SIGNED_DIR --out KIT_DIR

SIGNED_DIR holds one folder per driver package (.inf, .sys, .cat) and the
.cer files they are signed with. The kit is what a user downloads: the
stick makers for Windows, macOS and Linux, the install guide, and payload/,
which they put into the official ISO's Setup image as X:\\T70.
"""
import argparse
import hashlib
import shutil
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Driver load order in WinPE: buses and pins first, the stick's USB last.
ORDER = ['MtkGpio', 'MtkI2c', 'MtkUfsPower', 'MtkUfs', 'MtkSd', 'MtkUsb']
KIT_FILES = ['make-usb.py', 'reghive.py', 'make-usb.cmd', 'make-usb.ps1', 'README.md']


def die(msg):
    sys.exit(f'make-kit: {msg}')


def der_items(b, i, end):
    while i < end:
        tag = b[i]
        n = b[i + 1]
        i += 2
        if n & 0x80:
            k = n & 0x7F
            n = int.from_bytes(b[i:i + k], 'big')
            i += k
        yield tag, i, i + n
        i += n


def cert_names(der):
    """(issuer, subject) DER of an X.509 certificate."""
    (tag, s, e), = der_items(der, 0, len(der))
    tag, s, e = next(der_items(der, s, e))       # tbsCertificate
    fields = list(der_items(der, s, e))
    if fields[0][0] == 0xA0:                      # explicit version
        fields = fields[1:]
    issuer, subject = fields[2], fields[4]
    return der[issuer[1]:issuer[2]], der[subject[1]:subject[2]]


def store_blob(der):
    """Certificate as Windows keeps it under SystemCertificates: SHA-1 and DER properties."""
    sha1 = hashlib.sha1(der).digest()
    return struct.pack('<III', 3, 1, 20) + sha1 + struct.pack('<III', 0x20, 1, len(der)) + der


def reg_file(root, certs):
    lines = ['Windows Registry Editor Version 5.00', '']
    for store, der in certs:
        thumb = hashlib.sha1(der).hexdigest().upper()
        lines.append(f'[{root}\\Microsoft\\SystemCertificates\\{store}\\Certificates\\{thumb}]')
        hexes = [f'{b:02x}' for b in store_blob(der)]
        text = '"Blob"=hex:'
        out = []
        for i, h in enumerate(hexes):
            piece = h + (',' if i + 1 < len(hexes) else '')
            if len(text) + len(piece) > 77:
                out.append(text + '\\')
                text = '  '
            text += piece
        out.append(text)
        lines += out + ['']
    return ('\r\n'.join(lines) + '\r\n').encode('utf-16')     # with BOM, as regedit writes


def crlf(path):
    return path.read_text().replace('\r\n', '\n').replace('\n', '\r\n').encode('ascii')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--drivers', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    a = ap.parse_args()
    src = a.drivers.resolve()
    if a.out.exists():
        die(f'{a.out} exists; give a new folder')

    packages = {}
    for d in sorted(p for p in src.iterdir() if p.is_dir()):
        infs = list(d.glob('*.inf'))
        if len(infs) != 1 or not list(d.glob('*.cat')) or not list(d.glob('*.sys')):
            die(f'{d.name}: expected one .inf with its .cat and .sys')
        packages[d.name] = infs[0].name
    if 'MtkUsb' not in packages:
        die('no MtkUsb package; the stick cannot be read without it')
    order = [n for n in ORDER if n in packages] + sorted(n for n in packages if n not in ORDER)

    certs = []
    for c in sorted(src.glob('*.cer')):
        der = c.read_bytes()
        issuer, subject = cert_names(der)
        certs.append(('ROOT' if issuer == subject else 'TrustedPublisher', der))
    if not any(s == 'ROOT' for s, _ in certs) or not any(s == 'TrustedPublisher' for s, _ in certs):
        die('expected the test root (.cer, self-signed) and the publisher (.cer) next to the packages')

    out = a.out
    payload = out / 'payload'
    (payload / 'Drivers').mkdir(parents=True)
    for name in packages:
        shutil.copytree(src / name, payload / 'Drivers' / name)
    (payload / 'Drivers' / 'order.txt').write_bytes(
        ('; drvload order\r\n' + ''.join(f'{n}\\{packages[n]}\r\n' for n in order)).encode('ascii'))
    (payload / 'certs').mkdir()
    for c in sorted(src.glob('*.cer')):
        shutil.copyfile(c, payload / 'certs' / c.name)
    (payload / 'trust-pe.reg').write_bytes(reg_file('HKEY_LOCAL_MACHINE\\SOFTWARE', certs))
    (payload / 'trust-os.reg').write_bytes(reg_file('HKEY_LOCAL_MACHINE\\T70OS', certs))
    (payload / 'StartT70Setup.cmd').write_bytes(crlf(HERE / 'StartT70Setup.cmd'))
    (payload / 'winpeshl.ini').write_bytes(crlf(HERE / 'winpeshl.ini'))
    for f in KIT_FILES:
        if f.endswith(('.cmd', '.ps1')):
            (out / f).write_bytes(crlf(HERE / f))
        else:
            shutil.copyfile(HERE / f, out / f)

    sums = []
    for p in sorted(out.rglob('*')):
        if p.is_file():
            sums.append(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(out).as_posix()}')
    (out / 'SHA256SUMS.txt').write_text('\n'.join(sums) + '\n')
    print(f'kit: {out} ({len(packages)} driver packages: {", ".join(order)}; '
          f'{len(certs)} certificates)')


if __name__ == '__main__':
    main()
