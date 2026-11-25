#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Static comparison of the audio project's used KMDF 1.33/1.35 contracts.
Reads installed official headers only. Does not compile 1.33 headers, prove
runtime compatibility, download packages, or copy vendor implementations.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


APIS = {
    'WdfIoTargetCreate', 'WdfIoTargetOpen', 'WdfIoTargetClose',
    'WdfIoTargetSendWriteSynchronously', 'WdfIoTargetSendIoctlSynchronously',
    'WdfObjectDelete', 'WdfDriverCreate', 'WdfDriverMiniportUnload', 'WdfDeviceMiniportCreate',
}
STRUCTS = {
    'WDF_DRIVER_CONFIG': 'wdfdriver.h',
    'WDF_IO_TARGET_OPEN_PARAMS': 'wdfiotarget.h',
    'WDF_REQUEST_SEND_OPTIONS': 'wdfrequest.h',
    'WDF_MEMORY_DESCRIPTOR': 'wdfmemory.h',
}
HELPERS = {
    'WdfGetDriver': 'wdfdriver.h',
    'WDF_DRIVER_CONFIG_INIT': 'wdfdriver.h',
    'WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME': 'wdfiotarget.h',
    'WDF_REQUEST_SEND_OPTIONS_INIT': 'wdfrequest.h',
    'WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT': 'wdfrequest.h',
    'WDF_MEMORY_DESCRIPTOR_INIT_BUFFER': 'wdfmemory.h',
    'WDF_REL_TIMEOUT_IN_MS': 'wdfcore.h',
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def uncomment(text):
    # Preserve strings/characters, including comment-looking text inside them.
    return re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                  lambda m: ' ' if m[0].startswith(('/',)) else m[0], text, flags=re.S)


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def block(text, start, left, right):
    require(start < len(text) and text[start] == left, 'missing opening delimiter')
    depth = 0
    for i in range(start, len(text)):
        if text[i] == left:
            depth += 1
        elif text[i] == right:
            depth -= 1
            if not depth:
                return i + 1
    raise ValueError('unbalanced official header declaration')


def declaration(text, name, structure):
    text = uncomment(text)
    pattern = (r'\btypedef\s+struct\s+_' + re.escape(name) + r'\s*\{'
               if structure else r'\b' + re.escape(name) + r'\s*\(')
    matches = list(re.finditer(pattern, text))
    require(len(matches) == 1, 'expected one declaration: ' + name)
    start = matches[0].start()
    pos = matches[0].end() - 1
    if not structure:
        pos = block(text, pos, '(', ')')
        while pos < len(text) and text[pos].isspace():
            pos += 1
    end = block(text, pos, '{', '}')
    if structure:
        end = text.index(';', end) + 1
    # Compare tokens, ignoring comments/formatting, retaining literal values.
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\w+|[^\s]', text[start:end])
    return '\n'.join(tokens).encode()


def audit(cache):
    root = Path(__file__).resolve().parent
    paths = sorted(p for p in root.iterdir()
                   if p.suffix in ('.c', '.cpp', '.h') and 'Test' not in p.name)
    sources = {p.name: p.read_bytes() for p in paths}
    texts = {name: uncomment(data.decode()) for name, data in sources.items()}
    uses = {}
    for name, text in texts.items():
        # Exclude comments and string literals from executable call discovery.
        code = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', ' ', text)
        calls = sorted(set(re.findall(r'\b(?:Wdf\w+|WDF_[A-Z_0-9]+)\s*(?=\()', code)))
        if calls:
            uses[name] = calls
    found = {call for calls in uses.values() for call in calls}
    api_calls = {call for call in found if call.startswith('Wdf') and call not in HELPERS}
    helper_calls = found - api_calls
    require(api_calls == APIS, 'WDF API audit set changed: ' + str(sorted(api_calls ^ APIS)))
    require(helper_calls == set(HELPERS), 'WDF helper audit set changed: ' + str(sorted(helper_calls ^ set(HELPERS))))
    headers = cache / 'microsoft.windows.wdk.arm64/c/Include/wdf/kmdf'
    report = {'result': 'pass', 'kind': 'static_official_header_comparison',
              'minimum_minor': 33, 'target_minor': 35,
              'limitations': 'Not a KMDF 1.33 compile or runtime/load test; no hardware verification.',
              'source_calls': uses, 'source_sha256': {n: digest(b) for n, b in sources.items()},
              'header_sha256': {}, 'apis': {}, 'structures': {}, 'helpers': {}}
    contents = {}
    thresholds = {}
    indices = {}
    for minor in (33, 35):
        directory = headers / ('1.' + str(minor))
        contents[minor] = {}
        for filename in sorted({'wdffuncenum.h'} | set(STRUCTS.values()) | set(HELPERS.values())):
            data = (directory / filename).read_bytes()
            report['header_sha256'][f'1.{minor}/{filename}'] = digest(data)
            contents[minor][filename] = data.decode()
        enum = uncomment(contents[minor]['wdffuncenum.h'])
        threshold = re.search(r'^\s*#define\s+WDF_ALWAYS_AVAILABLE_FUNCTION_COUNT\s+(\d+)\b', enum, re.M)
        require(threshold is not None, 'missing always-available threshold')
        thresholds[minor] = int(threshold[1])
        indices[minor] = {n: int(v) for n, v in re.findall(r'\b(Wdf\w+)TableIndex\s*=\s*(\d+)\b', enum)}
    report['always_available_function_count'] = thresholds
    for name in sorted(api_calls):
        values = [indices[m].get(name) for m in (33, 35)]
        require(values[0] is not None and values[0] == values[1], 'missing/different API index: ' + name)
        require(all(values[i] < thresholds[m] for i, m in enumerate((33, 35))), 'API requires availability guard: ' + name)
        report['apis'][name] = {'table_index': values[0], 'always_available': True}
    joined = '\n'.join(texts.values())
    for structure, names, key in ((True, STRUCTS, 'structures'), (False, HELPERS, 'helpers')):
        for name, filename in sorted(names.items()):
            require(re.search(r'\b' + name + r'\b', joined) is not None, 'audited contract no longer used: ' + name)
            bodies = [declaration(contents[m][filename], name, structure) for m in (33, 35)]
            require(bodies[0] == bodies[1], 'contract differs between 1.33/1.35: ' + name)
            report[key][name] = {'identical': True, 'token_sha256': digest(bodies[0])}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = audit(args.cache.resolve())
    except (OSError, ValueError, UnicodeError) as error:
        print(json.dumps({'result': 'fail', 'error': str(error)}, sort_keys=True))
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
