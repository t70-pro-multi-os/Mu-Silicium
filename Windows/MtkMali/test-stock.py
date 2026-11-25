#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare G57 PTEs and IRQ offsets with original ARM64 instruction bodies.

Pure encoders and the register-map initializer with preallocated virtual buffers
run without external calls. No module initialization, MMIO, kernel or device is
executed. Unsupported production inputs are covered separately by CoreTests.c.
"""
from pathlib import Path
import ctypes as C
import hashlib,json,re,struct,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_SP
R=Path(__file__).resolve().parents[2]
P=R/'Windows/MtkMali'
B=R.parent/'analysis/stock-binaries/vendor_dlkm_a/mali_kbase_mt6789_a16w_jm.ko'
SHA='3b5a27d70fbe3891f6e0a051899a4cff22bf89a3f8aefebfe1981ae31ef5b4cf'
raw=B.read_bytes();assert hashlib.sha256(raw).hexdigest()==SHA
assert raw[:6]==bytes.fromhex('7f454c460201') and struct.unpack_from('<H',raw,18)[0]==183
sht=struct.unpack_from('<Q',raw,40)[0];width,n,st=struct.unpack_from('<HHH',raw,58)
assert width==64 and sht+n*width<=len(raw)
secs=[struct.unpack_from('<IIQQQQIIQQ',raw,sht+i*width) for i in range(n)]
def data(i):
 s=secs[i];assert s[4]+s[5]<=len(raw);return raw[s[4]:s[4]+s[5]]
symbols={}
for i,s in enumerate(secs):
 if s[1]!=2:continue
 names=data(s[6]);sym=data(i);assert s[9]==24
 for k in range(0,len(sym),24):
  no,info,other,si,off,size=struct.unpack_from('<IBBHQQ',sym,k)
  name=names[no:].split(b'\0',1)[0].decode()
  if name:symbols[name]=(si,off,size)
functions={'entry_set_ate':(0x48f7c,0xa4),'entry_set_pte':(0x49024,0x10),'kbase_mmu_get_as_setup':(0x490c0,0x28)}
textsec=symbols['entry_set_ate'][0]
for name,(offset,size) in functions.items():assert symbols[name]==(textsec,offset,size)
for i,s in enumerate(secs):
 if s[1]==4 and s[7]==textsec:
  for k in range(0,len(data(i)),24):
   off,info,add=struct.unpack_from('<QQq',data(i),k)
   assert not any(a<=off<a+z for a,z in functions.values()),'pure function unexpectedly has relocation'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);base=0x100000
text=data(textsec);u.mem_map(base,(len(text)+4095)&~4095);u.mem_write(base,text)
u.mem_map(0x2000000,0x10000);u.mem_map(0x3000000,0x10000)
stop=0x200f000;out=0x2001000;context=0x2002000
lib=C.CDLL(str(Path(sys.argv[1]).resolve()))
class Pte(C.Structure):_fields_=[('Address',C.c_uint64),('Flags',C.c_uint32)]
lib.MaliJmEncodePte.argtypes=[C.c_uint32,C.POINTER(Pte),C.POINTER(C.c_uint64)]
lib.MaliJmEncodePte.restype=C.c_int
checks=0

def call(name,*args):
 for reg,val in zip((UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3),args):u.reg_write(reg,val)
 u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,0x300fff0)
 u.mem_write(out,bytes(32));u.emu_start(base+functions[name][0],stop,count=128)
 assert u.reg_read(UC_ARM64_REG_PC)==stop
 return struct.unpack('<Q',u.mem_read(out,8))[0]

for i in range(256):
 pa=((((i*0x9e3779b1+0x12345)&0xfffffff) or 1)<<12)
 for perm in range(8):
  flags=1|2|((perm&1)*4)|((perm&2)*4)|((perm&4)*4)
  kflags=(0x4000 if flags&4 else 4)|(0 if flags&8 else 8)|(0x20000 if flags&16 else 0)
  actual=call('entry_set_ate',out,pa,kflags,3);value=C.c_uint64()
  assert lib.MaliJmEncodePte(3,C.byref(Pte(pa,flags)),C.byref(value))==0
  assert value.value==actual,(hex(pa),flags,hex(value.value),hex(actual));checks+=1
 for level in range(3):
  actual=call('entry_set_pte',out,pa);value=C.c_uint64()
  assert lib.MaliJmEncodePte(level,C.byref(Pte(pa,1|32)),C.byref(value))==0
  assert value.value==actual;checks+=1
 u.mem_write(context+0x30,struct.pack('<Q',pa));call('kbase_mmu_get_as_setup',context,out)
 root,attrs,mode=struct.unpack('<QQQ',u.mem_read(out,24))
 assert (root,attrs,mode)==(pa,0x00004c8d888d8f88,6);checks+=1
# Execute the original generated map rather than reconstructing its thousands
# of pointer stores. Both buffers are preallocated, so no kernel allocator runs.
map_name='kbase_regmap_v6_0_init';map_offset,map_bytes=0x5b7cc,16484
assert symbols[map_name]==(textsec,map_offset,map_bytes)
map_address,flags_address,mmio=0x2008000,0x200a000,0x4000000
u.mem_write(context+0x88,struct.pack('<QQQQ',mmio,map_address,flags_address,0x2af))
u.mem_write(map_address,bytes(0x2af*8));u.mem_write(flags_address,bytes(0x2af*4))
u.reg_write(UC_ARM64_REG_X0,context);u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,0x300fff0)
u.emu_start(base+map_offset,stop,count=8192);assert u.reg_read(UC_ARM64_REG_PC)==stop
core=(P/'MaliJm.c').read_text();ddi=(P/'Ddi.c').read_text()
def ddi_offsets(name):
 match=re.search(r'Irq'+name+r'Offsets\[3\]=\{([^}]+)\}',ddi);assert match
 return [int(v.strip(),0) for v in match[1].split(',')]
masks=ddi_offsets('Mask');statuses=ddi_offsets('Status');register_map={}
for i,(bank,index) in enumerate((('GPU',7),('JOB',0x4d),('MMU',0x187))):
 values=[]
 for j,suffix in enumerate(('RAW','CLEAR','MASK','STATUS')):
  address=struct.unpack('<Q',u.mem_read(map_address+8*(index+j),8))[0]-mmio
  if suffix=='STATUS':assert address==statuses[i]
  else:
   match=re.search(r'#define '+bank+'_IRQ_'+suffix+r' (0x[0-9a-f]+)u',core);assert match
   assert address==int(match[1],0)
   if suffix=='MASK':assert address==masks[i]
  values.append(address)
 register_map[bank]=dict(zip(('RAW','CLEAR','MASK','STATUS'),values))
report={'status':'passed','scope':'stock ARM64 pure PTE/table/setup functions and preallocated register-map initializer versus production C; no kernel, MMIO, Windows runtime or GPU','irq_register_offsets_compared':12,
 'irq_register_map':register_map,'register_map_initializer':{'name':map_name,'offset':map_offset,'bytes':map_bytes,'external_calls_executed':False},
 'comparisons':checks,'stock_module_sha256':SHA,'functions':functions,
 'argument_policy':'Stock encoder bit2 selects read-only and overrides bit14; bit14 alone selects read/write. These are observed internal encoder inputs, not public UAPI allocation flags.',
 'policy':'AArch64-4K, non-shareable, MEMATTR default index0 or write-allocate index2; validated PA<40bits',
 'sources':{str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [P/'MaliJm.c',P/'MaliJm.h',P/'Ddi.c',Path(__file__).resolve()]}}
(R/'Windows/Build/mali-stock-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'Stock G57 ARM64 encoder/map comparison: {checks} PTE cases and 12 IRQ offsets passed (virtual buffers only)')
