#!/usr/bin/env python3
"""Compile and run fake-W5500 MACRAW transport boundary tests."""

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
    test_dir = root / "static" / "tests" / "w5x00_transport"
    with tempfile.TemporaryDirectory(prefix="w5x00-transport-") as directory:
        executable = pathlib.Path(directory) / "w5x00_transport_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DW5X00_DIAGNOSTIC_PRINTF=w5x00_test_printf",
            f"-I{test_dir}",
            f"-I{root / 'drivers' / 'include'}",
            f"-I{root / 'port' / 'lwip'}",
            str(root / "drivers" / "src" / "w5x00_ethernet_driver.c"),
            str(root / "port" / "lwip" / "w5x00_lwip.c"),
            str(test_dir / "w5x00_transport_test.c"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("fake-W5500 MACRAW transport suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
