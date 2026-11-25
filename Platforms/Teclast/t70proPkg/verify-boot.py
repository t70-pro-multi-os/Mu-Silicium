#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Execute actual T70Pro BootShim/SEC/DXE code; CPU leaves are modeled, not tablet devices."""
import argparse
import ctypes
import hashlib
import json
import lzma
import uuid
from pathlib import Path
import struct
import re
import runpy
import subprocess

import capstone
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE, UC_HOOK_CODE, UC_HOOK_BLOCK, UC_HOOK_MEM_INVALID, UC_MEM_WRITE, UcError, UC_ERR_EXCEPTION
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X30, UC_ARM64_REG_SP, UC_ARM64_REG_PC, UC_ARM64_REG_PSTATE

BASE, DEST, HANDOFF, DTB_BASE, DTB_END = (
    0x40000000, 0x44000000, 0x43F7F000, 0x47C80000, 0x48080000)


def sections(data):
    offset = 0
    while offset < len(data):
        assert offset + 4 <= len(data)
        size = int.from_bytes(data[offset:offset+3], "little")
        header = 4
        if size == 0xFFFFFF:
            size = struct.unpack_from("<I", data, offset+4)[0]
            header = 8
        assert header <= size <= len(data)-offset
        yield data[offset+3], data[offset+header:offset+size]
        offset = (offset+size+3) & ~3



def verify_keypad_pwrap_dependency(data):
    """Evaluate the actual packaged DXE dependency with/without PWRAP."""
    wanted = "b89e7e13-22dd-47ea-a62c-8ce18917d352"
    def evaluate(present):
        stack, pushed, offset = [], set(), 0
        while offset < len(data):
            opcode = data[offset]; offset += 1
            if opcode == 2:  # EFI_DEP_PUSH
                assert offset+16 <= len(data)
                guid = str(uuid.UUID(bytes_le=data[offset:offset+16]))
                offset += 16; pushed.add(guid)
                stack.append(present or guid != wanted)
            elif opcode == 6:  # EFI_DEP_TRUE
                stack.append(True)
            elif opcode == 3:  # EFI_DEP_AND
                assert len(stack) >= 2
                rhs = stack.pop(); stack[-1] = stack[-1] and rhs
            elif opcode == 8:  # EFI_DEP_END
                assert offset == len(data) and len(stack) == 1 and wanted in pushed
                return stack[0]
            else:
                raise AssertionError(("unexpected keypad dependency opcode", opcode))
        raise AssertionError("missing dependency END")
    assert evaluate(False) is False and evaluate(True) is True
    return {"PWRAP_protocol_required_before_dispatch":True,
            "protocol_GUID":wanted,"packaged_DXE_depex_sha256":hashlib.sha256(data).hexdigest(),
            "evaluation_without_PWRAP":False,"evaluation_with_PWRAP":True,
            "scope":"Actual packaged dependency bytecode, with other protocol dependencies assumed available; no DXE dispatch execution"}


def verify_direct_i2c_dependency(data):
    """Reject unavailable library dependencies in the actual I2C FFS image."""
    available = {"13a3f0f6-264a-3ef0-f2e0-dec512342f34",  # PCD
                 "26baccb1-6f42-11d4-bce7-0080c73c8881"}  # CPU architecture
    stack, pushed, offset = [], set(), 0
    while offset < len(data):
        opcode = data[offset]; offset += 1
        if opcode == 2:
            assert offset+16 <= len(data), "truncated I2C dependency GUID"
            guid = str(uuid.UUID(bytes_le=data[offset:offset+16])); offset += 16
            assert guid in available, ("I2C waits for unavailable protocol", guid)
            pushed.add(guid); stack.append(True)
        elif opcode == 6:
            stack.append(True)
        elif opcode == 3:
            assert len(stack) >= 2
            rhs = stack.pop(); stack[-1] = stack[-1] and rhs
        elif opcode == 8:
            assert offset == len(data) and stack == [True] and pushed == available
            return {"packaged_DXE_depex_sha256":hashlib.sha256(data).hexdigest(),
                    "required_protocols":sorted(pushed), "IOMMU_provider_required":False,
                    "scope":"Actual packaged dependency bytecode; physical dispatch checked separately"}
        else:
            raise AssertionError(("unexpected I2C dependency opcode", opcode))
    raise AssertionError("missing I2C dependency END")


