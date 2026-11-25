#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Production export core + serialized mtools mock; never a physical/EFI test.

From repository root:
  python3 Resources/Scripts/with-build-lock handoff FAT-regression -- \
    python3 Platforms/Teclast/t70proPkg/Tests/DeviceCheck/test-export-fat-image.py
Requires existing cc and mtools. BMP is a deterministic 1x1 fixture, NOT the
previous GOP RAM capture. All temporary image/binary files are removed.

Only private tempfile regular images are accepted/created. No mount/raw path.
CreateNew atomicity here comes from the isolated single-writer test, NOT mtools
or a claim about the real EFI FAT protocol. Flush invokes mcopy and Close
fsyncs the regular image; a fresh mcopy process independently reads each file.
"""
import ctypes as C
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

class Request(C.Structure):
    _fields_ = [("Selection", C.c_uint64), ("Manual", C.c_uint),
                ("SdFat", C.c_uint), ("Writable", C.c_uint)]
class Result(C.Structure):
    _fields_ = [("Status", C.c_int), ("OperationError", C.c_int),
                ("CloseError", C.c_int), ("Written", C.c_size_t),
                ("Created", C.c_uint), ("FlushSucceeded", C.c_uint)]
Validate = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64)
Create = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.c_char_p, C.POINTER(C.c_void_p))
Write = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t))
Simple = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p)
class Io(C.Structure):
    _fields_ = [("Context", C.c_void_p), ("Validate", Validate), ("Create", Create),
                ("Write", Write), ("Flush", Simple), ("Close", Simple)]

def main():
    import os
    for name in ("cc", "mformat", "mcopy", "mdir"):
        if not shutil.which(name):
            raise SystemExit("Missing existing tool: " + name)
    source = Path(__file__).resolve().parents[2] / "Applications/DeviceCheck/ManualExportCore.c"
    with tempfile.TemporaryDirectory(prefix="t70-fat-regression-") as td:
        root = Path(td)
        image = root / "test.fat"
        with image.open("xb") as f:
            f.truncate(32 * 1024 * 1024)
        def run(*args, check=True):
            return subprocess.run(args, check=check, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        run("mformat", "-i", str(image), "::")
        run("cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(root / "core.so"))
        lib = C.CDLL(str(root / "core.so"))
        lib.T70ManualExport.argtypes = [C.POINTER(Request), C.c_int, C.c_uint32, C.c_void_p, C.c_size_t, C.POINTER(Io)]
        lib.T70ManualExport.restype = Result
        state = {"name": None, "data": b"", "calls": 0, "errors": []}
        def guarded(fn):
            def wrap(*args):
                try:
                    state["calls"] += 1
                    return fn(*args)
                except Exception as exc:
                    state["errors"].append(repr(exc))
                    return -1
            return wrap
        def create(ctx, selected, name, output):
            output[0] = None
            name = name.decode("ascii")
            assert selected == 1 and name.startswith("T70-") and "/" not in name
            if run("mdir", "-i", str(image), "::/" + name, check=False).returncode == 0:
                return 1
            empty = root / "empty"
            empty.write_bytes(b"")
            run("mcopy", "-i", str(image), str(empty), "::/" + name)
            state.update(name=name, data=b"")
            output[0] = 1
            return 0
        def write(ctx, handle, data, size, written):
            assert handle == 1
            state["data"] = C.string_at(data, size)
            written[0] = size
            return 0
        def flush(ctx, handle):
            assert handle == 1
            data = root / "payload"
            data.write_bytes(state["data"])
            # Only overwrite the new file owned by this isolated operation.
            run("mcopy", "-o", "-i", str(image), str(data), "::/" + state["name"])
            return 0
        def close(ctx, handle):
            assert handle == 1
            with image.open("rb") as f:
                os.fsync(f.fileno())
            return 0
        callbacks = [Validate(guarded(lambda c, s: int(s == 1))), Create(guarded(create)),
                     Write(guarded(write)), Simple(guarded(flush)), Simple(guarded(close))]
        io = Io(None, *callbacks)
        def export(kind, number, payload, manual=1):
            req = Request(1, manual, 1, 1)
            data = C.create_string_buffer(payload)
            return lib.T70ManualExport(C.byref(req), kind, number, data, len(payload), C.byref(io))
        digest = lambda: hashlib.sha256(image.read_bytes()).hexdigest()
        log = b"\xff\xfe" + "T70 diagnostic\r\n한글 로그\r\n".encode("utf-16le")
        pixels = bytes([0, 0, 255, 0])
        bmp = struct.pack("<2sIHHI", b"BM", 58, 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, 1, 1, 1, 24, 0, 4, 0, 0, 0, 0) + pixels
        before = digest()
        r = export(1, 1, log, manual=0)
        assert r.Status == 1 and state["calls"] == 0 and digest() == before
        verified = {}
        for kind, number, payload, suffix in ((1, 1, log, "txt"), (2, 2, bmp, "bmp")):
            r = export(kind, number, payload)
            assert r.Status == 0 and r.Written == len(payload) and r.FlushSucceeded and not r.CloseError
            name = f"T70-{number:08X}.{suffix}"
            reopened = root / (name + ".readback")
            run("mcopy", "-i", str(image), "::/" + name, str(reopened))
            assert reopened.read_bytes() == payload
            verified[name] = hashlib.sha256(payload).hexdigest()
            before = digest()
            r = export(kind, number, b"MUST NOT OVERWRITE")
            assert r.Status == 2 and not r.Created and digest() == before
        assert not state["errors"], state["errors"]
        print("PASS: production core; no-confirm image unchanged; log/BMP independent readback; collisions unchanged")
        print(verified)
        print("Mock callbacks only, not EFI FAT driver, SD identity, physical media or power-loss proof")

if __name__ == "__main__":
    main()
