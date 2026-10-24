#!/usr/bin/env python3
"""Compile and run deterministic v0.5.2 native diagnostic-service tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "diag_service"
    source = (root / "services" / "src" / "diag_service.c").read_text(encoding="utf-8")
    assert '#define DIAG_SAMPLING_SCRATCH_SECTION __scratch_y("diag_sampling")' in source
    assert "g_sampling_scratch DIAG_SAMPLING_SCRATCH_SECTION" in source
    assert "sizeof(diag_sampling_scratch_t) <= 2048u" in source
    with tempfile.TemporaryDirectory(prefix="diag-service-") as directory:
        executable = pathlib.Path(directory) / "diag_service_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "diag_service.c"),
            str(test_dir / "diag_service_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("v0.5.2 diagnostic service host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
