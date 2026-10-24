#!/usr/bin/env python3

from __future__ import annotations

import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "static" / "tools" / "validate_resource_exhaustion_evidence.py"
SPEC = importlib.util.spec_from_file_location("resource_evidence", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def stats(requested_available: bool, failed: int = 4) -> dict[str, object]:
    return {
        "available": True, "ready": True,
        "requestedBytesAvailable": requested_available,
        "minimumEverFreeBytesAvailable": not requested_available,
        "freeBytes": 1000, "usedBytes": 24, "peakUsedBytes": 900,
        "requestedBytes": 24 if requested_available else None,
        "minimumEverFreeBytes": None if requested_available else 100,
        "failedAllocations": {"value": failed, "wrapCount": 0},
        "lastFailureReason": 1,
    }


def allocator(name: str) -> dict[str, object]:
    initial = stats(name == "psram")
    final = stats(name == "psram", 104)
    if name == "heap":
        final["minimumEverFreeBytes"] = 50
    return {
        "status": "pass", "requestedCycles": 100, "completedCycles": 100,
        "minBlockCount": 2, "maxBlockCount": 17, "firstFailureCycle": 0,
        "baselineReturned": True, "counterExact": True, "oomReasonExact": True,
        "payloadVerified": True, "minimumEverStable": True,
        "initial": initial, "final": final,
    }


def evidence() -> dict[str, object]:
    return {
        "schemaVersion": 1, "kind": "v052-resource-exhaustion",
        "profileId": "v052-resource-exhaustion-reference",
        "constants": {"cycles": 100, "pointerCapacity": 512,
                      "heapRequestBytes": 1024, "psramRequestBytes": 16384},
        "startMonotonicMs": 10, "endMonotonicMs": 90, "durationMs": 80,
        "initTaskStackHighWaterStartBytes": 2000,
        "initTaskStackHighWaterEndBytes": 1800,
        "heap": allocator("heap"), "psram": allocator("psram"), "allPassed": True,
    }


class EvidenceValidationTests(unittest.TestCase):
    def test_accepts_complete_t2_result(self) -> None:
        self.assertEqual(MODULE.validate_document(evidence())["durationMs"], 80)

    def test_rejects_incomplete_cycle_count(self) -> None:
        sample = evidence()
        sample["heap"]["completedCycles"] = 99
        with self.assertRaisesRegex(MODULE.EvidenceError, "complete 100"):
            MODULE.validate_document(sample)

    def test_rejects_counter_mismatch(self) -> None:
        sample = evidence()
        sample["psram"]["final"]["failedAllocations"]["value"] = 103
        with self.assertRaisesRegex(MODULE.EvidenceError, "expected 100"):
            MODULE.validate_document(sample)

    def test_accepts_one_counter_wrap(self) -> None:
        sample = evidence()
        sample["heap"]["initial"]["failedAllocations"] = {
            "value": (1 << 64) - 40, "wrapCount": 7}
        sample["heap"]["final"]["failedAllocations"] = {"value": 60, "wrapCount": 8}
        MODULE.validate_document(sample)

    def test_rejects_baseline_drift_and_false_integrity(self) -> None:
        for path in (("heap", "final", "usedBytes"),
                     ("psram", "payloadVerified"),
                     ("heap", "minimumEverStable")):
            sample = evidence()
            target = sample
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = 25 if path[-1] == "usedBytes" else False
            with self.assertRaises(MODULE.EvidenceError):
                MODULE.validate_document(sample)

    def test_rejects_impossible_stack_and_peak_values(self) -> None:
        sample = evidence()
        sample["initTaskStackHighWaterEndBytes"] = 2100
        with self.assertRaisesRegex(MODULE.EvidenceError, "stack high-water mark increased"):
            MODULE.validate_document(sample)
        sample = evidence()
        sample["heap"]["final"]["peakUsedBytes"] = 10
        with self.assertRaisesRegex(MODULE.EvidenceError, "peakUsedBytes"):
            MODULE.validate_document(sample)

    def test_rejects_oversized_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "raw.json"
            path.write_text(json.dumps(evidence()) + " " * 4096, encoding="utf-8")
            with self.assertRaisesRegex(MODULE.EvidenceError, "4096-byte"):
                MODULE.load_and_validate(path)

    def test_committed_hardware_bundle_is_coherent(self) -> None:
        directory = ROOT / "static" / "benchmarks" / "v0.5.2" / "evidence"
        raw_path = directory / "resource-exhaustion-raw.json"
        MODULE.validate_run_bundle(
            raw_path, MODULE.load_and_validate(raw_path),
            directory / "resource-exhaustion-run.json")

    def test_bundle_rejects_tampered_hash(self) -> None:
        source = ROOT / "static" / "benchmarks" / "v0.5.2" / "evidence"
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            for name in ("resource-exhaustion-raw.json", "resource-exhaustion-serial.log",
                         "resource-exhaustion-profile.json",
                         "resource-exhaustion-static-size.json"):
                (directory / name).write_bytes((source / name).read_bytes())
            manifest = json.loads((source / "resource-exhaustion-run.json").read_text())
            manifest["evidence"]["rawJson"]["sha256"] = "0" * 64
            manifest_path = directory / "resource-exhaustion-run.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            raw_path = directory / "resource-exhaustion-raw.json"
            with self.assertRaisesRegex(MODULE.EvidenceError, "hash mismatch"):
                MODULE.validate_run_bundle(raw_path, MODULE.load_and_validate(raw_path),
                                           manifest_path)

    def test_bundle_rejects_unrelated_serial_record_even_with_updated_hash(self) -> None:
        source = ROOT / "static" / "benchmarks" / "v0.5.2" / "evidence"
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            for name in ("resource-exhaustion-raw.json", "resource-exhaustion-serial.log",
                         "resource-exhaustion-profile.json",
                         "resource-exhaustion-static-size.json"):
                (directory / name).write_bytes((source / name).read_bytes())
            serial_path = directory / "resource-exhaustion-serial.log"
            serial_path.write_bytes(serial_path.read_bytes().replace(
                b'\"durationMs\":569778', b'\"durationMs\":569777'))
            manifest = json.loads((source / "resource-exhaustion-run.json").read_text())
            manifest["evidence"]["serialLog"]["sha256"] = MODULE._sha256(serial_path)
            manifest_path = directory / "resource-exhaustion-run.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            raw_path = directory / "resource-exhaustion-raw.json"
            with self.assertRaisesRegex(MODULE.EvidenceError, "does not match raw JSON"):
                MODULE.validate_run_bundle(raw_path, MODULE.load_and_validate(raw_path),
                                           manifest_path)

    def test_bundle_rejects_mismatched_static_profile_even_with_updated_hash(self) -> None:
        source = ROOT / "static" / "benchmarks" / "v0.5.2" / "evidence"
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            for name in ("resource-exhaustion-raw.json", "resource-exhaustion-serial.log",
                         "resource-exhaustion-profile.json",
                         "resource-exhaustion-static-size.json"):
                (directory / name).write_bytes((source / name).read_bytes())
            static_path = directory / "resource-exhaustion-static-size.json"
            static_report = json.loads(static_path.read_text())
            static_report["provenance"]["benchmarkProfile"] = "off"
            static_path.write_text(json.dumps(static_report), encoding="utf-8")
            manifest = json.loads((source / "resource-exhaustion-run.json").read_text())
            manifest["evidence"]["staticSizeReport"]["sha256"] = MODULE._sha256(static_path)
            manifest_path = directory / "resource-exhaustion-run.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            raw_path = directory / "resource-exhaustion-raw.json"
            with self.assertRaisesRegex(MODULE.EvidenceError, "not for the resource-exhaustion"):
                MODULE.validate_run_bundle(raw_path, MODULE.load_and_validate(raw_path),
                                           manifest_path)

    def test_bundle_rejects_oversized_serial_before_full_hash_or_read(self) -> None:
        source = ROOT / "static" / "benchmarks" / "v0.5.2" / "evidence"
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            for name in ("resource-exhaustion-raw.json", "resource-exhaustion-profile.json",
                         "resource-exhaustion-static-size.json"):
                (directory / name).write_bytes((source / name).read_bytes())
            (directory / "resource-exhaustion-serial.log").write_bytes(
                b"x" * (MODULE.MAX_SERIAL_LOG_BYTES + 1))
            manifest_path = directory / "resource-exhaustion-run.json"
            manifest_path.write_bytes((source / "resource-exhaustion-run.json").read_bytes())
            raw_path = directory / "resource-exhaustion-raw.json"
            with self.assertRaisesRegex(MODULE.EvidenceError, "65536-byte bound"):
                MODULE.validate_run_bundle(raw_path, MODULE.load_and_validate(raw_path),
                                           manifest_path)


if __name__ == "__main__":
    unittest.main()
