#!/usr/bin/env python3
"""Compile and run v0.5.2 synthetic benchmark-profile host coverage."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile


def macros(channels: int, rate_hz: int, events: int, history: int) -> list[str]:
    return [
        "-DRP2_BENCHMARK_PROFILE_ENABLED=1",
        f"-DRP2_BENCHMARK_CHANNEL_COUNT={channels}",
        f"-DRP2_BENCHMARK_RATE_HZ={rate_hz}",
        f"-DRP2_BENCHMARK_EVENTS_ENABLED={events}",
        f"-DRP2_BENCHMARK_HISTORY_ENABLED={history}",
        "-DRP2_BENCHMARK_SECURITY_NONE=0",
        "-DRP2_BENCHMARK_CLIENT_COUNT=4",
        f'-DRP2_BENCHMARK_PROFILE_ID=\"synthetic-{channels}-{rate_hz}-{events}-{history}\"',
    ]

def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "benchmark_profile"
    with tempfile.TemporaryDirectory(prefix="benchmark-profile-") as directory:
        disabled_executable = pathlib.Path(directory) / "benchmark_profile_disabled.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            f"-I{root / 'services' / 'include'}", f"-I{root / 'drivers' / 'include'}",
            str(root / "services" / "src" / "benchmark_profile.c"),
            str(test_dir / "benchmark_profile_disabled_test.c"),
            "-o", str(disabled_executable),
        ], check=True)
        subprocess.run([str(disabled_executable)], check=True)

        for channels in (1, 4, 8, 16):
            for rate_hz in (1, 10):
                for events in (0, 1):
                    for history in (0, 1):
                        executable = pathlib.Path(directory) / (
                            f"benchmark_profile_{channels}_{rate_hz}_{events}_{history}.exe"
                        )
                        command = [
                            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            *macros(channels, rate_hz, events, history),
                            f"-I{root / 'services' / 'include'}",
                            f"-I{root / 'drivers' / 'include'}",
                            str(root / "services" / "src" / "benchmark_profile.c"),
                            str(test_dir / "benchmark_profile_test.c"),
                            "-o", str(executable),
                        ]
                        subprocess.run(command, check=True)
                        subprocess.run([str(executable)], check=True)

        config_service_executable = pathlib.Path(directory) / "benchmark_profile_config_service_test.exe"
        config_service_command = [
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-format-truncation",
            *macros(16, 10, 1, 1),
            f"-I{root / 'static' / 'tests' / 'config_service_identity'}",
            f"-I{root / 'services' / 'include'}",
            f"-I{root / 'services' / 'src'}",
            f"-I{root / 'drivers' / 'include'}",
            str(root / "services" / "src" / "benchmark_profile.c"),
            str(root / "services" / "src" / "config_service.c"),
            str(test_dir / "benchmark_profile_config_service_test.c"),
            "-o", str(config_service_executable),
        ]
        subprocess.run(config_service_command, check=True)
        subprocess.run([str(config_service_executable)], check=True)

        synthetic_executable = pathlib.Path(directory) / "benchmark_profile_synthetic_independence_test.exe"
        subprocess.run([
            "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", *macros(16, 10, 1, 1),
            f"-I{root / 'services' / 'include'}", f"-I{root / 'drivers' / 'include'}",
            str(root / "services" / "src" / "benchmark_profile.c"),
            str(root / "drivers" / "src" / "synthetic_driver.c"),
            str(test_dir / "benchmark_profile_synthetic_independence_test.c"),
            "-o", str(synthetic_executable),
        ], check=True)
        subprocess.run([str(synthetic_executable)], check=True)

        invalid = subprocess.run([
            "gcc", "-std=c11", "-Werror", "-DRP2_BENCHMARK_CHANNEL_COUNT=2",
            f"-I{root / 'services' / 'include'}", f"-I{root / 'drivers' / 'include'}",
            "-c", str(root / "services" / "src" / "benchmark_profile.c"),
            "-o", str(pathlib.Path(directory) / "invalid_profile.o"),
        ], capture_output=True, text=True)
        if invalid.returncode == 0:
            raise AssertionError("invalid benchmark macro unexpectedly compiled")
    print("benchmark profile host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
