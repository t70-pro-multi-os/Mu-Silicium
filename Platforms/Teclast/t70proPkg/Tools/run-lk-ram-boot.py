#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Observe RAM-patched LK; optionally request a hash-verified, bounded EDK2 trial."""
import argparse
import contextlib
from datetime import datetime,timezone
import hashlib
import json
import logging
import os
from pathlib import Path
import re
import runpy
import signal
import struct
import subprocess
import sys
import tempfile
import time

ENTRY=0x40200000
DA_SHA='79619e059128754ef49be6bc7f7ec5332f1d0743a814ef2b89d505c6abde33e0'
VERSION='tb8781p1_64_td89_11ee062dcfa_202607030435'
FAILURE=b'RAM boot: unlocked, valid EDK2 image required'
RESUME_TOKEN=b'T70GO32!'
FINAL_RECEIPT=b'T70RUN!!'
FASTBOOT_COMMANDS={'getvar:product','getvar:version-bootloader','getvar:unlocked',
                   'getvar:current-slot','getvar:is-userspace','boot','reboot'}


def require(ok,reason):
    if not ok:raise ValueError(reason)


def fastboot_command(write,read,command):
    require(command in FASTBOOT_COMMANDS,'command_outside_RAM_trial_allowlist')
    data=command.encode('ascii')
    require(write(data)==len(data),'fastboot_write_failed')
    for _ in range(64):
        reply=bytes(read(64))
        require(4<=len(reply)<=64,'invalid_fastboot_reply')
        token=reply[:4]
        if token==b'INFO':continue
        require(token in (b'OKAY',b'FAIL'),'unexpected_fastboot_token')
        return token,reply[4:]
    raise ValueError('too_many_fastboot_information_replies')


class DownloadError(ValueError):
    def __init__(self,reason,command_ready):
        super().__init__(reason)
        self.command_ready=command_ready


def fastboot_download(write,read,payload,*,clock=time.monotonic):
    """Send only a bounded RAM download, preserving partial-write accounting.

    After DATA, an error can leave the target consuming file bytes. Never send a
    reboot command into that stream without an observed terminal OKAY or FAIL.
    """
    if not 4096<=len(payload)<=0x200000:
        raise DownloadError('RAM_download_length_out_of_bounds',True)
    command_ready=False
    deadline=clock()+15
    def bounded():
        if clock()>=deadline:raise DownloadError('RAM_download_15_second_deadline',False)
    def reply():
        for _ in range(64):
            bounded()
            data=bytes(read(64))
            if not 4<=len(data)<=64:raise DownloadError('invalid_download_reply',False)
            if data[:4]!=b'INFO':return data
        raise DownloadError('too_many_download_information_replies',False)
    try:
        request=('download:%08x'%len(payload)).encode('ascii')
        bounded()
        if write(request)!=len(request):raise DownloadError('download_request_write_failed',False)
        response=reply()
        if response[:4]==b'FAIL':raise DownloadError('RAM_download_rejected_before_data',True)
        if response!=('DATA%08x'%len(payload)).encode('ascii'):
            raise DownloadError('RAM_download_DATA_length_mismatch',False)
        sent=0
        while sent<len(payload):
            bounded()
            chunk=payload[sent:sent+0x4000]
            count=write(chunk)
            if type(count) is not int or not 0<count<=len(chunk):
                raise DownloadError('RAM_download_invalid_partial_write',False)
            sent+=count
        response=reply()
        if response[:4]==b'FAIL':raise DownloadError('RAM_download_rejected_after_data',True)
        if response!=b'OKAY':raise DownloadError('RAM_download_terminal_OKAY_missing',False)
        command_ready=True
        return sent
    except DownloadError:raise
    except Exception as error:
        raise DownloadError('RAM_download_transport_'+type(error).__name__,command_ready) from None


def prior_lk_trial_ok(previous,stage_sha):
    require(previous.get('stock_Android_return_observed') is True and
            previous.get('after',{}).get('boot_completed') is True and
            previous.get('device_storage_write_commands')==[],
            'prior_LK_trial_ADB_stock_return_not_observed')
    require(previous.get('ARM32_stage_executed') is True and
            previous.get('RAM_patched_LK_executed') is True,'prior_LK_command_execution_not_observed')
    require(previous.get('stage_sha256')==stage_sha,'prior_LK_trial_used_different_stage')


def edk2_candidate(image_path,artifacts,plan):
    sha=lambda data:hashlib.sha256(data).hexdigest()
    image=image_path.read_bytes()
    require(4096<=len(image)<=0x200000 and image[:8]==b'ANDROID!','EDK2_candidate_not_bounded_boot_image')
    require(struct.unpack_from('<I',image,40)[0]==4 and
            struct.unpack_from('<I',image,12)[0]==0,'EDK2_candidate_not_no_ramdisk_v4')
    decode=json.loads((artifacts/'trial-kernel-image-verification.json').read_text())
    lk=json.loads((artifacts/'trial-LK-verification.json').read_text())
    wd=json.loads((artifacts/'watchdog-verification.json').read_text())
    handoff=json.loads((artifacts/'kernel-handoff-verification.json').read_text())
    firmware=json.loads((artifacts/'rebuilt-firmware-verification.json').read_text())
    require(all(x.get('all_passed') is True for x in (decode,lk,wd,handoff)),
            'EDK2_candidate_CPU_checks_not_passed')
    require(sha(image)==decode['candidate_boot_image_sha256']==lk['candidate_image_sha256']==
            handoff['candidate_image_sha256'],'EDK2_candidate_changed_after_checks')
    require(lk['payload_sha256']==handoff['payload_sha256']==plan['LK_payload_sha256'],
            'EDK2_candidate_LK_payload_changed')
    require(decode['raw_BootShim_image_sha256']==wd['trial_image_sha256'],
            'EDK2_candidate_is_not_verified_trial_image')
    require(firmware.get('status')=='pass' and firmware['continuous_boot']['status']=='passed' and
            firmware['fd_sha256']==decode['FD_sha256']==wd['FD_sha256'],
            'EDK2_candidate_firmware_changed')
    package=Path(__file__).resolve().parents[1]
    for name,digest in wd['source_sha256'].items():
        require(sha((package/name).read_bytes())==digest,'trial_watchdog_source_changed_after_check')
    require(sha((package/'Tests/LkKernelHandoffTest.py').read_bytes())==handoff['test_source_sha256'],
            'LK_handoff_test_changed_after_check')
    require(sha((package/'Tests/LkRamBootTest.py').read_bytes())==handoff['consumer_test_source_sha256'],
            'LK_consumer_test_changed_after_check')
    return image


def bootloader_trace(reply):
    require(reply[0]==b'OKAY','fastboot_version_reply_rejected')
    value=reply[1]
    if value==VERSION.encode():return {'observed':False,'last_completed_stage':'no_RAM_trace_observed'}
    prefix=VERSION[:-4].encode()+b'RB0'
    require(value[:len(prefix)]==prefix and len(value)==len(VERSION) and value[-1:] in b'12345',
            'fastboot_version-bootloader_mismatch')
    stage=int(chr(value[-1]))
    return {'observed':True,'last_completed_stage':('','LK_prefix_and_version_matched',
            'LK_registration_guard_matched','LK_source_guard_matched','LK_code_cave_matched',
            'LK_RAM_edits_and_cache_sync_finished')[stage]}


def preloader_return_allowed(phase,guard_audit):
    # The audit never waits for input. A normal bridge may instead be consuming
    # its resume token; never send a parser command into that unknown stream.
    return phase=='preloader' or (phase=='jump_requested' and guard_audit)


def resume_ram_bridge(write):
    result=write(RESUME_TOKEN)
    require(result is True or (type(result) is int and result==len(RESUME_TOKEN)),
            'RAM_bridge_resume_token_transfer_failed')


