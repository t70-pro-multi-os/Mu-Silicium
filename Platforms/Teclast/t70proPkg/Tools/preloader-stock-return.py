#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Finish the known interrupted RAM-only DA command, then reboot using stock DA."""
import argparse
import contextlib
from datetime import datetime,timezone
import functools
import hashlib
import json
import logging
import os
from pathlib import Path
import runpy
import signal
import struct
import sys
import tempfile

BAD_PROBE_SHA='a1495a5d45ef49801c6e394c5dc3e3dc747c9f940e197f3d7779570e6d1a8488'
DA_SHA='79619e059128754ef49be6bc7f7ec5332f1d0743a814ef2b89d505c6abde33e0'


def require(ok,message):
    if not ok:raise ValueError(message)


def read_exact(read,length):
    data=bytearray()
    while len(data)<length:
        part=bytes(read(length-len(data)))
        require(0<len(part)<=length-len(data),'short_or_overlong_USB_reply')
        data.extend(part)
    return bytes(data)


def finish_command(write,read,da1,accepted=lambda:None):
    require(len(da1)==427912,'wrong_DA1_size')
    # These bytes are inert unknown commands if the preloader is already idle.
    # No loader bytes are sent unless both echoes and the receive status match.
    for value in (len(da1),256):
        data=struct.pack('>I',value)
        require(write(data)==len(data),'remaining_header_write_failed')
        require(read_exact(read,4)==data,'remaining_header_echo_mismatch')
    require(read_exact(read,2)==b'\0\0','remaining_header_status_rejected')
    accepted()
    for pos in range(0,len(da1),512):
        data=da1[pos:pos+512]
        require(write(data)==len(data),'DA1_RAM_transfer_failed')
        if (pos+len(data))%0x2000==0:write(b'')
    write(b'')
    result=read_exact(read,4)
    require(len(result)==4,'DA1_RAM_response_size')
    checksum,status=struct.unpack('>HH',result)
    expected=functools.reduce(lambda a,b:a^b,struct.unpack('<'+str(len(da1)//2)+'H',da1),0)
    require(checksum==expected and status==0,'DA1_RAM_checksum_or_status_mismatch')


def main():
    a=argparse.ArgumentParser()
    for name in ('source-report','report','loader','vendor'):
        a.add_argument('--'+name,type=Path,required=True)
    a.add_argument('--execute',action='store_true')
    a.add_argument('--pending-final-status',type=Path)
    a=a.parse_args()
    for name in ('source_report','report','loader','vendor'):
        setattr(a,name,getattr(a,name).resolve())
    source=json.loads(a.source_report.read_text())
    require(source.get('ARM32_probe_executed') is True and source.get('UEFI_executed') is False and
            source.get('payload_sha256')==BAD_PROBE_SHA and source.get('live_preloader_target_config')==0,
            'wrong_interrupted_session')
    require(any(e.get('event')=='stopped' and e.get('phase')=='DA_uploading' and
                e.get('reason')=='shipping_DA_return_path_failed' for e in source['events']),
            'known_interrupted_DA_upload_required')
    binary=a.loader.read_bytes()
    require(hashlib.sha256(binary).hexdigest()==DA_SHA,'shipping_DA_changed')
    offset,size,entry,_,signature=struct.unpack_from('<5I',binary,0x94)
    require((offset,size,entry,signature)==(0xbc,427912,0x200000,256),'DA1_table_mismatch')
    da1=binary[offset:offset+size]
    if a.pending_final_status:
        previous_transfer=json.loads(a.pending_final_status.resolve().read_text())
        events=previous_transfer.get('events',[])
        require(any(e.get('event')=='remaining_RAM_DA_header_accepted' for e in events) and
                events[-1].get('reason')=='DA1_RAM_response_size',
                'known_split_final_reply_required')
    if not a.execute:
        print(json.dumps({'prepared':True,'RAM_bytes':size,'device_access':False}))
        return 0
    require(not a.report.exists(),'report_exists')
    doc={'started_at':datetime.now(timezone.utc).isoformat(),'events':[],
         'device_storage_write_commands':[],'stock_reboot_requested':False,'status':'prepared'}
    def event(name,**fields):
        doc['events'].append({'event':name,**fields})
        a.report.write_text(json.dumps(doc,indent=2)+'\n')
        print(json.dumps({'event':name,**fields}),flush=True)
    def timeout(*_):raise TimeoutError('bounded_stock_return')
    old=signal.signal(signal.SIGALRM,timeout)
    dev=mtk=None
    sys.dont_write_bytecode=True
    helpers=runpy.run_path(str(Path(__file__).with_name('preloader-ram-probe.py')))
    quiet=helpers['quiet']
    try:
        import usb.core,usb.util,usb.backend.libusb1
        devices=list(usb.core.find(find_all=True,idVendor=0x0e8d,idProduct=0x2000,
                                  backend=usb.backend.libusb1.get_backend()))
        require(len(devices)==1,'unique_preloader_not_present')
        dev=devices[0]
        conf=dev.get_active_configuration()
        interfaces=[i for i in conf if i.bInterfaceNumber==1 and i.bAlternateSetting==0 and i.bInterfaceClass==10]
        require(len(interfaces)==1 and
                {e.bEndpointAddress for e in interfaces[0]}=={0x01,0x81},
                'preloader_interface_mismatch')
        usb.util.claim_interface(dev,1)
        signal.alarm(15)
        read=lambda n:dev.read(0x81,n,timeout=1500)
        if a.pending_final_status:
            require(read_exact(read,2)==b'\0\0','pending_final_status_not_success')
            event('previous_DA1_final_status_accepted')
        else:
            finish_command(lambda data:dev.write(0x01,data,timeout=1500),read,da1,
                           lambda:event('remaining_RAM_DA_header_accepted'))
        event('unchanged_DA1_staged_command_parser_restored')
        require(dev.write(0x01,b'\xd8',timeout=1500)==1,'security_query_write_failed')
        require(read_exact(read,1)==b'\xd8','security_query_echo_failed')
        require(read_exact(read,6)==b'\0'*6,'security_query_result_mismatch')
        event('preloader_command_loop_reverified')
        usb.util.dispose_resources(dev);dev=None
        sys.path.insert(0,str(a.vendor))
        with tempfile.TemporaryDirectory(prefix='stock-return-',dir=a.report.parent) as work:
            with contextlib.chdir(work),quiet():
                from mtkclient.config.mtk_config import MtkConfig
                from mtkclient.Library.mtk_class import Mtk
                cfg=MtkConfig(loglevel=logging.ERROR)
                cfg.stock,cfg.generatekeys=True,False
                cfg.loader=str(a.loader);cfg.vid,cfg.pid=0x0e8d,0x2000;cfg.hwparam_path=work
                mtk=Mtk(cfg,loglevel=logging.ERROR)
                mtk.preloader.get_meid=lambda:None
                mtk.preloader.get_socid=lambda:b''
                # The existing command protocol is established above. Init still
                # queries the actual hardware/version/security; no new sync bytes.
                mtk.port.handshake=lambda maxtries:mtk.port.cdc.connect()
                def strict_config(display=False):
                    require(mtk.preloader.echo(b'\xd8'),'security_query_not_echoed')
                    require(bytes(mtk.preloader.rbyte(6))==b'\0'*6,'security_flags_changed')
                    return {k:False for k in ('sbc','sla','daa','epp','cert','memread','memwrite','cmdC8')}
                mtk.preloader.get_target_config=strict_config
                require(mtk.preloader.init(display=False,directory=work),'existing_session_init_failed')
                require(not cfg.is_brom and cfg.hwcode==0x1208 and cfg.hw_sub_code==0x8a00 and
                        cfg.hwver==0xca00,'live_hardware_mismatch')
                signal.alarm(45)
                mtk.daloader.patch=False
                require(mtk.daloader.upload_da(preloader=None),'unchanged_stock_DA_upload_failed')
                require(not mtk.daloader.patch,'stock_DA_modified')
                doc['stock_reboot_requested']=bool(mtk.daloader.shutdown(bootmode=0))
            doc['status']='stock_reboot_requested'
            event('stock_Android_reboot_requested',acknowledged=doc['stock_reboot_requested'])
        return 0
    except Exception as e:
        doc['status']='stock_return_not_observed'
        event('stopped',reason=str(e) if isinstance(e,ValueError) else type(e).__name__)
        return 1
    finally:
        signal.alarm(0);signal.signal(signal.SIGALRM,old)
        if dev is not None:
            with contextlib.suppress(Exception):usb.util.dispose_resources(dev)
        if mtk is not None:
            with quiet(),contextlib.suppress(Exception):mtk.port.close(reset=False)

if __name__=='__main__':
    raise SystemExit(main())
