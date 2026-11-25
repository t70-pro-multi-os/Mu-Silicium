#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Recover exact stock-kernel export CRCs without saving an unpacked kernel.

No tablet access. The original boot partition is opened rb. Kallsyms is a
locator, independently checked against every PREL32 export name and symbol.
CRC layout: the pinned reference include/linux/export-internal.h SYMBOL_CRC.
The output is evidence for this stock image, not a candidate kernel or a
claim that firmware, userspace, signature/namespace policy or devices work.
"""
import hashlib
import importlib.metadata
import json
import io
import logging
from pathlib import Path
import re
import struct
import subprocess
from vmlinux_to_elf.core.kallsyms import KallsymsFinder

PKG = Path(__file__).resolve().parent
WORK = PKG.parents[3]
BACKUP = Path('/mnt/mac/Users/you/.tmp/teclast-t70pro/device-backup')
EXPECTED = '5a8be048cddedd45da4d8a9ce415d3cbf89046d4ac549dfd8d398dd6359daa9d'
EXPECTED_PROTECTED_EXPORT_COUNT = 590
EXPECTED_VENDOR_IMPORT_COUNT = 9176

def bounded(raw, offset, size):
    if offset < 0 or size < 0 or offset + size > len(raw):
        raise ValueError('Kernel offset outside original Image')
    return raw[offset:offset + size]

def read_kernel():
    layout = json.loads((BACKUP / 'partition-layout.json').read_text())
    if not layout['primary_header_crc_valid'] or not layout['partition_entries_crc_valid']:
        raise ValueError('Original GPT verification missing')
    part = next(p for p in layout['partitions'] if p['name'] == 'boot_a')
    with (BACKUP / 'images/ufs-lu2.img').open('rb') as source:
        source.seek(part['offset_bytes'])
        header = source.read(4096)
        if header[:8] != b'ANDROID!' or struct.unpack_from('<I', header, 40)[0] != 4:
            raise ValueError('Not the expected Android boot v4 header')
        size = struct.unpack_from('<I', header, 8)[0]
        if not 0 < size <= 32 * 1024 * 1024 or size + 4096 > part['size_bytes']:
            raise ValueError('Unexpected bounded stock kernel size')
        compressed = source.read(size)
        if len(compressed) != size or compressed[:4] != bytes.fromhex('02214c18'):
            raise ValueError('Truncated or unexpected compressed kernel')
    raw = subprocess.run(['lz4', '-dc'], input=compressed, capture_output=True,
                         check=True, timeout=30).stdout
    if hashlib.sha256(raw).hexdigest() != EXPECTED or raw[56:60] != b'ARM\x64':
        raise ValueError('Stock ARM64 Image identity mismatch')
    return raw

def builtin_provider_evidence(raw, symbols, base):
    """Prove two exact stock providers through descriptors and initcall code."""
    def address(name):
        if name not in symbols:raise ValueError("Missing built-in locator: "+name)
        return symbols[name].virtual_address
    def offset(name):return address(name)-base
    def u64(pos):return struct.unpack("<Q",bounded(raw,pos,8))[0]
    def text(pos,width):
        data=bounded(raw,pos,width)
        end=data.find(bytes([0]))
        if end<0:raise ValueError("Unterminated built-in descriptor string")
        return data[:end].decode("ascii")
    def table(name):
        start=offset(name);matches=[]
        for i in range(16):
            entry=bounded(raw,start+i*200,200)
            if entry==bytes(200):return matches
            compat=text(start+i*200+64,128)
            if not compat:raise ValueError("Malformed built-in OF table")
            matches.append(compat)
        raise ValueError("Unterminated built-in OF table")
    def registered(driver,probe,ids,init,level):
        d=offset(driver)
        if u64(d)!=address(probe) or u64(d+80)!=address(ids):
            raise ValueError("Built-in platform_driver pointers disagree")
        name=text(u64(d+40)-base,128)
        # Pinned ARM64 init: ADRP x0; ADD x0,x0; MOV x1,xzr; BL register.
        pos=offset(init)+12
        adrp,add,mov,branch=struct.unpack("<4I",bounded(raw,pos,16))
        if adrp&0x9f00001f!=0x90000000 or add&0xffc003ff!=0x91000000 or mov!=0xaa1f03e1 or branch&0xfc000000!=0x94000000:
            raise ValueError("Unsupported built-in registration instruction sequence")
        delta=((adrp>>5&0x7ffff)<<2)|(adrp>>29&3)
        if delta&0x100000:delta-=0x200000
        arg=((base+pos)&~4095)+(delta<<12)+(add>>10&4095)
        call=branch&0x3ffffff
        if call&0x2000000:call-=0x4000000
        if arg!=address(driver) or base+pos+12+call*4!=address("__platform_driver_register"):
            raise ValueError("Built-in registration target disagrees")
        calls=[n for n in symbols if n.startswith("__initcall__") and n.endswith("_"+init+str(level))]
        if len(calls)!=1:raise ValueError("Missing or ambiguous built-in initcall")
        entry=offset(calls[0]);relative=struct.unpack("<i",bounded(raw,entry,4))[0]
        if base+entry+relative!=address(init):raise ValueError("Built-in initcall target disagrees")
        return {"driver_name":name,"driver_symbol":driver,"driver_image_offset":hex(d),
                "probe_symbol":probe,"of_table_symbol":ids,"compatible":table(ids),
                "registration_initcall":calls[0],"initcall_image_offset":hex(entry),
                "registration_instructions_image_offset":hex(pos),
                "registration_function":"__platform_driver_register","runtime_registration_verified":False}
    fixed=registered("of_fixed_clk_driver","of_fixed_clk_probe","of_fixed_clk_ids","of_fixed_clk_driver_init",6)
    entry=offset("__of_table_fixed_clk")
    if not offset("__clk_of_table")<=entry<offset("__clk_of_table_sentinel") or \
       text(entry+64,128)!="fixed-clock" or u64(entry+192)!=address("__fixed_clk_of_clk_init_declare"):
        raise ValueError("Built-in early clock declaration disagrees")
    fixed["early_declaration"]={"table":"__clk_of_table","entry":"__of_table_fixed_clk",
        "entry_image_offset":hex(entry),"callback":"__fixed_clk_of_clk_init_declare"}
    regulator=registered("regulator_fixed_voltage_driver","reg_fixed_voltage_probe","fixed_of_match","regulator_fixed_voltage_init",4)
    return {"scope":"Exact stock descriptors, pointer targets, ARM64 registration arguments/callee and PREL32 initcalls; no device execution",
            "providers":[fixed,regulator],
            "other_providers":"Not classified by this bounded check"}

def firmware_parameter_evidence(raw, symbols, base):
    """Match the exact built-in path parameter and its setter/getter pointers."""
    names = ["__param_path", "__param_str_path", "firmware_param_ops",
             "firmware_param_path_set", "firmware_param_path_get", "fw_path_para"]
    if any(n not in symbols for n in names):
        raise ValueError("Missing stock firmware parameter locator")
    address = lambda n: symbols[n].virtual_address
    offset = lambda n: address(n) - base
    p = offset("__param_path")
    name, owner, ops, perm, level, flags, arg = struct.unpack(
        "<QQQHbB4xQ", bounded(raw, p, 40))
    text = b"firmware_class.path" + bytes([0])
    if name != address("__param_str_path") or bounded(raw, name-base, len(text)) != text or owner != 0:
        raise ValueError("Firmware parameter identity or built-in owner differs")
    if ops != address("firmware_param_ops") or perm != 0o644 or level != -1 or flags or arg:
        raise ValueError("Firmware parameter descriptor differs")
    op_flags, reserved, setter, getter, release = struct.unpack(
        "<IIQQQ", bounded(raw, offset("firmware_param_ops"), 32))
    if op_flags or reserved or release or setter != address("firmware_param_path_set") or getter != address("firmware_param_path_get"):
        raise ValueError("Firmware parameter callbacks differ")
    return {"parameter":"firmware_class.path", "value":"/lib/firmware",
            "descriptor_image_offset":hex(p), "permissions":"0644",
            "setter":"firmware_param_path_set", "getter":"firmware_param_path_get",
            "proof":"Exact built-in parameter name/owner/flags and operations pointers",
            "runtime_parameter_acceptance_verified":False}

def _arm64_word(raw, offset):
    return struct.unpack("<I", bounded(raw, offset, 4))[0]

def _arm64_adrp(raw, base, offset):
    ins = _arm64_word(raw, offset)
    if ins & 0x9f000000 != 0x90000000:
        raise ValueError("Expected ARM64 ADRP in policy lookup")
    imm = ((ins >> 5 & 0x7ffff) << 2) | (ins >> 29 & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return ((base + offset) & ~4095) + (imm << 12), ins & 31

def _arm64_add_target(raw, page, offset, register):
    ins = _arm64_word(raw, offset)
    if ins & 0xffc00000 != 0x91000000:
        raise ValueError("Expected ARM64 ADD-immediate in policy lookup")
    rd, rn = ins & 31, (ins >> 5) & 31
    if rd != register or rn != register:
        raise ValueError("Policy lookup ADD register mismatch")
    imm = (ins >> 10) & 0xfff
    if ins & (1 << 22):
        imm <<= 12
    return page + imm

def _arm64_ldr_x_address(raw, page, offset, base_register, target_register):
    ins = _arm64_word(raw, offset)
    if ins & 0xffc00000 != 0xf9400000:
        raise ValueError("Expected ARM64 LDR-X in policy lookup")
    if ((ins >> 5) & 31) != base_register or (ins & 31) != target_register:
        raise ValueError("Policy lookup LDR-X register mismatch")
    return page + (((ins >> 10) & 0xfff) * 8)

def _arm64_bl_target(raw, base, offset):
    ins = _arm64_word(raw, offset)
    if ins & 0xfc000000 != 0x94000000:
        raise ValueError("Expected ARM64 BL in policy lookup")
    imm = ins & 0x3ffffff
    if imm & (1 << 25):
        imm -= 1 << 26
    return base + offset + (imm << 2)

def _arm64_b_target(raw, base, offset):
    ins = _arm64_word(raw, offset)
    if ins & 0xfc000000 != 0x14000000:
        raise ValueError("Expected ARM64 B in policy lookup")
    imm = ins & 0x3ffffff
    if imm & (1 << 25):
        imm -= 1 << 26
    return base + offset + (imm << 2)

def _arm64_cbz_x_target(raw, base, offset, register):
    ins = _arm64_word(raw, offset)
    if ins & 0x7f000000 != 0x34000000 or ins & 0x01000000:
        raise ValueError("Expected ARM64 CBZ-X in policy lookup")
    if not (ins & 0x80000000) or (ins & 31) != register:
        raise ValueError("Policy lookup CBZ-X register mismatch")
    imm = (ins >> 5) & 0x7ffff
    if imm & (1 << 18):
        imm -= 1 << 19
    return base + offset + (imm << 2)

def _arm64_tbz_register(raw, base, offset, register, bit, target, nonzero):
    ins = _arm64_word(raw, offset)
    if ins & 0x7e000000 != 0x36000000:
        raise ValueError("Expected ARM64 TBZ/TBNZ in signature gate")
    is_nonzero = bool(ins & 0x01000000)
    test_bit = (((ins >> 31) & 1) << 5) | ((ins >> 19) & 0x1f)
    imm = (ins >> 5) & 0x3fff
    if imm & (1 << 13):
        imm -= 1 << 14
    actual_target = base + offset + (imm << 2)
    if (ins & 31) != register or test_bit != bit or is_nonzero != nonzero or actual_target != target:
        raise ValueError("ARM64 signature gate branch differs")

def _arm64_ldrb_field(raw, offset, target_register, base_register, field_offset):
    ins = _arm64_word(raw, offset)
    if ins & 0xffc00000 != 0x39400000:
        raise ValueError("Expected ARM64 LDRB signature flag")
    if (ins & 31) != target_register or ((ins >> 5) & 31) != base_register or \
       ((ins >> 10) & 0xfff) != field_offset:
        raise ValueError("ARM64 signature flag field differs")

def _exact_symbol(symbols, values_by_name, name):
    if name not in symbols:
        raise ValueError("Missing policy callsite symbol: " + name)
    address = symbols[name].virtual_address
    if values_by_name.get(name, set()) != {address}:
        raise ValueError("Ambiguous policy callsite symbol: " + name)
    return address

def _read_symbol_pointer_array(raw, base, start, count, expected_count, label):
    if count != expected_count:
        raise ValueError("Unexpected " + label + " count")
    if count <= 0:
        raise ValueError("Empty " + label + " table")
    data = bounded(raw, start, count * 8)
    names = []
    for index in range(count):
        pointer = struct.unpack_from("<Q", data, index * 8)[0]
        pos = pointer - base
        if pointer < base or pos >= len(raw):
            raise ValueError("Out-of-image " + label + " string pointer")
        end = raw.find(bytes([0]), pos, min(len(raw), pos + 512))
        if end < 0:
            raise ValueError("Unterminated " + label + " string")
        try:
            name = raw[pos:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError("Non-ASCII " + label + " string") from exc
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise ValueError("Malformed " + label + " symbol name")
        names.append(name)
    if names != sorted(names) or len(names) != len(set(names)):
        raise ValueError("Unsorted or duplicate " + label + " names")
    return names

def protected_symbol_exports_evidence(raw, symbols, base, values_by_name):
    array_address = _exact_symbol(symbols, values_by_name, "protected_symbol_exports")
    count_address = _exact_symbol(symbols, values_by_name, "protected_symbol_exports_count")
    formation = _exact_symbol(symbols, values_by_name, "complete_formation")
    bsearch = _exact_symbol(symbols, values_by_name, "bsearch")
    comparator = _exact_symbol(symbols, values_by_name, "cmp_string")
    array_offset, count_offset = array_address - base, count_address - base
    count = struct.unpack("<Q", bounded(raw, count_offset, 8))[0]
    names = _read_symbol_pointer_array(raw, base, array_offset, count,
                                       EXPECTED_PROTECTED_EXPORT_COUNT,
                                       "protected export")
    fn = formation - base
    page, reg = _arm64_adrp(raw, base, fn + 0x74)
    if reg != 27 or _arm64_ldr_x_address(raw, page, fn + 0xd8, 27, 2) != count_address:
        raise ValueError("Unsigned-export check does not load protected export count")
    page, reg = _arm64_adrp(raw, base, fn + 0x78)
    if reg != 21 or _arm64_add_target(raw, page, fn + 0x7c, reg) != array_address:
        raise ValueError("Unsigned-export check does not reference protected export array")
    page, reg = _arm64_adrp(raw, base, fn + 0x80)
    if reg != 22 or _arm64_add_target(raw, page, fn + 0x84, reg) != comparator:
        raise ValueError("Unsigned-export check comparator differs")
    if _arm64_bl_target(raw, base, fn + 0xec) != bsearch:
        raise ValueError("Unsigned-export check is not a bsearch call")
    _arm64_ldrb_field(raw, fn + 0xcc, 8, 19, 0x179)
    _arm64_tbz_register(raw, base, fn + 0xd0, 8, 0, formation + 0x90, True)
    if _arm64_cbz_x_target(raw, base, fn + 0xf0, 0) != formation + 0x90:
        raise ValueError("Protected export search miss does not continue the loop")
    for at in (0xdc, 0x17c):
        expected_args = (0xaa1503e1, 0x52800103, 0xaa1603e4, 0x8b0802e0)
        if tuple(_arm64_word(raw, fn + at + index * 4) for index in range(4)) != expected_args:
            raise ValueError("Protected export bsearch arguments differ")
    if _arm64_b_target(raw, base, fn + 0xf4) != formation + 0x194:
        raise ValueError("Protected export match does not reach denial path")
    if _arm64_word(raw, fn + 0x1ac) != 0x12800194 or \
       _arm64_word(raw, fn + 0x224) != 0x2a1403e0:
        raise ValueError("Protected export denial is not returned as -EACCES")
    page2, reg2 = _arm64_adrp(raw, base, fn + 0x114)
    if reg2 != 26 or _arm64_ldr_x_address(raw, page2, fn + 0x178, 26, 2) != count_address:
        raise ValueError("GPL-export check does not load protected export count")
    page2, reg2 = _arm64_adrp(raw, base, fn + 0x118)
    if reg2 != 21 or _arm64_add_target(raw, page2, fn + 0x11c, reg2) != array_address or \
       _arm64_bl_target(raw, base, fn + 0x18c) != bsearch:
        raise ValueError("GPL-export check does not use protected export table")
    page2, reg2 = _arm64_adrp(raw, base, fn + 0x120)
    if reg2 != 22 or _arm64_add_target(raw, page2, fn + 0x124, reg2) != comparator:
        raise ValueError("GPL-export comparator differs")
    _arm64_ldrb_field(raw, fn + 0x16c, 8, 19, 0x179)
    _arm64_tbz_register(raw, base, fn + 0x170, 8, 0, formation + 0x130, True)
    if _arm64_cbz_x_target(raw, base, fn + 0x190, 0) != formation + 0x130:
        raise ValueError("GPL protected export search miss does not continue the loop")
    return {
        "status": "verified",
        "image_sha256": EXPECTED,
        "array_symbol": "protected_symbol_exports",
        "count_symbol": "protected_symbol_exports_count",
        "count": count,
        "names": names,
        "array_image_range": [hex(array_offset), hex(array_offset + count * 8)],
        "validation": "Each 64-bit pointer resolves inside the exact Image to a NUL-terminated ASCII C symbol name; count, bounds, sorted order and uniqueness checked",
        "verified_callsite": {
            "status": "verified",
            "function": "complete_formation",
            "function_image_offset": hex(fn),
            "array_reference_instruction_offset": hex(fn + 0x78),
            "count_reference_instruction_offset": hex(fn + 0x74),
            "search": "bsearch",
            "comparator": "cmp_string",
            "pointer_size": 8,
            "signature_flag_field_offset": "module+0x179 (sig_ok field in kernel-reference/include/linux/module.h)",
            "modules_with_sig_ok_skip_protected_export_search": True,
            "unsigned_module_export_match_returns": "-EACCES (-13)",
            "runtime_execution_verified": False
        },
        "source_module_names": {
            "status": "not_recovered_from_Image",
            "runtime_required_for_protected_export_check": False,
            "reason": "The verified runtime check consumes this derived symbol-name array and its count; it does not consume the original protected module-name input."
        }
    }

def vendor_symbol_access_evidence(raw, symbols, base, values_by_name, protected):
    if protected.get("status") != "verified":
        raise ValueError("Protected export evidence is not verified")
    array_address = _exact_symbol(symbols, values_by_name, "protected_symbol_exports")
    count_address = _exact_symbol(symbols, values_by_name, "protected_symbol_exports_count")
    resolver = _exact_symbol(symbols, values_by_name, "resolve_symbol")
    bsearch = _exact_symbol(symbols, values_by_name, "bsearch")
    comparator = _exact_symbol(symbols, values_by_name, "cmp_string")
    protected_count = protected["count"]
    protected_start = array_address - base
    import_start = protected_start + protected_count * 8
    if import_start != int(protected["array_image_range"][1], 16):
        raise ValueError("Protected export range metadata differs")
    if import_start < protected_start or import_start + EXPECTED_VENDOR_IMPORT_COUNT * 8 > len(raw):
        raise ValueError("Unsigned-import table overlaps or exceeds Image")
    count_data_offset = count_address - base
    adjacent_counts = bounded(raw, count_data_offset, 16)
    protected_count_field, import_count = struct.unpack("<QQ", adjacent_counts)
    if protected_count_field != protected_count or import_count != EXPECTED_VENDOR_IMPORT_COUNT:
        raise ValueError("Protected/export and unsigned/import counts disagree")
    if protected_start < count_data_offset + 16 and count_data_offset < import_start + import_count * 8:
        raise ValueError("Policy pointer arrays overlap their count storage")
    names = _read_symbol_pointer_array(raw, base, import_start, import_count,
                                       EXPECTED_VENDOR_IMPORT_COUNT,
                                       "unsigned import allowlist")
    fn = resolver - base
    count_page, count_reg = _arm64_adrp(raw, base, fn + 0x174)
    if count_reg != 8 or _arm64_ldr_x_address(raw, count_page, fn + 0x180, 8, 2) != count_address + 8:
        raise ValueError("Unsigned-import check does not load adjacent import count")
    array_page, array_reg = _arm64_adrp(raw, base, fn + 0x178)
    if array_reg != 1 or _arm64_add_target(raw, array_page, fn + 0x17c, array_reg) != base + import_start:
        raise ValueError("Unsigned-import check does not reference adjacent allowlist")
    compare_page, compare_reg = _arm64_adrp(raw, base, fn + 0x184)
    if compare_reg != 4 or _arm64_add_target(raw, compare_page, fn + 0x188, compare_reg) != comparator:
        raise ValueError("Unsigned-import check comparator differs")
    if _arm64_bl_target(raw, base, fn + 0x194) != bsearch:
        raise ValueError("Unsigned-import check is not a bsearch call")
    _arm64_ldrb_field(raw, fn + 0x150, 8, 20, 0x179)
    _arm64_ldrb_field(raw, fn + 0x158, 9, 21, 0x179)
    gate_words = {
        0x154: 0xb40000b5, 0x15c: 0x52000129, 0x160: 0x36000088,
        0x164: 0x1400000e, 0x168: 0x2a1f03e9, 0x16c: 0x37000188,
        0x170: 0x37000169, 0x18c: 0xaa1603e0, 0x190: 0x52800103,
        0x198: 0xb4000200, 0x1d8: 0x92800188
    }
    if any(_arm64_word(raw, fn + offset) != word for offset, word in gate_words.items()):
        raise ValueError("Unsigned-import signature gate or denial path differs")
    if _arm64_cbz_x_target(raw, base, fn + 0x198, 0) != resolver + 0x1d8:
        raise ValueError("Unsigned-import denial branch differs")
    return {
        "status": "verified",
        "image_sha256": EXPECTED,
        "runtime_required_for_unsigned_import_check": True,
        "policy": "Unsigned module imports from the built-in kernel or a sig_ok provider must match this name table; imports from unsigned module providers bypass this table",
        "array_start_image_offset": hex(import_start),
        "array_image_range": [hex(import_start), hex(import_start + import_count * 8)],
        "count_storage": "protected_symbol_exports_count + 8",
        "count_storage_image_offset": hex(count_data_offset + 8),
        "count": import_count,
        "names": names,
        "validation": "The resolver's direct pointer and adjacent count match this bounded 64-bit pointer array; every pointer resolves to a NUL-terminated ASCII C symbol name; sorted order and uniqueness checked",
        "verified_callsite": {
            "status": "verified",
            "function": "resolve_symbol",
            "function_image_offset": hex(fn),
            "array_reference_instruction_offset": hex(fn + 0x178),
            "count_reference_instruction_offset": hex(fn + 0x174),
            "search": "bsearch",
            "comparator": "cmp_string",
            "pointer_size": 8,
            "caller_signature_flag_field_offset": "module+0x179 (sig_ok field in kernel-reference/include/linux/module.h)",
            "provider_signature_flag_field_offset": "module+0x179 (sig_ok field in kernel-reference/include/linux/module.h)",
            "unsigned_caller_to_kernel_or_signed_provider_is_checked": True,
            "unsigned_provider_bypasses_import_allowlist": True,
            "missing_name_returns": "-EACCES (-13)",
            "runtime_execution_verified": False
        },
        "source_kmi_whitelist": {
            "status": "not_recovered_from_Image",
            "source_input_required_for_runtime_check": False,
            "reason": "The exact compiled runtime allowlist is recovered above. The original build-time text input is not required to evaluate this kernel's name-based import check."
        },
        "signature_trust": {
            "status": "unknown",
            "reason": "A module signature trailer or sig_ok branch does not prove that the stock signing key or any supplied module signature is trusted."
        }
    }


def trusted_public_certificate_evidence(raw, symbols, base, values_by_name):
    start = _exact_symbol(symbols, values_by_name, "__cert_list_start")
    end = _exact_symbol(symbols, values_by_name, "__cert_list_end")
    if _exact_symbol(symbols, values_by_name, "system_certificate_list") != start:
        raise ValueError("Built-in public certificate aliases disagree")
    if end <= start or end - start > 131072:
        raise ValueError("Invalid built-in public certificate range")
    data = bounded(raw, start - base, end - start)
    certificates = []
    pos = 0
    while pos < len(data):
        if data[pos] != 0x30 or pos + 2 > len(data):
            raise ValueError("Public certificate is not a bounded DER sequence")
        length = data[pos + 1]
        prefix = 2
        if length & 0x80:
            width = length & 0x7f
            if not 1 <= width <= 4 or pos + 2 + width > len(data) or data[pos + 2] == 0:
                raise ValueError("Invalid public certificate DER length")
            length = int.from_bytes(data[pos + 2:pos + 2 + width], "big")
            if length < 128:
                raise ValueError("Noncanonical public certificate DER length")
            prefix += width
        size = prefix + length
        if length == 0 or size > len(data) - pos:
            raise ValueError("Public certificate exceeds built-in table")
        certificate = data[pos:pos + size]
        certificates.append({"image_offset": hex(start - base + pos), "bytes": size,
                             "sha256": hashlib.sha256(certificate).hexdigest()})
        pos += size
    if not certificates or len({x["sha256"] for x in certificates}) != len(certificates):
        raise ValueError("Empty or duplicate built-in public certificate list")
    return {"status": "verified", "image_sha256": EXPECTED,
            "source": "__cert_list_start..__cert_list_end / system_certificate_list",
            "image_range": [hex(start - base), hex(end - base)],
            "count": len(certificates), "certificates": certificates,
            "public_certificates_only": True,
            "private_key_access": False, "runtime_keyring_execution_verified": False}


def extract(raw):
    if hashlib.sha256(raw).hexdigest() != EXPECTED:
        raise ValueError("Stock Image identity mismatch before policy extraction")
    diagnostics = io.StringIO()
    handler = logging.StreamHandler(diagnostics)
    handler.setLevel(logging.WARNING)
    logging.getLogger().addHandler(handler)
    try:
        k = KallsymsFinder(raw, bit_size=64)
    finally:
        logging.getLogger().removeHandler(handler)
    syms = k.name_to_symbol
    values_by_name = {}
    for symbol in k.symbols:
        values_by_name.setdefault(symbol.name, set()).add(symbol.virtual_address)
    base = syms['_text'].virtual_address
    if base != k.relative_base_address or k.kernel_text_candidate != base:
        raise ValueError('Kallsyms image base disagreement')
    if not base < syms['_stext'].virtual_address < base + len(raw):
        raise ValueError('Kallsyms text outside Image')
    protected_policy = protected_symbol_exports_evidence(raw, syms, base, values_by_name)
    vendor_policy = vendor_symbol_access_evidence(raw, syms, base, values_by_name, protected_policy)
    def offset(name):
        return syms[name].virtual_address - base
    def string(address):
        start = address - base
        data = bounded(raw, start, min(512, len(raw) - start))
        if b'\0' not in data:
            raise ValueError('Unterminated export string')
        return data.split(b'\0', 1)[0].decode('ascii')
    exports = {}
    tables = []
    for suffix, export_type in [('', 'EXPORT_SYMBOL'), ('_gpl', 'EXPORT_SYMBOL_GPL')]:
        start = offset('__start___ksymtab' + suffix)
        end = offset('__stop___ksymtab' + suffix)
        crc_start = offset('__start___kcrctab' + suffix)
        crc_end = offset('__stop___kcrctab' + suffix)
        if end < start or (end - start) % 12 or crc_end - crc_start != (end - start) // 3:
            raise ValueError('PREL32 export and inline CRC table sizes disagree')
        bounded(raw, start, end - start)
        bounded(raw, crc_start, crc_end - crc_start)
        names = []
        for pos in range(start, end, 12):
            value_delta, name_delta, ns_delta = struct.unpack_from('<iii', raw, pos)
            name = string(base + pos + 4 + name_delta)
            namespace = string(base + pos + 8 + ns_delta)
            if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', name) or name in exports:
                raise ValueError('Invalid or duplicate exported symbol')
            # Check locator against two distinct pieces of actual export data.
            if offset('__ksymtab_' + name) != pos:
                raise ValueError('Kallsyms/export entry disagreement: ' + name)
            target = base + pos + value_delta
            if target not in values_by_name.get(name, set()):
                raise ValueError('Kallsyms/export value disagreement: ' + name)
            crc = struct.unpack_from('<I', raw, crc_start + (pos - start) // 3)[0]
            exports[name] = {'crc': crc, 'export': export_type, 'namespace': namespace,
                             'provider': 'stock-vmlinux', 'ksymtab_image_offset': hex(pos)}
            names.append(name)
        if names != sorted(names):
            raise ValueError('Export table is not name-sorted')
        tables.append({'kind': export_type, 'count': len(names),
                       'ksymtab_image_range': [hex(start), hex(end)],
                       'kcrctab_image_range': [hex(crc_start), hex(crc_end)]})
    return {'schema': 1, 'scope': 'Exact shipped ARM64 Image export CRCs; offline static evidence',
            'device_access': False, 'original_open_mode': 'rb', 'unpacked_kernel_saved': False,
            'image_sha256': EXPECTED, 'image_bytes': len(raw),
            'parser_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'locator': {'tool': 'vmlinux-to-elf', 'version': importlib.metadata.version('vmlinux-to-elf'),
                        'symbols': len(k.symbols), 'base': hex(base),
                        'heuristic_warnings': diagnostics.getvalue().splitlines()},
            'validation': 'All PREL32 names, table entries, target symbols and parallel CRC counts cross-checked; duplicate local names retained when checking values',
            'tables': tables, 'export_count': len(exports), 'exports': exports,
            'protected_symbol_exports_evidence': protected_policy,
            'vendor_symbol_access_evidence': vendor_policy,
            'trusted_public_certificates': trusted_public_certificate_evidence(raw,syms,base,values_by_name),
            'built_in_provider_evidence': builtin_provider_evidence(raw,syms,base),
            'firmware_search_parameter': firmware_parameter_evidence(raw,syms,base),
            'limitations': ['No candidate kernel comparison or runtime symbol addresses',
                'No module loading or runtime policy execution; signature trust/key validation, namespace/license acceptance, hardware and userspace remain unverified']}

def main():
    report = extract(read_kernel())
    out = WORK / 'analysis/stock-kernel-exports.json'
    out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['scope','image_sha256','locator','tables','export_count']}))
if __name__ == '__main__':
    main()
