"""Execute the embedded EFI inspector with real UFS and USB keyboard protocols."""
from pathlib import Path
import sys, runpy, re

q = Path(__file__).resolve().parents[1] / 'Qemu'
sys.path.insert(0, str(q))
import firmware_probe as f

original = f.exercise
def exercise(r, m, b, o, mode, report):
    assert mode == 'ufs', 'this test requires --ufs --usb keyboard'
    original(r, m, b, o, 'none', report)
    p = f.Probe(r, m, b, o); p.refresh(); p.idle()
    from ufs_fixture import unit_descriptor_probe
    report['ufs_unit_descriptors']=unit_descriptor_probe(p)
    user=report['ufs_unit_descriptors'][2]
    assert user['enable']==2 and (1<<user['block_shift'])*user['blocks']==127959826432,user
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
        text = r.memory(p.address('T70DeviceCheck', 'mDeviceCheckLog'), count * 2).decode('utf-16le').replace('\r\n', '\n')
        (o / name).write_text(text)
        return text
    units = 3 if report.get('ufs_boot_lus') else 1
    def check(text, connecting=True):
        for required in ('T70 Device Check v3 - auto SD/UFS; RAM test on M',
                         'UFS host controllers: 1', f'UFS SCSI units: {units}', 'bus=UFS',
                         'UFS unit LU2 enable=02 boot=00',
                         f'UFS SUMMARY hosts=1 units={units} blocks=1 filesystems=1',
                         'UFS child H', 'LU=2', 'P0=1',
                         f'UFS bind diagnostics records={units} dropped=0',
                         'UFS bind first LU=2 start=Success/ready',
                         'supported=Success/supported', 'start=Success/ready',
                         'C10=Success', 'install=Success',
                         'LBA0 read=Success', 'BOOTAA64.EFI open: Success'):
            assert required in text, (required, text)
        if report.get('ufs_boot_lus'):
            for required in ('UFS unit LU0 enable=01 boot=01',
                             'UFS unit LU1 enable=01 boot=02'):
                assert required in text, (required, text)
        children = re.findall(r'^UFS child H[^\n]* C=([^\n]+)$', text, re.MULTILINE)
        assert len(children) == units, (children, text)
        # A recursive host connect may already bind children before this call.
        expected = ('Success', 'Not Found') if connecting else ('Not started',)
        assert all(status in expected for status in children), (children, text)
        assert 'UFS bind issue' not in text and 'UFS bind reject' not in text, text
    initial_log = log('ufs-device-check.txt')
    check(initial_log)
    assert 'UFS host connect H' in initial_log and ': Success' in initial_log, 'internal UFS was not connected at app entry'
    assert 'UFS child H' in initial_log, 'UFS child connection was not inspected at app entry'
    before = p.framebuffer('app-before'); p.dirty_pages(); r.resume_for(1)
    assert p.dirty_pages() == 0, 'idle diagnostic rewrites framebuffer'
    assert p.framebuffer('app-after') == before
    p.key('r'); refreshed = log('ufs-device-check-refresh.txt'); check(refreshed, connecting=False)
    assert 'UFS host connect H' not in refreshed and 'C=Not' in refreshed, \
        'R refresh unexpectedly retried storage connection'
    p.key('c'); check(log('ufs-device-check-reconnect.txt'))
    p.key('esc'); p.framebuffer('returned')
    assert p.ui_hash == selected, 'ESC failed to restore the menu'
    p.key('ret'); p.refresh(); check(log('ufs-device-check-reopened.txt'))
    p.key('esc')
    report['device_check_ufs'] = dict(embedded_menu_launch=True, host_scsi_block_filesystem=True,
        boot_file_open=True, refresh=True, reconnect=True, ESC_return_and_reopen=True,
        idle_dirty_pages=0, usb_keyboard=True)
f.exercise = exercise
runpy.run_path(str(q / 'verify-qemu.py'), run_name='__main__')
