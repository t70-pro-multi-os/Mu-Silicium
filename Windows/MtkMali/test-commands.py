#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare production command rebuilding against the existing Mesa arch-9 XML."""
from pathlib import Path
import ctypes as C
import hashlib,json,random,struct,sys,xml.etree.ElementTree as ET
p=Path(__file__).resolve();r=p.parents[2];xml=r.parent/'mesa/src/panfrost/genxml/v9.xml'
root=ET.parse(xml).getroot();lib=C.CDLL(sys.argv[1]);rng=random.Random(0x9093)
class Caps(C.Structure):
 _fields_=[('gpu',C.c_uint32),('mmu',C.c_uint32),('as_',C.c_uint32),('slots',C.c_uint32),
  ('features',C.c_uint32*3),('shaders',C.c_uint64),('tilers',C.c_uint64),('l2',C.c_uint64)]
CB=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32)
f=lib.MaliCommandsPrepare;f.restype=C.c_int
f.argtypes=[C.POINTER(Caps),C.c_void_p,C.c_uint64,C.c_uint32,C.c_void_p,C.c_uint32,
 C.c_void_p,C.c_uint64,C.c_uint32,C.c_void_p,CB,C.c_void_p]
def pack(name,values,size):
 node=root.find("struct[@name='%s']"%name);assert node is not None
 data=0
 for field in node.findall('field'):
  supplied=field.attrib['name'] in values
  value=values.pop(field.attrib['name'],0);width=int(field.attrib['size'])
  if supplied:
   mod=field.attrib.get('modifier','')
   if mod=='minus(1)':value-=1
   elif mod=='log2':
    assert value and value&(value-1)==0
    value=value.bit_length()-1
   elif mod.startswith('shr('):
    shift=int(mod[4:-1]);assert value&((1<<shift)-1)==0
    value>>=shift
   elif mod.startswith('align('):assert value%int(mod[6:-1])==0
   else:assert not mod,mod
  start=field.attrib['start'];word,bit=map(int,start.split(':')) if ':' in start else (0,int(start));assert 0<=value<(1<<width)
  data|=value<<(word*32+bit)
 assert not values,values
 return data.to_bytes(size,'little')
