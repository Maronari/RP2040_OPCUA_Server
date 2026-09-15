#!/usr/bin/env python3
"""Compile and run native MAX22190 industrial digital-input driver host tests (v0.5.1 Phase 1)."""

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
    test_dir = root / "static" / "tests" / "max22190_driver"
    with tempfile.TemporaryDirectory(prefix="max22190-driver-") as directory:
        executable = pathlib.Path(directory) / "max22190_driver_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            # test_dir first so its hardware/gpio.h shadows the Pico SDK's --
            # the driver reads DI_READY_N (GPIO24) directly, same technique
            # as static/tools/test_ips2050_driver.py.
            f"-I{test_dir}",
            f"-I{root / 'drivers' / 'include'}",
            f"-I{root / 'services' / 'include'}",
            str(root / "drivers" / "src" / "max22190_driver.c"),
            str(root / "drivers" / "src" / "spi_transport.c"),
            str(test_dir / "hardware_gpio_stub.c"),
            str(test_dir / "max22190_driver_test.c"),
            "-lm",
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("native MAX22190 driver host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
