#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run pinned upstream host checks with EDK2's actual unaligned-helper pointer types."""
from pathlib import Path
import re
import sys

package = Path(__file__).resolve().parents[1]
source_dir = package.parents[2].parent / "xnu-sources/DarwinBoot/Test"
expected = {"test-kernel-parser.py": 0, "test-adt-window.py": 0,
            "test-memory-window.py": 2, "test-memory-outside.py": 2,
            "test-loader-inputs.py": 4, "test-packed-ramdisk.py": 2}
if len(sys.argv) not in (2,3) or sys.argv[1] not in expected:
    sys.exit("Pass one supported pinned DarwinBoot host-check filename")
path = source_dir / sys.argv[1]
text = path.read_text()
# Current EDK2 BaseLib/Unaligned.c takes CONST VOID*/VOID*, even for 64-bit values.
# Upstream's typed test stubs make Clang 21 assume alignment in their memcpy calls.
# Change only these stub parameter types; keep the production header and test cases intact.
text, count = re.subn(r"(ReadUnaligned(?:16|32|64)\()const (?:UINT16|UINT32|UINT64|uint16_t|uint32_t|uint64_t) \*p",
                      r"\1const void *p", text)
text, writes = re.subn(r"(WriteUnaligned(?:16|32|64)\()(?:UINT16|UINT32|UINT64|uint16_t|uint32_t|uint64_t) \*p", r"\1void *p", text)
assert count + writes == expected[path.name], (path.name, count, writes)
sys.path.insert(0, str(source_dir))
sys.argv = [str(path), *sys.argv[2:]]
if count + writes:
    print(f"EDK2 host-helper signature adapter: {path.name}, {count + writes} parameters", flush=True)
if path.name == "test-adt-window.py" and len(sys.argv) == 2:
    text += "\nimport sys\nfor filename in sys.argv[1:]:\n    actual = parse_all(Path(filename).read_bytes())\n    assert actual == (True, 0x10000000, none, 0, none), actual\n    print('PASS: actual T70PRO full-allocation ADT', filename)\n"
exec(compile(text, str(path), "exec"), {"__name__": "__main__", "__file__": str(path)})
