#!/usr/bin/env python3
"""Compile and run deterministic v0.5.2 OPC UA resource-service tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "opcua_resource_service"
    with tempfile.TemporaryDirectory(prefix="opcua-resource-service-") as directory:
        executable = pathlib.Path(directory) / "opcua_resource_service_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DOPCUA_RESOURCE_SERVICE_TEST_HOOKS",
            f"-I{test_dir}", f"-I{root / 'services' / 'include'}",
            str(root / "services" / "src" / "opcua_resource_service.c"),
            str(test_dir / "opcua_resource_service_test.c"),
            "-o", str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)
    print("v0.5.2 OPC UA resource service host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
