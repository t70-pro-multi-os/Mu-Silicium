#!/usr/bin/env python3
"""Stage ARM64 Wi-Fi/BlueZ services without installing packages on the builder."""
import collections
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
K = Path(__file__).resolve().parents[1]
REPO = K.parents[2]
PROJECT = REPO.parent
DEPS = PROJECT / 'tools/linux-radio-deps'
REPORT = PROJECT / 'analysis/linux-radio-runtime.json'

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value)
    return value

def sha(data):
    return hashlib.sha256(data).hexdigest()

def prepare():
    if sys.platform != 'linux':
        raise ValueError('Prepare radio services on the ARM64 Linux builder')
    desktop = module('radio_package_stage', K/'Linux/desktop.py')
    elf = module('radio_elf_check', K/'Linux/console.py')
    desktop.DEPS = DEPS
    desktop.PACKAGES = ['wpasupplicant', 'bluez', 'dbus-daemon']
    packages = desktop.stage_dependencies()
    plan = json.loads((DEPS/'packages.json').read_text())
    def find(relative):
        for base in [DEPS, Path('/')]:
            path = base/relative
            if path.is_file(): return path
        raise ValueError('Missing packaged radio runtime: '+relative)
    bluez = [DEPS/n for row in packages.values() for n in row['files']
             if n.endswith('/bluetoothd')]
    if not bluez:
        bluez = [Path('/usr/libexec/bluetooth/bluetoothd')]
    if len(bluez) != 1 or not bluez[0].is_file():
        raise ValueError('No unique BlueZ daemon')
    roots = [('sbin/wpa_supplicant', find('usr/sbin/wpa_supplicant')),
             ('sbin/wpa_cli', find('usr/sbin/wpa_cli')),
             ('sbin/bluetoothd', bluez[0]),
             ('bin/bluetoothctl', find('usr/bin/bluetoothctl')),
             ('sbin/dbus-daemon', find('usr/bin/dbus-daemon')),
             ('bin/dbus-send', find('usr/bin/dbus-send')),
             ('bin/dbus-uuidgen', find('usr/bin/dbus-uuidgen'))]
    entries, records = {}, []
    pending = collections.deque(roots)
    while pending:
        name, path = pending.popleft(); data = path.read_bytes()
        if name in entries:
            if entries[name][2] != data: raise ValueError('Conflicting radio runtime: '+name)
            continue
        interpreter = elf.arm64_elf(data)
        dynamic = subprocess.check_output(['readelf', '--wide', '--dynamic', str(path)], text=True)
        if re.search(r'\((?:RPATH|RUNPATH)\)', dynamic):
            raise ValueError('Radio runtime unexpectedly requires an embedded search path')
        needed = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic)
        entries[name] = (name, 0o100755, data)
        records.append(dict(path=name, source=str(path.resolve()), bytes=len(data), sha256=sha(data), needed=needed))
        for library in needed:
            if not re.fullmatch(r'[A-Za-z0-9_.+-]+', library): raise ValueError('Invalid ELF dependency')
            choices = [base/part/library for base in [DEPS, Path('/')]
                       for part in ['usr/lib/aarch64-linux-gnu', 'lib/aarch64-linux-gnu']]
            source = next((p for p in choices if p.is_file()), None)
            if source is None: raise ValueError('Missing radio library: '+library)
            pending.append(('usr/lib/aarch64-linux-gnu/'+library, source))
        if interpreter: pending.append((interpreter[1:], find(interpreter[1:])))
    config = b"""<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=/run/dbus/system_bus_socket</listen>
  <auth>EXTERNAL</auth>
  <policy context="default"><allow user="0"/><deny own="*"/><deny send_type="method_call"/></policy>
  <policy user="0"><allow own="org.bluez"/><allow send_destination="*"/><allow receive_sender="*"/></policy>
</busconfig>
"""
    entries['etc/teclast/dbus-system.conf'] = ('etc/teclast/dbus-system.conf', 0o100644, config)
    entries['etc/bluetooth/main.conf'] = ('etc/bluetooth/main.conf', 0o100644,
        b'[General]\nName=T70Pro\n[Policy]\nAutoEnable=false\n')
    for name in ['etc/machine-id', 'var/lib/dbus/machine-id']:
        entries[name] = (name, 0o120777, b'/run/dbus/machine-id')
    # Retain the original licensing notices for the packages supplying this runtime.
    names = set(plan['packages']) | set(plan['reused_builder_packages'])
    for name in sorted(names):
        relative = 'usr/share/doc/'+name+'/copyright'
        try: path = find(relative)
        except ValueError: continue
        entries[relative] = (relative, 0o100644, path.read_bytes())
    with tempfile.TemporaryDirectory(prefix='t70-radio-runtime-') as temporary:
        stage = Path(temporary)
        for name, mode, data in entries.values():
            target = stage/name; target.parent.mkdir(parents=True, exist_ok=True)
            if mode & 0o170000 == 0o120000: continue
            target.write_bytes(data); target.chmod(mode & 0o777)
        loader = stage/'lib/ld-linux-aarch64.so.1'
        libraries = str(stage/'usr/lib/aarch64-linux-gnu')
        def command(name, *arguments):
            return [str(loader), '--library-path', libraries, str(stage/name), *arguments]
        links = {}
        for name, _ in roots:
            listing = subprocess.check_output([str(loader), '--library-path', libraries, '--list', str(stage/name)],
                                               text=True, stderr=subprocess.STDOUT, timeout=10)
            if 'not found' in listing: raise ValueError('Unresolved radio runtime dependency')
            resolved = re.findall(r'=>\s+(/[^\s]+)', listing)
            if any(not Path(p).is_relative_to(stage) for p in resolved):
                raise ValueError('Radio binary resolved a library outside the packaged runtime')
            links[name] = dict(status='passed', libraries=len(resolved))
        versions = {}
        for name, option in [('sbin/wpa_supplicant','-v'),('sbin/wpa_cli','-v'),
                             ('sbin/bluetoothd','--version'),('bin/bluetoothctl','--version'),
                             ('sbin/dbus-daemon','--version')]:
            value = subprocess.check_output(command(name, option), text=True, stderr=subprocess.STDOUT, timeout=10)
            versions[name] = value.splitlines()[0]
        helptext = subprocess.run(command('sbin/wpa_supplicant', '-h'), text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10).stdout
        if 'nl80211' not in helptext: raise ValueError('Wi-Fi service lacks nl80211 backend')
        control = stage/'wpa-global'
        daemon = subprocess.Popen(command('sbin/wpa_supplicant', '-g', str(control)),
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic()+5
            while not control.is_socket() and daemon.poll() is None and time.monotonic()<deadline: time.sleep(.02)
            if not control.is_socket(): raise ValueError('WPA global control socket did not start')
            answer = subprocess.check_output(command('sbin/wpa_cli', '-g', str(control), 'ping'), text=True, timeout=5)
            if answer.strip() != 'PONG': raise ValueError('WPA control round trip failed')
        finally:
            if daemon.poll() is None: daemon.send_signal(signal.SIGTERM)
            daemon.communicate(timeout=5)
        if daemon.returncode: raise ValueError('WPA service did not shut down cleanly')
        bus = stage/'private-bus'
        busconfig = stage/'test-bus.conf'
        busconfig.write_bytes(config.replace(b'/run/dbus/system_bus_socket', str(bus).encode()))
        daemon = subprocess.Popen(command('sbin/dbus-daemon', '--nofork', '--nopidfile', '--nosyslog',
                                          '--config-file='+str(busconfig)),
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.monotonic()+5
            while not bus.is_socket() and daemon.poll() is None and time.monotonic()<deadline: time.sleep(.02)
            if not bus.is_socket(): raise ValueError('D-Bus config or private listener failed')
        finally:
            if daemon.poll() is None: daemon.send_signal(signal.SIGTERM)
            daemon.communicate(timeout=5)
        if daemon.returncode: raise ValueError('D-Bus service did not shut down cleanly')
    report = dict(status='passed', source_sha256=sha(Path(__file__).read_bytes()),
                  package_plan_sha256=sha((DEPS/'packages.json').read_bytes()), versions=versions,
                  dynamic_link_checks=links, wpa_private_control='PONG; SIGTERM exit 0', dbus_private_listener='started; SIGTERM exit 0',
                  network_interfaces_attached=False, kernel_radio_execution=False, tablet_execution=False,
                  files=records, members={n:dict(mode=mode,bytes=len(data),sha256=sha(data)) for n,mode,data in entries.values()})
    REPORT.write_text(json.dumps(report, indent=2)+'\n')
    return list(entries.values()), report

if __name__ == '__main__':
    if '--locked' not in sys.argv:
        os.execv(REPO/'Resources/Scripts/with-build-lock', [str(REPO/'Resources/Scripts/with-build-lock'),
                 'T70Pro','Stage and execute Wi-Fi / BlueZ runtime','--',sys.executable,__file__,'--locked'])
    entries, report = prepare()
    print(json.dumps(dict(status=report['status'],files=len(entries),versions=report['versions'],wpa_private_control=report['wpa_private_control'])))
