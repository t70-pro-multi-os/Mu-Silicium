# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run selected original GPS/FM PALDO dispatch and VCN28 bodies virtually.
LLVM/Unicorn check, not REA Evidence. Regmap, regulators and ADIE probe are
virtual; no PMIC, module init, firmware startup, radio or location operation.
"""
import ctypes as C
import hashlib,runpy
import struct as S
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
READ=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint,C.POINTER(C.c_uint))
WRITE=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint,C.c_uint)
DELAY=C.CFUNCTYPE(C.c_int,C.c_void_p,C.c_uint)
AUTH=C.CFUNCTYPE(C.c_uint,C.c_void_p)
class Io(C.Structure):
 _fields_=[('Context',C.c_void_p),('Read16',READ),('Write16',WRITE),('DelayUs',DELAY),('Authorized',AUTH)]
class Rail(C.Structure):
 _fields_=[('Io',Io)]+[(n,C.c_uint) for n in ('Initialized','AdieChipId','Users','Captured','Faulted','DelayPending','SavedEnable','SavedOpEnable','SavedOpConfig')]
def compare(lib,loader,read_module,path):
 selected=('wmt_plat_soc_paldo_ctrl','mtk_wcn_consys_hw_vcn28_ctrl','consys_hw_vcn28_ctrl')
 raw,sec,sy,ranges,bases,u,stub,external,address,relocations=loader(path,selected,image_base=0x14000000)
 assert hashlib.sha256(raw).hexdigest()=='d395f5249bdeb2b5f6134f70c64e156bd682ba895a6da2d4c2544836a27b91e3'
 _,_,_,tabs,data=read_module(path)
 ops=sy['consys_ic_ops_mt6789'];binding=[]
 for i,q in enumerate(sec):
  if q[1]!=4 or q[7]!=ops[1]:continue
  for k in range(0,q[5],24):
   off,info,add=S.unpack_from('<QQq',data(i),k)
   if off==ops[2]+0x88:
    target=tabs[q[6]][info>>32]
    assert info&0xffffffff==257 and target[1]==sy['consys_hw_vcn28_ctrl'][1] and target[2]+add==sy['consys_hw_vcn28_ctrl'][2]
    binding.append((off,add))
 assert len(binding)==1
 u.mem_write(address(ops)+0x88,S.pack('<Q',address(sy['consys_hw_vcn28_ctrl'])))
 u.mem_write(address(sy['wmt_consys_ic_ops']),S.pack('<Q',address(ops)))
 assert S.unpack('<I',u.mem_read(address(sy['consys_hw_vcn28_ctrl'])-4,4))[0]==0xf80d940d
 dat=0x30000000;stack=0x40000000;u.mem_map(dat,0x10000);u.mem_map(stack,0x10000)
 u.mem_write(address(sy['g_regmap']),S.pack('<Q',dat+0x1000))
 u.mem_write(address(sy['reg_VCN28']),S.pack('<Q',dat+0x2000))
 u.mem_write(address(sy['wmtPlatLogLvl']),S.pack('<I',0))
 u.mem_write(address(sy['gCoClockFlag']),S.pack('<I',1))
 stop=stub+0xf000;traps={v:k for k,v in external.items()}
 allowed={'mtk_wcn_consys_get_adie_chipid','regmap_write','regulator_set_voltage','regulator_enable','regulator_disable','mtk_wcn_consys_hw_wifi_paldo_ctrl','mtk_wcn_consys_hw_bt_paldo_ctrl','mtk_wcn_consys_hw_efuse_paldo_ctrl','_printk'}
 for n in allowed:
  if n in sy:traps[address(sy[n])]=n
 trace=[];adie=0;fail=False;executed=set()
 def hook(uc,pc,size,context):
  name=traps.get(pc)
  if name:
   assert name in allowed,('unselected VCN28 service',name)
   executed.add(name);a=uc.reg_read(UC_ARM64_REG_X0);b=uc.reg_read(UC_ARM64_REG_X1);c=uc.reg_read(UC_ARM64_REG_X2);v=0
   if name=='mtk_wcn_consys_get_adie_chipid':v=adie
   elif name=='regmap_write':
    assert a==dat+0x1000 and b in (0x1d8a,0x1d90);trace.append(('write',b,c));v=-5 if fail else 0
   elif name=='regulator_set_voltage':
    assert a==dat+0x2000 and b==c==2800000;trace.append(('fixed_uv',b))
   elif name in ('regulator_enable','regulator_disable'):
    assert a==dat+0x2000;trace.append(('enable',int(name=='regulator_enable')));v=-5 if fail else 0
   elif name!='_printk':trace.append((name,a,b if name=='mtk_wcn_consys_hw_efuse_paldo_ctrl' else 0))
   uc.reg_write(UC_ARM64_REG_X0,v&0xffffffffffffffff);uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR));return
  assert any(bases[i]+lo<=pc<bases[i]+hi for i,lo,hi in ranges),('unselected original VCN28 PC',hex(pc))
 u.hook_add(UC_HOOK_CODE,hook)
 def call(name,a,b=0):
  trace.clear();u.reg_write(UC_ARM64_REG_X0,a);u.reg_write(UC_ARM64_REG_X1,b)
  u.reg_write(UC_ARM64_REG_LR,stop);u.reg_write(UC_ARM64_REG_SP,stack+0xfff0)
  u.emu_start(address(sy[name]),stop,count=10000)
  assert u.reg_read(UC_ARM64_REG_PC)==stop
  return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
 port={};port_writes=[];port_reads=0
 @READ
 def rd(context,a,v):
  nonlocal port_reads
  assert a in port;port_reads+=1;v[0]=port[a];return 0
 @WRITE
 def wr(context,a,v):
  assert a in (0x1d88,0x1d8a,0x1d90);port_writes.append((a,v))
  if fail:return 4
  port[a]=v
  if a==0x1d88:port[0x1d96]=(port[0x1d96]&~0x8000)|((v&1)<<15)
  return 0
 @DELAY
 def wait(context,us):assert us in (10,270);return 0
 @AUTH
 def auth(context):return 1
 io=Io(None,rd,wr,wait,auth)
 lib.MtkVcn28Initialize.argtypes=[C.POINTER(Rail),C.POINTER(Io),C.c_uint]
 lib.MtkVcn28Set.argtypes=[C.POINTER(Rail),C.c_uint,C.c_uint]
 cases=0;bypass=0
 for adie in (0x6631,0x6635):
  for selector,user in ((2,1),(3,2)):
   for enable in (0,1,0xa4,0xa5,0x8000,0x8001,0xfffe,0xffff):
    port={0x1d88:enable,0x1d8a:0x42,0x1d90:0x84,0x1d96:0x18|((enable&1)<<15)};port_writes.clear();port_reads=0;rail=Rail()
    assert lib.MtkVcn28Initialize(C.byref(rail),C.byref(io),adie)==0 and not port_reads
    assert call('wmt_plat_soc_paldo_ctrl',selector,1)==0
    assert lib.MtkVcn28Set(C.byref(rail),user,1)==0
    if adie==0x6635:
     assert trace==[] and port_writes==[] and not port_reads;bypass+=1
    else:
     assert trace==[('write',0x1d8a,3),('write',0x1d90,0),('fixed_uv',2800000),('enable',1)]
     assert port_writes[:2]==[(0x1d8a,3),(0x1d90,0)]
     assert port_writes[2:]==([] if enable&1 else [(0x1d88,enable|1)])
     assert port[0x1d88]==enable|1 and port[0x1d8a]==3 and port[0x1d90]==0
    before=list(port_writes)
    assert lib.MtkVcn28Set(C.byref(rail),3^user,1)==0 and rail.Users==3 and port_writes==before
    assert lib.MtkVcn28Set(C.byref(rail),user,0)==0 and port_writes==before
    assert call('wmt_plat_soc_paldo_ctrl',selector,0)==0
    assert lib.MtkVcn28Set(C.byref(rail),3^user,0)==0 and not rail.Users
    if adie!=0x6635:
     assert trace==[('write',0x1d8a,0),('write',0x1d90,0),('enable',0)]
     assert port[0x1d88]==enable|1 and port[0x1d8a]==0x42 and port[0x1d90]==0x84
    cases+=1
 dispatch=0
 for selector in range(8):
  for on in (0,1):
   adie=0x6635
   assert call('wmt_plat_soc_paldo_ctrl',selector,on)==0
   expected={0:'mtk_wcn_consys_hw_bt_paldo_ctrl',1:'mtk_wcn_consys_hw_wifi_paldo_ctrl',6:'mtk_wcn_consys_hw_efuse_paldo_ctrl'}
   assert trace==([(expected[selector],on,1 if selector==6 else 0)] if selector in expected else [])
   dispatch+=1
 adie=0x6631;fail=True
 assert call('consys_hw_vcn28_ctrl',1)==0 # Original swallows provider errors.
 port={0x1d88:1,0x1d8a:0x42,0x1d90:0x84,0x1d96:0x8000};rail=Rail()
 assert lib.MtkVcn28Initialize(C.byref(rail),C.byref(io),adie)==0
 assert lib.MtkVcn28Set(C.byref(rail),2,1)==4 and rail.Faulted and rail.Captured and rail.Users==2
 fail=False;assert lib.MtkVcn28Set(C.byref(rail),2,0)==0
 rr,rs,rsy,rt,rdata=read_module(path.parents[1]/'vendor_boot_a_0/mt6358-regulator.ko')
 assert hashlib.sha256(rr).hexdigest()=='393eadc1ce78ebe87fb4bcf9b0c041ccb46d78cb0a04ed86e7506b0cd4a78b31'
 descriptor=rdata(1);base=0x2640
 fields={hex(x):S.unpack_from('<I',descriptor,base+x)[0] for x in (0xc0,0xc4,0x140,0x144)}
 assert fields=={'0xc0':0x1d88,'0xc4':1,'0x140':0x1d96,'0x144':0x8000}
 root=path.parents[3]/'Mu-Silicium';dtb=root.parent/'analysis/stock-fdt/effective.dtb'
 # read_module has authenticated the same stock inputs; the existing DT parser
 # reads properties only and has no module/device entry point.
 fdt=runpy.run_path(str(root/'Platforms/Teclast/t70proPkg/Linux/platform-modules.py'))
 nodes,_=fdt['tree'](dtb.read_bytes());props=nodes['/soc/pwrap@10026000/mt6366/mt6358regulator/ldo-vcn28']['properties']
 assert 'regulator-always-on' in props
 assert S.unpack('>I',props['regulator-min-microvolt'])[0]==S.unpack('>I',props['regulator-max-microvolt'])[0]==2800000
 assert S.unpack('>I',props['regulator-enable-ramp-delay'])[0]==270
 return {'status':'passed','comparisons':cases+dispatch+1,'power_lease_cases':cases,'ADIE_6635_bypass_cases':bypass,'dispatch_cases':dispatch,'original_provider_error_cases':1,
  'original_functions':{n:{'section':sy[n][1],'offset':sy[n][2],'bytes':sy[n][3]} for n in selected},
  'relocations':relocations,'executed_virtual_services':sorted(executed),'MT6789_ops_VCN28_callback_offset':0x88,
  'regulator_module_sha256':hashlib.sha256(rr).hexdigest(),'VCN28_descriptor_offset':base,'descriptor_fields':fields,
  'DTB_sha256':hashlib.sha256(dtb.read_bytes()).hexdigest(),'DT_voltage_uv':2800000,'DT_always_on':True,'DT_enable_delay_us':270,
  'intentional_differences':['Read and verify PMIC writes, propagate failed/ignored IO and retain failed ownership; original returns zero on regulator/regmap errors','One arbiter tracks GPS/FM leases; only the last confirmed/quiesced owner restores inherited mode/select','Preserve inherited enable bits and the exact DT always-on supply instead of blindly regulator_disable/mode zero','Skip redundant stable writes; fixed voltage needs no selector programming','Virtual ADIE probes (including 0x6631) do not identify the fitted ADIE or runtime configuration'],
  'scope':'actual selected original ARM64 dispatch/wrapper/MT6789 callback with virtual regmap, regulator and ADIE probe; production C physical/lease core; no real PMIC, WMT on/off, RF, GNSS consumer or positioning execution'}
