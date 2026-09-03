#!/usr/bin/env python3
"""Compile and run the v0.5.0 T9 reboot-recovery host suite.

Links the REAL services/config_service.c -> services/config_store_service.c
-> drivers/config_transaction_backend.c chain against one simulated Flash
media, plus real services/sensor_manager.c driver activation against a fake
telemetry driver -- proving reboot recovery end to end rather than only
through fakes at the config_store_service.h boundary (that narrower coverage
already exists in test_config_transaction.py/test_config_transaction_backend.py).
"""
from __future__ import annotations
import pathlib
import subprocess
import tempfile

def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "config_transaction"
    with tempfile.TemporaryDirectory(prefix="config-transaction-reboot-recovery-") as directory:
        executable = pathlib.Path(directory) / "config_transaction_reboot_recovery_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}", f"-I{root / 'services' / 'src'}",
            f"-I{root / 'drivers' / 'include'}", f"-I{root / 'port' / 'psram'}",
            str(root / "services" / "src" / "config_service.c"),
            str(root / "services" / "src" / "config_compat.c"),
            str(root / "services" / "src" / "config_store_service.c"),
            str(root / "services" / "src" / "channel_manager.c"),
            str(root / "services" / "src" / "sensor_manager.c"),
            str(root / "drivers" / "src" / "config_transaction_backend.c"),
            str(test_dir / "config_transaction_reboot_recovery_test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("config transaction reboot-recovery host suite: PASS")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
