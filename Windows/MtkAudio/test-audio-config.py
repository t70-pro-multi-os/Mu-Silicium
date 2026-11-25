#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test only the portable audio config code; no firmware build or hardware I/O."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('stock_acf', type=Path)
parser.add_argument('--linux-system-sanitizers', action='store_true',
                    help='link existing system libasan/libubsan with Clang instrumentation')
args = parser.parse_args()
if args.linux_system_sanitizers and sys.platform != 'linux':
    parser.error('--linux-system-sanitizers requires Linux')


def compile_test(command, **kwargs):
    if args.linux_system_sanitizers and '-c' not in command:
        # Keep the sanitizer runtimes first in ELF DT_NEEDED.
        command = command[:1] + ['-fno-sanitize-link-runtime', '-lasan', '-lubsan'] + command[1:]
    return subprocess.run(command, **kwargs)



fixture = args.stock_acf.resolve()
expected = '4bb530416e53d20bdb4e42fd63a77e04c7d1aa26ced0c24404752bf967385bd5'
if hashlib.sha256(fixture.read_bytes()).hexdigest() != expected:
    parser.error('fixture is not the independently identified T70 stock ACF')
root = Path(__file__).resolve().parent
# Static reference-name check, not Microsoft's INFVerif or installation.
inf = (root/'MtkAudio.inf').read_text()
assert inf.count('AddInterface=') == 4
# Target ABI remains 1.35; the official range-build macro supplies runtime floor.
assert 'KmdfLibraryVersion=1.35' in inf
checker = (root/'check-wdk-components.py').read_text()
assert '/DKMDF_VERSION_MINOR=35' in checker
assert '/DKMDF_MINIMUM_VERSION_REQUIRED=33' in checker
# Core PortCls/KS registration belongs to the model's actual DDInstall.
install = inf.split('[Install.NT]', 1)[1].split('[', 1)[0]
entries = dict(line.lower().replace(' ', '').split('=', 1) for line in install.splitlines()
               if '=' in line and not line.lstrip().startswith(';'))
assert set(entries['include'].split(',')) == {'ks.inf', 'wdmaudio.inf'}
assert set(entries['needs'].split(',')) == {'ks.registration', 'wdmaudio.registration'}
for name, source in (('T70Wave', 'WaveCyclicPortOwner.cpp'), ('T70Topology', 'AudioEndpoint.cpp')):
    assert ','.join(repr(c) for c in name) in (root/source).read_text()
    assert inf.count(','+name+',') == 2
