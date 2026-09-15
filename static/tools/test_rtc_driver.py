#!/usr/bin/env python3
"""Compile and run the rtc_driver AON-timer serialization host suite.

Built twice: PICO_RP2040=0 exercises the RP2350 timespec path the firmware
ships, PICO_RP2040=1 the calendar path, which no firmware build compiles.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc", help="Host C compiler")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "rtc_driver"
    with tempfile.TemporaryDirectory(prefix="rtc-driver-") as directory:
        for flavor, rp2040 in (("timespec", "0"), ("calendar", "1")):
            executable = pathlib.Path(directory) / f"rtc_driver_test_{flavor}.exe"
            subprocess.run([
                args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DPICO_RP2040={rp2040}",
                # test_dir first: its FreeRTOS.h/semphr.h/pico stubs shadow the real ones.
                f"-I{test_dir}",
                f"-I{root / 'drivers' / 'include'}",
                str(root / "drivers" / "src" / "rtc_driver.c"),
                str(test_dir / "rtc_driver_test.c"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    print("rtc driver host suite: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
