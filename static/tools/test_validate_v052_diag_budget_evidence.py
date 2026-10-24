#!/usr/bin/env python3
"""Focused fail-closed tests for diagnostic snapshot budget evidence."""

from __future__ import annotations

import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "static/tools/validate_v052_diag_budget_evidence.py"
EVIDENCE = ROOT / "static/benchmarks/v0.5.2/evidence"
MANIFEST = EVIDENCE / "secure-c1-diag-budget-manifest.json"


def run(manifest: pathlib.Path, expected: int) -> None:
    result = subprocess.run([sys.executable, str(TOOL), str(manifest)], capture_output=True,
                            text=True, check=False)
    assert result.returncode == expected, result.stdout + result.stderr


def rewrite_descriptor(manifest: dict, root: pathlib.Path, key: str) -> None:
    path = root / manifest["files"][key]["path"]
    manifest["files"][key]["sizeBytes"] = path.stat().st_size
    manifest["files"][key]["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: dict) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def mutate_bundle(callback) -> None:
    with tempfile.TemporaryDirectory(prefix="v052-diag-budget-") as directory:
        root = pathlib.Path(directory)
        for path in EVIDENCE.iterdir():
            if path.is_file():
                shutil.copy2(path, root / path.name)
        manifest_path = root / MANIFEST.name
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        callback(root, manifest)
        write_json(manifest_path, manifest)
        run(manifest_path, 1)


def main() -> int:
    run(MANIFEST, 0)

    def breach_p99(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["measurement"]["path"]
        value = json.loads(path.read_text(encoding="utf-8"))
        value["diagServiceSampleDurationUs"]["p99"] = 2001
        value["diagServiceSampleDurationUs"]["max"] = 2001
        write_json(path, value)
        rewrite_descriptor(manifest, root, "measurement")

    mutate_bundle(breach_p99)

    def add_unknown(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["measurement"]["path"]
        value = json.loads(path.read_text(encoding="utf-8"))
        value["unexpected"] = True
        write_json(path, value)
        rewrite_descriptor(manifest, root, "measurement")

    mutate_bundle(add_unknown)

    def forge_profile(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["profile"]["path"]
        value = json.loads(path.read_text(encoding="utf-8"))
        value["channels"] = 16
        write_json(path, value)
        rewrite_descriptor(manifest, root, "profile")

    mutate_bundle(forge_profile)

    def credential_in_profile(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["profile"]["path"]
        value = json.loads(path.read_text(encoding="utf-8"))
        value["password"] = "forbidden"
        write_json(path, value)
        rewrite_descriptor(manifest, root, "profile")

    mutate_bundle(credential_in_profile)

    def exceed_sram(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["staticSize"]["path"]
        value = json.loads(path.read_text(encoding="utf-8"))
        value["totals"]["SramStaticTotalBytes"] += 4097
        write_json(path, value)
        rewrite_descriptor(manifest, root, "staticSize")

    mutate_bundle(exceed_sram)

    def token_in_static_report(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["staticSize"]["path"]
        value = json.loads(path.read_text(encoding="utf-8"))
        value["provenance"]["buildFlags"]["accessToken"] = "forbidden"
        write_json(path, value)
        rewrite_descriptor(manifest, root, "staticSize")

    mutate_bundle(token_in_static_report)

    def replace_static_baseline_with_current(root: pathlib.Path, manifest: dict) -> None:
        current_path = root / manifest["files"]["staticSize"]["path"]
        baseline_path = root / manifest["files"]["staticComparisonBaseline"]["path"]
        shutil.copy2(current_path, baseline_path)
        rewrite_descriptor(manifest, root, "staticComparisonBaseline")

    mutate_bundle(replace_static_baseline_with_current)

    def exceed_stack_reduction(root: pathlib.Path, manifest: dict) -> None:
        comparison_path = root / manifest["files"]["stackComparison"]["path"]
        comparison = json.loads(comparison_path.read_text(encoding="utf-8"))
        comparison["current"]["stackHighWaterWords"] = 7803
        comparison["current"]["stackHighWaterBytes"] = 31212
        comparison["reserveReductionBytes"] = 260
        comparison["headroomBytes"] = -4
        write_json(comparison_path, comparison)
        capture_path = root / manifest["files"]["v052StackCapture"]["path"]
        capture = capture_path.read_text(encoding="utf-8")
        capture = capture.replace('"opcStackHighWatermark": 7842',
                                  '"opcStackHighWatermark": 7803')
        capture_path.write_text(capture, encoding="utf-8")
        rewrite_descriptor(manifest, root, "v052StackCapture")
        rewrite_descriptor(manifest, root, "stackComparison")

    mutate_bundle(exceed_stack_reduction)

    def forge_stack_artifact(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["stackComparison"]["path"]
        value = json.loads(path.read_text(encoding="utf-8"))
        value["current"]["firmwareArtifact"]["sha256"] = "0" * 64
        write_json(path, value)
        rewrite_descriptor(manifest, root, "stackComparison")

    mutate_bundle(forge_stack_artifact)

    def sensitive_key(root: pathlib.Path, manifest: dict) -> None:
        manifest["privateKey"] = "forbidden"

    mutate_bundle(sensitive_key)

    def sensitive_serial(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["serialCapture"]["path"]
        path.write_text(path.read_text(encoding="utf-8") + "password=forbidden\n",
                        encoding="utf-8")
        rewrite_descriptor(manifest, root, "serialCapture")

    mutate_bundle(sensitive_serial)

    def mismatch_serial_measurement(root: pathlib.Path, manifest: dict) -> None:
        path = root / manifest["files"]["serialCapture"]["path"]
        lines = path.read_text(encoding="utf-8").splitlines()
        for index, line in enumerate(lines):
            if '"kind":"v052-diag-snapshot-budget-evidence"' not in line:
                continue
            value = json.loads(line)
            value["diagServiceSampleDurationUs"]["p99"] = 1600
            lines[index] = json.dumps(value, separators=(",", ":"))
            break
        else:
            raise AssertionError("raw serial measurement fixture is missing")
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        rewrite_descriptor(manifest, root, "serialCapture")

    mutate_bundle(mismatch_serial_measurement)

    with tempfile.TemporaryDirectory(prefix="v052-diag-budget-hash-") as directory:
        root = pathlib.Path(directory)
        for path in EVIDENCE.iterdir():
            if path.is_file():
                shutil.copy2(path, root / path.name)
        manifest_path = root / MANIFEST.name
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        measurement = root / manifest["files"]["measurement"]["path"]
        measurement.write_bytes(measurement.read_bytes() + b" ")
        run(manifest_path, 1)

    print("v0.5.2 diagnostic budget evidence validator suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
