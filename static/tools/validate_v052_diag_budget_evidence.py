#!/usr/bin/env python3
"""Validate committed v0.5.2 diagnostic snapshot budget evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
from typing import Any


PROFILE_ID = "v052-syn-c1-ns0full-secbasic256sha256-signandencrypt-cl1-hz1-ev0-hist0"
PHASES = {"acquire", "copy", "heap", "psram", "lwip", "tasks", "publish"}
SENSITIVE_FRAGMENTS = (
    "password", "privatekey", "private_key", "credential", "secret", "token", "username",
)


class EvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(path: pathlib.Path) -> dict[str, Any]:
    require(path.is_file(), f"missing evidence file: {path}")
    require(path.stat().st_size <= 128 * 1024, f"evidence file is too large: {path.name}")
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=reject_duplicate_keys)
    require(isinstance(value, dict), f"JSON root must be an object: {path.name}")
    return value


def exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    require(set(value) == expected, f"{label} keys mismatch")


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_descriptor(root: pathlib.Path, descriptor: dict[str, Any], label: str) -> pathlib.Path:
    exact_keys(descriptor, {"path", "sizeBytes", "sha256"}, label)
    relative = descriptor["path"]
    require(isinstance(relative, str) and pathlib.PurePath(relative).name == relative,
            f"{label} path must be a local filename")
    path = root / relative
    require(path.is_file(), f"missing {label}: {relative}")
    require(path.stat().st_size <= 128 * 1024, f"{label} is too large")
    require(path.stat().st_size == descriptor["sizeBytes"], f"{label} size mismatch")
    require(sha256(path) == descriptor["sha256"], f"{label} hash mismatch")
    return path


def reject_sensitive_keys(value: Any, path: str = "root") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            normalized = key.lower().replace("-", "").replace("_", "")
            require(not any(fragment.replace("_", "") in normalized for fragment in SENSITIVE_FRAGMENTS),
                    f"sensitive key forbidden at {path}.{key}")
            reject_sensitive_keys(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_sensitive_keys(child, f"{path}[{index}]")


def validate(manifest_path: pathlib.Path) -> None:
    manifest = load_json(manifest_path)
    exact_keys(manifest, {
        "schemaVersion", "kind", "firmwareVersion", "sourceBaselineCommit",
        "profileId", "board", "measuredOn", "transport", "firmwareArtifact",
        "files", "limits", "scope",
    }, "manifest")
    reject_sensitive_keys(manifest)
    require(manifest["schemaVersion"] == 1, "manifest schemaVersion mismatch")
    require(manifest["kind"] == "v052-diag-snapshot-budget-evidence-manifest",
            "manifest kind mismatch")
    require(manifest["firmwareVersion"] == "0.5.2", "firmware version mismatch")
    require(manifest["sourceBaselineCommit"] == "2881230c2ccbb98b8701730c4de930dc95da30fc",
            "source baseline mismatch")
    require(manifest["profileId"] == PROFILE_ID, "manifest profile mismatch")
    require(manifest["board"] == "waveshare_core2350b_16mb_psram8mb", "board mismatch")

    artifact = manifest["firmwareArtifact"]
    exact_keys(artifact, {"name", "sizeBytes", "sha256"}, "firmwareArtifact")
    require(artifact["name"] == "device_profile_basic.uf2", "UF2 name mismatch")
    require(isinstance(artifact["sizeBytes"], int) and artifact["sizeBytes"] > 0,
            "UF2 size is invalid")
    require(isinstance(artifact["sha256"], str) and len(artifact["sha256"]) == 64,
            "UF2 hash is invalid")

    files = manifest["files"]
    exact_keys(files, {"measurement", "serialCapture", "profile", "staticSize",
                       "staticComparisonBaseline", "stackComparison",
                       "stackConfiguration", "v051StackCapture", "v052StackCapture"},
               "files")
    require(files["staticComparisonBaseline"] == {
        "path": "secure-c1-opcua-resources-static-size.json",
        "sizeBytes": 11526,
        "sha256": "f0d50488056af03ab9e9f100502b5521fadbc75685bd856dcd602fabdb0b0f8c",
    }, "static comparison baseline descriptor mismatch")
    root = manifest_path.parent
    measurement_path = validate_descriptor(root, files["measurement"], "measurement")
    serial_path = validate_descriptor(root, files["serialCapture"], "serialCapture")
    profile_path = validate_descriptor(root, files["profile"], "profile")
    static_path = validate_descriptor(root, files["staticSize"], "staticSize")
    baseline_path = validate_descriptor(root, files["staticComparisonBaseline"],
                                        "staticComparisonBaseline")
    stack_comparison_path = validate_descriptor(root, files["stackComparison"],
                                                "stackComparison")
    stack_configuration_path = validate_descriptor(root, files["stackConfiguration"],
                                                   "stackConfiguration")
    v051_stack_capture_path = validate_descriptor(root, files["v051StackCapture"],
                                                  "v051StackCapture")
    v052_stack_capture_path = validate_descriptor(root, files["v052StackCapture"],
                                                  "v052StackCapture")

    limits = manifest["limits"]
    exact_keys(limits, {"snapshotP99Us", "sampleCount", "captureLimitSeconds",
                        "additionalInternalSramBytes", "opcUaTaskStackReserveReductionBytes"},
               "limits")
    require(limits == {
        "snapshotP99Us": 2000,
        "sampleCount": 100,
        "captureLimitSeconds": 120,
        "additionalInternalSramBytes": 4096,
        "opcUaTaskStackReserveReductionBytes": 256,
    }, "release limits mismatch")

    measurement = load_json(measurement_path)
    reject_sensitive_keys(measurement)
    exact_keys(measurement, {
        "schemaVersion", "kind", "state", "profileId", "clock", "sampleTarget",
        "captureLimitSeconds", "sampleCount", "captureStartedMonotonicUs",
        "captureEndedMonotonicUs", "captureDurationUs", "diagServiceSampleDurationUs",
        "opcUaTaskStackHighWaterBytes", "phaseMaxUs", "maxSamplePhaseUs",
        "baselineComparisonState",
    }, "measurement")
    require(measurement["schemaVersion"] == 1, "measurement schema mismatch")
    require(measurement["kind"] == "v052-diag-snapshot-budget-evidence",
            "measurement kind mismatch")
    require(measurement["state"] == "complete", "measurement is incomplete")
    require(measurement["profileId"] == PROFILE_ID, "measurement profile mismatch")
    require(measurement["clock"] == {
        "domain": "device-monotonic", "unit": "microseconds", "valid": True,
    }, "measurement clock mismatch")
    require(measurement["sampleTarget"] == limits["sampleCount"] and
            measurement["sampleCount"] == limits["sampleCount"], "sample count mismatch")
    require(measurement["captureLimitSeconds"] == limits["captureLimitSeconds"],
            "capture limit mismatch")
    started = measurement["captureStartedMonotonicUs"]
    ended = measurement["captureEndedMonotonicUs"]
    duration = measurement["captureDurationUs"]
    require(all(isinstance(item, int) and item >= 0 for item in (started, ended, duration)),
            "capture times must be non-negative integers")
    require(ended >= started and ended - started == duration, "capture duration mismatch")
    require(duration <= limits["captureLimitSeconds"] * 1_000_000,
            "capture exceeded its bounded window")

    latency = measurement["diagServiceSampleDurationUs"]
    exact_keys(latency, {"p99", "max"}, "diagServiceSampleDurationUs")
    require(isinstance(latency["p99"], int) and 0 < latency["p99"] <= limits["snapshotP99Us"],
            "snapshot p99 exceeds release limit")
    require(isinstance(latency["max"], int) and latency["max"] >= latency["p99"],
            "snapshot max is invalid")
    require(isinstance(measurement["opcUaTaskStackHighWaterBytes"], int) and
            measurement["opcUaTaskStackHighWaterBytes"] > 0, "stack HWM is invalid")
    require(measurement["baselineComparisonState"] == "not-run",
            "unexpected stack baseline comparison state")
    for field in ("phaseMaxUs", "maxSamplePhaseUs"):
        phases = measurement[field]
        exact_keys(phases, PHASES, field)
        require(all(isinstance(value, int) and value >= 0 for value in phases.values()),
                f"{field} values are invalid")
    require(all(measurement["phaseMaxUs"][phase] >= measurement["maxSamplePhaseUs"][phase]
                for phase in PHASES), "phase maxima are inconsistent")
    require(sum(measurement["maxSamplePhaseUs"].values()) <= latency["max"],
            "maximum-sample phase sum exceeds total")

    profile = load_json(profile_path)
    reject_sensitive_keys(profile)
    exact_keys(profile, {
        "profileSchemaVersion", "profileId", "firmwareVersion", "buildGitHash",
        "buildTime", "board", "driverPack", "namespaceZero", "security",
        "channels", "rateHz", "clients", "events", "history", "testOnly",
        "hardwareState", "clientCountMeaning", "configuredCapacities",
    }, "profile")
    exact_keys(profile["configuredCapacities"], {
        "sessions", "secureChannels", "subscriptions", "monitoredItems",
        "publishRequestsPerSession",
    }, "profile.configuredCapacities")
    require(profile.get("profileId") == PROFILE_ID, "profile metadata mismatch")
    require(profile.get("firmwareVersion") == "0.5.2", "profile firmware mismatch")
    require(profile.get("board") == manifest["board"] and profile.get("driverPack") == "base",
            "profile target mismatch")
    require((profile.get("namespaceZero"), profile.get("security"), profile.get("channels"),
             profile.get("rateHz"), profile.get("clients"), profile.get("events"),
             profile.get("history"), profile.get("testOnly")) ==
            ("full", "basic256sha256-signandencrypt", 1, 1, 1, False, False, True),
            "profile workload axes mismatch")

    static = load_json(static_path)
    baseline = load_json(baseline_path)
    static_top_keys = {
        "schemaVersion", "contractId", "measurementKind", "provenance", "artifacts",
        "memoryRegions", "sections", "reservations", "totals", "thresholds", "notes",
    }
    provenance_keys = {
        "profileSchemaVersion", "profileId", "firmwareVersion", "gitCommit", "gitDirty",
        "buildType", "toolchain", "buildFlags", "driverPack", "boardRevision",
        "benchmarkProfile", "benchmarkProfileId", "namespaceZero", "securityPolicy",
        "securityMode",
    }
    total_keys = {
        "ExternalFlashLoadBytes", "SramDataBytes", "SramBssBytes",
        "SramStaticTotalBytes", "PsramStaticBytes", "SramReservedHeapBytes",
        "SramReservedTaskStackBytes", "ElfFileBytes", "Uf2FileBytes", "BinFileBytes",
    }
    for label, report in (("staticSize", static),
                          ("staticComparisonBaseline", baseline)):
        reject_sensitive_keys(report)
        exact_keys(report, static_top_keys, label)
        exact_keys(report["provenance"], provenance_keys, f"{label}.provenance")
        exact_keys(report["artifacts"], {"elf", "uf2", "bin", "map"},
                   f"{label}.artifacts")
        for artifact_name, artifact_descriptor in report["artifacts"].items():
            exact_keys(artifact_descriptor, {"name", "sizeBytes", "sha256"},
                       f"{label}.artifacts.{artifact_name}")
        exact_keys(report["totals"], total_keys, f"{label}.totals")
    require(static.get("provenance", {}).get("benchmarkProfileId") == PROFILE_ID,
            "static report profile mismatch")
    require(baseline.get("provenance", {}).get("benchmarkProfileId") == PROFILE_ID and
            baseline.get("provenance", {}).get("buildType") == "Debug" and
            baseline.get("provenance", {}).get("driverPack") == "base" and
            baseline.get("provenance", {}).get("namespaceZero") == "full" and
            baseline.get("provenance", {}).get("securityPolicy") == "Basic256Sha256" and
            baseline.get("provenance", {}).get("securityMode") == "SignAndEncrypt",
            "static comparison baseline provenance mismatch")
    require(baseline.get("artifacts", {}).get("uf2") == {
        "name": "device_profile_basic.uf2",
        "sizeBytes": 7092224,
        "sha256": "5f19f2442e14b3374b1e8bd9e385e68259492e20ae6d97e782b1a8fbfddc3d60",
    }, "static comparison baseline UF2 mismatch")
    require(static.get("artifacts", {}).get("uf2") == artifact,
            "static report is not bound to the measured UF2")
    current_sram = static.get("totals", {}).get("SramStaticTotalBytes")
    baseline_sram = baseline.get("totals", {}).get("SramStaticTotalBytes")
    require(isinstance(current_sram, int) and isinstance(baseline_sram, int),
            "static SRAM totals are unavailable")
    require(0 <= current_sram - baseline_sram <= limits["additionalInternalSramBytes"],
            "incremental timing SRAM exceeds release limit")

    stack = load_json(stack_comparison_path)
    reject_sensitive_keys(stack)
    exact_keys(stack, {
        "schemaVersion", "kind", "measuredOn", "board", "sourceBaselineCommit",
        "workload", "baseline", "current", "reserveReductionBytes", "limitBytes",
        "headroomBytes", "result",
    }, "stackComparison")
    require(stack["schemaVersion"] == 1 and
            stack["kind"] == "v052-opcua-task-stack-baseline-comparison",
            "stack comparison schema mismatch")
    require(stack["board"] == manifest["board"] and
            stack["sourceBaselineCommit"] == manifest["sourceBaselineCommit"],
            "stack comparison target mismatch")
    workload = stack["workload"]
    exact_keys(workload, {
        "buildProfile", "configuration", "channels", "rateHz", "clients",
        "namespaceZero", "security", "channelId", "clientTool", "clientOperations",
        "stackNodeId", "reportedUnit", "bytesPerWord",
    }, "stackComparison.workload")
    require((workload["buildProfile"], workload["configuration"], workload["channels"],
             workload["rateHz"], workload["clients"], workload["namespaceZero"],
             workload["security"], workload["channelId"], workload["clientTool"],
             workload["clientOperations"], workload["stackNodeId"],
             workload["reportedUnit"], workload["bytesPerWord"]) ==
            ("production-debug-base", files["stackConfiguration"]["path"], 1, 1, 1,
             "full", "basic256sha256-signandencrypt", "benchmark.synthetic.1",
             "static/tools/verify_channel_model.py",
             "identity, diagnostics and deterministic channel Browse/Read",
             "ns=1;s=Device.Diagnostics.Task.OPC.StackHighWatermark",
             "StackType_t words", 4), "stack comparison workload mismatch")

    configuration = load_json(stack_configuration_path)
    reject_sensitive_keys(configuration)
    require(configuration.get("schemaVersion") == 1, "stack configuration schema mismatch")
    require(configuration.get("diagnostics", {}).get("enableTaskDiagnostics") is True,
            "stack configuration disables task diagnostics")
    channels = configuration.get("channels")
    require(isinstance(channels, list) and len(channels) == 16,
            "stack configuration channel capacity mismatch")
    require((channels[0].get("inUse"), channels[0].get("enabled"),
             channels[0].get("driverId"), channels[0].get("id"),
             channels[0].get("samplePeriodMs"), channels[0].get("publishPeriodMs"),
             channels[0].get("flags")) ==
            (True, True, 10, "benchmark.synthetic.1", 1000, 1000, 1),
            "stack configuration active channel mismatch")
    require(all(channel.get("inUse") is False and channel.get("enabled") is False
                for channel in channels[1:]), "stack configuration has extra active channels")

    def validate_stack_capture(path: pathlib.Path, expected_version: str,
                               expected_words: int) -> None:
        text = path.read_text(encoding="utf-8", errors="strict")
        require(len(text.encode("utf-8")) <= 64 * 1024, "stack capture is too large")
        lower = text.lower()
        require(not any(fragment in lower for fragment in SENSITIVE_FRAGMENTS),
                "stack capture contains a sensitive-data marker")
        require("verify-channel-model: PASS" in text, "stack capture did not pass")
        identity_lines = [line for line in text.splitlines()
                          if line.startswith("Device identity: ")]
        diagnostic_lines = [line for line in text.splitlines()
                            if line.startswith("Resource diagnostics: ")]
        require(len(identity_lines) == 1 and len(diagnostic_lines) == 1,
                "stack capture payload count mismatch")
        identity = json.loads(identity_lines[0].removeprefix("Device identity: "))
        diagnostics = json.loads(diagnostic_lines[0].removeprefix("Resource diagnostics: "))
        require(identity.get("softwareRevision") == expected_version,
                "stack capture firmware version mismatch")
        require(diagnostics.get("opcStackHighWatermark") == expected_words,
                "stack capture high-water mark mismatch")

    expected_stack_artifacts = {
        "baseline": {
            "name": "device_profile_basic.uf2", "sizeBytes": 7122432,
            "sha256": "4c24871ba55622d74fcf1ce4369ace2fe14dd55c01201d7bbe01a0fb8b48fcec",
        },
        "current": {
            "name": "device_profile_basic.uf2", "sizeBytes": 7174144,
            "sha256": "0bae63ebf217231ab23087ab013d457359bb7cb8648c4029d3faaabcb2a3be09",
        },
    }
    for label, expected_version, descriptor, capture_path in (
        ("baseline", "0.5.1", files["v051StackCapture"], v051_stack_capture_path),
        ("current", "0.5.2", files["v052StackCapture"], v052_stack_capture_path),
    ):
        value = stack[label]
        exact_keys(value, {"firmwareVersion", "firmwareArtifact", "capture",
                           "stackHighWaterWords", "stackHighWaterBytes"},
                   f"stackComparison.{label}")
        artifact_value = value["firmwareArtifact"]
        exact_keys(artifact_value, {"name", "sizeBytes", "sha256"},
                   f"stackComparison.{label}.firmwareArtifact")
        require(value["firmwareVersion"] == expected_version,
                f"stackComparison.{label} version mismatch")
        require(value["capture"] == descriptor["path"],
                f"stackComparison.{label} capture mismatch")
        words = value["stackHighWaterWords"]
        require(isinstance(words, int) and words > 0 and
                value["stackHighWaterBytes"] == words * workload["bytesPerWord"],
                f"stackComparison.{label} unit conversion mismatch")
        require(artifact_value == expected_stack_artifacts[label],
                f"stackComparison.{label} artifact mismatch")
        validate_stack_capture(capture_path, expected_version, words)

    reduction = stack["baseline"]["stackHighWaterBytes"] - stack["current"]["stackHighWaterBytes"]
    require(stack["reserveReductionBytes"] == reduction and reduction >= 0,
            "stack reserve reduction mismatch")
    require(stack["limitBytes"] == limits["opcUaTaskStackReserveReductionBytes"],
            "stack comparison limit mismatch")
    require(reduction <= stack["limitBytes"], "stack reserve reduction exceeds release limit")
    require(stack["headroomBytes"] == stack["limitBytes"] - reduction,
            "stack comparison headroom mismatch")
    require(stack["result"] == "passed", "stack comparison did not pass")

    serial_text = serial_path.read_text(encoding="utf-8", errors="strict")
    require(len(serial_text.encode("utf-8")) <= 64 * 1024, "serial capture is too large")
    serial_lower = serial_text.lower()
    require(not any(fragment in serial_lower for fragment in SENSITIVE_FRAGMENTS),
            "serial capture contains a sensitive-data marker")
    serial_measurements = []
    for line in serial_text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("{"):
            continue
        try:
            candidate = json.loads(stripped, object_pairs_hook=reject_duplicate_keys)
        except json.JSONDecodeError:
            continue
        if (isinstance(candidate, dict) and
                candidate.get("kind") == "v052-diag-snapshot-budget-evidence"):
            serial_measurements.append(candidate)
    require(len(serial_measurements) == 1,
            "serial capture must contain exactly one raw measurement")
    require(serial_measurements[0] == measurement,
            "serial capture measurement differs from measurement JSON")

    scope = manifest["scope"]
    require(scope == {
        "referenceBoardLive": "measured",
        "collectionSnapshotP99": "passed",
        "staticSram": "measured",
        "incrementalTimingSramDelta": "passed",
        "opcUaTaskStackCurrentReserve": "measured",
        "opcUaTaskStackDeltaVsV051": "passed",
        "uaExpertInspection": "not-run",
    }, "manifest scope mismatch")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    args = parser.parse_args()
    try:
        validate(args.manifest)
    except (EvidenceError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"v0.5.2 diagnostic budget evidence: FAIL: {exc}", file=sys.stderr)
        return 1
    print("v0.5.2 diagnostic budget evidence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
