#!/usr/bin/env python3
"""Generate T4 profile settings and verify an actual ExportConfig document.

This tool reports configuration agreement only, never hardware performance.
"""
from __future__ import annotations

import argparse
import itertools
import json
from pathlib import Path
import sys

from validate_benchmark_contract import REQUIRED_AXES, validate_contract

CONTRACT = Path(__file__).resolve().parents[1] / "benchmarks/v0.5.2/measurement-contract.json"
MAX_BYTES = 1024 * 1024


def read_json(path: Path) -> dict:
    with path.open("rb") as stream:
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("input exceeds 1 MiB limit")
    result = json.loads(raw)
    if not isinstance(result, dict):
        raise ValueError("expected a JSON object")
    return result


def profiles() -> list[dict]:
    contract = read_json(CONTRACT)
    validate_contract(contract)
    matrix = contract["benchmarkMatrix"]
    result = []
    for values in itertools.product(*(matrix["axes"][key] for key in REQUIRED_AXES)):
        cell = dict(zip(REQUIRED_AXES, values))
        fields = dict(cell, events=int(cell["events"]), history=int(cell["history"]))
        result.append({"profileSchemaVersion": 1,
                       "profileId": matrix["profileIdTemplate"].format(**fields),
                       **cell})
    return result


def select(profile_id: str) -> dict:
    for profile in profiles():
        if profile["profileId"] == profile_id:
            return profile
    raise ValueError("unknown or unsupported profile ID")


def cmake_settings(profile: dict) -> dict:
    return {
        "FIRMWARE_BENCHMARK_PROFILE": "synthetic",
        "FIRMWARE_BENCHMARK_CHANNELS": str(profile["channels"]),
        "FIRMWARE_BENCHMARK_RATE_HZ": str(profile["rateHz"]),
        "FIRMWARE_BENCHMARK_CLIENTS": str(profile["clients"]),
        "FIRMWARE_BENCHMARK_SECURITY": profile["security"],
        "FIRMWARE_BENCHMARK_EVENTS": "ON" if profile["events"] else "OFF",
        "FIRMWARE_BENCHMARK_HISTORY": "ON" if profile["history"] else "OFF",
        "FIRMWARE_DRIVER_PACK": "base",
    }


def verify_export(profile: dict, document: dict) -> None:
    def require(value: bool, message: str) -> None:
        if not value:
            raise ValueError(message)

    require(document["device"]["fwRevision"] == "0.5.2", "firmware revision mismatch")
    require(document["mqtt"]["enabled"] is False, "MQTT workload must be disabled")
    for key in ("enableHeapDiagnostics", "enableTaskDiagnostics"):
        require(document["diagnostics"][key] is True, f"diagnostics.{key} mismatch")
    channels = document["channels"]
    require(isinstance(channels, list) and len(channels) == 16, "expected exactly 16 channel slots")
    require(all(type(c.get("slot")) is int for c in channels), "invalid slot type")
    require({c["slot"] for c in channels} == set(range(16)), "missing/duplicate slot")
    by_slot = {c["slot"]: c for c in channels}
    period = 1000 // profile["rateHz"]
    flags = 1 | 8 | (2 if profile["history"] else 0) | (4 if profile["events"] else 0)
    for slot in range(16):
        channel = by_slot[slot]
        active = slot < profile["channels"]
        require(channel.get("inUse") is active, f"slot {slot}: inUse mismatch")
        require(channel.get("enabled") is active, f"slot {slot}: enabled mismatch")
        if not active:
            continue
        expected = {"driverId": 10, "sourceIndex": slot % 8, "sourceFlags": 0,
                    "id": f"benchmark.synthetic.{slot + 1}", "kind": 0, "valueType": 1,
                    "name": f"BenchmarkSynthetic{slot + 1}", "unit": "",
                    "flags": flags, "samplePeriodMs": period,
                    "publishPeriodMs": period, "historyIntervalMs": period,
                    "scale": 1.0, "offset": 0.0}
        for key, value in expected.items():
            require(channel.get(key) == value, f"slot {slot}: {key} mismatch")


def verify_metadata(profile: dict, document: dict) -> None:
    for key in ("profileSchemaVersion", "profileId", *REQUIRED_AXES):
        if type(document.get(key)) is not type(profile[key]) or document.get(key) != profile[key]:
            raise ValueError(f"build metadata {key} mismatch")
    if document.get("testOnly") is not True or document.get("driverPack") != "base":
        raise ValueError("build metadata must identify test-only base firmware")
    if document.get("firmwareVersion") != "0.5.2":
        raise ValueError("build metadata firmware version mismatch")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-id")
    parser.add_argument("--verify-export", type=Path)
    parser.add_argument("--verify-metadata", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        if (args.verify_export or args.verify_metadata) and not args.profile_id:
            raise ValueError("verification requires --profile-id")
        if args.profile_id:
            profile = select(args.profile_id)
            result = {**profile, "cmakeSettings": cmake_settings(profile),
                      "hardwareState": "not-run"}
            if args.verify_export:
                verify_export(profile, read_json(args.verify_export))
                result["exportConfigMatches"] = True
            if args.verify_metadata:
                verify_metadata(profile, read_json(args.verify_metadata))
                result["buildMetadataMatches"] = True
        else:
            result = {"profileSchemaVersion": 1, "profiles": profiles()}
        text = json.dumps(result, indent=2) + "\n"
        if len(text.encode()) > MAX_BYTES:
            raise ValueError("output exceeds 1 MiB limit")
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8")
        else:
            print(text, end="")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"benchmark-profiles: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
