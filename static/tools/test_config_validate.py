#!/usr/bin/env python3
"""Compile and run bounded staged-configuration validation host tests."""

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
    test_dir = root / "static" / "tests" / "config_validate"
    with tempfile.TemporaryDirectory(prefix="config-validate-") as directory:
        executable = pathlib.Path(directory) / "config_validate_test.exe"
        subprocess.run([
            args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}",
            f"-I{root / 'services' / 'src'}", f"-I{root / 'ota' / 'include'}",
            str(root / "services" / "src" / "config_import_service.c"),
            str(root / "services" / "src" / "config_compat.c"),
            str(root / "services" / "src" / "config_export_service.c"),
            str(root / "services" / "src" / "config_validate_service.c"),
            str(root / "ota" / "src" / "ota_state.c"),
            str(test_dir / "config_validate_test.c"), "-lm", "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("staged configuration validation host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
