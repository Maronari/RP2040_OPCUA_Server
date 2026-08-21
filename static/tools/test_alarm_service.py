#!/usr/bin/env python3
"""Compile and run the v0.4.3 per-channel alarm_service host suite.

Runs the same test binary twice: once at the reference PLATFORM_CHANNEL_LIMIT
(16) for normal multi-channel coverage, and once at a deliberately small
limit (4) so the resource-limit / overflow-safety assertions in the test are
actually exercised (the default limit is larger than the test's synthetic
8-channel overflow scenario, so it would otherwise never hit the bound).
"""

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
    test_dir = root / "static" / "tests" / "alarm_service"

    with tempfile.TemporaryDirectory(prefix="alarm-service-") as directory:
        for limit in (16, 4):
            executable = pathlib.Path(directory) / f"alarm_service_test_{limit}.exe"
            subprocess.run([
                args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DPLATFORM_CHANNEL_LIMIT={limit}",
                f"-I{test_dir}",
                f"-I{root / 'services' / 'include'}",
                f"-I{root / 'port' / 'psram'}",
                str(root / "services" / "src" / "alarm_service.c"),
                str(test_dir / "alarm_service_test.c"),
                "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)

    print("alarm service host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
