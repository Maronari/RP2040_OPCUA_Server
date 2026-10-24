#!/usr/bin/env python3
"""Validate the bounded v0.5.2 measurement and benchmark contract."""

from __future__ import annotations

import argparse
import itertools
import json
import pathlib
import sys
from typing import Any


EXPECTED_SCHEMA_VERSION = 1
REQUIRED_FAMILY_FIELDS = {
    "id",
    "scope",
    "metrics",
    "owner",
    "unit",
    "kind",
    "lifetime",
    "reset",
    "overflow",
    "synchronization",
    "availability",
    "implementationTask",
}
REQUIRED_AXES = (
    "channels",
    "namespaceZero",
    "security",
    "clients",
    "rateHz",
    "events",
    "history",
)
REQUIRED_BOUNDS = {
    "maxProfiles",
    "maxRepetitionsPerProfile",
    "maxWarmupSeconds",
    "maxRunSeconds",
    "operationTimeoutMs",
    "connectTimeoutMs",
    "maxRetriesPerOperation",
    "maxOperationSamplesPerMetric",
    "maxNotificationSamplesPerMetric",
    "maxPendingCorrelations",
    "maxHistoryContinuationPointsPerClient",
    "maxErrorsPerRun",
    "maxErrorTextBytes",
    "maxJsonBytes",
}
REQUIRED_RESULT_STATES = {
    "measured",
    "unsupported",
    "unavailable",
    "exhausted",
    "failed",
    "not-run",
}


