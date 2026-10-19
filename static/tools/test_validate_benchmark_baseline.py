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
import validate_benchmark_baseline as subject  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT = REPO_ROOT
BASELINE = REPO_ROOT / "static/benchmarks/v0.5.2/baseline.json"
THRESHOLDS = REPO_ROOT / "static/benchmarks/v0.5.2/thresholds.json"
STATIC_REPORT = REPO_ROOT / "static/benchmarks/v0.5.2/evidence/secure-c1-static-size.json"
NATIVE_PROFILE_ID = "v052-syn-c16-ns0full-sectest-none-cl4-hz10-ev1-hist1"


def rejects(callable_, phrase: str) -> None:
    try:
        callable_()
    except BaselineError as exc:
        assert phrase in str(exc), (phrase, exc)
    else:
        raise AssertionError(f"expected BaselineError containing {phrase!r}")


def write_document(path: Path, document: dict) -> str:
    path.write_bytes(json.dumps(document, indent=2, sort_keys=True).encode("utf-8") + b"\n")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def paired_native_fixture(directory: Path) -> tuple[dict, Path, dict, Path]:
    """Make one complete runner-associated c16 cell without changing retained evidence."""
    root = directory
    evidence_root = root / "static/benchmarks/v0.5.2/evidence"
    shutil.copytree(ROOT / "static/benchmarks/v0.5.2/evidence", evidence_root)
    fixture = copy.deepcopy(baseline)
    entry = next(item for item in fixture["profiles"] if item["profileId"] == NATIVE_PROFILE_ID)
    client_item = next(item for item in entry["evidence"] if item["kind"] == "client")
    static_item = next(item for item in entry["evidence"] if item["kind"] == "static-size")
    client_path = root / client_item["path"]
    static_path = root / static_item["path"]
    client, static = read_json(client_path), read_json(static_path)
    provenance = static["provenance"]
    client["boardRevision"] = provenance["boardRevision"]
    client["toolchain"] = provenance["toolchain"]
    client_item["sha256"] = write_document(client_path, client)
    entry["environment"]["clientBoardRevision"] = provenance["boardRevision"]
    entry["environment"]["clientToolchain"] = provenance["toolchain"]
    native_path = evidence_root / "fixture-native.json"
    native_path.write_bytes((ROOT / "static/benchmarks/v0.5.2/evidence/test-none-c16-native.json").read_bytes())
    native = read_json(native_path)
    native_hash = hashlib.sha256(native_path.read_bytes()).hexdigest()
    native_item = {"kind": "native", "path": "static/benchmarks/v0.5.2/evidence/fixture-native.json",
                   "sha256": native_hash}
    entry["evidence"].append(native_item)
    profile = next(item for item in subject.benchmark_profiles.profiles()
                   if item["profileId"] == entry["profileId"])
    channels = {f"benchmark.synthetic.{slot}" for slot in range(1, profile["channels"] + 1)}
    per_channel = subject.load_test_opcua.native_jitter_metrics(
        native, 1000 / profile["rateHz"], 8192, entry["profileId"], 10.0, 30.0, channels)
    entry["metrics"]["SamplingJitterMs.p99"] = {
        "value": max(value["samplingJitterMs"]["p99"] for value in per_channel.values()),
        "unit": "milliseconds",
    }
    client_raw = client_path.read_bytes()
    envelope = {
        "schemaVersion": 1, "kind": "v052-host-orchestrated-benchmark-capture", "state": "complete",
        "profile": {"profileId": entry["profileId"], "profileSchemaVersion": profile["profileSchemaVersion"],
                    "channels": profile["channels"], "clients": profile["clients"], "rateHz": profile["rateHz"]},
        "capture": {"warmupSeconds": 10.0, "durationSeconds": 30.0,
                    "readiness": {"tokenCount": 4, "requiredTokenCount": 4, "stdoutBytesObserved": 24},
                    "nativeCaptureId": native["captureId"]},
        "provenance": {"gitCommit": provenance["gitCommit"], "gitDirty": provenance["gitDirty"],
                       "boardRevision": provenance["boardRevision"], "toolchain": provenance["toolchain"],
                       "buildType": provenance["buildType"], "flashedUf2Sha256": static["artifacts"]["uf2"]["sha256"]},
        "evidence": {"client": {"sha256": client_item["sha256"], "bytes": len(client_raw)},
                     "native": {"sha256": native_hash, "bytes": native_path.stat().st_size},
                     "staticSize": {"sha256": static_item["sha256"]}},
    }
    envelope["associationId"] = hashlib.sha256(
        (entry["profileId"] + provenance["gitCommit"] + static["artifacts"]["uf2"]["sha256"]
         + client_item["sha256"] + native_hash).encode("ascii")).hexdigest()
    envelope_path = evidence_root / "fixture-native-association.json"
    association_item = {"kind": "native-association",
                        "path": "static/benchmarks/v0.5.2/evidence/fixture-native-association.json",
                        "sha256": write_document(envelope_path, envelope)}
    entry["evidence"].append(association_item)
    return fixture, root, envelope, envelope_path


