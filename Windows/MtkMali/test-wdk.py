#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute actual ARM64 test PEs with explicit OS/Win32/KMT/GPU boundaries."""
from pathlib import Path
import hashlib,json,struct,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_MEM_INVALID,UC_QUERY_TIMEOUT
from unicorn.arm64_const import UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_X0,UC_ARM64_REG_X30
p=Path(sys.argv[1]); symbol=sys.argv[2]; b=p.read_bytes()
pe=struct.unpack_from('<I',b,0x3c)[0]
assert b[:2]==b'MZ' and b[pe:pe+4]==b'PE\0\0'
machine,count=struct.unpack_from('<HH',b,pe+4); assert machine==0xaa64
opt=pe+24; optsize=struct.unpack_from('<H',b,pe+20)[0]
assert struct.unpack_from('<H',b,opt)[0]==0x20b
entry=struct.unpack_from('<I',b,opt+16)[0]; base=struct.unpack_from('<Q',b,opt+24)[0]
size,headers=struct.unpack_from('<II',b,opt+56)
exports_rva,exports_size=struct.unpack_from('<II',b,opt+112)
imports_rva,imports_size=struct.unpack_from('<II',b,opt+120)
assert not imports_rva and not imports_size, 'Unexpected real OS import in self-contained test PE'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM); u.mem_map(base,(size+4095)&~4095); u.mem_write(base,b[:headers])
for i in range(count):
 off=opt+optsize+i*40; vs,rva,rs,raw=struct.unpack_from('<IIII',b,off+8)
 assert rva+max(rs,vs)<=size and raw+rs<=len(b)
 if rs:u.mem_write(base+rva,b[raw:raw+rs])
end=0x60000000; stack=0x70000000
u.mem_map(end,4096);u.mem_map(stack,0x200000)
u.reg_write(UC_ARM64_REG_SP,stack+0x200000-16);u.reg_write(UC_ARM64_REG_X30,end)
def invalid_memory(uc,access,address,length,value,_):
 raise AssertionError(f'{p.stem}: invalid emulated memory access at image RVA {uc.reg_read(UC_ARM64_REG_PC)-base:#x}, address {address:#x}, length {length}')
u.hook_add(UC_HOOK_MEM_INVALID,invalid_memory)
# MDL integration covers 33 leases, larger boundary-safe table budgets and
# whole-range R/W/X resources with failure/reset retention. Commands mutate all
# 1024 compute and 512 fragment header/payload bits and check five fragment render
# orders/eight target counts/ZS extents. Each rejected graph preserves all output.
# Four clear formats, each with successful completion and failed-reset retention,
# now map a real page-padded 2D allocation and a 256 KiB tiler heap. A bounded
# diagnostic exhausted 300M instructions with the new 24-KiB snapshots.
# Six private MDL pages and full-payload raster checks use a bounded larger
# allowance. The miniport repeatedly zeros the larger production bundle through
# the byte-wise CRT fixture; its observed 50M stop was inside that memset loop.
# Variable-resource raster ownership fixtures add bounded event tables; the
# observed UMD 50M stop was also inside the byte-wise memset at RVA 0x163d8.
# Preload success/failure cases add four code objects and 272-KiB scratch buffers;
# the observed UMD 100M stop is the same byte-wise memset at RVA 0x1d620.
# The independent ten-second execution timeout still applies.
instruction_budget=(750000000 if p.stem in ('MaliResidencyTests','MaliCommandsTests')
                    else 250000000 if p.stem=='MaliUmdTests'
                    else 100000000 if p.stem=='MaliMiniportTests'
                    else 50000000)
u.emu_start(base+entry,end,timeout=10000000,count=instruction_budget)
assert u.reg_read(UC_ARM64_REG_PC)==end,f'{p.stem}: bounded emulation stopped at RVA {u.reg_read(UC_ARM64_REG_PC)-base:#x}; budget={instruction_budget}, timeout={bool(u.query(UC_QUERY_TIMEOUT))}'
line=u.reg_read(UC_ARM64_REG_X0)&0xffffffff
assert line==0,f'{p.stem} production code failed at test source line {line}'
def read32(a):return struct.unpack('<I',u.mem_read(a,4))[0]
def cstr(a):
 out=bytearray()
 while (x:=bytes(u.mem_read(a+len(out),1)))!=b'\0':
  assert len(out)<256;out+=x
 return out.decode()
d=base+exports_rva;names=base+read32(d+32);ords=base+read32(d+36);funcs=base+read32(d+28)
assert exports_rva and exports_size
checks=None
for i in range(read32(d+24)):
 if cstr(base+read32(names+4*i))==symbol:
  ordinal=struct.unpack('<H',u.mem_read(ords+2*i,2))[0]
  checks=read32(base+read32(funcs+4*ordinal))
assert checks and checks>20,'Test assertion counter missing or empty'
report={'status':'passed','assertions':checks,'pe_sha256':hashlib.sha256(b).hexdigest(),
 'scope':('actual production user-mode C with real WDK D3D/KMT types in Windows ARM64 PE under Unicorn; Win32 heap/critical-section/events, KMT allocation/paging/submission and completion are fixtures. Generated submissions also execute the real command preparation parser. No Windows, Direct3D runtime, actual OS concurrency or GPU/pixel execution. Security cookies and stack probes disabled only in test objects.' if p.stem=='MaliUmdTests' else 'actual production C in Windows ARM64 PE under Unicorn; GPU MMIO, kernel allocation/MDL/cache/synchronization/thread services and Dxgkrnl callbacks are mocks. VM/residency are production code in MaliVmTests/MaliResidencyTests; parent preparation services are fixtures in legacy tests. Security cookies and stack probes disabled in test objects only. SEH is compiled but Windows exception dispatch is not executed; no Windows OS, real kernel concurrency or GPU'),
 'instruction_budget':instruction_budget,
 'command_preparation':('production parser/preparation, OS table tracking, MDL pinning, private roots, real DDI and scheduler; OS MDL services mocked' if p.stem=='MaliResidencyTests' else 'production parser/preparation integrated with real DDI and scheduler; parent VM/residency callbacks mocked' if p.stem=='MaliPrepareTests' else 'production bounded parser' if p.stem=='MaliCommandsTests' else 'fixture command preparation with production allocation references and scheduler' if p.stem=='MaliAllocationTests' else 'actual raster/clear producer and bounded resource parser; KMT, paging and completion services are explicit mocks' if p.stem=='MaliUmdTests' else 'legacy preparation fixture or not applicable'),
 'runner_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
p.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'{p.stem}: {checks} assertions passed (ARM64 PE emulation)')
