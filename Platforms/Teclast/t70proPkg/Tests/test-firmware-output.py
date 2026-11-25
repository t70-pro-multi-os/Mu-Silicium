#!/usr/bin/env python3
"""Reject unsafe/custom unsupported output paths before invoking any builder."""
import os
from pathlib import Path
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "build.sh"

class FirmwareOutputTest(unittest.TestCase):
    def test_rejected_inputs_do_not_start_build(self):
        cases = [("firmware", "../escape"), ("firmware", "/tmp/out"),
                 ("firmware", "Build/a/b"), ("firmware", "Build/a b"),
                 ("acpi", "Build/test"), ("xnuboot", "Build/test")]
        for target, output in cases:
            with self.subTest(target=target, output=output):
                env = dict(os.environ, T70_BUILD_OUTPUT=output, BUILD_ACTION="genc")
                result = subprocess.run(["bash", str(SCRIPT), "--locked", target],
                                        env=env, text=True, capture_output=True,
                                        timeout=10)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn("Build start time", result.stdout)

if __name__ == "__main__":
    unittest.main()