repository_baseline = read_json(BASELINE)
rejects(lambda: validate_baseline(repository_baseline, REPO_ROOT),
        "client read arrival schedule mismatch")

# Retained hardware evidence intentionally remains untouched until a real
# phase-staggered recapture.  Build a hash-consistent positive fixture so the
# validator's other positive/negative cases remain independently testable.
_fixture_temp = tempfile.TemporaryDirectory()
ROOT = Path(_fixture_temp.name)
shutil.copytree(REPO_ROOT / "static/benchmarks/v0.5.2/evidence",
                ROOT / "static/benchmarks/v0.5.2/evidence")
baseline = copy.deepcopy(repository_baseline)
for measured_entry in (entry for entry in baseline["profiles"] if entry["resultState"] == "measured"):
    profile = subject.benchmark_profiles.select(measured_entry["profileId"])
    client_item = next(item for item in measured_entry["evidence"] if item["kind"] == "client")
    client_path = ROOT / client_item["path"]
    client = read_json(client_path)
    client.setdefault("clientLoad", {})["readArrivalSchedule"] = {
        "policy": subject.load_test_opcua.READ_ARRIVAL_PHASE_POLICY,
        "phaseSpacingMs": round((1000 / profile["rateHz"]) / profile["clients"], 6),
        "phaseOffsetFormula": "clientIndex * (1 / rateHz) / clientCount",
        "windowClock": "host-monotonic",
    }
    client_item["sha256"] = write_document(client_path, client)
STATIC_REPORT = ROOT / "static/benchmarks/v0.5.2/evidence/secure-c1-static-size.json"
thresholds = read_json(THRESHOLDS)
measured = validate_baseline(baseline, ROOT)
validate_thresholds(thresholds, measured)
validate_static_report(read_json(STATIC_REPORT), thresholds)
assert len(baseline["profiles"]) == 192
assert len(measured) == 10
assert sum(entry["resultState"] == "measured" for entry in baseline["profiles"]) == 2
assert sum(entry["resultState"] == "failed" for entry in baseline["profiles"]) == 0
assert sum(entry["resultState"] == "not-run" for entry in baseline["profiles"]) == 190

with tempfile.TemporaryDirectory() as directory:
    mismatch_root = Path(directory)
    shutil.copytree(ROOT / "static/benchmarks/v0.5.2/evidence",
                    mismatch_root / "static/benchmarks/v0.5.2/evidence")
    mismatch = copy.deepcopy(baseline)
    mismatch_entry = next(entry for entry in mismatch["profiles"] if entry["resultState"] == "measured")
    mismatch_item = next(item for item in mismatch_entry["evidence"] if item["kind"] == "client")
    mismatch_path = mismatch_root / mismatch_item["path"]
    mismatch_client = read_json(mismatch_path)
    mismatch_client["clientLoad"]["readArrivalSchedule"]["phaseSpacingMs"] = 999.0
    mismatch_item["sha256"] = write_document(mismatch_path, mismatch_client)
    rejects(lambda: validate_baseline(mismatch, mismatch_root),
            "client read arrival schedule mismatch")

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
        "native capture requires its association envelope")

with tempfile.TemporaryDirectory() as directory:
    paired, paired_root, envelope, envelope_path = paired_native_fixture(Path(directory))
    paired_measured = validate_baseline(paired, paired_root)
    assert len(paired_measured) == 11
    paired_entry = next(item for item in paired["profiles"] if item["profileId"] == NATIVE_PROFILE_ID)
    assert paired_measured[f"{paired_entry['profileId']}:SamplingJitterMs.p99"] == 53.089
    paired_thresholds = copy.deepcopy(thresholds)
    paired_thresholds["hardwareMaxima"].append({"profileId": NATIVE_PROFILE_ID,
                                                  "metric": "SamplingJitterMs.p99",
                                                  "max": 60.0, "unit": "milliseconds"})
    validate_thresholds(paired_thresholds, paired_measured)

with tempfile.TemporaryDirectory() as directory:
    paired, paired_root, envelope, envelope_path = paired_native_fixture(Path(directory))
    native_item = next(item for item in next(entry for entry in paired["profiles"]
                                             if entry["profileId"] == NATIVE_PROFILE_ID)["evidence"]
                       if item["kind"] == "native")
    native_path = paired_root / native_item["path"]
    native = read_json(native_path)
    native["records"][0]["sampleMonotonicUs"] += 1
    native_item["sha256"] = write_document(native_path, native)
    rejects(lambda: validate_baseline(paired, paired_root), "native association evidence hash mismatch")

with tempfile.TemporaryDirectory() as directory:
    paired, paired_root, envelope, envelope_path = paired_native_fixture(Path(directory))
    association_item = next(item for item in next(entry for entry in paired["profiles"]
                                                  if entry["profileId"] == NATIVE_PROFILE_ID)["evidence"]
                            if item["kind"] == "native-association")
    envelope["provenance"]["gitCommit"] = "0" * 40
    association_item["sha256"] = write_document(envelope_path, envelope)
    rejects(lambda: validate_baseline(paired, paired_root), "native association provenance mismatch")

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