assert 'StartDevice,2,' in (root/'AudioDriver.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='t70-audio-config-') as tmp:
    for name, sources in (
        ('T70TopologyMiniportTest', ('T70TopologyMiniport', 'T70TopologyMiniportTest')),
        ('AudioServiceSinkOwnerTest', ('AudioServiceSinkOwner', 'AudioServiceSinkOwnerTest')),
        ('AudioEndpointTest', ('T70TopologyMiniport', 'AudioEndpoint', 'AudioEndpointTest'))):
        exe = Path(tmp)/name
        compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I'+str(root/'Tests/include'),
                        *[str(root/(x+'.cpp')) for x in sources], '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    # Actual topology property dispatch -> KS adapter; only SPB resources and
    # profile fixtures are mocked. C ABI is compiled separately from C++.
    topology_objects = []
    for name in ('AwKsVolume', 'AwVolume'):
        obj = Path(tmp)/(name+'-topology.o')
        compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I'+str(root/'Tests/spb-resources'), '-I'+str(root/'Tests/include'),
                        '-c', str(root/(name+'.c')), '-o', str(obj)], check=True)
        topology_objects.append(str(obj))
    exe = Path(tmp)/'T70TopologyVolumeTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/spb-resources'), '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('T70TopologyMiniport',
                      'T70TopologyVolume','AudioEndpointVolume','T70TopologyVolumeTest')],
                    *topology_objects, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveCyclicBufferTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveCyclicBuffer.cpp'),
                    str(root/'WaveCyclicBufferTest.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveCyclicMiniportTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveCyclicMiniport.cpp'),
                    str(root/'WaveCyclicMiniportTest.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveCyclicPortOwnerTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveCyclicMiniport',
                      'WaveCyclicPortOwner','WaveCyclicPortOwnerTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveCyclicServiceTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveCyclicService.cpp'),
                    str(root/'WaveCyclicServiceTest.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'AfeDl1InterruptTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveCyclicService',
                      'AfeDl1Interrupt','AfeDl1InterruptTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'AfeInterruptOwnerTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveCyclicService',
                      'AfeDl1Interrupt','AfeInterruptOwner','AfeIrqSource','AfeInterruptOwnerTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'AfeIrqSourceTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'AfeIrqSource.cpp'),
                    str(root/'AfeIrqSourceTest.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'AfeIrqRecoveryTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveCyclicService',
                      'AfeDl1Interrupt','AfeInterruptOwner','AfeIrqSource',
                      'AfeIrqRecovery','AfeIrqRecoveryTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    board_objects = []
    for name in ('AfeMapping','AfeMappedIo'):
        obj = Path(tmp)/('board-'+name+'.o')
        compile_test(['clang','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(root/'Tests/include'),'-c',str(root/(name+'.c')),
                        '-o',str(obj)],check=True)
        board_objects.append(str(obj))
    exe = Path(tmp)/'WaveCyclicBoardTest'
    compile_test(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveCyclicService',
                      'AfeDl1Interrupt','AfeInterruptOwner','AfeIrqSource',
                      'AfeIrqRecovery','WaveCyclicBoard','WaveCyclicBoardTest')],
                    *board_objects,'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    config_objects = []
    for name in ('WaveRtFormat', 'WavePrepare', 'AfePrepare', 'AfeFormat', 'AfeGeometry',
                 'ConfigureMapped', 'AfeMapping', 'AfeConfigure', 'AfeAddress', 'AfeFormatUpdate'):
        obj = Path(tmp)/('config-'+name+'.o')
        compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I'+str(root/'Tests/include'), '-c', str(root/(name+'.c')),
                        '-o', str(obj)], check=True)
        config_objects.append(str(obj))
    start_objects=[]
    for name in ('AfeStartRequest','AfeIrqCount','AfeCur32','AfePosition','AfeStopRequest','AfeDisableRequest','AfePrefetch'):
        obj=Path(tmp)/(name+'.o')
        compile_test(['clang','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        '-I'+str(root/'Tests/include'),'-c',str(root/(name+'.c')),
                        '-o',str(obj)],check=True)
        start_objects.append(str(obj))
    exe = Path(tmp)/'WaveCyclicStreamTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveCyclicBuffer',
                      'WaveCyclicStream','WaveCyclicFactory','WaveCyclicService','WaveCyclicMiniport','AudioServiceSinkOwner','AudioExportOwner','WaveCyclicStreamTest')], *config_objects[:5], *config_objects[7:], *start_objects,
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveRtStreamConfigureTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveRtStreamBase',
                      'WaveRtStreamConfigure', 'WaveRtDmaOwner', 'WaveRtMdlOwner',
                      'WaveRtStreamConfigureTest')], *config_objects, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveRtStreamTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveRtStream','WaveRtComStreamBase',
                      'WaveRtBufferedStreamBase','WaveRtStreamBase','WaveRtStreamConfigure',
                      'WaveRtDmaOwner','WaveRtMdlOwner','WaveRtMiniportBoundary','WaveRtStreamTest')],
                    *config_objects, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveRtComStreamBaseTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveRtComStreamBase',
                      'WaveRtBufferedStreamBase','WaveRtStreamBase','WaveRtStreamConfigure',
                      'WaveRtDmaOwner','WaveRtMdlOwner','WaveRtComStreamBaseTest')],
                    *config_objects, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveRtBufferedStreamBaseTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('WaveRtBufferedStreamBase',
                      'WaveRtStreamBase', 'WaveRtStreamConfigure', 'WaveRtDmaOwner',
                      'WaveRtMdlOwner', 'WaveRtBufferedStreamBaseTest')],
                    *config_objects, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveRtDmaOwnerTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveRtDmaOwner.cpp'),
                    str(root/'WaveRtMdlOwner.cpp'), str(root/'WaveRtDmaOwnerTest.cpp'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp)/'WaveRtMdlOwnerTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveRtMdlOwner.cpp'),
                    str(root/'WaveRtMdlOwnerTest.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    objects = []
    for name in ('WaveRtFormat', 'WavePrepare', 'AfePrepare', 'AfeFormat', 'AfeGeometry'):
        obj = Path(tmp)/(name+'.o')
        compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I'+str(root/'Tests/include'), '-c', str(root/(name+'.c')),
                        '-o', str(obj)], check=True)
        objects.append(str(obj))
    exe = Path(tmp)/'WaveRtStreamBaseTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveRtStreamBase.cpp'),
                    str(root/'WaveRtStreamBaseTest.cpp'), *objects, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp) / 'WaveRtFormatTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('WaveRtFormat', 'WavePrepare',
                      'AfePrepare', 'AfeFormat', 'AfeGeometry', 'WaveRtFormatTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp) / 'AudioResourceOwnerTest'
    compile_test(['clang++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('AudioResourceOwner',
                      'AfeResourceMapping','WaveRtMiniport','WaveRtIntersection',
                      'WaveRtMiniportBoundary','WaveRtMdlOwner','WaveRtDmaOwner','AudioResourceOwnerTest')],
                    str(Path(tmp)/'config-AfeMapping.o'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'AudioExportOwnerTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    str(root/'AudioServiceSinkOwner.cpp'),str(root/'AudioExportOwner.cpp'),str(root/'AudioExportOwnerTest.cpp'),
                    '-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

    exe = Path(tmp) / 'AudioDomainOwnerTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    str(root/'AudioDomainOwner.cpp'),str(root/'AudioDomainOwnerTest.cpp'),
                    '-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

    exe = Path(tmp) / 'AudioDriverTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('AudioDomainOwner',
                      'AudioServiceSinkOwner','AudioExportOwner','AudioCyclicStart','AudioDriverTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'AudioDeviceStartTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('AudioDeviceStart',
                      'AudioResourceOwner','AfeResourceMapping','DmaAdapterOwner',
                      'WaveRtPortBind','WaveRtMiniport','WaveRtIntersection',
                      'WaveRtMiniportBoundary','WaveRtMdlOwner','WaveRtDmaOwner','AudioDeviceStartTest')],
                    str(Path(tmp)/'config-AfeMapping.o'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'DmaAdapterOwnerTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.cpp')) for name in ('DmaAdapterOwner',
                      'WaveRtMiniport','WaveRtIntersection','WaveRtMiniportBoundary','WaveRtMdlOwner','WaveRtDmaOwner','DmaAdapterOwnerTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'AfeResourceMappingTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'AfeResourceMapping.cpp'),
                    str(root/'WaveRtMiniportBoundary.cpp'), str(root/'AfeResourceMappingTest.cpp'),
                    str(Path(tmp)/'config-AfeMapping.o'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'WaveRtMiniportTest'
    compile_test(['clang++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveRtMiniport.cpp'), str(root/'WaveRtIntersection.cpp'),
                    str(root/'WaveRtMiniportBoundary.cpp'), str(root/'WaveRtMiniportTest.cpp'),
                    *objects, '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'WaveRtMiniportBoundaryTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveRtMiniportBoundary.cpp'),
                    str(root/'WaveRtMiniportBoundaryTest.cpp'), str(root/'WaveRtPortBind.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'WaveRtPortBindTest'
    compile_test(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'WaveRtPortBind.cpp'),
                    str(root/'WaveRtPortBindTest.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'PortClsOwnerTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'PortClsOwner.c'),
                    str(root/'PortClsOwnerTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    exe = Path(tmp) / 'RouteObserveTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeRouteObserve.c'), str(root/'RouteObserveTest.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    for test in ('AcfTest', 'RegisterTest', 'BoardTest', 'PreflightTest', 'ProfileTest', 'PairProfileTest'):
        exe = Path(tmp) / test
        compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        str(root/'AwAcf.c'), str(root/'AwRegisters.c'), str(root/'AwBoard.c'), str(root/'AwPreflight.c'), str(root/'AwProfile.c'), str(root/'AwPairProfile.c'),
                        str(root/(test+'.c')), '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(fixture)], check=True)

    exe = Path(tmp) / 'SpbTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'), str(root/'AwSpb.c'), str(root/'AwMask.c'), str(root/'AwIdentity.c'),
                    str(root/'AwVolume.c'), str(root/'AwVolumeUpdate.c'),
                    str(root/'AwTxField.c'), str(root/'AwTxUpdate.c'), str(root/'AwStatus.c'), str(root/'AwProbe.c'),
                    str(root/'SpbTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'MaskTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwMask.c'), str(root/'MaskTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'IdentityTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwIdentity.c'), str(root/'IdentityTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'VolumeTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwVolume.c'), str(root/'VolumeTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'TxFieldTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwTxField.c'), str(root/'TxFieldTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'VolumeUpdateTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwVolume.c'), str(root/'AwVolumeUpdate.c'),
                    str(root/'VolumeUpdateTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'TxUpdateTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwTxField.c'), str(root/'AwTxUpdate.c'),
                    str(root/'TxUpdateTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'StatusTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwStatus.c'), str(root/'StatusTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'ProbeTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwIdentity.c'), str(root/'AwStatus.c'), str(root/'AwProbe.c'),
                    str(root/'ProbeTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'PairProbeTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AwIdentity.c'), str(root/'AwStatus.c'), str(root/'AwProbe.c'),
                    str(root/'AwPairProbe.c'), str(root/'PairProbeTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'GeometryTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeGeometry.c'), str(root/'GeometryTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'FormatTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeFormat.c'), str(root/'FormatTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'PrepareTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeFormat.c'), str(root/'AfeGeometry.c'),
                    str(root/'AfePrepare.c'), str(root/'PrepareTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'PositionTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeFormat.c'), str(root/'AfeGeometry.c'), str(root/'AfePrepare.c'),
                    str(root/'AfePosition.c'), str(root/'PositionTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'IrqCountTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeFormat.c'), str(root/'AfeGeometry.c'), str(root/'AfePrepare.c'),
                    str(root/'AfeIrqCount.c'), str(root/'IrqCountTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'AddressTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeFormat.c'), str(root/'AfeGeometry.c'), str(root/'AfePrepare.c'),
                    str(root/'AfeAddress.c'), str(root/'AddressTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'FormatUpdateTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeFormat.c'), str(root/'AfeFormatUpdate.c'),
                    str(root/'FormatUpdateTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'ConfigureTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(root/'AfeFormat.c'), str(root/'AfeGeometry.c'), str(root/'AfePrepare.c'),
                    str(root/'AfeAddress.c'), str(root/'AfeFormatUpdate.c'),
                    str(root/'AfeConfigure.c'), str(root/'ConfigureTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'PairInspectTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AwAcf','AwRegisters','AwBoard',
                      'AwPreflight','AwProfile','AwPairProfile','AwIdentity','AwStatus',
                      'AwProbe','AwPairProbe','AwPairInspect','PairInspectTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(fixture)], check=True)

    exe = Path(tmp) / 'IrqFieldsTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry',
                      'AfePrepare','AfeIrqCount','AfeIrqFields','IrqFieldsTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'AudioInspectTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry','AfePrepare',
                      'AwAcf','AwRegisters','AwBoard','AwPreflight','AwProfile','AwPairProfile',
                      'AwIdentity','AwStatus','AwProbe','AwPairProbe','AwPairInspect',
                      'AudioInspect','AudioInspectTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(fixture)], check=True)

    exe = Path(tmp) / 'AudioRouteInspectTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry','AfePrepare',
                      'AwAcf','AwRegisters','AwBoard','AwPreflight','AwProfile','AwPairProfile',
                      'AwIdentity','AwStatus','AwProbe','AwPairProbe','AwPairInspect',
                      'AudioInspect','AfeRouteObserve','AudioRouteInspect',
                      'AudioRouteInspectTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(fixture)], check=True)

    exe = Path(tmp) / 'PcmRingCopyTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry',
                      'AfePrepare','PcmRingCopy','PcmRingCopyTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'MappedIoTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeMappedIo','AfeFormat',
                      'AfeGeometry','AfePrepare','AfeAddress','AfeFormatUpdate',
                      'AfeConfigure','MappedIoTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'MappingTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeMapping','AfeMappedIo','AfeFormat','AfeGeometry',
                      'AfePrepare','AfeAddress','AfeFormatUpdate','AfeConfigure',
                      'ConfigureMapped','AfeRouteObserve','RouteMapped','MappingTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'ConfigureMappedTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeMapping','AfeFormat',
                      'AfeGeometry','AfePrepare','AfeAddress','AfeFormatUpdate',
                      'AfeConfigure','ConfigureMapped','ConfigureMappedTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'RouteMappedTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeMapping','AfeRouteObserve',
                      'RouteMapped','RouteMappedTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'WavePrepareTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry',
                      'AfePrepare','WavePrepare','WavePrepareTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'Wave24ConvertTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in
                      ('Wave24Convert','Wave24ConvertTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'Wave24RingTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry',
                      'AfePrepare','WavePrepare','Wave24Convert','PcmRingCopy',
                      'Wave24Ring','Wave24RingTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'WaveRingTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry',
                      'AfePrepare','WavePrepare','Wave24Convert','PcmRingCopy',
                      'Wave24Ring','WaveRing','WaveRingTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'FormatMappedObserveTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeMapping','AfeFormat',
                      'AfeGeometry','AfePrepare','FormatMappedObserve',
                      'FormatMappedObserveTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'AddressMappedObserveTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeMapping','AfeFormat',
                      'AfeGeometry','AfePrepare','AddressMappedObserve',
                      'AddressMappedObserveTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'SerialDecodeTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in
                      ('AwAcf','AwRegisters','AwSerialDecode','SerialDecodeTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(fixture)], check=True)

    exe = Path(tmp) / 'PairSerialTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in ('AwAcf','AwRegisters',
                      'AwBoard','AwPreflight','AwProfile','AwPairProfile',
                      'AwSerialDecode','AwPairSerial','PairSerialTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(fixture)], check=True)

    exe = Path(tmp) / 'SpbProfileVolumeTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AwAcf','AwRegisters',
                      'AwBoard','AwPreflight','AwProfile','AwSpb','AwMask','AwIdentity',
                      'AwVolume','AwVolumeUpdate','AwTxField','AwTxUpdate','AwStatus',
                      'AwSpbProfileVolume','AwPairProfile','AwSerialDecode','AwPairSerial',
                      'AwSpbProfileSerial','SpbProfileVolumeTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(fixture)], check=True)

    exe = Path(tmp) / 'SpbPairTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AwAcf','AwRegisters',
                      'AwBoard','AwIdentity','AwStatus','AwProbe','AwPairProbe',
                      'AwSpbPair','SpbPairTest')], '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'SpbTargetTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AwAcf','AwRegisters',
                      'AwBoard','AwSpbTarget','SpbTargetTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'SpbResourcesTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/spb-resources'),
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AwAcf','AwRegisters',
                      'AwBoard','AwSpbTarget','AwSpbResources','SpbResourcesTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'AwKsVolumeTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/spb-resources'),
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AwAcf','AwRegisters',
                      'AwBoard','AwVolume','AwVolumeUpdate','AwSpbProfileVolume',
                      'AwSpbTarget','AwSpbResources','AwKsVolume','AwKsVolumeTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'CommonBufferTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeFormat','AfeGeometry',
                      'AfePrepare','AfeCommonBuffer','CommonBufferTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'CommonBufferConfigureTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeCommonBuffer',
                      'AfeFormat','AfeGeometry','AfePrepare','AfeMapping',
                      'AfeAddress','AfeFormatUpdate','AfeConfigure','ConfigureMapped',
                      'CommonBufferConfigure','CommonBufferConfigureTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'DisableRequestTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *[str(root/(name+'.c')) for name in
                      ('AfeDisableRequest','DisableRequestTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

    exe = Path(tmp) / 'CommonBufferFillTest'
    compile_test(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I'+str(root/'Tests/include'),
                    *[str(root/(name+'.c')) for name in ('AfeCommonBuffer',
                      'AfeFormat','AfeGeometry','AfePrepare','WavePrepare',
                      'Wave24Convert','PcmRingCopy','Wave24Ring','WaveRing',
                      'CommonBufferFill','CommonBufferFillTest')],
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