def finish_ram_bridge(write):
    result=write(FINAL_RECEIPT)
    require(result is True or (type(result) is int and result==len(FINAL_RECEIPT)),
            'RAM_bridge_readback_receipt_transfer_failed')


def mutation_stage_record(raw):
    require(len(raw)==40,'RAM_mutation_record_length_mismatch')
    magic,version,state,offset,instruction,readback,rvbar,receive,token=struct.unpack('<8s8I',raw)
    require(magic==b'T70ARM32' and version==1,'RAM_mutation_record_marker_mismatch')
    require(state in range(9) and readback==int(state==7) and rvbar==1 and token==1,
            'RAM_mutation_record_flags_invalid')
    require(offset==0 or state==5 and 0x400<=offset<0x800 and offset%4==0,
            'RAM_mutation_record_failure_offset_invalid')
    require(state<5 or instruction==0x94000349,'RAM_mutation_instruction_inconsistent')
    return {'patch_state':('not_attempted','MMU_enabled','physical_range_rejected','prefix_mismatch',
                           'original_instruction_mismatch','code_cave_occupied','writes_in_progress',
                           'RAM_patch_readback_matched','RAM_patch_readback_mismatch')[state],
            'BL2_RAM_patch_readback_matched':bool(readback),'RVBAR_hook_readback_matched':bool(rvbar),
            'first_occupied_cave_offset':hex(offset) if offset else None,
            'shipping_BL2_hook_instruction':hex(instruction),
            'USB_receive_return_value':receive,'exact_host_resume_token_received':True}


def read_stage_record(read,size):
    require(size in (40,64,80,224,296),'stage_record_size_outside_allowlist')
    result=bytearray()
    # A bulk read can end after one USB packet. Consume the rest of this exact
    # record before issuing another parser command. The caller also has an alarm.
    for _ in range(size):
        chunk=bytes(read(size-len(result)))
        require(len(chunk)<=size-len(result),'stage_record_read_exceeded_request')
        if not chunk:break
        result.extend(chunk)
        if len(result)==size:break
    return bytes(result)


def boot_stage_record(raw):
    require(len(raw)==64,'stage_record_length_mismatch')
    r=struct.unpack('<8s14I',raw)
    require(r[:3]==(b'T70STG32',3,ENTRY) and r[6:9]==(0x401ffff4,12,0x2091cb),
            'stage_execution_marker_mismatch')
    state,base_low,base_high,size,matched,descriptor=r[9:]
    require(state in range(5) and matched==int(state==4),'stage_allocation_flags_invalid')
    if state<=1:require((base_low,base_high,size,matched,descriptor)==(0,0,0,0,0),'stage_absent_allocation_has_values')
    else:
        require(descriptor%4==0 and (0x200000<=descriptor<=0x2fffec or
                0x40000000<=descriptor<=0xffffffec),'stage_allocation_descriptor_outside_RAM')
        if state>=3:
            require(base_high==0 and 0x40000000<=base_low<=0xfff00000 and
                    base_low%4096==0 and size>=645992,'stage_allocation_probe_bounds_invalid')
    return {'state':('table_unavailable','descriptor_absent','descriptor_found_unprobed',
                     'descriptor_found_prefix_differs','BL2_prefix_already_loaded')[state],
            'BL2_physical_base':hex((base_high<<32)|base_low),
            'reserved_bytes_low32':size,'descriptor_address':hex(descriptor),
            'shipping_BL2_prefix_already_present':bool(matched)}


def guard_audit_candidate(artifacts,preloader):
    sha=lambda data:hashlib.sha256(data).hexdigest()
    proof=json.loads((artifacts/'guard-audit-verification.json').read_text())
    stage=(artifacts/'preloader-guard-audit.bin').read_bytes()
    require(proof.get('all_passed') is True and proof.get('probe_version')==3 and
            proof.get('record_bytes')==80 and
            proof.get('shipping_code_and_saved_caller_frame_unchanged') is True and
            proof.get('following_shipping_SEND_DA_header_verified',{}).get('actual_next_SEND_DA_handler_reached') is True,
            'guard_audit_CPU_checks_not_passed')
    require(proof['payload_sha256']==sha(stage) and proof['preloader_sha256']==sha(preloader),
            'guard_audit_binary_changed_after_CPU_checks')
    package=Path(__file__).resolve().parents[1]
    required={'Tools/PreloaderRamProbe.S','Tests/PreloaderGuardAuditTest.py',
              'Tests/PreloaderRamProbeTest.py','Tests/RamBootChainTest.py'}
    require(set(proof['source_sha256'])==required,'guard_audit_proof_source_set_changed')
    for name,digest in proof['source_sha256'].items():
        require(sha((package/name).read_bytes())==digest,'guard_audit_source_changed_after_CPU_checks')
    require(80<len(stage)<=4096 and len(stage)%4==0,'guard_audit_size_invalid')
    return stage


def retained_probe_candidate(artifacts,preloader):
    sha=lambda data:hashlib.sha256(data).hexdigest()
    stage=(artifacts/'preloader-retained-probe.bin').read_bytes()
    proof=json.loads((artifacts/'retained-probe-verification.json').read_text())
    require(proof.get('all_passed') is True and proof.get('record_bytes')==296,
            'retained_probe_CPU_checks_not_passed')
    require(proof['payload_sha256']==sha(stage) and proof['preloader_sha256']==sha(preloader),
            'retained_probe_changed_after_CPU_checks')
    package=Path(__file__).resolve().parents[1]
    required={'Tools/PreloaderRamBoot.S','Tests/RetainedBootProbeTest.py','Tests/RamBootChainTest.py'}
    require(set(proof['source_sha256'])==required,'retained_probe_source_set_changed')
    for name,digest in proof['source_sha256'].items():
        require(sha((package/name).read_bytes())==digest,'retained_probe_source_changed_after_CPU_checks')
    require(296<len(stage)<=4096 and len(stage)%4==0,'retained_probe_size_invalid')
    return stage


