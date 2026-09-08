#!/usr/bin/env python3
"""Compile and run the generic SPI transport framing host suite (v0.5.1 T2)."""

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
    test_dir = root / "static" / "tests" / "spi_transport"
    with tempfile.TemporaryDirectory(prefix="spi-transport-") as directory:
        executable = pathlib.Path(directory) / "spi_transport_test.exe"
        command = [
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'drivers' / 'include'}",
            str(root / "drivers" / "src" / "spi_transport.c"),
            str(test_dir / "spi_transport_test.c"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
        backend = pathlib.Path(directory) / "rp2350_spi_bus_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{test_dir}", f"-I{root / 'drivers' / 'include'}",
            str(root / "drivers" / "src" / "spi_transport.c"),
            str(root / "drivers" / "src" / "rp2350_spi_bus.c"),
            str(test_dir / "rp2350_spi_bus_test.c"), "-o", str(backend),
        ], check=True)
        subprocess.run([str(backend)], check=True)

    print("SPI transport framing host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
