#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build the common STP core and compare it with isolated original ARM64 bodies.

Only the selected send, parser, function/register/pin-control and ring bodies run
from wmt_drv.ko; GPS_hw_suspend_ctrl is isolated from gps_drv_stp.ko.
The BTIF FIFO MMIO/IRQ instruction bodies also run with virtual registers.
OS, sleep and physical transport calls are virtual services.
Parser-routing tests capture queue calls; ring tests execute the real bodies. No probe,
module init, kernel, device node, bus, radio or real GPS operation executes.
"""
from pathlib import Path
import ctypes as C
import hashlib,json,os,struct as S,subprocess,sys,tempfile,shutil,time,importlib.util,faulthandler
faulthandler.enable()
from contextlib import contextmanager
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X8,UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_SP
PKG=Path(__file__).resolve().parents[1];R=PKG.parents[2]
if '--locked' not in sys.argv:
 os.execv(R/'Resources/Scripts/with-build-lock',[str(R/'Resources/Scripts/with-build-lock'),
  'T70Pro','Build and compare common STP transport with original ARM64 module','--',sys.executable,__file__,'--locked',*sys.argv[1:]])
os.chdir(R)
B=R/'Windows/Build';INC=R/'Silicon/MediaTek/MediaTekPkg/Include'
SOURCE=R/'Silicon/MediaTek/MediaTekPkg/Library/MtkStpCoreLib/MtkStpCore.c'
BTIF_SOURCE=SOURCE.with_name('MtkBtifCore.c')
VCN28_SOURCE=SOURCE.with_name('MtkVcn28Core.c')
VCN28_HELPER=PKG/'Tests/vcn28-binary.py'
BTIF_HELPER=PKG/'Tests/btif-binary.py'
GNSS_SOURCE=SOURCE.with_name('MtkGnssCore.c')
GNSS_HELPER=PKG/'Tests/gnss-binary.py'
SOURCES=(SOURCE,BTIF_SOURCE,VCN28_SOURCE,GNSS_SOURCE)
STOCK=R.parent/'analysis/stock-binaries/vendor_dlkm_a/wmt_drv.ko'
SHA='d395f5249bdeb2b5f6134f70c64e156bd682ba895a6da2d4c2544836a27b91e3'
GPS=R.parent/'analysis/stock-binaries/vendor_dlkm_a/gps_drv_stp.ko'
GPS_SHA='938639b15d88f4a2d777c14bdd777408c561c341cc8805c250c4db3568908260'
commands=[]
def run(args):
 args=[str(x) for x in args];commands.append(args)
 p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 if p.returncode:print(p.stdout);raise SystemExit(p.returncode)
 return p.stdout.strip()
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def module(path):
 raw=path.read_bytes();assert raw[:6]==b'\x7fELF\x02\x01' and S.unpack_from('<H',raw,18)[0]==183
 start=S.unpack_from('<Q',raw,40)[0];width,count=S.unpack_from('<HH',raw,58)
 assert width==64 and start+width*count<=len(raw)
 sec=[S.unpack_from('<IIQQQQIIQQ',raw,start+i*width) for i in range(count)]
 def data(i):
  q=sec[i]
  if q[1]==8:return bytes(q[5])
  assert q[4]+q[5]<=len(raw);return raw[q[4]:q[4]+q[5]]
 sy={};tabs={}
 for i,q in enumerate(sec):
  if q[1]!=2:continue
  assert q[9]==24;names=data(q[6]);entries=[]
  for k in range(0,q[5],24):
   no,info,other,index,value,size=S.unpack_from('<IBBHQQ',data(i),k)
   name=names[no:].split(b'\0',1)[0].decode();v=(name,index,value,size);entries.append(v)
   if name:sy[name]=v
  tabs[i]=entries
 return raw,sec,sy,tabs,data
class Decoder(C.Structure):
 _fields_=[(n,C.c_uint) for n in ('Mode','State','Length','Used','Channel','Padding','Faulted')]+[('Payload',C.c_ubyte*1999)]
class Frame(C.Structure):
 _fields_=[('Channel',C.c_uint),('Length',C.c_uint),('Data',C.POINTER(C.c_ubyte))]
class Queue(C.Structure):
 _fields_=[(n,C.c_uint) for n in ('Channel','Epoch','Head','Tail','Count','Faulted')]+[('Data',C.c_ubyte*16384)]
def isolated_module(path,selected,image_base=0x20000000):
 raw,sec,sy,tabs,data=module(path)
 ranges=[(sy[n][1],sy[n][2],sy[n][2]+sy[n][3]) for n in selected]
 bases={};cursor=image_base
 for i,q in enumerate(sec):
  if q[2]&2 and q[5]:bases[i]=cursor;cursor+=(q[5]+4095)&~4095
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(image_base,cursor-image_base)
 for i,base in bases.items():
  if sec[i][1]!=8:u.mem_write(base,data(i))
 stub=(cursor+0x10000+4095)&~4095;u.mem_map(stub,0x10000);external={}
 def address(v):
  name,index,value,size=v
  if index:return bases[index]+value
  if name not in external:external[name]=stub+len(external)*16
  return external[name]
 relocations=0
 for i,q in enumerate(sec):
  if q[1]!=4:continue
  for k in range(0,q[5],24):
   off,info,add=S.unpack_from('<QQq',data(i),k)
   if not any(si==q[7] and lo<=off<hi for si,lo,hi in ranges):continue
   kind=info&0xffffffff;target=address(tabs[q[6]][info>>32])+add;place=bases[q[7]]+off
   word=S.unpack('<I',u.mem_read(place,4))[0]
   if kind==275:
    delta=(target>>12)-(place>>12);assert -(1<<20)<=delta<(1<<20)
    word=(word&~((3<<29)|(0x7ffff<<5)))|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
   elif kind in (277,278,284,285,286):
    shift={277:0,278:0,284:1,285:2,286:3}[kind];assert target% (1<<shift)==0
    word=(word&~(4095<<10))|(((target&4095)>>shift)<<10)
   elif kind in (282,283):
    delta=target-place;assert not delta&3 and -(1<<27)<=delta<(1<<27)
    word=(word&0xfc000000)|((delta>>2)&0x3ffffff)
   else:raise AssertionError(('unsupported STP relocation',kind))
   u.mem_write(place,S.pack('<I',word));relocations+=1
 return raw,sec,sy,ranges,bases,u,stub,external,address,relocations
def stock_compare(lib):
 selected=('stp_send_data_no_ps','mtk_wcn_stp_send_data','mtk_wcn_stp_parser_data','wmt_core_func_ctrl_cmd','stp_add_to_rx_queue','mtk_wcn_stp_receive_data','opfunc_gps_mcu_ctrl','wmt_core_reg_rw_raw','mtk_wcn_soc_pin_ctrl')
 raw,sec,sy,ranges,bases,u,stub,external,address,relocations=isolated_module(STOCK,selected)
 assert hashlib.sha256(raw).hexdigest()==SHA
 stop=stub+0xf000;tx=stub+0xe000;notify=tx+32;status=tx+64
 for ptr,tag in ((tx,0x94b3d001),(notify,0x81e1eabd),(status,0x3463e8d3)):
  u.mem_write(ptr-4,S.pack('<I',tag));u.mem_write(ptr,bytes.fromhex('c0035fd6'))
 dat=0x30000000;stack=0x40000000;u.mem_map(dat,0x10000);u.mem_map(stack,0x10000)
 bss=next(bases[i] for i,q in enumerate(sec) if q[1]==8 and q[5]>0x30060)
 bss_size=next(q[5] for q in sec if q[1]==8 and q[5]>0x30060)
 traps={v:k for k,v in external.items()}
 traps.update({tx:'tx',notify:'notify',status:'status',address(sy['stp_add_to_rx_queue']):'rx_queue',address(sy['stp_dbg_pkt_log']):'pkt_log'})
 noops={'osal_lock_sleepable_lock','osal_unlock_sleepable_lock','osal_lock_unsleepable_lock','osal_unlock_unsleepable_lock','osal_ftrace_print',
  'osal_err_print','osal_warn_print','___ratelimit','stp_psm_disable_by_tx_rx_density',
  'stp_psm_thread_lock_aquire','stp_psm_thread_lock_release',
  'stp_psm_is_to_block_traffic','stp_psm_has_pending_data','stp_psm_is_quick_ps_support','wmt_plat_get_comm_if_type','wmt_lib_trigger_assert','pkt_log','mtk_wcn_stp_dbg_dump_package','wmt_core_trigger_assert'}
 for name in noops|{'osal_memcpy','osal_memset','osal_malloc','osal_free','wmt_core_tx','wmt_ctrl'}:
  if name in sy:traps[address(sy[name])]=name
 received=[];sent=[];events=[];tx_written=None;executed=set();requests=[];queue_mode=False
 control_response=bytes((2,6,1,0,0));control_txstatus=control_rxstatus=0;control_actual=5
 mcu_mode=False;reg_mode=False;reg_write=0;control_written=None;heap_cursor=dat+0x8000;allocations={}
 def hook(uc,pc,size,ctx):
  nonlocal heap_cursor
  name=traps.get(pc)
  if name=='rx_queue' and queue_mode:name=None
  if name:
   executed.add(name);a=uc.reg_read(UC_ARM64_REG_X0);b=uc.reg_read(UC_ARM64_REG_X1);c=uc.reg_read(UC_ARM64_REG_X2);result=0
   if name=='osal_memcpy':uc.mem_write(a,bytes(uc.mem_read(b,c)));result=a
   elif name=='osal_memset':uc.mem_write(a,bytes([b&255])*c);result=a
   elif name=='osal_malloc':
    assert mcu_mode and 0<a<=2000 and heap_cursor+a<dat+0xe000
    result=heap_cursor;heap_cursor+=(a+15)&~15;allocations[result]=a;uc.mem_write(result,b'\xee'*a)
   elif name=='osal_free':
    assert a in allocations;del allocations[a]
   elif name=='wmt_core_tx':
    assert 1<=b<=1996 and uc.reg_read(UC_ARM64_REG_X3)==0
    requests.append(bytes(uc.mem_read(a,b)));uc.mem_write(c,S.pack('<I',b if control_written is None else control_written));result=control_txstatus
   elif name=='wmt_ctrl':
    assert S.unpack('<I',uc.mem_read(a,4))[0]==17
    buf,wanted,actual=S.unpack('<QQQ',uc.mem_read(a+8,24));assert wanted==((8 if reg_write else 16) if reg_mode else mcu_capacity+4 if mcu_mode else 5)
    uc.mem_write(buf,control_response[:min(control_actual,wanted)]);uc.mem_write(actual,S.pack('<I',control_actual));result=control_rxstatus
   elif name=='tx':
    sent.append(bytes(uc.mem_read(a,b)));uc.mem_write(c,S.pack('<I',b if tx_written is None else tx_written))
   elif name=='rx_queue':received.append((c,bytes(uc.mem_read(a,b))))
   elif name=='notify':events.append(a)
   elif name=='status':result=0x1f
   elif name not in noops:raise AssertionError(('unexpected virtual service executed',name,hex(pc)))
   uc.reg_write(UC_ARM64_REG_X0,result);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert any(bases[i]+lo<=pc<bases[i]+hi for i,lo,hi in ranges),('unselected original code',hex(pc))
  if S.unpack('<I',uc.mem_read(pc,4))[0]==0xd5384108:
   uc.reg_write(UC_ARM64_REG_X8,dat+0xf000);uc.reg_write(UC_ARM64_REG_PC,pc+4)
 u.hook_add(UC_HOOK_CODE,hook)
 def reset(mode):
  u.mem_write(bss,bytes(bss_size));u.mem_write(bss+0x30038,b'\x01');u.mem_write(bss+0x3004c,S.pack('<I',mode))
  for off,ptr in ((0x7e48,tx),(0x7e58,notify),(0x7e68,status)):u.mem_write(bss+off,S.pack('<Q',ptr))
  received.clear();sent.clear();events.clear();requests.clear()
 def call(name,a,b,c=0,d=0):
  u.reg_write(UC_ARM64_REG_X0,a);u.reg_write(UC_ARM64_REG_X1,b);u.reg_write(UC_ARM64_REG_X2,c);u.reg_write(UC_ARM64_REG_X3,d)
  u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,stack+0xfff0)
  u.emu_start(address(sy[name]),stop,count=100000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop,'Original STP function exceeded finite instruction budget'
  v=u.reg_read(UC_ARM64_REG_X0)&0xffffffff;return v if v<0x80000000 else v-0x100000000
 def invoke(name,body,channel=2):
  u.mem_write(dat,body or b'\0');return call(name,dat,len(body),channel)
 lib.MtkStpEncode.argtypes=[C.c_uint,C.c_uint,C.c_void_p,C.c_uint,C.c_void_p,C.c_uint,C.POINTER(C.c_uint)]
 lib.MtkStpDecoderInit.argtypes=[C.POINTER(Decoder),C.c_uint]
 lib.MtkStpReceive.argtypes=[C.POINTER(Decoder),C.c_void_p,C.c_uint,C.POINTER(C.c_uint),C.POINTER(Frame)]
 def encoded(mode,channel,body):
  out=C.create_string_buffer(2005);written=C.c_uint()
  assert lib.MtkStpEncode(mode,channel,body,len(body),out,2005,C.byref(written))==0
  return out.raw[:written.value]
 tx_cases=rx_cases=fragment_cases=0
 for mode in (2,8,16):
  for n in range(1,1997):
   channel=(0,1,2,3,7)[n%5];body=bytes((i*73+(n>>3))&255 for i in range(n));expected=encoded(mode,channel,body)
   reset(mode);assert invoke('mtk_wcn_stp_send_data',body,channel)==n
   assert sent==[expected],(mode,n,'send_data mismatch');tx_cases+=1
  for n in (1,2,3,255,256,257,1996):
   body=bytes((i*57+13)&255 for i in range(n));reset(mode)
   assert invoke('stp_send_data_no_ps',body)==n and sent==[encoded(mode,2,body)];tx_cases+=1
  for n in (0,1997,1998,1999,2000):
   reset(mode);assert invoke('mtk_wcn_stp_send_data',b'\x33'*n)==-1 and not sent;tx_cases+=1
  for n in (1,2,3,4,7,15,255,256,257,511,1996,1997,1998,1999):
   body=bytes((i*73+(i>>8))&255 for i in range(n))
   for channel in (0,1,2,3,7):
    wire=bytes((0x80,(channel<<4)|(n>>8),n&255,0))+body+b'\0\0'
    if mode==16:wire+=bytes((-len(wire))&3)
    chunks=(len(wire),) if mode==16 else (1,2,3,7,257,len(wire))
    for chunk in chunks:
     reset(mode);dec=Decoder();assert lib.MtkStpDecoderInit(C.byref(dec),mode)==0;view=Frame();actual=[]
     for off in range(0,len(wire),chunk):
      part=wire[off:off+chunk];assert invoke('mtk_wcn_stp_parser_data',part)==0
      used=C.c_uint();s=lib.MtkStpReceive(C.byref(dec),part,len(part),C.byref(used),C.byref(view));assert used.value==len(part)
      assert s in (3,4)
      if s==4:actual.append((view.Channel,bytes(view.Data[:view.Length])))
     assert received==actual==[(channel,body)] and events==[channel],(mode,n,channel,chunk,len(received),actual)
     rx_cases+=1;fragment_cases+=chunk<len(wire)
 # Execute both original ring bodies rather than the parser's capture service.
 # The stock ring has 16384 bytes, leaves one slot vacant, and drops overflow.
 queue_mode=True;reset(8);q=Queue()
 lib.MtkStpQueueInit.argtypes=[C.POINTER(Queue),C.c_uint]
 assert lib.MtkStpQueueInit(C.byref(q),2)==0
 lib.MtkStpQueuePush.argtypes=[C.POINTER(Queue),C.c_uint,C.POINTER(Frame)]
 lib.MtkStpQueueRead.argtypes=[C.POINTER(Queue),C.c_uint,C.c_void_p,C.c_uint,C.POINTER(C.c_uint)]
 queue_cases=0;wraps=0;seed=0x20f30f39;produced=0;pending=b'';out=C.create_string_buffer(16384)
 ring=bss+0x7e88+2*0x4018
 def push(body):
  buf=C.create_string_buffer(body);view=Frame(2,len(body),C.cast(buf,C.POINTER(C.c_ubyte)))
  return lib.MtkStpQueuePush(C.byref(q),q.Epoch,C.byref(view))
 def pop(n):
  count=C.c_uint();result=lib.MtkStpQueueRead(C.byref(q),q.Epoch,out,n,C.byref(count))
  return result,out.raw[:count.value]
 for i in range(1600):
  seed^=(seed<<13)&0xffffffff;seed^=seed>>17;seed^=(seed<<5)&0xffffffff
  n=seed%1999+1;oldhead=q.Head;oldtail=q.Tail
  if seed&1 and n<=16383-len(pending):
   body=bytes(((produced+j)*73)&255 for j in range(n))
   assert invoke('stp_add_to_rx_queue',body)==0 and push(body)==0
   produced+=n;pending+=body
  else:
   expected=pending[:n];original=call('mtk_wcn_stp_receive_data',dat+0x4000,n,2)
   result,actual=pop(n)
   assert result==(0 if pending else 3) and original==len(expected)
   assert bytes(u.mem_read(dat+0x4000,original))==actual==expected
   pending=pending[len(actual):]
  head,tail=S.unpack('<II',u.mem_read(ring+0xc068,8))
  assert (head&16383,tail&16383,len(pending))==(q.Head,q.Tail,q.Count)
  wraps+=q.Head<oldhead or q.Tail<oldtail;queue_cases+=1
 assert wraps>20
 original=call('mtk_wcn_stp_receive_data',dat+0x4000,16384,2);result,actual=pop(16384)
 assert original==len(pending) and actual==pending and not q.Count;queue_cases+=1
 # Confirm the intentional overflow policy difference without losing bytes
 # silently in the new receiver: the port clears/faults until owner recovery.
 for n in (1999,)*8+(391,):
  body=bytes([n&255])*n
  assert invoke('stp_add_to_rx_queue',body)==0 and push(body)==0;queue_cases+=1
 assert q.Count==16383
 assert invoke('stp_add_to_rx_queue',b'X')==-1 and push(b'X')==9
 head,tail=S.unpack('<II',u.mem_read(ring+0xc068,8))
 assert ((tail-head)&16383)==16383 and q.Faulted and not q.Count and not any(q.Data)
 queue_cases+=1;queue_mode=False
 print('Stock STP TX/RX/ring passed; checking WMT control',flush=True)
 # A single WMT owner shares channel 4; function-control ACKs have no ID.
 Write=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_void_p,C.c_uint,C.POINTER(C.c_uint))
 class Transmitter(C.Structure):
  _fields_=[('Context',C.c_void_p),('Write',Write),('Mode',C.c_uint),('Faulted',C.c_uint),('Buffer',C.c_ubyte*2005)]
 class Control(C.Structure):
  _fields_=[('Transmitter',C.POINTER(Transmitter))]+[(n,C.c_uint) for n in ('Pending','Faulted','Confirmed','Function','Enable','FirmwareStatus','Opcode','ReplyCapacity','ReplyLength','GpsPowerSave','RegisterWrite','RegisterAddress','RegisterValue')]+[('Reply',C.c_ubyte*1995)]
 written=[]
 @Write
 def write_callback(context,buf,length,actual):
  written.append(C.string_at(buf,length));actual[0]=length;return 0
 lib.MtkStpTransmitterInit.argtypes=[C.POINTER(Transmitter),C.c_uint,C.c_void_p,Write]
 lib.MtkWmtControlInit.argtypes=[C.POINTER(Control),C.POINTER(Transmitter)]
 lib.MtkWmtControlBegin.argtypes=[C.POINTER(Control),C.c_uint,C.c_uint]
 lib.MtkWmtControlEvent.argtypes=[C.POINTER(Control),C.POINTER(Frame)]
 # C stores a raw callback pointer: keep its Python owner alive with t.
 t=Transmitter();t.write_callback=write_callback;control=Control()
 assert lib.MtkStpTransmitterInit(C.byref(t),8,None,write_callback)==0
 def begin(function,enable):
  written.clear();assert lib.MtkWmtControlInit(C.byref(control),C.byref(t))==0
  assert lib.MtkWmtControlBegin(C.byref(control),function,enable)==3
 def event(body):
  buffer=C.create_string_buffer(body);view=Frame(4,len(body),C.cast(buffer,C.POINTER(C.c_ubyte)))
  return lib.MtkWmtControlEvent(C.byref(control),C.byref(view))
 control_cases=0
 for function in range(256):
  for enable in (0,1):
   reset(8);assert call('wmt_core_func_ctrl_cmd',function,enable)==0
   begin(function,enable)
   assert requests==[bytes((1,6,2,0,function,enable))]
   assert written==[encoded(8,4,requests[0])]
   assert event(control_response)==0 and control.Confirmed==1 and control.Pending==0
   control_cases+=1
 for at in (0,2,3,4):
  for value in range(256):
   body=bytearray((2,6,1,0,0));body[at]=value;control_response=bytes(body)
   reset(8);original=call('wmt_core_func_ctrl_cmd',2,1)
   begin(2,1);ported=event(control_response)
   assert (original==0)==(ported==0),(at,value,original,ported)
   assert control.Confirmed==(ported==0)
   if ported:assert control.Faulted==1 and control.Pending==0
   control_cases+=1
 # Original zero-initialized status makes a four-byte reply look successful.
 # The port requires the actual five bytes and never confirms that short read.
 control_response=bytes((2,6,1,0,0));control_actual=4;reset(8)
 assert call('wmt_core_func_ctrl_cmd',2,1)==0
 begin(2,1);assert event(control_response[:4])==6 and not control.Confirmed
 # Execute actual opcode-8 read/write and conditional SoC GPS pin-control.
 # The original write worker accepts any exact-length event; reads additionally
 # check the echoed address. The port also checks type/opcode/length/status.
 print('Stock WMT opcode-6 passed; checking register worker',flush=True)
 reg_mode=True;register_cases=0;sync_cases=0
 lib.MtkWmtRegisterBegin.argtypes=[C.POINTER(Control),C.c_uint,C.c_uint,C.c_uint,C.c_uint]
 lib.MtkWmtGpsSyncBegin.argtypes=[C.POINTER(Control),C.c_uint,C.c_uint]
 def register_reply(write,address,value):
  return bytes((2,8,4 if write else 12,0,0,0x5a,0xc3,0x19))+(b'' if write else S.pack('<II',address,value))
 def original_register(write,address,value,mask,response,actual=None,txstatus=0,rxstatus=0,wrote=None):
  nonlocal reg_write,control_response,control_actual,control_txstatus,control_rxstatus,control_written
  reg_write=write;control_response=response;control_actual=len(response) if actual is None else actual
  control_txstatus=txstatus;control_rxstatus=rxstatus;control_written=wrote;reset(8)
  u.mem_write(dat+0x4000,S.pack('<I',value))
  result=call('wmt_core_reg_rw_raw',write,address,dat+0x4000,mask)
  return result,S.unpack('<I',u.mem_read(dat+0x4000,4))[0]
 def register_begin(write,address,value,mask):
  written.clear();assert lib.MtkWmtControlInit(C.byref(control),C.byref(t))==0
  assert lib.MtkWmtRegisterBegin(C.byref(control),write,address,value,mask)==3
  expected=bytes((1,8,16,0,1 if write else 2,1,0,1))+S.pack('<III',address,value,mask)
  assert requests==[expected] and written==[encoded(8,4,expected)]
 for write in (0,1):
  for address_value in (0,4,0x80050078,0xffffffff):
   for value in (0,1,0x10000000,0xdeadbeef,0xffffffff):
    for mask in (0,1,0x70000000,0xffffffff):
     returned=value^0xa53c719e;reply=register_reply(write,address_value,returned)
     assert original_register(write,address_value,value,mask,reply)==(0,value if write else returned)
     register_begin(write,address_value,value,mask)
     assert event(reply)==0 and control.Confirmed and not control.Faulted
     assert control.RegisterValue==(0 if write else returned) and control.RegisterAddress==address_value
     register_cases+=1
  for at in range(5):
   for byte in range(256):
    reply=bytearray(register_reply(write,0x80050078,0xdeadbeef));reply[at]=byte;reply=bytes(reply)
    assert original_register(write,0x80050078,0,0x70000000,reply)==(0,0 if write else 0xdeadbeef)
    register_begin(write,0x80050078,0,0x70000000);result=event(reply)
    if byte==register_reply(write,0x80050078,0xdeadbeef)[at]:assert result==0 and control.Confirmed
    elif at==1 and byte not in (6,0x32):assert result==4 and control.Pending and not control.Confirmed
    else:assert result==(5 if at==4 else 6) and control.Faulted and not control.Confirmed and not control.RegisterValue
    register_cases+=1
  for actual in range(18):
   reply=register_reply(write,0x80050078,0xdeadbeef);expected=8 if write else 16
   original=original_register(write,0x80050078,0,0xffffffff,reply,actual)[0]
   assert original==(0 if actual==expected else -3)
   register_begin(write,0x80050078,0,0xffffffff)
   assert event((reply+bytes(18))[:actual])==(0 if actual==expected else 6)
   assert control.Confirmed==(actual==expected);register_cases+=1
 for bit in range(32):
  reply=register_reply(0,0x80050078^(1<<bit),0xdeadbeef)
  assert original_register(0,0x80050078,0,0xffffffff,reply)==(-4,0)
  register_begin(0,0x80050078,0,0xffffffff)
  assert event(reply)==6 and not control.RegisterValue and control.Faulted;register_cases+=1
 for write in (0,1):
  reply=register_reply(write,0x80050078,0)
  for wrote in range(20):
   assert original_register(write,0x80050078,0,0xffffffff,reply,wrote=wrote)[0]==-2;register_cases+=1
  assert original_register(write,0x80050078,0,0xffffffff,reply,txstatus=1)[0]==-2;register_cases+=1
  assert original_register(write,0x80050078,0,0xffffffff,reply,rxstatus=1)[0]==-3;register_cases+=1
 # No board capability is assumed: the static stock flag is zero. Test all
 # synthetic runtime flag bytes against the original branch's actual execution.
 print(f'Stock WMT register: {register_cases} cases passed; checking GPS sync',flush=True)
 ops=address(sy['wmt_ic_ops_soc']);assert bytes(u.mem_read(ops+9,1))==b'\0'
 for flags in range(256):
  for enable in (0,1):
   reset(8);u.mem_write(ops+9,bytes([flags]));reg_write=1
   control_response=register_reply(1,0,0);control_actual=8;control_written=None;control_txstatus=control_rxstatus=0
   assert call('mtk_wcn_soc_pin_ctrl',3,6 if enable else 7,1)==0
   written.clear();assert lib.MtkWmtControlInit(C.byref(control),C.byref(t))==0
   assert lib.MtkWmtGpsSyncBegin(C.byref(control),flags,enable)==(3 if flags&128 else 8)
   if flags&128:
    expected=bytes((1,8,16,0,1,1,0,1))+S.pack('<III',0x80050078,0x10000000 if enable else 0x50000000,0x70000000)
    assert requests==[expected] and written==[encoded(8,4,expected)]
    assert event(control_response)==0 and control.Confirmed
   else:assert not requests and not written and not control.Confirmed and not control.Pending
   sync_cases+=1
 # Stock pin_ctrl swallows worker errors. The new API retains the strict WMT
 # fault and never converts a rejected/short GPS sync event to success.
 for reply in (bytes((2,8,4,0,1,0,0,0)),bytes((2,8,4,0,0,0,0))):
  reset(8);u.mem_write(ops+9,b'\x80');control_response=reply;control_actual=len(reply)
  assert call('mtk_wcn_soc_pin_ctrl',3,6,1)==0
  written.clear();assert lib.MtkWmtControlInit(C.byref(control),C.byref(t))==0
  assert lib.MtkWmtGpsSyncBegin(C.byref(control),128,1)==3
  assert event(reply) in (5,6) and control.Faulted and not control.Confirmed;sync_cases+=1
 u.mem_write(ops+9,b'\0');reg_mode=False;control_written=None
 print(f'Stock WMT register: {register_cases} read/write and {sync_cases} conditional GPS sync comparisons passed',flush=True)
 # Execute the original opcode-0x32 command/response worker with virtual
 # allocation/transport; production port uses bounded preallocated storage.
 mcu_mode=True;mcu_capacity=3;control_actual=7;control_rxstatus=control_txstatus=0
 lib.MtkWmtGpsMcuBegin.argtypes=[C.POINTER(Control),C.c_void_p,C.c_uint,C.c_uint]
 lib.MtkWmtGpsPowerSaveBegin.argtypes=[C.POINTER(Control),C.c_uint,C.c_uint]
 def original_mcu(body,response,capacity=3,actual=None):
  nonlocal heap_cursor,mcu_capacity,control_response,control_actual
  reset(8);heap_cursor=dat+0x8000;assert not allocations
  mcu_capacity=capacity;control_response=response;control_actual=len(response) if actual is None else actual
  u.mem_write(dat,body);u.mem_write(dat+0x2000,bytes(64));u.mem_write(dat+0x6000,bytes(4))
  u.mem_write(dat+0x2000,S.pack('<QQQQQQ',34,dat,len(body),dat+0x4000,capacity,dat+0x6000))
  result=call('opfunc_gps_mcu_ctrl',dat+0x2000,0)
  assert not allocations
  count=S.unpack('<I',u.mem_read(dat+0x6000,4))[0]
  return result,count,bytes(u.mem_read(dat+0x4000,min(count,capacity)))
 def mcu_begin(body,capacity):
  written.clear();assert lib.MtkWmtControlInit(C.byref(control),C.byref(t))==0
  assert lib.MtkWmtGpsMcuBegin(C.byref(control),body,len(body),capacity)==3
 mcu_cases=0
 for n in range(1,1993):
  body=bytes((i*73+n)&255 for i in range(n));reply=bytes((i*57+n)&255 for i in range(n))
  response=bytes((2,0x32,n&255,n>>8))+reply
  assert original_mcu(body,response,n)==(0,n,reply)
  mcu_begin(body,n)
  assert requests==[bytes((1,0x32,n&255,n>>8))+body]
  assert written==[encoded(8,4,requests[0])]
  assert event(response)==0 and control.Confirmed and control.ReplyLength==n
  assert bytes(control.Reply[:n])==reply;mcu_cases+=1
 for n in (0,1,2,3,255,256,257,1995):
  reply=bytes((i*61+n)&255 for i in range(n));response=bytes((2,0x32,n&255,n>>8))+reply
  assert original_mcu(b'\x04\x02',response,max(1,n))==(0,n,reply)
  mcu_begin(b'\x04\x02',max(1,n));assert event(response)==0
  assert control.ReplyLength==n and bytes(control.Reply[:n])==reply;mcu_cases+=1
 # Stock accepts the wrong event-type byte and truncated actual body because it
 # tests only opcode. The port checks the complete event header and byte count.
 for response,actual in ((bytes((1,0x32,3,0,0,7,8)),7),(bytes((2,0x32,3,0,0,7,8)),4)):
  assert original_mcu(b'\x04\x02',response,3,actual)[0]==0
  mcu_begin(b'\x04\x02',3);assert event(response[:actual])==6 and not control.Confirmed
 print(f'Stock GPS MCU: {mcu_cases} opcode-0x32 comparisons passed',flush=True)
 gps=gps_power_save_compare(lib,t,control,event,written,encoded)
 print(f'Stock STP: {tx_cases} TX, {rx_cases} RX, {queue_cases} ring and {control_cases} WMT control comparisons passed',flush=True)
 return {'original_sha256':SHA,'functions':{n:{'section':sy[n][1],'offset':sy[n][2],'bytes':sy[n][3]} for n in selected},
  'relocations':relocations,'register_comparisons':register_cases,'gps_sync_comparisons':sync_cases,'register_header_status_original_accepts_port_rejects':True,'gps_sync_worker_error_original_swallows_port_rejects':True,'gps_sync_runtime_capability_established_for_board':False,'gps_mcu_comparisons':mcu_cases,'gps_power_save':gps,'short_or_wrong_type_mcu_reply_original_accepts_port_rejects':True,'ring_comparisons':queue_cases,'ring_wraps':wraps,'ring_overflow_original_drops_port_invalidates':True,'wmt_control_comparisons':control_cases,'short_wmt_reply_original_accepts_port_rejects':True,'tx_comparisons':tx_cases,'rx_comparisons':rx_cases,'fragmented_rx_comparisons':fragment_cases,
  'executed_virtual_services':sorted(executed),'modes':[2,8,16],
  'scope':'original ARM64 mandatory-mode TX bytes, regular-channel RX/notification routing, ring add/read wrap and capacity, WMT opcode-6 commands/ACK, opcode-8 masked register commands/read echo, conditional GPS sync and opcode-0x32 GPS MCU commands/replies compared. Parser queue edges are captured; ring tests execute the actual queue bodies. Physical callback and OS/power services virtual. SDIO RX tests use complete padded reads. No fullset ARQ, firmware dump, inband reset or physical controller execution.'}
def gps_power_save_compare(lib,transmitter,control,event,written,encoded):
 selected=('GPS_hw_suspend_ctrl',)
 raw,sec,sy,ranges,bases,u,stub,external,address,relocations=isolated_module(GPS,selected,0x21000000)
 assert hashlib.sha256(raw).hexdigest()==GPS_SHA
 dat=0x31000000;stack=0x41000000;stop=stub+0xf000
 u.mem_map(dat,0x10000);u.mem_map(stack,0x10000)
 traps={v:k for k,v in external.items()};requests=[];executed=set()
 response=bytes((0,7,8));actual=3;mcu_result=0
 noops={'ktime_get','sched_clock','_printk','down','up'}
 for name in noops|{'mtk_wmt_gps_mcu_ctrl'}:
  if name in sy:traps[address(sy[name])]=name
 def hook(uc,pc,size,ctx):
  name=traps.get(pc)
  if name:
   executed.add(name);result=0
   if name=='mtk_wmt_gps_mcu_ctrl':
    a=uc.reg_read(UC_ARM64_REG_X0);n=uc.reg_read(UC_ARM64_REG_X1)
    reply=uc.reg_read(UC_ARM64_REG_X2);capacity=uc.reg_read(UC_ARM64_REG_X3);count=uc.reg_read(UC_ARM64_REG_X4)
    assert n==2 and capacity==3 and 0<=actual<=capacity
    requests.append(bytes(uc.mem_read(a,n)));uc.mem_write(reply,response[:actual]);uc.mem_write(count,S.pack('<I',actual));result=mcu_result
   elif name not in noops:raise AssertionError(('unselected GPS service',name,hex(pc)))
   uc.reg_write(UC_ARM64_REG_X0,result&0xffffffffffffffff);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert any(bases[i]+lo<=pc<bases[i]+hi for i,lo,hi in ranges),('unselected GPS original body',hex(pc))
  if S.unpack('<I',uc.mem_read(pc,4))[0]==0xd5384108:
   uc.reg_write(UC_ARM64_REG_X8,dat+0xf000);uc.reg_write(UC_ARM64_REG_PC,pc+4)
 u.hook_add(UC_HOOK_CODE,hook)
 def original(suspend,firmware_control):
  requests.clear();u.reg_write(UC_ARM64_REG_X0,suspend);u.reg_write(UC_ARM64_REG_X1,firmware_control)
  u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,stack+0xfff0)
  u.emu_start(address(sy['GPS_hw_suspend_ctrl']),stop,count=100000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop
  v=u.reg_read(UC_ARM64_REG_X0)&0xffffffff;return v if v<0x80000000 else v-0x100000000
 def ported(suspend,firmware_control):
  written.clear();assert lib.MtkWmtControlInit(C.byref(control),C.byref(transmitter))==0
  assert lib.MtkWmtGpsPowerSaveBegin(C.byref(control),suspend,firmware_control)==3
  assert requests==[bytes((4 if suspend else 5,firmware_control))]
  assert written==[encoded(8,4,bytes((1,0x32,2,0))+requests[0])]
  return event(bytes((2,0x32,actual,0))+response[:actual])
 cases=0
 for suspend in (0,1):
  for firmware_control in range(256):
   assert original(suspend,firmware_control)==0 and ported(suspend,firmware_control)==0;cases+=1
 for status in range(256):
  response=bytes((status,7,8));old=original(1,2);new=ported(1,2)
  assert (old==0)==(new==0) and control.FirmwareStatus==status
  assert control.Confirmed==(status==0);cases+=1
 response=bytes((0,7,8))
 for n in range(4):
  actual=n;assert original(1,2)==0;new=ported(1,2)
  assert new==(0 if n>=2 else 6) and control.Confirmed==(n>=2);cases+=1
 print(f'Stock GPS power save: {cases} command/status comparisons passed',flush=True)
 return {'module_sha256':GPS_SHA,'functions':{n:{'section':sy[n][1],'offset':sy[n][2],'bytes':sy[n][3]} for n in selected},
  'comparisons':cases,'relocations':relocations,'executed_virtual_services':sorted(executed),
  'short_actual_reply_original_accepts_port_rejects':True,
  'scope':'original GPS_hw_suspend_ctrl request bytes and MCU status acceptance; time/locks/logging and MCU transport virtual. No GPS_open, LNA, MMIO, firmware startup or actual suspend/resume execution'}
def arm64_test(path,counter='MtkStpAssertionCount',minimum=1000000):
 raw=path.read_bytes();pe=S.unpack_from('<I',raw,0x3c)[0]
 assert raw[:2]==b'MZ' and raw[pe:pe+4]==b'PE\0\0';machine,count=S.unpack_from('<HH',raw,pe+4);assert machine==0xaa64
 opt=pe+24;optbytes=S.unpack_from('<H',raw,pe+20)[0];assert S.unpack_from('<H',raw,opt)[0]==0x20b
 entry=S.unpack_from('<I',raw,opt+16)[0];base=S.unpack_from('<Q',raw,opt+24)[0];size,headers=S.unpack_from('<II',raw,opt+56)
 export,exportbytes=S.unpack_from('<II',raw,opt+112);assert not any(S.unpack_from('<II',raw,opt+120))
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,(size+4095)&~4095);u.mem_write(base,raw[:headers])
 for i in range(count):
  at=opt+optbytes+i*40;vs,rva,rs,off=S.unpack_from('<IIII',raw,at+8);assert rva+max(vs,rs)<=size and off+rs<=len(raw)
  if rs:u.mem_write(base+rva,raw[off:off+rs])
 stop=0x60000000;stack=0x70000000;u.mem_map(stop,4096);u.mem_map(stack,65536)
 u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,stack+65520)
 u.emu_start(base+entry,stop,timeout=30000000,count=500000000)
 assert u.reg_read(UC_ARM64_REG_PC)==stop,'STP ARM64 tests exceeded bounded emulation'
 assert u.reg_read(UC_ARM64_REG_X0)==0,('STP ARM64 test failure at line',u.reg_read(UC_ARM64_REG_X0))
 def rd(at):return S.unpack('<I',u.mem_read(at,4))[0]
 def cstr(at):
  out=bytearray()
  while (v:=bytes(u.mem_read(at+len(out),1)))!=b'\0':assert len(out)<128;out+=v
  return out.decode()
 at=base+export;checks=None
 for i in range(rd(at+24)):
  if cstr(base+rd(base+rd(at+32)+i*4))==counter:
   ordinal=S.unpack('<H',u.mem_read(base+rd(at+36)+i*2,2))[0];checks=rd(base+rd(base+rd(at+28)+ordinal*4))
 assert checks and checks>minimum
 return {'assertions':checks,'scope':'actual production C and the same native test suite in ARM64 PE under Unicorn; no Windows, physical bus, RF or GPS operation'}
if '--mode' in sys.argv:
 args=[arg for arg in sys.argv[1:] if arg!='--locked']
 if len(args)!=2 or args[0]!='--mode':
  sys.exit(f'Usage: {Path(__file__).name} --mode NEW_OUTPUT_DIR')
 spec=importlib.util.spec_from_file_location('wmt_mode_binary',PKG/'Tests/wmt-mode-binary.py')
 helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
 helper.verify(R,Path(args[1]).resolve(),STOCK,SHA,module,isolated_module,arm64_test)
 sys.exit(0)
if '--full' in sys.argv:
 args=[arg for arg in sys.argv[1:] if arg!='--locked']
 if len(args)!=2 or args[0]!='--full':
  sys.exit(f'Usage: {Path(__file__).name} --full NEW_OUTPUT_DIR')
 spec=importlib.util.spec_from_file_location('stp_full_binary',PKG/'Tests/stp-full-binary.py')
 helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
 helper.verify(R,Path(args[1]).resolve(),STOCK,SHA,isolated_module,arm64_test)
 sys.exit(0)
if '--compare' in sys.argv:
 path=Path(sys.argv[sys.argv.index('--compare')+1]);lib=C.CDLL(str(path));comparison=stock_compare(lib)
 spec=importlib.util.spec_from_file_location('btif_binary',BTIF_HELPER);helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
 comparison['btif_fifo']=helper.compare(lib,isolated_module,STOCK.with_name('btif_drv.ko'))
 spec=importlib.util.spec_from_file_location('vcn28_binary',VCN28_HELPER);helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
 comparison['vcn28_power']=helper.compare(lib,isolated_module,module,STOCK)
 spec=importlib.util.spec_from_file_location('gnss_binary',GNSS_HELPER);helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
 comparison['gnss_inner_frames']=helper.compare(lib,STOCK.parents[1]/'vendor_a_gnss/libmnl.so')
 comparison['gnss_offload']=helper.compare_offload(lib,STOCK.parents[1]/'vendor_a_gnss/libmnl.so')
 comparison['gnss_consumer_records']=helper.compare_records(lib,STOCK.parents[1]/'vendor_a_gnss/android.hardware.gnss-impl-mediatek.so')
 comparison['gnss_mcu_position']=helper.compare_mcu_position(lib,STOCK.parents[1]/'vendor_a_gnss/libmnl.so')
 comparison['_software_NMEA']=helper.check_nmea(lib)
 comparison['_software_NMEA_STREAM']=helper.check_nmea_stream(lib)
 print(json.dumps(comparison));sys.exit(0)
@contextmanager
def build_directory():
 folder=tempfile.mkdtemp(prefix='stp-check-',dir=B)
 try:yield folder
 finally:
  # The mounted builder can briefly retain an empty directory after unlink.
  for attempt in range(8):
   try:
    shutil.rmtree(folder);break
   except FileNotFoundError:break
   except OSError as e:
    if e.errno not in (39,66) or attempt==7:raise
    time.sleep(0.5)
with build_directory() as folder:
 t=Path(folder);native=t/'StpTest';test=PKG/'Tests/MtkStpCoreTest.c';btif_test=PKG/'Tests/MtkBtifCoreTest.c';vcn28_test=PKG/'Tests/MtkVcn28CoreTest.c';gnss_test=PKG/'Tests/MtkGnssCoreTest.c';cc=['clang','-std=c11','-Wall','-Wextra','-Werror','-I',INC]
 native_result=run([*cc,'-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DSTP_TEST_NATIVE',SOURCE,test,'-o',native]);native_result=run([native]);print(native_result,flush=True)
 btif_native=t/'BtifTest';run([*cc,'-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DSTP_TEST_NATIVE',*SOURCES,btif_test,'-o',btif_native]);btif_native_result=run([btif_native]);print(btif_native_result,flush=True)
 vcn28_native=t/'Vcn28Test';run([*cc,'-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DSTP_TEST_NATIVE',VCN28_SOURCE,vcn28_test,'-o',vcn28_native]);vcn28_native_result=run([vcn28_native]);print(vcn28_native_result,flush=True)
 gnss_native=t/'GnssTest';run([*cc,'-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DSTP_TEST_NATIVE',SOURCE,GNSS_SOURCE,gnss_test,'-o',gnss_native]);gnss_native_result=run([gnss_native]);print(gnss_native_result,flush=True)
 shared=t/('libStp.dylib' if sys.platform=='darwin' else 'libStp.so');run([*cc,'-O2','-fPIC','-dynamiclib' if sys.platform=='darwin' else '-shared',*SOURCES,'-o',shared])
 output=run([sys.executable,__file__,'--locked','--compare',shared]).splitlines();print('\n'.join(output[:-1]),flush=True);comparison=json.loads(output[-1]);nmea=comparison.pop('_software_NMEA');nmea_stream=comparison.pop('_software_NMEA_STREAM')
 zig=os.environ.get('T70_MALI_ZIG');assert zig,'Set T70_MALI_ZIG to the existing Zig binary; no download is performed'
 flags=[*cc,'--target=aarch64-pc-windows-msvc','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-mstack-probe-size=65536','-fstack-usage','-c']
 obj=t/'StpCore.obj';btif_obj=t/'BtifCore.obj';testobj=t/'StpTest.obj';btif_testobj=t/'BtifTest.obj';vcn28_obj=t/'Vcn28Core.obj';vcn28_testobj=t/'Vcn28Test.obj';gnss_obj=t/'GnssCore.obj';gnss_testobj=t/'GnssTest.obj'
 for source,target in ((SOURCE,obj),(BTIF_SOURCE,btif_obj),(test,testobj),(btif_test,btif_testobj),(VCN28_SOURCE,vcn28_obj),(vcn28_test,vcn28_testobj),(GNSS_SOURCE,gnss_obj),(gnss_test,gnss_testobj)):
  run([*flags,*(['-mgeneral-regs-only'] if source==GNSS_SOURCE else []),source,'-o',target]);assert target.read_bytes()[:2]==b'\x64\xaa'
 frames={}
 for target in (obj,btif_obj,vcn28_obj,gnss_obj):
  for line in target.with_suffix('.su').read_text().splitlines():
   func,size,kind=line.rsplit('\t',2);frames[func.rsplit(':',1)[-1]]={'bytes':int(size),'kind':kind}
 assert all(v['bytes']<=2048 and v['kind']=='static' for v in frames.values())
 library=B/'MtkStpCore.lib';run([zig,'lib','/nologo','/machine:arm64','/out:'+str(library),obj,btif_obj,vcn28_obj,gnss_obj])
 exe=B/'MtkStpTests.exe';run([zig,'lld-link','/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib','/opt:ref','/export:MtkStpAssertionCount,DATA','/out:'+str(exe),obj,btif_obj,testobj])
 arm=arm64_test(exe);print(f'STP ARM64: {arm["assertions"]} assertions passed',flush=True)
 btif_exe=B/'MtkBtifTests.exe';run([zig,'lld-link','/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib','/opt:ref','/export:MtkBtifAssertionCount,DATA','/out:'+str(btif_exe),obj,btif_obj,btif_testobj])
 btif_arm=arm64_test(btif_exe,'MtkBtifAssertionCount',100000);print(f'BTIF ARM64: {btif_arm["assertions"]} assertions passed',flush=True)
 vcn28_exe=B/'MtkVcn28Tests.exe';run([zig,'lld-link','/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib','/opt:ref','/export:MtkVcn28AssertionCount,DATA','/out:'+str(vcn28_exe),vcn28_obj,vcn28_testobj])
 vcn28_arm=arm64_test(vcn28_exe,'MtkVcn28AssertionCount',1000);print(f'VCN28 ARM64: {vcn28_arm["assertions"]} assertions passed',flush=True)
 gnss_exe=B/'MtkGnssTests.exe';run([zig,'lld-link','/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native','/entry:TestMain','/nodefaultlib','/opt:ref','/export:MtkGnssAssertionCount,DATA','/out:'+str(gnss_exe),obj,gnss_obj,gnss_testobj])
 gnss_arm=arm64_test(gnss_exe,'MtkGnssAssertionCount',100000);print(f'GNSS ARM64: {gnss_arm["assertions"]} assertions passed',flush=True)
 elf=t/'StpCore.o';run([*cc,'--target=aarch64-linux-gnu','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-c',SOURCE,'-o',elf]);assert elf.read_bytes()[:6]==b'\x7fELF\x02\x01' and S.unpack_from('<H',elf.read_bytes(),18)[0]==183
 btif_elf=t/'BtifCore.o';run([*cc,'--target=aarch64-linux-gnu','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-c',BTIF_SOURCE,'-o',btif_elf])
 vcn28_elf=t/'Vcn28Core.o';run([*cc,'--target=aarch64-linux-gnu','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-c',VCN28_SOURCE,'-o',vcn28_elf])
 gnss_elf=t/'GnssCore.o';run([*cc,'--target=aarch64-linux-gnu','-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only','-c',GNSS_SOURCE,'-o',gnss_elf])
 archive=B/'libMtkStpCore.a';run([zig,'ar','rcs',archive,elf,btif_elf,vcn28_elf,gnss_elf])
 sources=[*SOURCES,gnss_test,GNSS_HELPER,INC/'Library/MtkGnssCore.h',btif_test,BTIF_HELPER,vcn28_test,VCN28_HELPER,INC/'Library/MtkVcn28Core.h',INC/'Library/MtkBtifCore.h',INC/'Library/MtkStpCore.h',SOURCE.with_name('MtkStpCoreLib.inf'),test,Path(__file__),R/'Silicon/MediaTek/MediaTekPkg/MediaTekPkg.dec',R/'Silicon/MediaTek/MediaTekPkg/MediaTekPkg.dsc.inc']
 report={'status':'passed','kind':'common mandatory STP frame encoder/stream decoder/transmitter, GPS byte ring/reset epochs, WMT function/register/MCU requests, conditional GPS sync and typed power-save responses/timeout ownership; BTIF FIFO MMIO/IRQ and STP backend; MT6358 VCN28 physical/lease core with DT always-on policy; bounded inner GNSS framing/escaping/checksum and MCU/offload fragment assembly, bounded MCU position views, typed stock location/SV consumer decoding with original callback comparisons and NMEA RMC/GGA output decoding and bounded epoch/line/pair stream handling; no GNSS position engine or OS/board resource provider',
  'native_asan_ubsan':native_result,'btif_native_asan_ubsan':btif_native_result,'vcn28_native_asan_ubsan':vcn28_native_result,'gnss_native_asan_ubsan':gnss_native_result,'original_arm64':comparison,'nmea_output_decode':nmea,'nmea_output_stream':nmea_stream,'arm64_pe':arm,'btif_arm64_pe':btif_arm,'vcn28_arm64_pe':vcn28_arm,'gnss_arm64_pe':gnss_arm,'production_stack_frames':frames,
  'sources':{str(p.relative_to(R)):digest(p) for p in sources},
  'artifacts':{p.name:{'path':str(p),'bytes':p.stat().st_size,'sha256':digest(p)} for p in (library,archive,exe,btif_exe,vcn28_exe,gnss_exe)},
  'compiler':run(['clang','--version']).splitlines()[0],'commands':commands,
  'scope_limits':['Consumer-record semantics are implemented; no raw receiver or 3168-byte MCU-record field interpretation, absolute fix-age policy, offload endpoint/mode discovery, OS location adapter or position engine','No BTIF OS mappings/clocks/IRQ registration or DMA provider, WMT/conninfra firmware startup or whole RF/OS power integration','No fullset ARQ, firmware dump or inband reset parser','No tablet access, GPS open/ioctl, RF enable or location collection'],
  'intentional_validation_changes':['Reject nonzero mandatory header byte 3/tail and reserved channel bit 7; stock parser logs or ignores these','Latch malformed framing/partial TX until explicit owner recovery','Preserve SDIO padding across split reads instead of dropping it','Require five actual WMT reply bytes; original accepts missing zero status from a zero-initialized buffer','Ignore unrelated WMT opcodes; reject unowned/expired replies. The wire has no request ID and still requires exactly-once ordered responses','Invalidate and wipe the GPS byte queue on overflow instead of silently dropping bytes; reject callbacks from previous reset epochs','Wipe consumed/reset queue bytes; stock queue leaves them in memory','Validate opcode-0x32 reply type, exact declared/actual length and caller capacity; stock checks only opcode and truncates or copies unread bytes','Require at least two actual power-save reply bytes and reject nonzero MCU status; stock accepts absent status/detail','Validate opcode-8 type/opcode/declared length/status and read address before publishing data; preserve unknown response bytes 5..7 as opaque','GPS sync requires established runtime SoC flag bit 7; propagate worker rejection/malformed/uncertain result instead of stock pin-control success on error']}
 target=R.parent/'analysis/gnss-stp-verification.json';target.write_text(json.dumps(report,indent=2)+'\n')
 print('Verified STP ARM64 libraries and source identities; temporary objects removed on exit',flush=True)
