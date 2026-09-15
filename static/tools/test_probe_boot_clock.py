#!/usr/bin/env python3
"""Host suite for probe_boot_clock.py's sample classification (no device)."""

from __future__ import annotations

import datetime
import importlib.util
import pathlib
import sys


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

    print("probe-boot-clock: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
