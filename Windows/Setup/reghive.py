#!/usr/bin/env python3
"""Minimal Windows registry hive (regf) reader and editor.

make-t70-usb.py uses it on macOS and Linux to switch on test signing in the
install stick's BCD store, which is a registry hive; Windows hosts use
bcdedit for the same change. Only what that needs is implemented: walk keys,
read small values, and set a small value, creating the last key of the path
when it is missing. New cells go into a new hive bin appended at the end, as
Windows grows a hive. Anything unexpected raises HiveError and nothing is
written.

    python3 reghive.py dump BCD
    python3 reghive.py testsigning BCD        (edits BCD in place)
"""
import struct
import sys
import time

BASE = 4096          # the base block; cell offsets count from the first bin
BIN = 4096
NONE = 0xFFFFFFFF
KEY_COMP_NAME = 0x20
VALUE_COMP_NAME = 0x0001
REG_BINARY = 3

# BcdLibraryBoolean_AllowPrereleaseSignatures ("testsigning") on {default},
# which Windows installation media point at {7619dcc9-fafe-11d9-b411-000476eba25f}.
BCD_DEFAULT_LINK = 'Objects\\{9dea862c-5cdd-4e70-acc1-f32b344d4795}\\Elements\\23000003'
TESTSIGNING = '16000049'


class HiveError(Exception):
    pass


def _filetime():
    return int((time.time() + 11644473600) * 10_000_000)


def _checksum(block):
    x = 0
    for (v,) in struct.iter_unpack('<I', bytes(block[:508])):
        x ^= v
    return 0xFFFFFFFE if x == 0xFFFFFFFF else (1 if x == 0 else x)


def _lh_hash(name):
    h = 0
    for ch in name.upper():
        h = (h * 37 + ord(ch)) & 0xFFFFFFFF
    return h


def _lf_hint(name):
    return name.encode('latin-1')[:4].ljust(4, b'\0')


