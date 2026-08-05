#!/usr/bin/env python3
"""Compile and run the storage-independent maintenance journal codec test."""

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
    with tempfile.TemporaryDirectory(prefix="maintenance-journal-codec-") as directory:
        executable = pathlib.Path(directory) / "maintenance_journal_codec_test.exe"
        command = [
            args.cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "maintenance_journal_codec.c"),
            str(root / "static" / "tests" / "maintenance_journal_codec" /
                "maintenance_journal_codec_test.c"),
            "-lm",
            "-o",
            str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("maintenance journal codec host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
