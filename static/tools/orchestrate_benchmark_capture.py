#!/usr/bin/env python3
"""Run one bounded v0.5.2 client/native capture and write provenance evidence.

The runner deliberately never echoes the client command or its stdout: either
may contain an endpoint or credentials.  It only records hashes and bounded
stage counters in the output envelope.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import secrets
import subprocess
import sys
import threading
import time
from typing import Any, Callable

import benchmark_profiles
import load_test_opcua

MAX_JSON_BYTES = 8 * 1024 * 1024
MAX_NATIVE_BYTES = 1024 * 1024
MAX_CLIENT_STDOUT_BYTES = 64 * 1024
MAX_ENVELOPE_BYTES = 64 * 1024
MAX_READY_SECONDS = 120.0
MAX_RUN_SECONDS = 900.0
MAX_DUMP_SECONDS = 60.0
FIRMWARE_WARMUP_SECONDS = 10.0
FIRMWARE_CAPTURE_SECONDS = 30.0
NATIVE_SERIAL_DRAIN_SECONDS = 0.1
GIT_SHA = re.compile(r"[0-9a-f]{40}")
NATIVE_OBJECT_START = re.compile(r'\{\s*"schemaVersion"\s*:')


class CaptureError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CaptureError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path, maximum: int = MAX_JSON_BYTES) -> tuple[dict[str, Any], bytes]:
    with path.open("rb") as stream:
        raw = stream.read(maximum + 1)
    require(bool(raw) and len(raw) <= maximum, f"{path.name}: JSON input is empty or exceeds its byte bound")
    value = json.loads(raw.decode("utf-8"))
    require(isinstance(value, dict), f"{path.name}: expected a JSON object")
    return value, raw


def write_json(path: Path, value: dict[str, Any], maximum: int) -> None:
    raw = json.dumps(value, indent=2, sort_keys=True, allow_nan=False).encode("utf-8") + b"\n"
    require(len(raw) <= maximum, "output exceeds its byte bound")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(raw)


def static_provenance(path: Path, profile_id: str, git_commit: str) -> tuple[dict[str, Any], str]:
    report, _raw = read_json(path)
    require(report.get("schemaVersion") == 1 and report.get("measurementKind") == "static-build-layout",
            "static-size report is not a v0.5.2 static build layout")
    provenance = report.get("provenance")
    require(isinstance(provenance, dict), "static-size provenance is missing")
    require(provenance.get("benchmarkProfileId") == profile_id and provenance.get("firmwareVersion") == "0.5.2",
            "static-size profile or firmware version does not match the selected benchmark")
    require(provenance.get("gitCommit") == git_commit and provenance.get("gitDirty") is False,
            "static-size provenance is not the requested clean commit")
    for key in ("boardRevision", "toolchain", "buildType"):
        require(isinstance(provenance.get(key), str) and provenance[key],
                f"static-size provenance {key} is missing")
    uf2 = report.get("artifacts", {}).get("uf2", {})
    flashed = uf2.get("sha256") if isinstance(uf2, dict) else None
    require(isinstance(flashed, str) and re.fullmatch(r"[0-9a-f]{64}", flashed) is not None,
            "static-size UF2 SHA-256 is invalid")
    return provenance, flashed


def git_identity(root: Path) -> tuple[str, bool]:
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                          capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=root, check=True,
                                capture_output=True, text=True).stdout.strip())
    return head, dirty


def client_command(args: argparse.Namespace, profile: dict[str, Any], provenance: dict[str, Any]) -> list[str]:
    forbidden = {"--benchmark", "--benchmark-profile-id", "--output", "--git-commit",
                 "--git-dirty", "--board-revision", "--toolchain", "--build-type",
                 "--runner-readiness-token", "--warmup", "--duration"}
    require(all(item.split("=", 1)[0] not in forbidden for item in args.client_arg),
            "client arguments must not override runner-controlled provenance")
    command = [args.python, str(args.client_script), *args.client_arg,
               "--benchmark", "--benchmark-profile-id", profile["profileId"],
               "--output", str(args.client_output), "--git-commit", args.git_commit,
               "--board-revision", provenance["boardRevision"],
               "--toolchain", provenance["toolchain"], "--build-type", provenance["buildType"],
               "--runner-readiness-token", args.ready_token,
               "--warmup", str(args.warmup_seconds), "--duration", str(args.duration_seconds)]
    if profile["security"] == "test-none":
        command.append("--test-none")
    return command


class ReadinessProbe:
    def __init__(self, stream: Any, token: bytes, required: int) -> None:
        self.stream, self.token, self.required = stream, token, required
        self.count = self.bytes_seen = 0
        self.failed = False
        self.ready = threading.Event()
        self._tail = b""
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        while True:
            chunk = self.stream.read(4096)
            if not chunk:
                return
            self.bytes_seen += len(chunk)
            if self.bytes_seen > MAX_CLIENT_STDOUT_BYTES:
                self.failed = True
                self.ready.set()
                return
            for byte in chunk:
                self._tail = (self._tail + bytes((byte,)))[-len(self.token):]
                if self._tail == self.token:
                    self.count += 1
                    if self.count >= self.required:
                        self.ready.set()


def stop_process(process: Any) -> None:
    if process.poll() is not None:
        return
    try:
        process.terminate()
        process.wait(timeout=2)
    except BaseException:
        try:
            process.kill()
            process.wait(timeout=2)
        except BaseException:
            pass


def wait_for_readiness(process: Any, probe: ReadinessProbe, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while not probe.ready.is_set():
        if process.poll() is not None:
            raise CaptureError("client exited before declared readiness")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            stop_process(process)
            raise CaptureError("declared client readiness timed out")
        probe.ready.wait(min(0.05, remaining))
    if probe.failed:
        stop_process(process)
        raise CaptureError("client stdout exceeds readiness byte bound")
    if probe.count < probe.required:
        stop_process(process)
        raise CaptureError("client exited before declared readiness")


def native_objects(raw: bytes) -> list[dict[str, Any]]:
    """Extract top-level native evidence objects from bounded UART noise."""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise CaptureError("native serial dump is not UTF-8") from exc
    decoder, documents = json.JSONDecoder(), []
    for match in NATIVE_OBJECT_START.finditer(text):
        try:
            candidate, _end = decoder.raw_decode(text, match.start())
        except json.JSONDecodeError:
            continue  # The UART may still be delivering this one object.
        if (isinstance(candidate, dict) and candidate.get("schemaVersion") == 1
                and candidate.get("kind") == "v052-native-sampling-evidence"):
            documents.append(candidate)
    return documents


def parse_native(raw: bytes, profile: dict[str, Any], warmup_s: float,
                 duration_s: float) -> dict[str, Any]:
    require(bool(raw) and len(raw) <= MAX_NATIVE_BYTES, "native serial dump exceeds its byte bound")
    documents = native_objects(raw)
    require(len(documents) == 1,
            "native serial dump must contain exactly one native JSON object")
    document = documents[0]
    expected_channels = {f"benchmark.synthetic.{slot}" for slot in range(1, profile["channels"] + 1)}
    try:
        load_test_opcua.native_jitter_metrics(document, 1000 / profile["rateHz"], 8192,
                                              profile["profileId"], warmup_s, duration_s,
                                              expected_channels)
    except (TypeError, ValueError, KeyError) as exc:
        raise CaptureError("native serial dump does not match the selected profile") from exc
    return document


def read_native_dump(console: Any, timeout_s: float, profile: dict[str, Any],
                     warmup_s: float, duration_s: float) -> dict[str, Any]:
    raw = bytearray()
    deadline = time.monotonic() + timeout_s
    document: dict[str, Any] | None = None
    last_serial_data = time.monotonic()
    while time.monotonic() < deadline:
        chunk = console.read(min(4096, MAX_NATIVE_BYTES + 1 - len(raw)))
        if chunk:
            raw.extend(chunk)
            last_serial_data = time.monotonic()
            if len(raw) > MAX_NATIVE_BYTES:
                break
            try:
                document = parse_native(bytes(raw), profile, warmup_s, duration_s)
            except CaptureError as exc:
                if "multiple native JSON" in str(exc) or "does not match" in str(exc):
                    raise
                # A partial JSON fragment is expected while the UART is still
                # delivering the one bounded dump; keep reading until the
                # bounded drain interval proves the stream is quiet.
                continue
        else:
            if document is not None and time.monotonic() - last_serial_data >= NATIVE_SERIAL_DRAIN_SECONDS:
                return document
            time.sleep(0.01)
    raise CaptureError("native serial dump was missing, malformed, or exceeded its bound")


def recover_native_capture(console: Any, started_at: float, warmup_s: float,
                           duration_s: float, dump_timeout_s: float) -> None:
    """Best-effort return to READY after a client or UART failure.

    Firmware accepts ``dump`` only after the armed warmup/capture interval.  A
    failed host run therefore waits no longer than that already-declared
    interval, sends one bounded dump, and discards its serial bytes.  Recovery
    errors deliberately never hide the primary run failure.
    """
    capture_ends = started_at + warmup_s + duration_s
    while True:
        remaining = capture_ends - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.05, remaining))
    try:
        console.write(b"benchmark dump\n")
        console.flush()
        deadline, total, quiet_since = time.monotonic() + dump_timeout_s, 0, None
        while time.monotonic() < deadline and total <= MAX_NATIVE_BYTES:
            chunk = console.read(min(4096, MAX_NATIVE_BYTES + 1 - total))
            if chunk:
                total += len(chunk)
                quiet_since = None
            else:
                quiet_since = time.monotonic() if quiet_since is None else quiet_since
                if time.monotonic() - quiet_since >= NATIVE_SERIAL_DRAIN_SECONDS:
                    break
                time.sleep(0.01)
    except Exception:
        pass


def validate_client(client: dict[str, Any], profile: dict[str, Any], provenance: dict[str, Any],
                    git_commit: str, warmup_s: float, duration_s: float) -> None:
    require(client.get("schemaVersion") == 1 and client.get("profileId") == profile["profileId"]
            and client.get("firmwareVersion") == "0.5.2", "client evidence profile or firmware mismatch")
    require(client.get("gitCommit") == git_commit and client.get("gitDirty") is False,
            "client evidence is not the requested clean commit")
    for key in ("boardRevision", "toolchain", "buildType"):
        require(client.get(key) == provenance[key], f"client evidence {key} differs from static-size provenance")
    require(client.get("warmupSeconds") == warmup_s and client.get("durationSeconds") == duration_s,
            "client evidence capture window differs from the declared window")
    require(client.get("repetitions") == 1 and client.get("channelCount") == profile["channels"]
            and client.get("clientCount") == profile["clients"],
            "client evidence profile cardinality mismatch")


def run_capture(args: argparse.Namespace, *, process_factory: Callable[..., Any] = subprocess.Popen,
                serial_factory: Callable[..., Any] | None = None,
                identity: Callable[[Path], tuple[str, bool]] = git_identity,
                recovery: Callable[[Any, float, float, float, float], None] = recover_native_capture) -> dict[str, Any]:
    require(GIT_SHA.fullmatch(args.git_commit) is not None, "git commit must be a full lowercase SHA")
    if args.ready_token is None:
        args.ready_token = f"RP2_BENCHMARK_READY_{secrets.token_hex(16)}\n"
    require(args.ready_token.isascii() and 1 <= len(args.ready_token) <= 64,
            "readiness token must be 1..64 ASCII bytes")
    require(len(args.client_arg) <= 64 and all(len(item.encode("utf-8")) <= 1024 for item in args.client_arg),
            "client argument count or byte length exceeds its bound")
    for name, value, maximum in (("readiness", args.readiness_timeout, MAX_READY_SECONDS),
                                 ("client", args.client_timeout, MAX_RUN_SECONDS),
                                 ("dump", args.dump_timeout, MAX_DUMP_SECONDS)):
        require(math.isfinite(value) and 0 < value <= maximum, f"{name} timeout is outside its bound")
    require(args.warmup_seconds == FIRMWARE_WARMUP_SECONDS
            and args.duration_seconds == FIRMWARE_CAPTURE_SECONDS,
            "capture window differs from the v0.5.2 firmware timing contract")
    root = args.root.resolve()
    profile = benchmark_profiles.select(args.profile_id)
    commit, dirty = identity(root)
    require(commit == args.git_commit and dirty is False, "working tree is not the requested clean commit")
    provenance, uf2_sha256 = static_provenance(args.static_size_report, args.profile_id, args.git_commit)
    require(args.flashed_uf2_sha256 == uf2_sha256,
            "declared flashed UF2 SHA-256 differs from static-size report")
    require(serial_factory is not None, "serial transport is unavailable")

    command = client_command(args, profile, provenance)
    process = process_factory(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL, text=False, bufsize=0)
    require(getattr(process, "stdout", None) is not None, "client stdout pipe is unavailable")
    probe = ReadinessProbe(process.stdout, args.ready_token.encode("ascii"), profile["clients"])
    probe.start()
    native: dict[str, Any]
    capture_started = capture_completed = False
    try:
        wait_for_readiness(process, probe, args.readiness_timeout)
        with serial_factory(args.serial_port, baudrate=115200, timeout=0.1, write_timeout=2) as console:
            console.reset_input_buffer()
            console.write(b"benchmark start\n")
            console.flush()
            capture_started_at, capture_started = time.monotonic(), True
            try:
                client_deadline = time.monotonic() + args.client_timeout
                while process.poll() is None and time.monotonic() < client_deadline:
                    # UART logging may continue during capture. It remains
                    # bounded by read_native_dump and is never retained in the
                    # normalized native evidence.
                    time.sleep(0.01)
                if process.poll() is None:
                    stop_process(process)
                    raise CaptureError("client workload timed out")
                if process.returncode != 0:
                    raise CaptureError("client workload failed")
                probe.thread.join(timeout=1)
                if probe.thread.is_alive() or probe.failed:
                    raise CaptureError("client stdout exceeds readiness byte bound")
                console.write(b"benchmark dump\n")
                console.flush()
                native = read_native_dump(console, args.dump_timeout, profile,
                                          args.warmup_seconds, args.duration_seconds)
                capture_completed = True
            finally:
                if capture_started and not capture_completed:
                    recovery(console, capture_started_at, args.warmup_seconds,
                             args.duration_seconds, args.dump_timeout)
    finally:
        if process.poll() is None:
            stop_process(process)
        probe.thread.join(timeout=1)

    client, client_raw = read_json(args.client_output)
    validate_client(client, profile, provenance, args.git_commit,
                    args.warmup_seconds, args.duration_seconds)
    native_raw = json.dumps(native, indent=2, sort_keys=True, allow_nan=False).encode("utf-8") + b"\n"
    require(len(native_raw) <= MAX_NATIVE_BYTES, "normalized native evidence exceeds its byte bound")
    args.native_output.parent.mkdir(parents=True, exist_ok=True)
    args.native_output.write_bytes(native_raw)
    native_hash = hashlib.sha256(native_raw).hexdigest()
    client_hash = hashlib.sha256(client_raw).hexdigest()
    static_hash = sha256_file(args.static_size_report)
    association_id = hashlib.sha256(
        (args.profile_id + args.git_commit + uf2_sha256 + client_hash + native_hash).encode("ascii")
    ).hexdigest()
    envelope = {
        "schemaVersion": 1,
        "kind": "v052-host-orchestrated-benchmark-capture",
        "state": "complete",
        "associationId": association_id,
        "profile": {"profileId": args.profile_id, "profileSchemaVersion": profile["profileSchemaVersion"],
                    "channels": profile["channels"], "clients": profile["clients"], "rateHz": profile["rateHz"]},
        "capture": {"warmupSeconds": args.warmup_seconds, "durationSeconds": args.duration_seconds,
                    "readiness": {"tokenCount": probe.count, "requiredTokenCount": profile["clients"],
                                  "stdoutBytesObserved": probe.bytes_seen},
                    "nativeCaptureId": native["captureId"]},
        "provenance": {"gitCommit": args.git_commit, "gitDirty": False,
                       "boardRevision": provenance["boardRevision"], "toolchain": provenance["toolchain"],
                       "buildType": provenance["buildType"], "flashedUf2Sha256": uf2_sha256},
        "evidence": {"client": {"sha256": client_hash, "bytes": len(client_raw)},
                     "native": {"sha256": native_hash, "bytes": len(native_raw)},
                     "staticSize": {"sha256": static_hash}},
    }
    write_json(args.output, envelope, MAX_ENVELOPE_BYTES)
    return envelope


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-id", required=True)
    parser.add_argument("--client-arg", action="append", default=[],
                        help="one non-secret load_test_opcua argument; repeat as needed")
    parser.add_argument("--client-script", type=Path,
                        default=Path(__file__).with_name("load_test_opcua.py"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--client-output", type=Path, required=True)
    parser.add_argument("--serial-port", required=True)
    parser.add_argument("--ready-token",
                        help="optional non-secret readiness token; a random token is generated by default")
    parser.add_argument("--static-size-report", type=Path, required=True)
    parser.add_argument("--flashed-uf2-sha256", required=True)
    parser.add_argument("--git-commit", required=True)
    parser.add_argument("--warmup-seconds", type=float, default=10.0)
    parser.add_argument("--duration-seconds", type=float, default=30.0)
    parser.add_argument("--readiness-timeout", type=float, default=30.0)
    parser.add_argument("--client-timeout", type=float, default=180.0)
    parser.add_argument("--dump-timeout", type=float, default=30.0)
    parser.add_argument("--native-output", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args()
    try:
        import serial
        envelope = run_capture(args, serial_factory=serial.Serial)
        print(f"benchmark-capture: PASS profile={envelope['profile']['profileId']} association={envelope['associationId']}")
        return 0
    except (OSError, json.JSONDecodeError, ValueError, TypeError, subprocess.SubprocessError) as exc:
        # Never include command arguments, client stdout, or exception text: they
        # may contain endpoint credentials or server-provided strings.
        print(f"benchmark-capture: FAIL: {type(exc).__name__}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
