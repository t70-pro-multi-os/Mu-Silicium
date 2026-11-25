#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Compare packaged ACPI and installer DEPEX with independently built tables.

This inspects existing FD bytes; it does not execute DXE or prove a fresh full
firmware build. Pass fresh source-built tables to --tables-dir for provenance.
"""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import struct
import uuid


def sections(data):
    offset = 0
    while offset < len(data):
        assert offset + 4 <= len(data)
        size = int.from_bytes(data[offset:offset+3], 'little')
        header = 4
        if size == 0xffffff:
            assert offset + 8 <= len(data)
            size = struct.unpack_from('<I', data, offset+4)[0]
            header = 8
        assert header <= size <= len(data)-offset
        yield data[offset+3], data[offset+header:offset+size]
        offset = (offset+size+3) & ~3


def files(volume):
    assert volume[40:44] == b'_FVH'
    assert struct.unpack_from('<Q', volume, 32)[0] <= len(volume)
    offset = (struct.unpack_from('<H', volume, 48)[0]+7) & ~7
    result = {}
    while offset+24 <= len(volume) and volume[offset:offset+24] != b'\xff'*24:
        size = int.from_bytes(volume[offset+20:offset+23], 'little')
        assert 24 <= size <= len(volume)-offset
        if volume[offset+18] != 0xf0:
            guid = str(uuid.UUID(bytes_le=volume[offset:offset+16]))
            assert guid not in result
            result[guid] = volume[offset+24:offset+size]
        offset = (offset+size+7) & ~7
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--tables-dir', type=Path, required=True)
    args = parser.parse_args()
    build, tables = args.build_dir, args.tables_dir
    fd = (build/'FV/SILICIUM_UEFI.fd').read_bytes()
    outer = files(fd)
    guided = list(sections(outer['8642f9e8-2005-4199-89f2-0552c6810876']))
    assert len(guided) == 1 and guided[0][0] == 2
    payload = guided[0][1]
    assert payload[:16] == uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le
    offset = struct.unpack_from('<H', payload, 16)[0]-4
    assert 20 <= offset < len(payload)
    volumes = [d for k, d in sections(lzma.decompress(payload[offset:])) if k == 0x17]
    assert len(volumes) == 1
    assert volumes[0] == (build/'FV/FVMAIN.Fv').read_bytes()
    packaged = files(volumes[0])
    raws = [d for k, d in sections(packaged['dc48e1ab-ed92-4cb3-a12f-8149619187ef']) if k == 0x19]
    names = {b'FACP':'FADT.acpi', b'APIC':'MADT.acpi', b'GTDT':'GTDT.acpi', b'PPTT':'PPTT.acpi'}
    for path in sorted(tables.glob("*.aml")):
        table = path.read_bytes()
        signature = table[:4]
        assert len(table) >= 36 and signature in {b'DSDT', b'SSDT'}, path
        table_id = signature if signature == b'DSDT' else table[16:24].rstrip(b"\x00 ")
        assert table_id and table_id not in names, (table_id, path)
        names[table_id] = path.name
    assert b'DSDT' in names
    assert len(raws) == len(names), (len(raws), names)
    seen, result = set(), []
    for data in raws:
        signature = data[:4]
        table_id = data[16:24].rstrip(b"\x00 ") if signature == b'SSDT' else signature
        assert table_id in names and table_id not in seen, table_id
        seen.add(table_id)
        size = struct.unpack_from('<I', data, 4)[0]
        assert 36 <= size <= len(data)
        actual = bytearray(data[:size])
        expected = bytearray((tables/names[table_id]).read_bytes())
        assert size == len(expected)
        ignored = [9] + (list(range(32,36)) if signature in (b'DSDT', b'SSDT') else [])
        # Installer recomputes checksums; IASL version is not AML semantics.
        for index in ignored:
            actual[index] = expected[index] = 0
        assert actual == expected, signature
        result.append(dict(signature=signature.decode(), oem_table_id=data[16:24].decode().rstrip("\x00 "),
                           bytes=size, raw_bytes=len(data), ignored_header_offsets=ignored))
    drivers = []
    for guid, name in [('cb933912-df8f-4305-b1f9-7b44fa11395c','AcpiPlatform'), ('9622e42c-8e38-4a08-9e8f-54f784652f6b','AcpiTableDxe')]:
        records = list(sections(packaged[guid]))
        images = [d for k,d in records if k == 0x10]
        dependencies = [d for k,d in records if k == 0x13]
        assert len(images) == len(dependencies) == 1
        assert images[0] == (build/'AARCH64'/f'{name}.efi').read_bytes()
        if name == 'AcpiPlatform':
            assert dependencies[0] == (b'\x02'+uuid.UUID('ffe06bdd-6107-46a6-7bb2-5a9c7ec5275c').bytes_le
                + b'\x02'+uuid.UUID('13a3f0f6-264a-3ef0-f2e0-dec512342f34').bytes_le+b'\x03\x08')
            assert uuid.UUID('dc48e1ab-ed92-4cb3-a12f-8149619187ef').bytes_le in images[0]
        else:
            assert dependencies[0] == b'\x02'+uuid.UUID('13a3f0f6-264a-3ef0-f2e0-dec512342f34').bytes_le+b'\x08'
        drivers.append(dict(module=name, embedded_pe_matches_build=True, depex=dependencies[0].hex()))
    print(json.dumps(dict(status='pass', scope='existing FD/FV packaging; no DXE execution or device boot', fd_sha256=hashlib.sha256(fd).hexdigest(), tables=result, drivers=drivers), indent=2))


if __name__ == '__main__':
    main()