def retained_boot_record(raw):
    require(len(raw) in (224,296),'retained_record_length_mismatch')
    header=struct.unpack_from('<8s8I',raw)
    require(header[:3]==(b'T70DIA32',3 if len(raw)==296 else 2,ENTRY) and header[6:]==(0x401ffff4,12,0x2091cb),
            'retained_record_execution_marker_mismatch')
    require(header[3]&0x3f==0x13 and header[4]&0x1005==0,'retained_probe_CPU_mode_mismatch')
    cookie,handoff=struct.unpack_from('<II',raw,40)
    cleared,head_cleared,gop_cleared,snapshot_cleared=struct.unpack_from('<4I',raw,208)
    require(cookie in (0,1) and handoff in (0,1) and cleared==cookie and head_cleared==handoff and
            gop_cleared in (0,1) and snapshot_cleared in ((0,1) if len(raw)==296 else (0,)),
            'retained_record_flags_invalid')
    trial=struct.unpack_from('<4Q',raw,48);entry=struct.unpack_from('<12Q',raw,80)
    if cookie:
        require(trial[:2]==(0x0000000157443754,0xfffffffea8bbc8ab) and trial[3] in (1,2,3),
                'retained_trial_cookie_invalid')
    else:require(trial==(0,)*4,'unrecognized_trial_RAM_was_exported')
    if handoff:
        require(entry[0]==0x0000000148303754 and entry[3]==0x40000000 and entry[7] in (1,2,3,4),
                'retained_handoff_invalid')
    else:require(entry==(0,)*12,'unrecognized_handoff_RAM_was_exported')
    if not handoff or entry[7]<4:
        require(entry[8:]==(0,)*4,'uninitialized_DXE_RAM_was_exported')
    gop=struct.unpack_from('<QIIQQ',raw,176)
    if gop_cleared:
        require(handoff and gop[0]==0x0000000147503754 and 1<=gop[1]<=8 and gop[2]>0,
                'retained_GOP_record_invalid')
    else:require(gop==(0,)*5,'unrecognized_GOP_RAM_was_exported')
    snapshot = None
    if len(raw)==296:
        values=struct.unpack_from('<Q16I',raw,224)
        if snapshot_cleared:
            ovl = values[0]==0x000000014c443754
            dsi = values[0]==0x000000024c443754
            overlap = values[0]==0x000000034c443754
            sec_map = values[0]==0x000000044c443754
            sec_exception = values[0]==0x000000054c443754
            bds = values[0] in (0x000000064c443754,0x000000074c443754)
            bds_launch = values[0]==0x000000074c443754
            valid_ovl = (ovl and values[1] in (0x14005000,0x14006000) and
                         values[2] < (4 if values[1]==0x14005000 else 2) and values[6]==1<<values[2])
            valid_dsi = dsi and values[1:3]==(0x14013000,0) and values[15:]==(0,0)
            valid_overlap = overlap and values[1:3]==(0x43f7f080,0) and values[13:]==(0,0,0,0)
            valid_sec_map = (sec_map and handoff and entry[7]>=3 and not gop_cleared and
                             values[1:3]==(0x43f7f060,0) and values[3]<=11 and values[4]<128 and
                             values[11]<=4 and values[12:]==(0,)*5)
            valid_sec_exception = (sec_exception and handoff and entry[7]>=3 and not gop_cleared and
                                   values[1:3]==(0x43f7f060,0) and values[3] in (4,8) and
                                   values[4]<=3 and values[15:]==(0,0))
            valid_bds = (bds and gop[1]==8 and gop[3]==0 and values[1:3]==(0x43f7f060,0) and 1<=values[3]<=11 and
                         all(v<=65535 for v in values[4:12]) and values[12] in (0,1) and
                         values[13] in (0,1) and values[14]<=7 and
                         ((not bds_launch and values[15:]==(0,0)) or
                          (bds_launch and values[15]<=8 and values[16]<0x20000)))
            require((gop_cleared and (valid_ovl or valid_dsi or valid_overlap or valid_bds)) or valid_sec_map or valid_sec_exception,
                    'retained_display_snapshot_invalid')
            names = (('enable','ROI','datapath','source','extended_layers','compression',
                      'control','size','offset','pitch_msb','pitch','clip','RDMA_control','DMA_address') if ovl else
                     ('MODE_CTRL','TXRX_CTRL','PSCTRL','VSA_NL','VBP_NL','VFP_NL','VACT_NL',
                      'SIZE_CON','HSA_WC','HBP_WC','HFP_WC','BLLP_WC'))
            snapshot={'kind':'OVL' if ovl else 'DSI','controller':hex(values[1]),'layer':values[2],
                      'registers':dict(zip(names,(hex(v) for v in values[3:])))}
            if overlap:
                details=struct.unpack_from('<5Q',raw,240)
                require(details[0]<details[1] and details[2]<details[3],
                        'retained_memory_overlap_bounds_invalid')
                owner=details[4]>>32
                if owner&0x80000000:owner-=1<<32
                snapshot={'kind':'MEMORY_OVERLAP','video_start':hex(details[0]),'video_end':hex(details[1]),
                          'private_start':hex(details[2]),'private_end':hex(details[3]),
                          'owner':owner,'reservation_index':details[4]&0xffffffff}
            if sec_map:
                address,value,operation=values[9:12]
                require((operation==0 and address==0 and value==0) or
                        (operation>0 and 0x10001000<=address<0x14014000 and address%4==0),
                        'retained_SEC_MMIO_metadata_invalid')
                snapshot={'kind':'SEC_MEMORY_MAP','phase':values[3],
                          'phase_name':('not_started','FDT_validation','reserve_map',
                            'reserved_memory','modem_metadata','LK_video_reservation','private_overlap',
                            'framebuffer_parse','framebuffer_acquisition','framebuffer_reservation',
                            'RAM_banks','map_complete')[values[3]],
                          'additional_descriptors':values[4],
                          'status':hex(values[5]|values[6]<<32),
                          'display_adoption_status':hex(values[7]|values[8]<<32),
                          'last_MMIO_address':hex(address) if operation else None,
                          'last_MMIO_value':hex(value) if operation else None,
                          'last_MMIO_operation':('none','read_requested','read_completed',
                                                 'write_requested','write_completed')[operation]}
            if bds:
                snapshot={'kind':'BDS_PROGRESS','stage':values[3],
                          'stage_name':('','BDS_entry','before_device_connection','console_paths_registered',
                            'after_console_entry','boot_options_refreshed','on_demand_input_connected',
                            'recovery_shell_entered','recovery_shell_returned','setup_application_entered',
                            'setup_SendForm_entered','setup_SendForm_returned')[values[3]],
                          'sequence':values[4],'registered_input_paths':values[5],
                          'registered_output_paths':values[6],
                          'absolute_pointer_handles':values[7],'simple_pointer_handles':values[8],
                          'USB_IO_handles':values[9],'filesystem_handles':values[10],
                          'GOP_handles':values[11],
                          'system_console_input_present':bool(values[12]),
                          'system_console_output_present':bool(values[13]),
                          'connecting_devices':bool(values[14]&1),'recovery_shell_active':bool(values[14]&2),
                          'setup_application_active':bool(values[14]&4),
                          'setup_launch_phase':values[15] if bds_launch else None,
                          'setup_launch_phase_name':('not_attempted','FV_lookup_started','FV_lookup_failed',
                            'LoadImage_entered','LoadImage_failed','image_loaded','UI_timer_failed',
                            'StartImage_entered','StartImage_returned')[values[15]] if bds_launch else None,
                          'setup_launch_EFI_error':bool(values[16]&0x10000) if bds_launch else None,
                          'setup_launch_status_code':values[16]&0xffff if bds_launch else None,
                          'physical_input_action_verified':False}
            if sec_exception:
                esr,pc,far,sctlr,spsr=struct.unpack_from('<5Q',raw,248)
                snapshot={'kind':'SEC_EXCEPTION','CurrentEL':values[3],'exception_type':values[4],
                          'ESR':hex(esr),'exception_class':hex((esr>>26)&0x3f),
                          'PC':hex(pc),'FAR':hex(far),'SCTLR':hex(sctlr),'SPSR':hex(spsr)}
        else:require(values==(0,)*17,'unrecognized_display_snapshot_was_exported')
    early_map=None
    if gop_cleared and gop[1]==2 and gop[4]>>56==0x4D:
        tag,code=gop[4]>>32,gop[4]&0xffffffff
        phase,adoption,added=(tag>>16)&0xff,(tag>>8)&0xff,tag&0xff
        require(phase<=11 and added<128,'retained_early_memory_map_metadata_invalid')
        status=(code&0x7fffffff) | ((1<<63) if code&0x80000000 else 0)
        adopted=(adoption&0x7f) | ((1<<63) if adoption&0x80 else 0)
        early_map={'phase':phase,'phase_name':('not_started','FDT_validation','reserve_map',
                   'reserved_memory','modem_metadata','LK_video_reservation','private_overlap',
                   'framebuffer_parse','framebuffer_acquisition','framebuffer_reservation',
                   'RAM_banks','map_complete')[phase],'status':hex(status),
                   'display_adoption_status':hex(adopted),'additional_descriptors':added}
    activity = bool(gop_cleared and gop[4] & (1 << 63))
    if activity:
        require(gop[1]==8 and gop[3]==0 and
                0<((gop[4]>>32)&0xffff)<=4096 and 0<(gop[4]&0xffff)<=4096,
                'retained_GOP_activity_metadata_invalid')
    sec=bool(cookie and trial[3] in (2,3))
    return {'trial_cookie_observed':bool(cookie),'recognized_old_cookie_cleared':bool(cleared),
            'recognized_old_handoff_cleared':bool(head_cleared),
            'baseline_no_valid_trial_cookie_after_probe':True,
            'baseline_no_valid_handoff_after_probe':True,
            'BootShim_timer_accepted_marker':bool(cookie and trial[2]&1),
             'interactive_RAM_trial_watchdog_disabled':bool(cookie and trial[3]==2 and not trial[2]&1),
            'trial_phase':trial[3] if cookie else None,
            'watchdog_mode':hex(trial[2]&0xffffffff) if cookie else None,
            'watchdog_length':hex(trial[2]>>32) if cookie else None,
            'handoff_observed':bool(handoff),'handoff_stage':entry[7] if handoff else None,
            'FDT_address':hex(entry[1]) if handoff else None,
            'FDT_bytes':entry[2] if handoff else None,
            'CurrentEL':entry[4] if handoff else None,
            'SCTLR':hex(entry[6]) if handoff else None,
            'BootShim_FDT_validation_completed':bool(handoff or sec),
            'FD_copy_completed_marker':bool(sec or handoff and entry[7]>=2),
            'SEC_C_entry_marker':bool(sec or handoff and entry[7]>=3),
            'SEC_evidence_basis':'PlatformInitialize writes trial phase 2/3 only after the completed FD handoff' if sec else 'handoff phase' if handoff and entry[7]>=3 else None,
            'DXE_entry_marker':bool(handoff and entry[7]>=4),
            'UEFI_system_table':hex(entry[8]) if handoff and entry[7]>=4 else None,
            'published_FDT_address':hex(entry[9]) if handoff and entry[7]>=4 else None,
            'published_FDT_bytes':entry[10] if handoff and entry[7]>=4 else None,
            'FDT_publish_status':hex(entry[11]) if handoff and entry[7]>=4 else None,
            'GOP_record_observed':bool(gop_cleared),
            'baseline_no_valid_GOP_record_after_probe':True,
            'GOP_phase':gop[1] if gop_cleared else None,
            'GOP_phase_name':('','start','runtime_framebuffer','allocate_context','memory_map',
                              'protect_framebuffer','exit_event','install_protocol','GOP_installed')[gop[1]] if gop_cleared else None,
            'GOP_attempts':gop[2] if gop_cleared else None,
            'GOP_status':hex(gop[3]) if gop_cleared else None,
            'GOP_width':((gop[4]>>32)&0xffff if activity else gop[4]>>32) if gop_cleared and gop[1]>=3 else None,
            'GOP_height':(gop[4]&0xffff if activity else gop[4]&0xffffffff) if gop_cleared and gop[1]>=3 else None,
            'GOP_activity_counter_format':1 if activity else None,
            'GOP_successful_SetMode_calls':(gop[4]>>16)&0xffff if activity else None,
            'GOP_successful_buffer_to_video_calls':(gop[4]>>48)&0x7fff if activity else None,
            'GOP_activity_counts_saturated':bool(activity and
                (((gop[4]>>16)&0xffff)==0xffff or ((gop[4]>>48)&0x7fff)==0x7fff)),
            'GOP_early_memory_map':early_map,
            'GOP_runtime_step':'early_memory_map' if early_map else {1:'configuration_table',2:'handoff_publication',3:'FDT_validation',
                                4:'simple_framebuffer',5:'LK_handoff_before_MMIO'}.get(gop[4]>>32)
                                if gop_cleared and gop[1]==2 else None,
            'GOP_last_MMIO_address':hex(gop[4]>>32) if gop_cleared and gop[1]==2 and
                                    0x10001000<=gop[4]>>32<0x14014000 else None,
            'GOP_last_MMIO_value':hex(gop[4]&0xffffffff) if gop_cleared and gop[1]==2 and
                                  0x10001000<=gop[4]>>32<0x14014000 else None,
            'LK_display_snapshot':snapshot,
            'SEC_memory_map_snapshot':snapshot if snapshot and snapshot.get('kind')=='SEC_MEMORY_MAP' else None,
            'SEC_exception_snapshot':snapshot if snapshot and snapshot.get('kind')=='SEC_EXCEPTION' else None,
            'BDS_progress_snapshot':snapshot if snapshot and snapshot.get('kind')=='BDS_PROGRESS' else None,
            'recognized_old_display_snapshot_cleared':bool(snapshot_cleared),
            'GOP_installed_marker':bool(gop_cleared and gop[1]==8 and gop[3]==0),
            'negative_result_limitation':'A reset or later boot may overwrite DRAM; absence alone does not prove that code never ran.'}


