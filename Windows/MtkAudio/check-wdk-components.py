#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compile audio components with installed official headers; audit a diagnostic link.
No downloads, mocks, entry-point stubs, hardware access or loadable-driver claim.
Requires the previously prepared case-insensitive official-header VFS overlay.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ENV = {k: v for k, v in os.environ.items()
       if k.upper() not in {"INCLUDE", "CL", "_CL_", "CPATH", "C_INCLUDE_PATH", "LIB", "LINK", "_LINK_"}}


def run(argv, log):
    result = subprocess.run([str(x) for x in argv], capture_output=True, text=True, env=ENV)
    log.write_text(result.stdout + result.stderr)
    return result


def symbols(path, undefined=False):
    args = ['llvm-nm', '--format=posix', '--extern-only',
            '--undefined-only' if undefined else '--defined-only', str(path)]
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    # Archive member headings have no symbol type column.
    return {p[0] for line in result.stdout.splitlines()
            if len(p := line.split()) >= 2 and len(p[1]) == 1}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    a = parser.parse_args()
    root = Path(__file__).resolve().parent
    cache, out = a.cache.resolve(), a.output.resolve()
    if out == root or root in out.parents:
        parser.error('output must be outside the source directory')
    out.mkdir(parents=True, exist_ok=False)
    wdk = cache/'microsoft.windows.wdk.arm64/c'
    sdk = cache/'microsoft.windows.sdk.cpp/c'
    version = '10.0.28000.0'
    includes = [wdk/'Include'/version/x for x in ('km', 'km/crt', 'shared')]
    includes += [sdk/'Include'/version/x for x in ('shared', 'ucrt')]
    includes += [wdk/'Include/wdf/kmdf/1.35']
    overlay = cache/'case-overlay.json'
    for path in includes + [overlay]:
        if not path.exists():
            parser.error('missing official input: ' + str(path))
    def validate_overlay(node):
        if isinstance(node, dict):
            if 'external-contents' in node:
                target = Path(node['external-contents']).resolve()
                if not any(base.resolve() in target.parents for base in includes):
                    parser.error('overlay points outside official includes: ' + str(target))
            for value in node.values():
                validate_overlay(value)
        elif isinstance(node, list):
            for value in node:
                validate_overlay(value)
    validate_overlay(json.loads(overlay.read_text()))
    flags = ['clang-cl' , '--target=aarch64-pc-windows-msvc', '/TC', '/c', '/kernel',
             '/D_ARM64_', '/D_KERNEL_MODE', '/DNTDDI_VERSION=0x0A000010',
             '/D_WIN32_WINNT=0x0A00', '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=35',
             '/DKMDF_MINIMUM_VERSION_REQUIRED=33',
             '/clang:-ivfsoverlay', '/clang:' + str(overlay), '/showIncludes']
    flags += ['/I' + str(x) for x in includes]
    report = {'components': [], 'compiler': subprocess.check_output(
        ['clang-cl', '--version'], text=True), 'overlay_sha256': hashlib.sha256(overlay.read_bytes()).hexdigest()}
    report['tools'] = {tool: subprocess.check_output([tool, '--version'], text=True) for tool in ('lld-link', 'llvm-nm')}
    report['kmdf_target_minor'] = 35
    report['kmdf_minimum_required'] = 33
    report['kmdf_api_compatibility'] = json.loads(subprocess.check_output(
        [sys.executable, str(root/'check-kmdf-compat.py'), '--cache', str(cache)], text=True))
    report['project_headers_sha256'] = {x.name: hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(root.glob('*.h'))}
    objects = []
    for source in sorted([*root.glob('*.c'), *root.glob('*.cpp')]):
        if source.stem.endswith('Test'):
            continue
        obj = out/(source.stem+'.obj')
        language_flags = [('/TP' if source.suffix == '.cpp' else '/TC') if x == '/TC' else x for x in flags]
        if source.suffix == '.cpp':
            language_flags += ['/GR-', '/EHs-c-']
        cmd = language_flags + ['/Fo'+str(obj), str(source)]
        result = run(cmd, out/(source.stem+'.log'))
        entry = {'source': source.name, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                 'command': cmd, 'exit': result.returncode}
        report['components'].append(entry)
        if result.returncode or 'tests/include' in (result.stdout + result.stderr).replace('\\', '/').lower():
            (out/'report.json').write_text(json.dumps(report, indent=2))
            raise SystemExit('compile failed or mock contamination: ' + source.name)
        if obj.read_bytes()[:2] != b'\x64\xaa':
            raise SystemExit('not ARM64 COFF: ' + str(obj))
        objects.append(obj)
    defined = set().union(*(symbols(x) for x in objects))
    undefined = set().union(*(symbols(x, True) for x in objects)) - defined
    report['external_symbols'] = sorted(undefined)
    report['unresolved_project_symbols'] = sorted(x for x in undefined if x.removeprefix('__imp_').startswith('T70'))
    libs = [wdk/'Lib'/version/'km/ARM64'/x for x in
            ('ntoskrnl.lib', 'hal.lib', 'volatileaccessk.lib', 'bufferoverflowfastfailk.lib', 'portcls.lib')]
    libs += [wdk/'Lib/wdf/kmdf/ARM64/1.35'/x for x in ('wdfdriverentry.lib', 'wdfldr.lib')]
    report['libraries_sha256'] = {str(x): hashlib.sha256(x.read_bytes()).hexdigest() for x in libs}
    report['library_providers'] = {str(x): sorted(undefined & symbols(x)) for x in libs}
    # No fake DriverEntry, /force:unresolved or dead-code removal hiding missing consumers.
    link = ['lld-link', '/machine:arm64', '/driver', '/subsystem:native',
            '/entry:FxDriverEntry', '/map:'+str(out/'MtkAudio.map'), '/nodefaultlib', '/opt:noref', '/out:'+str(out/'MtkAudio.sys')]
    link += [str(x) for x in objects + libs]
    result = run(link, out/'link.log')
    report['audit_completed'] = True
    report['driver_link_succeeded'] = result.returncode == 0
    if result.returncode == 0:
        report['driver_image'] = str(out/'MtkAudio.sys')
        report['driver_sha256'] = hashlib.sha256((out/'MtkAudio.sys').read_bytes()).hexdigest()
        report['kmdf_binary_minimum'] = json.loads(subprocess.check_output(
            [sys.executable, str(root/'check-kmdf-minimum.py'), str(out),
             '--pe', str(out/'MtkAudio.sys'), '--map', str(out/'MtkAudio.map'),
             '--expected', '33'], text=True))
    report['diagnostic_link'] = {'command': link, 'exit': result.returncode,
                                'diagnostics': result.stdout + result.stderr}
    (out/'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'audit_completed': True, 'driver_link_succeeded': result.returncode == 0, 'compiled': len(objects), 'external_symbols': sorted(undefined),
                      'unresolved_project_symbols': report['unresolved_project_symbols'],
                      'diagnostic_link_exit': result.returncode}, indent=2))
    if report['unresolved_project_symbols']:
        raise SystemExit('unresolved project references')
    # Link failure is recorded evidence, not represented as driver success.


if __name__ == '__main__':
    main()
