#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Synthetic fixtures only; never stages or signs a real driver image."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('stage', ROOT / 'stage-audio-package.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class StagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='t70-package-fixture-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.output = self.base / 'package'
        self.rp = self.base / 'report.json'
        self.inf = self.base / 'source.inf'
        self.inf.write_bytes((ROOT / 'MtkAudio.inf').read_bytes())
        self.image = bytearray(512)
        self.image[:2] = b'MZ'
        struct.pack_into('<I', self.image, 60, 64)
        self.image[64:68] = b'PE\0\0'
        struct.pack_into('<HH', self.image, 68, 0xaa64, 1)
        struct.pack_into('<HH', self.image, 84, 240, 2)
        struct.pack_into('<H', self.image, 88, 0x20b)
        struct.pack_into('<H', self.image, 156, 1)
        struct.pack_into('<I', self.image, 196, 16)
        self.report = {
            'audit_completed': True, 'driver_link_succeeded': True,
            'diagnostic_link': {'exit': 0}, 'unresolved_project_symbols': [],
            'components': [{'source': p.name, 'source_sha256': m.sha(p.read_bytes()), 'exit': 0}
                           for p in ROOT.iterdir() if p.suffix in ('.c', '.cpp') and not p.stem.endswith('Test')],
            'project_headers_sha256': {p.name: m.sha(p.read_bytes()) for p in ROOT.glob('*.h')},
            'kmdf_api_compatibility': {'result': 'pass'},
            'kmdf_binary_minimum': {'linked_minimum': {'value': 33}},
            'driver_image': '/unrelated/build/MtkAudio.sys',
        }

    def save(self):
        (self.base / 'MtkAudio.sys').write_bytes(self.image)
        self.report['driver_sha256'] = m.sha(self.image)
        self.rp.write_text(json.dumps(self.report))

    def stage(self):
        return m.stage(self.rp, self.inf, self.output)

    def test_success_exact_bytes_no_catalog(self):
        self.save()
        r = self.stage()
        self.assertEqual({p.name for p in self.output.iterdir()}, {'MtkAudio.sys', 'MtkAudio.inf', 'manifest.json'})
        self.assertEqual((self.output / 'MtkAudio.sys').read_bytes(), self.image)
        self.assertEqual((self.output / 'MtkAudio.inf').read_bytes(), self.inf.read_bytes())
        self.assertEqual(r['status'], 'unsigned_unvalidated_staging_only')
        self.assertIn('/os:10_GE_ARM64', r['inf2cat_command_template'])
        with self.assertRaises(ValueError): self.stage()
        self.assertEqual((self.output / 'MtkAudio.sys').read_bytes(), self.image)

    def test_report_failures_no_directory(self):
        cases = [('audit_completed', False), ('driver_link_succeeded', False),
                 ('diagnostic_link', {'exit': 1}), ('unresolved_project_symbols', ['T70Missing']),
                 ('components', []), ('project_headers_sha256', {}),
                 ('kmdf_api_compatibility', {}), ('kmdf_binary_minimum', {}),
                 ('driver_image', 'wrong.sys')]
        original = copy.deepcopy(self.report)
        for key, value in cases:
            with self.subTest(key=key):
                self.report = copy.deepcopy(original); self.report[key] = value; self.save()
                with self.assertRaises(ValueError): self.stage()
                self.assertFalse(self.output.exists())
        self.report = original
        self.report['components'][0]['source_sha256'] = 'stale'
        self.save()
        with self.assertRaises(ValueError): self.stage()
        self.assertFalse(self.output.exists())

    def test_hash_and_inf_failures(self):
        self.save()
        (self.base / 'MtkAudio.sys').write_bytes(b'corrupt')
        with self.assertRaises(ValueError): self.stage()
        self.assertFalse(self.output.exists())
        original = self.inf.read_text()
        for old, new in [('MtkAudio.cat', 'Other.cat'), ('MtkAudio.sys', 'Other.sys'),
                         ('NTarm64', 'NTamd64'), ('CopyFiles=DriverCopy', 'CopyFiles=Other')]:
            with self.subTest(old=old):
                self.save(); self.inf.write_text(original.replace(old, new))
                with self.assertRaises(ValueError): self.stage()
                self.assertFalse(self.output.exists())

    def test_pe_failures(self):
        good = bytes(self.image)
        for offset, fmt, value in [(68, '<H', 0x8664), (88, '<H', 0x10b),
                                   (156, '<H', 3), (60, '<I', 500),
                                   (196, '<I', 17), (232, '<I', 1),
                                   (344, '<I', 1024)]:
            with self.subTest(offset=offset):
                self.image = bytearray(good); struct.pack_into(fmt, self.image, offset, value); self.save()
                with self.assertRaises(ValueError): self.stage()
                self.assertFalse(self.output.exists())

    def test_output_and_write_failure(self):
        self.save()
        with self.assertRaises(ValueError): m.stage(self.rp, self.inf, ROOT / 'not-created-package')
        self.output.symlink_to(self.base / 'absent')
        with self.assertRaises(ValueError): self.stage()
        self.output.unlink()
        original = Path.write_bytes
        def fail(path, data):
            if path.name == 'MtkAudio.inf': raise OSError('injected disk failure')
            return original(path, data)
        with patch.object(Path, 'write_bytes', fail):
            with self.assertRaises(OSError): self.stage()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
