#!/usr/bin/env python3
"""Validate the bounded v0.5.2 hardware baseline and regression thresholds."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

import benchmark_profiles
import load_test_opcua

MAX_JSON_BYTES = 8 * 1024 * 1024
REQUIRED_METRICS = {
    "ReadLatencyMs.p99": "milliseconds",
    "SecureSessionEstablishmentLatencyMs.Cold.max": "milliseconds",
    "SecureSessionEstablishmentLatencyMs.Warm.max": "milliseconds",
    "SamplingJitterMs.p99": "milliseconds",
    "Heap.PeakUsedBytes": "bytes",
    "PSRAM.PeakUsedBytes": "bytes",
}
STATIC_TOTALS = {
    "ElfFileBytes", "Uf2FileBytes", "BinFileBytes",
    "ExternalFlashLoadBytes", "SramStaticTotalBytes",
}


class BaselineError(ValueError):
    pass


def read_json(path: Path) -> dict[str, Any]:
    with path.open("rb") as stream:
        raw = stream.read(MAX_JSON_BYTES + 1)
    if not raw or len(raw) > MAX_JSON_BYTES:
        raise BaselineError(f"{path}: empty or exceeds {MAX_JSON_BYTES} bytes")
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise BaselineError(f"{path}: expected JSON object")
    return value


def require(condition: bool, message: str) -> None:
    if not condition:
        raise BaselineError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_environment(environment: Any, release: str, prefix: str = "environment") -> None:
    require(isinstance(environment, dict), f"{prefix} is required")
    for key in ("recordedAt", "board", "boardId", "clientBoardRevision", "network",
                "toolchain", "clientToolchain", "compiler", "buildType", "firmwareVersion",
                "firmwareSource", "gitCommit", "firmwareUf2Sha256"):
        require(isinstance(environment.get(key), str) and environment[key],
                f"{prefix}.{key} is required")
    require(environment.get("gitDirty") is True,
            f"{prefix} must disclose the dirty source image")
    require(environment.get("firmwareVersion") == release,
            f"{prefix} firmware version differs from release")


def validate_evidence(root: Path, entry: dict[str, Any], environment: dict[str, Any]) -> None:
    evidence = entry.get("evidence")
    require(isinstance(evidence, list) and 3 <= len(evidence) <= 8,
            f"{entry.get('profileId')}: expected 3..8 evidence files")
    seen: set[str] = set()
    documents: dict[str, dict[str, Any]] = {}
    for item in evidence:
        require(isinstance(item, dict), "evidence entry must be an object")
        kind, relative, expected = item.get("kind"), item.get("path"), item.get("sha256")
        require(kind in {"client", "native", "resources", "boot-clock", "static-size"}
                and kind not in documents, f"unknown or duplicate evidence kind: {kind}")
        require(isinstance(relative, str) and relative.startswith("static/benchmarks/v0.5.2/evidence/"),
                "evidence path must remain under static/benchmarks/v0.5.2/evidence")
        require(relative not in seen, f"duplicate evidence path: {relative}")
        seen.add(relative)
        require(isinstance(expected, str) and len(expected) == 64,
                f"{relative}: invalid SHA-256")
        path = root / relative
        require(path.is_file(), f"missing evidence file: {relative}")
        require(sha256(path) == expected.lower(), f"evidence hash mismatch: {relative}")
        documents[kind] = read_json(path)
    required_documents = {"client", "native", "resources", "static-size"}
    require(required_documents <= set(documents),
            f"{entry.get('profileId')}: evidence kinds are incomplete")

    profile_id = entry["profileId"]
    metrics = entry["metrics"]
    client = documents["client"]
    require(client.get("schemaVersion") == 1 and client.get("profileId") == profile_id,
            f"{profile_id}: client evidence identity mismatch")
    for client_key, environment_key in (
        ("gitCommit", "gitCommit"), ("gitDirty", "gitDirty"),
        ("firmwareVersion", "firmwareVersion"), ("buildType", "buildType"),
        ("boardRevision", "clientBoardRevision"), ("toolchain", "clientToolchain"),
    ):
        require(client.get(client_key) == environment[environment_key],
                f"{profile_id}: client provenance {client_key} mismatch")
    require(client.get("repetitions") == entry["repetitions"],
            f"{profile_id}: client evidence repetitions mismatch")
    accounting = client.get("loadAccounting", {})
    if entry["resultState"] == "measured":
        require(client.get("errors") == [] and client.get("errorRecordOverflow") == 0,
                f"{profile_id}: measured client evidence contains errors")
        require(accounting.get("scheduledOperationCount") == accounting.get("completedOperationCount")
                and accounting.get("missedDeadlineCount") == 0
                and accounting.get("deadlineExceededOperationCount") == 0
                and accounting.get("failedOperationCount") == 0,
                f"{profile_id}: incomplete client workload")
        require(client.get("metrics", {}).get("ReadLatencyMs", {}).get("state") == "measured",
                f"{profile_id}: measured Read metric state mismatch")
    else:
        errors = client.get("errors")
        overflow = client.get("errorRecordOverflow")
        require((isinstance(errors, list) and bool(errors))
                or (type(overflow) is int and overflow > 0),
                f"{profile_id}: failed client evidence has no recorded failure")
        expected_accounting = entry["loadAccounting"]
        accounting_keys = {
            "scheduled": "scheduledOperationCount",
            "completed": "completedOperationCount",
            "missed": "missedDeadlineCount",
            "deadlineExceeded": "deadlineExceededOperationCount",
            "failed": "failedOperationCount",
        }
        for baseline_key, evidence_key in accounting_keys.items():
            require(accounting.get(evidence_key) == expected_accounting[baseline_key],
                    f"{profile_id}: failed load accounting {baseline_key} mismatch")
        require(expected_accounting["scheduled"] == expected_accounting["completed"]
                + expected_accounting["missed"] + expected_accounting["deadlineExceeded"]
                + expected_accounting["failed"],
                f"{profile_id}: failed load accounting is inconsistent")
        require(client.get("metrics", {}).get("ReadLatencyMs", {}).get("state") == "failed",
                f"{profile_id}: failed Read metric state mismatch")
    for name in ("ReadLatencyMs.p99", "SecureSessionEstablishmentLatencyMs.Cold.max",
                 "SecureSessionEstablishmentLatencyMs.Warm.max"):
        family, statistic = name.rsplit(".", 1)
        require(client.get("metrics", {}).get(family, {}).get(statistic) == metrics[name]["value"],
                f"{profile_id}: {name} differs from client evidence")

    native = documents["native"]
    jitter = load_test_opcua.native_jitter_metrics(
        native, client["samplePeriodMs"], 8192, profile_id,
        client["warmupSeconds"], client["durationSeconds"],
        set(client["clientLoad"]["channelIds"]),
    )
    jitter_p99 = max(value["samplingJitterMs"]["p99"] for value in jitter.values())
    require(jitter_p99 == metrics["SamplingJitterMs.p99"]["value"],
            f"{profile_id}: sampling jitter differs from native evidence")

    resources = documents["resources"]
    require(resources.get("schemaVersion") == 1 and resources.get("profileId") == profile_id,
            f"{profile_id}: resource evidence identity mismatch")
    samples = resources.get("samples")
    require(isinstance(samples, list) and 1 <= len(samples) <= 120,
            f"{profile_id}: invalid resource sample count")
    for metric_name, node_name in (
        ("Heap.PeakUsedBytes", "Device.Diagnostics.Heap.PeakUsedBytes"),
        ("PSRAM.PeakUsedBytes", "Device.Diagnostics.PSRAM.PeakUsedBytes"),
    ):
        values = [sample[node_name]["value"] for sample in samples
                  if sample.get(node_name, {}).get("status") == "Good"]
        require(values and all(type(value) is int and value >= 0 for value in values),
                f"{profile_id}: {node_name} has no Good measurements")
        require(max(values) == metrics[metric_name]["value"],
                f"{profile_id}: {metric_name} differs from resource evidence")
    if "boot-clock" in documents:
        boot = documents["boot-clock"]
        require(isinstance(boot.get("samples"), list) and boot["samples"]
                and all(sample.get("class") == "utc" for sample in boot["samples"]),
                f"{profile_id}: UTC clock evidence is invalid")
    static = documents["static-size"]
    require(static.get("schemaVersion") == 1 and static.get("measurementKind") == "static-build-layout",
            f"{profile_id}: static evidence is invalid")
    provenance, artifacts = static.get("provenance", {}), static.get("artifacts", {})
    require(artifacts.get("uf2", {}).get("sha256") == environment["firmwareUf2Sha256"],
            f"{profile_id}: static report UF2 hash differs from flashed firmware")
    for source_key, environment_key in (
        ("gitCommit", "gitCommit"), ("gitDirty", "gitDirty"),
        ("boardRevision", "boardId"), ("toolchain", "compiler"),
        ("buildType", "buildType"),
    ):
        require(provenance.get(source_key) == environment[environment_key],
                f"{profile_id}: static provenance {source_key} mismatch")


def validate_baseline(document: dict[str, Any], root: Path) -> dict[str, float]:
    require(document.get("schemaVersion") == 1, "unsupported baseline schema")
    require(document.get("contractId") == "rp2-opcua-v0.5.2-resource-benchmark",
            "contractId mismatch")
    require(document.get("release") == "0.5.2", "release mismatch")
    environment = document.get("environment")
    validate_environment(environment, document["release"])
    profiles = document.get("profiles")
    expected = {item["profileId"]: item for item in benchmark_profiles.profiles()}
    require(isinstance(profiles, list) and len(profiles) == len(expected),
            f"baseline must contain exactly {len(expected)} profile cells")
    actual: set[str] = set()
    measured_values: dict[str, float] = {}
    measured_count = 0
    for entry in profiles:
        require(isinstance(entry, dict), "profile entry must be an object")
        profile_id = entry.get("profileId")
        require(profile_id in expected and profile_id not in actual,
                f"unknown or duplicate profile: {profile_id}")
        actual.add(profile_id)
        state = entry.get("resultState")
        require(state in {"measured", "failed", "not-run"}, f"{profile_id}: invalid resultState")
        if state == "not-run":
            require(isinstance(entry.get("reason"), str) and entry["reason"].strip(),
                    f"{profile_id}: not-run requires a reason")
            require("metrics" not in entry and "evidence" not in entry,
                    f"{profile_id}: not-run cell cannot carry measurements")
            continue
        if state == "measured":
            measured_count += 1
        else:
            require(isinstance(entry.get("failureReason"), str)
                    and entry["failureReason"].strip(),
                    f"{profile_id}: failed result requires a failureReason")
            accounting = entry.get("loadAccounting")
            require(isinstance(accounting, dict)
                    and set(accounting) == {"scheduled", "completed", "missed",
                                            "deadlineExceeded", "failed"}
                    and all(type(value) is int and value >= 0 for value in accounting.values()),
                    f"{profile_id}: failed result requires bounded loadAccounting")
        require(entry.get("repetitions") == 1,
                f"{profile_id}: this baseline records exactly one repetition")
        metrics = entry.get("metrics")
        require(isinstance(metrics, dict) and set(metrics) == set(REQUIRED_METRICS),
                f"{profile_id}: measured metric set mismatch")
        for name, unit in REQUIRED_METRICS.items():
            metric = metrics[name]
            require(isinstance(metric, dict) and metric.get("unit") == unit,
                    f"{profile_id}/{name}: invalid metric/unit")
            value = metric.get("value")
            require(type(value) in (int, float) and value >= 0,
                    f"{profile_id}/{name}: invalid value")
            if state == "measured":
                measured_values[f"{profile_id}:{name}"] = float(value)
        entry_environment = entry.get("environment", environment)
        validate_environment(entry_environment, document["release"],
                             f"{profile_id}.environment")
        validate_evidence(root, entry, entry_environment)
    require(actual == set(expected), "profile matrix is incomplete")
    require(measured_count >= 1, "at least one actual hardware profile is required")
    return measured_values


def validate_thresholds(document: dict[str, Any], measured: dict[str, float]) -> None:
    require(document.get("schemaVersion") == 1, "unsupported threshold schema")
    maxima = document.get("staticMaxima")
    require(isinstance(maxima, dict) and set(maxima) == STATIC_TOTALS,
            "staticMaxima keys mismatch")
    require(all(type(value) is int and value > 0 for value in maxima.values()),
            "static maxima must be positive integers")
    hardware = document.get("hardwareMaxima")
    require(isinstance(hardware, list) and len(hardware) == len(measured),
            "hardwareMaxima must cover every measured baseline metric")
    seen: set[str] = set()
    for item in hardware:
        require(isinstance(item, dict), "hardware threshold must be an object")
        key = f"{item.get('profileId')}:{item.get('metric')}"
        require(key in measured and key not in seen, f"unknown or duplicate hardware threshold: {key}")
        seen.add(key)
        maximum = item.get("max")
        require(type(maximum) in (int, float) and maximum > 0, f"{key}: invalid maximum")
        require(maximum >= measured[key],
                f"{key}: baseline {measured[key]} exceeds threshold {maximum}")
        require(item.get("unit") == REQUIRED_METRICS[item["metric"]], f"{key}: unit mismatch")


def validate_static_report(report: dict[str, Any], thresholds: dict[str, Any]) -> None:
    require(report.get("schemaVersion") == 1 and report.get("measurementKind") == "static-build-layout",
            "invalid static size report")
    totals = report.get("totals")
    require(isinstance(totals, dict), "static report totals missing")
    for key, maximum in thresholds["staticMaxima"].items():
        value = totals.get(key)
        require(type(value) is int and value >= 0, f"static report {key} missing")
        require(value <= maximum, f"{key}: {value} exceeds {maximum}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--thresholds", type=Path, required=True)
    parser.add_argument("--static-report", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        measured = validate_baseline(read_json(args.baseline), args.root.resolve())
        thresholds = read_json(args.thresholds)
        validate_thresholds(thresholds, measured)
        if args.static_report:
            validate_static_report(read_json(args.static_report), thresholds)
        print(f"benchmark-baseline: PASS ({len(measured)} measured metrics, 192 profile cells)")
        return 0
    except (OSError, json.JSONDecodeError, ValueError, KeyError, TypeError) as exc:
        print(f"benchmark-baseline: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
