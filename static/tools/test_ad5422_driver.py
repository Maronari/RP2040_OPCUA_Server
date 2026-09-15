#!/usr/bin/env python3
"""Compile and run native AD5422 industrial analog-output driver host tests (v0.5.1 Phase 1)."""

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
    test_dir = root / "static" / "tests" / "ad5422_driver"
    with tempfile.TemporaryDirectory(prefix="ad5422-driver-") as directory:
        executable = pathlib.Path(directory) / "ad5422_driver_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            # test_dir first so its hardware/gpio.h and hardware/timer.h
            # shadow the Pico SDK's -- the driver drives AO_ENABLE_REQ and
            # busy-waits during chain startup.
            f"-I{test_dir}",
            f"-I{root / 'drivers' / 'include'}",
            f"-I{root / 'services' / 'include'}",
            str(root / "drivers" / "src" / "ad5422_driver.c"),
            str(root / "drivers" / "src" / "spi_transport.c"),
            str(test_dir / "ad5422_driver_test.c"),
            "-lm",
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("native AD5422 driver host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
