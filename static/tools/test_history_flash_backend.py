#!/usr/bin/env python3
"""Compile and run the Flash-backed history ring host suite (drivers layer).

Covers power loss before/during/after a sample write (via the "empty
restore", "torn record", and "mixed value type round trip after restart"
scenarios respectively), sector-wraparound retention, corrupt/torn-sample
skip behavior, foreign-channel-identity rejection, and interrupted-sector-
swap recovery -- entirely through the injected mock storage callbacks, no
real hardware or open62541 dependency required.
"""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "history_flash_backend"
    with tempfile.TemporaryDirectory(prefix="history-flash-backend-") as directory:
        executable = pathlib.Path(directory) / "history_flash_backend_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'drivers' / 'include'}", f"-I{root / 'services' / 'include'}",
            str(root / "drivers" / "src" / "history_flash_backend.c"),
            str(root / "services" / "src" / "maintenance_journal_codec.c"),
            str(test_dir / "history_flash_backend_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("history flash backend host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
