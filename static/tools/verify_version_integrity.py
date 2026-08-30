#!/usr/bin/env python3
"""Fail-fast version integrity verification for release builds."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify that all generated version surfaces agree with the authoritative firmware version."
    )
    parser.add_argument(
        "--build-dir", required=True, type=pathlib.Path, help="CMake build directory"
    )
    parser.add_argument(
        "--source-dir",
        default=pathlib.Path(__file__).resolve().parents[2],
        type=pathlib.Path,
        help="Repository root",
    )
    parser.add_argument("--expected-tag", help="Expected Git tag name, for example v0.3.4")
    parser.add_argument("--output", type=pathlib.Path, help="Optional JSON evidence output path")
    return parser.parse_args()


def fail(message: str) -> None:
    raise SystemExit(f"version-integrity: FAIL: {message}")


def parse_authoritative_version(source_dir: pathlib.Path) -> tuple[str, int]:
    version_file = source_dir / "cmake" / "firmware_version.cmake"
    text = version_file.read_text(encoding="utf-8")
    match = re.search(r'set\(RP2_FIRMWARE_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', text)
    if not match:
        fail(f"unable to parse RP2_FIRMWARE_VERSION from {version_file}")
    version = match.group(1)
    major, minor, patch = (int(part) for part in version.split("."))
    if major > 255 or minor > 255 or patch > 65535:
        fail(f"firmware version components exceed OTA limits: {version}")
    packed = (major << 24) | (minor << 16) | patch
    return version, packed


def parse_cache(cache_path: pathlib.Path) -> dict[str, str]:
    if not cache_path.exists():
        fail(f"missing CMake cache: {cache_path}")
    values: dict[str, str] = {}
    for raw_line in cache_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("//") or line.startswith("#"):
            continue
        match = re.match(r"([^:=]+)(?::[^=]+)?=(.*)", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def parse_compile_definition(compile_commands_path: pathlib.Path) -> str:
    if not compile_commands_path.exists():
        fail(f"missing compile commands: {compile_commands_path}")
    entries = json.loads(compile_commands_path.read_text(encoding="utf-8"))
    for entry in entries:
        file_name = pathlib.Path(entry.get("file", "")).name
        if file_name != "main.c":
            continue
        command_text = entry.get("command")
        if not command_text:
            command_text = " ".join(entry.get("arguments", []))
        match = re.search(
            r"DEVICE_PROFILE_FW_REVISION=[^0-9]*([0-9]+\.[0-9]+\.[0-9]+)",
            command_text,
        )
        if match:
            return match.group(1)
    fail("unable to find DEVICE_PROFILE_FW_REVISION in compile commands")


def load_json(path: pathlib.Path) -> Any:
    if not path.exists():
        fail(f"missing JSON artifact: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_equal(label: str, actual: Any, expected: Any) -> None:
    if actual != expected:
        fail(f"{label} mismatch: expected {expected!r}, got {actual!r}")


def collect_release_artifacts(build_dir: pathlib.Path) -> list[pathlib.Path]:
    app_dir = build_dir / "app" / "basic_profile"
    artifacts = [
        app_dir / "device_profile_basic.elf",
        app_dir / "device_profile_basic.uf2",
        app_dir / "device_profile_basic.bin",
    ]
    missing = [str(path) for path in artifacts if not path.exists()]
    if missing:
        fail(f"missing release artifacts: {', '.join(missing)}")
    return artifacts


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    source_dir = args.source_dir.resolve()

    version, packed = parse_authoritative_version(source_dir)
    expected_tag = args.expected_tag
    if expected_tag:
        require_equal("Git tag", expected_tag, f"v{version}")

    cache = parse_cache(build_dir / "CMakeCache.txt")
    cache_project_version = cache.get("CMAKE_PROJECT_VERSION") or cache.get("RP2_HAT_LWIP_C_VERSION")
    if cache_project_version is None:
        fail("missing CMAKE_PROJECT_VERSION in CMakeCache.txt")
    require_equal("CMake cache RP2_FIRMWARE_VERSION", cache.get("RP2_FIRMWARE_VERSION"), version)
    require_equal("CMake cache RP2_FIRMWARE_VERSION_NUMBER", cache.get("RP2_FIRMWARE_VERSION_NUMBER"), str(packed))
    require_equal("CMake project version", cache_project_version, version)

    compile_version = parse_compile_definition(build_dir / "compile_commands.json")
    require_equal("firmware compile definition", compile_version, version)

    config_export = load_json(build_dir / "config_export.json")
    require_equal("config export fwRevision", config_export["device"]["fwRevision"], version)

    ota_layout = load_json(build_dir / "generated" / "ota_layout" / "ota_layout.json")
    require_equal("OTA/layout manifest version", ota_layout["firmware"]["version"], version)
    require_equal("OTA/layout manifest version_number", int(ota_layout["firmware"]["version_number"]), packed)

    ota_artifacts_enabled = cache.get("OTA_BUILD_BOOTLOADER", "OFF").upper() in {
        "1",
        "ON",
        "TRUE",
        "YES",
        "Y",
    }
    ota_payload_path = build_dir / "ota_artifacts" / "ota_payload.json"
    ota_payload_summary: dict[str, Any] | None = None
    if ota_artifacts_enabled:
        ota_payload = load_json(ota_payload_path)
        require_equal("OTA payload manifest firmware_version", ota_payload["firmware_version"], version)
        require_equal("OTA payload manifest version_number", int(ota_payload["version_number"]), packed)
        ota_payload_summary = {
            "path": str(ota_payload_path.relative_to(build_dir)),
            "firmware_version": ota_payload["firmware_version"],
            "version_number": int(ota_payload["version_number"]),
        }

    artifacts = collect_release_artifacts(build_dir)
    artifact_entries = [
        {
            "path": str(path.relative_to(build_dir)),
            "size_bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        for path in artifacts
    ]

    evidence = {
        "firmware_version": version,
        "firmware_version_number": packed,
        "expected_tag": expected_tag,
        "cmake_project_version": cache_project_version,
        "compile_definition_version": compile_version,
        "config_export_path": "config_export.json",
        "ota_layout_manifest_path": "generated/ota_layout/ota_layout.json",
        "ota_artifacts_enabled": ota_artifacts_enabled,
        "ota_payload_manifest": ota_payload_summary,
        "release_artifacts": artifact_entries,
    }

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")

    print(f"version-integrity: PASS ({version})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
