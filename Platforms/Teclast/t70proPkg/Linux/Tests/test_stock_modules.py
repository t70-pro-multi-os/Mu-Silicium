#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
import importlib.util
import copy
import hashlib
import json
from pathlib import Path
import sys
import struct
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[6]
SCRIPT = ROOT / "Mu-Silicium/Platforms/Teclast/t70proPkg/Linux/prepare-stock-modules.py"
spec = importlib.util.spec_from_file_location("stock_modules", SCRIPT)
stock_modules = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stock_modules)


def arguments(**overrides):
    values = {
        "workspace": ROOT,
        "manifest": ROOT / "analysis/stock-binaries/manifest.json",
        "stock_modules": ROOT / "analysis/stock-modules.json",
        "hardware": ROOT / "analysis/runtime-hardware.json",
        "stock_config": ROOT / "analysis/stock-kernel.config",
        "stock_kernel_exports": ROOT / "analysis/stock-kernel-exports.json",
        "kernel_info": ROOT / "analysis/kernel-info.json",
        "feature": ["input", "storage"],
        "extra_module": [],
        "kernel_release": None,
        "candidate_vermagic": None,
        "candidate_config": None,
        "module_symvers": None,
        "package_output": None,
        "report": None,
    }
    values.update(overrides)
    return type("Args", (), values)()


