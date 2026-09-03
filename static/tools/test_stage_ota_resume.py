#!/usr/bin/env python3
"""Host tests for safe OTA upload resume validation."""

import copy
import hashlib
import json
import sys
import tempfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "static" / "tools"))

from stage_ota import load_manifest, resolve_resume_offset  # noqa: E402


PAYLOAD = {
    "image_size": 8192,
    "image_crc32": 0x12345678,
    "version_number": 262146,
}
STATUS = {
    "sessionActive": True,
    "expectedImageSize": 8192,
    "expectedImageCrc32": "0x12345678",
    "versionNumber": 262146,
    "bytesWritten": 4096,
    "remainingBytes": 4096,
}


def expect_rejected(field, value):
    status = copy.deepcopy(STATUS)
    status[field] = value
    try:
        resolve_resume_offset(status, PAYLOAD, 4096)
    except (RuntimeError, ValueError):
        return
    raise AssertionError(f"unsafe resume status was accepted: {field}={value!r}")


def write_local_build_tree(build_dir, *, ota_build_bootloader="ON", layout_overrides=None, manifest_overrides=None):
    ota_dir = build_dir / "ota_artifacts"
    generated_dir = build_dir / "generated" / "ota_layout"
    ota_dir.mkdir(parents=True, exist_ok=True)
    generated_dir.mkdir(parents=True, exist_ok=True)

    payload = ota_dir / "ota_payload.bin"
    payload_bytes = b"bounded OTA payload"
    payload.write_bytes(payload_bytes)
    payload_crc32 = zlib.crc32(payload_bytes) & 0xFFFFFFFF
    payload_sha256 = hashlib.sha256(payload_bytes).hexdigest()

    cache_text = "\n".join(
        [
            f"OTA_BUILD_BOOTLOADER:BOOL={ota_build_bootloader}",
            "PICO_BOARD:STRING=waveshare_core2350b_16mb_psram8mb",
            "PICO_PLATFORM:STRING=rp2350-arm-s",
            "RP2_FIRMWARE_VERSION:INTERNAL=0.5.0",
            "RP2_FIRMWARE_VERSION_NUMBER:INTERNAL=327680",
            "",
        ]
    )
    (build_dir / "CMakeCache.txt").write_text(cache_text, encoding="utf-8")

    layout = {
        "board": "waveshare_core2350b_16mb_psram8mb",
        "platform": "rp2350-arm-s",
        "firmware": {
            "version": "0.5.0",
            "version_number": 327680,
        },
        "regions": {
            "bank_a": {"origin": "0x10010000"},
            "bank_b": {"offset_bytes": 0x7B4000},
        },
    }
    if layout_overrides:
        for key, value in layout_overrides.items():
            if key == "firmware":
                layout["firmware"].update(value)
            elif key == "regions":
                for region_name, region_value in value.items():
                    layout["regions"][region_name].update(region_value)
            else:
                layout[key] = value
    (generated_dir / "ota_layout.json").write_text(
        json.dumps(layout),
        encoding="utf-8",
    )

    manifest = {
        "format_version": 1,
        "board": layout["board"],
        "platform": layout["platform"],
        "payload_file": payload.name,
        "payload_size_bytes": len(payload_bytes),
        "payload_sha256": payload_sha256,
        "payload_crc32": f"0x{payload_crc32:08x}",
        "execution_origin": layout["regions"]["bank_a"]["origin"],
        "staging_offset": f"0x{layout['regions']['bank_b']['offset_bytes']:x}",
        "firmware_version": layout["firmware"]["version"],
        "version_number": layout["firmware"]["version_number"],
        "git_hash": "deadbeefcafe",
    }
    if manifest_overrides:
        manifest.update(manifest_overrides)
    manifest_path = ota_dir / "ota_payload.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path, payload


