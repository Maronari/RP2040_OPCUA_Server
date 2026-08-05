#!/usr/bin/env python3
"""Compile and run the persisted-config runtime-identity regression test."""

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
    test_dir = root / "static" / "tests" / "config_service_identity"
    with tempfile.TemporaryDirectory(prefix="config-service-identity-") as directory:
        executable = pathlib.Path(directory) / "config_service_identity_test.exe"
        command = [
            args.cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            # Existing bounded default-string formatting is diagnosed by host
            # GCC, although the RP2350 build does not enable this warning.
            "-Wno-format-truncation",
            f"-I{test_dir}",
            f"-I{root / 'services' / 'include'}",
            f"-I{root / 'services' / 'src'}",
            f"-I{root / 'drivers' / 'include'}",
            str(root / "services" / "src" / "config_service.c"),
            str(test_dir / "config_service_identity_test.c"),
            "-o",
            str(executable),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)

    print("persisted config runtime identity regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
