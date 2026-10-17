#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "static" / "tools" / "capture_resource_exhaustion.py"
SPEC = importlib.util.spec_from_file_location("resource_capture", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class CaptureTests(unittest.TestCase):
    def test_extracts_result_among_boot_logs(self) -> None:
        line = b'{"schemaVersion":1,"kind":"v052-resource-exhaustion"}'
        self.assertEqual(MODULE.extract_evidence(b"boot\r\n" + line + b"\r\n"), line + b"\n")

    def test_waits_for_complete_line(self) -> None:
        self.assertIsNone(MODULE.extract_evidence(
            b'{"kind":"v052-resource-exhaustion"'))

    def test_rejects_duplicate_results(self) -> None:
        line = b'{"kind":"v052-resource-exhaustion"}\n'
        with self.assertRaisesRegex(MODULE.EvidenceError, "more than one"):
            MODULE.extract_evidence(line + line)

    def test_rejects_oversized_result(self) -> None:
        line = b'{"kind":"v052-resource-exhaustion","padding":"' + b"x" * 4096 + b'"}'
        with self.assertRaisesRegex(MODULE.EvidenceError, "4096-byte"):
            MODULE.extract_evidence(line)


if __name__ == "__main__":
    unittest.main()
