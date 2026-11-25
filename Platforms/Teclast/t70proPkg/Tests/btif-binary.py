# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Selected stock BTIF bodies with virtual FIFO/IRQ; used by verify-stp-binary.

LLVM/Unicorn checks, not REA Evidence. No kernel/module init, clock operation,
DMA execution, RF or device access. The original kfifo/locks/logging are virtual.
"""
import ctypes as C
import hashlib
import struct as S
from unicorn import UC_HOOK_CODE,UC_HOOK_MEM_READ,UC_HOOK_MEM_WRITE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X8,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
SHA='1570949e96c7d9495be844a05f50925088b60bfbd9054d40867f7c4885233bbc'
READ=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint,C.c_uint,C.c_uint,C.POINTER(C.c_uint))
WRITE=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint,C.c_uint,C.c_uint,C.c_uint)
BARRIER=C.CFUNCTYPE(None,C.c_void_p)
WAIT=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint,C.c_uint)
class Io(C.Structure):
 _fields_=[('Context',C.c_void_p),('Read',READ),('Write',WRITE),('Barrier',BARRIER)]
class Core(C.Structure):
 _fields_=[('Io',Io)]+[(n,C.c_uint) for n in ('Initialized','Owned','Running','Faulted','Uncertain')]+[('Saved',C.c_uint*5)]+[(n,C.c_uint) for n in ('SavedWake','WakeOwned','Head','Tail','Count')]+[('Tx',C.c_ubyte*4096)]
class Service(C.Structure):
 _fields_=[(n,C.c_uint) for n in ('Received','Transmitted','More','Handled')]
def compare(lib,loader,path):
 selected=('hal_btif_hw_init','hal_btif_send_data','hal_btif_irq_handler','hal_btif_raise_wak_sig','readl','writel','writeb')
 raw,sec,sy,ranges,bases,u,stub,external,address,relocations=loader(path,selected,image_base=0x18000000)
 assert hashlib.sha256(raw).hexdigest()==SHA
 assert sy['readl'][2]==0xa7f0 and sy['writel'][2]==0xa774
 stop=stub+0xf000;callback=stub+0xe000
 u.mem_write(callback-4,S.pack('<I',0x24091eec));u.mem_write(callback,bytes.fromhex('c0035fd6'))
 dat=0x30000000;stack=0x40000000;mmio=0x50000000
 u.mem_map(dat,0x10000);u.mem_map(stack,0x10000);u.mem_map(mmio,0x1000)
 info=dat;kfifo=dat+0x1000;executed=set();old=None;port=None;pending=bytearray();inq=outq=0
 traps={v:k for k,v in external.items()};traps[callback]='rx_callback'
 allowed={'_raw_spin_lock_irqsave','_raw_spin_unlock_irqrestore','log_read_mmio','log_post_read_mmio','log_write_mmio','log_post_write_mmio','mtk_btif_hal_get_log_lvl','__kfifo_in','__kfifo_out','rx_callback','usleep_range_state'}
 # Defined symbols called with direct branches also need explicit boundaries.
 for n in allowed:
  if n in sy:traps[address(sy[n])]=n
 def fresh(seed=0):
  return {'words':{0x0c:0x37^seed,0x6c:0x80000002^(seed<<4),0x60:0x135a^seed,0x4c:0x80^(seed<<4),4:0xa0^(seed<<4),0x14:0x60,0x64:0xa4},
   'rx':b'','offset':0,'kind':4,'sent':bytearray(),'writes':[],'barriers':0,'callbacks':[],'waits':[]}
 def read(state,region,offset,width):
  if region in (1,2):assert offset==8 and width==4;return 0
  assert region==0
  if offset==8:assert width==4;return (state['kind'] if state['offset']<len(state['rx']) else 0)|(2 if state['words'][4]&2 else 0)
  if offset==0:
   assert width==1 and state['offset']<len(state['rx'])
   v=state['rx'][state['offset']];state['offset']+=1;return v
  assert width==4 and offset in state['words']
  if offset==0x14:return (state['words'][offset]&~1)|int(state['offset']<len(state['rx']))
  return state['words'][offset]
 def write(state,region,offset,width,value):
  assert region==0;state['writes'].append((offset,width,value))
  if offset==0:assert width==1 and value<=255;state['sent'].append(value)
  elif offset==8:assert width==4 # IIR read/FCR write alias
  else:assert width==4 and offset in state['words'];state['words'][offset]=value
 @READ
 def rd(context,region,offset,width,value):
  value[0]=read(port,region,offset,width);return 0
 @WRITE
 def wr(context,region,offset,width,value):
  write(port,region,offset,width,value);return 0
 @BARRIER
 def barrier(context):port['barriers']+=1
 @WAIT
 def wait(context,minimum,maximum):
  assert (minimum,maximum)==(128,160) and not port['words'][0x64]&1
  port['waits'].append((minimum,maximum));return 0
 io=Io(None,rd,wr,barrier);b=Core();result=Service();rx=C.create_string_buffer(256)
 lib.MtkBtifInit.argtypes=[C.POINTER(Core),C.POINTER(Io)]
 lib.MtkBtifStart.argtypes=[C.POINTER(Core),C.c_uint,C.c_uint]
 lib.MtkBtifService.argtypes=[C.POINTER(Core),C.c_void_p,C.c_uint,C.POINTER(Service)]
 lib.MtkBtifStpWrite.argtypes=[C.c_void_p,C.c_void_p,C.c_uint,C.POINTER(C.c_uint)]
 lib.MtkBtifRaiseWake.argtypes=[C.POINTER(Core),WAIT]
 lib.MtkBtifStop.argtypes=[C.POINTER(Core)]
 def memory_read(uc,access,at,size,value,context):
  assert mmio<=at<mmio+0x1000
  v=read(old,0,at-mmio,size);uc.mem_write(at,v.to_bytes(size,'little'))
 def memory_write(uc,access,at,size,value,context):write(old,0,at-mmio,size,value)
 u.hook_add(UC_HOOK_MEM_READ,memory_read,begin=mmio,end=mmio+0xfff)
 u.hook_add(UC_HOOK_MEM_WRITE,memory_write,begin=mmio,end=mmio+0xfff)
 def update_kfifo():u.mem_write(kfifo,S.pack('<IIIIQ',inq,outq,4095,1,dat+0x4000))
 def hook(uc,pc,size,context):
  nonlocal inq,outq
  name=traps.get(pc)
  if name:
   assert name in allowed,('unexpected original BTIF service',name)
   executed.add(name);a=uc.reg_read(UC_ARM64_REG_X0);c=uc.reg_read(UC_ARM64_REG_X2);p=uc.reg_read(UC_ARM64_REG_X1);v=0
   if name=='mtk_btif_hal_get_log_lvl':v=0xffffffff
   elif name=='__kfifo_in':
    assert a==kfifo and c<=4096-len(pending);pending.extend(bytes(uc.mem_read(p,c)));inq+=c;update_kfifo();v=c
   elif name=='__kfifo_out':
    assert a==kfifo and c in (8,16);v=min(c,len(pending));uc.mem_write(p,bytes(pending[:v]));del pending[:v];outq+=v;update_kfifo()
   elif name=='usleep_range_state':
    assert (a,p,c)==(128,160,2) and not old['words'][0x64]&1
    old['waits'].append((a,p))
   elif name=='rx_callback':
    assert a==info and 1<=c<=256;old['callbacks'].append(bytes(uc.mem_read(p,c)))
   uc.reg_write(UC_ARM64_REG_X0,v);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert any(bases[i]+lo<=pc<bases[i]+hi for i,lo,hi in ranges),('unselected original BTIF instruction',hex(pc))
  word=S.unpack('<I',uc.mem_read(pc,4))[0]
  if word==0xd5384108:uc.reg_write(UC_ARM64_REG_X8,dat+0xf000);uc.reg_write(UC_ARM64_REG_PC,pc+4)
  elif word==0xd5033f9f:old['barriers']+=1
 u.hook_add(UC_HOOK_CODE,hook)
 def call(name,a=info,p=0,c=0):
  u.reg_write(UC_ARM64_REG_X0,a);u.reg_write(UC_ARM64_REG_X1,p);u.reg_write(UC_ARM64_REG_X2,c)
  u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,stack+0xfff0)
  u.emu_start(address(sy[name]),stop,count=1000000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop,'BTIF function exceeded finite instruction budget'
  v=u.reg_read(UC_ARM64_REG_X0)&0xffffffff;return v if v<0x80000000 else v-0x100000000
 def reset(seed=0,tx=8,rx_trigger=1):
  nonlocal old,port,inq,outq
  old=fresh(seed);port=fresh(seed);pending.clear();inq=outq=0;update_kfifo()
  u.mem_write(info,bytes(0x48));u.mem_write(info,S.pack('<Q',mmio));u.mem_write(info+0x18,S.pack('<II',tx,rx_trigger))
  u.mem_write(info+0x30,S.pack('<QQ',callback,kfifo))
  assert lib.MtkBtifInit(C.byref(b),C.byref(io))==0
 def start():
  assert call('hal_btif_hw_init')==0
  assert lib.MtkBtifStart(C.byref(b),tx,trigger)==0
  assert old['writes']==port['writes'] and old['barriers']==port['barriers']==13
 init_cases=tx_cases=rx_cases=irq_cases=wake_cases=0
 for seed in (0,1,4,0x80):
  for tx in range(16):
   for trigger in range(8):
    reset(seed,tx,trigger);start();init_cases+=1
 tx=8;trigger=1
 for baseline in (0,1,0xa4,0xa5,0x80000000,0x80000001):
  reset();start();old['words'][0x64]=port['words'][0x64]=baseline
  assert call('hal_btif_raise_wak_sig')==0 and lib.MtkBtifRaiseWake(C.byref(b),wait)==0
  assert old['writes']==port['writes'] and old['barriers']==port['barriers']
  assert old['waits']==port['waits']==[(128,160)] and b.SavedWake==baseline
  assert lib.MtkBtifStop(C.byref(b))==0 and port['words'][0x64]==baseline
  wake_cases+=1
 for count in (1,2,7,8,9,15,16,17,31,32,255,256,257,1994,1995,1996):
  for lsr in (0,0x20,0x40,0x60):
   reset();start();body=bytes((i*57+count)&255 for i in range(count))
   wire=bytes((0x80,0x20|(count>>8),count&255,0))+body+b'\0\0';n=len(wire)
   u.mem_write(dat+0x2000,wire)
   assert call('hal_btif_send_data',info,dat+0x2000,n)==n
   written=C.c_uint();assert lib.MtkBtifStpWrite(C.byref(b),wire,n,C.byref(written))==0 and written.value==n
   assert old['writes']==port['writes'] and old['barriers']==port['barriers']
   old['words'][0x14]=port['words'][0x14]=lsr
   for step in range(512):
    assert call('hal_btif_irq_handler')==0
    status=lib.MtkBtifService(C.byref(b),rx,256,C.byref(result))
    assert status in (0,3) and result.Received==0
    assert old['writes']==port['writes'] and old['sent']==port['sent'] and old['barriers']==port['barriers']
    assert bytes(pending)==bytes(b.Tx[(b.Head+i)&4095] for i in range(b.Count))
    irq_cases+=1
    if lsr==0:assert not old['sent'] and b.Count==n;break
    if b.Count==0:assert old['sent']==wire;break
   else:raise AssertionError('BTIF TX failed to finish')
   tx_cases+=1
 for count in (1,2,3,15,16,17,255,256,257,511,512,513,1999,2005):
  for kind in (4,0x40):
   for capacity in (1,16,255,256):
    reset();start();body=bytes((i*73+count)&255 for i in range(count))
    old['rx']=port['rx']=body;old['kind']=port['kind']=kind
    original_result=call('hal_btif_irq_handler');got=bytearray();passes=0
    while port['offset']<count:
     status=lib.MtkBtifService(C.byref(b),rx,capacity,C.byref(result));passes+=1
     assert 1<=result.Received<=capacity and not result.Transmitted and result.Handled
     assert status==(3 if port['offset']<count else 0)
     got.extend(rx.raw[:result.Received]);assert passes<=count
    assert got==body==b''.join(old['callbacks']) and old['offset']==port['offset']==count
    # The stock function omits full 256-byte chunks from its returned count.
    assert original_result==(count%256 if count%256 else -1)
    assert old['writes']==port['writes'] and old['barriers']==port['barriers']
    rx_cases+=1
 print(f'Stock BTIF: {init_cases} initialization, {tx_cases} queued TX, {irq_cases} TX IRQ and {rx_cases} RX and {wake_cases} wake comparisons passed',flush=True)
 return {'status':'passed','module_sha256':SHA,'functions':{n:{'section':sy[n][1],'offset':sy[n][2],'bytes':sy[n][3]} for n in selected},
  'relocations':relocations,'initialization_cases':init_cases,'tx_queue_cases':tx_cases,'tx_irq_cases':irq_cases,'rx_cases':rx_cases,'wake_cases':wake_cases,
  'comparisons':init_cases+tx_cases+irq_cases+rx_cases+wake_cases,'executed_virtual_services':sorted(executed),
  'actual_original_MMIO_instruction_widths_and_DSB_SY_checked':True,
  'FIFO_2002_byte_frame_through_original_hal_send_and_irq':True,
  'intentional_changes':['Bound RX work to caller capacity instead of unbounded stock IRQ loop','Report all received bytes; stock return count omits complete 256-byte chunks','Reject queue pressure without accepting a frame prefix','Require idle exclusive handoff and retain baseline/lease after failed IO'],
  'scope':'Original ARM64 init/send/IRQ and ordered readl/writel/writeb bodies in Unicorn; physical FIFO/IRQ, kfifo, locks and logging virtual. No module init, DMA engine, clock/power provider, OS IRQ registration, RF or tablet access'}
