#!/usr/bin/env python3
"""Compile and run bounded history ownership and microsecond-timestamp tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "history_service"
    with tempfile.TemporaryDirectory(prefix="history-service-") as directory:
        for name, defines in (("host", []), ("device-allocation", ["-DPICO_ON_DEVICE=1"])):
            executable = pathlib.Path(directory) / f"history_service_{name}.exe"
            subprocess.run([
                "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", *defines,
                f"-I{test_dir}", f"-I{root / 'services' / 'include'}", f"-I{root / 'port' / 'psram'}",
                f"-I{root / 'drivers' / 'include'}",
                str(root / "services" / "src" / "history_service.c"),
                str(root / "services" / "src" / "channel_manager.c"),
                str(test_dir / "history_service_test.c"), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)
    print("history service ownership/timestamp host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
