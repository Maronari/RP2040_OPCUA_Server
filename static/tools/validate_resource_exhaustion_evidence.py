#!/usr/bin/env python3
"""Validate the bounded JSON emitted by the v0.5.2 exhaustion-test image."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


EXPECTED_PROFILE = "v052-resource-exhaustion-reference"
EXPECTED_CONSTANTS = {
    "cycles": 100,
    "pointerCapacity": 512,
    "heapRequestBytes": 1024,
    "psramRequestBytes": 16384,
}
MAX_EVIDENCE_BYTES = 4096
MAX_SERIAL_LOG_BYTES = 65536
SERIAL_MARKER = b'\"kind\":\"v052-resource-exhaustion\"'


class EvidenceError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def _object(value: Any, path: str) -> dict[str, Any]:
    _require(isinstance(value, dict), f"{path} must be an object")
    return value


def _integer(value: Any, path: str) -> int:
    _require(isinstance(value, int) and not isinstance(value, bool) and value >= 0,
             f"{path} must be a non-negative integer")
    return value


def _boolean(value: Any, path: str) -> bool:
    _require(isinstance(value, bool), f"{path} must be a boolean")
    return value


def _counter_delta(before: dict[str, Any], after: dict[str, Any], path: str) -> int:
    before_value = _integer(before.get("value"), f"{path}.initial.value")
    before_wrap = _integer(before.get("wrapCount"), f"{path}.initial.wrapCount")
    after_value = _integer(after.get("value"), f"{path}.final.value")
    after_wrap = _integer(after.get("wrapCount"), f"{path}.final.wrapCount")
    _require(after_wrap >= before_wrap, f"{path} wrap count moved backwards")
    wrap_delta = after_wrap - before_wrap
    _require(wrap_delta <= 1, f"{path} has an ambiguous multi-wrap delta")
    if wrap_delta == 0:
        _require(after_value >= before_value, f"{path} value moved backwards without a wrap")
        return after_value - before_value
    return (1 << 64) - before_value + after_value


def _validate_stats(stats: dict[str, Any], path: str, requested_available: bool) -> None:
    _require(_boolean(stats.get("available"), f"{path}.available"),
             f"{path} is unavailable")
    _require(_boolean(stats.get("ready"), f"{path}.ready"), f"{path} is not ready")
    _require(_boolean(stats.get("requestedBytesAvailable"),
                     f"{path}.requestedBytesAvailable") is requested_available,
             f"{path}.requestedBytesAvailable has the wrong capability value")
    minimum_ever_available = path.startswith("heap.")
    _require(_boolean(stats.get("minimumEverFreeBytesAvailable"),
                     f"{path}.minimumEverFreeBytesAvailable") is minimum_ever_available,
             f"{path}.minimumEverFreeBytesAvailable has the wrong capability value")
    for field in ("freeBytes", "usedBytes", "peakUsedBytes", "lastFailureReason"):
        _integer(stats.get(field), f"{path}.{field}")
    if requested_available:
        _integer(stats.get("requestedBytes"), f"{path}.requestedBytes")
    else:
        _require(stats.get("requestedBytes") is None,
                 f"{path}.requestedBytes must be null when unavailable")
    if minimum_ever_available:
        _integer(stats.get("minimumEverFreeBytes"), f"{path}.minimumEverFreeBytes")
    else:
        _require(stats.get("minimumEverFreeBytes") is None,
                 f"{path}.minimumEverFreeBytes must be null when unavailable")
    _object(stats.get("failedAllocations"), f"{path}.failedAllocations")


def _validate_allocator(document: dict[str, Any], name: str) -> None:
    result = _object(document.get(name), name)
    _require(result.get("status") == "pass", f"{name}.status must be pass")
    _require(_integer(result.get("requestedCycles"), f"{name}.requestedCycles") == 100,
             f"{name} must request 100 cycles")
    _require(_integer(result.get("completedCycles"), f"{name}.completedCycles") == 100,
             f"{name} did not complete 100 cycles")
    minimum = _integer(result.get("minBlockCount"), f"{name}.minBlockCount")
    maximum = _integer(result.get("maxBlockCount"), f"{name}.maxBlockCount")
    _require(1 <= minimum <= maximum < EXPECTED_CONSTANTS["pointerCapacity"],
             f"{name} did not reach allocator exhaustion before the pointer bound")
    _require(_integer(result.get("firstFailureCycle"), f"{name}.firstFailureCycle") == 0,
             f"{name} first failure cycle must be zero for a passing run")
    for field in ("baselineReturned", "counterExact", "oomReasonExact",
                  "payloadVerified", "minimumEverStable"):
        _require(_boolean(result.get(field), f"{name}.{field}"), f"{name}.{field} is false")

    initial = _object(result.get("initial"), f"{name}.initial")
    final = _object(result.get("final"), f"{name}.final")
    requested_available = name == "psram"
    _validate_stats(initial, f"{name}.initial", requested_available)
    _validate_stats(final, f"{name}.final", requested_available)
    for field in ("freeBytes", "usedBytes"):
        _require(final[field] == initial[field], f"{name}.{field} did not return to baseline")
    _require(final["peakUsedBytes"] >= initial["peakUsedBytes"],
             f"{name}.peakUsedBytes moved backwards")
    _require(final["peakUsedBytes"] >= final["usedBytes"],
             f"{name}.peakUsedBytes is below current use")
    if requested_available:
        _require(final["requestedBytes"] == initial["requestedBytes"],
                 f"{name}.requestedBytes did not return to baseline")
    if name == "heap":
        _require(final["minimumEverFreeBytes"] <= initial["minimumEverFreeBytes"],
                 f"{name}.minimumEverFreeBytes increased")
    delta = _counter_delta(_object(initial["failedAllocations"],
                                  f"{name}.initial.failedAllocations"),
                           _object(final["failedAllocations"],
                                  f"{name}.final.failedAllocations"),
                           f"{name}.failedAllocations")
    _require(delta == 100, f"{name} failure counter delta is {delta}, expected 100")


def validate_document(document: Any) -> dict[str, Any]:
    root = _object(document, "root")
    _require(root.get("schemaVersion") == 1, "schemaVersion must be 1")
    _require(root.get("kind") == "v052-resource-exhaustion", "unexpected evidence kind")
    _require(root.get("profileId") == EXPECTED_PROFILE, "unexpected profileId")
    _require(root.get("constants") == EXPECTED_CONSTANTS, "test constants do not match T2")
    start = _integer(root.get("startMonotonicMs"), "startMonotonicMs")
    end = _integer(root.get("endMonotonicMs"), "endMonotonicMs")
    duration = _integer(root.get("durationMs"), "durationMs")
    _require(end >= start and duration == end - start, "duration does not match timestamps")
    _require(duration > 0, "duration must be positive for a hardware run")
    stack_start = _integer(root.get("initTaskStackHighWaterStartBytes"),
                           "initTaskStackHighWaterStartBytes")
    stack_end = _integer(root.get("initTaskStackHighWaterEndBytes"),
                         "initTaskStackHighWaterEndBytes")
    _require(stack_start > 0 and stack_end > 0, "InitTask stack high-water mark reached zero")
    _require(stack_end <= stack_start, "InitTask stack high-water mark increased")
    _validate_allocator(root, "heap")
    _validate_allocator(root, "psram")
    _require(_boolean(root.get("allPassed"), "allPassed"), "allPassed is false")
    return root


def load_and_validate(path: Path) -> dict[str, Any]:
    raw = path.read_bytes()
    _require(len(raw) <= MAX_EVIDENCE_BYTES, "evidence exceeds the 4096-byte output bound")
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"invalid UTF-8 JSON: {exc}") from exc
    return validate_document(document)


def _sha256(path: Path, max_bytes: int | None = None) -> str:
    digest = hashlib.sha256()
    total = 0
    with path.open("rb") as stream:
        while True:
            read_size = 65536 if max_bytes is None else min(65536, max_bytes - total + 1)
            chunk = stream.read(read_size)
            if not chunk:
                break
            total += len(chunk)
            if max_bytes is not None and total > max_bytes:
                raise EvidenceError(f"evidence file exceeds the {max_bytes}-byte bound: {path.name}")
            digest.update(chunk)
    return digest.hexdigest()


def _extract_serial_record(path: Path) -> bytes:
    _require(path.stat().st_size <= MAX_SERIAL_LOG_BYTES,
             "serial log exceeds the 65536-byte bound")
    with path.open("rb") as stream:
        raw = stream.read(MAX_SERIAL_LOG_BYTES + 1)
    _require(len(raw) <= MAX_SERIAL_LOG_BYTES, "serial log exceeds the 65536-byte bound")
    matches: list[bytes] = []
    for line in raw.splitlines():
        candidate = line.strip()
        if SERIAL_MARKER in candidate and candidate.startswith(b"{") and candidate.endswith(b"}"):
            _require(len(candidate) <= MAX_EVIDENCE_BYTES,
                     "serial JSON exceeds the 4096-byte output bound")
            matches.append(candidate + b"\n")
    _require(len(matches) == 1, "serial log must contain exactly one exhaustion JSON record")
    return matches[0]


def validate_run_bundle(raw_path: Path, document: dict[str, Any], manifest_path: Path) -> None:
    try:
        manifest = _object(json.loads(manifest_path.read_text(encoding="utf-8")), "manifest")
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"invalid run manifest: {exc}") from exc
    _require(manifest.get("schemaVersion") == 1, "manifest schemaVersion must be 1")
    _require(manifest.get("kind") == "v052-resource-exhaustion-run",
             "unexpected run manifest kind")
    _require(manifest.get("result") == "pass", "run manifest result must be pass")
    directory = manifest_path.resolve().parent
    entries = _object(manifest.get("evidence"), "manifest.evidence")
    resolved: dict[str, Path] = {}
    for key in ("rawJson", "serialLog", "profileMetadata", "staticSizeReport"):
        entry = _object(entries.get(key), f"manifest.evidence.{key}")
        relative = entry.get("path")
        expected_hash = entry.get("sha256")
        _require(isinstance(relative, str) and Path(relative).name == relative,
                 f"manifest.evidence.{key}.path must be a local filename")
        _require(isinstance(expected_hash, str) and len(expected_hash) == 64,
                 f"manifest.evidence.{key}.sha256 must be a SHA-256 hex digest")
        path = directory / relative
        _require(path.is_file(), f"manifest evidence file is missing: {relative}")
        digest = _sha256(path, MAX_SERIAL_LOG_BYTES if key == "serialLog" else None)
        _require(digest == expected_hash.lower(),
                 f"manifest evidence hash mismatch: {relative}")
        resolved[key] = path
    _require(resolved["rawJson"].resolve() == raw_path.resolve(),
             "manifest rawJson does not identify the validated raw evidence")
    _require(_extract_serial_record(resolved["serialLog"]) == raw_path.read_bytes(),
             "serial log record does not match raw JSON evidence")

    profile = _object(json.loads(resolved["profileMetadata"].read_text(encoding="utf-8")),
                      "profileMetadata")
    expected_profile = {
        "profileId": EXPECTED_PROFILE, "board": "waveshare_core2350b_16mb_psram8mb",
        "driverPack": "base", "testOnly": True, "persistentWritesAllowed": False,
        "cyclesPerAllocator": 100, "pointerCapacity": 512,
        "heapRequestBytes": 1024, "psramRequestBytes": 16384,
    }
    for key, value in expected_profile.items():
        _require(profile.get(key) == value, f"profile metadata mismatch: {key}")

    static_report = _object(json.loads(resolved["staticSizeReport"].read_text(encoding="utf-8")),
                            "staticSizeReport")
    firmware = _object(manifest.get("firmware"), "manifest.firmware")
    _require(firmware.get("version") == profile.get("firmwareVersion") == "0.5.2",
             "firmware version mismatch in bundle")
    _require(profile.get("gitCommit") == firmware.get("gitBaseline", "")[:12],
             "profile metadata git commit mismatch in bundle")
    _require(firmware.get("profileId") == document["profileId"],
             "firmware profile mismatch in bundle")
    _require(firmware.get("driverPack") == "base" and firmware.get("buildType") == "Debug",
             "firmware build identity mismatch in bundle")
    static_layout = _object(manifest.get("staticLayout"), "manifest.staticLayout")
    static_provenance = _object(static_report.get("provenance"), "staticSizeReport.provenance")
    _require(static_provenance.get("benchmarkProfile") == "resource-exhaustion",
             "static report is not for the resource-exhaustion profile")
    _require(static_provenance.get("profileId") ==
             "static-resource-exhaustion-base-waveshare_core2350b_16mb_psram8mb-debug",
             "unexpected static resource-exhaustion profileId")
    _require(static_provenance.get("boardRevision") == profile.get("board") and
             static_provenance.get("driverPack") == profile.get("driverPack") and
             static_provenance.get("buildType") == firmware.get("buildType") and
             static_provenance.get("firmwareVersion") == firmware.get("version") and
             static_provenance.get("gitCommit") == firmware.get("gitBaseline") and
             static_provenance.get("gitDirty") == firmware.get("gitDirty") and
             static_provenance.get("securityPolicy") == "not-applicable" and
             static_provenance.get("securityMode") == "not-applicable",
             "static report provenance does not match run identity")
    totals = _object(static_report.get("totals"), "staticSizeReport.totals")
    artifacts = _object(static_report.get("artifacts"), "staticSizeReport.artifacts")
    uf2 = _object(artifacts.get("uf2"), "staticSizeReport.artifacts.uf2")
    _require(uf2.get("sha256") == firmware.get("uf2Sha256"), "UF2 hash mismatch in bundle")
    for manifest_key, report_key in (("externalFlashLoadBytes", "ExternalFlashLoadBytes"),
                                     ("internalSramStaticBytes", "SramStaticTotalBytes"),
                                     ("uf2FileBytes", "Uf2FileBytes")):
        _require(static_layout.get(manifest_key) == totals.get(report_key),
                 f"static layout mismatch: {manifest_key}")
    heap_reservation = _object(_object(static_report.get("reservations"),
                                      "staticSizeReport.reservations").get("freeRtosHeap"),
                               "staticSizeReport.reservations.freeRtosHeap")
    _require(static_layout.get("freeRtosHeapReservationBytes") == heap_reservation.get("bytes"),
             "FreeRTOS heap reservation mismatch in bundle")

    workload = _object(manifest.get("workload"), "manifest.workload")
    _require(workload.get("durationMs") == document["durationMs"], "duration mismatch in bundle")
    for manifest_key, raw_key in (("heapCycles", "cycles"), ("psramCycles", "cycles"),
                                  ("heapRequestBytes", "heapRequestBytes"),
                                  ("psramRequestBytes", "psramRequestBytes"),
                                  ("pointerCapacity", "pointerCapacity")):
        _require(workload.get(manifest_key) == document["constants"][raw_key],
                 f"workload mismatch in bundle: {manifest_key}")
    runtime = _object(manifest.get("runtime"), "manifest.runtime")
    heap = _object(runtime.get("heap"), "manifest.runtime.heap")
    psram = _object(runtime.get("psram"), "manifest.runtime.psram")
    stack = _object(runtime.get("initTaskStack"), "manifest.runtime.initTaskStack")
    for name, summary in (("heap", heap), ("psram", psram)):
        raw = document[name]
        _require(summary.get("blocksPerCycle") == raw["minBlockCount"] == raw["maxBlockCount"],
                 f"{name} block count mismatch in bundle")
        _require(summary.get("usedBytesBaseline") == raw["initial"]["usedBytes"],
                 f"{name} baseline use mismatch in bundle")
        _require(summary.get("usedBytesFinal") == raw["final"]["usedBytes"],
                 f"{name} final use mismatch in bundle")
        _require(summary.get("peakUsedBytes") == raw["final"]["peakUsedBytes"],
                 f"{name} peak use mismatch in bundle")
        _require(summary.get("peakDeltaFromBaselineBytes") ==
                 raw["final"]["peakUsedBytes"] - raw["initial"]["usedBytes"],
                 f"{name} peak delta mismatch in bundle")
        _require(summary.get("failedAllocationDelta") == 100,
                 f"{name} failure delta mismatch in bundle")
    _require(heap.get("minimumEverFreeBytes") == document["heap"]["final"]["minimumEverFreeBytes"],
             "heap minimum-ever mismatch in bundle")
    _require(psram.get("requestedBytesBaseline") == document["psram"]["initial"]["requestedBytes"] and
             psram.get("requestedBytesFinal") == document["psram"]["final"]["requestedBytes"],
             "PSRAM requested-byte mismatch in bundle")
    _require(stack.get("highWaterStartBytes") == document["initTaskStackHighWaterStartBytes"],
             "stack start mismatch in bundle")
    _require(stack.get("highWaterEndBytes") == document["initTaskStackHighWaterEndBytes"],
             "stack end mismatch in bundle")
    _require(stack.get("highWaterDecreaseBytes") ==
             document["initTaskStackHighWaterStartBytes"] -
             document["initTaskStackHighWaterEndBytes"],
             "stack delta mismatch in bundle")
    observations = _object(manifest.get("observations"), "manifest.observations")
    for field in ("allCyclesCompleted", "payloadVerified", "baselineReturned",
                  "failureCountersExact", "oomReasonsExact",
                  "lowWaterStableAfterFirstCycle"):
        _require(observations.get(field) is True, f"manifest observation is not true: {field}")
    _require(observations.get("rebootObserved") is False and
             observations.get("usbDisconnectObserved") is False,
             "manifest records a reboot or USB disconnect")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    try:
        document = load_and_validate(args.evidence)
        if args.manifest is not None:
            validate_run_bundle(args.evidence, document, args.manifest)
    except (OSError, EvidenceError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        print(f"resource exhaustion evidence invalid: {exc}", file=sys.stderr)
        return 1
    print(json.dumps({"status": "pass", "profileId": document["profileId"],
                      "durationMs": document["durationMs"]}, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
