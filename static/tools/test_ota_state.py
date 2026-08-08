#!/usr/bin/env python3
"""Compile and run OTA state-record integrity host tests."""

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
    test_dir = root / "static" / "tests" / "ota_state"
    with tempfile.TemporaryDirectory(prefix="ota-state-") as directory:
        executable = pathlib.Path(directory) / "ota_state_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'ota' / 'include'}", str(root / "ota" / "src" / "ota_state.c"),
            str(test_dir / "ota_state_test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("OTA state-record host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
