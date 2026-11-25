#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute the real desktop shell flow with explicit device/OS command fixtures."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import unittest

PKG = Path(__file__).resolve().parents[2]
REPO = PKG.parents[2]
PROJECT = REPO.parent
SCRIPT = PKG/'Linux/desktop-start.sh'


@contextlib.contextmanager
def fixture(mode='normal', renderer='vulkan', card=True, mali=True, existing=False):
    with tempfile.TemporaryDirectory(prefix='desktop-start-', dir=REPO/'Build') as directory:
        root = Path(directory)
        run = root/'run'
        dri = root/'dri'; dri.mkdir()
        if card: (dri/'card0').touch()
        if mali: (root/'mali0').touch()
        (root/'startup').write_text('bindings-incomplete\n')
        (root/'mode').write_text(mode)
        if existing:
            run.mkdir(); (run/'state').write_text('previous-run\n')
        command = root/'fixture.py'
        command.write_text(r"""import json,os,signal,socket,subprocess,sys,time
from pathlib import Path
root=Path(__file__).parent
role=sys.argv[1];args=sys.argv[2:];mode=(root/'mode').read_text()
with (root/'calls').open('a') as f:
 f.write(json.dumps({'role':role,'args':args,'seat':os.getenv('LIBSEAT_BACKEND'),
                    'vt':os.getenv('SEATD_VTBOUND'),'icd':os.getenv('VK_DRIVER_FILES')})+'\n')
if role=='udev':
 if args[0]=='control' and mode=='pingfail':sys.exit(1)
 if args[0]=='trigger' and mode=='triggerfail':sys.exit(1)
 if args[0]=='settle' and mode=='settlefail':sys.exit(1)
 sys.exit(0)
if role=='font':sys.exit(1 if mode=='fontfail' else 0)
if mode=='early':sys.exit(7)
if mode == 'hold-grandchild':
 grandchild=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])
 (root/'grandchild-pid').write_text(str(grandchild.pid))
def stop(*unused):
 (root/'child-stopped').write_text('TERM\n');sys.exit(0)
signal.signal(signal.SIGTERM,stop)
(root/'child-pid').write_text(str(os.getpid()))
if mode!='nosocket':
 os.chdir(os.environ['XDG_RUNTIME_DIR']);s=socket.socket(socket.AF_UNIX);s.bind('wayland-0');s.listen(1)
if mode in ['hold','nosocket','hold-grandchild']:
 while True:time.sleep(0.01)
time.sleep(0.08)
sys.exit(9 if mode=='exit9' else 0)
""")
        session = root/'setsid'
        session.write_text('\n'.join([
            '#!'+sys.executable,
            'import os,sys',
            'os.setsid()',
            'os.execvp(sys.argv[1],sys.argv[1:])',
            '',
        ]))
        session.chmod(0o755)
        programs = {}
        for role in ['udev','weston','font']:
            path = root/role
            path.write_text('#!/bin/sh\nexec '+shlex.quote(sys.executable)+' '+shlex.quote(str(command))+' '+role+' "$@"\n')
            path.chmod(0o755); programs[role] = path
        timer = root/'timeout'
        timer.write_text('#!/bin/sh\n[ "$1" = -s ] && [ "$2" = TERM ] && [ "$3" = -k ] || exit 99\nshift 5\nexec "$@"\n')
        timer.chmod(0o755)
        sleeper = root/'sleep'; sleeper.write_text('#!/bin/sh\nexec /bin/sleep 0.001\n'); sleeper.chmod(0o755)
        text = SCRIPT.read_text()
        mapping = {'RUN':run,'STARTUP':root/'startup','DRI':dri,'MALI':root/'mali0',
                   'UDEVADM':programs['udev'],'WESTON':programs['weston'],'FC_CACHE':programs['font'],
                   'SETSID':session,
                   'TIMEOUT':timer,'SLEEP':sleeper}
        for name, path in mapping.items():
            import re
            text, count = re.subn('^'+name+'=.*$',name+'='+shlex.quote(str(path)),text,count=1,flags=re.M)
            assert count == 1
        # Only the device-file type boundary changes: no real device node is used.
        assert 'node() { [ -c "$1" ]; }' in text
        text = text.replace('node() { [ -c "$1" ]; }','node() { [ -f "$1" ]; }')
        script = root/'start'; script.write_text(text)
        subprocess.run(['/bin/sh','-n',str(script)],check=True)
        args=[] if renderer is None else [renderer]
        process = subprocess.Popen(['/bin/sh',str(script),*args],stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                   text=True,start_new_session=True)
        try:
            yield root,run,process
        finally:
            child_file = root/'child-pid'
            if child_file.is_file():
                try:
                    os.killpg(int(child_file.read_text()),signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if process.poll() is None:
                os.killpg(process.pid,signal.SIGKILL)
            process.communicate(timeout=3)


def calls(root):
    return [json.loads(line) for line in (root/'calls').read_text().splitlines()] if (root/'calls').is_file() else []


class DesktopStartup(unittest.TestCase):
    def result(self, mode='normal', renderer='vulkan', **options):
        with fixture(mode,renderer,**options) as (root,run,p):
            out,err=p.communicate(timeout=8)
            return p.returncode,(run/'state').read_text().strip() if (run/'state').exists() else None,calls(root),err

    def test_actual_vulkan_flow_coldplugs_before_desktop_and_uses_vtless_seat(self):
        code,state,events,err=self.result()
        self.assertEqual(code,0,err);self.assertEqual(state,'exited:0')
        self.assertEqual([e['role'] for e in events],['udev']*4+['font','weston'])
        self.assertEqual([e['args'] for e in events[:4]],[['control','--ping','--timeout=1'],
            ['trigger','--type=subsystems','--action=add'],['trigger','--type=devices','--action=add'],['settle','--timeout=20']])
        desktop=events[-1]
        self.assertIn('--renderer=vulkan',desktop['args']);self.assertIn('--backend=drm',desktop['args'])
        self.assertEqual((desktop['seat'],desktop['vt']),('builtin','0'))
        self.assertEqual(desktop['icd'],'/usr/share/vulkan/icd.d/teclast-panvk.json')

    def test_explicit_pixman_does_not_require_mali(self):
        code,state,events,err=self.result(renderer='pixman',mali=False)
        self.assertEqual((code,state),(0,'exited:0'),err)
        self.assertIn('--renderer=pixman',events[-1]['args'])

    def test_vulkan_missing_gpu_never_falls_back_to_cpu(self):
        code,state,events,_=self.result(mali=False)
        self.assertEqual((code,state),(1,'required-device-missing'))
        self.assertNotIn('weston',[e['role'] for e in events])

    def test_missing_display_after_module_finish_is_reported(self):
        code,state,events,_=self.result(card=False)
        self.assertEqual((code,state),(1,'required-device-missing'))
        self.assertNotIn('weston',[e['role'] for e in events])

    def test_invalid_or_missing_renderer_never_creates_a_run(self):
        for renderer in [None,'auto','vulkan;echo bad','$(bad)','', 'VULKAN']:
            code,state,events,_=self.result(renderer=renderer)
            self.assertEqual((code,state,events),(64,None,[]))

    def test_previous_run_is_not_replaced(self):
        code,state,events,_=self.result(existing=True)
        self.assertEqual((code,state,events),(73,'previous-run',[]))

    def test_unavailable_udev_has_bounded_attempts_and_no_desktop(self):
        code,state,events,_=self.result('pingfail')
        self.assertEqual((code,state),(1,'udev-unavailable'))
        self.assertEqual(len(events),10)
        self.assertTrue(all(e['role']=='udev' and e['args'][0]=='control' for e in events))

    def test_failed_coldplug_never_launches_desktop(self):
        for mode in ['triggerfail','settlefail']:
            code,state,events,_=self.result(mode)
            self.assertEqual((code,state),(1,'coldplug-failed'))
            self.assertNotIn('weston',[e['role'] for e in events])

    def test_font_cache_failure_is_distinct(self):
        code,state,events,_=self.result('fontfail')
        self.assertEqual((code,state),(1,'font-cache-failed'))
        self.assertNotIn('weston',[e['role'] for e in events])

    def test_child_exit_before_socket_does_not_claim_readiness(self):
        code,state,events,_=self.result('early')
        self.assertEqual((code,state),(1,'exited-before-socket:7'))
        self.assertEqual(sum(e['role']=='weston' for e in events),1)

    def test_child_exit_after_socket_keeps_exit_code_without_restart(self):
        code,state,events,_=self.result('exit9')
        self.assertEqual((code,state),(9,'exited:9'))
        self.assertEqual(sum(e['role']=='weston' for e in events),1)

    def test_missing_socket_times_out_and_reaps_owned_process(self):
        with fixture('nosocket') as (root,run,p):
            p.communicate(timeout=8)
            self.assertEqual((p.returncode,(run/'state').read_text().strip()),(1,'compositor-timeout'))
            self.assertTrue((root/'child-stopped').is_file())
            child=int((root/'child-pid').read_text())
            with self.assertRaises(ProcessLookupError):os.kill(child,0)

    def test_sigterm_reaps_the_compositor_process_group(self):
        with fixture('hold-grandchild') as (root,run,p):
            until=time.monotonic()+5
            while not (run/'state').exists() or (run/'state').read_text().strip()!='socket-created':
                if p.poll() is not None:
                    _,error=p.communicate()
                    self.fail('desktop exited before creating its socket: '+error)
                self.assertLess(time.monotonic(),until)
                time.sleep(0.005)
            parent=int((root/'child-pid').read_text())
            child=int((root/'grandchild-pid').read_text())
            p.send_signal(signal.SIGTERM);p.communicate(timeout=5)
            self.assertEqual((p.returncode,(run/'state').read_text().strip()),(143,'interrupted'))
            self.assertTrue((root/'child-stopped').is_file())
            for pid in (parent,child):
                with self.subTest(pid=pid),self.assertRaises(ProcessLookupError):os.kill(pid,0)

    def test_sigterm_stops_and_reaps_desktop_after_real_unix_socket(self):
        with fixture('hold') as (root,run,p):
            until=time.monotonic()+5
            while not (run/'state').exists() or (run/'state').read_text().strip()!='socket-created':
                self.assertIsNone(p.poll())
                self.assertLess(time.monotonic(),until)
                time.sleep(0.005)
            child=int((root/'child-pid').read_text())
            p.send_signal(signal.SIGTERM);p.communicate(timeout=5)
            self.assertEqual((p.returncode,(run/'state').read_text().strip()),(143,'interrupted'))
            self.assertTrue((root/'child-stopped').is_file())
            with self.assertRaises(ProcessLookupError):os.kill(child,0)

    def test_actual_pid1_selection_functions_under_sanitizers(self):
        text=(PKG/'Linux/Init.c').read_text()
        source=text[text.index('#define CMDLINE_LIMIT'):text.index('static int ReadSelection(')]
        body=r"""
int main(void) {
 char out[257],line[CMDLINE_LIMIT+1];
 const char *valid[]={"none","drm","desktop-vulkan","desktop-pixman"};
 for(unsigned i=0;i<4;i++) {
  snprintf(line,sizeof(line),"quiet teclast.console=%s loglevel=7",valid[i]);
  assert(SelectConsole(line,strlen(line),out,sizeof(out))==0 && !strcmp(out,valid[i]));
 }
 const char *bad[]={"", "auto", "desktop", "desktop-vulkan,pixman", "desktop-vulkan teclast.console=none", "$(bad)", "DESKTOP-VULKAN"};
 for(unsigned i=0;i<7;i++) {
  snprintf(line,sizeof(line),"teclast.console=%s",bad[i]);
  assert(SelectConsole(line,strlen(line),out,sizeof(out))==-1);
 }
 assert(!SelectStartup("teclast.modules=usb,input,display,storage",strlen("teclast.modules=usb,input,display,storage"),out,sizeof(out)));
 assert(!strcmp(out,"usb,input,display,storage"));
 assert(SelectConsole("a\0b",3,out,sizeof(out))==-1);
 memset(line,'a',sizeof(line));assert(SelectConsole(line,sizeof(line),out,sizeof(out))==-1);
 puts("PID1 actual selection functions PASS");
 return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix='desktop-parser-',dir=REPO/'Build') as directory:
            exe=Path(directory)/'parser'
            subprocess.run(['clang' if sys.platform=='darwin' else 'gcc','-x','c','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-o',str(exe),'-'],
                input=('#include <assert.h>\n#include <stdio.h>\n#include <string.h>\n'+source+body).encode(),check=True)
            subprocess.run([str(exe)],check=True,timeout=5,capture_output=True)


def main():
    if '--locked' not in sys.argv:
        os.execv(REPO/'Resources/Scripts/with-build-lock',[
            str(REPO/'Resources/Scripts/with-build-lock'),'T70Pro','Test real desktop startup lifecycle with explicit OS fixtures','--',
            sys.executable,__file__,'--locked'])
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DesktopStartup))
    paths=[Path(__file__),SCRIPT,PKG/'Linux/Init.c']
    report={'status':'passed' if result.wasSuccessful() else 'failed','tests':result.testsRun,
        'failures':len(result.failures),'errors':len(result.errors),
        'scope':'Actual production shell flow executed by native POSIX shell and real subprocess/AF_UNIX/signal/reaping behavior; udev, Weston, font cache, device-file type and timeout/sleep commands are explicit fixtures. Actual PID1 selection functions executed under ASan/UBSan.',
        'source_sha256':{str(p.relative_to(REPO)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
        'temporary_fixtures_removed':True,'Linux_runtime_executed':False,'actual_udev_or_compositor_executed':False,
        'device_execution':False,'host_mount_or_network_changed':False}
    (PROJECT/'analysis/linux-desktop-startup-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':sys.exit(main())
