#!/usr/bin/env python3
"""Failure-focused tests for offline provider/CRC comparisons."""
import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("gki_compare", Path(__file__).with_name("compare-abi.py"))
abi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(abi)


def export(crc=0x12345678, provider="vmlinux", kind="EXPORT_SYMBOL", namespace=""):
    return {"crc": crc, "provider": provider, "export": kind, "namespace": namespace}


def module(name="consumer", imports=None, exports=None, depends=None):
    return {"name": name, "source": "vendor", "sha256": name,
            "license": ["GPL"], "import_ns": [], "depends": depends or [],
            "modversion_status": "parsed", "modversions": imports or {},
            "exported_symbol_info": exports or {}, "export_integrity_errors": []}


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.kernel = {"kernel_api": export()}
        self.consumer = module(imports={"kernel_api": 0x12345678, "vendor_api": 0x89abcdef}, depends=["provider"])
        self.provider = module("provider", exports={"vendor_api": export(0x89abcdef, "provider")})
        self.modules = [self.consumer, self.provider]

    def check(self, candidate=None):
        return abi.compare(self.modules, self.kernel, self.kernel if candidate is None else candidate)

    def test_vendor_provider_does_not_have_to_be_in_vmlinux(self):
        report = self.check()
        self.assertTrue(report["static_import_compatible"])
        self.assertEqual(report["counts"]["kernel_import_records"], 1)
        self.assertEqual(report["counts"]["module_import_records"], 1)

    def test_changed_kernel_crc_is_rejected(self):
        report = self.check({"kernel_api": export(1)})
        self.assertEqual(report["errors_by_reason"], {"candidate kernel CRC differs": 1})

    def test_missing_kernel_export_is_rejected(self):
        self.assertIn("candidate kernel export missing", self.check({})["errors_by_reason"])

    def test_kernel_export_moved_to_module_is_rejected(self):
        self.assertIn("kernel export moved to a module", self.check({"kernel_api": export(provider="drivers/foo")})["errors_by_reason"])

    def test_vendor_provider_crc_mismatch_is_rejected(self):
        self.provider["exported_symbol_info"]["vendor_api"]["crc"] = 1
        self.assertFalse(self.check()["static_import_compatible"])

    def test_gpl_only_export_rejects_non_gpl_importer(self):
        self.consumer["license"] = ["Proprietary"]
        candidate = {"kernel_api": export(kind="EXPORT_SYMBOL_GPL")}
        self.assertIn("GPL-only export denied", self.check(candidate)["errors_by_reason"])

    def test_namespace_must_be_imported(self):
        candidate = {"kernel_api": export(namespace="PRIVATE_API")}
        self.assertIn("namespace not imported", self.check(candidate)["errors_by_reason"])
        self.consumer["import_ns"] = ["PRIVATE_API"]
        self.assertTrue(self.check(candidate)["static_import_compatible"])

    def test_unrecoverable_module_namespace_is_not_empty(self):
        self.provider["exported_symbol_info"]["vendor_api"]["namespace"] = None
        self.assertIn("export namespace could not be recovered", self.check()["errors_by_reason"])

    def test_kernel_cannot_take_over_a_stock_module_export(self):
        candidate = {**self.kernel, "vendor_api": export(0x89abcdef)}
        self.assertIn("kernel/module export collision", self.check(candidate)["errors_by_reason"])

    def test_invalid_module_version_sections_rejected(self):
        self.consumer["modversion_status"] = "incomplete extended modversion sections"
        self.assertFalse(self.check()["static_import_compatible"])

    def test_kernel_uses_valid_extended_table_before_legacy(self):
        self.consumer["modversion_format"] = "extended"
        self.consumer["modversion_status"] = "extended/legacy CRC mismatch"
        result = self.check()
        self.assertTrue(result["static_import_compatible"])
        self.assertEqual(len(result["warnings"]), 1)
        self.consumer["modversion_status"] = "incomplete extended modversion sections"
        self.assertFalse(self.check()["static_import_compatible"])

    def test_ambiguous_variant_policy_is_rejected(self):
        other = copy.deepcopy(self.provider)
        other["sha256"] = "different"
        other["exported_symbol_info"]["vendor_api"]["namespace"] = "OTHER"
        self.modules.append(other)
        self.assertIn("module provider variants disagree on export policy", self.check()["errors_by_reason"])


class SymversTests(unittest.TestCase):
    def test_preserves_provider_export_class_and_namespace(self):
        result = abi.symvers("0x12345678\tkernel_api\tvmlinux\tEXPORT_SYMBOL_GPL\tSCOPE\n")
        self.assertEqual(result["kernel_api"], export(kind="EXPORT_SYMBOL_GPL", namespace="SCOPE"))

    def test_malformed_and_duplicate_records_rejected(self):
        record = "0x12345678\ta\tvmlinux\tEXPORT_SYMBOL\t\n"
        for text in ("", "123 a vmlinux EXPORT_SYMBOL", record + record, record.replace("0x12345678", "0x100000000")):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    abi.symvers(text)


if __name__ == "__main__":
    unittest.main()
