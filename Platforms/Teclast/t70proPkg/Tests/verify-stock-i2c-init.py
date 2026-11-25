#!/usr/bin/env python3
"""Run original shipped I2C init instructions with modeled kernel/MMIO leaves."""
from pathlib import Path
import hashlib
import importlib.util
import json
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
    UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC)

here=Path(__file__).resolve()
spec=importlib.util.spec_from_file_location('stock_init_helpers',here.with_name('verify-stock-power-binary.py'))
stock=importlib.util.module_from_spec(spec)
spec.loader.exec_module(stock)
binary=stock.ANALYSIS_DIR/'stock-binaries/vendor_boot_a_0/i2c-mt65xx.ko'
digest=hashlib.sha256(binary.read_bytes()).hexdigest()
assert digest=='4d8b8931bd69f292efb8d3f6a41bdf2e9fa71a0575adf2bd0a1296ffc0e60a1e'
elf=stock.parse_arm64_elf(binary.read_bytes(),'shipped I2C init')
function=elf['symbols']['mtk_i2c_init_hw']
assert function[2:]==(0x19b0,0xa50)
externals=sorted({n for _,k,n,_ in stock.function_relocations(elf,function)
    if k in (stock.R_AARCH64_CALL26,stock.R_AARCH64_JUMP26) and not elf['symbols'][n][1]})
assert externals==['__const_udelay','_raw_spin_lock_irqsave','_raw_spin_unlock_irqrestore']
uc,bases,stubs,stop,_,relocations=stock.make_module_emulator(elf,[function],externals,0x10000000)
entry=bases[function[1]]+function[2]
leaves={bases[elf['symbols'][n][1]]+elf['symbols'][n][2]:n
        for n in ('readl','readw','writel','writew')}
external_at={a:n for n,a in stubs.items()}
arena,stack,mmio,dma=0x40000000,0x30000000,0x50000000,0x60000000
uc.mem_map(arena,0x10000)
uc.mem_map(stack,0x10000)
controller,compat,reg_table=arena,arena+0x3000,arena+0x3100

def put(a,f,*values):
    uc.mem_write(a,struct.pack('<'+f,*values))

def data(n):
    _,s,o,z=elf['symbols'][n]
    return elf['section_bytes'](s)[o:o+z]

uc.mem_write(compat,data('mt6983_compat'))
uc.mem_write(reg_table,data('mt_i2c_regs_v2'))
assert data('mt6983_compat')[23]==1
assert struct.unpack_from('<H',data('mt_i2c_regs_v2'),56)[0]==0x90
assert struct.unpack_from('<H',data('mt_i2c_regs_v2'),8)[0]==0x0c
put(compat+8,'Q',reg_table)
active={}

def ret(m,v=0):
    m.reg_write(UC_ARM64_REG_X0,v)
    m.reg_write(UC_ARM64_REG_PC,m.reg_read(UC_ARM64_REG_LR))

def instruction(m,address,size,context):
    if address==stop:
        active['returned']=True
        m.emu_stop()
        return
    name=leaves.get(address)
    if name:
        writing=name.startswith('write')
        addr=m.reg_read(UC_ARM64_REG_X1 if writing else UC_ARM64_REG_X0)
        assert mmio<=addr<mmio+0x1000 or dma<=addr<dma+0x1000,hex(addr)
        offset=addr-mmio if addr<dma else 0x1000+addr-dma
        assert name in ('readl','writel'),name
        if writing:
            value=m.reg_read(UC_ARM64_REG_X0)&0xffffffff
            active['writes'].append([offset,4,value])
            if offset==0x0c:
                active['registers'][offset]=active['registers'].get(offset,0)&~value
            else:
                active['registers'][offset]=value
            ret(m)
        else:
            active['reads'].append(offset)
            ret(m,active['registers'].get(offset,0))
    elif address in external_at:
        name=external_at[address]
        active['kernel_calls'].append(name)
        ret(m)

uc.hook_add(UC_HOOK_CODE,instruction)
cases=[]
for initial_status in (0,1,0x1f,0x200,0x201,0x21f):
    for inherited_compensation in (0,1,0xa5a5,0xffffffff):
        uc.mem_write(controller,bytes(0x1000))
        put(controller+1280,'Q',mmio)
        put(controller+1288,'Q',dma)
        put(controller+1456,'Q',compat)
        put(controller+1352,'I',400000)
        # Use fixed cached timing values: this test executes init, not clock calculation.
        for offset,value in ((1388,0x123),(1392,0),(1396,0x234),(1416,0x123),
                             (1420,0x234),(1424,0),(1428,0x8001),(1432,0),(1452,0x200)):
            put(controller+offset,'I',value)
        active.clear()
        active.update(registers={0x0c:initial_status,0x90:inherited_compensation},
                      writes=[],reads=[],kernel_calls=[],returned=False)
        for r,v in ((UC_ARM64_REG_X0,controller),(UC_ARM64_REG_SP,stack+0xf000),(UC_ARM64_REG_LR,stop)):
            uc.reg_write(r,v)
        uc.emu_start(entry,stop+4,count=10000)
        assert active['returned']
        assert active['registers'][0x90]==0 and [0x90,4,0] in active['writes']
        assert active['registers'][0x0c]==0
        if initial_status&0x200:
            assert [0x0c,4,0x200] in active['writes']
        assert [0x100c,4,1] in active['writes'] and [0x100c,4,6] in active['writes']
        if inherited_compensation==0xa5a5:
            cases.append(dict(initial_status=initial_status,inherited_compensation=inherited_compensation,
                              writes=list(active['writes']),kernel_calls=list(active['kernel_calls'])))

assert hashlib.sha256(binary.read_bytes()).hexdigest()==digest
report=dict(status='passed',cases=24,module_sha256=digest,function=function[0],
    function_offset=hex(function[2]),function_bytes=function[3],function_instruction_patches=0,
    relocations_applied=relocations,controller_MMIO_width_bytes=4,
    cleared_registers=['INTR_STAT 0x0c bit9','SCL_MIS_COMP_POINT 0x90'],
    cached_timing_values_are_fixture=True,physical_timing_reproduced=False,physical_device_access=False,
    scope='Original shipped ARM64 init instructions; kernel/MMIO accessors and register effects modeled',
    test_sha256=hashlib.sha256(here.read_bytes()).hexdigest(),selected_cases=cases)
out=stock.ANALYSIS_DIR/'stock-i2c-init-arm64.json'
out.write_text(json.dumps(report,indent=2)+'\n')
print('Original ARM64 I2C init: 24 inherited-status/timing cases PASS; physical timing unmodeled')