def configure_stock_preloader(loader,work):
    from mtkclient.config.mtk_config import MtkConfig
    from mtkclient.Library.mtk_class import Mtk
    cfg=MtkConfig(loglevel=logging.ERROR)
    cfg.stock,cfg.generatekeys=True,False
    cfg.skipwdt=True
    cfg.loader=str(loader);cfg.vid,cfg.pid=0x0e8d,0x2000;cfg.hwparam_path=work
    mtk=Mtk(cfg,loglevel=logging.ERROR)
    mtk.preloader.get_meid=lambda:None
    mtk.preloader.get_socid=lambda:b''
    def security_check(display=False):
        require(mtk.preloader.echo(b'\xd8'),'preloader_security_echo_failed')
        require(bytes(mtk.preloader.rbyte(6))==b'\0'*6,'preloader_security_changed')
        return {k:False for k in ('sbc','sla','daa','epp','cert','memread','memwrite','cmdC8')}
    mtk.preloader.get_target_config=security_check
    return mtk,cfg,security_check


def reboot_observed_stock_fastboot(backend,event,*,seconds=25):
    import usb.util
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        found=find_fastboot(backend)
        if found:
            device,ep=found
            try:
                usb.util.claim_interface(device,ep[0])
                command=lambda name:fastboot_command(lambda d:device.write(ep[1],d,timeout=3000),
                                                     lambda n:device.read(ep[2],n,timeout=3000),name)
                require(command('getvar:product')==(b'OKAY',b'tb8781p1_64_td89'),
                        'diagnostic_return_fastboot_product_mismatch')
                version=bootloader_trace(command('getvar:version-bootloader'))
                event('diagnostic_return_fastboot_observed',RAM_trace=version['observed'])
                reply=command('reboot')
                require(reply==(b'OKAY',b''),'diagnostic_stock_reboot_not_acknowledged')
                event('stock_Android_reboot_after_diagnostic_requested',acknowledged=True)
                return True
            finally:usb.util.dispose_resources(device)
        time.sleep(0.2)
    event('diagnostic_continuation_no_fastboot_observed')
    return False


def guard_audit_record(raw):
    require(len(raw)==80,'guard_audit_record_length_mismatch')
    r=struct.unpack('<8s18I',raw)
    require(r[:3]==(b'T70RAM32',3,ENTRY) and r[6:9]==(0x401ffff4,12,0x2091cb),
            'guard_audit_execution_marker_mismatch')
    failure,address,expected,actual,sp,caller,mask,count,reserved1,reserved2=r[9:]
    require(failure in range(5) and 0<=mask<=0x1ff and count==51 and
            reserved1==reserved2==0,'guard_audit_invalid_fields')
    require((0x200000<=sp<=0x2ffe80 or 0x40000000<=sp<=0x4ffffe80) and
            sp%4==0,'guard_audit_stack_outside_RAM')
    cache_bad=bool(r[4]&0x1005);mode_bad=(r[3]&0x3f)!=0x13
    expected_failure=1 if cache_bad else 2 if mode_bad else 3 if mask!=0x1ff else 4 if caller!=0x224e5d else 0
    require(failure==expected_failure,'guard_audit_failure_inconsistent_with_values')
    if failure==0:require((address,expected,actual)==(0,0,0),'guard_audit_unexpected_failure_values')
    elif failure==1:require(address==expected==0 and actual==r[4]&0x1005,'guard_audit_cache_values_mismatch')
    elif failure==2:require(address==0 and expected==0x13 and actual==r[3]&0x3f,'guard_audit_mode_values_mismatch')
    elif failure==3:
        windows=((0x201170,3),(0x209178,21),(0x208e44,5),(0x2093d0,3),
                 (0x224cd4,3),(0x224e58,4),(0x224f2c,5),(0x230f44,3),(0x236904,4))
        index=next((i for i,(base,n) in enumerate(windows) if base<=address<base+n*4 and address%4==0),None)
        require(index is not None and not mask&(1<<index) and expected!=actual,'guard_audit_fingerprint_values_mismatch')
    else:require((address,expected,actual)==(sp+0x174,0x224e5d,caller),'guard_audit_caller_values_mismatch')
    return {'all_bridge_entry_guards_passed':failure==0,
            'first_failed_guard':('none','cache_or_MMU','CPU_mode','shipping_code_window','saved_caller_LR')[failure],
            'address':hex(address),'expected':hex(expected),'actual':hex(actual),
            'parser_SP':hex(sp),'saved_caller_LR':hex(caller),
            'matching_window_mask':hex(mask),'compared_words':count,
            'CPSR':hex(r[3]),'SCTLR':hex(r[4]),'MPIDR':hex(r[5])}


