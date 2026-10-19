#!/usr/bin/env python3
"""Focused positive and negative tests for the T6 baseline gate."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate_benchmark_baseline import (  # noqa: E402
    BaselineError, read_json, validate_baseline, validate_environment,
    validate_release_readiness, validate_static_report, validate_thresholds,
)

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "static/benchmarks/v0.5.2/baseline.json"
THRESHOLDS = ROOT / "static/benchmarks/v0.5.2/thresholds.json"
STATIC_REPORT = ROOT / "static/benchmarks/v0.5.2/evidence/secure-c1-static-size.json"


def rejects(callable_, phrase: str) -> None:
    try:
        callable_()
    except BaselineError as exc:
        assert phrase in str(exc), (phrase, exc)
    else:
        raise AssertionError(f"expected BaselineError containing {phrase!r}")


baseline = read_json(BASELINE)
thresholds = read_json(THRESHOLDS)
measured = validate_baseline(baseline, ROOT)
validate_thresholds(thresholds, measured)
validate_static_report(read_json(STATIC_REPORT), thresholds)
assert len(baseline["profiles"]) == 192
assert len(measured) == 10
assert sum(entry["resultState"] == "measured" for entry in baseline["profiles"]) == 2
assert sum(entry["resultState"] == "failed" for entry in baseline["profiles"]) == 0
assert sum(entry["resultState"] == "not-run" for entry in baseline["profiles"]) == 190

clean_environment = copy.deepcopy(baseline["environment"])
clean_environment["gitDirty"] = False
validate_environment(clean_environment, baseline["release"])
bad_dirty_type = copy.deepcopy(clean_environment)
bad_dirty_type["gitDirty"] = 0
rejects(lambda: validate_environment(bad_dirty_type, baseline["release"]),
        "gitDirty must be a Boolean")

rejects(lambda: validate_release_readiness(baseline, thresholds),
        "required release profile uses a dirty source tree")
release_ready = copy.deepcopy(baseline)
release_ready["environment"]["gitDirty"] = False
release_commit = release_ready["environment"]["gitCommit"]
for profile_id in thresholds["releaseRequiredProfiles"]:
    required_entry = next(entry for entry in release_ready["profiles"]
                          if entry["profileId"] == profile_id)
    required_environment = required_entry.setdefault(
        "environment", copy.deepcopy(release_ready["environment"]))
    required_environment["gitDirty"] = False
    required_environment["gitCommit"] = release_commit
validate_release_readiness(release_ready, thresholds)
short_commit = copy.deepcopy(release_ready)
short_commit["environment"]["gitCommit"] = "2881230"
rejects(lambda: validate_release_readiness(short_commit, thresholds),
        "full lowercase firmware commit SHA")
dirty_required = copy.deepcopy(release_ready)
next(entry for entry in dirty_required["profiles"]
     if entry["profileId"] == thresholds["releaseRequiredProfiles"][1]
     )["environment"]["gitDirty"] = True
rejects(lambda: validate_release_readiness(dirty_required, thresholds),
        "required release profile uses a dirty source tree")
mixed_required_commit = copy.deepcopy(release_ready)
next(entry for entry in mixed_required_commit["profiles"]
     if entry["profileId"] == thresholds["releaseRequiredProfiles"][1]
     )["environment"]["gitCommit"] = "0" * 40
rejects(lambda: validate_release_readiness(mixed_required_commit, thresholds),
        "firmware commit differs from baseline")
missing_required = copy.deepcopy(release_ready)
next(entry for entry in missing_required["profiles"]
     if entry["profileId"] == thresholds["releaseRequiredProfiles"][1])["resultState"] = "not-run"
rejects(lambda: validate_release_readiness(missing_required, thresholds),
        "required release profile is not measured")

with tempfile.TemporaryDirectory() as directory:
    oversized = Path(directory) / "oversized.json"
    oversized.write_bytes(b" " * (8 * 1024 * 1024 + 1))
    rejects(lambda: read_json(oversized), "exceeds")

missing = copy.deepcopy(baseline)
missing["profiles"].pop()
rejects(lambda: validate_baseline(missing, ROOT), "exactly 192")

no_reason = copy.deepcopy(baseline)
next(entry for entry in no_reason["profiles"] if entry["resultState"] == "not-run")["reason"] = ""
rejects(lambda: validate_baseline(no_reason, ROOT), "requires a reason")

no_failure_reason = copy.deepcopy(baseline)
synthetic_failed = next(entry for entry in no_failure_reason["profiles"]
                        if entry["resultState"] == "measured")
synthetic_failed["resultState"] = "failed"
synthetic_failed["failureReason"] = ""
rejects(lambda: validate_baseline(no_failure_reason, ROOT), "requires a failureReason")

bad_failed_accounting = copy.deepcopy(baseline)
synthetic_failed = next(entry for entry in bad_failed_accounting["profiles"]
                        if entry["resultState"] == "measured")
synthetic_failed["resultState"] = "failed"
synthetic_failed["failureReason"] = "fixture failure"
synthetic_failed["loadAccounting"] = {"scheduled": 1, "completed": 2}
rejects(lambda: validate_baseline(bad_failed_accounting, ROOT),
        "failed result requires bounded loadAccounting")

bad_failed_environment = copy.deepcopy(baseline)
next(entry for entry in bad_failed_environment["profiles"]
     if "environment" in entry)["environment"]["firmwareVersion"] = "0.5.1"
rejects(lambda: validate_baseline(bad_failed_environment, ROOT), "firmware version differs")

with tempfile.TemporaryDirectory() as directory:
    temp_root = Path(directory)
    evidence_root = temp_root / "static/benchmarks/v0.5.2/evidence"
    shutil.copytree(ROOT / "static/benchmarks/v0.5.2/evidence", evidence_root)
    no_recorded_failure = copy.deepcopy(baseline)
    failed_entry = next(entry for entry in no_recorded_failure["profiles"]
                        if entry["profileId"] == thresholds["releaseRequiredProfiles"][1])
    failed_entry["resultState"] = "failed"
    failed_entry["failureReason"] = "fixture failure without a recorded client error"
    client_item = next(item for item in failed_entry["evidence"] if item["kind"] == "client")
    client_path = temp_root / client_item["path"]
    accounting = read_json(client_path)["loadAccounting"]
    failed_entry["loadAccounting"] = {
        "scheduled": accounting["scheduledOperationCount"],
        "completed": accounting["completedOperationCount"],
        "missed": accounting["missedDeadlineCount"],
        "deadlineExceeded": accounting["deadlineExceededOperationCount"],
        "failed": accounting["failedOperationCount"],
    }
    client = read_json(client_path)
    client["errors"] = []
    client["errorRecordOverflow"] = 0
    client_path.write_text(json.dumps(client, indent=2) + "\n", encoding="utf-8")
    client_item["sha256"] = hashlib.sha256(client_path.read_bytes()).hexdigest()
    rejects(lambda: validate_baseline(no_recorded_failure, temp_root), "no recorded failure")

unassociated_jitter = copy.deepcopy(baseline)
jitter_entry = next(entry for entry in unassociated_jitter["profiles"]
                    if entry["resultState"] == "measured")
jitter_entry["metrics"]["SamplingJitterMs.p99"] = {"value": 1.0, "unit": "milliseconds"}
rejects(lambda: validate_baseline(unassociated_jitter, ROOT),
        "SamplingJitter requires runner-proven native association")

native_as_baseline_evidence = copy.deepcopy(baseline)
native_entry = next(entry for entry in native_as_baseline_evidence["profiles"]
                    if entry["resultState"] == "measured")
native_entry["evidence"].append({
    "kind": "native", "path": "static/benchmarks/v0.5.2/evidence/test-none-c16-native.json",
    "sha256": hashlib.sha256((ROOT / "static/benchmarks/v0.5.2/evidence/test-none-c16-native.json").read_bytes()).hexdigest(),
})
rejects(lambda: validate_baseline(native_as_baseline_evidence, ROOT),
        "unassociated native capture must not be baseline evidence")

bad_hash = copy.deepcopy(baseline)
next(entry for entry in bad_hash["profiles"] if entry["resultState"] == "measured")["evidence"][0]["sha256"] = "0" * 64
rejects(lambda: validate_baseline(bad_hash, ROOT), "hash mismatch")

mixed_firmware = copy.deepcopy(baseline)
next(entry for entry in mixed_firmware["profiles"]
     if entry["resultState"] == "measured")["environment"]["firmwareUf2Sha256"] = "0" * 64
rejects(lambda: validate_baseline(mixed_firmware, ROOT), "differs from flashed firmware")

mixed_client = copy.deepcopy(baseline)
next(entry for entry in mixed_client["profiles"]
     if entry["resultState"] == "measured")["environment"]["clientToolchain"] = "different-toolchain"
rejects(lambda: validate_baseline(mixed_client, ROOT), "client provenance toolchain mismatch")

too_low = copy.deepcopy(thresholds)
too_low["hardwareMaxima"][0]["max"] = 1
rejects(lambda: validate_thresholds(too_low, measured), "exceeds threshold")

static_too_low = copy.deepcopy(thresholds)
static_too_low["staticMaxima"]["ExternalFlashLoadBytes"] = 1
rejects(lambda: validate_static_report(read_json(STATIC_REPORT), static_too_low), "exceeds")

static_above_capacity = copy.deepcopy(thresholds)
static_above_capacity["staticMaxima"]["SramStaticTotalBytes"] = 532225
rejects(lambda: validate_static_report(read_json(STATIC_REPORT), static_above_capacity),
        "must leave at least 256 bytes")

duplicate_region = read_json(STATIC_REPORT)
duplicate_region["memoryRegions"].append(copy.deepcopy(
    next(region for region in duplicate_region["memoryRegions"] if region["name"] == "RAM")))
rejects(lambda: validate_static_report(duplicate_region, thresholds),
        "differ from the reference RP2350 layout")

inflated_region = read_json(STATIC_REPORT)
next(region for region in inflated_region["memoryRegions"]
     if region["name"] == "RAM")["lengthBytes"] += 1
rejects(lambda: validate_static_report(inflated_region, thresholds),
        "differ from the reference RP2350 layout")

print("benchmark-baseline tests: PASS")
