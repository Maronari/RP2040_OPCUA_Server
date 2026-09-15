#!/usr/bin/env python3
"""Compile and run native IPS2050 protected high-side digital-output driver host tests (v0.5.1 Phase 1)."""

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
    test_dir = root / "static" / "tests" / "ips2050_driver"
    with tempfile.TemporaryDirectory(prefix="ips2050-driver-") as directory:
        executable = pathlib.Path(directory) / "ips2050_driver_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{test_dir}",
            f"-I{root / 'drivers' / 'include'}",
            f"-I{root / 'services' / 'include'}",
            str(root / "drivers" / "src" / "ips2050_driver.c"),
            str(test_dir / "hardware_gpio_stub.c"),
            str(test_dir / "ips2050_driver_test.c"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("native IPS2050 driver host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