class Hive:
    NK = struct.Struct('<HQIIIIIIIIIIIIIIIHH')   # after the 'nk' signature
    VK = struct.Struct('<HIIIHH')                # after the 'vk' signature

    def __init__(self, data):
        d = bytearray(data)
        if len(d) < BASE or d[:4] != b'regf':
            raise HiveError('not a registry hive')
        seq1, seq2 = struct.unpack_from('<II', d, 4)
        if seq1 != seq2:
            raise HiveError('hive has unapplied log changes')
        if struct.unpack_from('<I', d, 508)[0] != _checksum(d):
            raise HiveError('base block checksum mismatch')
        size = struct.unpack_from('<I', d, 40)[0]
        if size == 0 or size % BIN or BASE + size > len(d):
            raise HiveError('bad hive bins size')
        self.d = d[:BASE + size]
        self.size = size
        self.root = struct.unpack_from('<I', d, 36)[0]
        self.check()

    # ---- structure -------------------------------------------------------
    def check(self):
        """Every bin is chained and every bin is exactly filled with cells."""
        off = 0
        while off < self.size:
            p = BASE + off
            if self.d[p:p + 4] != b'hbin' or struct.unpack_from('<I', self.d, p + 4)[0] != off:
                raise HiveError(f'bad hive bin at {off:#x}')
            size = struct.unpack_from('<I', self.d, p + 8)[0]
            if size < BIN or size % BIN or off + size > self.size:
                raise HiveError(f'bad hive bin size at {off:#x}')
            c = p + 32
            while c < p + size:
                cs = abs(struct.unpack_from('<i', self.d, c)[0])
                if cs < 8 or cs % 8 or c + cs > p + size:
                    raise HiveError(f'bad cell at {c - BASE:#x}')
                c += cs
            off += size

    def _cell(self, off):
        if off == NONE or off + 4 > self.size:
            raise HiveError(f'bad cell offset {off:#x}')
        p = BASE + off
        size = struct.unpack_from('<i', self.d, p)[0]
        if size >= 0:
            raise HiveError(f'cell {off:#x} is free')
        return p + 4, -size - 4

    def key(self, off):
        p, n = self._cell(off)
        if self.d[p:p + 2] != b'nk':
            raise HiveError(f'cell {off:#x} is not a key')
        f = self.NK.unpack_from(self.d, p + 2)
        flags, parent, nsub, sublist, nval, vallist, sec, namelen = (
            f[0], f[3], f[4], f[6], f[8], f[9], f[10], f[17])
        raw = bytes(self.d[p + 76:p + 76 + namelen])
        name = raw.decode('latin-1') if flags & KEY_COMP_NAME else raw.decode('utf-16-le')
        return dict(off=off, pos=p, flags=flags, parent=parent, nsub=nsub, sublist=sublist,
                    nval=nval, vallist=vallist, sec=sec, name=name,
                    maxsub=f[12], maxvname=f[14], maxvdata=f[15])

    def _list(self, off):
        p, _ = self._cell(off)
        sig = bytes(self.d[p:p + 2])
        n = struct.unpack_from('<H', self.d, p + 2)[0]
        if sig in (b'lf', b'lh'):
            return [struct.unpack_from('<I', self.d, p + 4 + 8 * i)[0] for i in range(n)], sig
        if sig == b'li':
            return [struct.unpack_from('<I', self.d, p + 4 + 4 * i)[0] for i in range(n)], sig
        if sig == b'ri':
            out = []
            for i in range(n):
                out += self._list(struct.unpack_from('<I', self.d, p + 4 + 4 * i)[0])[0]
            return out, sig
        raise HiveError(f'unknown subkey list {sig!r}')

    def subkeys(self, k):
        return [self.key(o) for o in self._list(k['sublist'])[0]] if k['nsub'] else []

    def values(self, k):
        out = []
        if not k['nval']:
            return out
        p, n = self._cell(k['vallist'])
        if n < 4 * k['nval']:
            raise HiveError('value list too short')
        for i in range(k['nval']):
            v = struct.unpack_from('<I', self.d, p + 4 * i)[0]
            q, _ = self._cell(v)
            if self.d[q:q + 2] != b'vk':
                raise HiveError(f'cell {v:#x} is not a value')
            namelen, dsize, doff, vtype, flags, _ = self.VK.unpack_from(self.d, q + 2)
            raw = bytes(self.d[q + 20:q + 20 + namelen])
            name = raw.decode('latin-1') if flags & VALUE_COMP_NAME else raw.decode('utf-16-le')
            if dsize & 0x80000000:
                data = struct.pack('<I', doff)[:dsize & 0x7FFFFFFF]
            else:
                if dsize > 16344:
                    raise HiveError('big values are not supported')
                dp, dn = self._cell(doff)
                if dn < dsize:
                    raise HiveError('value data cell too short')
                data = bytes(self.d[dp:dp + dsize])
            out.append(dict(off=v, pos=q, name=name, type=vtype, data=data))
        return out

    def child(self, k, name):
        for s in self.subkeys(k):
            if s['name'].upper() == name.upper():
                return s
        return None

    def open(self, path):
        k = self.key(self.root)
        for part in filter(None, path.split('\\')):
            k = self.child(k, part)
            if k is None:
                return None
        return k

    def value(self, path, name):
        k = self.open(path)
        if k is None:
            return None
        for v in self.values(k):
            if v['name'].upper() == name.upper():
                return v
        return None

    # ---- editing ---------------------------------------------------------
    def set_value(self, path, name, vtype, data):
        """Set a value of at most four bytes; create the last key if missing."""
        if len(data) > 4:
            raise HiveError('only values of up to four bytes are supported')
        parts = [p for p in path.split('\\') if p]
        parent = self.open('\\'.join(parts[:-1]))
        if parent is None:
            raise HiveError(f'missing key {"/".join(parts[:-1])}')
        k = self.child(parent, parts[-1])
        if k is None:
            self._add_key(parent, parts[-1], name, vtype, data)
            return 'created'
        for v in self.values(k):
            if v['name'].upper() == name.upper():
                if v['type'] == vtype and v['data'] == data:
                    return 'unchanged'
                struct.pack_into('<III', self.d, v['pos'] + 4,
                                 len(data) | 0x80000000,
                                 int.from_bytes(data.ljust(4, b'\0'), 'little'), vtype)
                self._commit(0)
                return 'updated'
        raise HiveError('adding a value to an existing key is not supported')

    def _add_key(self, parent, keyname, vname, vtype, data):
        if not keyname.isascii() or not vname.isascii():
            raise HiveError('only ASCII names are supported')
        entries, sig = (self._list(parent['sublist']) if parent['nsub'] else ([], b'lh'))
        if sig == b'ri':
            raise HiveError('indexed subkey lists are not supported')
        if len(entries) != parent['nsub']:
            raise HiveError('subkey count mismatch')
        start = self.size
        cells = bytearray()

        def alloc(payload):
            size = (4 + len(payload) + 7) & ~7
            off = start + 32 + len(cells)
            cells.extend(struct.pack('<i', -size) + payload + bytes(size - 4 - len(payload)))
            return off

        now = _filetime()
        vk = alloc(b'vk' + self.VK.pack(len(vname), len(data) | 0x80000000,
                                        int.from_bytes(data.ljust(4, b'\0'), 'little'),
                                        vtype, VALUE_COMP_NAME, 0) + vname.encode())
        vlist = alloc(struct.pack('<I', vk))
        nk = alloc(b'nk' + self.NK.pack(KEY_COMP_NAME, now, 0, parent['off'], 0, 0, NONE, NONE,
                                        1, vlist, parent['sec'], NONE, 0, 0,
                                        2 * len(vname), len(data), 0, len(keyname), 0)
                   + keyname.encode())
        names = [(self.key(o)['name'], o) for o in entries] + [(keyname, nk)]
        names.sort(key=lambda t: t[0].upper())
        if sig == b'li':
            body = b''.join(struct.pack('<I', o) for _, o in names)
        else:
            hint = _lh_hash if sig == b'lh' else (lambda s: int.from_bytes(_lf_hint(s), 'little'))
            body = b''.join(struct.pack('<II', o, hint(n)) for n, o in names)
        newlist = alloc(sig + struct.pack('<H', len(names)) + body)

        binsize = (32 + len(cells) + 8 + BIN - 1) // BIN * BIN
        free = binsize - 32 - len(cells)
        cells.extend(struct.pack('<i', free) + bytes(free - 4))
        header = b'hbin' + struct.pack('<IIQQI', start, binsize, 0, now, 0)
        assert len(header) == 32
        self.d.extend(header + cells)
        self.size += binsize

        # Parent: one more subkey, the new list, the longest name in UTF-16 bytes.
        p = parent['pos']
        struct.pack_into('<I', self.d, p + 20, parent['nsub'] + 1)
        struct.pack_into('<I', self.d, p + 28, newlist)
        struct.pack_into('<I', self.d, p + 52, max(parent['maxsub'], 2 * len(keyname)))
        struct.pack_into('<Q', self.d, p + 4, now)
        if parent['nsub']:
            old = BASE + parent['sublist']
            struct.pack_into('<i', self.d, old, -struct.unpack_from('<i', self.d, old)[0])
        # The new key shares its parent's security descriptor.
        sp, _ = self._cell(parent['sec'])
        if self.d[sp:sp + 2] != b'sk':
            raise HiveError('parent security cell is not a descriptor')
        struct.pack_into('<I', self.d, sp + 12, struct.unpack_from('<I', self.d, sp + 12)[0] + 1)
        self._commit(binsize)

    def _commit(self, grown):
        seq = struct.unpack_from('<I', self.d, 4)[0] + 1
        struct.pack_into('<IIQ', self.d, 4, seq, seq, _filetime())
        struct.pack_into('<I', self.d, 40, self.size)
        struct.pack_into('<I', self.d, 508, _checksum(self.d))
        self.check()

    def bytes(self):
        return bytes(self.d)


