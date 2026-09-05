#!/usr/bin/env python3
"""Compile and run native TCAL9539 register driver host tests (v0.5.1 Phase 1)."""

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
    test_dir = root / "static" / "tests" / "tcal9539_driver"
    with tempfile.TemporaryDirectory(prefix="tcal9539-driver-") as directory:
        executable = pathlib.Path(directory) / "tcal9539_driver_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'drivers' / 'include'}",
            str(root / "drivers" / "src" / "tcal9539_driver.c"),
            str(root / "drivers" / "src" / "i2c_transport.c"),
            str(test_dir / "tcal9539_driver_test.c"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("native TCAL9539 driver host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
