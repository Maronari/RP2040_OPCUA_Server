#!/usr/bin/env python3
"""Render the validated v0.5.2 baseline as a compact Markdown report."""
from __future__ import annotations

import argparse
from pathlib import Path

from validate_benchmark_baseline import read_json, validate_baseline, validate_thresholds


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--thresholds", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    baseline, thresholds = read_json(args.baseline), read_json(args.thresholds)
    measured = validate_baseline(baseline, args.root.resolve())
    validate_thresholds(thresholds, measured)
    rows = [entry for entry in baseline["profiles"] if entry["resultState"] in {"measured", "failed"}]
    measured_rows = [entry for entry in rows if entry["resultState"] == "measured"]
    failed_rows = [entry for entry in rows if entry["resultState"] == "failed"]
    not_run = sum(entry["resultState"] == "not-run" for entry in baseline["profiles"])
    env = baseline["environment"]
    lines = [
        "# v0.5.2 hardware benchmark baseline", "",
        "Default environment (passing secure profile):  ",
        f"Recorded: `{env['recordedAt']}`  ",
        f"Board: `{env['board']}`  ",
        f"Network: `{env['network']}`  ",
        f"Toolchain: `{env['toolchain']}`  ",
        f"Firmware source: `{env['firmwareSource']}` (`gitDirty=true`)  ",
        f"UF2 SHA-256: `{env['firmwareUf2Sha256']}`", "",
        f"Passing measured profiles: **{len(measured_rows)}**. Failed profiles: **{len(failed_rows)}**. "
        f"Explicitly not run: **{not_run}**.", "",
        "| State | Profile | Read p99 | Cold session max | Warm session max | Sampling jitter p99 | Heap peak | PSRAM peak |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for entry in rows:
        m = entry["metrics"]
        lines.append(
            f"| `{entry['resultState']}` | `{entry['profileId']}` | {m['ReadLatencyMs.p99']['value']:.3f} ms | "
            f"{m['SecureSessionEstablishmentLatencyMs.Cold.max']['value']:.3f} ms | "
            f"{m['SecureSessionEstablishmentLatencyMs.Warm.max']['value']:.3f} ms | "
            f"{m['SamplingJitterMs.p99']['value']:.3f} ms | "
            f"{m['Heap.PeakUsedBytes']['value']:,} B | {m['PSRAM.PeakUsedBytes']['value']:,} B |"
        )
    if failed_rows:
        lines += ["", "Failed-profile latency values describe the successful subset before the declared failure; they are not regression baselines."]
        for entry in failed_rows:
            lines.append(f"- `{entry['profileId']}`: {entry['failureReason']}")
    lines += ["", "## Executed-profile provenance", ""]
    for entry in rows:
        profile_env = entry.get("environment", env)
        lines += [
            f"### `{entry['profileId']}`", "",
            f"- State: `{entry['resultState']}`",
            f"- Recorded: `{profile_env['recordedAt']}`",
            f"- Firmware source: `{profile_env['firmwareSource']}` (`gitDirty=true`)",
            f"- UF2 SHA-256: `{profile_env['firmwareUf2Sha256']}`",
            f"- Board: `{profile_env['board']}`",
            f"- Toolchain/build: `{profile_env['toolchain']}` / `{profile_env['buildType']}`",
            "",
        ]
    lines += ["", "The remaining matrix cells carry an explicit `not-run` reason in the JSON baseline.",
              "This representative run does not claim full 192-cell hardware coverage, T2 exhaustion evidence, or UaExpert verification.", ""]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
