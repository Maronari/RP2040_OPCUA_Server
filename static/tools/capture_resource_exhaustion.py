#!/usr/bin/env python3
"""Capture and validate the one-line T2 result from the board USB console."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from validate_resource_exhaustion_evidence import (
    EvidenceError,
    MAX_EVIDENCE_BYTES,
    validate_document,
)


MARKER = b'"kind":"v052-resource-exhaustion"'
MAX_CAPTURE_BYTES = 65536


def extract_evidence(capture: bytes) -> bytes | None:
    """Return the unique bounded JSON line, or None when it has not arrived."""
    matches: list[bytes] = []
    for line in capture.splitlines():
        candidate = line.strip()
        if MARKER in candidate and candidate.startswith(b"{") and candidate.endswith(b"}"):
            if len(candidate) > MAX_EVIDENCE_BYTES:
                raise EvidenceError("board JSON exceeds the 4096-byte output bound")
            matches.append(candidate)
    if not matches:
        return None
    if len(matches) != 1:
        raise EvidenceError("capture contains more than one exhaustion result")
    return matches[0] + b"\n"


def _select_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    pico = [port.device for port in ports
            if any(token in ((port.description or "") + " " + (port.manufacturer or "")).lower()
                   for token in ("pico", "rp2350", "raspberry pi"))]
    if len(pico) == 1:
        return pico[0]
    devices = ", ".join(port.device for port in ports) or "none"
    raise EvidenceError(f"cannot select one RP2350 USB serial port (visible: {devices}); use --port")


def capture(port: str, baud: int, timeout_seconds: float) -> tuple[bytes, bytes]:
    import serial

    deadline = time.monotonic() + timeout_seconds
    raw = bytearray()
    with serial.Serial(port, baud, timeout=0.2) as stream:
        while time.monotonic() < deadline:
            chunk = stream.read(1024)
            if not chunk:
                continue
            if len(raw) + len(chunk) > MAX_CAPTURE_BYTES:
                raise EvidenceError("serial capture exceeded the 65536-byte bound")
            raw.extend(chunk)
            evidence = extract_evidence(bytes(raw))
            if evidence is not None:
                return evidence, bytes(raw)
    raise EvidenceError("timed out before a complete exhaustion result arrived")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--capture-log", type=Path)
    args = parser.parse_args()
    if args.timeout_seconds <= 0 or args.timeout_seconds > 1200:
        parser.error("--timeout-seconds must be in (0, 1200]")
    try:
        port = _select_port(args.port)
        evidence, raw = capture(port, args.baud, args.timeout_seconds)
        document = json.loads(evidence.decode("utf-8"))
        validate_document(document)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(evidence)
        if args.capture_log:
            args.capture_log.parent.mkdir(parents=True, exist_ok=True)
            args.capture_log.write_bytes(raw)
    except (EvidenceError, OSError, UnicodeDecodeError, json.JSONDecodeError,
            ImportError) as exc:
        print(f"resource exhaustion capture failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps({"status": "pass", "port": port,
                      "output": str(args.output),
                      "durationMs": document["durationMs"]}, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
