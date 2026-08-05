#!/usr/bin/env python3
"""Compile and run deterministic RP2 OPC UA DateTime conversion tests."""

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
    source = root / "port" / "open62541" / "rp2_ua_datetime_clock.c"
    test = root / "static" / "tests" / "rp2_ua_datetime_clock" / "rp2_ua_datetime_clock_test.c"
    with tempfile.TemporaryDirectory(prefix="rp2-ua-datetime-") as directory:
        executable = pathlib.Path(directory) / "rp2_ua_datetime_clock_test.exe"
        subprocess.run(
            [
                args.cc,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{root / 'port' / 'open62541'}",
                f"-I{root / 'services' / 'include'}",
                str(source),
                str(test),
                "-o",
                str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True)

    print("RP2 OPC UA DateTime host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
