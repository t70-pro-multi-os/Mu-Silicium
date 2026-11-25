"""Boot the embedded EFI diagnostic against a QEMU SDHC card, including hotplug."""
from pathlib import Path
import re, sys, runpy

q = Path(__file__).resolve().parents[1] / 'Qemu'
sys.path.insert(0, str(q))
import firmware_probe as f

original = f.exercise
def exercise(r, m, b, o, mode, report):
    assert mode == 'sd', 'this test requires --sd or --sdhc'
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
    text = log('sd-device-check.txt')
    for required in ('SD/MMC host protocols: 1', 'media-path=Success', 'bus=SD',
                     'SD SUMMARY blocks=1 filesystems=1', 'LBA0 read=Success',
                     'BOOTAA64.EFI open: Success'):
        assert required in text, (required, text)
    before = p.framebuffer('app-before'); p.dirty_pages(); r.resume_for(1)
    assert p.dirty_pages() == 0, 'idle diagnostic rewrites framebuffer'
    assert p.framebuffer('app-after') == before
    p.key('m'); p.framebuffer('ram-completed')
    ram = log('ram-device-check.txt')
    allocation = re.search(r'RAM allocation: base=([0-9a-fA-F]+) bytes=16777216 pages=4096', ram)
    assert allocation, ram
    base = int(allocation.group(1), 16)
    assert base != 0 and base % 4096 == 0, hex(base)
    for required in ('RAM patterns: 6, rounds=1, passes=12 (6 writes + 6 verifies)',
                     'RAM progress: 25165824/25165824 word operations',
                     'RAM outcome: PASS; 16 MiB allocation only; not whole-RAM certification',
                     'RAM page release: Success'):
        assert required in ram, (required, ram)
    report['device_check_ram'] = dict(allocated_bytes=16777216, base=hex(base),
        patterns=6, rounds=1, word_operations=25165824, mismatch_count=0,
        page_release=True, all_installed_ram=False, app_return_after_completion=True,
        mid_test_ESC=False)
    m.command('blockdev-remove-medium', id='sd'); r.resume_for(3)
    removed = log('sd-device-check-removed.txt')
    assert 'SD SUMMARY blocks=0 filesystems=0' in removed, removed
    m.command('blockdev-insert-medium', id='sd', **{'node-name': 'sdmedium'}); r.resume_for(4)
    restored = log('sd-device-check-reinserted.txt')
    assert 'SD SUMMARY blocks=1 filesystems=1' in restored, restored
    assert 'BOOTAA64.EFI open: Success' in restored, restored
    p.key('r'); assert 'SD SUMMARY blocks=1 filesystems=1' in log('sd-device-check-refresh.txt')
    p.key('c'); assert 'SD SUMMARY blocks=1 filesystems=1' in log('sd-device-check-reconnect.txt')
    p.key('esc'); p.framebuffer('returned')
    assert p.ui_hash == selected, 'ESC failed to restore the menu'
    report['device_check_sd'] = dict(embedded_menu_launch=True, sd_host_slot=True,
        block_and_filesystem=True, boot_file_open=True, automatic_remove_reinsert=True,
        refresh=True, reconnect=True, ESC_return=True, idle_dirty_pages=0)
f.exercise = exercise
runpy.run_path(str(q / 'verify-qemu.py'), run_name='__main__')
