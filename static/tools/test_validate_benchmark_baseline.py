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
    BaselineError, read_json, validate_baseline, validate_static_report,
    validate_thresholds,
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
assert len(measured) == 6
assert sum(entry["resultState"] == "measured" for entry in baseline["profiles"]) == 1
assert sum(entry["resultState"] == "failed" for entry in baseline["profiles"]) == 1
assert sum(entry["resultState"] == "not-run" for entry in baseline["profiles"]) == 190

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
next(entry for entry in no_failure_reason["profiles"] if entry["resultState"] == "failed")["failureReason"] = ""
rejects(lambda: validate_baseline(no_failure_reason, ROOT), "requires a failureReason")

bad_failed_accounting = copy.deepcopy(baseline)
next(entry for entry in bad_failed_accounting["profiles"] if entry["resultState"] == "failed")["loadAccounting"]["completed"] += 1
rejects(lambda: validate_baseline(bad_failed_accounting, ROOT), "load accounting completed mismatch")

bad_failed_environment = copy.deepcopy(baseline)
next(entry for entry in bad_failed_environment["profiles"] if entry["resultState"] == "failed")["environment"]["firmwareVersion"] = "0.5.1"
rejects(lambda: validate_baseline(bad_failed_environment, ROOT), "firmware version differs")

with tempfile.TemporaryDirectory() as directory:
    temp_root = Path(directory)
    evidence_root = temp_root / "static/benchmarks/v0.5.2/evidence"
    shutil.copytree(ROOT / "static/benchmarks/v0.5.2/evidence", evidence_root)
    no_recorded_failure = copy.deepcopy(baseline)
    failed_entry = next(entry for entry in no_recorded_failure["profiles"]
                        if entry["resultState"] == "failed")
    client_item = next(item for item in failed_entry["evidence"] if item["kind"] == "client")
    client_path = temp_root / client_item["path"]
    client = read_json(client_path)
    client["errors"] = []
    client["errorRecordOverflow"] = 0
    client_path.write_text(json.dumps(client, indent=2) + "\n", encoding="utf-8")
    client_item["sha256"] = hashlib.sha256(client_path.read_bytes()).hexdigest()
    rejects(lambda: validate_baseline(no_recorded_failure, temp_root), "no recorded failure")

bad_hash = copy.deepcopy(baseline)
next(entry for entry in bad_hash["profiles"] if entry["resultState"] == "measured")["evidence"][0]["sha256"] = "0" * 64
rejects(lambda: validate_baseline(bad_hash, ROOT), "hash mismatch")

mixed_firmware = copy.deepcopy(baseline)
mixed_firmware["environment"]["firmwareUf2Sha256"] = "0" * 64
rejects(lambda: validate_baseline(mixed_firmware, ROOT), "differs from flashed firmware")

mixed_client = copy.deepcopy(baseline)
mixed_client["environment"]["clientToolchain"] = "different-toolchain"
rejects(lambda: validate_baseline(mixed_client, ROOT), "client provenance toolchain mismatch")

too_low = copy.deepcopy(thresholds)
too_low["hardwareMaxima"][0]["max"] = 1
rejects(lambda: validate_thresholds(too_low, measured), "exceeds threshold")

static_too_low = copy.deepcopy(thresholds)
static_too_low["staticMaxima"]["ExternalFlashLoadBytes"] = 1
rejects(lambda: validate_static_report(read_json(STATIC_REPORT), static_too_low), "exceeds")

print("benchmark-baseline tests: PASS")
