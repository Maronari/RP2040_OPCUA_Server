#!/usr/bin/env python3
"""Validate committed live v0.5.2 OPC UA resource evidence and provenance."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


MAX_MANIFEST_BYTES = 16 * 1024
MAX_EVIDENCE_BYTES = 64 * 1024
MAX_PROFILE_BYTES = 16 * 1024
MAX_STATIC_REPORT_BYTES = 8 * 1024 * 1024
EXPECTED_BASELINE = "2881230c2ccbb98b8701730c4de930dc95da30fc"
EXPECTED_BOARD = "waveshare_core2350b_16mb_psram8mb"
EXPECTED_PROFILE = (
    "v052-syn-c1-ns0full-secbasic256sha256-signandencrypt-cl1-hz1-ev0-hist0"
)
SHA256_RE = re.compile(r"[0-9a-f]{64}")
SENSITIVE_KEY_FRAGMENTS = ("password", "username", "credential", "secret", "token", "privatekey")


class EvidenceError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise EvidenceError(message)


def obj(value: Any, path: str) -> dict[str, Any]:
    require(isinstance(value, dict), f"{path} must be an object")
    return value


def nonnegative_int(value: Any, path: str) -> int:
    require(isinstance(value, int) and not isinstance(value, bool) and value >= 0,
            f"{path} must be a non-negative integer")
    return value


def exact_keys(value: dict[str, Any], expected: set[str], path: str) -> None:
    require(set(value) == expected,
            f"{path} keys mismatch: expected {sorted(expected)}, got {sorted(value)}")


def read_json(path: Path, maximum: int) -> tuple[dict[str, Any], bytes]:
    require(path.is_file(), f"missing evidence file: {path}")
    size = path.stat().st_size
    require(0 < size <= maximum, f"{path.name} exceeds its byte bound or is empty")
    raw = path.read_bytes()
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"{path.name} is not valid UTF-8 JSON") from exc
    return obj(value, path.name), raw


def reject_credentials(value: Any, path: str = "document") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            folded = re.sub(r"[^a-z]", "", str(key).lower())
            require(not any(fragment in folded for fragment in SENSITIVE_KEY_FRAGMENTS),
                    f"{path} contains forbidden credential/endpoint key {key}")
            reject_credentials(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_credentials(child, f"{path}[{index}]")


def validate_report(report: dict[str, Any]) -> None:
    exact_keys(report, {"schemaVersion", "kind", "result", "server", "security",
                        "roles", "checks", "profile", "runtime", "elapsedMonotonicMs"},
               "verification")
    require(report.get("schemaVersion") == 1, "verification schemaVersion must be 1")
    require(report.get("kind") == "v052-opcua-resource-verification",
            "verification kind is invalid")
    result = obj(report.get("result"), "result")
    exact_keys(result, {"state"}, "result")
    require(result.get("state") == "passed",
            "verification result must be passed")
    server = obj(report.get("server"), "server")
    exact_keys(server, {"authority", "port", "scheme"}, "server")
    require(server == {"authority": "redacted", "port": 4840, "scheme": "opc.tcp"},
            "server identity must contain only the redacted authority and expected port")
    security = obj(report.get("security"), "security")
    exact_keys(security, {"autoClientCertificateDisabled", "clientCertificateConfigured",
                          "mode", "policy", "serverCertificateConfigured"}, "security")
    require(security.get("policy") == "Basic256Sha256", "security policy mismatch")
    require(security.get("mode") == "SignAndEncrypt", "security mode mismatch")

    roles = report.get("roles")
    require(isinstance(roles, list) and len(roles) == 5, "roles must contain five entries")
    by_name = {entry.get("role"): obj(entry, "roles[]") for entry in roles
               if isinstance(entry, dict)}
    require(set(by_name) == {"service", "admin", "viewer", "operator", "anonymous"},
            "roles set is invalid or duplicated")
    for role in ("service", "admin"):
        exact_keys(by_name[role], {"role", "authorization", "browse", "directRead", "write"},
                   f"roles.{role}")
        require(by_name[role].get("authorization") == "privileged",
                f"{role} authorization mismatch")
    for role in ("viewer", "operator"):
        exact_keys(by_name[role], {"role", "authorization", "browse", "directRead", "write"},
                   f"roles.{role}")
        require(by_name[role].get("authorization") == "unprivileged",
                f"{role} authorization mismatch")
    for role in ("service", "admin", "viewer", "operator"):
        for operation in ("browse", "directRead", "write"):
            require(by_name[role].get(operation) == "measured",
                    f"{role}.{operation} must be measured")
    anonymous = by_name["anonymous"]
    exact_keys(anonymous, {"role", "authorization", "anonymousOutcome", "browse",
                           "directRead", "write"}, "roles.anonymous")
    require(anonymous.get("authorization") == "unprivileged",
            "anonymous authorization mismatch")
    require(anonymous.get("anonymousOutcome") == "activation-denied",
            "anonymous activation must be denied")
    for operation in ("browse", "directRead", "write"):
        require(anonymous.get(operation) == "not-run",
                f"anonymous.{operation} must be not-run after activation denial")

    checks = obj(report.get("checks"), "checks")
    exact_keys(checks, {"browse", "directRead", "write", "capacity"}, "checks")
    browse = obj(checks.get("browse"), "checks.browse")
    direct = obj(checks.get("directRead"), "checks.directRead")
    write = obj(checks.get("write"), "checks.write")
    capacity = obj(checks.get("capacity"), "checks.capacity")
    exact_keys(browse, {"roleCount", "state"}, "checks.browse")
    exact_keys(direct, {"nodeCountPerRole", "roleCount", "state"}, "checks.directRead")
    exact_keys(write, {"deniedRoleCount", "nodeCountPerRole", "probeNode", "roleCount", "state"},
               "checks.write")
    exact_keys(capacity, {"extraServiceClients", "state"}, "checks.capacity")
    require(browse == {"roleCount": 4, "state": "measured"}, "Browse evidence mismatch")
    require(direct == {"nodeCountPerRole": 22, "roleCount": 4, "state": "measured"},
            "direct Read evidence mismatch")
    require(write.get("state") == "measured" and write.get("roleCount") == 4 and
            write.get("deniedRoleCount") == 4 and write.get("nodeCountPerRole") == 1,
            "write-denial evidence mismatch")
    require(write.get("probeNode") ==
            "ns=1;s=Device.Diagnostics.OpcUa.Sessions.Capacity",
            "write probe NodeId mismatch")
    require(capacity == {"extraServiceClients": 2, "state": "measured"},
            "capacity exercise evidence mismatch")
    nonnegative_int(report.get("elapsedMonotonicMs"), "elapsedMonotonicMs")
    run_profile = obj(report.get("profile"), "profile")
    exact_keys(run_profile, {"capacityExerciseEnabled", "extraServiceClients", "id",
                             "requestTimeoutMs", "snapshotSettleMs"}, "profile")
    runtime = obj(report.get("runtime"), "runtime")
    exact_keys(runtime, {"asyncua", "python", "tool", "toolVersion"}, "runtime")
    reject_credentials(report)


def validate_profile(profile: dict[str, Any]) -> None:
    exact_keys(profile, {"profileSchemaVersion", "profileId", "firmwareVersion",
                         "buildGitHash", "buildTime", "board", "driverPack",
                         "namespaceZero", "security", "channels", "rateHz", "clients",
                         "events", "history", "testOnly", "hardwareState",
                         "clientCountMeaning", "configuredCapacities"}, "buildProfile")
    require(profile.get("profileSchemaVersion") == 1, "profile schema mismatch")
    require(profile.get("profileId") == EXPECTED_PROFILE, "profile ID mismatch")
    require(profile.get("firmwareVersion") == "0.5.2", "profile firmware version mismatch")
    require(profile.get("buildGitHash") == EXPECTED_BASELINE[:12], "profile source hash mismatch")
    require(profile.get("board") == EXPECTED_BOARD, "profile board mismatch")
    require(profile.get("driverPack") == "base", "profile driver pack mismatch")
    require(profile.get("namespaceZero") == "full", "profile Namespace 0 mismatch")
    require(profile.get("security") == "basic256sha256-signandencrypt",
            "profile security mismatch")
    require(profile.get("channels") == 1 and profile.get("clients") == 1 and
            profile.get("rateHz") == 1, "profile workload mismatch")
    require(profile.get("events") is False and profile.get("history") is False and
            profile.get("testOnly") is True, "profile feature flags mismatch")
    capacities = obj(profile.get("configuredCapacities"), "configuredCapacities")
    exact_keys(capacities, {"sessions", "secureChannels", "subscriptions",
                            "monitoredItems", "publishRequestsPerSession"},
               "configuredCapacities")
    reject_credentials(profile)


def validate_static_report(report: dict[str, Any], artifact: dict[str, Any]) -> None:
    exact_keys(report, {"schemaVersion", "contractId", "measurementKind", "provenance",
                        "artifacts", "memoryRegions", "sections", "totals", "reservations",
                        "notes", "thresholds"}, "static")
    require(report.get("schemaVersion") == 1, "static report schema mismatch")
    require(report.get("contractId") == "rp2-opcua-v0.5.2-resource-benchmark",
            "static report contract mismatch")
    provenance = obj(report.get("provenance"), "static.provenance")
    exact_keys(provenance, {"profileSchemaVersion", "profileId", "benchmarkProfileId",
                            "benchmarkProfile", "firmwareVersion", "gitCommit", "gitDirty",
                            "boardRevision", "driverPack", "namespaceZero", "securityPolicy",
                            "securityMode", "toolchain", "buildType", "buildFlags"},
               "static.provenance")
    require(provenance.get("gitCommit") == EXPECTED_BASELINE, "static source hash mismatch")
    require(provenance.get("gitDirty") is True, "dirty source state must be disclosed")
    require(provenance.get("boardRevision") == EXPECTED_BOARD, "static board mismatch")
    require(provenance.get("benchmarkProfile") == "synthetic", "static profile kind mismatch")
    require(provenance.get("benchmarkProfileId") == EXPECTED_PROFILE,
            "static benchmark profile ID mismatch")
    require(provenance.get("driverPack") == "base", "static driver pack mismatch")
    require(provenance.get("buildType") == "Debug", "static build type mismatch")
    require(provenance.get("firmwareVersion") == "0.5.2", "static version mismatch")
    require(provenance.get("securityPolicy") == "Basic256Sha256" and
            provenance.get("securityMode") == "SignAndEncrypt", "static security mismatch")
    flags = obj(provenance.get("buildFlags"), "static.provenance.buildFlags")
    for key, expected in {
        "FIRMWARE_BENCHMARK_PROFILE": "synthetic",
        "FIRMWARE_BENCHMARK_SECURITY": "basic256sha256-signandencrypt",
        "FIRMWARE_BENCHMARK_CHANNELS": "1",
        "FIRMWARE_BENCHMARK_RATE_HZ": "1",
        "FIRMWARE_BENCHMARK_CLIENTS": "1",
        "FIRMWARE_BENCHMARK_EVENTS": "OFF",
        "FIRMWARE_BENCHMARK_HISTORY": "OFF",
    }.items():
        require(flags.get(key) == expected, f"static build flag {key} mismatch")
    uf2 = obj(obj(report.get("artifacts"), "static.artifacts").get("uf2"),
              "static.artifacts.uf2")
    exact_keys(uf2, {"name", "sizeBytes", "sha256"}, "static.artifacts.uf2")
    require(uf2 == artifact, "static report UF2 identity differs from manifest")


def validate_manifest(path: Path) -> None:
    manifest, _ = read_json(path, MAX_MANIFEST_BYTES)
    exact_keys(manifest, {"schemaVersion", "kind", "firmwareVersion",
                          "sourceBaselineCommit", "profileId", "board",
                          "firmwareArtifact", "files", "scope"}, "manifest")
    require(manifest.get("schemaVersion") == 1, "manifest schemaVersion must be 1")
    require(manifest.get("kind") == "v052-opcua-resource-evidence-manifest",
            "manifest kind mismatch")
    require(manifest.get("firmwareVersion") == "0.5.2", "manifest version mismatch")
    require(manifest.get("sourceBaselineCommit") == EXPECTED_BASELINE,
            "manifest source baseline mismatch")
    require(manifest.get("profileId") == EXPECTED_PROFILE, "manifest profile mismatch")
    require(manifest.get("board") == EXPECTED_BOARD, "manifest board mismatch")
    artifact = obj(manifest.get("firmwareArtifact"), "firmwareArtifact")
    exact_keys(artifact, {"name", "sizeBytes", "sha256"}, "firmwareArtifact")
    require(artifact.get("name") == "device_profile_basic.uf2", "UF2 name mismatch")
    require(SHA256_RE.fullmatch(str(artifact.get("sha256", ""))) is not None,
            "UF2 SHA-256 is invalid")
    nonnegative_int(artifact.get("sizeBytes"), "firmwareArtifact.sizeBytes")

    files = obj(manifest.get("files"), "files")
    expected = {
        "verification": MAX_EVIDENCE_BYTES,
        "profile": MAX_PROFILE_BYTES,
        "staticSize": MAX_STATIC_REPORT_BYTES,
    }
    require(set(files) == set(expected), "manifest files set mismatch")
    loaded: dict[str, dict[str, Any]] = {}
    for key, maximum in expected.items():
        item = obj(files.get(key), f"files.{key}")
        exact_keys(item, {"path", "sizeBytes", "sha256"}, f"files.{key}")
        name = item.get("path")
        require(isinstance(name, str) and name == Path(name).name,
                f"files.{key}.path must be a sibling filename")
        document, raw = read_json(path.parent / name, maximum)
        require(item.get("sizeBytes") == len(raw), f"files.{key} size mismatch")
        require(item.get("sha256") == hashlib.sha256(raw).hexdigest(),
                f"files.{key} SHA-256 mismatch")
        loaded[key] = document

    scope = obj(manifest.get("scope"), "scope")
    require(scope == {
        "referenceBoardLive": "measured",
        "roleAuthorization": "measured",
        "sessionAndSecureChannelCapacity": "measured",
        "collectionSnapshotP99": "not-run",
        "opcUaTaskStackDelta": "not-run",
        "uaExpertInspection": "not-run",
    }, "manifest scope must disclose measured and outstanding gates")
    validate_report(loaded["verification"])
    validate_profile(loaded["profile"])
    validate_static_report(loaded["staticSize"], artifact)
    reject_credentials(manifest)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    try:
        validate_manifest(args.manifest)
    except (EvidenceError, OSError) as exc:
        print(f"v0.5.2 OPC UA resource evidence: FAIL: {exc}", file=sys.stderr)
        return 1
    print("v0.5.2 OPC UA resource evidence: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
