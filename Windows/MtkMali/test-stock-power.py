#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Original MT6789 ARM64 MFG domains/clocks versus portable C, using virtual services.

Four domain functions, stock clock/composite power control, WACS2 and actual PLL/mux/gate provider bodies and original readl/writel
wrappers execute in Unicorn. External clock-provider, delay and telemetry calls
are bounded fixtures. No module probe/init, real clock/PMIC/kernel/device code
or physical MMIO runs.
"""
from pathlib import Path
import ctypes as C
import hashlib,json,struct,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X19,UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_SP
ROOT=Path(__file__).resolve().parents[2]
STOCK=ROOT.parent/'analysis/stock-binaries/vendor_dlkm_a/mtk_gpufreq_mt6789.ko'
SHA='628068564e29bf109a2c5fa9059db7fa5a111fa1fbfb000eaa9e047260cf2b33'
def read_module(path):
 raw=path.read_bytes()
 assert raw[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',raw,18)[0]==183
 sht=struct.unpack_from('<Q',raw,40)[0];width,count=struct.unpack_from('<HH',raw,58)
 assert width==64 and sht+width*count<=len(raw)
 secs=[struct.unpack_from('<IIQQQQIIQQ',raw,sht+i*width) for i in range(count)]
 def data(i):
  q=secs[i]
  if q[1]==8:return bytes(q[5])
  assert q[4]+q[5]<=len(raw);return raw[q[4]:q[4]+q[5]]
 symbols={};tabs={}
 for i,q in enumerate(secs):
  if q[1]!=2:continue
  names=data(q[6]);syms=[];assert q[9]==24
  for k in range(0,q[5],24):
   no,info,other,si,value,size=struct.unpack_from('<IBBHQQ',data(i),k)
   name=names[no:].split(b'\0',1)[0].decode();v=(name,si,value,size);syms.append(v)
   if name:symbols[name]=v
  tabs[i]=syms
 return raw,secs,symbols,tabs,data
raw,secs,symbols,tabs,data=read_module(STOCK)
assert hashlib.sha256(raw).hexdigest()==SHA
functions={f'__gpufreq_mfg{i}_control':(0x2184,0x2408,0x2818,0x2a9c)[i] for i in range(4)}
section=symbols['__gpufreq_mfg0_control'][1];ranges=[]
for name,off in functions.items():
 v=symbols[name];assert v[1]==section and v[2]==off and v[3]>0;ranges.append((v[1],v[2],v[2]+v[3]))
for name in ('readl','writel','__gpufreq_clock_control','__gpufreq_power_control','__gpufreq_volt_scale_gpu','__gpufreq_generic_commit_gpu'):
 v=symbols[name];ranges.append((v[1],v[2],v[2]+v[3]))
# Only the pure inlined springboard block, never the probe/init body.
spring_range=(section,0x4108,0x4208);ranges.append(spring_range)
# The linked module has two local __gpufreq_abort symbols. Clock control calls
# the first body's 0x20ec entry; selecting the last name match is incorrect.
abort_symbol=next(v for tab in tabs.values() for v in tab if v[0]=='__gpufreq_abort' and v[1]==section and v[2]==0x20ec)
bases={};cursor=0x20000000
for i,s in enumerate(secs):
 if s[2]&2 and s[5]:bases[i]=cursor;cursor+=(s[5]+4095)&~4095
stub=(cursor+0x10000+4095)&~4095
external=('__const_udelay','aee_rr_rec_gpu_dvfs_vgpu','log_read_mmio','log_post_read_mmio','log_write_mmio','log_post_write_mmio','clk_prepare','clk_enable','clk_disable','clk_unprepare','clk_set_parent','_printk','mutex_lock','mutex_unlock','aee_rr_rec_gpu_dvfs_power_count','regulator_enable','regulator_disable','regulator_set_voltage','regulator_is_enabled','regulator_get_voltage','__udelay','get_random_bytes','aee_rr_rec_gpu_dvfs_oppidx','__stack_chk_fail')
stubs={name:stub+i*16 for i,name in enumerate(external)};stop=stub+4096
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x20000000,cursor-0x20000000)
for i,base in bases.items():
 if secs[i][1]!=8:u.mem_write(base,data(i))
u.mem_map(stub,8192)
for addr in stubs.values():u.mem_write(addr,bytes.fromhex('00008052c0035fd6'))
u.mem_map(0x60000000,65536)
mmio=(0x50000000,0x50010000,0x50020000,0x50030000)
for base in mmio:u.mem_map(base,4096)
for name,base in (('g_sleep',mmio[0]),('g_infracfg_ao_base',mmio[1]),('g_mfg_top_base',mmio[2]),('g_topckgen_base',mmio[3])):
 _,si,off,size=symbols[name];assert size==8;u.mem_write(bases[si]+off,struct.pack('<Q',base))
relocations=0
for i,s in enumerate(secs):
 if s[1]!=4:continue
 for k in range(0,s[5],24):
  off,info,add=struct.unpack_from('<QQq',data(i),k)
  if not any(si==s[7] and lo<=off<hi for si,lo,hi in ranges):continue
  kind=info&0xffffffff;name,si,value,size=tabs[s[6]][info>>32]
  assert si or name in stubs,('unrecognized external',name)
  target=(bases[si]+value if si else stubs[name])+add;place=bases[s[7]]+off
  word=struct.unpack('<I',u.mem_read(place,4))[0]
  if kind==275:
   delta=(target>>12)-(place>>12);assert -(1<<20)<=delta<(1<<20)
   word=(word&~((3<<29)|(0x7ffff<<5)))|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
  elif kind==277:word=(word&~(4095<<10))|((target&4095)<<10)
  elif kind in (278,284,285,286):
   shift={278:0,284:1,285:2,286:3}[kind]
   assert not target&((1<<shift)-1)
   word=(word&~(4095<<10))|(((target&4095)>>shift)<<10)
  elif kind in (282,283):
   delta=target-place;assert not delta&3 and -(1<<27)<=delta<(1<<27);word=(word&0xfc000000)|((delta>>2)&0x3ffffff)
  else:raise AssertionError(('unrecognized relocation',kind))
  u.mem_write(place,struct.pack('<I',word));relocations+=1
class Model:
 def __init__(self,domain,on,extra,lag):
  self.reg={};self.writes=[];self.delays=[];self.pending={};self.lag=lag
  mask=(1<<domain)-1 if on else (1<<(domain+1))-1
  self.reg[0,0x16c]=self.reg[0,0x170]=mask<<2
  for n in range(4):self.reg[0,0x308+n*4]=(13 if mask&(1<<n) else 0x1112)|extra
  if not mask&2:self.reg[1,0x258]=0x200000;self.reg[1,0x228]=0x600000;self.reg[1,0x724]=0xe0
 def schedule(self,key,mask,value):
  if self.reg.get(key,0)&mask==value:return
  # Power bits can be written separately before either status is polled.
  if key in self.pending and self.pending[key][0:2]==(mask,value):return
  self.pending[key]=(mask,value,self.lag)
 def read(self,r,o):
  key=r,o
  if key in self.pending:
   mask,value,left=self.pending[key]
   if left:self.pending[key]=mask,value,left-1
   else:self.reg[key]=(self.reg.get(key,0)&~mask)|value;del self.pending[key]
  return self.reg.get(key,0)
 def write(self,r,o,v):
  self.writes.append((r,o,v));old=self.reg.get((r,o),0);self.reg[r,o]=v
  if r==0 and 0x308<=o<=0x314:
   n=(o-0x308)//4;mask=1<<(n+2);value=mask if v&12==12 else 0
   for status in (0x16c,0x170):self.schedule((0,status),mask,value)
   self.reg[r,o]=(v&~4096)|(old&4096);self.schedule((r,o),4096,4096 if v&256 else 0)
  if r==1:
   for setreg,clearreg,status in ((0x2a8,0x2ac,0x258),(0x2a0,0x2a4,0x228),(0x714,0x718,0x724)):
    if o==setreg:self.reg[1,status]=self.reg.get((1,status),0)|v
    if o==clearreg:self.reg[1,status]=self.reg.get((1,status),0)&~v
model=None
clock_mode=False
power_mode=False
voltage_mode=False
commit_mode=False
commit_trace=[]
commit_fail=0
voltage_model=None
reference_handle=0x53000400
rail_handles=(0x53000500,0x53000600)
clock_handles=(0x53000000,0x53000100)
clock_parents=(0x53000200,0x53000300)
clock_data=0x52000000
u.mem_map(clock_data,4096)
_,si,off,size=symbols["g_clk"];u.mem_write(bases[si]+off,struct.pack("<Q",clock_data))
u.mem_write(clock_data,struct.pack("<5Q",clock_handles[0],reference_handle,*clock_parents,clock_handles[1]))
pmic_data=clock_data+0x100
_,si,off,size=symbols['g_pmic'];u.mem_write(bases[si]+off,struct.pack('<Q',pmic_data))
u.mem_write(pmic_data,struct.pack('<2Q',*rail_handles))
# The first g_gpu owns power_count at BSS+0x24; a later duplicate local
# symbol of the same name is a different object.
gpu_symbol=next(v for tab in tabs.values() for v in tab if v[0]=='g_gpu' and v[2]==0 and v[3]==88)
active=set()
for si,lo,hi in ranges:
 for pc in range(bases[si]+lo,bases[si]+hi,4):active.add(pc)
def code(uc,pc,size,_):
 if commit_mode:
  voltage_pc=bases[section]+symbols['__gpufreq_volt_scale_gpu'][2]
  rate_pc=bases[section]+symbols['__gpufreq_freq_scale_gpu'][2]
  if pc in (voltage_pc,rate_pc):
   a=[uc.reg_read(r)&0xffffffff for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3)]
   commit_trace.append(('voltage',a[0]*10,a[1]*10,a[2]*10,a[3]*10) if pc==voltage_pc else ('rate',a[0]*1000,a[1]*1000))
   failed=commit_fail==len(commit_trace)
   if not failed:
    if pc==voltage_pc:uc.mem_write(bases[gpu_symbol[1]]+0x4c,struct.pack('<II',a[1],a[3]))
    else:uc.mem_write(bases[gpu_symbol[1]]+0x48,struct.pack('<I',a[1]))
   uc.reg_write(UC_ARM64_REG_X0,0xfffffffb if failed else 0)
   uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert pc not in (stubs['get_random_bytes'],stubs['__stack_chk_fail']),'Unexpected stress/cookie service in fixed fixture'
 if (clock_mode or power_mode or voltage_mode) and pc==bases[abort_symbol[1]]+abort_symbol[2]:
  assert not voltage_mode,'Original voltage body aborted'
  model.aborted=True;uc.emu_stop();return
 assert pc in active or stub<=pc<stub+len(stubs)*16,hex(pc)
 if voltage_mode and pc in (stubs['regulator_set_voltage'],stubs['regulator_is_enabled'],stubs['regulator_get_voltage'],stubs['__udelay']):
  x0=uc.reg_read(UC_ARM64_REG_X0)
  if pc==stubs['__udelay']:voltage_model.delays.append(x0);result=0
  else:
   n=rail_handles.index(x0)
   if pc==stubs['regulator_set_voltage']:
    uv=uc.reg_read(UC_ARM64_REG_X1);assert uc.reg_read(UC_ARM64_REG_X2)==0x13bb45
    voltage_model.select(n,uv);result=0
   elif pc==stubs['regulator_is_enabled']:result=1
   else:result=voltage_model.voltages[n]
  uc.reg_write(UC_ARM64_REG_X0,result);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
 if pc==stubs['__const_udelay']:
  value=uc.reg_read(UC_ARM64_REG_X0);model.delays.append(value)
  if clock_mode or power_mode:model.trace.append(('delay',value))
 if power_mode and pc in (stubs['regulator_enable'],stubs['regulator_disable']):
  index=rail_handles.index(uc.reg_read(UC_ARM64_REG_X0))
  result=model.operation('rail_on' if pc==stubs['regulator_enable'] else 'rail_off',index)
  uc.reg_write(UC_ARM64_REG_X0,result&0xffffffffffffffff)
  uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
 if clock_mode or power_mode:
  for name in ('clk_prepare','clk_enable','clk_disable','clk_unprepare','clk_set_parent'):
   if pc!=stubs[name]:continue
   arg=uc.reg_read(UC_ARM64_REG_X0)
   if name=='clk_set_parent':
    assert arg==clock_handles[0]
    operation='parent';index=clock_parents.index(uc.reg_read(UC_ARM64_REG_X1))
   else:operation=name.removeprefix('clk_');index=2 if power_mode and arg==reference_handle else clock_handles.index(arg)
   result=model.operation(operation,index)
   uc.reg_write(UC_ARM64_REG_X0,result&0xffffffffffffffff)
   uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
u.hook_add(UC_HOOK_CODE,code)
def memory(uc,access,address,size,value,_):
 assert size==4 and not address&3
 region=next(r for r,base in enumerate(mmio) if base<=address<base+4096);offset=address-mmio[region]
 if access==16:uc.mem_write(address,struct.pack('<I',model.read(region,offset)))
 else:model.write(region,offset,value)
for base in mmio:u.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,memory,begin=base,end=base+4095)
READ=C.CFUNCTYPE(C.c_uint32,C.c_void_p,C.c_uint32,C.c_uint32)
WRITE=C.CFUNCTYPE(None,C.c_void_p,C.c_uint32,C.c_uint32,C.c_uint32)
VOID=C.CFUNCTYPE(None,C.c_void_p)
DELAY=C.CFUNCTYPE(None,C.c_void_p,C.c_uint32)
AUTH=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32,C.c_uint32)
read=READ(lambda x,r,o:model.read(r,o));write=WRITE(lambda x,r,o,v:model.write(r,o,v))
barrier=VOID(lambda x:None);delay=DELAY(lambda x,us:model.delays.append(us*4295));authority=AUTH(lambda x,n,on:1)
class Io(C.Structure):_fields_=[('Context',C.c_void_p),('Read',READ),('Write',WRITE),('Barrier',VOID),('Delay',DELAY),('Authorized',AUTH)]
class Device(C.Structure):_fields_=[('Io',Io),('Initialized',C.c_uint32),('Poisoned',C.c_uint32)]
lib=C.CDLL(str(Path(sys.argv[1]).resolve()));lib.MaliMfgInitialize.argtypes=[C.POINTER(Device),C.POINTER(Io)]
lib.MaliMfgSetDomain.argtypes=[C.POINTER(Device),C.c_uint32,C.c_uint32];comparisons=0
for domain in range(4):
 for on in (0,1):
  for seed in range(16):
   extra=(seed*0x9e3779b1)&0xffffffff&~0x111f;lag=seed%4
   model=Model(domain,on,extra,lag);u.reg_write(UC_ARM64_REG_X0,on);u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,0x6000fff0)
   u.emu_start(bases[section]+functions[f'__gpufreq_mfg{domain}_control'],stop,count=200000)
   assert u.reg_read(UC_ARM64_REG_PC)==stop,'Stock domain function exceeded bounded emulation'
   expected=model;model=Model(domain,on,extra,lag);dev=Device();io=Io(None,read,write,barrier,delay,authority)
   assert lib.MaliMfgInitialize(C.byref(dev),C.byref(io))==0
   assert lib.MaliMfgSetDomain(C.byref(dev),domain,on)==0,(domain,on,seed)
   assert model.writes==expected.writes,(domain,on,seed,model.writes,expected.writes)
   assert model.delays==expected.delays,(domain,on,seed,model.delays,expected.delays)
   for offset in (0x16c,0x170,0x308+domain*4):assert model.read(0,offset)==expected.read(0,offset)
   comparisons+=1
class ClockModel:
 def __init__(self,on,seed,bad,fail):
  self.reg={(2,o):(seed*0x9e3779b1)&0xffffffff for o in (0x20,0xb0,0xb4,0x24,0xf60)}
  self.reg[3,0x50]=((seed*0x9e3779b1)&0xffffffff&~0x40000)|(0 if on else 0x40000)
  self.bad=bad;self.fail=fail;self.reads=0;self.operations=0;self.trace=[];self.delays=[];self.aborted=False
 def read(self,r,o):
  assert (r,o) in self.reg,(r,o)
  if r==3:
   if self.reads>=self.bad:self.reg[r,o]|=0x40000
   else:self.reg[r,o]&=~0x40000
   self.reads+=1
  return self.reg[r,o]
 def write(self,r,o,v):
  assert r==2 and (r,o) in self.reg
  self.trace.append(('write',r,o,v));self.reg[r,o]=v
 def operation(self,name,index):
  self.trace.append((name,index))
  if name in ('prepare','enable','parent'):
   self.operations+=1
   if self.operations==self.fail:return -5
  return 0
OP=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32)
VOP=OP
prepare=OP(lambda x,n:model.operation('prepare',n));enable=OP(lambda x,n:model.operation('enable',n))
disable=VOP(lambda x,n:model.operation('disable',n));unprepare=VOP(lambda x,n:model.operation('unprepare',n))
parent=OP(lambda x,n:model.operation('parent',n))
clock_delay=DELAY(lambda x,us:(model.delays.append(us*4295),model.trace.append(('delay',us*4295))))
class ClockIo(C.Structure):_fields_=[('Registers',Io),('Prepare',OP),('Enable',OP),('Disable',VOP),('Unprepare',VOP),('Parent',OP)]
class ClockDevice(C.Structure):_fields_=[('Io',ClockIo),('Initialized',C.c_uint32),('Poisoned',C.c_uint32),('Stages',C.c_uint32)]
lib.MaliMfgClockInitialize.argtypes=[C.POINTER(ClockDevice),C.POINTER(ClockIo)]
lib.MaliMfgClockSet.argtypes=[C.POINTER(ClockDevice),C.c_uint32]
clock_cases=[];clock_mode=True
profiles=[(on,seed,bad,0) for on in (0,1) for seed in range(16) for bad in (range(8) if on else (0,))]
profiles += [(1,0,0,f) for f in range(1,6)]+[(1,0,3,f) for f in range(4,9)]+[(0,0,0,1)]
for on,seed,bad,fail in profiles:
 model=ClockModel(on,seed,bad,fail)
 u.reg_write(UC_ARM64_REG_X0,on);u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,0x6000fff0)
 v=symbols['__gpufreq_clock_control'];u.emu_start(bases[v[1]]+v[2],stop,count=200000)
 expected=model
 assert expected.aborted or u.reg_read(UC_ARM64_REG_PC)==stop,'Clock function exceeded bounded emulation'
 model=ClockModel(on,seed,bad,fail);dev=ClockDevice()
 cio=ClockIo(Io(None,read,write,barrier,clock_delay,authority),prepare,enable,disable,unprepare,parent)
 assert lib.MaliMfgClockInitialize(C.byref(dev),C.byref(cio))==0
 if not on:dev.Stages=63
 result=lib.MaliMfgClockSet(C.byref(dev),on)
 assert bool(result)==expected.aborted,(on,seed,bad,fail,result,expected.aborted)
 assert model.trace==expected.trace,(on,seed,bad,fail,model.trace,expected.trace)
 assert bool(dev.Poisoned)==expected.aborted
 if expected.aborted:assert result in (4,5)
 clock_cases.append({'on':on,'seed':seed,'initial_bad_mux_reads':bad,'provider_failure_operation':fail,
                     'stock_abort_trapped':expected.aborted,'port_status':result,'trace_events':len(model.trace)})
clock_result={'status':'passed','comparisons':len(clock_cases),'function':{'section_offset':v[2],'file_offset':secs[v[1]][4]+v[2],'bytes':v[3]},
 'evidence_id':'ev_aece4b78c5c1298f62accee4f164eff08edf9fa6fdba1d14921fe974fb004f6b',
 'compared':'exact clock-provider operation, MMIO write and delay order/arguments; both directions; mux retry boundary and provider failures',
 'intentional_difference':'Original abort body is trapped. Port returns MaliFault/MaliTimeout and preserves acquired ownership; port also verifies configuration readback.',
 'actual_clock_provider_executed':False,'cases':clock_cases}
class PowerModel(Model):
 def __init__(self,on,refs,seed,bad,fail):
  powered=refs>0
  d=Model(3 if powered else 0,0 if powered else 1,(seed*0x9e3779b1)&0xffffffff&~0x111f,seed%4)
  c=ClockModel(0 if powered else 1,seed,0 if powered else bad,fail)
  self.reg={**d.reg,**c.reg,(2,0x130):3 if powered else 0,(2,0x170):0,(2,0x178):4}
  if powered:
   for o,b in [(0x20,0x3c00000),(0xb4,0x10),(0x24,1),(0xf60,1)]:self.reg[2,o]|=b
   self.reg[2,0xb0]=(self.reg[2,0xb0]&~0x700)|0x80000
  self.pending={};self.lag=d.lag;self.writes=[];self.delays=[];self.trace=[]
  self.bad=c.bad;self.reads=0;self.fail=fail;self.operations=0;self.aborted=False
 def read(self,r,o):
  return ClockModel.read(self,r,o) if r==3 else Model.read(self,r,o)
 def write(self,r,o,v):
  self.trace.append(('write',r,o,v))
  if r<2:Model.write(self,r,o,v)
  else:self.reg[r,o]=v
 def operation(self,name,index):
  self.trace.append((name,index))
  if name in ('prepare','enable','parent','rail_on','rail_off'):
   self.operations+=1
   if self.operations==self.fail:return -5
  return 0
ZEROOP=C.CFUNCTYPE(C.c_int,C.c_void_p)
ref_prepare=ZEROOP(lambda x:model.operation('prepare',2));ref_enable=ZEROOP(lambda x:model.operation('enable',2))
ref_disable=ZEROOP(lambda x:model.operation('disable',2));ref_unprepare=ZEROOP(lambda x:model.operation('unprepare',2))
power_auth=OP(lambda x,on:1)
rail_set=AUTH(lambda x,n,on:model.operation('rail_on' if on else 'rail_off',n))
class PowerIo(C.Structure):
 _fields_=[('Domains',Io),('Clocks',ClockIo),('Authorized',OP),('RailSet',AUTH),
  ('ReferencePrepare',ZEROOP),('ReferenceEnable',ZEROOP),('ReferenceDisable',ZEROOP),('ReferenceUnprepare',ZEROOP)]
class PowerDevice(C.Structure):
 _fields_=[('Io',PowerIo),('Domains',Device),('Clocks',ClockDevice),('Initialized',C.c_uint32),
  ('Poisoned',C.c_uint32),('Stages',C.c_uint32),('References',C.c_uint32)]
lib.MaliMfgPowerInitialize.argtypes=[C.POINTER(PowerDevice),C.POINTER(PowerIo)]
lib.MaliMfgPowerReference.argtypes=[C.POINTER(PowerDevice),C.c_uint32]
clock_mode=False;power_mode=True;power_cases=[]
profiles=[(on,1-on,seed,bad,0) for on in (0,1) for seed in range(16) for bad in ((0,3) if on else (0,))]
profiles += [(1,0,0,0,f) for f in range(1,10)]+[(0,1,0,0,f) for f in range(1,4)]
profiles += [(1,refs,0,0,0) for refs in (1,2,7)]+[(0,refs,0,0,0) for refs in (2,3,7)]
for on,refs,seed,bad,fail in profiles:
 model=PowerModel(on,refs,seed,bad,fail)
 u.mem_write(bases[gpu_symbol[1]],bytes(0x5c))
 u.mem_write(bases[gpu_symbol[1]]+0x24,struct.pack('<I',refs))
 u.reg_write(UC_ARM64_REG_X0,on);u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,0x6000fff0)
 pv=symbols['__gpufreq_power_control'];u.emu_start(bases[pv[1]]+pv[2],stop,count=400000)
 expected=model
 assert expected.aborted or u.reg_read(UC_ARM64_REG_PC)==stop,'Power control exceeded bounded emulation'
 model=PowerModel(on,refs,seed,bad,fail);dev=PowerDevice()
 domain_io=Io(None,read,write,barrier,clock_delay,authority)
 clock_io=ClockIo(domain_io,prepare,enable,disable,unprepare,parent)
 pio=PowerIo(domain_io,clock_io,power_auth,rail_set,ref_prepare,ref_enable,ref_disable,ref_unprepare)
 assert lib.MaliMfgPowerInitialize(C.byref(dev),C.byref(pio))==0
 if refs:dev.References=refs;dev.Stages=1023;dev.Clocks.Stages=63
 result=lib.MaliMfgPowerReference(C.byref(dev),on)
 assert bool(result)==expected.aborted,(on,refs,seed,bad,fail,result,expected.aborted)
 assert model.trace==expected.trace,(on,refs,seed,bad,fail,model.trace,expected.trace)
 assert bool(dev.Poisoned)==expected.aborted
 if not result:
  expected_refs=refs+(1 if on else -1)
  assert dev.References==expected_refs
  assert u.reg_read(UC_ARM64_REG_X0)==expected_refs
 power_cases.append({'on':on,'initial_references':refs,'seed':seed,'initial_bad_mux_reads':bad,
  'provider_failure_operation':fail,'stock_abort_trapped':expected.aborted,'port_status':result,'trace_events':len(model.trace)})
power_result={'status':'passed','comparisons':len(power_cases),
 'function':{'section_offset':pv[2],'file_offset':secs[pv[1]][4]+pv[2],'bytes':pv[3]},
 'evidence_id':'ev_6160f2ba70b7dafea1a872b5dc2ab6afcc6e321e663b01b1dfa5b685a19ea077',
 'compared':'original complete power body calls the original domain and clock bodies; exact rail/reference/mux/BG3D operations, all MMIO writes/delays and reference count transitions',
 'intentional_difference':'caller serializes instead of Linux mutex. Port rejects reference underflow/overflow, bounds idle wait with delay, verifies DCM/ready reads and returns status instead of original abort; ownership retained on failure. Idle timeout/invalid-count cases are port-native tests, not original trace equivalence.',
 'actual_PMIC_PLL_or_device_executed':False,'cases':power_cases}
# The stock WACS2 read/write function bodies execute with their Linux poll and
# MMIO wrappers replaced. This compares actual command encoding/order and read
# response masking, not Linux timeout implementation or physical communication.
PWRAP=ROOT.parent/'analysis/stock-binaries/vendor_boot_a_0/mtk-pmic-wrap.ko'
pr,ps,psy,pt,pdata=read_module(PWRAP)
PSHA='f0a232edcf1b00ed991b61f719b09e94d1fff4f0b0a7a1508c656bbae3d37ee7'
assert hashlib.sha256(pr).hexdigest()==PSHA
_,mi,mo,mz=psy['pwrap_mt6789'];master=pdata(mi)[mo:mo+mz]
_,ri,ro,rz=psy['mt6789_regs'];regmap=pdata(ri)[ro:ro+rz]
assert struct.unpack_from('<I',master,0x30)[0]==0x8800
for offset,value in [(0x88,0x880),(0x8c,0x8a8),(0x90,0x8a4),(0x528,0x884),(0x52c,0x894)]:
 assert struct.unpack_from('<I',regmap,offset)[0]==value
textsec=psy['pwrap_read16'][1];assert psy['pwrap_write16'][1]==textsec
pu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);pb=0x24000000;pm=0x62000000;pstop=pb+0x100000
pu.mem_map(pb,((ps[textsec][5]+4095)//4096)*4096);pu.mem_write(pb,pdata(textsec))
pu.mem_map(pm,65536);pu.mem_write(pm+0x1000,master);pu.mem_write(pm+0x2000,regmap)
pu.mem_write(pm+8,struct.pack('<Q',0x54000000));pu.mem_write(pm+0x20,struct.pack('<Q',pm+0x1000))
pu.mem_write(pm+0x1000,struct.pack('<Q',pm+0x2000))
readpc=pb+next(v[2] for t in pt.values() for v in t if v[0]=='readl' and v[1]==textsec and v[2]==0x160c)
writepc=pb+next(v[2] for t in pt.values() for v in t if v[0]=='writel' and v[1]==textsec and v[2]==0x35ec)
pollpc=pb+psy['pwrap_wait_for_state'][2]
class PmicModel:
 def __init__(self,address,value):self.address=address;self.value=value;self.state=0;self.wdata=0;self.trace=[];self.polls=0
 def read(self,o):
  if o==0x8a8:return 0xa5000000|(self.state<<1)
  assert o==0x894;return 0xbeef0000|self.value
 def write(self,o,v):
  self.trace.append((o,v))
  if o==0x884:self.wdata=v
  elif o==0x8a4:assert v==1 and self.state==6;self.state=0
  else:
   assert o==0x880 and (v&65535)==self.address
   if v&0x20000000:self.value=self.wdata;self.state=0
   else:self.state=6
pm_model=None
p_ranges=[(pb+psy[n][2],pb+psy[n][2]+psy[n][3]) for n in ['pwrap_read16','pwrap_write16']]
def pm_code(uc,pc,size,_):
 if pc==readpc:
  uc.reg_write(UC_ARM64_REG_X0,pm_model.read(uc.reg_read(UC_ARM64_REG_X0)-0x54000000))
 elif pc==writepc:pm_model.write(uc.reg_read(UC_ARM64_REG_X1)-0x54000000,uc.reg_read(UC_ARM64_REG_X0))
 elif pc==pollpc:
  want=0 if not pm_model.polls else 6;pm_model.polls+=1;assert pm_model.state==want
  uc.reg_write(UC_ARM64_REG_X0,0)
 else:assert any(lo<=pc<hi for lo,hi in p_ranges),hex(pc);return
 uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
pu.hook_add(UC_HOOK_CODE,pm_code)
PREAD=C.CFUNCTYPE(C.c_uint32,C.c_void_p,C.c_uint32)
PWRITE=C.CFUNCTYPE(None,C.c_void_p,C.c_uint32,C.c_uint32)
PREAD16=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32,C.POINTER(C.c_uint16))
PWRITE16=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint32,C.c_uint16)
NULL_PREAD16=C.cast(C.c_void_p(),PREAD16)
NULL_PWRITE16=C.cast(C.c_void_p(),PWRITE16)
PAUTH=C.CFUNCTYPE(C.c_int,C.c_void_p)
pm_read=PREAD(lambda x,o:pm_model.read(o));pm_write=PWRITE(lambda x,o,v:pm_model.write(o,v))
pm_auth=PAUTH(lambda x:1);pm_delay=DELAY(lambda x,v:None)
class PmicIo(C.Structure):
 _fields_=[('Context',C.c_void_p),('Read',PREAD),('Write',PWRITE),('Barrier',VOID),('Delay',DELAY),('Authorized',PAUTH),('BrokerRead16',PREAD16),('BrokerWrite16',PWRITE16)]
assert C.sizeof(PmicIo)==64
class PmicDevice(C.Structure):_fields_=[('Io',PmicIo),('Initialized',C.c_uint32),('Poisoned',C.c_uint32)]
lib.MaliMfgPmicInitialize.argtypes=[C.POINTER(PmicDevice),C.POINTER(PmicIo)]
lib.MaliMfgPmicRead16.argtypes=[C.POINTER(PmicDevice),C.c_uint32,C.POINTER(C.c_uint16)]
lib.MaliMfgPmicWrite16.argtypes=[C.POINTER(PmicDevice),C.c_uint32,C.c_uint32]
pmic_cases=[]
for address in [0,2,0x1508,0x1526,0x1bc8,0x1be2,0x1bf4,65534]:
 for seed in range(16):
  for write_mode in (0,1):
   value=(seed*0x9e37)&65535;new=(65535-value)&65535
   pm_model=PmicModel(address,value);name='pwrap_write16' if write_mode else 'pwrap_read16';v=psy[name]
   pu.reg_write(UC_ARM64_REG_X0,pm);pu.reg_write(UC_ARM64_REG_X1,address)
   pu.reg_write(UC_ARM64_REG_X2,new if write_mode else pm+0x3000)
   pu.reg_write(UC_ARM64_REG_LR,pstop);pu.reg_write(UC_ARM64_REG_SP,pm+0xfff0)
   pu.emu_start(pb+v[2],pstop,count=10000)
   assert pu.reg_read(UC_ARM64_REG_PC)==pstop and pu.reg_read(UC_ARM64_REG_X0)==0
   if not write_mode:assert struct.unpack('<I',pu.mem_read(pm+0x3000,4))[0]==value
   expected=pm_model;pm_model=PmicModel(address,value);pd=PmicDevice()
   pio=PmicIo(None,pm_read,pm_write,barrier,pm_delay,pm_auth,NULL_PREAD16,NULL_PWRITE16)
   assert lib.MaliMfgPmicInitialize(C.byref(pd),C.byref(pio))==0
   out=C.c_uint16(0xa55a)
   result=lib.MaliMfgPmicWrite16(C.byref(pd),address,new) if write_mode else lib.MaliMfgPmicRead16(C.byref(pd),address,C.byref(out))
   assert result==0 and not pd.Poisoned and pm_model.state==0
   assert pm_model.trace==expected.trace,(address,seed,write_mode,pm_model.trace,expected.trace)
   assert pm_model.value==expected.value
   if not write_mode:assert out.value==value
   pmic_cases.append({'address':address,'initial_value':value,'write':bool(write_mode),'write_events':len(pm_model.trace)})
# Authenticate regulator offsets against the shipped table and exact board DT.
REG=ROOT.parent/'analysis/stock-binaries/vendor_boot_a_0/mt6358-regulator.ko'
rr,rs,rsy,rt,rdata=read_module(REG)
assert hashlib.sha256(rr).hexdigest()=='393eadc1ce78ebe87fb4bcf9b0c041ccb46d78cb0a04ed86e7506b0cd4a78b31'
rail_data=[]
for name_offset,expected in [(0xa98,(0x1508,0x1520,0x1526,0x1526,0)),(0x29f0,(0x1bc8,0x1be4,0x1bf4,0x1be2,8))]:
 b=rdata(1);fields=tuple(struct.unpack_from('<I',b,name_offset+x)[0] for x in (0xc0,0x140,0xa4,0x14c,0x154))
 assert fields==expected
 for x,mask in [(0xc4,1),(0x144,1),(0xa8,127),(0x150,127)]:assert struct.unpack_from('<I',b,name_offset+x)[0]==mask
 rail_data.append({'descriptor_name_offset':name_offset,'enable':fields[0],'status':fields[1],'selector':fields[2],'da_selector':fields[3],'da_shift':fields[4]})
import runpy
fdt=runpy.run_path(str(ROOT/'Platforms/Teclast/t70proPkg/Linux/platform-modules.py'))
dtb=ROOT.parent/'analysis/stock-fdt/effective.dtb';nodes,_=fdt['tree'](dtb.read_bytes())
for node,delay_us in [('buck-vgpu',200),('ldo-vsram-gpu',240)]:
 props=nodes['/soc/pwrap@10026000/mt6366/mt6358regulator/'+node]['properties']
 assert struct.unpack('>I',props['regulator-enable-ramp-delay'])[0]==delay_us
 assert struct.unpack('>I',props['regulator-min-microvolt'])[0]==500000
 assert struct.unpack('>I',props['regulator-max-microvolt'])[0]==1293750
pmic_result={'status':'passed','comparisons':len(pmic_cases),'stock_module_sha256':PSHA,
 'regulator_module_sha256':hashlib.sha256(rr).hexdigest(),'dtb_sha256':hashlib.sha256(dtb.read_bytes()).hexdigest(),
 'stock_capabilities':0x8800,'rail_descriptors':rail_data,
 'functions':{n:{'section_offset':psy[n][2],'file_offset':ps[psy[n][1]][4]+psy[n][2],'bytes':psy[n][3]} for n in ['pwrap_read16','pwrap_write16']},
 'compared':'original WACS2 read16/write16 bodies; exact command/data/valid-clear MMIO write sequence and masked read value',
 'intentional_difference':'port checks even 16-bit address/data, gates a global bus lease, pre-clears a stale response, waits for write completion and post-read idle, commits output only after acknowledgement, and poisons on bounded timeout. Failure semantics are native tests, not original trace equivalence.',
 'linux_poll_and_MMIO_wrappers_are_virtual':True,'real_PMIC_or_device_executed':False,'cases':pmic_cases}
# Original gpufreq pair transitions versus the actual PWRAP-backed C core.
# Default OPP bytes are test vectors only, never an authorized board OPP table.
class RailIo(C.Structure):
 _fields_=[('Pmic',C.POINTER(PmicDevice)),('Context',C.c_void_p),('Authorized',AUTH)]
class Rails(C.Structure):
 _fields_=[('Io',RailIo),('Initialized',C.c_uint32),('Poisoned',C.c_uint32),('Acquired',C.c_uint32),('Owned',C.c_uint32),('Microvolts',C.c_uint32*2),('OriginalMicrovolts',C.c_uint32*2)]
class VoltageIo(C.Structure):_fields_=[('Context',C.c_void_p),('Authorized',AUTH)]
lib.MaliMfgRailsInitialize.argtypes=[C.POINTER(Rails),C.POINTER(RailIo)]
lib.MaliMfgRailSet.argtypes=[C.POINTER(Rails),C.c_uint32,C.c_uint32]
lib.MaliMfgRailsVoltage.argtypes=[C.POINTER(Rails),C.c_uint32,C.c_uint32,C.POINTER(VoltageIo)]
class VoltageModel:
 def __init__(self,gpu,sram):
  self.reg={0x1508:1,0x1520:1,0x1bc8:1,0x1be4:1,
   0x1526:0x9b80|((gpu-500000)//6250),0x1bf4:0xb780|((sram-500000)//6250),
   0x1be2:0x805a|(((sram-500000)//6250)<<8)}
  self.voltages=[gpu,sram];self.trace=[];self.delays=[];self.state=0;self.wdata=0;self.response=0
 def select(self,n,uv):
  assert 500000<=uv<=1293750 and (uv-500000)%6250==0
  self.trace.append((n,uv));self.voltages[n]=uv;a=[0x1526,0x1bf4][n]
  self.reg[a]=(self.reg[a]&~127)|((uv-500000)//6250)
  if n==1:self.reg[0x1be2]=(self.reg[0x1be2]&~0x7f00)|(((uv-500000)//6250)<<8)
 def read(self,o):
  if o==0x8a8:return self.state<<1
  assert o==0x894;return self.response
 def write(self,o,v):
  if o==0x884:self.wdata=v;return
  if o==0x8a4:assert v==1 and self.state==6;self.state=0;return
  assert o==0x880 and self.state==0;a=v&65535
  assert a in self.reg
  if v&0x20000000:
   assert a in [0x1526,0x1bf4] and self.wdata&~127==self.reg[a]&~127
   self.select([0x1526,0x1bf4].index(a),500000+6250*(self.wdata&127))
  else:self.response=self.reg[a];self.state=6
vr=PREAD(lambda x,o:voltage_model.read(o));vw=PWRITE(lambda x,o,v:voltage_model.write(o,v))
vd=DELAY(lambda x,us:voltage_model.delays.append(us))
va=AUTH(lambda x,g,rs:rs>=g and rs-g<=200000)
_,oi,oo,oz=symbols['g_default_gpu'];assert oz==45*24
opp_bytes=data(oi)[oo:oo+oz];opps=[struct.unpack_from('<6I',opp_bytes,i) for i in range(0,oz,24)]
voltage_comparisons=0
for a in opps:
 for b in opps:
  oldgpu,oldsram=a[1]*10,a[2]*10;gpu,sram=b[1]*10,b[2]*10
  midgpu,midsram=(oldgpu,sram) if gpu>oldgpu else (gpu,oldsram)
  if midsram-midgpu>200000:continue
  voltage_mode=True;voltage_model=VoltageModel(oldgpu,oldsram)
  for reg,val in [(UC_ARM64_REG_X0,a[1]),(UC_ARM64_REG_X1,b[1]),(UC_ARM64_REG_X2,a[2])]:u.reg_write(reg,val)
  u.reg_write(UC_ARM64_REG_X3,b[2]);u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,0x6000fff0)
  u.emu_start(bases[section]+symbols['__gpufreq_volt_scale_gpu'][2],stop,count=10000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop and u.reg_read(UC_ARM64_REG_X0)==0
  expected=voltage_model;voltage_mode=False;voltage_model=VoltageModel(oldgpu,oldsram)
  bus=PmicDevice();rails=Rails();pio=PmicIo(None,vr,vw,barrier,vd,pm_auth,NULL_PREAD16,NULL_PWRITE16)
  assert lib.MaliMfgPmicInitialize(C.byref(bus),C.byref(pio))==0
  rio=RailIo(C.pointer(bus),None,authority);vio=VoltageIo(None,va)
  assert lib.MaliMfgRailsInitialize(C.byref(rails),C.byref(rio))==0
  for n in [0,1]:assert lib.MaliMfgRailSet(C.byref(rails),n,1)==0
  assert lib.MaliMfgRailsVoltage(C.byref(rails),gpu,sram,C.byref(vio))==0
  assert voltage_model.trace==expected.trace and voltage_model.reg==expected.reg
  assert voltage_model.delays==[x for x in expected.delays if x],(a[:3],b[:3],voltage_model.delays,expected.delays)
  assert list(rails.Microvolts)==[gpu,sram] and rails.Acquired==3 and not rails.Owned and not rails.Poisoned
  voltage_comparisons+=1
voltage_result={'status':'passed','comparisons':voltage_comparisons,'stock_module_sha256':SHA,
 'functions':{n:{'section_offset':symbols[n][2],'file_offset':secs[symbols[n][1]][4]+symbols[n][2],'bytes':symbols[n][3]} for n in ['__gpufreq_volt_scale_gpu']},
 'default_table':{'sha256':hashlib.sha256(opp_bytes).hexdigest(),'count':45,'eligible_device_OPPs':False},
 'compared':'original gpufreq voltage pair body versus real C pair/PMIC cores: regulator target order, microvolts, settle time, final selectors and preserved fields',
 'intentional_difference':'port validates bounds/step/alignment/monotonic direction and a board authorization for old, intermediate and final pairs, confirms both status/selector/DA values before committing saved voltage and retains acquired rails on failure. Zero delay is omitted. Original regulator/delay services are virtual; fixture 200mV pair gap is not a qualified board OPP policy.',
 'original_gpufreq_body_executed':True,'real_PMIC_or_device_executed':False}
# Execute the shipped clock-provider primitives too, rather than substituting
# clk_prepare/enable in the gpufreq comparison. Only MMIO/regmap, delay and
# telemetry services are virtual. Descriptor bytes are the original tables.
from unicorn.arm64_const import UC_ARM64_REG_SP_EL0
CLK=ROOT.parent/'analysis/stock-binaries/vendor_boot_a_0/clk-common.ko'
CHIP=ROOT.parent/'analysis/stock-binaries/vendor_boot_a_0/clk-mt6789.ko'
GATE=ROOT.parent/'analysis/stock-binaries/vendor_boot_a_0/clk-mt6789-mfgcfg.ko'
cr,cs,csy,ct,cdata=read_module(CLK);chipr,chips,chipsy,chipt,chipdata=read_module(CHIP)
gr,gs,gsy,gt,gdata=read_module(GATE)
for body,sha in [(cr,'11548b41f27584f0c2e348fb533546b2ff7974bcdf82129787784a697af3c981'),
 (chipr,'162ddd1cc3a88c6f884d153d04889071b8cf9574bf04a10684e5f72b04d2c32b'),
 (gr,'1ac893b2f2aef0b25cac1c1935ba7a874668495f23965057fcb673d6bfc71766')]:assert hashlib.sha256(body).hexdigest()==sha
pll_descriptor=chipdata(30)[0x918:0x918+192]
ref_descriptor=chipdata(30)[0x2a00:0x2a00+112]
mux_descriptor=chipdata(30)[0x2a70:0x2a70+112]
for offset,value in [(0x20,0x268),(0x24,0x274),(0x28,0x268),(0x2c,0),(0x68,24),(0x90,22),(0x94,8),(0x98,0x26c),(0x9c,0)]:assert struct.unpack_from('<I',pll_descriptor,offset)[0]==value
assert pll_descriptor[0x6e]==0 and pll_descriptor[0x6f]==4 and pll_descriptor[0xb8]==0
assert struct.unpack_from('<QQ',pll_descriptor,0x80)==(1500000000,3800000000)
assert ref_descriptor[0x50:0x54]==bytes([16,2,23,18]) and mux_descriptor[0x50:0x52]==bytes([18,1])
for desc in [ref_descriptor,mux_descriptor]:assert struct.unpack_from('<III',desc,0x24)==(0x50,0x54,0x58)
assert struct.unpack_from('<I',mux_descriptor,0x30)[0]==0
assert struct.unpack_from('<III',gdata(25),0x1a0)==(0,8,4)
assert struct.unpack_from('<I',gdata(25),0x1b0+0x28)[0]==0  # BG3D shift
# Authenticate the providers, MMIO and exact clock specifiers in the board DT.
for path,base in [('/syscon@10000000',0x10000000),('/syscon@1000C000',0x1000c000),('/syscon@13fbf000',0x13fbf000)]:
 assert struct.unpack('>QQ',nodes[path]['properties']['reg'])==(base,4096)
assert struct.unpack('>12I',nodes['/soc/gpufreq']['properties']['clocks'])==(0x2a,10,0x2a,9,0x2a,74,0x2a,145,0x85,0,0x3b,9)
cnames=['mtk_pll_prepare','mtk_pll_unprepare','mtk_pll_recalc_rate','mtk_pll_set_rate','mtk_clk_mux_set_parent_setclr_lock',
 '__mtk_clk_mux_set_parent_lock','mtk_clk_mux_enable_setclr','mtk_clk_mux_disable_setclr','mtk_cg_enable','mtk_cg_disable']
cbases={};cursor=0x30000000
for i,q in enumerate(cs):
 if q[2]&2 and q[5]:cbases[i]=cursor;cursor+=(q[5]+4095)&~4095
cu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);cu.mem_map(0x30000000,cursor-0x30000000)
for i,base in cbases.items():
 if cs[i][1]!=8:cu.mem_write(base,cdata(i))
cranges=[(csy[n][1],csy[n][2],csy[n][2]+csy[n][3]) for n in cnames]
# Even unreachable optional paths are relocated, but execution outside the
# authenticated primitives/services aborts this test instead of pretending success.
cstub=(cursor+65535)&~4095;creloc=[];cexternal=set()
for i,q in enumerate(cs):
 if q[1]!=4:continue
 for k in range(0,q[5],24):
  off,info,add=struct.unpack_from('<QQq',cdata(i),k)
  if not any(si==q[7] and lo<=off<hi for si,lo,hi in cranges):continue
  name,si,value,sz=ct[q[6]][info>>32];kind=info&0xffffffff
  creloc.append((q[7],off,kind,name,si,value,add))
  if not si or name=='mtk_clk_notify':cexternal.add(name)
cstubs={n:cstub+i*16 for i,n in enumerate(sorted(cexternal))};cstop=cstub+4096
cu.mem_map(cstub,8192)
for addr in cstubs.values():cu.mem_write(addr,bytes.fromhex('00008052c0035fd6'))
for si,off,kind,name,targetsi,value,add in creloc:
 target=(cstubs[name] if name in cstubs else cbases[targetsi]+value)+add;place=cbases[si]+off
 word=struct.unpack('<I',cu.mem_read(place,4))[0]
 if kind==275:
  delta=(target>>12)-(place>>12);assert -(1<<20)<=delta<(1<<20)
  word=(word&~((3<<29)|(0x7ffff<<5)))|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
 elif kind==277:word=(word&~(4095<<10))|((target&4095)<<10)
 elif kind in (278,284,285,286):
  shift={278:0,284:1,285:2,286:3}[kind];assert not target&((1<<shift)-1)
  word=(word&~(4095<<10))|(((target&4095)>>shift)<<10)
 elif kind in (282,283):
  delta=target-place;assert not delta&3 and -(1<<27)<=delta<(1<<27);word=(word&0xfc000000)|((delta>>2)&0x3ffffff)
 else:raise AssertionError(('clock relocation',kind))
 cu.mem_write(place,struct.pack('<I',word))
cmem=0x65000000;cu.mem_map(cmem,65536);cu.reg_write(UC_ARM64_REG_SP_EL0,cmem+0xe000)
cu.mem_write(cmem+0xe710,struct.pack('<Q',0x73616665636c6b31))
pllctx=cmem;refctx=cmem+0x100;muxctx=cmem+0x200;gatectx=cmem+0x300
for off,desc in [(0x1000,pll_descriptor),(0x1100,ref_descriptor),(0x1200,mux_descriptor)]:cu.mem_write(cmem+off,desc)
clockmmio={2:0x56000000,3:0x56010000,4:0x56020000}
for offset,value in [(0x18,0x268),(0x20,0x26c),(0x28,0x274),(0x40,0x26c),(0x48,0x26c),(0x50,0x268)]:cu.mem_write(pllctx+offset,struct.pack('<Q',clockmmio[4]+value))
cu.mem_write(pllctx+0x88,struct.pack('<Q',cmem+0x1000))
for ctx,desc in [(refctx,0x1100),(muxctx,0x1200)]:
 cu.mem_write(ctx+0x18,struct.pack('<Q',3));cu.mem_write(ctx+0x28,struct.pack('<Q',cmem+desc))
cu.mem_write(gatectx+0x20,struct.pack('<Q',2));cu.mem_write(gatectx+0x30,struct.pack('<II',4,8))
readclock=cbases[20]+0x3428;writeclock=cbases[20]+0x34a8
assert any(v[0]=='readl' and v[1]==20 and v[2]==0x3428 for t in ct.values() for v in t)
assert any(v[0]=='writel' and v[1]==20 and v[2]==0x34a8 for t in ct.values() for v in t)
clock_model=None
class HardwareClockModel:
 def __init__(self,seed,ref,div):
  v=(seed*0x9e3779b1)&0xffffffff
  self.reg={(2,0):v|1,(3,0x50):(v&~0x870000)|(ref<<16)|0x800000,
   (4,0x268):v&~1,(4,0x274):(v&~3)|2,(4,0x26c):(v&~0x073fffff)|(div<<24)|1260307,
   (4,0x340):v|0xff800001,(4,0x34c):(v&~3)|1};self.trace=[];self.delays=[]
 def read(self,r,o):return self.reg.get((r,o),0)
 def write(self,r,o,v):
  self.trace.append((r,o,v))
  if r==2:assert o in [4,8] and v==1;self.reg[2,0]=(self.reg[2,0]|v) if o==4 else (self.reg[2,0]&~v)
  elif r==3:assert o in [0x54,0x58];self.reg[3,0x50]=(self.reg[3,0x50]|v) if o==0x54 else (self.reg[3,0x50]&~v)
  else:assert r==4 and o in [0x268,0x274,0x26c];self.reg[r,o]=v
cstubnames={v:k for k,v in cstubs.items()}
def clock_code(uc,pc,size,_):
 x0=uc.reg_read(UC_ARM64_REG_X0);x1=uc.reg_read(UC_ARM64_REG_X1);x2=uc.reg_read(UC_ARM64_REG_X2)
 if pc==readclock:uc.reg_write(UC_ARM64_REG_X0,clock_model.read(4,x0-clockmmio[4]))
 elif pc==writeclock:clock_model.write(4,x1-clockmmio[4],x0&0xffffffff)
 elif pc in cstubnames:
  name=cstubnames[pc]
  if name=='regmap_read':
   assert x0 in [2,3];uc.mem_write(x2,struct.pack('<I',clock_model.read(x0,x1)));uc.reg_write(UC_ARM64_REG_X0,0)
  elif name=='regmap_write':assert x0 in [2,3];clock_model.write(x0,x1,x2&0xffffffff);uc.reg_write(UC_ARM64_REG_X0,0)
  elif name=='__const_udelay':assert x0 in [4295,85900];clock_model.delays.append({4295:1,85900:20}[x0])
  elif name in ['clk_hw_get_name','mtk_clk_notify']:uc.reg_write(UC_ARM64_REG_X0,0)
  else:raise AssertionError(('unexpected optional clock service',name))
 else:
  assert any(cbases[si]+lo<=pc<cbases[si]+hi for si,lo,hi in cranges),hex(pc);return
 uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
cu.hook_add(UC_HOOK_CODE,clock_code)
def clock_call(name,ctx,index=0):
 cu.reg_write(UC_ARM64_REG_X0,ctx);cu.reg_write(UC_ARM64_REG_X1,index);cu.reg_write(UC_ARM64_REG_X2,26000000)
 cu.reg_write(UC_ARM64_REG_LR,cstop);cu.reg_write(UC_ARM64_REG_SP,cmem+0xfff0)
 cu.emu_start(cbases[csy[name][1]]+csy[name][2],cstop,count=10000)
 assert cu.reg_read(UC_ARM64_REG_PC)==cstop
 return cu.reg_read(UC_ARM64_REG_X0)
hr=READ(lambda x,r,o:clock_model.read(r,o));hw=WRITE(lambda x,r,o,v:clock_model.write(r,o,v))
hdelay=DELAY(lambda x,n:clock_model.delays.append(n));hauth=AUTH(lambda x,n,on:1)
class HardwareClock(C.Structure):
 _fields_=[('Io',Io),('Initialized',C.c_uint32),('Poisoned',C.c_uint32),('Held',C.c_uint32),('PllSetting',C.c_uint32),('RefSetting',C.c_uint32)]
lib.MaliMfgHwClockInitialize.argtypes=[C.POINTER(HardwareClock),C.POINTER(Io)]
lib.MaliMfgHwClockSet.argtypes=[C.POINTER(HardwareClock),C.c_uint32,C.c_uint32]
lib.MaliMfgHwClockParent.argtypes=[C.POINTER(HardwareClock),C.c_uint32]
lib.MaliMfgHwClockReady.argtypes=[C.POINTER(HardwareClock)]
lib.MaliMfgPllDecodeRate.argtypes=[C.c_uint32,C.POINTER(C.c_uint64)]
hardware_cases=[]
for seed in range(16):
 for ref in range(4):
  for div in range(5):
   clock_model=HardwareClockModel(seed,ref,div)
   for name,ctx,index in [('mtk_clk_mux_enable_setclr',refctx,0),('mtk_pll_prepare',pllctx,0),
    ('mtk_clk_mux_set_parent_setclr_lock',muxctx,1),('mtk_cg_enable',gatectx,0),('mtk_cg_disable',gatectx,0),
    ('mtk_clk_mux_set_parent_setclr_lock',muxctx,0),('mtk_pll_unprepare',pllctx,0),('mtk_clk_mux_disable_setclr',refctx,0)]:clock_call(name,ctx,index)
   expected=clock_model;clock_model=HardwareClockModel(seed,ref,div);hd=HardwareClock();hio=Io(None,hr,hw,barrier,hdelay,hauth)
   assert lib.MaliMfgHwClockInitialize(C.byref(hd),C.byref(hio))==0
   for clock,on in [(0,1)]:assert lib.MaliMfgHwClockSet(C.byref(hd),clock,on)==0
   assert lib.MaliMfgHwClockParent(C.byref(hd),0)==0 and lib.MaliMfgHwClockSet(C.byref(hd),2,1)==0
   assert lib.MaliMfgHwClockReady(C.byref(hd))==0 and hd.Held==7
   assert lib.MaliMfgHwClockSet(C.byref(hd),2,0)==0 and lib.MaliMfgHwClockParent(C.byref(hd),1)==0
   assert lib.MaliMfgHwClockSet(C.byref(hd),0,0)==0 and not hd.Held and not hd.Poisoned
   assert clock_model.trace==expected.trace,(seed,ref,div,clock_model.trace,expected.trace)
   assert clock_model.delays==expected.delays==[1,1,20] and clock_model.reg==expected.reg
   hardware_cases.append({'seed':seed,'reference_parent':ref,'postdivider':div,'write_events':len(clock_model.trace)})
rate_comparisons=0
for p in range(128):
 for div in range(8):
  setting=0xf8c00000|(div<<24)|(p*33026);clock_model.reg[4,0x26c]=setting
  original=clock_call('mtk_pll_recalc_rate',pllctx,26000000);rate=C.c_uint64()
  assert lib.MaliMfgPllDecodeRate(setting,C.byref(rate))==0 and rate.value==original
  rate_comparisons+=1
RATEAUTH=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint64,C.c_uint64)
ra=RATEAUTH(lambda x,old,wanted,actual:1)
class RateIo(C.Structure):_fields_=[('Context',C.c_void_p),('Authorized',RATEAUTH)]
lib.MaliMfgPllEncodeRate.argtypes=[C.c_uint64,C.POINTER(C.c_uint32),C.POINTER(C.c_uint64)]
lib.MaliMfgHwClockProgramRate.argtypes=[C.POINTER(HardwareClock),C.c_uint64,C.POINTER(RateIo)]
program_comparisons=0
requests=[a[0]*1000 for a in opps]+[93750000,187500000,375000000,750000000,1500000000,2600000000,3799999000]
requests += [93750000+(i*0x9e3779b1)%(3799999000-93750000) for i in range(256)]
for i,request in enumerate(requests):
 clock_model=HardwareClockModel(i%16,0,1);clock_model.reg[3,0x50]&=~0x800000
 assert clock_call('mtk_pll_set_rate',pllctx,request)==0
 expected=clock_model;setting=C.c_uint32();actual=C.c_uint64()
 assert lib.MaliMfgPllEncodeRate(request,C.byref(setting),C.byref(actual))==0
 assert setting.value==expected.reg[4,0x26c]&0x073fffff
 decoded=C.c_uint64();lib.MaliMfgPllDecodeRate(setting.value,C.byref(decoded));assert decoded.value==actual.value
 clock_model=HardwareClockModel(i%16,0,1);hd=HardwareClock();hio=Io(None,hr,hw,barrier,hdelay,hauth)
 assert lib.MaliMfgHwClockInitialize(C.byref(hd),C.byref(hio))==0
 assert lib.MaliMfgHwClockSet(C.byref(hd),0,1)==0
 clock_model.trace=[];clock_model.delays=[];rio=RateIo(None,ra)
 assert lib.MaliMfgHwClockProgramRate(C.byref(hd),request,C.byref(rio))==0
 assert clock_model.trace==expected.trace,(request,clock_model.trace,expected.trace)
 assert clock_model.delays==expected.delays==[20] and clock_model.reg==expected.reg
 assert hd.Held==1 and not hd.Poisoned and hd.PllSetting==setting.value
 program_comparisons+=1
# Original adjusted-table springboards and commit planning. Only the 256-byte
# pure probe block executes; no allocation/probe/init or live voltage/rate work.
# Generic commit calls to voltage/frequency bodies are intercepted and recorded;
# their real PMIC/clock primitives are independently compared above.
class Opp(C.Structure):_fields_=[('Hz',C.c_uint64),('Gpu',C.c_uint32),('Sram',C.c_uint32)]
class OppPolicy(C.Structure):
 _fields_=[('Signature',C.c_uint32),('Count',C.c_uint32),('Entries',Opp*64),('ActualHz',C.c_uint64*64),('Up',C.c_uint32*64),('Down',C.c_uint32*64)]
class OppStep(C.Structure):_fields_=[('Kind',C.c_uint32),('Index',C.c_uint32),('Hz',C.c_uint64),('Gpu',C.c_uint32),('Sram',C.c_uint32)]
class OppPlan(C.Structure):_fields_=[('Count',C.c_uint32),('Initial',C.c_uint32),('Target',C.c_uint32),('Reserved',C.c_uint32),('Steps',OppStep*65)]
assert C.sizeof(Opp)==16 and C.sizeof(OppPolicy)==2056 and C.sizeof(OppPlan)==1576
lib.MaliMfgOppBuild.argtypes=[C.POINTER(OppPolicy),C.POINTER(Opp),C.c_uint32]
lib.MaliMfgOppPlan.argtypes=[C.POINTER(OppPolicy),C.c_uint32,C.c_uint32,C.POINTER(OppPlan)]
fixture=0x54000000;u.mem_map(fixture,65536);spring=fixture+0x2000;thread=fixture+0x5000
u.reg_write(UC_ARM64_REG_SP_EL0,thread);u.mem_write(thread+0x710,struct.pack('<Q',0x6f70702d66697874))
gpu_base=bases[gpu_symbol[1]];commit_pc=bases[section]+symbols['__gpufreq_generic_commit_gpu'][2]
def stock_commit(table,initial,target,fail=0):
 global commit_mode,commit_trace,commit_fail
 u.mem_write(gpu_base+0x44,struct.pack('<4I',initial,*table[initial][:3]))
 u.mem_write(gpu_base+0x58,struct.pack('<I',0));u.mem_write(gpu_base+0x68,struct.pack('<I',0))
 commit_trace=[];commit_fail=fail;commit_mode=True
 try:
  for reg,value in [(UC_ARM64_REG_X0,target),(UC_ARM64_REG_X1,0),(UC_ARM64_REG_LR,stop),(UC_ARM64_REG_SP,0x6000fff0)]:u.reg_write(reg,value)
  u.emu_start(commit_pc,stop,count=10000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop,'Unbounded original OPP commit'
  result=C.c_int32(u.reg_read(UC_ARM64_REG_X0)).value
  state=struct.unpack('<4I',u.mem_read(gpu_base+0x44,16))
  assert result==(-5 if fail else 0) and state[0]==(initial if fail else target)
  if not fail:assert state[1:]==table[target][:3]
  return list(commit_trace)
 finally:commit_mode=False;commit_fail=0
synthetic=[(1100000,100000,100000),(1040000,95000,95000),(980000,90000,90000),
 (920000,85000,90000),(860000,80000,90000),(800000,75000,87500),(700000,70000,85000),
 (600000,67500,82500),(500000,65000,80000),(400000,62500,77500),(300000,62500,77500)]
fixtures=[('compiled_default_test_vectors',opps),('single_point',opps[:1]),('tail_pair',opps[-2:]),
 ('middle_segment',opps[5:22]),('synthetic_multiple_springboards',[(*v,0,0,0) for v in synthetic]),
 ('uniform_test_voltage_adjustment',[(v[0],v[1]-625,v[2]-625,*v[3:]) for v in opps])]
spring_points=plan_comparisons=failure_prefixes=max_steps=multi_step=no_op=voltage_plateau=0;fixture_results=[]
for name,table in fixtures:
 n=len(table);contents=b''.join(struct.pack('<6I',*v) for v in table)
 u.mem_write(fixture,contents);u.mem_write(spring-8,b'\xa5'*(n*8+16))
 u.mem_write(gpu_base+8,struct.pack('<QQ',fixture,spring))
 u.mem_write(gpu_base+0x38,struct.pack('<I',n));u.mem_write(gpu_base+0x40,struct.pack('<I',n-1))
 for reg,value in [(UC_ARM64_REG_X0,spring),(UC_ARM64_REG_X19,n*8),(UC_ARM64_REG_SP,0x6000fff0)]:u.reg_write(reg,value)
 u.emu_start(bases[section]+spring_range[1],bases[section]+spring_range[2],count=100000)
 assert u.reg_read(UC_ARM64_REG_PC)==bases[section]+spring_range[2],'Unbounded original springboards'
 assert bytes(u.mem_read(fixture,len(contents)))==contents
 assert bytes(u.mem_read(spring-8,8))==bytes(u.mem_read(spring+n*8,8))==b'\xa5'*8
 original=[struct.unpack('<II',u.mem_read(spring+i*8,8)) for i in range(n)]
 entries=(Opp*n)(*(Opp(v[0]*1000,v[1]*10,v[2]*10) for v in table));policy=OppPolicy()
 assert lib.MaliMfgOppBuild(C.byref(policy),entries,n)==0,(name,'fixture rejected')
 assert original==[(policy.Up[i],policy.Down[i]) for i in range(n)],name
 for a in range(n):
  for b in range(n):
   plan=OppPlan();assert lib.MaliMfgOppPlan(C.byref(policy),a,b,C.byref(plan))==0
   assert 1<=plan.Count<=65 and plan.Initial==a and plan.Target==b
   trace=[];hz,gpu,sram=table[a][0]*1000,table[a][1]*10,table[a][2]*10
   for step in plan.Steps[:plan.Count]:
    if step.Kind==1:
     trace.append(('voltage',gpu,step.Gpu,sram,step.Sram));gpu,sram=step.Gpu,step.Sram
    else:
     assert step.Kind==2;trace.append(('rate',hz,step.Hz));hz=step.Hz
   expected=stock_commit(table,a,b);assert trace==expected,(name,a,b,trace,expected)
   plan_comparisons+=1;max_steps=max(max_steps,plan.Count);multi_step+=plan.Count>2;no_op+=a==b
   voltage_plateau+=a!=b and table[a][1:3]==table[b][1:3]
   if name=='synthetic_multiple_springboards' and (a,b) in [(0,n-1),(n-1,0),(n-1,n-1)]:
    for failed in range(1,len(trace)+1):
     assert stock_commit(table,a,b,failed)==trace[:failed];failure_prefixes+=1
 spring_points+=n;fixture_results.append({'name':name,'points':n,'table_sha256':hashlib.sha256(contents).hexdigest(),'plans':n*n})
opp_result={'status':'passed','springboard_tables':len(fixtures),'springboard_points':spring_points,
 'plan_comparisons':plan_comparisons,'failure_stop_prefixes':failure_prefixes,'max_transition_steps':max_steps,
 'multi_step_transitions':multi_step,'same_index_transitions':no_op,'equal_voltage_frequency_transitions':voltage_plateau,
 'stock_module_sha256':SHA,'compiled_default_table_sha256':hashlib.sha256(opp_bytes).hexdigest(),
 'springboard_block':{'section_offset':spring_range[1],'file_offset':secs[section][4]+spring_range[1],
  'bytes':spring_range[2]-spring_range[1],'sha256':hashlib.sha256(data(section)[spring_range[1]:spring_range[2]]).hexdigest()},
 'commit_function':{'section_offset':symbols['__gpufreq_generic_commit_gpu'][2],'bytes':symbols['__gpufreq_generic_commit_gpu'][3]},
 'compared':'exact original 200mV unsigned-delta springboard block and generic commit old/new voltage and frequency call order, clamp and equal-voltage plateaus, stable index only on success; errors stop at the failing service',
 'intentional_difference':'C rejects malformed/nonmonotonic/unreachable tables and duplicate rounded rates before output mutation; no stress mode, live FHCTL or implicit board authorization. The C executor uses the independently verified real PMIC pair and stopped-PLL cores and poisons partial transitions instead of retrying with stale indices.',
 'original_springboard_block_executed':True,'original_commit_body_executed':True,
 'stock_voltage_and_frequency_calls_mocked_for_plan_comparison':True,
 'device_qualified_OPP_table':False,'AVS_aging_segment_thermal_policy_implemented':False,
 'real_probe_init_clock_PMIC_device_executed':False,'fixtures':fixture_results}
hardware_result={'status':'passed','comparisons':len(hardware_cases),'rate_comparisons':rate_comparisons,'program_comparisons':program_comparisons,
 'common_module_sha256':hashlib.sha256(cr).hexdigest(),'chip_module_sha256':hashlib.sha256(chipr).hexdigest(),
 'gate_module_sha256':hashlib.sha256(gr).hexdigest(),'dtb_sha256':hashlib.sha256(dtb.read_bytes()).hexdigest(),
 'functions':{n:{'section_offset':csy[n][2],'file_offset':cs[csy[n][1]][4]+csy[n][2],'bytes':csy[n][3]} for n in cnames},
 'relocations_applied':len(creloc),'compared':'actual common PLL prepare/unprepare/recalc, reference SET/CLR gate, no-update parent mux, BG3D SET/CLR; full exact write/delay sequence, preserved registers and output rate; stopped PLL nearest-PCW/postdivider/change-strobe programming',
 'intentional_difference':'port requires serialized exclusive leases, validates frequency/ref-parent prerequisites and 1.5..3.8GHz VCO with postdivider<=4 before enabling; verifies final writes and retains ownership on failure. Rate programming requires GPU gate off, Sub mux, stopped/isolated MFGPLL and board approval of requested and rounded Hz; no live DVFS/FHCTL or reference selector programming. Optional clock-HWV/fence/tuner/resetbar paths are disabled for the exact MFG descriptors.',
 'mmio_regmap_delay_and_telemetry_services_are_virtual':True,'original_clock_provider_bodies_executed':True,'real_PLL_or_device_executed':False,'cases':hardware_cases}
# Pure shipped AVS/segment/aging blocks. All inputs below are synthetic; no
# per-device fuse, NVRAM, MMIO, nvmem, module probe/init or allocator is executed.
from unicorn.arm64_const import UC_ARM64_REG_X23,UC_ARM64_REG_X28
class Calibration(C.Structure):
 _fields_=[('Size',C.c_uint32),('Version',C.c_uint32),('Segment',C.c_uint32),
  ('Mcl50Load',C.c_uint32),('Aging',C.c_uint32),('PresentMask',C.c_uint32),('AvsWords',C.c_uint32*3)]
assert C.sizeof(Calibration)==36
lib.MaliMfgOppCalibrate.argtypes=[C.POINTER(OppPolicy),C.POINTER(Opp),C.c_uint32,C.POINTER(Calibration)]
aranges=[(0x3420,0x3474),(0x36a4,0x3d50),(0x3e68,0x3ea4)]
au=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
au.mem_map(0x20000000,max(base+((secs[si][5]+4095)&~4095) for si,base in bases.items())-0x20000000)
for si,base in bases.items():
 if secs[si][1]!=8:au.mem_write(base,data(si))
astub=(max(base+((secs[si][5]+4095)&~4095) for si,base in bases.items())+65535)&~65535
au.mem_map(astub,4096)
astubs={n:astub+i*16 for i,n in enumerate(('readl','mutex_lock','mutex_unlock','_printk'))}
for addr in astubs.values():au.mem_write(addr,bytes.fromhex('c0035fd6'))
afixture=0x59000000;au.mem_map(afixture,65536)
asigned=afixture;awork=afixture+0x1000;aefuse=afixture+0x2000;astack=afixture+0xfff0
arelocations=0
for si,q in enumerate(secs):
 if q[1]!=4 or q[7]!=section:continue
 for k in range(0,q[5],24):
  off,info,add=struct.unpack_from('<QQq',data(si),k)
  if not any(lo<=off<hi for lo,hi in aranges):continue
  name,targetsi,value,size=tabs[q[6]][info>>32];kind=info&0xffffffff
  assert targetsi or name in astubs,('AVS external not allowed',name)
  target=(astubs[name] if name in astubs else bases[targetsi]+value)+add;place=bases[section]+off
  word=struct.unpack('<I',au.mem_read(place,4))[0]
  if kind==275:
   delta=(target>>12)-(place>>12);assert -(1<<20)<=delta<(1<<20)
   word=(word&~((3<<29)|(0x7ffff<<5)))|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
  elif kind==277:word=(word&~(4095<<10))|((target&4095)<<10)
  elif kind in (278,284,285,286):
   shift={278:0,284:1,285:2,286:3}[kind];assert not target&((1<<shift)-1)
   word=(word&~(4095<<10))|(((target&4095)>>shift)<<10)
  elif kind in (282,283):
   delta=target-place;assert not delta&3 and -(1<<27)<=delta<(1<<27)
   word=(word&0xfc000000)|((delta>>2)&0x3ffffff)
  else:raise AssertionError(('AVS relocation not allowed',kind))
  au.mem_write(place,struct.pack('<I',word));arelocations+=1
agb=bases[symbols['g_avs_enable'][1]];av=symbols['g_avs_adj'];avaddr=bases[av[1]]+av[2]
ev=symbols['g_efuse_base'];efaddr=bases[ev[1]]+ev[2]
awords=[];areads=[];afailure=None;active_range=None
def avs_hook(uc,pc,size,unused):
 global afailure
 if pc in (astubs['readl'],bases[section]+symbols['readl'][2]):
  address=uc.reg_read(UC_ARM64_REG_X0);assert address in [aefuse+0x5c0+4*i for i in range(3)]
  index=(address-aefuse-0x5c0)//4;areads.append(index)
  uc.reg_write(UC_ARM64_REG_X0,awords[index]);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
 if pc in astubs.values():
  uc.reg_write(UC_ARM64_REG_X0,0);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
 if not (bases[section]+active_range[0]<=pc<bases[section]+active_range[1]):
  afailure=pc-bases[section];uc.emu_stop()
au.hook_add(UC_HOOK_CODE,avs_hook)
def avs_block(lo,hi):
 global active_range,afailure
 active_range=lo,hi;afailure=None;au.reg_write(UC_ARM64_REG_SP,astack)
 try:au.emu_start(bases[section]+lo,bases[section]+hi,count=30000)
 except Exception as error:raise AssertionError(('AVS emulation',hex(au.reg_read(UC_ARM64_REG_PC)-bases[section]),areads,active_range)) from error
 assert afailure is not None or au.reg_read(UC_ARM64_REG_PC)==bases[section]+hi,'Unbounded AVS calculation'
 return afailure is None
raw_base=(Opp*45)(*[Opp(f*1000,v*10,s*10) for f,v,s,*rest in opps])
calibration_cases=calibration_points=unsafe_tables=0
# None of these words is read from this tablet or retained in the report.
inputs=[(0,0,0),(144,112,108),(144,112,96),(255,255,255)]
inputs += [(96+(i*13)%65,90+(i*7)%40,88+(i*11)%35) for i in range(32)]
for vector,selectors in enumerate(inputs):
 words=[0 if not x else (mhz<<8)|x for mhz,x in zip((1100,700,390),selectors)]
 for segment in (1,2):
  for mcl in (0,1):
   for aging in (0,1):
    au.mem_write(agb,bytes(secs[symbols['g_avs_enable'][1]][5]))
    au.mem_write(efaddr,struct.pack('<Q',aefuse));au.mem_write(agb,struct.pack('<Q',asigned))
    au.mem_write(agb+0x28,struct.pack('<I',segment));au.mem_write(agb+0xdc,struct.pack('<I',mcl))
    au.mem_write(asigned,opp_bytes);au.mem_write(avaddr,data(av[1])[av[2]:av[2]+av[3]])
    awords=words;areads=[]
    assert avs_block(0x3420,0x3474);first=struct.unpack('<I',au.mem_read(agb+0x30,4))[0]
    assert first==(7 if segment==1 or mcl else 0)
    ok=avs_block(0x36a4,0x3d50);assert areads==[0,1,2] or not ok
    cal=Calibration(C.sizeof(Calibration),1,segment,mcl,aging,7,(C.c_uint32*3)(*words));result=OppPolicy()
    actual=lib.MaliMfgOppCalibrate(C.byref(result),raw_base,45,C.byref(cal))
    calibration_cases+=1
    if not ok:
     assert actual!=0 and bytes(result)==bytes(C.sizeof(result)),(vector,segment,mcl,aging,afailure,actual)
     unsafe_tables+=1;continue
    adjusted=bytes(au.mem_read(asigned,45*24))[first*24:]
    if aging:
     au.mem_write(awork,adjusted);au.reg_write(UC_ARM64_REG_X23,45-first);au.reg_write(UC_ARM64_REG_X19,awork)
     assert avs_block(0x3e68,0x3ea4);adjusted=bytes(au.mem_read(awork,len(adjusted)))
    decoded=[struct.unpack_from('<6I',adjusted,i) for i in range(0,len(adjusted),24)]
    expected_entries=(Opp*len(decoded))(*[Opp(f*1000,v*10,s*10) for f,v,s,*rest in decoded])
    expected=OppPolicy();expected_status=lib.MaliMfgOppBuild(C.byref(expected),expected_entries,len(decoded))
    assert actual==expected_status,(vector,segment,mcl,aging,actual,expected_status)
    if actual:
     assert bytes(result)==bytes(C.sizeof(result));unsafe_tables+=1
    else:
     assert bytes(result)==bytes(expected),(vector,segment,mcl,aging)
     calibration_points+=len(decoded)
calibration_bad_frequency_cases=0
for index in range(3):
 words=[0,0,0];words[index]=((1100,700,390)[index]-1)<<8|100
 au.mem_write(agb,bytes(secs[symbols['g_avs_enable'][1]][5]))
 au.mem_write(agb,struct.pack('<Q',asigned));au.mem_write(agb+0x2c,struct.pack('<I',45))
 au.mem_write(efaddr,struct.pack('<Q',aefuse));au.mem_write(asigned,opp_bytes)
 au.mem_write(avaddr,data(av[1])[av[2]:av[2]+av[3]]);awords=words;areads=[]
 au.reg_write(UC_ARM64_REG_X28,agb)
 assert not avs_block(0x36a4,0x3d50) and afailure in (0x45e4,0x45ec,0x45f0)
 result=OppPolicy();cal=Calibration(C.sizeof(Calibration),1,2,0,0,7,(C.c_uint32*3)(*words))
 assert lib.MaliMfgOppCalibrate(C.byref(result),raw_base,45,C.byref(cal))!=0 and not result.Signature
 calibration_bad_frequency_cases+=1
calibration_result={'status':'passed','synthetic_combinations':calibration_cases,
 'exact_policy_points_compared':calibration_points,'unsafe_tables_refused':unsafe_tables,
 'mismatched_anchor_frequency_cases':calibration_bad_frequency_cases,'stock_module_sha256':SHA,
 'unadjusted_table_sha256':hashlib.sha256(opp_bytes).hexdigest(),'relocations_applied':arelocations,
 'original_pure_blocks':[{'start':hex(lo),'bytes':hi-lo,
   'sha256':hashlib.sha256(data(section)[lo:hi]).hexdigest()} for lo,hi in aranges],
 'scope':'original segment selection, three AVS anchor decodes/adjustments and both rounded interpolations, then original one-time aging subtraction; complete resulting policy including rates and springboards compared',
 'intentional_difference':'missing input and unknown segment are refused, test/fallback fuse constants are never installed; invalid/nonmonotonic/unreachable final policies are refused before output mutation, including unsafe stock aging combinations',
 'raw_input_source':'deterministic synthetic fixtures only','per_device_calibration_read':False,
 'firmware_or_IOMMU_ownership_proved':False,'thermal_or_battery_sensor_policy_implemented':False,
 'probe_init_allocators_or_real_device_executed':False}
report={'status':'passed','scope':__doc__.strip(),'comparisons':comparisons,'clock':clock_result,'power':power_result,'pmic':pmic_result,'hardware_clock':hardware_result,'voltage':voltage_result,'opp':opp_result,'calibration':calibration_result,'stock_module_sha256':SHA,'relocations_applied':relocations,
 'functions':{name:{'section_offset':off,'file_offset':secs[section][4]+off,'bytes':symbols[name][3]} for name,off in functions.items()},
 'checks':['all four domains on/off','unrelated control bits preserved','delayed independent power/SRAM acknowledgements','exact MMIO write values/order and delay arguments'],
 'original_driver_functions_executed':True,'all_mmio_and_kernel_services_are_virtual_fixtures':True,'full_board_power_implemented':False,'device_executed':False,
 'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (ROOT/'Windows/MtkMali/Mfg.c',ROOT/'Windows/MtkMali/Mfg.h',Path(__file__).resolve())}}
(ROOT/'Windows/Build/mali-stock-power-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'Stock MT6789 ARM64 MFG comparison: {comparisons} domain, {len(clock_cases)} clock, {len(power_cases)} composite power, {len(pmic_cases)} WACS2, {len(hardware_cases)} hardware clock, {rate_comparisons} PLL decode, {program_comparisons} PLL programming, {voltage_comparisons} voltage-pair, {spring_points} springboard entries and {plan_comparisons} OPP-plan and {calibration_cases} synthetic calibration comparisons passed (virtual services only)')
