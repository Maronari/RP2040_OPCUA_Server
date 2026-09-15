#!/usr/bin/env python3
"""Compile and run native ADS8688 industrial ADC driver host tests (v0.5.1 T2)."""

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
    test_dir = root / "static" / "tests" / "ads8688_driver"
    with tempfile.TemporaryDirectory(prefix="ads8688-driver-") as directory:
        executable = pathlib.Path(directory) / "ads8688_driver_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'drivers' / 'include'}",
            f"-I{root / 'services' / 'include'}",
            str(root / "drivers" / "src" / "ads8688_driver.c"),
            str(root / "drivers" / "src" / "spi_transport.c"),
            str(test_dir / "ads8688_driver_test.c"),
            "-lm",
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("native ADS8688 driver host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
