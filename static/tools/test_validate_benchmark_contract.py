#!/usr/bin/env python3
"""Host tests for the v0.5.2 measurement-contract validator."""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib


TOOL_DIR = pathlib.Path(__file__).resolve().parent
CONTRACT_PATH = TOOL_DIR.parent / "benchmarks" / "v0.5.2" / "measurement-contract.json"
SPEC = importlib.util.spec_from_file_location(
    "validate_benchmark_contract", TOOL_DIR / "validate_benchmark_contract.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def expect_invalid(document, fragment: str) -> None:
    try:
        MODULE.validate_contract(document)
    except MODULE.ContractError as exc:
        assert fragment in str(exc), (fragment, str(exc))
    else:
        raise AssertionError(f"invalid contract unexpectedly passed: {fragment}")


def main() -> None:
    document = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))
    summary = MODULE.validate_contract(document)
    assert summary["profileCount"] == 192
    assert summary["metricCount"] >= 100
    assert summary["unsupportedCount"] == 1

    broken = copy.deepcopy(document)
    del broken["metricFamilies"][0]["synchronization"]
    expect_invalid(broken, "missing")

    broken = copy.deepcopy(document)
    broken["metricFamilies"][1]["metrics"].append(
        broken["metricFamilies"][0]["metrics"][0]
    )
    expect_invalid(broken, "duplicate metric id")

    broken = copy.deepcopy(document)
    del broken["metricFamilies"][2]["unitByMetric"]["Heap.TotalBytes"]
    expect_invalid(broken, "map every metric")

    broken = copy.deepcopy(document)
    broken["bounds"]["maxRunSeconds"] = 0
    expect_invalid(broken, "positive integer")

    broken = copy.deepcopy(document)
    broken["bounds"]["maxProfiles"] = 191
    expect_invalid(broken, "above maxProfiles")

    broken = copy.deepcopy(document)
    broken["benchmarkMatrix"]["axes"]["namespaceZero"].append("reduced")
    expect_invalid(broken, "axes drifted")

    broken = copy.deepcopy(document)
    broken["benchmarkMatrix"]["unsupported"][0]["value"] = "full"
    expect_invalid(broken, "also claimed as supported")

    broken = copy.deepcopy(document)
    broken["requiredRunAccounting"].remove("droppedSampleCount")
    expect_invalid(broken, "silent loss")

    print("benchmark measurement contract suite: PASS")


if __name__ == "__main__":
    main()
