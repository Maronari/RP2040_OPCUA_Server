#!/usr/bin/env python3
"""Verify that the required release-build artifacts exist."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys


REQUIRED_RELATIVE_PATHS = (
    "app/basic_profile/device_profile_basic.elf",
    "app/basic_profile/device_profile_basic.uf2",
    "app/basic_profile/device_profile_basic.bin",
    "app/basic_profile/device_profile_basic.elf.map",
    "config_export.json",
    "generated/ota_layout/ota_layout.json",
    "reports/size.txt",
    "reports/sections.txt",
    "reports/static_size_report.json",
    "reports/version_integrity.json",
)

STATIC_REPORT_ARTIFACTS = {
    "elf": "app/basic_profile/device_profile_basic.elf",
    "uf2": "app/basic_profile/device_profile_basic.uf2",
    "bin": "app/basic_profile/device_profile_basic.bin",
    "map": "app/basic_profile/device_profile_basic.elf.map",
}


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_static_size_report(build_dir: pathlib.Path) -> list[str]:
    report_path = build_dir / "reports/static_size_report.json"
    try:
        document = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"reports/static_size_report.json (malformed: {exc})"]
    problems: list[str] = []
    if document.get("schemaVersion") != 1:
        problems.append("reports/static_size_report.json (schemaVersion must be 1)")
    totals = document.get("totals")
    if not isinstance(totals, dict):
        problems.append("reports/static_size_report.json (totals missing)")
    else:
        for metric in (
            "ElfFileBytes", "Uf2FileBytes", "BinFileBytes", "ExternalFlashLoadBytes",
            "SramDataBytes", "SramBssBytes", "SramStaticTotalBytes",
            "SramReservedHeapBytes", "PsramStaticBytes",
        ):
            value = totals.get(metric)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                problems.append(f"reports/static_size_report.json ({metric} invalid)")
    artifacts = document.get("artifacts")
    if not isinstance(artifacts, dict):
        problems.append("reports/static_size_report.json (artifacts missing)")
        return problems
    for key, relative in STATIC_REPORT_ARTIFACTS.items():
        entry = artifacts.get(key)
        path = build_dir / relative
        if not isinstance(entry, dict):
            problems.append(f"reports/static_size_report.json (artifact {key} missing)")
            continue
        if not path.is_file():
            continue
        actual_size = path.stat().st_size
        actual_hash = file_sha256(path)
        if entry.get("sizeBytes") != actual_size:
            problems.append(f"reports/static_size_report.json ({key} size mismatch)")
        if entry.get("sha256") != actual_hash:
            problems.append(f"reports/static_size_report.json ({key} sha256 mismatch)")
    return problems


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check that release artifacts were generated.")
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, help="Optional JSON report output path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()

    report: list[dict[str, object]] = []
    invalid: list[str] = []
    for relative in REQUIRED_RELATIVE_PATHS:
        path = build_dir / relative
        exists = path.exists()
        is_file = path.is_file()
        size_bytes = path.stat().st_size if is_file else None
        valid = is_file and size_bytes is not None and size_bytes > 0
        report.append(
            {
                "path": relative,
                "exists": exists,
                "is_file": is_file,
                "size_bytes": size_bytes,
                "valid": valid,
            }
        )
        if not valid:
            invalid.append(relative)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    if invalid:
        print("artifact-check: FAIL")
        for relative in invalid:
            path = build_dir / relative
            if not path.exists():
                reason = "missing"
            elif not path.is_file():
                reason = "not a file"
            else:
                reason = "empty"
            print(f"invalid: {relative} ({reason})")
        return 1

    report_problems = validate_static_size_report(build_dir)
    if report_problems:
        print("artifact-check: FAIL")
        for problem in report_problems:
            print(f"invalid: {problem}")
        return 1

    map_path = build_dir / "app/basic_profile/device_profile_basic.elf.map"
    map_text = map_path.read_text(encoding="utf-8", errors="replace")
    if ".bss.ram_heap" in map_text or "g_lwip_ram_heap" not in map_text:
        print("artifact-check: FAIL")
        print("invalid: lwIP heap is not linked through the bounded PSRAM arena")
        return 1

    print("artifact-check: PASS")
    for relative in REQUIRED_RELATIVE_PATHS:
        print(f"found: {relative}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
