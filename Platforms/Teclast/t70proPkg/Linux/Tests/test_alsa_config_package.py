#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Check the actual desktop data collection against installed ALSA assets."""
import importlib.util
from pathlib import Path
import re
import unittest

file = Path(__file__).resolve().parents[1] / "desktop.py"
spec = importlib.util.spec_from_file_location("t70_desktop_data", file)
desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop)


class AlsaConfigPackageTest(unittest.TestCase):
    def test_default_pcm_has_the_package_alias_include_closure(self):
        entries = desktop.data_inputs()
        root = desktop.DEPS / "usr/share/alsa"
        prefix = "usr/share/alsa/"
        alias = (root / "cards/aliases.conf").read_text()
        include = set(re.findall(r"^<confdir:([^>]+)>$", alias, flags=re.MULTILINE))
        self.assertIn("pcm/default.conf", include)
        missing = sorted(name for name in include if prefix + name not in entries)
        self.assertEqual(missing, [], "missing ALSA aliases.conf includes")
        for name in include:
            self.assertEqual(entries[prefix + name], root / name)


if __name__ == "__main__":
    unittest.main()
