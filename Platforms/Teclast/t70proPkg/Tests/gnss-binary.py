# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Selected stock libmnl ARM64 framing only, under a guarded virtual machine.
No constructors, engine init, daemon, syscalls, device access or collected location.
Consumer record tests use synthetic coordinates and bounded virtual callbacks.
This intentionally is not a general dynamic linker or Bionic runtime.
LLVM/Unicorn evidence, not REA Evidence.
"""
import ctypes as C
import hashlib
import struct as S
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,
 UC_ARM64_REG_X3,UC_ARM64_REG_X8,UC_ARM64_REG_LR,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_TPIDR_EL0)

class Decoder(C.Structure):
 _fields_=[(n,C.c_uint) for n in ('State','Used','Expected','Initialized','Faulted')]+[('Buffer',C.c_ubyte*512)]
class Frame(C.Structure):
 _fields_=[(n,C.c_uint) for n in ('Type','Flags','Length')]+[('Data',C.c_void_p),('Raw',C.c_void_p),('RawLength',C.c_uint)]

def _map_loads(u,raw,base):
 phoff=S.unpack_from('<Q',raw,32)[0];width,count=S.unpack_from('<HH',raw,54)
 assert width==56 and phoff+count*width<=len(raw)
 loads=[]
 for i in range(count):
  kind,flags,off,va,pa,filesz,memsz,align=S.unpack_from('<IIQQQQQQ',raw,phoff+i*width)
  if kind!=1:continue
  assert off+filesz<=len(raw) and filesz<=memsz and va+memsz<=0x9a0000
  u.mem_write(base+va,raw[off:off+filesz]);loads.append({'offset':off,'vaddr':va,'filesz':filesz,'memsz':memsz})
 return loads

class Offload(C.Structure):
 _fields_=[('Buffer',C.c_void_p)]+[(n,C.c_uint) for n in ('Capacity','Used','Total','Next','Epoch','Faulted')]
class Message(C.Structure):
 _fields_=[('Data',C.c_void_p),('Length',C.c_uint)]

def compare(lib,path):
 raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
 assert sha=='ae71dc763ef0f69ee2be01fde6dd7c48e4c4ddd2aab4cd3eeaa868d0abb3c319'
 assert raw[:6]==b'\x7fELF\x02\x01' and S.unpack_from('<HH',raw,16)==(3,183)
 selected={
  'mtk_gps_data_input':(0x551dd8,0x551fa4),
  'primary_frame_reset':(0x55d274,0x55d2c8),
  'frame_descriptor':(0x56b6b4,0x56b79c),
  'frame_serialize':(0x56b79c,0x56b830),
  'frame_checksum':(0x56b830,0x56b908),
  'primary_frame_read':(0x56b970,0x56bb4c),
  'primary_byte_input':(0x56bb4c,0x56be08),
  'escape_output':(0x6c4f9c,0x6c501c)}
 base=0x20000000;dat=0x30000000;stack=0x40000000;stop=0x50000000
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,0x9a0000)
 loads=_map_loads(u,raw,base)
 u.mem_map(dat,0x10000);u.mem_map(stack,0x10000);u.mem_map(stop,4096)
 # These are the complete GOT operands used by the selected receiver bodies.
 # Each is redirected to a separate virtual cell; no packed relocations,
 # constructors or runtime symbol resolver is executed.
 got=(0x7320f8,0x732100,0x732108,0x732110,0x732118,0x732120,
      0x7323d0,0x730598,0x7318f0,0x7315f8,0x731600,0x731608)
 cells={v:dat+0x4000+i*32 for i,v in enumerate(got)};cells[0x7323d0]=dat+0x6000
 for slot,target in cells.items():u.mem_write(base+slot,S.pack('<Q',target))
 frames=[];output=bytearray();services=set();pcs=set()
 traps={0x551fa4:'receiver_queue',0x72a7a0:'memset',0x51c574:'log',0x51f69c:'log',0x691820:'append',0x6916f4:'append_byte'}
 def hook(uc,pc,size,context):
  offset=pc-base;name=traps.get(offset)
  if name:
   services.add(name);a=uc.reg_read(UC_ARM64_REG_X0);b=uc.reg_read(UC_ARM64_REG_X1);c=uc.reg_read(UC_ARM64_REG_X2);result=0
   if name=='receiver_queue':
    assert 10<=b<=512;frames.append(bytes(uc.mem_read(a,b)))
   elif name=='memset':
    assert stack<=a and a+c<=stack+0x10000 and c==1024;uc.mem_write(a,bytes([b&255])*c);result=a
   elif name=='append':
    assert a==dat+0x3000 and c==2;output.extend(uc.mem_read(b,c))
   elif name=='append_byte':
    assert a==dat+0x3000 and b<=255;output.append(b)
   uc.reg_write(UC_ARM64_REG_X0,result);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert any(lo<=offset<hi for lo,hi in selected.values()),('unselected original instruction',hex(offset))
  pcs.add(offset)
 u.hook_add(UC_HOOK_CODE,hook)
 def call(offset,*args):
  for reg,v in zip((UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3),args):u.reg_write(reg,v)
  u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,stack+0xfff0)
  u.reg_write(UC_ARM64_REG_TPIDR_EL0,dat+0xf000)
  u.emu_start(base+offset,stop,count=100000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop,'bounded original GNSS emulation did not return'
  return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 def reset():
  for target in cells.values():u.mem_write(target,bytes(512 if target==dat+0x6000 else 16))
  frames.clear();call(0x55d274)
 def stock_input(data):
  assert len(data)<=1020;u.mem_write(dat,data);u.mem_write(dat+0x1000,b'\xff'*4)
  assert call(0x551dd8,dat,len(data),dat+0x1000)==0
  assert S.unpack('<I',u.mem_read(dat+0x1000,4))[0]==len(data)
 def descriptor(kind,payload):
  u.mem_write(dat+0x200, payload or b'\x00')
  assert call(0x56b6b4,dat+0x2000,kind,dat+0x200,len(payload))==0
 def original_encode(kind,payload):
  descriptor(kind,payload);output.clear();assert call(0x56b79c,dat+0x3000,dat+0x2000)==0
  return bytes(output)
 lib.MtkGnssDecoderInit.argtypes=[C.POINTER(Decoder)]
 lib.MtkGnssReceive.argtypes=[C.POINTER(Decoder),C.c_void_p,C.c_uint,C.POINTER(C.c_uint),C.POINTER(Frame)]
 lib.MtkGnssEncode.argtypes=[C.c_uint,C.c_void_p,C.c_uint,C.c_void_p,C.c_uint,C.POINTER(C.c_uint)]
 d=Decoder();f=Frame();consumed=C.c_uint();buf=C.create_string_buffer(1020);written=C.c_uint()
 def port_input(data):
  source=C.create_string_buffer(data)
  result=lib.MtkGnssReceive(C.byref(d),source,len(data),C.byref(consumed),C.byref(f))
  assert consumed.value==len(data)
  return result, C.string_at(f.Raw,f.RawLength) if result==3 else None
 def escaped(body):
  return b'\xaa\xf0'+body.replace(b'\xde',b'\xde\xe0').replace(b'\xaa',b'\xde\xdf')+b'\xaa\x0f'
 cases=encodes=reads=checksums=0
 lengths=(0,1,2,3,15,16,17,127,128,255,256,499,500,501,502)
 for n in lengths:
  for pattern in range(4):
   kind=(0,0xfe2f,0xaade,0xffff)[pattern]
   payload=bytes((i if pattern==0 else (0xaa if i&1 else 0xde) if pattern==1 else (i*37+n+pattern))&255 for i in range(n))
   wire=original_encode(kind,payload)
   source=C.create_string_buffer(payload)
   assert lib.MtkGnssEncode(kind,source,n,buf,1020,C.byref(written))==0
   assert buf.raw[:written.value]==wire;encodes+=1
   for flags in (0,1,7,15):
    length=(n+4)|(flags<<12);inner=S.pack('<HH',length,kind)+payload
    inner+=S.pack('<H',sum(inner)&65535);wire=escaped(inner);expected=b'\xaa\xf0'+inner+b'\xaa\x0f'
    for step in (len(wire),1,7):
     reset();assert lib.MtkGnssDecoderInit(C.byref(d))==0
     for at in range(0,len(wire),step):
      block=wire[at:at+step];stock_input(block);result,frame=port_input(block)
      final=at+len(block)==len(wire)
      assert result==(3 if final else 2)
      assert frames==([expected] if final else [])
      if final:assert frame==expected and f.Type==kind and f.Flags==flags and f.Length==n
     cases+=1;reads+=1
   descriptor(kind,payload)
   for checksum_ok in (True,False):
    if not checksum_ok:u.mem_write(dat+0x2000+14,S.pack('<H',(sum(S.pack('<HH',n+4,kind)+payload)+1)&65535))
    assert call(0x56b830,dat+0x2000,dat+0x1000)==0
    assert bytes(u.mem_read(dat+0x1000,1))==bytes([checksum_ok]);checksums+=1
 # Stock input framing defers checksum verification. The port rejects it before
 # handing data to its consumer and does not inherit ring overwrite recovery.
 bad=bytearray(original_encode(0,b'abc'));bad[-3]^=1
 reset();stock_input(bytes(bad));assert len(frames)==1
 assert lib.MtkGnssDecoderInit(C.byref(d))==0
 result,frame=port_input(bytes(bad));assert result==4 and frame is None and d.Faulted
 cases+=1
 return {'status':'passed','library_sha256':sha,'comparisons':cases+encodes+checksums,
  'receive_cases':reads,'encode_cases':encodes,'checksum_cases':checksums,'bad_checksum_ingress_difference_cases':1,
  'selected_functions':{name:{'vaddr':lo,'bytes':hi-lo,'executed_instruction_locations':sum(lo<=pc<hi for pc in pcs)} for name,(lo,hi) in selected.items()},
  'LOAD_segments':loads,'virtual_GOT_operands':[hex(x) for x in got],'executed_virtual_services':sorted(services),
  'format':{'start':'aa f0','end':'aa 0f','escape_aa':'de df','escape_de':'de e0','length':'low 12 bits, little-endian; payload + 4','opaque_flags':'high 4 length bits','type':'little-endian 16-bit, opaque','checksum':'16-bit sum of both length bytes, both type bytes and payload','decoded_frame_limit':512,'payload_limit':502},
  'intentional_differences':['Validate checksum before publishing; original framing queues it for a later engine checksum stage','Latch and wipe malformed/oversized streams instead of ring wrap or heuristic resync','Limit generated frames to the stock 512-byte primary receive-ring capacity; atomic output with caller capacity'],
  'scope':'Only selected original ARM64 framing/descriptor/checksum and the public data_input entry point; queue, logs, memset and string append are virtual. No constructors, engine init, Bionic runtime, syscalls, device, RF or location execution'}

def compare_offload(lib,path):
 """Selected offload assembly and actual NMEA callback routing, synthetic bytes.
 Endpoint discovery, offload selection, RF and position calculation never run.
 """
 raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
 assert sha=='ae71dc763ef0f69ee2be01fde6dd7c48e4c4ddd2aab4cd3eeaa868d0abb3c319'
 assert raw[:6]==b'\x7fELF\x02\x01' and S.unpack_from('<HH',raw,16)==(3,183)
 selected={
  'mtk_gps_ofl_ap_pack_data':(0x6fd608,0x6fd7ec),
  'mtk_gps_ofl_ap_nmea_recv':(0x6fcfb8,0x6fd01c),
  'read_byte':(0x52d064,0x52d078),'read_le16':(0x52d078,0x52d0a0),
  'nmea_output_dispatch':(0x536040,0x53619c)}
 base=0x20000000;dat=0x30000000;heap=0x31000000;stack=0x40000000;stop=0x50000000
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,0x9a0000);loads=_map_loads(u,raw,base)
 u.mem_map(dat,0x10000);u.mem_map(heap,0x80000);u.mem_map(stack,0x10000);u.mem_map(stop,4096)
 config=dat+0xa000;primary_cell=dat+0xa400;observer_cell=dat+0xa408;state=dat+0x9000
 got={0x7309d8:config,0x7319d0:primary_cell,0x730810:observer_cell}
 for slot,target in got.items():u.mem_write(base+slot,S.pack('<Q',target))
 u.mem_write(config+0x1f4,b'UseCallback')
 u.mem_write(primary_cell,S.pack('<Q',stop+0x100));u.mem_write(observer_cell,S.pack('<Q',stop+0x200))
 u.mem_write(dat+0xa800,b'virtual ENOMEM\0');u.mem_write(dat+0xa900,S.pack('<I',12))
 traps={base+0x72bb90:'pack_plt',base+0x51bc98:'allocate',base+0x51bc9c:'free',
  base+0x72a7a0:'memset',base+0x72a7b0:'memcpy',base+0x5373ac:'debug_disabled',
  base+0x72a7e0:'log',base+0x5339ac:'output_gate',base+0x72a860:'errno',base+0x72aa30:'strerror',
  stop+0x100:'nmea_callback',stop+0x200:'output_observer'}
 allocation=0;fail_alloc=False;gate=0;delivered=[];observed=[];services=set();pcs=set();allocations=frees=0
 def hook(uc,pc,size,context):
  nonlocal allocation,allocations,frees
  name=traps.get(pc)
  if name:
   services.add(name);a=uc.reg_read(UC_ARM64_REG_X0);b=uc.reg_read(UC_ARM64_REG_X1);c=uc.reg_read(UC_ARM64_REG_X2);result=0
   if name=='pack_plt':uc.reg_write(UC_ARM64_REG_PC,base+0x6fd608);return
   if name=='allocate':
    assert not allocation and 6<=a<=255*1532+6
    if not fail_alloc:
     allocation=a;allocations+=1;uc.mem_write(heap,b'\xee'*a);result=heap
   elif name=='free':
    assert a==heap and allocation;uc.mem_write(heap,bytes(allocation));allocation=0;frees+=1
   elif name=='memset':
    assert a==heap and allocation and c<=allocation;uc.mem_write(a,bytes([b&255])*c);result=a
   elif name=='memcpy':
    assert b==dat+4 and c<=1532 and heap+4<=a and a+c<=heap+allocation
    uc.mem_write(a,bytes(uc.mem_read(b,c)));result=a
   elif name in ('nmea_callback','output_observer'):
    assert a==heap+4 and b<=65535 and b+4<=allocation
    (delivered if name=='nmea_callback' else observed).append(bytes(uc.mem_read(a,b)))
   elif name=='output_gate':result=gate
   elif name=='errno':result=dat+0xa900
   elif name=='strerror':assert a==12;result=dat+0xa800
   elif name not in ('debug_disabled','log'):raise AssertionError(name)
   uc.reg_write(UC_ARM64_REG_X0,result);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  offset=pc-base
  assert any(lo<=offset<hi for lo,hi in selected.values()),('unselected original offload instruction',hex(pc))
  pcs.add(offset)
 u.hook_add(UC_HOOK_CODE,hook)
 def call(data):
  assert len(data)<=1536
  u.mem_write(dat,data+b'\xcc'*8)
  for reg,v in ((UC_ARM64_REG_X0,dat),(UC_ARM64_REG_X1,len(data)),(UC_ARM64_REG_X2,state),(UC_ARM64_REG_X3,0),
                (UC_ARM64_REG_LR,stop),(UC_ARM64_REG_SP,stack+0xfff0),(UC_ARM64_REG_TPIDR_EL0,dat+0xf000)):
   u.reg_write(reg,v)
  u.emu_start(base+0x6fcfb8,stop,count=100000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop,'bounded original offload emulation did not return'
  return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 lib.MtkGnssOffloadInit.argtypes=[C.POINTER(Offload),C.c_void_p,C.c_uint]
 lib.MtkGnssOffloadReset.argtypes=[C.POINTER(Offload),C.POINTER(C.c_uint)]
 lib.MtkGnssOffloadReceive.argtypes=[C.POINTER(Offload),C.c_uint,C.c_void_p,C.c_uint,C.POINTER(Message)]
 d=Offload();message=Message();buffer=C.create_string_buffer(65535);epoch=C.c_uint()
 assert lib.MtkGnssOffloadInit(C.byref(d),buffer,65535)==0
 def reset():
  nonlocal allocation
  if allocation:u.mem_write(heap,bytes(allocation));allocation=0
  u.mem_write(state,bytes(16));delivered.clear();observed.clear()
  assert lib.MtkGnssOffloadReset(C.byref(d),C.byref(epoch))==0
 def port(data):
  source=C.create_string_buffer(data)
  result=lib.MtkGnssOffloadReceive(C.byref(d),d.Epoch,source,len(data),C.byref(message))
  return result,C.string_at(message.Data,message.Length) if result==3 else None
 def fragment(total,index,data):return S.pack('<HBB',len(data)+4,total,index)+data
 cases=fragments=0
 for n in (0,1,2,3,4,15,255,1531,1532,1533,3064,3065,65534,65535):
  payload=bytes((i*37+n)&255 for i in range(n))
  for width in (1,7,1532):
   count=max(1,(n+width-1)//width)
   if count>255:continue
   reset()
   for i in range(count):
    part=fragment(count,i+1,payload[i*width:(i+1)*width]);assert call(part)==0
    result,output=port(part);final=i+1==count
    assert result==(3 if final else 2)
    assert delivered==observed==([payload] if final else [])
    assert (S.unpack('<Q',u.mem_read(state,8))[0]==0)==final
    if final:assert output==payload and not allocation
    fragments+=1
   cases+=1
 # The total counter is genuinely U8 and fragments need not have a fixed stride.
 for total in (2,3,42,43,127,254,255):
  reset();payload=b''
  for index in range(1,total+1):
   body=bytes([index&255])*(index%7);payload+=body
   assert call(fragment(total,index,body))==0
   result,output=port(fragment(total,index,body));final=index==total
   assert result==(3 if final else 2) and delivered==observed==([payload] if final else [])
   if final:assert output==payload and not allocation
   fragments+=1
  cases+=1
 # Safe virtual counterexamples expose stock's missing actual-length/order checks.
 bad_sequences=[
  [S.pack('<HBB',6,1,1)+b'A'],
  [fragment(3,1,b'A'),fragment(3,3,b'C')],
  [fragment(2,1,b'A'),fragment(1,1,b'B')],
  [fragment(3,1,b'A'),fragment(3,2,b'B'),fragment(3,2,b'B'),fragment(3,3,b'C')]]
 differences=0
 for sequence in bad_sequences:
  reset();fault=False
  for part in sequence:
   assert call(part)==0
   result,output=port(part)
   assert result in (2,4,5) and output is None
   fault|=result==4
  assert fault and d.Faulted and not any(buffer.raw)
  assert len(delivered)==len(observed)==1 and not allocation
  differences+=1
 # The stock dispatcher gate is a virtual service: this proves the route only,
 # not its real offload/config policy. No callback occurs when it disables output.
 reset();gate=1;assert call(fragment(1,1,b'synthetic'))==0
 assert not delivered and not observed and not allocation;gate=0
 reset();fail_alloc=True;assert call(fragment(1,1,b'synthetic'))==0xffffffff
 assert not delivered and not observed and not allocation;fail_alloc=False
 # The actual stock output callback's synthetic NMEA bytes feed the new parser.
 # NMEA numeric semantics are independently checked by check_nmea, not stock.
 lib.MtkGnssNmeaParse.argtypes=[C.c_void_p,C.c_uint,C.c_uint,C.POINTER(Nmea)]
 bridge_cases=0;merged_bridge_cases=0
 nmea_stream_api(lib)
 bridge_states={}
 for width in (1,7,1532):
  for source in range(2):
   nmea_state=NmeaStream();assert lib.MtkGnssNmeaStreamInit(C.byref(nmea_state),1900,1000)==0
   bridge_states[width,source]=nmea_state
 for text in (b'GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W',
              b'GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,'):
  checksum=0
  for value in text:checksum^=value
  payload=b'$'+text+f'*{checksum:02X}\r\n'.encode()
  for width in (1,7,1532):
   reset();count=(len(payload)+width-1)//width
   for i in range(count):
    part=fragment(count,i+1,payload[i*width:(i+1)*width]);assert call(part)==0
    result,output=port(part);assert result==(3 if i+1==count else 2);fragments+=1
   assert delivered==observed==[payload] and output==payload and not allocation
   for source,value in enumerate((delivered[0],output)):
    report=Nmea();assert lib.MtkGnssNmeaParse(C.create_string_buffer(value),len(value),1900,C.byref(report))==0
    assert report.Location.LatitudeBits==0x40480f03afb7e910 and report.Location.LongitudeBits==0x4027088888888889
    nmea_state=bridge_states[width,source];consumed=C.c_uint()
    assert lib.MtkGnssNmeaStreamReceive(C.byref(nmea_state),nmea_state.Epoch,10 if report.Kind==1 else 20,C.create_string_buffer(value),len(value),C.byref(consumed),C.byref(report))==3
    assert consumed.value==len(value)
    if text.startswith(b'GPGGA'):
     assert report.Kind==3 and report.Location.UtcMilliseconds==764426119000
     assert report.Location.Flags==15 and report.Location.AltitudeBits==0x4082826666666666
     assert not nmea_state.HaveRmc and not nmea_state.HaveGga;merged_bridge_cases+=1
   cases+=1;bridge_cases+=1
 return {'status':'passed','library_sha256':sha,'comparisons':fragments+differences+2,
  'complete_message_cases':cases,'nmea_output_decode_bridge_cases':bridge_cases,'stream_merged_output_bridge_cases':merged_bridge_cases,'fragment_comparisons':fragments,'intentional_validation_difference_cases':differences,
  'disabled_output_gate_cases':1,'allocation_failure_cases':1,'successful_virtual_allocations':allocations,'original_free_calls':frees,
  'selected_functions':{name:{'vaddr':lo,'bytes':hi-lo,'executed_instruction_locations':sum(lo<=pc<hi for pc in pcs)} for name,(lo,hi) in selected.items()},
  'LOAD_segments':loads,'virtual_GOT_operands':[hex(x) for x in got],'executed_virtual_services':sorted(services),
  'format':{'header':'LE16 fragment length including header, U8 total, U8 1-based index','fragment_body_max':1532,
   'assembled_output_length_bits':16,'assembled_output_max':65535,'output':'Actual nmea_recv -> pack_data -> output dispatcher -> registered callback and output observer; bytes remain opaque'},
  'intentional_differences':['Require actual length equal declared length','Require consistent total and exactly-once consecutive fragment indices; reject replacement of an incomplete message','Wipe/latch malformed, capacity overflow and reset; reject stale callbacks by owner epoch','Use caller-owned capacity with no heap, 16-bit count wrap, or unbounded append'],
  'scope':'Selected original ARM64 offload reassembly, byte readers, NMEA receiver and callback dispatcher only. Alloc/free/memcpy/memset/log/debug/gate and callbacks are bounded virtual services. Synthetic bytes only; the portable RMC/GGA parser consumes six completed callback messages; separate stream states for original callback and portable bytes produce six merged reports. Stream semantics are software checks, not original position-engine comparisons. No original position engine/parser, daemon, endpoint open, port/mode discovery, syscalls, RF or collected location'}


class Location(C.Structure):
 _fields_=[('Flags',C.c_uint32),('Reserved',C.c_uint32)]+[(n,C.c_uint64) for n in (
  'LatitudeBits','LongitudeBits','AltitudeBits','SpeedBits','BearingBits','HorizontalAccuracyBits',
  'VerticalAccuracyBits','SpeedAccuracyBits','BearingAccuracyBits')]+[('UtcMilliseconds',C.c_int64),
  ('ElapsedFlags',C.c_uint32),('Reserved2',C.c_uint32),('ElapsedTimestampNs',C.c_uint64),('ElapsedUncertaintyBits',C.c_uint64)]
class Satellite(C.Structure):
 _fields_=[('Svid',C.c_int32),('Constellation',C.c_int32)]+[(n,C.c_uint32) for n in (
  'Cn0Bits','BasebandCn0Bits','ElevationBits','AzimuthBits')]+[('CarrierFrequencyHz',C.c_uint64),('Flags',C.c_uint32),('Reserved',C.c_uint32)]

def compare_records(lib,path):
 """The exact stock location converter and entire SV callback, virtual sink.
 Only synthetic consumer records; no GNSS receiver/engine or Android runtime.
 """
 import random
 raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
 assert sha=='968f41196e50b2be082fc105435962f0676c75dcbe059f932d1a27fbaef28ef2'
 assert raw[:6]==b'\x7fELF\x02\x01' and S.unpack_from('<HH',raw,16)==(3,183)
 assert C.sizeof(Location)==112 and C.sizeof(Satellite)==40
 selected={'convertToGnssLocation(GpsLocation_ext*)':(0x3ffb0,0x40020),
  'AidlGnss::gnssSvStatusCb(GnssSvStatus_ext*)':(0x23310,0x2364c)}
 base=0x20000000;dat=0x30000000;heap=0x31000000;stack=0x40000000;stop=0x50000000
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,0x9a0000);loads=_map_loads(u,raw,base)
 u.mem_map(dat,0x20000);u.mem_map(heap,0x100000);u.mem_map(stack,0x10000);u.mem_map(stop,4096)
 got={0x4d368:dat+0x18000,0x4d328:dat+0x18010,0x4d330:dat+0x18020}
 for slot,target in got.items():u.mem_write(base+slot,S.pack('<Q',target))
 u.mem_write(dat+0x18000,b'\x01');u.mem_write(dat+0x18020,S.pack('<Q',dat+0x18100))
 u.mem_write(dat+0x18100,S.pack('<Q',dat+0x18200));u.mem_write(dat+0x18240,S.pack('<Q',stop+0x100))
 services=set();pcs=set();allocated={};cursor=0;allocations=frees=0;delivered=[]
 traps={base+0x4a000:'vector_allocate',base+0x4a020:'vector_free',base+0x4a200:'memcpy',
  base+0x4a040:'mutex_lock',base+0x4a080:'mutex_unlock',base+0x4a060:'log',
  base+0x4a050:'status_ok',base+0x4a070:'status_delete',stop+0x100:'satellite_sink'}
 def hook(uc,pc,size,context):
  nonlocal cursor,allocations,frees
  name=traps.get(pc)
  if name:
   services.add(name);a=uc.reg_read(UC_ARM64_REG_X0);b=uc.reg_read(UC_ARM64_REG_X1);c=uc.reg_read(UC_ARM64_REG_X2);result=0
   if name=='vector_allocate':
    assert 0<a<=128*40 and cursor+a<=0x100000
    result=heap+cursor;cursor+=(a+63)&~63;allocated[result]=a;allocations+=1;uc.mem_write(result,b'\xee'*a)
   elif name=='vector_free':
    assert allocated.pop(a)==b;uc.mem_write(a,bytes(b));frees+=1
   elif name=='memcpy':
    assert any(lo<=a and a+c<=lo+n for lo,n in allocated.items())
    assert not c or any(lo<=b and b+c<=lo+n for lo,n in allocated.items())
    if c:uc.mem_write(a,bytes(uc.mem_read(b,c)))
    result=a
   elif name=='satellite_sink':
    assert a==dat+0x18100 and stack<=b<stack+0x10000
    first,last,end=S.unpack('<QQQ',uc.mem_read(b,24));assert first<=last<=end and (last-first)%40==0
    assert not first or allocated[first]==end-first
    delivered.append(bytes(uc.mem_read(first,last-first)) if first else b'')
    ret=uc.reg_read(UC_ARM64_REG_X8);assert stack<=ret<=stack+0xfff8
    uc.mem_write(ret,S.pack('<Q',dat+0x18300))
   elif name=='status_ok':assert a==dat+0x18300;result=1
   elif name=='status_delete':assert a==dat+0x18300
   elif name.startswith('mutex_'):assert a==dat+0x18010
   elif name!='log':raise AssertionError(name)
   uc.reg_write(UC_ARM64_REG_X0,result);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  off=pc-base;assert any(lo<=off<hi for lo,hi in selected.values()),('unselected GNSS consumer instruction',hex(off))
  pcs.add(off)
 u.hook_add(UC_HOOK_CODE,hook)
 def call(at,argument):
  u.reg_write(UC_ARM64_REG_X0,argument);u.reg_write(UC_ARM64_REG_X8,dat+0x10000)
  u.reg_write(UC_ARM64_REG_SP,stack+0xfff0);u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_TPIDR_EL0,dat+0x19000)
  u.emu_start(base+at,stop,count=200000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop,'bounded original GNSS consumer did not return'
 def original_location(data):
  u.mem_write(dat,data);call(0x3ffb0,dat);out=bytearray(u.mem_read(dat+0x10000,112))
  flags=S.unpack_from('<I',out)[0]
  for mask,at,n in ((1,8,16),(2,24,8),(4,32,8),(8,40,8),(16,48,8),(32,56,8),(64,64,8),(128,72,8)):
   if not flags&mask:out[at:at+n]=bytes(n)
  elapsed=S.unpack_from('<I',out,88)[0]
  if not elapsed&1:out[96:104]=bytes(8)
  if not elapsed&2:out[104:112]=bytes(8)
  return bytes(out)
 lib.MtkGnssDecodeLocation.argtypes=[C.c_void_p,C.c_uint,C.POINTER(Location)]
 lib.MtkGnssDecodeSvStatus.argtypes=[C.c_void_p,C.c_uint,C.POINTER(Satellite),C.c_uint,C.POINTER(C.c_uint)]
 rng=random.Random(0x6789);location_cases=sv_cases=0
 def location(flags,elapsed,uncertainty,speed=0x00000001):
  b=bytearray(b'\xa5'*104);S.pack_into('<H',b,8,flags)
  S.pack_into('<ddd',b,16,-45.25,135.,-3.);S.pack_into('<II',b,40,speed,0x43b38000)
  S.pack_into('<q',b,56,-123456789 if flags&1 else 1700000000123)
  S.pack_into('<IIII',b,64,0x007fffff,0x00800000,0x7f7fffff,0x80000000)
  S.pack_into('<H',b,80,elapsed);S.pack_into('<QQ',b,88,1234567890123,uncertainty)
  return bytes(b)
 for flags in range(256):
  for elapsed in (0,1,3):
   data=location(flags,elapsed,rng.getrandbits(64));out=Location();source=C.create_string_buffer(data)
   assert lib.MtkGnssDecodeLocation(source,104,C.byref(out))==0
   assert bytes(out)==original_location(data),(flags,elapsed);location_cases+=1
 for value in (0,1,2,3,2**52-1,2**52,2**53-1,2**53,2**53+1,2**53+2,2**53+3,2**63-1,2**64-1,*[rng.getrandbits(64) for _ in range(128)]):
  data=location(255,3,value,speed=rng.randrange(0x7f800000));out=Location()
  assert lib.MtkGnssDecodeLocation(C.create_string_buffer(data),104,C.byref(out))==0
  assert bytes(out)==original_location(data),hex(value);location_cases+=1
 # Actual original SIMD double->float narrowing, signed SVID, scalar truncation,
 # vector growth/copy/free, lock and final callback. OS services remain virtual.
 boundaries=(0,0x8000000000000000,0x3690000000000000,0x3690000000000001,0x36a0000000000000,
  0x380fffffe0000000,0x4042800010000000,0x4042800030000000,0x47efffffe0000000)
 for count in (0,1,2,7,16,35,64,128):
  for pattern in range(3):
   data=bytearray(b'\xa5'*(16+48*count));S.pack_into('<i',data,8,count)
   for i in range(count):
    at=16+48*i;S.pack_into('<hB',data,at+8,7 if i&1 else 12,i%8)
    S.pack_into('<fff',data,at+12,37.25,-90.0+(i%181),float(i%361))
    data[at+24]=(0,7,15)[pattern];S.pack_into('<I',data,at+32,(1,0x4ebbcf26,0x4ebbcf26)[pattern])
    S.pack_into('<Q',data,at+40,boundaries[i%len(boundaries)] if pattern==0 else S.unpack('<Q',S.pack('<d',rng.random()*63))[0])
   out=(Satellite*max(count,1))();n=C.c_uint(0xfeed)
   assert lib.MtkGnssDecodeSvStatus(C.create_string_buffer(bytes(data)),len(data),out,count,C.byref(n))==0 and n.value==count
   assert not allocated;cursor=0;delivered.clear();u.mem_write(dat,bytes(data));call(0x23310,dat)
   assert len(delivered)==1 and not allocated
   expected=bytearray(delivered[0])
   for i in range(count):
    if not data[16+48*i+24]&8:expected[i*40+24:i*40+32]=bytes(8)
   assert bytes(out)[:count*40]==expected,(count,pattern);sv_cases+=1
 assert allocations==frees
 call(0x3ffb0,0);assert bytes(u.mem_read(dat+0x10000,112))==bytes(112)
 return {'status':'passed','library_sha256':sha,'comparisons':location_cases+sv_cases+1,
  'location_cases':location_cases,'satellite_callback_cases':sv_cases,'null_original_location_case':1,
  'successful_virtual_allocations':allocations,'original_vector_free_calls':frees,
  'selected_functions':{n:{'vaddr':lo,'bytes':hi-lo,'executed_instruction_locations':sum(lo<=pc<hi for pc in pcs)} for n,(lo,hi) in selected.items()},
  'LOAD_segments':loads,'virtual_GOT_operands':[hex(v) for v in got],'executed_virtual_services':sorted(services),
  'intentional_differences':['Check exact location length and actual SV array bytes/capacity before reads; original receives trusted in-process structs',
   'Reject unknown validity flags, nonfinite/present out-of-range values, negative count and malformed entries before any output write',
   'Zero absent location/timing/carrier-frequency fields instead of copying stale bytes; preserve opaque size/padding words without guessing a version',
   'Require elapsed uncertainty to accompany a signed-range timestamp; carry signed UTC unchanged and perform IEEE conversions using integer arithmetic'],
  'scope':'Exact stock location converter and full SV callback including actual vector growth and callback dispatch. Alloc/free/memcpy/log/mutex/AStatus/callback are bounded virtual services; no Android runtime, Binder, engine, endpoint, RF or collected location'}

def compare_mcu_position(lib,path):
 raw=path.read_bytes();sha=hashlib.sha256(raw).hexdigest()
 assert sha=='ae71dc763ef0f69ee2be01fde6dd7c48e4c4ddd2aab4cd3eeaa868d0abb3c319'
 selected={'mcu_dispatch_header':(0x6fb2a8,0x6fb36c),'mcu_kind2_copy':(0x6fb4bc,0x6fb4e4),
  'mcu_mark_ready':(0x6fb868,0x6fb878),'mcu_dispatch_return':(0x6fbf30,0x6fbf5c),
  'mtk_gps_get_position_from_mcu':(0x6fade0,0x6fae24),'read_le16':(0x52d078,0x52d0a0)}
 base=0x20000000;dat=0x30000000;stack=0x40000000;stop=0x50000000
 u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(base,0x9a0000);loads=_map_loads(u,raw,base)
 u.mem_map(dat,0x10000);u.mem_map(stack,0x10000);u.mem_map(stop,4096)
 got={0x732e58:dat+0x3000,0x732e60:dat+0x4000}
 for slot,target in got.items():u.mem_write(base+slot,S.pack('<Q',target))
 services=set();pcs=set()
 traps={0x538a30:'trace',0x51f69c:'log',0x72a7b0:'memcpy'}
 def hook(uc,pc,size,context):
  off=pc-base;name=traps.get(off)
  if name:
   services.add(name);result=0
   if name=='memcpy':
    a=uc.reg_read(UC_ARM64_REG_X0);b=uc.reg_read(UC_ARM64_REG_X1);c=uc.reg_read(UC_ARM64_REG_X2)
    assert (a,b) in ((dat+0x3000,dat+10),(dat+0x5000,dat+0x3000)) and c==3168
    uc.mem_write(a,bytes(uc.mem_read(b,c)));result=a
   uc.reg_write(UC_ARM64_REG_X0,result);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert any(lo<=off<hi for lo,hi in selected.values()),('unselected MCU position instruction',hex(off));pcs.add(off)
 u.hook_add(UC_HOOK_CODE,hook)
 def call(at,arg):
  u.reg_write(UC_ARM64_REG_X0,arg);u.reg_write(UC_ARM64_REG_SP,stack+0xfff0);u.reg_write(UC_ARM64_REG_LR,stop)
  u.reg_write(UC_ARM64_REG_TPIDR_EL0,dat+0xf000);u.emu_start(base+at,stop,count=10000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop;return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 lib.MtkGnssMcuPosition.argtypes=[C.POINTER(Message),C.POINTER(Message),C.POINTER(C.c_uint)]
 assert call(0x6fade0,dat+0x5000)==0 and call(0x6fade0,0)==0
 cases=2
 for pattern in range(32):
  for opaque in (0,0xdea0,0xffff):
   body=bytes((i*37+pattern*13)&255 for i in range(3168))
   payload=S.pack('<HHH',2,3168,opaque)+body;u.mem_write(dat,S.pack('<HH',0,len(payload))+payload)
   u.mem_write(dat+0x4000,b'\0');assert call(0x6fb2a8,dat)==0 and bytes(u.mem_read(dat+0x4000,1))==b'\1'
   source=C.create_string_buffer(payload);message=Message(C.addressof(source),len(payload));position=Message();unknown=C.c_uint()
   assert lib.MtkGnssMcuPosition(C.byref(message),C.byref(position),C.byref(unknown))==0
   assert unknown.value==opaque and position.Length==3168
   for _ in range(2):
    assert call(0x6fade0,dat+0x5000)==1
    assert bytes(u.mem_read(dat+0x5000,3168))==C.string_at(position.Data,position.Length)==body;cases+=1
 return {'status':'passed','library_sha256':sha,'comparisons':cases,'message_cases':96,
  'selected_functions':{n:{'vaddr':lo,'bytes':hi-lo,'executed_instruction_locations':sum(lo<=pc<hi for pc in pcs)} for n,(lo,hi) in selected.items()},
  'LOAD_segments':loads,'virtual_GOT_operands':[hex(v) for v in got],'executed_virtual_services':sorted(services),
  'format':{'kind':2,'body_bytes':3168,'payload_header_bytes':6,'header_bytes_4_5':'opaque LE16'},
  'scope':'Only MCU kind-2 dispatch/copy/ready and getter with synthetic bytes; trace/log/memcpy virtual. No other MCU event, position-record interpretation, engine, endpoint or RF. Original ready flag remains set on repeated getters; portable view does not assert freshness'}


class Nmea(C.Structure):
 _fields_=[('Location',Location)]+[(n,C.c_uint) for n in (
  'Kind','AuxFlags','TimeOfDayMilliseconds','Quality','SatellitesUsed','Year','Month','Day','Mode')]+[(n,C.c_uint64) for n in (
  'HdopBits','MslAltitudeBits','GeoidSeparationBits','TimeOfDayNanoseconds')]+[('LatitudeNanominutes',C.c_int64),('LongitudeNanominutes',C.c_int64),('LatitudeStepNanominutes',C.c_uint),('LongitudeStepNanominutes',C.c_uint)]

def check_nmea(lib):
 """Independent Fraction/datetime numeric oracle, not original ARM64 evidence."""
 from fractions import Fraction
 import datetime as dt
 import random
 assert C.sizeof(Nmea)==208
 lib.MtkGnssNmeaParse.argtypes=[C.c_void_p,C.c_uint,C.c_uint,C.POINTER(Nmea)]
 rng=random.Random(0x1800);cases=0
 def bits(value):return S.unpack('<Q',S.pack('<d',float(value)))[0]
 def decimal(n):return ('-' if n<0 else '')+str(abs(n)//10**9)+f'.{abs(n)%10**9:09d}'
 def coordinate(degree,minute,longitude):return f'{degree:0{3 if longitude else 2}d}{minute//10**9:02d}.{minute%10**9:09d}'
 def sentence(text):
  b=text.encode();check=0
  for v in b:check^=v
  return b'$'+b+f'*{check:02X}\r\n'.encode()
 for i in range(512):
  lat_degree=i%91;lon_degree=i%181
  lat_min=0 if lat_degree==90 else rng.randrange(60*10**9)
  lon_min=0 if lon_degree==180 else rng.randrange(60*10**9)
  lat=coordinate(lat_degree,lat_min,False);lon=coordinate(lon_degree,lon_min,True)
  ns='S' if i&1 else 'N';ew='W' if i&2 else 'E'
  lat_bits=bits((lat_degree+Fraction(lat_min,60*10**9))*(-1 if ns=='S' else 1))
  lon_bits=bits((lon_degree+Fraction(lon_min,60*10**9))*(-1 if ew=='W' else 1))
  speed=rng.randrange(5000*10**9);bearing=rng.randrange(360*10**9)
  hour=i%24;minute=i%60;second=i%60;fraction=rng.randrange(10**9)
  time=f'{hour:02d}{minute:02d}{second:02d}.{fraction:09d}'
  year=2000+i%100;month=i%12+1;day=i%28+1
  expected_ms=(dt.datetime(year,month,day,hour,minute,second)-dt.datetime(1970,1,1)).days*86400000+(hour*3600+minute*60+second)*1000+fraction//10**6
  active=i%5!=0;have_speed=i%3!=0;have_bearing=i%7!=0
  data=sentence(f'GNRMC,{time},{"A" if active else "V"},{lat},{ns},{lon},{ew},{decimal(speed) if have_speed else ""},{decimal(bearing) if have_bearing else ""},{day:02d}{month:02d}{year%100:02d},,,A')
  r=Nmea();assert lib.MtkGnssNmeaParse(C.create_string_buffer(data),len(data),2000,C.byref(r))==0
  assert r.Kind==1 and r.AuxFlags==33 and (r.Year,r.Month,r.Day)==(year,month,day)
  assert r.Location.UtcMilliseconds==expected_ms and r.Mode==ord('A')
  assert r.TimeOfDayNanoseconds==(hour*3600+minute*60+second)*10**9+fraction
  assert r.Location.Flags==((1+(4 if have_speed else 0)+(8 if have_bearing else 0)) if active else 0)
  if active:
   assert (r.Location.LatitudeBits,r.Location.LongitudeBits)==(lat_bits,lon_bits)
   assert r.LatitudeNanominutes==(lat_degree*60*10**9+lat_min)*(-1 if ns=='S' else 1)
   assert r.LongitudeNanominutes==(lon_degree*60*10**9+lon_min)*(-1 if ew=='W' else 1)
   assert r.LatitudeStepNanominutes==r.LongitudeStepNanominutes==1
   assert r.Location.SpeedBits==(bits(Fraction(speed*463,900*10**9)) if have_speed else 0)
   assert r.Location.BearingBits==(bits(Fraction(bearing,10**9)) if have_bearing else 0)
  else:assert not r.Location.LatitudeBits and not r.Location.LongitudeBits and not r.Location.SpeedBits and not r.Location.BearingBits
  assert not r.Location.AltitudeBits and not r.Location.HorizontalAccuracyBits;cases+=1
  quality=i%9;satellites=i%129;hdop=rng.randrange(100*10**9)
  msl=rng.randrange(-1000*10**9,10000*10**9);geoid=rng.randrange(-200*10**9,200*10**9)
  data=sentence(f'GPGGA,{time},{lat},{ns},{lon},{ew},{quality},{satellites},{decimal(hdop)},{decimal(msl)},M,{decimal(geoid)},M,,')
  r=Nmea();assert lib.MtkGnssNmeaParse(C.create_string_buffer(data),len(data),0,C.byref(r))==0
  assert r.Kind==2 and r.AuxFlags==(46 if quality else 32) and r.Quality==quality and r.SatellitesUsed==satellites
  assert r.Location.Flags==(3 if quality else 0) and not r.Location.UtcMilliseconds and not r.Year
  if quality:
   assert (r.Location.LatitudeBits,r.Location.LongitudeBits)==(lat_bits,lon_bits)
   assert r.LatitudeNanominutes==(lat_degree*60*10**9+lat_min)*(-1 if ns=='S' else 1)
   assert r.LongitudeNanominutes==(lon_degree*60*10**9+lon_min)*(-1 if ew=='W' else 1)
   assert r.LatitudeStepNanominutes==r.LongitudeStepNanominutes==1
   assert r.Location.AltitudeBits==bits(Fraction(msl+geoid,10**9))
   assert r.MslAltitudeBits==bits(Fraction(msl,10**9)) and r.GeoidSeparationBits==bits(Fraction(geoid,10**9))
   assert r.HdopBits==bits(Fraction(hdop,10**9)) and not r.Location.HorizontalAccuracyBits
  else:assert not r.Location.LatitudeBits and not r.Location.LongitudeBits and not r.Location.AltitudeBits and not r.HdopBits
  cases+=1
 return {'status':'passed','synthetic_numeric_cases':cases,'original_ARM64_comparisons':False,
  'reference':'Independent Python Fraction correctly rounded binary64 and datetime calendar; public NMEA RMC/GGA definitions',
  'scope':'Complete checksum-verified CRLF sentences; explicit century, immutable per-sentence output, exact coordinate/knots/metres conversion, RMC invalidation and separate GGA time/MSL/geoid/HDOP. This numeric check excludes stream semantics, endpoint, engine, OS adapter and collected location'}


class NmeaStream(C.Structure):
 _fields_=[(n,C.c_uint) for n in ('Century','PairWindowMs','Epoch','Faulted','Used','Initialized','ClockValid','HaveRmc','HaveGga','HaveChronology')]+[(n,C.c_uint64) for n in ('LastSeenMs','LineStartMs','RmcAtMs','GgaAtMs')]+[('LastRmcDay',C.c_int64),('LastRmcTimeNs',C.c_uint64),('Rmc',Nmea),('Gga',Nmea),('Line',C.c_ubyte*256)]

def nmea_stream_api(lib):
 assert C.sizeof(NmeaStream)==760
 lib.MtkGnssNmeaStreamInit.argtypes=[C.POINTER(NmeaStream),C.c_uint,C.c_uint]
 lib.MtkGnssNmeaStreamReset.argtypes=[C.POINTER(NmeaStream),C.c_uint,C.POINTER(C.c_uint)]
 lib.MtkGnssNmeaStreamReceive.argtypes=[C.POINTER(NmeaStream),C.c_uint,C.c_uint64,C.c_void_p,C.c_uint,C.POINTER(C.c_uint),C.POINTER(Nmea)]

def check_nmea_stream(lib):
 """Chunking, invalidation and pairing checks; no original position engine."""
 import random
 from fractions import Fraction
 nmea_stream_api(lib);rng=random.Random(0x1810);cases=0;calls=0
 def sentence(text):
  body=text.encode();checksum=0
  for byte in body:checksum^=byte
  return b'$'+body+f'*{checksum:02X}\r\n'.encode()
 def receive(state,data,now,epoch=None):
  nonlocal calls
  report=Nmea();C.memset(C.byref(report),0xa5,C.sizeof(report));before=bytes(report);used=C.c_uint(0xfeed)
  buf=C.create_string_buffer(data) if data else None
  status=lib.MtkGnssNmeaStreamReceive(C.byref(state),state.Epoch if epoch is None else epoch,now,buf,len(data),C.byref(used),C.byref(report));calls+=1
  assert used.value<=min(len(data),256)
  if status!=3:assert bytes(report)==before
  return status,used.value,report
 def reset(state):
  old=state.Epoch;epoch=C.c_uint()
  assert lib.MtkGnssNmeaStreamReset(C.byref(state),1900,C.byref(epoch))==0 and epoch.value==old+1
  assert not state.Used and not state.HaveRmc and not state.HaveGga and not state.HaveChronology
  assert not any(bytes(state.Rmc)+bytes(state.Gga)+bytes(state.Line))
 def bits(value):return S.unpack('<Q',S.pack('<d',float(value)))[0]
 for i in range(512):
  state=NmeaStream();assert lib.MtkGnssNmeaStreamInit(C.byref(state),1900,1000)==0
  precision=i%10;fraction=rng.randrange(10**precision) if precision else 0
  lat='4807'+(f'.{fraction:0{precision}d}' if precision else '')
  negative=bool(i&1);hemisphere='S' if negative else 'N';ns=i*1999
  time=f'123519.{ns:09d}'
  rmc=sentence(f'GNRMC,{time},A,{lat},{hemisphere},01131.000,E,22.4,84.4,230394,,,A')
  gga=sentence(f'GPGGA,{time},{lat},{hemisphere},01131.000,E,1,08,0.9,545.4,M,46.9,M,,')
  unknown=sentence('GNGSV,1,1,00')
  expected_kinds=[2,3] if i&2 else [1,3]
  data=b'\r\n'+(gga+unknown+rmc if i&2 else rmc+unknown+gga);reports=[];now=10
  while data:
   amount=rng.randrange(1,min(83,len(data))+1);part=data[:amount];offset=0
   while offset<len(part):
    status,used,report=receive(state,part[offset:],now);assert used and status in (2,3,8)
    offset+=used
    if status==3:reports.append(report)
   data=data[amount:];now+=1
  assert [r.Kind for r in reports]==expected_kinds
  report=reports[-1];expected_minutes=48*60*10**9+7*10**9+fraction*10**(9-precision)
  assert report.TimeOfDayNanoseconds==45319*10**9+ns
  assert report.LatitudeNanominutes==(-expected_minutes if negative else expected_minutes)
  assert report.LatitudeStepNanominutes==10**(9-precision)
  assert report.Location.LatitudeBits==bits(Fraction(expected_minutes,60*10**9)*(-1 if negative else 1))
  assert report.Location.Flags==15 and report.AuxFlags==47 and report.Location.UtcMilliseconds==764426119000+ns//10**6
  assert report.Location.AltitudeBits==bits(Fraction(5923,10)) and report.Quality==1 and report.SatellitesUsed==8
  assert not state.HaveRmc and not state.HaveGga and not any(bytes(state.Rmc)+bytes(state.Gga)+bytes(state.Line));cases+=1
 rmc=sentence('GPRMC,123519,A,4807.038,N,01131.000,E,22.4,84.4,230394,,,A')
 gga=sentence('GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,')
 for trigger in ('expire','void','quality-zero','reset','fault','ns-mismatch','coordinate-mismatch'):
  state=NmeaStream();assert lib.MtkGnssNmeaStreamInit(C.byref(state),1900,1000)==0
  assert receive(state,rmc,1)[2].Kind==1
  if trigger=='expire':
   assert receive(state,b'',1002)[0]==2 and not state.HaveRmc
   assert not any(bytes(state.Rmc))
  elif trigger=='void':assert receive(state,sentence(','.join(['GPRMC','','V']+['']*9+['N'])),2)[2].Location.Flags==0
  elif trigger=='quality-zero':assert receive(state,sentence('GPGGA,,,,,,0,00,,,,,,,'),2)[2].Location.Flags==0
  elif trigger=='reset':
   old=state.Epoch;reset(state);before=bytes(state)
   assert receive(state,b'invalid old callback',0,old)[0]==7 and bytes(state)==before
  elif trigger=='fault':
   assert receive(state,b'$broken*00\r\n',2)[0]==4 and state.Faulted
   assert not any(bytes(state.Rmc)+bytes(state.Gga)+bytes(state.Line))
   assert receive(state,gga,3)[0]==5;reset(state)
  else:
   mismatch=(gga.replace(b'123519,',b'123519.000000001,') if trigger=='ns-mismatch' else gga.replace(b'4807.038,',b'4807.040,'))
   text=mismatch[1:mismatch.index(b'*')].decode();result=receive(state,sentence(text),2)
   assert result[0]==3 and result[2].Kind==2;cases+=1
   continue
  status,used,report=receive(state,gga,1003 if trigger=='expire' else 3)
  assert status==3 and used==len(gga) and report.Kind==2 and not report.Location.UtcMilliseconds and not report.Year;cases+=1
 # A new epoch can accept a fresh calendar while an old callback changes nothing.
 reset(state);before=bytes(state);assert receive(state,b'$',0,state.Epoch-1)[0]==7 and bytes(state)==before;cases+=1
 return {'status':'passed','synthetic_scenarios':cases,'receive_calls':calls,'original_ARM64_comparisons':False,
  'randomized_chunk_pair_scenarios':512,'report_bytes':C.sizeof(Nmea),'state_bytes':C.sizeof(NmeaStream),
  'scope':'Synthetic NMEA output only: joined/chunked sentences in both orders, exact consumption and output immutability, nanosecond and coordinate-resolution pairing, pair wipes, age expiry, invalid fixes, fault/reset and stale epochs. Native/ARM64 suite additionally covers aliases, chronology/leap/clock, partial timeout and bounds. No endpoint, receiver/RF, engine, OS adapter or absolute fix-age assertion'}
