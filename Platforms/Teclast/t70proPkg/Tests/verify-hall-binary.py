#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute shipped MH248 input/PM and EINT suspend/resume with virtual services.

No module init, kernel loading, GPIO device or tablet operation is performed.
The stock module remains unchanged and is used only with its exact kernel ABI.
"""
from pathlib import Path
import hashlib,importlib.util,json,re,struct
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_LR,UC_ARM64_REG_SP,UC_ARM64_REG_PC
PKG=Path(__file__).resolve().parents[1];ROOT=PKG.parents[2]
MODULE=ROOT.parent/'analysis/stock-binaries/vendor_dlkm_a/hall_sensor_p.ko'
SHA='d4d1c2f656a78afab80debfd67045f54e51b34bb818addd0dbad29119c1b11c3'
HELPER=Path(__file__).with_name('verify-himax-binary.py')
spec=importlib.util.spec_from_file_location('stock_arm64_machine',HELPER)
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
def read_elf(module,sha):
 raw=module.read_bytes();assert hashlib.sha256(raw).hexdigest()==sha
 assert raw[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<HH',raw,16)==(1,183)
 start=struct.unpack_from('<Q',raw,40)[0];width,count=struct.unpack_from('<HH',raw,58)
 assert width==64 and start+width*count<=len(raw)
 sections=[struct.unpack_from('<IIQQQQIIQQ',raw,start+i*width) for i in range(count)]
 def content(i):
  s=sections[i]
  if s[1]==8:return bytes(s[5])
  assert s[4]+s[5]<=len(raw);return raw[s[4]:s[4]+s[5]]
 symbols={};tabs={}
 for i,s in enumerate(sections):
  if s[1]!=2:continue
  assert s[9]==24 and s[5]%24==0
  names=content(s[6]);tab=[]
  for offset in range(0,s[5],24):
   no,info,other,si,value,size=struct.unpack_from('<IBBHQQ',content(i),offset)
   assert no<len(names) and names.find(b'\0',no)>=0
   name=names[no:].split(b'\0',1)[0].decode();item=(name,si,value,size);tab.append(item)
   if name:symbols[name]=item
  tabs[i]=tab
 bases={};cursor=0x10000000
 for i,s in enumerate(sections):
  if s[2]&2 and s[5]:bases[i]=cursor;cursor+=(s[5]+4095)&~4095
 return raw,sections,content,symbols,tabs,bases
raw,sections,content,symbols,tabs,bases=read_elf(MODULE,SHA)
functions={'hall_work_func':(0x88,0x188),'switch_handle':(4,0x80),
 'input_hal_suspend':(0x214,0x98),'input_hal_resume':(0x2b0,0x18c)}
ranges=[]
for name,expected in functions.items():
 s=symbols[name];assert s[1]==13 and s[2:]==expected;ranges.append((s[1],s[2],s[2]+s[3]))
allowed={'_printk','gpio_to_desc','gpiod_get_raw_value','gpiod_to_irq','msleep','input_event',
 '_raw_spin_lock_irqsave','_raw_spin_unlock_irqrestore','enable_irq','disable_irq_nosync',
 'queue_work_on','__flush_workqueue','__const_udelay'}
u,stop,externals,address,relocations=helper.make_stock_machine(raw,sections,content,tabs,symbols,bases,ranges,allowed)
external_at={pc:name for name,pc in externals.items()};trace=[];samples=[]
INPUT=0x30002000;WQ=0x30003000;DESC=0x30000800;IRQ=123
active={pc for si,lo,hi in ranges for pc in range(bases[si]+lo,bases[si]+hi,4)}
def trap(uc,pc,size,unused):
 if pc==stop:uc.emu_stop();return
 name=external_at.get(pc)
 if not name:
  assert pc in active,('unselected stock code',hex(pc));return
 a,b,c,d=(uc.reg_read(r) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3))
 result=0
 if name=='gpio_to_desc':assert a==0;result=DESC
 elif name=='gpiod_get_raw_value':assert a==DESC and samples;result=samples.pop(0);trace.append(('raw',result))
 elif name=='gpiod_to_irq':assert a==DESC;result=IRQ
 elif name=='input_event':assert a==INPUT;trace.append(('event',b,c,d))
 elif name in ('msleep','__const_udelay'):trace.append((name,a))
 elif name in ('enable_irq','disable_irq_nosync'):assert a==IRQ;trace.append((name,a))
 elif name=='_raw_spin_lock_irqsave':assert a==address(symbols['irq_lock']);result=0x5a5a
 elif name=='_raw_spin_unlock_irqrestore':assert a==address(symbols['irq_lock']) and b==0x5a5a
 elif name=='queue_work_on':assert b==WQ and c==address(symbols['work']);trace.append((name,a));result=1
 elif name=='__flush_workqueue':assert a==WQ;trace.append((name,))
 elif name!='_printk':raise AssertionError(name)
 uc.reg_write(UC_ARM64_REG_X0,result);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
u.hook_add(UC_HOOK_CODE,trap)
def put(name,value):
 sym=symbols[name];assert sym[3] in (1,4,8);u.mem_write(address(sym),value.to_bytes(sym[3],'little'))
def get(name):
 sym=symbols[name];return int.from_bytes(u.mem_read(address(sym),sym[3]),'little')
def reset(old,first=1,wake=1):
 trace.clear();samples.clear()
 for name,value in {'input_dev':INPUT,'hal_wq':WQ,'int_number':0,'irq_number':IRQ,
  'sNearKey':142,'sFarKey':143,'far_gpvalue':0,'old_level_status':old,
  'is_first':first,'isWakeSource':wake,'sys_status':0}.items():put(name,value)
def call(name,values):
 samples.extend(values);u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_SP,0x4000fff0);u.reg_write(UC_ARM64_REG_LR,stop)
 u.emu_start(address(symbols[name]),stop,count=20000,timeout=1000000)
 assert u.reg_read(UC_ARM64_REG_PC)==stop and not samples,(name,trace,samples)
def pulse(code):return [('event',1,code,1),('event',0,0,0),('event',1,code,0),('event',0,0,0)]
cases=[]
for first in (0,1):
 for old in (0,1):
  for a,b in ((0,0),(0,1),(1,0),(1,1)):
   reset(old,first);call('hall_work_func',[a,b]);events=[x for x in trace if x[0]=='event']
   report=a==b and (not first or b!=old)
   assert events==(pulse(143 if b==0 else 142) if report else []),(first,old,a,b,trace)
   assert [x for x in trace if x[0]=='msleep']==([('msleep',10),('msleep',100)] if report else [('msleep',10)]),trace
   assert [x for x in trace if x[0]=='enable_irq']==[('enable_irq',IRQ)],trace
   cases.append({'function':'hall_work_func','first_event_seen':first,'old':old,'samples':[a,b],'events':events})
for raw_level in (0,1):
 reset(1-raw_level);call('input_hal_suspend',[raw_level])
 assert get('sys_status')==1 and get('old_level_status')==raw_level and ('__flush_workqueue',) in trace,trace
 cases.append({'function':'input_hal_suspend','raw':raw_level,'saved':get('old_level_status')})
for wake in (0,1):
 for first in (0,1):
  for old in (0,1):
   for raw_level in (0,1):
    reset(old,first,wake);put('sys_status',1);call('input_hal_resume',[raw_level])
    report=bool(wake and first and old!=raw_level and raw_level==0)
    events=[x for x in trace if x[0]=='event']
    assert events==(pulse(143) if report else []),(wake,first,old,raw_level,trace)
    assert [x for x in trace if x[0]=='__const_udelay']==([('__const_udelay',0x14f8c)] if report else []),trace
    assert get('sys_status')==0
    cases.append({'function':'input_hal_resume','wake_enabled':wake,'first_event_seen':first,'old':old,'raw':raw_level,'events':events})
reset(0);call('switch_handle',[])
assert u.reg_read(UC_ARM64_REG_X0)==1 and [x[0] for x in trace]==['disable_irq_nosync','queue_work_on'],trace
cases.append({'function':'switch_handle','trace':trace.copy()})
def verify_eint():
 module=ROOT.parent/'analysis/stock-binaries/vendor_boot_a_0/pinctrl-mtk-v2.ko'
 sha='e06f26d9006bedb7114f4a634517d5080a895b1093362ddf8727f73b1f096cd5'
 raw,sections,content,symbols,tabs,bases=read_elf(module,sha)
 functions={'mtk_eint_do_suspend':(4,0xdc),'mtk_eint_do_resume':(0x15c,0xdc)}
 ranges=[]
 for name,expected in functions.items():
  sym=symbols[name];assert sym[2:]==expected;ranges.append((sym[1],sym[2],sym[2]+sym[3]))
 # The ELF has two local writel_relaxed symbols. These functions call 0xe0.
 writer=next(sym for tab in tabs.values() for sym in tab if sym[0]=='writel_relaxed' and sym[2:]==(0xe0,0x78))
 ranges.append((writer[1],writer[2],writer[2]+writer[3]))
 allowed={'log_write_mmio','log_post_write_mmio'}
 u,stop,external,address,relocs=helper.make_stock_machine(raw,sections,content,tabs,symbols,bases,ranges,allowed)
 regsym=symbols['mtk_generic_eint_regs'];assert regsym[3]==19*4
 offsets=struct.unpack('<19I',content(regsym[1])[regsym[2]:regsym[2]+regsym[3]])
 core=ROOT/'Windows/MtkGpio/Hardware.c';source=core.read_text()
 for name,index in (('ACK',1),('MASK',2),('MASK_SET',3),('MASK_CLR',4)):
  assert int(re.search(r'^#define '+name+r' (0x[0-9A-Fa-f]+)U$',source,re.M)[1],16)==offsets[index]
 EINT=0x30000000;INST=0x30001000;COMP=0x30002000;WAKE=0x30003000;CUR=0x30004000;MMIO=0x50000000
 u.mem_map(MMIO,0x2000)
 u.mem_write(EINT+0x20,struct.pack('<Q',INST));u.mem_write(EINT+0x38,struct.pack('<Q',COMP))
 u.mem_write(COMP+8,struct.pack('<Q',address(regsym)))
 active={bases[si]+pc for si,lo,hi in ranges for pc in range(lo,hi,4)}
 external_at={pc:name for name,pc in external.items()};events=[];writes=[];masks=[]
 def trap(uc,pc,size,unused):
  if pc==stop:uc.emu_stop();return
  name=external_at.get(pc)
  if name:
   value=uc.reg_read(UC_ARM64_REG_X0)&0xffffffff
   assert uc.reg_read(UC_ARM64_REG_X1)==32
   target=uc.reg_read(UC_ARM64_REG_X2)
   assert MMIO<=target<MMIO+0x2000
   events.append((name,target-MMIO,value))
   uc.reg_write(UC_ARM64_REG_X0,0);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert pc in active,('unselected EINT code',hex(pc))
  if bytes(uc.mem_read(pc,4))==struct.pack('<I',0xd5033f9f):events.append(('dsb_sy',))
 def write(uc,access,target,size,value,unused):
  assert size==4
  instance,offset=divmod(target-MMIO,0x1000)
  assert instance<len(masks)
  if offsets[3]<=offset<offsets[3]+len(masks[instance])*4:
   port=(offset-offsets[3])//4;masks[instance][port]|=value
  else:
   assert offsets[4]<=offset<offsets[4]+len(masks[instance])*4
   port=(offset-offsets[4])//4;masks[instance][port]&=~value&0xffffffff
  writes.append((target-MMIO,value));events.append(('write',target-MMIO,value))
 u.hook_add(UC_HOOK_CODE,trap)
 u.hook_add(UC_HOOK_MEM_WRITE,write,begin=MMIO,end=MMIO+0x1fff)
 results=[]
 for counts in ((),(0,),(1,),(31,),(32,),(33,),(126,),(160,),(33,65),(0,160),(160,0),(160,160)):
  for pattern in range(4):
   for name in functions:
    events.clear();writes.clear();masks.clear();expected=[];wake=[];current=[]
    u.mem_write(EINT+0x2c,struct.pack('<I',len(counts)))
    for i,number in enumerate(counts):
     ports=(number+31)//32;instance=INST+i*0x228
     u.mem_write(instance+8,struct.pack('<QI',MMIO+i*0x1000,number))
     u.mem_write(instance+0x218,struct.pack('<QQ',WAKE+i*0x100,CUR+i*0x100))
     masks.append([0x967A5A69]*ports)
     def value(port,salt):
      return (0,0xffffffff,0x55555555^salt,((port+1)*0x12679A3B+salt*0x10203041)&0xffffffff)[pattern]
     w=[value(p,i+1) for p in range(ports)];c=[value(p,i+9)^0xffffffff for p in range(ports)]
     wake.append(w);current.append(c)
     if ports:
      u.mem_write(WAKE+i*0x100,struct.pack('<'+'I'*ports,*w))
      u.mem_write(CUR+i*0x100,struct.pack('<'+'I'*ports,*c))
     selected=w if name.endswith('suspend') else c
     for port,bits in enumerate(selected):
      expected.extend(((i*0x1000+offsets[3]+port*4,(~bits)&0xffffffff),(i*0x1000+offsets[4]+port*4,bits)))
    u.reg_write(UC_ARM64_REG_X0,EINT);u.reg_write(UC_ARM64_REG_SP,0x4000fff0);u.reg_write(UC_ARM64_REG_LR,stop)
    u.emu_start(address(symbols[name]),stop,count=20000,timeout=1000000)
    assert u.reg_read(UC_ARM64_REG_PC)==stop and u.reg_read(UC_ARM64_REG_X0)==0
    assert writes==expected,(name,counts,pattern,writes,expected)
    assert events==[event for target,value in expected for event in
      (('log_write_mmio',target,value),('write',target,value),('log_post_write_mmio',target,value))]+[('dsb_sy',)]
    selected=wake if name.endswith('suspend') else current
    assert masks==[[~bits&0xffffffff for bits in instance] for instance in selected]
    results.append({'function':name,'instance_pin_counts':counts,'pattern':pattern,'writes':len(writes),
      'trace_sha256':hashlib.sha256(json.dumps(events,separators=(',',':')).encode()).hexdigest()})
 return {'status':'passed','module':str(module.relative_to(ROOT.parent)),'module_sha256':sha,
  'scope':'Original EINT suspend/resume and writel_relaxed ARM64 bodies; exact generic register table from ELF. Synthetic instances/masks/MMIO; logging imports mocked; no kernel load, power transition or tablet access.',
  'method':'Local LLVM disassembly and Unicorn; REA Ghidra provider was unavailable because GHIDRA_INSTALL_DIR was not configured; no decompiler evidence claimed',
  'functions':{name:{'section_offset':off,'bytes':size} for name,(off,size) in functions.items()},
  'writel_relaxed_offset':writer[2],'elf_relocations_applied':relocs,
  'registers':dict(zip(('ACK','MASK','MASK_SET','MASK_CLR'),(offsets[1],offsets[2],offsets[3],offsets[4]))),
  'case_count':len(results),'cases':results,'set_then_clear_and_final_dsb_verified':True,
  'port_difference':'The port restricts SET/CLR to caller-owned IRQs, reads the mask back, and restores snapshots on failure. OS wake-mask and powered-MMIO producers remain required.'}
eint=verify_eint()
report={'status':'passed','scope':__doc__.strip(),'stock_module_sha256':SHA,'elf_relocations_applied':relocations,
 'eint_suspend_resume':eint,
 'gpio_fixture':{'controller_pin':0,'far_open_raw':0,'near_closed_raw':1},'cases':cases,'case_count':len(cases),
 'functions':{name:{'section_offset':off,'bytes':size} for name,(off,size) in functions.items()},
 'linux_events':{'closed':'KEY_SLEEP (142)','opened':'KEY_WAKEUP (143)','SW_LID_emitted_by_stock':False},
 'physical_suspend_resume_tested':False,'kernel_module_loaded':False,
 'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (Path(__file__).resolve(),HELPER,ROOT/'Windows/MtkGpio/Hardware.c')}}
(ROOT.parent/'analysis/hall-binary-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print('Original ARM64: MH248 input/IRQ/PM %d and EINT suspend/resume %d virtual cases passed; no tablet execution'%(len(cases),eint['case_count']))
