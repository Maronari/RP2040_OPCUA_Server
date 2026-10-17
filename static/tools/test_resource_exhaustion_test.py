#!/usr/bin/env python3
"""Compile and run the bounded native resource-exhaustion runner tests."""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "resource_exhaustion_test"
    with tempfile.TemporaryDirectory(prefix="resource-exhaustion-") as directory:
        executable = pathlib.Path(directory) / "resource-exhaustion-test.exe"
        evidence = pathlib.Path(directory) / "resource-exhaustion.json"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DRP2_RESOURCE_EXHAUSTION_TEST_ENABLED=1",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "resource_exhaustion_test.c"),
            str(test_dir / "resource_exhaustion_test.c"), "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable), str(evidence)], check=True)
        document = json.loads(evidence.read_text(encoding="utf-8"))
        assert document["schemaVersion"] == 1
        assert document["kind"] == "v052-resource-exhaustion"
        assert document["profileId"] == "native-test"
        assert document["allPassed"] is True
        assert document["heap"]["completedCycles"] == 100
        assert document["psram"]["completedCycles"] == 100
        assert document["heap"]["initial"]["peakUsedBytes"] == 7072
        assert document["psram"]["final"]["peakUsedBytes"] == 7072
        assert document["initTaskStackHighWaterStartBytes"] == 0
        assert document["initTaskStackHighWaterEndBytes"] == 0
        assert document["durationMs"] == document["endMonotonicMs"] - document["startMonotonicMs"]
        assert len(evidence.read_bytes()) <= 4096
        disabled = pathlib.Path(directory) / "resource-exhaustion-disabled.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "resource_exhaustion_test.c"),
            str(test_dir / "resource_exhaustion_test_disabled.c"), "-o", str(disabled),
        ], check=True)
        subprocess.run([str(disabled)], check=True)
    print("resource exhaustion native suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
