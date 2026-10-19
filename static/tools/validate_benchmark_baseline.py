#!/usr/bin/env python3
"""Validate the bounded v0.5.2 hardware baseline and regression thresholds."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

import benchmark_profiles
import load_test_opcua

MAX_JSON_BYTES = 8 * 1024 * 1024
MAX_NATIVE_EVIDENCE_BYTES = 1024 * 1024
MAX_ASSOCIATION_ENVELOPE_BYTES = 64 * 1024
REQUIRED_METRICS = {
    "ReadLatencyMs.p99": "milliseconds",
    "SecureSessionEstablishmentLatencyMs.Cold.max": "milliseconds",
    "SecureSessionEstablishmentLatencyMs.Warm.max": "milliseconds",
    "Heap.PeakUsedBytes": "bytes",
    "PSRAM.PeakUsedBytes": "bytes",
}
SAMPLING_JITTER_METRIC = "SamplingJitterMs.p99"
NATIVE_ASSOCIATION_KIND = "native-association"
METRIC_UNITS = {**REQUIRED_METRICS, SAMPLING_JITTER_METRIC: "milliseconds"}
STATIC_TOTALS = {
    "ElfFileBytes", "Uf2FileBytes", "BinFileBytes",
    "ExternalFlashLoadBytes", "SramStaticTotalBytes",
}
INTERNAL_SRAM_REGIONS = {"RAM": 524288, "SCRATCH_X": 4096, "SCRATCH_Y": 4096}
MINIMUM_INTERNAL_SRAM_RESERVE_BYTES = 256


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
    total = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            total += len(block)
            if total > MAX_JSON_BYTES:
                raise BaselineError(f"{path}: exceeds {MAX_JSON_BYTES} bytes")
            digest.update(block)
    return digest.hexdigest()


def validate_environment(environment: Any, release: str, prefix: str = "environment") -> None:
    require(isinstance(environment, dict), f"{prefix} is required")
    for key in ("recordedAt", "board", "boardId", "clientBoardRevision", "network",
                "toolchain", "clientToolchain", "compiler", "buildType", "firmwareVersion",
                "firmwareSource", "gitCommit", "firmwareUf2Sha256"):
        require(isinstance(environment.get(key), str) and environment[key],
                f"{prefix}.{key} is required")
    require(type(environment.get("gitDirty")) is bool,
            f"{prefix}.gitDirty must be a Boolean")
    require(environment.get("firmwareVersion") == release,
            f"{prefix} firmware version differs from release")


def validate_native_association(profile_id: str, profile: dict[str, Any], metrics: dict[str, Any],
                                environment: dict[str, Any], documents: dict[str, dict[str, Any]],
                                evidence_hashes: dict[str, str], evidence_sizes: dict[str, int]) -> None:
    """Validate the runner's byte-level association before accepting native jitter."""
    envelope = documents[NATIVE_ASSOCIATION_KIND]
    require(set(envelope) == {"schemaVersion", "kind", "state", "associationId", "profile", "capture",
                              "provenance", "evidence"}
            and envelope.get("schemaVersion") == 1
            and envelope.get("kind") == "v052-host-orchestrated-benchmark-capture"
            and envelope.get("state") == "complete",
            f"{profile_id}: native association envelope is invalid")
    profile_evidence = envelope.get("profile")
    require(isinstance(profile_evidence, dict) and profile_evidence == {
        "profileId": profile_id,
        "profileSchemaVersion": profile["profileSchemaVersion"],
        "channels": profile["channels"], "clients": profile["clients"],
        "rateHz": profile["rateHz"],
    }, f"{profile_id}: native association profile mismatch")
    provenance = envelope.get("provenance")
    static = documents["static-size"]
    static_provenance = static.get("provenance", {})
    static_uf2 = static.get("artifacts", {}).get("uf2", {}).get("sha256")
    require(isinstance(provenance, dict) and provenance == {
        "gitCommit": environment["gitCommit"], "gitDirty": environment["gitDirty"],
        "boardRevision": environment["boardId"], "toolchain": environment["compiler"],
        "buildType": environment["buildType"], "flashedUf2Sha256": environment["firmwareUf2Sha256"],
    }, f"{profile_id}: native association provenance mismatch")
    require(static_provenance.get("gitCommit") == provenance["gitCommit"]
            and static_provenance.get("gitDirty") is provenance["gitDirty"]
            and static_provenance.get("boardRevision") == provenance["boardRevision"]
            and static_provenance.get("toolchain") == provenance["toolchain"]
            and static_provenance.get("buildType") == provenance["buildType"]
            and static_uf2 == provenance["flashedUf2Sha256"],
            f"{profile_id}: native association static provenance mismatch")
    client = documents["client"]
    for key in ("gitCommit", "gitDirty", "boardRevision", "toolchain", "buildType"):
        require(client.get(key) == provenance[key],
                f"{profile_id}: native association client provenance {key} mismatch")
    capture = envelope.get("capture")
    require(isinstance(capture, dict)
            and set(capture) == {"warmupSeconds", "durationSeconds", "readiness", "nativeCaptureId"}
            and capture.get("warmupSeconds") == client.get("warmupSeconds")
            and capture.get("durationSeconds") == client.get("durationSeconds")
            and capture.get("nativeCaptureId") == documents["native"].get("captureId"),
            f"{profile_id}: native association capture mismatch")
    readiness = capture.get("readiness")
    require(isinstance(readiness, dict)
            and set(readiness) == {"tokenCount", "requiredTokenCount", "stdoutBytesObserved"}
            and readiness.get("tokenCount") == profile["clients"]
            and readiness.get("requiredTokenCount") == profile["clients"]
            and type(readiness.get("stdoutBytesObserved")) is int
            and 0 <= readiness["stdoutBytesObserved"] <= 64 * 1024,
            f"{profile_id}: native association readiness mismatch")
    association_evidence = envelope.get("evidence")
    require(isinstance(association_evidence, dict)
            and set(association_evidence) == {"client", "native", "staticSize"}
            and association_evidence.get("client") == {
                "sha256": evidence_hashes["client"], "bytes": evidence_sizes["client"]}
            and association_evidence.get("native") == {
                "sha256": evidence_hashes["native"], "bytes": evidence_sizes["native"]}
            and association_evidence.get("staticSize") == {"sha256": evidence_hashes["static-size"]},
            f"{profile_id}: native association evidence hash mismatch")
    expected_association = hashlib.sha256(
        (profile_id + provenance["gitCommit"] + provenance["flashedUf2Sha256"]
         + evidence_hashes["client"] + evidence_hashes["native"]).encode("ascii")
    ).hexdigest()
    require(envelope.get("associationId") == expected_association,
            f"{profile_id}: native association ID mismatch")
    expected_channels = {f"benchmark.synthetic.{slot}" for slot in range(1, profile["channels"] + 1)}
    try:
        per_channel = load_test_opcua.native_jitter_metrics(
            documents["native"], 1000 / profile["rateHz"], 8192, profile_id,
            capture["warmupSeconds"], capture["durationSeconds"], expected_channels)
    except (TypeError, ValueError, KeyError) as exc:
        raise BaselineError(f"{profile_id}: native association jitter evidence is invalid") from exc
    p99_values = []
    for value in per_channel.values():
        report = value.get("samplingJitterMs") if isinstance(value, dict) else None
        p99 = report.get("p99") if isinstance(report, dict) else None
        require(value.get("state") == "measured" and type(p99) in (int, float),
                f"{profile_id}: native association has incomplete channel samples")
        p99_values.append(float(p99))
    expected_jitter = max(p99_values)
    jitter = metrics.get(SAMPLING_JITTER_METRIC)
    require(isinstance(jitter, dict) and jitter.get("unit") == "milliseconds"
            and jitter.get("value") == expected_jitter,
            f"{profile_id}: SamplingJitter differs from associated native evidence")