def coff_procedure_matches(raw, function, linked_code):
    """Compare complete ARM64 COFF code; mask only declared relocation operands.

    Microsoft PE/COFF ARM64 relocation types; branch/address target values are
    checked by linker/runtime evidence separately, not by this byte comparison.
    Reference: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#arm64-processors
    """
    assert len(raw) >= 20 and struct.unpack_from("<H", raw)[0] == 0xaa64, (str(obj), "expected ARM64 COFF", raw[:4].hex())
    count = struct.unpack_from("<H", raw, 2)[0]
    sym_start, sym_count = struct.unpack_from("<II", raw, 8)
    assert 0 < count <= 4096 and struct.unpack_from("<H", raw, 16)[0] == 0
    assert 20 + count * 40 <= sym_start
    strings = sym_start + sym_count * 18
    assert sym_count > 0 and strings + 4 <= len(raw)
    strings_end = strings + struct.unpack_from("<I", raw, strings)[0]
    assert strings + 4 <= strings_end <= len(raw)
    matches, index = [], 0
    while index < sym_count:
        at = sym_start + index * 18
        name = raw[at:at+8]
        if name[:4] == bytes(4):
            offset = struct.unpack_from("<I", name, 4)[0]
            assert 4 <= offset < strings_end - strings
            end = raw.find(b"\0", strings+offset, strings_end)
            assert end >= strings+offset
            name = raw[strings+offset:end]
        else:
            name = name.split(b"\0", 1)[0]
        value, section, kind, storage, auxiliaries = struct.unpack_from("<IhHBB", raw, at+8)
        if name.decode() == function and section > 0 and kind == 0x20 and storage in (2, 3):
            matches.append((section, value))
        index += 1 + auxiliaries
    assert index == sym_count and len(matches) == 1, (function, matches)
    section, value = matches[0]
    assert section <= count and len(linked_code) > 0 and len(linked_code) % 4 == 0
    header = 20 + (section-1) * 40
    size, start, rel_start = struct.unpack_from("<III", raw, header+16)
    rel_count = struct.unpack_from("<H", raw, header+32)[0]
    flags = struct.unpack_from("<I", raw, header+36)[0]
    assert flags & 0x20000000 and not flags & 0x01000000
    assert start + size <= len(raw) and value + len(linked_code) <= size
    assert rel_start + rel_count * 10 <= len(raw)
    original = raw[start+value:start+value+len(linked_code)]
    masks = [0xffffffff] * (len(original)//4)
    operand_masks = {3:0x03ffffff, 4:0x60ffffe0, 5:0x60ffffe0,
                     6:0x003ffc00, 7:0x003ffc00, 9:0x003ffc00,
                     10:0x003ffc00, 11:0x003ffc00, 15:0x00ffffe0, 16:0x0007ffe0}
    relocations, occupied = [], set()
    for index in range(rel_count):
        offset, target, kind = struct.unpack_from("<IIH", raw, rel_start+index*10)
        assert target < sym_count
        if kind == 0 or not value <= offset < value+len(original):
            continue
        at = offset-value
        assert at % 4 == 0 and at+4 <= len(original) and at not in occupied
        assert kind in operand_masks, (function, "unsupported code relocation", kind)
        occupied.add(at)
        masks[at//4] &= ~operand_masks[kind] & 0xffffffff
        relocations.append({"offset":at, "type":kind})
    for index, mask in enumerate(masks):
        source = struct.unpack_from("<I", original, index*4)[0]
        target = struct.unpack_from("<I", linked_code, index*4)[0]
        assert ((source ^ target) & mask) == 0, (function, "COFF/PE opcode mismatch", index*4)
    return {"COFF_complete_code_matches":True,
            "COFF_unlinked_code_sha256":hashlib.sha256(original).hexdigest(),
            "COFF_code_relocations":relocations,
            "COFF_comparison_scope":"Complete procedure; only declared ARM64 relocation operand bits masked; relocation target values are not verified here"}


def codeview_inline_sites(obj):
    """Read LLVM COFF inline records; do not guess a missing entry's address.

    Layouts: llvm.org/docs/PDB/CodeViewTypes.html, CodeViewSymbols.html;
    llvm/lib/DebugInfo/CodeView/{Type,Symbol}RecordMapping.cpp.
    """
    raw = obj.read_bytes()
    assert len(raw) >= 20 and struct.unpack_from("<H", raw)[0] == 0xaa64, (str(obj), "expected ARM64 COFF", raw[:4].hex())
    count = struct.unpack_from("<H", raw, 2)[0]
    assert 0 < count <= 4096 and 20 + count * 40 <= len(raw)
    assert struct.unpack_from("<H", raw, 16)[0] == 0
    chunks = []
    for index in range(count):
        offset = 20 + index * 40
        name = raw[offset:offset+8].rstrip(b"\0")
        size, start = struct.unpack_from("<II", raw, offset+16)
        assert start + size <= len(raw)
        chunks.append((name, raw[start:start+size]))
    streams = [data for name, data in chunks if name == b".debug$T"]
    assert len(streams) == 1
    types = streams[0]
    assert types[:4] == b"\x04\0\0\0"
    offset, type_index, names = 4, 0x1000, {}
    while offset < len(types):
        length, kind = struct.unpack_from("<HH", types, offset)
        assert length >= 2 and offset+2+length <= len(types)
        if kind == 0x1601:  # LF_FUNC_ID: scope, function type, zero-ended name.
            assert length >= 11
            names[type_index] = types[offset+12:offset+2+length].split(b"\0", 1)[0].decode()
        offset += 2 + length
        type_index += 1
    found, procedure_bytes = {}, {}
    for name, data in chunks:
        if name != b".debug$S":
            continue
        assert data[:4] == b"\x04\0\0\0"
        offset = 4
        while offset < len(data):
            assert offset+8 <= len(data)
            kind, length = struct.unpack_from("<II", data, offset)
            assert offset+8+length <= len(data)
            symbols = data[offset+8:offset+8+length]
            if kind == 0xf1:  # DEBUG_S_SYMBOLS
                position, parent = 0, None
                while position < len(symbols):
                    size, typ = struct.unpack_from("<HH", symbols, position)
                    assert size >= 2 and position+2+size <= len(symbols)
                    payload = symbols[position+4:position+2+size]
                    if typ in (0x110f, 0x1110, 0x1146, 0x1147):
                        assert len(payload) >= 36
                        parent = payload[35:].split(b"\0", 1)[0].decode()
                        procedure_bytes[parent] = struct.unpack_from("<I", payload, 12)[0]
                    if typ == 0x114d:  # S_INLINESITE: parent, end, inlinee type ID.
                        assert len(payload) >= 12 and parent is not None
                        inlinee = struct.unpack_from("<I", payload, 8)[0]
                        assert inlinee in names
                        found.setdefault(names[inlinee], set()).add(parent)
                    position += 2 + size
            offset = (offset+8+length+3) & ~3
    return {name: sorted(parents) for name, parents in found.items()}, procedure_bytes



def verify_complete_owning_procedures(build, name, relative, owners, required):
    """Bind every emitted procedure in the changed owning objects to final PE."""
    pe = (build / "AARCH64" / (name + ".efi")).read_bytes()
    link_map = build / "AARCH64" / relative / "DEBUG" / (name + ".map")
    map_text = link_map.read_text()
    records = [(m[2], int(m[1], 16)) for m in re.finditer(
        r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$", map_text, re.M)]
    header = struct.unpack_from("<I", pe, 0x3c)[0]
    assert pe[:2] == b"MZ" and pe[header:header+4] == b"PE\0\0"
    assert struct.unpack_from("<H", pe, header+4)[0] == 0xaa64
    count = struct.unpack_from("<H", pe, header+6)[0]
    optional = struct.unpack_from("<H", pe, header+20)[0]
    complete = {}
    for owner, relative_object in owners.items():
        obj = build / "AARCH64" / relative_object
        _, procedures = codeview_inline_sites(obj)
        regions = {int(m[1], 16): int(m[2], 16) for m in re.finditer(
            r"^([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+4\s+.*?/" +
            re.escape(owner) + r"\.obj:\(\.text\)\s*$", map_text, re.M)
            if int(m[2], 16)}
        assert regions, (name, owner, "owning text regions missing")
        emitted = {(function, address) for function, address in records
                   if address in regions and function in procedures}
        assert set(regions) <= {address for _, address in emitted}, (
            name, owner, "unresolved owning procedure")
        for function, rva in sorted(emitted):
            size = procedures[function]
            assert size == regions[rva] and size > 0 and size % 4 == 0
            matches = []
            for index in range(count):
                at = header + 24 + optional + index * 40
                virtual_size, va, raw_size, raw_offset = struct.unpack_from("<4I", pe, at+8)
                flags = struct.unpack_from("<I", pe, at+36)[0]
                if va <= rva and rva+size <= va+min(virtual_size, raw_size):
                    assert flags & 0x20000000
                    offset = raw_offset + rva - va
                    assert offset+size <= len(pe)
                    matches.append(pe[offset:offset+size])
            assert len(matches) == 1, (name, owner, function, "PE code section")
            code = matches[0]
            complete[owner + "/" + function] = {
                "rva": rva, "bytes": size,
                "object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest(),
                "code_sha256": hashlib.sha256(code).hexdigest(),
                **coff_procedure_matches(obj.read_bytes(), function, code)}
    assert set(required) <= set(complete), (name, "required emitted callback", set(required)-set(complete))
    return {"PE_sha256": hashlib.sha256(pe).hexdigest(),
            "map_sha256": hashlib.sha256(link_map.read_bytes()).hexdigest(),
            "complete_owning_procedure_regions": complete,
            "required_callbacks": sorted(required),
            "scope": "Complete code of every separately linked procedure in the named owning objects; only declared ARM64 relocation operand bits masked. Inlined helpers and relocation targets are not independently proved here. Native C behavior, DXE execution and hardware execution are separate."}

def verify_panel_board_protocol(pe, linkage):
    """Match the actual protocol revision, all five callback pointers and GUID."""
    header=struct.unpack_from("<I",pe,0x3c)[0]
    optional=header+24
    assert struct.unpack_from("<H",pe,optional)[0]==0x20b
    image_base=struct.unpack_from("<Q",pe,optional+24)[0]
    methods=("PanelRunInit","PanelRunSuspend","PanelRunKernelPrepare",
             "PanelRunKernelEnable","PanelRunKernelUnprepare")
    regions=linkage["complete_owning_procedure_regions"]
    callbacks={name:regions["GpioPlatformDxe/"+name]["rva"] for name in methods}
    expected=struct.pack("<II5Q",2,0,*(image_base+callbacks[name] for name in methods))
    assert pe.count(expected)==1,("panel protocol revision/callback table",callbacks)
    guid="5a1e7c32-b4d6-4ba9-9c6f-2056e397a1d4"
    assert uuid.UUID(guid).bytes_le in pe
    return {"revision":2,"protocol_GUID":guid,"complete_callback_table_bytes":len(expected),
            "table_file_offset":pe.index(expected),"callback_RVAs":callbacks,
            "table_sha256":hashlib.sha256(expected).hexdigest(),
            "scope":"Actual linked PE protocol data and separately verified callback code; publication execution and hardware power sequencing are separate"}


def verify_prm_module(pe, module_guid):
    """Validate the actual ARM64 PRM exports and runtime section layout."""
    assert pe[:2] == b"MZ" and len(pe) >= 0x40
    at = struct.unpack_from("<I", pe, 0x3c)[0]
    assert at+24 <= len(pe) and pe[at:at+4] == b"PE\0\0"
    machine, count = struct.unpack_from("<HH", pe, at+4)
    optional_size = struct.unpack_from("<H", pe, at+20)[0]
    optional = at+24
    assert machine == 0xaa64 and 0 < count <= 96 and optional_size >= 120
    assert optional+optional_size+count*40 <= len(pe)
    assert struct.unpack_from("<H", pe, optional)[0] == 0x20b
    assert struct.unpack_from("<H", pe, optional+68)[0] == 12, "PRM must remain a runtime driver"
    alignment = struct.unpack_from("<I", pe, optional+32)[0]
    assert alignment == 0x10000, "ARM64 PRM sections require 64 KiB alignment"
    image_size, headers = struct.unpack_from("<II", pe, optional+56)
    assert 0 < headers <= len(pe) and headers <= image_size
    regions = []
    for index in range(count):
        off = optional+optional_size+index*40
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", pe, off+8)
        flags = struct.unpack_from("<I", pe, off+36)[0]
        extent = max(virtual_size, raw_size)
        assert rva % alignment == 0 and rva+extent <= image_size
        assert raw+raw_size <= len(pe)
        assert not ((flags & 0x20000000) and (flags & 0x80000000)), "runtime W+X section"
        for first, last, *_ in regions:
            assert rva+extent <= first or last <= rva, "overlapping PE sections"
        regions.append((rva, rva+extent, raw, raw_size, flags))
    def read_rva(rva, length):
        assert 0 < length <= image_size and rva+length <= image_size
        for first, last, raw, raw_size, _ in regions:
            if first <= rva and rva+length <= last:
                assert rva-first+length <= raw_size, "PRM metadata in unbacked section"
                start = raw+rva-first
                return pe[start:start+length]
        raise AssertionError(("RVA outside a section", rva, length))
    assert struct.unpack_from("<I", pe, optional+108)[0] >= 1
    export_rva, export_size = struct.unpack_from("<II", pe, optional+112)
    assert export_rva and export_size >= 40
    descriptor = read_rva(export_rva, 40)
    base, functions, names, function_table, name_table, ordinal_table = struct.unpack_from("<6I", descriptor, 16)
    assert 0 < names <= functions <= 64
    addresses = struct.unpack("<"+"I"*functions, read_rva(function_table, functions*4))
    name_rvas = struct.unpack("<"+"I"*names, read_rva(name_table, names*4))
    ordinals = struct.unpack("<"+"H"*names, read_rva(ordinal_table, names*2))
    exports = {}
    for name_rva, ordinal in zip(name_rvas, ordinals):
        name = bytearray()
        for byte_index in range(128):
            value = read_rva(name_rva+byte_index, 1)[0]
            if value == 0:
                break
            name.append(value)
        else:
            raise AssertionError("unterminated PRM export name")
        text = name.decode("ascii")
        assert text and text not in exports and ordinal < functions
        target = addresses[ordinal]
        assert target and not export_rva <= target < export_rva+export_size, "forwarded PRM export"
        read_rva(target, 1)
        exports[text] = target
    assert "PrmModuleExportDescriptor" in exports
    descriptor_header = read_rva(exports["PrmModuleExportDescriptor"], 44)
    assert descriptor_header[:8] == b"PRM_MEDT"
    revision, handler_count = struct.unpack_from("<HH", descriptor_header, 8)
    assert revision == 0 and handler_count == 2
    handler_entry_size = 16 + 128  # GUID plus PRM_HANDLER_NAME_MAXIMUM_LENGTH.
    descriptor_bytes = 44 + handler_count * handler_entry_size
    raw = read_rva(exports["PrmModuleExportDescriptor"], descriptor_bytes)
    assert raw[12:28] == uuid.UUID("a16813b1-6789-4b71-90df-d68d15d18156").bytes_le
    assert raw[28:44] == uuid.UUID(module_guid).bytes_le
    expected_handlers = {
        uuid.UUID("c9aef12d-e26c-4be7-b345-62c5f306d184"): "MtkUfsPrmHandler",
        uuid.UUID("9f58ca73-6bc4-45b8-b69c-f33c6261d081"): "MtkPmicPrmHandler",
    }
    handlers = []
    for index in range(handler_count):
        entry = 44 + index * handler_entry_size
        handler_guid = uuid.UUID(bytes_le=raw[entry:entry+16])
        name_bytes = raw[entry+16:entry+handler_entry_size]
        zero = name_bytes.index(0)
        assert zero and not any(name_bytes[zero:]), "noncanonical PRM handler name"
        handler = name_bytes[:zero].decode("ascii")
        assert handler_guid in expected_handlers and expected_handlers[handler_guid] == handler
        assert handler in exports
        code_rva = exports[handler]
        assert any(first <= code_rva < last and flags & 0x20000000
                   for first, last, _, _, flags in regions), "PRM handler export is not executable"
        handlers.append({"guid": str(handler_guid), "name": handler, "code_rva": code_rva})
    assert {uuid.UUID(item["guid"]) for item in handlers} == set(expected_handlers)
    assert {item["name"] for item in handlers} == set(expected_handlers.values())
    return {"status":"passed", "runtime_subsystem":12, "section_alignment":alignment,
            "module_guid":module_guid.lower(), "handler_count":handler_count,
            "handler_descriptor_bytes":descriptor_bytes, "handlers":handlers,
            "exports":exports, "image_bytes":len(pe),
            "sha256":hashlib.sha256(pe).hexdigest(),
            "scope":"actual PE runtime/ARM64/two-handler MEDT descriptors and embedded image identity; no OS or hardware execution"}


def packaged_content(fd, build):
    # Follow the FDF's LZMA-guided FV image inside the actual FD.
    start = fd.index(uuid.UUID("8642F9E8-2005-4199-89F2-0552C6810876").bytes_le) + 24
    size = int.from_bytes(fd[start:start+3], "little")
    assert fd[start+3] == 2 and start+size <= len(fd)
    data_offset = struct.unpack_from("<H", fd, start+20)[0]
    assert 24 <= data_offset < size
    unpacked = lzma.decompress(fd[start+data_offset:start+size])
    volumes = [data for kind, data in sections(unpacked) if kind == 0x17]
    assert len(volumes) == 1
    fv = volumes[0]
    volume_access = []
    for name, volume in (("FVMAIN_COMPACT",fd),("FVMAIN",fv)):
        attributes = struct.unpack_from("<I",volume,44)[0]
        header_size = struct.unpack_from("<H",volume,48)[0]
        assert attributes & 6 == 6, (name,"firmware volume read access disabled")
        assert attributes & 0x20 == 0, (name,"firmware volume must not be writable")
        assert sum(struct.unpack("<"+"H"*(header_size//2),volume[:header_size])) & 0xffff == 0
        volume_access.append({"volume":name,"attributes":hex(attributes),"read_enabled":True,"write_enabled":False,"header_checksum_valid":True})
    assert fv[40:44] == b"_FVH" and struct.unpack_from("<Q", fv, 32)[0] == len(fv)
    current_fv = build / "FV/FVMAIN.Fv"
    if current_fv.exists():
        assert fv == current_fv.read_bytes(), "FD contains a different FV than this build"
    offset = (struct.unpack_from("<H", fv, 48)[0]+7) & ~7
    files = {}
    while offset+24 <= len(fv) and fv[offset:offset+24] != b"\xff"*24:
        size = int.from_bytes(fv[offset+20:offset+23], "little")
        assert 24 <= size <= len(fv)-offset
        guid = str(uuid.UUID(bytes_le=fv[offset:offset+16]))
        if fv[offset+18] != 0xF0:
            files[guid] = fv[offset+24:offset+size]
        offset = (offset+size+7) & ~7
    acpi = files["dc48e1ab-ed92-4cb3-a12f-8149619187ef"]
    expected = {}
    for path in list(build.rglob("*.acpi")) + list(build.rglob("DSDT.aml")):
        data = path.read_bytes()
        if data[:4] in (b"FACP",b"GTDT",b"APIC",b"PPTT",b"DSDT"):
            expected[data[:4]] = data
    assert len(expected) == 5
    found = []
    for kind, data in sections(acpi):
        if kind != 0x19 or data[:4] not in expected:
            continue
        size = struct.unpack_from("<I", data, 4)[0]
        actual = bytearray(data[:size]); wanted = bytearray(expected[data[:4]])
        # AcpiPlatformDxe finalizes checksum when installing the table.
        actual[9] = wanted[9] = 0
        assert actual == wanted
        found.append(data[:4].decode())
    assert set(found) == {"FACP","GTDT","APIC","PPTT","DSDT"}
    drivers = {}
    repo = Path(__file__).resolve().parents[3]
    roots = {"MediaTekPkg": repo/"Silicon/MediaTek/MediaTekPkg", "t70proPkg": Path(__file__).resolve().parent,
             "SiliciumPkg": repo/"Silicon/Silicium/SiliciumPkg"}
    dsc = (roots["t70proPkg"]/"t70pro.dsc").read_text()
    components = re.findall(r"^\s*((?:MediaTekPkg|t70proPkg|SiliciumPkg)/Drivers/[^\s{]+\.inf)", dsc, re.M)
    fdf = (roots["t70proPkg"]/"t70pro.fdf").read_text()
    dispatched = set(re.findall(r"^\s*INF\s+((?:MediaTekPkg|t70proPkg|SiliciumPkg)/Drivers/[^\s{]+\.inf)", fdf, re.M))
    assert dispatched <= set(components), "FDF driver missing from platform components"
    assert len(components) == len(set(components))
    component_guids = set()
    for component in components:
        package, relative = component.split("/", 1)
        inf_path = roots[package]/relative
        inf = inf_path.read_text()
        name = re.search(r"BASE_NAME\s*=\s*(\w+)", inf)[1]
        guid = re.search(r"FILE_GUID\s*=\s*([0-9A-Fa-f-]+)",inf)[1].lower()
        assert guid not in component_guids, (component, "duplicate driver FILE_GUID")
        component_guids.add(guid)
        if component == "t70proPkg/Drivers/BootStatusDxe/BootStatusDxe.inf":
            assert guid in files
            continue
        path = build/"AARCH64"/(name+".efi")
        pe = path.read_bytes(); assert pe[:2] == b"MZ"
        offset = struct.unpack_from("<I",pe,0x3C)[0]
        assert pe[offset:offset+4] == b"PE\0\0"
        assert struct.unpack_from("<H",pe,offset+4)[0] == 0xAA64
        assert struct.unpack_from("<H",pe,offset+24)[0] == 0x20B
        expected_subsystem = 12 if re.search(r"MODULE_TYPE\s*=\s*DXE_RUNTIME_DRIVER\b", inf) else 11
        assert struct.unpack_from("<H",pe,offset+24+68)[0] == expected_subsystem
        in_fv = component in dispatched
        if in_fv:
            assert guid in files, (name, "driver missing from dispatched FV")
            payloads = [data for kind, data in sections(files[guid]) if kind == 0x10]
            assert len(payloads) == 1 and payloads[0] == pe, (name, "embedded board driver PE mismatch")
        else:
            assert guid not in files, (name, guid, "unexpected peripheral in dispatched FV")
        drivers[name] = {"bytes":len(pe),"sha256":hashlib.sha256(pe).hexdigest(),"in_fv":in_fv}
        if name == "KeypadDeviceDxe":
            depex = [data for kind,data in sections(files[guid]) if kind == 0x13]
            assert in_fv and len(depex)==1
            drivers[name]["physical_button_dependency"] = verify_keypad_pwrap_dependency(depex[0])
        if name == "I2cDxe":
            depex = [data for kind,data in sections(files[guid]) if kind == 0x13]
            assert in_fv and len(depex)==1
            drivers[name]["direct_APDMA_dispatch_dependency"] = verify_direct_i2c_dependency(depex[0])
    # Compare every required standard driver against the actual decompressed
    # FV: a compiled driver outside the image cannot provide console or files.
    standard_drivers = {}
    required = [
        "MdeModulePkg/Universal/PCD/Dxe/Pcd.inf",
        "ArmPkg/Drivers/CpuDxe/CpuDxe.inf",
        "ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf",
        "ArmPkg/Drivers/TimerDxe/TimerDxe.inf",
        "MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf",
        "MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf",
        "MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf",
        "MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf",
        "MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf",
        "MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf",
        "MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf",
        "EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf",
        "EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf",
        "EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf",
        "MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf",
        "MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf",
        "MdeModulePkg/Universal/BdsDxe/BdsDxe.inf",
        "MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf",
        "MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf",
        "MdeModulePkg/Bus/Usb/UsbMouseDxe/UsbMouseDxe.inf",
        "MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf",
        "MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf",
        "MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf",
        "MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf",
        "FatPkg/EnhancedFatDxe/Fat.inf",
        "MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf",
        "MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf",
        "MdeModulePkg/Bus/Sd/SdDxe/SdDxe.inf",
        "MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf",
        "MdeModulePkg/Universal/PrintDxe/PrintDxe.inf",
        "MdeModulePkg/Universal/SetupBrowserDxe/SetupBrowserDxe.inf",
        "MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf",
        "MdeModulePkg/Application/UiApp/UiApp.inf",
        "ShellPkg/Application/Shell/Shell.inf",
        "MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf",
        "PrmPkg/PrmConfigDxe/PrmConfigDxe.inf",
        "PrmPkg/PrmLoaderDxe/PrmLoaderDxe.inf",
    ]
    assert "ScsiDiskDxe/ScsiDiskDxe.inf" not in fdf, "writable internal disk driver must not be dispatched"
    for component in required:
        assert "INF " + component in fdf
        inf = (repo / "Mu_Basecore" / component).read_text()
        name = re.search(r"BASE_NAME\s*=\s*(\w+)", inf)[1]
        guid = re.search(r"FILE_GUID\s*=\s*([0-9A-Fa-f-]+)", inf)[1].lower()
        payloads = [data for kind, data in sections(files[guid]) if kind == 0x10]
        pe = (build / "AARCH64" / (name + ".efi")).read_bytes()
        assert len(payloads) == 1 and payloads[0] == pe, (name, "embedded boot-stack PE mismatch")
        pe_offset = struct.unpack_from("<I", pe, 0x3c)[0]
        assert pe[:2] == b"MZ" and pe[pe_offset:pe_offset+4] == b"PE\0\0"
        assert struct.unpack_from("<H", pe, pe_offset+4)[0] == 0xAA64
        module_type = re.search(r"MODULE_TYPE\s*=\s*(\w+)", inf)[1]
        expected_subsystem = 10 if module_type == "UEFI_APPLICATION" else (12 if module_type == "DXE_RUNTIME_DRIVER" else 11)
        subsystem = struct.unpack_from("<H", pe, pe_offset+24+68)[0]
        assert subsystem == expected_subsystem, (name, module_type, subsystem)
        standard_drivers[name] = {"bytes":len(pe), "sha256":hashlib.sha256(pe).hexdigest(),
                                  "PE_subsystem":subsystem,
                                  "in_fv":True, "hardware_tested":False}
    hid_drivers = {name:standard_drivers[name] for name in ("UsbBusDxe", "UsbKbDxe", "UsbMouseDxe")}
    standard_drivers["Shell"]["firmware_volume_access"] = volume_access
    policy_component = "MdeModulePkg/Universal/BdsDxe/BdsDxe.inf"
    assert "INF " + policy_component in fdf
    policy_inf = (repo / "Mu_Basecore" / policy_component).read_text()
    policy_guid = re.search(r"FILE_GUID\s*=\s*([0-9A-Fa-f-]+)", policy_inf)[1].lower()
    policy_pe = (build / "AARCH64/BdsDxe.efi").read_bytes()
    policy_payloads = [data for kind, data in sections(files[policy_guid]) if kind == 0x10]
    assert len(policy_payloads) == 1 and policy_payloads[0] == policy_pe, "Embedded board boot policy mismatch"
    policy_pe_offset = struct.unpack_from("<I", policy_pe, 0x3c)[0]
    assert policy_pe[:2] == b"MZ" and policy_pe[policy_pe_offset:policy_pe_offset+4] == b"PE\0\0"
    assert struct.unpack_from("<H", policy_pe, policy_pe_offset+4)[0] == 0xAA64
    policy_map = build / "AARCH64/MdeModulePkg/Universal/BdsDxe/BdsDxe/DEBUG/BdsDxe.map"
    map_text = policy_map.read_text()
    policy_symbols = ["DeviceBootManagerBeforeConsole", "DeviceBootManagerAfterConsole",
                      "DeviceBootManagerOnDemandConInConnect", "ConnectBoardDevices",
                      "RegisterConsoleHandles", "gEfiSimpleTextInProtocolGuid", "gEfiSimpleTextOutProtocolGuid"]
    for symbol in policy_symbols:
        assert re.search(r"^\s*[0-9a-fA-F]+\s+00000000\s+0\s+" + symbol + r"\s*$", map_text, re.M), symbol
    boot_policy = {"bytes":len(policy_pe), "sha256":hashlib.sha256(policy_pe).hexdigest(),
                   "in_fv":True, "linked_symbols":policy_symbols,
                   "link_map_sha256":hashlib.sha256(policy_map.read_bytes()).hexdigest(),
                   "hardware_tested":False}
    reset_component = "MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf"
    assert "INF " + reset_component in fdf
    reset_inf = (repo / "Mu_Basecore" / reset_component).read_text()
    reset_guid = re.search(r"FILE_GUID\s*=\s*([0-9A-Fa-f-]+)", reset_inf)[1].lower()
    reset_pe = (build / "AARCH64/ResetSystemRuntimeDxe.efi").read_bytes()
    reset_payloads = [data for kind, data in sections(files[reset_guid]) if kind == 0x10]
    assert len(reset_payloads) == 1 and reset_payloads[0] == reset_pe, "Embedded reset runtime mismatch"
    reset_driver = {"bytes":len(reset_pe), "sha256":hashlib.sha256(reset_pe).hexdigest(),
                    "in_fv":True, "hardware_tested":False}
    prm_inf = (roots["MediaTekPkg"]/"Drivers/MtkUfsPrm/MtkUfsPrm.inf").read_text()
    assert "MtkPmicBroker.c" in prm_inf
    prm_guid = re.search(r"FILE_GUID\s*=\s*([0-9A-Fa-f-]+)", prm_inf)[1]
    prm_pe = (build/"AARCH64/MtkUfsPrm.efi").read_bytes()
    prm_module = verify_prm_module(prm_pe, prm_guid)
    assert drivers["MtkUfsPrm"]["in_fv"]
    # Match the private controller ABI in both actual PE images, and require
    # each newly integrated function to resolve into executable ARM64 bytes.
    power_linkage = {}
    pll_linkage = {}
    cold_linkage = {}
    ufs_linkage = {}
    cold_functions = [
        "EndOfDxeNotify", "UsbRoleTick", "ColdControllerGuard", "ColdSetPhyMode", "BeforeExitNotify",
        "MtkMusbPlatformAdvanceColdHost", "MtkMusbPlatformStopColdHost",
        "MtkHusb311TypecInitialize", "MtkHusb311TypecBeginSourceProbe",
        "MtkHusb311TypecPollSourceProbe", "MtkHusb311TypecStepCleanup",
        "MtkHusb311TypecValidateSource", "MtkHusb311TypecTokenActive",
    ]
    exclusive_guid = uuid.UUID("d7d47231-8e82-4426-a29d-4be2667b9d31").bytes_le
    linked = {
        "I2cDxe": ("MediaTekPkg/Drivers/I2cDxe/I2cDxe", [
            "AcquireExclusive", "TransferExclusive", "ReleaseExclusive",
            "RegisterExitObserver", "UnregisterExitObserver"]),
        "MusbHandoffDxe": ("t70proPkg/Drivers/MusbHandoffDxe/MusbHandoffDxe", [
            "AcquirePowerBus", "ReleasePowerBus", "CapturePowerBeforeI2cExit",
            "OtgFreshEvidence", "OtgUpdateRegister", "OtgWriteGpio",
            "MtkSc8989xOtgInitialize", "MtkSc8989xOtgEnableCold",
            "MtkSc8989xOtgDisable", "MtkSc8989xOtgRetryCleanup",
            "SpHusb311VbusSafe0V"] + cold_functions),
        "ClockDxe": ("MediaTekPkg/Drivers/ClockDxe/ClockDxe", [
            "ClockSetEnable", "ClockIsEnabled"]),
        "UfsPassThruDxe": ("MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe", [
            "UfsPassThruDriverBindingStart", "UfsPassThruDriverBindingStop",
            "UfsPassThruPassThru", "UfsRwUfsDescriptor", "UfsRwUfsFlag",
            "UfsRwUfsAttribute", "UfsControllerStop", "ProcessAsyncTaskList",
            "UfsExecScsiCmds", "UfsSendDmRequestRetry", "UfsExecNopCmds",
            "UfsAbortAsyncRequests", "UfsEnterCall", "UfsLeaveCall",
            "UfsCleanupPrivate", "UfsPrepareDevicePowerDown",
            "UfsProcessAsyncTasks", "UfsExecScsiCmdsInternal", "UfsValidateScsiResponse",
            "UfsHcDriverInterfaceExecUicCommand"]),
        "MtkUfsHandoffDxe": ("MediaTekPkg/Drivers/MtkUfsHandoffDxe/MtkUfsHandoffDxe", [
            "MtkUfsDriverStart", "MtkUfsExitBootServices", "MtkUfsConfigureColdHost",
            "MtkUfsReleaseUnstartedColdHost", "MtkUfsValidateRetainedHost",
            "MtkUfsColdHostCallback", "MtkUfsColdHostSetManufacturer",
            "MtkUfsPlatformRestartStopped", "MtkUfsDriverStop",
            "MtkUfsAllocateBuffer", "MtkUfsFreeBuffer", "MtkUfsMap", "MtkUfsUnmap",
            "MtkUfsReadMmio", "MtkUfsWriteMmio", "MtkUfsFlush",
            "MtkUfsGetMmioBar", "MtkUfsPlatformCallback",
            "MtkUfsColdReleaseStartedHost", "MtkUfsPlatformPrepareStop",
            "MtkUfsPlatformHostStopped"]),
    }
    for name, (relative, functions) in linked.items():
        pe = (build / "AARCH64" / (name + ".efi")).read_bytes()
        link_map = build / "AARCH64" / relative / "DEBUG" / (name + ".map")
        symbol_records = [(m[2],int(m[1],16)) for m in re.finditer(
            r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$", link_map.read_text(), re.M)]
        symbols = dict(symbol_records)
        supporting = []
        if name == "MtkUfsHandoffDxe":
            # Discover every separately linked procedure from this provider's
            # owning object. Inlined or discarded functions have no separate
            # region and are not claimed as independently compared.
            provider_regions = {int(match[1], 16) for match in re.finditer(
                r"^([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+4\s+.*?/MtkUfsHandoffDxe\.obj:\(\.text\)\s*$",
                link_map.read_text(), re.M) if int(match[2], 16)}
            assert provider_regions, (name, "owning provider text regions missing")
            named_regions = {address for function, address in symbol_records
                             if function.startswith("MtkUfs")}
            assert provider_regions <= named_regions, (
                name, "provider text region lacks a separately checked procedure",
                sorted(provider_regions - named_regions))
            supporting = sorted({function for function, address in symbol_records
                                 if function.startswith("MtkUfs") and
                                 address in provider_regions and function not in functions})
            functions = functions + supporting
        if name in ("MtkUfsHandoffDxe", "UfsPassThruDxe"):
            # Stop/retry helpers may be separately emitted in any owning object.
            # Compare those regions too; comparing a caller's masked branch
            # does not prove the procedure at its relocation target.
            owners = (("MtkUfsColdHost", "MtkUfsPlatform") if name == "MtkUfsHandoffDxe" else
                      ("UfsPassThru", "UfsPassThruHci", "UfsDevConfigProtocol"))
            for owner in owners:
                obj = (build / "AARCH64/MediaTekPkg/Library/UfsCoreLib/MtkUfsCoreLib/OUTPUT/MtkUfsPlatform.obj"
                       if owner == "MtkUfsPlatform" else
                       link_map.parent.parent / "OUTPUT" / (owner + ".obj"))
                _, procedures = codeview_inline_sites(obj)
                regions = {int(match[1], 16) for match in re.finditer(
                    r"^([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+4\s+.*?/" +
                    re.escape(owner) + r"\.obj:\(\.text\)\s*$",
                    link_map.read_text(), re.M) if int(match[2], 16)}
                assert regions, (name, owner, "owning text regions missing")
                emitted = {function: address for function, address in symbol_records
                           if address in regions and function in procedures}
                assert regions <= set(emitted.values()), (
                    name, owner, "text region lacks a named COFF procedure",
                    sorted(regions - set(emitted.values())))
                extra = sorted(set(emitted) - set(functions))
                supporting.extend(extra)
                functions.extend(extra)
        header = struct.unpack_from("<I", pe, 0x3c)[0]
        count, opt_bytes = struct.unpack_from("<H", pe, header+6)[0], struct.unpack_from("<H", pe, header+20)[0]
        def at_rva(rva, length, executable=False):
            for index in range(count):
                section = header+24+opt_bytes+index*40
                virtual_size, va, raw_size, raw_offset = struct.unpack_from("<4I", pe, section+8)
                flags = struct.unpack_from("<I", pe, section+36)[0]
                if va <= rva and rva+length <= va+min(virtual_size, raw_size):
                    assert not executable or flags & 0x20000000, (name, hex(rva), "non-code link target")
                    offset = raw_offset+rva-va
                    assert offset+length <= len(pe)
                    return pe[offset:offset+length]
            raise AssertionError((name, hex(rva), "link target outside PE sections"))
        if name in ("I2cDxe", "MusbHandoffDxe"):
            guid_rva = symbols["gMediaTekI2cExclusiveProtocolGuid"]
            assert at_rva(guid_rva, 16) == exclusive_guid, (name, "private I2C ABI mismatch")
        resolved = {}
        for function in functions:
            rva = symbols[function]
            assert rva % 4 == 0
            code = at_rva(rva, 16, executable=True)
            assert code != bytes(16)
            resolved[function] = {"rva":rva, "first_16_bytes_sha256":hashlib.sha256(code).hexdigest()}
        item = {"PE_sha256":hashlib.sha256(pe).hexdigest(),
                "map_sha256":hashlib.sha256(link_map.read_bytes()).hexdigest(),
                "resolved_functions":resolved, "in_fv":(drivers if name in drivers else standard_drivers)[name]["in_fv"],
                "full_driver_execution_tested":False}
        if name == "ClockDxe":
            pll_linkage[name] = item
        elif name in ("MtkUfsHandoffDxe", "UfsPassThruDxe"):
            # Resolve complete procedures from their owning COFF, not merely
            # a same-named static symbol elsewhere in the PE.
            complete = {}
            object_names = (("MtkUfsHandoffDxe", "MtkUfsColdHost", "MtkUfsPlatform") if name == "MtkUfsHandoffDxe" else
                            ("UfsPassThru", "UfsPassThruHci", "UfsDevConfigProtocol"))
            for object_name in object_names:
                obj = (build / "AARCH64/MediaTekPkg/Library/UfsCoreLib/MtkUfsCoreLib/OUTPUT/MtkUfsPlatform.obj"
                       if object_name == "MtkUfsPlatform" else
                       link_map.parent.parent / "OUTPUT" / (object_name + ".obj"))
                inline_sites, procedure_bytes = codeview_inline_sites(obj)
                text_regions = {int(match[1], 16): int(match[2], 16) for match in re.finditer(
                    r"^([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+4\s+.*?/" +
                    re.escape(object_name) + r"\.obj:\(\.text\)\s*$", link_map.read_text(), re.M)
                    if int(match[2], 16)}
                for function in functions:
                    candidates = {address for symbol, address in symbol_records
                                  if symbol == function and address in text_regions}
                    if not candidates: continue
                    assert len(candidates) == 1 and function in procedure_bytes, (function, candidates)
                    rva = next(iter(candidates))
                    size = procedure_bytes[function]
                    assert size == text_regions[rva] and size % 4 == 0, (function, size)
                    code = at_rva(rva, size, executable=True)
                    complete[function] = {"rva": rva, "bytes": size,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "COFF_procedure_size_matches": True,
                        "COFF_object": object_name,
                        "COFF_object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest(),
                        **coff_procedure_matches(obj.read_bytes(), function, code)}
            assert set(complete) == set(functions), set(functions) - set(complete)
            item["complete_procedure_regions"] = {
                function: region for function, region in complete.items()
                if function not in supporting}
            item["supporting_lifecycle_procedure_regions"] = {
                function: complete[function] for function in supporting}
            item["supporting_lifecycle_scope"] = (
                "Complete code of every separately linked procedure in all owning provider/platform "
                "or consumer objects; helpers without standalone symbols are not claimed separately")
            if name == "MtkUfsHandoffDxe":
                sleep_names = ("MtkUfsColdHostSuspend", "MtkUfsColdHostResume",
                               "MtkUfsPlatformSuspend", "MtkUfsPlatformResume")
                item["sleep_functions_linked"] = {function: function in symbols for function in sleep_names}
                item["sleep_scope"] = "Portable C APIs have host tests; no OS power consumer or firmware sleep callback is implied by this boot-path proof"
            else:
                item["lifecycle_scope"] = "Actual consumer Start/Stop, SCSI/config/async entry and command cleanup procedures packaged in the final FV; source host tests and hardware execution are separate"
            ufs_linkage[name] = item
        else:
            item["private_protocol_guid"] = str(uuid.UUID(bytes_le=exclusive_guid))
            if name == "MusbHandoffDxe":
                cold_linkage[name] = dict(item, resolved_functions={
                    key: value for key, value in resolved.items() if key in cold_functions})
                item["resolved_functions"] = {
                    key: value for key, value in resolved.items() if key not in cold_functions}
                obj = link_map.parent.parent / "OUTPUT" / "MusbHandoffDxe.obj"
                inline_sites, procedure_bytes = codeview_inline_sites(obj)
                # Compiler size decisions can move a once-inlined helper into
                # its own entry when a second power route calls it. Prove either
                # actual executable entry or actual CodeView inline site.
                helper_names = [
                    "SetPogoRoute", "StepTypec", "StartBoardHost", "StopBoardHost",
                    "PrepareColdTypec", "SetTypecPower", "ReleaseOwnedSourcePower",
                ]
                text_regions = {int(match[1], 16): int(match[2], 16) for match in re.finditer(
                    r"^([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+4\s+.*?/MusbHandoffDxe\.obj:\(\.text\)\s*$",
                    link_map.read_text(), re.M) if int(match[2], 16)}
                reverse_symbols = {}
                for symbol, address in symbol_records:
                    reverse_symbols.setdefault(address, set()).add(symbol)
                regions = {}
                graph = {}
                for parent, size in procedure_bytes.items():
                    # Static helper names recur in other input objects. Only
                    # the map region owned by this exact COFF can identify it.
                    candidates = {address for symbol, address in symbol_records
                                  if symbol == parent and address in text_regions}
                    if not candidates: continue  # Not linked from this object.
                    assert len(candidates) == 1, (parent, candidates)
                    rva = next(iter(candidates))
                    assert text_regions[rva] == size and size % 4 == 0, (parent, size)
                    code = at_rva(rva, size, executable=True)
                    direct_calls = set()
                    indirect_calls = 0
                    for offset in range(0, size, 4):
                        instruction = struct.unpack_from("<I", code, offset)[0]
                        # BL imm26 and tail-call B imm26 both name concrete code.
                        if instruction & 0xfc000000 in (0x94000000, 0x14000000):
                            imm = instruction & 0x3ffffff
                            if imm & (1 << 25): imm -= 1 << 26
                            target = rva + offset + imm * 4
                            direct_calls.update(reverse_symbols.get(target, set()))
                        if instruction & 0xfffffc1f == 0xd63f0000:
                            indirect_calls += 1
                    graph[parent] = direct_calls
                    regions[parent] = {"rva": rva, "bytes": size,
                        "code_sha256": hashlib.sha256(code).hexdigest(),
                        "direct_call_targets": sorted(direct_calls),
                        "indirect_call_sites": indirect_calls,
                        "COFF_procedure_size_matches": True}
                inline_helpers = {}
                standalone_helpers = {}
                needed_regions = {"UsbRoleTick", "BeforeExitNotify"}
                for helper in helper_names:
                    if helper in regions:
                        standalone_helpers[helper] = regions[helper]
                        needed_regions.add(helper)
                        continue
                    parents = inline_sites.get(helper, [])
                    assert parents and all(parent in regions for parent in parents), (helper, parents)
                    inline_helpers[helper] = parents
                    needed_regions.update(parents)
                required_calls = {
                    "UsbRoleTick": {
                        "MtkMusbPlatformAdvanceColdHost", "MtkMusbPlatformStopColdHost",
                        "MtkUsbPhyPlatformBind", "MtkUsbPhyCorePowerOn",
                        "MtkHusb311TypecBeginSourceProbe", "MtkHusb311TypecPollSourceProbe",
                        "MtkHusb311TypecStepCleanup",
                        "MtkSc8989xOtgEnableCold", "MtkSc8989xOtgDisable", "RestoreColdPinField",
                    },
                    "BeforeExitNotify": {
                        "MtkMusbPlatformStopColdHost", "MtkHusb311TypecStepCleanup",
                        "MtkSc8989xOtgDisable", "RestoreColdPinField",
                    },
                }
                reachable_calls = {}
                for root, required in required_calls.items():
                    seen = set()
                    pending = [root]
                    while pending:
                        parent = pending.pop()
                        if parent in seen: continue
                        seen.add(parent)
                        pending.extend(graph.get(parent, set()) - seen)
                    assert required <= seen, (root, required - seen)
                    needed_regions.update(seen & regions.keys())
                    reachable_calls[root] = {"required_reachable_targets": sorted(required),
                                            "board_procedures_in_call_graph": sorted(seen & regions.keys())}
                cold_linkage[name].update({
                    "inlined_helpers": inline_helpers,
                    "standalone_helpers": standalone_helpers,
                    "inlined_caller_regions": {key: regions[key] for key in sorted(needed_regions)},
                    "helper_call_graph": reachable_calls,
                    "COFF_object_bytes": obj.stat().st_size,
                    "COFF_object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest(),
                    "inline_evidence": "Actual AARCH64 COFF LF_FUNC_ID/S_INLINESITE records or standalone entries, exact complete executable procedure bytes and reachable call targets in the PE present in final FV"})
            power_linkage[name] = item
    platform_linkage = {}
    for name, relative, owners, required in (
        ("MsdcDxe", "MediaTekPkg/Drivers/MsdcDxe/MsdcDxe", {
            "MsdcDxe": "MediaTekPkg/Drivers/MsdcDxe/MsdcDxe/OUTPUT/MsdcDxe.obj",
            "MsdcUtils": "MediaTekPkg/Drivers/MsdcDxe/MsdcDxe/OUTPUT/MsdcUtils.obj",
            "MsdcImplLib": "MT6789Pkg/Library/MsdcImplLib/MsdcImplLib/OUTPUT/MsdcImplLib.obj"},
            ("MsdcDxe/MsdcPassThru", "MsdcDxe/MsdcPollMedia",
             "MsdcDxe/MsdcExitBootServices", "MsdcDxe/InitMsdc")),
        ("T70I2cPlatformDxe", "t70proPkg/Drivers/I2cPlatformDxe/I2cPlatformDxe", {
            "I2cPlatformDxe": "t70proPkg/Drivers/I2cPlatformDxe/I2cPlatformDxe/OUTPUT/I2cPlatformDxe.obj"},
            ("I2cPlatformDxe/T70I2cPlatformEntry",)),
        ("SdDxe", "MdeModulePkg/Bus/Sd/SdDxe/SdDxe", {
            "SdDxe": "MdeModulePkg/Bus/Sd/SdDxe/SdDxe/OUTPUT/SdDxe.obj",
            "SdBlockIo": "MdeModulePkg/Bus/Sd/SdDxe/SdDxe/OUTPUT/SdBlockIo.obj",
            "SdDiskInfo": "MdeModulePkg/Bus/Sd/SdDxe/SdDxe/OUTPUT/SdDiskInfo.obj"},
            ("SdDxe/SdDxeDriverBindingStart", "SdDxe/SdDxeDriverBindingStop",
             "SdBlockIo/SdReadBlocks", "SdBlockIo/SdReset", "SdDiskInfo/SdDiskInfoInquiry")),
        ("FramebufferGopDxe", "MediaTekPkg/Drivers/FramebufferGopDxe/FramebufferGopDxe", {
            "FramebufferGopDxe": "MediaTekPkg/Drivers/FramebufferGopDxe/FramebufferGopDxe/OUTPUT/FramebufferGopDxe.obj",
            "MtkDisplayCore": "MediaTekPkg/Library/FramebufferCoreLib/FramebufferCoreLib/OUTPUT/MtkDisplayCore.obj"},
            ("FramebufferGopDxe/FramebufferGopEntry", "MtkDisplayCore/MtkDisplayAcquireLkHandoff")),
        ("ClockDxe", "MediaTekPkg/Drivers/ClockDxe/ClockDxe", {
            "ClockDxe": "MediaTekPkg/Drivers/ClockDxe/ClockDxe/OUTPUT/ClockDxe.obj"},
            ("ClockDxe/InitClockDriver", "ClockDxe/ClockSetEnable", "ClockDxe/ClockIsEnabled")),
        ("GpioDxe", "MediaTekPkg/Drivers/GpioDxe/GpioDxe", {
            "GpioDxe": "MediaTekPkg/Drivers/GpioDxe/GpioDxe/OUTPUT/GpioDxe.obj"},
            ("GpioDxe/InitGpioDriver", "GpioDxe/GpioAcquirePins",
             "GpioDxe/GpioReleasePins", "GpioDxe/GpioValidateLease", "GpioDxe/GpioIsBusy")),
        ("KeypadDeviceDxe", "SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe", {
            "KeypadDeviceDxe": "SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe/OUTPUT/KeypadDeviceDxe.obj",
            "KeypadDeviceLib": "t70proPkg/Library/KeypadDeviceLib/KeypadDeviceLib/OUTPUT/KeypadDeviceLib.obj"},
            ("KeypadDeviceDxe/InitializeKeypadDevice", "KeypadDeviceLib/KeypadDeviceGetKeys",
             "KeypadDeviceLib/KeypadDeviceReset")),
        ("KeypadDxe", "SiliciumPkg/Drivers/KeypadDxe/KeypadDxe", {
            "KeypadDxe": "SiliciumPkg/Drivers/KeypadDxe/KeypadDxe/OUTPUT/KeypadDxe.obj",
            "KeypadController": "SiliciumPkg/Drivers/KeypadDxe/KeypadDxe/OUTPUT/KeypadController.obj",
            "KeypadTextIn": "SiliciumPkg/Drivers/KeypadDxe/KeypadDxe/OUTPUT/KeypadTextIn.obj"},
            ("KeypadDxe/InitializeKeypad", "KeypadDxe/KeypadControllerDriverStart",
             "KeypadDxe/KeypadControllerDriverStop", "KeypadController/KeypadTimerHandler")),
        ("HimaxTouchDxe", "MediaTekPkg/Drivers/HimaxTouchDxe/HimaxTouchDxe", {
            "HimaxTouchDxe": "MediaTekPkg/Drivers/HimaxTouchDxe/HimaxTouchDxe/OUTPUT/HimaxTouchDxe.obj",
            "HimaxCore": "MediaTekPkg/Library/HimaxCoreLib/HimaxCoreLib/OUTPUT/HimaxCore.obj"},
            ("HimaxTouchDxe/HimaxEntry", "HimaxTouchDxe/Start", "HimaxTouchDxe/Stop",
             "HimaxTouchDxe/Poll", "HimaxTouchDxe/HimaxTransfer",
             "HimaxTouchDxe/ControlQuiesce", "HimaxTouchDxe/ControlResume",
             "HimaxCore/HmReadChipId", "HimaxCore/HmSenseOn", "HimaxCore/HmBusRead")),
        ("T70DsiPlatformDxe", "t70proPkg/Drivers/DsiPlatformDxe/DsiPlatformDxe", {
            "DsiPlatformDxe": "t70proPkg/Drivers/DsiPlatformDxe/DsiPlatformDxe/OUTPUT/DsiPlatformDxe.obj",
            "MtkDsiCore": "MediaTekPkg/Library/FramebufferCoreLib/FramebufferCoreLib/OUTPUT/MtkDsiCore.obj"},
            ("DsiPlatformDxe/T70DsiPlatformEntry", "DsiPlatformDxe/Acquire",
             "DsiPlatformDxe/Configure", "DsiPlatformDxe/StartVideo", "DsiPlatformDxe/Suspend",
             "DsiPlatformDxe/Resume", "DsiPlatformDxe/Release", "DsiPlatformDxe/QueryState",
             "MtkDsiCore/MtkDsiCoreInitialize", "MtkDsiCore/MtkDsiCoreRunAlongCyxOp7",
             "MtkDsiCore/MtkDsiCoreStartVideo", "MtkDsiCore/MtkDsiCoreSuspend",
             "MtkDsiCore/MtkDsiCoreResume")),
        ("T70GpioPlatformDxe", "t70proPkg/Drivers/GpioPlatformDxe/GpioPlatformDxe", {
            "GpioPlatformDxe": "t70proPkg/Drivers/GpioPlatformDxe/GpioPlatformDxe/OUTPUT/GpioPlatformDxe.obj",
            "MtkPanelCore": "MediaTekPkg/Library/FramebufferCoreLib/FramebufferCoreLib/OUTPUT/MtkPanelCore.obj"},
            ("GpioPlatformDxe/T70GpioPlatformEntry", "GpioPlatformDxe/ObserveTouch",
             "GpioPlatformDxe/ExportToAcpi", "GpioPlatformDxe/ReadTouchGpioSnapshotWithLease",
             "GpioPlatformDxe/ReleaseProbePinLease", "GpioPlatformDxe/ProbeTransfer",
             "GpioPlatformDxe/TouchValidateGpioLease", "GpioPlatformDxe/PanelRunInit",
             "GpioPlatformDxe/PanelRunSuspend", "GpioPlatformDxe/PanelRunKernelPrepare",
             "GpioPlatformDxe/PanelRunKernelEnable", "GpioPlatformDxe/PanelRunKernelUnprepare",
             "MtkPanelCore/MtkPanelCoreRunLkInit", "MtkPanelCore/MtkPanelCoreRunLkSuspend",
             "MtkPanelCore/MtkPanelCoreRunKernelPrepare", "MtkPanelCore/MtkPanelCoreRunKernelEnable",
             "MtkPanelCore/MtkPanelCoreRunKernelUnprepare")),
    ):
        assert (drivers if name in drivers else standard_drivers)[name]["in_fv"], (name, "not packaged")
        platform_linkage[name] = verify_complete_owning_procedures(
            build, name, relative, owners, required)
        platform_linkage[name]["in_fv"] = True
        if name=="HimaxTouchDxe":
            pe=(build/"AARCH64/HimaxTouchDxe.efi").read_bytes()
            opt=struct.unpack_from("<I",pe,0x3c)[0]+24
            image_base=struct.unpack_from("<Q",pe,opt+24)[0]
            methods=("ControlQuiesce","ControlResume")
            regions=platform_linkage[name]["complete_owning_procedure_regions"]
            callbacks={n:regions["HimaxTouchDxe/"+n]["rva"] for n in methods}
            header=Path(__file__).resolve().parents[3]/"Silicon/MediaTek/MediaTekPkg/Include/Protocol/HimaxTouch.h"
            revision_match=re.search(
                r"^\s*#define\s+HIMAX_TOUCH_CONTROL_REVISION\s+([0-9]+)U?\s*$",
                header.read_text(),re.M)
            assert revision_match, "Himax touch-control revision is missing"
            revision=int(revision_match.group(1))
            table=struct.pack("<II2Q",revision,0,*(image_base+callbacks[n] for n in methods))
            assert pe.count(table)==1 and uuid.UUID("9dbb1d38-d3c7-49f8-a428-7349b215806c").bytes_le in pe
            platform_linkage[name]["touch_control_protocol"]={
                "revision":revision,"callback_RVAs":callbacks,"table_bytes":len(table),
                "table_file_offset":pe.index(table),"table_sha256":hashlib.sha256(table).hexdigest()}
        if name=="T70DsiPlatformDxe":
            pe=(build/"AARCH64/T70DsiPlatformDxe.efi").read_bytes()
            opt=struct.unpack_from("<I",pe,0x3c)[0]+24
            image_base=struct.unpack_from("<Q",pe,opt+24)[0]
            methods=("Acquire","Configure","StartVideo","Suspend","Resume","Release","QueryState")
            regions=platform_linkage[name]["complete_owning_procedure_regions"]
            callbacks={n:regions["DsiPlatformDxe/"+n]["rva"] for n in methods}
            table=struct.pack("<II7Q",1,0,*(image_base+callbacks[n] for n in methods))
            assert pe.count(table)==1 and uuid.UUID("b14fe6d9-36ad-4dfc-a7b2-c580534e9122").bytes_le in pe
            platform_linkage[name]["DSI_host_protocol"]={
                "revision":1,"callback_RVAs":callbacks,"table_bytes":len(table),
                "table_file_offset":pe.index(table),"table_sha256":hashlib.sha256(table).hexdigest()}
        if name=="T70GpioPlatformDxe":
            platform_linkage[name]["panel_board_protocol"]=verify_panel_board_protocol(
                (build/"AARCH64/T70GpioPlatformDxe.efi").read_bytes(),platform_linkage[name])
    return {"acpi_ffs_payloads_match":found,"compiled_arm64_drivers":drivers,
            "packaged_usb_hid_drivers":hid_drivers,
            "packaged_standard_boot_drivers":standard_drivers,
            "packaged_board_boot_policy":boot_policy,
            "packaged_reset_runtime":reset_driver,
            "packaged_prm_runtime":prm_module,
            "packaged_source_power_and_i2c":power_linkage,
            "packaged_pll_gate_functions":pll_linkage,
            "packaged_usb_cold_host_functions":cold_linkage,
            "packaged_ufs_boot_functions":ufs_linkage,
            "packaged_sd_i2c_clock_functions":platform_linkage,
            "embedded_fv_sha256":hashlib.sha256(fv).hexdigest(),
            "current_fv_compared":current_fv.exists(),
            "source":"decompressed actual FD/FV, not a source-only FDF check"}



def reset_system_cases(build):
    image = build / "AARCH64/ResetSystemRuntimeDxe.efi"
    link_map = build / "AARCH64/MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe/DEBUG/ResetSystemRuntimeDxe.map"
    raw = image.read_bytes()
    pe = struct.unpack_from("<I", raw, 0x3c)[0]
    assert raw[:2] == b"MZ" and raw[pe:pe+4] == b"PE\0\0"
    assert struct.unpack_from("<H", raw, pe+4)[0] == 0xaa64
    opt = pe+24
    assert struct.unpack_from("<H", raw, opt)[0] == 0x20b
    assert struct.unpack_from("<H", raw, opt+68)[0] == 12
    count = struct.unpack_from("<H", raw, pe+6)[0]
    opt_size = struct.unpack_from("<H", raw, pe+20)[0]
    base = struct.unpack_from("<Q", raw, opt+24)[0]
    size, headers = struct.unpack_from("<II", raw, opt+56)
    symbols = {match[2]:int(match[1],16) for match in re.finditer(
        r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$", link_map.read_text(), re.M)}
    assert all(name in symbols for name in ("ResetCold", "ResetWarm", "ResetShutdown", "ResetPlatformSpecific", "ArmMonitorCall"))
    stack, sentinel = 0x70000000, 0x6000f000
    results = []
    configurations = [("cold", "ResetCold", 0, [0x84000009]),
                      ("shutdown", "ResetShutdown", 0, [0x84000008]),
                      ("platform-specific", "ResetPlatformSpecific", 0, [0x84000009]),
                      ("warm-supported", "ResetWarm", 0, [0x8400000a,0xc4000012]),
                      ("warm-unsupported", "ResetWarm", -1, [0x8400000a,0x84000009]),
                      ("warm-denied", "ResetWarm", -3, [0x8400000a,0x84000009]),
                      ("warm-unexpected-feature-result", "ResetWarm", 1, [0x8400000a,0x84000009])]
    for name, entry, feature_result, expected in configurations:
        engine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        engine.mem_map(base, (size+4095)&~4095)
        engine.mem_write(base, raw[:headers])
        for index in range(count):
            section = opt+opt_size+index*40
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", raw, section+8)
            assert rva+max(virtual_size,raw_size) <= size and raw_offset+raw_size <= len(raw)
            if raw_size: engine.mem_write(base+rva,raw[raw_offset:raw_offset+raw_size])
        engine.mem_map(stack,0x20000)
        engine.mem_map(sentinel&~4095,4096)
        calls = []
        def monitor(uc, address, instruction_size, unused):
            args = uc.reg_read(UC_ARM64_REG_X0)
            assert stack <= args <= stack+0x20000-64
            values = struct.unpack("<8Q", uc.mem_read(args,64))
            fid = values[0]
            assert len(calls) < len(expected) and fid == expected[len(calls)], (name,values)
            if fid == 0x8400000a: assert values[1] == 0xc4000012
            if fid == 0xc4000012: assert values[1] == 0
            calls.append(hex(fid))
            value = feature_result if fid == 0x8400000a else 0
            uc.mem_write(args,struct.pack("<Q",value&0xffffffffffffffff))
            uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_X30))
        engine.hook_add(UC_HOOK_CODE,monitor,begin=base+symbols["ArmMonitorCall"],end=base+symbols["ArmMonitorCall"])
        engine.reg_write(UC_ARM64_REG_SP,stack+0x20000-16)
        engine.reg_write(UC_ARM64_REG_X30,sentinel)
        engine.emu_start(base+symbols[entry],sentinel,timeout=1000000,count=100000)
        assert engine.reg_read(UC_ARM64_REG_PC) == sentinel and len(calls) == len(expected), name
        results.append({"case":name,"PSCI_calls":calls,"returned_to_sentinel":True})
    return {"status":"passed", "binary_sha256":hashlib.sha256(raw).hexdigest(),
            "map_sha256":hashlib.sha256(link_map.read_bytes()).hexdigest(), "cases":results,
            "scope":"Linked ARM64 PSCI reset functions in the exact packaged runtime PE; ArmMonitorCall replaced by a bounded argument/return fixture; full runtime-service callback dispatch, SMC/EL3 and physical reset/off not executed"}


def prm_runtime_cases(build):
    """Execute only the PRM wrapper/IO callbacks; the power core is mocked here."""
    image = build / "AARCH64/MtkUfsPrm.efi"
    raw = image.read_bytes()
    link_map = build / "AARCH64/MediaTekPkg/Drivers/MtkUfsPrm/MtkUfsPrm/DEBUG/MtkUfsPrm.map"
    map_text = link_map.read_text()
    records = [(match[2], int(match[1], 16)) for match in re.finditer(
        r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$", map_text, re.M)]
    symbols = dict(records)
    required = ("MtkUfsPrmHandler", "MtkPmicPrmHandler", "MtkUfsPowerCoreHandle",
                "MtkPmicBrokerInitialize", "MtkPmicBrokerBlocksUfs", "MtkPmicBrokerHandle",
                "MtkPmicBrokerWacsWriteAllowed", "RuntimePmicWrite32", "RuntimeTryLock", "RuntimeUnlock", "mPmicBroker", "mPowerCore", "mPowerLock",
                "ArmCallSmc", "GetPerformanceCounter", "GetTimeInNanoSecond")
    assert all(name in symbols for name in required)
    at = struct.unpack_from("<I", raw, 0x3c)[0]
    optional = at+24
    count = struct.unpack_from("<H", raw, at+6)[0]
    optional_size = struct.unpack_from("<H", raw, at+20)[0]
    base = struct.unpack_from("<Q", raw, optional+24)[0]
    size, headers = struct.unpack_from("<II", raw, optional+56)
    engine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    engine.mem_map(base, (size+4095)&~4095)
    engine.mem_write(base, raw[:headers])
    pe_sections = []
    for index in range(count):
        off = optional+optional_size+index*40
        virtual_size, rva, raw_size, raw_at = struct.unpack_from("<4I", raw, off+8)
        assert rva+max(virtual_size,raw_size) <= size and raw_at+raw_size <= len(raw)
        if raw_size:
            engine.mem_write(base+rva, raw[raw_at:raw_at+raw_size])
        pe_sections.append((rva, max(virtual_size,raw_size)))
    stack, sentinel, data = 0x70000000, 0x6000f000, 0x72000000
    engine.mem_map(stack, 0x40000)
    engine.mem_map(sentinel&~4095, 4096)
    engine.mem_map(data, 0x1000)
    virtual = [0x91000000 + index*0x10000 for index in range(4)]
    physical = [0x11270000,0x10001000,0x10000000,0x10026000]
    mapped = [0x3000,0x1000,0x1000,0x1000]
    accessible = [0x2300,0x1000,0x1000,0x1000]
    for address, length in zip(virtual,mapped):
        engine.mem_map(address,length)
    parameter, context, ranges, output = data, data+0x100, data+0x200, data+0x300
    handler_guid = uuid.UUID("c9aef12d-e26c-4be7-b345-62c5f306d184").bytes_le
    pmic_handler_guid = uuid.UUID("9f58ca73-6bc4-45b8-b69c-f33c6261d081").bytes_le
    context_bytes = struct.pack("<IHH",0x434d5250,1,0)+handler_guid+struct.pack("<QQ",0,ranges)
    pmic_context_bytes = struct.pack("<IHH",0x434d5250,1,0)+pmic_handler_guid+struct.pack("<QQ",0,ranges)
    range_bytes = struct.pack("<Q",4)+b"".join(
        struct.pack("<QQI",p,v,n) for p,v,n in zip(physical,virtual,mapped))
    request_bytes = struct.pack("<IHHIIQII4Q",64,1,0,0,0,0,0,0,0,0,0,0)
    captured, mmio, smcs = [], [], []
    pmic_hw = {"enabled":False, "registers":{}, "active_address":0, "now_ns":1234567890,
               "write_data":0, "fsm":0, "hold_clear":False,
               "hold_write":False, "commands":[], "data_writes":[], "clears":[]}
    def return_value(value):
        engine.reg_write(UC_ARM64_REG_X0,value)
        engine.reg_write(UC_ARM64_REG_PC,engine.reg_read(UC_ARM64_REG_X30))
    def core_fixture(uc,address,length,unused):
        io = uc.reg_read(UC_ARM64_REG_X1)
        captured.append(struct.unpack("<7Q",uc.mem_read(io,56)))
        return_value(0)
    def smc_fixture(uc,address,length,unused):
        args = uc.reg_read(UC_ARM64_REG_X0)
        smcs.append(struct.unpack("<8Q",uc.mem_read(args,64)))
        uc.mem_write(args,struct.pack("<Q",0))
        return_value(0)
    def timer_fixture(uc,address,length,unused):
        return_value(26000000 if address==base+symbols["GetPerformanceCounter"] else pmic_hw["now_ns"])
    engine.hook_add(UC_HOOK_CODE,core_fixture,begin=base+symbols["MtkUfsPowerCoreHandle"],
                    end=base+symbols["MtkUfsPowerCoreHandle"])
    engine.hook_add(UC_HOOK_CODE,smc_fixture,begin=base+symbols["ArmCallSmc"],end=base+symbols["ArmCallSmc"])
    for name in ("GetPerformanceCounter","GetTimeInNanoSecond"):
        engine.hook_add(UC_HOOK_CODE,timer_fixture,begin=base+symbols[name],end=base+symbols[name])
    def memory_event(uc,access,address,length,value,unused):
        mmio.append({"access":access,"address":address,"bytes":length,"value":value})
        if not pmic_hw["enabled"] or access != UC_MEM_WRITE:
            return
        offset = address - virtual[3]
        if offset == 0x884:
            pmic_hw["write_data"] = value & 0xFFFF
            pmic_hw["data_writes"].append(value & 0xFFFF)
        elif offset == 0x880:
            command = value & 0xFFFFFFFF
            pmic_hw["commands"].append(command)
            if command & 0x20000000:
                pmic_hw["active_address"] = command & 0xFFFF
                if pmic_hw["active_address"] == 0x91c:
                    pmic_hw["registers"][0x91c] = (
                        pmic_hw["registers"].get(0x91c,0) & ~pmic_hw["write_data"])
                elif pmic_hw["active_address"] == 0x1726:
                    pmic_hw["registers"][0x1724] |= pmic_hw["write_data"]
                elif pmic_hw["active_address"] == 0x1728:
                    pmic_hw["registers"][0x1724] &= ~pmic_hw["write_data"]
                else:
                    pmic_hw["registers"][pmic_hw["active_address"]] = pmic_hw["write_data"]
                pmic_hw["fsm"] = 1 if pmic_hw["hold_write"] else 0
            else:
                pmic_hw["active_address"] = command & 0x7FFF
                uc.mem_write(virtual[3]+0x894,struct.pack(
                    "<I",pmic_hw["registers"].get(pmic_hw["active_address"],0)))
                pmic_hw["fsm"] = 6
            uc.mem_write(virtual[3]+0x8A8,struct.pack("<I",pmic_hw["fsm"] << 1))
        elif offset == 0x8A4 and value == 1:
            pmic_hw["clears"].append(value)
            if not pmic_hw["hold_clear"]:
                pmic_hw["fsm"] = 0
                uc.mem_write(virtual[3]+0x8A8,struct.pack("<I",0))
    for address, length in zip(virtual,mapped):
        engine.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,memory_event,begin=address,end=address+length-1)
    registers = (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4)
    def call(address,*arguments,upper_stack=False):
        assert len(arguments)<=len(registers)
        for reg, value in zip(registers,arguments+(0,)*(len(registers)-len(arguments))):
            engine.reg_write(reg,value)
        # A second stack preserves the handler's temporary callback context.
        engine.reg_write(UC_ARM64_REG_SP,stack+(0x40000 if upper_stack else 0x20000)-16)
        engine.reg_write(UC_ARM64_REG_X30,sentinel)
        engine.emu_start(address,sentinel,timeout=1000000,count=1000000)
        assert engine.reg_read(UC_ARM64_REG_PC)==sentinel
        return engine.reg_read(UC_ARM64_REG_X0)
    def setup(cb=context_bytes,rb=range_bytes):
        engine.mem_write(parameter,request_bytes)
        engine.mem_write(context,cb)
        engine.mem_write(ranges,rb)
        mmio.clear()

    class _PowerCorePrefix(ctypes.Structure):
        _fields_ = [(name,ctypes.c_uint32) for name in (
            "Signature","State","Phase","Operation","RootIndex","GateIndex",
            "GateOwnedMask","GateObservedMask","IpVersion","WacsStage")]
    wacs_stage_offset = _PowerCorePrefix.WacsStage.offset
    assert wacs_stage_offset == 36
    class _PmicBrokerLayout(ctypes.Structure):
        _fields_ = [(name,ctypes.c_uint32) for name in (
            "Signature","State","Phase","Operation","Address","RequestedValue",
            "Value","Mask","ReadValue","PollCount","FinalResult","WriteCallbackActive",
            "SnapshotStage","SnapshotGroupStatus")]
        _fields_ += [(name,ctypes.c_uint64) for name in (
            "Generation","LastAckGeneration","StartedUs")]
        _fields_ += [(name,ctypes.c_uint32) for name in (
            "ProtocolVersion","RetainKeySnapshot","KeyLeaseState","KeyLeaseBaselineBits",
            "KeyLeaseOwnedMask","KeySnapshotValid","KeySnapshotStatus","KeySnapshotTopStatus",
            "KeySnapshotAckState","KeySnapshotAckMask","KeyAckWriteMayHaveEffect",
            "KeySnapshotSiblingStatus","LastKeyAckValue")]
        _fields_ += [(name,ctypes.c_uint64) for name in (
            "KeyLeaseToken","KeySnapshotToken","KeySnapshotLeaseToken",
            "LastReleasedLeaseToken","LastReleasedJobGeneration",
            "LastAckedSnapshotToken","LastKeyAckJobGeneration",
            "SdPowerToken","SdReleasedToken","SdReleaseGeneration")]
        _fields_ += [("SdPowerFault",ctypes.c_uint32),("SdStage",ctypes.c_uint32),
                     ("SdBaseline",ctypes.c_uint32*5)]
        _fields_ += [("ConnLeaseToken",ctypes.c_uint64),("ConnReleasedToken",ctypes.c_uint64),
                     ("ConnCaptured",ctypes.c_uint32),("ConnDirty",ctypes.c_uint32),
                     ("ConnBaseline",ctypes.c_uint16*12)]
    broker_journal_size=ctypes.sizeof(_PmicBrokerLayout)
    assert broker_journal_size==296
    empty_broker_journal=struct.pack("<I",0x504D4231)+bytes(broker_journal_size-4)
    pmic_broker_pointer = base + symbols["mPmicBroker"]
    power_core_pointer = base + symbols["mPowerCore"]
    power_lock_pointer = base + symbols["mPowerLock"]
    pmic_handler = base + symbols["MtkPmicPrmHandler"]
    ufs_try_lock = base + symbols["RuntimeTryLock"]
    ufs_unlock = base + symbols["RuntimeUnlock"]
    call(base+symbols["MtkPmicBrokerInitialize"],pmic_broker_pointer,upper_stack=True)
    engine.mem_write(power_lock_pointer,struct.pack("<I",0))
    engine.mem_write(power_core_pointer+wacs_stage_offset,struct.pack("<I",0))

    def broker_request(operation,generation=0,address=0,value=0,version=1):
        return struct.pack("<IHHIIQIIIIQQQ",64,version,operation,0,0,generation,
                           0,address,value,0,0,0,0)

    def broker_fields(raw_request):
        return struct.unpack("<IHHIIQIIIIQQQ",raw_request)

    def reset_pmic_channel(registers=None):
        call(base+symbols["MtkPmicBrokerInitialize"],pmic_broker_pointer,upper_stack=True)
        engine.mem_write(power_lock_pointer,struct.pack("<I",0))
        engine.mem_write(power_core_pointer+wacs_stage_offset,struct.pack("<I",0))
        pmic_hw.update({"enabled":True,"registers":dict(registers or {}),"active_address":0,
                        "write_data":0,"fsm":0,"hold_clear":False,"hold_write":False,"now_ns":1234567890,
                        "commands":[],"data_writes":[],"clears":[]})
        engine.mem_write(virtual[3],struct.pack("<I",1))
        engine.mem_write(virtual[3]+0x894,struct.pack("<I",0))
        engine.mem_write(virtual[3]+0x8A8,struct.pack("<I",0))
        engine.mem_write(context,pmic_context_bytes)
        engine.mem_write(ranges,range_bytes)
        mmio.clear()

    def call_pmic(raw_request,cb=pmic_context_bytes,param_ptr=parameter,ctx_ptr=context):
        engine.mem_write(parameter,raw_request)
        engine.mem_write(context,cb)
        engine.mem_write(ranges,range_bytes)
        status=call(pmic_handler,param_ptr,ctx_ptr,upper_stack=True)
        response=bytes(engine.mem_read(parameter,64)) if param_ptr==parameter else b""
        return status,response

    def pmic_step(generation,version=1):
        status,response=call_pmic(broker_request(3,generation,version=version))
        assert status==0
        return broker_fields(response)

    invalid = [
        ("null-parameter",None,None,0,context),
        ("null-context",None,None,parameter,0)]
    for offset, value, label in ((0,0,"signature"),(4,2,"version"),(6,1,"reserved")):
        cb=bytearray(context_bytes)
        struct.pack_into("<I" if offset==0 else "<H",cb,offset,value)
        invalid.append((label,bytes(cb),None,parameter,context))
    cb=bytearray(context_bytes); cb[8]^=1
    invalid.append(("handler-guid",bytes(cb),None,parameter,context))
    for offset, value, label in ((24,parameter,"unexpected-static-data"),(32,0,"null-ranges")):
        cb=bytearray(context_bytes);struct.pack_into("<Q",cb,offset,value)
        invalid.append((label,bytes(cb),None,parameter,context))
    for count_value in (0,3,5,0xffffffffffffffff):
        rb=bytearray(range_bytes);struct.pack_into("<Q",rb,0,count_value)
        invalid.append(("range-count-"+str(count_value),None,bytes(rb),parameter,context))
    for bank in range(4):
        for field, value, label in ((0,physical[bank]+0x1000,"physical"),
                                   (8,0,"null-virtual"),(8,virtual[bank]+1,"unaligned-virtual"),
                                   (8,0xfffffffffffff000,"overflow-virtual"),
                                   (16,mapped[bank]+0x1000,"length")):
            # A 4 KiB mapping at the top address does not overflow by itself.
            if label=="overflow-virtual" and mapped[bank]==0x1000: continue
            rb=bytearray(range_bytes)
            struct.pack_into("<I" if field==16 else "<Q",rb,8+bank*20+field,value)
            invalid.append((str(bank)+"-"+label,None,bytes(rb),parameter,context))
    rb=bytearray(range_bytes);struct.pack_into("<Q",rb,8+20+8,virtual[0])
    invalid.append(("aliased-virtual-ranges",None,bytes(rb),parameter,context))
    for bank in range(4):
        invalid.append((str(bank)+"-parameter-in-mmio",None,None,virtual[bank],context))
        rb=bytearray(range_bytes)
        struct.pack_into("<Q",rb,8+bank*20+8,data)
        invalid.append((str(bank)+"-metadata-in-declared-mmio",None,bytes(rb),parameter,context))
    for param_ptr,label in ((context,"parameter-context-alias"),
                            (context-63,"parameter-context-edge-alias"),
                            (ranges,"parameter-ranges-alias"),
                            (ranges+len(range_bytes)-1,"parameter-ranges-edge-alias")):
        invalid.append((label,None,None,param_ptr,context))
    cb=bytearray(context_bytes);struct.pack_into("<Q",cb,32,context)
    invalid.append(("context-ranges-alias",bytes(cb),None,parameter,context))
    invalid.extend([
        ("parameter-overflow",None,None,0xffffffffffffffc2,context),
        ("context-overflow",None,None,parameter,0xffffffffffffffd9)])
    rejected=[]
    for label, cb, rb, param_ptr, ctx_ptr in invalid:
        setup(cb or context_bytes,rb or range_bytes)
        before=len(captured)
        status=call(base+symbols["MtkUfsPrmHandler"],param_ptr,ctx_ptr)
        assert status>>63 and len(captured)==before and not mmio,(label,status)
        rejected.append(label)
    setup()
    assert call(base+symbols["MtkUfsPrmHandler"],parameter,context)==0
    assert len(captured)==1 and not mmio
    io_context,lock,unlock,read,write,smc,now=captured[0]
    assert all(base<=address<base+size for address in (lock,unlock,read,write,smc,now))
    assert struct.unpack("<4Q",engine.mem_read(io_context,32))==tuple(virtual)
    assert call(lock,io_context,upper_stack=True)==1
    assert call(lock,io_context,upper_stack=True)==0
    call(unlock,io_context,upper_stack=True)
    assert call(lock,io_context,upper_stack=True)==1
    call(unlock,io_context,upper_stack=True)
    callback_cases=4
    for bank in range(4):
        offset=0x894 if bank==3 else accessible[bank]-4
        engine.mem_write(virtual[bank]+offset,struct.pack("<I",0xa5a50000+bank))
        mmio.clear()
        assert call(read,io_context,bank,offset,output,upper_stack=True)==0
        assert struct.unpack("<I",engine.mem_read(output,4))[0]==0xa5a50000+bank
        assert len(mmio)==1 and mmio[0]["address"]==virtual[bank]+offset
        callback_cases+=1
        for bad_offset in (1,accessible[bank],0xfffffffc):
            mmio.clear()
            assert call(read,io_context,bank,bad_offset,output,upper_stack=True)!=0
            assert not mmio
            callback_cases+=1
    mmio.clear()
    assert call(read,io_context,4,0,output,upper_stack=True)!=0 and not mmio
    assert call(write,io_context,0,0x24,0,upper_stack=True)==0
    assert mmio[-1]["address"]==virtual[0]+0x24
    for bank,offset,value in ((2,0,1),(3,0x884,1),(3,0x880,0x20001b1c),(3,0x8a4,2),(1,0xa4,1),(1,0xa8,0),(1,0x130,1),(1,0,1),(4,0,1)):
        mmio.clear()
        assert call(write,io_context,bank,offset,value,upper_stack=True)!=0 and not mmio
        callback_cases+=1
    for offset,value in ((0x880,0x1b1c),(0x880,0x1e38),(0x8a4,1)):
        mmio.clear()
        assert call(write,io_context,3,offset,value,upper_stack=True)==0
        assert len(mmio)==1 and mmio[0]["address"]==virtual[3]+offset
        callback_cases+=1
    for command,value,stage in ((2,0,0),(2,1,0),(8,0,0),(8,0,1),(8,1,0),(8,1,1)):
        before=len(smcs)
        assert call(smc,io_context,command,value,stage,output,upper_stack=True)==0
        assert len(smcs)==before+1 and smcs[-1]==(0xc2000276,command,value,stage,0,0,0,0)
        callback_cases+=1
    for command,value,stage in ((32,0,0),(2,0,1),(8,2,0),(8,0,2)):
        before=len(smcs)
        assert call(smc,io_context,command,value,stage,output,upper_stack=True)!=0 and len(smcs)==before
        callback_cases+=1
    assert call(now,io_context,upper_stack=True)==1234567
    callback_cases+=1

    pmic_cases=[]
    pmic_request_size=64
    pmic_result_offset=24
    pmic_success,pmic_in_progress,pmic_invalid,pmic_busy,pmic_io_error=0,1,2,3,5
    def set_pmic_result(raw_request,result):
        updated=bytearray(raw_request)
        struct.pack_into("<I",updated,pmic_result_offset,result)
        return bytes(updated)

    # The PMIC handler has its own descriptor GUID and rejects bad contexts
    # before acquiring the shared lock or touching PWRAP.
    reset_pmic_channel()
    wrong_pmic_context=bytearray(pmic_context_bytes)
    wrong_pmic_context[8]^=1
    for label,param_ptr,ctx_ptr,ctx_bytes in (
        ("pmic-null-parameter",0,context,pmic_context_bytes),
        ("pmic-null-context",parameter,0,pmic_context_bytes),
        ("pmic-wrong-handler-guid",parameter,context,bytes(wrong_pmic_context)),
    ):
        mmio.clear()
        status,_=call_pmic(broker_request(0),ctx_bytes,param_ptr,ctx_ptr)
        assert status>>63 and not mmio,label
        pmic_cases.append(label)

    malformed_pmic=[]
    for offset,value,fmt,label in (
        (0,63,"<I","size"),(4,0,"<H","version"),(6,6,"<H","operation"),
        (8,1,"<I","flags"),(12,1,"<I","state"),(16,1,"<Q","generation"),
        (24,1,"<I","result"),(36,1,"<I","reserved0"),(40,1,"<Q","reserved"),
    ):
        malformed=bytearray(broker_request(0))
        struct.pack_into(fmt,malformed,offset,value)
        malformed_pmic.append((label,bytes(malformed)))
    malformed=bytearray(broker_request(1,0,0x1500,0))
    malformed_pmic.append(("invalid-register-read",bytes(malformed)))
    for label,raw_request in malformed_pmic:
        reset_pmic_channel()
        mmio.clear()
        status,response=call_pmic(raw_request)
        fields=broker_fields(response)
        assert status==0 and fields[6]==pmic_invalid and not mmio,label
        pmic_cases.append("invalid-wire-"+label)

    reset_pmic_channel()
    mmio.clear()
    status,response=call_pmic(broker_request(0))
    fields=broker_fields(response)
    assert status==0 and fields[6]==pmic_success and fields[4]==0 and not mmio
    pmic_cases.append("query-idle-no-mmio")

    # A held common CAS lock returns Busy without changing any request byte
    # except Result and without exposing a broker snapshot or MMIO access.
    reset_pmic_channel()
    lock_request=broker_request(1,0,0x1508,0)
    engine.mem_write(power_lock_pointer,struct.pack("<I",1))
    mmio.clear()
    status,response=call_pmic(lock_request)
    assert status==0 and response==set_pmic_result(lock_request,pmic_busy) and not mmio
    assert struct.unpack("<I",engine.mem_read(power_lock_pointer,4))[0]==1
    engine.mem_write(power_lock_pointer,struct.pack("<I",0))
    pmic_cases.append("common-cas-busy-input-preserved")

    # An owned UFS WACS stage (1..4) blocks a GPU Begin request without
    # changing the broker journal, returning a partial snapshot, or MMIO.
    for stage in range(1,5):
        reset_pmic_channel()
        broker_before=bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))
        assert broker_before==empty_broker_journal
        engine.mem_write(power_core_pointer+wacs_stage_offset,struct.pack("<I",stage))
        begin=broker_request(1,0,0x1508,0)
        mmio.clear()
        status,response=call_pmic(begin)
        assert status==0 and response==set_pmic_result(begin,pmic_busy) and not mmio
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==broker_before,stage
        pmic_cases.append("ufs-wacs-stage-%d-blocks-gpu-begin"%stage)

    # Start a real broker read and accept its WACS command. Stages 1..4 also
    # block later GPU Step calls, preserving both the request and live journal.
    reset_pmic_channel({0x1508:0x55})
    status,response=call_pmic(broker_request(1,0,0x1508,0))
    begin_fields=broker_fields(response)
    assert status==0 and begin_fields[6]==pmic_in_progress and begin_fields[4]==1
    broker_generation=begin_fields[5]
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["commands"]==[0x1508]
    broker_before=bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))
    for stage in range(1,5):
        engine.mem_write(power_core_pointer+wacs_stage_offset,struct.pack("<I",stage))
        blocked_step=broker_request(3,broker_generation)
        mmio.clear()
        status,response=call_pmic(blocked_step)
        assert status==0 and response==set_pmic_result(blocked_step,pmic_busy) and not mmio
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==broker_before,stage
        pmic_cases.append("ufs-wacs-stage-%d-blocks-gpu-step"%stage)
    engine.mem_write(power_core_pointer+wacs_stage_offset,struct.pack("<I",0))
    mmio.clear()
    assert call(base+symbols["MtkPmicBrokerBlocksUfs"],pmic_broker_pointer,upper_stack=True)==1
    assert call(ufs_try_lock,0,upper_stack=True)==0
    assert struct.unpack("<I",engine.mem_read(power_lock_pointer,4))[0]==0 and not mmio
    pmic_cases.append("pending-broker-blocks-ufs-lock")

    # Accepted read command and response clear are each issued once. Delayed
    # WACS status holds their journal; retry polls instead of replaying either.
    pmic_hw["fsm"]=1
    engine.mem_write(virtual[3]+0x8A8,struct.pack("<I",2))
    for _ in range(2):
        fields=pmic_step(broker_generation)
        assert fields[6]==pmic_in_progress and pmic_hw["commands"]==[0x1508]
    pmic_hw["fsm"]=6
    engine.mem_write(virtual[3]+0x8A8,struct.pack("<I",12))
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    pmic_hw["hold_clear"]=True
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["clears"]==[1] and pmic_hw["commands"]==[0x1508]
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["clears"]==[1]
    pmic_hw["hold_clear"]=False
    pmic_hw["fsm"]=0
    engine.mem_write(virtual[3]+0x8A8,struct.pack("<I",0))
    fields=pmic_step(broker_generation)
    assert fields[6]==pmic_success and fields[4]==2 and fields[5]==broker_generation and fields[8]==0x55
    assert pmic_hw["commands"]==[0x1508] and pmic_hw["clears"]==[1]
    assert call(base+symbols["MtkPmicBrokerBlocksUfs"],pmic_broker_pointer,upper_stack=True)==0
    assert call(ufs_try_lock,0,upper_stack=True)==1
    call(ufs_unlock,0,upper_stack=True)
    ack=broker_request(4,broker_generation)
    status,response=call_pmic(ack)
    assert status==0 and broker_fields(response)[6]==pmic_success and broker_fields(response)[4]==0
    assert pmic_hw["commands"]==[0x1508] and pmic_hw["clears"]==[1]
    pmic_cases.append("done-read-clear-no-replay-and-ufs-lock-before-ack")

    # An impossible idle status after an accepted read faults the channel;
    # the error is cached and UFS remains blocked without a second command.
    reset_pmic_channel({0x1508:0x33})
    status,response=call_pmic(broker_request(1,0,0x1508,0))
    broker_generation=broker_fields(response)[5]
    assert broker_fields(response)[6]==pmic_in_progress
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["commands"]==[0x1508]
    pmic_hw["fsm"]=0
    engine.mem_write(virtual[3]+0x8A8,struct.pack("<I",0))
    mmio.clear()
    fields=pmic_step(broker_generation)
    assert fields[6]==pmic_io_error and fields[4]==3
    mmio.clear()
    assert call(base+symbols["MtkPmicBrokerBlocksUfs"],pmic_broker_pointer,upper_stack=True)==1
    assert call(ufs_try_lock,0,upper_stack=True)==0 and not mmio
    assert pmic_step(broker_generation)[6]==pmic_io_error and not mmio
    assert pmic_hw["commands"]==[0x1508]
    pmic_cases.append("faulted-channel-blocks-ufs-and-caches-command")

    # The write side effect is accepted once. Busy readback polling and an
    # uncertain response-clear state never resend DATA, CMD, or VLDCLR.
    reset_pmic_channel({0x1508:0})
    pmic_hw["hold_write"]=True
    status,response=call_pmic(broker_request(2,0,0x1508,1))
    broker_generation=broker_fields(response)[5]
    assert status==0 and broker_fields(response)[6]==pmic_in_progress
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["commands"]==[0x1508]
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["commands"]==[0x1508,0x20001508]
    assert pmic_hw["registers"][0x1508]==1 and pmic_hw["data_writes"]==[1]
    assert pmic_hw["fsm"]==1
    for _ in range(2):
        assert pmic_step(broker_generation)[6]==pmic_in_progress
        assert pmic_hw["commands"]==[0x1508,0x20001508]
        assert pmic_hw["data_writes"]==[1]
    pmic_hw["hold_write"]=False
    pmic_hw["fsm"]=0
    engine.mem_write(virtual[3]+0x8A8,struct.pack("<I",0))
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["commands"]==[0x1508,0x20001508,0x1508]
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    pmic_hw["hold_clear"]=True
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["clears"]==[1,1] and pmic_hw["commands"]==[0x1508,0x20001508,0x1508]
    assert pmic_step(broker_generation)[6]==pmic_in_progress
    assert pmic_hw["clears"]==[1,1]
    pmic_hw["hold_clear"]=False
    pmic_hw["fsm"]=0
    engine.mem_write(virtual[3]+0x8A8,struct.pack("<I",0))
    fields=pmic_step(broker_generation)
    assert fields[6]==pmic_success and fields[4]==2 and fields[8]==1
    assert pmic_hw["commands"]==[0x1508,0x20001508,0x1508]
    assert pmic_hw["data_writes"]==[1] and pmic_hw["clears"]==[1,1]
    assert call(base+symbols["MtkPmicBrokerBlocksUfs"],pmic_broker_pointer,upper_stack=True)==0
    pmic_cases.append("accepted-write-and-clear-poll-without-replay")

    # Legacy power-button polling also needs the private PRM transport gate.
    # Exercise all low TOPSTATUS states through the actual linked wrapper.
    for top in range(4):
        reset_pmic_channel({0x28:top})
        status,response=call_pmic(broker_request(1,0,0x28))
        fields=broker_fields(response);generation=fields[5]
        assert status==0 and fields[4]==1 and fields[6]==pmic_in_progress
        for _ in range(12):
            fields=pmic_step(generation)
            if fields[4]==2:
                break
            assert fields[4]==1 and fields[6]==pmic_in_progress
        else:
            raise AssertionError("linked TOPSTATUS read did not finish")
        assert fields[6]==pmic_success and fields[7:9]==(0x28,top)
        assert pmic_hw["commands"]==[0x28] and not pmic_hw["data_writes"]
        assert pmic_hw["registers"]=={0x28:top}
        mmio.clear()
        status,response=call_pmic(broker_request(4,generation))
        assert status==0 and broker_fields(response)[4]==0 and not mmio
        pmic_cases.append("power-button-TOPSTATUS-%d-read-only"%top)

    # The fixed key snapshot uses the real linked opcode-5 core and wrapper.
    # PMIC registers are read only; WACS response clearing is transport state,
    # distinct from PMIC event W1C or the metadata-only broker Ack.
    for summary,group,top,expected_commands in (
        (4,0xa1b2,0xc3d4,[0x19e,0x91c,0x28]),
        (0,0x7654,0x0123,[0x19e,0x28]),
        (8,0x5555,0x0202,[0x19e,0x28]),
    ):
        original_registers={0x19e:summary,0x91c:group,0x28:top,0x910:0xa55a}
        reset_pmic_channel(original_registers)
        mmio.clear()
        status,response=call_pmic(broker_request(5))
        fields=broker_fields(response)
        generation=fields[5]
        assert status==0 and fields[4]==1 and fields[6]==pmic_in_progress
        assert generation==1 and fields[7:9]==(0x91c,0) and not mmio
        for _ in range(32):
            fields=pmic_step(generation)
            assert fields[5]==generation and fields[7]==0x91c
            if fields[4]==2:
                break
            assert fields[4]==1 and fields[6]==pmic_in_progress and fields[8]==0, (summary,fields,pmic_hw["commands"],pmic_hw["clears"])
        else:
            raise AssertionError("linked key snapshot did not finish")
        packed=((group if summary&4 else 0)<<16)|top
        assert fields[6]==pmic_success and fields[8]==packed
        assert pmic_hw["commands"]==expected_commands
        assert pmic_hw["data_writes"]==[]
        assert pmic_hw["clears"]==[1]*len(expected_commands)
        assert pmic_hw["registers"]==original_registers
        mmio.clear()
        assert pmic_step(generation)[8]==packed and not mmio
        status,response=call_pmic(broker_request(0))
        assert status==0 and broker_fields(response)[8]==packed and not mmio
        assert call(base+symbols["MtkPmicBrokerBlocksUfs"],pmic_broker_pointer,upper_stack=True)==0
        assert call(ufs_try_lock,0,upper_stack=True)==1
        call(ufs_unlock,0,upper_stack=True)
        status,response=call_pmic(broker_request(4,generation))
        fields=broker_fields(response)
        assert status==0 and fields[4]==0 and fields[6]==pmic_success
        assert fields[7:9]==(0x91c,packed) and not mmio
        assert pmic_hw["commands"]==expected_commands and pmic_hw["data_writes"]==[]
        assert pmic_hw["registers"]==original_registers
        pmic_cases.append("fixed-key-snapshot-summary-%x-packed-read-only-no-replay"%summary)

    for label,request in (
        ("snapshot-address",broker_request(5,0,0x91c,0)),
        ("snapshot-value",broker_request(5,0,0,1)),
        ("snapshot-generation",broker_request(5,1)),
        ("raw-summary-read",broker_request(1,0,0x19e)),
        ("raw-key-status-read",broker_request(1,0,0x91c)),
        ("raw-key-enable-write",broker_request(2,0,0x910,5)),
        ("raw-key-status-write",broker_request(2,0,0x91c,5)),
    ):
        reset_pmic_channel()
        mmio.clear()
        status,response=call_pmic(request)
        assert status==0 and broker_fields(response)[6]==pmic_invalid and not mmio,label
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==empty_broker_journal
        pmic_cases.append("fixed-key-snapshot-invalid-"+label)

    for stage in range(1,5):
        reset_pmic_channel()
        engine.mem_write(power_core_pointer+wacs_stage_offset,struct.pack("<I",stage))
        begin=broker_request(5)
        mmio.clear()
        status,response=call_pmic(begin)
        assert status==0 and response==set_pmic_result(begin,pmic_busy) and not mmio
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==empty_broker_journal
        pmic_cases.append("ufs-wacs-stage-%d-blocks-key-snapshot"%stage)


    def journal():
        return _PmicBrokerLayout.from_buffer_copy(
            bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size)))

    def metadata_ack(generation,version=2):
        mmio.clear()
        status,response=call_pmic(broker_request(4,generation,version=version))
        fields=broker_fields(response)
        assert status==0 and fields[4]==0 and fields[5]==generation and fields[6]==0 and not mmio
        return fields

    def finish_job(operation,token=0,version=2,result=0,ack=True,address=0,value=0):
        status,response=call_pmic(broker_request(operation,token,address,value,version=version))
        fields=broker_fields(response)
        assert status==0 and fields[4] in (1,2) and fields[5]>token, (operation,fields)
        generation=fields[5]
        for _ in range(64):
            if fields[4]==2:
                break
            assert fields[4]==1 and fields[6]==1, (operation,fields)
            fields=pmic_step(generation,version)
            assert fields[5]==generation
        else:
            raise AssertionError(("linked PMIC job did not terminate",operation,fields))
        assert fields[4]==2 and fields[6]==result, (operation,fields)
        if ack:
            ack_fields=metadata_ack(generation,version)
            assert ack_fields[7:9]==fields[7:9]
        return generation,fields

    def query_capabilities_preserves_journal(version,capabilities):
        before=bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))
        mmio.clear()
        status,response=call_pmic(broker_request(0,version=version))
        fields=broker_fields(response)
        assert status==0 and fields[3:9]==(capabilities,0,0,0,0,0) and not mmio, version
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==before, version

    reset_pmic_channel()
    for version,capabilities in ((2,3),(3,4),(4,8)):
        query_capabilities_preserves_journal(version,capabilities)
        pmic_cases.append(f"v{version}-capability-query-idle-no-MMIO-no-journal-mutation")

    # No private callback can be used independently of an executing broker
    # write, even when the target is one of the new fixed key registers.
    for offset,value in ((0x884,5),(0x880,0x20000910),(0x880,0x2000091c),
                         (0x880,0x28),(0x8a4,1)):
        mmio.clear()
        assert call(base+symbols["RuntimePmicWrite32"],io_context,offset,value,
                    upper_stack=True)!=0 and not mmio
    pmic_cases.append("v2-private-runtime-write-callback-denies-unowned-calls")

    for borrowed in (0,1,4,5):
        reset_pmic_channel({0x910:0x42|borrowed,0x19e:4,0x91c:1,0x28:0xbeef})
        lease,fields=finish_job(6)
        assert fields[7:9]==(0x910,5) and pmic_hw["registers"][0x910]==0x47
        assert journal().KeyLeaseToken==lease and journal().KeyLeaseOwnedMask==((~borrowed)&5)
        captured_token,fields=finish_job(5,lease)
        assert fields[7:9]==(0x91c,0x0001beef) and journal().KeySnapshotValid==1
        assert journal().KeySnapshotToken==captured_token and journal().KeySnapshotLeaseToken==lease
        # A v1 snapshot and GPU access may share WACS without replacing the
        # retained v2 capture. The fixed v1 snapshot never acknowledges it.
        retained=(journal().KeySnapshotToken,journal().KeySnapshotStatus,journal().KeySnapshotTopStatus)
        finish_job(5,version=1)
        assert retained==(journal().KeySnapshotToken,journal().KeySnapshotStatus,journal().KeySnapshotTopStatus)
        pmic_hw["registers"][0x1508]=0x55
        status,response=call_pmic(broker_request(1,address=0x1508))
        fields=broker_fields(response);gpu_generation=fields[5]
        for _ in range(12):
            if fields[4]==2:break
            fields=pmic_step(gpu_generation)
        assert fields[4]==2 and fields[8]==0x55
        metadata_ack(gpu_generation,version=1)
        assert retained==(journal().KeySnapshotToken,journal().KeySnapshotStatus,journal().KeySnapshotTopStatus)
        # Fresh opposite-edge status survives an acknowledgement of only the
        # captured press. Capture retirement waits for the metadata Ack.
        pmic_hw["registers"][0x91c]=5
        before_commands=list(pmic_hw["commands"])
        ack_job,fields=finish_job(8,captured_token,ack=False)
        assert fields[7:9]==(0x91c,0x0001beef) and pmic_hw["registers"][0x91c]==4
        assert pmic_hw["commands"][len(before_commands):]==[0x91c,0x2000091c]
        assert pmic_hw["data_writes"][-1]==1 and journal().KeySnapshotValid==1
        metadata_ack(ack_job)
        assert journal().KeySnapshotValid==0
        mmio.clear()
        status,response=call_pmic(broker_request(8,captured_token,version=2))
        fields=broker_fields(response)
        assert status==0 and fields[4:9]==(0,ack_job,0,0x91c,0x0001beef) and not mmio
        pmic_cases.append("v2-borrowed-mask-%x-capture-W1C-preserves-new-release-and-caches-ack"%borrowed)

        # Unrelated enable bits changed by a different client are preserved;
        # only the exact missing-at-acquire power bits are restored.
        pmic_hw["registers"][0x910]|=0x80
        before_commands=list(pmic_hw["commands"])
        released_job,fields=finish_job(7,lease)
        assert fields[7:9]==(0x910,5)
        assert pmic_hw["registers"][0x910]==(0xc2|borrowed) and journal().KeyLeaseToken==0
        if borrowed==5:
            assert pmic_hw["commands"]==before_commands
        mmio.clear()
        status,response=call_pmic(broker_request(7,lease,version=2))
        fields=broker_fields(response)
        assert status==0 and fields[4:9]==(0,released_job,0,0x910,5) and not mmio
        pmic_cases.append("v2-borrowed-mask-%x-release-restores-owned-bits-and-caches-result"%borrowed)

    reset_pmic_channel({0x19e:4,0x91c:5,0x28:2})
    lease,_=finish_job(6)
    captured_token,_=finish_job(5,lease)
    pmic_hw["registers"][0x91c]=0x8007
    old_writes=list(pmic_hw["data_writes"])
    old_commands=len(pmic_hw["commands"])
    finish_job(8,captured_token,result=9)
    assert journal().KeySnapshotValid==1 and journal().KeySnapshotToken==captured_token
    assert pmic_hw["registers"][0x91c]==0x8007 and pmic_hw["data_writes"]==old_writes
    assert pmic_hw["commands"][old_commands:]==[0x91c]
    query_capabilities_preserves_journal(2,3)
    mmio.clear()
    status,response=call_pmic(broker_request(7,lease,version=2))
    assert status==0 and broker_fields(response)[6]==4 and not mmio
    pmic_cases.append("v2-sibling-home-or-upper-status-retains-capture-without-W1C-or-release")
    pmic_hw["registers"][0x91c]=5
    finish_job(8,captured_token)
    assert pmic_hw["registers"][0x91c]==0 and not journal().KeySnapshotValid
    finish_job(7,lease)
    pmic_cases.append("v2-same-snapshot-retry-after-sibling-clears-successfully")

    # Every fixed operation, including accepted jobs, shares the actual UFS
    # WACS admission lock. Busy leaves all request bytes and journal intact.
    for operation,token in ((6,0),(5,1),(7,1),(8,2)):
        reset_pmic_channel()
        if operation in (5,7,8):
            token,_=finish_job(6)
        if operation==8:
            token,_=finish_job(5,token)
        engine.mem_write(power_core_pointer+wacs_stage_offset,struct.pack("<I",2))
        before=bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))
        request=broker_request(operation,token,version=2)
        mmio.clear()
        status,response=call_pmic(request)
        assert status==0 and response==set_pmic_result(request,pmic_busy) and not mmio
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==before
        query_capabilities_preserves_journal(2,3)
        pmic_cases.append("v2-op-%d-UFS-owned-WACS-busy-preserves-input-journal"%operation)

    for label,request in (
        ("v1-acquire",broker_request(6)),
        ("v1-release",broker_request(7,1)),
        ("v1-captured-ack",broker_request(8,1)),
        ("acquire-address",broker_request(6,address=0x910,version=2)),
        ("acquire-value",broker_request(6,value=5,version=2)),
        ("acquire-token",broker_request(6,1,version=2)),
        ("raw-key-enable",broker_request(2,address=0x910,value=5,version=2)),
        ("raw-key-W1C",broker_request(2,address=0x91c,value=5,version=2)),
    ):
        reset_pmic_channel()
        mmio.clear()
        status,response=call_pmic(request)
        assert status==0 and broker_fields(response)[6]==pmic_invalid and not mmio,label
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==empty_broker_journal
        pmic_cases.append("v2-fixed-operation-denies-"+label)

    # The final two generation values are reserved for capture cleanup and
    # lease release. Ordinary work cannot consume them or wrap to zero.
    reset_pmic_channel({0x19e:4,0x91c:1,0x28:2})
    lease,_=finish_job(6);captured_token,_=finish_job(5,lease)
    engine.mem_write(pmic_broker_pointer+_PmicBrokerLayout.Generation.offset,
                     struct.pack("<Q",0xffffffffffffffff-2))
    mmio.clear()
    status,response=call_pmic(broker_request(1,address=0x1508))
    assert status==0 and broker_fields(response)[6]==8 and not mmio
    ack_job,_=finish_job(8,captured_token)
    released_job,_=finish_job(7,lease)
    assert ack_job==0xffffffffffffffff-1 and released_job==0xffffffffffffffff
    mmio.clear()
    status,response=call_pmic(broker_request(6,version=2))
    assert status==0 and broker_fields(response)[6]==8 and not mmio
    assert journal().Generation==0xffffffffffffffff and not journal().KeySnapshotValid
    pmic_cases.append("v2-generation-exhaustion-keeps-exactly-two-cleanup-jobs-and-never-wraps")

    # Accepted W1C may have taken effect even when WACS never drains.
    # The real wrapper must cache a fault, retain ownership and refuse replay.
    reset_pmic_channel({0x19e:4,0x91c:5,0x28:2})
    lease,_=finish_job(6);captured_token,_=finish_job(5,lease)
    pmic_hw["hold_write"]=True
    status,response=call_pmic(broker_request(8,captured_token,version=2))
    fields=broker_fields(response);ack_job=fields[5]
    assert status==0 and fields[4]==1
    for _ in range(16):
        fields=pmic_step(ack_job,2)
        if 0x2000091c in pmic_hw["commands"]:break
        assert fields[4]==1
    else:raise AssertionError("linked captured W1C was not issued")
    assert pmic_hw["registers"][0x91c]==0
    before_commands=list(pmic_hw["commands"]);before_data=list(pmic_hw["data_writes"])
    pmic_hw["now_ns"]+=11000000
    fields=pmic_step(ack_job,2)
    assert fields[4]==3 and fields[6]==6 and journal().KeySnapshotValid
    assert journal().KeyLeaseToken==lease and journal().KeyAckWriteMayHaveEffect==1
    query_capabilities_preserves_journal(2,3)
    mmio.clear()
    assert pmic_step(ack_job,2)[6]==6 and not mmio
    assert pmic_hw["commands"]==before_commands and pmic_hw["data_writes"]==before_data
    assert call(ufs_try_lock,0,upper_stack=True)==0 and not mmio
    pmic_cases.append("v2-uncertain-W1C-timeout-retains-journal-blocks-UFS-and-never-replays")

    reset_pmic_channel({0x1e8c:0x6080,0x1724:0x200,0x1d2a:1})
    for request in (broker_request(11,version=1),broker_request(11,version=2),
                    broker_request(11,version=3),broker_request(0,version=6)):
        status,response=call_pmic(request)
        assert status==0 and broker_fields(response)[6]==pmic_invalid and not mmio
        assert bytes(engine.mem_read(pmic_broker_pointer,broker_journal_size))==empty_broker_journal
    lease,fields=finish_job(11,version=4,ack=False)
    assert journal().ConnLeaseToken==lease and fields[7:9]==(0,0) and not mmio
    metadata_ack(lease,version=4)
    pmic_cases.append("v4-lease-requires-v4-and-does-no-MMIO")

    _,fields=finish_job(13,lease,version=4,address=0x1d2a)
    assert fields[8]==1 and pmic_hw["commands"]==[0x1d2a]
    for address,value,result in ((0x1d2a,0,pmic_invalid),(0x1726,0x30,pmic_invalid),
                                 (0x1724,0x210,pmic_invalid),(0x1e8c,0x6c80,7)):
        token=lease+1 if result==7 else lease
        status,response=call_pmic(broker_request(14,token,address,value,version=4))
        assert status==0 and broker_fields(response)[6]==result and not mmio
    finish_job(14,lease,version=4,result=pmic_invalid,address=0x1e8c,value=0x6c90)
    assert pmic_hw["data_writes"]==[] and not journal().ConnDirty
    pmic_cases.append("v4-write-rejects-WiFi-foreign-vote-unowned-field-and-wrong-lease")

    for address,value,readback,expected in ((0x1e8c,0x6c80,0x1e8c,0x6c80),
                                             (0x1726,0x10,0x1724,0x210),
                                             (0x1728,0x10,0x1724,0x200),
                                             (0x1e8c,0x6080,0x1e8c,0x6080)):
        commands=len(pmic_hw["commands"])
        finish_job(14,lease,version=4,address=address,value=value)
        assert pmic_hw["commands"][commands:]==[readback,0x20000000|address,readback]
        assert pmic_hw["data_writes"][-1]==value and pmic_hw["registers"][readback]==expected
        if expected==0x6c80:
            status,response=call_pmic(broker_request(12,lease,version=4))
            assert status==0 and broker_fields(response)[6]==4 and not mmio
            assert journal().ConnLeaseToken==lease and journal().ConnDirty
    assert not journal().ConnDirty
    for _ in range(2):
        release=broker_request(12,lease,version=4)
        status,response=call_pmic(release)
        assert status==0 and response==release and not mmio and not journal().ConnLeaseToken
    pmic_cases.append("v4-voltage-and-voter-readback-preserve-other-bits-release-on-restoration")

    # Bind every emitted procedure in both owning objects to this exact PE.
    complete={}
    objects={
        "MtkUfsPrm":link_map.parent.parent/"OUTPUT/MtkUfsPrm.obj",
        "MtkPmicBroker":link_map.parent.parent/"OUTPUT/MtkPmicBroker.obj",
        "MtkUfsPowerCore":build/"AARCH64/MediaTekPkg/Library/UfsPowerLib/MtkUfsPowerCoreLib/OUTPUT/MtkUfsPowerCore.obj"}
    for owner,obj in objects.items():
        _,procedures=codeview_inline_sites(obj)
        regions={int(m[1],16):int(m[2],16) for m in re.finditer(
            r"^([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+4\s+.*?/"+
            re.escape(owner)+r"\.obj:\(\.text\)\s*$",map_text,re.M) if int(m[2],16)}
        assert regions
        emitted={name:rva for name,rva in records if rva in regions and name in procedures}
        assert set(regions)<=set(emitted.values()),(owner,"unresolved owning procedure")
        for name,rva in emitted.items():
            assert regions[rva]==procedures[name]
            code=bytes(engine.mem_read(base+rva,procedures[name]))
            complete[owner+"/"+name]={"rva":rva,"bytes":len(code),
                "object_sha256":hashlib.sha256(obj.read_bytes()).hexdigest(),
                "code_sha256":hashlib.sha256(code).hexdigest(),
                **coff_procedure_matches(obj.read_bytes(),name,code)}
    assert "MtkUfsPowerCore/MtkUfsPowerCoreHandle" in complete
    assert "MtkPmicBroker/MtkPmicBrokerInitialize" in complete
    assert "MtkPmicBroker/MtkPmicBrokerBlocksUfs" in complete
    assert "MtkPmicBroker/MtkPmicBrokerHandle" in complete
    assert "MtkPmicBroker/MtkPmicBrokerWacsWriteAllowed" in complete
    return {"status":"passed","PE_sha256":hashlib.sha256(raw).hexdigest(),
        "map_sha256":hashlib.sha256(link_map.read_bytes()).hexdigest(),
        "invalid_context_cases":rejected,"callback_cases":callback_cases,
        "pmic_broker_runtime_cases":pmic_cases,
        "pmic_broker_runtime_case_count":len(pmic_cases),
        "pmic_broker_journal_bytes":broker_journal_size,
        "pmic_broker_wire_versions_executed":[1,2,3,4],
        "pmic_broker_sd_power_v3_coverage":"capability query only",
        "captured_key_W1C_and_enable_lease_model":True,
        "converted_virtual_bank_addresses":virtual,
        "complete_owning_procedure_regions":complete,
        "scope":"Actual linked ARM64 PMIC broker core and PRM wrapper execute against a bounded WACS2 MMIO model; UFS power-core dispatch remains a fixture, while its actual PRM I/O callbacks and shared-lock admission are exercised. Timer leaves and EL3 SMC are fixtures. No firmware entry/PRM loader/Windows/hardware execution; portable UFS power-core has separate sanitizer tests."}