class StockModuleTests(unittest.TestCase):
    def test_real_input_and_storage_closures_are_hash_verified(self):
        report = stock_modules.audit(arguments())
        self.assertEqual(report["module_count"], 20)
        self.assertEqual(report["extracted_name_count"], 20)
        self.assertEqual(report["extracted_binary_count"], 20)
        self.assertEqual(report["missing_modules"], [])
        self.assertEqual(report["payload_integrity_errors"], [])
        self.assertTrue(all(row["catalog_match"] for row in report["selected_modules"]))
        self.assertTrue(all(row["listed_loaded_in_stock_android"] for row in report["selected_modules"]))
        self.assertFalse(report["candidate_kernel_abi_ready"])
        self.assertEqual(report["stock_kernel_exports"]["status"], "compared")
        self.assertEqual(report["stock_kernel_exports"]["provider_crc_audit"]["counts"][
            "stock_kernel_crc_mismatches"], 0)
        self.assertFalse(report["bootable_initramfs"])
        policy = report["module_load_policy"]
        self.assertTrue(policy["policy_ready"])
        self.assertEqual(policy["signature"]["force"], "n")
        self.assertEqual(policy["signature"]["protect"], "y")
        self.assertEqual(policy["unknowns"], [])
        self.assertEqual(policy["symbol_protection"]["protected_symbol_exports_evidence"]["count"], 590)
        self.assertEqual(policy["symbol_protection"]["vendor_symbol_access_evidence"]["count"], 9176)
        self.assertFalse(policy["kmi_whitelist"]["build_input_is_separate_runtime_requirement"])

    def test_missing_exact_policy_tables_do_not_claim_readiness(self):
        report = stock_modules.audit(arguments(stock_kernel_exports=None, kernel_info=None))
        policy = report["module_load_policy"]
        self.assertFalse(policy["policy_ready"])
        self.assertTrue(any("protected-export table" in value for value in policy["unknowns"]))
        self.assertTrue(any("symbol-access table" in value for value in policy["unknowns"]))

    def test_real_modversions_compare_matching_and_mismatching_crcs(self):
        manifest = json.loads((ROOT / "analysis/stock-binaries/manifest.json").read_text())
        item = next(x for x in manifest["modules"] if x["name"] == "ufs_mediatek_mod")
        parsed = stock_modules.elf_module(Path(item["path"]).read_bytes())
        self.assertEqual(parsed["modversion_status"], "parsed")
        self.assertEqual(parsed["license"], ["GPL v2"])
        self.assertEqual(parsed["import_ns"], [])
        self.assertEqual(parsed["modversion_format"], "extended")
        self.assertTrue(parsed["extended_legacy_match"])
        self.assertGreater(len(parsed["modversions"]), 0)
        self.assertEqual(parsed["export_integrity_errors"], [])
        ccci = next(x for x in manifest["modules"] if x["name"] == "ccci_util_lib")
        exported = stock_modules.elf_module(Path(ccci["path"]).read_bytes())
        self.assertEqual(exported["exported_symbols"]["ccci_dump_write"], 0x99c7c571)
        self.assertEqual(exported["exported_symbol_info"]["ccci_dump_write"]["ksymtab_offset"], 0)
        self.assertEqual(exported["exported_symbol_info"]["ccci_dump_write"]["kcrctab_offset"], 0)
        imports = parsed["modversions"]
        self.assertEqual(stock_modules.compare_modversions(imports, imports)["status"], "match")
        changed = dict(imports)
        name = next(iter(changed))
        changed[name] ^= 1
        result = stock_modules.compare_modversions(imports, changed)
        self.assertEqual(result["status"], "mismatch")
        self.assertEqual(result["crc_mismatches"], [name])

    def test_all_feature_crc_consistency_and_archive_readback(self):
        stock = json.loads((ROOT / "analysis/stock-modules.json").read_text())
        features = sorted(stock["dependency_analysis"]["features"])
        build_tmp = ROOT / "Mu-Silicium/Build/t70proPkg/LinuxTests"
        build_tmp.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=build_tmp) as tmp:
            archive_path = Path(tmp) / "stock-module-payload.cpio"
            report = stock_modules.audit(arguments(feature=features,
                extra_module=["trusted_mem", "device_apc_common"], package_output=archive_path))
            expected_feature_names = {name for group in features for name in
                                      stock["dependency_analysis"]["features"][group]["declared_dependency_closure"]}
            expected_names = expected_feature_names | {"trusted_mem", "device_apc_common"}
            self.assertEqual(report["module_count"], len(expected_names))
            self.assertEqual(report["feature_module_count"], len(expected_feature_names))
            self.assertEqual(report["extracted_name_count"], len(expected_names))
            self.assertEqual(report["missing_modules"], [])
            self.assertEqual(report["payload_integrity_errors"], [])
            self.assertTrue(report["module_payload_complete"])
            self.assertEqual(report["package_readback"]["status"],
                             "all CPIO members and binary payload hashes verified")
            self.assertFalse(report["autoload_files_included"])
            self.assertFalse(report["bootable_initramfs"])
            self.assertFalse(report["candidate_kernel_abi_ready"])
            # Every original source is checked, while byte-identical source
            # copies share a single archive payload. Keep both populations.
            manifest = json.loads((ROOT / "analysis/stock-binaries/manifest.json").read_text())
            selected = [row for row in manifest["modules"] if row["name"] in expected_names]
            source_keys = {(row["name"], row["source"]) for row in selected}
            binary_keys = {(row["name"], row["sha256"]) for row in selected}
            population = report["module_population"]
            self.assertEqual(population["selected_original_sources"], len(source_keys))
            self.assertEqual(population["verified_original_sources"], len(source_keys))
            self.assertEqual(population["verified_unique_binaries"], len(binary_keys))
            self.assertEqual(population["same_content_extra_sources"], len(source_keys) - len(binary_keys))
            self.assertEqual(report["extracted_binary_count"], len(binary_keys))
            self.assertEqual(report["module_version_section_audit"]["binary_count"], len(source_keys))
            self.assertEqual(report["module_version_section_audit"]["unique_binary_count"], len(binary_keys))
            self.assertEqual(report["module_version_section_audit"]["extended_legacy_match_count"],
                             len(source_keys))
            self.assertEqual(report["module_export_integrity"]["integrity_error_count"], 0)
            self.assertEqual(report["stock_module_export_catalog"]["module_binary_count"], 429)
            self.assertEqual(report["stock_module_export_catalog"]["integrity_error_count"], 0)
            provider = report["stock_kernel_exports"]["provider_crc_audit"]
            self.assertEqual(provider["counts"]["stock_kernel_crc_mismatches"], 0)
            self.assertEqual(provider["counts"]["stock_module_provider_crc_mismatches"], 0)
            self.assertEqual(provider["counts"]["unresolved_import_records"], 0)
            self.assertEqual(provider["counts"]["missing_module_provider_payload_import_records"], 0)
            self.assertEqual(provider["counts"]["missing_declared_dependency_import_records"], 27)
            self.assertEqual(provider["counts"]["module_provider_ambiguous_import_records"], 12)
            self.assertEqual(provider["status"], "crc_match_with_dependency_or_variant_gaps")
            self.assertEqual([x["name"] for x in report["variant_selection_pending"]], ["rfkill"])
            members = stock_modules.read_newc(archive_path.read_bytes())
            self.assertNotIn("init", members)
            self.assertFalse(any(name.endswith("/modules.load") for name in members))
            inventory_path = next(name for name in members if name.endswith("/stock-binary-inventory.json"))
            inventory = json.loads(members[inventory_path]["data"])
            self.assertFalse(inventory["autoload"])
            self.assertTrue({"trusted_mem", "device_apc_common"}.issubset(
                {x["name"] for x in inventory["modules"]}))
            rfkill = [x for x in inventory["modules"] if x["name"] == "rfkill"]
            self.assertEqual(len(rfkill), 2)
            self.assertTrue(all(x["variant_selection_pending"] for x in rfkill))
            self.assertNotEqual(rfkill[0]["path"], rfkill[1]["path"])

    def test_release_prefix_alone_is_advisory_with_module_crcs(self):
        report = stock_modules.audit(arguments(
            kernel_release="6.12.99-android16-test",
            candidate_config=ROOT / "analysis/stock-kernel.config",
        ))
        self.assertFalse(report["candidate_release_prefix_matches_stock"])
        self.assertTrue(report["candidate_release_prefix_advisory_with_module_crcs"])
        self.assertTrue(report["candidate_module_import_crcs_parsed"])
        self.assertEqual(report["candidate_module_symvers_status"], "not supplied")
        self.assertFalse(report["candidate_kernel_abi_ready"])


    def test_modinfo_duplicate_policy_tags_are_preserved(self):
        fields = stock_modules.modinfo_fields(
            b"license=GPL v2\0license=Proprietary\0"
            b"import_ns=DMA_BUF\0import_ns=DMA_BUF\0")
        self.assertEqual(fields["license"], ["GPL v2", "Proprietary"])
        self.assertEqual(fields["import_ns"], ["DMA_BUF", "DMA_BUF"])

    def test_export_namespace_comes_from_valid_rela_and_malformed_relas_error(self):
        manifest = json.loads((ROOT / "analysis/stock-binaries/manifest.json").read_text())
        item = next(x for x in manifest["modules"] if x["name"] == "ccci_util_lib")
        original = Path(item["path"]).read_bytes()

        def layout(raw):
            shoff = struct.unpack_from("<Q", raw, 40)[0]
            entsize, count, names_index = struct.unpack_from("<HHH", raw, 58)
            sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * entsize)
                        for i in range(count)]
            name_section = sections[names_index]
            names = raw[name_section[4]:name_section[4] + name_section[5]]
            section_names = [names[section[0]:].split(b"\0", 1)[0].decode()
                             for section in sections]
            string_index = section_names.index("__ksymtab_strings")
            string_section = sections[string_index]
            strings = raw[string_section[4]:string_section[4] + string_section[5]]
            rela_index = next(i for i, section in enumerate(sections)
                              if section_names[i].startswith(".rela__ksymtab")
                              and section[7] < len(section_names)
                              and section_names[section[7]] in ("__ksymtab", "__ksymtab_gpl"))
            rela = sections[rela_index]
            record = next(pos for pos in range(rela[4], rela[4] + rela[5], 24)
                          if struct.unpack_from("<Q", raw, pos)[0] % 12 == 8)
            target_offset, info, _ = struct.unpack_from("<QQq", raw, record)
            symtab = sections[rela[6]]
            sym_index = info >> 32
            sym = struct.unpack_from("<IBBHQQ", raw, symtab[4] + sym_index * symtab[9])
            return sections, section_names, rela, record, target_offset, info, sym[4], string_section, strings

        sections, names, rela, record, target, info, sym_value, string_section, strings = layout(original)
        symbol = next(name for name, details in
                      stock_modules.elf_module(original)["exported_symbol_info"].items()
                      if details["ksymtab_section"] == names[rela[7]]
                      and details["ksymtab_offset"] == target - 8)
        valid = bytearray(original)
        namespace_offset = strings.find(symbol.encode() + b"\0")
        self.assertGreaterEqual(namespace_offset, 0)
        struct.pack_into("<q", valid, record + 16, namespace_offset - sym_value)
        parsed = stock_modules.elf_module(bytes(valid))
        self.assertEqual(parsed["exported_symbol_info"][symbol]["namespace"], symbol)
        self.assertEqual(parsed["export_integrity_errors"], [])

        bad_type = bytearray(original)
        struct.pack_into("<I", bad_type, record + 8, 0)
        parsed = stock_modules.elf_module(bytes(bad_type))
        self.assertTrue(any("namespace relocation is not R_AARCH64_PREL32" in error
                            for error in parsed["export_integrity_errors"]))
        self.assertIsNone(parsed["exported_symbol_info"][symbol]["namespace"])

        missing = bytearray(original)
        struct.pack_into("<Q", missing, record, target + 1)
        parsed = stock_modules.elf_module(bytes(missing))
        self.assertTrue(any("missing export namespace relocation" in error
                            for error in parsed["export_integrity_errors"]))
        self.assertIsNone(parsed["exported_symbol_info"][symbol]["namespace"])

        out_of_bounds = bytearray(original)
        struct.pack_into("<q", out_of_bounds, record + 16, 1 << 30)
        parsed = stock_modules.elf_module(bytes(out_of_bounds))
        self.assertTrue(any("namespace string offset is outside" in error
                            for error in parsed["export_integrity_errors"]))


    def test_verified_symbol_policy_rejects_bad_names_counts_and_callsite(self):
        import copy
        key = "protected_symbol_exports_evidence"
        evidence = {"status": "verified", "count": 2, "names": ["alpha", "beta"],
                    "verified_callsite": {"status": "verified"}}
        self.assertEqual(stock_modules.read_symbol_policy_evidence({key:evidence}, key), evidence)
        self.assertIsNone(stock_modules.read_symbol_policy_evidence({}, key))
        for changes in ({"count": True}, {"count": 3}, {"names": ["beta", "alpha"]},
                        {"names": ["alpha", "alpha"]}, {"names": ["alpha", "bad name"]},
                        {"verified_callsite": {"status": "unverified"}}):
            bad = copy.deepcopy(evidence)
            bad.update(changes)
            with self.assertRaises(ValueError):
                stock_modules.read_symbol_policy_evidence({key:bad}, key)

    def test_protected_exports_do_not_treat_a_signature_trailer_as_trust(self):
        evidence = {"names": ["protected_api"]}
        check = stock_modules.protected_export_check
        self.assertEqual(check(["ordinary_api"], False, evidence)["status"], "clear")
        self.assertEqual(check(["protected_api"], False, evidence)["status"], "rejected")
        self.assertEqual(check(["protected_api"], True, evidence)["status"], "signature_trust_required")
        self.assertEqual(check(["ordinary_api"], False, None)["status"], "unknown")
        with self.assertRaises(ValueError):
            check([], 1, evidence)

    def test_unsigned_access_distinguishes_kernel_and_module_providers(self):
        evidence = {"names": ["vendor_api"]}
        check = stock_modules.vendor_symbol_access_check
        self.assertEqual(check("vendor_api", False, "kernel", evidence), "clear")
        self.assertEqual(check("private_api", False, "kernel", evidence), "rejected")
        self.assertEqual(check("private_api", False, "unsigned-module", evidence), "clear")
        self.assertEqual(check("private_api", False, "signature-trailer", evidence), "signature_trust_required")
        self.assertEqual(check("private_api", True, "kernel", evidence), "signature_trust_required")
        self.assertEqual(check("private_api", False, "kernel", None), "unknown")
        with self.assertRaises(ValueError):
            check("private_api", False, "unrecognized", evidence)


    def certificate_inputs(self):
        document=json.loads((ROOT / "analysis/stock-kernel-exports.json").read_text())
        image=ROOT / "Mu-Silicium/Build/t70proPkg/Linux/Image"
        certificates=stock_modules.read_trusted_public_certificates(document,image,document["image_sha256"])
        settings=stock_modules.config(ROOT / "analysis/stock-kernel.config")
        manifest=json.loads((ROOT / "analysis/stock-binaries/manifest.json").read_text())
        return document,image,certificates,settings,manifest

    def test_real_signature_content_and_exact_builtin_public_signer(self):
        document,image,certificates,settings,manifest=self.certificate_inputs()
        catalog=stock_modules.module_catalog(json.loads((ROOT / "analysis/stock-modules.json").read_text()))
        expected_signed={}
        for item in manifest["modules"]:
            original=catalog[item["name"]][item["source"]]
            self.assertEqual(item["sha256"],original["sha256"])
            if original["module_signature_trailer_present"]:
                expected_signed[(item["name"],item["source"],item["sha256"])]=item
        signed=[]
        for item in manifest["modules"]:
            raw=Path(item["path"]).read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(),item["sha256"])
            if raw.endswith(b"~Module signature appended~\n"):
                signed.append((item,stock_modules.verify_module_pkcs7(raw,certificates,settings)))
        identity=lambda item:(item["name"],item["source"],item["sha256"])
        self.assertEqual({identity(item) for item,result in signed},set(expected_signed))
        # These exact system partition bytes use the pinned stock Image signer.
        # A partition name alone never grants trust in the production verifier.
        expected_trusted={key for key in expected_signed if key[1]=="system_dlkm_a"}
        verified={identity(item) for item,result in signed if result["trusted_for_offline_policy"]}
        self.assertEqual(verified,expected_trusted)
        for item,result in signed:
            self.assertFalse(result["private_key_access"])
            self.assertFalse(result["runtime_keyring_execution_verified"])
            self.assertEqual(result["profile"]["digest"],"sha256")
            self.assertEqual(result["profile"]["public_key"],"rsa")
        unknown=[item for item,result in signed if not result["trusted_for_offline_policy"]]
        self.assertEqual({identity(item) for item in unknown},set(expected_signed)-expected_trusted)
        self.assertEqual({(item["name"],item["source"]) for item in unknown},
                         {("libarc4","vendor_dlkm_a"),("rfkill","vendor_dlkm_a"),("zsmalloc","vendor_dlkm_a")})
        self.assertEqual(len(certificates),1)
        self.assertEqual(hashlib.sha256(certificates[0]).hexdigest(),
                         "799764380a24c8d38d971588961dadd118ab1f013797aad81a5529b8e720f080")

    def test_pkcs7_rejects_changed_payload_signature_header_and_crypto_policy(self):
        document,image,certificates,settings,manifest=self.certificate_inputs()
        item=next(item for item in manifest["modules"] if item["name"]=="rfkill" and item["source"]=="system_dlkm_a")
        raw=Path(item["path"]).read_bytes()
        marker=b"~Module signature appended~\n"
        header=len(raw)-len(marker)-12
        for offset in (0,header-1):
            changed=bytearray(raw);changed[offset]^=1
            result=stock_modules.verify_module_pkcs7(bytes(changed),certificates,settings)
            self.assertFalse(result["trusted_for_offline_policy"])
            self.assertEqual(result["status"],"not-verified")
        for offset,value in ((header+2,3),(header+5,1),(header+8,0x7f)):
            changed=bytearray(raw);changed[offset]=value
            result=stock_modules.verify_module_pkcs7(bytes(changed),certificates,settings)
            self.assertFalse(result["trusted_for_offline_policy"])
            self.assertEqual(result["status"],"invalid")
        self.assertEqual(stock_modules.verify_module_pkcs7(marker,certificates,settings)["status"],"invalid")
        self.assertEqual(stock_modules.verify_module_pkcs7(raw,None,settings)["status"],"not-verified")
        for changes in ({"CRYPTO_SHA256":"n"},{"SYSTEM_BLACKLIST_KEYRING":"y"},{"SYSTEM_REVOCATION_LIST":"y"}):
            result=stock_modules.verify_module_pkcs7(raw,certificates,{**settings,**changes})
            self.assertFalse(result["trusted_for_offline_policy"])
        unsigned=raw
        stripped=0
        while stock_modules.module_pkcs7_parts(unsigned) is not None:
            unsigned,_=stock_modules.module_pkcs7_parts(unsigned)
            stripped+=1
            self.assertLessEqual(stripped,4)
        self.assertGreaterEqual(stripped,1)
        self.assertEqual(stock_modules.verify_module_pkcs7(unsigned,certificates,settings)["status"],"unsigned")

    def test_public_certificate_identity_bounds_and_fingerprint_are_checked(self):
        document,image,certificates,settings,manifest=self.certificate_inputs()
        self.assertIsNone(stock_modules.read_trusted_public_certificates(document,None,document["image_sha256"]))
        with self.assertRaisesRegex(ValueError,"Image SHA"):
            stock_modules.read_trusted_public_certificates(document,b"wrong Image",document["image_sha256"])
        for field,value in (("bytes",True),("bytes",1<<30),("image_offset","-0x1"),("sha256","0"*64)):
            altered=copy.deepcopy(document)
            altered["trusted_public_certificates"]["certificates"][0][field]=value
            with self.assertRaises(ValueError):
                stock_modules.read_trusted_public_certificates(altered,image,document["image_sha256"])
        for malformed in (b"\x30\x80",b"\x30\x82\x00\x01\x00",b"\x30\x82\x01",b"\x30\x03\x01"):
            with self.assertRaises(ValueError):
                stock_modules._der_only(malformed,0x30)

    def test_verified_signer_changes_only_proven_signature_policy_branches(self):
        evidence={"names":["protected_api"]}
        check=stock_modules.protected_export_check
        self.assertEqual(check(["protected_api"],True,evidence,trusted=True)["status"],"clear")
        with self.assertRaises(ValueError):
            check([],False,evidence,trusted=True)
        access=stock_modules.vendor_symbol_access_check
        self.assertEqual(access("private_api",True,"kernel",None,caller_trusted=True),"clear")
        self.assertEqual(access("private_api",False,"trusted-module",{"names":[]}),"rejected")
        self.assertEqual(access("protected_api",False,"trusted-module",evidence),"clear")
        with self.assertRaises(ValueError):
            access("private_api",False,"kernel",None,caller_trusted=True)

    def test_cpio_parser_rejects_truncated_data(self):
        archive = stock_modules.cpio_newc([("lib/modules/sample.ko", 0o100644, b"binary")])
        self.assertIn("lib/modules/sample.ko", stock_modules.read_newc(archive))
        with self.assertRaisesRegex(ValueError, "truncated"):
            stock_modules.read_newc(archive[:-32])


class ReadOnlyErofsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("bounded_erofs", SCRIPT.parent.parent / "inspect-stock-modules.py")
        cls.inspector = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.inspector)
        cls.reader = cls.inspector.ReadOnlyErofs

    @staticmethod
    def checksum(raw):
        struct.pack_into("<I", raw, 1028, 0)
        value = ReadOnlyErofsTests.reader.crc32c(raw[1024:4096])
        struct.pack_into("<I", raw, 1028, value)
        return raw

    @classmethod
    def fixture(cls, payload=b"hello", layout=0, extended=False, compact2=False, eof=False):
        import lz4.block
        raw = bytearray(12 * 4096)
        struct.pack_into("<III", raw, 1024, 0xe0f5e1e2, 0, 1)
        raw[1036] = 12
        struct.pack_into("<H", raw, 1038, 0)
        struct.pack_into("<II", raw, 1060, 12, 1)
        struct.pack_into("<I", raw, 1104, 1)
        struct.pack_into("<HHH", raw, 4096, 0, 0, 0x41ed)
        struct.pack_into("<I", raw, 4104, 16)
        struct.pack_into("<I", raw, 4112, 2)
        struct.pack_into("<QHBB", raw, 8192, 8, 12, 1, 0)
        raw[8204:8208] = b"file"
        inode = 4096 + 8 * 32
        struct.pack_into("<HHH", raw, inode, layout * 2 + int(extended), 0, 0x81a4)
        struct.pack_into("<Q" if extended else "<I", raw, inode + 8, len(payload))
        inline = inode + (64 if extended else 32)
        if layout in (0, 2):
            struct.pack_into("<I", raw, inode + 16, 3)
            n = len(payload) if layout == 0 else len(payload) // 4096 * 4096
            raw[3 * 4096:3 * 4096 + n] = payload[:n]
            raw[inline:inline + len(payload) - n] = payload[n:]
        else:
            start = (inline + 7) & ~7
            struct.pack_into("<IHBB", raw, start, 0, int(compact2), 0, 0)
            count = (len(payload) + 4095) // 4096
            values = [(1, 0)] + [(2, i) for i in range(1, count)]
            if eof:
                assert count == 2
                values[-1] = (0, len(payload) - 4096)
            if layout == 1:
                for i, (kind, offset) in enumerate(values):
                    struct.pack_into("<HHI", raw, start + 16 + i * 8, kind, offset, 6 if i == 0 else 0)
            else:
                base = start + 8
                initial = ((32 - base % 32) // 4) % 8
                middle = ((count - initial) // 16) * 16 if compact2 and count > initial else 0
                groups = []
                cursor = 0
                while cursor < count:
                    small = not initial <= cursor < initial + middle
                    size, entries, bits = (8, 2, 16) if small else (32, 16, 14)
                    groups.append((cursor, size, entries, bits))
                    cursor += entries
                position = base
                for cursor, size, entries, bits in groups:
                    packed = 0
                    for j, (kind, offset) in enumerate(values[cursor:cursor + entries]):
                        packed |= ((kind << 12) | offset) << (j * bits)
                    raw[position:position + size - 4] = packed.to_bytes(size - 4, "little")
                    struct.pack_into("<I", raw, position + size - 4, 5)
                    position += size
            encoded = lz4.block.compress(payload, store_size=False)
            assert len(encoded) <= 4096
            raw[7 * 4096 - len(encoded):7 * 4096] = encoded
        return cls.checksum(raw)

    def view(self, raw, short=False):
        return self.reader(lambda off, n: bytes(raw[off:off + n - (1 if short and n else 0)]), len(raw))

    def test_flat_inline_extended_directory_and_no_symlink_following(self):
        for layout in (0, 2):
            for extended in (False, True):
                payload = b"hello" if not layout else bytes(range(256)) * 17
                raw = self.fixture(payload, layout, extended)
                fs = self.view(raw)
                self.assertEqual(fs.directory(fs.inode(fs.root)), {"file": (8, 1)})
                self.assertEqual(fs.contents(fs.lookup("/file")), payload)
                for path in ("file", "/../file", "/./file", "//file", "/file/", "/file\0"):
                    with self.assertRaises(ValueError):
                        fs.lookup(path)
        raw = self.fixture(b"/outside", 2)
        struct.pack_into("<H", raw, 4356, 0xa1ff)
        fs = self.view(raw)
        self.assertEqual(fs.contents(fs.lookup("/file")), b"/outside")
        with self.assertRaises(ValueError):
            fs.lookup("/file/other")

    def test_lz4_full_compact_and_two_byte_index_packs(self):
        cases = [(1, False, False, 9000), (3, False, False, 9000),
                 (3, False, True, 4300), (3, True, False, 24 * 4096)]
        for layout, compact2, eof, size in cases:
            with self.subTest(layout=layout, compact2=compact2, eof=eof):
                payload = (b"synthetic GNSS file\n" * (size // 20 + 2))[:size]
                self.assertEqual(len(payload), size)
                fs = self.view(self.fixture(payload, layout, compact2=compact2, eof=eof))
                self.assertEqual(fs.contents(fs.lookup("/file")), payload)

    def test_crc_features_ranges_lengths_and_directory_bounds(self):
        self.assertEqual(self.reader.crc32c(b"123456789"), 0x1cf96d7c)
        raw = self.fixture()
        for offset, value in ((1024, 0), (1036, 13), (1037, 1), (1104, 2), (1063, 0xff)):
            bad = bytearray(raw); bad[offset] = value
            with self.assertRaises(ValueError):
                self.view(self.checksum(bad))
        bad = bytearray(raw); bad[1200] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC32C"):
            self.view(bad)
        with self.assertRaisesRegex(ValueError, "Truncated"):
            self.view(raw, short=True)
        fs = self.view(raw)
        for off, n in ((-1, 1), (0, -1), (len(raw), 1), (0.5, 1)):
            with self.assertRaises(ValueError):
                fs.read(off, n)
        for nid in (-1, 1 << 50, 0.5):
            with self.assertRaises(ValueError):
                fs.inode(nid)
        for field, value, fmt in ((4352, 8, "H"), (4360, self.reader.MAX_FILE + 1, "I"), (4368, 1000000, "I")):
            bad = bytearray(raw); struct.pack_into("<" + fmt, bad, field, value)
            with self.assertRaises(ValueError):
                fs = self.view(bad); fs.contents(fs.lookup("/file"))
        for off, value in ((8200, 11), (8200, 100), (8203, 1), (8204, ord("/"))):
            bad = bytearray(raw); bad[off] = value
            with self.assertRaises(ValueError):
                fs = self.view(bad); fs.directory(fs.inode(fs.root))

    def test_unsupported_compression_truncation_and_bad_extents_fail(self):
        import lz4.block
        raw = self.fixture(b"a" * 9000, 1)
        start = 4384
        for off, value in ((start, 1), (start + 4, 2), (start + 6, 1),
                           (start + 7, 1), (start + 16, 2), (start + 18, 1),
                           (start + 17, 8), (start + 20, 0xff)):
            bad = bytearray(raw); bad[off] = value
            with self.assertRaises((ValueError, lz4.block.LZ4BlockError)):
                fs = self.view(bad); fs.contents(fs.lookup("/file"))
        bad = bytearray(raw); bad[6 * 4096:7 * 4096] = bytes(4096)
        with self.assertRaisesRegex(ValueError, "bounds"):
            fs = self.view(bad); fs.contents(fs.lookup("/file"))
        # A valid compressed block with the wrong final length is still invalid.
        bad = bytearray(raw); struct.pack_into("<I", bad, 4360, 9001)
        with self.assertRaisesRegex(ValueError, "length mismatch"):
            fs = self.view(bad); fs.contents(fs.lookup("/file"))

    def test_authorized_backup_matches_independent_firmware_and_gnss_hashes(self):
        base = ROOT / "analysis/stock-binaries"
        manifest = json.loads((base / "gnss-userspace.json").read_text())
        backup = Path(manifest["source_backup"])
        layout = json.loads((backup.parents[1] / "partition-layout.json").read_text())
        self.assertTrue(layout["primary_header_crc_valid"] and layout["partition_entries_crc_valid"])
        sup = next(x for x in layout["partitions"] if x["name"] == "super")
        with backup.open("rb") as stream:
            partitions, checks = self.inspector.logical_partitions(stream, sup)
            self.assertTrue(checks["geometry_header_tables_sha256_valid"])
            p = partitions["vendor_a"]
            self.assertEqual(len(p["extents"]), 1)
            e = p["extents"][0]
            self.assertEqual(e["type"], 0)
            start = sup["offset_bytes"] + e["super_offset"]
            fs = self.reader(lambda off, n: self.inspector.read_at(stream, start + off, n), p["bytes"])
            firmware = json.loads((base / "firmware-manifest.json").read_text())["files"]
            for item in firmware + manifest["files"]:
                data = fs.contents(fs.lookup(item["source_path"]))
                self.assertEqual(len(data), item["bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
                self.assertEqual(data, (base / item["path"]).read_bytes())


if __name__ == "__main__":
    unittest.main()

