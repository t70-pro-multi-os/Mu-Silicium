# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Resolve build output paths without touching the filesystem or toolchain.

--output-dir PATH (or --output-dir=PATH) selects artifacts only. Relative
paths are relative to the repository root, also after the build-lock re-exec.
The existing Windows/Build default is unchanged.
"""
from pathlib import Path


def output_directory(arguments, repository_root):
    value = None
    args = iter(arguments)
    for argument in args:
        if argument == "--output-dir":
            candidate = next(args, None)
            if candidate is None or candidate.startswith("--"):
                raise ValueError("--output-dir requires a directory path")
        elif argument.startswith("--output-dir="):
            candidate = argument.split("=", 1)[1]
        else:
            continue
        if value is not None:
            raise ValueError("--output-dir may be specified only once")
        if not candidate.strip():
            raise ValueError("--output-dir requires a nonempty directory path")
        value = candidate
    root = Path(repository_root)
    if value is None:
        return root / "Windows" / "Build"
    path = Path(value).expanduser()
    return path if path.is_absolute() else root / path
