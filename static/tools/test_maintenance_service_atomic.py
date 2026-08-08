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
    test_sources = [test_dir / "maintenance_service_atomic_test.c",
                    test_dir / "maintenance_service_contract_test.c",
                    test_dir / "maintenance_service_journal_test.c"]

    with tempfile.TemporaryDirectory(prefix="maintenance-atomic-") as directory:
        for test_source in test_sources:
            executable = pathlib.Path(directory) / f"{test_source.stem}.exe"
            compile_command = [
            args.cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{test_dir}",
            f"-I{root / 'services' / 'include'}",
            f"-I{root / 'drivers' / 'include'}",
            str(root / "drivers" / "src" / "maintenance_journal_backend.c"),
            str(root / "services" / "src" / "maintenance_journal_codec.c"),
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