class ContractError(ValueError):
    """Raised when the contract is missing or internally inconsistent."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def _nonempty_text(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def validate_contract(document: dict[str, Any]) -> dict[str, int]:
    _require(document.get("schemaVersion") == EXPECTED_SCHEMA_VERSION,
             f"schemaVersion must be {EXPECTED_SCHEMA_VERSION}")
    _require(document.get("reportSchemaVersion") == 1,
             "reportSchemaVersion must be 1")
    _require(document.get("profileSchemaVersion") == 1,
             "profileSchemaVersion must be 1")
    _require(document.get("release") == "0.5.2", "release must be 0.5.2")
    _require(set(document.get("resultStates", ())) == REQUIRED_RESULT_STATES,
             "resultStates must distinguish measured/unsupported/unavailable/exhausted/failed/not-run")

    bounds = document.get("bounds")
    _require(isinstance(bounds, dict), "bounds must be an object")
    _require(REQUIRED_BOUNDS <= bounds.keys(), "bounds is missing required finite limits")
    for name in REQUIRED_BOUNDS:
        value = bounds[name]
        _require(isinstance(value, int) and not isinstance(value, bool) and value > 0,
                 f"bounds.{name} must be a positive integer")

    families = document.get("metricFamilies")
    _require(isinstance(families, list) and families, "metricFamilies must be a non-empty array")
    family_ids: set[str] = set()
    metric_ids: set[str] = set()
    for index, family in enumerate(families):
        _require(isinstance(family, dict), f"metricFamilies[{index}] must be an object")
        missing = REQUIRED_FAMILY_FIELDS - family.keys()
        _require(not missing, f"metricFamilies[{index}] missing {sorted(missing)}")
        family_id = family["id"]
        _require(_nonempty_text(family_id), f"metricFamilies[{index}].id must be non-empty")
        _require(family_id not in family_ids, f"duplicate metric family id: {family_id}")
        family_ids.add(family_id)
        for field in REQUIRED_FAMILY_FIELDS - {"metrics"}:
            _require(_nonempty_text(family[field]),
                     f"metric family {family_id}.{field} must be non-empty")
        metrics = family["metrics"]
        _require(isinstance(metrics, list) and metrics,
                 f"metric family {family_id} must contain metrics")
        for metric in metrics:
            _require(_nonempty_text(metric), f"metric family {family_id} has an empty metric")
            _require(metric not in metric_ids, f"duplicate metric id: {metric}")
            metric_ids.add(metric)
        if family["unit"] == "mixed":
            unit_map = family.get("unitByMetric")
            _require(isinstance(unit_map, dict),
                     f"mixed-unit metric family {family_id} requires unitByMetric")
            _require(set(unit_map) == set(metrics),
                     f"mixed-unit metric family {family_id} must map every metric exactly once")
            for metric, unit in unit_map.items():
                _require(_nonempty_text(unit),
                         f"mixed-unit metric family {family_id} has no unit for {metric}")

    matrix = document.get("benchmarkMatrix")
    _require(isinstance(matrix, dict), "benchmarkMatrix must be an object")
    axes = matrix.get("axes")
    _require(isinstance(axes, dict), "benchmarkMatrix.axes must be an object")
    _require(tuple(axes.keys()) == REQUIRED_AXES,
             f"benchmarkMatrix.axes must be ordered and complete: {REQUIRED_AXES}")
    for axis, values in axes.items():
        _require(isinstance(values, list) and values, f"matrix axis {axis} must be non-empty")
        _require(len(values) == len({json.dumps(value, sort_keys=True) for value in values}),
                 f"matrix axis {axis} contains duplicates")

    expected_axes = {
        "channels": [1, 4, 8, 16],
        "namespaceZero": ["full"],
        "security": ["basic256sha256-signandencrypt", "test-none"],
        "clients": [1, 2, 4],
        "rateHz": [1, 10],
        "events": [False, True],
        "history": [False, True],
    }
    _require(axes == expected_axes, "benchmark matrix axes drifted from the frozen T0 contract")
    profile_count = 1
    for values in axes.values():
        profile_count *= len(values)
    _require(profile_count <= bounds["maxProfiles"],
             f"matrix expands to {profile_count} profiles above maxProfiles={bounds['maxProfiles']}")

    unsupported = matrix.get("unsupported")
    _require(isinstance(unsupported, list) and unsupported,
             "benchmarkMatrix.unsupported must explicitly list unsupported cells")
    unsupported_pairs: set[tuple[str, str]] = set()
    for entry in unsupported:
        _require(isinstance(entry, dict), "unsupported entry must be an object")
        axis = entry.get("axis")
        value = entry.get("value")
        reason = entry.get("reason")
        _require(axis in axes, f"unsupported entry refers to unknown axis: {axis}")
        _require(_nonempty_text(str(value)) and _nonempty_text(reason),
                 "unsupported entry requires value and reason")
        _require(value not in axes[axis],
                 f"unsupported {axis}={value!r} is also claimed as supported")
        pair = (axis, json.dumps(value, sort_keys=True))
        _require(pair not in unsupported_pairs, f"duplicate unsupported cell: {axis}={value!r}")
        unsupported_pairs.add(pair)

    template = matrix.get("profileIdTemplate")
    _require(_nonempty_text(template), "profileIdTemplate must be non-empty")
    for axis in REQUIRED_AXES:
        _require("{" + axis + "}" in template,
                 f"profileIdTemplate is missing {{{axis}}}")
    generated_ids: set[str] = set()
    for values in itertools.product(*(axes[axis] for axis in REQUIRED_AXES)):
        fields = dict(zip(REQUIRED_AXES, values))
        fields["events"] = int(fields["events"])
        fields["history"] = int(fields["history"])
        profile_id = template.format(**fields)
        _require(profile_id not in generated_ids, f"profile id collision: {profile_id}")
        generated_ids.add(profile_id)

    required_metadata = document.get("requiredSetupMetadata")
    required_accounting = document.get("requiredRunAccounting")
    _require(isinstance(required_metadata, list) and len(required_metadata) == len(set(required_metadata)),
             "requiredSetupMetadata must be a unique array")
    _require(isinstance(required_accounting, list) and len(required_accounting) == len(set(required_accounting)),
             "requiredRunAccounting must be a unique array")
    _require({"gitCommit", "gitDirty", "timeSynchronization", "configuredCapacities"}
             <= set(required_metadata), "required setup provenance is incomplete")
    _require({"sampleCount", "errorCount", "droppedSampleCount", "timedOutOperationCount"}
             <= set(required_accounting), "run accounting cannot detect silent loss")

    return {
        "familyCount": len(family_ids),
        "metricCount": len(metric_ids),
        "profileCount": profile_count,
        "unsupportedCount": len(unsupported_pairs),
    }


def parse_args() -> argparse.Namespace:
    default_contract = pathlib.Path(__file__).resolve().parents[1] / "benchmarks" / "v0.5.2" / "measurement-contract.json"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("contract", nargs="?", type=pathlib.Path, default=default_contract)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        raw = args.contract.read_bytes()
        document = json.loads(raw)
        _require(len(raw) <= document["bounds"]["maxJsonBytes"],
                 "contract itself exceeds bounds.maxJsonBytes")
        summary = validate_contract(document)
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ContractError) as exc:
        print(f"benchmark-contract: FAIL: {exc}", file=sys.stderr)
        return 1
    print("benchmark-contract: PASS " + " ".join(f"{key}={value}" for key, value in summary.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
