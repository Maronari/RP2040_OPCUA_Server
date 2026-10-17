#!/usr/bin/env python3
"""Compile and run benchmark-only native sampling timing tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import importlib.util
import json
import sys


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def load_client_module(root: pathlib.Path):
    specification = importlib.util.spec_from_file_location("load_test_opcua", root / "static" / "tools" / "load_test_opcua.py")
    assert specification and specification.loader
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def main() -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    test_dir = root / "static" / "tests" / "benchmark_timing"
    channel_test_dir = root / "static" / "tests" / "channel_manager"
    diag_source = (root / "services" / "src" / "diag_service.c").read_text(encoding="utf-8")
    diag_sample = diag_source[diag_source.index("bool diag_service_sample("):]
    assert '#include "services/benchmark_timing.h"' in diag_source
    assert "benchmark_timing_diag_sample_begin()" in diag_sample
    assert diag_sample.count("benchmark_timing_diag_sample_set_opcua_task_stack_high_water") == 4
    assert "benchmark_timing_diag_sample_set_phase_durations(benchmark_phase_durations_us)" in diag_sample
    assert "benchmark_timing_diag_sample_end(\n            benchmark_started_monotonic_us, false);\n        benchmark_timing_diag_sample_set_opcua_task_stack_high_water(\n            (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));" in diag_sample
    assert "benchmark_timing_diag_sample_end(\n        benchmark_started_monotonic_us, true);\n    benchmark_timing_diag_sample_set_opcua_task_stack_high_water(\n        (uint64_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));" in diag_sample
    common = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
              f"-I{test_dir}", f"-I{channel_test_dir}",
              f"-I{root / 'services' / 'include'}", f"-I{root / 'port' / 'psram'}"]
    with tempfile.TemporaryDirectory(prefix="benchmark-timing-") as directory:
        enabled = pathlib.Path(directory) / "enabled.exe"
        run(common + [
            "-DRP2_BENCHMARK_PROFILE_ENABLED=1", "-DRP2_BENCHMARK_CHANNEL_COUNT=2",
            "-DRP2_BENCHMARK_RATE_HZ=1", '-DRP2_BENCHMARK_PROFILE_ID="native-test"',
            str(root / "services" / "src" / "channel_manager.c"),
            str(root / "services" / "src" / "benchmark_timing.c"),
            str(test_dir / "benchmark_timing_test.c"), "-o", str(enabled),
        ])
        native_json = pathlib.Path(directory) / "native-evidence.json"
        resource_budget_json = pathlib.Path(directory) / "resource-budget-evidence.json"
        run([str(enabled), str(native_json), str(resource_budget_json)])
        document = json.loads(native_json.read_text(encoding="utf-8"))
        load_client_module(root).validate_native_evidence(document, "native-test", 10, 30)
        budget_document = json.loads(resource_budget_json.read_text(encoding="utf-8"))
        assert budget_document["schemaVersion"] == 1
        assert budget_document["kind"] == "v052-diag-snapshot-budget-evidence"
        assert budget_document["state"] == "complete"
        assert budget_document["profileId"] == "native-test"
        assert budget_document["sampleTarget"] == 100
        assert budget_document["sampleCount"] == 100
        assert budget_document["diagServiceSampleDurationUs"] == {"p99": 99, "max": 100}
        assert budget_document["opcUaTaskStackHighWaterBytes"] == 8092
        assert budget_document["phaseMaxUs"] == {
            "acquire": 100, "copy": 101, "heap": 102, "psram": 103,
            "lwip": 104, "tasks": 105, "publish": 106,
        }
        assert budget_document["maxSamplePhaseUs"] == budget_document["phaseMaxUs"]
        assert budget_document["baselineComparisonState"] == "not-run"
        assert len(resource_budget_json.read_bytes()) <= 2048
        capacity = pathlib.Path(directory) / "capacity.exe"
        run(common + [
            "-DRP2_BENCHMARK_PROFILE_ENABLED=1", "-DRP2_BENCHMARK_CHANNEL_COUNT=16",
            "-DRP2_BENCHMARK_RATE_HZ=10", "-DRP2_BENCHMARK_TIMING_CAPTURE_SECONDS=900",
            '-DRP2_BENCHMARK_PROFILE_ID="capacity-test"',
            str(root / "services" / "src" / "channel_manager.c"),
            str(root / "services" / "src" / "benchmark_timing.c"),
            str(test_dir / "benchmark_timing_capacity_test.c"), "-o", str(capacity),
        ])
        run([str(capacity)])
        allocation_failure = pathlib.Path(directory) / "allocation-failure.exe"
        run(common + [
            "-DRP2_BENCHMARK_PROFILE_ENABLED=1", "-DRP2_BENCHMARK_CHANNEL_COUNT=1",
            "-DRP2_BENCHMARK_RATE_HZ=1", '-DRP2_BENCHMARK_PROFILE_ID="allocation-test"',
            str(root / "services" / "src" / "benchmark_timing.c"),
            str(test_dir / "benchmark_timing_allocation_failure_test.c"), "-o", str(allocation_failure),
        ])
        run([str(allocation_failure)])
        disabled = pathlib.Path(directory) / "disabled.exe"
        run(common + [
            "-DRP2_BENCHMARK_PROFILE_ENABLED=0",
            str(root / "services" / "src" / "benchmark_timing.c"),
            str(test_dir / "benchmark_timing_disabled_test.c"), "-o", str(disabled),
        ])
        run([str(disabled)])
    print("benchmark timing suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
