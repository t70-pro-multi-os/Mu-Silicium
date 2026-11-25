#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the T70 stock battery profile from its locked source and run the SOC core."""
from pathlib import Path
import argparse, hashlib, json, os, subprocess, tempfile

EXPECTED_COMMIT = 'a71515dbaf0d539ffb36416f14186dad17eef743'
EXPECTED_TABLE_SHA256 = '5630de3582929dc5a8ac82be6ce36f07855359de6077a04da3874e7085d40489'
SOURCE_PATH = 'mindone/modules/mt6358_battery/mtk_battery_table.h'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    battery = Path(__file__).resolve().parent
    root = battery.parents[1]
    kernel = root.parent / 'kernel-reference'
    commit = subprocess.check_output(
        ['git', '-C', str(kernel), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != EXPECTED_COMMIT:
        raise SystemExit(f'kernel source commit changed: {commit}')
    source = subprocess.check_output(
        ['git', '-C', str(kernel), 'show', f'HEAD:{SOURCE_PATH}'])
    digest = hashlib.sha256(source).hexdigest()
    if digest != EXPECTED_TABLE_SHA256:
        raise SystemExit(f'kernel battery profile changed: {digest}')

    with tempfile.TemporaryDirectory(prefix='mtk-battery-core-') as directory:
        temp = Path(directory)
        source_file = temp / 'mtk_battery_table.h'
        generated = temp / 'StockProfile.h'
        source_file.write_bytes(source)
        subprocess.run([
            'python3', str(battery / 'generate-profile.py'),
            str(source_file), str(generated)], check=True, capture_output=True, text=True)
        checked_in = (battery / 'StockProfile.h').read_bytes()
        if generated.read_bytes() != checked_in:
            raise SystemExit('checked-in stock profile does not match the locked kernel source')

        compiler = os.environ.get('CC', 'gcc')
        executable = temp / 'MtkBatteryCoreTest'
        subprocess.run([
            compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-I', str(battery), str(battery / 'BatteryCore.c'),
            str(battery / 'BatteryCoreTest.c'), '-o', str(executable)
        ], check=True)
        output = subprocess.check_output([str(executable)], text=True).strip()

    report = {
        'status': 'passed',
        'host_test': output,
        'kernel_reference': {
            'commit': commit,
            'path': SOURCE_PATH,
            'sha256': digest,
            'battery_id': 0,
            'profile_tables_in_port': 5,
            'points_per_table': 100,
            'kernel_active_table_config': 'not present in the checked-out source; port includes T0-T4',
        },
        'scope': 'production SOC profile interpolation, OCV initialization, stock 0.1 mA current conversion boundary, software coulomb integration, sleep re-anchor and status flags under ASan/UBSan; PMIC and Windows class APIs are not exercised',
    }
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(output)
    print(f'kernel profile parity passed: {commit} {digest}')


if __name__ == '__main__':
    main()
