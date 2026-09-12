#!/usr/bin/env python3
"""Host suite for label_driver_qualification_status.py (ROADMAP.md:604, v0.5.1 T18)."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = ROOT / "static" / "tools"


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def run(module, argv):
    old_argv = sys.argv
    sys.argv = ["label_driver_qualification_status.py"] + argv
    try:
        return module.main()
    finally:
        sys.argv = old_argv


def main() -> int:
    module = load("label_driver_qualification_status")
    real_report = ROOT / "docs" / "releases" / "v0.5.1-driver-verification.md"
    assert real_report.is_file(), "the real compile/mock-validation report must exist for this test to be meaningful"

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)

        industrial_output = tmp_path / "industrial.json"
        exit_code = run(
            module,
            ["--driver-pack", "industrial", "--report", str(real_report), "--output", str(industrial_output)],
        )
        assert exit_code == 0, "industrial pack with a real report must succeed"
        payload = json.loads(industrial_output.read_text(encoding="utf-8"))
        assert payload["driver_pack"] == "industrial"
        assert payload["qualification_status"] == "compile/mock validated, not hardware qualified"
        assert payload["report_path"] == str(real_report)
        assert "not hardware qualified" in payload["label"]

        base_output = tmp_path / "base.json"
        exit_code = run(module, ["--driver-pack", "base", "--report", str(real_report), "--output", str(base_output)])
        assert exit_code == 0, "base pack must succeed regardless of the report"
        payload = json.loads(base_output.read_text(encoding="utf-8"))
        assert payload["driver_pack"] == "base"
        assert payload["qualification_status"] == "not applicable"
        assert payload["report_path"] is None

        missing_report = tmp_path / "does-not-exist.md"
        exit_code = run(module, ["--driver-pack", "industrial", "--report", str(missing_report)])
        assert exit_code == 1, "industrial pack must fail loudly when the compile/mock-validation report is missing"

    print("label-driver-qualification-status: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
