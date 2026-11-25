# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Build and package the VT-less DRM console from the single builder checkout."""
import hashlib,json,re,shlex,struct,subprocess,sys
from pathlib import Path

PKG=Path(__file__).resolve().parent.parent
REPO=PKG.parents[2]
PROJECT=REPO.parent
SOURCE=PROJECT/'kmscon'
BUILD=REPO/'Build/t70proPkg/LinuxConsole'
COMMIT='9ce1dc0dc639e54e243a21d93e13f502a746f0a8'
OPTIONS=[
    '--prefix=/usr','--libdir=lib','--libexecdir=libexec','--sysconfdir=/etc',
    '-Dbuildtype=release','-Dearly_userspace=true','-Dmulti_seat=disabled',
    '-Dvideo_fbdev=disabled','-Dvideo_drm2d=enabled','-Dvideo_drm3d=disabled',
    '-Drenderer_gltex=disabled','-Dfont_unifont=enabled','-Dfont_pango=disabled',
    '-Dsession_terminal=enabled','-Dsession_dummy=disabled','-Ddocs=disabled',
    '-Dtests=true','--wrap-mode=nofallback',
]
CONFIG=b"""# Exact-stock VT-less diagnostic console; no display timing override.
drm
no-hwaccel
use-original-mode
gpus=all
font-engine=unifont
font-size=32
xkb-keymap=/etc/kmscon/us.xkb
no-mouse
dpms-timeout=0
session-max=1
sb-size=1000
no-issue
term=linux
"""
def digest(data):
    return hashlib.sha256(data).hexdigest()

def arm64_elf(data):
    if (len(data)<64 or data[:7]!=b'\x7fELF\x02\x01\x01' or
            struct.unpack_from('<HHI',data,16) not in [(2,183,1),(3,183,1)] or
            struct.unpack_from('<H',data,52)[0]!=64):
        raise ValueError('Console runtime is not an ARM64 ELF')
    off=struct.unpack_from('<Q',data,32)[0]
    size,count=struct.unpack_from('<HH',data,54)
    if size!=56 or not count or off+size*count>len(data):
        raise ValueError('Console ELF program header bounds')
    interpreter=None
    for i in range(count):
        h=struct.unpack_from('<IIQQQQQQ',data,off+i*size)
        if h[2]+h[5]>len(data) or (h[0]==1 and h[5]>h[6]):
            raise ValueError('Console ELF segment bounds')
        if h[0]==3:
            if interpreter is not None or h[5]<2 or h[5]>256 or h[2]+h[5]>len(data):
                raise ValueError('Console ELF interpreter bounds')
            raw=data[h[2]:h[2]+h[5]]
            if raw[-1:]!=b'\0' or b'\0' in raw[:-1]:
                raise ValueError('Console ELF interpreter string')
            interpreter=raw[:-1].decode('ascii')
            if interpreter!='/lib/ld-linux-aarch64.so.1':
                raise ValueError('Unexpected console ELF interpreter')
    return interpreter

def runtime_entries(roots,sysroot=Path("/")):
    """Resolve only DT_NEEDED libraries; no ldd execution and no host root copy."""
    entries={};records=[];pending=list(roots)
    directories=[sysroot/'lib/aarch64-linux-gnu',sysroot/'usr/lib/aarch64-linux-gnu']
    while pending:
        name,path=pending.pop(0)
        data=path.read_bytes()
        if name in entries:
            if entries[name][2]!=data:raise ValueError('Conflicting console runtime '+name)
            continue
        interpreter=arm64_elf(data)
        entries[name]=(name,0o100755,data)
        records.append({'path':name,'source':str(path.resolve()),'bytes':len(data),'sha256':digest(data)})
        command=(['xcrun','llvm-objdump','--private-headers'] if sys.platform=='darwin' else
                 ['readelf','--wide','--dynamic'])
        text=subprocess.check_output(command+[str(path)],text=True)
        needed_names=(re.findall(r'^\s*NEEDED\s+(\S+)\s*$',text,re.M) if sys.platform=='darwin' else
                      re.findall(r'\(NEEDED\).*?\[([^\]]+)\]',text))
        for needed in needed_names:
            if not re.fullmatch(r'[A-Za-z0-9_.+-]+',needed):
                raise ValueError('Invalid ELF dependency')
            candidates=[p/needed for p in directories if (p/needed).is_file()]
            if not candidates:raise ValueError('Missing console ELF dependency '+needed)
            if len({digest(p.read_bytes()) for p in candidates})!=1:
                raise ValueError('Ambiguous console ELF dependency '+needed)
            pending.append(('lib/aarch64-linux-gnu/'+needed,candidates[0]))
        if interpreter:pending.append((interpreter[1:],sysroot/interpreter[1:]))
    return list(entries.values()),records

