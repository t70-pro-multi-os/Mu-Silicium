#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Stage a verified diagnostic ARM64 link and its source INF, without signing.
This is not InfVerif/Inf2Cat validation, installation, or a working-driver claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys


def sha(data):
    return hashlib.sha256(data).hexdigest()


def need(ok, message):
    if not ok:
        raise ValueError(message)


def verify_pe(data):
    need(len(data) >= 64 and data[:2] == b'MZ', 'missing DOS header')
    pe = struct.unpack_from('<I', data, 60)[0]
    need(pe >= 64 and pe + 24 <= len(data) and data[pe:pe+4] == b'PE\0\0', 'invalid PE header')
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    size, flags = struct.unpack_from('<HH', data, pe + 20)
    op = pe + 24
    need(machine == 0xaa64 and count > 0 and flags & 2, 'not executable ARM64 PE')
    need(size >= 112 and op + size + count * 40 <= len(data), 'truncated PE headers')
    need(struct.unpack_from('<H', data, op)[0] == 0x20b, 'not PE32+')
    need(struct.unpack_from('<H', data, op + 68)[0] == 1, 'not native subsystem')
    directories = struct.unpack_from('<I', data, op + 108)[0]
    need(directories <= (size - 112) // 8, 'truncated PE directories')
    if directories > 4:
        need(data[op+144:op+152] == bytes(8), 'unexpected Authenticode certificate')
    for i in range(count):
        row = op + size + i * 40
        length, offset = struct.unpack_from('<II', data, row + 16)
        need(not length or (offset > 0 and offset + length <= len(data)), 'section outside image')


def verify_inf(data):
    # Deliberately bounded to this project's source INF, not a general INF parser.
    sections = {}
    current = None
    for raw in data.decode('utf-8-sig').splitlines():
        line = raw.split(';', 1)[0].strip()
        if not line:
            continue
        if line.startswith('[') and line.endswith(']'):
            current = line[1:-1].lower()
            need(current not in sections, 'duplicate INF section')
            sections[current] = []
        else:
            need(current is not None, 'INF entry outside section')
            sections[current].append(line.lower())
    def entry(section, key):
        pairs = [line.split('=', 1)[1].strip() for line in sections.get(section, [])
                 if '=' in line and line.split('=', 1)[0].strip() == key]
        need(len(pairs) == 1, 'missing/duplicate INF ' + section + '/' + key)
        return pairs[0]
    need(entry('version', 'catalogfile') == 'mtkaudio.cat', 'unexpected catalog name')
    need(not any(x.split('=', 1)[0].strip().startswith('catalogfile.') for x in sections['version']), 'decorated catalog unsupported')
    need(entry('sourcedisksfiles', 'mtkaudio.sys') == '1' and len(sections['sourcedisksfiles']) == 1, 'unexpected source files')
    need(sections.get('drivercopy') == ['mtkaudio.sys'], 'unexpected driver copy list')
    need(entry('install.nt', 'copyfiles') == 'drivercopy', 'copy section mismatch')
    need(entry('service', 'servicebinary') == '%13%\\mtkaudio.sys', 'service binary mismatch')
    need(entry('manufacturer', '%provider%') == 'models,ntarm64.10.0...26100', 'unexpected architecture/OS target')


def stage(report_path, inf_path, output):
    root = Path(__file__).resolve().parent
    repo = next((p for p in root.parents if (p / '.git').exists()), root)
    output = output.absolute()
    resolved = output.resolve()
    need(resolved != repo and repo not in resolved.parents, 'output must be outside the repository')
    need(not output.exists() and not output.is_symlink(), 'output already exists')
    need(output.parent.is_dir(), 'output parent must already exist')
    report_bytes = report_path.read_bytes()
    report = json.loads(report_bytes)
    need(report.get('audit_completed') is True and report.get('driver_link_succeeded') is True, 'incomplete/failed build report')
    need(report.get('diagnostic_link', {}).get('exit') == 0, 'failed link')
    need(report.get('unresolved_project_symbols') == [], 'unresolved project symbols')
    components = report.get('components')
    need(isinstance(components, list) and components and all(x.get('exit') == 0 for x in components), 'failed/missing components')
    current_sources = {p.name: sha(p.read_bytes()) for p in root.iterdir()
                       if p.suffix in ('.c', '.cpp') and not p.stem.endswith('Test')}
    recorded_sources = {x.get('source'): x.get('source_sha256') for x in components}
    need(len(recorded_sources) == len(components) and recorded_sources == current_sources,
         'stale/incomplete production source hashes')
    current_headers = {p.name: sha(p.read_bytes()) for p in root.glob('*.h')}
    need(report.get('project_headers_sha256') == current_headers, 'stale/incomplete header hashes')
    need(report.get('kmdf_api_compatibility', {}).get('result') == 'pass', 'KMDF compatibility audit missing')
    need(report.get('kmdf_binary_minimum', {}).get('linked_minimum', {}).get('value') == 33, 'KMDF linked minimum not verified')
    need(Path(report.get('driver_image', '')).name == 'MtkAudio.sys', 'unexpected reported driver name')
    # Use the sibling artifact so a copied report cannot select an unrelated path.
    driver = (report_path.parent / 'MtkAudio.sys').read_bytes()
    need(sha(driver) == report.get('driver_sha256'), 'driver hash mismatch')
    verify_pe(driver)
    inf = inf_path.read_bytes()
    verify_inf(inf)
    manifest = {
        'status': 'unsigned_unvalidated_staging_only',
        'files': {'MtkAudio.sys': sha(driver), 'MtkAudio.inf': sha(inf)},
        'build_report_sha256': sha(report_bytes),
        'source_inf': str(inf_path.resolve()),
        'catalog_expected_but_not_generated': 'MtkAudio.cat',
        'inf2cat_command_template': 'Inf2Cat /driver:"<package-directory-on-Windows>" /os:10_GE_ARM64',
        'limitations': ['No CAT/signature generated; no InfVerif or Inf2Cat run.',
                        'No installation, OS load, endpoint or playback validation.',
                        'StartDevice physical readiness remains unavailable.'],
    }
    # All input validation precedes creation. Exclusive mkdir refuses collisions.
    output.mkdir()
    try:
        (output / 'MtkAudio.sys').write_bytes(driver)
        (output / 'MtkAudio.inf').write_bytes(inf)
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    except BaseException:
        shutil.rmtree(output)
        raise
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--inf', type=Path, default=Path(__file__).with_name('MtkAudio.inf'))
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    try:
        result = stage(a.report, a.inf, a.output)
    except (OSError, ValueError, TypeError, AttributeError, struct.error) as error:
        print(json.dumps({'result': 'fail', 'error': str(error)}))
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
