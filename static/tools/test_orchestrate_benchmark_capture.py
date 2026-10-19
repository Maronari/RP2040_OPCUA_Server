#!/usr/bin/env python3
"""Focused offline tests for the bounded host benchmark capture runner."""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import time

import benchmark_profiles
import orchestrate_benchmark_capture as subject

COMMIT = "a" * 40
UF2 = "b" * 64
WARMUP = subject.FIRMWARE_WARMUP_SECONDS
DURATION = subject.FIRMWARE_CAPTURE_SECONDS


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def native_document(profile: dict) -> dict:
    records = []
    stamp = 1_000_000
    for sequence in (1, 2):
        for slot in range(1, profile["channels"] + 1):
            records.append({"channelId": f"benchmark.synthetic.{slot}", "sequence": sequence,
                            "sampleMonotonicUs": stamp})
            stamp += 10
        stamp += 1_000_000 // profile["rateHz"]
    return {"schemaVersion": 1, "kind": "v052-native-sampling-evidence", "state": "complete",
            "clock": {"domain": "device-monotonic", "unit": "microseconds", "valid": True},
            "bootStartedMonotonicUs": 1, "captureStartedMonotonicUs": 2,
            "captureEndedMonotonicUs": stamp + 1, "captureId": 7,
            "profileId": profile["profileId"], "warmupSeconds": WARMUP,
            "durationSeconds": DURATION, "recordCapacity": 8192, "records": records}


def client_document(profile: dict, board: str, toolchain: str, build_type: str) -> dict:
    return {"schemaVersion": 1, "profileId": profile["profileId"], "firmwareVersion": "0.5.2", "gitCommit": COMMIT,
            "gitDirty": False, "boardRevision": board, "toolchain": toolchain,
            "buildType": build_type, "warmupSeconds": WARMUP, "durationSeconds": DURATION,
            "repetitions": 1, "channelCount": profile["channels"], "clientCount": profile["clients"]}


def static_document(profile: dict, board: str, toolchain: str, build_type: str, dirty=False) -> dict:
    return {"schemaVersion": 1, "measurementKind": "static-build-layout",
            "provenance": {"benchmarkProfileId": profile["profileId"], "firmwareVersion": "0.5.2", "gitCommit": COMMIT,
                           "gitDirty": dirty, "boardRevision": board, "toolchain": toolchain,
                           "buildType": build_type},
            "artifacts": {"uf2": {"sha256": UF2}}}


class FakeProcess:
    def __init__(self, output: bytes, polls=(None, 0)):
        self.stdout, self._polls, self.returncode = io.BytesIO(output), list(polls), None
        self.terminated = self.killed = False
    def poll(self):
        value = self._polls.pop(0) if self._polls else self.returncode
        if value is not None:
            self.returncode = value
        return value
    def terminate(self): self.terminated = True; self.returncode = -15
    def kill(self): self.killed = True; self.returncode = -9
    def wait(self, timeout=None): return self.returncode


class FakeSerial:
    def __init__(self, native: bytes):
        self.native, self.writes, self.dumped, self.ready = native, [], False, True
    def __enter__(self): return self
    def __exit__(self, *_args): return False
    def reset_input_buffer(self): pass
    def write(self, value):
        self.writes.append(value)
        if value == b"benchmark start\n":
            assert self.ready, "a previous capture was not recovered"
            self.ready = False
            self.dumped = False
        if value == b"benchmark dump\n":
            self.dumped = True
        return len(value)
    def flush(self): pass
    def read(self, count):
        if not self.dumped or not self.native: return b""
        value, self.native = self.native[:count], self.native[count:]
        if not self.native:
            self.ready = True
        return value


def options(directory: Path, profile: dict, static: Path, client: Path) -> SimpleNamespace:
    return SimpleNamespace(profile_id=profile["profileId"], client_arg=["opc.tcp://device.invalid:4840"],
        client_script=Path("static/tools/load_test_opcua.py"), python="python", client_output=client,
        serial_port="COM-test", ready_token="READY\n", static_size_report=static,
        flashed_uf2_sha256=UF2, git_commit=COMMIT, warmup_seconds=WARMUP, duration_seconds=DURATION,
        readiness_timeout=0.2, client_timeout=0.2, dump_timeout=0.2,
        native_output=directory / "native.json", output=directory / "envelope.json", root=directory)


def identity(_root: Path): return COMMIT, False


def fast_recovery(console, _started_at, warmup_s, duration_s, dump_timeout_s):
    subject.recover_native_capture(console, time.monotonic() - warmup_s - duration_s,
                                   warmup_s, duration_s, dump_timeout_s)


