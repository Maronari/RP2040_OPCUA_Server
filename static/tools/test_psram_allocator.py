#!/usr/bin/env python3
"""Compile and run v0.5.2 PSRAM allocation/accounting failure tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "psram_allocator"
    with tempfile.TemporaryDirectory(prefix="psram-allocator-") as directory:
        executable = pathlib.Path(directory) / "psram_allocator_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DPSRAM_ALLOCATOR_TEST_HOOKS=1",
            f"-I{test_dir}", f"-I{root / 'port' / 'psram'}",
            str(root / "port" / "psram" / "psram_allocator.c"),
            str(test_dir / "psram_allocator_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("v0.5.2 PSRAM allocator host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
