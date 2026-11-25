#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Emit bounded, hash-bound RAM edits. Never write a modified LK image or use USB."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

LK_SHA256 = '05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4'
CAVE, CAVE_END, REGISTER, SOURCE = 0x400, 0x800, 0x8358, 0x5b5e4


def branch(source, destination, link=False):
    delta = destination - source
    if delta & 3 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('AArch64 branch target out of range or unaligned')
    return struct.pack('<I', (0x94000000 if link else 0x14000000) | ((delta >> 2) & 0x3ffffff))


def symbols(path):
    result = {}
    for line in path.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3:
            try:
                result[fields[2]] = int(fields[0], 16)
            except ValueError:
                pass
    return result


def make_plan(lk, payload, names):
    if hashlib.sha256(lk).hexdigest() != LK_SHA256:
        raise ValueError('Shipping LK hash differs; no edits generated')
    if not 0 < len(payload) <= CAVE_END - CAVE or len(payload) % 4:
        raise ValueError('Payload size exceeds the verified alignment gap')
    if names.get('register_ram_boot') != CAVE or names.get('patch_end') != CAVE + len(payload):
        raise ValueError('Payload symbol layout differs')
    for name in ('ram_boot', 'ram_image_source_hook'):
        if not CAVE <= names.get(name, -1) < CAVE + len(payload) or names[name] % 4:
            raise ValueError('Invalid payload entry: ' + name)
    if any(lk[CAVE:CAVE_END]):
        raise ValueError('Shipping alignment gap is not entirely zero')
    if lk[REGISTER:REGISTER+4] != branch(REGISTER, 0x7750, True):
        raise ValueError('Original continue registration call differs')
    if lk[SOURCE:SOURCE+4] != bytes.fromhex('1b0540f9'):
        raise ValueError('Original image pointer load differs')
    edits = []
    for offset, data, purpose in (
        (CAVE, payload, 'RAM-only command and image source replacement'),
        (REGISTER, branch(REGISTER, names['register_ram_boot'], True), 'Preserve continue and register boot'),
        (SOURCE, branch(SOURCE, names['ram_image_source_hook']), 'Select RAM boot before parsing its header'),
    ):
        edits.append({'offset': offset, 'expected_hex': lk[offset:offset+len(data)].hex(),
                      'replacement_hex': data.hex(), 'purpose': purpose})
    return {'schema_version': 1, 'shipping_lk_sha256': LK_SHA256,
            'payload_sha256': hashlib.sha256(payload).hexdigest(), 'payload_bytes': len(payload),
            'link_offset': CAVE, 'symbols': names, 'edits': edits,
            'storage_writes': [], 'applied_to_device': False,
            'requires_actual_unlocked_state': True,
            'requires_loaded_LK_RAM_mapping': True,
            'requires_data_clean_and_instruction_invalidate_before_execution': True,
            'preserves_init_boot_and_vendor_boot_sources': True,
            'UEFI_execution_observed': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('lk', 'payload', 'symbols', 'report'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    plan = make_plan(args.lk.read_bytes(), args.payload.read_bytes(), symbols(args.symbols))
    args.report.write_text(json.dumps(plan, indent=2)+'\n')
    print(json.dumps({'RAM_edit_regions': len(plan['edits']), 'payload_bytes': plan['payload_bytes'],
                      'storage_writes': [], 'applied_to_device': False}))


if __name__ == '__main__':
    main()
