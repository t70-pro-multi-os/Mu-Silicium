#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the exact physical-trial protocol boundaries without USB or ADB."""
import argparse
import hashlib
import json
import struct
from types import SimpleNamespace
from unittest.mock import patch
import importlib.util
from pathlib import Path
import sys
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('trial',Path(__file__).parents[1]/'Tools/run-lk-ram-boot.py')
t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--report',type=Path)
    a=p.parse_args()
    if a.report:a.report.write_text(json.dumps({'all_passed':False,'status':'running'})+'\n')
    cases=0
    for stdout,returncode,serial,expected in (
        ('List of devices attached\nA\tdevice\n',0,None,'A'),
        ('List of devices attached\nA\tdevice\nB\tdevice\n',0,'A','A'),
        ('List of devices attached\nA\tdevice\nB\tdevice\n',0,'B','B'),
        ('List of devices attached\nA\tdevice\nB\tdevice\n',0,None,None),
        ('List of devices attached\nA\tunauthorized\nB\tdevice\n',0,'A',None),
        ('List of devices attached\nB\tdevice\n',0,'A',None),
        ('List of devices attached\nA\tdevice\nA\tdevice\n',0,'A',None),
        ('List of devices attached\nA\tdevice\n',1,'A',None)):
        with patch.object(t.subprocess,'run',return_value=SimpleNamespace(stdout=stdout,returncode=returncode)) as call:
            try:observed=t.adb_device(Path('/test/adb'),serial)
            except ValueError:assert expected is None
            else:assert observed==expected and expected is not None
            call.assert_called_once_with(['/test/adb','devices'],capture_output=True,text=True,timeout=15)
        cases+=1
    for command,replies,expected in (
        ('boot',[b'FAIL'+t.FAILURE],(b'FAIL',t.FAILURE)),
        ('boot',[b'INFOworking',b'FAILunknown command'],(b'FAIL',b'unknown command')),
        ('reboot',[b'OKAY'],(b'OKAY',b'')),
        ('getvar:unlocked',[b'OKAYyes'],(b'OKAY',b'yes'))):
        writes=[];iterator=iter(replies)
        def write(data):writes.append(data);return len(data)
        assert t.fastboot_command(write,lambda n:next(iterator),command)==expected
        assert writes==[command.encode()];cases+=1
    for command,reply in (('flash:boot',b'OKAY'),('flashing unlock',b'OKAY'),('boot',b''),
                           ('boot',b'DATA00000000'),('boot',b'OKAY'+b'x'*61)):
        writes=[]
        try:t.fastboot_command(lambda data:writes.append(data) or len(data),lambda n:reply,command)
        except ValueError:pass
        else:raise AssertionError('Invalid protocol was accepted')
        if command not in t.FASTBOOT_COMMANDS:assert writes==[]
        cases+=1
    snapshot={'boot_completed':True,'user_running_unlocked':True,'user_download_directory_accessible':True,
              'slot':'_a','flash_locked':'0','vbmeta_state':'unlocked','SIM_states':['LOADED','ABSENT'],
              'USB_charging':True,'WiFi_enabled':True,'WiFi_connected':True}
    t.preflight_ok({'stock_return_verified':True},snapshot);cases+=1
    # Both SIM slots may be populated; reject incomplete or malformed states.
    for states,allowed in (
        (['LOADED','LOADED'],True),(['ABSENT','LOADED'],True),
        (['ABSENT','ABSENT'],False),(['LOADED','NOT_READY'],False),
        (['NOT_READY','LOADED'],False),(['READY','LOADED'],False),
        (['LOADED','PIN_REQUIRED'],False),(['LOADED',''],False),
        (['LOADED'],False),(['LOADED','LOADED','ABSENT'],False),
        ([],False),('LOADED,LOADED',False),(None,False)):
        current=dict(snapshot,SIM_states=states)
        try:t.preflight_ok({'stock_return_verified':True},current)
        except ValueError:assert not allowed
        else:assert allowed
        assert t.stock_ADB_services_ready(current)==allowed
        try:t.ram_probe_preflight_ok({'stock_return_verified':True},current)
        except ValueError:assert not allowed
        else:assert allowed
        cases+=1
    for battery,usb,ac in (
        ('USB powered: true\nAC powered: false',True,False),
        ('USB powered: false\nAC powered: true',False,True),
        (' USB powered: true\r\n AC powered: true\r\n',True,True),
        ('USB powered: false\nAC powered: false\nWireless powered: true',False,False),
        ('USB powered: truefalse\nAC powered: true false',False,False),
        ('prefix USB powered: true\nunknown AC powered: true',False,False),
        ('USB powered:\ntrue\nAC powered: false',False,False)):
        assert t.battery_power_snapshot(battery)=={'USB_charging':usb,'AC_powered':ac};cases+=1
    for usb,ac,allowed in ((True,False,True),(False,True,True),(True,True,True),(False,False,False)):
        current=dict(snapshot,USB_charging=usb,AC_powered=ac)
        try:t.preflight_ok({'stock_return_verified':True},current)
        except ValueError:assert not allowed
        else:assert allowed
        assert t.stock_ADB_services_ready(current)==allowed;cases+=1
    for image,capture,detach,allowed in (
        (False,False,False,True),(True,False,False,True),(True,True,False,True),
        (True,False,True,True),(False,True,False,False),(False,False,True,False),
        (True,True,True,False),(False,True,True,False)):
        try:t.validate_trial_mode(image,capture,detach)
        except ValueError:assert not allowed
        else:assert allowed
        cases+=1
    for image,capture,detach,leave,allowed in (
        (True,False,True,True,True),(True,False,False,True,True),
        (False,False,False,True,False),(True,True,False,True,False),
        (True,True,True,True,False)):
        try:t.validate_trial_mode(image,capture,detach,leave)
        except ValueError:assert not allowed
        else:assert allowed
        cases+=1
    for field,value in (('user_running_unlocked',False),('user_download_directory_accessible',False),
                         ('slot','_b'),('flash_locked','1'),('SIM_states',['NOT_READY','ABSENT'])):
        changed=dict(snapshot);changed[field]=value
        try:t.preflight_ok({'stock_return_verified':True},changed)
        except ValueError:pass
        else:raise AssertionError('Preflight failure accepted')
        cases+=1
    try:t.preflight_ok({'stock_return_verified':False},snapshot)
    except ValueError:pass
    else:raise AssertionError('Unfinished previous return accepted')
    cases+=1
    base_cases=cases
    image=bytes(range(256))*160
    for partial in (False,True):
        sent=[];calls=[0];replies=iter([b'INFOready',b'DATA0000a000',b'INFOchecking',b'OKAY'])
        def write(data):
            calls[0]+=1
            if calls[0]==1:
                assert data==b'download:0000a000';return len(data)
            count=min(31,len(data)) if partial and calls[0]==2 else len(data)
            sent.append(data[:count]);return count
        assert t.fastboot_download(write,lambda n:next(replies),image)==len(image)
        assert b''.join(sent)==image;cases+=1
    download_cases=[
        ('short_image',[b'OKAY'],b'x',True),
        ('oversized_image',[b'OKAY'],bytes(0x200001),True),
        ('rejected_request',[b'FAILnot allowed'],image,True),
        ('wrong_DATA_size',[b'DATA00000001'],image,False),
        ('missing_DATA',[b'OKAY'],image,False),
        ('zero_write',[b'DATA0000a000'],image,False),
        ('over_reported_write',[b'DATA0000a000'],image,False),
        ('bool_write',[b'DATA0000a000'],image,False),
        ('read_transport_error',[],image,False),
        ('write_transport_error',[b'DATA0000a000'],image,False),
        ('request_partial_write',[],image,False),
        ('terminal_FAIL',[b'DATA0000a000',b'FAILbad image'],image,True),
        ('terminal_unknown',[b'DATA0000a000',b'DATA00000000'],image,False),
        ('INFO_flood',[b'INFOwait']*64,image,False),
        ('deadline',[],image,False)]
    for name,raw,data,ready in download_cases:
        replies=iter(raw);calls=[0];clock=iter([0,16]) if name=='deadline' else None
        def write(chunk):
            calls[0]+=1
            if name=='request_partial_write':return len(chunk)-1
            if calls[0]>1:
                if name=='zero_write':return 0
                if name=='over_reported_write':return len(chunk)+1
                if name=='bool_write':return True
                if name=='write_transport_error':raise TimeoutError()
            return len(chunk)
        try:t.fastboot_download(write,lambda n:next(replies),data,**({'clock':lambda:next(clock)} if clock else {}))
        except t.DownloadError as e:assert e.command_ready==ready,(name,e.command_ready)
        else:raise AssertionError('Bad RAM download accepted: '+name)
        if name in ('short_image','oversized_image','deadline'):assert calls[0]==0
        if name in ('rejected_request','wrong_DATA_size','missing_DATA','read_transport_error','INFO_flood'):assert calls[0]==1
        cases+=1
    prior={'stock_return_verified':False,'stock_Android_return_observed':True,
           'after':{'boot_completed':True},'device_storage_write_commands':[],
           'ARM32_stage_executed':True,'RAM_patched_LK_executed':True,'stage_sha256':'exact-stage'}
    t.prior_lk_trial_ok(prior,'exact-stage');cases+=1
    for field,value in (('stock_Android_return_observed',False),('after',{'boot_completed':False}),
                        ('device_storage_write_commands',['flash']),('ARM32_stage_executed',False),
                        ('RAM_patched_LK_executed',False),('stage_sha256','older-stage')):
        changed=dict(prior);changed[field]=value
        try:t.prior_lk_trial_ok(changed,'exact-stage')
        except ValueError:pass
        else:raise AssertionError('Incomplete physical LK gate accepted')
        cases+=1
    previous_cases=cases
    locked=dict(snapshot,user_running_unlocked=False,user_download_directory_accessible=False)
    ram_previous={'stock_return_verified':False,'stock_Android_return_observed':True,
                  'after':{'boot_completed':True},'device_storage_write_commands':[],
                  'trial_candidate_sha256':None,'EDK2_boot_command_acknowledged':False,'UEFI_executed':False}
    t.ram_probe_preflight_ok(ram_previous,locked)
    assert locked['user_running_unlocked'] is False and locked['user_download_directory_accessible'] is False
    cases+=1
    image_prior=dict(ram_previous,ARM32_stage_executed=True,RAM_patched_LK_executed=True,stage_sha256='exact-stage')
    t.prior_lk_trial_ok(image_prior,'exact-stage');t.ram_probe_preflight_ok(image_prior,locked)
    assert image_prior['stock_return_verified'] is False and locked['user_running_unlocked'] is False
    cases+=1
    for key,value in (('stock_Android_return_observed',False),('device_storage_write_commands',['flash']),
                      ('trial_candidate_sha256','image'),('EDK2_boot_command_acknowledged',True),('UEFI_executed',True)):
        changed=dict(ram_previous);changed[key]=value
        try:t.ram_probe_preflight_ok(changed,locked)
        except ValueError:pass
        else:raise AssertionError('Unsafe intermediate probe accepted')
        cases+=1
    for key,value in (('boot_completed',False),('slot','_b'),('flash_locked','1'),
                      ('SIM_states',['NOT_READY','ABSENT']),('USB_charging',False),('WiFi_connected',False)):
        changed=dict(locked);changed[key]=value
        try:t.ram_probe_preflight_ok(ram_previous,changed)
        except ValueError:pass
        else:raise AssertionError('Unrestored Android accepted for intermediate probe')
        cases+=1
    try:t.preflight_ok(ram_previous,locked)
    except ValueError:pass
    else:raise AssertionError('Final user-file/app return was incorrectly marked verified')
    cases+=1
    for size,chunks in ((40,[40]),(64,[64]),(64,[32,32]),(80,[80]),(80,[64,16]),(80,[1]*80),(224,[224]),(224,[64,64,64,32]),(224,[1]*224),(296,[296]),(296,[64]*4+[40]),(296,[1]*296)):
        payload=bytes(i&255 for i in range(size));sizes=iter(chunks);received=[0]
        def read_stage(requested):
            assert requested==size-received[0]
            count=next(sizes);data=payload[received[0]:received[0]+count]
            received[0]+=count;return data
        assert t.read_stage_record(read_stage,size)==payload;cases+=1
    assert t.read_stage_record(lambda n:b'',80)==b'';cases+=1
    for size,read in ((81,lambda n:b''),(80,lambda n:b'x'*(n+1))):
        try:t.read_stage_record(read,size)
        except ValueError:pass
        else:raise AssertionError('Out-of-bound record read was accepted')
        cases+=1
    for result,allowed in ((True,True),(8,True),(False,False),(None,False),(1,False),(7,False),(9,False)):
        calls=[]
        try:t.resume_ram_bridge(lambda data:calls.append(data) or result)
        except ValueError:assert not allowed
        else:assert allowed
        assert calls==[b'T70GO32!'];cases+=1
    assert not t.bootloader_trace((b'OKAY',t.VERSION.encode()))['observed'];cases+=1
    for n in range(1,6):
        assert t.bootloader_trace((b'OKAY',t.VERSION[:-4].encode()+('RB0'+str(n)).encode()))['observed'];cases+=1
    for reply in ((b'FAIL',t.VERSION.encode()),(b'OKAY',b''),(b'OKAY',t.VERSION[:-4].encode()+b'RB00'),
                  (b'OKAY',t.VERSION[:-4].encode()+b'RB06'),(b'OKAY',t.VERSION[:-4].encode()+b'RB0x'),
                  (b'OKAY',b'bad!'+t.VERSION[4:].encode()),(b'OKAY',t.VERSION.encode()+b'x')):
        try:t.bootloader_trace(reply)
        except ValueError:pass
        else:raise AssertionError('Invalid BL2 trace version accepted')
        cases+=1
    for result,allowed in ((True,True),(8,True),(False,False),(None,False),(7,False),(9,False)):
        calls=[]
        try:t.finish_ram_bridge(lambda data:calls.append(data) or result)
        except ValueError:assert not allowed
        else:assert allowed
        assert calls==[b'T70RUN!!'];cases+=1
    mutation=[b'T70ARM32',1,7,0,0x94000349,1,1,8,1]
    mutation_pack=lambda r:struct.pack('<8s8I',*r)
    assert t.mutation_stage_record(mutation_pack(mutation))['BL2_RAM_patch_readback_matched'];cases+=1
    for state in range(9):
        r=list(mutation);r[2]=state;r[5]=int(state==7)
        assert t.mutation_stage_record(mutation_pack(r))['BL2_RAM_patch_readback_matched']==(state==7)
        cases+=1
    r=list(mutation);r[2]=5;r[3]=0x500;r[5]=0
    assert t.mutation_stage_record(mutation_pack(r))['first_occupied_cave_offset']=='0x500';cases+=1
    bad_mutations=[b'',mutation_pack(mutation)[:-1],mutation_pack(mutation)+b'x']
    for index,value in ((0,b'BADMAGIC'),(1,2),(2,9),(3,0x500),(4,0),(5,0),(6,0),(8,0)):
        r=list(mutation);r[index]=value;bad_mutations.append(mutation_pack(r))
    for raw in bad_mutations:
        try:t.mutation_stage_record(raw)
        except ValueError:pass
        else:raise AssertionError('Invalid RAM mutation result accepted')
        cases+=1
    for phase,audit_mode,allowed in (('preloader',False,True),('preloader',True,True),
                                   ('jump_requested',False,False),('jump_requested',True,True),
                                   ('guard_audit_return',True,False),('normal_boot_chain',False,False)):
        assert t.preloader_return_allowed(phase,audit_mode)==allowed;cases+=1
    marker=[b'T70STG32',3,t.ENTRY,0x800001d3,0xc50838,0x81000000,
            0x401ffff4,12,0x2091cb,1,0,0,0,0,0]
    boot_pack=lambda data:struct.pack('<8s14I',*data)
    assert t.boot_stage_record(boot_pack(marker))['state']=='descriptor_absent';cases+=1
    for state in (2,3,4):
        r=list(marker);r[9:]=[state,0x62f00000,0,0x100000,int(state==4),0x4207cd00]
        assert t.boot_stage_record(boot_pack(r))['shipping_BL2_prefix_already_present']==(state==4)
        cases+=1
    bad_markers=[b'',boot_pack(marker)[:-1],boot_pack(marker)+b'x']
    for index,value in ((0,b'BADMAGIC'),(1,2),(2,t.ENTRY+4),(6,0),(7,16),(8,0),
                        (9,5),(10,1),(13,1),(14,1)):
        r=list(marker);r[index]=value;bad_markers.append(boot_pack(r))
    for raw in bad_markers:
        try:t.boot_stage_record(raw)
        except ValueError:pass
        else:raise AssertionError('Invalid allocation marker accepted')
        cases+=1
    audit=[b'T70RAM32',3,t.ENTRY,0x800001d3,0x00c50838,0x81000000,
           0x401ffff4,12,0x2091cb,0,0,0,0,0x2fde30,0x224e5d,0x1ff,51,0,0]
    pack=lambda r:struct.pack('<8s18I',*r)
    assert t.guard_audit_record(pack(audit))['all_bridge_entry_guards_passed'];cases+=1
    for failure,changes in (
        ('cache_or_MMU',{4:audit[4]|1,9:1,12:1}),
        ('CPU_mode',{3:0x1f,9:2,11:0x13,12:0x1f}),
        ('shipping_code_window',{9:3,10:0x224f2c,11:1,12:2,15:0x1bf}),
        ('saved_caller_LR',{9:4,10:audit[13]+0x174,11:0x224e5d,12:0x224e61,14:0x224e61})):
        r=list(audit)
        for index,value in changes.items():r[index]=value
        parsed=t.guard_audit_record(pack(r))
        assert not parsed['all_bridge_entry_guards_passed'] and parsed['first_failed_guard']==failure
        cases+=1
    bad_records=[b'',pack(audit)[:-1],pack(audit)+b'x']
    for index,value in ((0,b'BADMAGIC'),(1,2),(2,t.ENTRY+4),(6,0),(7,16),(8,0),
                         (9,5),(10,4),(13,0),(13,0x2fde31),(14,0x224e61),
                         (15,0x200),(15,0),(16,50),(17,1),(18,1)):
        r=list(audit);r[index]=value;bad_records.append(pack(r))
    for raw in bad_records:
        try:t.guard_audit_record(raw)
        except ValueError:pass
        else:raise AssertionError('Invalid guard record was accepted')
        cases+=1
    for case in ('success','shutdown_not_acknowledged','security_rejected','upload_failed',
                 'patch_changed','vendor_SystemExit','vendor_transport_error','vendor_no_return_status'):
        calls=[];loader=SimpleNamespace(patch=True)
        def security():
            calls.append('security')
            if case=='security_rejected':raise ValueError('security mismatch')
        def upload_da(*,preloader):
            calls.append('upload')
            assert preloader is None and loader.patch is False
            if case=='vendor_SystemExit':raise SystemExit(1)
            if case=='vendor_transport_error':raise OSError()
            if case=='patch_changed':loader.patch=True
            return case!='upload_failed'
        def shutdown(*,bootmode):
            calls.append('shutdown');assert bootmode==1
            if case=='vendor_no_return_status':return None
            return case!='shutdown_not_acknowledged'
        loader.upload_da=upload_da;loader.shutdown=shutdown
        acknowledged,error=t.attempt_stock_da_return(SimpleNamespace(daloader=loader),security)
        assert (acknowledged is None) if case=='vendor_no_return_status' else acknowledged==(case=='success')
        assert (error is None)==(case in ('success','shutdown_not_acknowledged','vendor_no_return_status'))
        assert calls==(['security'] if case=='security_rejected' else
                       ['security','upload','shutdown'] if case in ('success','shutdown_not_acknowledged','vendor_no_return_status') else
                       ['security','upload'])
        if case=='vendor_SystemExit':assert error=='SystemExit'
        cases+=1
    assert t.stock_ADB_services_ready(locked);cases+=1
    for key,value in (('SIM_states',['READY','ABSENT']),('WiFi_connected',False),('boot_completed',False)):
        assert not t.stock_ADB_services_ready(dict(locked,**{key:value}));cases+=1
    returned_image=dict(ram_previous,trial_candidate_sha256='a'*64,
                        EDK2_boot_command_acknowledged=True,ARM32_stage_executed=True,
                        RAM_patched_LK_executed=True,after=dict(locked))
    t.ram_probe_preflight_ok(returned_image,locked,allow_returned_image=True);cases+=1
    # Actual legacy reports recorded USB=false for a charger classified as AC.
    # Recheck wired power live; do not mutate or invent historical observations.
    legacy=dict(returned_image,after=dict(locked,USB_charging=False))
    ac_live=dict(locked,USB_charging=False,AC_powered=True)
    t.ram_probe_preflight_ok(legacy,ac_live,allow_returned_image=True);cases+=1
    assert 'AC_powered' not in legacy['after'] and legacy['after']['USB_charging'] is False
    for field,value in (('slot','_b'),('SIM_states',['NOT_READY','ABSENT']),('WiFi_connected',False)):
        changed=dict(legacy,after=dict(legacy['after'],**{field:value}))
        try:t.ram_probe_preflight_ok(changed,ac_live,allow_returned_image=True)
        except ValueError:pass
        else:raise AssertionError('Invalid historical Android state accepted')
        cases+=1
    for current in (dict(ac_live,AC_powered=False),dict(ac_live,AC_powered=False,Wireless_powered=True)):
        try:t.ram_probe_preflight_ok(legacy,current,allow_returned_image=True)
        except ValueError:pass
        else:raise AssertionError('Missing live wired power accepted')
        cases+=1
    for key,value in (('trial_candidate_sha256','not-a-hash'),('EDK2_boot_command_acknowledged',False),
                      ('ARM32_stage_executed',False),('RAM_patched_LK_executed',False),
                      ('device_storage_write_commands',['flash']),('after',dict(locked,slot='_b'))):
        changed=dict(returned_image);changed[key]=value
        try:t.ram_probe_preflight_ok(changed,locked,allow_returned_image=True)
        except ValueError:pass
        else:raise AssertionError('Unobserved image return accepted for diagnostic')
        cases+=1
    record=bytearray(224)
    struct.pack_into('<8s8I',record,0,b'T70DIA32',2,t.ENTRY,0x800001d3,0xc50838,0x81000000,
                     0x401ffff4,12,0x2091cb)
    assert not t.retained_boot_record(bytes(record))['trial_cookie_observed'];cases+=1
    struct.pack_into('<II',record,40,1,1)
    struct.pack_into('<4Q',record,48,0x0000000157443754,0xfffffffea8bbc8ab,0xf00822000005,2)
    struct.pack_into('<8Q',record,80,0x0000000148303754,0x47c80000,4096,0x40000000,4,0x81000000,0x30d00800,3)
    struct.pack_into('<4I',record,208,1,1,0,0)
    parsed=t.retained_boot_record(bytes(record))
    assert parsed['SEC_C_entry_marker'] and parsed['FDT_address']=='0x47c80000'
    assert parsed['recognized_old_cookie_cleared'] and parsed['baseline_no_valid_trial_cookie_after_probe']
    cases+=1
    only_sec=bytearray(record)
    struct.pack_into('<I',only_sec,44,0);only_sec[80:176]=bytes(96)
    struct.pack_into('<I',only_sec,212,0)
    assert t.retained_boot_record(bytes(only_sec))['SEC_C_entry_marker'];cases+=1
    dxe=bytearray(record)
    struct.pack_into('<Q',dxe,136,4)
    struct.pack_into('<4Q',dxe,144,0x42000000,0x43000000,4096,0)
    assert t.retained_boot_record(bytes(dxe))['DXE_entry_marker'];cases+=1
    no_cookie=bytearray(dxe);struct.pack_into('<I',no_cookie,40,0)
    no_cookie[48:80]=bytes(32);struct.pack_into('<I',no_cookie,208,0)
    assert t.retained_boot_record(bytes(no_cookie))['DXE_entry_marker'];cases+=1
    with_gop=bytearray(dxe)
    struct.pack_into('<QIIQQ',with_gop,176,0x0000000147503754,8,2,0,(1920<<32)|1200)
    struct.pack_into('<I',with_gop,216,1)
    result=t.retained_boot_record(bytes(with_gop))
    assert result['GOP_installed_marker'] and result['GOP_width']==1920 and result['GOP_height']==1200;cases+=1
    assert result['GOP_activity_counter_format'] is None
    assert result['GOP_successful_SetMode_calls'] is None and result['GOP_successful_buffer_to_video_calls'] is None;cases+=1
    for modes,blts in ((0,0),(1,1),(4,123),(65535,32767)):
        activity=bytearray(with_gop)
        detail=(1<<63)|(blts<<48)|(2240<<32)|(modes<<16)|1400
        struct.pack_into('<Q',activity,200,detail)
        result=t.retained_boot_record(bytes(activity))
        assert result['GOP_installed_marker'] and result['GOP_width']==2240 and result['GOP_height']==1400
        assert result['GOP_activity_counter_format']==1 and result['GOP_successful_SetMode_calls']==modes
        assert result['GOP_successful_buffer_to_video_calls']==blts
        assert result['GOP_activity_counts_saturated']==(modes==65535 or blts==32767);cases+=1
    for phase,status,width,height in ((7,0,2240,1400),(8,1,2240,1400),
                                       (8,0,0,1400),(8,0,2240,0),(8,0,4097,1400)):
        bad=bytearray(with_gop)
        struct.pack_into('<I',bad,184,phase);struct.pack_into('<Q',bad,192,status)
        struct.pack_into('<Q',bad,200,(1<<63)|(width<<32)|height)
        try:t.retained_boot_record(bytes(bad))
        except ValueError:cases+=1
        else:raise AssertionError('invalid GOP activity metadata accepted')
    runtime_gop=bytearray(with_gop)
    struct.pack_into('<I',runtime_gop,184,2)
    struct.pack_into('<Q',runtime_gop,200,0x1000616C00000000)
    result=t.retained_boot_record(bytes(runtime_gop))
    assert not result['GOP_installed_marker'] and result['GOP_width'] is None
    assert result['GOP_last_MMIO_address']=='0x1000616c' and result['GOP_last_MMIO_value']=='0x0';cases+=1
    struct.pack_into('<Q',runtime_gop,200,5<<32)
    result=t.retained_boot_record(bytes(runtime_gop))
    assert result['GOP_runtime_step']=='LK_handoff_before_MMIO' and result['GOP_last_MMIO_address'] is None;cases+=1
    early_map=bytearray(runtime_gop)
    struct.pack_into('<Q',early_map,200,0x4D048E0080000006)
    result=t.retained_boot_record(bytes(early_map))
    assert result['GOP_runtime_step']=='early_memory_map'
    assert result['GOP_early_memory_map']=={'phase':4,'phase_name':'modem_metadata',
           'status':'0x8000000000000006','display_adoption_status':'0x800000000000000e',
           'additional_descriptors':0}
    assert result['GOP_last_MMIO_address'] is None;cases+=1
    struct.pack_into('<Q',early_map,200,0x4D0B000C00000000)
    result=t.retained_boot_record(bytes(early_map))
    assert result['GOP_early_memory_map']['phase_name']=='map_complete'
    assert result['GOP_early_memory_map']['display_adoption_status']=='0x0'
    assert result['GOP_early_memory_map']['additional_descriptors']==12;cases+=1
    for value in (0x4DFF000000000000,0x4D00008000000000):
        struct.pack_into('<Q',early_map,200,value)
        try:t.retained_boot_record(bytes(early_map))
        except ValueError:pass
        else:raise AssertionError('Invalid early memory map metadata accepted')
        cases+=1
    v3=bytearray(with_gop)+bytes(72)
    struct.pack_into('<I',v3,8,3);struct.pack_into('<I',v3,220,1)
    snapshot=[0x000000014c443754,0x14005000,1]+list(range(14));snapshot[6]=2
    struct.pack_into('<Q16I',v3,224,*snapshot)
    result=t.retained_boot_record(bytes(v3))
    assert result['recognized_old_display_snapshot_cleared']
    assert result['LK_display_snapshot']['layer']==1
    assert result['LK_display_snapshot']['registers']['control']=='0x6';cases+=1
    overlap_v3=bytearray(v3)
    struct.pack_into('<QII5Q4I',overlap_v3,224,0x000000034c443754,0x43f7f080,0,
                     0xfe400000,0x100000000,0xfe400000,0x100000000,(0xfffffffd<<32)|2,0,0,0,0)
    result=t.retained_boot_record(bytes(overlap_v3))
    assert result['LK_display_snapshot']=={'kind':'MEMORY_OVERLAP','video_start':'0xfe400000',
           'video_end':'0x100000000','private_start':'0xfe400000','private_end':'0x100000000',
           'owner':-3,'reservation_index':2};cases+=1
    for offset in (224,228,232,236,280,284,288,292):
        bad=bytearray(overlap_v3);bad[offset]^=4
        try:t.retained_boot_record(bytes(bad))
        except ValueError:pass
        else:raise AssertionError('Invalid memory overlap snapshot accepted')
        cases+=1
    bad=bytearray(overlap_v3);struct.pack_into('<Q',bad,248,0xfe400000)
    try:t.retained_boot_record(bytes(bad))
    except ValueError:pass
    else:raise AssertionError('Empty memory overlap range accepted')
    cases+=1
    sec_v3=bytearray(v3)
    sec_v3[176:208]=bytes(32);struct.pack_into('<I',sec_v3,216,0)
    struct.pack_into('<Q',sec_v3,136,3);sec_v3[144:176]=bytes(32)
    sec_snapshot=[0x000000044c443754,0x43f7f060,0,8,0,6,0x80000000,14,0x80000000,
                  0x14005044,0x10002300,3]+[0]*5
    struct.pack_into('<Q16I',sec_v3,224,*sec_snapshot)
    result=t.retained_boot_record(bytes(sec_v3))
    assert not result['DXE_entry_marker'] and not result['GOP_record_observed']
    assert result['SEC_memory_map_snapshot']=={
        'kind':'SEC_MEMORY_MAP','phase':8,'phase_name':'framebuffer_acquisition',
        'additional_descriptors':0,'status':'0x8000000000000006',
        'display_adoption_status':'0x800000000000000e','last_MMIO_address':'0x14005044',
        'last_MMIO_value':'0x10002300','last_MMIO_operation':'write_requested'};cases+=1
    for offset,value in ((232,0),(236,1),(240,12),(244,128),(264,0x14005045),
                         (264,0x90000000),(272,5),(276,1),(292,1),(136,2)):
        bad=bytearray(sec_v3);struct.pack_into('<I',bad,offset,value)
        try:t.retained_boot_record(bytes(bad))
        except ValueError:pass
        else:raise AssertionError('Invalid bounded SEC memory progress accepted')
        cases+=1
    fault_v3=bytearray(sec_v3)
    fault_snapshot=[0x000000054c443754,0x43f7f060,0,4,0,0x96000021,0,0x44001234,0,
                    0x47c81234,0,0x30d00800,0,0x3c5,0,0,0]
    struct.pack_into('<Q16I',fault_v3,224,*fault_snapshot)
    result=t.retained_boot_record(bytes(fault_v3))
    assert result['SEC_exception_snapshot']=={
        'kind':'SEC_EXCEPTION','CurrentEL':4,'exception_type':0,'ESR':'0x96000021',
        'exception_class':'0x25','PC':'0x44001234','FAR':'0x47c81234',
        'SCTLR':'0x30d00800','SPSR':'0x3c5'}
    assert result['SEC_memory_map_snapshot'] is None and not result['GOP_record_observed'];cases+=1
    for offset,value in ((232,0),(236,1),(240,12),(244,4),(288,1),(292,1),(136,2)):
        bad=bytearray(fault_v3);struct.pack_into('<I',bad,offset,value)
        try:t.retained_boot_record(bytes(bad))
        except ValueError:pass
        else:raise AssertionError('Invalid bounded SEC exception accepted')
        cases+=1
    bds=bytearray(v3)
    struct.pack_into('<Q16I',bds,224,0x000000064c443754,0x43f7f060,0,
                     7,3,1,1,1,0,2,1,1,1,1,2,0,0)
    result=t.retained_boot_record(bytes(bds))
    b=result['BDS_progress_snapshot']
    assert b['stage_name']=='recovery_shell_entered' and b['registered_input_paths']==1
    assert b['GOP_handles']==1 and b['USB_IO_handles']==2 and b['recovery_shell_active']
    assert b['system_console_output_present'] and not b['physical_input_action_verified'];cases+=1
    for stage,name in ((9,'setup_application_entered'),(10,'setup_SendForm_entered'),(11,'setup_SendForm_returned')):
        setup=bytearray(bds)
        struct.pack_into('<I',setup,240,stage);struct.pack_into('<I',setup,284,4)
        b=t.retained_boot_record(bytes(setup))['BDS_progress_snapshot']
        assert b['stage_name']==name and b['setup_application_active'] and not b['recovery_shell_active'];cases+=1
    for offset,value in ((232,0),(236,1),(240,0),(240,12),(244,65536),(272,65536),
                         (276,2),(280,2),(284,8),(288,1),(292,1),(184,7)):
        bad=bytearray(bds);struct.pack_into('<I',bad,offset,value)
        try:t.retained_boot_record(bytes(bad))
        except ValueError:cases+=1
        else:raise AssertionError('Invalid bounded BDS progress accepted at '+str(offset))
    for phase in range(9):
        launch=bytearray(bds)
        struct.pack_into('<Q',launch,224,0x000000074c443754)
        struct.pack_into('<II',launch,288,phase,0x1001a)
        b=t.retained_boot_record(bytes(launch))['BDS_progress_snapshot']
        assert b['setup_launch_phase']==phase and b['setup_launch_EFI_error']
        assert b['setup_launch_status_code']==26;cases+=1
    for offset,value in ((288,9),(292,0x20000),(228,8)):
        bad=bytearray(launch);struct.pack_into('<I',bad,offset,value)
        try:t.retained_boot_record(bytes(bad))
        except ValueError:cases+=1
        else:raise AssertionError('Invalid bounded setup launch metadata accepted')
    dsi_v3=bytearray(v3)
    dsi_snapshot=[0x000000024c443754,0x14013000,0]+list(range(12))+[0,0]
    struct.pack_into('<Q16I',dsi_v3,224,*dsi_snapshot)
    result=t.retained_boot_record(bytes(dsi_v3))
    assert result['LK_display_snapshot']['kind']=='DSI'
    assert result['LK_display_snapshot']['registers']['PSCTRL']=='0x2'
    assert len(result['LK_display_snapshot']['registers'])==12;cases+=1
    for offset in (224,228,232,236,288,292):
        bad=bytearray(dsi_v3);bad[offset]^=4
        try:t.retained_boot_record(bytes(bad))
        except ValueError:pass
        else:raise AssertionError('Invalid bounded DSI snapshot accepted')
        cases+=1
    for offset in (224,228,232,236,252):
        bad=bytearray(v3);bad[offset]^=4
        try:t.retained_boot_record(bytes(bad))
        except ValueError:pass
        else:raise AssertionError('Invalid bounded display snapshot accepted')
        cases+=1
    bad=bytearray(v3);struct.pack_into('<I',bad,220,0)
    try:t.retained_boot_record(bytes(bad))
    except ValueError:pass
    else:raise AssertionError('Unrecognized display RAM exported')
    cases+=1
    for offset in (176,180,184,188):
        bad=bytearray(with_gop)
        if offset in (184,188):struct.pack_into('<I',bad,offset,0)
        else:bad[offset]^=1
        try:t.retained_boot_record(bytes(bad))
        except ValueError:pass
        else:raise AssertionError('Invalid GOP record accepted')
        cases+=1
    for offset in (0,8,12,16,20,28,32,36,40,44,48,52,56,60,72,76,80,84,104,108,136,140,176,208,212,216,220,223):
        bad=bytearray(record)
        if offset in (72,136):struct.pack_into('<I',bad,offset,0)
        else:bad[offset]^=1
        try:t.retained_boot_record(bytes(bad))
        except ValueError:pass
        else:raise AssertionError('Damaged or unbounded retained record accepted at '+str(offset))
        cases+=1
    for bad in (bytes(record[:-1]),bytes(record)+b'x'):
        try:t.retained_boot_record(bad)
        except ValueError:pass
        else:raise AssertionError('Wrong retained record size accepted')
        cases+=1
    doc={'all_passed':True,'protocol_and_preflight_cases':cases,'original_boundary_cases':base_cases,
         'new_download_and_physical_gate_cases':previous_cases-base_cases,
         'guard_audit_and_stock_return_cases':cases-previous_cases,'device_access':False,
         'runner_source_sha256':hashlib.sha256(Path(t.__file__).read_bytes()).hexdigest(),
         'test_source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
         'USB_transport':'read/write callbacks are fixtures; no real device',
         'download_failure_policy':'reboot allowed only before DATA rejection or after terminal reply; unknown data-parser state is never sent a command'}
    if a.report:a.report.write_text(json.dumps(doc,indent=2)+'\n')
    print(json.dumps(doc))


if __name__=='__main__':main()
