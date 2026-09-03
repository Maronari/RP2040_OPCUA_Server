#!/usr/bin/env python3
"""Compile and run the LAST_KNOWN_GOOD rollback/runtime-apply regression test."""

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
    test_dir = root / "static" / "tests" / "config_service_rollback"
    with tempfile.TemporaryDirectory(prefix="config-service-rollback-") as directory:
        executable = pathlib.Path(directory) / "config_service_rollback_test.exe"
        command = [
            args.cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-format-truncation",
            f"-I{test_dir}",
            f"-I{root / 'services' / 'include'}",
            f"-I{root / 'services' / 'src'}",
            f"-I{root / 'drivers' / 'include'}",
            str(root / "services" / "src" / "config_service.c"),
            str(root / "services" / "src" / "config_compat.c"),
            str(test_dir / "config_service_rollback_test.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("config-service LAST_KNOWN_GOOD rollback/runtime-apply regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
