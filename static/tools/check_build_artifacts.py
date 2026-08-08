#!/usr/bin/env python3
"""Verify that the required release-build artifacts exist."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys


REQUIRED_RELATIVE_PATHS = (
    "app/basic_profile/device_profile_basic.elf",
    "app/basic_profile/device_profile_basic.uf2",
    "app/basic_profile/device_profile_basic.bin",
    "config_export.json",
    "generated/ota_layout/ota_layout.json",
    "reports/size.txt",
    "reports/sections.txt",
    "reports/version_integrity.json",
)


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

    print("artifact-check: PASS")
    for relative in REQUIRED_RELATIVE_PATHS:
        print(f"found: {relative}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