def source_hashes():
    names=subprocess.check_output(['git','-C',str(SOURCE),'ls-files','-z']).decode().split('\0')
    return {'kmscon/'+name:digest((SOURCE/name).read_bytes()) for name in names if name}

def verify_software():
    flags=shlex.split(subprocess.check_output(
        ['pkg-config','--cflags','--libs','xkbcommon','libtsm','libudev','libdrm','zlib'],text=True))
    libraries=[BUILD/'src'/name for name in ['libuterm.a','libeloop.a','libshl.a']]
    libraries.append(BUILD/'external/libext-htable.a')
    common=['gcc','-D_GNU_SOURCE','-std=gnu11','-O1','-g','-Wall','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        '-I'+str(SOURCE/'src'),'-I'+str(SOURCE/'external'),'-I'+str(BUILD),'-include',str(BUILD/'config.h')]
    font_build=BUILD/'src/mod-unifont.so.p'
    sources=[SOURCE/'src'/name for name in ['pty.c','font.c','font_rotate.c','font_8x16.c','font_unifont.c']]
    sources.append(font_build/'font_unifont_data.bin.c')
    results=[]
    for name,extra,args in [
        ('ConsoleTest',sources+[Path('-I'+str(font_build))],[SOURCE/'src/uterm_input_fallback.xkb']),
        ('ConsoleMonitorTest',[],[]),
    ]:
        output=BUILD/name
        command=common+[str(PKG/'Linux/Tests'/f'{name}.c')]+list(map(str,extra))+[
            '-Wl,--start-group',*map(str,libraries),'-Wl,--end-group',*flags,'-ldl','-lpthread','-o',str(output)]
        subprocess.run(command,check=True)
        result=subprocess.run([str(output),*map(str,args)],check=True,capture_output=True,text=True,timeout=30)
        if 'checks PASS' not in result.stdout:raise ValueError('Missing console test result')
        results.append({'test':name,'output':result.stdout.strip(),'binary_sha256':digest(output.read_bytes())})
    report={'status':'passed','scope':'Actual kmscon XKB, VT, Unifont, PTY and modified monitor code; builder only',
        'cases':results,'device_execution':False,'drm_or_evdev_open':False,
        'source_sha256':{str(path.relative_to(REPO)):digest(path.read_bytes()) for path in [
            Path(__file__),PKG/'Linux/Tests/ConsoleTest.c',PKG/'Linux/Tests/ConsoleMonitorTest.c']},
        'kmscon_source_sha256':source_hashes(),
        'limitations':['No tablet boot or kernel module execution','Production static libraries and system shared libraries are not sanitizer-instrumented']}
    (PROJECT/'analysis/linux-console-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

def prepare():
    if subprocess.check_output(['git','-C',str(SOURCE),'rev-parse','HEAD'],text=True).strip()!=COMMIT:
        raise ValueError('kmscon checkout differs from the pinned console version')
    before=source_hashes()
    command=['meson','setup']
    if (BUILD/'build.ninja').exists():command.append('--reconfigure')
    command += [str(BUILD),str(SOURCE),*OPTIONS]
    configured=subprocess.run(command,check=True,capture_output=True,text=True)
    compiled=subprocess.run(['ninja','-C',str(BUILD),'-j6'],check=True,capture_output=True,text=True)
    tested=subprocess.run(['meson','test','-C',str(BUILD),'--no-rebuild','--print-errorlogs'],
        check=True,capture_output=True,text=True,timeout=60)
    software=verify_software()
    if source_hashes()!=before:raise ValueError('Console sources changed during the build')
    return package(before,software,{
        'upstream_tests':tested.stdout.strip(),'build_tail':compiled.stdout.splitlines()[-5:],
        'configure_tail':configured.stdout.splitlines()[-6:],
        'build_host':'Linux ARM64','native_linux_build':True,'native_linux_tests':True})

def package(before,software,build,sysroot=Path('/')):
    entries,runtime=runtime_entries([
        ('usr/libexec/kmscon/kmscon',BUILD/'src/kmscon'),
        ('usr/lib/kmscon/mod-unifont.so',BUILD/'src/mod-unifont.so'),
    ],sysroot)
    entries += [('etc/kmscon/kmscon.conf',0o100644,CONFIG),
        ('etc/kmscon/us.xkb',0o100644,(SOURCE/'src/uterm_input_fallback.xkb').read_bytes()),
        ('sbin/teclast-shell',0o100755,(PKG/'Linux/console-shell.sh').read_bytes()),
        ('usr/share/licenses/teclast-console/kmscon-COPYING',0o100644,(SOURCE/'COPYING').read_bytes())]
    # Retain the license notices of the runtime libraries actually shipped.
    for package in ['libc6','libdrm2','libtsm4','libudev1','libxkbcommon0','zlib1g']:
        path=sysroot/'usr/share/doc'/package/'copyright'
        if path.is_file():
            entries.append(('usr/share/licenses/teclast-console/'+package,0o100644,path.read_bytes()))
    report={'status':'built; no tablet execution','upstream':'https://github.com/kmscon/kmscon',
        'version':'9.3.2','commit':COMMIT,'source_sha256':before,'options':OPTIONS,
        'local_changes':['opt-in raw kernel uevents for mdev','matching device-node readiness before add',
            'one-second rescan after delayed node creation'],
        'runtime':runtime,'configuration_sha256':digest(CONFIG),
        'package_files':[{'path':n,'mode':oct(m),'bytes':len(d),'sha256':digest(d)} for n,m,d in entries],
        'software_tests':software,**build,
        'activation':'teclast.console=drm; opt-in diagnostic EFI entry; none by default',
        'input':'evdev USB HID keyboard, XKB modifiers/repeat, PTY foreground shell',
        'display':'DRM/KMS CPU dumb buffers; original/preferred kernel panel mode; GNU Unifont UTF-8/wide glyphs',
        'gpu_acceleration':False,'device_execution':False,
        'limitations':['No tablet boot, display or keyboard execution',
            'No Linux desktop or Hangul input method',
            'No display/USB power initialization proof or suspend/wake proof']}
    return entries,report

def prepare_cross(sysroot):
    """Package the existing cross build only while every recorded input matches."""
    proof=json.loads((PROJECT/'analysis/linux-console-build.json').read_text())
    cross=proof['cross_build']
    before=source_hashes()
    if (cross.get('target')!='aarch64-linux-gnu' or cross.get('options')!=OPTIONS or
            cross.get('pinned_checkout_verified')!=COMMIT or
            subprocess.check_output(['git','-C',str(SOURCE),'rev-parse','HEAD'],text=True).strip()!=COMMIT or
            cross['source_sha256']!=before):
        raise ValueError('Cross console build source/version/options proof is stale')
    for name,sha in cross['configuration'].items():
        if digest((REPO/name).read_bytes())!=sha:
            raise ValueError('Cross console configuration changed: '+name)
    artifacts={item['path']:item for item in cross['artifacts']}
    for name in ['src/kmscon','src/mod-unifont.so']:
        path=BUILD/name;item=artifacts[str(path.relative_to(REPO))]
        data=path.read_bytes()
        if len(data)!=item['bytes'] or digest(data)!=item['sha256']:
            raise ValueError('Cross console binary changed: '+name)
    software={'status':'pending','execution_host':None,'native_linux_execution':False,
        'required':['XKB/repeat/fake VT/PTY/kernel monitor tests','isolated dynamic-loader execution'],
        'device_execution':False}
    entries,report=package(before,software,{
        'status':'cross-built and packaged; native Linux execution pending',
        'build_host':cross['build_host'],'native_linux_build':False,'native_linux_tests':False,
        'compiler':cross['compiler'],'build_configuration_sha256':cross['configuration'],
        'compiled_artifacts':[artifacts[str((BUILD/name).relative_to(REPO))]
                              for name in ['src/kmscon','src/mod-unifont.so']]},sysroot)
    expected={item['path']:(item['bytes'],item['sha256']) for item in
              cross['offline_dynamic_link_check']['runtime_files']}
    actual={item['path']:(item['bytes'],item['sha256']) for item in report['runtime']}
    if actual!=expected:raise ValueError('Cross console runtime libraries changed since link checks')
    if source_hashes()!=before:raise ValueError('Console sources changed during packaging')
    return entries,report
