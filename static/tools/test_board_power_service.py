#!/usr/bin/env python3
"""Compile and run board-power-service host tests (v0.5.1 Phase 1)."""

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
    test_dir = root / "static" / "tests" / "board_power_service"
    with tempfile.TemporaryDirectory(prefix="board-power-service-") as directory:
        executable = pathlib.Path(directory) / "board_power_service_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'drivers' / 'include'}",
            f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "board_power_service.c"),
            str(root / "drivers" / "src" / "tcal9539_driver.c"),
            str(root / "drivers" / "src" / "i2c_transport.c"),
            str(test_dir / "board_power_service_test.c"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("board-power-service host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
