#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Audit extracted stock modules and optionally create a non-bootable CPIO payload."""
import argparse
import base64
import os
import subprocess
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


GPL_COMPATIBLE_LICENSES = {
    "GPL", "GPL v2", "GPL and additional rights", "Dual BSD/GPL",
    "Dual MIT/GPL", "Dual MPL/GPL",
}


def modinfo_fields(raw):
    fields = {}
    for item in raw.split(b"\0"):
        if b"=" not in item:
            continue
        key, value = item.split(b"=", 1)
        fields.setdefault(key.decode("ascii", "replace"), []).append(
            value.decode("utf-8", "replace"))
    return fields


def config(path):
    result = {}
    for line in path.read_text().splitlines():
        m = re.fullmatch(r"CONFIG_([A-Za-z0-9_]+)=(.*)", line)
        if m:
            result[m.group(1)] = m.group(2)
        else:
            m = re.fullmatch(r"# CONFIG_([A-Za-z0-9_]+) is not set", line)
            if m:
                result[m.group(1)] = "n"
    return result


def elf_module(data):
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("not ELF64 little-endian")
    kind, machine = struct.unpack_from("<HH", data, 16)
    if kind != 1 or machine != 183:
        raise ValueError("not an AArch64 relocatable module")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    entsize, count, names_i = struct.unpack_from("<HHH", data, 58)
    if entsize != 64 or not count or names_i >= count or shoff + entsize * count > len(data):
        raise ValueError("bad ELF section table")
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * entsize) for i in range(count)]

    def contents(sec):
        off, size = sec[4], sec[5]
        if off + size > len(data):
            raise ValueError("section exceeds file bounds")
        return data[off:off + size]

    table = contents(sections[names_i])
    named, section_names = {}, []
    for sec in sections:
        if sec[0] >= len(table):
            raise ValueError("section name exceeds string table")
        name = table[sec[0]:].split(b"\0", 1)[0].decode("ascii")
        section_names.append(name)
        if name:
            named[name] = sec
    if ".modinfo" not in named:
        raise ValueError("missing .modinfo")
    fields = modinfo_fields(contents(named[".modinfo"]))

    legacy = {}
    legacy_status = "absent"
    if "__versions" in named:
        raw = contents(named["__versions"])
        if len(raw) % 64:
            legacy_status = "invalid legacy __versions size"
        else:
            for pos in range(0, len(raw), 64):
                name = raw[pos + 8:pos + 64].split(b"\0", 1)[0].decode("ascii")
                if not name or name in legacy:
                    legacy_status = "invalid legacy __versions names"
                    legacy = {}
                    break
                legacy[name] = struct.unpack_from("<Q", raw, pos)[0]
            else:
                legacy_status = "parsed"

    ext_names_present = "__version_ext_names" in named
    ext_crcs_present = "__version_ext_crcs" in named
    versions, version_format, version_status, legacy_match = {}, "absent", "absent", None
    if ext_names_present or ext_crcs_present:
        if not (ext_names_present and ext_crcs_present):
            version_status = "incomplete extended modversion sections"
        else:
            names_raw = contents(named["__version_ext_names"])
            crcs_raw = contents(named["__version_ext_crcs"])
            if len(crcs_raw) % 4 or not names_raw.endswith(b"\0"):
                version_status = "invalid extended modversion section size"
            else:
                raw_names = names_raw.split(b"\0")
                while raw_names and not raw_names[-1]:
                    raw_names.pop()
                if any(not name for name in raw_names):
                    version_status = "empty extended symbol name"
                else:
                    names = [name.decode("ascii") for name in raw_names]
                    crcs = [struct.unpack_from("<I", crcs_raw, pos)[0]
                            for pos in range(0, len(crcs_raw), 4)]
                    if len(names) != len(crcs) or len(set(names)) != len(names):
                        version_status = "extended symbol/CRC count or uniqueness mismatch"
                    else:
                        versions = dict(zip(names, crcs))
                        version_format = "extended"
                        version_status = "parsed"
                        if legacy_status == "parsed":
                            legacy_match = legacy == versions
                            if not legacy_match:
                                version_status = "extended/legacy CRC mismatch"
    elif legacy_status == "parsed":
        versions = legacy
        version_format = "legacy"
        version_status = "parsed"

    symtab_indexes = [index for index, sec in enumerate(sections) if sec[1] == 2]
    if not symtab_indexes:
        raise ValueError("missing ELF symbol table")
    ksymtab, kcrctab, symbol_tables = {}, {}, {}
    for symtab_index in symtab_indexes:
        symtab = sections[symtab_index]
        entsize = symtab[9]
        if entsize != 24 or symtab[5] % entsize or symtab[6] >= len(sections):
            raise ValueError("invalid ELF64 symbol table")
        strings = contents(sections[symtab[6]])
        raw_symbols = contents(symtab)
        for pos in range(0, len(raw_symbols), entsize):
            nameoff, _, _, shndx, value, _ = struct.unpack_from("<IBBHQQ", raw_symbols, pos)
            symbol_tables.setdefault(symtab_index, []).append(
                (nameoff, shndx, value, strings))
            if nameoff >= len(strings) or shndx >= len(sections):
                continue
            symbol = strings[nameoff:].split(b"\0", 1)[0].decode("ascii", errors="replace")
            if symbol.startswith("__ksymtab_") and symbol != "__ksymtab_strings":
                export = symbol[len("__ksymtab_"):]
                if export in ksymtab:
                    raise ValueError(f"duplicate __ksymtab entry for {export}")
                ksymtab[export] = (shndx, value)
            elif symbol.startswith("__crc_"):
                export = symbol[len("__crc_"):]
                if export in kcrctab:
                    raise ValueError(f"duplicate __crc entry for {export}")
                kcrctab[export] = (shndx, value)

    exports, export_info, export_errors = {}, {}, []
    namespace_by_slot, namespace_slot_errors = {}, {}
    ksym_section_indexes = {index for index, name in enumerate(section_names)
                            if name in ("__ksymtab", "__ksymtab_gpl")}
    string_section_indexes = {index for index, name in enumerate(section_names)
                              if name == "__ksymtab_strings"}

    def namespace_error(slot, message):
        namespace_slot_errors.setdefault(slot, []).append(message)

    # The namespace pointer is the third field of each 12-byte __ksymtab
    # entry. Resolve its AArch64 RELA record; never infer an empty namespace.
    for relocation in sections:
        if relocation[1] != 4 or relocation[7] not in ksym_section_indexes:
            continue
        target_index = relocation[7]
        target_name = section_names[target_index]
        if (relocation[9] != 24 or relocation[5] % 24
                or relocation[6] not in symbol_tables):
            export_errors.append(f"{target_name}: invalid namespace RELA section")
            namespace_slot_errors["section:" + str(target_index)] = [
                "invalid namespace RELA section layout"]
            continue
        rela_data = contents(relocation)
        symbols = symbol_tables[relocation[6]]
        for rela_offset in range(0, len(rela_data), 24):
            target_offset, info, addend = struct.unpack_from("<QQq", rela_data, rela_offset)
            if target_offset % 12 != 8:
                continue
            slot = (target_index, target_offset - 8)
            if slot in namespace_by_slot or slot in namespace_slot_errors:
                namespace_error(slot, "duplicate namespace relocation")
                namespace_by_slot.pop(slot, None)
                continue
            if target_offset < 8 or target_offset + 4 > sections[target_index][5]:
                namespace_error(slot, "namespace relocation target is outside __ksymtab")
                continue
            symbol_index, relocation_type = info >> 32, info & 0xffffffff
            if relocation_type != 261:  # R_AARCH64_PREL32
                namespace_error(slot, "namespace relocation is not R_AARCH64_PREL32")
                continue
            if symbol_index >= len(symbols):
                namespace_error(slot, "namespace relocation symbol index is invalid")
                continue
            _, string_section_index, string_value, _ = symbols[symbol_index]
            if string_section_index not in string_section_indexes:
                namespace_error(slot, "namespace relocation does not reference __ksymtab_strings")
                continue
            string_section = contents(sections[string_section_index])
            string_offset = string_value + addend
            if string_offset < 0 or string_offset >= len(string_section):
                namespace_error(slot, "namespace string offset is outside __ksymtab_strings")
                continue
            string_end = string_section.find(b"\0", string_offset)
            if string_end < 0:
                namespace_error(slot, "namespace string is not NUL terminated")
                continue
            try:
                namespace = string_section[string_offset:string_end].decode("ascii")
            except UnicodeDecodeError:
                namespace_error(slot, "namespace string is not ASCII")
                continue
            namespace_by_slot[slot] = namespace

    for relocation_slot, messages in namespace_slot_errors.items():
        if isinstance(relocation_slot, tuple):
            for message in messages:
                export_errors.append(
                    f"{section_names[relocation_slot[0]]}+0x{relocation_slot[1]:x}: {message}")

    if set(ksymtab) != set(kcrctab):
        export_errors.append("export symbol and CRC symbol sets differ")
    used_ksym_slots, used_crc_slots = set(), set()
    for export in sorted(set(ksymtab) | set(kcrctab)):
        if export not in ksymtab or export not in kcrctab:
            continue
        ksym_index, ksym_offset = ksymtab[export]
        crc_index, crc_offset = kcrctab[export]
        ksym_section, crc_section = section_names[ksym_index], section_names[crc_index]
        expected_crc_section = "__kcrctab_gpl" if ksym_section == "__ksymtab_gpl" else "__kcrctab"
        if ksym_section not in ("__ksymtab", "__ksymtab_gpl") or crc_section != expected_crc_section:
            export_errors.append(f"{export}: __ksymtab/__kcrctab section membership mismatch")
            continue
        ksym_data, crc_data = contents(sections[ksym_index]), contents(sections[crc_index])
        if ksym_offset % 12 or ksym_offset + 12 > len(ksym_data):
            export_errors.append(f"{export}: invalid __ksymtab entry offset")
            continue
        if crc_offset % 4 or crc_offset + 4 > len(crc_data):
            export_errors.append(f"{export}: invalid __kcrctab entry offset")
            continue
        if ksym_offset // 12 != crc_offset // 4:
            export_errors.append(f"{export}: __ksymtab/__kcrctab ordinal mismatch")
            continue
        ksym_slot, crc_slot = (ksym_index, ksym_offset // 12), (crc_index, crc_offset // 4)
        if ksym_slot in used_ksym_slots or crc_slot in used_crc_slots:
            export_errors.append(f"{export}: duplicate __ksymtab/__kcrctab ordinal")
            continue
        used_ksym_slots.add(ksym_slot)
        used_crc_slots.add(crc_slot)
        crc = struct.unpack_from("<I", crc_data, crc_offset)[0]
        exports[export] = crc
        namespace_slot = (ksym_index, ksym_offset)
        namespace = namespace_by_slot.get(namespace_slot)
        if namespace is None:
            errors = namespace_slot_errors.get(namespace_slot)
            if errors:
                export_errors.extend(f"{export}: {message}" for message in errors)
            else:
                export_errors.append(f"{export}: missing export namespace relocation")
        export_info[export] = {
            "crc": crc,
            "export": "EXPORT_SYMBOL_GPL" if ksym_section == "__ksymtab_gpl" else "EXPORT_SYMBOL",
            "namespace": namespace,
            "ksymtab_section": ksym_section,
            "ksymtab_offset": ksym_offset,
            "kcrctab_section": crc_section,
            "kcrctab_offset": crc_offset,
        }
    for index, section_name in enumerate(section_names):
        if section_name in ("__ksymtab", "__ksymtab_gpl"):
            size = sections[index][5]
            used = sum(slot[0] == index for slot in used_ksym_slots)
            if size % 12 or used != size // 12:
                export_errors.append(f"{section_name}: symbol count does not cover table section")
        elif section_name in ("__kcrctab", "__kcrctab_gpl"):
            size = sections[index][5]
            used = sum(slot[0] == index for slot in used_crc_slots)
            if size % 4 or used != size // 4:
                export_errors.append(f"{section_name}: CRC count does not cover table section")

    return {
        "name": fields.get("name", [None])[0],
        "vermagic": fields.get("vermagic", [None])[0],
        "depends": sorted({d.replace("-", "_") for value in fields.get("depends", [])
                           for d in value.split(",") if d}),
        "firmware": sorted(set(fields.get("firmware", []))),
        "license": list(fields.get("license", [])),
        "import_ns": list(fields.get("import_ns", [])),
        "modversions": versions,
        "modversion_format": version_format,
        "modversion_status": version_status,
        "modversion_count": len(versions),
        "legacy_modversion_count": len(legacy),
        "extended_legacy_match": legacy_match,
        "exported_symbols": exports,
        "exported_symbol_info": export_info,
        "exported_symbol_count": len(exports),
        "export_integrity_errors": export_errors,
        "signed": data.endswith(b"~Module signature appended~\n"),
    }

def module_catalog(stock):
    result = {}
    for source, part in stock.get("partitions", {}).items():
        for name, info in part.get("modules", {}).items():
            result.setdefault(name, {})[source] = info
    for i, frag in enumerate(stock.get("vendor_boot_a", {}).get("fragments", [])):
        for name, info in frag.get("modules", {}).items():
            result.setdefault(name, {})[f"vendor_boot_a:{i}"] = info
    return result


def read_symvers(path):
    result = {}
    for no, line in enumerate(path.read_text().splitlines(), 1):
        cols = line.split()
        if not cols:
            continue
        if len(cols) < 2 or not re.fullmatch(r"0x[0-9a-fA-F]+", cols[0]) or cols[1] in result:
            raise ValueError(f"{path}:{no}: invalid or duplicate Module.symvers entry")
        result[cols[1]] = int(cols[0], 16)
    return result


def read_stock_kernel_exports(path, kernel_info_path):
    document = json.loads(path.read_text())
    kernel_info = json.loads(kernel_info_path.read_text())
    if document.get("schema") != 1 or not isinstance(document.get("exports"), dict):
        raise ValueError("unsupported stock kernel export catalog")
    expected_hash = kernel_info.get("uncompressed_sha256")
    if not expected_hash or document.get("image_sha256") != expected_hash:
        raise ValueError("stock kernel export catalog does not match kernel-info image hash")
    result = {}
    for name, info in document["exports"].items():
        if (not isinstance(name, str) or not name or not isinstance(info, dict)
                or info.get("provider") != "stock-vmlinux"
                or info.get("export") not in ("EXPORT_SYMBOL", "EXPORT_SYMBOL_GPL")
                or isinstance(info.get("crc"), bool) or not isinstance(info.get("crc"), int)
                or not 0 <= info["crc"] <= 0xffffffff):
            raise ValueError(f"invalid stock kernel export entry: {name!r}")
        result[name] = info
    return document["image_sha256"], result



def read_symbol_policy_evidence(document, key):
    item = document.get(key)
    if item is None:
        return None
    if not isinstance(item, dict):
        raise ValueError("invalid symbol policy evidence: " + key)
    if item.get("status") != "verified":
        return None
    names = item.get("names")
    count = item.get("count")
    callsite = item.get("verified_callsite")
    if (not isinstance(names, list) or isinstance(count, bool) or not isinstance(count, int)
            or not 0 < count <= 131072 or count != len(names)
            or not isinstance(callsite, dict) or callsite.get("status") != "verified"):
        raise ValueError("inconsistent verified symbol policy evidence: " + key)
    if (any(not isinstance(name, str) or len(name) > 512
            or re.fullmatch(r"[A-Za-z_.$][A-Za-z0-9_.$]*", name) is None for name in names)
            or names != sorted(set(names))):
        raise ValueError("invalid, duplicate or unsorted symbol policy names: " + key)
    return item



def _der_items(data):
    """Bounded, canonical definite-length DER items; no certificate text output."""
    result, pos = [], 0
    while pos < len(data):
        if pos + 2 > len(data):
            raise ValueError("truncated DER item")
        tag, length = data[pos:pos + 2]
        if tag & 0x1f == 0x1f:
            raise ValueError("unsupported DER high tag number")
        start = pos + 2
        if length & 0x80:
            width = length & 0x7f
            if not 1 <= width <= 4 or start + width > len(data) or data[start] == 0:
                raise ValueError("invalid DER length")
            length = int.from_bytes(data[start:start + width], "big")
            if length < 128:
                raise ValueError("noncanonical DER length")
            start += width
        end = start + length
        if end > len(data):
            raise ValueError("DER item exceeds bounds")
        result.append((tag, data[start:end]))
        pos = end
    return result


def _der_only(data, tag):
    items = _der_items(data)
    if len(items) != 1 or items[0][0] != tag:
        raise ValueError("unexpected DER container")
    return items[0][1]


def read_trusted_public_certificates(document, image_path, image_sha256):
    """Read only the public DER table embedded in this exact stock Image."""
    evidence = document.get("trusted_public_certificates")
    if not image_path or not isinstance(evidence, dict) or evidence.get("status") != "verified":
        return None
    if evidence.get("image_sha256") != image_sha256:
        raise ValueError("public certificate Image identity disagrees with export catalog")
    raw = image_path if isinstance(image_path, bytes) else image_path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != image_sha256:
        raise ValueError("public certificate source Image SHA-256 mismatch")
    records, count = evidence.get("certificates"), evidence.get("count")
    bounds = evidence.get("image_range")
    if (not isinstance(records, list) or isinstance(count, bool) or not isinstance(count, int)
            or not 1 <= count <= 256 or count != len(records)
            or not isinstance(bounds, list) or len(bounds) != 2
            or evidence.get("public_certificates_only") is not True
            or evidence.get("private_key_access") is not False):
        raise ValueError("invalid public certificate evidence")
    try:
        start, end = (int(value, 16) for value in bounds)
    except (TypeError, ValueError):
        raise ValueError("invalid public certificate range") from None
    if not 0 <= start < end <= len(raw) or end - start > 131072:
        raise ValueError("public certificate range is outside exact Image")
    certificates, cursor = [], start
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("invalid public certificate record")
        try:
            offset = int(record.get("image_offset"), 16)
        except (TypeError, ValueError):
            raise ValueError("invalid public certificate offset") from None
        size, expected = record.get("bytes"), record.get("sha256")
        if (isinstance(size, bool) or not isinstance(size, int) or size <= 0
                or offset != cursor or size > end - cursor
                or not isinstance(expected, str) or re.fullmatch("[0-9a-f]{64}", expected) is None):
            raise ValueError("invalid public certificate bounds or fingerprint")
        certificate = raw[offset:offset + size]
        _der_only(certificate, 0x30)
        if hashlib.sha256(certificate).hexdigest() != expected:
            raise ValueError("built-in public certificate SHA-256 mismatch")
        certificates.append(certificate)
        cursor += size
    if cursor != end or len(set(certificates)) != len(certificates):
        raise ValueError("incomplete or duplicate public certificate table")
    return certificates


def module_pkcs7_parts(raw):
    marker = b"~Module signature appended~\n"
    if not raw.endswith(marker):
        return None
    header_end = len(raw) - len(marker)
    if header_end < 12:
        raise ValueError("truncated module signature header")
    header = raw[header_end - 12:header_end]
    # PKCS#7 carries its own signer and hash identifiers. Linux requires these
    # obsolete header fields and the padding to remain zero.
    if header[:8] != bytes((0, 0, 2, 0, 0, 0, 0, 0)):
        raise ValueError("unsupported module PKCS#7 signature header")
    size = int.from_bytes(header[8:], "big")
    payload_end = header_end - 12 - size
    if not 0 < size <= 1048576 or payload_end <= 0:
        raise ValueError("module signature size exceeds payload")
    return raw[:payload_end], raw[payload_end:header_end - 12]


def pkcs7_stock_profile(signature):
    """Require the detached single-signer RSA profile supported by this Image."""
    signed_data_oid = bytes.fromhex("2a864886f70d010702")
    data_oid = bytes.fromhex("2a864886f70d010701")
    rsa_oid = bytes.fromhex("2a864886f70d010101")
    hashes = {
        bytes.fromhex("2b0e03021a"): ("sha1", "CRYPTO_SHA1"),
        bytes.fromhex("608648016503040201"): ("sha256", "CRYPTO_SHA256"),
        bytes.fromhex("608648016503040202"): ("sha384", "CRYPTO_SHA512"),
        bytes.fromhex("608648016503040203"): ("sha512", "CRYPTO_SHA512"),
    }
    def algorithm(item):
        if item[0] != 0x30:
            raise ValueError("invalid PKCS#7 algorithm sequence")
        values = _der_items(item[1])
        if not values or values[0][0] != 6 or values[1:] not in ([], [(5, b"")]):
            raise ValueError("unsupported PKCS#7 algorithm parameters")
        return values[0][1]
    outer = _der_items(_der_only(signature, 0x30))
    if len(outer) != 2 or outer[0] != (6, signed_data_oid) or outer[1][0] != 0xa0:
        raise ValueError("signature is not PKCS#7 SignedData")
    data = _der_items(_der_only(outer[1][1], 0x30))
    if len(data) != 4 or data[0] != (2, b"\x01") or data[1][0] != 0x31:
        raise ValueError("unsupported PKCS#7 SignedData layout")
    digests = _der_items(data[1][1])
    if len(digests) != 1:
        raise ValueError("PKCS#7 requires exactly one digest algorithm")
    digest_oid = algorithm(digests[0])
    if digest_oid not in hashes or data[2] != (0x30, bytes((6, len(data_oid))) + data_oid):
        raise ValueError("unsupported PKCS#7 digest or attached content")
    if data[3][0] != 0x31:
        raise ValueError("invalid PKCS#7 signer set")
    signers = _der_items(data[3][1])
    if len(signers) != 1 or signers[0][0] != 0x30:
        raise ValueError("PKCS#7 requires exactly one signer")
    signer = _der_items(signers[0][1])
    if (len(signer) != 5 or signer[0] != (2, b"\x01") or signer[1][0] != 0x30
            or algorithm(signer[2]) != digest_oid or algorithm(signer[3]) != rsa_oid
            or signer[4][0] != 4 or not signer[4][1]):
        raise ValueError("unsupported PKCS#7 signer, attributes or signature algorithm")
    return {"digest": hashes[digest_oid][0], "digest_config": hashes[digest_oid][1],
            "public_key": "rsa", "public_key_config": "CRYPTO_RSA",
            "detached_content": True, "signers": 1, "embedded_certificates": False}


def verify_module_pkcs7(raw, certificates, stock_config):
    result = {"status": "unsigned", "trusted_for_offline_policy": False,
              "runtime_keyring_execution_verified": False, "private_key_access": False}
    try:
        parts = module_pkcs7_parts(raw)
    except ValueError as error:
        return {**result, "status": "invalid", "reason": str(error)}
    if parts is None:
        return result
    payload, signature = parts
    result.update(signature_bytes=len(signature), signed_payload_bytes=len(payload),
                  signed_payload_sha256=hashlib.sha256(payload).hexdigest(),
                  signed_payload_has_older_signature_trailer=payload.endswith(b"~Module signature appended~\n"))
    try:
        profile = pkcs7_stock_profile(signature)
    except ValueError as error:
        return {**result, "status": "unsupported", "reason": str(error)}
    result["profile"] = profile
    if not certificates:
        return {**result, "status": "not-verified", "reason": "exact Image public certificate table not supplied"}
    required = ("MODULE_SIG", "SYSTEM_TRUSTED_KEYRING", "PKCS7_MESSAGE_PARSER",
                "X509_CERTIFICATE_PARSER", "ASYMMETRIC_KEY_TYPE",
                profile["digest_config"], profile["public_key_config"])
    if any(stock_config.get(name) != "y" for name in required):
        return {**result, "status": "unsupported", "reason": "required signature parser or crypto algorithm is not built in"}
    if any(stock_config.get(name, "n") != "n" for name in ("SYSTEM_BLACKLIST_KEYRING", "SYSTEM_REVOCATION_LIST")):
        return {**result, "status": "not-verified", "reason": "runtime blacklist or revocation policy requires a separate audit"}
    pem = b"".join(b"-----BEGIN CERTIFICATE-----\n" +
                   b"\n".join(base64.b64encode(cert)[i:i + 64]
                              for i in range(0, len(base64.b64encode(cert)), 64)) +
                   b"\n-----END CERTIFICATE-----\n" for cert in certificates)
    fds = []
    try:
        for name, data in (("module-signed-content", payload), ("module-pkcs7", signature),
                           ("exact-image-public-certificates", pem)):
            fd = os.memfd_create(name, os.MFD_CLOEXEC)
            fds.append(fd)
            with os.fdopen(os.dup(fd), "wb") as stream:
                stream.write(data)
            os.lseek(fd, 0, os.SEEK_SET)
        content_path, signature_path, cert_path = ("/proc/self/fd/" + str(fd) for fd in fds)
        # Direct built-in signer trust, not a web/CA chain: -nointern forbids
        # signer lookup in untrusted embedded certificates; -noverify skips CA
        # policy only. Content and public-key signature checks remain enabled.
        command = ["openssl", "cms", "-verify", "-binary", "-inform", "DER",
                   "-in", signature_path, "-content", content_path,
                   "-nointern", "-noverify", "-certfile", cert_path, "-out", os.devnull]
        process = subprocess.run(command, pass_fds=tuple(fds), capture_output=True, timeout=20,
                                 env={**os.environ, "OPENSSL_CONF": os.devnull})
        if process.returncode != 0:
            return {**result, "status": "not-verified", "reason": "PKCS#7 did not verify with the exact built-in public signer",
                    "verifier_exit_code": process.returncode}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {**result, "status": "not-verified", "reason": "PKCS#7 verifier failed: " + type(error).__name__}
    finally:
        for fd in fds:
            os.close(fd)
    return {**result, "status": "verified-with-built-in-public-certificate",
            "trusted_for_offline_policy": True,
            "certificate_sha256": [hashlib.sha256(cert).hexdigest() for cert in certificates],
            "verifier": "OpenSSL CMS detached binary signature; explicit exact-Image signer only",
            "documentation": "https://docs.openssl.org/3.5/man1/openssl-cms/"}


def protected_export_check(exports, signed, evidence, trusted=False):
    """A signature trailer is not sig_ok; only unsigned denial is conclusive."""
    if not isinstance(signed, bool) or not isinstance(trusted, bool) or (trusted and not signed):
        raise ValueError("module signature state must be boolean and internally consistent")
    if trusted:
        return {"status": "clear", "symbols": []}
    if evidence is None:
        return {"status": "unknown", "symbols": []}
    matches = sorted(set(exports) & set(evidence["names"]))
    return {"status": ("clear" if not matches else
                       "signature_trust_required" if signed else "rejected"),
            "symbols": matches}


def vendor_symbol_access_check(symbol, caller_signed, provider_kind, evidence, caller_trusted=False):
    if (not isinstance(caller_signed, bool) or not isinstance(caller_trusted, bool)
            or (caller_trusted and not caller_signed) or provider_kind not in (
            "kernel", "unsigned-module", "signature-trailer", "trusted-module")):
        raise ValueError("invalid caller/provider signature policy state")
    if caller_trusted or provider_kind == "unsigned-module":
        return "clear"
    if evidence is None:
        return "unknown"
    if symbol in evidence["names"]:
        return "clear"
    # A trailer may or may not verify. It cannot establish either sig_ok branch.
    if caller_signed or provider_kind == "signature-trailer":
        return "signature_trust_required"
    return "rejected"


def compare_stock_providers(import_claims, module_exports, kernel_exports):
    providers = {}
    for provider in module_exports:
        for symbol, details in provider["exports"].items():
            providers.setdefault(symbol, []).append({
                "module": provider["module"], "source": provider["source"],
                "sha256": provider["sha256"], "crc": details["crc"],
                "export": details["export"],
                "present_in_feature_roots": provider.get("present_in_feature_roots", False),
                "selected_for_payload": provider.get("selected_for_payload", False),
            })

    counts = {
        "total_import_crc_records": sum(len(row["imports"]) for row in import_claims),
        "unique_imported_symbols": len({symbol for row in import_claims for symbol in row["imports"]}),
        "stock_module_binary_catalog_count": len({(row["module"], row["source"]) for row in module_exports}),
        "stock_module_export_entry_count": sum(len(row["exports"]) for row in module_exports),
        "stock_kernel_provider_records": 0,
        "stock_kernel_crc_matches": 0,
        "stock_kernel_crc_mismatches": 0,
        "stock_module_provider_comparisons": 0,
        "stock_module_provider_crc_matches": 0,
        "stock_module_provider_crc_mismatches": 0,
        "selected_module_export_candidate_comparisons": 0,
        "selected_module_export_candidate_crc_matches": 0,
        "selected_module_export_candidate_crc_mismatches": 0,
        "missing_declared_dependency_import_records": 0,
        "module_provider_ambiguous_import_records": 0,
        "missing_module_provider_payload_import_records": 0,
        "unresolved_import_records": 0,
    }
    kernel_mismatches, module_mismatches = [], []
    unresolved, missing_dependencies, ambiguities, missing_payload_providers = {}, [], [], []
    for importer in import_claims:
        for symbol, imported_crc in importer["imports"].items():
            source_ref = {"module": importer["module"], "source": importer["source"],
                          "crc": f"0x{imported_crc:08x}"}
            kernel_provider = kernel_exports.get(symbol)
            if kernel_provider is not None:
                counts["stock_kernel_provider_records"] += 1
                if kernel_provider["crc"] == imported_crc:
                    counts["stock_kernel_crc_matches"] += 1
                else:
                    counts["stock_kernel_crc_mismatches"] += 1
                    kernel_mismatches.append({
                        "symbol": symbol, "importer": source_ref,
                        "import_crc": f"0x{imported_crc:08x}",
                        "provider_crc": f"0x{kernel_provider['crc']:08x}",
                    })
                continue

            all_candidates = [item for item in providers.get(symbol, [])
                              if item["module"] != importer["module"]]
            dependencies = set(importer["depends"])
            direct = [item for item in all_candidates if item["module"] in dependencies]
            matches = [item for item in all_candidates if item["crc"] == imported_crc]
            mismatching = [item for item in all_candidates if item["crc"] != imported_crc]
            if matches and not any(item["selected_for_payload"] for item in matches):
                counts["missing_module_provider_payload_import_records"] += 1
                missing_payload_providers.append({
                    "symbol": symbol, "importer": source_ref,
                    "matching_stock_providers_not_in_payload": [
                        {"module": item["module"], "source": item["source"],
                         "sha256": item["sha256"], "crc": f"0x{item['crc']:08x}"}
                        for item in matches
                    ],
                })
            counts["selected_module_export_candidate_comparisons"] += len(all_candidates)
            counts["selected_module_export_candidate_crc_matches"] += len(matches)
            counts["selected_module_export_candidate_crc_mismatches"] += len(mismatching)

            if direct:
                counts["stock_module_provider_comparisons"] += len(direct)
                direct_mismatching = [item for item in direct if item["crc"] != imported_crc]
                if not direct_mismatching:
                    counts["stock_module_provider_crc_matches"] += 1
                else:
                    counts["stock_module_provider_crc_mismatches"] += 1
                    module_mismatches.append({
                        "symbol": symbol, "importer": source_ref,
                        "provider_crcs": [{"module": item["module"], "source": item["source"],
                                           "crc": f"0x{item['crc']:08x}"} for item in direct],
                    })
                providers_by_binary = {(item["module"], item["sha256"]) for item in direct}
                if len(providers_by_binary) > 1:
                    counts["module_provider_ambiguous_import_records"] += 1
                    ambiguities.append({
                        "symbol": symbol, "importer": source_ref,
                        "providers": [{"module": item["module"], "source": item["source"],
                                       "sha256": item["sha256"], "crc": f"0x{item['crc']:08x}",
                                       "present_in_feature_roots": item["present_in_feature_roots"],
                                       "selected_for_payload": item["selected_for_payload"]}
                                      for item in direct],
                    })
                continue

            if matches:
                counts["missing_declared_dependency_import_records"] += 1
                providers_by_binary = {(item["module"], item["sha256"]) for item in matches}
                if len(providers_by_binary) > 1:
                    counts["module_provider_ambiguous_import_records"] += 1
                    ambiguities.append({
                        "symbol": symbol, "importer": source_ref,
                        "providers": [{"module": item["module"], "source": item["source"],
                                       "sha256": item["sha256"], "crc": f"0x{item['crc']:08x}",
                                       "present_in_feature_roots": item["present_in_feature_roots"],
                                       "selected_for_payload": item["selected_for_payload"]}
                                      for item in matches],
                    })
                missing_dependencies.append({
                    "symbol": symbol, "importer": source_ref,
                    "matching_providers_not_declared_as_dependencies": [
                        {"module": item["module"], "source": item["source"],
                         "sha256": item["sha256"], "crc": f"0x{item['crc']:08x}",
                         "present_in_feature_roots": item["present_in_feature_roots"],
                                       "selected_for_payload": item["selected_for_payload"]}
                        for item in matches
                    ],
                })
                continue

            counts["unresolved_import_records"] += 1
            unresolved.setdefault(symbol, []).append({
                **source_ref, "declared_dependencies": sorted(dependencies),
                "selected_module_export_candidates": [
                    {"module": item["module"], "source": item["source"],
                     "crc": f"0x{item['crc']:08x}", "matches_import": False}
                    for item in all_candidates
                ],
            })

    no_crc_issues = (counts["stock_kernel_crc_mismatches"] == 0
                     and counts["stock_module_provider_crc_mismatches"] == 0
                     and counts["selected_module_export_candidate_crc_mismatches"] == 0)
    complete = (counts["unresolved_import_records"] == 0
                and counts["missing_module_provider_payload_import_records"] == 0)
    return {
        "status": ("crc_match" if no_crc_issues and complete and not missing_dependencies and not ambiguities
                   else "crc_match_with_dependency_or_variant_gaps" if no_crc_issues and complete
                   else "issues"),
        "crc_status": "all compared provider CRCs match" if no_crc_issues else "provider CRC mismatch detected",
        "provider_set_scope": "Exact stock kernel exports plus all 429 exact stock module export catalogs; direct module providers follow .modinfo depends, with matching undeclared candidates reported separately.",
        "counts": counts,
        "stock_kernel_crc_mismatches": kernel_mismatches,
        "stock_module_provider_crc_mismatches": module_mismatches,
        "matching_module_providers_not_declared_as_dependencies": missing_dependencies,
        "ambiguous_module_provider_imports": ambiguities,
        "matching_module_providers_not_in_payload": missing_payload_providers,
        "unresolved_symbols": [
            {"symbol": symbol, "importers": claimants}
            for symbol, claimants in sorted(unresolved.items())
        ],
    }

def compare_modversions(imports, exports):
    missing = sorted(set(imports) - exports.keys())
    mismatches = sorted(name for name, crc in imports.items()
                        if name in exports and exports[name] != crc)
    return {"imports": len(imports), "missing_exports": missing, "crc_mismatches": mismatches,
            "status": "match" if not missing and not mismatches else "mismatch"}


def topological_order(names, dependencies):
    selected, visiting, done, ordered = set(names), set(), set(), []

    def visit(name):
        if name in visiting:
            raise ValueError(f"module dependency cycle at {name}")
        if name in done:
            return
        visiting.add(name)
        for dep in dependencies.get(name, []):
            if dep in selected:
                visit(dep)
        visiting.remove(name)
        done.add(name)
        ordered.append(name)

    for name in sorted(selected):
        visit(name)
    return ordered


def cpio_newc(entries):
    output, inode, directories = bytearray(), 1, set()
    for name, _, _ in entries:
        parts = name.split("/")[:-1]
        directories.update("/".join(parts[:i]) for i in range(1, len(parts) + 1))
    all_entries = [(name, 0o040755, b"") for name in sorted(directories)] + entries
    seen = set()
    for name, mode, content in all_entries:
        if name in seen:
            raise ValueError(f"duplicate CPIO member {name}")
        seen.add(name)
        encoded = name.encode("utf-8") + b"\0"
        fields = (inode, mode, 0, 0, 1, 0, len(content), 0, 0, 0, 0, len(encoded), 0)
        output.extend(b"070701" + b"".join(f"{value:08x}".encode("ascii") for value in fields))
        output.extend(encoded)
        output.extend(b"\0" * (-len(output) % 4))
        output.extend(content)
        output.extend(b"\0" * (-len(output) % 4))
        inode += 1
    name = b"TRAILER!!!\0"
    fields = (inode, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0)
    output.extend(b"070701" + b"".join(f"{value:08x}".encode("ascii") for value in fields))
    output.extend(name)
    output.extend(b"\0" * (-len(output) % 4))
    return bytes(output)


def read_newc(data):
    pos, members = 0, {}
    while True:
        if pos + 110 > len(data) or data[pos:pos + 6] != b"070701":
            raise ValueError("invalid or truncated newc archive header")
        fields = [int(data[pos + 6 + i * 8:pos + 14 + i * 8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        if namesize < 1:
            raise ValueError("empty CPIO filename")
        pos += 110
        if pos + namesize > len(data) or data[pos + namesize - 1] != 0:
            raise ValueError("truncated or unterminated CPIO filename")
        name = data[pos:pos + namesize - 1].decode("utf-8")
        pos = (pos + namesize + 3) & ~3
        if pos + size > len(data):
            raise ValueError(f"truncated CPIO content: {name}")
        content = data[pos:pos + size]
        pos = (pos + size + 3) & ~3
        if name == "TRAILER!!!":
            if any(data[pos:]):
                raise ValueError("nonzero data after CPIO trailer")
            return members
        if name in members:
            raise ValueError(f"duplicate CPIO member {name}")
        members[name] = {"mode": fields[1], "data": content}


def source_dir(source):
    return re.sub(r"[^A-Za-z0-9_.-]", "_", source)


def audit(a):
    root = a.workspace.resolve()
    input_paths = [a.manifest, a.stock_modules, a.hardware, a.stock_config]
    image_path = getattr(a, "stock_kernel_image", None)
    optional_paths = [a.candidate_config, a.module_symvers, a.stock_kernel_exports, a.kernel_info, a.report, a.package_output, image_path]
    if any(not path.resolve().is_relative_to(root) for path in input_paths + [p for p in optional_paths if p]):
        raise ValueError("an input or output path is outside the Teclast workspace")
    if a.report and a.package_output and a.report.resolve() == a.package_output.resolve():
        raise ValueError("report and archive paths must differ")

    manifest, stock, hw = [json.loads(p.read_text()) for p in input_paths[:3]]
    if manifest.get("schema") != 1 or manifest.get("device_access") is not False:
        raise ValueError("unsupported or unsafe extraction manifest")
    features = stock["dependency_analysis"]["features"]
    groups = a.feature or sorted(features)
    if set(groups) - features.keys():
        raise ValueError("unknown feature group")
    feature_closure = sorted({name for group in groups for name in features[group]["declared_dependency_closure"]})
    extra_roots = sorted(set(a.extra_module or []))
    closure = sorted(set(feature_closure) | set(extra_roots))
    roots = sorted({name for group in groups for name in hw["groups"].get(group, [])})
    runtime_names = set(hw["modules"])
    catalog = module_catalog(stock)
    stock_module_export_catalog, stock_module_catalog_export_errors = [], []
    for module_name, sources in catalog.items():
        for source, info in sources.items():
            export_info = info.get("exported_symbol_info")
            if not isinstance(export_info, dict):
                stock_module_catalog_export_errors.append(f"{module_name}@{source}: export metadata absent")
                continue
            if info.get("export_integrity_errors"):
                stock_module_catalog_export_errors.extend(
                    f"{module_name}@{source}: {error}" for error in info["export_integrity_errors"])
                continue
            exports = {}
            for symbol, details in export_info.items():
                if (not isinstance(details, dict) or isinstance(details.get("crc"), bool)
                        or not isinstance(details.get("crc"), int)
                        or not 0 <= details["crc"] <= 0xffffffff
                        or details.get("export") not in ("EXPORT_SYMBOL", "EXPORT_SYMBOL_GPL")):
                    stock_module_catalog_export_errors.append(
                        f"{module_name}@{source}: invalid export metadata for {symbol}")
                    continue
                exports[symbol] = {"crc": details["crc"], "export": details["export"],
                                   "namespace": details.get("namespace")}
            stock_module_export_catalog.append({
                "module": module_name, "source": source, "sha256": info.get("sha256"),
                "present_in_feature_roots": bool(info.get("feature_selected")),
                "selected_for_payload": module_name in closure,
                "exports": exports,
            })

    binaries = {}
    for item in manifest.get("modules", []):
        name, path = item.get("name"), Path(item.get("path", ""))
        if not isinstance(name, str) or not path.is_file() or not path.resolve().is_relative_to(root):
            raise ValueError("invalid extracted module path or name")
        binaries.setdefault(name, []).append(item)

    distinct_variants = {}
    feature_plan = {}
    static_variant_names = set(stock.get("dependency_analysis", {}).get(
        "different_binary_variants_requiring_selection", {}))
    for name, options in binaries.items():
        hashes = {option.get("sha256") for option in options}
        if len(hashes) > 1:
            distinct_variants[name] = sorted(hashes)
    for group, details in features.items():
        members = sorted(set(details.get("declared_dependency_closure", [])))
        feature_plan[group] = {
            "roots": sorted(hw.get("groups", {}).get(group, [])),
            "closure_count": len(members),
            "extracted_names": sum(bool(binaries.get(name)) for name in members),
            "missing_modules": [name for name in members if not binaries.get(name)],
            "ambiguous_variants": [name for name in members if name in distinct_variants],
            "runtime_loaded_name_count": sum(name in runtime_names for name in members),
            "unresolved_declared_dependencies": details.get("unresolved_names", []),
        }

    symvers = read_symvers(a.module_symvers) if a.module_symvers else None
    if bool(a.stock_kernel_exports) != bool(a.kernel_info):
        raise ValueError("--stock-kernel-exports and --kernel-info must be supplied together")
    if a.stock_kernel_exports:
        stock_kernel_image_sha256, kernel_exports = read_stock_kernel_exports(
            a.stock_kernel_exports, a.kernel_info)
    else:
        stock_kernel_image_sha256, kernel_exports = None, None
    protection_document = json.loads(a.stock_kernel_exports.read_text()) if a.stock_kernel_exports else {}
    protected_evidence = read_symbol_policy_evidence(protection_document, "protected_symbol_exports_evidence")
    vendor_access_evidence = read_symbol_policy_evidence(protection_document, "vendor_symbol_access_evidence")
    candidate_config = config(a.candidate_config) if a.candidate_config else {}
    stock_config = config(a.stock_config)
    certificates = read_trusted_public_certificates(protection_document, image_path, stock_kernel_image_sha256)
    extended_required = stock_config.get("EXTENDED_MODVERSIONS") == "y"
    verified = {}
    rows, missing, payload_errors = [], [], []
    all_vermagics, crc_claims, firmware_refs = set(), {}, set()
    import_claims, module_export_claims = [], []

    for name in closure:
        options = binaries.get(name, [])
        if not options:
            missing.append(name)
            rows.append({"name": name, "status": "missing"})
            continue
        by_hash = {}
        for item in options:
            path = Path(item["path"]).resolve()
            data = path.read_bytes()
            digest = hashlib.sha256(data).hexdigest()
            problems = []
            if len(data) != item.get("bytes") or digest != item.get("sha256"):
                problems.append("manifest size/SHA-256 mismatch")
            source = item.get("source")
            expected = catalog.get(name, {}).get(source)
            if not expected or expected.get("sha256") != digest:
                problems.append("binary does not match selected stock source metadata")
            if expected and Path(item.get("member", "")).name != expected.get("filename"):
                problems.append("member filename does not match stock source metadata")
            try:
                parsed = elf_module(data)
            except (ValueError, UnicodeError, struct.error) as error:
                parsed = None
                problems.append(str(error))
            row = {
                "name": name, "source": source, "bytes": len(data), "sha256": digest,
                "catalog_match": bool(expected and expected.get("sha256") == digest),
                "listed_loaded_in_stock_android": bool(item.get("loaded_on_stock_android") and name in runtime_names),
                "runtime_hash_match": None, "status": "verified" if not problems else "invalid",
            }
            if parsed:
                for dependency in parsed["depends"]:
                    if dependency in catalog and dependency not in closure:
                        problems.append(f"module dependency {dependency} is outside the selected closure")
                row.update({key: parsed[key] for key in (
                    "name", "vermagic", "depends", "firmware", "modversion_format",
                    "modversion_status", "modversion_count", "legacy_modversion_count",
                    "extended_legacy_match", "exported_symbol_count", "export_integrity_errors",
                    "license", "import_ns", "signed")})
                row["signature_verification"] = verify_module_pkcs7(data, certificates, stock_config)
                import_claims.append({"module": name, "source": source, "sha256": digest,
                                      "depends": parsed["depends"], "imports": parsed["modversions"],
                                      "license": parsed["license"], "import_ns": parsed["import_ns"]})
                module_export_claims.append({"module": name, "source": source, "sha256": digest,
                                             "exports": parsed["exported_symbol_info"]})
                if parsed["name"] not in (None, name):
                    problems.append(".modinfo module name mismatch")
                if not parsed["vermagic"]:
                    problems.append("missing module vermagic")
                else:
                    all_vermagics.add(parsed["vermagic"])
                if parsed["modversion_status"] != "parsed":
                    problems.append("module version CRC sections are missing or inconsistent")
                if parsed["export_integrity_errors"]:
                    problems.extend(parsed["export_integrity_errors"])
                if extended_required and parsed["modversion_format"] != "extended":
                    problems.append("stock config requires extended module version sections")
                for symbol, crc in parsed["modversions"].items():
                    crc_claims.setdefault(symbol, []).append({"module": name, "source": source, "crc": crc})
                firmware_refs.update(parsed["firmware"])
                if symvers is not None:
                    check = compare_modversions(parsed["modversions"], symvers)
                    row["candidate_crc_check"] = check
                    if check["status"] != "match":
                        problems.append("candidate Module.symvers import CRC mismatch")
            if problems:
                row["status"], row["problems"] = "invalid", problems
                payload_errors.append(f"{name}@{source}: {'; '.join(problems)}")
            rows.append(row)
            if parsed and not problems:
                by_hash.setdefault(digest, (item, data, parsed, []))[3].append(source)
        if by_hash:
            verified[name] = list(by_hash.values())

    conflicts = []
    for symbol, claims in sorted(crc_claims.items()):
        values = sorted({claim["crc"] for claim in claims})
        if len(values) > 1:
            conflicts.append({"symbol": symbol, "crcs": [f"0x{x:x}" for x in values],
                              "modules": sorted({f"{c['module']}@{c['source']}" for c in claims})})
    closure_conflicts = [item for item in conflicts
                         if any(c["module"] in closure for c in crc_claims.get(item["symbol"], []))]
    prefixes = sorted({value.split(" ", 1)[0] for value in all_vermagics})
    tails = sorted({value.split(" ", 1)[1] if " " in value else "" for value in all_vermagics})
    common_vermagic = len(all_vermagics) == 1
    common_vermagic_tail = len(tails) == 1
    stock_vermagic = next(iter(all_vermagics), None) if common_vermagic else None
    stock_release = prefixes[0] if len(prefixes) == 1 else None
    stock_tail = tails[0] if common_vermagic_tail else None
    candidate_release = a.kernel_release
    candidate_vermagic = a.candidate_vermagic
    candidate_tail = candidate_vermagic.split(" ", 1)[1] if " " in (candidate_vermagic or "") else None
    all_have_crcs = bool(rows) and all(
        row.get("modversion_status") == "parsed" and row.get("modversion_count", 0) > 0
        for row in rows if row.get("vermagic")
    )
    candidate_release_prefix_matches = (None if not candidate_release else candidate_release in prefixes)
    candidate_release_prefix_advisory = bool(
        candidate_release and not candidate_release_prefix_matches
        and candidate_config.get("MODVERSIONS") == "y" and all_have_crcs)
    if not candidate_release:
        release_status = "not supplied"
    elif candidate_release_prefix_matches:
        release_status = "matches a stock vermagic release prefix"
    elif candidate_release_prefix_advisory:
        release_status = "different prefix; release field is advisory when module CRCs are present"
    else:
        release_status = "different prefix; CRC-based compatibility is not established"
    if candidate_tail is None:
        tail_status = "not supplied"
    elif not common_vermagic_tail:
        tail_status = "stock module vermagic tails differ"
    else:
        tail_status = "match" if candidate_tail == stock_tail else "mismatch"

    candidate_config_status = {key: candidate_config.get(key, "not supplied")
                               for key in ("MODULES", "MODVERSIONS", "MODULE_SIG_FORCE")}
    crc_status = "not supplied"
    candidate_checks = [row.get("candidate_crc_check", {}).get("status") for row in rows
                        if row.get("candidate_crc_check")]
    if symvers is not None:
        crc_status = "match" if candidate_checks and all(x == "match" for x in candidate_checks) else "mismatch"
    cross_status = "conflicts" if closure_conflicts else "consistent"
    stock_provider_audit = (compare_stock_providers(import_claims, stock_module_export_catalog, kernel_exports)
                            if kernel_exports is not None else None)
    module_dependency_catalog_gaps = [
        {"module": row["name"], "source": row["source"], "dependency": dependency,
         "reason": "no exact normalized module name in the stock module catalog"}
        for row in rows if row.get("depends")
        for dependency in row["depends"] if dependency not in catalog
    ]
    provider_payload_gaps = (stock_provider_audit["counts"]["missing_module_provider_payload_import_records"]
                             if stock_provider_audit is not None else 0)
    payload_ready = not missing and not payload_errors and not provider_payload_gaps
    candidate_abi_ready = (
        payload_ready and not distinct_variants and not closure_conflicts and common_vermagic_tail
        and candidate_config_status["MODULES"] == "y"
        and candidate_config_status["MODVERSIONS"] == "y"
        and candidate_config_status["MODULE_SIG_FORCE"] != "y"
        and crc_status == "match" and tail_status == "match"
    )

    total_import_records = sum(len(claims) for claims in crc_claims.values())
    internal_crc_comparisons = sum(len(claims) * (len(claims) - 1) // 2 for claims in crc_claims.values())
    version_rows = [row for row in rows if row.get("vermagic")]
    candidate_import_comparisons = sum(
        row.get("candidate_crc_check", {}).get("imports", 0) for row in rows
    )
    candidate_unique_symbols = (len(set(crc_claims) & symvers.keys()) if symvers is not None else 0)
    vermagic_variants = []
    for value in sorted(all_vermagics):
        vermagic_variants.append({
            "vermagic": value, "release_prefix": value.split(" ", 1)[0],
            "nonrelease_tail": value.split(" ", 1)[1] if " " in value else "",
            "module_binary_count": sum(row.get("vermagic") == value for row in rows),
        })
    module_policy_unknowns = [
        item for item in [
            ("exact runtime protected-export table was not verified"
             if stock_config.get("MODULE_SIG_PROTECT") == "y" and protected_evidence is None else None),
            ("exact unsigned-module symbol-access table was not verified"
             if stock_config.get("MODULE_SIG_PROTECT") == "y" and vendor_access_evidence is None else None),
        ] if item
    ]
    module_signature_unknowns = [
        {"module": row["name"], "source": row["source"], "sha256": row["sha256"],
         "reason": row.get("signature_verification", {}).get("reason", "signature was not verified")}
        for row in rows if row.get("signed") and
        not row.get("signature_verification", {}).get("trusted_for_offline_policy", False)
    ]
    if stock_config.get("MODULE_SIG_FORCE") == "y" and any(not row.get("signed") for row in rows):
        module_policy_unknowns.append("CONFIG_MODULE_SIG_FORCE rejects unsigned modules in the payload")
    module_load_policy = {
        "policy_ready": not module_policy_unknowns and not module_signature_unknowns,
        "shared_policy_ready": not module_policy_unknowns,
        "shared_unknowns": module_policy_unknowns,
        "module_signature_unknowns": module_signature_unknowns,
        "exact_provider_gpl_namespace_checks": "performed by early-userspace load plans using exact CRC providers",
        "symbol_protection": {
            "protected_symbol_exports_evidence": protected_evidence,
            "vendor_symbol_access_evidence": vendor_access_evidence,
            "enforcement": "early-userspace rechecks exact CPIO exports and resolved imports; an unsigned kernel import outside the verified runtime whitelist is rejected",
            "signature_trailer_is_sig_ok": False,
        },
        "license": {
            "gpl_compatible_values": sorted(GPL_COMPATIBLE_LICENSES),
            "duplicate_tag_evidence": "Android common kernel/module/main.c uses get_modinfo(info, \"license\") for module taint and GPL-only eligibility; the exact device loader source was not included in kernel-reference",
            "mixed_compatibility_duplicate_tags": "unknown; not treated as confirmed rejection",
        },
        "namespace": {
            "allow_missing_imports": stock_config.get("MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS", "n"),
            "missing_import_behavior": (
                "allowed with kernel warning" if stock_config.get("MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS") == "y"
                else "rejected" if stock_config.get("MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS", "n") == "n"
                else "unknown"),
            "module_namespace_grammar": "module:<comma-separated module names>; a trailing * wildcard per name is supported; other forms are unsupported",
        },
        "signature": {
            "force": stock_config.get("MODULE_SIG_FORCE", "n"),
            "protect": stock_config.get("MODULE_SIG_PROTECT", "n"),
            "protected_name_list_configured": bool(stock_config.get("MODULE_SIG_PROTECT_LIST")),
            "signing_key_configured": bool(stock_config.get("MODULE_SIG_KEY")),
            "unsigned_forced_rejections": [
                {"module": row["name"], "source": row["source"]}
                for row in rows if stock_config.get("MODULE_SIG_FORCE") == "y" and not row.get("signed")
            ],
            "trailer_marker_is_cryptographic_proof": False,
        },
        "kmi_whitelist": {
            "trim_unused_ksyms": stock_config.get("TRIM_UNUSED_KSYMS", "n"),
            "whitelist_reference": stock_config.get("UNUSED_KSYMS_WHITELIST", "not supplied"),
            "whitelist_contents_status": "build input not recovered; actual Image exports and CRCs are checked",
            "runtime_export_table": "verified" if kernel_exports is not None else "not supplied",
            "runtime_vendor_access_table": "verified" if vendor_access_evidence is not None else "not supplied",
            "build_input_is_separate_runtime_requirement": False,
        },
        "unknowns": module_policy_unknowns + (
            ["PKCS#7 trust was not verified for every module variant; feature plans select and check exact hashes"]
            if module_signature_unknowns else []),
    }

    report = {
        "schema": 1, "scope": "Offline stock binary audit and module-only archive; no loading or device test",
        "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "device_access": False, "requested_features": groups, "feature_roots": roots,
        "feature_closure": feature_closure, "feature_module_count": len(feature_closure),
        "additional_module_roots": extra_roots, "module_closure": closure,
        "module_count": len(closure),
        "extracted_name_count": len(verified), "extracted_binary_count": sum(len(v) for v in verified.values()),
        "module_population": {
            "selected_module_names": len(closure),
            "selected_original_sources": sum("source" in row for row in rows),
            "verified_original_sources": sum(len(sources) for variants in verified.values()
                                               for _, _, _, sources in variants),
            "verified_unique_binaries": sum(len(variants) for variants in verified.values()),
            "same_content_extra_sources": sum(len(sources) - 1 for variants in verified.values()
                                                for _, _, _, sources in variants),
            "unique_binary_identity": "module name and SHA-256; original source copies are retained separately",
        },
        "missing_modules": missing, "payload_integrity_errors": payload_errors,
        "variant_selection_pending": [
            {"name": name, "sha256s": hashes, "sources": sorted({row["source"] for row in rows
             if row["name"] == name})} for name, hashes in sorted(distinct_variants.items()) if name in closure
        ],
        "selected_modules": rows, "all_hardware_feature_plans": feature_plan,
        "stock_module_vermagic": stock_vermagic, "stock_module_release": stock_release,
        "stock_module_release_prefixes": prefixes,
        "stock_module_vermagic_variants": vermagic_variants,
        "stock_module_vermagic_tails_match": common_vermagic_tail,
        "stock_config_extended_modversions": extended_required,
        "module_version_section_audit": {
            "binary_count": len(version_rows),
            "count_scope": "selected original source rows; identical content may occur in multiple sources",
            "unique_binary_count": len({(row["name"], row["sha256"]) for row in version_rows}),
            "extended_section_count": sum(row.get("modversion_format") == "extended" for row in version_rows),
            "legacy_only_count": sum(row.get("modversion_format") == "legacy" for row in version_rows),
            "extended_legacy_match_count": sum(row.get("extended_legacy_match") is True for row in version_rows),
            "bad_or_missing_version_count": sum(row.get("modversion_status") != "parsed" for row in version_rows),
        },
        "candidate_kernel_release": candidate_release, "candidate_release_status": release_status,
        "candidate_release_prefix_matches_stock": candidate_release_prefix_matches,
        "candidate_release_prefix_advisory_with_module_crcs": candidate_release_prefix_advisory,
        "candidate_module_import_crcs_parsed": all_have_crcs,
        "candidate_kernel_vermagic": candidate_vermagic,
        "candidate_nonrelease_vermagic_status": tail_status,
        "candidate_config": candidate_config_status,
        "candidate_module_symvers_status": crc_status,
        "stock_kernel_exports": {
            "status": "compared" if stock_provider_audit is not None else "not supplied",
            "image_sha256": stock_kernel_image_sha256,
            "export_count": len(kernel_exports) if kernel_exports is not None else 0,
            "provider_crc_audit": stock_provider_audit,
        },
        "stock_module_export_catalog": {
            "module_binary_count": len(stock_module_export_catalog),
            "export_entry_count": sum(len(row["exports"]) for row in stock_module_export_catalog),
            "integrity_error_count": len(stock_module_catalog_export_errors),
            "integrity_errors": stock_module_catalog_export_errors,
            "scope": "All module binaries cataloged from stock vendor_dlkm, system_dlkm, odm_dlkm, and vendor_boot metadata; no module contents were copied for this index.",
        },
        "module_export_integrity": {
            "parsed_binary_count": sum(row.get("exported_symbol_count") is not None for row in rows),
            "exported_symbol_count": sum(row.get("exported_symbol_count", 0) for row in rows),
            "integrity_error_count": sum(len(row.get("export_integrity_errors", [])) for row in rows),
        },
        "imported_crc_consistency": {
            "status": cross_status,
            "unique_imported_symbols": len(crc_claims),
            "total_import_crc_records": total_import_records,
            "pairwise_internal_crc_comparisons": internal_crc_comparisons,
            "conflict_count": len(closure_conflicts), "conflicts": closure_conflicts,
            "candidate_kernel_export_comparisons": candidate_import_comparisons,
            "candidate_unique_export_symbols_compared": candidate_unique_symbols,
            "candidate_comparison_status": crc_status,
        },
        "module_signatures": {
            "inspected_binary_count": sum(len(v) for v in verified.values()),
            "with_signature_trailer": sum(parsed["signed"] for variants in verified.values()
                                          for _, _, parsed, _ in variants),
            "trailer_marker_only": False,
            "cryptographic_signature_verification": "OpenSSL CMS; exact Image built-in public certificate only",
            "verified_with_builtin_public_certificate": sum(
                row.get("signature_verification", {}).get("trusted_for_offline_policy", False) for row in rows),
            "verification_status_counts": dict(Counter(
                row.get("signature_verification", {}).get("status", "unparsed") for row in rows)),
            "private_key_access": False,
            "runtime_keyring_execution_verified": False,
            "public_certificate_source": {
                "exact_image_sha256": stock_kernel_image_sha256,
                "status": "verified" if certificates else "not supplied",
                "certificate_sha256": [hashlib.sha256(cert).hexdigest() for cert in certificates or []],
            },
        },
        "module_metadata_policy_inputs": {
            "license_value_counts": dict(Counter(value for row in rows for value in row.get("license", []))),
            "import_namespace_value_counts": dict(Counter(value for row in rows for value in row.get("import_ns", []))),
            "missing_license_binary_count": sum("license" in row and not row["license"] for row in rows),
            "duplicate_license_tag_binary_count": sum(len(row.get("license", [])) > 1 for row in rows),
            "duplicate_import_ns_tag_binary_count": sum(
                len(row.get("import_ns", [])) > len(set(row.get("import_ns", []))) for row in rows),
            "export_namespace_value_counts": dict(Counter(
                details.get("namespace")
                for variants in verified.values()
                for _, _, parsed, _ in variants
                for details in parsed["exported_symbol_info"].values()
                if isinstance(details.get("namespace"), str))),
            "gpl_compatible_license_values": sorted(GPL_COMPATIBLE_LICENSES),
            "license_policy_source": "Linux license_is_gpl_compatible() accepted exact stock-kernel license strings",
        },
        "firmware_references": sorted(firmware_refs),
        "firmware_reference_interpretation": (
            "These are .modinfo MODULE_FIRMWARE declarations; they do not by themselves prove an unconditional runtime requirement."
        ),
        "firmware_payload_availability": "unknown outside the searched stock locations; no payload included",
        "firmware_path_search": {
            "status": "no matching basenames in searched locations",
            "searched_locations": [
                {"partition": "vendor_a", "result": "not found"},
                {"partition": "system_a", "result": "not found"},
                {"partition": "product_a", "result": "not found"},
                {"partition": "vendor_boot_a", "fragment": "normal-boot ramdisk", "result": "not found"},
            ],
            "not_searched": ["vendor_boot_a recovery-only ramdisk fragment"],
            "searched_basenames": sorted(firmware_refs),
            "method": "Read-only metadata/name search against the original backup; EROFS direct offsets, normal ramdisk CPIO names in memory, no partition image copies or firmware extraction.",
            "interpretation_note": "cfg80211 built-in regdb and optional firmware request behavior was not verified from source code.",
        },
        "firmware_payloads_included": False,
        "stock_kernel_config": {key: stock_config.get(key, "n") for key in
                                ("MODULES", "MODVERSIONS", "MODULE_SIG_FORCE", "MODULE_SIG_PROTECT",
                                 "MODULE_SIG_PROTECT_LIST", "MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS",
                                 "TRIM_UNUSED_KSYMS", "UNUSED_KSYMS_WHITELIST", "DEVTMPFS", "ACPI",
                                 "MODULE_SIG", "SYSTEM_TRUSTED_KEYRING", "PKCS7_MESSAGE_PARSER",
                                 "X509_CERTIFICATE_PARSER", "ASYMMETRIC_KEY_TYPE", "CRYPTO_RSA",
                                 "CRYPTO_SHA1", "CRYPTO_SHA256", "CRYPTO_SHA512",
                                 "SYSTEM_BLACKLIST_KEYRING", "SYSTEM_REVOCATION_LIST")},
        "module_load_policy": module_load_policy,
        "runtime_module_hashes_available": False,
        "module_payload_complete": payload_ready,
        "candidate_kernel_abi_ready": candidate_abi_ready,
        "autoload_files_included": False,
        "bootable_initramfs": False,
        "package_output_written": False,
        "limitations": [
            "Stock Android's observed module names do not identify the exact file hash loaded at runtime.",
            "The stock config has DEVTMPFS=n and ACPI=n; a Linux init needs explicit /dev setup and cannot assume ACPI discovery.",
            "Imported CRC consistency between modules does not prove those CRCs match candidate kernel exports.",
            "A different vermagic release prefix alone is not treated as incompatibility when CONFIG_MODVERSIONS CRCs are present; nonrelease vermagic and every exported symbol CRC still need a candidate comparison.",
            "The module-only archive has no /init, automatic-load list, Linux root filesystem, kernel, DTB, boot command line, firmware payloads, or UFS power/clock setup.",
            "Static dependency closure and archive read-back do not prove driver hardware operation.",
        ],
    }

    if a.package_output:
        if not payload_ready:
            raise ValueError("refusing archive: stock module closure or extracted binary integrity is incomplete")
        archive_root = f"lib/modules/{stock_release or 'stock-mixed'}"
        archive_entries, expected_files, inventory = [], {}, []
        for name in closure:
            variants = verified.get(name, [])
            different_hashes = len(variants) > 1
            for item, data, parsed, sources in variants:
                filename = Path(item["member"]).name
                if different_hashes:
                    member_path = f"{archive_root}/.stock-variants/{source_dir(item['source'])}/{filename}"
                else:
                    member_path = f"{archive_root}/{filename}"
                if member_path in expected_files:
                    raise ValueError(f"archive path collision: {member_path}")
                archive_entries.append((member_path, 0o100644, data))
                expected_files[member_path] = data
                inventory.append({
                    "name": name, "path": member_path, "sources": sorted(set(sources)),
                    "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data),
                    "depends": parsed["depends"], "firmware": parsed["firmware"],
                    "signed": parsed["signed"], "variant_selection_pending": different_hashes,
                    "runtime_loaded_name": any(source_item.get("loaded_on_stock_android")
                        for source_item in binaries[name] if source_item.get("sha256") == hashlib.sha256(data).hexdigest()),
                    "runtime_hash_match": None,
                })
        inventory_data = json.dumps({
            "schema": 1, "scope": "Stock module payload only; no automatic loading",
            "features": groups, "module_name_count": len(closure),
            "binary_count": len(inventory), "autoload": False,
            "variant_selection_pending": sorted(name for name, variants in verified.items() if len(variants) > 1),
            "modules": inventory,
        }, indent=2).encode() + b"\n"
        inventory_path = f"{archive_root}/stock-binary-inventory.json"
        archive_entries.append((inventory_path, 0o100644, inventory_data))
        expected_files[inventory_path] = inventory_data
        if a.report and a.package_output.resolve() == a.report.resolve():
            raise ValueError("archive and report paths must differ")
        archive = cpio_newc(archive_entries)
        output = a.package_output.resolve()
        if output.exists():
            if output.read_bytes() != archive:
                raise ValueError(f"refusing to overwrite different existing archive: {output}")
        else:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(archive)
        readback = output.read_bytes()
        if hashlib.sha256(readback).digest() != hashlib.sha256(archive).digest():
            raise ValueError("archive read-back SHA-256 mismatch")
        members = read_newc(readback)
        file_members = {name: entry for name, entry in members.items() if entry["mode"] & 0o170000 == 0o100000}
        if set(file_members) != set(expected_files):
            raise ValueError("archive read-back member list mismatch")
        for name, content in expected_files.items():
            if file_members[name]["data"] != content:
                raise ValueError(f"archive read-back content mismatch: {name}")
        report["package_output_written"] = True
        report["package_output"] = str(output)
        report["package_bytes"] = len(readback)
        report["package_sha256"] = hashlib.sha256(readback).hexdigest()
        report["package_readback"] = {
            "status": "all CPIO members and binary payload hashes verified",
            "member_count": len(members), "module_binary_count": len(expected_files) - 1,
            "sha256": report["package_sha256"],
        }
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--workspace", type=Path, required=True)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--stock-modules", type=Path, required=True)
    p.add_argument("--hardware", type=Path, required=True)
    p.add_argument("--stock-config", type=Path, required=True)
    p.add_argument("--feature", action="append", choices=[
        "input", "storage", "cellular", "display", "usb", "power",
        "connectivity", "audio", "sensors", "camera", "sim_radio"])
    p.add_argument("--extra-module", action="append", default=[],
                   help="add an exact extracted stock module as a payload root")
    p.add_argument("--kernel-release", help="candidate UTS_RELEASE; not compared alone when module CRCs exist")
    p.add_argument("--candidate-config", type=Path)
    p.add_argument("--candidate-vermagic", help="full vermagic string from a module built against the candidate kernel")
    p.add_argument("--module-symvers", type=Path)
    p.add_argument("--stock-kernel-exports", type=Path,
                   help="exact built-in exports parsed from the original stock kernel image")
    p.add_argument("--stock-kernel-image", type=Path,
                   help="reuse exact existing stock Image to verify public certificate and module PKCS#7 signatures")
    p.add_argument("--kernel-info", type=Path,
                   help="kernel image identity metadata used to validate the stock export catalog")
    p.add_argument("--package-output", type=Path, help="write a non-bootable CPIO payload; no module loader list")
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args()
    try:
        report = audit(a)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    a.report.parent.mkdir(parents=True, exist_ok=True)
    a.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({
        "features": report["requested_features"], "closure_modules": report["module_count"],
        "extracted_names": report["extracted_name_count"], "extracted_binaries": report["extracted_binary_count"],
        "missing": report["missing_modules"], "payload_integrity_errors": len(report["payload_integrity_errors"]),
        "variant_pending": len(report["variant_selection_pending"]),
        "internal_crc_conflicts": report["imported_crc_consistency"]["conflict_count"],
        "candidate_module_symvers_status": report["candidate_module_symvers_status"],
        "candidate_kernel_abi_ready": report["candidate_kernel_abi_ready"],
        "archive_written": report["package_output_written"], "autoload": report["autoload_files_included"],
        "bootable_initramfs": report["bootable_initramfs"], "report": str(a.report.resolve())
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

