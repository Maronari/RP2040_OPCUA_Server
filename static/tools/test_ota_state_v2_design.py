#!/usr/bin/env python3
"""Compile and run the host-only T0 OTA-state-v2 byte-codec prototype."""

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
    test_dir = root / "static" / "tests" / "ota_state_v2_design"

    with tempfile.TemporaryDirectory(prefix="ota-state-v2-") as temporary:
        executable = pathlib.Path(temporary) / "ota_state_v2_design_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-DNDEBUG",
            f"-I{test_dir}",
            str(test_dir / "ota_state_v2_design.c"),
            str(test_dir / "ota_state_v2_design_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("OTA state v2 byte-codec host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
