#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the bounded ARM32 RAM probe, then use the unchanged shipping DA to reboot."""
import argparse
import contextlib
import hashlib
import io
import json
import logging
import os
from pathlib import Path
import signal
import struct
import sys
import tempfile
from datetime import datetime, timezone

ENTRY, BASE = 0x40200000, 0x200f10
DA_SHA = '79619e059128754ef49be6bc7f7ec5332f1d0743a814ef2b89d505c6abde33e0'


class ProbeError(Exception):
    pass


def require(ok, reason):
    if not ok:
        raise ProbeError(reason)



@contextlib.contextmanager
def quiet():
    level = logging.root.manager.disable
    logging.disable(logging.CRITICAL)
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        try:
            yield
        finally:
            logging.disable(level)


def main():
    parser = argparse.ArgumentParser()
    for name in ('vendor','loader','preloader','payload','verification','report'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--execute',action='store_true')
    a = parser.parse_args()
    for name in ('vendor','loader','preloader','payload','verification','report'):
        setattr(a,name,getattr(a,name).resolve())
    binary, original = a.payload.read_bytes(), a.preloader.read_bytes()
    proof = json.loads(a.verification.read_text())
    require(proof.get('all_passed') is True and proof.get('probe_version')==2 and
            proof.get('live_code_verification_before_USB_callback') is True,
            'offline_probe_execution_has_not_passed')
    continuation=proof.get('following_shipping_SEND_DA_header_verified',{})
    require(continuation.get('actual_next_SEND_DA_handler_reached') is True and
            continuation.get('header_destination_is_original_stack_buffer') is True,
            'original_command_loop_continuation_not_tested')
    require(proof.get('payload_sha256') == hashlib.sha256(binary).hexdigest(),'probe_changed_after_test')
    require(proof.get('preloader_sha256') == hashlib.sha256(original).hexdigest(),'preloader_changed_after_test')
    require(hashlib.sha256(a.loader.read_bytes()).hexdigest() == DA_SHA,'shipping_DA_mismatch')
    require(40 < len(binary) <= 4096 and len(binary)%4 == 0,'probe_size_invalid')
    report = {'started_at':datetime.now(timezone.utc).isoformat(),'events':[],
              'status':'prepared','ARM32_probe_executed':False,'UEFI_executed':False,
              'device_storage_write_commands':[],'stock_reboot_requested':False,
              'stock_return_verified':False,'payload_sha256':proof['payload_sha256']}

    def event(name,**values):
        report['events'].append({'event':name,**values})
        tmp = a.report.with_suffix('.json.tmp')
        tmp.write_text(json.dumps(report,indent=2)+'\n')
        os.replace(tmp,a.report)
        print(json.dumps({'event':name,**values}),flush=True)

    if not a.execute:
        print(json.dumps({'prepared':True,'payload_bytes':len(binary),'entry_address':hex(ENTRY),
                          'device_access':False,'UEFI_payload':False}))
        return 0
    import usb.core
    import usb.backend.libusb1
    backend = usb.backend.libusb1.get_backend()
    devices = list(usb.core.find(find_all=True,idVendor=0x0e8d,idProduct=0x2000,backend=backend))
    require(len(devices)==1,'one_preloader_device_required')
    event('unique_preloader_USB_present')
    sys.dont_write_bytecode=True
    sys.path.insert(0,str(a.vendor))
    mtk = None
    phase = 'not_connected'
    working = None
    def deadline(*_):
        raise TimeoutError('bounded_probe_deadline')
    previous = signal.signal(signal.SIGALRM,deadline)
    try:
        working = tempfile.TemporaryDirectory(prefix='t70-ram-',dir=a.report.parent)
        work = working.name
        cwd=Path.cwd()
        try:
            os.chdir(work)
            with quiet():
                from mtkclient.config.mtk_config import MtkConfig
                from mtkclient.Library.mtk_class import Mtk
                config=MtkConfig(loglevel=logging.ERROR)
                config.stock,config.generatekeys=True,False
                config.loader=str(a.loader)
                config.vid,config.pid=0x0e8d,0x2000
                config.hwparam_path=work
                mtk=Mtk(config,loglevel=logging.ERROR)
                mtk.preloader.get_meid=lambda:None
                mtk.preloader.get_socid=lambda:b''
                def strict_config(display=False):
                    pre=mtk.preloader
                    require(pre.echo(pre.Cmd.GET_TARGET_CONFIG.value),'security_query_not_supported')
                    bits,status=struct.unpack('>IH',pre.rbyte(6))
                    require(status==0 and bits==0,'preloader_security_flags_changed')
                    report['live_preloader_target_config']=bits
                    return {k:False for k in ('sbc','sla','daa','epp','cert','memread','memwrite','cmdC8')}
                mtk.preloader.get_target_config=strict_config
                signal.alarm(30)
                require(mtk.preloader.init(display=False,directory=work),'preloader_init_failed')
                require(not config.is_brom and config.hwcode==0x1208,'wrong_preloader_or_soc')
                phase='preloader'
                require(config.hw_sub_code==0x8a00 and config.hwver==0xca00,'hardware_revision_mismatch')
            event('preloader_security_query_verified',security_flags=0)
            with quiet():
                require(mtk.preloader.send_da(ENTRY,len(binary),0,binary),'RAM_probe_transfer_failed')
            event('unsigned_ARM32_probe_transferred',bytes=len(binary),
                  integrity='shipping_SEND_DA_transport_checksum',
                  live_code_check='performed_by_probe_before_USB_callback')
            phase='probe_running'
            with quiet():
                require(mtk.preloader.jump_da(ENTRY),'RAM_probe_jump_rejected')
                raw=bytes(mtk.port.usbread(40))
            require(len(raw)==40,'probe_response_size_mismatch')
            magic,version,pc,cpsr,sctlr,mpidr,arg,arg_size,return_pc=struct.unpack('<8s8I',raw)
            require(magic==b'T70RAM32' and version==2 and pc==ENTRY,'probe_execution_marker_mismatch')
            require(arg==0x401ffff4 and arg_size==12 and return_pc==0x2091cb,'probe_handoff_mismatch')
            report['ARM32_probe_executed']=True
            event('live_preloader_code_and_transport_verified_by_CPU')
            event('ARM32_probe_execution_observed',entry=hex(pc),cpsr=hex(cpsr),sctlr=hex(sctlr),mpidr=hex(mpidr))
            with quiet():
                strict_config()
            phase='preloader'
            event('probe_returned_to_preloader_command_loop')
            with quiet():
                phase='DA_uploading'
                signal.alarm(45)
                mtk.daloader.patch=False
                require(mtk.daloader.upload_da(preloader=None),'shipping_DA_return_path_failed')
                require(not mtk.daloader.patch,'return_path_modified_DA')
                phase='stock_DA'
                report['stock_reboot_request_attempted']=True
                acknowledged=mtk.daloader.shutdown(bootmode=0)
            report['stock_reboot_requested']=bool(acknowledged)
            report['status']='ARM32_probe_executed_stock_reboot_requested'
            event('stock_Android_reboot_requested')
            signal.alarm(0)
        finally:
            os.chdir(cwd)
    except BaseException as exc:
        signal.alarm(0)
        report['status']='stopped'
        event('stopped',reason=str(exc) if isinstance(exc,ProbeError) else type(exc).__name__,phase=phase)
        if mtk and phase=='probe_running':
            # A guard failure returns silently. Re-establish the exact command
            # protocol before considering the unchanged DA reboot path.
            try:
                signal.alarm(10)
                with contextlib.chdir(work), quiet():
                    strict_config()
                phase='preloader'
                event('preloader_command_loop_reestablished_after_missing_probe_response')
            except BaseException:
                event('probe_state_unknown_no_DA_upload_attempted')
            finally:
                signal.alarm(0)
        if mtk and phase=='preloader':
            try:
                signal.alarm(45)
                with contextlib.chdir(work), quiet():
                    strict_config()
                    mtk.daloader.patch=False
                    phase='DA_uploading'
                    require(mtk.daloader.upload_da(preloader=None),'shipping_DA_return_path_failed')
                    require(not mtk.daloader.patch,'return_path_modified_DA')
                    report['stock_reboot_request_attempted']=True
                    report['stock_reboot_requested']=bool(mtk.daloader.shutdown(bootmode=0))
                event('shipping_DA_reboot_after_rejected_probe')
            except BaseException as recovery_exc:
                event('stock_reboot_needs_manual_check',reason=type(recovery_exc).__name__)
            finally:
                signal.alarm(0)
        return 1
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM,previous)
        if mtk:
            with quiet():
                try:mtk.port.close(reset=False)
                except Exception:pass
        if working:
            working.cleanup()
    return 0

if __name__=='__main__':
    raise SystemExit(main())
