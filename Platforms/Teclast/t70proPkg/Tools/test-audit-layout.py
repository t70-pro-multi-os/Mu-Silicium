#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("layout", Path(__file__).with_name("audit-layout.py"))
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)


def fixture():
    return {"logical_sector_bytes": 4096, "physical_main_LUN_bytes": 100 * 4096,
            "first_usable_lba": 8, "last_usable_lba": 91, "backup_header_lba": 99,
            "primary_header_crc_valid": True, "partition_entries_crc_valid": True,
            "backup_header_crc_valid": True, "backup_entries_match_primary": True,
            "partition_count": 2, "partitions": [
                {"name": "boot_a", "start_lba": 8, "end_lba": 19, "offset_bytes": 8 * 4096, "size_bytes": 12 * 4096},
                {"name": "userdata", "start_lba": 20, "end_lba": 91, "offset_bytes": 20 * 4096, "size_bytes": 72 * 4096}]}


class LayoutTest(unittest.TestCase):
    def test_full_and_input_unchanged(self):
        d = fixture(); before = copy.deepcopy(d); result = layout.audit(d)
        self.assertEqual(result["free_bytes"], 0)
        self.assertEqual(result["occupied_bytes"], 84 * 4096)
        self.assertEqual(result["largest_gap_bytes"], 0)
        self.assertIn("requires_reclaim", result["recommendation"])
        self.assertEqual(d, before)

    def test_unsorted_leading_internal_trailing_gaps(self):
        d = fixture()
        d["partitions"] = [{"name": str(a), "start_lba": a, "end_lba": b,
                            "offset_bytes": a * 4096, "size_bytes": (b-a+1)*4096}
                           for a,b in [(30,40),(10,19)]]
        result = layout.audit(d)
        self.assertEqual([(g["start_lba"],g["end_lba"]) for g in result["gaps"]], [(8,9),(20,29),(41,91)])
        self.assertEqual(result["free_bytes"], 63 * 4096)
        self.assertEqual(result["largest_gap_bytes"], 51 * 4096)

    def test_reject_bad_metadata(self):
        cases = [("logical_sector_bytes", True), ("logical_sector_bytes", 513),
                 ("partition_count", 3), ("backup_header_lba", 98),
                 ("last_usable_lba", 99), ("physical_main_LUN_bytes", 409601)]
        cases += [(k, False) for k in fixture() if "valid" in k or "match" in k]
        for key,value in cases:
            with self.subTest(key=key):
                d=fixture(); d[key]=value
                with self.assertRaises(ValueError): layout.audit(d)
        for key,value in [("name","boot_a"),("start_lba",19),("end_lba",92),
                          ("offset_bytes",0),("size_bytes",1),("end_lba",True)]:
            d=fixture(); d["partitions"][1][key]=value
            with self.assertRaises(ValueError): layout.audit(d)
        d=fixture(); d["partitions"][1].update(start_lba=19, offset_bytes=19*4096, size_bytes=73*4096)
        with self.assertRaisesRegex(ValueError,"overlapping"): layout.audit(d)

    def test_regular_json_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/"layout.json"; p.write_text(json.dumps(fixture()))
            self.assertEqual(layout.read_inventory(p), fixture())
            link=Path(tmp)/"link.json"; link.symlink_to(p)
            with self.assertRaises(ValueError): layout.read_inventory(link)
            image=Path(tmp)/"image.img"; image.write_bytes(b"{}")
            with self.assertRaises(ValueError): layout.read_inventory(image)
            p.write_bytes(b" "*(1024*1024+1))
            with self.assertRaises(ValueError): layout.read_inventory(p)


if __name__ == "__main__":
    unittest.main()
