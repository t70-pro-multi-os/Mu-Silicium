#!/usr/bin/env python3
"""Execute the shipped ARM64 I2C PIO write/read and failure paths.

The original transfer instructions run unmodified apart from ELF relocations.
MMIO accessors, completion and kernel calls are modeled. This establishes the
original transport sequence; it cannot establish physical HUSB311 timing.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
    UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC,
)

here = Path(__file__).resolve()
spec = importlib.util.spec_from_file_location('stock', here.with_name('verify-stock-power-binary.py'))
stock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stock)
binary = stock.ANALYSIS_DIR / 'stock-binaries/vendor_boot_a_0/i2c-mt65xx.ko'
raw = binary.read_bytes()
digest = hashlib.sha256(raw).hexdigest()
assert digest == '4d8b8931bd69f292efb8d3f6a41bdf2e9fa71a0575adf2bd0a1296ffc0e60a1e'
elf = stock.parse_arm64_elf(raw, 'shipped I2C')
# Follow the actual board compatible through the original driver's OF table
# and its relocated data pointer; do not assume a similarly named SoC config.
dt_spec = importlib.util.spec_from_file_location('stock_i2c_dt', here.parents[1]/'Linux/platform-modules.py')
dt_parser = importlib.util.module_from_spec(dt_spec)
dt_spec.loader.exec_module(dt_parser)
dt_raw = (stock.ANALYSIS_DIR/'stock-fdt/effective.dtb').read_bytes()
dt_sha = hashlib.sha256(dt_raw).hexdigest()
assert dt_sha == 'a3dd45e28ee284f1995374091eca9634f68c88dfddc5ecd27a37cdcc877dd391'
nodes, _ = dt_parser.tree(dt_raw)
controller_nodes = ['/soc/i2c@11e00000', '/soc/i2c@1101a000']
for path in controller_nodes:
    assert nodes[path]['enabled'] and nodes[path]['compatible'] == ['mediatek,mt6983-i2c']
matches = [m for m in dt_parser.of_matches(raw) if m['compatible']=='mediatek,mt6983-i2c']
assert len(matches)==1
match = matches[0]
table = elf['symbols'][match['symbol']]
slot = match['module_file_offset']-elf['sections'][table[1]][4]+192
targets = []
for i, section in enumerate(elf['sections']):
    if section[1]!=stock.SHT_RELA or section[7]!=table[1]: continue
    for offset in range(0, section[5], 24):
        place, info, addend = struct.unpack_from('<QQq', elf['section_bytes'](i), offset)
        if place!=slot: continue
        target = elf['symbol_tables'][section[6]][info >> 32]
        assert info & 0xffffffff == 257
        targets.append((target[1],target[2]+addend))
assert targets == [elf['symbols']['mt6983_compat'][1:3]]
function = elf['symbols']['mtk_i2c_do_transfer']
assert function[2:] == (0x2ae0, 0x1da8)
externals = sorted({name for _, kind, name, _ in stock.function_relocations(elf, function)
                    if kind in (stock.R_AARCH64_CALL26, stock.R_AARCH64_JUMP26)
                    and not elf['symbols'][name][1]})
uc, bases, stubs, stop, _, relocations = stock.make_module_emulator(elf, [function], externals, 0x10000000)
entry = bases[function[1]] + function[2]
leaves = {bases[elf['symbols'][n][1]] + elf['symbols'][n][2]: n for n in
          ('readb', 'readw', 'readl', 'writeb', 'writew', 'writel', 'mtk_i2c_init_hw')}
external_at = {a: n for n, a in stubs.items()}
arena, stack = 0x40000000, 0x30000000
uc.mem_map(arena, 0x10000)
uc.mem_map(stack, 0x10000)
controller, messages, payload, result, compat, reg_table = [arena+x for x in (0,0x2000,0x2100,0x2200,0x3000,0x3100)]
mmio, dma = 0x50000000, 0x60000000

def put(address, format, *values):
    uc.mem_write(address, struct.pack('<'+format, *values))

def symbol_bytes(name):
    _, section, offset, size = elf['symbols'][name]
    return elf['section_bytes'](section)[offset:offset+size]

assert len(symbol_bytes('mt6983_compat')) == 24
uc.mem_write(compat, symbol_bytes('mt6983_compat'))
uc.mem_write(reg_table, symbol_bytes('mt_i2c_regs_v2'))
put(compat+8, 'Q', reg_table)
put(controller+1280, 'Q', mmio)
put(controller+1288, 'Q', dma)
put(controller+1456, 'Q', compat)
put(controller+1352, 'I', 400000)
active = {}

def ret(machine, value=0):
    machine.reg_write(UC_ARM64_REG_X0, value & 0xffffffffffffffff)
    machine.reg_write(UC_ARM64_REG_PC, machine.reg_read(UC_ARM64_REG_LR))

def instruction(machine, address, size, context):
    if address == stop:
        active['returned'] = True
        machine.emu_stop()
        return
    name = leaves.get(address)
    if name == 'mtk_i2c_init_hw':
        active['resets'] += 1
        assert active['waited'], 'Unexpected initialization in configured fixture'
        ret(machine)
    elif name and name.startswith('write'):
        value = machine.reg_read(UC_ARM64_REG_X0)
        destination = machine.reg_read(UC_ARM64_REG_X1)
        width = {'writeb':1, 'writew':2, 'writel':4}[name]
        assert mmio <= destination < mmio+0x1000 or dma <= destination < dma+0x1000, hex(destination)
        offset = destination-mmio if destination<dma else 0x1000+destination-dma
        assert width == (1 if offset==0 else 4), (offset,width)
        value &= (1 << (width*8))-1
        active['writes'].append([offset, width, value])
        if offset == 0:
            assert width == 1
            active['tx'].append(value)
        elif offset == 0x38:
            active['tx'].clear()
        active['registers'][offset] = value
        ret(machine)
    elif name and name.startswith('read'):
        source = machine.reg_read(UC_ARM64_REG_X0)
        if dma <= source < dma+0x1000:
            offset=0x1000+source-dma
            value=active['registers'].get(offset,0)
            if offset==0x100c: value &= ~1  # original warm-reset bit self-clears
            ret(machine, value)
            return
        assert mmio <= source < mmio+0x1000, hex(source)
        offset = source-mmio
        assert name == ('readb' if offset==0 else 'readl'), (offset,name)
        if offset == 0:
            assert name == 'readb' and active['rx']
            value = active['rx'].pop(0)
        else:
            value = active['registers'].get(offset, 0)
        ret(machine, value)
    elif address in external_at:
        name = external_at[address]
        if name == 'cache_line_size':
            ret(machine, 64)
        elif name == 'wait_for_completion_timeout':
            assert active['registers'].get(0x24,0)&1
            active['waited'] = True
            active['frame'] = list(active['tx'])
            flags = {'ok':1, 'nack':3, 'timeout':0}[active['fault']]
            put(controller+1344,'I',flags)
            if flags == 1:
                assert active['registers'][0x94] == (0x9d if active['read_only'] else 0x9c)
                if active['read_only']:
                    assert active['tx']==[]
                    assert [0x100c,4,5] in active['writes']
                    assert [0x50,4,0x20] in active['writes']
                    active['rx']=[active['control']]
                elif active['combined']:
                    assert active['tx'] == [0x1c]
                    assert active['registers'][0x14] == 1
                    assert active['registers'][0x44] == 1
                    assert active['registers'][0x18] == 2
                    active['rx'] = [active['control']]
                else:
                    assert active['tx'] == [0x1c, active['control']&0xfe]
                    assert active['registers'][0x14] == 2
                    assert active['registers'][0x18] == 1
                active['registers'][0x24] &= ~1
            ret(machine, int(flags != 0))
        elif name in ('sched_clock', 'ktime_get'):
            active['clock'] = active.get('clock', 0)+1000
            ret(machine, active['clock'])
        elif name in ('_dev_info', 'gpio_dump_regs_range'):
            ret(machine)
        else:
            raise AssertionError('Unmodeled kernel call: '+name)

uc.hook_add(UC_HOOK_CODE,instruction)
cases = []
for kind in ('write', 'combined', 'multi-read'):
    combined=kind=='combined'
    read_only=kind=='multi-read'
    for control in range(256):
        for fault in ('ok','nack','timeout'):
            active.clear()
            active.update(control=control, combined=combined, read_only=read_only, fault=fault,
                          registers={0x20:1,0x10:0x28}, writes=[], tx=[], rx=[],
                          resets=0, waited=False, returned=False)
            put(controller+1384,'I',3 if combined else 2 if read_only else 1)
            put(controller+1400,'B',0 if combined else 1)
            put(messages,'HHH2xQ',0x4e,int(read_only),1 if (combined or read_only) else 2,result if read_only else payload)
            put(messages+16,'HHH2xQ',0x4e,1,1,result)
            uc.mem_write(payload,bytes([0x1c,control&0xfe]))
            for reg,value in ((UC_ARM64_REG_X0,controller),(UC_ARM64_REG_X1,messages),
                              (UC_ARM64_REG_X2,2 if (combined or read_only) else 1),(UC_ARM64_REG_X3,0),
                              (UC_ARM64_REG_SP,stack+0xf000),(UC_ARM64_REG_LR,stop)):
                uc.reg_write(reg,value)
            uc.emu_start(entry,stop+4,count=20000)
            code=uc.reg_read(UC_ARM64_REG_X0)&0xffffffff
            assert active['returned'] and active['waited']
            assert code == {'ok':0,'nack':0xfffffffa,'timeout':0xffffff92}[fault], (control,combined,fault,hex(code))
            assert active['resets'] == (fault != 'ok')
            if fault == 'ok':
                assert not any(o in (0x24,0x50) for o,_,_ in active['writes'][next(i for i,w in enumerate(active['writes']) if w[0]==0x24)+1:])
                if combined or read_only:
                    assert bytes(uc.mem_read(result,1)) == bytes([control])
            if control in (1,3) and fault=='ok':
                cases.append({k:active[k] for k in ('control','combined','read_only','fault','frame','writes','resets')})

assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest
out = stock.ANALYSIS_DIR / 'stock-i2c-completion-arm64.json'
out.write_text(json.dumps({
    'status':'passed','module_sha256':digest,'function':function[0],
    'function_offset':hex(function[2]),'function_bytes':function[3],
    'function_instruction_patches':0,'relocations_applied':relocations,
    'cases':2304,'normal_write_combined_and_sequence_read_cases':768,
    'nack_and_timeout_cases':1536,
    'successful_transfer_resets':0,'error_path_resets':1536,
    'sequence_read_DMA_handshake_reset_value':5,'warm_reset_busy_mask':1,
    'scope':'Original shipped ARM64 transfer instructions; MMIO/completion/kernel calls modeled',
    'physical_timing_reproduced':False,'physical_device_access':False,
    'controller_MMIO_width_bytes':4,'FIFO_width_bytes':1,
    'mt6983_compat_width_selector_byte_23':symbol_bytes('mt6983_compat')[23],
    'stock_DTB_sha256':dt_sha,'board_I2C_nodes':controller_nodes,
    'original_driver_match':match,'OF_data_pointer_target':'mt6983_compat',
    'test_sha256':hashlib.sha256(here.read_bytes()).hexdigest(),'selected_cases':cases,
},indent=2)+'\n')
print('Original ARM64 I2C: 2304 cases PASS; success does not reset; physical timing unmodeled')