def validate_evidence(root: Path, entry: dict[str, Any], environment: dict[str, Any]) -> bool:
    profile_id = entry["profileId"]
    evidence = entry.get("evidence")
    require(isinstance(evidence, list) and 3 <= len(evidence) <= 8,
            f"{entry.get('profileId')}: expected 3..8 evidence files")
    seen: set[str] = set()
    documents: dict[str, dict[str, Any]] = {}
    evidence_hashes: dict[str, str] = {}
    evidence_sizes: dict[str, int] = {}
    for item in evidence:
        require(isinstance(item, dict), "evidence entry must be an object")
        kind, relative, expected = item.get("kind"), item.get("path"), item.get("sha256")
        require(kind in {"client", "native", NATIVE_ASSOCIATION_KIND, "resources", "boot-clock", "static-size"}
                and kind not in documents, f"unknown or duplicate evidence kind: {kind}")
        require(isinstance(relative, str) and relative.startswith("static/benchmarks/v0.5.2/evidence/"),
                "evidence path must remain under static/benchmarks/v0.5.2/evidence")
        require(relative not in seen, f"duplicate evidence path: {relative}")
        seen.add(relative)
        require(isinstance(expected, str) and re.fullmatch(r"[0-9a-f]{64}", expected) is not None,
                f"{relative}: invalid SHA-256")
        path = root / relative
        require(path.is_file(), f"missing evidence file: {relative}")
        require(sha256(path) == expected.lower(), f"evidence hash mismatch: {relative}")
        documents[kind] = read_json(path)
        evidence_hashes[kind] = expected.lower()
        evidence_sizes[kind] = path.stat().st_size
        if kind == "native":
            require(evidence_sizes[kind] <= MAX_NATIVE_EVIDENCE_BYTES,
                    f"{profile_id}: native evidence exceeds byte bound")
        if kind == NATIVE_ASSOCIATION_KIND:
            require(evidence_sizes[kind] <= MAX_ASSOCIATION_ENVELOPE_BYTES,
                    f"{entry.get('profileId')}: native association envelope exceeds byte bound")
    required_documents = {"client", "resources", "static-size"}
    require(required_documents <= set(documents),
            f"{entry.get('profileId')}: evidence kinds are incomplete")
    has_native = "native" in documents
    require(has_native == (NATIVE_ASSOCIATION_KIND in documents),
            f"{entry.get('profileId')}: native capture requires its association envelope")

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
    profile = benchmark_profiles.select(profile_id)
    expected_read_schedule = {
        "policy": load_test_opcua.READ_ARRIVAL_PHASE_POLICY,
        "phaseSpacingMs": round((1000 / profile["rateHz"]) / profile["clients"], 6),
        "phaseOffsetFormula": "clientIndex * (1 / rateHz) / clientCount",
        "windowClock": "host-monotonic",
    }
    require(client.get("clientLoad", {}).get("readArrivalSchedule") == expected_read_schedule,
            f"{profile_id}: client read arrival schedule mismatch")
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
    if has_native:
        validate_native_association(profile_id, benchmark_profiles.select(profile_id), entry["metrics"],
                                    environment, documents, evidence_hashes, evidence_sizes)
    return has_native


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
        require(isinstance(metrics, dict), f"{profile_id}: metrics are invalid")
        entry_environment = entry.get("environment", environment)
        validate_environment(entry_environment, document["release"],
                             f"{profile_id}.environment")
        has_native_association = validate_evidence(root, entry, entry_environment)
        require(has_native_association or SAMPLING_JITTER_METRIC not in metrics,
                f"{profile_id}: SamplingJitter requires runner-proven native association")
        expected_metrics = set(REQUIRED_METRICS)
        if has_native_association:
            expected_metrics.add(SAMPLING_JITTER_METRIC)
        require(set(metrics) == expected_metrics,
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
        if has_native_association:
            jitter = metrics[SAMPLING_JITTER_METRIC]
            require(jitter.get("unit") == "milliseconds"
                    and type(jitter.get("value")) in (int, float) and jitter["value"] >= 0,
                    f"{profile_id}/{SAMPLING_JITTER_METRIC}: invalid metric/unit")
            if state == "measured":
                measured_values[f"{profile_id}:{SAMPLING_JITTER_METRIC}"] = float(jitter["value"])
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
        require(item.get("unit") == METRIC_UNITS.get(item["metric"]), f"{key}: unit mismatch")


def validate_release_readiness(document: dict[str, Any], thresholds: dict[str, Any]) -> None:
    environment = document.get("environment", {})
    require(environment.get("gitDirty") is False,
            "release baseline must come from a clean source tree")
    require(re.fullmatch(r"[0-9a-f]{40}", environment.get("gitCommit", "")) is not None,
            "release baseline requires a full lowercase firmware commit SHA")
    required = thresholds.get("releaseRequiredProfiles")
    require(isinstance(required, list) and required
            and all(isinstance(profile_id, str) and profile_id for profile_id in required)
            and len(required) == len(set(required)),
            "releaseRequiredProfiles must be a non-empty unique string list")
    profiles = document.get("profiles", [])
    states = {entry.get("profileId"): entry.get("resultState")
              for entry in profiles if isinstance(entry, dict)}
    require(not any(state == "failed" for state in states.values()),
            "release baseline contains a failed profile")
    for profile_id in required:
        require(states.get(profile_id) == "measured",
                f"required release profile is not measured: {profile_id}")
        entry = next(item for item in profiles if item.get("profileId") == profile_id)
        profile_environment = entry.get("environment", environment)
        require(profile_environment.get("gitDirty") is False,
                f"required release profile uses a dirty source tree: {profile_id}")
        require(re.fullmatch(r"[0-9a-f]{40}",
                             profile_environment.get("gitCommit", "")) is not None,
                f"required release profile lacks a full firmware commit SHA: {profile_id}")
        require(profile_environment.get("gitCommit") == environment.get("gitCommit"),
                f"required release profile firmware commit differs from baseline: {profile_id}")
        require(profile_environment.get("firmwareVersion") == document.get("release"),
                f"required release profile firmware version differs from release: {profile_id}")


def validate_static_report(report: dict[str, Any], thresholds: dict[str, Any]) -> None:
    require(report.get("schemaVersion") == 1 and report.get("measurementKind") == "static-build-layout",
            "invalid static size report")
    totals = report.get("totals")
    require(isinstance(totals, dict), "static report totals missing")
    regions = report.get("memoryRegions")
    require(isinstance(regions, list), "static report memoryRegions missing")
    internal_names = set(INTERNAL_SRAM_REGIONS)
    internal = [region for region in regions
                if isinstance(region, dict) and region.get("name") in internal_names]
    require(len(internal) == len(INTERNAL_SRAM_REGIONS)
            and {region.get("name"): region.get("lengthBytes") for region in internal}
            == INTERNAL_SRAM_REGIONS,
            "static report internal SRAM regions differ from the reference RP2350 layout")
    internal_capacity = sum(region["lengthBytes"] for region in internal)
    static_limit = thresholds["staticMaxima"]["SramStaticTotalBytes"]
    require(static_limit <= internal_capacity - MINIMUM_INTERNAL_SRAM_RESERVE_BYTES,
            f"SramStaticTotalBytes threshold {static_limit} must leave at least "
            f"{MINIMUM_INTERNAL_SRAM_RESERVE_BYTES} bytes below physical internal SRAM "
            f"{internal_capacity}")
    for key, maximum in thresholds["staticMaxima"].items():
        value = totals.get(key)
        require(type(value) is int and value >= 0, f"static report {key} missing")
        require(value <= maximum, f"{key}: {value} exceeds {maximum}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--thresholds", type=Path, required=True)
    parser.add_argument("--static-report", type=Path)
    parser.add_argument("--require-release-ready", action="store_true")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        measured = validate_baseline(read_json(args.baseline), args.root.resolve())
        thresholds = read_json(args.thresholds)
        validate_thresholds(thresholds, measured)
        if args.require_release_ready:
            validate_release_readiness(read_json(args.baseline), thresholds)
        if args.static_report:
            validate_static_report(read_json(args.static_report), thresholds)
        print(f"benchmark-baseline: PASS ({len(measured)} measured metrics, 192 profile cells)")
        return 0
    except (OSError, json.JSONDecodeError, ValueError, KeyError, TypeError) as exc:
        print(f"benchmark-baseline: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
