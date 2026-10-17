#!/usr/bin/env python3
"""Focused positive and fail-closed tests for committed live resource evidence."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import shutil
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "static/tools/validate_v052_opcua_resource_evidence.py"
SPEC = importlib.util.spec_from_file_location("resource_bundle", TOOL)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
EVIDENCE = ROOT / "static/benchmarks/v0.5.2/evidence"
MANIFEST = EVIDENCE / "secure-c1-opcua-resources-manifest.json"


def rejects(action, fragment: str) -> None:
    try:
        action()
    except MODULE.EvidenceError as exc:
        assert fragment in str(exc), (fragment, str(exc))
    else:
        raise AssertionError(f"expected rejection containing {fragment!r}")


def bundle_copy(directory: pathlib.Path) -> pathlib.Path:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    shutil.copy2(MANIFEST, directory / MANIFEST.name)
    for entry in manifest["files"].values():
        shutil.copy2(EVIDENCE / entry["path"], directory / entry["path"])
    return directory / MANIFEST.name


def main() -> None:
    MODULE.validate_manifest(MANIFEST)

    with tempfile.TemporaryDirectory() as raw:
        manifest_path = bundle_copy(pathlib.Path(raw))
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["firmwareArtifact"]["sha256"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        rejects(lambda: MODULE.validate_manifest(manifest_path), "differs from manifest")

    with tempfile.TemporaryDirectory() as raw:
        manifest_path = bundle_copy(pathlib.Path(raw))
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        report_entry = manifest["files"]["verification"]
        report_path = manifest_path.parent / report_entry["path"]
        report = json.loads(report_path.read_text(encoding="utf-8"))
        report["roles"][2]["directRead"] = "not-run"
        payload = (json.dumps(report) + "\n").encode()
        report_path.write_bytes(payload)
        report_entry["sizeBytes"] = len(payload)
        report_entry["sha256"] = hashlib.sha256(payload).hexdigest()
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        rejects(lambda: MODULE.validate_manifest(manifest_path), "viewer.directRead")

    with tempfile.TemporaryDirectory() as raw:
        manifest_path = bundle_copy(pathlib.Path(raw))
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        profile_entry = manifest["files"]["profile"]
        profile_path = manifest_path.parent / profile_entry["path"]
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        profile["password"] = "must-never-be-committed"
        payload = (json.dumps(profile) + "\n").encode()
        profile_path.write_bytes(payload)
        profile_entry["sizeBytes"] = len(payload)
        profile_entry["sha256"] = hashlib.sha256(payload).hexdigest()
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        rejects(lambda: MODULE.validate_manifest(manifest_path), "keys mismatch")

    with tempfile.TemporaryDirectory() as raw:
        manifest_path = bundle_copy(pathlib.Path(raw))
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        static_entry = manifest["files"]["staticSize"]
        static_path = manifest_path.parent / static_entry["path"]
        static_report = json.loads(static_path.read_text(encoding="utf-8"))
        static_report["provenance"]["buildFlags"]["FIRMWARE_BENCHMARK_CHANNELS"] = "16"
        payload = (json.dumps(static_report) + "\n").encode()
        static_path.write_bytes(payload)
        static_entry["sizeBytes"] = len(payload)
        static_entry["sha256"] = hashlib.sha256(payload).hexdigest()
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        rejects(lambda: MODULE.validate_manifest(manifest_path),
                "FIRMWARE_BENCHMARK_CHANNELS mismatch")

    print("v0.5.2 OPC UA resource evidence validator suite: PASS")


if __name__ == "__main__":
    main()
