#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Extract battery ID 0 OCV tables from the locked T70 kernel source."""
from pathlib import Path
import hashlib, re, sys

EXPECTED_SHA256 = '5630de3582929dc5a8ac82be6ce36f07855359de6077a04da3874e7085d40489'
EXPECTED_COMMIT = 'a71515dbaf0d539ffb36416f14186dad17eef743'


def table_body(source: str, name: str) -> str:
    match = re.search(rf'fg_profile_{name}\[TOTAL_BATTERY_NUMBER\]\[100\]\s*=\s*\{{', source)
    if not match:
        raise ValueError(f'missing stock profile array {name}')
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if depth == 0:
                return source[start + 1:pos]
    raise ValueError(f'unclosed stock profile array {name}')


def battery_rows(body: str, name: str):
    blocks = []
    depth = 0
    start = None
    for pos, char in enumerate(body):
        if char == '{':
            if depth == 0:
                start = pos
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0 and start is not None:
                blocks.append(body[start + 1:pos])
                start = None
    if len(blocks) != 4:
        raise ValueError(f'{name}: expected four battery profiles, got {len(blocks)}')
    rows = [tuple(map(int, values)) for values in re.findall(
        r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}', blocks[0])]
    if len(rows) != 100:
        raise ValueError(f'{name}: expected 100 stock points, got {len(rows)}')
    return rows


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: generate-profile.py <mtk_battery_table.h> <output.h>')
    source_path, output_path = map(Path, sys.argv[1:])
    source_bytes = source_path.read_bytes()
    digest = hashlib.sha256(source_bytes).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f'kernel battery table source changed: {digest}')
    source = source_bytes.decode('utf-8')
    temp_match = re.search(r'int g_temperature\[MAX_TABLE\]\s*=\s*\{([^}]+)\}', source)
    if not temp_match:
        raise SystemExit('missing g_temperature source array')
    temp_text = re.sub(r'/\*.*?\*/', '', temp_match.group(1), flags=re.S)
    temperatures = [int(v) for v in re.findall(r'-?\d+', temp_text)]
    if temperatures != [50, 25, 10, 0, -10, -25, -30, -35, -40, -45]:
        raise SystemExit(f'unexpected stock temperatures: {temperatures}')
    tables = [battery_rows(table_body(source, f't{i}'), f't{i}') for i in range(5)]
    for index, table in enumerate(tables):
        if any(table[i][0] > table[i + 1][0] for i in range(99)) or table[0][1] <= table[-1][1]:
            raise SystemExit(f'stock profile t{index} is not monotonic')
    lines = [
        '/* SPDX-License-Identifier: GPL-2.0-only */',
        '/* Copyright (C) 2019 MediaTek Inc. Stock fuel-gauge profile. */',
        '/* Generated from T70 kernel-reference commit ' + EXPECTED_COMMIT + '. */',
        '#ifndef T70_MTK_BATTERY_STOCK_PROFILE_H',
        '#define T70_MTK_BATTERY_STOCK_PROFILE_H',
        '#include "BatteryCore.h"',
        f'#define T70_BATTERY_PROFILE_SOURCE_SHA256 "{digest}"',
        'static const int32_t T70BatteryProfileTemperatureC[5] = {',
        '  ' + ', '.join(map(str, temperatures[:5])),
        '};',
        'static const MTK_BATTERY_PROFILE_POINT T70BatteryStockProfile[5][100] = {'
    ]
    for table in tables:
        lines.append('  {')
        lines.extend(f'    {{{mah}, {mv10}, {milliohms}}},' for mah, mv10, milliohms in table)
        lines.append('  },')
    lines.extend(['};', '#endif'])
    output_path.write_text('\n'.join(lines) + '\n')
    print(f'generated {output_path}: 500 profile points, source sha256={digest}')


if __name__ == '__main__':
    main()
