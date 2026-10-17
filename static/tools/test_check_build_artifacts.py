#!/usr/bin/env python3
"""Fixture tests for semantic build-artifact report validation."""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import sys
import tempfile


TOOL = pathlib.Path(__file__).with_name("check_build_artifacts.py")
SPEC = importlib.util.spec_from_file_location("check_build_artifacts", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def main() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        artifacts = {}
        for index, (key, relative) in enumerate(MODULE.STATIC_REPORT_ARTIFACTS.items(), start=1):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            payload = bytes([index]) * index
            path.write_bytes(payload)
            artifacts[key] = {
                "name": path.name,
                "sizeBytes": len(payload),
                "sha256": MODULE.file_sha256(path),
            }
        report = {
            "schemaVersion": 1,
            "artifacts": artifacts,
            "totals": {
                "ElfFileBytes": 1,
                "Uf2FileBytes": 2,
                "BinFileBytes": 3,
                "ExternalFlashLoadBytes": 3,
                "SramDataBytes": 1,
                "SramBssBytes": 1,
                "SramStaticTotalBytes": 2,
                "SramReservedHeapBytes": 1,
                "PsramStaticBytes": 0,
            },
        }
        report_path = root / "reports/static_size_report.json"
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report), encoding="utf-8")
        assert MODULE.validate_static_size_report(root) == []

        report_path.write_text("{broken", encoding="utf-8")
        assert "malformed" in MODULE.validate_static_size_report(root)[0]
        report_path.write_text(json.dumps(report), encoding="utf-8")

        broken = copy.deepcopy(report)
        broken["schemaVersion"] = 2
        report_path.write_text(json.dumps(broken), encoding="utf-8")
        assert any("schemaVersion" in problem for problem in MODULE.validate_static_size_report(root))

        broken = copy.deepcopy(report)
        broken["totals"]["SramStaticTotalBytes"] = -1
        report_path.write_text(json.dumps(broken), encoding="utf-8")
        assert any("SramStaticTotalBytes" in problem for problem in MODULE.validate_static_size_report(root))

        broken = copy.deepcopy(report)
        broken["artifacts"]["elf"]["sha256"] = "0" * 64
        report_path.write_text(json.dumps(broken), encoding="utf-8")
        assert any("elf sha256 mismatch" in problem for problem in MODULE.validate_static_size_report(root))

        broken = copy.deepcopy(report)
        broken["artifacts"]["bin"]["sizeBytes"] += 1
        report_path.write_text(json.dumps(broken), encoding="utf-8")
        assert any("bin size mismatch" in problem for problem in MODULE.validate_static_size_report(root))

    print("build artifact semantic checker suite: PASS")


if __name__ == "__main__":
    main()
