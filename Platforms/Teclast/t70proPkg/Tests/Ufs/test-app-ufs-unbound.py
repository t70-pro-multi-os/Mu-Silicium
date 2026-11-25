"""Check first-entry auto-connect and failure reporting when UFS has no logical unit."""
from pathlib import Path
import sys, runpy

q = Path(__file__).resolve().parents[1] / 'Qemu'
sys.path.insert(0, str(q))
import firmware_probe as f

original = f.exercise
def exercise(r, m, b, o, mode, report):
    assert mode == 'ufs-no-lu', 'this test requires --ufs-no-lu --usb keyboard'
    original(r, m, b, o, 'none', report)
    p = f.Probe(r, m, b, o); p.refresh(); p.idle()
    report['usb_fixture'] = p.prepare_usb(report['usb_power_path_fixture'])
    r.resume_for(2); p.idle()
    p.call('DxeCore', 'CoreConnectController', int(report['usb_fixture']['handle'], 16), 0, 0, 1)
    r.resume_for(2); p.idle()
    for _ in range(6): p.key('down')
    p.framebuffer('selected'); selected = p.ui_hash
    p.key('ret'); p.refresh()
    assert 'T70DeviceCheck' in p.images, 'embedded EFI diagnostic did not launch'
    def log(name):
        count = p.read64(p.address('T70DeviceCheck', 'mDeviceCheckLogChars'))
        assert 0 < count < 32768
        text = r.memory(p.address('T70DeviceCheck', 'mDeviceCheckLog'), count * 2).decode('utf-16le')
        (o / name).write_text(text)
        return text
    def check(text):
        for required in ('T70 Device Check v3 - auto SD/UFS; RAM test on M',
                         'UFS host controllers: 1', 'UFS SCSI units: 0',
                         'UFS SUMMARY hosts=1 units=0 blocks=0 filesystems=0',
                         'UFS bind diagnostics records=0 dropped=0'):
            assert required in text, (required, text)
    initial_log = log('ufs-device-check.txt')
    check(initial_log)
    assert 'UFS host connect H' in initial_log and ': Success' in initial_log, 'UFS host auto-connect was not attempted at app entry'
    assert 'UFS child H' not in initial_log, 'an absent UFS child was reported as connected'
    before = p.framebuffer('app-before'); p.dirty_pages(); r.resume_for(1)
    assert p.dirty_pages() == 0, 'idle diagnostic rewrites framebuffer'
    assert p.framebuffer('app-after') == before
    p.key('r'); refreshed = log('ufs-device-check-refresh.txt'); check(refreshed)
    assert 'UFS host connect H' not in refreshed and 'UFS child H' not in refreshed, \
        'R refresh unexpectedly retried storage connection'
    p.key('c'); reconnect = log('ufs-device-check-reconnect.txt'); check(reconnect)
    assert 'UFS host connect H' in reconnect and ': Success' in reconnect, 'C reconnect did not retry the UFS host'
    p.key('esc'); p.framebuffer('returned')
    assert p.ui_hash == selected, 'ESC failed to restore the menu'
    p.key('ret'); p.refresh(); check(log('ufs-device-check-reopened.txt'))
    p.key('esc')
    report['device_check_ufs_unbound'] = dict(embedded_menu_launch=True, host_auto_connect=True,
        zero_scsi_units_reported=True, refresh_no_retry=True, manual_reconnect=True,
        ESC_return_and_reopen=True, idle_dirty_pages=0, usb_keyboard=True)
f.exercise = exercise
runpy.run_path(str(q / 'verify-qemu.py'), run_name='__main__')
