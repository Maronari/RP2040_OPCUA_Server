#!/usr/bin/env python3
"""Compile and run the native maintenance service transaction/fault test."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc", help="Host C compiler")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "maintenance_service"
    source = root / "services" / "src" / "maintenance_service.c"
    test_source = test_dir / "maintenance_service_atomic_test.c"

    with tempfile.TemporaryDirectory(prefix="maintenance-atomic-") as directory:
        executable = pathlib.Path(directory) / "maintenance_service_atomic_test.exe"
        compile_command = [
            args.cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{test_dir}",
            f"-I{root / 'services' / 'include'}",
            str(source),
            str(test_source),
            "-lm",
            "-o",
            str(executable),
        ]
        subprocess.run(compile_command, check=True)
        subprocess.run([str(executable)], check=True)

    print("maintenance service transaction and fault policy: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
