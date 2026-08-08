#!/usr/bin/env python3
"""Compile and run bounded channel-manager registry host tests."""

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
    test_dir = root / "static" / "tests" / "channel_manager"
    with tempfile.TemporaryDirectory(prefix="channel-manager-") as directory:
        executable = pathlib.Path(directory) / "channel_manager_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "channel_manager.c"),
            str(test_dir / "channel_manager_test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("channel-manager registry host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
