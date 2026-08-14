#!/usr/bin/env python3
"""Compile and run frozen v0.4.0 configuration migration tests."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="config-compat-") as directory:
        for limit in (1, 4, 16):
            executable = pathlib.Path(directory) / f"config_compat_test_{limit}.exe"
            subprocess.run([
                args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DPLATFORM_CHANNEL_LIMIT={limit}",
                f"-I{root / 'services' / 'include'}",
                str(root / "services" / "src" / "config_compat.c"),
                str(root / "static" / "tests" / "config_compat" / "config_compat_test.c"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

        for invalid in (0, 17):
            result = subprocess.run([
                args.cc, "-std=c11", f"-DPLATFORM_CHANNEL_LIMIT={invalid}",
                f"-I{root / 'services' / 'include'}", "-x", "c", "-", "-c",
                "-o", str(pathlib.Path(directory) / f"invalid_{invalid}.o"),
            ], input='#include "services/platform_limits.h"\n',
               text=True, capture_output=True, check=False)
            if result.returncode == 0:
                raise SystemExit(f"invalid channel limit {invalid} unexpectedly compiled")
    print("v0.4.0 config migration host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
