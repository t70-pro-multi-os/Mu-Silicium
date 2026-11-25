#!/usr/bin/env python3
"""Execute the exact shipping LK video-tag updater; no physical device I/O."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC

EXPECTED = '05e0155f32c46109cad92b46e65d4943852b91c84d19c99d2e52d49443cf33b4'
BASE = 0xffff000050f00000
GLOBALS = 0xffff000051080000
END = 0xffff000052000000
STACK = 0x60000000
FDT = 0x61000000
NAMES = ('atag,videolfb-fb_base_h', 'atag,videolfb-fb_base_l', 'atag,videolfb-vramSize')


def execute(code, framebuffer, size, node):
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(BASE, (len(code) + 4095) & ~4095)
    uc.mem_write(BASE, code)
    uc.mem_map(GLOBALS, 0x2000)
    uc.mem_map(END, 4096)
    uc.mem_map(STACK, 0x20000)
    uc.mem_write(GLOBALS + 0x9b4, struct.pack('<II', size, framebuffer))
    uc.reg_write(UC_ARM64_REG_SP, STACK + 0x10000)
    uc.reg_write(UC_ARM64_REG_X0, FDT)
    uc.reg_write(UC_ARM64_REG_X30, END)
    properties = {'atag,videolfb-islcmfound': bytes.fromhex('00000001'),
                  'atag,videolfb-islcm_inited': bytes(4)}
    calls = []
    writes = []

    def cstring(address):
        return bytes(uc.mem_read(address, 64)).split(b'\0', 1)[0].decode('ascii')

    def on_code(machine, address, length, user):
        if address == BASE + 0x5d9f8:
            assert machine.reg_read(UC_ARM64_REG_X0) == FDT
            assert cstring(machine.reg_read(UC_ARM64_REG_X1)) == '/chosen'
            machine.reg_write(UC_ARM64_REG_X0, node & ((1 << 64) - 1))
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_X30))
        elif address == BASE + 0x5e598:
            assert machine.reg_read(UC_ARM64_REG_X0) == FDT
            assert machine.reg_read(UC_ARM64_REG_X1) == node
            name = cstring(machine.reg_read(UC_ARM64_REG_X2))
            assert name in NAMES
            assert machine.reg_read(UC_ARM64_REG_X4) == 4
            value = bytes(machine.mem_read(machine.reg_read(UC_ARM64_REG_X3), 4))
            calls.append(name)
            properties[name] = value
            machine.reg_write(UC_ARM64_REG_X0, 0)
            machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_X30))

    def on_write(machine, access, address, length, value, user):
        assert (STACK <= address and address + length <= STACK + 0x20000) or (address == GLOBALS + 0x9b4 and length == 4)
        writes.append((address, length))

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    uc.emu_start(BASE + 0x5454c, END, count=400)
    assert uc.reg_read(UC_ARM64_REG_PC) == END
    assert properties['atag,videolfb-islcm_inited'] == bytes(4)
    assert properties['atag,videolfb-islcmfound'] == bytes.fromhex('00000001')
    if node >= 0:
        assert tuple(calls) == NAMES
        assert properties[NAMES[0]] == bytes(4)
        assert properties[NAMES[1]] == framebuffer.to_bytes(4, 'big')
        assert properties[NAMES[2]] == size.to_bytes(4, 'big')
    else:
        assert not calls
    return {'chosen_node_found': node >= 0, 'FDT_properties_updated': calls,
            'static_islcm_inited_preserved': True, 'DT_cell_encoding': 'big_endian',
            'only_stack_and_original_vram_global_written': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lk', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    code = args.lk.read_bytes()
    assert hashlib.sha256(code).hexdigest() == EXPECTED, 'unexpected shipping LK'
    cases = [execute(code, base, size, node) for base, size, node in
             ((0x90000000, 0x20000, 7), (0x7e605000, 0x1be0000, 7),
              (0xff000000, 0x100000, 7), (0x90000000, 0x20000, -1))]
    result = {'status': 'pass', 'shipping_LK_sha256': EXPECTED,
              'test_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'actual_ARM64_function_offset': '0x5454c', 'cases': cases,
              'physical_execution': False, 'device_storage_writes': []}
    args.report.write_text(json.dumps(result, indent=2) + '\n')
    print('Shipping LK video-tag CPU checks: 4 passed; islcm_inited stays zero.')


if __name__ == '__main__':
    main()
