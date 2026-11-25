from pathlib import Path
import sys,runpy,json,subprocess,os
empty=os.environ.get("EMPTY_READER")=="1"
popen=subprocess.Popen
def launch(args,*a,**kw):
 args=[x+",removable=on" if isinstance(x,str) and x.startswith("usb-storage,id=") else x for x in args]
 return popen(args,*a,**kw)
subprocess.Popen=launch
q=Path('/home/builder/teclast-t70pro/Mu-Silicium/Platforms/Teclast/t70proPkg/Tests/Qemu')
sys.path.insert(0,str(q))
import firmware_probe as f
import media_fixture,hashlib
no_fs=os.environ.get('NO_FILESYSTEM')=='1'
make_media=media_fixture.make_media
def media(*args):
 result=make_media(*args)
 if no_fs:
  path=args[1]
  with path.open('r+b') as fd: fd.write(bytes(512))
  result['sha256']=hashlib.sha256(path.read_bytes()).hexdigest()
  result['injected_fault']='zeroed filesystem boot sector in disposable QEMU media'
 return result
media_fixture.make_media=media
original=f.exercise
def exercise(r,m,b,o,mode,report):
 original(r,m,b,o,'none',report)
 p=f.Probe(r,m,b,o);p.refresh();p.idle()
 if empty:
  blocks=m.command('query-block');report['qemu_block_devices']=blocks
  m.command('eject',id=blocks[0]['qdev'],force=True)
 report['usb_fixture']=p.prepare_usb(report['usb_power_path_fixture'])
 r.resume_for(2);p.idle()
 p.call('DxeCore','CoreConnectController',int(report['usb_fixture']['handle'],16),0,0,1)
 r.resume_for(2);p.idle()
 p.framebuffer('menu')
 # Device Check follows the stock Reset action.
 for _ in range(12):p.key('down')
 p.framebuffer('selected')
 p.key('ret',wait_for_idle=False);r.resume_for(2);p.refresh();p.framebuffer('app')
 assert 'T70DeviceCheck' in p.images, 'EFI app did not launch from menu'
 def log():
  n=p.read64(p.address('T70DeviceCheck','mDeviceCheckLogChars'))
  assert 0<n<32768
  return r.memory(p.address('T70DeviceCheck','mDeviceCheckLog'),n*2).decode('utf-16le')
 text=log();(o/'device-check.txt').write_text(text)
 if mode=='keyboard': assert 'SUMMARY mass-interfaces=0 usb-blocks=0 usb-filesystems=0' in text,text
 else:
  assert 'mass-interfaces=1' in text,text
  if empty: assert 'usb-blocks=1 usb-filesystems=0' in text and 'present=0' in text,text
  elif no_fs: assert 'usb-blocks=1 usb-filesystems=0' in text and 'LBA0 read=Success signature55AA=0' in text,text
  else: assert 'usb-filesystems=1' in text and 'BOOTAA64.EFI open: Success' in text and 'LBA0 read=Success' in text,text
 before=p.framebuffer('app-idle-before');p.dirty_pages();r.resume_for(1)
 assert p.dirty_pages()==0,'idle app redraw'
 assert p.framebuffer('app-idle-after')==before
 if mode=='hub' and not empty and not no_fs:
  m.command('device_del',id='storage');r.resume_for(3)
  removed=log();(o/'device-check-auto-removed.txt').write_text(removed)
  assert 'SUMMARY mass-interfaces=0 usb-blocks=0 usb-filesystems=0' in removed,removed
  m.command('device_add',driver='usb-storage',id='storage_live',drive='stick',bus='usb-bus.0',port='1.1')
  r.resume_for(4)
  attached=log();(o/'device-check-auto-attached.txt').write_text(attached)
  assert 'usb-filesystems=1' in attached and 'BOOTAA64.EFI open: Success' in attached,attached
  report['automatic_hotplug_refresh_without_keypress']=True
 p.key('r',wait_for_idle=False);r.resume_for(1);assert 'SUMMARY' in log()
 p.key('c',wait_for_idle=False);r.resume_for(1);assert 'SUMMARY' in log()
 if empty:
  m.command('blockdev-insert-medium',id=blocks[0]['qdev'],**{'node-name':'stick'})
  m.command('blockdev-close-tray',id=blocks[0]['qdev'])
  p.key('c',wait_for_idle=False);r.resume_for(1)
  restored=log();(o/'device-check-reinserted.txt').write_text(restored)
  assert 'usb-filesystems=1' in restored and 'BOOTAA64.EFI open: Success' in restored,restored
  report['empty_reader_reinsert_and_reconnect']=True
 p.key('esc');p.framebuffer('returned')
 # The form redraw clears the independently rendered yellow USB status row.
 # Compare the form and its key legend above that row.
 end_of_form=1516*1408*4
 assert (o/'returned.raw').read_bytes()[:end_of_form]==(o/'selected.raw').read_bytes()[:end_of_form], 'ESC did not restore the menu'
 # Reopen proves ESC returned to the front-page rather than just hiding text.
 p.key('ret',wait_for_idle=False);r.resume_for(2);p.refresh();assert 'SUMMARY' in log()
 p.key('esc')
 report['device_check']={'embedded_menu_launch':True,'storage_report':True,'empty_reader':empty,'invalid_filesystem':no_fs,'refresh':True,'reconnect':True,'ESC_return_and_reopen':True,'idle_redraw_pages':0,'returned_form_and_legend_pixels_match':True}
f.exercise=exercise
runpy.run_path(str(q/'verify-qemu.py'),run_name='__main__')