def verify_page_tables(engine, state, translations, el):
    """Walk real ArmMmuLib output; CPU registers/cache operations are modeled."""
    assert state["TCR"] & 63 == 28  # Modeled 36-bit PA, 4 KiB granule.
    assert state["MAIR"] == 0xffbb4400
    assert state["enable_requested"]
    if el == 1:
        assert (state["TCR"] >> 32) & 7 == 1
    else:
        assert state["TCR"] & ((1 << 31) | (1 << 23)) == (1 << 31) | (1 << 23)
        assert (state["TCR"] >> 16) & 7 == 1
    pages=set(); leaves=[]
    address_mask=0xfffffffff000
    attribute_mask=(0xfff << 52) | 0xffc

    def walk(table, level, base, count=512):
        assert table % 4096 == 0 and 0x44400000 <= table < 0x47c80000
        assert table not in pages, "page-table cycle or aliased table"
        pages.add(table)
        entries=struct.unpack("<"+"Q"*count,engine.mem_read(table,count*8))
        span=1 << (39-9*level)
        for i,entry in enumerate(entries):
            if not entry & 1:
                assert entry == 0
                continue
            va=base+i*span
            if level < 3 and entry & 3 == 3:
                assert entry & ~address_mask == 3, "unexpected table restrictions"
                walk(entry & address_mask,level+1,va)
            else:
                assert entry & 3 == (3 if level == 3 else 1)
                assert entry & address_mask == va and va+span <= 1 << 36
                leaves.append((va,span,entry & attribute_mask))
    walk(state["TTBR0"],1,0,64)

    def merged(rows):
        result=[]
        for base,length,attributes in sorted(rows):
            if result:
                previous,size,flags=result[-1]
                assert previous+size <= base, "overlapping CPU mappings"
                if previous+size == base and flags == attributes:
                    result[-1]=(previous,size+length,flags)
                    continue
            result.append((base,length,attributes))
        return result
    # ArmLib enum: uncached normal=0, write-back=1, device=6.
    attributes={0:0x404,1:0x70c,6:0x400 | ((3 if el == 1 else 2) << 53)}
    expected=[(base,length,attributes[arm]) for base,length,arm in translations]
    assert merged(leaves) == merged(expected), "actual page tables differ from complete CPU region map"
    digest=hashlib.sha256()
    for table in sorted(pages):
        digest.update(struct.pack("<Q",table));digest.update(engine.mem_read(table,4096))
    return {"modeled_EL":el,"modeled_PA_bits":36,"actual_ArmConfigureMmu_executed":True,
            "page_table_pages":len(pages),"leaf_entries":len(leaves),
            "identity_addresses_and_attributes_match_all_regions":True,
            "unmapped_holes_preserved":True,"TCR":hex(state["TCR"]),
            "MAIR":hex(state["MAIR"]),"table_content_sha256":digest.hexdigest(),
            "hardware_enable_requested_but_substituted":True}