def main():
    assert resolve_resume_offset(STATUS, PAYLOAD, 4096) == 4096
    inactive = copy.deepcopy(STATUS)
    inactive["sessionActive"] = False
    assert resolve_resume_offset(inactive, PAYLOAD, 4096) is None

    expect_rejected("expectedImageSize", 4097)
    expect_rejected("expectedImageCrc32", "0x87654321")
    expect_rejected("versionNumber", 262147)
    expect_rejected("bytesWritten", 1)
    expect_rejected("bytesWritten", 12288)
    expect_rejected("remainingBytes", 0)

    complete = copy.deepcopy(STATUS)
    complete["bytesWritten"] = 8192
    complete["remainingBytes"] = 0
    assert resolve_resume_offset(complete, PAYLOAD, 4096) == 8192

    stage_source = (ROOT / "static" / "tools" / "stage_ota.py").read_text(encoding="utf-8")
    preserve_branch = "if args.resume_active:" in stage_source
    preserve_message = "AbortFirmwareUpdate was not requested after resume failure" in stage_source
    assert preserve_branch and preserve_message

    with tempfile.TemporaryDirectory() as temporary_directory:
        directory = Path(temporary_directory)
        payload = directory / "payload.bin"
        manifest = directory / "payload.json"
        payload_bytes = b"bounded OTA payload"
        payload.write_bytes(payload_bytes)
        manifest.write_text(
            json.dumps(
                {
                    "payload_file": payload.name,
                    "payload_size_bytes": len(payload_bytes),
                    "payload_crc32": f"0x{zlib.crc32(payload_bytes) & 0xFFFFFFFF:08x}",
                    "payload_sha256": hashlib.sha256(payload_bytes).hexdigest(),
                    "version_number": 262146,
                }
            ),
            encoding="utf-8",
        )
        assert load_manifest(manifest)["image_size"] == len(payload_bytes)

        corrupted = bytearray(payload_bytes)
        corrupted[-1] ^= 0x01
        payload.write_bytes(corrupted)
        try:
            load_manifest(manifest)
        except ValueError as exc:
            assert "CRC32 mismatch" in str(exc)
        else:
            raise AssertionError("same-size corrupted resume payload was accepted")

    with tempfile.TemporaryDirectory() as temporary_directory:
        build_dir = Path(temporary_directory) / "build"
        manifest, _payload = write_local_build_tree(build_dir)
        loaded = load_manifest(manifest)
        assert loaded["firmware_version"] == "0.5.0"
        assert loaded["version_number"] == 327680

    with tempfile.TemporaryDirectory() as temporary_directory:
        archive_dir = Path(temporary_directory) / "release_bundle"
        manifest, _payload = write_local_build_tree(archive_dir)
        (archive_dir / "CMakeCache.txt").unlink()
        (archive_dir / "generated" / "ota_layout" / "ota_layout.json").unlink()
        loaded = load_manifest(manifest)
        assert loaded["firmware_version"] == "0.5.0"
        assert loaded["version_number"] == 327680

    with tempfile.TemporaryDirectory() as temporary_directory:
        build_dir = Path(temporary_directory) / "build"
        manifest, _payload = write_local_build_tree(build_dir, ota_build_bootloader="OFF")
        try:
            load_manifest(manifest)
        except ValueError as exc:
            assert "OTA_BUILD_BOOTLOADER=OFF" in str(exc)
        else:
            raise AssertionError("stale local build OTA manifest was accepted with OTA_BUILD_BOOTLOADER=OFF")

    with tempfile.TemporaryDirectory() as temporary_directory:
        build_dir = Path(temporary_directory) / "build"
        manifest, _payload = write_local_build_tree(
            build_dir,
            manifest_overrides={"firmware_version": "0.4.3"},
        )
        try:
            load_manifest(manifest)
        except ValueError as exc:
            assert "firmware_version mismatch" in str(exc)
        else:
            raise AssertionError("local build OTA manifest with mismatched firmware version was accepted")

    with tempfile.TemporaryDirectory() as temporary_directory:
        build_dir = Path(temporary_directory) / "build"
        manifest, _payload = write_local_build_tree(
            build_dir,
            manifest_overrides={"execution_origin": "0x10020000"},
        )
        try:
            load_manifest(manifest)
        except ValueError as exc:
            assert "execution_origin mismatch" in str(exc)
        else:
            raise AssertionError("local build OTA manifest with mismatched layout origin was accepted")

    with tempfile.TemporaryDirectory() as temporary_directory:
        build_dir = Path(temporary_directory) / "build"
        manifest, _payload = write_local_build_tree(
            build_dir,
            layout_overrides={"firmware": {"version": "0.4.3", "version_number": 262147}},
            manifest_overrides={"firmware_version": "0.4.3", "version_number": 262147},
        )
        try:
            load_manifest(manifest)
        except ValueError as exc:
            assert "layout firmware_version mismatch" in str(exc)
        else:
            raise AssertionError("local OTA manifest matching a stale layout but not CMakeCache was accepted")

    print("OTA resume validation host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
