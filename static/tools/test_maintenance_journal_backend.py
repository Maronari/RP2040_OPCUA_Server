#!/usr/bin/env python3
"""Compile and run maintenance-journal backend recovery boundary tests."""

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
    with tempfile.TemporaryDirectory(prefix="maintenance-journal-backend-") as directory:
        executable = pathlib.Path(directory) / "maintenance_journal_backend_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'drivers' / 'include'}", f"-I{root / 'services' / 'include'}",
            str(root / "drivers" / "src" / "maintenance_journal_backend.c"),
            str(root / "services" / "src" / "maintenance_journal_codec.c"),
            str(root / "static" / "tests" / "maintenance_journal_backend" /
                "maintenance_journal_backend_test.c"),
            "-lm", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("maintenance journal backend recovery host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
