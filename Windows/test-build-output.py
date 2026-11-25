#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Toolchain-free tests; does not run a driver build or access a device."""
from pathlib import Path
import ast
import subprocess
import sys
import tempfile
import unittest
from build_output import output_directory


class OutputDirectoryTests(unittest.TestCase):
    root = Path("/repository")

    def test_default(self):
        self.assertEqual(output_directory([], self.root), self.root / "Windows/Build")

    def test_selected_driver_default(self):
        for driver in ("--ufs", "--himax"):
            self.assertEqual(output_directory([driver], self.root), self.root / "Windows/Build")

    def test_relative(self):
        self.assertEqual(output_directory(["--ufs", "--output-dir", "out/ufs"], self.root), self.root / "out/ufs")

    def test_absolute_and_spaces(self):
        self.assertEqual(output_directory(["--output-dir=/tmp/build outputs"], self.root), Path("/tmp/build outputs"))

    def test_lock_reexec(self):
        args = ["--himax", "--output-dir", "out/touch"]
        self.assertEqual(output_directory(args, self.root), output_directory(["--locked", *args], self.root))

    def test_expand_user(self):
        self.assertEqual(output_directory(["--output-dir=~/build-out"], self.root), Path.home() / "build-out")

    def test_invalid(self):
        for args in (["--output-dir"], ["--output-dir", "--ufs"], ["--output-dir="], ["--output-dir= "], ["--output-dir=a", "--output-dir", "b"]):
            with self.subTest(args=args), self.assertRaises(ValueError):
                output_directory(args, self.root)

    def test_help_does_not_build(self):
        script = Path(__file__).with_name("build.py")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "must-not-be-created"
            result = subprocess.run([sys.executable, str(script), "--help", "--output-dir", str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Default target: I2C", result.stdout)
            self.assertFalse(output.exists())

    def test_build_wiring(self):
        # Check the actual entrypoint without executing its lock/toolchain steps.
        tree = ast.parse(Path(__file__).with_name("build.py").read_text())
        assignments = [n for n in tree.body if isinstance(n, ast.Assign)]
        b = next(n for n in assignments if any(isinstance(t, ast.Name) and t.id == "b" for t in n.targets))
        self.assertIsInstance(b.value, ast.Name)
        self.assertEqual(b.value.id, "output")
        text = ast.unparse(tree)
        self.assertLess(text.index("output_directory(sys.argv[1:], root)"), text.index("os.execv("))
        self.assertIn("*sys.argv[1:]", text)
        self.assertIn("w / 'build_output.py'", text)
        self.assertIn("wdm_image = True", text)
        self.assertIn("if wdm_image", text)
        self.assertIn("checksum != expected_checksum", text)
        self.assertIn("TERMINAL_SERVER_AWARE", text)


if __name__ == "__main__":
    unittest.main()
