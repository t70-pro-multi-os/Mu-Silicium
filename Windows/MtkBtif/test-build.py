#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build target selection/error-path checks; no linker, driver or hardware IO."""
from pathlib import Path
import importlib.util
import os
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
inf = (here / "MtkBtif.inf").read_text()
driver = (here / "Driver.c").read_text()
build_source = (here / "build.py").read_text()
assert "shutil.copyfile(here/\"MtkBtif.inf\", stage/\"MtkBtif.inf\")" in build_source
assert "ACPI" + chr(92) + "HANA6795" in inf
assert "KmdfLibraryVersion=1.33" in inf and "MtkBtif.sys=1" in inf
assert "MS_BTHX_BTHMINI" in driver and "WdfFdoAddStaticChild" in driver
spec = importlib.util.spec_from_file_location('btif_build', here/'build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)
wmt_source=build.verify_t70_wmt_function(here.parents[1])
assert wmt_source['enum']=='WMTDRV_TYPE_BT' and wmt_source['value']==0
libs = [Path('wdk')/n for n in ('wdfdriverentry.lib', 'wdfldr.lib', 'ntoskrnl.lib', 'hal.lib', 'bufferoverflowfastfailk.lib')]
for linker in (['lld-link'], ['/existing/zig', 'lld-link']):
    objects = [Path(name) for name in ('Driver.obj', 'MtkBtifCore.obj', 'MtkStpCore.obj')]
    command = build.link_command(linker, objects, Path('Owner.sys'), Path('Owner.map'), libs)
    assert command[:len(linker)] == linker
    for option in ('/machine:arm64', '/entry:FxDriverEntry', '/driver:wdm', '/release', '/tsaware:no', '/nodefaultlib'):
        assert option in command
    assert command[-5:] == list(map(str, libs))
    assert command[-8:-5] == list(map(str, objects)) and '/entry:DriverEntry' not in command
    assert not any('.inf' in item.lower() for item in command)
with tempfile.TemporaryDirectory(prefix='btif-build-test-') as tmp:
    tmp = Path(tmp)
    out = tmp/'artifacts'
    env = dict(os.environ, PATH=str(tmp))
    env.pop('T70_WINDOWS_ZIG', None)
    cases = [([], 'required'),
             (['--output-dir', str(out)], '--prototype'),
             (['--prototype', '--output-dir', str(out)], 'existing lld-link'),
             (['--prototype', '--output-dir', str(out), '--install'], 'unrecognized arguments')]
    for args, message in cases:
        result = subprocess.run([sys.executable, str(here/'build.py'), *args], env=env, text=True, capture_output=True)
        assert result.returncode == 2 and message in result.stderr, result
        assert not out.exists()
    out.mkdir()
    sentinel = out/'keep'; sentinel.write_text('unchanged')
    result = subprocess.run([sys.executable, str(here/'build.py'), '--prototype', '--output-dir', str(out)],
                            env=env, text=True, capture_output=True)
    assert result.returncode == 2 and 'must not already exist' in result.stderr
    assert sentinel.read_text() == 'unchanged' and list(out.iterdir()) == [sentinel]
print("BTIF build checks passed; T70 WMT enum {}={} confirmed at {}".format(wmt_source["enum"], wmt_source["value"], wmt_source["commit"]))