def return_stock_da(mtk,security_check):
    security_check()
    mtk.daloader.patch=False
    require(mtk.daloader.upload_da(preloader=None),'stock_DA_return_failed')
    require(not mtk.daloader.patch,'return_DA_was_modified')
    # Shipping DA mode 1 is HOME_SCREEN on legacy, XFlash and XML transports.
    # Legacy shutdown returns None even after sending FINISH; keep that as an
    # unknown acknowledgment and determine the actual outcome from Android.
    return mtk.daloader.shutdown(bootmode=1)


def attempt_stock_da_return(mtk,security_check):
    # The vendor transport can raise SystemExit on disconnect. It must not skip
    # closing that transport, checking fastboot, and observing Android afterward.
    try:return return_stock_da(mtk,security_check),None
    except (Exception,SystemExit) as error:return False,type(error).__name__


def find_fastboot(backend):
    import usb.core
    matches=[]
    for candidate in usb.core.find(find_all=True,idVendor=0x0e8d,backend=backend):
        try:
            for interface in candidate.get_active_configuration():
                if (interface.bInterfaceClass,interface.bInterfaceSubClass,interface.bInterfaceProtocol)!=(255,66,3):
                    continue
                outs=[e for e in interface if e.bEndpointAddress&128==0 and e.bmAttributes&3==2]
                ins=[e for e in interface if e.bEndpointAddress&128 and e.bmAttributes&3==2]
                if len(outs)==len(ins)==1:
                    matches.append((candidate,(interface.bInterfaceNumber,outs[0].bEndpointAddress,ins[0].bEndpointAddress)))
        except usb.core.USBError:continue
    require(len(matches)<=1,'multiple_fastboot_interfaces')
    return matches[0] if matches else None


def adb_device(adb,serial=None):
    result=subprocess.run([str(adb),'devices'],capture_output=True,text=True,timeout=15)
    devices=[x.split()[0] for x in result.stdout.splitlines()[1:] if x.endswith('\tdevice')]
    require(result.returncode==0,'ADB_device_inventory_failed')
    if serial is not None:
        require(devices.count(serial)==1,'selected_ADB_device_not_ready')
        return serial
    require(len(devices)==1,'one_authorized_ADB_device_required')
    return devices[0]


def battery_power_snapshot(text):
    # Stock Android classifies this wired charger as AC, including with ADB.
    def powered(kind):
        return bool(re.search(r'(?m)^[ \t]*'+kind+r' powered:[ \t]*true[ \t]*\r?$',text))
    return {'USB_charging':powered('USB'),'AC_powered':powered('AC')}


def android_snapshot(adb,device):
    def shell(*args):
        r=subprocess.run([str(adb),'-s',device,'shell',*args],capture_output=True,text=True,timeout=15)
        return r.returncode,r.stdout.strip()
    def prop(name):return shell('getprop',name)[1]
    users=shell('dumpsys','user')[1]
    accessible=shell('ls','-d','/sdcard/Download')
    battery=shell('dumpsys','battery')[1]
    wifi=shell('cmd','wifi','status')[1]
    return {'boot_completed':prop('sys.boot_completed')=='1','slot':prop('ro.boot.slot_suffix'),
            'flash_locked':prop('ro.boot.flash.locked'),'vbmeta_state':prop('ro.boot.vbmeta.device_state'),
            'user_running_unlocked':bool(re.search(r'State: RUNNING_UNLOCKED\b',users)),
            'user_download_directory_accessible':accessible==(0,'/sdcard/Download'),
            'SIM_states':prop('gsm.sim.state').split(','),
            **battery_power_snapshot(battery),
            'WiFi_enabled':'Wifi is enabled' in wifi,
            'WiFi_connected':'Supplicant state: COMPLETED' in wifi or 'Supplicant state: completed' in wifi}


def preflight_ok(previous,snapshot,*,require_wired_power=True):
    require(previous.get('stock_return_verified') is True,'previous_stock_return_user_checks_pending')
    require(snapshot['boot_completed'] and snapshot['user_running_unlocked'] and
            snapshot['user_download_directory_accessible'],'Android_user_storage_still_locked')
    require(snapshot['slot']=='_a' and snapshot['flash_locked']=='0' and
            snapshot['vbmeta_state']=='unlocked','Android_slot_or_lock_state_changed')
    sim_states=snapshot['SIM_states']
    require(isinstance(sim_states,list) and len(sim_states)==2 and 'LOADED' in sim_states and
            all(state in ('LOADED','ABSENT') for state in sim_states) and
            snapshot['WiFi_enabled'] and snapshot['WiFi_connected'],'stock_daily_use_checks_not_passed')
    if require_wired_power:
        require(snapshot.get('USB_charging') is True or snapshot.get('AC_powered') is True,
                'wired_power_not_observed')


def stock_ADB_services_ready(snapshot):
    checks=dict(snapshot,user_running_unlocked=True,user_download_directory_accessible=True)
    try:preflight_ok({'stock_return_verified':True},checks)
    except (ValueError,KeyError):return False
    return True


def ram_probe_preflight_ok(previous,snapshot,*,allow_returned_image=False):
    # User-file/app checks finish this authorized RAM-boot trial after stock
    # return. Intermediate diagnostics and its image request need ADB-observed
    # healthy Android; never label normally locked CE storage as verified.
    if previous.get('stock_return_verified') is not True:
        require(previous.get('stock_Android_return_observed') is True and
                previous.get('after',{}).get('boot_completed') is True,
                'previous_RAM_probe_Android_return_not_observed')
        require(previous.get('device_storage_write_commands')==[],
                'intermediate_probe_requires_RAM_only_previous_trial')
        candidate=previous.get('trial_candidate_sha256','missing')
        if allow_returned_image and isinstance(candidate,str) and re.fullmatch('[0-9a-f]{64}',candidate):
            require(previous.get('EDK2_boot_command_acknowledged') is True and
                    previous.get('ARM32_stage_executed') is True and
                    previous.get('RAM_patched_LK_executed') is True,
                    'previous_image_trial_execution_evidence_missing')
            prior_after=dict(previous.get('after_settled',previous['after']),user_running_unlocked=True,
                             user_download_directory_accessible=True)
            # Older immutable reports did not capture AC power. Keep their
            # slot/SIM/Wi-Fi checks; require wired power from the fresh live
            # snapshot below instead of reusing a historical classification.
            preflight_ok({'stock_return_verified':True},prior_after,require_wired_power=False)
        else:
            require(candidate is None and previous.get('EDK2_boot_command_acknowledged') is False and
                    previous.get('UEFI_executed') is False,
                    'intermediate_probe_requires_RAM_only_previous_trial')
    checks=dict(snapshot)
    # These two CE fields remain recorded as observed, without pretending the
    # user unlocked. Only this RAM-only trial gate omits them.
    checks['user_running_unlocked']=True
    checks['user_download_directory_accessible']=True
    preflight_ok({'stock_return_verified':True},checks)


