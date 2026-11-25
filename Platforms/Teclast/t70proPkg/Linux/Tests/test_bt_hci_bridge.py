#!/usr/bin/env python3
"""Execute the real H4 bridge against Linux PTYs and disposable STP sockets."""
from pathlib import Path
import hashlib, json, os, platform, re, subprocess, sys

k = Path(__file__).resolve().parents[2]
repo = k.parents[2]
project = repo.parent
if '--locked' not in sys.argv:
    os.execv(repo / 'Resources/Scripts/with-build-lock', [
        str(repo / 'Resources/Scripts/with-build-lock'), 'T70Pro',
        'Execute ARM64 Bluetooth H4 PTY bridge', '--', sys.executable, __file__, '--locked'])
if sys.platform != 'linux' or platform.machine() != 'aarch64':
    raise SystemExit('Run on the ARM64 Linux builder')
source = k / 'Linux/BtHciBridge.c'
header = k / 'Linux/WmtReady.h'
test = k / 'Linux/Tests/BtHciBridgeTest.c'
out = project / 'analysis/sd-radio-20261117'
out.mkdir(exist_ok=True, parents=True)
binary = out / 'bt-hci-test'
subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', str(test), '-o', str(binary)], check=True)
run = subprocess.run([str(binary)], check=True, text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, timeout=25)
checks = int(re.fullmatch(r'BT H4 PTY bridge: (\d+) checks passed\n', run.stdout)[1])
release = repo / 'Build/t70proPkg/Linux/teclast-bt-hci'
subprocess.run(['gcc', '-static', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wl,--build-id=none', str(source), '-o', str(release)], check=True)
version = subprocess.check_output([str(release), '--version'], text=True, timeout=5).strip()
bad = subprocess.run([str(release), '--invalid'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
assert bad.returncode == 64
report = dict(status='passed', checks=checks, execution='actual Linux ARM64 PTY and SOCK_SEQPACKET',
    sanitizers=['address', 'undefined'], kernel_HCI_attach=False, tablet_execution=False,
    radio_execution=False, cases=['H4 command/ACL/SCO/ISO framing', 'fragmented commands',
    'coalesced commands preserve one STP frame per write', 'fragmented event stream',
    'controller hardware-error event', 'backpressure recovery', 'bounded stalled transfer',
    'partial-frame deadline', 'invalid H4 type', 'transport disconnect', 'SIGTERM shutdown'],
    source_sha256={str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in [source, header, test, Path(__file__)]},
    artifact=dict(path=str(release), sha256=hashlib.sha256(release.read_bytes()).hexdigest(),
                  bytes=release.stat().st_size, version=version))
(project / 'analysis/linux-bt-hci-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