SHADOW_BYTES=24576
WORK_BYTES=32768
count=0
for kind in (2,3):
 for mode in ((2,4,5,6,7) if kind==2 else (0,)):
  for i in range(32):
   target=0xa0000000+128*i
   bits=64 if mode==2 else 8<<(mode-4) if mode else 0
   value=0 if mode==2 else rng.getrandbits(bits) if bits else 0
   header=pack('Job Header',{'Type':kind,'Index':rng.randrange(1,65536),'Suppress Prefetch':i&1},32)
   payload=pack('Write Value Job Payload',{'Address':target,'Type':mode,'Immediate Value':value},24) if kind==2 else pack(
    'Cache Flush Job Payload',{'Invalidate Shader Core LS':1,'L2 Invalidate':1},8)
   data=header+payload;data+=bytes(4096-len(data));src=C.create_string_buffer(data,len(data))
   private=struct.pack('<IIIIQII',1,32,1,0,0x80000000,1,0);prv=C.create_string_buffer(private,len(private))
   out=C.create_string_buffer(SHADOW_BYTES);work=C.create_string_buffer(WORK_BYTES);pins=[]
   @CB
   def pin(ctx,va,n):
    pins.append((va,n));return 0 if va==target and n==bits//8 else 1
   caps=Caps();caps.gpu=0x90930002;caps.slots=7;caps.features[1]=12
   assert f(C.byref(caps),src,0x80000000,len(data),prv,len(private),out,0x90000000,SHADOW_BYTES,work,pin,None)==0
   expected=pack('Job Header',{'Type':kind,'Index':1,'Barrier':1,'Suppress Prefetch':1},32)
   expected+=payload if kind==2 else pack('Cache Flush Job Payload',{
    'Clean Shader Core LS':1,'Invalidate Shader Core LS':1,'Invalidate Shader Core Other':1,
    'Job Manager Clean':1,'Job Manager Invalidate':1,'Tiler Clean':1,'Tiler Invalidate':1,'L2 Clean':1,'L2 Invalidate':1},8)
   expected+=bytes(SHADOW_BYTES-len(expected));assert out.raw==expected,(kind,mode,i)
   assert src.raw==data and prv.raw==private
   assert pins==([(target,bits//8)] if kind==2 else [])
   count+=1
legacy_count=count
RCB=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint64,C.c_uint32,C.c_uint32)
g=lib.MaliCommandsPrepareResources;g.restype=C.c_int
g.argtypes=[*f.argtypes[:-1],RCB,C.c_void_p]
compute_vectors=256
for i in range(compute_vectors):
 sizes=[rng.randrange(1,9),rng.randrange(1,9),rng.randrange(1,9)]
 counts=[rng.randrange(1,65536) for _ in range(3)]
 offsets=[rng.randrange(0,65536) for _ in range(3)]
 fau_count=rng.randrange(0,64)
 env=pack('Shader Environment',{'FAU count':fau_count,'Resources':0xa0000084 if i&1 else 0,
  'Shader':0xa0000000,'Thread storage':0xa0000040,'FAU':0xa0000400 if fau_count else 0},64)
 payload=pack('Compute Payload',{
  **{'Workgroup size '+a:v for a,v in zip('XYZ',sizes)},
  **{'Workgroup count '+a:v for a,v in zip('XYZ',counts)},
  **{'Offset '+a:v for a,v in zip('XYZ',offsets)},
  'Task increment':1,'Task axis':2,'Allow merging workgroups':i&1,'Compute':int.from_bytes(env,'little')},96)
 header=pack('Job Header',{'Type':4,'Index':rng.randrange(1,65536)},32)
 data=header+payload+bytes(4096-128);src=C.create_string_buffer(data,len(data))
 resources=[(0xa0000000,4096,2),(0xb0000000,8192,10),(0xc0000000,1048576,6)]
 private=struct.pack('<IIIIQII',2,80,1,len(resources),0x80000000,1,0)+b''.join(struct.pack('<QII',*r) for r in resources)
 prv=C.create_string_buffer(private,len(private));out=C.create_string_buffer(SHADOW_BYTES);work=C.create_string_buffer(WORK_BYTES);pins=[]
 @CB
 def no_write(ctx,va,n):
  raise AssertionError('compute has no immediate writes')
 @RCB
 def resource(ctx,va,n,access):
  pins.append((va,n,access));return 0
 caps=Caps();caps.gpu=0x90930002;caps.slots=7;caps.features[1]=28
 assert g(C.byref(caps),src,0x80000000,len(data),prv,len(private),out,0x90000000,SHADOW_BYTES,work,no_write,resource,None)==0
 expected=pack('Job Header',{'Type':4,'Index':1,'Barrier':1,'Suppress Prefetch':1},32)+payload+bytes(SHADOW_BYTES-128)
 assert out.raw==expected and src.raw==data and prv.raw==private
 assert pins==resources
 count+=1
fragment_vectors=0
orders=[int(n.attrib['value']) for n in root.find("enum[@name='Tile Render Order']").findall('value')]
for order in orders:
 for targets in range(1,9):
  for zs in range(2):
   minx,miny=rng.randrange(4096),rng.randrange(4096)
   maxx,maxy=rng.randrange(minx,4096),rng.randrange(miny,4096)
   fbd=0xa0000001+((targets-1)<<2)+(zs<<1)
   payload=pack('Fragment Job Payload',{'Bound Min X':minx,'Bound Min Y':miny,
    'Bound Max X':maxx,'Bound Max Y':maxy,'Tile render order':order,'Framebuffer':fbd},32)
   data=pack('Job Header',{'Type':9,'Index':rng.randrange(1,65536)},32)+payload
   src=C.create_string_buffer(data,len(data));resources=[(0xa0000000,4096,2),(0xc0000000,1048576,6)]
   private=struct.pack('<IIIIQII',2,64,1,2,0x80000000,0,0)+b''.join(struct.pack('<QII',*r) for r in resources)
   prv=C.create_string_buffer(private,len(private));out=C.create_string_buffer(SHADOW_BYTES);work=C.create_string_buffer(WORK_BYTES);pins=[]
   @CB
   def no_write(ctx,va,n):
    raise AssertionError('fragment has no immediate-write payload')
   @RCB
   def resource(ctx,va,n,access):
    pins.append((va,n,access));return 0
   caps=Caps();caps.gpu=0x90930002;caps.slots=1;caps.features[0]=1<<9
   assert g(C.byref(caps),src,0x80000000,len(data),prv,len(private),out,0x90000000,SHADOW_BYTES,work,no_write,resource,None)==0
   expected=pack('Job Header',{'Type':9,'Index':1,'Barrier':1,'Suppress Prefetch':1},32)+payload+bytes(SHADOW_BYTES-64)
   assert out.raw==expected and src.raw==data and prv.raw==private and pins==resources
   fragment_vectors+=1;count+=1
# Independently use XML aggregate section offsets and field layouts, including
# the 256/384-byte tails the old fixed 128-byte snapshot could not retain.
def aggregate(name,sections):
 node=root.find("aggregate[@name='%s']"%name);assert node is not None
 out=bytearray(int(node.attrib['size']))
 for n,values in sections.items():
  section=node.find("section[@name='%s']"%n);assert section is not None
  fields=root.find("struct[@name='%s']"%section.attrib['type']);assert fields is not None
  bits=0
  for field in fields.findall('field'):
   start=field.attrib['start'];word,bit=map(int,start.split(':')) if ':' in start else (0,int(start))
   bits=max(bits,word*32+bit+int(field.attrib['size']))
  data=pack(section.attrib['type'],values,(bits+7)//8);offset=int(section.attrib['offset'])
  assert offset+len(data)<=len(out);out[offset:offset+len(data)]=data
 return bytes(out)
def raster_record(kind,i,index,nxt):
 secondary=bool(kind==11 and i&1);indexed=(i%4) if kind!=12 else 0
 point=((0,2,3)[i%3]) if kind!=12 else 0
 count=rng.randrange(1,128)
 primitive={'Draw mode':(1,2,3,4,5,6,8,9,10,11,12,13,14)[i%13],
  'Index type':indexed,'Point size array format':point,'Index count':count,
  'Base vertex offset':rng.getrandbits(32),'Instance offset':rng.getrandbits(32),
  'Allow rotating primitives':i&1,'Low Depth Cull':1,'High Depth Cull':1,
  'Primitive restart':int(bool(indexed and i&1)),'Secondary Shader':int(secondary)}
 sections={'Header':{'Type':kind,'Index':index,'Next':nxt},'Primitive':primitive,
  'Tiler':{'Address':0xc0000000},'Scissor':{'Scissor Minimum X':i,'Scissor Minimum Y':i,
   'Scissor Maximum X':65535-i,'Scissor Maximum Y':65535-i}}
 if kind==12:
  sections['DCD']={'Address':0xa0000100};return aggregate('Fullscreen Job',sections)
 env=lambda attrib:pack('Shader Environment',{'Attribute offset':attrib,'FAU count':i%33,
  'Resources':0xa0000084 if i&2 else 0,'Shader':0xa0000000,'Thread storage':0xa0000040,
  'FAU':0xa0000400 if i%33 else 0},64)
 query=(0,1,3)[i%3];blends=i%9
 flags=pack('DCD Flags 0',{'Occlusion query':query,'Front face CCW':i&1,
  'Cull front face':int(bool(i&2)),'Allow primitive reorder':1,'Pixel kill operation':i%4},4)
 vertex=pack('Vertex Array',{'Packet':1,'Pointer':0 if kind==11 else 0xa0000200,
  'Vertex packet stride':0 if kind==11 else 32,'Vertex attribute stride':0 if kind==11 else 16},12)
 sections['Draw']={'Flags 0':int.from_bytes(flags,'little'),'Flags 1':0x1ffff,
  'Vertex array':int.from_bytes(vertex,'little'),'Minimum Z':0,'Maximum Z':0x3f800000,
  'Depth/stencil':0xa0000300 if i&4 else 0,'Blend count':blends,'Blend':0xa0000500 if blends else 0,
  'Occlusion':0xc0000100 if query else 0,'Shader':int.from_bytes(env(0),'little') if i%5 else 0}
 sections['Instance Count']={'Count':rng.randrange(1,1000)}
 sections['Indices']={'Address':0xa0000c00 if indexed else 0}
 sections['Primitive Size']={'Size Array':0xa0000800} if kind==7 and point else {'Fixed Sized':0x3f800000}
 if kind==11:
  sections['Allocation']={'Vertex packet stride':32 if secondary else 16,'Vertex attribute stride':16 if secondary else 0}
  sections['Position']={'Attribute offset':i,'FAU count':i%33,'Resources':0xa0000084,
   'Shader':0xa0000000,'Thread storage':0xa0000040,'FAU':0xa0000400 if i%33 else 0}
  if secondary:sections['Varying']={'Shader':0xa0000020,'Thread storage':0xa0000040}
 else:sections['Vertex Count']={'Count':count}
 return aggregate('Malloc Vertex Job' if kind==11 else 'Tiler Job',sections)
raster_vectors=0
resources=[(0xa0000000,4096,2),(0xb0000000,4096,10),(0xc0000000,1048576,6)]
for i in range(256):
 for kinds in ((11,),(7,),(12,),(11,7,12,9)):
  offsets=[];offset=0
  for kind in kinds:offsets.append(offset);offset+=max(128,{11:384,7:256,12:128,9:64}[kind])
  data=bytearray(offset);expected=bytearray(SHADOW_BYTES)
  for j,(kind,off) in enumerate(zip(kinds,offsets)):
   nxt=0x80000000+offsets[j+1] if j+1<len(kinds) else 0
   if kind==9:
    record=pack('Job Header',{'Type':9,'Index':100+j,'Next':nxt},32)+pack('Fragment Job Payload',{'Framebuffer':0xa0000001},32)
   else:record=raster_record(kind,i,100+j,nxt)
   data[off:off+len(record)]=record
   expected[off:off+len(record)]=record
   expected[off:off+32]=pack('Job Header',{'Type':kind,'Index':j+1,'Dependency 1':j,
    'Barrier':1,'Suppress Prefetch':1,'Next':0x90000000+offsets[j+1] if j+1<len(kinds) else 0},32)
  private=struct.pack('<IIIIQII',2,80,1,3,0x80000000,1,0)+b''.join(struct.pack('<QII',*r) for r in resources)
  src=C.create_string_buffer(bytes(data),len(data));prv=C.create_string_buffer(private,len(private))
  out=C.create_string_buffer(SHADOW_BYTES);work=C.create_string_buffer(WORK_BYTES);pins=[]
  @CB
  def no_write(ctx,va,n):raise AssertionError('raster has no immediate writes')
  @RCB
  def resource(ctx,va,n,access):pins.append((va,n,access));return 0
  caps=Caps();caps.gpu=0x90930002;caps.slots=7;caps.features[1]=sum(1<<kind for kind in (7,9,11,12))
  assert g(C.byref(caps),src,0x80000000,len(data),prv,len(private),out,0x90000000,SHADOW_BYTES,work,no_write,resource,None)==0,(i,kinds)
  assert out.raw==expected and src.raw==data and prv.raw==private and pins==resources,(i,kinds)
  raster_vectors+=1
count+=raster_vectors
# Feed the independently XML-packed jobs back through the new producer API,
# then through the actual KMD shadow preparer in two separate ordered stages.
class Environment(C.Structure):
 _fields_=[('attribute',C.c_uint32),('count',C.c_uint32),('resources',C.c_uint64),
  ('shader',C.c_uint64),('tls',C.c_uint64),('fau',C.c_uint64)]
class Raster(C.Structure):
 _fields_=[(n,C.c_uint32) for n in ('bytes','vertex_slot','fragment_slot','primitive','base_vertex',
  'base_instance','index_count','instance_count','varying_stride')]+[
  ('scissor',C.c_uint32*2),('primitive_size',C.c_uint32),('flags',C.c_uint32*2),
  ('min_z',C.c_uint32),('max_z',C.c_uint32),('blend_count',C.c_uint32),('tiles',C.c_uint32*2)]+[
  (n,C.c_uint64) for n in ('tiler','indices','depth_stencil','blend','occlusion','framebuffer')]+[
  (n,Environment) for n in ('position','varying','fragment')]
class ResourceRange(C.Structure):
 _fields_=[('va',C.c_uint64),('bytes',C.c_uint32),('access',C.c_uint32)]
assert C.sizeof(Raster)==248 and C.sizeof(Environment)==40 and C.sizeof(ResourceRange)==16
emit=lib.MaliCommandsBuildRaster;emit.restype=C.c_int
emit.argtypes=[C.POINTER(Caps),C.POINTER(Raster),C.POINTER(ResourceRange),C.c_uint32,
 C.c_void_p,C.c_uint64,C.c_uint32,C.c_void_p,C.c_uint32]
raster_producer_vectors=256
for i in range(raster_producer_vectors):
 vertex=raster_record(11,i,1,0)
 u32=lambda offset:struct.unpack_from('<I',vertex,offset)[0]
 u64=lambda offset:struct.unpack_from('<Q',vertex,offset)[0]
 env=lambda offset:Environment(u32(offset),u32(offset+4),u64(offset+32),u64(offset+40),u64(offset+48),u64(offset+56))
 args=Raster();args.bytes=C.sizeof(args);args.vertex_slot=1;args.fragment_slot=0
 for name,offset in (('primitive',32),('base_vertex',36),('base_instance',40),('index_count',44),
  ('instance_count',48),('primitive_size',112),('min_z',152),('max_z',156)):
  setattr(args,name,u32(offset))
 args.varying_stride=u32(52)>>16;args.scissor[:]=[u32(104),u32(108)];args.flags[:]=[u32(128),u32(132)]
 args.tiler=u64(56);args.indices=u64(120);args.depth_stencil=u64(168)
 args.blend_count=u64(176)&15;args.blend=u64(176)&~15;args.occlusion=u64(184)
 args.position=env(256);args.varying=env(320);args.fragment=env(192)
 x,y=rng.randrange(4096),rng.randrange(4096);xx,yy=rng.randrange(x,4096),rng.randrange(y,4096)
 order=orders[i%len(orders)];targets=i%8+1;zs=(i>>3)&1
 args.tiles[:]=[x|(y<<16),xx|(yy<<16)|(order<<28)]
 args.framebuffer=0xa0000001+((targets-1)<<2)+(zs<<1)
 fragment=pack('Job Header',{'Type':9,'Index':1},32)+pack('Fragment Job Payload',{
  'Bound Min X':x,'Bound Min Y':y,'Bound Max X':xx,'Bound Max Y':yy,'Tile render order':order,
  'Framebuffer':args.framebuffer},32)+bytes(64)
 expected=vertex+fragment
 ranges=(ResourceRange*len(resources))(*(ResourceRange(*r) for r in resources))
 src=C.create_string_buffer(bytes([0x5a])*512,512);prv=C.create_string_buffer(bytes([0xa5])*96,96)
 caps=Caps();caps.gpu=0x90930002;caps.tilers=1;caps.slots=3;caps.features[1]=1<<11;caps.features[0]=1<<9
 assert emit(C.byref(caps),C.byref(args),ranges,len(ranges),src,0x80000000,512,prv,96)==0,i
 private=struct.pack('<IIIIQIIQII',2,96,2,3,0x80000000,1,0,0x80000180,0,0)+b''.join(struct.pack('<QII',*r) for r in resources)
 assert src.raw==expected and prv.raw==private,i
 out=C.create_string_buffer(SHADOW_BYTES);work=C.create_string_buffer(WORK_BYTES);pins=[]
 @CB
 def no_write(ctx,va,n):raise AssertionError('raster producer has no immediate writes')
 @RCB
 def resource(ctx,va,n,access):pins.append((va,n,access));return 0
 assert g(C.byref(caps),src,0x80000000,512,prv,96,out,0x90000000,SHADOW_BYTES,work,no_write,resource,None)==0,i
 shadow=bytearray(SHADOW_BYTES);shadow[:384]=vertex;shadow[384:448]=fragment[:64]
 for offset,kind in ((0,11),(384,9)):
  shadow[offset:offset+32]=pack('Job Header',{'Type':kind,'Index':1,'Barrier':1,'Suppress Prefetch':1},32)
 assert out.raw==shadow and pins==resources,i
count+=raster_producer_vectors
# Produce real clears, independently pack every XML field and compare color
# conversion with both Mesa's original native helper and Python binary32 RNE.
class Clear(C.Structure):
 _fields_=[('bytes',C.c_uint32),('width',C.c_uint32),('height',C.c_uint32),('pitch',C.c_uint32),
  ('format',C.c_uint32),('slot',C.c_uint32),('color',C.c_uint32*4),('target',C.c_uint64),
  ('target_bytes',C.c_uint32),('reserved',C.c_uint32),('heap',C.c_uint64)]
assert C.sizeof(Clear)==64
clear=lib.MaliCommandsBuildClear;clear.restype=C.c_int
clear.argtypes=[C.POINTER(Caps),C.POINTER(Clear),C.c_void_p,C.c_uint64,C.c_uint32,
 C.c_void_p,C.c_uint64,C.c_uint32,C.c_void_p,C.c_uint32]
oracle=lib.MaliMesaClearReference;oracle.restype=C.c_uint32;oracle.argtypes=[C.c_uint32]
def floatbits(f):return struct.unpack('<I',struct.pack('<f',f))[0]
def unorm(bits):
 f=struct.unpack('<f',struct.pack('<I',bits))[0]
 if not f>0:return 0
 if f>=1:return 255
 product=struct.unpack('<f',struct.pack('<f',f*255))[0]
 return round(product)
special=[0,0x80000000,1,0x007fffff,0x00800000,0x3b000000,0x3b008080,0x3b008081,
 0x3effffff,0x3f000000,0x3f000001,0x3f7fffff,0x3f800000,0x40000000,0xbf800000,
 0x7f800000,0xff800000,0x7fc00001,0x7f800001,0xffc00001]
colors=[(b,)*4 for b in special]
for n in range(255):
 center=floatbits((n+.5)/255)
 for b in (center-1,center,center+1):colors.append((b,)*4)
for i in range(2048):colors.append(tuple(rng.getrandbits(32) for _ in range(4)))
for i in range(1024):colors.append(tuple((rng.randrange(118,127)<<23)|rng.getrandbits(23) for _ in range(4)))
dimensions=[(1,1),(17,19),(2048,3),(2049,1),(4096,1),(4097,1),(16384,17),(16384,2048),(1,16384)]
clear_vectors=0;color_channels=0
for i,color in enumerate(colors):
 fmt=(21,22,32,33)[i%4]
 if i<len(dimensions)*4:w,h=dimensions[i//4]
 else:w,h=rng.randrange(1,16385),rng.randrange(1,1025)
 pitch=(w*4+63)&~63;used=pitch*h;target_bytes=(used+4095)&~4095
 desc=Clear();desc.bytes=C.sizeof(desc);desc.width=w;desc.height=h;desc.pitch=pitch;desc.format=fmt;desc.slot=i%3
 desc.color[:]=color;desc.target=0xc0000000;desc.target_bytes=target_bytes;desc.heap=0xd0000000
 cmd_va=0x80000000+128*(i%32);desc_va=0xa0000000
 cmd=C.create_string_buffer(128);fbd=C.create_string_buffer(4096);prv=C.create_string_buffer(80)
 caps=Caps();caps.gpu=0x90930002;caps.slots=7;caps.tilers=1;caps.features[desc.slot]=1<<9
 before=bytes(desc)
 assert clear(C.byref(caps),C.byref(desc),cmd,cmd_va,128,fbd,desc_va,4096,prv,80)==0
 assert bytes(desc)==before
 quant=[]
 for b in color:
  expected=unorm(b);assert oracle(b)==expected,(hex(b),oracle(b),expected)
  quant.append(expected);color_channels+=1
 if fmt in (22,33):quant[3]=255
 packed=sum(v<<(8*n) for n,v in enumerate(quant))
 target=pack('RT Buffer',{'Base':desc.target,'Row Stride':pitch,'Surface Stride':used},16)
 rt_clear=pack('RT Clear',dict(zip(('Color 0','Color 1','Color 2','Color 3'),(packed,)*4)),16)
 rt=pack('RGB Render Target',{'Internal Format':1,'Write Enable':1,'Writeback Format':19,
  'Writeback Block Format':2,'Dithering Enable':1,'Swizzle':0x60a if fmt in (21,22) else 0x688,
  'Clean Tile Write Enable':1,'Writeback buffer':int.from_bytes(target,'little'),'Clear':int.from_bytes(rt_clear,'little')},64)
 params=pack('Framebuffer Parameters',{'Sample Locations':desc_va+512,'Width':w,'Height':h,
  'Bound Max X':w-1,'Bound Max Y':h-1,'Sample Count':1,'Tie-Break Rule':2,
  'Effective Tile Size':256,'First provoking vertex':1,'Point sprite coord origin max Y':0,
  'Render Target Count':1,'Color Buffer Allocation':1024,'Z Internal Format':1,'Tiler':desc_va+192},64)
 # This is Mesa's max_levels=1 hierarchy policy; no tiler primitives are emitted.
 level=((max(w,h)+15)//16).bit_length()
 hierarchy=0x80<<max(0,level-8)
 context=pack('Tiler Context',{'Hierarchy Mask':hierarchy,'First provoking vertex':1,
  'FB Width':w,'FB Height':h,'Layer count':1,'Heap':desc_va+384},192)
 heap=pack('Tiler Heap',{'Type':9,'Buffer type':2,'Chunk size':0,'Size':262144,
  'Base':desc.heap,'Bottom':desc.heap,'Top':desc.heap+262144},32)
 samples=bytearray(256);struct.pack_into('<HH',samples,0,128,128);struct.pack_into('<HH',samples,128,128,128)
 expected=params+bytes(64)+rt+context+heap+bytes(96)+samples+bytes(4096-768)
 assert fbd.raw==expected,('clear descriptor',i,w,h,fmt)
 payload=pack('Fragment Job Payload',{'Bound Max X':(w-1)//16,'Bound Max Y':(h-1)//16,'Framebuffer':desc_va|1},32)
 expected_job=pack('Job Header',{'Type':9,'Index':1},32)+payload+bytes(64)
 assert cmd.raw==expected_job
 resources=[(desc_va,4096,6),(desc.target,target_bytes,6),(desc.heap,262144,6)]
 expected_private=struct.pack('<IIIIQII',2,80,1,3,cmd_va,desc.slot,0)+b''.join(struct.pack('<QII',*v) for v in resources)
 assert prv.raw==expected_private
 # The generated data must also pass the real producer's downstream parser.
 out=C.create_string_buffer(SHADOW_BYTES);work=C.create_string_buffer(WORK_BYTES);pins=[]
 @CB
 def no_write(ctx,va,n):return 1
 @RCB
 def resource(ctx,va,n,access):
  pins.append((va,n,access));return 0
 assert g(C.byref(caps),cmd,cmd_va,128,prv,80,out,0x90000000,SHADOW_BYTES,work,no_write,resource,None)==0
 expected_shadow=pack('Job Header',{'Type':9,'Index':1,'Barrier':1,'Suppress Prefetch':1},32)+payload+bytes(SHADOW_BYTES-64)
 assert out.raw==expected_shadow and pins==resources and fbd.raw==expected
 clear_vectors+=1
count+=clear_vectors
class Shader(C.Structure):
 _fields_=[('bytes',C.c_uint32),('stage',C.c_uint32),('registers',C.c_uint32),
  ('warp',C.c_uint32),('ftz',C.c_uint32),('helpers',C.c_uint32),('code',C.c_uint64),('preload',C.c_uint64)]
shader=lib.MaliCommandsBuildShader;shader.restype=C.c_int
shader.argtypes=[C.POINTER(Caps),C.POINTER(Shader),C.c_void_p,C.c_uint32]
shader_vectors=1024
for i in range(shader_vectors):
 s=Shader();s.bytes=C.sizeof(s);s.stage=2+i%2;s.registers=(0,1,31,32,33,64)[i%6]
 s.warp=rng.randrange(4) if s.stage==3 else 0;s.ftz=i%3;s.helpers=rng.randrange(2) if s.stage==2 else 0
 s.code=(0xb0000000+8*i) if i else ((1<<48)-8);s.preload=rng.getrandbits(16)<<48
 out=C.create_string_buffer(b'\x5a'*32,32);caps=Caps();caps.gpu=0x90930000;before=bytes(s)
 assert shader(C.byref(caps),C.byref(s),out,32)==0 and bytes(s)==before
 values={'Type':8,'Stage':s.stage,'Flush to zero mode':s.ftz,
  'Register allocation':2 if s.registers<=32 else 0,'Preload':s.preload>>48,'Binary':s.code}
 if s.stage==3:values['Vertex warp limit']=s.warp
 else:values['Requires helper threads']=s.helpers;values['Fragment coverage bitmask type']=0
 assert out.raw==pack('Shader Program',values,32),(i,out.raw.hex())
count+=shader_vectors
# Check the LOAD_IMAGE descriptor bundle independently with named Mesa XML
# fields, including table-0 sampler/texture typing and addresses above 4 GiB.
class Preload(C.Structure):
 _fields_=[(n,C.c_uint32) for n in ('bytes','width','height','pitch','format','target_bytes','fau_count','tls_bytes')]+[
  ('reserved',C.c_uint32*2)]+[(n,C.c_uint64) for n in ('target','heap','program','fau','tls')]
assert C.sizeof(Preload)==80
load_image=lib.MaliCommandsBuildPreload;load_image.restype=C.c_int
load_image.argtypes=[C.POINTER(Caps),C.POINTER(Preload),C.c_void_p,C.c_uint64,C.c_uint32]
def enum(name,value):
 node=root.find("enum[@name='%s']/value[@name='%s']"%(name,value))
 assert node is not None,(name,value)
 return int(node.attrib['value'],0)
preload_vectors=0
preload_dimensions=dimensions+[(rng.randrange(1,16385),rng.randrange(1,513)) for _ in range(32)]
for fmt,order in ((32,'RGBA'),(21,'BGRA'),(22,'BGR1')):
 for high in (0,1<<36):
  for i,(w,h) in enumerate(preload_dimensions):
   va=high+0xa0000000;d=Preload();d.bytes=C.sizeof(d)
   d.width=w;d.height=h;d.pitch=(w*4+63)&~63;d.format=fmt
   used=d.pitch*h;d.target_bytes=(used+4095)&~4095
   d.fau_count=(1,2,64,255)[i%4];d.tls_bytes=64
   d.target=high+0xc0000000;d.heap=high+0xd0000000
   d.program=high+0xb0000000;d.fau=high+0xb0100000;d.tls=high+0xb0200000
   caps=Caps();caps.gpu=0x90930000;caps.tilers=1
   out=C.create_string_buffer(4096);before=bytes(d)
   assert load_image(C.byref(caps),C.byref(d),out,va,4096)==0 and bytes(d)==before
   expected=bytearray(4096)
   def place(offset,data):expected[offset:offset+len(data)]=data
   place(0,pack('Framebuffer Parameters',{'Pre Frame 0':enum('Pre Post Frame Shader Mode','Always'),
    'Sample Locations':va+512,'Frame Shader DCDs':va+768,'Width':w,'Height':h,
    'Bound Max X':w-1,'Bound Max Y':h-1,'Sample Count':1,'Tie-Break Rule':2,
    'Effective Tile Size':256,'First provoking vertex':1,'Render Target Count':1,
    'Color Buffer Allocation':1024,'Z Internal Format':1,'Tiler':va+192},64))
   target=pack('RT Buffer',{'Base':d.target,'Row Stride':d.pitch,'Surface Stride':used},16)
   place(128,pack('RGB Render Target',{'Internal Format':1,'Write Enable':1,'Writeback Format':19,
    'Writeback Block Format':enum('Block Format','Linear'),'Dithering Enable':1,
    'Swizzle':0x688 if fmt==32 else 0x60a,'Clean Tile Write Enable':1,
    'Writeback buffer':int.from_bytes(target,'little')},64))
   hierarchy=0x80<<max(0,((max(w,h)+15)//16).bit_length()-8)
   place(192,pack('Tiler Context',{'Hierarchy Mask':hierarchy,'First provoking vertex':1,
    'FB Width':w,'FB Height':h,'Layer count':1,'Heap':va+384},192))
   place(384,pack('Tiler Heap',{'Type':9,'Buffer type':2,'Size':262144,
    'Base':d.heap,'Bottom':d.heap,'Top':d.heap+262144},32))
   samples=bytearray(256);struct.pack_into('<HH',samples,0,128,128);struct.pack_into('<HH',samples,128,128,128)
   place(512,samples)
   flags0=pack('DCD Flags 0',{'Allow forward pixel to kill':1,'Allow forward pixel to be killed':1,
    'Clean Fragment Write':1},4)
   flags1=pack('DCD Flags 1',{'Sample mask':0xffff,'Render target mask':1},4)
   environment=pack('Shader Environment',{'FAU count':d.fau_count,'Resources':(va+1280)|4,
    'Shader':d.program,'Thread storage':d.tls,'FAU':d.fau},64)
   place(768,pack('Draw',{'Flags 0':int.from_bytes(flags0,'little'),'Flags 1':int.from_bytes(flags1,'little'),
    'Maximum Z':0x3f800000,'Depth/stencil':va+1376,'Blend count':1,'Blend':va+1344,
    'Shader':int.from_bytes(environment,'little')},128))
   place(1152,pack('Sampler',{'Type':enum('Descriptor Type','Sampler'),
    **{'Wrap Mode '+a:enum('Wrap Mode','Clamp to Edge') for a in 'RTS'},
    'Round to nearest even':1,'Minify nearest':1,'Magnify nearest':1,'Maximum anisotropy':1},32))
   texture_format=(enum('Format','RGBA8 UNORM')<<12)|enum('RGB Component Order',order)
   place(1184,pack('Texture',{'Type':enum('Descriptor Type','Texture'),
    'Dimension':enum('Texture Dimension','2D'),'Format':texture_format,
    'Width':w,'Height':h,'Swizzle':0x688,'Levels':1,'Sample count':1,
    'Surfaces':va+1216,'Array size':1,'Depth':1},32))
   place(1216,pack('Generic Plane',{'Type':enum('Descriptor Type','Plane'),
    'Plane type':enum('Plane Type','Generic'),'Clump ordering':enum('Clump Ordering','Linear'),
    'Clump format':enum('Clump Format','RAW32'),'Size':used,'Pointer':d.target,'Row stride':d.pitch},32))
   place(1280,pack('Resource',{'Address':va+1152,'Contains descriptors':1,'Size':64},16))
   function=pack('Blend Function',{'A':enum('Blend Operand A','Src'),
    'B':enum('Blend Operand B','Src'),'C':enum('Blend Operand C','Zero')},2)
   # Blend Function occupies 12 bits; its XML struct has no explicit size.
   function=int.from_bytes(function,'little')
   equation=pack('Blend Equation',{'RGB':function,'Alpha':function,'Color Mask':15},4)
   conversion=pack('Internal Conversion',{'Memory Format':enum('Format','RGBA8 TB')<<12},4)
   fixed=pack('Blend Fixed-Function',{'Num Comps':4,'Conversion':int.from_bytes(conversion,'little')},8)
   internal=pack('Internal Blend',{'Mode':enum('Blend Mode','Opaque'),'Fixed-Function':int.from_bytes(fixed,'little')},8)
   place(1344,pack('Blend',{'Enable':1,'Round to FB precision':1,'Equation':int.from_bytes(equation,'little'),
    'Internal':int.from_bytes(internal,'little')},16))
   depth={'Type':enum('Descriptor Type','Depth/stencil'),'Depth clamp mode':enum('Depth Clamp Mode','[0, 1]'),
    'Depth source':enum('Depth Source','Fixed function'),'Depth function':enum('Func','Always')}
   for side in ('Front','Back'):
    depth[side+' compare function']=enum('Func','Always')
    for operation in ('stencil fail','depth fail','depth pass'):depth[side+' '+operation]=enum('Stencil Op','Replace')
    for mask in ('write mask','value mask'):depth[side+' '+mask]=255
   place(1376,pack('Depth/stencil',depth,32))
   assert out.raw==expected,('preload descriptor',fmt,high,w,h,next((i for i,(a,b) in enumerate(zip(out.raw,expected)) if a!=b),None))
   preload_vectors+=1
count+=preload_vectors

report={'status':'passed','vectors':count,'legacy_write_cache_vectors':legacy_count,'compute_vectors':compute_vectors,'fragment_vectors':fragment_vectors,'clear_vectors':clear_vectors,'shader_program_vectors':shader_vectors,'framebuffer_preload_vectors':preload_vectors,'raster_vectors':raster_vectors,'raster_producer_vectors':raster_producer_vectors,'clear_color_channels':color_channels,'clear_half_thresholds':765,'reference':'existing Mesa src/panfrost/genxml/v9.xml',
 'reference_sha256':hashlib.sha256(xml.read_bytes()).hexdigest(),
 'clear_references':{str(n.relative_to(r.parent)):hashlib.sha256(n.read_bytes()).hexdigest() for n in
  (r.parent/'mesa/src/panfrost/lib/pan_clear.c',r.parent/'mesa/src/util/rounding.h',r.parent/'mesa/src/panfrost/lib/pan_fb.c',
   r.parent/'mesa/src/panfrost/lib/pan_desc.c',r.parent/'mesa/src/panfrost/lib/pan_samples.c',
   r.parent/'mesa/src/panfrost/vulkan/jm/panvk_vX_cmd_buffer.c')},
 'clear_scope':'actual fixed-function full-surface clear producer and downstream v2 parser; independent arch-9 XML packing, original native Mesa float_to_fixed helper, independent binary32 reference, 255 half thresholds with both adjacent binary32 inputs, NaN/infinity/negative/X-alpha cases and all four allocation UNORM formats; no GPU pixel execution',
 'shader_program_scope':'actual CPU shader-program encoder, 1024 independent XML comparisons for VS/PS stages, all warp limits, 32/64-register thresholds, R48-R63 preload, FTZ modes, fragment helpers and zero reserved fields. DirectX coverage bitmask; no GPU execution',
 'raster_scope':'full 384-byte malloc-vertex, 256-byte tiler, 128-byte fullscreen records and mixed-size chains independently packed from XML aggregate offsets; all index sizes, optional VS secondary/FS, point-size arrays, blend and occlusion buffers, scissor bounds; no GPU execution',
 'raster_producer_scope':'actual malloc-vertex/fragment producer, 256 independent XML comparisons and two-stage downstream preparation with exact immutable resource wire data; shader/indirect descriptor addresses are fixtures, not GPU execution',
 'preload_scope':'actual one-target RGBA8/BGRA8/BGRX8 LOAD_IMAGE descriptor producer, all 4096 bytes compared with independently packed named Mesa arch-9 XML fields at 32/64-bit addresses; dimensions, hierarchy levels and FAU limits; no GPU pixel execution',
 'scope':'actual command parser and shadow builder compared byte-for-byte with independently packed XML fields; compute and five-order/eight-target/ZS fragment payloads; explicit resource R/W/X callbacks with mocked ownership/residency; no indirect descriptor, shader or GPU execution'}
(r/'Windows/Build/mali-command-format-tests.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'Mali arch-9 descriptors: {count} Mesa XML format comparisons passed')