def bcd_default(h):
    """The object id {default} names in a Windows boot manager store."""
    v = h.value(BCD_DEFAULT_LINK, 'Element')
    if v is None or v['type'] != 1:
        raise HiveError('BCD has no boot manager default entry')
    guid = v['data'].decode('utf-16-le').rstrip('\0')
    if h.open('Objects\\' + guid + '\\Elements') is None:
        raise HiveError(f'BCD default entry {guid} is missing')
    return guid


def bcd_testsigning(data):
    """Return (new BCD bytes, default object id, 'created'|'updated'|'unchanged')."""
    h = Hive(data)
    guid = bcd_default(h)
    state = h.set_value(f'Objects\\{guid}\\Elements\\{TESTSIGNING}', 'Element', REG_BINARY, b'\x01')
    again = Hive(h.bytes())
    v = again.value(f'Objects\\{guid}\\Elements\\{TESTSIGNING}', 'Element')
    if v is None or v['type'] != REG_BINARY or v['data'] != b'\x01':
        raise HiveError('test signing did not read back')
    return again.bytes(), guid, state


def _dump(h, k, depth=0):
    vals = ' '.join(f"{v['name'] or '@'}={v['type']}:{v['data'][:16].hex()}" for v in h.values(k))
    print('  ' * depth + k['name'], vals)
    for s in h.subkeys(k):
        _dump(h, s, depth + 1)


def main(argv):
    if len(argv) == 3 and argv[1] == 'dump':
        h = Hive(open(argv[2], 'rb').read())
        _dump(h, h.key(h.root))
        return 0
    if len(argv) == 3 and argv[1] == 'testsigning':
        data, guid, state = bcd_testsigning(open(argv[2], 'rb').read())
        if state != 'unchanged':
            with open(argv[2], 'r+b') as f:
                f.write(data)
                f.truncate()
        print(f'{argv[2]}: testsigning on for {guid} ({state})')
        return 0
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
