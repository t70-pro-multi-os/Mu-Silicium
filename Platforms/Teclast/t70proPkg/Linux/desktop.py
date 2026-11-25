#!/usr/bin/env python3
"""Cross-build the real Weston desktop against builder's Linux libraries."""
import argparse
import collections
import datetime
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

PKG = Path(__file__).resolve().parents[1]
REPO = PKG.parents[2]
PROJECT = REPO.parent
SYSROOT = PROJECT.parents[2]
DEPS = PROJECT / 'tools/linux-desktop-deps'
BUILD = REPO / 'Build/t70proPkg/LinuxDesktop'
SOURCE = PROJECT / 'weston'
REPORT = PROJECT / 'analysis/linux-desktop-build.json'
PACKAGES = '''libpixman-1-dev libinput-dev libasound2-dev libseat-dev libcairo2-dev libpng-dev
libgbm-dev liblcms2-dev libdisplay-info-dev libevdev-dev libvulkan-dev
libpango1.0-dev seatd fontconfig fonts-noto-cjk dmz-cursor-theme'''.split()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def paragraphs(text):
    for raw in text.split('\n\n'):
        result = {}
        key = None
        for line in raw.splitlines():
            if line.startswith(' ') and key:
                result[key] += ' ' + line[1:]
            elif ':' in line:
                key, value = line.split(':', 1)
                result[key] = value.lstrip()
        if 'Package' in result:
            yield result


def compare_version(left, right):
    def split(value):
        epoch, sep, rest = value.partition(':')
        epoch, value = (int(epoch), rest) if sep else (0, value)
        upstream, sep, revision = value.rpartition('-')
        return epoch, upstream if sep else value, revision if sep else '0'

    def part(a, b):
        def order(c):
            return -1 if c == '~' else 0 if not c or c.isdigit() else ord(c) if c.isalpha() else ord(c) + 256
        while a or b:
            while (a and not a[0].isdigit()) or (b and not b[0].isdigit()):
                x, y = order(a[:1]), order(b[:1])
                if x != y:
                    return (x > y) - (x < y)
                a, b = a[1:], b[1:]
            a, b = a.lstrip('0'), b.lstrip('0')
            x = re.match(r'\d*', a)[0]
            y = re.match(r'\d*', b)[0]
            if len(x) != len(y):
                return (len(x) > len(y)) - (len(x) < len(y))
            if x != y:
                return (x > y) - (x < y)
            a, b = a[len(x):], b[len(y):]
        return 0
    a, b = split(left), split(right)
    return (a[0] > b[0]) - (a[0] < b[0]) or part(a[1], b[1]) or part(a[2], b[2])


def relation(text):
    m = re.fullmatch(r'([a-z0-9][a-z0-9+.-]*)(?::(?:any|native|arm64))?(?:\s*\((<<|<=|=|>=|>>) ([^)]+)\))?', text.strip())
    if not m:
        raise ValueError('Unsupported package relation: ' + text)
    return m.groups()


def satisfies(version, rel):
    _, op, target = rel
    if op is None:
        return True
    c = compare_version(version, target)
    return {'<<': c < 0, '<=': c <= 0, '=': c == 0, '>=': c >= 0, '>>': c > 0}[op]


def dependency_plan():
    installed = {p['Package']: p for p in paragraphs((SYSROOT/'var/lib/dpkg/status').read_text())
                 if p.get('Status') == 'install ok installed'}
    available = {}
    for suite in ['resolute', 'resolute-updates', 'resolute-security']:
        for component in ['main', 'universe']:
            path = SYSROOT/'var/lib/apt/lists'/f'ports.ubuntu.com_ubuntu-ports_dists_{suite}_{component}_binary-arm64_Packages'
            if not path.is_file():
                raise ValueError('Missing existing builder package index: ' + str(path))
            for p in paragraphs(path.read_text()):
                old = available.get(p['Package'])
                if old is None or compare_version(p['Version'], old['Version']) > 0:
                    available[p['Package']] = p
    selected, reused = {}, {}
    providers = {}
    for catalog_name, catalog in [('installed', installed), ('available', available)]:
        for p in catalog.values():
            for item in p.get('Provides', '').split(','):
                if not item.strip():
                    continue
                virtual, _, version = relation(item)
                providers.setdefault((catalog_name, virtual), []).append((p, version))

    def candidates(rel, catalog_name, catalog):
        name, op, _ = rel
        if name in catalog and satisfies(catalog[name]['Version'], rel):
            yield catalog[name]
        for p, version in providers.get((catalog_name, name), []):
            if p['Package'] not in catalog:
                continue
            if op is None or (version is not None and satisfies(version, rel)):
                yield p

    pending = PACKAGES[:]
    while pending:
        alternatives = [relation(x) for x in pending.pop(0).split('|')]
        matched = False
        for rel in alternatives:
            matches = list(candidates(rel, 'installed', installed))
            if matches:
                p = matches[0]
                reused[p['Package']] = p['Version']
                matched = True
                break
            if list(candidates(rel, 'available', selected)):
                matched = True
                break
        if matched:
            continue
        for rel in alternatives:
            matches = list(candidates(rel, 'available', available))
            if not matches:
                continue
            p = matches[0]
            name = p['Package']
            if name in selected:
                if selected[name]['Version'] != p['Version']:
                    raise ValueError('Conflicting versions for ' + name)
                matched = True
                break
            selected[name] = p
            for key in ['Pre-Depends', 'Depends']:
                if p.get(key):
                    pending.extend(p[key].split(','))
            matched = True
            break
        if not matched:
            raise ValueError('Unsatisfied package dependency: ' + str(alternatives))
    return selected, reused


