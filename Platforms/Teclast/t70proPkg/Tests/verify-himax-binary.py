#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare production HimaxCore C with the shipped ARM64 driver in isolated Unicorn.

Report, startup, sense-off, firmware parser/AP CRC, page program and HW CRC
bodies execute with stock ELF relocations and mocked bus/OS edges. No kernel,
physical device bus, USB, network, or tablet is accessed.
"""
from pathlib import Path
import ctypes as C
import hashlib
import json
import random
import struct as S
import subprocess
import tempfile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC

PKG=Path(__file__).resolve().parents[1]
REPO=PKG.parents[2]
BINARY=REPO.parent/'analysis/stock-binaries/vendor_dlkm_a/hxchipset_i2c.ko'
FIRMWARE=REPO.parent/'analysis/stock-binaries/vendor_a_firmware/Himax_firmware.bin'
FIRMWARE_SHA='49852ca3bcc829247f319372003a7c12fac2b3b9a517d61a50c066a24df26add'
SHA='c1d7d3ae53580d3d3a0b7cfdd99e01785d5d8fd6b5fd42b5d89659c00bd64929'
class Contact(C.Structure):
    _fields_=[('X',C.c_ushort),('Y',C.c_ushort),('Pressure',C.c_ubyte),('Active',C.c_ubyte)]
class Frame(C.Structure):
    _fields_=[('Contacts',Contact*10),('Count',C.c_ubyte),('DeclaredCount',C.c_ubyte),('State',C.c_ubyte*2)]
class Descriptor(C.Structure):
    _fields_=[('Tag',C.c_uint),('Value',C.c_uint),('Offset',C.c_uint)]
class FirmwareInfo(C.Structure):
    _fields_=[('Length',C.c_uint),('Crc32c',C.c_uint),('DescriptorCount',C.c_uint),('Descriptors',Descriptor*5)]
HM_TIMEOUT=6
class SenseOffProfile(C.Structure):
    _fields_=[('Version',C.c_uint),('Size',C.c_uint),('FeatureFlags',C.c_uint),('KnownFlagsMask',C.c_uint)]

def compare_startup(core, raw, sections, content, symbol_tables, symbols, base):
    """Execute original wake/reset/sense-on bodies with only bus/OS calls trapped.

    Compare wire requests and delay ranges to production portable C. Stock void
    functions cannot report exhaustion. The diagnostic API reports HmTimeout,
    while sense-on follows the stock continuation after readback mismatches.
    """
    from unicorn.arm64_const import UC_ARM64_REG_X3, UC_ARM64_REG_X8
    names=('hx_parse_assign_cmd','hx83132_burst_enable','hx83132_register_write',
           'hx83132_system_reset','himax_mcu_interface_on','hx83132_sense_on')
    ranges=[(symbols[n][1],symbols[n][2],symbols[n][2]+symbols[n][3]) for n in names]
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    end=max(base[i]+((sections[i][5]+4095)&~4095) for i in base)
    uc.mem_map(0x10000000,end-0x10000000)
    for i,addr in base.items():
        if sections[i][1]!=8:uc.mem_write(addr,content(i))
    STUB=0x11000000;STOP=STUB+0x100
    uc.mem_map(STUB,4096); externals={}; relocations=0
    def address(sym):
        name,index,value,size=sym
        if not index:
            if name not in ('_printk','mutex_lock','mutex_unlock','__stack_chk_fail','usleep_range_state'):
                raise ValueError('Unexpected startup external: '+name)
            if name not in externals:externals[name]=STUB+len(externals)*16
            return externals[name]
        return base[index]+value
    for i,sec in enumerate(sections):
        if sec[1]!=4:continue
        entries=symbol_tables[sec[6]]
        for off in range(0,sec[5],24):
            offset,info,addend=S.unpack_from('<QQq',content(i),off)
            if not any(sec[7]==idx and lo<=offset<hi for idx,lo,hi in ranges):continue
            kind=info&0xffffffff;target=address(entries[info>>32])+addend;place=base[sec[7]]+offset
            ins=S.unpack('<I',uc.mem_read(place,4))[0]
            if kind==275:
                delta=(target>>12)-(place>>12)
                if not -(1<<20)<=delta<(1<<20):raise ValueError('Startup ADRP overflow')
                ins=(ins&~((3<<29)|(0x7ffff<<5)))|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
            elif kind in (277,278,284,285,286):
                shift={277:0,278:0,284:1,285:2,286:3}[kind]
                ins=(ins&~(0xfff<<10))|(((target&0xfff)>>shift)<<10)
            elif kind in (282,283):
                delta=target-place
                if delta%4 or not -(1<<27)<=delta<(1<<27):raise ValueError('Startup branch overflow')
                ins=(ins&~0x3ffffff)|((delta>>2)&0x3ffffff)
            else:raise ValueError('Unsupported startup relocation '+str(kind))
            uc.mem_write(place,S.pack('<I',ins));relocations+=1
    DATA=0x30000000;STACK=0x40000000;TLS=DATA+0x1000;TS=DATA+0x2000
    uc.mem_map(DATA,0x10000);uc.mem_map(STACK,0x10000)
    uc.mem_write(address(symbols['hx_s_ts']),S.pack('<Q',TS))
    table=address(symbols['hx_s_core_fp'])
    for off,name in ((0x10,'hx83132_register_write'),(0x18,'himax_mcu_interface_on')):
        uc.mem_write(table+off,S.pack('<Q',address(symbols[name])))
    special={address(symbols[n]):n for n in ('himax_bus_read','himax_bus_write')}
    special.update({address(symbols[n]):n for n in externals})
    special[address(symbols['usleep_range_state'])]='delay'
    trace=[];bad=0;readbacks=0
    def read_response(command,length):
        nonlocal readbacks
        data=bytearray(bytes((0x55,0x2a,0x13,0x83))*((length+3)//4))[:length]
        if command==0x13:data[0]=0 if readbacks<bad else 0x31
        if command==0x0d:
            data[0]=0 if readbacks<bad else 0x10
            readbacks+=1
        return bytes(data)
    def transfer(write,length):
        response=read_response(write[0],length) if length else b''
        trace.append(('io',bytes(write).hex(),length,response.hex()))
        return response
    def trap(u,a,size,ctx):
        if a==STOP:u.emu_stop();return
        name=special.get(a)
        if name:
            x0=u.reg_read(UC_ARM64_REG_X0);x1=u.reg_read(UC_ARM64_REG_X1)
            x2=u.reg_read(UC_ARM64_REG_X2);x3=u.reg_read(UC_ARM64_REG_X3)
            if name=='himax_bus_read':u.mem_write(x1,transfer(bytes([x0]),x2))
            elif name=='himax_bus_write':
                body=bytes(u.mem_read(x2,x3)) if x1==0xffffdaf1 else S.pack('<I',x1)+bytes(u.mem_read(x2,x3-4))
                transfer(bytes([x0])+body,0)
            elif name=='delay':trace.append(('delay',x0,x1))
            elif name=='__stack_chk_fail':raise RuntimeError('Stock stack cookie mismatch')
            u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR));return
        if not any(base[idx]+lo<=a<base[idx]+hi for idx,lo,hi in ranges):
            raise RuntimeError('Untrapped startup execution: '+hex(a))
        # Linux current-task TLS access is the only privileged CPU operation.
        if S.unpack('<I',u.mem_read(a,4))[0]==0xd5384108:
            u.reg_write(UC_ARM64_REG_X8,TLS);u.reg_write(UC_ARM64_REG_PC,a+4)
    uc.hook_add(UC_HOOK_CODE,trap)
    Transfer=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(C.c_ubyte),C.c_uint,C.POINTER(C.c_ubyte),C.c_uint)
    Delay=C.CFUNCTYPE(None,C.c_void_p,C.c_uint,C.c_uint)
    class Io(C.Structure):_fields_=[('context',C.c_void_p),('transfer',Transfer),('delay',Delay)]
    callback_errors=[]
    @Transfer
    def native_transfer(context,write,wlen,read,rlen):
        try:
            response=transfer(bytes(write[:wlen]),rlen)
            if rlen:C.memmove(read,response,rlen)
            return 0
        except Exception as exc:callback_errors.append(str(exc));return 5
    @Delay
    def native_delay(context,lo,hi):trace.append(('delay',lo,hi))
    io=Io(None,native_transfer,native_delay);cases=[]
    for stock,port in (('himax_mcu_interface_on','HmInterfaceOn'),('hx83132_sense_on','HmSenseOn'),('hx83132_system_reset','HmSystemReset')):
        fn=getattr(core,port);fn.argtypes=[C.POINTER(Io)];fn.restype=C.c_int
        for failures in (range(11) if port!='HmSystemReset' else (0,)):
            bad=failures;readbacks=0;trace.clear()
            uc.reg_write(UC_ARM64_REG_X0,1 if stock=='hx83132_sense_on' else 0)
            uc.reg_write(UC_ARM64_REG_SP,STACK+0xff00);uc.reg_write(UC_ARM64_REG_LR,STOP)
            uc.emu_start(address(symbols[stock]),STOP,timeout=1000000,count=50000)
            if uc.reg_read(UC_ARM64_REG_PC)!=STOP:raise RuntimeError('Stock startup failed to return')
            expected=list(trace);trace.clear();readbacks=0
            status=fn(C.byref(io));actual=list(trace)
            if callback_errors:raise RuntimeError(callback_errors)
            assert status==(6 if failures==10 and port=='HmInterfaceOn' else 0),(port,status)
            assert actual==expected,(stock,failures,actual,expected)
            cases.append({'function':stock,'bad_readbacks':failures,'wire_requests':sum(t[0]=='io' for t in actual),'delays':sum(t[0]=='delay' for t in actual),'port_status':status})
    return {'status':'passed','cases':len(cases),'elf_relocations_applied':relocations,
        'stock_text_offsets':{n:hex(symbols[n][2]) for n in names},'trapped_calls':list(special.values()),
        'comparison':'Exact write bytes, read lengths/results and delay ranges; readback retries 0..10',
        'diagnostic_difference':'HmInterfaceOn reports exhausted readback mismatches as HmTimeout; HmSenseOn follows the complete stock continuation',
        'case_results':cases}


def make_stock_machine(raw, sections, content, symbol_tables, symbols, base, ranges, allowed_external):
    """Map the module and relocate only the selected functions and their data references."""
    from unicorn.arm64_const import UC_ARM64_REG_X8
    end=max(base[i]+((sections[i][5]+4095)&~4095) for i in base)
    if end>0x11000000: raise ValueError("Stock module overlaps external-call stubs")
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    uc.mem_map(0x10000000,end-0x10000000)
    for i,addr in base.items():
        if sections[i][1]!=8: uc.mem_write(addr,content(i))
    STUB=0x11000000; STOP=STUB+0x100
    uc.mem_map(STUB,4096); externals={}; relocations=0
    def address(sym):
        name,index,value,size=sym
        if index==0:
            if name not in allowed_external: raise ValueError("Unexpected module external: "+name)
            if name not in externals: externals[name]=STUB+len(externals)*16
            return externals[name]
        if index==0xfff1: return value
        if index not in base: raise ValueError("Unmapped module symbol: "+name+" section="+str(index))
        return base[index]+value
    for i,sec in enumerate(sections):
        if sec[1]!=4: continue
        entries=symbol_tables[sec[6]]
        for off in range(0,sec[5],24):
            offset,info,addend=S.unpack_from('<QQq',content(i),off)
            if not any(sec[7]==idx and lo<=offset<hi for idx,lo,hi in ranges): continue
            kind=info&0xffffffff; target=address(entries[info>>32])+addend
            place=base[sec[7]]+offset
            ins=S.unpack('<I',uc.mem_read(place,4))[0]
            if kind==275:
                delta=(target>>12)-(place>>12)
                if not -(1<<20)<=delta<(1<<20): raise ValueError("Stock ADRP relocation overflow")
                ins=(ins&~((3<<29)|(0x7ffff<<5)))|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
            elif kind in (277,278,284,285,286):
                shift={277:0,278:0,284:1,285:2,286:3}[kind]
                ins=(ins&~(0xfff<<10))|(((target&0xfff)>>shift)<<10)
            elif kind in (282,283):
                delta=target-place
                if delta%4 or not -(1<<27)<=delta<(1<<27): raise ValueError("Stock branch relocation overflow")
                ins=(ins&~0x3ffffff)|((delta>>2)&0x3ffffff)
            else: raise ValueError("Unsupported stock relocation "+str(kind))
            uc.mem_write(place,S.pack('<I',ins)); relocations+=1
    uc.mem_map(0x30000000,0x10000)
    uc.mem_map(0x40000000,0x10000)
    return uc,STOP,externals,address,relocations

class ScriptedBus:
    def __init__(self):
        self.selected=None
        self.registers={0x80070004:bytearray(4)}
        self.read_script={
            0x900000a8:[0x05,0x0c,0x05,0x05],
            0x9000005c:[0x05,0x0c,0x6a],
            0x80070034:[0x00,0x00],
        }
        self.read_index={}
        self.trace=[]
    def transfer(self,write,length):
        response=b''
        if write and write[0]==0 and len(write)>=5:
            self.selected=int.from_bytes(write[1:5],"little")
            if len(write)>5:
                self.registers[self.selected]=bytearray(write[5:])
        if length:
            if write==b'\x08' and self.selected is not None:
                values=self.read_script.get(self.selected)
                if values:
                    index=self.read_index.get(self.selected,0)
                    value=values[min(index,len(values)-1)]
                    self.read_index[self.selected]=index+1
                    response=bytes([value])+bytes(max(0,length-1))
                else:
                    data=self.registers.get(self.selected,bytearray())
                    response=bytes(data[:length])+bytes(max(0,length-len(data)))
            else:
                response=bytes(length)
        self.trace.append(("io",write.hex(),length,response.hex()))
        return response
    def delay(self,minimum,maximum):
        self.trace.append(("delay",minimum,maximum))

def compare_sense_off(core, raw, sections, content, symbol_tables, symbols, base, firmware_info):
    from unicorn.arm64_const import UC_ARM64_REG_X3, UC_ARM64_REG_X8
    names=('hx_parse_assign_cmd','hx83132_burst_enable','hx83132_register_read',
           'hx83132_system_reset','hx83132_sense_off','himax_stop_FW_byHost_enable',
           'hx83132_wait_broadcast_line')
    for name,expected in (('hx83132_sense_off',(0xe1b0,0xb74)),
                          ('hx83132_system_reset',(0xd39c,0xb8)),
                          ('hx83132_register_read',(0xe09c,0x110)),
                          ('himax_stop_FW_byHost_enable',(0xf32c,0x220)),
                          ('hx83132_wait_broadcast_line',(0xf54c,0x2c8))):
        if symbols[name][2:4]!=expected: raise ValueError("Stock function fingerprint mismatch: "+name)
    ranges=[(symbols[n][1],symbols[n][2],symbols[n][2]+symbols[n][3]) for n in names]
    allowed={'_printk','mutex_lock','mutex_unlock','__stack_chk_fail','usleep_range_state'}
    uc,STOP,externals,address,relocations=make_stock_machine(raw,sections,content,symbol_tables,symbols,base,ranges,allowed)
    DATA=0x30000000; STACK=0x40000000; TLS=DATA+0x1000; TS=DATA+0x2000; IC=DATA+0x3000
    uc.mem_write(address(symbols['hx_s_ts']),S.pack('<Q',TS))
    uc.mem_write(address(symbols['hx_s_ic_data']),S.pack('<Q',IC))
    uc.mem_write(TS+1,b'\0')
    uc.mem_write(TS+0x78,b'\0')
    special={address(symbols[n]):n for n in ('himax_bus_read','himax_bus_write')}
    special.update({address(symbols[n]):n for n in externals})
    stock_bus=ScriptedBus(); pc_history=[]
    def trap(u,a,size,ctx):
        pc_history.append((a,u.reg_read(UC_ARM64_REG_LR),u.reg_read(UC_ARM64_REG_X0),
                          u.reg_read(UC_ARM64_REG_X1),u.reg_read(UC_ARM64_REG_X2),
                          u.reg_read(UC_ARM64_REG_X3)))
        if len(pc_history)>32: del pc_history[0]
        if a==STOP:
            u.emu_stop(); return
        name=special.get(a)
        if name:
            x0=u.reg_read(UC_ARM64_REG_X0); x1=u.reg_read(UC_ARM64_REG_X1)
            x2=u.reg_read(UC_ARM64_REG_X2); x3=u.reg_read(UC_ARM64_REG_X3)
            if name=='himax_bus_read':
                u.mem_write(x1,stock_bus.transfer(bytes([x0&255]),x2))
            elif name=='himax_bus_write':
                if x1==0xffffdaf1:
                    body=bytes(u.mem_read(x2,x3))
                else:
                    if x3<4: raise RuntimeError("Stock bus write length underflow")
                    body=S.pack('<I',x1&0xffffffff)+bytes(u.mem_read(x2,x3-4))
                stock_bus.transfer(bytes([x0&255])+body,0)
            elif name=='usleep_range_state':
                stock_bus.delay(x0,x1)
            elif name=='__stack_chk_fail':
                raise RuntimeError("Stock sense-off stack cookie mismatch")
            u.reg_write(UC_ARM64_REG_X0,0)
            u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
            return
        if not any(base[idx]+lo<=a<base[idx]+hi for idx,lo,hi in ranges):
            raise RuntimeError("Untrapped sense-off execution: "+hex(a)+" previous="+repr([(hex(pc),hex(lr),hex(x0),hex(x1),hex(x2),hex(x3)) for pc,lr,x0,x1,x2,x3 in pc_history[-12:]]))
        if S.unpack('<I',u.mem_read(a,4))[0]==0xd5384108:
            u.reg_write(UC_ARM64_REG_X8,TLS); u.reg_write(UC_ARM64_REG_PC,a+4)
    uc.hook_add(UC_HOOK_CODE,trap)
    class Io(C.Structure):
        pass
    Transfer=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(C.c_ubyte),C.c_uint,C.POINTER(C.c_ubyte),C.c_uint)
    Delay=C.CFUNCTYPE(None,C.c_void_p,C.c_uint,C.c_uint)
    Io._fields_=[('context',C.c_void_p),('transfer',Transfer),('delay',Delay)]
    core_bus=ScriptedBus(); callback_errors=[]
    @Transfer
    def native_transfer(context,write,wlen,read,rlen):
        try:
            response=core_bus.transfer(bytes(write[:wlen]),rlen)
            if rlen: C.memmove(read,response,rlen)
            return 0
        except Exception as exc:
            callback_errors.append(str(exc)); return 5
    @Delay
    def native_delay(context,lo,hi):
        core_bus.delay(lo,hi)
    io=Io(None,native_transfer,native_delay)
    profile=SenseOffProfile(1,C.sizeof(SenseOffProfile),0,0x10)
    fn=core.HmSenseOff
    fn.argtypes=[C.POINTER(Io),C.POINTER(FirmwareInfo),C.POINTER(SenseOffProfile)]
    fn.restype=C.c_int
    vectors=[
        ('normal',{},0,1),
        ('stop_6a_first',{0x9000005c:[5,0x6a]},0,1),
        ('stop_no_6a',{0x9000005c:[5,1]},0,1),
        ('stop_already_in_safe_mode',{0x900000a8:[5,0x0c,0x0c,5]},0,1),
        ('final_status_clear',{0x900000a8:[5,0x0c,5,0x0c]},0,1),
        ('initial_safe_mode',{0x900000a8:[0x0c,0x0c,5,5],
                              0x9000005c:[0x0c,0x6a]},0,1),
        ('broadcast_first_99_busy',{0x80070034:[1]*99+[0,0]},0,1),
        ('broadcast_shared_99_busy',{0x80070034:[1]*49+[0]+[1]*50+[0]},0,1),
        ('broadcast_first_100_busy',{0x80070034:[1]*100+[0,0]},HM_TIMEOUT,1),
        ('broadcast_first_expired',{0x80070034:[1]*101+[0]},HM_TIMEOUT,1),
        ('broadcast_second_expired',{0x80070034:[0]+[1]*101},HM_TIMEOUT,1),
    ]
    for retries in range(1,6):
        vectors.append(('phase_retries_'+str(retries),
            {0x900000a8:[5]+[1]*retries+[0x0c,5,5]},0,1))
    vectors.append(('phase_expired',{0x900000a8:[5]+[1]*6},HM_TIMEOUT,0))
    cases=[]
    for label,script,expected_status,expected_stock_return in vectors:
        stock_bus=ScriptedBus(); stock_bus.read_script.update(script)
        core_bus=ScriptedBus(); core_bus.read_script.update(script)
        uc.reg_write(UC_ARM64_REG_X0,0)
        uc.reg_write(UC_ARM64_REG_SP,STACK+0xff00)
        uc.reg_write(UC_ARM64_REG_LR,STOP)
        uc.emu_start(address(symbols['hx83132_sense_off']),STOP,timeout=10000000,count=50000000)
        if uc.reg_read(UC_ARM64_REG_PC)!=STOP: raise RuntimeError("Stock sense-off failed to return: "+label)
        stock_return=uc.reg_read(UC_ARM64_REG_X0)&255
        assert stock_return==expected_stock_return,(label,stock_return)
        status=fn(C.byref(io),C.byref(firmware_info),C.byref(profile))
        if callback_errors: raise RuntimeError("Native sense-off callback failed: "+str(callback_errors))
        assert status==expected_status,(label,status,expected_status)
        if core_bus.trace!=stock_bus.trace:
            common=min(len(stock_bus.trace),len(core_bus.trace))
            mismatch=next((i for i in range(common) if stock_bus.trace[i]!=core_bus.trace[i]),common)
            start=max(0,mismatch-3); end=mismatch+4
            raise AssertionError("Sense-off mismatch "+label+" index="+str(mismatch)+
                " stock_length="+str(len(stock_bus.trace))+" core_length="+str(len(core_bus.trace))+
                " stock_context="+repr(stock_bus.trace[start:end])+" core_context="+repr(core_bus.trace[start:end]))
        cases.append({'case':label,'port_status':status,'stock_return':stock_return,
            'wire_requests':sum(t[0]=='io' for t in core_bus.trace),
            'delays':sum(t[0]=='delay' for t in core_bus.trace),
            'trace_sha256':hashlib.sha256(json.dumps(core_bus.trace,separators=(',',':')).encode()).hexdigest()})
    return {'status':'passed','stock_text_offsets':{n:hex(symbols[n][2]) for n in names},
            'elf_relocations_applied':relocations,'profile':'stock argument zero, hx_s_ts[0x78].bit4=0; no slave scan',
            'binary_returned_to_sentinel':True,'case_results':cases,
            'comparison':'Complete exact stock wire bytes, read results and delay ranges through return',
            'intentional_difference':'An expired broadcast handshake returns HmTimeout; stock sense_off logs it but returns true'}

def c_firmware_descriptors(info):
    return {int(info.Descriptors[i].Tag):{'value':int(info.Descriptors[i].Value),
        'offset':int(info.Descriptors[i].Offset)} for i in range(info.DescriptorCount)}

def compare_crc_and_firmware(core, raw, sections, content, symbol_tables, symbols, base):
    from unicorn.arm64_const import UC_ARM64_REG_X2
    firmware=FIRMWARE.read_bytes()
    if len(firmware)!=261120 or hashlib.sha256(firmware).hexdigest()!=FIRMWARE_SHA:
        raise ValueError("T70 firmware artifact identity mismatch")
    crc_fn=symbols['himax_mcu_calc_crc_by_ap']
    if crc_fn[2:4]!=(0x10238,0x5c): raise ValueError("Stock AP CRC function fingerprint mismatch")
    text_index=crc_fn[1]; text_addr=base[text_index]
    crc_uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM); text_size=(sections[text_index][5]+4095)&~4095
    crc_uc.mem_map(text_addr,text_size); crc_uc.mem_write(text_addr,content(text_index))
    STOP=0x11000100; crc_uc.mem_map(0x11000000,4096)
    DATA=0x30000000; FW=0x50000000
    crc_uc.mem_map(DATA,0x10000); crc_uc.mem_map(FW,0x40000)
    def stock_crc(data,length):
        crc_uc.mem_write(FW,data)
        crc_uc.reg_write(UC_ARM64_REG_X0,FW)
        crc_uc.reg_write(UC_ARM64_REG_X2,length)
        crc_uc.reg_write(UC_ARM64_REG_SP,DATA+0xff00)
        crc_uc.reg_write(UC_ARM64_REG_LR,STOP)
        crc_uc.emu_start(text_addr+crc_fn[2],STOP,timeout=10000000,count=50000000)
        if crc_uc.reg_read(UC_ARM64_REG_PC)!=STOP: raise RuntimeError("Stock CRC function failed to return")
        return crc_uc.reg_read(UC_ARM64_REG_X0)&0xffffffff
    crc_api=core.HmCrc32c
    crc_api.argtypes=[C.c_void_p,C.c_uint,C.POINTER(C.c_uint)]; crc_api.restype=C.c_int
    crc_cases=[]
    crc_vectors=[('stock',firmware,n) for n in (4,8,16,0x80,0x1000,0x10000,len(firmware))]
    changed=bytearray(firmware); changed[0x1234]^=0x01
    crc_vectors.extend(('mutated_0x1234',bytes(changed),n) for n in (0x2000,0x10000,len(firmware)))
    changed=bytearray(firmware); changed[-4]^=0x80
    crc_vectors.append(('mutated_last_word',bytes(changed),len(firmware)))
    for label,data,length in crc_vectors:
        buf=C.create_string_buffer(data,len(data)); result=C.c_uint(0xa5a5a5a5)
        status=crc_api(buf,length,C.byref(result))
        if status!=0: raise AssertionError("HmCrc32c rejected aligned vector "+label)
        expected=stock_crc(data,length)
        if result.value!=expected:
            raise AssertionError("AP CRC mismatch "+label+" length="+hex(length))
        crc_cases.append({'input':label,'length':length,'crc32c':hex(expected)})

    parse_names=('hx83132_bin_desc_get','hx_bin_desc_data_get')
    if symbols['hx83132_bin_desc_get'][2:4]!=(0xd584,0x134) or symbols['hx_bin_desc_data_get'][2:4]!=(0x1358c,0x2d8):
        raise ValueError("Stock firmware parser fingerprint mismatch")
    parse_ranges=[(symbols[n][1],symbols[n][2],symbols[n][2]+symbols[n][3]) for n in parse_names]
    parser,STOP,externals,address,relocations=make_stock_machine(
        raw,sections,content,symbol_tables,symbols,base,parse_ranges,{'_printk','__stack_chk_fail'})
    DATA=0x30000000; INFO=DATA+0x4000; TS=DATA+0x8000; FW=0x50000000
    parser.mem_map(FW,0x40000)
    parser.mem_write(address(symbols['hx_s_fwbin']),S.pack('<Q',INFO))
    parser.mem_write(address(symbols['hx_s_ts']),S.pack('<Q',TS))
    core_table=address(symbols['hx_s_core_fp'])
    parser.mem_write(core_table+0x1bc,S.pack('<I',0x35043f2d))
    parser.mem_write(core_table+0x1c0,S.pack('<Q',address(symbols['hx_bin_desc_data_get'])))
    special={address(symbols[name]):name for name in externals}
    def parser_trap(u,a,size,ctx):
        from unicorn.arm64_const import UC_ARM64_REG_X8
        if a==STOP: u.emu_stop(); return
        name=special.get(a)
        if name:
            if name=='__stack_chk_fail': raise RuntimeError("Stock firmware parser stack cookie mismatch")
            u.reg_write(UC_ARM64_REG_X0,0);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR));return
        if not any(base[idx]+lo<=a<base[idx]+hi for idx,lo,hi in parse_ranges):
            raise RuntimeError("Untrapped firmware parser execution: "+hex(a))
        if S.unpack('<I',u.mem_read(a,4))[0]==0xd5384108:
            u.reg_write(UC_ARM64_REG_X8,DATA+0x1000);u.reg_write(UC_ARM64_REG_PC,a+4)
    parser.hook_add(UC_HOOK_CODE,parser_trap)
    def stock_parse(data):
        parser.mem_write(FW,data)
        parser.mem_write(INFO,b'\0'*0x100)
        parser.mem_write(TS+0x78,b'\0')
        parser.reg_write(UC_ARM64_REG_X0,FW);parser.reg_write(UC_ARM64_REG_X1,len(data))
        parser.reg_write(UC_ARM64_REG_SP,0x4000ff00);parser.reg_write(UC_ARM64_REG_LR,STOP)
        parser.emu_start(address(symbols['hx83132_bin_desc_get']),STOP,timeout=10000000,count=50000000)
        if parser.reg_read(UC_ARM64_REG_PC)!=STOP: raise RuntimeError("Stock descriptor parser failed to return")
        values={0x00000a00:0x30,0x10000000:0x20,0x10000100:0x00,0x10000600:0x10}
        parsed={tag:S.unpack('<I',parser.mem_read(INFO+offset,4))[0] for tag,offset in values.items()}
        parsed={tag:value for tag,value in parsed.items() if value}
        return parser.reg_read(UC_ARM64_REG_X0)&0xffffffff,parsed
    inspect=core.HmInspectFirmware
    inspect.argtypes=[C.c_void_p,C.c_uint,C.POINTER(FirmwareInfo)];inspect.restype=C.c_int
    validate=core.HmValidateT70Firmware
    validate.argtypes=[C.c_void_p,C.c_uint,C.POINTER(FirmwareInfo)];validate.restype=C.c_int
    def core_parse(data):
        buf=C.create_string_buffer(data,len(data)); info=FirmwareInfo()
        status=inspect(buf,len(data),C.byref(info))
        return status,c_firmware_descriptors(info),info
    def compare_parse(label,data,expected_stock,expected_core):
        stock_status,stock_desc=stock_parse(data)
        core_status,core_desc,info=core_parse(data)
        if stock_status!=expected_stock or core_status!=expected_core:
            raise AssertionError("Firmware parser status mismatch "+label+": stock="+str(stock_status)+" core="+str(core_status))
        if stock_status and stock_desc!={tag:entry['value'] for tag,entry in core_desc.items()}:
            raise AssertionError("Firmware descriptor mismatch "+label+": stock="+repr(stock_desc)+" core="+repr(core_desc))
        return {'input':label,'stock_return':stock_status,'core_status':core_status,
                'descriptors':len(core_desc),'descriptor_values_match':bool(stock_status)}
    parser_cases=[compare_parse('stock_blob',firmware,1,0)]
    original_info=FirmwareInfo(); fw_buf=C.create_string_buffer(firmware,len(firmware))
    validate_status=validate(fw_buf,len(firmware),C.byref(original_info))
    if validate_status!=0: raise AssertionError("Actual T70 firmware failed format/CRC validation: "+str(validate_status))
    parsed_info=FirmwareInfo(); inspect(fw_buf,len(firmware),C.byref(parsed_info))
    sense=compare_sense_off(core,raw,sections,content,symbol_tables,symbols,base,parsed_info)

    payload_mut=bytearray(firmware);payload_mut[0x1234]^=0x01
    parser_cases.append(compare_parse('payload_mutation',bytes(payload_mut),1,0))
    checksum_mut=bytearray(firmware);checksum_mut[0x7f]^=0x01
    parser_cases.append(compare_parse('descriptor_checksum_mutation',bytes(checksum_mut),1,0))
    header_mut=bytearray(firmware);header_mut[0]=0x01
    parser_cases.append(compare_parse('header_mutation',bytes(header_mut),0,1))
    check_info=FirmwareInfo();check_buf=C.create_string_buffer(bytes(checksum_mut),len(checksum_mut))
    check_variant=validate(check_buf,len(checksum_mut),C.byref(check_info))
    if check_variant==0: raise AssertionError("T70 variant validator accepted a changed descriptor record")
    return {'status':'passed','firmware_path':str(FIRMWARE),'firmware_sha256':FIRMWARE_SHA,
            'firmware_bytes':len(firmware),'ap_crc_helper':{'text_offset':hex(crc_fn[2]),
                'cases':len(crc_cases),'vectors':crc_cases,'interpretation':'CRC32C reflected, init 0xffffffff, no final xor; aligned lengths only'},
            'descriptor_parser':{'stock_offsets':{n:hex(symbols[n][2]) for n in parse_names},
                'elf_relocations_applied':relocations,'cases':parser_cases,
                'format_validation_status':validate_status,'t70_variant_mutation_status':check_variant},
            'sense_off':sense}


def compare_flash_update(core, raw, sections, content, symbol_tables, symbols, base):
    """Execute the stock page writer and HW CRC reader against a deterministic bus mock."""
    from unicorn.arm64_const import UC_ARM64_REG_X3, UC_ARM64_REG_X8
    firmware=FIRMWARE.read_bytes()
    if len(firmware)!=261120 or hashlib.sha256(firmware).hexdigest()!=FIRMWARE_SHA:
        raise ValueError("T70 firmware artifact identity mismatch")
    expected={
        "himax_mcu_flash_programming":(0x128d8,0x478),
        "himax_mcu_calc_crc_by_hw":(0x10298,0x224),
        "himax_mcu_interface_on":(0xfb20,0x174),
        "himax_mcu_wait_wip":(0xfc98,0x220),
        "hx83132_register_write":(0xd2f4,0xa4),
        "hx83132_register_read":(0xe09c,0x110),
        "hx83132_burst_enable":(0xd208,0xe8),
    }
    for name,fingerprint in expected.items():
        if symbols[name][2:4]!=fingerprint:
            raise ValueError("Stock flash function fingerprint mismatch: "+name)

    class UpdateProfile(C.Structure):
        _fields_=[("Version",C.c_uint),("Size",C.c_uint),
                  ("Facts",C.c_uint),("KnownFactsMask",C.c_uint)]
    class Progress(C.Structure):
        _fields_=[("Stage",C.c_uint),("TotalBytes",C.c_uint),
                  ("BytesCompleted",C.c_uint),("PagesCompleted",C.c_uint),
                  ("CurrentOffset",C.c_uint),("ObservedCrc32c",C.c_uint),
                  ("FlashContentsMayBeChanged",C.c_uint),("LastStatus",C.c_uint)]

    crc_api=core.HmCrc32c
    crc_api.argtypes=[C.c_void_p,C.c_uint,C.POINTER(C.c_uint)]
    crc_api.restype=C.c_int
    firmware_buffer=C.create_string_buffer(firmware,len(firmware))
    expected_crc=C.c_uint()
    if crc_api(firmware_buffer,len(firmware),C.byref(expected_crc))!=0:
        raise AssertionError("Core AP CRC rejected stock firmware")

    class FlashBus:
        def __init__(self, crc, program_busy=0, crc_busy=0):
            self.crc=crc
            self.program_busy=program_busy
            self.crc_busy=crc_busy
            self.control=0
            self.selected=None
            self.trace=[]
        def transfer(self, write, length):
            write=bytes(write)
            if write and write[0]==0 and len(write)>=5:
                self.selected=int.from_bytes(write[1:5],"little")
                if self.selected==0x80000020 and len(write)==9:
                    self.control=int.from_bytes(write[5:9],"little")
            response=bytes(length)
            if length:
                if write==b"\x13":
                    response=b"\x31"+bytes(max(0,length-1))
                elif write==b"\x0d":
                    response=b"\x10"+bytes(max(0,length-1))
                elif write==b"\x08":
                    value=0
                    if self.selected==0x900000d0:
                        response=bytes((0x55,0x2a,0x13,0x83))+bytes(max(0,length-4))
                    elif self.selected==0x8000002c:
                        value=2
                        if self.control==0x42000003 and self.program_busy:
                            self.program_busy-=1
                            value|=1
                        response=bytes((value,))+bytes(max(0,length-1))
                    elif self.selected==0x80050000 and self.crc_busy:
                        self.crc_busy-=1
                        response=b"\x01"+bytes(max(0,length-1))
                    elif self.selected==0x80050018:
                        response=S.pack("<I",self.crc)[:length].ljust(length,b"\0")
                    else:
                        response=bytes(length)
            self.trace.append(("io",write.hex(),length,response.hex()))
            return response
        def delay(self, minimum, maximum):
            self.trace.append(("delay",minimum,maximum))

    names=("hx_parse_assign_cmd","hx83132_burst_enable",
           "hx83132_register_read","hx83132_register_write",
           "himax_mcu_interface_on","himax_mcu_wait_wip",
           "himax_mcu_flash_programming","himax_mcu_calc_crc_by_hw",
           "himax_mcu_read_FW_status")
    ranges=[(symbols[n][1],symbols[n][2],symbols[n][2]+symbols[n][3]) for n in names]
    externals={"_printk","mutex_lock","mutex_unlock","__stack_chk_fail","usleep_range_state"}
    fw=0x50000000
    data=0x30000000
    stack=0x40000000

    def setup_stock_machine(program_busy=0, crc_busy=0):
        uc,stop,external_addrs,address,relocations=make_stock_machine(
            raw,sections,content,symbol_tables,symbols,base,ranges,externals)
        bus=FlashBus(expected_crc.value, program_busy, crc_busy)
        ts=data+0x2000
        ic=data+0x3000
        uc.mem_write(address(symbols["hx_s_ts"]),S.pack("<Q",ts))
        uc.mem_write(address(symbols["hx_s_ic_data"]),S.pack("<Q",ic))
        uc.mem_write(ts+1,b"\0")
        uc.mem_write(ts+0x78,b"\0")
        table=address(symbols["hx_s_core_fp"])
        for offset,name in ((0x8,"hx83132_register_read"),
                            (0x10,"hx83132_register_write"),
                            (0x18,"himax_mcu_interface_on"),
                            (0x50,"himax_mcu_wait_wip"),
                            (0x138,"himax_mcu_read_FW_status")):
            uc.mem_write(table+offset,S.pack("<Q",address(symbols[name])))
        uc.mem_write(address(symbols["hx_s_ic_setup"]),
                     bytes(uc.mem_read(address(symbols["g_hx83132_setup"]),0x1b0)))
        special={address(symbols[name]):name for name in ("himax_bus_read","himax_bus_write")}
        special.update({address(symbols[name]):name for name in external_addrs})
        special[address(symbols["himax_mcu_read_FW_status"])]= "diagnostic_fw_status"
        def trap(machine,pc,size,context):
            if pc==stop:
                machine.emu_stop()
                return
            name=special.get(pc)
            if name:
                x0=machine.reg_read(UC_ARM64_REG_X0)
                x1=machine.reg_read(UC_ARM64_REG_X1)
                x2=machine.reg_read(UC_ARM64_REG_X2)
                x3=machine.reg_read(UC_ARM64_REG_X3)
                if name=="himax_bus_read":
                    machine.mem_write(x1,bus.transfer(bytes((x0&255,)),x2))
                elif name=="himax_bus_write":
                    if x1==0xffffdaf1:
                        body=bytes(machine.mem_read(x2,x3))
                    else:
                        if x3<4: raise RuntimeError("Stock flash bus write length underflow")
                        body=S.pack("<I",x1&0xffffffff)+bytes(machine.mem_read(x2,x3-4))
                    bus.transfer(bytes((x0&255,))+body,0)
                elif name=="usleep_range_state":
                    bus.delay(x0,x1)
                elif name=="diagnostic_fw_status":
                    bus.trace.append(("diagnostic_fw_status_mocked",))
                elif name=="__stack_chk_fail":
                    raise RuntimeError("Stock flash helper stack cookie mismatch")
                machine.reg_write(UC_ARM64_REG_X0,0)
                machine.reg_write(UC_ARM64_REG_PC,machine.reg_read(UC_ARM64_REG_LR))
                return
            if not any(base[index]+lo<=pc<base[index]+hi for index,lo,hi in ranges):
                raise RuntimeError("Untrapped stock flash execution: "+hex(pc))
            if S.unpack("<I",machine.mem_read(pc,4))[0]==0xd5384108:
                machine.reg_write(UC_ARM64_REG_X8,data+0x1000)
                machine.reg_write(UC_ARM64_REG_PC,pc+4)
        uc.hook_add(UC_HOOK_CODE,trap)
        uc.mem_write(data+0x1710,S.pack("<Q",0x12345678))
        return uc,stop,address,bus,relocations

    def marker_index(trace,address_value,payload):
        pattern=b"\x00"+S.pack("<I",address_value)+payload
        for index,event in enumerate(trace):
            if event[0]=="io" and bytes.fromhex(event[1]).startswith(pattern):
                return index
        raise AssertionError("Flash trace marker not found at "+hex(address_value))

    def compare_traces(label,stock_trace,port_trace,address_value,payload):
        stock_start=marker_index(stock_trace,address_value,payload)
        port_start=marker_index(port_trace,address_value,payload)
        stock=stock_trace[stock_start:]
        port=port_trace[port_start:]
        if stock!=port:
            common=min(len(stock),len(port))
            mismatch=next((i for i in range(common) if stock[i]!=port[i]),common)
            lo=max(0,mismatch-3)
            hi=mismatch+4
            def commands(events):
                out=[]
                for event in events[:80]:
                    if event[0]=="delay":
                        out.append(event)
                    elif event[0]=="io":
                        body=bytes.fromhex(event[1])
                        if len(body)>=9 and body[0]==0:
                            out.append((hex(int.from_bytes(body[1:5],"little")),
                                body[5:].hex() if len(body)==9 else "payload-bytes="+str(len(body)-5)))
                        elif body==b"\x08":
                            out.append(("read",event[2],event[3]))
                return out
            raise AssertionError(label+" trace mismatch index="+str(mismatch)+
                " stock_length="+str(len(stock))+" core_length="+str(len(port))+
                " stock_context="+repr(stock[lo:hi])+" core_context="+repr(port[lo:hi])+
                " stock_commands="+repr(commands(stock))+" core_commands="+repr(commands(port)))
        return {"stock_trace_events":len(stock),"core_trace_events":len(port),
                "stock_wire_requests":sum(event[0]=="io" for event in stock),
                "core_wire_requests":sum(event[0]=="io" for event in port),
                "matched_from_marker":hex(address_value)}

    def run_stock(function,arguments,program_busy=0,crc_busy=0):
        uc,stop,address,bus,relocations=setup_stock_machine(program_busy,crc_busy)
        uc.mem_map(fw,0x40000)
        if function=="himax_mcu_flash_programming":
            uc.mem_write(fw,firmware)
        elif function=="himax_mcu_calc_crc_by_hw":
            uc.mem_write(data+0x4000,S.pack("<I",0))
        uc.reg_write(UC_ARM64_REG_X0,arguments[0])
        uc.reg_write(UC_ARM64_REG_X1,arguments[1])
        uc.reg_write(UC_ARM64_REG_SP,stack+0xff00)
        uc.reg_write(UC_ARM64_REG_LR,stop)
        uc.emu_start(address(symbols[function]),stop,timeout=10000000,count=50000000)
        if uc.reg_read(UC_ARM64_REG_PC)!=stop:
            raise RuntimeError("Stock "+function+" did not return to sentinel")
        return uc.reg_read(UC_ARM64_REG_X0)&0xffffffff,bus.trace,relocations

    Transfer=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(C.c_ubyte),C.c_uint,
                         C.POINTER(C.c_ubyte),C.c_uint)
    Delay=C.CFUNCTYPE(None,C.c_void_p,C.c_uint,C.c_uint)
    class Io(C.Structure):
        _fields_=[("context",C.c_void_p),("transfer",Transfer),("delay",Delay)]

    def run_core(api_name,facts,program_busy=0,crc_busy=0,expected_status=0):
        bus=FlashBus(expected_crc.value,program_busy,crc_busy)
        errors=[]
        @Transfer
        def native_transfer(context,write,wlen,read,rlen):
            try:
                response=bus.transfer(bytes(write[:wlen]),rlen)
                if rlen: C.memmove(read,response,rlen)
                return 0
            except Exception as exc:
                errors.append(str(exc))
                return 5
        @Delay
        def native_delay(context,minimum,maximum):
            bus.delay(minimum,maximum)
        io=Io(None,native_transfer,native_delay)
        profile=UpdateProfile(1,C.sizeof(UpdateProfile),facts,0x1ff)
        progress=Progress()
        if api_name=="HmProgramT70Firmware":
            fn=core.HmProgramT70Firmware
            fn.argtypes=[C.POINTER(Io),C.c_void_p,C.c_uint,
                         C.POINTER(UpdateProfile),C.POINTER(Progress)]
            fn.restype=C.c_int
            status=fn(C.byref(io),firmware_buffer,len(firmware),
                      C.byref(profile),C.byref(progress))
        else:
            fn=core.HmVerifyT70FirmwareFlash
            fn.argtypes=[C.POINTER(Io),C.c_void_p,C.c_uint,
                         C.POINTER(UpdateProfile),C.POINTER(Progress)]
            fn.restype=C.c_int
            status=fn(C.byref(io),firmware_buffer,len(firmware),
                      C.byref(profile),C.byref(progress))
        if errors: raise RuntimeError("Core flash callback failed: "+repr(errors))
        if status!=expected_status: raise AssertionError(api_name+" status "+str(status)+" expected "+str(expected_status))
        return bus.trace,progress

    stock_return,stock_program_trace,program_relocations=run_stock(
        "himax_mcu_flash_programming",(fw,len(firmware)))
    port_program_trace,program_progress=run_core("HmProgramT70Firmware",0xff)
    if stock_return!=1 or program_progress.PagesCompleted!=len(firmware)//256:
        raise AssertionError("Stock/core page programming did not complete")
    program=compare_traces("Page-program",stock_program_trace,port_program_trace,
                           0x80000010,S.pack("<I",0x00020780))

    start_pointer=data+0x4000
    stock_crc_return,stock_crc_trace,crc_relocations=run_stock(
        "himax_mcu_calc_crc_by_hw",(start_pointer,len(firmware)))
    port_crc_trace,crc_progress=run_core("HmVerifyT70FirmwareFlash",0x1bf)
    if stock_crc_return!=expected_crc.value or crc_progress.ObservedCrc32c!=expected_crc.value:
        raise AssertionError("Stock/core hardware CRC result mismatch")
    crc=compare_traces("Hardware-CRC",stock_crc_trace,port_crc_trace,
                       0x80050020,S.pack("<I",0))
    busy_cases=[]
    for count in (2,101):
        stock_result,stock_trace,relocations=run_stock(
            "himax_mcu_flash_programming",(fw,len(firmware)),program_busy=count)
        port_trace,progress=run_core("HmProgramT70Firmware",0xff,
            program_busy=count,expected_status=6 if count==101 else 0)
        if stock_result!=(0 if count==101 else 1):
            raise AssertionError("Unexpected stock page busy outcome")
        match=compare_traces("Page busy "+str(count),stock_trace,port_trace,
                             0x80000010,S.pack("<I",0x00020780))
        if progress.PagesCompleted!=(0 if count==101 else 1020):
            raise AssertionError("Page progress after busy/timeout")
        busy_cases.append({"stage":"program","busy_reads":count,
                           "port_status":progress.LastStatus,**match})
    for count in (2,101):
        stock_result,stock_trace,relocations=run_stock(
            "himax_mcu_calc_crc_by_hw",(start_pointer,len(firmware)),crc_busy=count)
        port_trace,progress=run_core("HmVerifyT70FirmwareFlash",0x1bf,
            crc_busy=count,expected_status=6 if count==101 else 0)
        if count==101:
            stock_start=marker_index(stock_trace,0x80050020,S.pack("<I",0))
            port_start=marker_index(port_trace,0x80050020,S.pack("<I",0))
            prefix=port_trace[port_start:]
            if stock_result!=0 or stock_trace[stock_start:stock_start+len(prefix)]!=prefix:
                raise AssertionError("Hardware CRC timeout prefix mismatch")
            match={"matched_prefix_events":len(prefix),
                   "intentional_difference":"HmTimeout; stock invokes a mocked diagnostic FW-status callback and returns ambiguous zero"}
        else:
            match=compare_traces("CRC busy "+str(count),stock_trace,port_trace,
                                  0x80050020,S.pack("<I",0))
        busy_cases.append({"stage":"hardware_crc","busy_reads":count,
                           "port_status":progress.LastStatus,**match})
    return {"status":"passed","firmware_sha256":FIRMWARE_SHA,
            "firmware_bytes":len(firmware),
            "stock_functions":{name:hex(symbols[name][2]) for name in expected},
            "page_program":program,"hardware_crc":crc,"busy_and_timeout_cases":busy_cases,
            "elf_relocations_applied":program_relocations+crc_relocations,
            "programmed_pages":program_progress.PagesCompleted,
            "hardware_crc32c":hex(expected_crc.value),
            "comparison":"Exact bus write bytes, read lengths/results and delay ranges from the first stock program/CRC command; stock routines executed in ARM64 Unicorn with bus/OS edges mocked",
            "limitations":["No device I2C, erase, flash-unlock, reset, or restart is executed or inferred",
                           "Stock CRC-timeout diagnostic FW-status callback is mocked at the verified function-table edge (hx_s_core_fp+0x138)"]}

def main():
    raw=BINARY.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=SHA: raise ValueError('Shipped module identity mismatch')
    if raw[:6]!=b'\x7fELF\x02\x01' or S.unpack_from('<H',raw,18)[0]!=183: raise ValueError('Expected ARM64 ELF64LE')
    table=S.unpack_from('<Q',raw,40)[0]; width,count,names=S.unpack_from('<HHH',raw,58)
    if width!=64 or table+width*count>len(raw): raise ValueError('Invalid section table')
    sections=[S.unpack_from('<IIQQQQIIQQ',raw,table+i*width) for i in range(count)]
    def content(i):
        sec=sections[i]; data=raw[sec[4]:sec[4]+sec[5]]
        if len(data)!=sec[5]: raise ValueError('Section bounds')
        return data
    symbols={}; symbol_tables={}
    for i,sec in enumerate(sections):
        if sec[1]!=2: continue
        strings=content(sec[6]); entries=[]
        for off in range(0,sec[5],24):
            n,info,other,index,value,size=S.unpack_from('<IBBHQQ',content(i),off)
            name=strings[n:].split(b'\0',1)[0].decode(); entry=(name,index,value,size)
            entries.append(entry)
            if name: symbols[name]=entry
        symbol_tables[i]=entries
    fn=symbols['himax_parse_report_points']; assert fn[2:]==(0xf38,2156)
    base={}; end=0x10000000
    for i,sec in enumerate(sections):
        if sec[2]&2 and sec[5]:
            base[i]=end;end+=(sec[5]+4095)&~4095
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM);uc.mem_map(0x10000000,end-0x10000000)
    for i,addr in base.items():
        if sections[i][1]!=8: uc.mem_write(addr,content(i))
    STUB=0x11000000;STOP=STUB+16
    uc.mem_map(STUB,4096);uc.mem_write(STUB,S.pack('<I',0xd65f03c0))
    def address(sym):
        name,index,value,size=sym
        if not index:
            if name!='_printk': raise ValueError('Unexpected external call: '+name)
            return STUB
        return base[index]+value
    relocations=0
    for i,sec in enumerate(sections):
        if sec[1]!=4 or sec[7]!=fn[1]: continue
        entries=symbol_tables[sec[6]]
        for off in range(0,sec[5],24):
            offset,info,addend=S.unpack_from('<QQq',content(i),off)
            if not fn[2]<=offset<fn[2]+fn[3]:continue
            kind=info&0xffffffff;target=address(entries[info>>32])+addend;place=base[fn[1]]+offset
            ins=S.unpack('<I',uc.mem_read(place,4))[0]
            if kind==275:
                delta=(target>>12)-(place>>12)
                if not -(1<<20)<=delta<(1<<20):raise ValueError('ADRP overflow')
                ins=(ins&~((3<<29)|(0x7ffff<<5)))|((delta&3)<<29)|(((delta>>2)&0x7ffff)<<5)
            elif kind in (277,278,284,285,286):
                shift={277:0,278:0,284:1,285:2,286:3}[kind]
                ins=(ins&~(0xfff<<10))|(((target&0xfff)>>shift)<<10)
            elif kind in (282,283):
                delta=target-place
                if delta%4 or not -(1<<27)<=delta<(1<<27):raise ValueError('Branch overflow')
                ins=(ins&~0x3ffffff)|((delta>>2)&0x3ffffff)
            else:raise ValueError('Unsupported projection relocation '+str(kind))
            uc.mem_write(place,S.pack('<I',ins));relocations+=1
    DATA=0x30000000;uc.mem_map(DATA,0x10000);uc.mem_map(0x40000000,0x10000)
    IC,TOUCH,TS,PLATFORM,TARGET,POINTS,PACKET=[DATA+i*0x1000 for i in range(7)]
    def u32(a,n):uc.mem_write(a,S.pack('<I',n))
    def ptr(a,n):uc.mem_write(a,S.pack('<Q',n))
    for name,val in [('hx_s_ic_data',IC),('hx_s_touch_data',TOUCH),('hx_s_ts',TS),('g_target_report_data',TARGET)]:ptr(address(symbols[name]),val)
    uc.mem_write(IC+0x61,b'\0');uc.mem_write(IC+0x63,b'\x0a')
    u32(TOUCH+0x14,56);ptr(TOUCH+0x20,PACKET);ptr(TS+0x190,PLATFORM)
    uc.mem_write(TS+0x34,b'\x0a');uc.mem_write(TS+0x2e,b'\x02')
    u32(PLATFORM+4,1400);u32(PLATFORM+16,2240);ptr(TARGET,POINTS)
    def trap(u,a,size,ctx):
        if a==STOP:u.emu_stop()
        elif a==STUB:u.reg_write(UC_ARM64_REG_X0,0)
        elif not base[fn[1]]+fn[2]<=a<base[fn[1]]+fn[2]+fn[3]:raise RuntimeError('Unexpected instruction outside parser: '+hex(a))
    uc.hook_add(UC_HOOK_CODE,trap)
    randomizer=random.Random(0x83132);passed=0
    with tempfile.TemporaryDirectory(prefix='himax-binary-',dir=REPO/'Build') as temp:
        so=Path(temp)/'core.so'
        subprocess.run(['gcc','-shared','-fPIC','-O2','-I'+str(REPO/'Silicon/MediaTek/MediaTekPkg/Include'),
            str(REPO/'Silicon/MediaTek/MediaTekPkg/Library/HimaxCoreLib/HimaxCore.c'),'-o',str(so)],check=True,capture_output=True)
        core=C.CDLL(str(so));decode=core.HmDecode;decode.argtypes=[C.c_void_p,C.c_uint,C.c_uint,C.c_uint,C.c_uint,C.POINTER(Frame)];decode.restype=C.c_int
        for case in range(1024):
            active=randomizer.sample(range(10),case%11)
            packet=bytearray(b'\xff'*40+b'\0'*16)
            for slot in active:
                x=randomizer.randrange(1401);y=randomizer.randrange(2241);pressure=randomizer.randrange(256)
                packet[4*slot:4*slot+4]=S.pack('>HH',x,y);packet[40+slot]=pressure
            packet[52]=len(active);packet[53]=randomizer.randrange(256);packet[54]=randomizer.randrange(256)
            packet[55]=(-sum(packet))&255
            frame=Frame();buf=C.create_string_buffer(bytes(packet));assert decode(buf,56,10,1400,2240,C.byref(frame))==0
            uc.mem_write(PACKET,bytes(packet));uc.mem_write(POINTS,b'\0'*160);u32(TS+0x70,len(active))
            uc.reg_write(UC_ARM64_REG_X0,TS);uc.reg_write(UC_ARM64_REG_X1,TOUCH);uc.reg_write(UC_ARM64_REG_X2,0)
            uc.reg_write(UC_ARM64_REG_SP,0x4000ff00);uc.reg_write(UC_ARM64_REG_LR,STOP)
            uc.emu_start(base[fn[1]]+fn[2],STOP,timeout=100000,count=10000)
            if uc.reg_read(UC_ARM64_REG_PC)!=STOP:raise RuntimeError('Stock parser failed to return')
            for slot in range(10):
                x,y,pressure,on=S.unpack('<4I',uc.mem_read(POINTS+slot*16,16));c=frame.Contacts[slot]
                assert bool(on)==bool(c.Active),(case,slot,'active',on,c.Active)
                if c.Active:assert (x,y,pressure)==(c.X,c.Y,c.Pressure),(case,slot,'coordinates')
            passed+=1
        startup=compare_startup(core,raw,sections,content,symbol_tables,symbols,base)
        recovery=compare_crc_and_firmware(core,raw,sections,content,symbol_tables,symbols,base)
        flash_update=compare_flash_update(core,raw,sections,content,symbol_tables,symbols,base)
    report={'status':'passed','scope':'Shipped report/startup/parser/AP CRC, complete guarded bit4-clear sense-off sequence, page program and HW CRC compared with production HimaxCore C',
            'binary':str(BINARY),'binary_sha256':SHA,'firmware':str(FIRMWARE),'firmware_sha256':FIRMWARE_SHA,
            'stock_text_offset':hex(fn[2]),'stock_function_bytes':fn[3],
            'elf_relocations_applied':relocations,'valid_normal_frames_compared':passed,
            'startup_comparison':startup,'recovery_firmware_comparison':recovery,
            'firmware_update_comparison':flash_update,
            'contacts_per_frame':'0..10; independent slots, inclusive coordinate limits, pressure',
            'source_sha256':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (
                Path(__file__),REPO/'Silicon/MediaTek/MediaTekPkg/Library/HimaxCoreLib/HimaxCore.c',
                REPO/'Silicon/MediaTek/MediaTekPkg/Include/Library/HimaxCore.h',
                REPO/'Platforms/Teclast/t70proPkg/Tests/HimaxCoreTest.c')},
            'external_calls':'Stock bus, delay, mutex, printk and stack-check edges are trapped; unexpected execution rejected',
            'limitations':['No physical I2C, GPIO, erase, flash unlock, reset/restart, IRQ, sleep/wake, or on-device firmware upload is executed',
                'T70 firmware validation checks format/variant and AP CRC32C; the recorded SHA256 manifest identifies the exact blob',
                'Sense-off is compared through return for stock argument zero and bit4 clear; the bit4-set slave scan and cold firmware recovery orchestration remain unsupported']}
    path=REPO.parent/'analysis/himax-binary-tests.json';path.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
if __name__=='__main__':main()