def runtime_ccci_tags(modem_base=0x42000000, modem_size=0x04000000):
    # The board's runtime DT points to these synthetic LK tags in RAM.
    tags = bytearray(0x1000)
    tags[0:16] = b"hdr_count".ljust(16, b"\0")
    struct.pack_into("<III", tags, 16, 0x100, 4, 28)
    tags[28:44] = b"hdr_tbl_inf".ljust(16, b"\0")
    struct.pack_into("<III", tags, 44, 0x104, 4 * 24, 0)
    struct.pack_into("<I", tags, 0x100, 4)
    for index in range(4):
        entry = 0x104 + index * 24
        struct.pack_into("<QIBB", tags, entry, modem_base if index == 0 else 0,
                         modem_size if index == 0 else 0, index, int(index != 0))
    return bytes(tags)


def runtime_fdt_fixture(video=None, simple=False, bad=None, dynamic=False, display=False,
            ccci_base=0x100400100, console=False, dsi=False):
    def cells(*values): return struct.pack(">"+"I"*len(values),*values)
    def reg(base,size): return cells(base>>32,base&0xffffffff,size>>32,size&0xffffffff)
    memory = [("memory@40000000",{"device_type":b"memory\0",
               "reg":reg(0x40000000,0x10000000) if console else reg(0x40000000,0x80000000)+reg(0x100000000,0x100000000)},[])]
    reserves = [("modem@60000000",{"reg":reg(0x60000000,0x10000000),"no-map":b""},[]),
                ("cma@a0000000",{"reg":reg(0xa0000000,0x10000000),"reusable":b""},[])]
    if dynamic:
        for name,size,alignment,flags,limit in (
            ("tkcore",0x1000000,0x1000,{"no-map":b""},0x80000000),
            ("ssmr-reserved-cma_memory",0x10000000,0x1000000,{"reusable":b""},None),
            ("ccci-dpmaif-cache-memory",0x50000,0x10000,{},0xf0000000),
            ("ccci-dpmaif-nocache-memory",0x20000,0x10000,{"no-map":b""},0xf0000000)):
            props={"size":cells(size>>32,size&0xffffffff),
                   "alignment":cells(alignment>>32,alignment&0xffffffff),**flags}
            if limit is not None: props["alloc-ranges"]=reg(0x40000000,limit)
            if bad=="dynamic-size" and name=="tkcore": props["size"]=b""
            if bad=="dynamic-flags" and name=="tkcore": props["reusable"]=b""
            if bad=="dynamic-alignment" and name=="tkcore": props["alignment"]=cells(0,0x3000)
            if bad=="dynamic-overflow" and name=="tkcore": props["alloc-ranges"]=reg(0xffffff000,0x2000)
            reserves.append((name,props,[]))
    if bad=="overlapping-no-map":
        reserves.append(("overlap",{"reg":reg(0xa1000001,0x1000),"no-map":b""},[]))
    chosen = {"#address-cells":cells(2),"#size-cells":cells(2)}
    if video is not None:
        chosen.update({"atag,videolfb-fb_base_h":cells(video[0]>>32),
                       "atag,videolfb-fb_base_l":cells(video[0]&0xffffffff),
                       "atag,videolfb-vramSize":cells(video[1])})
    if display:
        assert video is not None and not simple
        chosen.update({"atag,videolfb-islcmfound":cells(1),
                       "atag,videolfb-islcm_inited":cells(1)})
    if bad=="partial": del chosen["atag,videolfb-fb_base_h"]
    framebuffer = []
    if simple:
        desc = {"compatible":b"simple-framebuffer\0","format":b"x8r8g8b8\0",
                "width":cells(1400 if console else 2),"height":cells(2240 if console else 3),"stride":cells(5632 if console else 16),
                "reg":reg(0x49000000,0xc10000) if console else reg(0x80000000,0x5000)}
        if bad=="format": desc["format"]=b"r5g6b5\0"
        framebuffer = [("framebuffer@49000000" if console else "framebuffer@80000000",desc,[])]
        if bad!="unreserved":
            desc["memory-region"]=cells(31)
            reserves += [("framebuffer@49000000" if console else "framebuffer@80000000",{"reg":reg(0x49000000,0xc10000) if console else reg(0x80000000,0x5000),"phandle":cells(31)},[])]
    if bad=="multi-fixed":
        reserves.insert(0,("two-fixed-ranges",{"reg":reg(0x90000000,0x2000)+reg(0x91000000,0x3000),"no-map":b""},[]))
    if bad=="multi-framebuffer":
        assert simple and reserves[-1][0]=="framebuffer@80000000"
        reserves[-1][1]["reg"]=reg(0x90000000,0x2000)+reg(0x49000000,0xc10000) if console else reg(0x80000000,0x5000)
        reserves[-1][1]["phandle"]=cells(31)
        desc["memory-region"]=cells(31)
    # Mirror the exact stock compatible/reg allowlist in Tests/Display/LkDisplayFixture.h.
    display_nodes=[]
    if display or dsi:
        for name,compatible,base in (
            ("power-controller@10006000","mediatek,mt6789-scpsys",0x10006000),
            ("infracfg@10001000","mediatek,mt6789-infracfg_ao",0x10001000),
            ("mmsys@14000000","mediatek,mt6789-mmsys",0x14000000),
            ("larb0@14003000","mediatek,smi_larb0",0x14003000),
            ("larb1@14004000","mediatek,smi_larb1",0x14004000),
            ("ovl0@14005000","mediatek,disp_ovl0",0x14005000),
            ("ovl2l@14006000","mediatek,disp_ovl0_2l",0x14006000),
            ("rdma0@14007000","mediatek,disp_rdma0",0x14007000),
            ("dsi0@14013000","mediatek,dsi0",0x14013000),
            ("mipi_tx_config@11f60000","mediatek,mipi_tx_config0",0x11F60000)):
            display_nodes.append((name,{"compatible":compatible.encode()+b"\0",
                                        "reg":reg(base,0x1000)},[]))
    if bad=="mblock-framebuffer-container":
        assert video is not None
        reserves.append(("mblock-7-framebuffer",{"compatible":b"mediatek,framebuffer\0",
                         "reg":reg(video[0],video[1]+0x2000),"no-map":b""},[]))
    root = ("",{"#address-cells":cells(2),"#size-cells":cells(2),
                **({"compatible":b"mediatek,MT6789\0"} if dsi else {})},
            memory+[("reserved-memory",{"#address-cells":cells(2),"#size-cells":cells(2),"ranges":b""},reserves),
            ("modem",{"ccci,modem_info":struct.pack("<QII",ccci_base,0x1000,2)},[]),
            ("chosen",chosen,framebuffer)]+display_nodes)
    strings = bytearray(); names = {}
    def pad(data): return data+bytes((-len(data))%4)
    def node(item):
        name,props,children=item
        data=cells(1)+pad(name.encode()+b"\0")
        for name,value in props.items():
            if name not in names:
                names[name]=len(strings);strings.extend(name.encode()+b"\0")
            data+=cells(3,len(value),names[name])+pad(value)
        return data+b"".join(node(child) for child in children)+cells(2)
    structure=node(root)+cells(9)
    reserve_entries=[(0x50000001,0x1001)]
    if bad in ("duplicate-LK-reservation","partial-LK-reservation","shifted-LK-reservation"):
        assert video is not None
        start,size=video
        if bad=="partial-LK-reservation":start+=4096;size-=4096
        if bad=="shifted-LK-reservation":start+=1;size-=1
        reserve_entries.append((start,size))
    reserve=b"".join(struct.pack(">2Q",*entry) for entry in reserve_entries+[(0,0)])
    offset=40+len(reserve);end=offset+len(structure)
    return cells(0xd00dfeed,end+len(strings),offset,end,40,17,16,0,len(strings),len(structure))+reserve+structure+strings



