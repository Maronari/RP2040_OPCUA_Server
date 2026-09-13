#!/usr/bin/env python3
"""Compile and run the time_service synchronization-state host suite."""

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
    test_dir = root / "static" / "tests" / "time_service"
    with tempfile.TemporaryDirectory(prefix="time-service-") as directory:
        executable = pathlib.Path(directory) / "time_service_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            # test_dir first: its FreeRTOS.h/task.h/lwip stubs shadow the real ones.
            f"-I{test_dir}",
            f"-I{root / 'services' / 'include'}",
            f"-I{root / 'drivers' / 'include'}",
            str(root / "services" / "src" / "time_service.c"),
            str(test_dir / "time_service_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)

    print("time service host suite: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
