#!/usr/bin/env python3
"""Bounded T4 matrix and observed configuration mismatch tests."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import benchmark_profiles as subject


def fixture(profile):
    period = 1000 // profile["rateHz"]
    flags = 1 | (2 if profile["history"] else 0) | (4 if profile["events"] else 0)
    return {"device": {"fwRevision": "0.5.2"}, "mqtt": {"enabled": False},
            "diagnostics": {"enableHeapDiagnostics": True, "enableTaskDiagnostics": True},
            "channels": [dict(slot=i, inUse=i < profile["channels"],
                              enabled=i < profile["channels"], driverId=10,
                              sourceIndex=i % 8, sourceFlags=0, kind=0, valueType=1,
                              id=f"benchmark.synthetic.{i + 1}", name=f"BenchmarkSynthetic{i + 1}",
                              unit="", flags=flags, samplePeriodMs=period,
                              publishPeriodMs=period, historyIntervalMs=period,
                              scale=1.0, offset=0.0) for i in range(16)]}


class ProfileTests(unittest.TestCase):
    def test_matrix_and_export(self):
        profiles = subject.profiles()
        self.assertEqual(len(profiles), 192)
        self.assertEqual(len({p["profileId"] for p in profiles}), 192)
        for p in profiles:
            subject.verify_export(p, fixture(p))
            self.assertEqual(subject.select(p["profileId"]), p)
            settings = subject.cmake_settings(p)
            self.assertEqual(settings["FIRMWARE_DRIVER_PACK"], "base")
            self.assertEqual(settings["FIRMWARE_BENCHMARK_PROFILE"], "synthetic")

    def test_unknown_profiles(self):
        for bad in ("production", "dev_benchmark", "reduced", "", "x" * 1000):
            with self.assertRaises(ValueError):
                subject.select(bad)

    def test_workload_mismatches(self):
        p = subject.profiles()[0]
        for field, value in (("driverId", 1), ("sourceIndex", 8), ("samplePeriodMs", 200),
                             ("flags", 7), ("valueType", 0), ("inUse", False),
                             ("enabled", False), ("scale", 2), ("id", "physical")):
            document = fixture(p)
            document["channels"][0][field] = value
            with self.assertRaises(ValueError, msg=field):
                subject.verify_export(p, document)

    def test_metadata_requires_json_boolean_axes(self):
        p = subject.profiles()[0]
        document = {**p, "testOnly": True, "driverPack": "base",
                    "firmwareVersion": "0.5.2"}
        subject.verify_metadata(p, document)
        document["events"] = int(p["events"])
        with self.assertRaises(ValueError):
            subject.verify_metadata(p, document)
        for mutate in (lambda d: d["channels"].pop(),
                       lambda d: d["channels"][1].update(slot=0),
                       lambda d: d["channels"][1].update(inUse=True),
                       lambda d: d["mqtt"].update(enabled=True),
                       lambda d: d["device"].update(fwRevision="0.5.1")):
            document = fixture(p)
            mutate(document)
            with self.assertRaises(ValueError):
                subject.verify_export(p, document)

    def test_cli_failure_and_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "export.json"
            path.write_text("{}", encoding="utf-8")
            result = subprocess.run([sys.executable, subject.__file__, "--profile-id",
                                     subject.profiles()[0]["profileId"], "--verify-export", str(path)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            path.write_bytes(b" " * (subject.MAX_BYTES + 1))
            with self.assertRaises(ValueError):
                subject.read_json(path)


if __name__ == "__main__":
    unittest.main()
