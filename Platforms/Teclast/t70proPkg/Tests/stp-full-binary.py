# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare the full-mode STP link with the stock ARM64 send, RX and retry code."""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

from unicorn import UC_HOOK_CODE, UcError
from unicorn.arm64_const import (
    UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0,
    UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X8,
)

Write = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p, C.c_uint, C.POINTER(C.c_uint))


class Link(C.Structure):
    _fields_ = [("context", C.c_void_p), ("write", Write),
                ("first", C.c_uint64), ("last", C.c_uint64)]
    _fields_ += [(n, C.c_uint) for n in ("tx_next", "tx_length", "rx_next", "ack",
                                        "fault", "header_used", "body_used",
                                        "resync", "sync_used", "offered_length")]
    _fields_ += [("header", C.c_ubyte * 4), ("body", C.c_ubyte * 2050),
                 ("tx", C.c_ubyte * 2006)]


class Frame(C.Structure):
    _fields_ = [("channel", C.c_uint), ("length", C.c_uint), ("data", C.c_void_p)]


def compare(lib, stock, sha, isolated_module):
    selected = (
        "mtk_wcn_stp_enable", "mtk_wcn_stp_set_mode", "mtk_wcn_stp_send_data",
        "stp_is_tx_res_available", "stp_add_to_tx_queue", "stp_send_tx_queue", "osal_crc16",
        "mtk_wcn_stp_parser_data", "stp_send_ack", "stp_do_tx_timeout",
    )
    raw, sections, symbols, ranges, bases, uc, stub, external, address, relocations = (
        isolated_module(stock, selected))
    assert hashlib.sha256(raw).hexdigest() == sha
    stop, tx, notify, status = stub + 0xf000, stub + 0xe000, stub + 0xe020, stub + 0xe040
    counter = stub + 0xe060
    for pointer, tag in ((tx, 0x94b3d001), (notify, 0x81e1eabd), (status, 0x3463e8d3),
                         (counter, 0x6fbb3035)):
        uc.mem_write(pointer - 4, struct.pack("<II", tag, 0xd65f03c0))
    data, stack = 0x30000000, 0x40000000
    uc.mem_map(data, 0x10000)
    uc.mem_map(stack, 0x10000)
    bss = next(bases[i] for i, s in enumerate(sections) if s[1] == 8 and s[5] > 0x30068)
    traps = {pointer: name for name, pointer in external.items()}
    traps.update({tx: "tx", notify: "notify", status: "status", counter: "traffic_counter"})
    noops = {
        "osal_lock_sleepable_lock", "osal_unlock_sleepable_lock", "osal_lock_unsleepable_lock",
        "osal_unlock_unsleepable_lock", "osal_ftrace_print", "osal_err_print", "osal_warn_print",
        "osal_buffer_dump", "osal_timer_stop", "osal_dump_thread_state", "___ratelimit",
        "stp_psm_disable_by_tx_rx_density", "stp_psm_thread_lock_aquire",
        "stp_psm_thread_lock_release", "stp_psm_is_to_block_traffic", "stp_psm_has_pending_data",
        "stp_psm_is_quick_ps_support", "stp_dbg_pkt_log", "stp_dbg_log_pkt",
        "mtk_wcn_consys_stp_btif_logger_ctrl", "stp_dbg_poll_cpupcr", "notify", "traffic_counter",
    }
    for name in noops | {"osal_memcpy", "osal_memset", "osal_timer_start", "wmt_plat_get_comm_if_type",
                         "stp_add_to_rx_queue", "wmt_parser_data"}:
        if name in symbols and symbols[name][1]:
            traps[address(symbols[name])] = name
    sent, received, ported, timers = [], [], [], []
    executed = set()

    def hook(u, pc, _size, _context):
        name = traps.get(pc)
        if name is None:
            assert any(bases[i]+lo <= pc < bases[i]+hi for i, lo, hi in ranges), (
                hex(pc), [n for n, i, start, length in symbols.values()
                          if i in bases and bases[i]+start <= pc < bases[i]+start+length])
            if struct.unpack("<I", u.mem_read(pc, 4))[0] == 0xd5384108:
                u.reg_write(UC_ARM64_REG_X8, data + 0xf000)
                u.reg_write(UC_ARM64_REG_PC, pc + 4)
            return
        executed.add(name)
        a, b, c = (u.reg_read(reg) for reg in (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
        result = 0
        if name == "tx":
            sent.append(bytes(u.mem_read(a, b)))
            u.mem_write(c, struct.pack("<I", b))
        elif name == "stp_add_to_rx_queue":
            received.append((c, bytes(u.mem_read(a, b))))
        elif name == "wmt_parser_data":
            assert b == 4
            received.append((b, bytes(u.mem_read(bss + 0x7e94, a))))
        elif name == "osal_memcpy":
            u.mem_write(a, bytes(u.mem_read(b, c)))
            result = a
        elif name == "osal_memset":
            u.mem_write(a, bytes([b & 255]) * c)
            result = a
        elif name == "osal_timer_start":
            timers.append(b)
        elif name == "status":
            result = 0x1f
        elif name == "wmt_plat_get_comm_if_type":
            result = 2
        else:
            assert name in noops, (name, hex(pc))
        u.reg_write(UC_ARM64_REG_X0, result)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)

    def original(name, a=0, b=0, c=0):
        for reg, value in ((UC_ARM64_REG_X0, a), (UC_ARM64_REG_X1, b), (UC_ARM64_REG_X2, c),
                           (UC_ARM64_REG_LR, stop), (UC_ARM64_REG_SP, stack + 0xfff0)):
            uc.reg_write(reg, value)
        try:
            uc.emu_start(address(symbols[name]), stop, count=100000)
        except UcError as error:
            raise AssertionError((name, hex(uc.reg_read(UC_ARM64_REG_PC) - bases[symbols[name][1]]))) from error
        assert uc.reg_read(UC_ARM64_REG_PC) == stop, name
        return C.c_int32(uc.reg_read(UC_ARM64_REG_X0)).value

    def invoke(name, wire, channel=0):
        uc.mem_write(data, wire)
        return original(name, data, len(wire), channel)

    @Write
    def write(_context, buffer, length, written):
        ported.append(C.string_at(buffer, length))
        written[0] = length
        return 0

    link, frame, used = Link(), Frame(), C.c_uint()
    lib.MtkStpFullInit.argtypes = [C.POINTER(Link), C.c_void_p, Write]
    lib.MtkStpFullTransmit.argtypes = [C.POINTER(Link), C.c_uint, C.c_void_p, C.c_uint, C.c_uint64]
    lib.MtkStpFullReceive.argtypes = [C.POINTER(Link), C.c_void_p, C.c_uint, C.POINTER(C.c_uint),
                                     C.POINTER(Frame)]
    lib.MtkStpFullService.argtypes = [C.POINTER(Link), C.c_uint64]
    lib.MtkStpFullAccept.argtypes = [C.POINTER(Link)]

    def reset():
        for offset, pointer in ((0x7e38, counter), (0x7e40, counter), (0x7e48, tx),
                                (0x7e58, notify), (0x7e60, notify), (0x7e68, status)):
            uc.mem_write(bss + offset, struct.pack("<Q", pointer))
        original("mtk_wcn_stp_enable", 1)
        original("mtk_wcn_stp_set_mode", 8)
        uc.mem_write(bss + 0x30039, b"\1")
        query = bytes.fromhex("01 04 01 00 04")
        sent.clear()
        assert invoke("mtk_wcn_stp_send_data", query, 4) == len(query)
        assert sent == [b"\x80\x40\x05\0" + query + b"\0\0"]
        original("mtk_wcn_stp_set_mode", 4)
        assert lib.MtkStpFullInit(C.byref(link), None, write) == 0
        sent.clear()
        ported.clear()
        received.clear()
        timers.clear()

    def peer(seq, ack, channel=0, payload=b""):
        head = bytes((0x80 | (seq << 3) | ack, (channel << 4) | (len(payload) >> 8), len(payload) & 255))
        head += bytes([sum(head) & 255])
        return head + payload + struct.pack("<H", invoke("osal_crc16", payload)) if payload else head

    def tx_pair(payload, channel, now):
        sent.clear()
        ported.clear()
        assert invoke("mtk_wcn_stp_send_data", payload, channel) == len(payload), (len(payload), channel)
        assert lib.MtkStpFullTransmit(C.byref(link), channel, payload, len(payload), now) == 0
        assert b"".join(sent) == b"".join(ported), (len(payload), channel, sent[:1], ported[:1])

    def rx_pair(wire, now, chunk, result):
        sent.clear()
        ported.clear()
        received.clear()
        got = []
        for offset in range(0, len(wire), chunk):
            part = wire[offset:offset+chunk]
            invoke("mtk_wcn_stp_parser_data", part)
            response = lib.MtkStpFullReceive(C.byref(link), part, len(part), C.byref(used), C.byref(frame))
            assert response in (3, 4) and used.value == len(part), (offset, response, used.value)
            if response == 4:
                got.append((frame.channel, C.string_at(frame.data, frame.length)))
                assert lib.MtkStpFullAccept(C.byref(link)) == 0
            assert lib.MtkStpFullService(C.byref(link), now) == 0
        assert received == got == result, (received, got, result)
        assert sent == ported, (sent, ported)

    reset()
    lengths = range(1, 1997)
    for i, length in enumerate(lengths):
        channel = (0, 1, 2, 3, 4, 7)[i % 6]
        payload = bytes((k * 73 + i) & 255 for k in range(length))
        tx_pair(payload, channel, i)
        rx_pair(peer(0, i & 7), i, 4, [])
        assert not link.tx_length
    assert timers and set(timers) == {180}

    reset()
    rx_cases = 0
    for i, length in enumerate((1, 2, 3, 9, 255, 256, 257, 511, 1024, 1999, 2048) * 3):
        seq = i & 7
        prev = (i-1) & 7
        channel = (0, 2, 4)[i % 3]
        payload = bytes((k * 37 + i) & 255 for k in range(length))
        tx_pair(b"\x01\x04\x01\0\x04", 4, i)
        incoming = peer(seq, seq, channel, payload)
        bad_crc = incoming[:-1] + bytes([incoming[-1] ^ 0x80])
        rx_pair(bad_crc, i, len(bad_crc), [])
        assert link.tx_length
        rx_pair(peer(prev, seq, channel, payload), i, length + 6, [])
        assert link.tx_length
        rx_pair(incoming, i, (1, 7, 99, length + 6)[i % 4], [(channel, payload)])
        assert not link.tx_length
        rx_cases += 3

    reset()
    payload = b"\x01\x04\x01\0\x04"
    tx_pair(payload, 4, 0)
    reply = b"\x02\x04\x06\0\0\x04\xdb\x0e\x68\x01"
    rx_pair(peer(0, 7, 4, reply), 0, 2, [(4, reply)])
    sent.clear()
    ported.clear()
    original("stp_do_tx_timeout")
    assert lib.MtkStpFullService(C.byref(link), 180) == 0
    assert b"".join(sent) == b"".join(ported) == b"\x7f"*4 + peer(0, 0, 4, payload)
    rx_pair(peer(0, 0), 180, 1, [])
    assert not link.tx_length
    return {"module_sha256": sha, "relocations": relocations, "tx_cases": len(lengths),
            "rx_cases": rx_cases, "standalone_acks": len(lengths)+1, "retry_cases": 1,
            "original_functions": {n: symbols[n][2:] for n in selected},
            "virtual_services_called": sorted(executed)}


def verify(root, out, stock, sha, isolated_module, arm64_test):
    if out.exists():
        raise SystemExit("Use a new output directory for STP full-mode results")
    silicon = root / "Silicon/MediaTek/MediaTekPkg"
    include = silicon / "Include"
    core = silicon / "Library/MtkStpCoreLib"
    sources = [core / (stem+".c") for stem in ("MtkStpFull", "MtkBtifCore")]
    fixture = Path(__file__).with_name("MtkStpFullTest.c")
    inputs = [*sources, fixture, include/"Library/MtkStpFull.h", include/"Library/MtkBtifCore.h",
              include/"Library/MtkStpCore.h", Path(__file__), Path(__file__).with_name("verify-stp-binary.py")]

    def hashes():
        return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}

    before = hashes()
    cc = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", include]
    clang = ["clang", *cc[1:]]
    def run(*args):
        subprocess.run(args, check=True)
    with tempfile.TemporaryDirectory(prefix="t70-stp-full-") as directory:
        stage = Path(directory)
        native, shared, image = (stage / name for name in ("native", "shared.so", "StpFullTest.exe"))
        run(*cc, "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-DSTP_TEST_NATIVE", *sources, fixture, "-o", native)
        run(native)
        run(*clang, "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC", "-O2",
            sources[0], "-o", shared)
        original = compare(C.CDLL(str(shared)), stock, sha, isolated_module)
        flags = ["--target=arm64-pc-windows-msvc", "-O2", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-mstack-probe-size=65536", "-fstack-usage", "-c"]
        objects = []
        for source in [*sources, fixture]:
            obj = stage / (source.stem+".obj")
            run(*clang, *flags, source, "-o", obj)
            objects.append(obj)
        for obj in objects[:-1]:
            for line in obj.with_suffix(".su").read_text().splitlines():
                _, size, kind = line.rsplit("\t", 2)
                assert kind == "static" and int(size) <= 256, line
        run("lld-link", "/nologo", "/machine:arm64", "/entry:TestMain", "/subsystem:native",
            "/nodefaultlib", "/noimplib", "/export:MtkStpFullAssertionCount,DATA", f"/out:{image}", *objects)
        arm = arm64_test(image, "MtkStpFullAssertionCount", 10000)
        assert hashes() == before
        report = {"original": original, "arm64": arm, "inputs": before,
                  "pe_sha256": hashlib.sha256(image.read_bytes()).hexdigest()}
        out.mkdir(parents=True, exist_ok=False)
        shutil.copyfile(image, out / image.name)
        (out / "verification.json").write_text(json.dumps(report, indent=2)+"\n")
        print(f"STP mode 4 matched {original['tx_cases']} stock TX frames, "
              f"{original['rx_cases']} RX cases, {original['standalone_acks']} ACKs and a timed retry")
        print(f"ARM64 STP full-mode test passed {arm['assertions']} assertions: {out}")