def run_success() -> None:
    profile = next(item for item in benchmark_profiles.profiles()
                   if item["channels"] == 1 and item["clients"] == 1 and item["rateHz"] == 1
                   and item["security"] == "test-none" and not item["events"] and not item["history"])
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        client, static = directory / "client.json", directory / "static.json"
        write_json(client, client_document(profile, "board", "toolchain", "Debug"))
        write_json(static, static_document(profile, "board", "toolchain", "Debug"))
        args = options(directory, profile, static, client)
        process = FakeProcess(b"READY\n")
        serial = FakeSerial(b"[open62541/info] bounded UART noise\r\n" +
                            json.dumps(native_document(profile)).encode() + b"\r\ntrailing log\r\n")
        envelope = subject.run_capture(args, process_factory=lambda *_a, **_k: process,
                                       serial_factory=lambda *_a, **_k: serial, identity=identity)
        assert serial.writes == [b"benchmark start\n", b"benchmark dump\n"]
        assert envelope["state"] == "complete" and envelope["capture"]["readiness"]["tokenCount"] == 1
        assert envelope["provenance"] == {"gitCommit": COMMIT, "gitDirty": False,
                                            "boardRevision": "board", "toolchain": "toolchain",
                                            "buildType": "Debug", "flashedUf2Sha256": UF2}
        saved = json.loads(args.output.read_text(encoding="utf-8"))
        assert saved == envelope
        assert args.native_output.is_file()
        assert "opc.tcp" not in args.output.read_text(encoding="utf-8")
        command = subject.client_command(args, profile, {"boardRevision": "board",
                                         "toolchain": "toolchain", "buildType": "Debug"})
        token_index = command.index("--runner-readiness-token")
        assert command[token_index + 1] == args.ready_token
        assert command[command.index("--warmup") + 1] == str(WARMUP)
        assert command[command.index("--duration") + 1] == str(DURATION)


def rejects(callable_, phrase: str) -> None:
    try:
        callable_()
    except subject.CaptureError as exc:
        assert phrase in str(exc), exc
    else:
        raise AssertionError(f"expected CaptureError containing {phrase!r}")


def negative_cases() -> None:
    profile = next(item for item in benchmark_profiles.profiles()
                   if item["channels"] == 1 and item["clients"] == 1 and item["rateHz"] == 1
                   and item["security"] == "test-none" and not item["events"] and not item["history"])
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        client, static = directory / "client.json", directory / "static.json"
        write_json(client, client_document(profile, "board", "toolchain", "Debug"))
        write_json(static, static_document(profile, "board", "toolchain", "Debug"))
        args = options(directory, profile, static, client)
        args.readiness_timeout = 0.01
        no_ready = FakeProcess(b"")
        rejects(lambda: subject.run_capture(args, process_factory=lambda *_a, **_k: no_ready,
                                            serial_factory=lambda *_a, **_k: FakeSerial(b""), identity=identity),
                "readiness")

        args = options(directory, profile, static, client)
        malformed = FakeSerial(b"not-json")
        rejects(lambda: subject.run_capture(args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n"),
                                            serial_factory=lambda *_a, **_k: malformed, identity=identity,
                                            recovery=fast_recovery),
                "native serial dump")

        args = options(directory, profile, static, client)
        duplicate = json.dumps(native_document(profile)).encode()
        rejects(lambda: subject.run_capture(
                    args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n"),
                    serial_factory=lambda *_a, **_k: FakeSerial(duplicate + b"\n" + duplicate),
                    identity=identity, recovery=fast_recovery), "native serial dump")

        args = options(directory, profile, static, client)
        oversized = FakeSerial(b"x" * (subject.MAX_NATIVE_BYTES + 1))
        rejects(lambda: subject.run_capture(args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n"),
                                            serial_factory=lambda *_a, **_k: oversized, identity=identity,
                                            recovery=fast_recovery),
                "native serial dump")

        args = options(directory, profile, static, client)
        args.duration_seconds = 15.0
        rejects(lambda: subject.run_capture(
                    args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n"),
                    serial_factory=lambda *_a, **_k: FakeSerial(b""), identity=identity),
                "firmware timing contract")

        args = options(directory, profile, static, client)
        args.client_arg.append("--output=forbidden.json")
        rejects(lambda: subject.run_capture(args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n"),
                                            serial_factory=lambda *_a, **_k: FakeSerial(b""), identity=identity),
                "runner-controlled provenance")

        dirty_static = directory / "dirty-static.json"
        write_json(dirty_static, static_document(profile, "board", "toolchain", "Debug", dirty=True))
        args = options(directory, profile, dirty_static, client)
        rejects(lambda: subject.run_capture(args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n"),
                                            serial_factory=lambda *_a, **_k: FakeSerial(b""), identity=identity),
                "static-size provenance")

        # A failed workload must dump and drain the armed firmware capture so a
        # second invocation can issue a new start on the same board.
        board = FakeSerial(json.dumps(native_document(profile)).encode())
        args = options(directory, profile, static, client)
        rejects(lambda: subject.run_capture(
                    args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n", polls=(None, 1)),
                    serial_factory=lambda *_a, **_k: board, identity=identity,
                    recovery=fast_recovery),
                "client workload failed")
        assert board.ready and board.writes == [b"benchmark start\n", b"benchmark dump\n"]
        board.native = json.dumps(native_document(profile)).encode()
        args = options(directory, profile, static, client)
        subject.run_capture(args, process_factory=lambda *_a, **_k: FakeProcess(b"READY\n"),
                            serial_factory=lambda *_a, **_k: board, identity=identity)
        assert board.writes[-2:] == [b"benchmark start\n", b"benchmark dump\n"]


run_success()
negative_cases()
print("orchestrate-benchmark-capture tests: PASS")
