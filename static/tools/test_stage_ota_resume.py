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

    print("OTA resume validation host suite: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