def full_stock_lk_validation_case(fd, build, stock):
    import time
    from unicorn.arm64_const import UC_ARM64_REG_X5
    b=build
    h=struct.unpack_from('>10I',stock);strings=stock[h[3]:h[3]+h[8]];pos=h[2]
    def word():
     nonlocal pos
     v=struct.unpack_from('>I',stock,pos)[0];pos+=4;return v
    def node():
     nonlocal pos
     assert word()==1
     end=stock.index(0,pos);name=stock[pos:end].decode();pos=(end+4)&~3;props={};children=[]
     while True:
      tag=struct.unpack_from('>I',stock,pos)[0]
      if tag==1:children.append(node())
      elif tag==3:
       word();length=word();offset=word();end=strings.index(0,offset)
       props[strings[offset:end].decode()]=stock[pos:pos+length];pos=(pos+length+3)&~3
      elif tag==4:word()
      else:assert word()==2;return [name,props,children]
    root=node()
    cells=lambda *v:struct.pack('>'+'I'*len(v),*v)
    chosen=next(x for x in root[2] if x[0]=='chosen')
    for n,v in (('fb_base_h',0),('fb_base_l',0x90000000),('vramSize',0x20000),('islcmfound',1),('islcm_inited',0)):
     chosen[1]['atag,videolfb-'+n]=cells(v)
    root[2]=[x for x in root[2] if x[1].get('device_type')!=b'memory\0']
    root[2].insert(0,['memory@40000000',{'device_type':b'memory\0','reg':cells(0,0x40000000,0,0xc0000000)},[]])
    newstrings=bytearray();names={}
    def pad(v):return v+bytes(-len(v)%4)
    def wire(n):
     name,props,children=n;v=cells(1)+pad(name.encode()+b'\0')
     for name,value in props.items():
      if name not in names:names[name]=len(newstrings);newstrings.extend(name.encode()+b'\0')
      v+=cells(3,len(value),names[name])+pad(value)
     return v+b''.join(wire(c) for c in children)+cells(2)
    tree=wire(root)+cells(9);start=56;end=start+len(tree)
    blob=cells(0xd00dfeed,end+len(newstrings),start,end,40,17,16,0,len(newstrings),len(tree))+bytes(16)+tree+newstrings
    m=(b/'AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map').read_text()
    syms={n:int(a,16) for a,n in re.findall(r'^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$',m,re.M)}
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for a,n in ((0x44000000,len(fd)),(0x47c80000,0x200000),(0x70000000,0x100000),(0x50000000,0x1000),(0x60000000,0x1000)):uc.mem_map(a,n)
    uc.mem_write(0x44000000,fd);uc.mem_write(0x47c80000,bytes(blob))
    for r,v in ((UC_ARM64_REG_X0,0x47c80000),(UC_ARM64_REG_X1,len(blob)),(UC_ARM64_REG_X2,0x44000000+syms['ReadDisplay32']),
     (UC_ARM64_REG_X3,0x44000000+syms['WriteDisplay32']),(UC_ARM64_REG_X4,0),(UC_ARM64_REG_X5,0x50000000),
     (UC_ARM64_REG_X30,0x60000000),(UC_ARM64_REG_SP,0x70100000-16)):uc.reg_write(r,v)
    state={'estimated_instruction_count':0,'first_MMIO_callback_reached':False}
    def block(u,pc,n,_):state['estimated_instruction_count']+=n//4
    def code(u,pc,n,_):
     if pc==0x44000000+syms['ReadDisplay32']:
      state['first_MMIO_callback_reached']=True;u.emu_stop()
    uc.hook_add(UC_HOOK_BLOCK,block);uc.hook_add(UC_HOOK_CODE,code,begin=0x44000000+syms['ReadDisplay32'],end=0x44000000+syms['ReadDisplay32'])
    start=time.monotonic();uc.emu_start(0x44000000+syms['MtkDisplayAcquireLkHandoff'],0x60000000,timeout=30000000,count=100000000)
    state.update(full_stock_fixture_bytes=len(blob),fixture_runtime_values_modeled=True,native_CPU_emulation_seconds=time.monotonic()-start,PC=hex(uc.reg_read(UC_ARM64_REG_PC)),X0_at_stop=hex(uc.reg_read(UC_ARM64_REG_X0)))
    assert state['first_MMIO_callback_reached'], state
    state.update(status='passed',stock_DTB_sha256=hashlib.sha256(stock).hexdigest(),fixture_sha256=hashlib.sha256(blob).hexdigest(),scope='Actual SEC display acquisition up to its first MMIO callback; full stock DTB with modeled runtime RAM/video cells; CPU instruction count is not hardware timing')
    return state

