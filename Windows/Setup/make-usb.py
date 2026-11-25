#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Make a T70 Pro Windows install stick from the official Windows ARM64 ISO.

macOS and Linux. (Windows: make-usb.cmd.)

    sudo python3 make-usb.py --iso Win11_arm64.iso --disk /dev/sdX     (Linux, as root)
    python3 make-usb.py --iso Win11_arm64.iso --disk /dev/disk4        (macOS)

It needs wimlib (brew install wimlib / apt install wimtools) and, on Linux,
dosfstools. The whole stick is erased. The ISO is used as it is; the only
changes are, all on the stick:

  - one FAT32 partition holding the ISO's files;
  - sources\\install.wim split into install*.swm (FAT32 files stop at 4 GiB);
  - the payload folder added to sources\\boot.wim (Setup image) as \\T70,
    with winpeshl.ini so that WinPE runs \\T70\\StartT70Setup.cmd;
  - test signing switched on for the Setup entry of efi\\microsoft\\boot\\bcd.

Everything written is read back and compared before the stick is ejected.
"""
import argparse
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.dont_write_bytecode = True      # leave the kit folder as it was
sys.path.insert(0, str(HERE))
import reghive  # noqa: E402

LABEL = 'T70WIN'
SPLIT_MIB = 3800
FAT32_MAX = 4 * 1024 ** 3 - 1


def die(msg):
    sys.exit(f'make-usb: {msg}')


def run(*cmd, capture=False, check=True, quiet=False):
    if not quiet:
        print('  $', ' '.join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], text=True,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE if capture else None)
    if check and r.returncode:
        die(f'{cmd[0]} failed ({r.returncode})' + (f': {r.stderr.strip()}' if capture else ''))
    return r


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()


def check_kit():
    payload = HERE / 'payload'
    sums = HERE / 'SHA256SUMS.txt'
    if not payload.is_dir() or not sums.is_file():
        die('payload/ or SHA256SUMS.txt missing next to this script; use the kit made by make-kit.py')
    bad = []
    for line in sums.read_text().splitlines():
        if not line.strip():
            continue
        digest, name = line.split(None, 1)
        name = name.lstrip('*')
        p = HERE / name
        if not p.is_file() or sha256(p) != digest:
            bad.append(name)
    if bad:
        die('kit files differ from SHA256SUMS.txt: ' + ', '.join(bad[:5]))
    for need in ('StartT70Setup.cmd', 'winpeshl.ini', 'trust-pe.reg', 'trust-os.reg', 'Drivers/order.txt'):
        if not (payload / need).is_file():
            die(f'payload/{need} missing')
    return payload


def need_tools():
    tools = ['wimlib-imagex']
    tools += ['diskutil', 'hdiutil'] if sys.platform == 'darwin' else ['sfdisk', 'mkfs.vfat', 'mount', 'umount', 'lsblk', 'blockdev']
    missing = [t for t in tools if shutil.which(t) is None]
    if missing:
        hint = 'brew install wimlib' if sys.platform == 'darwin' else 'apt install wimtools dosfstools (or your distribution\'s packages)'
        die('missing tools: ' + ', '.join(missing) + f' ({hint})')


# ---- the target disk ------------------------------------------------------
def linux_disk(dev):
    rows = json.loads(run('lsblk', '-J', '--list', '-b', '-o', 'NAME,PATH,TYPE,SIZE,RM,TRAN,MODEL,MOUNTPOINTS', dev,
                          capture=True, quiet=True).stdout)['blockdevices']
    disk = [r for r in rows if r['path'] == os.path.realpath(dev)]
    if len(disk) != 1:
        die(f'{dev} not found')
    info = disk[0]
    if info['type'] not in ('disk', 'loop'):
        die(f'{dev} is a {info["type"]}, not a whole disk')
    usb = info['type'] == 'loop' or info.get('tran') == 'usb' or info.get('rm') in (True, '1', 1)
    if not usb:
        die(f'{dev} is not a USB or removable disk; refusing to erase it')
    mounts = [m for r in rows for m in (r.get('mountpoints') or []) if m]
    if any(m in ('/', '/boot', '/boot/efi', '/usr', '/home') for m in mounts):
        die(f'{dev} holds a system mount ({mounts}); refusing')
    return dict(size=int(info['size']), name=f"{info.get('model') or info['type']} {dev}", mounts=mounts)


def mac_disk(dev):
    r = run('diskutil', 'info', '-plist', dev, capture=True, quiet=True)
    info = plistlib.loads(r.stdout.encode())
    if not info.get('WholeDisk'):
        die(f'{dev} is not a whole disk')
    proto = info.get('BusProtocol', '')
    if info.get('Internal') and proto != 'Disk Image':
        die(f'{dev} is an internal disk; refusing to erase it')
    if proto not in ('USB', 'Disk Image') and not info.get('Removable') and not info.get('RemovableMedia'):
        die(f'{dev} ({proto}) is not a USB or removable disk; refusing to erase it')
    boot = plistlib.loads(run('diskutil', 'info', '-plist', '/', capture=True, quiet=True).stdout.encode())
    if boot.get('ParentWholeDisk') == info.get('DeviceIdentifier'):
        die(f'{dev} holds the running system; refusing')
    return dict(size=int(info['TotalSize']), name=f"{info.get('MediaName', '')} {dev} ({proto})", mounts=[])


def format_disk(dev, mnt):
    """Erase dev to one MBR FAT32 partition labelled T70WIN, mounted at mnt."""
    if sys.platform == 'darwin':
        run('diskutil', 'unmountDisk', dev)
        run('diskutil', 'eraseDisk', 'FAT32', LABEL, 'MBRFormat', dev)
        part = plistlib.loads(run('diskutil', 'list', '-plist', dev, capture=True, quiet=True).stdout.encode())
        parts = part['AllDisksAndPartitions'][0].get('Partitions', [])
        if len(parts) != 1:
            die(f'expected one partition on {dev} after erasing, found {len(parts)}')
        pdev = '/dev/' + parts[0]['DeviceIdentifier']
        # eraseDisk mounts the new volume under /Volumes, where Spotlight
        # starts indexing it; work on a nobrowse mount it leaves alone.
        run('diskutil', 'unmount', pdev)
        run('diskutil', 'mount', 'nobrowse', '-mountPoint', mnt, pdev)
        remove_mac_housekeeping(mnt)
        return pdev
    for m in linux_disk(dev)['mounts']:
        run('umount', m)
    if shutil.which('wipefs'):
        run('wipefs', '-a', dev)
    r = subprocess.run(['sfdisk', '--quiet', dev], input='label: dos\n,,c,*\n', text=True)
    if r.returncode:
        die('sfdisk failed')
    run('blockdev', '--rereadpt', dev, check=False)
    if shutil.which('udevadm'):
        run('udevadm', 'settle', check=False)
    pdev = None
    for _ in range(50):
        rows = json.loads(run('lsblk', '-J', '--list', '-o', 'PATH,TYPE', dev, capture=True, quiet=True).stdout)
        parts = [r['path'] for r in rows['blockdevices'] if r['type'] == 'part']
        if len(parts) == 1 and os.path.exists(parts[0]):
            pdev = parts[0]
            break
        time.sleep(0.2)
    if pdev is None:
        die(f'the new partition on {dev} did not appear')
    run('mkfs.vfat', '-F', '32', '-n', LABEL, pdev)
    run('mount', '-t', 'vfat', '-o', 'utf8,flush', pdev, mnt)
    return pdev


def unmount(mnt, dev):
    if sys.platform == 'darwin':
        run('diskutil', 'unmount', mnt)
    else:
        run('sync')
        run('umount', mnt)
        run('blockdev', '--flushbufs', dev, check=False)


def remount(pdev, mnt):
    if sys.platform == 'darwin':
        run('diskutil', 'mount', 'readOnly', 'nobrowse', '-mountPoint', mnt, pdev)
    else:
        run('mount', '-t', 'vfat', '-o', 'ro,utf8', pdev, mnt)


# ---- the ISO --------------------------------------------------------------
def attach_iso(iso, mnt):
    if sys.platform == 'darwin':
        out = run('hdiutil', 'attach', '-readonly', '-nobrowse', '-noverify', '-plist',
                  '-mountpoint', mnt, iso, capture=True).stdout
        devs = [e['dev-entry'] for e in plistlib.loads(out.encode())['system-entities'] if 'dev-entry' in e]
        return lambda: run('hdiutil', 'detach', devs[0] if devs else mnt, check=False)
    run('mount', '-o', 'loop,ro', iso, mnt)
    return lambda: run('umount', mnt, check=False)


def find(root, rel):
    """Case-insensitive lookup of rel (a/b/c) under root."""
    p = Path(root)
    for part in rel.split('/'):
        hits = [c for c in p.iterdir() if c.name.lower() == part.lower()] if p.is_dir() else []
        if len(hits) != 1:
            return None
        p = hits[0]
    return p


def check_iso(src):
    for rel in ('sources/setup.exe', 'sources/boot.wim', 'efi/microsoft/boot/bcd', 'efi/boot/bootaa64.efi'):
        if find(src, rel) is None:
            die(f'the ISO has no {rel}; is it the Windows 11 ARM64 ISO?')
    image = find(src, 'sources/install.wim') or find(src, 'sources/install.esd')
    if image is None:
        die('the ISO has no sources/install.wim or install.esd')
    info = run('wimlib-imagex', 'info', find(src, 'sources/boot.wim'), '2', capture=True, quiet=True).stdout
    fields = dict(l.split(':', 1) for l in info.splitlines() if ':' in l)
    if fields.get('Architecture', '').strip() != 'ARM64':
        die('boot.wim image 2 is not ARM64; use the Windows 11 ARM64 ISO')
    if 'Setup' not in fields.get('Name', ''):
        die('boot.wim image 2 is not Windows Setup')
    if image.suffix.lower() == '.esd' and image.stat().st_size > FAT32_MAX:
        die('install.esd is larger than 4 GiB; this script splits only install.wim')
    return image, fields


# ---- the stick ------------------------------------------------------------
def copy_tree(src, dst, skip):
    copied = []
    for root, dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        for f in files:
            r = (rel / f)
            if r.as_posix().lower() in skip:
                continue
            shutil.copyfile(Path(root) / f, dst / r)
            copied.append(r)
    return copied


MAC_HOUSEKEEPING = ('.Spotlight-V100', '.fseventsd', '.Trashes', '.TemporaryItems')


def remove_mac_housekeeping(mnt):
    """macOS writes ._name files beside files and folders on FAT volumes, and
    Spotlight and fseventsd keep folders at the root of volumes they watch."""
    for name in MAC_HOUSEKEEPING:
        p = Path(mnt) / name
        if p.is_dir() and not p.is_symlink():
            shutil.rmtree(p)
        elif p.exists():
            p.unlink()
    for root, dirs, files in os.walk(mnt):
        for f in files:
            if f.startswith('._') and (Path(root) / f[2:]).exists():
                (Path(root) / f).unlink()


def make_boot_wim(src_wim, payload, work):
    wim = work / 'boot.wim'
    shutil.copyfile(src_wim, wim)
    listing = run('wimlib-imagex', 'dir', wim, '2', '--path=/Windows/System32', capture=True, quiet=True).stdout
    if any(l.lower().endswith('/winpeshl.ini') for l in listing.splitlines()):
        die('boot.wim already has a winpeshl.ini; use the unmodified official ISO')
    cmds = work / 'update.txt'
    cmds.write_text(f'add "{payload}" /T70\nadd "{payload / "winpeshl.ini"}" /Windows/System32/winpeshl.ini\n')
    with open(cmds) as f:
        r = subprocess.run(['wimlib-imagex', 'update', str(wim), '2'], stdin=f)
    if r.returncode:
        die('wimlib-imagex update failed')
    run('wimlib-imagex', 'verify', wim)
    return wim


def verify_stick(mnt, src, copied, image, boot_wim, bcd_bytes):
    print('Reading the stick back ...', flush=True)
    bad = [r for r in copied if sha256(mnt / r) != sha256(Path(src) / r)]
    if bad:
        die('files on the stick differ from the ISO: ' + ', '.join(map(str, bad[:5])))
    if sha256(find(mnt, 'sources/boot.wim')) != sha256(boot_wim):
        die('boot.wim on the stick differs from the one made')
    if find(mnt, 'efi/microsoft/boot/bcd').read_bytes() != bcd_bytes:
        die('BCD on the stick differs from the one made')
    h = reghive.Hive(bcd_bytes)
    guid = reghive.bcd_default(h)
    v = h.value(f'Objects\\{guid}\\Elements\\{reghive.TESTSIGNING}', 'Element')
    if v is None or v['data'] != b'\x01':
        die('test signing is not on in the stick BCD')
    expected = {r.as_posix().lower() for r in copied}
    expected |= {find(src, 'sources/boot.wim').relative_to(src).as_posix().lower(),
                 find(src, 'efi/microsoft/boot/bcd').relative_to(src).as_posix().lower()}
    present = set()
    for root, dirs, files in os.walk(mnt):
        for f in files:
            present.add((Path(root) / f).relative_to(mnt).as_posix().lower())
    image_rel = image.relative_to(src).parent.as_posix().lower()
    extra = sorted(p for p in present - expected if not p.startswith(image_rel + '/install'))
    if extra:
        die('the stick has files the ISO does not: ' + ', '.join(extra[:8]))
    sw = sorted(p.name for p in find(mnt, 'sources').iterdir() if p.name.lower().startswith('install'))
    if image.suffix.lower() == '.wim' and image.stat().st_size > FAT32_MAX:
        if any(n.lower() == 'install.wim' for n in sw):
            die('install.wim was copied unsplit')
        first = find(mnt, 'sources/install.swm')
        run('wimlib-imagex', 'verify', first, f'--ref={first.parent}/install*.swm')
    tree = run('wimlib-imagex', 'dir', find(mnt, 'sources/boot.wim'), '2', '--path=/T70', capture=True, quiet=True).stdout
    for need in ('StartT70Setup.cmd', 'trust-pe.reg', 'trust-os.reg', 'Drivers/order.txt'):
        if f'/T70/{need}' not in tree:
            die(f'\\T70\\{need} missing from boot.wim')
    return sw


def release(tmp, src, mnt, detach):
    """Unmount the stick and the ISO, then remove the work folder; never
    remove anything while a volume is still mounted inside it."""
    if os.path.ismount(mnt):
        if sys.platform == 'darwin':
            run('diskutil', 'unmount', mnt, check=False, quiet=True)
        else:
            run('umount', mnt, check=False, quiet=True)
    detach()
    busy = [str(p) for p in (src, mnt) if os.path.ismount(p)]
    if busy:
        print(f'make-usb: still mounted, so {tmp} is left in place: {", ".join(busy)}', file=sys.stderr)
        return
    shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--iso', required=True, help='official Windows 11 ARM64 ISO from microsoft.com')
    ap.add_argument('--disk', required=True, help='the whole USB disk to erase (/dev/sdX, /dev/diskN)')
    ap.add_argument('--yes', action='store_true', help='do not ask before erasing')
    a = ap.parse_args()
    if sys.platform not in ('darwin', 'linux'):
        die('run make-usb.cmd on Windows')
    if sys.platform == 'linux' and os.geteuid() != 0:
        die('run as root (sudo); the whole disk is erased and the ISO mounted')
    need_tools()
    payload = check_kit()
    iso = Path(a.iso).resolve()
    if not iso.is_file():
        die(f'{iso} not found')
    disk = mac_disk(a.disk) if sys.platform == 'darwin' else linux_disk(a.disk)

    tmp = Path(tempfile.mkdtemp(prefix='t70-usb-'))
    src, mnt, work = tmp / 'iso', tmp / 'usb', tmp / 'work'
    for d in (src, mnt, work):
        d.mkdir()
    detach = lambda: None
    try:
        detach = attach_iso(iso, src)
        image, fields = check_iso(src)
        need = sum(f.stat().st_size for f in Path(src).rglob('*') if f.is_file()) + (64 << 20)
        if disk['size'] < need:
            die(f'{a.disk} is {disk["size"] >> 20} MiB; at least {need >> 20} MiB is needed')
        print(f'ISO: {iso.name} (Setup {fields.get("Build", "").strip()}.{fields.get("Service Pack Build", "").strip()}, '
              f'{fields.get("Default Language", "").strip()})')
        print(f'Disk: {disk["name"]}, {disk["size"] / 1e9:.1f} GB. EVERYTHING ON IT WILL BE ERASED.')
        if not a.yes and input(f'Type the disk name ({a.disk}) to continue: ').strip() != a.disk:
            die('cancelled; nothing written')

        print('Making the Setup image ...', flush=True)
        boot_wim = make_boot_wim(find(src, 'sources/boot.wim'), payload, work)
        bcd_src = find(src, 'efi/microsoft/boot/bcd')
        bcd_bytes, guid, state = reghive.bcd_testsigning(bcd_src.read_bytes())
        print(f'  BCD: testsigning on for {guid} ({state})')

        print('Erasing and formatting the stick ...', flush=True)
        pdev = format_disk(a.disk, mnt)
        boot_rel = find(src, 'sources/boot.wim').relative_to(src).as_posix().lower()
        image_rel = image.relative_to(src).as_posix().lower()
        bcd_rel = bcd_src.relative_to(src).as_posix().lower()
        print('Copying the ISO files ...', flush=True)
        copied = copy_tree(src, mnt, {boot_rel, image_rel, bcd_rel})
        shutil.copyfile(boot_wim, mnt / find(src, 'sources/boot.wim').relative_to(src))
        (mnt / bcd_src.relative_to(src)).write_bytes(bcd_bytes)
        if image.suffix.lower() == '.wim' and image.stat().st_size > FAT32_MAX:
            print(f'Splitting {image.name} ({image.stat().st_size >> 20} MiB) ...', flush=True)
            run('wimlib-imagex', 'split', image, mnt / image.parent.relative_to(src) / 'install.swm', SPLIT_MIB)
        else:
            shutil.copyfile(image, mnt / image.relative_to(src))
        if sys.platform == 'darwin':
            remove_mac_housekeeping(mnt)
        unmount(mnt, a.disk)
        remount(pdev, mnt)
        parts = verify_stick(mnt, src, copied, image, boot_wim, bcd_bytes)
        unmount(mnt, a.disk)
    finally:
        release(tmp, src, mnt, detach)
    if sys.platform == 'darwin':
        run('diskutil', 'eject', a.disk, check=False)
    print(f'\nDone. {len(copied)} ISO files, install image as {", ".join(parts)}, '
          f'Setup image with the T70 drivers, test signing on.')
    print('Plug the stick into the T70 Pro and start it from the stick.')


if __name__ == '__main__':
    main()
