#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# Host checks only: the launcher's order of steps, the BCD editor against a
# hive Windows wrote, and the kit builder. Windows Setup itself runs in the
# virtual-PC test, not here.
import hashlib
import importlib.util
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
source = (HERE / 'StartT70Setup.cmd').read_text()
usb_test = HERE.parent.joinpath('Tests', 'check-usb-winpe.cmd').read_text()


def before(a, b, text=source):
    assert text.index(a) < text.index(b), (a, b)


for script in (source, usb_test):
    assert script.lower().count('wpeinit.exe') == 1
    assert 'waitforremovablestorage' not in script.lower()
    assert script.index('wpeinit.exe') < script.index('for /l %%R')
before('for /l %%R', 'call :Probe')
assert usb_test.index('for /l %%R') < usb_test.index('call :CheckUSB')
assert source.count('ambiguous=0') == 1
assert all(name in source for name in ('install.wim', 'install.esd', 'install*.swm'))
assert '%SystemDrive%' in source
assert 'HKLM\\SYSTEM\\Setup\\LabConfig' in source and 'Bypass%%CCheck' in source
assert all(c in source for c in ('TPM', 'SecureBoot', 'CPU', 'RAM', 'Storage'))
# Certificates, then drivers (the stick is behind the T70 USB controller), then the media.
before('wpeinit.exe', 'trust-pe.reg')
before('trust-pe.reg', 'order.txt')
before('order.txt', 'for /l %%R')
before('LabConfig', 'sources\\setup.exe" /noreboot')
# Setup starts only from :Found, reached when :Resolve saw exactly one medium.
resolve = source[source.index('\n:Resolve\n'):source.index('\n:PickLetter\n')]
before('More than one Windows installation medium', 'exit /b 3', resolve)
assert source.count('goto :Found') == 1 and source.count('/noreboot') == 1
before(':Found\n', 'sources\\setup.exe" /noreboot')
# The boot store is kept before Setup and compared after it, so a closed
# Setup leaves an existing Windows alone.
before('"%T70%\\bcd-before" >nul', 'sources\\setup.exe" /noreboot')
# The EFI system partition takes a letter only through mountvol /S.
assert source.count('mountvol.exe %ESP% /S') == 1 and 'mountvol.exe %ESP% "%' not in source
before('sources\\setup.exe" /noreboot', 'fc.exe /b')
before('fc.exe /b', 'testsigning on')
# The new Windows gets test signing, drivers and certificates before the restart.
before('testsigning on', '/Add-Driver')
before('/Add-Driver', 'trust-os.reg')
before('trust-os.reg', 'wpeutil.exe reboot')
assert source.count('reg.exe unload HKLM\\T70OS') == 1
assert source.count('call :PickLetter') == 1     # only when the ESP has no letter yet
# Right after Setup the ESP still has Setup's letter: look for it first,
# never counting the stick or X:, and only then mount it with /S.
esp = source[source.index('\n:MountEsp\n'):source.index('\n:FindTarget\n')]
before('call :EspLetter', 'mountvol.exe %ESP% /S', esp)
assert 'if /i "%~1"=="%SystemDrive%" exit /b 0' in esp and 'if /i "%~1"=="%installMedia%" exit /b 0' in esp
assert source.count('if defined espMounted mountvol.exe %ESP% /D') == 3
assert 'mountvol.exe S:' not in source            # the letter comes from :PickLetter

spec = importlib.util.spec_from_file_location('reghive', HERE / 'reghive.py')
reghive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reghive)
mk = importlib.util.spec_from_file_location('make_kit', HERE / 'make-kit.py')
make_kit = importlib.util.module_from_spec(mk)
mk.loader.exec_module(make_kit)

# The lh hash is the one Windows writes.
assert reghive._lh_hash('16000049') == (((((((ord('1') * 37 + ord('6')) * 37 + ord('0')) * 37 + ord('0')) * 37
                                          + ord('0')) * 37 + ord('0')) * 37 + ord('4')) * 37 + ord('9')) & 0xFFFFFFFF

# A BCD given on the command line (the official ISO's efi/microsoft/boot/bcd):
# the edit adds exactly the key Windows' own bcdedit adds, nothing else.
if len(sys.argv) > 1:
    original = Path(sys.argv[1]).read_bytes()
    data, guid, state = reghive.bcd_testsigning(original)
    assert state == 'created' and guid == '{7619dcc9-fafe-11d9-b411-000476eba25f}'
    assert reghive.bcd_testsigning(data)[2] == 'unchanged'

    def tree(h, k, path=''):
        out = {(path, v['name']): (v['type'], v['data']) for v in h.values(k)}
        for s in h.subkeys(k):
            out.update(tree(h, s, path + '\\' + s['name']))
        return out
    a, b = reghive.Hive(data), reghive.Hive(original)
    ta, tb = tree(a, a.key(a.root)), tree(b, b.key(b.root))
    added = {k: ta[k] for k in ta.keys() - tb.keys()}
    assert added == {(f'\\Objects\\{guid}\\Elements\\16000049', 'Element'): (3, b'\x01')}, added
    assert not (tb.keys() - ta.keys()) and all(ta[k] == tb[k] for k in tb)
    new = a.open(f'Objects\\{guid}\\Elements\\16000049')
    assert (new['flags'], new['nval'], new['maxvname'], new['maxvdata']) == (0x20, 1, 14, 1)
    names = [s['name'] for s in a.subkeys(a.open(f'Objects\\{guid}\\Elements'))]
    assert names == sorted(names, key=str.upper)

# The certificate blob is the SHA-1 and DER properties Windows stores.
with tempfile.TemporaryDirectory() as tmp:
    key = Path(tmp) / 'k.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key),
                    '-subj', '/CN=Test Root', '-days', '1', '-outform', 'DER', '-out', str(Path(tmp) / 'r.cer')],
                   check=True, capture_output=True)
    der = (Path(tmp) / 'r.cer').read_bytes()
issuer, subject = make_kit.cert_names(der)
assert issuer == subject
blob = make_kit.store_blob(der)
assert blob[:12] == struct.pack('<III', 3, 1, 20) and blob[12:32] == hashlib.sha1(der).digest()
assert blob[32:44] == struct.pack('<III', 0x20, 1, len(der)) and blob[44:] == der
reg = make_kit.reg_file('HKEY_LOCAL_MACHINE\\SOFTWARE', [('ROOT', der)]).decode('utf-16')
assert reg.startswith('Windows Registry Editor Version 5.00\r\n')
hexes = ''.join(c for c in reg.split('"Blob"=hex:')[1] if c in '0123456789abcdef')
assert bytes.fromhex(hexes) == blob
print('StartT70Setup, reghive and make-kit checks passed')