def sec_exception_cases(fd, build):
    """Run the real SEC exception call and board writer, with modeled MRS/cache leaves."""
    import unicorn.arm64_const as regs
    mapping=build/"AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map"
    symbols={name:int(address,16) for address,name in re.findall(
        r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$",mapping.read_text(),re.M)}
    caller=DEST+symbols["SecCommonExceptionEntry"]
    writer=DEST+symbols["T70ProRecordSecException"]
    md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_LITTLE_ENDIAN)
    results=[]
    for level,kind,invalid in [(level,kind,None) for level in (4,8) for kind in range(4)]+[
            (4,0,"magic"),(4,0,"pre-C"),(4,0,"phase-high"),(12,0,"EL")]:
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        uc.mem_map(DEST,len(fd));uc.mem_write(DEST,fd)
        uc.mem_map(HANDOFF,0x1000);uc.mem_write(HANDOFF,bytes([0xA5])*0x1000)
        uc.mem_map(0x43f80000,0x80000);uc.reg_write(UC_ARM64_REG_SP,0x44000000-16)
        uc.mem_write(HANDOFF,struct.pack("<Q",0 if invalid=="magic" else 0x0000000148303754))
        uc.mem_write(HANDOFF+0x38,struct.pack("<Q",2 if invalid=="pre-C" else 5 if invalid=="phase-high" else 3))
        before=bytes(uc.mem_read(HANDOFF,4096))
        uc.reg_write(UC_ARM64_REG_X0,kind);uc.reg_write(UC_ARM64_REG_X1,DEST+0x1234)
        uc.reg_write(UC_ARM64_REG_X30,0x60000000)
        leaves={"currentel":level,"esr_el1":0x96000021,"esr_el2":0x96000021,
                "far_el1":0x47c81234,"far_el2":0x47c81234,
                "sctlr_el1":0x30d00800,"sctlr_el2":0x30d00800,
                "spsr_el1":0x3c5,"spsr_el2":0x3c9}
        state={"entered":False,"return":None,"saved":None,"cache":[]};writes=[]
        def hook(engine,pc,size,_):
            if pc==writer:
                state["entered"]=True;state["return"]=engine.reg_read(UC_ARM64_REG_X30)
                state["saved"]=[engine.reg_read(getattr(regs,"UC_ARM64_REG_X"+str(n))) for n in range(19,30)]
            elif state["return"]==pc:
                assert state["saved"]==[engine.reg_read(getattr(regs,"UC_ARM64_REG_X"+str(n))) for n in range(19,30)]
                engine.emu_stop();return
            ins=next(md.disasm(bytes(engine.mem_read(pc,4)),pc))
            if ins.mnemonic=="mrs":
                register,leaf=ins.op_str.split(", ")
                assert leaf in leaves,leaf
                engine.reg_write(getattr(regs,"UC_ARM64_REG_"+register.upper()),leaves[leaf])
                engine.reg_write(UC_ARM64_REG_PC,pc+4)
            elif ins.mnemonic=="dc":
                operation,register=ins.op_str.split(", ")
                assert operation=="cvac"
                state["cache"].append(engine.reg_read(getattr(regs,"UC_ARM64_REG_"+register.upper())))
                engine.reg_write(UC_ARM64_REG_PC,pc+4)
        def write(engine,access,address,size,value,_):
            assert HANDOFF+0x800<=address and address+size<=HANDOFF+0x848 or 0x43f80000<=address<0x44000000,hex(address)
            if address<0x43f80000:writes.append((address,size,value))
        uc.hook_add(UC_HOOK_CODE,hook);uc.hook_add(UC_HOOK_MEM_WRITE,write)
        uc.emu_start(caller,0,count=10000)
        assert state["entered"] and uc.reg_read(UC_ARM64_REG_PC)==state["return"]
        after=bytearray(uc.mem_read(HANDOFF,4096))
        if invalid:
            assert bytes(after)==before and writes==[] and state["cache"]==[]
        else:
            record=struct.unpack("<QIIII5Q2I",after[0x800:0x848])
            assert record==(0x000000054c443754,HANDOFF+0x60,0,level,kind,
                            0x96000021,DEST+0x1234,0x47c81234,0x30d00800,
                            0x3c5 if level==4 else 0x3c9,0,0),record
            assert writes[0]==(HANDOFF+0x800,8,0) and writes[-1]==(HANDOFF+0x800,8,0x000000054c443754)
            assert state["cache"]==list(range(HANDOFF+0x800,HANDOFF+0x848,8))
            after[0x800:0x848]=before[0x800:0x848];assert bytes(after)==before
        results.append({"modeled_CurrentEL":level,"type":kind,"rejected":invalid,
                        "real_SEC_exception_caller_and_board_writer_executed":True,
                        "only_own_72_byte_record_written":not invalid})
    return {"status":"passed","cases":results,"case_count":len(results),
            "scope":"Actual compiled SEC exception caller and board assembly writer; MRS/cache leaves modeled; hardware exception delivery and reset untested",
            "device_storage_writes":[]}


def runtime_memory_cases(fd, build, stock):
    """Execute the actual SEC GetMemoryMap in-place in the finished FD."""
    package = Path(__file__).resolve().parent
    mapping = build/"AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map"
    symbols = {name:int(address,16) for address,name in re.findall(
        r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$",mapping.read_text(),re.M)}
    assert "GetMemoryMap" in symbols and "BuildRuntimeMemoryMap" in symbols
    pos = (struct.unpack_from("<H",fd,48)[0]+7)&~7
    assert fd[pos:pos+16] == uuid.UUID("9affb503-e643-4141-8b90-17e8588b1d35").bytes_le
    length = int.from_bytes(fd[pos+20:pos+23],"little")
    te = [data for kind,data in sections(fd[pos+24:pos+length]) if kind==0x12]
    assert len(te)==1 and te[0][:4]==b"VZ\x64\xaa"
    stripped = struct.unpack_from("<H",te[0],6)[0]-40
    assert struct.unpack_from("<Q",te[0],16)[0]==DEST
    assert fd[stripped:stripped+len(te[0])]==te[0]
    for i in range(te[0][4]):
        vsize,rva,rsize,raw = struct.unpack_from("<IIII",te[0],40+i*40+8)
        assert raw>=stripped and raw+rsize-stripped<=len(te[0])
        assert fd[rva:rva+rsize]==te[0][raw-stripped:raw-stripped+rsize]

    fixture = runtime_fdt_fixture

    error=1<<63
    cases = [
        ("no-video-tags",fixture(),0,None,False),
        ("LK-VRAM",fixture((0x80001003,0x1234)),0,(0x80001000,0x80003000),False),
        ("LK-VRAM-above-4GiB",fixture((0x180001003,0x1234)),0,(0x180001000,0x180003000),False),
        ("LK-display-fallback",fixture((0x90000000,0x20000),display=True),0,(0x90000000,0x90020000),True),
        ("LK-named-framebuffer-container",fixture((0x90000000,0x20000),display=True,bad="mblock-framebuffer-container"),0,(0x90000000,0x90022000),True),
        ("LK-display-exact-memreserve",fixture((0x90000000,0x20000),display=True,bad="duplicate-LK-reservation"),0,(0x90000000,0x90020000),True),
        ("LK-display-partial-memreserve",fixture((0x90000000,0x20000),display=True,bad="partial-LK-reservation"),error|15,None,False),
        ("LK-display-shifted-memreserve",fixture((0x90000000,0x20000),display=True,bad="shifted-LK-reservation"),error|15,None,False),
        ("CCCI-overlaps-simple-GOP",fixture(simple=True,ccci_base=0x80001000),error|15,None,False),
        ("CCCI-overlaps-LK-display",fixture((0x90000000,0x20000),display=True,
                                             ccci_base=0x90001000),error|15,None,False),
        ("partial-LK-tags",fixture((0x80000000,0x8000),bad="partial"),error|33,None,False),
        ("zero-VRAM",fixture((0x80000000,0)),error|6,None,False),
        ("bootstrap-overlap",fixture((0x44000000,0x8000)),error|15,None,False),
        ("preloader-AEE-overlap",fixture((DTB_END,0x8000)),error|15,None,False),
        ("outside-DRAM",fixture((0x380000000,0x8000)),error|15,None,False),
        ("PA-overflow",fixture((0xffffff000,0x2000)),error|33,None,False),
        ("GOP-UC-map",fixture((0x80000000,0x8000),simple=True),0,(0x80000000,0x80008000),True),
        ("standard-GOP-only",fixture(simple=True),0,(0x80000000,0x80005000),True),
        ("GOP-after-multi-tuple-fixed-pool",fixture(simple=True,bad="multi-fixed"),0,(0x80000000,0x80005000),True),
        ("GOP-in-second-phandle-tuple",fixture(simple=True,bad="multi-framebuffer"),0,(0x80000000,0x80005000),True),
        ("GOP-exceeds-LK-VRAM",fixture((0x80000000,0x1000),simple=True),error|33,None,False),
        ("unsupported-GOP-format",fixture((0x80000000,0x8000),simple=True,bad="format"),error|3,None,False),
        ("unreserved-GOP",fixture(simple=True,bad="unreserved"),error|15,None,False),
        ("OS-dynamic-pools",fixture(dynamic=True),0,None,False),
        ("GOP-with-OS-dynamic-pools",fixture((0x80000000,0x8000),simple=True,dynamic=True),0,(0x80000000,0x80008000),True),
        ("standard-GOP-with-OS-dynamic-pools",fixture(simple=True,dynamic=True),0,(0x80000000,0x80005000),True),
        ("dynamic-bad-size",fixture(dynamic=True,bad="dynamic-size"),error|33,None,False),
        ("dynamic-bad-flags",fixture(dynamic=True,bad="dynamic-flags"),error|33,None,False),
        ("dynamic-bad-alignment",fixture(dynamic=True,bad="dynamic-alignment"),error|33,None,False),
        ("dynamic-overflow",fixture(dynamic=True,bad="dynamic-overflow"),error|33,None,False),
        ("no-map-inside-fixed-CMA",fixture(bad="overlapping-no-map"),0,None,False),
        ("original-stock-no-runtime-modem-data",stock,error|6,None,False),
    ]
    def ccci_tag_block():
        data=bytearray(0x1000)
        data[0:16]=b"hdr_count\0".ljust(16,b"\0")
        struct.pack_into("<III",data,16,0x100,4,28)
        data[28:44]=b"hdr_tbl_inf\0".ljust(16,b"\0")
        struct.pack_into("<III",data,44,0x104,4*24,0)
        struct.pack_into("<I",data,0x100,4)
        for index in range(4):
            at=0x104+index*24
            struct.pack_into("<Q",data,at,0x42000000 if index==0 else 0)
            struct.pack_into("<I",data,at+8,0x04000000 if index==0 else 0)
            data[at+12]=index
            data[at+13]=0 if index==0 else 1
        return bytes(data)

    ccci_tags=ccci_tag_block()
    ccci_info_bases=(0x100400100,0x80001000,0x90001000)
    output = 0x50000000; stack = 0x70000000; sentinel = 0x60000000
    results=[]
    bootstrap_count=len(re.findall(r'\{"[^"\n]+",\s*0x', (package/"Library/MemoryMapLib/MemoryMapLib.c").read_text()))
    for name,tree,expected,video,mapped in cases:
        display_case=name in ("LK-named-framebuffer-container","LK-display-fallback","CCCI-overlaps-LK-display","LK-display-exact-memreserve","LK-display-partial-memreserve","LK-display-shifted-memreserve")
        display_rejected=name in ("CCCI-overlaps-LK-display","LK-display-partial-memreserve","LK-display-shifted-memreserve")
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for address,size in ((DEST,len(fd)),(HANDOFF,0x1000),(DTB_BASE,0x200000),
                             (output,0x1000),(stack,0x100000),(sentinel,0x1000),(0x44400000,0x3880000)):
            uc.mem_map(address,size)
        for info_base in ccci_info_bases:
            uc.mem_map(info_base&~0xfff,0x10000)
            uc.mem_write(info_base,ccci_tags)
        display_reads=[];display_writes=[]
        required_reads=set()
        if display_case:
            for address,size in ((0x10001000,0x1000),(0x10006000,0x1000),(0x14000000,0x14000)):
                uc.mem_map(address,size)
            # Values mirror LkDisplayFixtureInitialize with OVL0/layer0 and a non-opaque pitch.
            display_registers={
                0x1000616C:0x200000,0x10006170:0x200000,0x10006354:0xD,
                0x100012D0:0,0x100012EC:0,0x10001220:0,0x10001228:0,
                0x14000100:0,0x140001A0:0,0x14007010:1,
                0x14013000:1,0x14013014:1,0x14013018:0x3C,0x1401301C:0x2C030000|(64*3),
                0x14013020:8,0x14013024:32,0x14013028:230,0x1401302C:32,
                0x14013038:(32<<16)|64,0x14013050:16,0x14013054:88,0x14013058:108,0x1401305C:0,
                0x14000F18:1,0x14000F14:0,0x14000F28:0,0x14000F0C:0,0x14000F30:0,0x14000F20:0,
                0x14003384:0,0x14003388:0,0x1400500C:0x400001,
                0x14005020:(32<<16)|64,0x14005024:0x05000001,0x1400502C:1,
                0x140052D0:0,0x14005324:0,0x14005030:0x21FF,
                0x14005038:(32<<16)|64,0x1400503C:0,0x14005040:0,
                0x14005044:260,0x1400504C:0,0x140050C0:1,0x14005F40:0x90000000,
                0x14007014:64,0x14007018:32}
            for address,value in display_registers.items():
                uc.mem_write(address,struct.pack("<I",value))
            display_ranges=((0x10001000,0x10002000),(0x10006000,0x10007000),(0x14000000,0x14014000))
            def display_read(engine,access,address,size,value,user_data):
                if any(begin<=address<end for begin,end in display_ranges):
                    display_reads.append((address,size))
            def display_write(engine,access,address,size,value,user_data):
                if any(begin<=address<end for begin,end in display_ranges):
                    display_writes.append((address,size,value))
            uc.hook_add(UC_HOOK_MEM_READ,display_read)
            uc.hook_add(UC_HOOK_MEM_WRITE,display_write)
        uc.mem_write(DEST,fd);uc.mem_write(DTB_BASE,bytes(tree))
        uc.mem_write(HANDOFF,struct.pack("<3Q",0x0000000148303754,DTB_BASE,len(tree)))
        uc.mem_write(HANDOFF+0x38,struct.pack("<Q",3))
        uc.mem_write(output,bytes([0xa5])*0x1000)
        uc.reg_write(UC_ARM64_REG_X0,output);uc.reg_write(UC_ARM64_REG_X1,output+8)
        uc.reg_write(UC_ARM64_REG_X30,sentinel);uc.reg_write(UC_ARM64_REG_SP,stack+0x100000-16)
        # The full shipping FDT has far more nodes than the small wire fixtures.
        uc.emu_start(DEST+symbols["GetMemoryMap"],sentinel,timeout=30000000,count=100000000)
        assert uc.reg_read(UC_ARM64_REG_PC)==sentinel,(name,"SEC did not return",hex(uc.reg_read(UC_ARM64_REG_PC)))
        status,memory_meta=struct.unpack("<2Q",uc.mem_read(HANDOFF+0x60,16))
        assert memory_meta >> 48 == 0x4D4D
        assert (memory_meta >> 40) & 0xFF <= 11
        additional=memory_meta & 0xFFFFFFFF
        sec_snapshot=struct.unpack("<Q16I",uc.mem_read(HANDOFF+0x800,72))
        assert sec_snapshot[:3]==(0x000000044c443754,HANDOFF+0x60,0)
        assert sec_snapshot[3]==(memory_meta >> 40) & 0xFF
        assert sec_snapshot[4]==additional
        assert sec_snapshot[5] | sec_snapshot[6]<<32 == status
        assert sec_snapshot[11]<=4 and sec_snapshot[12:]==(0,)*5
        if not display_case:assert sec_snapshot[9:12]==(0,0,0)
        pointer=struct.unpack("<Q",uc.mem_read(output,8))[0];count=uc.mem_read(output+8,1)[0]
        layout_pointer=pointer
        assert status==expected,(name,hex(status),hex(expected))
        memory_phase=(memory_meta >> 40) & 0xFF
        if expected==0:assert memory_phase==11,(name,memory_phase)
        if name=="original-stock-no-runtime-modem-data":assert memory_phase==4
        if display_rejected:assert memory_phase==6
        if display_case and display_rejected:
            assert display_reads==[] and display_writes==[],(name,"CCCI collision reached display MMIO",display_reads,display_writes)
        elif display_case:
            read_addresses=[address for address,size in display_reads]
            required_reads={0x1000616C,0x10006170,0x10006354,
                0x100012D0,0x100012EC,0x10001220,0x10001228,
                0x14000100,0x140001A0,0x14007010,0x14013000,0x14013014,
                0x14013018,0x1401301C,0x14013020,0x14013024,0x14013028,0x1401302C,
                0x14013038,0x14013050,0x14013054,0x14013058,0x1401305C,
                0x14000F18,0x14000F14,0x14000F28,0x14000F0C,0x14000F30,0x14000F20,
                0x14003384,0x14003388,0x14007014,0x14007018}
            assert required_reads<=set(read_addresses),(name,"SPM/INFRA/MMSYS/RDMA/DSI/SMI guards not read",required_reads-set(read_addresses))
            snapshot=(0x1400500C,0x14005020,0x14005024,0x1400502C,0x140052D0,0x14005324,
                      0x14005030,0x14005038,0x1400503C,0x14005040,0x14005044,0x1400504C,
                      0x140050C0,0x14005F40)
            assert all(read_addresses.count(address)==(4 if address==0x14005044 else 3)
                       for address in snapshot),(name,"OVL snapshots were not stable/repeated")
            dsi_state = tuple(0x14013000 + offset for offset in
                              (0x14,0x18,0x1C,0x20,0x24,0x28,0x2C,0x38,0x50,0x54,0x58,0x5C))
            # +0x14 is also checked twice by ReadLkAccessState; each other
            # control is read once in each of the three stable DSI snapshots.
            assert all(read_addresses.count(address)==(5 if address==0x14013014 else 3)
                       for address in dsi_state),(name,"LK DSI controls were not read in every stable snapshot")
            assert all(size==4 for address,size in display_reads if address in dsi_state),(
                name,"LK DSI control read width changed")
            assert display_writes==[(0x14005044,4,0x10000104)],(name,"unexpected display MMIO write",display_writes)
            assert struct.unpack("<I",uc.mem_read(0x14005044,4))[0]==0x10000104
        assert count==bootstrap_count+additional and count<128
        assert bytes(uc.mem_read(DTB_BASE,len(tree)))==bytes(tree),(name,"FDT changed")
        assert bytes(uc.mem_read(output+9,16))==bytes([0xa5])*16
        assert pointer>=DEST and pointer+count*72<=DEST+len(fd)
        regions=[]
        for i in range(bootstrap_count,count):
            descriptor=bytes(uc.mem_read(pointer+i*72,72))
            base,size=struct.unpack_from("<2Q",descriptor,32)
            hob,resource,attributes,kind,arm=struct.unpack_from("<IIIII",descriptor,48)
            assert base%4096==size%4096==0 and size>0
            regions.append((base,size,kind,hob,resource,arm))
            if kind==7 and video: assert base+size<=video[0] or base>=video[1],(name,"VRAM exposed to allocator")
            if kind==7:
                for a,b in ((0,DTB_END+0x10000),(0x50000000,0x50002000),(0x60000000,0x70000000),
                            (0xa0000000,0xb0000000),(0x100400000,0x100411000)):
                    assert base+size<=a or base>=b,(name,"reserved memory exposed")
        if expected: assert additional==0
        else:
            assert additional>0
            framebuffer=[r for r in regions if r[2]==0 and r[3]==1]
            assert len(framebuffer)==int(mapped)
            if mapped and display_case:
                assert framebuffer==[(0x90000000,0x3000,0,1,0,0)]
                assert (0x90003000,0x1F000 if name=="LK-named-framebuffer-container" else 0x1D000,0,3,0,1) in regions
            elif mapped: assert framebuffer==[(0x80000000,0x5000,0,1,0,0)]
            for base,size in ((DTB_END,0x10000),(0x50000000,0x2000),
                              (0x60000000,0x10000000),(0x100400000,0x11000)):
                assert (base,size,0,3,0,1) in regions,(name,"fixed no-map type wrong")
            if name=="no-map-inside-fixed-CMA":
                assert (0xa0000000,0x1000000,4,1,0,1) in regions
                assert (0xa1000000,0x2000,0,3,0,1) in regions
                assert (0xa1002000,0xeffe000,4,1,0,1) in regions
            else: assert (0xa0000000,0x10000000,4,1,0,1) in regions
            if video and not mapped:
                assert (video[0],video[1]-video[0],0,3,0,1) in regions
        # Execute the real SEC HOB constructor, allocator, sort, MemoryPeim
        # and ArmMmuLib page-table builder. Substitute only CPU hardware leaves.
        resources=[];allocations=[];translations=[];page_table_runs=[]
        if expected==0:
            def invoke(function,*values):
                for register,value in zip((UC_ARM64_REG_X0,UC_ARM64_REG_X1,
                                           UC_ARM64_REG_X2,UC_ARM64_REG_X3),values):
                    uc.reg_write(register,value)
                uc.reg_write(UC_ARM64_REG_X30,sentinel)
                uc.reg_write(UC_ARM64_REG_SP,stack+0x100000-16)
                uc.emu_start(DEST+symbols[function],sentinel,timeout=10000000,count=20000000)
                assert uc.reg_read(UC_ARM64_REG_PC)==sentinel,(name,function,"did not return")
                return uc.reg_read(UC_ARM64_REG_X0)
            assert invoke("HobConstructor",0x44400000,0x3880000,0x44400000,0x47c80000)==0x44400000
            assert invoke("PrePeiSetHobList",0x44400000)==0
            mmu={"EL":1}
            constants={"ArmGetPhysicalAddressBits":36,"ArmHas52BitTgran4":0,
                       "ArmMmuEnabled":0,"ArmReadHcr":0}
            setters={"ArmSetTCR":"TCR","ArmSetMAIR":"MAIR","ArmSetTTBR0":"TTBR0"}
            hardware=("ArmDisableAlignmentCheck","ArmEnableStackAlignmentCheck",
                      "ArmEnableInstructionCache","ArmEnableDataCache","ArmEnableMmu",
                      "ArmUpdateTranslationTableEntry","InvalidateDataCacheRange")
            def capture(engine,address,size,function):
                if function=="CpuDeadLoop":
                    raise AssertionError((name,"MemoryPeim/MMU rejected map"))
                if function=="ArmConfigureMmu":
                    table=engine.reg_read(UC_ARM64_REG_X0)
                    mmu["input_table"]=table
                    assert not translations
                    for i in range(128):
                        base,virtual,length,arm=struct.unpack("<3QI4x",engine.mem_read(table+i*32,32))
                        if length==0: break
                        assert base==virtual
                        translations.append((base,length,arm))
                    else: raise AssertionError("MMU table missing terminator")
                    return  # Execute the actual page-table builder in the FD.
                result=0
                if function in constants: result=constants[function]
                elif function=="ArmReadCurrentEL": result=mmu["EL"]*4
                elif function=="ArmGetTCR": result=mmu["TCR"]
                elif function in setters:
                    key=setters[function]
                    assert key not in mmu, (name,"MMU register written twice",key)
                    mmu[key]=engine.reg_read(UC_ARM64_REG_X0)
                elif function=="ArmEnableMmu": mmu["enable_requested"]=True
                else: assert function in hardware
                engine.reg_write(UC_ARM64_REG_X0,result)
                engine.reg_write(UC_ARM64_REG_PC,engine.reg_read(UC_ARM64_REG_X30))
            hooked=("ArmConfigureMmu","CpuDeadLoop","ArmReadCurrentEL","ArmGetTCR",
                    *constants,*setters,*hardware)
            handles=[uc.hook_add(UC_HOOK_CODE,capture,user_data=n,
                     begin=DEST+symbols[n],end=DEST+symbols[n]) for n in hooked]
            assert invoke("MemoryPeim",0x44400000,0x3880000)==0
            page_table_runs.append(verify_page_tables(uc,mmu,translations,1))
            # Build a second table for non-VHE EL2 from the same production
            # region list, using the real allocator again without rebuilding HOBs.
            original=list(translations);table=mmu["input_table"]
            translations.clear();mmu.clear();mmu["EL"]=2
            assert invoke("ArmConfigureMmu",table,0,0)==0
            assert translations==original
            page_table_runs.append(verify_page_tables(uc,mmu,translations,2))
            for h in handles: uc.hook_del(h)
            pointer=0x44400000
            for i in range(512):
                kind,length,_=struct.unpack("<HHI",uc.mem_read(pointer,8))
                if kind==0xffff:
                    assert length==8;break
                assert length>=8 and length%8==0 and pointer+length<=0x47c80000
                data=bytes(uc.mem_read(pointer,length))
                if kind==3:
                    assert length==48
                    resource,attributes,base,size=struct.unpack_from("<IIQQ",data,24)
                    resources.append((resource,base,size))
                if kind==2:
                    assert length>=48
                    allocations.append(struct.unpack_from("<QQI",data,24))
                pointer+=length
            else: raise AssertionError("HOB list missing terminator")
            for base,size in ((BASE,0x800000),(HANDOFF,0x1000),(DTB_BASE,DTB_END-DTB_BASE)):
                assert (base,size,0) in allocations,(name,"bootstrap data missing from UEFI allocation HOBs")
            for base,length,kind,hob,resource,arm in regions:
                assert resources.count((resource,base,length))==1,(name,"missing resource HOB")
                assert allocations.count((base,length,kind))==1,(name,"missing typed allocation HOB")
                if hob==3:
                    assert kind==0
                    for a,n,_ in translations:
                        assert a+n<=base or a>=base+length,(name,"no-map region CPU mapped")
                else: assert translations.count((base,length,arm))==1,(name,"missing CPU mapping")
            assert bytes(uc.mem_read(DTB_BASE,len(tree)))==bytes(tree)
        lifecycle={"cached_lookup_reused":False,"post_DXE_fresh_library_preserved_records":False,
                   "failed_layout_retried":False}
        def call_platform_map(limit):
            uc.reg_write(UC_ARM64_REG_X0,output+0x80)
            uc.reg_write(UC_ARM64_REG_X1,output+0x88)
            uc.reg_write(UC_ARM64_REG_X30,sentinel)
            uc.reg_write(UC_ARM64_REG_SP,stack+0x100000-16)
            uc.emu_start(DEST+symbols["GetMemoryMap"],sentinel,timeout=30000000,count=limit)
            assert uc.reg_read(UC_ARM64_REG_PC)==sentinel,(name,"repeated platform map did not return")
            return struct.unpack("<Q",uc.mem_read(output+0x80,8))[0],uc.mem_read(output+0x88,1)[0]
        if expected==0:
            before_page=bytes(uc.mem_read(HANDOFF,4096))
            before_map=bytes(uc.mem_read(layout_pointer,count*72))
            before_io=(len(display_reads),len(display_writes))
            assert call_platform_map(1000)==(layout_pointer,count)
            assert before_page==bytes(uc.mem_read(HANDOFF,4096))
            assert before_map==bytes(uc.mem_read(layout_pointer,count*72))
            assert before_io==(len(display_reads),len(display_writes))
            lifecycle["cached_lookup_reused"]=True
            if name=="LK-display-fallback":
                # A separately linked DXE copy begins with an empty cache.
                uc.mem_write(DEST+symbols["mMapCached"],b"\0")
                uc.mem_write(HANDOFF+0x38,struct.pack("<Q",4))
                uc.mem_write(HANDOFF+0x60,struct.pack("<QIIQQ",0x0000000147503754,8,1,0,(64<<32)|32))
                before_page=bytes(uc.mem_read(HANDOFF,4096))
                assert call_platform_map(100000000)==(layout_pointer,count)
                assert before_page==bytes(uc.mem_read(HANDOFF,4096))
                assert len(display_reads)>before_io[0] and len(display_writes)==before_io[1]
                lifecycle["post_DXE_fresh_library_preserved_records"]=True
        elif name=="original-stock-no-runtime-modem-data":
            repaired=fixture()
            uc.mem_write(DTB_BASE,bytes(repaired))
            uc.mem_write(HANDOFF+0x10,struct.pack("<Q",len(repaired)))
            retry_pointer,retry_count=call_platform_map(100000000)
            assert retry_pointer==layout_pointer and retry_count>bootstrap_count
            assert struct.unpack("<Q",uc.mem_read(HANDOFF+0x60,8))[0]==0
            lifecycle["failed_layout_retried"]=True
        results.append({"case":name,"status":hex(status),"additional_descriptors":additional,
                        "GOP_reserved_UC_mapping":mapped and not display_case,
                        "LK_display_UC_mapping":mapped and display_case,
                        "modeled_display_mmio":{"all_five_lk_video_tags":True,
                            "stock_allowlist_nodes":9,
                            "checked_guard_reads":len(required_reads) if not display_rejected else 0,
                            "ovl_snapshots":3 if not display_rejected else 0,
                            "dsi_snapshots":3 if not display_rejected else 0,
                            "pitch_write":"0x14005044=0x10000104" if not display_rejected else None,
                            "display_reads":len(display_reads),"display_writes":len(display_writes),
                            "ccci_collision_rejected_before_mmio":display_rejected} if display_case else None,
                        "original_FDT_preserved":True,
                        "MemoryPeim_executed":expected==0,"resource_HOBs":len(resources),
                        "typed_allocation_HOBs":len(allocations),"MMU_table_entries":len(translations),
                        "actual_page_table_runs":page_table_runs,
                        "platform_map_lifecycle":lifecycle})
    return {"scope":"Actual SEC GetMemoryMap including the LK display fallback with modeled MMIO, linked libfdt/GOP parser, MemoryPeim, HOB allocator/sort and ArmMmuLib page-table builder in finished FD, ARM64 Unicorn; real page-table memory verified for modeled EL1 and non-VHE EL2 with 36-bit PA, CPU feature/register/cache/enable leaves substituted, no hardware MMU enable, DXE or tablet execution",
            "descriptor_ABI":"72-byte EFI_MEMORY_REGION_DESCRIPTOR: UINT32 ResourceAttribute at 56, MemoryType at 60, ArmAttributes at 64; previous decoder treated attributes as UINT64 and is superseded",
            "sec_TE_sha256":hashlib.sha256(te[0]).hexdigest(),"map_sha256":hashlib.sha256(mapping.read_bytes()).hexdigest(),
            "source_sha256":hashlib.sha256((package/"Library/MemoryMapLib/MemoryMapLib.c").read_bytes()).hexdigest(),
            "display_source_sha256":hashlib.sha256((package.parents[2]/"Silicon/MediaTek/MediaTekPkg/Library/FramebufferCoreLib/MtkDisplayCore.c").read_bytes()).hexdigest(),
            "page_table_source_sha256":{name:hashlib.sha256((package.parents[2]/"Mu_Basecore"/name).read_bytes()).hexdigest()
                for name in ("UefiCpuPkg/Library/ArmMmuLib/AArch64/ArmMmuLibCore.c",
                             "MdePkg/Include/AArch64/AArch64Mmu.h","MdePkg/Include/Library/ArmLib.h")},
            "modeled_hardware_leaves":["ArmReadCurrentEL","ArmGetPhysicalAddressBits","ArmHas52BitTgran4",
                "ArmMmuEnabled","ArmReadHcr","ArmGetTCR","ArmSetTCR","ArmSetMAIR","ArmSetTTBR0",
                "ArmDisableAlignmentCheck","ArmEnableStackAlignmentCheck","ArmEnableInstructionCache",
                "ArmEnableDataCache","ArmEnableMmu","ArmUpdateTranslationTableEntry","InvalidateDataCacheRange"],
            "exception_capture":sec_exception_cases(fd,build),
            "full_stock_LK_validation":full_stock_lk_validation_case(fd,build,stock),
            "cases":results}



def continuous_boot_cases(image, fd, build, stock, ram_trial=False):
    """Run SEC failures plus actual DXE/BDS/Shell against explicit CPU and bus fixtures."""
    import sys,time
    def trace(*args,**kwargs):
        kwargs.setdefault("file",sys.stderr)
        print(*args,**kwargs)
    package = Path(__file__).resolve().parent
    repo = package.parents[2]
    touch_header = repo / "Silicon/MediaTek/MediaTekPkg/Include/Protocol/HimaxTouch.h"
    touch_revision_match = re.search(
        r"^\s*#define\s+HIMAX_TOUCH_CONTROL_REVISION\s+([0-9]+)U?\s*$",
        touch_header.read_text(), re.M)
    assert touch_revision_match, "Himax touch-control revision is missing"
    touch_control_revision = int(touch_revision_match.group(1))
    secmap = build/"AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map"
    dxemap = build/"AARCH64/MdeModulePkg/Core/Dxe/DxeMain/DEBUG/DxeCore.map"

    def symbol_map(path):
        return {n:int(a,16) for a,n in re.findall(
            r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$",
            path.read_text(),re.M)}
    sec, dxe_symbols = symbol_map(secmap), symbol_map(dxemap)
    # Recover the expected DXE PE from this FD, independently of the ARM decoder.
    guided = fd.index(uuid.UUID("8642f9e8-2005-4199-89f2-0552c6810876").bytes_le)+24
    guided_size = int.from_bytes(fd[guided:guided+3],"little")
    compressed = guided+struct.unpack_from("<H",fd,guided+20)[0]
    unpacked = lzma.decompress(fd[compressed:guided+guided_size])
    fv, = [v for kind,v in sections(unpacked) if kind == 0x17]
    offset = (struct.unpack_from("<H",fv,48)[0]+7)&~7
    embedded = None
    while offset+24 <= len(fv) and fv[offset:offset+24] != b"\xff"*24:
        size = int.from_bytes(fv[offset+20:offset+23],"little")
        assert 24 <= size <= len(fv)-offset
        if fv[offset:offset+16] == uuid.UUID("d6a2cb7f-6a18-4e2f-b43b-9920a733700a").bytes_le:
            embedded, = [v for kind,v in sections(fv[offset+24:offset+size]) if kind == 0x10]
        offset = (offset+size+7)&~7
    assert embedded is not None
    assert embedded == (build/"AARCH64/DxeCore.efi").read_bytes()

    def relocated_pe(base, source=embedded):
        embedded = source
        """Independently build the expected loaded PE, including DIR64 fixups."""
        peoff = struct.unpack_from("<I",embedded,0x3c)[0]
        assert embedded[peoff:peoff+4] == b"PE\0\0"
        machine,count = struct.unpack_from("<HH",embedded,peoff+4)
        optional = peoff+24
        assert machine == 0xaa64 and struct.unpack_from("<H",embedded,optional)[0] == 0x20b
        size,headers = struct.unpack_from("<II",embedded,optional+56)
        expected = bytearray(size)
        expected[:headers] = embedded[:headers]
        section = optional+struct.unpack_from("<H",embedded,peoff+20)[0]
        for i in range(count):
            _,rva,rawsize,raw = struct.unpack_from("<IIII",embedded,section+i*40+8)
            expected[rva:rva+rawsize] = embedded[raw:raw+rawsize]
        oldbase = struct.unpack_from("<Q",expected,optional+24)[0]
        rva,n = struct.unpack_from("<II",expected,optional+112+5*8)
        end = rva+n; relocations = 0
        while rva < end:
            page,block = struct.unpack_from("<II",expected,rva)
            assert 8 <= block <= end-rva and block%2 == 0
            for off in range(rva+8,rva+block,2):
                item = struct.unpack_from("<H",expected,off)[0]
                kind,at = item>>12,page+(item&0xfff)
                if kind == 0:
                    continue
                assert kind == 10 and at+8 <= size
                value = struct.unpack_from("<Q",expected,at)[0]
                struct.pack_into("<Q",expected,at,(value+base-oldbase)&0xffffffffffffffff)
                relocations += 1
            rva += block
        struct.pack_into("<Q",expected,optional+24,base)
        return expected,relocations

    def initialized_pe_ranges(source):
        peoff=struct.unpack_from("<I",source,0x3c)[0]
        count=struct.unpack_from("<H",source,peoff+6)[0]
        optional=peoff+24
        size,headers=struct.unpack_from("<II",source,optional+56)
        section=optional+struct.unpack_from("<H",source,peoff+20)[0]
        ranges=[(0,headers)]
        for index in range(count):
            virtual_size,rva,raw_size=struct.unpack_from("<III",source,section+index*40+8)
            end=rva+max(virtual_size,raw_size)
            assert rva<=end<=size
            ranges.append((rva,end))
        return ranges

    error = 1 << 63
    variants = [
        ("EL1-normal",1,None,"core"),("EL2-normal",2,None,"core"),
        ("malformed-LZMA-properties",1,"lzma","core"),
        ("malformed-embedded-DXE-PE",1,"pe","core"),
        ("DXE-image-page-allocation-failure",1,"allocate","core"),
        ("DXE-image-read-failure",1,"load","core"),
        ("DXE-relocation-failure",1,"relocate","core"),
        ("DXE-file-info-failure",1,"file-info","core"),
        ("EL1-stock-FDT-to-Shell",1,None,"shell"),
        ("EL2-stock-FDT-to-Shell",2,None,"shell"),
        ("EL1-GOP-USB-keyboard-Shell-command",1,None,"console"),
        ("EL2-GOP-USB-keyboard-Shell-command",2,None,"console"),
        ("EL1-FAT-removable-EFI-and-OS-handoff",1,None,"media"),
        ("EL2-FAT-removable-EFI-and-OS-handoff",2,None,"media"),
    ]
    if ram_trial:
        # The production trial policy starts UiApp instead of the recovery Shell.
        # Keep the SEC failure cases and execute the actual BDS LoadImage/StartImage.
        variants = variants[:8] + [
            ("EL1-RAM-trial-setup-entry",1,None,"setup"),
            ("EL2-RAM-trial-setup-entry",2,None,"setup")]
    results = []
    for name,el,fault,mode in variants:
        tree = runtime_fdt_fixture(simple=True,ccci_base=0x4f000100,console=True,
                                   dsi=(mode=="media" and el==2)) if mode in ("console","media","setup") else stock
        uc = Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        uc.mem_map(BASE,0x10000000)
        if mode in ("console","media","setup"):
            uc.mem_write(0x4f000100,runtime_ccci_tags())
        uc.mem_map(0x10007000,0x1000)
        uc.mem_map(0x3fff0000,0x1000)
        uc.mem_write(0x3fff0000,b"\xc0\x03\x5f\xd6")
        uc.mem_map(0x0c000000,0x400000)
        uc.mem_write(0x0c000fe8,struct.pack("<I",0x30))
        uc.mem_write(0x0c000004,struct.pack("<I",15))
        uc.mem_write(0x0c040008,struct.pack("<Q",1<<4))
        uc.mem_map(0x10026000,0x1000)
        uc.mem_write(0x10026000,struct.pack("<I",1))
        pmic = {0x28:0xa}
        pwrap = {"fsm":0,"data":0,"write_data":0,"reads":0,"writes":0}
        def pwrap_read(cpu,access,at,sz,value,data):
            if at == 0x100268a8:
                cpu.mem_write(at,struct.pack("<I",pwrap["fsm"]<<1))
            elif at == 0x10026894:
                cpu.mem_write(at,struct.pack("<I",pwrap["data"]))
        def pwrap_write(cpu,access,at,sz,value,data):
            if at == 0x10026884:
                pwrap["write_data"] = value
            elif at == 0x100268a4:
                pwrap["fsm"] = 0
            elif at == 0x10026880:
                reg = value & 0xffff
                if value & (1<<29):
                    pmic[reg] = pwrap["write_data"] & 0xffff
                    pwrap["fsm"] = 0
                    pwrap["writes"] += 1
                else:
                    pwrap["data"] = pmic.get(reg,0)
                    pwrap["fsm"] = 6
                    pwrap["reads"] += 1
        uc.hook_add(UC_HOOK_MEM_READ,pwrap_read,begin=0x10026000,end=0x10026fff)
        uc.hook_add(UC_HOOK_MEM_WRITE,pwrap_write,begin=0x10026000,end=0x10026fff)
        mmio_fixture = []
        mmap = (repo/"Platforms/Teclast/t70proPkg/Library/MemoryMapLib/MemoryMapLib.c").read_text()
        for region,a,n in re.findall(r'\{"([^"]+)",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*AddDev,\s*MMAP_IO',mmap):
            at,ln = int(a,16),int(n,16)
            if (0x0c000000 <= at < 0x10000000) or at in (0x10007000,0x10026000):
                continue
            uc.mem_map(at,ln)
            mmio_fixture.append((region,at,ln,"zero reset/unresponsive fixture"))
        uc.cpr_write(3,3,14,0,0,26000000)
        uc.mem_write(BASE,image)
        uc.mem_write(DTB_BASE,tree)
        if fault == "lzma":
            uc.mem_write(BASE+0x1000+compressed,b"\xff")
        uc.reg_write(UC_ARM64_REG_PSTATE,0x3c0|(el<<2)|1)
        uc.cpr_write(3,0 if el == 1 else 4,1,0,0,0)
        uc.reg_write(UC_ARM64_REG_X0,DTB_BASE)
        state = {"EL":el,"enable_requested":False,"counter":0,"timer_ctl":0,"timer_cval":0,"irq_active":False,"timer_IRQs":0}
        irq_frames = []
        def irq_return(cpu,pc,length,data):
            assert len(irq_frames)==1 and state["irq_active"]
            frame,return_pc = irq_frames.pop()
            cpu.context_restore(frame)
            cpu.reg_write(UC_ARM64_REG_PC,return_pc)
            state["irq_active"] = False
        uc.hook_add(UC_HOOK_BLOCK,irq_return,begin=0x3fff0000,end=0x3fff0000)
        calls, milestones, dxe_steps, translations, loaded, rejected = {},[],[],[],{},{}
        outputs, watchdog, injections = [],[],[]
        terminal = {}
        drivers, driver_hooks, driver_history = {}, {}, []
        guid_names = {uuid.UUID(line.split()[0]):line.split()[1] for line in (build/"FV/Guid.xref").read_text().splitlines() if len(line.split()) == 2}
        driver_maps = {v.stem:v for v in (build/"AARCH64").glob("**/DEBUG/*.map")}
        constants = {"ArmGetPhysicalAddressBits":36,"ArmHas52BitTgran4":0,"ArmReadHcr":0,"ArmReadIdAA64Pfr0":1<<24}
        setters = {"ArmSetTCR":"TCR","ArmSetMAIR":"MAIR","ArmSetTTBR0":"TTBR0",
                   "ArmWriteVBar":"VBAR","ArmWriteCpacr":"CPACR",
                   "ArmGicV3SetControlSystemRegisterEnable":"ICC_SRE",
                   "ArmGicV3SetControlRegister":"ICC_CTLR",
                   "ArmGicV3SetBinaryPointer":"ICC_BPR",
                   "ArmGicV3SetPriorityMask":"ICC_PMR",
                   "ArmWriteCntpCtl":"timer_ctl","ArmWriteCntpCval":"timer_cval"}
        nops = ("ArmDisableAlignmentCheck","ArmEnableStackAlignmentCheck",
                "ArmEnableInstructionCache","ArmEnableDataCache",
                "ArmUpdateTranslationTableEntry","InvalidateDataCacheRange",
                "WriteBackDataCacheRange","InvalidateInstructionCacheRange",
                "ArmDisableDataCache","ArmInvalidateInstructionCache","ArmEnableVFP",
                "ArmGicV3EnableInterruptInterface","ArmGicV3DisableInterruptInterface","ArmGicV3EndOfInterrupt")
        leaves = (*constants,*setters,*nops,"ArmReadCurrentEL","ArmMmuEnabled",
                  "ArmGetTCR","ArmGetTTBR0BaseAddress","ArmEnableMmu","CpuDeadLoop",
                  "ArmGicV3GetControlSystemRegisterEnable","ArmGicV3GetControlRegister",
                  "ArmReadCntPct","ArmReadCntvCt","ArmReadCntpCtl","ArmReadCntpCval",
                  "ArmWriteCntpTval","CpuPause","CpuSleep","ArmCallWFI","ArmGicV3AcknowledgeInterrupt")

        if mode == "media":
            # RAM-only USB-I/O device; real USB storage class, DiskIo, FAT and BDS execute.
            shell_pe=(build/"AARCH64/Shell.efi").read_bytes()
            disk_data=bytearray(4096*512)
            disk_data[:11]=b"\xeb\x3c\x90T70TEST "
            struct.pack_into("<HBHBHHBHHHII",disk_data,11,512,1,1,2,512,4096,0xf8,12,18,2,0,0)
            disk_data[36:62]=bytes.fromhex("80 00 29 12 34 56 78")+b"T70VMTEST  "+b"FAT12   "
            disk_data[510:512]=b"\x55\xaa"
            assert len(disk_data)==4096*512
            fat=bytearray(12*512)
            def fat12(cluster,value):
                off=cluster*3//2
                old=int.from_bytes(fat[off:off+2],"little")
                old=(old&0xf) | (value<<4) if cluster&1 else (old&0xf000)|value
                fat[off:off+2]=old.to_bytes(2,"little")
            fat12(0,0xff8);fat12(1,0xfff);fat12(2,0xfff);fat12(3,0xfff)
            clusters=(len(shell_pe)+511)//512
            for i in range(clusters): fat12(4+i,0xfff if i==clusters-1 else 5+i)
            for offset in (512,13*512):disk_data[offset:offset+len(fat)]=fat
            data_start=57*512
            def dirent(name,cluster,size=0,directory=False):
                value=bytearray(32);value[:11]=name
                value[11]=0x10 if directory else 0x20
                struct.pack_into("<H",value,26,cluster);struct.pack_into("<I",value,28,size)
                return value
            disk_data[25*512:25*512+32]=dirent(b"EFI        ",2,directory=True)
            for cluster,parent,child_name,child,size,isdir in (
                (2,0,b"BOOT       ",3,0,True),
                (3,2,b"BOOTAA64EFI",4,len(shell_pe),False)):
                off=data_start+(cluster-2)*512
                disk_data[off:off+32]=dirent(b".          ",cluster,directory=True)
                disk_data[off+32:off+64]=dirent(b"..         ",parent,directory=True)
                disk_data[off+64:off+96]=dirent(child_name,child,size,directory=isdir)
            disk_data[data_start+2*512:data_start+2*512+len(shell_pe)]=shell_pe
            disk_base=0x3ffe0000
            uc.mem_map(disk_base,0x10000)
            for at in (0x3fff0300,):
                uc.mem_write(at,b"\xc0\x03\x5f\xd6")
            disk={"read_calls":0,"read_bytes":0,"write_calls":0,"reset_calls":0,
                  "block_size":512,"sectors":4096,"read_only":True,
                  "fixture_sha256":hashlib.sha256(disk_data).hexdigest(),
                  "boot_file_sha256":hashlib.sha256(shell_pe).hexdigest()}
            storage_guid=uuid.UUID("2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75").bytes_le
            storage_dp_guid=uuid.UUID("09576e91-6d3f-11d2-8e39-00a0c969723b").bytes_le
            disk_dp=bytes.fromhex("01 04 14 00")+uuid.UUID("cfec7c90-13e3-4f07-b419-aae7879470f7").bytes_le
            disk_dp+=bytes.fromhex("03 05 06 00 01 00 7f ff 04 00")
            uc.mem_write(disk_base+0x1100,storage_guid);uc.mem_write(disk_base+0x1120,storage_dp_guid)
            uc.mem_write(disk_base+0x1300,disk_dp)
            disk.update(boundary="USB-I/O storage device; actual UsbMassStorage/BOT/SCSI/BlockIo/Partition/FAT/BDS execute",
                        commands={},control_requests=[],bulk_calls=0,cbw_count=0,csw_count=0)
            device=bytes.fromhex("12 01 00 02 00 00 00 40 34 12 79 56 00 01 00 00 00 01")
            config=bytes.fromhex("09 02 20 00 01 01 00 80 32")
            interface=bytes.fromhex("09 04 00 00 02 08 06 50 00")
            endpoints=(bytes.fromhex("07 05 81 02 00 02 00"),bytes.fromhex("07 05 02 02 00 02 00"))
            transport={"payload":b"","position":0,"csw":None}
            def storage_command(cbw):
                signature,tag,n,flags,lun,cmdlen=struct.unpack_from("<IIIBBB",cbw)
                assert signature==0x43425355 and lun==0 and 1<=cmdlen<=16
                assert transport["csw"] is None,("unfinished BOT command",transport)
                cmd=cbw[15:15+cmdlen];op=cmd[0]
                disk["cbw_count"]+=1
                disk["commands"][str(op)]=disk["commands"].get(str(op),0)+1
                payload=b"";result=0
                if op==0x12:
                    payload=bytes.fromhex("00 80 05 02 1f 00 00 00")+b"T70SIM  "+b"UEFI USB DISK   "+b"0001"
                elif op==0x03:
                    payload=bytes.fromhex("70 00 00 00 00 00 00 0a")+bytes(10)
                elif op==0x25:
                    payload=struct.pack(">II",len(disk_data)//512-1,512)
                elif op==0x1a:
                    payload=bytes.fromhex("03 00 80 00")
                elif op==0x5a:
                    payload=bytes.fromhex("00 06 00 80 00 00 00 00")
                elif op==0x00:
                    assert n==0
                elif op in (0x28,0x88):
                    lba,count=(struct.unpack_from(">I",cmd,2)[0],struct.unpack_from(">H",cmd,7)[0]) if op==0x28 else (struct.unpack_from(">Q",cmd,2)[0],struct.unpack_from(">I",cmd,10)[0])
                    assert n==count*512 and lba*512+n<=len(disk_data),(op,lba,count,n)
                    assert flags==0x80
                    payload=bytes(disk_data[lba*512:lba*512+n])
                    disk["read_calls"]+=1;disk["read_bytes"]+=n
                elif op in (0x2a,0x8a):
                    disk["write_calls"]+=1
                    raise AssertionError("unexpected storage write attempt")
                else:
                    raise AssertionError(("unmodeled SCSI command",cmd.hex(),n))
                payload=payload[:n]
                assert n==0 or flags==0x80
                transport.update(payload=payload,position=0,csw=struct.pack("<IIIB",0x53425355,tag,n-len(payload),result))
            def storage_io(cpu,pc,length,method):
                args=[cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(7)]
                assert args[0]==disk_base+0x1200,(method,args)
                status=0
                if method=="Control":
                    typ,request,value,index,n=struct.unpack("<BBHHH",bytes(cpu.mem_read(args[1],8)))
                    disk["control_requests"].append([typ,request,value,index,n])
                    cpu.mem_write(args[6],bytes(4))
                    if (typ,request)==(0xa1,0xfe):
                        assert args[5]==1;cpu.mem_write(args[4],b"\0")
                    elif (typ,request)==(0x80,8):
                        assert args[5]==1;cpu.mem_write(args[4],b"\1")
                    elif (typ,request)==(0x21,0xff):
                        disk["reset_calls"]+=1;transport.update(payload=b"",position=0,csw=None)
                    elif (typ,request) in ((0,9),(2,1)):
                        pass
                    else:raise AssertionError(("unmodeled storage control",disk["control_requests"][-1]))
                elif method=="Bulk":
                    disk["bulk_calls"]+=1
                    endpoint,buffer,length_pointer,result_pointer=args[1],args[2],args[3],args[5]
                    requested=struct.unpack("<Q",bytes(cpu.mem_read(length_pointer,8)))[0]
                    cpu.mem_write(result_pointer,bytes(4))
                    if endpoint==2:
                        assert requested==31,("CBW length",requested)
                        storage_command(bytes(cpu.mem_read(buffer,31)))
                    else:
                        assert endpoint==0x81 and transport["csw"] is not None
                        left=len(transport["payload"])-transport["position"]
                        if left:
                            returned=min(requested,left);pos=transport["position"]
                            cpu.mem_write(buffer,transport["payload"][pos:pos+returned])
                            transport["position"]+=returned
                        else:
                            assert requested==13,("CSW request length",requested)
                            returned=13;cpu.mem_write(buffer,transport["csw"])
                            transport["csw"]=None;disk["csw_count"]+=1
                        cpu.mem_write(length_pointer,struct.pack("<Q",returned))
                elif method in ("DeviceDescriptor","ConfigDescriptor","InterfaceDescriptor"):
                    cpu.mem_write(args[1],{"DeviceDescriptor":device,"ConfigDescriptor":config,"InterfaceDescriptor":interface}[method])
                elif method=="EndpointDescriptor":
                    assert args[1]<2;cpu.mem_write(args[2],endpoints[args[1]])
                elif method=="PortReset":
                    disk["reset_calls"]+=1
                else:status=error|3
                cpu.reg_write(UC_ARM64_REG_X0,status)
                cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_X30))
            storage_methods=("Control","Bulk","AsyncInterrupt","SyncInterrupt","Iso","AsyncIso",
                             "DeviceDescriptor","ConfigDescriptor","InterfaceDescriptor","EndpointDescriptor",
                             "StringDescriptor","Languages","PortReset")
            for index,method in enumerate(storage_methods):
                at=disk_base+index*16
                uc.mem_write(at,b"\xc0\x03\x5f\xd6")
                uc.mem_write(disk_base+0x1200+index*8,struct.pack("<Q",at))
                uc.hook_add(UC_HOOK_BLOCK,storage_io,user_data=method,begin=at,end=at)
            def disk_install_return(cpu,pc,length,data):
                assert cpu.reg_read(UC_ARM64_REG_X0)==0
                disk["handle"]=struct.unpack("<Q",cpu.mem_read(disk_base+0x1000,8))[0]
                assert disk["handle"]
                context,resume=usb_saved.pop()
                cpu.context_restore(context);cpu.reg_write(UC_ARM64_REG_PC,resume)
            uc.hook_add(UC_HOOK_BLOCK,disk_install_return,begin=0x3fff0300,end=0x3fff0300)

        # The provider is a USB-I/O boundary fixture, not an MUSB bus model.
        usb_base = 0x3ffd0000
        uc.mem_map(usb_base,0x10000)
        for at in (0x3fff0100,0x3fff0210,0x3fff0230,0x3fff0240,usb_base+0x200):
            uc.mem_write(at,b"\xc0\x03\x5f\xd6")
        usb = {"installed":False,"callback":0,"context":0,"reports_sent":0,
               "requests":[],"method_calls":{},"start_status":None,"decoded":[],"commands":[]}
        usb_frames = [bytes((0,0,key,0,0,0,0,0)) for code in (0x19,0x08,0x15,0x28)
                      for key in (code,0)]
        usb_saved = []
        device_desc = bytes.fromhex("12 01 00 02 00 00 00 40 34 12 78 56 00 01 00 00 00 01")
        config_desc = bytes.fromhex("09 02 19 00 01 01 00 80 32")
        interface_desc = bytes.fromhex("09 04 00 00 01 03 01 01 00")
        endpoint_desc = bytes.fromhex("07 05 81 03 08 00 0a")
        usb_guid = uuid.UUID("2b2f68d6-0cd2-44cf-8e8b-bba20b1b5b75").bytes_le
        dp_guid = uuid.UUID("09576e91-6d3f-11d2-8e39-00a0c969723b").bytes_le
        dp = bytes.fromhex("01 04 14 00")+uuid.UUID("298d9718-5585-4f39-94cb-f50dd910945f").bytes_le
        dp += bytes.fromhex("03 05 06 00 00 00 7f ff 04 00")
        uc.mem_write(usb_base+0x1100,usb_guid)
        uc.mem_write(usb_base+0x1120,dp_guid)
        uc.mem_write(usb_base+0x1300,dp)
        methods = ("Control","Bulk","AsyncInterrupt","SyncInterrupt","Iso","AsyncIso",
                   "DeviceDescriptor","ConfigDescriptor","InterfaceDescriptor","EndpointDescriptor",
                   "StringDescriptor","Languages","PortReset")
        for index,method in enumerate(methods):
            at = usb_base+index*16
            uc.mem_write(at,b"\xc0\x03\x5f\xd6")
            uc.mem_write(usb_base+0x1200+index*8,struct.pack("<Q",at))
        def usb_io(cpu,pc,length,method):
            args = [cpu.reg_read(UC_ARM64_REG_X0+i) for i in range(7)]
            assert args[0] == usb_base+0x1200,(method,args)
            usb["method_calls"][method] = usb["method_calls"].get(method,0)+1
            status = 0
            if method == "Control":
                req = bytes(cpu.mem_read(args[1],8))
                typ,request,value,index,n = struct.unpack("<BBHHH",req)
                usb["requests"].append([typ,request,value,index,n])
                cpu.mem_write(args[6],bytes(4))
                if (typ,request) == (0x80,8):
                    assert args[5] == 1
                    cpu.mem_write(args[4],b"\x01")
                elif (typ,request) in ((0,9),(0x21,0x0b),(0x21,0x0a),(0x21,9)):
                    pass
                else:
                    raise AssertionError(("unexpected USB control request",usb["requests"][-1]))
            elif method in ("DeviceDescriptor","ConfigDescriptor","InterfaceDescriptor"):
                desc = {"DeviceDescriptor":device_desc,"ConfigDescriptor":config_desc,
                        "InterfaceDescriptor":interface_desc}[method]
                cpu.mem_write(args[1],desc)
            elif method == "EndpointDescriptor":
                assert args[1] == 0
                cpu.mem_write(args[2],endpoint_desc)
            elif method == "AsyncInterrupt":
                assert args[1] == 0x81
                if args[2]:
                    assert args[4] == 8 and args[5] and args[6]
                    usb["callback"],usb["context"] = args[5:7]
                else:
                    usb["callback"] = 0
            elif method == "PortReset":
                pass
            else:
                status = error|3
            cpu.reg_write(UC_ARM64_REG_X0,status)
            cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_X30))
        for index,method in enumerate(methods):
            uc.hook_add(UC_HOOK_BLOCK,usb_io,user_data=method,
                        begin=usb_base+index*16,end=usb_base+index*16)
        def usb_install_return(cpu,pc,length,data):
            assert cpu.reg_read(UC_ARM64_REG_X0) == 0
            usb["handle"] = struct.unpack("<Q",cpu.mem_read(usb_base+0x1000,8))[0]
            assert usb["handle"]
            trace("USB-I/O keyboard fixture installed",hex(usb["handle"]),flush=True)
            args=(0x200,8,usb_base+0x200,0,usb_base+0x1450)
            for reg,val in enumerate(args): cpu.reg_write(UC_ARM64_REG_X0+reg,val)
            cpu.reg_write(UC_ARM64_REG_X30,0x3fff0240)
            cpu.reg_write(UC_ARM64_REG_PC,loaded["base"]+dxe_symbols["CoreCreateEvent"])
        def usb_event_created(cpu,pc,length,data):
            assert cpu.reg_read(UC_ARM64_REG_X0)==0
            usb["event"]=struct.unpack("<Q",cpu.mem_read(usb_base+0x1450,8))[0]
            assert usb["event"]
            if mode != "media":
                context,resume=usb_saved.pop()
                cpu.context_restore(context);cpu.reg_write(UC_ARM64_REG_PC,resume)
                return
            args=(disk_base+0x1000,disk_base+0x1100,disk_base+0x1200,disk_base+0x1120,disk_base+0x1300,0)
            for reg,val in enumerate(args):cpu.reg_write(UC_ARM64_REG_X0+reg,val)
            cpu.reg_write(UC_ARM64_REG_X30,0x3fff0300)
            cpu.reg_write(UC_ARM64_REG_PC,loaded["base"]+dxe_symbols["CoreInstallMultipleProtocolInterfaces"])
        usb_notify_returns=[]
        def usb_event_notify(cpu,pc,length,data):
            assert usb_frames and usb["callback"] and loaded.get("Shell_prompt_reached")
            usb["event_pending"]=False
            report=usb_frames.pop(0)
            cpu.mem_write(usb_base+0x1400,report)
            usb["reports_sent"]+=1
            usb_notify_returns.append(cpu.reg_read(UC_ARM64_REG_X30))
            for reg,val in enumerate((usb_base+0x1400,8,usb["context"],0)):
                cpu.reg_write(UC_ARM64_REG_X0+reg,val)
            cpu.reg_write(UC_ARM64_REG_X30,0x3fff0210)
            cpu.reg_write(UC_ARM64_REG_PC,usb["callback"])
        def usb_callback_return(cpu,pc,length,data):
            assert cpu.reg_read(UC_ARM64_REG_X0)==0
            cpu.reg_write(UC_ARM64_REG_PC,usb_notify_returns.pop())
        def usb_signal_return(cpu,pc,length,data):
            assert cpu.reg_read(UC_ARM64_REG_X0)==0
            context,resume=usb_saved.pop()
            cpu.context_restore(context);cpu.reg_write(UC_ARM64_REG_PC,resume)
        for at,callback in ((0x3fff0100,usb_install_return),(0x3fff0240,usb_event_created),
                            (usb_base+0x200,usb_event_notify),(0x3fff0210,usb_callback_return),
                            (0x3fff0230,usb_signal_return)):
            uc.hook_add(UC_HOOK_BLOCK,callback,begin=at,end=at)

        def hardware(engine,address,size,function):
            calls[function] = calls.get(function,0)+1
            if function == "CpuDeadLoop" and loaded.get("base"):
                table = loaded["base"]+dxe_symbols["mArchProtocols"]
                arch = []
                for i in range(14):
                    ptr = struct.unpack("<Q",engine.mem_read(table+i*40,8))[0]
                    if not ptr:break
                    guid = uuid.UUID(bytes_le=bytes(engine.mem_read(ptr,16)))
                    present = bytes(engine.mem_read(table+i*40+32,1)) != bytes(1)
                    arch.append({"guid":str(guid),"name":guid_names.get(guid),"present":present})
                trace("ARCHITECTURAL_PROTOCOLS",arch,flush=True)
            assert function != "CpuDeadLoop",(name,"CPU dead loop")
            value = engine.reg_read(UC_ARM64_REG_X0)
            if function in ("CpuPause","CpuSleep","ArmCallWFI"):
                if not loaded.get("OS_handoff_probe") and loaded.get("Shell_prompt_count",0)>=2 and not usb_frames:
                    assert usb["reports_sent"]==8 and usb["start_status"]==0,usb
                    assert {"command":"ver","status":0} in usb["commands"],usb
                    from PIL import Image
                    pixels=bytes(engine.mem_read(0x49000000,5632*2240));assert any(pixels)
                    loaded["GOP_pixels_sha256"]=hashlib.sha256(pixels).hexdigest()
                    if mode=="media" and el==2:
                        screenshot=repo.parent/"analysis/edk2-shell-console.png"
                        Image.frombytes("RGB",(1400,2240),pixels,"raw","BGRX",5632,1).save(screenshot)
                        loaded["console_capture"]={"path":str(screenshot),
                            "sha256":hashlib.sha256(screenshot.read_bytes()).hexdigest()}
                    loaded["USB_keyboard_ver_command_executed"]=True
                    trace("USB KEYBOARD EVIDENCE",usb,flush=True)
                    engine.emu_stop()
                    return
                if loaded.get("Shell_prompt_reached") and usb_frames and usb["callback"] and not usb_saved and not usb.get("event_pending") and not state["irq_active"]:
                    usb_saved.append((engine.context_save(),engine.reg_read(UC_ARM64_REG_X30)))
                    usb["event_pending"]=True
                    engine.reg_write(UC_ARM64_REG_X0,usb["event"])
                    engine.reg_write(UC_ARM64_REG_X30,0x3fff0230)
                    engine.reg_write(UC_ARM64_REG_PC,loaded["base"]+dxe_symbols["CoreSignalEvent"])
                    return
                if calls[function] <= 6:
                    trace("CPU WAIT",function,"DAIF",hex(engine.reg_read(UC_ARM64_REG_PSTATE)&0x3c0),"timer_ctl",state["timer_ctl"],"counter",state["counter"],"cval",state["timer_cval"],"IRQs",state["timer_IRQs"],flush=True)
                timer = next(((a,v) for a,v in drivers.items() if v["module"]=="ArmTimerDxe"),None)
                gic = next(((a,v) for a,v in drivers.items() if v["module"]=="ArmGicDxe"),None)
                if timer and gic and not state["irq_active"] and (state["timer_ctl"]&3)==1 and not (engine.reg_read(UC_ARM64_REG_PSTATE)&0x80):
                    assert not irq_frames
                    state["counter"] = max(state["counter"],state["timer_cval"])+1
                    irq_frames.append((engine.context_save(),engine.reg_read(UC_ARM64_REG_X30)))
                    state["irq_active"] = True
                    state["irq_ack_pending"] = True
                    state["timer_IRQs"] += 1
                    engine.reg_write(UC_ARM64_REG_PSTATE,engine.reg_read(UC_ARM64_REG_PSTATE)|0x80)
                    engine.reg_write(UC_ARM64_REG_X0,1)
                    engine.reg_write(UC_ARM64_REG_X1,0)
                    engine.reg_write(UC_ARM64_REG_X30,0x3fff0000)
                    engine.reg_write(UC_ARM64_REG_PC,gic[0]+gic[1]["map_symbols"]["GicV3IrqInterruptHandler"])
                else:
                    engine.reg_write(UC_ARM64_REG_PC,engine.reg_read(UC_ARM64_REG_X30))
                return
            if function == "ArmGicV3AcknowledgeInterrupt":
                value = 30 if state.pop("irq_ack_pending",False) else 1023
            elif function == "ArmReadCntpCtl":
                value = (state["timer_ctl"]&3) | (4 if state["counter"]>=state["timer_cval"] else 0)
            elif function == "ArmReadCntpCval":
                value = state["timer_cval"]
            elif function == "ArmWriteCntpTval":
                val = value&0xffffffff
                val = val if val<0x80000000 else val-0x100000000
                state["timer_cval"] = state["counter"]+val
                value = 0
            elif function in constants:
                value = constants[function]
            elif function == "ArmReadCurrentEL":
                value = el*4
            elif function == "ArmMmuEnabled":
                value = int(state["enable_requested"])
            elif function == "ArmGetTCR":
                value = state["TCR"]
            elif function == "ArmGetTTBR0BaseAddress":
                value = state["TTBR0"] & 0x0000fffffffff000
            elif function == "ArmGicV3GetControlSystemRegisterEnable":
                value = state.get("ICC_SRE",0)
            elif function == "ArmGicV3GetControlRegister":
                value = state.get("ICC_CTLR",0)
            elif function in ("ArmReadCntPct","ArmReadCntvCt"):
                state["counter"] = state.get("counter",0)+2600
                value = state["counter"]
            elif function == "ArmReadVBar":
                value = state["VBAR"]
            elif function in setters:
                key = setters[function]
                state[key] = value
                # Exception setup in the real DXE core reads VBAR directly.
                if key == "VBAR":
                    engine.cpr_write(3,0 if el == 1 else 4,12,0,0,value)
                value = 0
            elif function == "ArmEnableMmu":
                state["enable_requested"] = True
                value = 0
            else:
                assert function in nops
            engine.reg_write(UC_ARM64_REG_X0,value)
            engine.reg_write(UC_ARM64_REG_PC,engine.reg_read(UC_ARM64_REG_X30))

        def reject_return(engine,address,size,function):
            status = engine.reg_read(UC_ARM64_REG_X0)
            if fault and status&error:
                rejected.update(function=function,status=status,pc=address)

        def stage(engine,address,size,function):
            milestones.append(function)
            if function == "SecEntry":
                park = engine.reg_read(UC_ARM64_REG_X30)
                assert bytes(engine.mem_read(park,4)) == b"\x00\x00\x00\x14"
                def parked(cpu,pc,length,data):
                    assert fault and rejected,(name,"unexpected SEC return")
                    rejected["SEC_error_park_reached"] = True
                    rejected["SEC_error_park_PC"] = pc
                    cpu.emu_stop()
                terminal["park"] = park
                engine.hook_add(UC_HOOK_BLOCK,parked,begin=park,end=park)
            if function in ("DecompressFirstFv","LoadDxeCoreFromFv"):
                resume = engine.reg_read(UC_ARM64_REG_X30)
                engine.hook_add(UC_HOOK_BLOCK,reject_return,user_data=function,
                                begin=resume,end=resume)
            if function == "LoadPeCoffImage":
                outputs[:] = [engine.reg_read(r) for r in
                              (UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3)]
                if fault == "pe":
                    address = engine.reg_read(UC_ARM64_REG_X0)
                    assert bytes(engine.mem_read(address,2)) == b"MZ"
                    engine.mem_write(address,b"NO")
                    injections.append("in-memory DXE MZ signature corruption")

        def table_input(engine,address,size,data):
            pointer = engine.reg_read(UC_ARM64_REG_X0)
            for i in range(128):
                base,virtual,length,arm = struct.unpack("<3QI4x",engine.mem_read(pointer+i*32,32))
                if length == 0:
                    break
                assert base == virtual
                translations.append((base,length,arm))
            else:
                raise AssertionError("unterminated MMU input")
            # Execute the actual page-table builder; do not substitute it.

        for function in leaves:
            if function not in sec:
                continue
            uc.hook_add(UC_HOOK_BLOCK,hardware,user_data=function,
                        begin=DEST+sec[function],end=DEST+sec[function])
        for function in ("SecEntry","PlatformInitialize","HobConstructor","MemoryPeim",
                         "ProcessLibraryConstructorList","DecompressFirstFv",
                         "LoadDxeCoreFromFv","LoadDxeCoreFromFfsFile","LoadPeCoffImage"):
            uc.hook_add(UC_HOOK_BLOCK,stage,user_data=function,
                        begin=DEST+sec[function],end=DEST+sec[function])
        uc.hook_add(UC_HOOK_BLOCK,table_input,
                    begin=DEST+sec["ArmConfigureMmu"],end=DEST+sec["ArmConfigureMmu"])

        def injected_error(engine,address,size,function):
            if function in ("FfsGetFileInfo","InternalAllocatePages") and not outputs:
                return
            if function == "InternalAllocatePages":
                assert engine.reg_read(UC_ARM64_REG_X1) == 4, "AllocateCodePages first allocates BootServicesData, then updates its HOB to Code"
            status = 0 if function == "InternalAllocatePages" else error|(14 if function == "FfsGetFileInfo" else 1)
            injections.append(function)
            engine.reg_write(UC_ARM64_REG_X0,status)
            engine.reg_write(UC_ARM64_REG_PC,engine.reg_read(UC_ARM64_REG_X30))
        injected = {"allocate":"InternalAllocatePages","load":"PeCoffLoaderLoadImage",
                    "relocate":"PeCoffLoaderRelocateImage","file-info":"FfsGetFileInfo"}
        if fault in injected:
            function = injected[fault]
            assert function in sec,function
            uc.hook_add(UC_HOOK_BLOCK,injected_error,user_data=function,
                        begin=DEST+sec[function],end=DEST+sec[function])

        def watchdog_write(engine,access,address,size,value,data):
            assert (address,size,value) == (0x10007000,4,0x22000000)
            watchdog.append((address,size,value))
            assert len(watchdog) <= (2 if ram_trial else 1), "Unexpected watchdog programming"
        uc.hook_add(UC_HOOK_MEM_WRITE,watchdog_write,begin=0x10007000,end=0x10007fff)

        def dxe_stage(engine,address,size,function):
            dxe_steps.append(function)
            if function == "CoreDispatcher":
                loaded["dispatcher_reached"] = True
                if mode == "core":
                    engine.emu_stop()

        first_code_hook = None
        def enter_dxe(engine,address,size,data):
            assert not fault,(name,"failed load reached executable DXE")
            base = address-dxe_symbols["_ModuleEntryPoint"]
            loaded.update(base=base,entry=address,hob=engine.reg_read(UC_ARM64_REG_X0),
                          sp=engine.reg_read(UC_ARM64_REG_SP))
            expected,relocations = relocated_pe(base)
            assert bytes(engine.mem_read(base,len(expected))) == expected,"actual SEC PE load/relocation differs from FD"
            loaded.update(whole_loaded_PE_matched=True,relocations=relocations)
            # Real ARM LZMA output and real DXE loader HOBs must agree with the FD.
            cur = loaded["hob"]; volumes = []; modules = []
            for i in range(1024):
                kind,length,_ = struct.unpack("<HHI",engine.mem_read(cur,8))
                if kind == 0xffff:
                    break
                assert length >= 8 and length%8 == 0
                if kind == 5:
                    volumes.append(struct.unpack("<QQ",engine.mem_read(cur+8,16)))
                if kind == 2 and length == 72:
                    row = bytes(engine.mem_read(cur,72))
                    modules.append((struct.unpack_from("<Q",row,24)[0],
                                    struct.unpack_from("<Q",row,32)[0],
                                    struct.unpack_from("<Q",row,64)[0]))
                cur += length
            else:
                raise AssertionError("missing SEC HOB end")
            assert (base,len(expected),address) in modules
            decoded, = [(a,n) for a,n in volumes if n == len(fv)]
            assert bytes(engine.mem_read(decoded[0],decoded[1])) == fv
            loaded["actual_ARM64_LZMA_FV_matched"] = True
            loaded["SEC_page_tables"] = verify_page_tables(engine,state,translations,el)
            engine.hook_del(first_code_hook)
            for function in (*leaves,"ArmReadVBar"):
                if function in dxe_symbols:
                    engine.hook_add(UC_HOOK_BLOCK,hardware,user_data=function,
                                    begin=base+dxe_symbols[function],end=base+dxe_symbols[function])

            def image_unload(cpu,pc,length,data):
                private=cpu.reg_read(UC_ARM64_REG_X0)
                imagebase=struct.unpack("<Q",cpu.mem_read(private+104,8))[0]
                for hook in driver_hooks.pop(imagebase,[]):
                    cpu.hook_del(hook)
                drivers.pop(imagebase,None)
            engine.hook_add(UC_HOOK_BLOCK,image_unload,begin=base+dxe_symbols["CoreUnloadAndCloseImage"],end=base+dxe_symbols["CoreUnloadAndCloseImage"])

            def info_enter(cpu,pc,length,data):
                resume = cpu.reg_read(UC_ARM64_REG_X30)
                h = None
                def info_return(e,a,n,u):
                    e.hook_del(h)
                    ptr = e.reg_read(UC_ARM64_REG_X0)
                    if not ptr:
                        return
                    assert struct.unpack("<Q",e.mem_read(ptr,8))[0] == 0x6972646c
                    entry = struct.unpack("<Q",e.mem_read(ptr+32,8))[0]
                    imagebase,imagesize = struct.unpack("<QQ",e.mem_read(ptr+104,16))
                    if not entry or not imagebase or imagebase == base or imagebase in drivers:
                        return
                    content = bytes(e.mem_read(imagebase,imagesize))
                    path = struct.unpack("<Q",e.mem_read(ptr+72,8))[0]
                    image_guid = None
                    file_paths=[]
                    for node in range(256):
                        typ,sub,ln = struct.unpack("<BBH",e.mem_read(path,4))
                        assert 4 <= ln <= 4096
                        if typ == 4 and sub == 6:
                            assert ln == 20
                            image_guid = uuid.UUID(bytes_le=bytes(e.mem_read(path+4,16)))
                        if typ==4 and sub==4:
                            file_paths.append(bytes(e.mem_read(path+4,ln-4)).decode("utf-16-le").rstrip("\0"))
                        if typ == 0x7f:
                            break
                        path += ln
                    if image_guid is None and file_paths:
                        assert "".join(file_paths).lower()==r"\efi\boot\bootaa64.efi",file_paths
                        image_guid=uuid.UUID("7c04a583-9e3e-4f1c-ad65-e05268d0b4d1")
                        expected_shell,fixups=relocated_pe(imagebase,shell_pe)
                        assert len(content)==len(expected_shell)
                        ranges=initialized_pe_ranges(shell_pe)
                        bad=next(((start,end) for start,end in ranges
                            if content[start:end]!=expected_shell[start:end]),None)
                        assert bad is None,("external FAT boot PE/relocation mismatch",bad)
                        loaded["external_FAT_EFI_boot"]={"path":"".join(file_paths),
                            "loaded_PE_headers_and_sections_matched":True,
                            "ignored_uninitialized_image_gaps":True,"relocations":fixups,
                            "source_sha256":hashlib.sha256(shell_pe).hexdigest()}
                    assert image_guid in guid_names,(image_guid,hex(path))
                    module = guid_names[image_guid]
                    assert content[:2] == b"MZ" and module in driver_maps,module
                    symbols = symbol_map(driver_maps[module])
                    assert entry == imagebase+symbols["_ModuleEntryPoint"],(module,hex(entry),hex(imagebase))
                    drivers[imagebase] = {"module":module,"entry":entry,"bytes":imagesize,"guid":str(image_guid),"map_symbols":symbols}
                    driver_hooks[imagebase]=[]
                    def add_driver_hook(callback,**kwargs):
                        hook=e.hook_add(UC_HOOK_BLOCK,callback,**kwargs)
                        driver_hooks[imagebase].append(hook)
                        return hook
                    if module == "BdsDxe":
                        def policy_stage(cpu,pc,ln,function):
                            trace("BDS POLICY",function,flush=True)
                        for function in ("PlatformBootManagerBeforeConsole","PlatformBootManagerAfterConsole","DeviceBootManagerBeforeConsole","DeviceBootManagerAfterConsole","DeviceBootManagerUnableToBoot","PlatformBootManagerUnableToBoot"):
                            if function in symbols:
                                add_driver_hook(policy_stage,user_data=function,begin=imagebase+symbols[function],end=imagebase+symbols[function])
                    if module == "BdsDxe":
                        def bds_entry(cpu,pc,ln,data):
                            loaded["BdsEntry_reached"] = True
                            trace("ACTUAL BdsEntry reached",hex(pc),flush=True)
                            if mode in ("console","media") and not usb["installed"]:
                                usb["installed"] = True
                                usb_saved.append((cpu.context_save(),pc))
                                for reg,val in enumerate((usb_base+0x1000,usb_base+0x1100,usb_base+0x1200,usb_base+0x1120,usb_base+0x1300,0)):
                                    cpu.reg_write(UC_ARM64_REG_X0+reg,val)
                                cpu.reg_write(UC_ARM64_REG_X30,0x3fff0100)
                                cpu.reg_write(UC_ARM64_REG_PC,base+dxe_symbols["CoreInstallMultipleProtocolInterfaces"])
                        add_driver_hook(bds_entry,begin=imagebase+symbols["BdsEntry"],end=imagebase+symbols["BdsEntry"])
                    if module in ("T70I2cPlatformDxe","T70GpioPlatformDxe","MusbHandoffDxe","T70DsiPlatformDxe"):
                        def handoff_notify(cpu,pc,ln,label):
                            if loaded.get("OS_handoff_probe"):
                                loaded.setdefault("EBS_notifications",[]).append(label)
                        for notification in ("ExportToAcpi","EbsPowerNotify","EbsPublishNotify","Exit"):
                            if notification in symbols:
                                add_driver_hook(handoff_notify,user_data=module+"."+notification,
                                    begin=imagebase+symbols[notification],end=imagebase+symbols[notification])
                    if module == "Shell":
                        loaded["Shell_image_handle"]=struct.unpack("<Q",e.mem_read(ptr+8,8))[0]
                    if module == "UsbKbDxe":
                        def keyboard_start(cpu,pc,ln,data):
                            trace("USB KEYBOARD START ENTER",flush=True)
                            resume=cpu.reg_read(UC_ARM64_REG_X30); hook=None
                            cpu.ctl_remove_cache(resume,resume+4)
                            def returned(e,a,n,u):
                                e.hook_del(hook)
                                usb["start_status"]=e.reg_read(UC_ARM64_REG_X0)
                                trace("ACTUAL USB KEYBOARD start",hex(usb["start_status"]),flush=True)
                            hook=cpu.hook_add(UC_HOOK_BLOCK,returned,begin=resume,end=resume)
                        add_driver_hook(keyboard_start,
                            begin=imagebase+symbols["USBKeyboardDriverBindingStart"],end=imagebase+symbols["USBKeyboardDriverBindingStart"])
                        def key_read(cpu,pc,ln,data):
                            output=cpu.reg_read(UC_ARM64_REG_X1)
                            resume=cpu.reg_read(UC_ARM64_REG_X30); hook=None
                            def returned(e,a,n,u):
                                e.hook_del(hook)
                                if e.reg_read(UC_ARM64_REG_X0) == 0:
                                    key=struct.unpack("<HH",e.mem_read(output,4))
                                    usb["decoded"].append(list(key))
                            hook=cpu.hook_add(UC_HOOK_BLOCK,returned,begin=resume,end=resume)
                        add_driver_hook(key_read,
                            begin=imagebase+symbols["USBKeyboardReadKeyStrokeWorker"],end=imagebase+symbols["USBKeyboardReadKeyStrokeWorker"])
                    if module == "Shell":
                        def shell_prompt(cpu,pc,ln,data):
                            loaded["Shell_prompt_reached"] = True
                            loaded["Shell_prompt_count"] = loaded.get("Shell_prompt_count",0)+1
                            trace("ACTUAL Shell prompt reached",hex(pc),"count",loaded["Shell_prompt_count"],"timer_IRQs",state["timer_IRQs"],flush=True)
                            if mode == "shell":
                                cpu.emu_stop()
                                return
                            assert mode in ("console","media") and usb["start_status"]==0 and usb["callback"],usb
                        add_driver_hook(shell_prompt,begin=imagebase+symbols["DoShellPrompt"],end=imagebase+symbols["DoShellPrompt"])
                        def command_enter(cpu,pc,ln,data):
                            ptr=cpu.reg_read(UC_ARM64_REG_X0)
                            words=bytearray()
                            for i in range(1024):
                                word=bytes(cpu.mem_read(ptr+i*2,2))
                                if word == bytes(2): break
                                words+=word
                            command=words.decode("utf-16-le")
                            trace("ACTUAL SHELL COMMAND",repr(command),flush=True)
                            resume=cpu.reg_read(UC_ARM64_REG_X30);hook=None
                            cpu.ctl_remove_cache(resume,resume+4)
                            def returned(e,a,n,u):
                                e.hook_del(hook)
                                usb["commands"].append({"command":command,"status":e.reg_read(UC_ARM64_REG_X0)})
                            hook=cpu.hook_add(UC_HOOK_BLOCK,returned,begin=resume,end=resume)
                        add_driver_hook(command_enter,begin=imagebase+symbols["RunShellCommand"],end=imagebase+symbols["RunShellCommand"])
                    def driver_entry(cpu,pc,ln,module_name):
                        driver_history.append(module_name)
                        if mode == "setup" and module_name == "UiApp":
                            record=struct.unpack("<4Q",cpu.mem_read(HANDOFF+0xFE0,32))
                            assert record == (0x0000000157443754,0xFFFFFFFEA8BBC8AB,0x22000000,2), record
                            loaded["RAM_trial_setup_entry_reached"]=True
                            loaded["trial_watchdog_record"]=list(record)
                            # Stop at the actual application entry; menu drawing/input
                            # and physical keyboard behavior are not claimed here.
                            cpu.emu_stop()
                    add_driver_hook(driver_entry,user_data=module,begin=entry,end=entry)
                    for function in (*leaves,"ArmReadVBar"):
                        if function in symbols:
                            add_driver_hook(hardware,user_data=function,
                                       begin=imagebase+symbols[function],end=imagebase+symbols[function])
                h=cpu.hook_add(UC_HOOK_BLOCK,info_return,begin=resume,end=resume)
            def observe(cpu,pc,length,function):
                resume = cpu.reg_read(UC_ARM64_REG_X30)
                pass
                h = None
                def returned(e,a,n,u):
                    e.hook_del(h)
                    status = e.reg_read(UC_ARM64_REG_X0)
                    if function != "CoreAllEfiServicesAvailable" and status:
                        trace("DRIVER API RETURN",function,hex(status),flush=True)
                h=cpu.hook_add(UC_HOOK_BLOCK,returned,begin=resume,end=resume)
            for function in ("CoreLoadImage","CoreStartImage","CoreAllEfiServicesAvailable"):
                if function in dxe_symbols:
                    engine.hook_add(UC_HOOK_BLOCK,observe,user_data=function,begin=base+dxe_symbols[function],end=base+dxe_symbols[function])
            engine.hook_add(UC_HOOK_BLOCK,info_enter,begin=base+dxe_symbols["CoreLoadedImageInfo"],end=base+dxe_symbols["CoreLoadedImageInfo"])

            for function in ("DxeMain","CoreInitializeMemoryServices","CoreInitializeHandleServices",
                             "CoreInitializeImageServices","CoreInitializeGcdServices",
                             "CoreInitializeDispatcher","CoreDispatcher"):
                assert function in dxe_symbols,function
                engine.hook_add(UC_HOOK_BLOCK,dxe_stage,user_data=function,
                                begin=base+dxe_symbols[function],end=base+dxe_symbols[function])
        first_code_hook = uc.hook_add(UC_HOOK_BLOCK,enter_dxe,begin=0x44400000,end=0x50000000-1)

        def invalid(e,access,at,size,value,data):
            trace("INVALID ACCESS",access,hex(at),size,"PC",hex(e.reg_read(UC_ARM64_REG_PC)), "X0-X4",[hex(e.reg_read(r)) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4)],flush=True)
            return False
        uc.hook_add(UC_HOOK_MEM_INVALID,invalid)
        started = time.monotonic()
        try:
            for run in range(15):
                uc.emu_start(BASE if run==0 else uc.reg_read(UC_ARM64_REG_PC),0,timeout=40000000,count=500000000)
                if (mode=="core" and (loaded.get("dispatcher_reached") or rejected.get("SEC_error_park_reached"))) or (mode=="shell" and loaded.get("Shell_prompt_reached")) or loaded.get("USB_keyboard_ver_command_executed") or loaded.get("RAM_trial_setup_entry_reached"):
                    break
                pc = uc.reg_read(UC_ARM64_REG_PC)
                current = next((item["module"] for at,item in drivers.items() if at<=pc<at+item["bytes"]),"SEC/core")
                trace("DXE progress",run+1,"seconds",round(time.monotonic()-started,2),"PC",hex(pc),"module",current,flush=True)
        except BaseException as exc:
            pc = uc.reg_read(UC_ARM64_REG_PC)
            for at,item in drivers.items():
                if at <= pc < at+item["bytes"]:
                    near = sorted([(n,v) for n,v in item["map_symbols"].items() if v<=pc-at],key=lambda row:row[1])[-5:]
                    trace("STOP MODULE",item["module"],"offset",hex(pc-at),"near",[(n,hex(v)) for n,v in near],flush=True)
            trace("DISPATCH PROTOTYPE STOP",type(exc).__name__,str(exc),"PC",hex(uc.reg_read(UC_ARM64_REG_PC)),"modules",[v["module"] for v in drivers.values()],"calls",calls,flush=True)
            raise
        trace("DISPATCH RETURN PC",hex(uc.reg_read(UC_ARM64_REG_PC)),"modules",[v["module"] for v in drivers.values()],flush=True)
        assert watchdog == [(0x10007000,4,0x22000000)] * (2 if ram_trial else 1)
        assert bytes(uc.mem_read(DTB_BASE,len(tree))) == tree
        if fault:
            assert rejected.get("SEC_error_park_reached") and not loaded,(name,"failure did not reach SEC error park",hex(uc.reg_read(UC_ARM64_REG_PC)))
            if fault in ("pe","allocate","load","relocate"):
                assert outputs and all(bytes(uc.mem_read(a,8)) == bytes(8) for a in outputs)
            if fault == "allocate":
                assert rejected["status"] == error|9
            if fault in ("load","relocate"):
                assert rejected["status"] == error|1
            if fault == "file-info":
                assert rejected["status"] == error|14
        else:
            expected_stage = {"core":"dispatcher_reached","shell":"Shell_prompt_reached",
                              "console":"USB_keyboard_ver_command_executed",
                              "media":"USB_keyboard_ver_command_executed",
                              "setup":"RAM_trial_setup_entry_reached"}[mode]
            assert loaded.get(expected_stage),(name,"boot stage not reached",expected_stage,hex(uc.reg_read(UC_ARM64_REG_PC)))
            if mode != "core":
                arch = []
                for index in range(14):
                    row=loaded["base"]+dxe_symbols["mArchProtocols"]+index*40
                    ptr=struct.unpack("<Q",uc.mem_read(row,8))[0]
                    if not ptr: break
                    guid=uuid.UUID(bytes_le=bytes(uc.mem_read(ptr,16)))
                    present=bytes(uc.mem_read(row+32,1)) != bytes(1)
                    assert present,("required architecture absent",str(guid))
                    arch.append(str(guid))
                assert len(arch)==13
                loaded["all_13_architecture_protocols_present"]=True
                loaded["architecture_protocols"]=arch
                loaded["actual_timer_interrupts_handled"]=state["timer_IRQs"]
                if mode != "setup":
                    assert state["timer_IRQs"]>=8 if mode in ("console","media") else state["timer_IRQs"]>=301
                loaded["executed_driver_entries"]=driver_history
            if mode in ("console","media"):
                assert usb["decoded"]==[[0,118],[0,101],[0,114],[0,13]],usb
                loaded["USB_IO_fixture"]=usb
                if mode=="media":
                    assert loaded.get("external_FAT_EFI_boot"),("external removable EFI was not loaded",disk)
                    assert disk["read_calls"] and disk["write_calls"]==0,disk
                    assert disk["commands"].get("40") and disk["cbw_count"]==disk["csw_count"],disk
                    loaded["USB_storage_FAT_fixture"]=disk
                    loaded["Block_IO_FAT_fixture"]=disk
                loaded["GOP_framebuffer"]={"width":1400,"height":2240,"stride":5632,
                    "format":"x8r8g8b8","nonblank":True,
                    "pixels_sha256":loaded["GOP_pixels_sha256"],
                    "capture":loaded.get("console_capture")}
            st = struct.unpack("<Q",uc.mem_read(loaded["base"]+dxe_symbols["gDxeCoreST"],8))[0]
            header = bytes(uc.mem_read(st,120))
            assert struct.unpack_from("<Q",header)[0] == 0x5453595320494249
            bs = struct.unpack_from("<Q",header,96)[0]
            assert struct.unpack("<Q",uc.mem_read(bs,8))[0] == 0x56524553544f4f42
            loaded["system_table"] = hex(st)
            loaded["boot_services_table"] = hex(bs)
            loaded["system_and_boot_service_headers_verified"] = True
            if mode!="core":
                count,configs=struct.unpack("<QQ",uc.mem_read(st+104,16))
                assert 0<count<128
                acpi_root=None
                for index in range(count):
                    row=bytes(uc.mem_read(configs+index*24,24))
                    if uuid.UUID(bytes_le=row[:16])==uuid.UUID("8868e871-e4f1-11d3-bc22-0080c73c8881"):
                        acpi_root=struct.unpack_from("<Q",row,16)[0]
                assert acpi_root,"ACPI 2.0 configuration table absent"
                root=bytes(uc.mem_read(acpi_root,36))
                assert root[:8]==b"RSD PTR " and root[15]>=2
                assert sum(root[:20])%256==0 and sum(root)%256==0
                assert struct.unpack_from("<I",root,20)[0]==36
                xsdt=struct.unpack_from("<Q",root,24)[0]
                def acpi_table(at,signature=None):
                    header=bytes(uc.mem_read(at,36))
                    length=struct.unpack_from("<I",header,4)[0]
                    assert 36<=length<=0x100000
                    body=bytes(uc.mem_read(at,length))
                    assert sum(body)%256==0,("ACPI checksum",body[:4])
                    if signature:assert body[:4]==signature
                    return body
                xdata=acpi_table(xsdt,b"XSDT")
                assert (len(xdata)-36)%8==0
                entries={}
                for off in range(36,len(xdata),8):
                    at=struct.unpack_from("<Q",xdata,off)[0];body=acpi_table(at)
                    sig=body[:4].decode("ascii")
                    table_id=body[16:24].decode("ascii").rstrip("\x00 ")
                    key="SSDT/"+table_id if sig=="SSDT" else sig
                    assert key not in entries and (sig!="SSDT" or table_id),(sig,table_id)
                    entries[key]={"address":hex(at),"bytes":len(body),"sha256":hashlib.sha256(body).hexdigest()}
                    if sig=="FACP":
                        dsdt=struct.unpack_from("<Q",body,140)[0] or struct.unpack_from("<I",body,40)[0]
                        dsdt_body=acpi_table(dsdt,b"DSDT")
                assert {"FACP","APIC","GTDT","PPTT"}<=entries.keys(),entries
                entries["DSDT"]={"address":hex(dsdt),"bytes":len(dsdt_body),"sha256":hashlib.sha256(dsdt_body).hexdigest()}
                loaded["published_ACPI"]={"RSDP_address":hex(acpi_root),"XSDT_address":hex(xsdt),
                    "checksums_verified":True,"tables":entries}
        if mode in ("console","media"):
            # Exercise the actual OS-facing handoff APIs at TPL_APPLICATION.
            # No Windows/Linux kernel or device state is substituted by this probe.
            loaded["OS_handoff_probe"]=True
            sentinel=0x3fff0500
            uc.mem_write(sentinel,b"\xc0\x03\x5f\xd6")
            returns=[]
            def probe_return(cpu,pc,length,data):
                returns.append(cpu.reg_read(UC_ARM64_REG_X0));cpu.emu_stop()
            probe_hook=uc.hook_add(UC_HOOK_BLOCK,probe_return,begin=sentinel,end=sentinel)
            def firmware_call(address,args,label):
                before=len(returns)
                for i,value in enumerate(args):uc.reg_write(UC_ARM64_REG_X0+i,value)
                uc.reg_write(UC_ARM64_REG_X30,sentinel)
                uc.emu_start(address,0,timeout=40000000,count=500000000)
                assert len(returns)==before+1,("firmware API did not return",label,hex(uc.reg_read(UC_ARM64_REG_PC)))
                return returns[-1]
            def core_call(function,args):
                return firmware_call(loaded["base"]+dxe_symbols[function],args,function)
            core_call("CoreRestoreTpl",(4,))
            assert core_call("CoreAllocatePool",(2,0x20000,usb_base+0x1600))==0
            pool=struct.unpack("<Q",uc.mem_read(usb_base+0x1600,8))[0]
            assert pool
            def get_final_map():
                uc.mem_write(pool,struct.pack("<Q",0x1f000))
                assert core_call("CoreGetMemoryMap",(pool,pool+0x1000,pool+8,pool+16,pool+24))==0
                mapsize,key,descsize=struct.unpack("<QQQ",uc.mem_read(pool,24))
                version=struct.unpack("<I",uc.mem_read(pool+24,4))[0]
                assert version==1 and descsize>=40 and mapsize%descsize==0
                rawmap=bytes(uc.mem_read(pool+0x1000,mapsize))
                ranges=[]
                for off in range(0,mapsize,descsize):
                    typ,phys,virtual,pages,attributes=struct.unpack_from("<I4xQQQQ",rawmap,off)
                    assert typ<=15 and phys%4096==0 and pages
                    ranges.append((phys,phys+pages*4096,typ))
                ranges.sort()
                assert all(a[1]<=b[0] for a,b in zip(ranges,ranges[1:])),ranges
                return key,{"bytes":mapsize,"descriptor_bytes":descsize,"descriptor_version":version,
                    "descriptor_count":len(ranges),"nonoverlapping_ranges":True,
                    "sha256":hashlib.sha256(rawmap).hexdigest()}
            if mode=="media" and el==2:
                # Invoke the actual published firmware DSI methods, not a
                # native recompile. Electrical responses remain explicit RAM
                # fixtures. The display pipeline/panel are not emulated.
                scratch=pool+0x100
                uc.mem_write(scratch,uuid.UUID("b14fe6d9-36ad-4dfc-a7b2-c580534e9122").bytes_le)
                assert core_call("CoreLocateProtocol",(scratch,0,scratch+0x20))==0
                host=struct.unpack("<Q",uc.mem_read(scratch+0x20,8))[0]
                revision,padding,*methods=struct.unpack("<II7Q",uc.mem_read(host,64))
                assert revision==1 and padding==0 and all(methods)
                def dsi_call(index,*args):
                    return firmware_call(methods[index],(host,)+args,"DSI method "+str(index))
                assert dsi_call(6,scratch+0x30)==0
                assert struct.unpack("<I",uc.mem_read(scratch+0x30,4))[0]==0
                dsi_entry=next((v for v in drivers.values() if v["module"]=="T70DsiPlatformDxe"),None)
                assert dsi_entry and "T70DsiPlatformDxe" in driver_history
                # The touch control publishes passively even when no verified
                # physical touch session is present. Its methods must fail
                # without touching I2C or claiming reset ownership in that case.
                touch_at=scratch+0x80
                uc.mem_write(touch_at,uuid.UUID("9dbb1d38-d3c7-49f8-a428-7349b215806c").bytes_le)
                assert core_call("CoreLocateProtocol",(touch_at,0,touch_at+0x20))==0
                touch_control=struct.unpack("<Q",uc.mem_read(touch_at+0x20,8))[0]
                touch_rev,pad,quiesce,resume=struct.unpack("<II2Q",uc.mem_read(touch_control,24))
                assert touch_rev==touch_control_revision and pad==0 and quiesce and resume
                uc.mem_write(touch_at+0x30,bytes(8))
                assert firmware_call(quiesce,(touch_control,loaded["Shell_image_handle"],touch_at+0x30),"touch quiesce without active session")==error|6
                assert struct.unpack("<Q",uc.mem_read(touch_at+0x30,8))[0]==0
                assert firmware_call(resume,(touch_control,1),"touch resume without active session")==error|6
                loaded["touch_control_protocol_probe"]={"status":"passed","implementation":"actual PE from final FV",
                    "revision":touch_rev,"quiesce_and_resume_without_live_session_rejected":True,
                    "active_touch_lifecycle_executed":False,"tablet_executed":False}
                # Original get_params executes against its own separate LK
                # instruction/MMIO fixture. No C output/expected trace is
                # supplied to the shipping DXE driver.
                lk=runpy.run_path(str(package/"Tests/verify-display-binary.py"))
                proof=lk["original_lk_dsi_mipi_op7"]((repo.parent/"analysis/lk/lk.bin").read_bytes(),0)
                params=bytes.fromhex(proof["supported_input"]["get_params_raw_hex"])
                params_at=pool+0x400;profile_at=pool+0xC00
                uc.mem_write(params_at,params)
                # Profile wire is 56 bytes: pointer, five U32 fields, initial
                # six U32 words, and trailing alignment.
                uc.mem_write(profile_at,struct.pack("<Q5I6I4x",params_at,len(params),0x10,0,2240,1400,*([0]*6)))
                for at,val in ((0x1000616C,0x200000),(0x10006170,0x200000),(0x10006354,0xD)):
                    uc.mem_write(at,struct.pack("<I",val))
                uc.mem_write(0x14000000,bytes(0x1000))
                uc.mem_write(0x14013000,bytes(0x1000))
                uc.mem_write(0x11F60000,bytes(0x1000))
                dsi_writes=[]
                def dsi_write(cpu,access,at,length,value,data):
                    if 0x14013000<=at<0x14014000 or 0x11F60000<=at<0x11F61000 or at in (0x14000108,0x140001A8):
                        assert length==4
                        dsi_writes.append((at,value))
                dsi_hook=uc.hook_add(UC_HOOK_MEM_WRITE,dsi_write)
                assert dsi_call(0,loaded["Shell_image_handle"],scratch+0x40)==0
                token=struct.unpack("<Q",uc.mem_read(scratch+0x40,8))[0];assert token
                assert dsi_call(1,token,profile_at)==0
                assert dsi_call(2,token)==0
                assert struct.unpack("<I",uc.mem_read(0x14013018,4))[0]&0x1F0FF==0x3C
                assert dsi_call(3,token)==0
                assert dsi_call(4,token)==0
                assert dsi_call(2,token)==0
                assert dsi_call(3,token)==0
                assert dsi_call(5,token)==0
                uc.hook_del(dsi_hook)
                loaded["DSI_host_protocol_probe"]={
                    "status":"passed","implementation":"actual PE from final FV",
                    "protocol_revision":revision,"cycles":2,"MMIO_writes":len(dsi_writes),
                    "write_trace_sha256":hashlib.sha256(b"".join(struct.pack("<QI",a,v) for a,v in dsi_writes)).hexdigest(),
                    "operations":["Acquire","Configure","StartVideo","Suspend","Resume","StartVideo","Suspend","Release"],
                    "all_four_data_lanes_enabled":True,
                    "fixtures":["SPM power/status","zeroed DSI/MIPI MMIO","no busy transfer","counter/timer leaves"],
                    "panel_or_display_pipeline_emulated":False,"tablet_executed":False}
            key,initial_map=get_final_map()
            stale=core_call("CoreExitBootServices",(loaded["Shell_image_handle"],key^1))
            assert stale==error|2,("stale memory-map key accepted",hex(stale))
            assert struct.unpack("<Q",uc.mem_read(st+96,8))[0]==bs
            key,final_map=get_final_map()
            boot_service_size=struct.unpack("<I",uc.mem_read(bs+12,4))[0]
            assert 24<=boot_service_size<4096
            success=core_call("CoreExitBootServices",(loaded["Shell_image_handle"],key))
            assert success==0,("valid ExitBootServices failed",hex(success))
            assert bytes(uc.mem_read(st+40,48))==bytes(48),"console services retained after EBS"
            assert struct.unpack("<Q",uc.mem_read(st+96,8))[0]==0,"BootServices retained after EBS"
            assert bytes(uc.mem_read(bs,boot_service_size))==bytes(boot_service_size)
            assert uc.reg_read(UC_ARM64_REG_PSTATE)&0x80,"CPU IRQ not masked at EBS"
            assert state["timer_ctl"]&1==0,"architectural timer remains enabled"
            import zlib
            st_header=bytearray(uc.mem_read(st,120))
            crc=struct.unpack_from("<I",st_header,16)[0]
            struct.pack_into("<I",st_header,16,0)
            assert zlib.crc32(st_header)==crc,"system-table CRC stale after EBS"
            required={"T70I2cPlatformDxe.ExportToAcpi","T70GpioPlatformDxe.ExportToAcpi",
                      "MusbHandoffDxe.EbsPowerNotify","MusbHandoffDxe.EbsPublishNotify","T70DsiPlatformDxe.Exit"}
            observed=loaded.get("EBS_notifications",[])
            assert required<=set(observed),("handoff notification missing",observed)
            assert all(observed.count(n)==1 for n in required),observed
            if mode=="media" and el==2:
                assert dsi_call(6,scratch+0x30)==error|21
                loaded["DSI_host_protocol_probe"]["post_EBS_query_rejected"]=True
            loaded["OS_handoff_API_probe"]={"status":"passed","initial_memory_map":initial_map,
                "stale_map_exit_status":hex(stale),"fresh_memory_map":final_map,
                "fresh_map_exit_status":success,"notifications":observed,
                "boot_and_console_services_cleared":True,"CPU_IRQ_and_timer_disabled":True,
                "system_table_CRC_verified":True,"OS_kernel_entry_executed":False}
            uc.hook_del(probe_hook)
        results.append({"case":name,"status":"passed","elapsed_seconds":time.monotonic()-started,
                        "SEC_milestones":milestones,"DXE_core_milestones":dxe_steps,
                        "loaded":loaded,"error_return":rejected,
                        "fault_injections":injections+(["LZMA property byte"] if fault == "lzma" else []),
                        "CPU_hardware_leaf_calls":calls,
                        "test_input_FDT_preserved":True,"original_FDT_used":mode not in ("console","media","setup"),
                        "FDT_sha256":hashlib.sha256(tree).hexdigest(),
                        "driver_dispatch_executed":mode!="core" and not fault,
                        "fixture_scope":None if mode=="core" else {
                            "GICv3_registers_and_timer_sources":"modeled; actual firmware IRQ handler and event dispatcher execute",
                            "PWRAP":"modeled registers; no pressed buttons",
                            "other_MMIO":mmio_fixture,
                            "framebuffer":"synthetic RAM framebuffer" if mode in ("console","media","setup") else None,
                            "USB":"synthetic USB-I/O boot keyboard; actual class driver/console/Shell" if mode in ("console","media") else None,
                            "Block_IO":"USB-I/O storage device fixture; actual UsbMassStorage/BOT/SCSI/BlockIo/Partition/FAT/PE load" if mode=="media" else None,
                            "physical_MUSB_UFS_SD_panel_or_PMIC_behavior_emulated":False}})
        trace("Continuous ARM64 case passed: "+name,file=sys.stderr,flush=True)
    return {"status":"passed","scope":("Actual shipping RAM-trial BootShim/SEC/LZMA/PE relocation, DXE architecture and driver dispatch, BDS LoadImage/StartImage to UiApp entry with synthetic framebuffer; menu/input and physical devices not executed" if ram_trial else "Actual shipping BootShim/SEC/LZMA/PE relocation, DXE architecture and driver dispatch, BDS recovery Shell; synthetic GOP framebuffer and USB-I/O class input execute ver through the actual UEFI event/console/Shell path, removable FAT EFI loading, published ACPI and stale/fresh-map ExitBootServices"),
            "FD_sha256":hashlib.sha256(fd).hexdigest(),
            "DxeCore_PE_sha256":hashlib.sha256(embedded).hexdigest(),
            "sec_map_sha256":hashlib.sha256(secmap.read_bytes()).hexdigest(),
            "dxe_map_sha256":hashlib.sha256(dxemap.read_bytes()).hexdigest(),
            "PrePi_source_sha256":hashlib.sha256((repo/"Mu_Basecore/EmbeddedPkg/Library/PrePiLib/PrePiLib.c").read_bytes()).hexdigest(),
            "CPU_cache_register_and_MMU_enable_leaves_modeled":True,
            "actual_SEC_LZMA_and_DXE_loader_executed":True,"actual_DXE_core_initialization_executed":True,
            "driver_dispatch_executed":True,"whole_firmware_software_boot_to_shell_executed":not ram_trial,
            "whole_firmware_software_boot_to_trial_setup_entry_executed":ram_trial,
            "trial_setup_menu_or_keyboard_executed":False,
            "whole_firmware_boot_executed":False,
            "physical_GIC_timer_display_USB_or_storage_emulated":False,
            "modeled_boundaries":["CPU/cache/MMU activation","GIC registers and timer sources","PWRAP responses","USB-I/O provider","RAM framebuffer","read-only RAM-backed USB-I/O mass-storage device"],
            "QEMU_used":False,"operating_system_loaded":False,"device_executed":False,
            "allocator_error_limit":"PrePi cannot reclaim failed image pages; buffers remain reserved until this failed boot ends, with no automatic load retry",
            "cases":results}



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--fd", type=Path, required=True)
    parser.add_argument("--boot", type=Path)
    parser.add_argument("--shim-only", action="store_true", help="execute a newly compiled shim with an existing FD fixture; skip boot-container/FV integration checks")
    parser.add_argument("--python-dtb", action="store_true", help="use the existing bounded stock DT parser instead of fdtget")
    parser.add_argument("--ram-trial", action="store_true", help="verify the watchdog-disabled interactive trial shim and its actual BDS setup-entry policy")
    parser.add_argument("--dtb", type=Path, required=True)
    parser.add_argument("--preloader", type=Path, required=True,
                        help="Original ufs-lu0.img; read only")
    args = parser.parse_args()
    image, fd, dtb = (p.read_bytes() for p in (args.image, args.fd, args.dtb))
    if not args.shim_only and args.boot is None:
        parser.error("--boot is required unless --shim-only is selected")
    boot = None
    if not args.shim_only:
        boot = args.boot.read_bytes()
        assert boot[:8] == b"ANDROID!"
        kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from("<4I", boot, 8)
        assert header_size == 1584 and ramdisk_size == 0
        assert struct.unpack_from("<I", boot, 40)[0] == 4
        kernel = boot[4096:4096+kernel_size]
        assert kernel[:4] == b"\x02\x21\x4c\x18"  # Stock legacy LZ4 format
        expanded = subprocess.run(["lz4", "-dc"], input=kernel, capture_output=True, check=True).stdout
        assert expanded == image
        signature_size = struct.unpack_from("<I", boot, 1580)[0]
        signature_offset = (4096+kernel_size+4095) & ~4095
        assert signature_size == 4096
        assert len(boot) == signature_offset+signature_size
        assert boot[signature_offset:] == bytes(signature_size)
    assert image[56:60] == b"ARM\x64"
    assert struct.unpack_from("<3Q", image, 8) == (0, len(image), 0xA)
    assert len(fd) == 0x400000 and image[0x1000:] == fd
    assert fd[40:44] == b"_FVH"
    fv_length = struct.unpack_from("<Q", fd, 32)[0]
    fv_header_length = struct.unpack_from("<H", fd, 48)[0]
    assert fv_length == len(fd) and 56 <= fv_header_length <= len(fd)
    assert sum(struct.unpack_from("<%dH" % (fv_header_length//2), fd)) & 0xFFFF == 0
    assert dtb[:4] == b"\xd0\x0d\xfe\xed" and len(dtb) <= 0x200000
    assert struct.unpack_from(">I", dtb, 4)[0] == len(dtb)
    disassembler = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
    disassembler.skipdata = True
    park = [a for a, size, mnemonic, operands in
            disassembler.disasm_lite(image[:0x1000], BASE) if mnemonic == "wfe"]
    assert len(park) == 1
    park_offset = park[0] - BASE
    # Independently recover reservations from the original GFH and descriptor data.
    raw = args.preloader.read_bytes()
    assert raw[0x1000:0x1004] == b"MMM\x01"
    load_address = struct.unpack_from("<I", raw, 0x101C)[0]
    file_to_ram = load_address-0x1000
    descriptor = struct.unpack_from("<I", raw, 0x2A024)[0]-file_to_ram
    reservations = {}
    for offset in (descriptor, descriptor+40, descriptor+80):
        name_address = struct.unpack_from("<I", raw, offset)[0]
        name = raw[name_address-file_to_ram:].split(b"\0", 1)[0].decode()
        address, length = struct.unpack_from("<QQ", raw, offset+8)
        reservations[name] = (address, length)
    assert set(reservations) == {"mb_kernel", "mb_dtb", "aee_debug_kinfo"}
    assert reservations["aee_debug_kinfo"] == (DTB_END,0x10000)
    package = Path(__file__).resolve().parent
    source = (package/"Library/MemoryMapLib/MemoryMapLib.c").read_text()
    regions = {name: (int(address,16), int(length,16)) for name,address,length in
               re.findall(r'\{"([^"\n]+)",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)', source)}
    kernel = reservations["mb_kernel"]
    cursor = kernel[0]
    kinds = {name:kind for name,kind in re.findall(
        r'\{"([^"\n]+)",\s*0x[0-9A-Fa-f]+,\s*0x[0-9A-Fa-f]+,\s*(AddMem|AddDev)', source)}
    assert set(kinds) == set(regions)
    # Device mappings are independent of the contiguous preloader RAM block.
    ram = sorted((address,length) for name,(address,length) in regions.items()
                 if kinds[name] == "AddMem" and name != "Bootloader DTB")
    ordered = sorted((address,address+length,name) for name,(address,length) in regions.items())
    for i,(begin,end,name) in enumerate(ordered):
        assert begin%4096 == end%4096 == 0 and begin < end
        if i: assert ordered[i-1][1] <= begin, (ordered[i-1],name)
    for address,length in ram:
        assert address == cursor and length > 0
        cursor += length
    assert cursor == sum(kernel)
    assert kernel[0] == BASE and regions["Bootloader DTB"] == reservations["mb_dtb"]
    assert reservations["mb_dtb"] == (DTB_BASE, DTB_END-DTB_BASE)
    assert regions["UEFI FD"] == (DEST, len(fd))
    assert regions["Boot handoff"] == (HANDOFF, 0x1000)
    assert BASE+len(image) <= sum(regions["Kernel payload"])
    dsc = (package/"t70pro.dsc").read_text()
    stack_base = int(re.search(r'PcdCPUCoresStackBase\|(0x[0-9A-Fa-f]+)', dsc)[1],16)
    stack_size = int(re.search(r'PcdCPUCorePrimaryStackSize\|(0x[0-9A-Fa-f]+)', dsc)[1],16)
    assert regions["UEFI Stack"] == (stack_base, stack_size)
    dt_reader = runpy.run_path(str(package/"Linux/platform-modules.py")) if args.python_dtb else None
    dt_nodes = dt_reader["tree"](dtb)[0] if dt_reader else None
    for name,node,start_cell in (
        ("GIC Distributor", "/interrupt-controller", 0),
        ("GIC Redistributors", "/interrupt-controller", 4),
        ("WatchDog Timer", "/soc/watchdog@10007000", 0),
        ("Top Ck Gen", "/soc/topckgen@10000000", 0),
        ("Infra Cfg AO", "/soc/infracfg_ao@10001000", 0),
        ("Pinctrl", "/soc/gpio@10005000", 0),
        ("IOCFG RB", "/soc/pinctrl", 4),
        ("IOCFG BM", "/soc/pinctrl", 8),
        ("IOCFG BR", "/soc/pinctrl", 12),
        ("IOCFG LM", "/soc/pinctrl", 16),
        ("IOCFG BL", "/soc/pinctrl", 20),
        ("IOCFG RT", "/soc/pinctrl", 24),
        ("EINT", "/soc/apirq@1000b000", 0),
        ("AP Mixed", "/soc/apmixed@1000c000", 0),
        ("PMIC Wrapper", "/soc/pwrap@10026000", 0),
        ("APDMA", "/soc/apdma@10217000", 0),
        ("I2C-0", "/soc/i2c@11e00000", 0),
        ("I2C-6", "/soc/i2c@1101a000", 0),
        ("I2C Wrap W", "/syscon@11E02000", 0),
        ("I2C Wrap C", "/syscon@1101B000", 0),
        ("Display MIPI TX", "/soc/mipi_tx_config@11f60000", 0),
        ("MSDC-1", "/soc/mmc@11240000", 0),
        ("MSDC Top-1", "/soc/mmc@11240000", 4),
        ("UFS", "/soc/ufshci@11270000", 0),
        ("USB0", "/soc/usb0@11200000", 0),
        ("USB PHY", "/soc/usb0@11200000", 4)):
        assert kinds[name] == "AddDev"
        row = next(line for line in source.splitlines() if '{"'+name+'"' in line)
        assert "MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE" in row
        if dt_reader:
            cells = dt_reader["cells"](dt_nodes[node]["properties"]["reg"])[start_cell:start_cell+4]
        else:
            values = subprocess.check_output(["fdtget", "-t", "x", str(args.dtb), node, "reg"], text=True)
            cells = [int(value,16) for value in values.split()][start_cell:start_cell+4]
        address, length = (cells[0]<<32)|cells[1], (cells[2]<<32)|cells[3]
        assert address & 0xFFF == 0
        assert regions[name] == (address, (length+0xFFF) & ~0xFFF)
    cases = []

    def execute(name, *, el=1, sctlr=0, load=BASE, pointer=DTB_BASE,
                magic=0xD00DFEED, size=len(dtb), accepted=False):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(BASE, DTB_BASE-BASE)
        uc.mem_map(DTB_BASE, DTB_END-DTB_BASE)
        uc.mem_write(load, image)
        uc.mem_write(HANDOFF+0x60, bytes([0xa5])*32)
        uc.mem_write(HANDOFF+0x800, bytes([0xa5])*72)
        uc.mem_write(HANDOFF+0x80, bytes([0xff])*112)
        uc.mem_write(HANDOFF+0xf0, bytes([0xa5])*16)
        uc.mem_write(HANDOFF+0x100, bytes([0xff])*72)
        uc.mem_write(HANDOFF+0xFE0, bytes([0xff])*32)
        if DTB_BASE <= pointer and pointer+40 <= DTB_END:
            payload = bytearray(dtb)
            struct.pack_into(">II", payload, 0, magic, size)
            # Only the header is needed on deliberately malformed inputs.
            uc.mem_write(pointer, bytes(payload[:min(len(payload), DTB_END-pointer)]))
        uc.reg_write(UC_ARM64_REG_PSTATE, 0x3C0 | (el<<2) | (1 if el else 0))
        if el in (1, 2):
            uc.cpr_write(3, 0 if el == 1 else 4, 1, 0, 0, sctlr)
        uc.reg_write(UC_ARM64_REG_X0, pointer)
        writes = [0]

        def written(engine, access, address, length, value, user_data):
            assert (DEST <= address and address+length <= DEST+len(fd)) or (
                HANDOFF <= address and address+length <= HANDOFF+64) or (
                HANDOFF+0x60 <= address and address+length <= HANDOFF+0x80) or (
                HANDOFF+0x80 <= address and address+length <= HANDOFF+0x100) or (
                HANDOFF+0x100 <= address and address+length <= HANDOFF+0x148) or (
                HANDOFF+0x800 <= address and address+length <= HANDOFF+0x808) or (
                HANDOFF+0xFE0 <= address and address+length <= HANDOFF+0x1000), hex(address)
            writes[0] += length

        uc.hook_add(UC_HOOK_MEM_WRITE, written)
        # Stop at the FD entry even if an invalid-input test is accidentally accepted.
        uc.hook_add(UC_HOOK_CODE, lambda engine, address, size, data: engine.emu_stop(),
                    begin=DEST, end=DEST)
        # The normal terminal is either the FD branch or the rejection WFE.
        terminal = DEST if accepted else load+park_offset
        trapped = False
        try:
            uc.emu_start(load, terminal, timeout=15000000, count=4000000)
        except UcError as error:
            # EL0 cannot read CurrentEL: the CPU traps before any memory write.
            assert el == 0 and error.errno == UC_ERR_EXCEPTION, (name, error)
            trapped = True
        if not trapped:
            assert uc.reg_read(UC_ARM64_REG_PC) == terminal, name
        assert bytes(uc.mem_read(HANDOFF+0x800,72)) == ((bytes(8)+bytes([0xa5])*64) if accepted else bytes([0xa5])*72)
        if accepted:
            assert bytes(uc.mem_read(DEST, len(fd))) == fd
            assert uc.reg_read(UC_ARM64_REG_X0) == pointer
            record = struct.unpack("<8Q", bytes(uc.mem_read(HANDOFF, 64)))
            assert record[:5] == (0x0000000148303754, pointer, size, load, el<<2)
            assert record[6:] == (sctlr, 2)
            assert bytes(uc.mem_read(HANDOFF+0x60,32)) == bytes(32)
            assert bytes(uc.mem_read(HANDOFF+0x80,112)) == bytes(112)
            assert bytes(uc.mem_read(HANDOFF+0xf0,16)) == bytes(16)
            assert bytes(uc.mem_read(HANDOFF+0x100,72)) == bytes(72)
            assert bytes(uc.mem_read(HANDOFF+0xFE0,32)) == bytes(32)
            assert writes[0] == len(fd)+344  # CPU record, stage, readiness and stale trial/GOP records
        else:
            assert writes[0] == 0, (name, writes[0])
        cases.append({"case": name, "result": "pass", "bytes_written": writes[0]})

    if args.ram_trial:
        watchdog_tests=runpy.run_path(str(package/"Tests/RamBootWatchdogTest.py"))
        check=watchdog_tests["shim_case"]
        entry = {n:int(at,16) for at,n in re.findall(
            r"^([0-9a-fA-F]{8,16})\s+00000000\s+0\s+(\S+)\s*$",
            (args.fd.parent.parent/"AARCH64/SiliciumPkg/Sec/Sec/DEBUG/Sec.dll.map").read_text(), re.M)}
        for name,kw in (
            ("trial-EL1-valid",dict(accepted=True,sec_entry=entry["PlatformInitialize"])),
            ("trial-EL2-valid",dict(el=2,accepted=True,sec_entry=entry["PlatformInitialize"])),
            ("trial-wrong-placement",dict(placement=BASE+0x80000,armed=False)),
            ("trial-EL0-rejected",dict(el=0,armed=False)),
            ("trial-MMU-enabled-rejected",dict(sctlr=1,armed=False)),
            ("trial-D-cache-enabled-rejected",dict(sctlr=4,armed=False)),
            ("trial-EL2-D-cache-enabled-rejected",dict(el=2,sctlr=4,armed=False)),
            ("trial-unaligned-FDT",dict(pointer=DTB_BASE+1)),
            ("trial-FDT-below-reservation",dict(pointer=DTB_BASE-8)),
            ("trial-FDT-pointer-wrap",dict(pointer=0xFFFFFFFFFFFFFFF8)),
            ("trial-FDT-header-crosses-end",dict(pointer=DTB_END-32)),
            ("trial-invalid-FDT-magic",dict(magic=0)),
            ("trial-oversized-FDT",dict(size=0x200001)),
            ("trial-short-FDT",dict(size=39)),
            ("trial-FDT-content-crosses-end",dict(pointer=DTB_END-0x1000,size=0x2000)),
            ("trial-watchdog-disable-rejected",dict(fault="mode"))):
            cases.append(check(image,fd,name,**kw))
    else:
        execute("EL1 valid handoff", accepted=True)
        execute("EL2 valid handoff", el=2, accepted=True)
        execute("Wrong kernel placement", load=BASE+0x80000)
        execute("EL0 rejected", el=0)
        execute("D-cache enabled rejected", sctlr=4)
        execute("EL2 D-cache enabled rejected", el=2, sctlr=4)
        execute("Unaligned FDT rejected", pointer=DTB_BASE+1)
        execute("FDT below reservation rejected", pointer=DTB_BASE-8)
        execute("FDT pointer wraparound rejected", pointer=0xFFFFFFFFFFFFFFF8)
        execute("FDT header crosses reservation end", pointer=DTB_END-32)
        execute("Invalid FDT magic rejected", magic=0)
        execute("Oversized FDT rejected", size=0x200001)
        execute("Short FDT rejected", size=39)
        execute("FDT content crosses reservation end", pointer=DTB_END-0x1000)
    print(json.dumps({"status": "pass", "scope": ("Preloader/Image/FV checks, actual RAM-trial SEC/DXE/BDS to UiApp entry with explicit CPU/display boundary fixtures; menu and input not executed" if args.ram_trial else "Preloader/Image/FV checks, actual SEC/DXE/BDS/Shell software execution with explicit CPU and USB/display boundary fixtures"),
                      "preloader_sha256": hashlib.sha256(raw).hexdigest(),
                      "memory_regions_checked": len(regions),
                      "firmware_dxe_execution_tested": not args.shim_only,
                      "el3_emulation_tested": False,
                      "mmu_enabled_execution_tested": False,
                      "image_sha256": hashlib.sha256(image).hexdigest(),
                      "boot_image_sha256": hashlib.sha256(boot).hexdigest() if boot is not None else None,
                      "boot_image_bytes": len(boot) if boot is not None else None,
                      "shim_only": args.shim_only, "ram_trial": args.ram_trial,
                      "trial_shim_test_source_sha256": hashlib.sha256((package/"Tests/RamBootWatchdogTest.py").read_bytes()).hexdigest() if args.ram_trial else None,
                      "boot_container_and_fv_integration_verified": not args.shim_only,
                      "shim_source_sha256": hashlib.sha256((package/"BootShim.S").read_bytes()).hexdigest(),
                      "boot_header_version": 4, "signed": False,
                      "fd_sha256": hashlib.sha256(fd).hexdigest(),
                      "packaged_content": None if args.shim_only else packaged_content(fd, args.fd.parent.parent),
                      "reset_system": None if args.shim_only else reset_system_cases(args.fd.parent.parent),
                      "prm_runtime": None if args.shim_only else prm_runtime_cases(args.fd.parent.parent),
                      "runtime_memory": None if args.shim_only else runtime_memory_cases(fd, args.fd.parent.parent, dtb),
                      "continuous_boot": None if args.shim_only else continuous_boot_cases(image, fd, args.fd.parent.parent, dtb, args.ram_trial), "cases": cases}, indent=2))

if __name__ == "__main__":
    main()
