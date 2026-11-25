#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check Mesa's actual JM ABI core against virtual I/O and selected stock code.

No device node, kernel module initialization, MMIO or real ioctl is executed.
The production Linux translation unit is cross-compiled, not linked/installed.
"""
from pathlib import Path
import ctypes as C
import hashlib
import json
import os
import shlex
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

P = Path(__file__).resolve().parent
R = P.parents[1]
PROJECT = R.parent
MESA = PROJECT / 'mesa'
BUILD = R / 'Windows/Build'
HEADER = MESA / 'src/panfrost/lib/kmod/kbase_jm_abi.h'
TEST = HEADER.parent / 'tests/kbase_jm_abi_test.c'
REPORT = PROJECT / 'analysis/panvk-kbase-abi-verification.json'
STOCK_SHA = '3b5a27d70fbe3891f6e0a051899a4cff22bf89a3f8aefebfe1981ae31ef5b4cf'
PINNED = '58f2c55854bd48b18df756edb11f187af9cd2fde'
if '--locked' not in sys.argv:
    lock = R / 'Resources/Scripts/with-build-lock'
    os.execv(lock, [str(lock), 'T70Pro', 'Check stock JM ABI and compile Mesa kbase backend',
                       '--', sys.executable, __file__, '--locked', *sys.argv[1:]])

commands = []
def run(argv, cwd=None):
    argv = [str(x) for x in argv]
    commands.append(argv)
    result = subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        print(result.stdout, flush=True)
        raise RuntimeError('Command failed: ' + shlex.join(argv))
    return result.stdout.strip()

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stock_comparison(library):
    import struct
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    import unicorn.arm64_const as A
    stock = PROJECT / 'analysis/stock-binaries/vendor_dlkm_a/mali_kbase_mt6789_a16w_jm.ko'
    raw = stock.read_bytes()
    assert sha(stock) == STOCK_SHA
    assert raw[:6] == bytes.fromhex('7f454c460201') and struct.unpack_from('<H', raw, 18)[0] == 183
    sht = struct.unpack_from('<Q', raw, 40)[0]
    width, n, names_index = struct.unpack_from('<HHH', raw, 58)
    assert width == 64 and sht + n * width <= len(raw)
    sections = [struct.unpack_from('<IIQQQQIIQQ', raw, sht + i * width) for i in range(n)]
    def data(i):
        s = sections[i]
        assert s[4] + s[5] <= len(raw)
        return raw[s[4]:s[4] + s[5]]
    strings = data(names_index)
    text_index = next(i for i,s in enumerate(sections) if strings[s[0]:].split(b'\0',1)[0] == b'.text')
    code = data(text_index)
    relocs = {}
    for i,s in enumerate(sections):
        if s[1] != 4 or s[7] != text_index:
            continue
        symbols = data(s[6]); symbol_names = data(sections[s[6]][6])
        for k in range(0, len(data(i)), 24):
            off, info, addend = struct.unpack_from('<QQq', data(i), k)
            name_offset = struct.unpack_from('<I', symbols, (info >> 32) * 24)[0]
            relocs[off] = symbol_names[name_offset:].split(b'\0', 1)[0].decode()
    assert relocs[0x29ae8] == '__check_object_size'
    assert relocs[0x284cc] == 'mutex_lock'
    assert relocs[0x284fc] == '__wake_up'

    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, storage, stack, context = 0x100000, 0x2000000, 0x3000000, 0x4000000
    u.mem_map(base, (len(code) + 4095) & ~4095); u.mem_write(base, code)
    for addr, size in ((storage, 0x10000), (stack, 0x10000), (context, 0x40000)):
        u.mem_map(addr, size)
    sp = stack + 0x8000; fp = sp + 0x100; normalized = fp - 0x58
    mode = None; single_pc = None; service_calls = {'copy_from_user': 0, 'object_size': 0, 'mutex': 0, 'wake': 0}
    def hook(uc, address, size, _):
        off = address - base
        if mode == 'normalize':
            if off == 0x29b00:
                uc.emu_stop(); return
            assert 0x29abc <= off < 0x29b00 or 0x2a3d4 <= off < 0x2a3f4
            if off in (0x29af8, 0x2a3e4):
                dst, src, count = [uc.reg_read(reg) for reg in (A.UC_ARM64_REG_X0, A.UC_ARM64_REG_X1, A.UC_ARM64_REG_X2)]
                assert src == storage and count in (64, 72)
                assert dst in (normalized, normalized + 8) and dst + count == normalized + 72
                uc.mem_write(dst, bytes(uc.mem_read(src, count)))
                uc.reg_write(A.UC_ARM64_REG_X0, 0); uc.reg_write(A.UC_ARM64_REG_PC, address + 4)
                service_calls['copy_from_user'] += 1
            elif off == 0x29ae8:
                assert uc.reg_read(A.UC_ARM64_REG_X0) == normalized and uc.reg_read(A.UC_ARM64_REG_X1) == 72
                uc.reg_write(A.UC_ARM64_REG_PC, address + 4); service_calls['object_size'] += 1
        elif mode == 'guard':
            if off in (0x29a3c, 0x2a558, 0x2a508):
                uc.emu_stop(); return
            assert 0x29a0c <= off < 0x29a3c
        elif mode == 'event':
            if off == 0x28508:
                uc.emu_stop(); return
            assert 0x2848c <= off < 0x28508
            if off in (0x284cc, 0x284fc):
                service_calls['mutex' if off == 0x284cc else 'wake'] += 1
                uc.reg_write(A.UC_ARM64_REG_PC, address + 4)
        elif mode == 'cookie':
            if off in (0x19c4, 0x19d0, 0x1a78):
                uc.emu_stop(); return
            assert 0x1984 <= off < 0x19c4
        else:
            assert mode == 'single' and off == single_pc
    u.hook_add(UC_HOOK_CODE, hook)
    def normalize(wire):
        nonlocal mode
        mode = 'normalize'
        u.mem_write(storage, wire)
        u.mem_write(sp, bytes(0x200))
        u.mem_write(sp + 0x78, struct.pack('<Q', len(wire)))
        u.reg_write(A.UC_ARM64_REG_SP, sp); u.reg_write(A.UC_ARM64_REG_X29, fp)
        u.reg_write(A.UC_ARM64_REG_X10, len(wire)); u.reg_write(A.UC_ARM64_REG_X23, storage)
        u.emu_start(base + 0x29abc, base + 0x29b00, count=100)
        assert u.reg_read(A.UC_ARM64_REG_PC) == base + 0x29b00
        return bytes(u.mem_read(normalized, 72))
    def load(pc, registers, pointer=None):
        nonlocal mode, single_pc
        mode = 'single'; single_pc = pc
        u.reg_write(A.UC_ARM64_REG_X29, fp)
        if pointer is not None: u.reg_write(A.UC_ARM64_REG_X10, pointer)
        u.emu_start(base + pc, base + pc + 4, count=1)
        assert u.reg_read(A.UC_ARM64_REG_PC) == base + pc + 4
        return tuple(u.reg_read(reg) for reg in registers)
    def event(atom_number, code, userdata):
        nonlocal mode
        mode = 'event'
        atom = context + 0x9278 + atom_number * 0x228
        kbdev = context + 0x35000
        u.mem_write(context + 8, struct.pack('<Q', kbdev))
        u.mem_write(kbdev + 0x1b98, bytes(4))
        u.mem_write(atom + 0x110, struct.pack('<I', code) + bytes(4))
        u.mem_write(atom + 0x28, struct.pack('<QQ', *userdata))
        u.mem_write(storage + 0x1000, bytes([0xa5]) * 24)
        for reg,val in ((A.UC_ARM64_REG_X20, context), (A.UC_ARM64_REG_X22, atom),
                        (A.UC_ARM64_REG_X21, atom + 0x60), (A.UC_ARM64_REG_X19, storage + 0x1000)):
            u.reg_write(reg, val)
        u.emu_start(base + 0x2848c, base + 0x28508, count=64)
        assert u.reg_read(A.UC_ARM64_REG_PC) == base + 0x28508
        return bytes(u.mem_read(storage + 0x1000, 24))

    class Dep(C.Structure):
        _fields_ = [('atom_id', C.c_uint8), ('dependency_type', C.c_uint8)]
    class Atom(C.Structure):
        _fields_ = [('jc', C.c_uint64), ('udata', C.c_uint64 * 2), ('extres_list', C.c_uint64),
                    ('nr_extres', C.c_uint16), ('jit_id', C.c_uint8 * 2), ('pre_dep', Dep * 2),
                    ('atom_number', C.c_uint8), ('prio', C.c_int8), ('device_nr', C.c_uint8),
                    ('jobslot', C.c_uint8), ('core_req', C.c_uint32)]
    assert C.sizeof(Atom) == 48
    lib = C.CDLL(str(library))
    lib.T70KbasePack.argtypes = [C.c_int, C.POINTER(Atom), C.c_uint, C.c_void_p]
    lib.T70KbaseProbeCompletion.argtypes = [C.c_void_p, C.c_uint]
    lib.T70KbaseProbeCompletion.restype = C.c_int
    pack = C.create_string_buffer(64)
    loads = [('atom_number', 0x29b0c, A.UC_ARM64_REG_X8), ('nr_extres', 0x29b70, A.UC_ARM64_REG_X8),
             ('core_req', 0x29b78, A.UC_ARM64_REG_X9), ('device_nr', 0x29b80, A.UC_ARM64_REG_X8),
             ('jobslot', 0x29b8c, A.UC_ARM64_REG_X8), ('jit0', 0x29bd0, A.UC_ARM64_REG_X8),
             ('jit1', 0x29bdc, A.UC_ARM64_REG_X9), ('dep0_atom', 0x29c18, A.UC_ARM64_REG_X8),
             ('dep0_type', 0x29c20, A.UC_ARM64_REG_X9), ('dep1_atom', 0x29c30, A.UC_ARM64_REG_X9),
             ('dep1_type', 0x29c38, A.UC_ARM64_REG_X9), ('prio', 0x29eac, A.UC_ARM64_REG_X8),
             ('extres_list', 0x29fd8, A.UC_ARM64_REG_X10)]
    cases = fields = guards = events = 0
    for layout in (0, 1):
        for i in range(256):
            a = Atom(); a.jc = 0x1234567800000080 + i * 128
            a.udata[:] = (0xfedcba9800000000 | i, 0xabcdef0000000000 | i)
            a.extres_list = 0x8877665500000000 + i * 16; a.nr_extres = i * 257
            a.jit_id[:] = (i, (i + 1) & 255)
            a.pre_dep[:] = (Dep((i - 1) & 255, 1), Dep((i + 1) & 255, 2))
            a.atom_number = i; a.prio = i if i < 128 else i - 256
            a.device_nr = i ^ 0xa5; a.jobslot = i ^ 0x5a; a.core_req = 0x88770000 | i
            lib.T70KbasePack(layout, C.byref(a), 1, pack)
            wire = pack.raw if layout == 1 else pack.raw + bytes(8)
            actual = normalize(wire)
            assert actual == bytes(8) + bytes(a) + bytes(16)
            values = dict(atom_number=i, nr_extres=a.nr_extres, core_req=a.core_req, device_nr=a.device_nr,
                          jobslot=a.jobslot, jit0=i, jit1=(i+1)&255, dep0_atom=(i-1)&255,
                          dep0_type=1, dep1_atom=(i+1)&255, dep1_type=2, prio=i, extres_list=a.extres_list)
            for name,pc,reg in loads:
                assert load(pc, [reg]) == (values[name],), (name, i, layout)
                fields += 1
            assert load(0x29b98, [A.UC_ARM64_REG_X9, A.UC_ARM64_REG_X8]) == (0, a.jc)
            assert load(0x29b58, [A.UC_ARM64_REG_X9, A.UC_ARM64_REG_X10], normalized) == tuple(a.udata)
            fields += 4; cases += 1
    for stride in list(range(0, 129)) + [0xffffffff, 0x100000040, 0x100000048]:
        for count in (0, 1, 255, 256, 257, 0xffffffff):
            mode = 'guard'
            u.reg_write(A.UC_ARM64_REG_X3, stride & 0xffffffff)
            u.reg_write(A.UC_ARM64_REG_X2, count)
            u.emu_start(base + 0x29a0c, base + len(code), count=32)
            accepted = u.reg_read(A.UC_ARM64_REG_PC) == base + 0x29a3c
            assert accepted == ((stride & 0xffffffff) in (64, 72) and count <= 256)
            guards += 1
    for i in range(256):
        for code_value in (1, 0x4000, 0x4001, 0xffffffff):
            ud = (0x1234000000000000 | i, 0xabcdef0123456789 ^ i)
            b = event(i, code_value, ud)
            assert struct.unpack('<IB3xQQ', b) == (code_value, i, *ud)
            assert b[5:8] == bytes([0xa5]) * 3
            events += 1
    probe_atom = Atom(); probe_atom.udata[0] = 0x45424f5250; probe_atom.atom_number = 1
    lib.T70KbasePack(0, C.byref(probe_atom), 1, pack)
    actual = normalize(pack.raw)
    record = event(actual[48], 1, struct.unpack_from('<QQ', actual, 16))
    assert lib.T70KbaseProbeCompletion(record, len(record)) == 1
    fault = event(actual[48], 0x4001, struct.unpack_from('<QQ', actual, 16))
    assert lib.T70KbaseProbeCompletion(fault, len(fault)) == -1
    cookies = 0
    for i in range(64):
        for present in (False, True):
            mode = 'cookie'
            handle = 0x40000 + i * 4096
            slot = context + 0x2bcb8 + i * 8
            region = storage + 0x2000 if present else 0
            bitmap = 0xa55aa55aa55aa55a & ~(1 << i)
            u.mem_write(slot, struct.pack('<Q', region))
            u.mem_write(context + 0x2bcb0, struct.pack('<Q', bitmap))
            u.reg_write(A.UC_ARM64_REG_X19, context)
            u.reg_write(A.UC_ARM64_REG_X20, handle)
            u.emu_start(base + 0x1984, base + len(code), count=32)
            assert u.reg_read(A.UC_ARM64_REG_PC) == base + (0x19c4 if present else 0x1a78)
            assert struct.unpack('<Q', u.mem_read(slot,8))[0] == 0
            assert struct.unpack('<Q', u.mem_read(context+0x2bcb0,8))[0] == bitmap | ((1 << i) if present else 0)
            if present: assert u.reg_read(A.UC_ARM64_REG_X0) == region
            cookies += 1
    ranges = [(0x1984,0x19c4), (0x29a0c,0x29a3c), (0x29abc,0x29b00), (0x2a3d4,0x2a3f4), (0x2848c,0x28508)]
    return {'status': 'passed', 'stock_module_sha256': STOCK_SHA, 'normalization_cases': cases,
            'semantic_field_loads_compared': fields, 'entry_stride_count_guard_cases': guards,
            'completion_records_compared': events, 'pending_cookie_release_cases': cookies, 'stock_probe_echo_selects': 'KBASE_ATOM_V2_FRAME_NR',
            'stock_fault_echo_rejected': True, 'stock_accepted_strides': [64,72],
            'stock_64_byte_layout': 'v2 fields, renderpass/padding/frame_nr tail; no seq_nr',
            'stock_72_byte_layout': 'seq_nr followed by the 64-byte v2 atom',
            'scope': 'selected original ARM64 normalization, field-load, entry-guard, event-emission and pending-cookie release instructions in virtual memory; copy/object-size/mutex/wakeup services are explicit mocks; not full submit/event functions or GPU execution',
            'mock_service_calls': service_calls,
            'instruction_blocks': [{'start':hex(a),'end_exclusive':hex(b),'sha256':hashlib.sha256(code[a:b]).hexdigest()} for a,b in ranges],
            'consumer_instructions': [hex(x[1]) for x in loads] + ['0x29b98','0x29b58'],
            'complete_kernel_atom_semantics_verified': False,
            'unverified_semantics': ['nonzero renderpass/frame_nr extensions', 'external-resource import/lifetime', 'actual job scheduling/fences/fault handling']}


def compile_backend(zig, output, source=HEADER.parent / 'kbase_kmod.c', arch=None):
    build = MESA / 'build-t70pro-panvk'
    entry = next(x for x in json.loads((build / 'compile_commands.json').read_text())
                 if x['file'].endswith(str(source.relative_to(MESA)))
                 and (arch is None or '-DPAN_ARCH=' + str(arch) in x['command']))
    args = shlex.split(entry['command'])[1:]
    mapped = []; skip = False
    vm_mount = PROJECT.parents[2]
    for arg in args:
        if skip:
            skip = False; continue
        if arg in ('-MF','-MQ','-MT','-o'):
            skip = True; continue
        if arg in ('-MD','-MMD','-c','-mtls-dialect=desc','-DHAVE___BUILTIN_ADD_OVERFLOW_P',
                   '-DHAVE_FUNC_ATTRIBUTE_OPTIMIZE') or arg.endswith(str(source.relative_to(MESA))):
            continue
        arg = arg.replace('/home/builder/teclast-t70pro', str(PROJECT))
        if arg.startswith('-I/usr/include/'):
            arg = '-I' + str(vm_mount / arg[2:].lstrip('/'))
        mapped.append(arg)
    # The rest of libc/Linux UAPI headers come from Zig's existing Linux sysroot.
    # xf86drm.h lives beside libdrm/, so expose only that header via a VFS overlay.
    overlay = output.parent / 'drm-header-overlay.json'
    drm = vm_mount / 'usr/include/xf86drm.h'
    overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':'/t70-drm/xf86drm.h','external-contents':str(drm)}]}))
    command = [zig,'cc','-target','aarch64-linux-gnu','-Wno-unknown-warning-option',
               '-ivfsoverlay',overlay,'-I/t70-drm',*mapped,'-c',source,'-o',output]
    diagnostic = run(command, cwd=build)
    raw = output.read_bytes()
    assert raw[:6] == bytes.fromhex('7f454c460201') and raw[18:20] == bytes.fromhex('b700')
    return {'status':'passed','format':'ELF64 AArch64 relocatable','bytes':len(raw),'sha256':sha(output),
            'source':str(source.relative_to(PROJECT)),'retained':False,
            'scope':'actual complete Linux translation unit cross-compiled with existing generated Mesa config and libdrm header; no ICD relink or Linux execution',
            'compiler_adaptations':['GCC-only mtls-dialect flag omitted','GCC-only builtin/optimize feature defines omitted'],
            'diagnostics':diagnostic}, command


def backend_test(zig, command, tmp, kind="memory"):
    import struct
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    import unicorn.arm64_const as A
    wsi_sync, queue = kind == 'wsi_sync', kind == 'queue'
    assert kind in ('memory', 'wsi_sync', 'queue')
    name = 'panvk_kbase_queue_test' if queue else 'kbase_wsi_sync_test' if wsi_sync else 'kbase_memory_test'
    source = (MESA / 'src/panfrost/vulkan/jm/tests' if queue else HEADER.parent / 'tests') / (name + '.c')
    obj = tmp / (name + '.o')
    extra = []
    if wsi_sync:
        for part in ['wsi', 'runtime', 'util']:
            extra += ['-Isrc/vulkan/' + part, '-I../src/vulkan/' + part]
        extra += ['-Isrc/compiler/spirv']
    # Include the original backend; retain its real BO paths at link time.
    run([*command[:-4],*extra,'-fno-builtin','-mno-outline-atomics','-fno-stack-protector',
         '-fno-unwind-tables','-fno-asynchronous-unwind-tables','-c',source,'-o',obj],
        cwd=MESA/'build-t70pro-panvk')
    exe = BUILD / ('PanvkKbaseQueueTests.elf' if queue else 'PanvkKbaseWsiSyncTests.elf' if wsi_sync else 'PanvkKbaseMemoryTests.elf')
    run([zig,'ld.lld','-m','aarch64linux','--gc-sections','--entry=TestMain','-o',exe,obj])
    raw = exe.read_bytes()
    assert raw[:6] == bytes.fromhex('7f454c460201') and struct.unpack_from('<HH',raw,16) == (2,183)
    phoff = struct.unpack_from('<Q',raw,32)[0]
    phsize, count = struct.unpack_from('<HH',raw,54)
    assert phsize == 56
    segments = [struct.unpack_from('<IIQQQQQQ',raw,phoff+i*phsize) for i in range(count)]
    assert not any(x[0] in (2,3) for x in segments), 'Unexpected interpreter/dynamic runtime'
    loads = [x for x in segments if x[0] == 1]
    start = min(x[3] for x in loads) & ~4095
    end = (max(x[3]+x[6] for x in loads)+4095) & ~4095
    assert 0 < end-start < 0x2000000
    u = Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(start,end-start)
    for x in loads:
        assert x[2]+x[5] <= len(raw) and x[5] <= x[6]
        if x[5]:u.mem_write(x[3],raw[x[2]:x[2]+x[5]])
    stop,stack=0x60000000,0x70000000
    u.mem_map(stop,4096);u.mem_map(stack,0x200000)
    u.reg_write(A.UC_ARM64_REG_SP,stack+0x200000-16);u.reg_write(A.UC_ARM64_REG_X30,stop)
    def interrupt(uc,number,_):
        raise AssertionError('Unexpected OS syscall or CPU exception in backend test: '+str(number))
    u.hook_add(UC_HOOK_INTR,interrupt)
    u.emu_start(struct.unpack_from('<Q',raw,24)[0],stop,timeout=10000000,count=5000000)
    assert u.reg_read(A.UC_ARM64_REG_PC)==stop,kind + ' test exceeded bounded emulation budget'
    assert u.reg_read(A.UC_ARM64_REG_X0)==0,kind + ' test failed at C line '+str(u.reg_read(A.UC_ARM64_REG_X0))
    sht=struct.unpack_from('<Q',raw,40)[0];sw,n,_=struct.unpack_from('<HHH',raw,58)
    sections=[struct.unpack_from('<IIQQQQIIQQ',raw,sht+i*sw) for i in range(n)]
    counter=None
    counter_name = 'T70KbaseQueueAssertionCount' if queue else 'T70KbaseWsiSyncAssertionCount' if wsi_sync else 'T70KbaseMemoryAssertionCount'
    for sec in sections:
        if sec[1]!=2:continue
        ns=sections[sec[6]];names=raw[ns[4]:ns[4]+ns[5]]
        for pos in range(sec[4],sec[4]+sec[5],24):
            no,info,other,si,value,size=struct.unpack_from('<IBBHQQ',raw,pos)
            name=names[no:].split(b'\0',1)[0].decode()
            assert si or not name,'Undefined ELF import: '+name
            if name==counter_name:counter=value
    assert counter
    checks=struct.unpack('<I',u.mem_read(counter,4))[0];assert checks>(20 if queue else 70 if wsi_sync else 200)
    scope = ('actual production kbase sync-file import/export, wait, transfer, attach and merge, common WSI semaphore-to-dma-buf signaling, and JM submit/event-thread completion identity with GPU/fence allocation lifetime; OS fd/poll/read/clock/thread/allocator/merge/dma-buf ioctls are explicit C mocks; no Linux OS or GPU' if wsi_sync else
             'actual production kbase_kmod.c BO allocate/map/free/import/export and DMA-heap shared allocation paths in a freestanding AArch64 Linux test ELF; ioctl, fd/open/close/dup, CPU mappings, public import lock boundary, allocator, sparse registry and logging are C mocks; no Linux OS or GPU')
    if queue:
        scope = 'actual production JM queue submission/completion, chunk rejection and device-loss paths; kmod submission/completion, memory, sync provider, cache flush and logging are explicit C mocks; no Linux OS or GPU'
    return {'status':'passed','assertions':checks,'elf_bytes':len(raw),'elf_sha256':sha(exe),
            'scope':scope,
            'real_ioctl_executed':False,'undefined_imports':False,'instruction_budget':5000000,
            'path':str(exe.relative_to(PROJECT))}


def drm_identity_test(tmp):
    source = MESA/'src/panfrost/vulkan/panvk_physical_device.c'
    arch_source = MESA/'src/panfrost/vulkan/panvk_vX_physical_device.c'
    template = R/'Platforms/Teclast/t70proPkg/Linux/Tests/DrmIdentityTests.c'
    text = source.read_text()
    start = text.index('static VkResult\nget_drm_device_ids(')
    end = text.index('\n#if defined(ENABLE_SHADER_CACHE)', start)
    function = text[start:end].strip()
    assert function.endswith('}')
    text = arch_source.read_text()
    start = text.index('   /* VK_EXT_physical_device_drm */')
    end = text.index('   /* VK_EXT_shader_module_identifier */', start)
    properties = text[start:end].strip()
    fixture = tmp/'DrmIdentityTests.c'
    fixture.write_text(template.read_text().replace('/* ACTUAL_DRM_IDS */', function)
                       .replace('/* ACTUAL_DRM_PROPERTIES */', properties))
    binary = tmp/'DrmIdentityTests'
    run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-fno-sanitize-recover=all',fixture,'-o',binary])
    result = subprocess.run([str(binary)],text=True,capture_output=True)
    record = {'status':'passed' if result.returncode == 0 else 'failed',
              'at':datetime.now(timezone.utc).isoformat(),'result':(result.stdout+result.stderr).strip(),
              'function_sha256':hashlib.sha256(function.encode()).hexdigest(),
              'properties_body_sha256':hashlib.sha256(properties.encode()).hexdigest(),
              'fixture_sha256':sha(fixture),'template_sha256':sha(template),
              'source_sha256':{str(p.relative_to(PROJECT)):sha(p) for p in [source,arch_source,template]},
              'scope':'actual unmodified Mesa function and DRM property body compiled and executed under native ASan/UBSan; stat and surrounding object types are explicit test boundaries; no device access'}
    target=PROJECT/'analysis/panvk-drm-identity-tests.json'
    if target.exists():
        previous=json.loads(target.read_text())
        if previous['status']=='failed' and previous['template_sha256']==record['template_sha256']:
            record['previous_failed_implementation']={k:v for k,v in previous.items() if k!='previous_failed_implementation'}
        elif 'previous_failed_implementation' in previous:record['previous_failed_implementation']=previous['previous_failed_implementation']
    target.write_text(json.dumps(record,indent=2)+'\n')
    if result.returncode:
        raise AssertionError(record['result'])
    print(record['result'],flush=True)
    return record


if '--identity-only' in sys.argv:
    with tempfile.TemporaryDirectory(prefix='t70-panvk-identity-') as temp:
        drm_identity_test(Path(temp))
    sys.exit(0)


BUILD.mkdir(exist_ok=True)
zig = os.environ['T70_MALI_ZIG']
assert run(['git','-C',MESA,'rev-parse','HEAD']) == PINNED
# Intermediates use the host temporary filesystem; source files remain in builder.
# Python fd-relative directory removal is unreliable on this shared mount.
with tempfile.TemporaryDirectory(prefix='t70-panvk-kbase-') as temp:
    tmp = Path(temp)
    cc = 'clang' if sys.platform == 'darwin' else 'cc'
    identity = drm_identity_test(tmp)
    identity_compile, _ = compile_backend(zig,tmp/'panvk_physical_device.o',MESA/'src/panfrost/vulkan/panvk_physical_device.c')
    native = tmp / 'KbaseAbiTests'
    run([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
         '-fno-sanitize-recover=all','-DKBASE_ABI_TEST_NATIVE',TEST,'-o',native])
    native_result = run([native]); print(native_result, flush=True)
    shared = tmp / ('libKbaseAbi.dylib' if sys.platform == 'darwin' else 'libKbaseAbi.so')
    run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror','-dynamiclib' if sys.platform == 'darwin' else '-shared',
         '-fPIC',TEST,'-o',shared])
    stock = stock_comparison(shared)
    print('Stock JM ABI: ' + str(stock['normalization_cases']) + ' atoms, ' +
          str(stock['semantic_field_loads_compared']) + ' field loads, ' +
          str(stock['completion_records_compared']) + ' events passed', flush=True)
    sdk = R/'Windows/Toolchain/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0'
    crt = next((R/'Windows/Toolchain/msvc-crt-headers').rglob('vcruntime.h')).parent
    obj = tmp/'KbaseAbiTests.obj'
    run(['clang','--target=aarch64-pc-windows-msvc','-std=c11','-O2','-ffreestanding',
         '-DNDEBUG','-DKBASE_ABI_TEST_FREESTANDING','-fno-stack-protector','-mstack-probe-size=65536',
         '-isystem',sdk/'ucrt','-isystem',sdk/'shared','-isystem',crt,'-c',TEST,'-o',obj])
    exe = BUILD/'PanvkKbaseAbiTests.exe'
    run([zig,'lld-link','/nologo','/threads:6','/noimplib','/machine:arm64','/subsystem:native',
         '/entry:TestMain','/nodefaultlib','/opt:ref','/export:T70KbaseAbiAssertionCount,DATA','/out:'+str(exe),obj])
    print(run([sys.executable,P/'test-wdk.py',exe,'T70KbaseAbiAssertionCount']), flush=True)
    pe = json.loads(exe.with_suffix('.json').read_text())
    pe['scope'] = 'actual shared Linux Mesa JM ABI pack/probe header in an ARM64 test PE; all submit/wait/read I/O mocked; no Linux/Windows runtime, real ioctl, kernel or GPU'
    pe.pop('command_preparation',None)
    exe.with_suffix('.json').write_text(json.dumps(pe,indent=2)+'\n')
    backend, backend_command = compile_backend(zig,tmp/'kbase_kmod.o')
    memory = backend_test(zig,backend_command,tmp)
    wsi_sync = backend_test(zig,backend_command,tmp,"wsi_sync")
    print("Actual fence provider and WSI bridge: " + str(wsi_sync["assertions"]) + " assertions passed (ARM64 Linux ELF mocks)", flush=True)
    print('Actual Mesa memory paths: '+str(memory['assertions'])+' assertions passed (ARM64 Linux ELF mocks)',flush=True)
    print('Complete Mesa kbase backend: AArch64 Linux object cross-compiled',flush=True)
    external_memory_compile = []
    for name in ['panvk_device_memory.c', 'panvk_buffer.c', 'panvk_image.c']:
        result, _ = compile_backend(zig, tmp / (name + '.o'), MESA / 'src/panfrost/vulkan' / name)
        external_memory_compile.append(result)
    print('Complete Vulkan external-memory callers: 3 AArch64 Linux objects cross-compiled', flush=True)
    wsi_queue_compile = []
    for source, arch in [(MESA / 'src/vulkan/wsi/wsi_common_drm.c', None),
                         (MESA / 'src/panfrost/vulkan/jm/panvk_vX_gpu_queue.c', 9)]:
        result, command = compile_backend(zig, tmp / (source.name + '.o'), source, arch)
        wsi_queue_compile.append(result)
        if arch == 9:
            queue_command = command
    queue = backend_test(zig, queue_command, tmp, 'queue')
    print('Actual JM queue failure paths: ' + str(queue['assertions']) + ' assertions passed (ARM64 Linux ELF mocks)', flush=True)
    sources = [R/'Platforms/Teclast/t70proPkg/Linux/Tests/DrmIdentityTests.c',MESA/'src/panfrost/vulkan/panvk_physical_device.c',MESA/'src/panfrost/vulkan/panvk_vX_physical_device.c',MESA/'src/panfrost/vulkan/jm/tests/panvk_kbase_queue_test.c',MESA/'src/vulkan/wsi/wsi_common_drm.c',MESA/'src/panfrost/vulkan/jm/panvk_vX_gpu_queue.c',HEADER.parent/'tests/kbase_wsi_sync_test.c',HEADER,TEST,HEADER.parent/'tests/kbase_memory_test.c',HEADER.parent/'kbase_kmod.c',HEADER.parent/'kbase_kmod.h',HEADER.parent/'meson.build',MESA/'src/panfrost/vulkan/panvk_device_memory.c',MESA/'src/panfrost/vulkan/panvk_buffer.c',MESA/'src/panfrost/vulkan/panvk_image.c',Path(__file__).resolve(),P/'test-wdk.py',MESA/'src/panfrost/vulkan/panvk_vX_device.c']
    report = {'status':'passed','at':datetime.now(timezone.utc).isoformat(),'mesa_commit':PINNED,
              'scope':'offline JM wire-ABI implementation, actual probe with mocked I/O, original selected ARM64 code comparison and Linux backend compile; no device access',
              'native_drm_identity_test':identity,'linux_physical_device_compile':identity_compile,
              'native_result':native_result,'arm64_core_tests':pe,'arm64_memory_backend_tests':memory,'arm64_wsi_sync_tests':wsi_sync,'arm64_queue_failure_tests':queue,'stock_comparison':stock,
              'linux_backend_compile':backend,'linux_external_memory_compile':external_memory_compile,'linux_wsi_queue_compile':wsi_queue_compile,
              'panvk_icd_relinked_after_this_change':False,
              'device_runtime_tested':False,'real_ioctl_executed':False,
              'source_sha256':{str(p.relative_to(PROJECT)):sha(p) for p in sources},
              'artifacts':{'arm64_queue_tests':{'path':queue['path'],'bytes':queue['elf_bytes'],'sha256':queue['elf_sha256']},'arm64_wsi_sync_tests':{'path':wsi_sync['path'],'bytes':wsi_sync['elf_bytes'],'sha256':wsi_sync['elf_sha256']},'arm64_core_tests':{'path':str(exe.relative_to(PROJECT)),'bytes':exe.stat().st_size,'sha256':sha(exe)},'arm64_memory_tests':{'path':memory['path'],'bytes':memory['elf_bytes'],'sha256':memory['elf_sha256']}},
              'commands':commands}
report['temporary_outputs_removed'] = not tmp.exists()
assert report['temporary_outputs_removed']
REPORT.write_text(json.dumps(report,indent=2)+'\n')
print('kbase ABI report: '+str(REPORT),flush=True)