def deb_data(data):
    if data[:8] != b'!<arch>\n':
        raise ValueError('Invalid Debian ar header')
    offset = 8
    result = None
    while offset < len(data):
        header = data[offset:offset+60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid Debian ar member')
        size = int(header[48:58])
        if size < 0:
            raise ValueError('Negative Debian ar member size')
        name = header[:16].decode('ascii').strip().rstrip('/')
        offset += 60
        if offset + size > len(data):
            raise ValueError('Truncated Debian archive')
        if name.startswith('data.tar.'):
            if result is not None:
                raise ValueError('Duplicate Debian data archive')
            result = data[offset:offset+size]
        offset += size + size % 2
    if result is None or offset != len(data):
        raise ValueError('Missing Debian data archive')
    return result


def relative_path(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts:
        raise ValueError('Archive path outside dependency root')
    return path


def stage_dependencies():
    selected, reused = dependency_plan()
    if DEPS.is_symlink() or not DEPS.resolve().is_relative_to(PROJECT.resolve()):
        raise ValueError('Dependency root must remain inside the builder project')
    DEPS.mkdir(parents=True, exist_ok=True)
    manifest = DEPS/'packages.json'
    previous = json.loads(manifest.read_text()).get('packages', {}) if manifest.exists() else {}
    records = {}
    for name, p in sorted(selected.items()):
        identity = {k: p[k] for k in ['Version', 'Architecture', 'Filename', 'Size', 'SHA256']}
        if name in previous and previous[name]['identity'] == identity:
            valid = all((DEPS/n).is_file() and sha((DEPS/n).read_bytes()) == h for n, h in previous[name]['files'].items())
            valid &= all((DEPS/n).is_symlink() and os.readlink(DEPS/n) == t for n, t in previous[name]['links'].items())
            if valid:
                records[name] = previous[name]
                continue
        filename = relative_path(p['Filename'])
        if not str(filename).startswith('pool/') or not re.fullmatch('[0-9a-f]{64}', p['SHA256']):
            raise ValueError('Untrusted package identity')
        cache = SYSROOT/'var/cache/apt/archives'/filename.name
        data = cache.read_bytes() if cache.is_file() and sha(cache.read_bytes()) == p['SHA256'] else None
        if data is None:
            with urllib.request.urlopen('https://ports.ubuntu.com/ubuntu-ports/' + str(filename), timeout=45) as response:
                data = response.read(int(p['Size']) + 1)
        if len(data) != int(p['Size']) or sha(data) != p['SHA256']:
            raise ValueError('Package hash or length mismatch: ' + name)
        files, links = {}, {}
        with tarfile.open(fileobj=io.BytesIO(deb_data(data)), mode='r:*') as archive:
            for member in archive:
                rel = relative_path(member.name)
                if rel == PurePosixPath('.'):
                    if not member.isdir():
                        raise ValueError('Dependency archive root must be a directory')
                    continue
                target = DEPS/rel
                if not target.parent.resolve(strict=False).is_relative_to(DEPS.resolve()):
                    raise ValueError('Dependency parent symlink escapes root')
                if member.isdir():
                    if not target.resolve(strict=False).is_relative_to(DEPS.resolve()):
                        raise ValueError('Dependency directory escapes root')
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                if not member.isfile() and not member.issym():
                    raise ValueError('Unsupported dependency archive member')
                target.parent.mkdir(parents=True, exist_ok=True)
                if not target.parent.resolve().is_relative_to(DEPS.resolve()):
                    raise ValueError('Dependency parent symlink escapes root')
                if member.issym():
                    link = member.linkname
                    if link.startswith('/'):
                        link = os.path.relpath(DEPS/link[1:], target.parent)
                    if not (target.parent/link).resolve(strict=False).is_relative_to(DEPS.resolve()):
                        raise ValueError('Dependency symlink outside root')
                    if target.is_symlink():
                        if os.readlink(target) != link:
                            raise ValueError('Conflicting dependency link')
                    elif target.exists():
                        raise ValueError('Dependency link replaces a file')
                    else:
                        target.symlink_to(link)
                    links[str(rel)] = link
                else:
                    content = archive.extractfile(member).read()
                    if target.is_symlink() or (target.exists() and target.read_bytes() != content):
                        raise ValueError('Conflicting dependency file: ' + str(rel))
                    target.write_bytes(content)
                    target.chmod(member.mode & 0o777)
                    files[str(rel)] = sha(content)
        records[name] = {'identity': identity, 'files': files, 'links': links}
        manifest.write_text(json.dumps({'packages': records, 'reused_builder_packages': reused}, indent=2) + '\n')
        print('Staged', name, p['Version'], flush=True)
    manifest.write_text(json.dumps({'packages': records, 'reused_builder_packages': reused}, indent=2) + '\n')
    return records


def pkg_config(args):
    env = os.environ.copy()
    env['PKG_CONFIG_LIBDIR'] = ':'.join(str(root/d) for root in [DEPS, SYSROOT] for d in
                                     ['usr/lib/aarch64-linux-gnu/pkgconfig', 'usr/share/pkgconfig'])
    env['PKG_CONFIG_SYSROOT_DIR'] = str(DEPS)
    env.pop('PKG_CONFIG_PATH', None)
    result = subprocess.run(['/opt/homebrew/bin/pkg-config', *args], env=env, capture_output=True, text=True)
    words = shlex.split(result.stdout)
    for i, word in enumerate(words):
        prefix = word[:2] if word.startswith(('-I', '-L')) else ''
        path = word[len(prefix):]
        if path.startswith(str(DEPS) + '/'):
            relative = path[len(str(DEPS))+1:]
            if not Path(path).exists() and (SYSROOT/relative).exists():
                words[i] = prefix + str(SYSROOT/relative)
        elif path.startswith('/usr/') and (SYSROOT/path[1:]).exists():
            words[i] = prefix + str(SYSROOT/path[1:])
    print(shlex.join(words) if any(x in args for x in ['--cflags', '--libs', '--cflags-only-I', '--libs-only-L']) else ' '.join(words))
    sys.stderr.write(result.stderr)
    return result.returncode


def compile_shader(args):
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=Path)
    parser.add_argument('--quiet', action='store_true')
    parser.add_argument('--variable-name', required=True)
    parser.add_argument('-V', action='store_true', required=True)
    parser.add_argument('-x', action='store_true', required=True)
    parser.add_argument('-o', type=Path, required=True)
    a = parser.parse_args(args)
    if not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*', a.variable_name):
        raise ValueError('Invalid shader array name')
    result = subprocess.run(['/opt/homebrew/bin/glslc', '--target-env=vulkan1.0', str(a.input), '-o', '-'],
                            check=True, capture_output=True)
    data = result.stdout
    if len(data) < 20 or len(data) % 4:
        raise ValueError('Invalid compiler SPIR-V size')
    words = struct.unpack('<' + 'I' * (len(data)//4), data)
    if words[0] != 0x07230203 or words[1] != 0x10000 or not words[3] or words[4]:
        raise ValueError('Invalid compiler SPIR-V header')
    offset, entry = 5, False
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xffff
        if not count or offset + count > len(words):
            raise ValueError('Invalid compiler SPIR-V instruction bounds')
        if opcode == 15:
            entry = True
        offset += count
    if not entry:
        raise ValueError('Missing SPIR-V entry point')
    a.o.write_text('#pragma once\n#include <stdint.h>\nstatic const uint32_t ' + a.variable_name + '[] = {\n' +
                   ',\n'.join('0x%08x' % word for word in words) + '\n};\n')


def link_builder_runtime():
    record = BUILD/'builder-runtime-links.json'
    links = json.loads(record.read_text()) if record.exists() else {}
    for path in DEPS.rglob('*.so*'):
        if not path.is_symlink() or path.exists():
            continue
        missing = path.resolve(strict=False)
        if not missing.is_relative_to(DEPS.resolve()):
            raise ValueError('Unresolved dependency link escapes root')
        existing = SYSROOT/missing.relative_to(DEPS.resolve())
        if existing.is_file():
            missing.symlink_to(existing)
            links[str(missing.relative_to(DEPS))] = {'source': str(existing), 'sha256': sha(existing.read_bytes())}
    return links


def configure_and_build():
    BUILD.mkdir(parents=True, exist_ok=True)
    cross = BUILD.parent/'linux-desktop-cross.ini'
    native = BUILD.parent/'linux-desktop-native.ini'
    runtime_links = link_builder_runtime()
    (BUILD/'builder-runtime-links.json').write_text(json.dumps(runtime_links, indent=2)+'\n')
    zig = os.environ['T70_MALI_ZIG']
    compiler = ['--target=aarch64-linux-gnu', '--sysroot='+str(SYSROOT), '--gcc-toolchain='+str(SYSROOT/'usr'),
                '-isystem', str(SYSROOT/'usr/include/aarch64-linux-gnu'), '-I'+str(DEPS/'usr/include'),
                '-I'+str(DEPS/'usr/include/aarch64-linux-gnu'), '-B'+str(PROJECT/'tools/native-macos')]
    link = ['-fuse-ld=lld', '-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1',
            '-L'+str(DEPS/'usr/lib/aarch64-linux-gnu'), '-Wl,-rpath-link,'+str(DEPS/'usr/lib/aarch64-linux-gnu'),
            '-Wl,-rpath-link,'+str(SYSROOT/'usr/lib/aarch64-linux-gnu')]
    cross.write_text('[binaries]\nc = '+repr(['/usr/bin/clang', *compiler])+'\ncpp = '+repr(['/usr/bin/clang++', *compiler])+ '\nar = '+repr([zig, 'ar'])+
                     '\npkg-config = '+repr([sys.executable, str(Path(__file__).resolve()), 'pkg-config'])+
                     '\nglslangValidator = '+repr([sys.executable, str(Path(__file__).resolve()), 'shader'])+
                     "\n[host_machine]\nsystem = 'linux'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n"+
                     '[properties]\nneeds_exe_wrapper = true\n[built-in options]\nc_link_args = '+repr(link)+'\ncpp_link_args = '+repr(link)+'\n')
    native_pc = BUILD/'native-pkgconfig'
    native_pc.mkdir(exist_ok=True)
    scanner_pc = (PROJECT/'tools/wayland/build/meson-private/wayland-scanner.pc').read_text()
    scanner = PROJECT/'tools/wayland/build/src/wayland-scanner'
    if subprocess.check_output([str(scanner), '--version'], text=True, stderr=subprocess.STDOUT).strip() != 'wayland-scanner 1.24.0':
        raise ValueError('Unexpected native scanner version')
    scanner_pc = re.sub(r'^wayland_scanner=.*$', 'wayland_scanner='+str(scanner), scanner_pc, flags=re.M)
    (native_pc/'wayland-scanner.pc').write_text(scanner_pc)
    native.write_text("[binaries]\nc = '/usr/bin/clang'\ncpp = '/usr/bin/clang++'\npkg-config = '/opt/homebrew/bin/pkg-config'\n"+
                      "[built-in options]\npkg_config_path = ["+repr(str(native_pc))+"]\n")
    options = ['--prefix=/usr', '--libdir=lib/aarch64-linux-gnu', '--libexecdir=libexec', '--buildtype=release', '--wrap-mode=nofallback',
               '-Dbackend-drm=true', '-Dbackend-headless=true', '-Dbackend-wayland=true', '-Dbackend-default=drm',
               '-Drenderer-gl=false', '-Drenderer-vulkan=true', '-Dsystemd=false', '-Dxwayland=false',
               '-Dshell-desktop=true', '-Dshell-ivi=false', '-Dshell-kiosk=false', '-Dshell-lua=false',
               '-Dcolor-management-lcms=true', '-Dimage-jpeg=false', '-Dimage-webp=false',
               '-Dtools=debug,info,terminal', '-Dsimple-clients=damage,shm,touch,im', '-Ddemo-clients=false', '-Dtests=false', '-Ddoc=false']
    options += ['-Dbackend-'+name+'=false' for name in ['pipewire', 'rdp', 'vnc', 'x11']]
    env = os.environ.copy()
    env['T70_EDK_ZIG'] = zig
    env['PATH'] = str(PROJECT/'tools/wayland/build/src') + ':' + env['PATH']
    command = ['/opt/homebrew/bin/meson', 'setup']
    if (BUILD/'meson-private/coredata.dat').exists():
        command += ['--reconfigure', '--clearcache']
    subprocess.run([*command, str(BUILD), str(SOURCE), '--cross-file='+str(cross), '--native-file='+str(native), *options], check=True, env=env)
    subprocess.run(['/opt/homebrew/bin/ninja', '-C', str(BUILD), '-j6'], check=True, env=env)


def elf_info(path):
    from console import arm64_elf
    data = path.read_bytes()
    interpreter = arm64_elf(data)
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    for i in range(count):
        kind, flags = struct.unpack_from('<II', data, offset+i*size)
        if kind == 1 and flags & 3 == 3:
            raise ValueError('Writable executable Linux segment: ' + str(path))
    text = subprocess.check_output(['xcrun', 'llvm-objdump', '--private-headers', str(path)], text=True)
    needed = re.findall(r'^\s*NEEDED\s+(\S+)\s*$', text, re.M)
    soname = re.findall(r'^\s*SONAME\s+(\S+)\s*$', text, re.M)
    rpaths = re.findall(r'^\s*(?:RUNPATH|RPATH)\s+(\S+)\s*$', text, re.M)
    for name in needed + soname:
        if not re.fullmatch(r'[A-Za-z0-9_.+-]+', name):
            raise ValueError('Invalid ELF dependency name')
    if any('/Users/' in value or '/home/builder/' in value for value in rpaths):
        raise ValueError('Build-machine path retained in installed ELF: ' + str(path))
    return {'bytes': len(data), 'sha256': sha(data), 'needed': needed, 'soname': soname[0] if soname else None,
            'interpreter': interpreter, 'rpaths': rpaths}


def runtime_catalog():
    stage = BUILD/'stage'
    installed = json.loads(subprocess.check_output(['/opt/homebrew/bin/meson', 'introspect', '--installed', str(BUILD)], text=True))
    built, roots, artifacts = {}, [], []
    for destination in sorted(set(installed.values())):
        source = stage/destination[1:]
        if not source.is_file() or source.is_symlink() or source.read_bytes()[:4] != b'\x7fELF':
            continue
        info = elf_info(source)
        name = str(PurePosixPath(destination).parent/info['soname']) if info['soname'] else destination
        artifacts.append({'path': str(source.relative_to(PROJECT)), 'install_path': name, **info})
        roots.append((name, source))
        if info['soname']:
            built[info['soname']] = (name, source)
    required = ['weston', 'vulkan-renderer.so', 'drm-backend.so', 'desktop-shell.so', 'weston-terminal',
                'weston-desktop-shell', 'weston-keyboard']
    names = {PurePosixPath(x['install_path']).name: x for x in artifacts}
    if any(n not in names for n in required):
        raise ValueError('Missing required Weston desktop binary')
    assert {'libvulkan.so.1', 'libgbm.so.1'} <= set(names['vulkan-renderer.so']['needed'])
    assert {'libseat.so.1', 'libinput.so.10', 'libudev.so.1'} <= set(names['drm-backend.so']['needed'])
    # GBM loads its backend dynamically; DT_NEEDED alone cannot find it.
    extras = [('/usr/lib/aarch64-linux-gnu/gbm/dri_gbm.so', DEPS/'usr/lib/aarch64-linux-gnu/gbm/dri_gbm.so'),
              ('/usr/sbin/seatd', DEPS/'usr/sbin/seatd'),
              ('/usr/bin/fc-cache', DEPS/'usr/bin/fc-cache'),
              ('/usr/bin/udevadm', SYSROOT/'usr/bin/udevadm'),
              ('/usr/lib/systemd/systemd-udevd', SYSROOT/'usr/lib/systemd/systemd-udevd'),
              ('/usr/lib/aarch64-linux-gnu/libvulkan_panfrost.so', PROJECT/'mesa/build-t70pro-panvk/src/panfrost/vulkan/libvulkan_panfrost.so')]
    pending = collections.deque(roots + extras)
    runtime = {}
    while pending:
        name, source = pending.popleft()
        if name in runtime:
            if runtime[name]['sha256'] != sha(source.read_bytes()):
                raise ValueError('Conflicting desktop runtime identity: ' + name)
            continue
        info = elf_info(source)
        runtime[name] = {'source': source.resolve().relative_to(SYSROOT).as_posix(), **info}
        for needed in info['needed']:
            if needed in built:
                pending.append(built[needed])
                continue
            directories = []
            for value in info['rpaths']:
                for directory in value.split(':'):
                    directory = directory.replace('${ORIGIN}', str(PurePosixPath(name).parent)).replace('$ORIGIN', str(PurePosixPath(name).parent))
                    if directory:
                        if not directory.startswith('/'):
                            raise ValueError('Relative ELF search path')
                        directories.append(directory)
            directories += ['/usr/lib/aarch64-linux-gnu', '/lib/aarch64-linux-gnu']
            candidate = next(((directory+'/'+needed, root/directory[1:]/needed)
                              for directory in directories for root in [DEPS, SYSROOT] if (root/directory[1:]/needed).is_file()), None)
            if candidate is None:
                raise ValueError('Missing desktop runtime library: ' + needed)
            pending.append(candidate)
        if info['interpreter']:
            pending.append((info['interpreter'], SYSROOT/info['interpreter'][1:]))
    return artifacts, runtime


def packaging_sources():
    return [Path(__file__), PKG/'Linux/desktop-start.sh', PKG/'Linux/desktop.rules', PKG/'Linux/weston.ini']


def data_inputs():
    # Official package assets only: no builder accounts, local udev rules or settings.
    result = {}
    def add(name, path):
        if not path.is_file():
            raise ValueError('Missing desktop data input: ' + str(path))
        result[name] = path
    for name in ['60-input-id.rules', '60-evdev.rules', '60-persistent-input.rules',
                 '60-drm.rules', '70-mouse.rules', '70-touchpad.rules']:
        add('usr/lib/udev/rules.d/'+name, SYSROOT/'usr/lib/udev/rules.d'/name)
    add('usr/lib/udev/hwdb.bin', SYSROOT/'usr/lib/udev/hwdb.bin')
    # aliases.conf includes both the control and PCM definitions lazily.
    for name in ['alsa.conf', 'cards/aliases.conf', 'ctl/default.conf',
                 'pcm/default.conf', 'pcm/dmix.conf', 'pcm/dsnoop.conf', 'pcm/hdmi.conf']:
        add('usr/share/alsa/'+name, DEPS/'usr/share/alsa'/name)
    for prefix, directory in [('usr/share/X11/xkb', SYSROOT/'usr/share/X11/xkb'),
                              ('usr/share/libinput', DEPS/'usr/share/libinput'),
                              ('usr/share/icons/DMZ-White', DEPS/'usr/share/icons/DMZ-White')]:
        if not directory.is_dir():
            raise ValueError('Missing desktop input data directory: ' + str(directory))
        for path in sorted(directory.rglob('*')):
            if path.is_file():
                add(prefix+'/'+str(path.relative_to(directory)), path)
    add('usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc', DEPS/'usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc')
    for name in ['terminal.png', 'icon_window.png', 'border.png', 'sign_close.png',
                 'sign_maximize.png', 'sign_minimize.png']:
        add('usr/share/weston/'+name, SOURCE/'data'/name)
    add('usr/share/licenses/teclast-desktop/weston', SOURCE/'COPYING')
    add('usr/share/licenses/teclast-desktop/weston-data', SOURCE/'data/COPYING')
    packages = json.loads((DEPS/'packages.json').read_text())
    for package in sorted(set(packages['packages']) | set(packages['reused_builder_packages']) |
                          {'udev', 'libudev1', 'xkb-data', 'libc6', 'libllvm21'}):
        source = next((root/'usr/share/doc'/package/'copyright' for root in [DEPS, SYSROOT]
                       if (root/'usr/share/doc'/package/'copyright').is_file()), None)
        if source:
            add('usr/share/licenses/teclast-desktop/'+package, source)
    return result


def prepare_cross():
    """Package only identities qualified by the real desktop build."""
    proof = json.loads(REPORT.read_text())
    if proof.get('status') != 'passed' or proof.get('Linux_runtime_executed') is not False:
        raise ValueError('Missing desktop cross-build proof')
    for key, root in [('Weston_source_sha256', SOURCE), ('generated_input_sha256', BUILD),
                      ('build_configuration_sha256', PROJECT)]:
        for name, digest in proof[key].items():
            if sha((root/name).read_bytes()) != digest:
                raise ValueError('Desktop build input changed: ' + name)
    for name, digest in proof['packaging_source_sha256'].items():
        if sha((PROJECT/name).read_bytes()) != digest:
            raise ValueError('Desktop packaging source changed: ' + name)
    if sha((DEPS/'packages.json').read_bytes()) != proof['package_manifest_sha256']:
        raise ValueError('Desktop package manifest changed')
    for item in proof['artifacts']:
        data = (PROJECT/item['path']).read_bytes()
        if len(data) != item['bytes'] or sha(data) != item['sha256']:
            raise ValueError('Installed Weston binary changed')
    entries = {}
    def add(name, data, mode=0o100644):
        relative_path(name)
        if name in entries and entries[name] != (mode, data):
            raise ValueError('Conflicting desktop member: ' + name)
        entries[name] = (mode, data)
    for name, item in proof['runtime'].items():
        # Relative to the builder root so qualification and packing can use
        # the same files from the macOS mount or inside the Linux builder.
        path = SYSROOT / relative_path(item['source'])
        data = path.read_bytes()
        if len(data) != item['bytes'] or sha(data) != item['sha256']:
            raise ValueError('Desktop runtime changed: ' + name)
        # Share the console's existing loader/library paths instead of duplicating them.
        name = name.lstrip('/')
        if name.startswith('usr/lib/aarch64-linux-gnu/') and '/' not in name[len('usr/lib/aarch64-linux-gnu/'):]:
            name = name[4:]
        add(name, data, 0o100755)
    inputs = data_inputs()
    if proof['data_input_sha256'] != {name: sha(path.read_bytes()) for name, path in inputs.items()}:
        raise ValueError('Desktop input/font/cursor/rule data changed')
    for name, path in inputs.items():
        if name.startswith('usr/share/icons/DMZ-White/') and path.is_symlink():
            target = os.readlink(path)
            if '/' in target or target in ['', '.', '..']:
                raise ValueError('Cursor alias escapes its packaged directory')
            add(name, target.encode(), 0o120777)
        else:
            add(name, path.read_bytes())
    add('sbin/teclast-desktop', (PKG/'Linux/desktop-start.sh').read_bytes(), 0o100755)
    add('usr/lib/udev/rules.d/40-teclast-devnodes.rules', (PKG/'Linux/desktop.rules').read_bytes())
    add('etc/xdg/weston/weston.ini', (PKG/'Linux/weston.ini').read_bytes())
    icd = subprocess.check_output([sys.executable, str(PROJECT/'mesa/src/vulkan/util/vk_icd_gen.py'),
        '--api-version', '1.4', '--xml', str(PROJECT/'mesa/src/vulkan/registry/vk.xml'),
        '--sizeof-pointer', '8', '--icd-lib-path', '/lib/aarch64-linux-gnu',
        '--icd-filename', '/lib/aarch64-linux-gnu/libvulkan_panfrost.so'])
    add('usr/share/vulkan/icd.d/teclast-panvk.json', icd)
    add('etc/fonts/fonts.conf', b'<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">\n'
        b'<fontconfig><dir>/usr/share/fonts</dir><cachedir>/run/teclast-desktop/font-cache</cachedir>'
        b'<alias><family>sans-serif</family><prefer><family>Noto Sans CJK KR</family></prefer></alias>'
        b'<alias><family>monospace</family><prefer><family>Noto Sans Mono CJK KR</family></prefer></alias></fontconfig>\n')
    add('etc/passwd', b'root:x:0:0:root:/run/teclast-desktop/home:/bin/sh\n')
    add('etc/group', b'root:x:0:\ntty:x:5:\n')
    records = [{'path': n, 'mode': oct(m), 'bytes': len(d), 'sha256': sha(d)} for n, (m, d) in sorted(entries.items())]
    return [(n, m, d) for n, (m, d) in sorted(entries.items())], {
        'status': 'cross-built runtime packaged; Linux and tablet execution pending',
        'Weston_version': '16.0.2', 'build_report_sha256': sha(REPORT.read_bytes()),
        'package_files': records, 'runtime_ELF_count': proof['runtime_ELF_count'],
        'runtime_source_bytes': proof['runtime_bytes'],
        'activation': 'explicit desktop-vulkan or desktop-pixman console profile; no automatic renderer fallback',
        'device_manager': 'PID1 foreground udev; synchronous mdev PROGRAM before udev metadata/broadcast; bounded coldplug',
        'session': 'libseat builtin, SEATD_VTBOUND=0, private tmpfs runtime, socket and process lifecycle tracked',
        'Hangul_input_method_integrated': False, 'Linux_runtime_executed': False, 'device_execution': False,
        'limitations': ['No Linux compositor or device execution', 'No Hangul input method yet',
                       'Physical GPU, display, USB/input and suspend/wake remain untested']}


def qualify():
    self_test()
    stage = BUILD/'stage'
    subprocess.run(['/opt/homebrew/bin/meson', 'install', '-C', str(BUILD), '--no-rebuild', '--tags=runtime', '--destdir='+str(stage)], check=True)
    artifacts, runtime = runtime_catalog()
    shaders = []
    for path in sorted((BUILD/'libweston/renderer-vulkan').glob('*.spv.h')):
        words = [int(x, 16) for x in re.findall(r'0x([0-9a-f]{8})', path.read_text())]
        if not words or words[0] != 0x07230203:
            raise ValueError('Missing actual compiled Vulkan shader')
        shaders.append({'path': str(path.relative_to(PROJECT)), 'sha256': sha(path.read_bytes()), 'SPIRV_words': len(words)})
    if len(shaders) != 3:
        raise ValueError('Expected three real Weston Vulkan shaders')
    packages = json.loads((DEPS/'packages.json').read_text())
    font = DEPS/'usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'
    if font.read_bytes()[:4] != b'ttcf':
        raise ValueError('Missing CJK font collection')
    report = {'at': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'status': 'passed',
              'scope': 'Actual Weston 16.0.2 ARM64 Linux cross-build, installed ELF checks and complete static runtime-library closure; no Linux or device execution',
              'source': json.loads((SOURCE/'source.json').read_text()),
              'desktop_script_sha256': sha(Path(__file__).read_bytes()),
              'packaging_source_sha256': {str(p.relative_to(PROJECT)): sha(p.read_bytes()) for p in packaging_sources()},
              'data_input_sha256': {name: sha(p.read_bytes()) for name, p in data_inputs().items()},
              'self_checks_executed': True, 'self_check_scope': 'Debian version/relation/archive boundaries and actual valid/invalid GLSL compilation',
              'Weston_source_sha256': {str(p.relative_to(SOURCE)): sha(p.read_bytes()) for p in sorted(SOURCE.rglob('*')) if p.is_file()},
              'generated_input_sha256': {str(p.relative_to(BUILD)): sha(p.read_bytes()) for p in sorted(BUILD.rglob('*')) if p.is_file() and
                                          p.suffix in ['.h', '.c'] and not p.is_relative_to(stage)},
              'artifacts': artifacts, 'runtime': runtime, 'runtime_ELF_count': len(runtime),
              'runtime_bytes': sum(x['bytes'] for x in runtime.values()),
              'package_manifest': str((DEPS/'packages.json').relative_to(PROJECT)), 'package_manifest_sha256': sha((DEPS/'packages.json').read_bytes()),
              'dependency_packages': len(packages['packages']), 'existing_builder_packages_reused': len(packages['reused_builder_packages']),
              'Vulkan_shaders': shaders, 'shader_compiler': subprocess.check_output(['/opt/homebrew/bin/glslc', '--version'], text=True).strip(),
              'shader_check_scope': 'Actual glslc compilation and SPIR-V structure checks; no GPU execution or independent full SPIR-V validation',
              'CJK_font': {'source': str(font.relative_to(PROJECT)), 'bytes': font.stat().st_size, 'sha256': sha(font.read_bytes())},
              'features_compiled': ['DRM/KMS', 'Vulkan renderer', 'Pixman renderer', 'headless and nested Wayland backends', 'libinput and libseat',
                                    'desktop shell', 'Cairo/Pango text', 'terminal', 'on-screen keyboard', 'LCMS colour management'],
              'build_configuration_sha256': {str(p.relative_to(PROJECT)): sha(p.read_bytes()) for p in
                                              [BUILD/'config.h', BUILD.parent/'linux-desktop-cross.ini', BUILD.parent/'linux-desktop-native.ini']},
              'staging_reason': 'Meson runtime install fixes ELF runtime search paths for Linux; builder source trees and existing shared libraries are referenced without copies',
              'Linux_runtime_executed': False, 'desktop_boot_startup_integrated': False, 'initrd_rebuilt': False,
              'Hangul_input_method_integrated': False, 'device_access': False, 'whole_port_complete': False, 'installation_conditions_met': False,
              'remaining': ['udev/libseat startup and PID1 desktop supervision', 'runtime payload and LinuxBoot integration',
                            'Hangul input method',
                            'actual compositor, GPU, input and display execution under separate authorization']}
    REPORT.write_text(json.dumps(report, indent=2)+'\n')
    print('Weston artifacts', len(artifacts), 'Linux runtime ELFs', len(runtime), 'bytes', report['runtime_bytes'])
    return report


def self_test():
    versions = ['1.0~~', '1.0~rc1', '1.0', '1.0-0', '1.0-1', '1.0-1ubuntu1', '1.0-1ubuntu1.2', '1.0-1ubuntu1.10', '1.0+git1', '1.1', '1:0', '2:0']
    for i, a in enumerate(versions):
        assert compare_version(a, a) == 0
        for b in versions[i+1:]:
            assert compare_version(a, b) <= 0 and compare_version(b, a) >= 0
    assert list(paragraphs('Package: sample\nProvides: virtual\nConffiles:\n /etc/file abc\n'))[0]['Provides'] == 'virtual'
    assert relation('libc6:any (>= 2.34)') == ('libc6', '>=', '2.34')
    assert satisfies('1:2.4.0-1ubuntu1', ('x', '>=', '2.5'))
    for bad in ['/etc/escape', '../escape', 'usr/../../escape']:
        try:
            relative_path(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted path escape')
    try:
        deb_data(b'!<arch>\n')
    except ValueError:
        pass
    else:
        raise AssertionError('Accepted empty Debian archive')
    payload = b'archive-data'
    header = b'data.tar.xz/    ' + b'0           ' + b'0     ' + b'0     ' + b'100644  ' + str(len(payload)).ljust(10).encode() + b'`\n'
    archive = b'!<arch>\n' + header + payload
    assert len(header) == 60 and deb_data(archive) == payload
    for invalid in [archive[:-1], b'!<arch>\n' + header.replace(b'`\n', b'xx') + payload,
                    b'!<arch>\n' + header[:48] + b'-1        ' + header[58:] + payload]:
        try:
            deb_data(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted truncated or corrupt ar member')
    with tempfile.TemporaryDirectory(dir=BUILD, prefix='shader-check-') as directory:
        root = Path(directory)
        source, output = root/'sample.vert', root/'sample.h'
        source.write_text('#version 450\nvoid main() { gl_Position=vec4(0.0); }\n')
        args = [str(source), '--quiet', '--variable-name', 'sample', '-V', '-x', '-o', str(output)]
        compile_shader(args)
        assert '0x07230203' in output.read_text() and 'static const uint32_t sample[]' in output.read_text()
        source.write_text('#version 450\nthis is invalid shader source\n')
        try:
            compile_shader(args)
        except subprocess.CalledProcessError:
            pass
        else:
            raise AssertionError('Accepted invalid GLSL')
        try:
            compile_shader([str(source), '--variable-name', 'bad;name', '-V', '-x', '-o', str(output)])
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted invalid shader array name')
    print('Desktop package/parser and actual shader compiler self-checks PASS')


def check_lid_policy():
    """Execute installed ARM64 lid/power/volume policy with explicit OS boundaries."""
    from pathlib import Path
    import json,shlex,subprocess,tempfile,hashlib,struct
    from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_INVALID,UC_QUERY_TIMEOUT
    import unicorn.arm64_const as A
    p=PROJECT;m=p/'Mu-Silicium';src=p/'weston';b=m/'Build/t70proPkg/LinuxDesktop'
    lib=b/'stage/usr/lib/aarch64-linux-gnu/libweston-16.so.0.0.2'
    way=p.parents[2]/'usr/lib/aarch64-linux-gnu/libwayland-server.so.0'
    drm=b/'stage/usr/lib/aarch64-linux-gnu/libweston-16/drm-backend.so'
    commands=json.loads((b/'compile_commands.json').read_text())
    command=next(c for c in commands if c['file'].endswith('/compositor.c'))
    input_source=(src/'libweston/libinput-device.c').read_text()
    power_branch=input_source.index('if (key == KEY_POWER)')
    normal_notify=input_source.index('notify_key(&key_event);')
    assert power_branch < normal_notify
    assert 'weston_compositor_handle_power_key_event(' in input_source[power_branch:normal_notify]
    assert 'return;' in input_source[power_branch:normal_notify]
    volume_branch=input_source.index('if (key == KEY_VOLUMEUP || key == KEY_VOLUMEDOWN)')
    assert volume_branch < normal_notify
    assert 'weston_compositor_handle_volume_key_event(' in input_source[:normal_notify]
    assert 'LIBINPUT_KEY_STATE_REPEATED' not in input_source
    test_source=(src/'tests/lid-power-test.c').read_text()
    policy_cases=[('T70LidPowerAssertions','lid_switch_power_policy')]
    if 'volume_key_policy(' in test_source: policy_cases.append(('T70VolumePolicyAssertions','volume_key_policy'))
    policy_cases.append(('T70VolumeAlsaAssertions','volume_alsa_master_route'))
    policy_cases.append(('T70VolumeT70Assertions','volume_alsa_t70_route'))
    policy_cases.append(('T70VolumeRepeatAssertions','volume_repeat_timer'))
    for _,function in policy_cases: assert function+'(' in test_source
    case_calls='\n'.join('      before=T70LidAssertions; if(!result) result='+function+'(NULL,&compositor); '+counter+'=T70LidAssertions-before;' for counter,function in policy_cases)
    assert 'libasound.so.2' in elf_info(lib)['needed']
    harness=r'''
    #define THIS_TEST_NAME "t70-display-input-offline"
    #define T70_VOLUME_ALSA_TEST 1
    #include <stdarg.h>
    #include "config.h"
    #include <libweston/libweston.h>
    #include "libweston-internal.h"
    #include "weston-test-fixture-compositor.h"
    #include "weston-test-assert.h"
    #include "weston-test-runner.h"
    __attribute__((visibility("default"))) unsigned T70LidAssertions,T70LidTimerCalls,T70LidUnexpectedLog,T70LidTimerLastMs,T70LidTimerFailNext;
    __attribute__((visibility("default"))) unsigned T70LidPowerAssertions,T70VolumePolicyAssertions,T70VolumeAlsaAssertions,T70VolumeT70Assertions,T70VolumeRepeatAssertions;
    #undef test_assert_true
    #undef test_assert_u32_eq
    #undef test_assert_int_eq
    #define TEST_CHECK(x) do { ++T70LidAssertions; if(!(x))return __LINE__; } while(0)
    #define test_assert_true(x) TEST_CHECK(x)
    #define test_assert_u32_eq(a,b) TEST_CHECK((uint32_t)(a)==(uint32_t)(b))
    #define test_assert_int_eq(a,b) TEST_CHECK((int)(a)==(int)(b))
    #include "LID_TEST_SOURCE"
    __attribute__((visibility("default"))) int wl_event_source_timer_update(struct wl_event_source *source, int ms) {
      if(source!=(struct wl_event_source *)0x12345000 || ms<0)return -1;
      ++T70LidTimerCalls;T70LidTimerLastMs=(unsigned)ms;
      if(T70LidTimerFailNext){--T70LidTimerFailNext;return -1;}return 0;
    }
    __attribute__((visibility("default"))) int weston_log(const char *format, ...) { (void)format;++T70LidUnexpectedLog;return 0; }
    __attribute__((visibility("default"))) void *memset(void *v,int c,size_t n) { volatile unsigned char *p=v;while(n--)*p++=(unsigned char)c;return v; }
    __attribute__((visibility("default"))) int strcmp(const char *a,const char *b) {
      while(*a && (unsigned char)*a==(unsigned char)*b){++a;++b;}
      return (unsigned char)*a-(unsigned char)*b;
    }

    /* Bounded libc fixtures for the production control-list snapshot. A
       single allocation is sufficient; reuse is legal only after exact free. */
    static union { uint64_t align; unsigned char bytes[4096 * 128]; } T70LidHeap;
    static unsigned T70LidHeapBusy;
    __attribute__((visibility("default"))) void *malloc(size_t n) {
      if(!n || n>sizeof(T70LidHeap.bytes) || T70LidHeapBusy){++T70LidUnexpectedLog;return NULL;}
      T70LidHeapBusy=1;return T70LidHeap.bytes;
    }
    __attribute__((visibility("default"))) void *calloc(size_t count,size_t size) {
      if(count && size>sizeof(T70LidHeap.bytes)/count){++T70LidUnexpectedLog;return NULL;}
      size_t n=count*size;void *p=malloc(n);if(p)memset(p,0,n);return p;
    }
    __attribute__((visibility("default"))) void free(void *p) {
      if(!p)return;
      if(p!=T70LidHeap.bytes || !T70LidHeapBusy){++T70LidUnexpectedLog;return;}
      T70LidHeapBusy=0;
    }
    __attribute__((visibility("default"))) void *memcpy(void *d,const void *s,size_t n) {
      if(n>sizeof(T70LidHeap.bytes) || ((!d || !s) && n)){++T70LidUnexpectedLog;return d;}
      volatile unsigned char *out=d;const volatile unsigned char *in=s;
      for(size_t i=0;i<n;++i)out[i]=in[i];return d;
    }
    __attribute__((visibility("default"))) size_t strlen(const char *s) {
      if(!s){++T70LidUnexpectedLog;return 256;}
      for(size_t i=0;i<256;++i)if(!s[i])return i;
      ++T70LidUnexpectedLog;return 256;
    }
    /* Explicit libc boundary fixture: only the production hw:%d card name. */
    __attribute__((visibility("default"))) int snprintf(char *dst,size_t n,const char *fmt,...) {
      if(!fmt || fmt[0]!='h' || fmt[1]!='w' || fmt[2]!=':' || fmt[3]!='%' || fmt[4]!='d' || fmt[5]){
        ++T70LidUnexpectedLog;return -1;
      }
      va_list ap;va_start(ap,fmt);int card=va_arg(ap,int);va_end(ap);
      if(card<0){++T70LidUnexpectedLog;return -1;}
      char digits[12],out[16];unsigned count=0,len=0,value=(unsigned)card;
      do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
      out[len++]='h';out[len++]='w';out[len++]=':';
      while(count)out[len++]=digits[--count];
      if(n){size_t copied=len<n-1?len:n-1;for(size_t i=0;i<copied;++i)dst[i]=out[i];dst[copied]=0;}
      return (int)len;
    }
    __attribute__((visibility("default"))) int T70LidMain(void);
    int T70LidMain(void) {
      struct weston_compositor compositor={0};
      wl_list_init(&compositor.output_list);wl_list_init(&compositor.view_list);
      wl_signal_init(&compositor.wake_signal);wl_signal_init(&compositor.sleep_signal);
      compositor.session_active=true;compositor.idle_source=(struct wl_event_source *)0x12345000;
      compositor.idle_time=300;
      int result=0;unsigned before;
      POLICY_CASE_CALLS
      return result ? result : (T70LidUnexpectedLog || T70LidHeapBusy) ? 999999 : 0;
    }
    '''.replace('LID_TEST_SOURCE',str(src/'tests/lid-power-test.c')).replace('POLICY_CASE_CALLS',case_calls)
    sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
    files=[lib,way,drm,Path(__file__),src/'libweston/meson.build',DEPS/'packages.json',DEPS/'usr/include/alsa/asoundlib.h',DEPS/'usr/include/alsa/control.h',b/'config.h',src/'include/libweston/libweston.h',src/'libweston/libweston-internal.h',src/'libweston/libinput-device.h',src/'libweston/libinput-device.c',src/'libweston/backend-drm/drm.c',src/'tests/meson.build',src/'tests/lid-power-test.c',src/'libweston/compositor.c',*[src/'tests/harness'/name for name in ['weston-test-fixture-compositor.h','weston-test-assert.h','weston-test-runner.h']]]
    frozen={str(f):sha(f) for f in files}
    with tempfile.TemporaryDirectory(prefix='weston-lid-') as temporary:
     t=Path(temporary);c=t/'Lid.c';c.write_text(harness)
     args=shlex.split(command['command'])
     for flag in ['-MD','-MQ','-MF','-o','-c']:
      while flag in args:
       i=args.index(flag);del args[i:i+(2 if flag in ['-MQ','-MF','-o','-c'] else 1)]
     args+=['-I'+str(src/'tests/harness'),'-ffunction-sections','-fdata-sections','-fno-builtin','-fno-stack-protector','-o',str(t/'Lid.o'),'-c',str(c)]
     r=subprocess.run(args,cwd=b,capture_output=True,text=True)
     assert not r.returncode,r.stdout+r.stderr
     link=[str(p/'tools/native-macos/ld.lld'),'-pie','--export-dynamic','--gc-sections','--allow-shlib-undefined','-e','T70LidMain','-o',str(t/'Lid.elf'),str(t/'Lid.o'),str(lib),str(way),str(drm)]
     r=subprocess.run(link,capture_output=True,text=True);assert not r.returncode,r.stdout+r.stderr
     u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);images=[];symbols={}
     def get_image(path,base):
      raw=path.read_bytes();assert raw[:6]==bytes.fromhex('7f454c460201')
      assert struct.unpack_from('<HH',raw,16)==(3,183)
      po=struct.unpack_from('<Q',raw,32)[0];pw,pn=struct.unpack_from('<HH',raw,54)
      segs=[struct.unpack_from('<IIQQQQQQ',raw,po+i*pw) for i in range(pn)]
      loads=[s for s in segs if s[0]==1]
      lo=min(s[3]&~4095 for s in loads);hi=max((s[3]+s[6]+4095)&~4095 for s in loads)
      assert hi-lo<0x1000000
      u.mem_map(base+lo,hi-lo)
      for s in loads:
       assert s[2]+s[5]<=len(raw)
       if s[5]:u.mem_write(base+s[3],raw[s[2]:s[2]+s[5]])
      so=struct.unpack_from('<Q',raw,40)[0];sw,sn,_=struct.unpack_from('<HHH',raw,58)
      ss=[struct.unpack_from('<IIQQQQIIQQ',raw,so+i*sw) for i in range(sn)]
      def data(s):return raw[s[4]:s[4]+s[5]]
      syms={}
      for i,s in enumerate(ss):
       if s[1]!=11:continue
       strings=data(ss[s[6]]);table=[]
       for off in range(0,s[5],s[9]):
        no,info,other,section,value,size=struct.unpack_from('<IBBHQQ',data(s),off)
        name=strings[no:].split(b'\0',1)[0].decode()
        table.append((name,section,value,info,size))
        if section and info>>4 in (1,2) and name:
         assert info&15!=10,('IFUNC definition',name)
         symbols.setdefault(name,base+value)
       syms[i]=table
      image={'raw':raw,'base':base,'sections':ss,'syms':syms,'path':path,'data':data}
      images.append(image);return image
     main=get_image(t/'Lid.elf',0x1000000)
     get_image(lib,0x40000000);get_image(way,0x50000000);get_image(drm,0x52000000)
     assert 'evdev_volume_repeat_timeout' in symbols
     trap=0x60000000;u.mem_map(trap,0x10000);unresolved={};relocations=0
     for im in images:
      for s in im['sections']:
       if s[1]!=4 or not s[2]&2:continue
       data=im['data'](s)
       for off in range(0,len(data),s[9]):
        address,info,addend=struct.unpack_from('<QQq',data,off);kind=info&0xffffffff;index=info>>32
        if kind==1027:resolved=im['base']+addend
        else:
         assert kind in (257,1025,1026),(kind,hex(address),im['path'].name)
         name,section,value,flags,size=im['syms'][s[6]][index]
         if section:resolved=im['base']+value
         elif name in symbols:resolved=symbols[name]
         elif flags>>4==2:resolved=0
         else:
          if name not in unresolved:unresolved[name]=trap+16*len(unresolved)
          resolved=unresolved[name]
         resolved+=addend
        u.mem_write(im['base']+address,struct.pack('<Q',resolved))
        relocations+=1
     stack=0x71000000;stop=0x72000000
     u.mem_map(stack,0x200000);u.mem_map(stop,4096)
     executed_instructions=0
     def code(uc,pc,size,_):
      nonlocal executed_instructions
      executed_instructions+=1
      assert not trap<=pc<trap+0x10000,('Unexpected Linux service',next((n for n,a in unresolved.items() if a==pc),hex(pc)))
     def invalid(uc,access,address,size,value,_):raise AssertionError(('unmapped',hex(uc.reg_read(A.UC_ARM64_REG_PC)),hex(address),size))
     u.hook_add(UC_HOOK_CODE,code);u.hook_add(UC_HOOK_MEM_INVALID,invalid)
     u.reg_write(A.UC_ARM64_REG_SP,stack+0x200000-16);u.reg_write(A.UC_ARM64_REG_LR,stop)
     # Bound the five explicit cases by their own original one-million
     # instruction allowance. The added T70 route negatives exceeded the old
     # aggregate allowance while progressing through its control-list fixture.
     instruction_limit=1000000*len(policy_cases)
     u.emu_start(symbols['T70LidMain'],stop,count=instruction_limit,timeout=3000000*len(policy_cases))
     assert u.reg_read(A.UC_ARM64_REG_PC)==stop,('bounded stop',hex(u.reg_read(A.UC_ARM64_REG_PC)),u.query(UC_QUERY_TIMEOUT),{n:struct.unpack('<I',u.mem_read(symbols[n],4))[0] for n in ['T70LidAssertions','T70LidPowerAssertions','T70VolumePolicyAssertions','T70VolumeAlsaAssertions','T70VolumeT70Assertions','T70VolumeRepeatAssertions']},sorted([(hex(u.reg_read(A.UC_ARM64_REG_PC)-v),n) for n,v in symbols.items() if 0<=u.reg_read(A.UC_ARM64_REG_PC)-v<0x2000])[:5])
     result=u.reg_read(A.UC_ARM64_REG_X0);assert result==0,('lid policy source test line',result)
     counts={n:struct.unpack('<I',u.mem_read(symbols[n],4))[0] for n in ['T70LidAssertions','T70LidTimerCalls','T70LidUnexpectedLog','T70LidPowerAssertions','T70VolumePolicyAssertions','T70VolumeAlsaAssertions','T70VolumeT70Assertions','T70VolumeRepeatAssertions']}
     assert counts['T70LidPowerAssertions']>=90 and counts['T70VolumeAlsaAssertions']>0 and counts['T70VolumeT70Assertions']>0 and counts['T70VolumeRepeatAssertions']>0 and counts['T70LidTimerCalls']>0 and not counts['T70LidUnexpectedLog']
     assert frozen=={str(f):sha(f) for f in files}
     report={'status':'passed','counts':counts,'executed_instructions':executed_instructions,'instruction_limit':instruction_limit,'dynamic_relocations':relocations,'source_sha256':frozen,'harness_elf_sha256':sha(t/'Lid.elf'),'harness_elf_bytes':(t/'Lid.elf').stat().st_size,'compiler_command':args,'link_command':link,'harness_source_sha256':sha(c),'inputs_unchanged_during_execution':True,'Linux_OS_executed':False,'libinput_or_DRM_session_executed':False,'physical_lid_or_power_key_executed':False,'power_key_DPMS_policy_tested':True,'power_key_input_route_source_checked':True,'power_key_press_emits_unlock_signal':False,'volume_ALSA_control_policy_tested':True,'T70_exact_card_active_output_volume_policy_tested':True,'libasound_runtime_dependency_verified':True,'ALSA_library_executed':False,'ALSA_control_io_is_fixture':True,'volume_key_input_route_source_checked':True,'volume_repeat_actual_backend_callback_executed':True,'case_functions':[function for _,function in policy_cases],'scope':'actual installed ARM64 libweston, DRM backend timer callback and original libwayland-server code; lid, power-key DPMS and volume tests compiled with production ABI headers. ALSA control calls, bounded card-name, control-list heap/string libc helpers, timer and log services are fixtures. libinput key routing is source checked; no Linux OS, libinput/DRM session, ALSA hardware I/O or physical input/display execution'}
     (PROJECT/'analysis/linux-desktop-lid-tests.json').write_text(json.dumps(report,indent=2)+'\n')
     print('Installed libweston ARM64 lid/power/volume policy:',counts['T70LidAssertions'],'assertions passed; no Linux OS or device execution')
     return report



def main():
    action, *args = sys.argv[1:]
    if action == 'pkg-config':
        return pkg_config(args)
    if action == 'shader':
        compile_shader(args)
        return 0
    if action == 'self-test':
        self_test()
        return 0
    if action == 'plan':
        selected, reused = dependency_plan()
        print(json.dumps({'packages': {n: {'version': p['Version'], 'bytes': int(p['Size'])} for n, p in selected.items()},
                          'download_bytes': sum(int(p['Size']) for p in selected.values()), 'reused_builder_packages': reused}, indent=2))
        return 0
    marker = Path('/Users/you/.tmp/build.lock')
    if not marker.exists() or json.loads(marker.read_text()).get('model') != 'T70Pro':
        raise ValueError('Run desktop mutations under the existing with-build-lock T70Pro wrapper')
    if action == 'deps':
        stage_dependencies()
    elif action == 'build':
        configure_and_build()
        qualify()
    elif action == 'qualify':
        qualify()
    elif action == 'check-lid':
        check_lid_policy()
    else:
        raise ValueError('Expected plan, deps, build, qualify, check-lid, self-test, pkg-config or shader')
    return 0


if __name__ == '__main__':
    sys.exit(main())
