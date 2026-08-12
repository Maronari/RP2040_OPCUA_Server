#!/usr/bin/env python3
"""Compile and run the host-only bounded writable loopback lifecycle suite."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "channel_runtime"
    with tempfile.TemporaryDirectory(prefix="channel-runtime-") as directory:
        executable = pathlib.Path(directory) / "channel_runtime_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}", f"-I{root / 'port' / 'psram'}",
            str(root / "services" / "src" / "channel_manager.c"),
            str(test_dir / "channel_runtime_test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("channel runtime loopback host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
