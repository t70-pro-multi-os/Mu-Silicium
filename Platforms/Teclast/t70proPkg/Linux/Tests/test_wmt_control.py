#!/usr/bin/env python3
"""Execute the WMT request loop and ROM catalog on the ARM64 Linux builder."""
from pathlib import Path
import hashlib, json, os, platform, re, subprocess, sys

k = Path(__file__).resolve().parents[2]
repo = k.parents[2]
project = repo.parent
if '--locked' not in sys.argv:
    os.execv(repo / 'Resources/Scripts/with-build-lock', [
        str(repo / 'Resources/Scripts/with-build-lock'), 'T70Pro',
        'Execute ARM64 WMT command and firmware catalog', '--', sys.executable, __file__, '--locked'])
if sys.platform != 'linux' or platform.machine() != 'aarch64':
    raise SystemExit('Run on the ARM64 Linux builder')
source = k / 'Linux/WmtControl.c'
header = k / 'Linux/WmtReady.h'
test = k / 'Linux/Tests/WmtControlTest.c'
out = project / 'analysis/sd-radio-20261117'
out.mkdir(exist_ok=True, parents=True)
firmware = project / 'analysis/stock-binaries/vendor_a_firmware'
binary = out / 'wmt-control-test'
subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', str(test), '-o', str(binary)], check=True)
run = subprocess.run([str(binary), str(firmware)], check=True, text=True, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE, timeout=25)
checks = int(re.fullmatch(r'WMT control: (\d+) checks passed\n', run.stdout)[1])
release = repo / 'Build/t70proPkg/Linux/teclast-wmt-control'
subprocess.run(['gcc', '-static', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wl,--build-id=none', str(source), '-o', str(release)], check=True)
version = subprocess.check_output([str(release), '--version'], text=True, timeout=5).strip()
bad = subprocess.run([str(release), '--invalid'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
assert bad.returncode == 64
report = dict(status='passed', checks=checks, execution='actual Linux ARM64 file IO, poll and SOCK_SEQPACKET; driver ioctls captured by test callback',
    sanitizers=['address', 'undefined'], driver_execution=False, tablet_execution=False,
    radio_execution=False, cases=['cold SoC platform/chip/common-init ioctl order and failures', 'both shipped 6631/6635 ROM header sets',
    'runtime RF chip and revision selection', '264-byte ROM record layout and basename',
    'all records validated before publication', 'missing, short, FIFO and symlink firmware rejected',
    'wrong type, address, revision and unknown chip rejected', 'driver failure and retry',
    'no-NUL request and exact no-NUL reply', 'truncated, unknown and embedded-NUL requests',
    'disconnect and SIGTERM shutdown'],
    source_sha256={str(p.relative_to(repo)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in [source, header, test, Path(__file__)]},
    firmware_sha256={p.name:hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in sorted(firmware.glob('soc2_2_ram_*_1[de]_1_hdr.bin'))},
    artifact=dict(path=str(release), sha256=hashlib.sha256(release.read_bytes()).hexdigest(),
                  bytes=release.stat().st_size, version=version))
assert len(report['firmware_sha256']) == 6
(project / 'analysis/linux-wmt-control-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
