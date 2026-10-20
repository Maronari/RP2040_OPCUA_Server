#!/usr/bin/env python3
"""Host suite for probe_boot_clock.py's sample classification (no device)."""

from __future__ import annotations

import argparse
import asyncio
import datetime
import importlib.util
import json
import pathlib
import sys
import tempfile
from types import SimpleNamespace


TOOLS = pathlib.Path(__file__).resolve().parent
UTC = datetime.timezone.utc


def load():
    spec = importlib.util.spec_from_file_location("probe_boot_clock", TOOLS / "probe_boot_clock.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules["probe_boot_clock"] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    module = load()
    now = datetime.datetime(2026, 9, 13, 12, 51, 53, tzinfo=UTC)

    assert module.classify(None, now) == ("unreachable", None)
    # The OPC UA null DateTime, as asyncua decodes a zero timestamp (naive or aware).
    assert module.classify(datetime.datetime(1601, 1, 1), now) == ("null", None)
    assert module.classify(datetime.datetime(1601, 1, 1, tzinfo=UTC), now) == ("null", None)
    # The pre-ddc63c5 regression: 1970 + uptime.
    assert module.classify(datetime.datetime(1970, 1, 1, 0, 42, 10, tzinfo=UTC), now) == ("boot-relative", None)
    kind, skew = module.classify(datetime.datetime(2026, 9, 13, 12, 51, 52, 372000, tzinfo=UTC), now)
    assert kind == "utc" and abs(skew - (-0.628)) < 1e-6

    ports = [SimpleNamespace(device="COM6"), SimpleNamespace(device="COM12")]
    assert module.serial_port_present("COM6", ports)
    assert module.serial_port_present("com6", ports)
    assert not module.serial_port_present("COM7", ports)

    from serial.tools import list_ports

    original_comports = list_ports.comports
    original_token_created_at = module.token_created_at
    calls = 0

    rejected_args = argparse.Namespace(
        server_url="opc.tcp://device.invalid:4840", duration=0.1,
        interval=0.1, timeout=0.1, utc_samples=1, login_check=False,
        retry_after=1.5, evidence=pathlib.Path("unused.json"), cold_boot_serial="COM6",
        profile_id=None, git_commit=None, flashed_uf2_sha256=None)
    assert asyncio.run(module.run(rejected_args)) == 2

    list_ports.comports = lambda: [SimpleNamespace(device="COM6")]
    present_args = argparse.Namespace(**vars(rejected_args))
    present_args.profile_id = "profile"
    present_args.git_commit = "a" * 40
    present_args.flashed_uf2_sha256 = "b" * 64
    assert asyncio.run(module.run(present_args)) == 2

    unbounded_args = argparse.Namespace(**vars(present_args))
    unbounded_args.interval = 0.0
    assert asyncio.run(module.run(unbounded_args)) == 2
    list_ports.comports = original_comports

    def comports():
        nonlocal calls
        calls += 1
        return [] if calls == 1 else [SimpleNamespace(device="COM6")]

    async def token_created_at(_url, _timeout):
        return datetime.datetime.now(UTC)

    async def unreachable(_url, _timeout):
        raise ConnectionError("fixture endpoint unavailable")

    with tempfile.TemporaryDirectory() as directory:
        evidence = pathlib.Path(directory) / "unreachable.json"
        unavailable_args = argparse.Namespace(**vars(present_args))
        unavailable_args.duration = 0.11
        unavailable_args.interval = 0.1
        unavailable_args.timeout = 0.01
        unavailable_args.evidence = evidence
        list_ports.comports = lambda: []
        module.token_created_at = unreachable
        assert asyncio.run(module.run(unavailable_args)) == 1
        document = json.loads(evidence.read_text(encoding="utf-8"))
        assert document["firstReachableSeconds"] is None
        assert document["coldBootBoundary"]["enumerationObservedSeconds"] is None
        assert document["sampleCapacity"] == module.MAX_BOOT_CLOCK_SAMPLES
        assert document["sampleCapacityExhausted"] is False
        assert len(document["samples"]) <= module.MAX_BOOT_CLOCK_SAMPLES

    with tempfile.TemporaryDirectory() as directory:
        evidence = pathlib.Path(directory) / "cold-boot.json"
        args = argparse.Namespace(
            server_url="opc.tcp://device.invalid:4840", duration=2.0,
            interval=0.1, timeout=0.1, utc_samples=1, login_check=False,
            retry_after=1.5, evidence=evidence, cold_boot_serial="COM6",
            profile_id="profile", git_commit="a" * 40,
            flashed_uf2_sha256="b" * 64)
        list_ports.comports = comports
        module.token_created_at = token_created_at
        try:
            assert asyncio.run(module.run(args)) == 0
        finally:
            list_ports.comports = original_comports
            module.token_created_at = original_token_created_at
        document = json.loads(evidence.read_text(encoding="utf-8"))
        assert document["coldBootBoundary"]["absentAtObserverStart"] is True
        assert document["coldBootBoundary"]["enumerationObservedSeconds"] is not None
        assert document["firstReachableSeconds"] is not None

    print("probe-boot-clock: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
