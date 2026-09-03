#!/usr/bin/env python3
"""Compile and run synchronous config/runtime transaction regression coverage."""
from __future__ import annotations
import pathlib
import subprocess
import tempfile

def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "config_transaction"
    with tempfile.TemporaryDirectory(prefix="config-transaction-") as directory:
        executable = pathlib.Path(directory) / "config_transaction_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}", f"-I{root / 'services' / 'src'}",
            f"-I{root / 'drivers' / 'include'}", f"-I{root / 'port' / 'psram'}",
            str(root / "services" / "src" / "config_service.c"),
            str(root / "services" / "src" / "config_compat.c"),
            str(root / "services" / "src" / "config_presets.c"),
            str(root / "services" / "src" / "channel_manager.c"),
            str(root / "services" / "src" / "sensor_manager.c"),
            str(test_dir / "config_transaction_test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("config/runtime transaction host suite: PASS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