def validate_trial_mode(image_requested,capture_reset_record,pogo_detach_test,leave_uefi_running=False):
    require(not capture_reset_record or image_requested,'reset_capture_requires_EDK2_trial')
    require(not pogo_detach_test or image_requested,'pogo_detach_requires_EDK2_trial')
    require(not pogo_detach_test or not capture_reset_record,
            'pogo_detach_cannot_capture_disconnected_reset_record')
    require(not leave_uefi_running or (image_requested and not capture_reset_record),
            'interactive_UEFI_requires_image_without_reset_capture')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('artifacts','preloader','loader','vendor','previous-return','report'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--adb',type=Path,default=Path('/opt/homebrew/bin/adb'))
    p.add_argument('--serial',help='exact authorized ADB device; otherwise require only one connected device')
    p.add_argument('--execute',action='store_true')
    mode=p.add_mutually_exclusive_group()
    mode.add_argument('--edk2-trial-image',type=Path)
    mode.add_argument('--guard-audit',action='store_true')
    mode.add_argument('--retained-probe',action='store_true')
    p.add_argument('--capture-reset-record',action='store_true')
    p.add_argument('--pogo-detach-test',action='store_true',
                   help='disconnect PC USB after the RAM boot ACK, then test the pogo keyboard')
    p.add_argument('--leave-uefi-running',action='store_true',help='leave interactive UEFI running until manual restart')
    p.add_argument('--prior-lk-trial',type=Path)
    a=p.parse_args()
    validate_trial_mode(a.edk2_trial_image is not None,a.capture_reset_record,a.pogo_detach_test,a.leave_uefi_running)
    for name in ('artifacts','preloader','loader','vendor','previous_return','report','adb'):
        setattr(a,name,getattr(a,name).resolve())
    plan=json.loads((a.artifacts/'bootchain-ram-plan.json').read_text())
    chain=json.loads((a.artifacts/'bootchain-verification.json').read_text())
    lk=json.loads((a.artifacts/'verification.json').read_text())
    stage=(a.artifacts/'preloader-ram-boot.bin').read_bytes()
    sha=lambda data:hashlib.sha256(data).hexdigest()
    transport=json.loads((a.artifacts/'transport-verification.json').read_text())
    require(transport.get('all_passed') is True and
            transport['runner_source_sha256']==sha(Path(__file__).read_bytes()) and
            transport['test_source_sha256']==sha((Path(__file__).resolve().parents[1]/'Tests/LkRamBootTransportTest.py').read_bytes()),
            'host_protocol_code_changed_after_boundary_checks')
    require(chain.get('all_passed') is True and lk.get('all_passed') is True,'CPU_checks_not_passed')
    require(sha(stage)==plan['preloader_stage_sha256']==chain['preloader_stage_sha256'],
            'preloader_stage_changed_after_CPU_test')
    require(sha(a.preloader.read_bytes())==plan['shipping_preloader_sha256'],'shipping_preloader_changed')
    require(sha(a.loader.read_bytes())==DA_SHA,'shipping_return_DA_changed')
    require(plan['BL2_bridge_sha256']==chain['BL2_bridge_sha256'] and
            plan['LK_payload_sha256']==chain['LK_payload_sha256']==lk['payload_sha256'],
            'bridge_artifacts_changed_after_CPU_test')
    require(40<len(stage)<=4096 and len(stage)%4==0,'stage_size_invalid')
    if a.guard_audit:stage=guard_audit_candidate(a.artifacts,a.preloader.read_bytes())
    diagnostic_stage=retained_probe_candidate(a.artifacts,a.preloader.read_bytes()) if a.retained_probe or a.capture_reset_record else None
    if a.retained_probe:stage=diagnostic_stage
    trial_image=edk2_candidate(a.edk2_trial_image.resolve(),a.artifacts,plan) if a.edk2_trial_image else None
    if trial_image is not None:
        timer=json.loads((a.artifacts/'watchdog-verification.json').read_text())
        require(a.leave_uefi_running == (timer.get('trial_timeout_seconds') is None and
                timer.get('interactive_watchdog_disabled') is True),
                'trial_image_timer_policy_does_not_match_requested_session')
    if a.execute and trial_image is not None:
        require(a.prior_lk_trial is not None,'EDK2_trial_requires_prior_physical_LK_trial')
        previous=json.loads(a.previous_return.read_text())
        if a.prior_lk_trial.resolve()!=a.previous_return:
            require(previous.get('retained_probe_only') is True and
                    previous.get('prior_return_report_sha256')==sha(a.prior_lk_trial.read_bytes()) and
                    previous.get('retained_boot_record',{}).get('baseline_no_valid_trial_cookie_after_probe') is True,
                    'latest_diagnostic_not_bound_to_prior_LK_trial')
        prior_lk_trial_ok(json.loads(a.prior_lk_trial.read_text()),sha(stage))
        if a.capture_reset_record:
            require(previous.get('retained_probe_only') is True and
                    previous.get('retained_boot_record',{}).get('baseline_no_valid_trial_cookie_after_probe') is True,
                    'reset_capture_requires_observed_clean_baseline')
    if not a.execute:
        print(json.dumps({'prepared':True,'stage_bytes':len(stage),'device_access':False,
                          'trial':('bounded own-record RAM diagnostic and stock Android return' if a.retained_probe else
                                   'read-only bridge guard audit and stock Android return' if a.guard_audit else
                                   'EDK2 interactive RAM boot; manual restart required' if a.leave_uefi_running else
                                    'EDK2 RAM boot request and stock return observation' if trial_image is not None else
                                   'LK command execution followed by stock Android return'),
                          'EDK2_execution_will_be_requested':trial_image is not None,
                          'PC_USB_disconnect_after_ACK_requested':a.pogo_detach_test,
                           'leave_UEFI_running':a.leave_uefi_running,
                          'physical_LK_and_stock_return_gate_required':trial_image is not None}))
        return 0
    require(not a.report.exists(),'report_already_exists')
    doc={'started_at':datetime.now(timezone.utc).isoformat(),'events':[],'status':'prepared',
         'ARM32_stage_executed':False,'RAM_patched_LK_executed':False,'UEFI_executed':False,
         'device_storage_write_commands':[],'stock_return_verified':False,
         'stage_sha256':sha(stage),'device_reboot_requested':False,
         'runner_source_sha256':sha(Path(__file__).read_bytes()),
         'transport_verification_sha256':sha((a.artifacts/'transport-verification.json').read_bytes()),
         'stage_record_version':3,
         'reset_diagnostic_stage_sha256':sha(diagnostic_stage) if a.capture_reset_record else None,
         'guard_audit_only':a.guard_audit,'retained_probe_only':a.retained_probe,
         'prior_return_report_sha256':sha(a.previous_return.read_bytes()),
         'capture_reset_record_requested':a.capture_reset_record,
         'pogo_detach_test_requested':a.pogo_detach_test,
          'leave_UEFI_running_requested':a.leave_uefi_running,
         'EDK2_boot_command_acknowledged':False,
         'trial_candidate_sha256':sha(trial_image) if trial_image is not None else None}
    started_monotonic=time.monotonic()
    def event(name,**data):
        data={'at':datetime.now(timezone.utc).isoformat(),
              'elapsed_seconds':round(time.monotonic()-started_monotonic,3),**data}
        doc['events'].append({'event':name,**data})
        tmp=a.report.with_suffix('.json.tmp');tmp.write_text(json.dumps(doc,indent=2)+'\n');os.replace(tmp,a.report)
        print(json.dumps({'event':name,**data}),flush=True)
    def timeout(*_):raise TimeoutError('bounded_RAM_trial_timeout')
    old_signal=signal.signal(signal.SIGALRM,timeout)
    mtk=fb_device=backend=working=None;phase='preflight';quiet=contextlib.nullcontext
    command_ready=True
    try:
        device=adb_device(a.adb,a.serial)
        doc['before']=android_snapshot(a.adb,device)
        previous=json.loads(a.previous_return.read_text())
        ram_probe_preflight_ok(previous,doc['before'],allow_returned_image=a.retained_probe or trial_image is not None)
        doc['preflight_scope']='RAM_trial_ADB_return_user_file_checks_at_end'
        doc['user_file_app_checks_deferred_to_final_stock_return']=True
        event('stock_return_and_live_preflight_verified',scope=doc['preflight_scope'])
        helpers=runpy.run_path(str(Path(__file__).with_name('preloader-ram-probe.py')))
        quiet=helpers['quiet']
        import usb.core,usb.util,usb.backend.libusb1
        backend=usb.backend.libusb1.get_backend()
        require(backend is not None,'libusb_unavailable')
        sys.dont_write_bytecode=True;sys.path.insert(0,str(a.vendor))
        working=tempfile.TemporaryDirectory(prefix='lk-ram-',dir=a.report.parent)
        work=working.name
        with contextlib.chdir(work):
            with quiet():
                mtk,cfg,strict_config=configure_stock_preloader(a.loader,work)
            # Catch the preloader's normal USB window on the way to fastboot.
            event('bootloader_restart_requested')
            phase='reboot_requested'
            doc['device_reboot_requested']=True
            r=subprocess.run([str(a.adb),'-s',device,'reboot','bootloader'],capture_output=True,timeout=15)
            require(r.returncode==0,'ADB_bootloader_restart_failed')
            with quiet():
                signal.alarm(35)
                require(mtk.preloader.init(display=False,directory=work),'preloader_window_not_captured')
                require(not cfg.is_brom and (cfg.hwcode,cfg.hw_sub_code,cfg.hwver)==(0x1208,0x8a00,0xca00),
                        'unexpected_preloader_hardware')
            phase='preloader';event('original_preloader_security_and_hardware_verified')
            with quiet():
                require(mtk.preloader.send_da(ENTRY,len(stage),0,stage),'RAM_transfer_failed')
            phase='jump_requested';event('RAM_bridge_transferred',bytes=len(stage))
            with quiet():
                require(mtk.preloader.jump_da(ENTRY),'RAM_jump_rejected')
                signal.alarm(8)
                raw=read_stage_record(mtk.port.usbread,296 if a.retained_probe else 80 if a.guard_audit else 64)
            event('RAM_stage_reply_length_observed',bytes=len(raw),expected=296 if a.retained_probe else 80 if a.guard_audit else 64)
            if a.retained_probe:
                require(len(raw)==296,'retained_probe_incomplete_record')
                try:doc['retained_boot_record']=retained_boot_record(raw)
                finally:
                    with quiet():finish_ram_bridge(mtk.port.usbwrite)
                doc['ARM32_stage_executed']=True
                event('bounded_retained_boot_record_observed',**doc['retained_boot_record'])
                with quiet():mtk.port.close(reset=False)
                mtk=None;signal.alarm(0);phase='retained_probe_resumed'
                reboot_observed_stock_fastboot(backend,event)
                phase='stock_return'
            elif a.guard_audit:
                doc['guard_audit']=guard_audit_record(raw)
                doc['ARM32_stage_executed']=True
                phase='guard_audit_return'
                event('read_only_bridge_guard_audit_received',**doc['guard_audit'])
                signal.alarm(45)
                with quiet():
                    doc['stock_reboot_acknowledged']=return_stock_da(mtk,strict_config)
                doc['stock_return_driver']=type(mtk.daloader.da).__name__
                require(doc['stock_reboot_acknowledged'] is not False,'stock_DA_shutdown_rejected')
                event('shipping_parser_continuation_and_stock_DA_return_requested',
                      acknowledged=doc['stock_reboot_acknowledged'],bootmode='HOME_SCREEN')
                with quiet():mtk.port.close(reset=False)
                mtk=None;signal.alarm(0)
                phase='stock_return'
            else:
                doc['preloader_allocation']=boot_stage_record(raw)
                event('shipping_BL2_allocation_observed',**doc['preloader_allocation'])
                doc['ARM32_stage_executed']=True
                event('ARM32_bridge_execution_marker_observed')
                with quiet():resume_ram_bridge(mtk.port.usbwrite)
                doc['boot_chain_resume_requested']=True
                phase='normal_boot_chain';event('host_receipt_confirmed_boot_chain_resume_requested')
                with quiet():raw=read_stage_record(mtk.port.usbread,40)
                doc['preloader_mutation']=mutation_stage_record(raw)
                event('physical_RAM_patch_readback_received',**doc['preloader_mutation'])
                with quiet():finish_ram_bridge(mtk.port.usbwrite)
                event('RAM_patch_readback_receipt_sent')
                with quiet():mtk.port.close(reset=False)
                mtk=None;signal.alarm(0)
                deadline=time.monotonic()+45
                endpoints=None
                while time.monotonic()<deadline:
                    found=find_fastboot(backend) if command_ready else None
                    if not command_ready:event('fastboot_input_state_unknown_reboot_command_withheld')
                    if found:fb_device,endpoints=found
                    if endpoints:break
                    time.sleep(0.2)
                require(fb_device is not None,'fastboot_did_not_appear')
                usb.util.claim_interface(fb_device,endpoints[0])
                write=lambda data:fb_device.write(endpoints[1],data,timeout=3000)
                read=lambda size:fb_device.read(endpoints[2],size,timeout=3000)
                def command(name):return fastboot_command(write,read,name)
                phase='fastboot';event('fastboot_USB_transport_observed')
                try:
                    for name,value in (('product',b'tb8781p1_64_td89'),
                                       ('unlocked',b'yes'),('current-slot',b'a'),('is-userspace',b'no')):
                        require(command('getvar:'+name)==(b'OKAY',value),'fastboot_'+name+'_mismatch')
                    doc['BL2_to_LK_trace']=bootloader_trace(command('getvar:version-bootloader'))
                    event('BL2_to_LK_progress_observed',**doc['BL2_to_LK_trace'])
                    boot_reply=command('boot')
                    event('LK_boot_probe_reply_observed',status=boot_reply[0].decode('ascii'),
                          recognized=('RAM_boot_handler' if boot_reply==(b'FAIL',FAILURE) else
                                      'unknown_command' if boot_reply==(b'FAIL',b'unknown command') else
                                      'security_control_denied' if boot_reply==(b'FAIL',b'No support by security control') else 'other'),
                          reply_sha256=sha(boot_reply[1]))
                    require(boot_reply!=(b'FAIL',b'No support by security control'),
                            'shipping_security_control_rejected_RAM_boot')
                    require(boot_reply==(b'FAIL',FAILURE),'RAM_boot_command_not_installed_or_unexpected_reply')
                    doc['RAM_patched_LK_executed']=True
                    event('RAM_patched_64bit_LK_command_execution_observed')
                    if trial_image is not None:
                        phase='RAM_image_download';command_ready=False
                        try:
                            sent=fastboot_download(write,read,trial_image)
                        except DownloadError as error:
                            command_ready=error.command_ready
                            raise
                        command_ready=True
                        event('hash_verified_EDK2_trial_image_downloaded_to_RAM',bytes=sent)
                        phase='EDK2_boot_request';command_ready=False
                        reply=command('boot')
                        command_ready=reply[0]==b'FAIL'
                        require(reply==(b'OKAY',b''),'EDK2_RAM_boot_command_not_acknowledged')
                        doc['EDK2_boot_command_acknowledged']=True
                        phase='EDK2_RAM_requested'
                        event('EDK2_RAM_boot_requested',UEFI_execution_observed=False,
                              hardware_trial_timer_seconds=None if a.leave_uefi_running else 30,
                               maximum_live_setup_feed_seconds=None if a.leave_uefi_running else 120)
                        if a.pogo_detach_test:
                            event('pogo_keyboard_detach_window_opened',
                                  action='disconnect_PC_USB_then_attach_pogo_keyboard',
                                  reset_record_capture=False,
                                  reconnect_PC_USB_after_Android_return=True,
                                   manual_restart_required=a.leave_uefi_running)
                finally:
                    try:
                        if command_ready:
                            reply=command('reboot')
                            doc['stock_reboot_acknowledged']=reply[0]==b'OKAY'
                            event('stock_Android_reboot_requested',acknowledged=doc['stock_reboot_acknowledged'])
                            phase='stock_return'
                        else:
                            event('interactive_UEFI_transport_released' if a.leave_uefi_running else
                                  'awaiting_hardware_or_stock_return_without_further_fastboot_input')
                    finally:
                        usb.util.dispose_resources(fb_device);fb_device=None
                if a.leave_uefi_running and doc['EDK2_boot_command_acknowledged']:
                    doc['status']='interactive_UEFI_requested_manual_restart_pending'
                    doc['manual_reset_required_for_Android_return']=True
                    event('interactive_UEFI_left_running',automatic_reset_timer=False,
                          physical_UI_observation_pending=True)
                    return 0
                if a.capture_reset_record and doc['EDK2_boot_command_acknowledged']:
                    phase='reset_window_wait'
                    event('waiting_for_trial_reset_preloader_window')
                    with quiet():
                        mtk,cfg,strict_config=configure_stock_preloader(a.loader,work)
                        signal.alarm(200)
                        require(mtk.preloader.init(display=False,directory=work),'post_trial_preloader_window_not_captured')
                        require(not cfg.is_brom and (cfg.hwcode,cfg.hw_sub_code,cfg.hwver)==(0x1208,0x8a00,0xca00),
                                'post_trial_preloader_hardware_mismatch')
                    phase='preloader';event('post_trial_reset_preloader_captured')
                    with quiet():
                        require(mtk.preloader.send_da(ENTRY,len(diagnostic_stage),0,diagnostic_stage),
                                'post_trial_diagnostic_RAM_transfer_failed')
                        phase='jump_requested'
                        require(mtk.preloader.jump_da(ENTRY),'post_trial_diagnostic_jump_rejected')
                        signal.alarm(8)
                        raw=read_stage_record(mtk.port.usbread,296)
                    require(len(raw)==296,'post_trial_diagnostic_incomplete_record')
                    try:doc['post_reset_boot_record']=retained_boot_record(raw)
                    finally:
                        with quiet():finish_ram_bridge(mtk.port.usbwrite)
                    doc['UEFI_executed']=doc['post_reset_boot_record']['SEC_C_entry_marker']
                    event('post_reset_retained_boot_record_observed',**doc['post_reset_boot_record'])
                    with quiet():mtk.port.close(reset=False)
                    mtk=None;signal.alarm(0);phase='retained_probe_resumed'
                    reboot_observed_stock_fastboot(backend,event,seconds=8)
                    phase='stock_return'
        return_seconds=240 if a.pogo_detach_test else 180
        deadline=time.monotonic()+(return_seconds if doc['EDK2_boot_command_acknowledged'] else 120)
        while time.monotonic()<deadline:
            try:
                snapshot=android_snapshot(a.adb,adb_device(a.adb,a.serial))
                if snapshot['boot_completed']:
                    doc['after']=snapshot
                    if not doc.get('stock_Android_return_observed'):
                        doc['stock_Android_return_observed']=True
                        event('Android_boot_completed_observed',ADB_services_ready=stock_ADB_services_ready(snapshot))
                    if not stock_ADB_services_ready(snapshot):
                        time.sleep(1)
                        continue
                    doc['after_settled']=snapshot
                    doc['stock_ADB_services_verified']=True
                    doc['status']=('EDK2_SEC_execution_observed_Android_returned_user_checks_pending' if doc['UEFI_executed'] else
                                   'EDK2_request_acknowledged_Android_returned_user_checks_pending'
                                   if doc['EDK2_boot_command_acknowledged'] else 'stock_Android_returned_user_checks_pending')
                    event('stock_Android_return_observed',user_running_unlocked=snapshot['user_running_unlocked'])
                    return 0 if doc['RAM_patched_LK_executed'] or ((a.guard_audit or a.retained_probe) and doc['ARM32_stage_executed']) else 1
            except (ValueError,subprocess.TimeoutExpired):pass
            time.sleep(1)
        raise ValueError('stock_Android_return_not_observed_before_deadline')
    except BaseException as e:
        signal.alarm(0)
        doc['status']='preflight_gate_closed' if phase=='preflight' else 'stopped';event('stopped',phase=phase,reason=str(e) if isinstance(e,ValueError) else type(e).__name__)
        if a.leave_uefi_running and doc['EDK2_boot_command_acknowledged']:
            doc['status']='interactive_UEFI_requested_host_interrupted_manual_restart_pending'
            event('interactive_UEFI_preserved_after_host_error',automatic_reboot_command_sent=False)
            return 1
        # A rejected entry returns to the original parser with r5 restored. Only
        # its exact live security echo authorizes the existing stock DA return.
        if mtk is not None and preloader_return_allowed(phase,a.guard_audit):
            try:
                signal.alarm(45)
                with contextlib.chdir(work),quiet():
                    acknowledged,error=attempt_stock_da_return(mtk,strict_config)
                    doc['stock_reboot_acknowledged']=acknowledged
                    doc['stock_return_driver']=type(mtk.daloader.da).__name__
                if error:event('stock_return_needs_observation',reason=error)
                else:event('stock_DA_reboot_after_rejected_RAM_stage',acknowledged=acknowledged)
            except (Exception,SystemExit) as error:
                event('stock_return_needs_observation',reason=type(error).__name__)
            finally:
                signal.alarm(0)
        if mtk is not None and phase=='jump_requested' and not a.guard_audit:
            event('preloader_command_withheld_unconfirmed_RAM_resume_state')
        if phase!='preflight' and backend is not None:
            # A missed preloader window may have reached stock fastboot. Reboot
            # through that known protocol, then observe Android independently.
            if mtk is not None:
                with quiet(),contextlib.suppress(Exception,SystemExit):mtk.port.close(reset=False)
                mtk=None
            try:
                found=find_fastboot(backend)
                if found:
                    fallback_device,ep=found
                    try:
                        usb.util.claim_interface(fallback_device,ep[0])
                        ack=fastboot_command(lambda d:fallback_device.write(ep[1],d,timeout=3000),
                                             lambda n:fallback_device.read(ep[2],n,timeout=3000),'reboot')
                        event('stock_fastboot_reboot_after_trial_failure',acknowledged=ack[0]==b'OKAY')
                    finally:usb.util.dispose_resources(fallback_device)
            except Exception:
                event('no_confirmed_fastboot_reboot_transport')
            deadline=time.monotonic()+120
            while time.monotonic()<deadline:
                try:
                    snapshot=android_snapshot(a.adb,adb_device(a.adb,a.serial))
                    if snapshot['boot_completed']:
                        doc['after']=snapshot
                        doc['stock_Android_return_observed']=True
                        event('stock_Android_return_after_trial_failure_observed')
                        break
                except (ValueError,subprocess.TimeoutExpired):pass
                time.sleep(1)
        return 1
    finally:
        signal.alarm(0);signal.signal(signal.SIGALRM,old_signal)
        if mtk is not None:
            with quiet(),contextlib.suppress(Exception,SystemExit):mtk.port.close(reset=False)
        if fb_device is not None:
            with contextlib.suppress(Exception):usb.util.dispose_resources(fb_device)
        if working is not None:working.cleanup()


if __name__=='__main__':
    raise SystemExit(main())
